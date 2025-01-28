/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bmi160.h"
#include "bmi160_private.h"
#include "drivers/accel.h"

#include "bmi160_regs.h"
#include "drivers/exti.h"
#include "drivers/rtc.h"
#include "system/passert.h"
#include "system/logging.h"
#include "kernel/util/delay.h"
#include "util/math.h"
#include "util/size.h"
#include "kernel/util/sleep.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

// Note: Before adding a new header, be sure you actually need it! The goal
// is to keep the driver as unreliant on higher level constructs as possible
#ifdef BMI160_DEBUG
#include "console/dbgserial.h"
#define BMI160_DBG(msg, ...)                                    \
  do {                                                          \
    char _buf[80];                                              \
    dbgserial_putstr_fmt(_buf, sizeof(_buf), msg, __VA_ARGS__); \
  } while (0)
#else
#define BMI160_DBG(msg, ...)
#endif

#define NUM_AVERAGED_SAMPLES (4)

typedef enum {
  BMI160_SCALE_2G = 2,
  BMI160_SCALE_4G = 4,
  BMI160_SCALE_8G = 8,
  BMI160_SCALE_16G = 16,
} Bmi160Scale;

static int16_t s_raw_unit_to_mgs = 8192;

static BMI160AccelPowerMode s_accel_power_mode = BMI160_Accel_Mode_Suspend;
static BMI160GyroPowerMode s_gyro_power_mode = BMI160_Gyro_Mode_Suspend;

static bool s_accel_outstanding_motion_work = false;
static bool s_accel_outstanding_data_work = false;
static bool s_fifo_in_use = false;
static uint8_t curr_fifo_num_samples_wm = 0;

static bool s_double_tap_detection_enabled = false;
static bool s_shake_detection_enabled = false;

// Accelerometer configuration criteria
// Each operating mode can be enabled and disabled independently from each other and the driver
// will configure the accelerometer in the highest power mode and with the highest sampling rate
// required according which operating modes are enabled and what the requirements are thereof.

typedef enum {
  AccelOperatingModeData,
  AccelOperatingModeShakeDetection,
  AccelOperatingModeDoubleTapDetection,

  AccelOperatingMode_Num,
} AccelOperatingMode;

typedef enum {
  AccelPowerModeLowPower,
  AccelPowerModeNormal,

  AccelPowerMode_Num,
} AccelPowerMode;

static struct {
  bool enabled;
  BMI160SampleRate sample_interval;
} s_operating_states[] = {
  [AccelOperatingModeData] = {
    .enabled = false,
    .sample_interval = BMI160SampleRate_25_HZ,
  },
  [AccelOperatingModeShakeDetection] = {
    .enabled = false,
    .sample_interval = BMI160SampleRate_25_HZ,
  },
  [AccelOperatingModeDoubleTapDetection] = {
    .enabled = false,
    .sample_interval = BMI160SampleRate_200_HZ,
  },
};

#define HZ_TO_US(hz) 1000000 / (hz)

static void prv_write_reg(uint8_t reg, uint8_t value) {
  bmi160_write_reg(reg, value);
  // Wait 2 us (active mode) or 450 us (suspend mode)
  // before issuing the next read or write command.
  //
  // TODO: I'm pretty sure if commands are specifically targetting
  // a unit in suspend mode, we will need to delay for 450us even if
  // the other unit is powered up in Normal mode
  if (s_accel_power_mode == BMI160_Accel_Mode_Normal
      || s_gyro_power_mode == BMI160_Gyro_Mode_Normal) {
    // Apparently this delays for ~3.5 us. Unconfirmed.
    delay_us(5);
  } else {
    psleep(2); // must sleep >= 450us
  }
}


static void prv_burst_read(uint8_t reg, void *buf, size_t len) {
  reg |= BMI160_READ_FLAG;
  SPIScatterGather sg_info[2] = {
    {.sg_len = 1, .sg_out = &reg, .sg_in = NULL}, // address
    {.sg_len = len, .sg_out = NULL, .sg_in = buf} // read data
  };
  spi_slave_burst_read_write_scatter(BMI160_SPI, sg_info, ARRAY_LENGTH(sg_info));
}

static void prv_read_modify_write(uint8_t reg, uint8_t value, uint8_t mask) {
  uint8_t new_val = bmi160_read_reg(reg);
  new_val &= ~mask;
  new_val |= value;
  prv_write_reg(reg, new_val);
}

static void prv_run_command(uint8_t command) {
  prv_write_reg(BMI160_REG_CMD, command);
  if (command == BMI160_CMD_SOFTRESET) {
    s_accel_power_mode = BMI160_Accel_Mode_Suspend;
    s_gyro_power_mode = BMI160_Gyro_Mode_Suspend;
  }
}

static Bmi160Scale prv_get_accel_scale(void) {
  uint8_t scale_reg_val = bmi160_read_reg(BMI160_REG_ACC_RANGE) & 0xf;

  if (scale_reg_val == BMI160_ACC_RANGE_2G) {
    return BMI160_SCALE_2G;
  } else if (scale_reg_val == BMI160_ACC_RANGE_4G) {
    return BMI160_SCALE_4G;
  } else if (scale_reg_val == BMI160_ACC_RANGE_8G) {
    return BMI160_SCALE_8G;
  } else if (scale_reg_val == BMI160_ACC_RANGE_16G) {
    return BMI160_SCALE_16G;
  }

  WTF;
  return 0;
}

static void prv_set_accel_scale(Bmi160Scale scale) {
  uint8_t cfg_val;
  switch (scale) {
    case BMI160_SCALE_2G:
      cfg_val = BMI160_ACC_RANGE_2G;
      break;
    case BMI160_SCALE_4G:
      cfg_val = BMI160_ACC_RANGE_4G;
      break;
    case BMI160_SCALE_8G:
      cfg_val = BMI160_ACC_RANGE_8G;
      break;
    case BMI160_SCALE_16G:
      cfg_val = BMI160_ACC_RANGE_16G;
      break;
    default:
      WTF;
  }
  prv_write_reg(BMI160_REG_ACC_RANGE, cfg_val);
  s_raw_unit_to_mgs = 32768 / scale;
}

static int16_t prv_raw_to_mgs(int16_t raw_val) {
  int16_t mgs = (raw_val * 1000) / s_raw_unit_to_mgs;
  return mgs;
}

static void prv_convert_accel_raw_data_to_mgs(const uint8_t *raw_buf,
    AccelDriverSample *data) {
  int16_t readings[3];
  for (unsigned int i = 0; i < ARRAY_LENGTH(readings); i++) {
    int base = i * 2;
    readings[i] = raw_buf[base] | (raw_buf[base + 1] << 8);
  }

  const AccelConfig *cfg = &BOARD_CONFIG_ACCEL.accel_config;

  data->x = (cfg->axes_inverts[AXIS_X] ? -1 : 1) *
      prv_raw_to_mgs(readings[cfg->axes_offsets[AXIS_X]]);
  data->y = (cfg->axes_inverts[AXIS_Y] ? -1 : 1) *
      prv_raw_to_mgs(readings[cfg->axes_offsets[AXIS_Y]]);
  data->z = (cfg->axes_inverts[AXIS_Z] ? -1 : 1) *
      prv_raw_to_mgs(readings[cfg->axes_offsets[AXIS_Z]]);
}

static uint64_t prv_get_curr_system_time_ms(void) {
  time_t time_s;
  uint16_t time_ms;
  rtc_get_time_ms(&time_s, &time_ms);
  return (((uint64_t)time_s) * 1000 + time_ms);
}

static uint32_t prv_sensortime_to_timestamp(uint8_t sensor_time[3]) {
  return (sensor_time[0] | (sensor_time[1] << 8) | (sensor_time[2] << 16));
}

static uint32_t prv_get_min_sampling_interval_us(void) {
  BMI160SampleRate sample_rate_max = BMI160SampleRate_12p5_HZ;
  for (uint32_t i = 0; i < ARRAY_LENGTH(s_operating_states); i++) {
    if (s_operating_states[i].enabled) {
      // use MIN because sample rate enum value is actually the sampling interval in us
      sample_rate_max = MIN(sample_rate_max, s_operating_states[i].sample_interval);
    }
  }
  return (uint32_t)sample_rate_max;
}

// Determine the bit that flips when the sample is collected (as sensor events
// are synchronous to this register)
static uint8_t prv_get_sample_collection_bit(void) {
  return (31 - (__builtin_clz(prv_get_min_sampling_interval_us() /
      BMI160_SENSORTIME_RESOLUTION_US)));
}

// Converts BMI160 sensortime data to a uint32_t (units incremented every 39us)
static uint32_t prv_get_time_since_sample(uint8_t sensor_time[3]) {
  uint32_t sensor_timestamp = prv_sensortime_to_timestamp(sensor_time);

  uint8_t sample_time_bit = prv_get_sample_collection_bit();

  uint64_t time_since_data_collection_us = (sensor_timestamp &
      ((0x1 << sample_time_bit) - 1)) * BMI160_SENSORTIME_RESOLUTION_US;

  return time_since_data_collection_us;
}

// determines if a new sample was collected between the two sensor timestamps
// provided
static bool prv_new_sample_collected(uint8_t sensor_timestamp_before[3],
    uint8_t sensor_timestamp_after[3]) {

  uint32_t start_time = prv_sensortime_to_timestamp(sensor_timestamp_before);
  uint32_t end_time = prv_sensortime_to_timestamp(sensor_timestamp_after);

  uint32_t sample_time_bit = prv_get_sample_collection_bit();

  // see if the upper bits overflowed
  uint32_t upper_bits_mask = ~((0x1 << sample_time_bit) - 1);
  start_time &= upper_bits_mask;
  end_time &= upper_bits_mask;

  return (start_time != end_time);
}

// Converts the sensor time from the BMI160 captured at the time the sample
// was collected to the actual system time
static uint64_t prv_get_sample_time_us(uint8_t sensor_time[3]) {
  uint64_t curr_time_us = prv_get_curr_system_time_ms() * 1000;

  uint32_t time_since_data_collection_us = prv_get_time_since_sample(sensor_time);
  BMI160_DBG("%"PRIu32" us delay since sample was collected",
      time_since_data_collection_us);

  return (curr_time_us - time_since_data_collection_us);
}

static void prv_read_curr_accel_data(AccelDriverSample *data) {
  uint8_t res[9]; /* x, y, z & timestamp */
  prv_burst_read(BMI160_REG_ACC_X_LSB, res, sizeof(res));

  *data = (AccelDriverSample){};
  prv_convert_accel_raw_data_to_mgs(res, data);

  data->timestamp_us = prv_get_sample_time_us(&res[6]);

  BMI160_DBG("%"PRId16" %"PRId16" %"PRId16, data->x, data->y, data->z);
}

static IMUCoordinateAxis prv_get_axis_direction(uint8_t int0_status, uint8_t int2_status,
                                                uint8_t mask, uint8_t shift, int32_t *direction) {
  *direction = ((int2_status & BMI160_INT_STATUS_2_ANYM_SIGN) == 0) ? 1 : -1;
  IMUCoordinateAxis axis = AXIS_X;
  const AccelConfig *cfg = &BOARD_CONFIG_ACCEL.accel_config;

  bool invert = false;

  if ((int2_status & (shift << cfg->axes_offsets[AXIS_X])) != 0) {
    axis = AXIS_X;
    invert = cfg->axes_inverts[AXIS_X];
  } else if ((int2_status & (shift << cfg->axes_offsets[AXIS_Y])) != 0) {
    axis = AXIS_Y;
    invert = cfg->axes_inverts[AXIS_Y];
  } else if ((int2_status & (shift << cfg->axes_offsets[AXIS_Z])) != 0) {
    axis = AXIS_Z;
    invert = cfg->axes_inverts[AXIS_Z];
  } else {
    BMI160_DBG("No Axis?: 0x%"PRIx8" 0x%"PRIx8, int0_status, int2_status);
  }
  *direction *= (invert ? -1 : 1);
  return axis;
}

static void prv_dump_int_stats(void);

static void prv_handle_motion_interrupts(void) {
  s_accel_outstanding_motion_work = false;
  // Interestingly, the status registers for tap interrupts are updated _after_
  // the EXTI fires. Low power mode toggles between suspend and normal mode. I
  // believe the updates to the registers only occur during the run cycles
  // which occur at an interval dependent on the sampling frequency
  bool toggled_power_mode = (s_accel_power_mode == BMI160_Accel_Mode_Low);
  if (toggled_power_mode) {
    bmi160_set_accel_power_mode(BMI160_Accel_Mode_Normal);
  }

  uint8_t int0_status = bmi160_read_reg(BMI160_REG_INT_STATUS_0);
  uint8_t int2_status = bmi160_read_reg(BMI160_REG_INT_STATUS_2);
  prv_run_command(BMI160_CMD_INT_RESET);

  // debug
  bool anymotion = ((int0_status & BMI160_INT_STATUS_0_ANYM_MASK) != 0);
  if (anymotion) {
    int32_t direction;
    IMUCoordinateAxis axis = prv_get_axis_direction(int0_status, int2_status,
                                                    BMI160_INT_STATUS_2_ANYM_SIGN,
                                                    BMI160_INT_STATUS_2_ANYM_FIRST_X, &direction);
    BMI160_DBG("Anymotion on axis %"PRIu8" in direction %"PRId32, axis, direction);
    accel_cb_shake_detected(axis, direction);
  }
  bool double_tap = ((int0_status & BMI160_INT_STATUS_0_D_TAP_MASK) != 0);
  if (double_tap) {
    int32_t direction;
    IMUCoordinateAxis axis = prv_get_axis_direction(int0_status, int2_status,
                                                    BMI160_INT_STATUS_2_TAP_SIGN,
                                                    BMI160_INT_STATUS_2_TAP_FIRST_X, &direction);
    if (double_tap) {
      BMI160_DBG("Double tap on axis %"PRIu8" in direction %"PRId32, axis, direction);
      accel_cb_double_tap_detected(axis, direction);
    }
  } else if (!anymotion) {
    BMI160_DBG("Wahh, no motion/tap?: INT0: 0x%"PRIx8", INT2: 0x%"PRIx8, int0_status, int2_status);
    prv_dump_int_stats();
  }

  if (toggled_power_mode) {
    bmi160_set_accel_power_mode(BMI160_Accel_Mode_Low);
  }
}

// TODO: strictly for debug, remove when done
static void prv_dump_int_stats(void) {
  for (uint8_t addr = 0x1b; addr <= 0x1f; addr++) {
    BMI160_DBG("0x%"PRIx8" = 0x%"PRIx8, addr, bmi160_read_reg(addr));
  }
  BMI160_DBG("Latched = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_LATCH));
  BMI160_DBG("Err reg = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_ERR));
  BMI160_DBG("INT_MAP[0] = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_MAP_0));
  BMI160_DBG("INT_MAP[1] = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_MAP_1));
  BMI160_DBG("INT_EN[0] = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_EN_0));
  BMI160_DBG("INT_EN[1] = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_EN_1));
}

// should be servicing tap/motion interrupts
static void bmi160_IRQ1_handler(bool *should_context_switch) {
  if (!s_accel_outstanding_motion_work) {
    s_accel_outstanding_motion_work = true;
    accel_offload_work_from_isr(prv_handle_motion_interrupts, should_context_switch);
  } else {
    BMI160_DBG("%s", "We fell behind on motion interrupt handling!");
  }
}

static uint8_t prv_get_fifo_frame_size(void) {
  return 6; // we are just storing {x, y, z} accel data in the fifo today
}

static uint16_t prv_get_current_fifo_length_and_timestamp(uint64_t *timestamp_us) {
  int retries = 0;
  while (retries < 10) {
    uint8_t ts_before[3], ts_after[3];

    // We want to find the timestamp of the latest sample in the fifo so:
    //   1. read the current sensor timestamp
    //   2. read the current length of the fifo
    //   3. read the sensor timestamp again
    // Since new sample collection is synchronous with a particular bit of the
    // sensor timestamp, we can see if that bit overflowed between 1 & 3 to see
    // if a new sample was appended. Continue this process until there is no
    // overflow between the readings in 1 & 3.

    prv_burst_read(BMI160_REG_SENSORTIME_0, &ts_before[0], sizeof(ts_before));
    uint64_t sample_time_before = prv_get_sample_time_us(ts_before);
    uint16_t num_samples = bmi160_read_16bit_reg(BMI160_REG_FIFO_LENGTH_LSB);
    prv_burst_read(BMI160_REG_SENSORTIME_0, &ts_after[0], sizeof(ts_after));

    // check to see if we rolled
    if (!prv_new_sample_collected(ts_before, ts_after)) {
      *timestamp_us = sample_time_before;
      return (num_samples);
    }

    retries++;
  };

  // something has gone wrong if we fail to recover the right length & timestamp
  PBL_ASSERTN(retries < 10);
  return 0;
}

static void prv_process_fifo_frame(const uint8_t *frame_buf, AccelDriverSample *data) {
  const int a_begin = 0; // index within the frame where accel data starts
  prv_convert_accel_raw_data_to_mgs(&frame_buf[a_begin], data);
}

static void prv_drain_fifo(void) {
  // we can't drain the fifo if we are in low power mode so we have
  // to temporarily enter normal mode
  bool was_low_power = (s_accel_power_mode == BMI160_Accel_Mode_Low);
  if (was_low_power) {
    bmi160_set_accel_power_mode(BMI160_Accel_Mode_Normal);
  }

  // get the FIFO length
  uint64_t last_frame_time = 0;
  uint16_t len = prv_get_current_fifo_length_and_timestamp(&last_frame_time);
  BMI160_DBG("Reading %d bytes", len);

  uint8_t fifo_frame_len = prv_get_fifo_frame_size();

  bmi160_begin_burst(BMI160_REG_FIFO_DATA | BMI160_READ_FLAG);

  uint8_t curr_num_samples = (len / fifo_frame_len);
  uint32_t curr_sampling_interval_us = prv_get_min_sampling_interval_us();
  uint64_t start_time = last_frame_time - curr_num_samples * curr_sampling_interval_us;

  for (int i = 0; i < len; i += fifo_frame_len) {
    uint8_t burst_buf[fifo_frame_len];
    spi_ll_slave_burst_read(BMI160_SPI, &burst_buf[0], fifo_frame_len);

    AccelDriverSample data;
    prv_process_fifo_frame(burst_buf, &data);
    data.timestamp_us = start_time;
    start_time += curr_sampling_interval_us;

    BMI160_DBG("%2d: %"PRId16" %"PRId16" %"PRId16, i, data.x, data.y, data.z);
    accel_cb_new_sample(&data);
  }
  bmi160_end_burst();

  BMI160_DBG("%d bytes remain", prv_get_current_fifo_length_and_timestamp(&last_frame_time));

  if (was_low_power) {
    bmi160_set_accel_power_mode(BMI160_Accel_Mode_Low);
  }
}

static void prv_handle_data(void) {
  s_accel_outstanding_data_work = false;

  // if the task draining the fifo gets swapped out for a long enough duration,
  // its possible the fifo watermark interrupt may fire multiple times. Once
  // the task finishes draining the fifo, the interrupt will be cleared but a
  // fifo drain callback could have already been scheduled so don't check the
  // interrupt status
  if (s_fifo_in_use) {
    prv_drain_fifo();
    return;
  }

  // the int_status for drdy is not latched so we check the status register
  // instead to confirm new data accel data is available

  uint8_t status = bmi160_read_reg(BMI160_REG_STATUS);
  if ((status & BMI160_STATUS_DRDY_ACC_MASK) != 0) {
    AccelDriverSample data;
    prv_read_curr_accel_data(&data);
    accel_cb_new_sample(&data);
  } else {
    BMI160_DBG("Unexpected int status: 0x%"PRIx8" 0x%x",
      bmi160_read_reg(BMI160_REG_INT_STATUS_1), status);
  }
}

static void bmi160_IRQ2_handler(bool *should_context_switch) {
  if (!s_accel_outstanding_data_work) {
    s_accel_outstanding_data_work = true;
    accel_offload_work_from_isr(prv_handle_data, should_context_switch);
  } else {
    BMI160_DBG("%s", "We fell behind on data handling");
  }
}

// in order to actually enter 'low power' mode, we have to set up accel to do
// undersampling. The more samples we use for one reading, the higher the power
// consumption but the lower the RMS noise
static void prv_accel_enable_undersampling(bool enable) {
  const uint8_t acc_us_bwp_mask =
      (BMI160_ACC_CONF_ACC_BWP_MASK << BMI160_ACC_CONF_ACC_BWP_SHIFT) |
      (BMI160_ACC_CONF_ACC_US_MASK << BMI160_ACC_CONF_ACC_US_SHIFT);

  uint8_t acc_us_bwp;
  if (!enable) {
    acc_us_bwp = 0x2 << BMI160_ACC_CONF_ACC_BWP_SHIFT;
  } else {
    _Static_assert(NUM_AVERAGED_SAMPLES <= 128, "Number of averaged samples "
        "must be <= 128");
    const uint8_t acc_bwp = 31 - __builtin_clz((uint32_t)NUM_AVERAGED_SAMPLES);
    acc_us_bwp = (0x1 << BMI160_ACC_CONF_ACC_US_SHIFT) |
        (acc_bwp << BMI160_ACC_CONF_ACC_BWP_SHIFT);
  }

  prv_read_modify_write(BMI160_REG_ACC_CONF, acc_us_bwp, acc_us_bwp_mask);
}

static void prv_update_accel_interrupts(bool enable) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(BOARD_CONFIG_ACCEL.accel_ints); i++) {
    uint8_t int_cfg = 0;
    uint8_t int_mask = 0xf << (i * 4);
    if (enable) {
      int_cfg = 0xA << (i * 4); // INT EN, Push-Pull, Active High, Level Triggered
      exti_enable(BOARD_CONFIG_ACCEL.accel_ints[i]);
    } else { // disable
      exti_disable(BOARD_CONFIG_ACCEL.accel_ints[i]);
    }

    prv_read_modify_write(BMI160_REG_INT_OUT_CTRL, int_cfg, int_mask);
    BMI160_DBG("INT_OUT_CTRL = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_OUT_CTRL));
  }
}

void bmi160_init(void) {
  gpio_input_init(&BOARD_CONFIG_ACCEL.accel_int_gpios[0]);
  gpio_input_init(&BOARD_CONFIG_ACCEL.accel_int_gpios[1]);

  exti_configure_pin(BOARD_CONFIG_ACCEL.accel_ints[0], ExtiTrigger_Rising,
      bmi160_IRQ1_handler);
  exti_configure_pin(BOARD_CONFIG_ACCEL.accel_ints[1], ExtiTrigger_Rising,
      bmi160_IRQ2_handler);

  bmi160_enable_spi_mode();
  if (bmi160_query_whoami()) {
    prv_run_command(BMI160_CMD_SOFTRESET);
    bmi160_enable_spi_mode();
  } else {
    PBL_LOG(LOG_LEVEL_WARNING, "Failed to query BMI160");
  }

  prv_set_accel_scale(BMI160_SCALE_4G);
}

bool bmi160_query_whoami(void) {
  uint8_t whoami = bmi160_read_reg(BMI160_REG_CHIP_ID);
  PBL_LOG(LOG_LEVEL_DEBUG, "Read BMI160 whoami byte 0x%"PRIx8", expecting 0x%"PRIx8,
          whoami, BMI160_CHIP_ID);
  return (whoami == BMI160_CHIP_ID);
}

// TODO/NOTE: The accel & gyro self test routines changes some of the BMI160
// configuration state. In the future we could update them so they do not
// destroy the state

bool accel_run_selftest(void) {
  prv_update_accel_interrupts(false);

  prv_run_command(BMI160_CMD_SOFTRESET);
  psleep(50);

  bmi160_enable_spi_mode();

  bmi160_set_accel_power_mode(BMI160_Accel_Mode_Normal);
  psleep(10);

  // Set to 8g range, as required for the self test mode
  prv_set_accel_scale(BMI160_SCALE_8G);

  // Set ODR to 1600Hz
  accel_set_sampling_interval(BMI160SampleRate_1600_HZ);
  prv_accel_enable_undersampling(false);

  PBL_LOG(LOG_LEVEL_DEBUG, "Self Test: Negative offset");

  // Enable self test with high amplitude in the negative direction
  prv_write_reg(BMI160_REG_SELF_TEST, 0x8 | 0b01);
  psleep(50);

  struct {
    char axis_name;

    uint8_t register_address;
    int pass_threshold;

    int16_t negative_value;
    int16_t positive_value;
  } accel_test_axis[] = {
    {
      .axis_name = 'X',
      .register_address = BMI160_REG_ACC_X_LSB,
      .pass_threshold = 3277
    }, {
      .axis_name = 'Y',
      .register_address = BMI160_REG_ACC_Y_LSB,
      .pass_threshold = 3277
    }, {
      .axis_name = 'Z',
      .register_address = BMI160_REG_ACC_Z_LSB,
      .pass_threshold = 1639
    }
  };

  // Collect data with the negative offset applied
  for (unsigned int i = 0; i < ARRAY_LENGTH(accel_test_axis); ++i) {
    accel_test_axis[i].negative_value = bmi160_read_16bit_reg(accel_test_axis[i].register_address);
    PBL_LOG(LOG_LEVEL_DEBUG, "- %c: %"PRId16,
            accel_test_axis[i].axis_name, accel_test_axis[i].negative_value);
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Self Test: Positive offset");

  // Flip sign bit from negative to positive while leaving self test mode on at high amplitude
  prv_write_reg(BMI160_REG_SELF_TEST, 0x8 | 0x4 | 0b01);

  psleep(50);

  // Collect data with the positive offset applied
  for (unsigned int i = 0; i < ARRAY_LENGTH(accel_test_axis); ++i) {
    accel_test_axis[i].positive_value = bmi160_read_16bit_reg(accel_test_axis[i].register_address);
    PBL_LOG(LOG_LEVEL_DEBUG, "+ %c: %"PRId16,
            accel_test_axis[i].axis_name, accel_test_axis[i].positive_value);
  }

  // Verify each axis saw a big enough delta in response to the self test mode.
  // NOTE! For some reason, applying a "positive" force makes the number go lower and applying
  // a "negative" force makes the number go higher. And then, for some reason, we abs() it when
  // calculating a delta to hide the fact that it's backwards. This is all documented in a
  // document called "How to perform BMI160 accelerometer self-test" provided by Bosch, so I guess
  // it's the right thing to do. This document is attached to PBL-10951.
  bool pass = true;
  for (unsigned int i = 0; i < ARRAY_LENGTH(accel_test_axis); ++i) {
    int axis_delta = accel_test_axis[i].positive_value - accel_test_axis[i].negative_value;
    axis_delta = abs(axis_delta);

    if (axis_delta < accel_test_axis[i].pass_threshold) {
      PBL_LOG(LOG_LEVEL_WARNING, "Self test failed for axis %c: %d < %d",
              accel_test_axis[i].axis_name, axis_delta, accel_test_axis[i].pass_threshold);
      pass = false;
    }
  }

  prv_run_command(BMI160_CMD_SOFTRESET);
  psleep(50);

  bmi160_enable_spi_mode();

  return pass;
}

bool gyro_run_selftest(void) {
  prv_update_accel_interrupts(false);

  prv_run_command(BMI160_CMD_SOFTRESET);
  psleep(50);

  bmi160_enable_spi_mode();

  bmi160_set_gyro_power_mode(BMI160_Gyro_Mode_Normal);

  // Write the gyr_self_test_start bit
  prv_write_reg(BMI160_REG_SELF_TEST, 0x10);
  psleep(50);

  const uint8_t status = bmi160_read_reg(BMI160_REG_SELF_TEST);

  // power down the gyro
  bmi160_set_gyro_power_mode(BMI160_Gyro_Mode_Suspend);
  if (status | 0x2) {
    return true;
  }
  return false;
}

void bmi160_set_accel_power_mode(BMI160AccelPowerMode mode) {
  uint8_t status = 0;

  int retries = 10;
  prv_run_command(BMI160_CMD_ACC_SET_PMU_MODE | mode);
  while (retries--) {
    // Takes 3.2 to 3.8ms according to datasheet
    status = bmi160_read_reg(BMI160_REG_PMU_STATUS) >> 4;
    if (status == mode) {
      break;
    }
    BMI160_DBG("ACCEL: want mode %d, actual %d", mode, status);
  }
#ifdef BMI160_DEBUG
  PBL_ASSERT(retries > 0, "Could not set power mode to %d", mode);
#endif
  s_accel_power_mode = mode;
  BMI160_DBG("PMU_STATUS: 0x%x ACC_CONF: 0x%x",
    bmi160_read_reg(BMI160_REG_PMU_STATUS), bmi160_read_reg(BMI160_REG_ACC_CONF));
}

void bmi160_set_gyro_power_mode(BMI160GyroPowerMode mode) {
  int retries = 20;
  prv_run_command(BMI160_CMD_GYR_SET_PMU_MODE | mode);
  while (retries--) {
    uint8_t status = 0;
    // can take up to 80ms to power up
    status = bmi160_read_reg(BMI160_REG_PMU_STATUS) >> 2;
    if (status == mode) {
      break;
    }
    psleep(5);
    BMI160_DBG("GYRO: want mode %d, actual %d", mode, status);
  }
  PBL_ASSERT(retries > 0, "Gyro: Could not set power mode to %d", mode);

  s_gyro_power_mode = mode;
  BMI160_DBG("PMU_STATUS: 0x%x", bmi160_read_reg(BMI160_REG_PMU_STATUS));
}

/*
 * accel.h driver interface exposed to higher level code
 */

static uint32_t prv_get_sampling_interval_from_hw(void) {
  uint8_t acc_cfg = bmi160_read_reg(BMI160_REG_ACC_CONF);
  acc_cfg = (acc_cfg >> BMI160_ACC_CONF_ACC_ODR_SHIFT) &
      BMI160_ACC_CONF_ACC_ODR_MASK;

  // sample interval (us) = 10000 * (2 ^ (8 - val(acc_odr)))
  int shift_val = 8 - acc_cfg;
  uint32_t interval = 10000;
  if (shift_val >= 0) {
    interval <<= shift_val;
  } else {
    interval >>= -shift_val;
  }

  return interval;
}

static BMI160AccODR prv_get_odr(BMI160SampleRate sample_rate) {
  // sample rate = 100 / 2^(8- val(acc_odr))

  BMI160AccODR acc_odr = 0;
  if (BMI160SampleRate_12p5_HZ == sample_rate) {
    acc_odr = BMI160AccODR_12p5_HZ;
  } else if (BMI160SampleRate_25_HZ == sample_rate) {
    acc_odr = BMI160AccODR_25_HZ;
  } else if (BMI160SampleRate_50_HZ == sample_rate) {
    acc_odr = BMI160AccODR_50_HZ;
  } else if (BMI160SampleRate_100_HZ == sample_rate) {
    acc_odr = BMI160AccODR_100_HZ;
  } else if (BMI160SampleRate_200_HZ == sample_rate) {
    acc_odr = BMI160AccODR_200_HZ;
  } else if (BMI160SampleRate_400_HZ == sample_rate) {
    acc_odr = BMI160AccODR_400_HZ;
  } else if (BMI160SampleRate_800_HZ == sample_rate) {
    acc_odr = BMI160AccODR_800_HZ;
  } else { // any interval < min supported must saturate to 1600Hz
    acc_odr = BMI160AccODR_1600_HZ;
  }

  return acc_odr;
}

static BMI160SampleRate prv_get_supported_sample_rate(uint32_t interval_us) {
  BMI160SampleRate sample_rate;
  if (BMI160SampleRate_12p5_HZ <= interval_us) {
    sample_rate = BMI160SampleRate_12p5_HZ;
  } else if (BMI160SampleRate_25_HZ <= interval_us) {
    sample_rate = BMI160SampleRate_25_HZ;
  } else if (BMI160SampleRate_50_HZ <= interval_us) {
    sample_rate = BMI160SampleRate_50_HZ;
  } else if (BMI160SampleRate_100_HZ <= interval_us) {
    sample_rate = BMI160SampleRate_100_HZ;
  } else if (BMI160SampleRate_200_HZ <= interval_us) {
    sample_rate = BMI160SampleRate_200_HZ;
  } else if (BMI160SampleRate_400_HZ <= interval_us) {
    sample_rate = BMI160SampleRate_400_HZ;
  } else if (BMI160SampleRate_800_HZ <= interval_us) {
    sample_rate = BMI160SampleRate_800_HZ;
  } else { // any interval < min supported must saturate to 1600Hz
    sample_rate = BMI160SampleRate_1600_HZ;
  }
  return sample_rate;
}

static void prv_configure_operating_mode(void) {
  BMI160SampleRate interval_us = prv_get_min_sampling_interval_us();
  uint8_t acc_odr = (uint8_t)prv_get_odr(interval_us);

  // should be able to write the sample range at any time
  prv_read_modify_write(BMI160_REG_ACC_CONF, acc_odr,
      BMI160_ACC_CONF_ACC_ODR_MASK << BMI160_ACC_CONF_ACC_ODR_SHIFT);

#ifdef BMI160_DEBUG
  PBL_ASSERTN(interval_us == prv_get_sampling_interval_from_hw());
#endif

  BMI160_DBG("Set sampling rate to %"PRIu32, (uint32_t)(1000000/interval_us));

  if (s_accel_power_mode == BMI160_Accel_Mode_Normal) {
    // This should only execute on startup or if the power mode is left in normal power mode for
    // some reason
    prv_accel_enable_undersampling(true);
    bmi160_set_accel_power_mode(BMI160_Accel_Mode_Low);
    BMI160_DBG("%s", "Enable low power mode");
  }

  // TODO: If we aren't doing anything else, suspend the chip?
}

static void prv_enable_operating_mode(AccelOperatingMode mode, BMI160SampleRate sample_rate) {
  s_operating_states[mode].enabled = true;
  s_operating_states[mode].sample_interval = sample_rate;
  prv_configure_operating_mode();
}

static void prv_disable_operating_mode(AccelOperatingMode mode) {
  s_operating_states[mode].enabled = false;
  prv_configure_operating_mode();
}

uint32_t accel_set_sampling_interval(uint32_t interval_us) {
  BMI160SampleRate sample_rate = prv_get_supported_sample_rate(interval_us);
  prv_enable_operating_mode(AccelOperatingModeData, sample_rate);
  return prv_get_min_sampling_interval_us();
}

uint32_t accel_get_sampling_interval(void) {
  uint32_t curr_sampling_interval_us = prv_get_min_sampling_interval_us();
#ifdef BMI160_DEBUG
  PBL_ASSERTN(curr_sampling_interval_us == prv_get_sampling_interval_from_hw());
#endif
  return curr_sampling_interval_us;
}

static void prv_configure_accel_sampling(bool enable_int, bool use_fifo) {
  static bool int_enabled = false;

  if ((int_enabled == enable_int) && (s_fifo_in_use == use_fifo)) {
    return; // No changes to make so don't redo writes
  }

  uint8_t map_mask =
      BMI160_INT_MAP_1_INT2_DATA_READY | BMI160_INT_MAP_1_INT2_FIFO_WATERMARK;
  uint8_t int_en_mask =
      BMI160_INT_EN_1_DRDY_EN | BMI160_INT_EN_1_FWM_EN;

  uint8_t map_val;
  uint8_t int_en_val;
  if (!enable_int) {
    map_val = int_en_val = 0;
  } else if (!use_fifo) {
    map_val = BMI160_INT_MAP_1_INT2_DATA_READY;
    int_en_val = BMI160_INT_EN_1_DRDY_EN;
  } else {
    map_val = BMI160_INT_MAP_1_INT2_FIFO_WATERMARK;
    int_en_val = BMI160_INT_EN_1_FWM_EN;
  }

  prv_read_modify_write(BMI160_REG_INT_MAP_1, map_val, map_mask);
  prv_read_modify_write(BMI160_REG_INT_EN_1, int_en_val, int_en_mask);

  BMI160_DBG("INT_MAP[1] = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_MAP_1));
  BMI160_DBG("INT_EN[1] = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_INT_EN_1));
  int_enabled = enable_int;
  s_fifo_in_use = use_fifo;
}

void accel_set_num_samples(uint32_t num_samples) {
  uint8_t fifo_frame_size = prv_get_fifo_frame_size(); // x,y,z accel data
  uint16_t max_num_samples = (BMI160_FIFO_LEN_BYTES / fifo_frame_size);
  max_num_samples = (max_num_samples / BMI160_FIFO_WM_UNIT_BYTES) *
      BMI160_FIFO_WM_UNIT_BYTES;

  if (num_samples > max_num_samples) {
    num_samples = max_num_samples;
  }

  uint16_t curr_sample_size = bmi160_read_reg(BMI160_REG_FIFO_CONFIG_0) *
      BMI160_FIFO_WM_UNIT_BYTES;
  if (curr_sample_size > num_samples) { // flush what we have in the fifo, if any
    BMI160_DBG("Curr Sample Size = %"PRId16, curr_sample_size);
    prv_drain_fifo();
  }

  if (num_samples < 2) {
    prv_write_reg(BMI160_REG_FIFO_CONFIG_1, 0); // power down the fifo
    curr_fifo_num_samples_wm = 0;
  } else {
    curr_fifo_num_samples_wm = num_samples;
    // Set the new fifo watermark
    // TODO: I think we will want to try and make this a multiple of the frame size
    uint8_t fifo_wm = (num_samples * fifo_frame_size) / BMI160_FIFO_WM_UNIT_BYTES;
    prv_write_reg(BMI160_REG_FIFO_CONFIG_0, fifo_wm);
    BMI160_DBG("FWM = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_FIFO_CONFIG_0));

    uint8_t curr_frame_cfg = bmi160_read_reg(BMI160_REG_FIFO_CONFIG_1);
    uint8_t desired_cfg = BMI160_FIFO_CONFIG_1_ACC_EN;

    if (curr_frame_cfg != desired_cfg) {
      prv_run_command(BMI160_CMD_FIFO_FLUSH); // clear any lingering entries
      prv_write_reg(BMI160_REG_FIFO_CONFIG_1, desired_cfg);
    }
  }
  bool enable_int = num_samples != 0;
  bool use_fifo = num_samples > 1;
  prv_configure_accel_sampling(enable_int, use_fifo);
}

int accel_peek(AccelDriverSample *data) {
  prv_read_curr_accel_data(data);
  return 0;
}

void accel_set_shake_sensitivity_high(bool sensitivity_high) {
  // Configure the threshold level at which the BMI160 will think shake has occurred
  if (sensitivity_high) {
    prv_write_reg(BMI160_REG_INT_MOTION_1,
        BOARD_CONFIG_ACCEL.accel_config.shake_thresholds[AccelThresholdLow]);
  } else {
    prv_write_reg(BMI160_REG_INT_MOTION_1,
        BOARD_CONFIG_ACCEL.accel_config.shake_thresholds[AccelThresholdHigh]);
  }
}

static void prv_enable_shake_detection(void) {

  // don't automatically power-up the gryo when an anymotion interrupt fires!
  prv_write_reg(BMI160_REG_PMU_TRIGGER, 0x0);

  // Enable the anymotion detection interrupt
  uint8_t en_val = BMI160_INT_MAP_ANYMOTION_EN_MASK;
  prv_read_modify_write(BMI160_REG_INT_MAP_0, en_val, en_val);

  // Actually enable the anymotion interrupt
  uint8_t int_en = (BMI160_INT_EN_0_ANYMOTION_Z_EN |
      BMI160_INT_EN_0_ANYMOTION_Y_EN | BMI160_INT_EN_0_ANYMOTION_X_EN);
  prv_read_modify_write(BMI160_REG_INT_EN_0, int_en, int_en);

  // configure the anymotion interrupt to fire after 4 successive
  // samples are over the threhold specified
  accel_set_shake_sensitivity_high(false /* sensitivity_high */);
  prv_write_reg(BMI160_REG_INT_MOTION_0,
      0x3 << BMI160_INT_MOTION_1_ANYM_DUR_SHIFT);

  // We temporarily latch the interrupt & do not clear it for anymotion interrupts to
  // limit the number of anymotion interrupts to 1 per / 1.28 seconds
  prv_write_reg(BMI160_REG_INT_LATCH, 0xd);

  prv_enable_operating_mode(AccelOperatingModeShakeDetection, BMI160SampleRate_25_HZ);

  BMI160_DBG("ACC_CONF = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_ACC_CONF));
}

static void prv_disable_shake_detection(void) {
  // don't worry about the configuration registers but disable interrupts
  // generated for taps from the accel

  uint8_t dis_mask = BMI160_INT_MAP_ANYMOTION_EN_MASK;
  prv_read_modify_write(BMI160_REG_INT_MAP_0, 0, dis_mask);

  dis_mask = (BMI160_INT_EN_0_ANYMOTION_X_EN |  BMI160_INT_EN_0_ANYMOTION_Y_EN |
      BMI160_INT_EN_0_ANYMOTION_Z_EN);
  prv_read_modify_write(BMI160_REG_INT_EN_0, 0x0, dis_mask);

  prv_disable_operating_mode(AccelOperatingModeShakeDetection);
}

static void prv_enable_double_tap_detection(void) {
  uint8_t tap_0_cfg =
      (0x1 << BMI160_INT_TAP_QUIET_SHIFT) | // 0 = 20 ms, 1 = 30ms
      (0x1 << BMI160_INT_TAP_SHOCK_SHIFT) | // 0 = 50 ms, 1 = 75ms
      (0x4 << BMI160_INT_TAP_DUR_SHIFT);    // 4 = 300 ms
  prv_write_reg(BMI160_REG_INT_TAP_0, tap_0_cfg);

  // get the current scale
  Bmi160Scale scale = prv_get_accel_scale();

  // TODO: 4 or 5 bit granularity? - data sheet ambiguous, assume 5
  uint32_t threshold = BOARD_CONFIG_ACCEL.accel_config.double_tap_threshold;
  const uint32_t STEP = 625;      // 62.5 mg step at 2g range

  // calculate setting for 2g, then scale to higher g
  uint32_t setting = (threshold / STEP) / (scale / 2);
  uint8_t tap_1_cfg = (uint8_t)setting;
  prv_write_reg(BMI160_REG_INT_TAP_1, tap_1_cfg);

  // Enable the double tap detection interrupt
  uint8_t en_val = BMI160_INT_MAP_DOUBLE_TAP_EN_MASK;
  prv_read_modify_write(BMI160_REG_INT_MAP_0, en_val, en_val);

  // Actually enable the double tap interrupt
  uint8_t int_en = BMI160_INT_EN_0_D_TAP_EN;
  prv_read_modify_write(BMI160_REG_INT_EN_0, int_en, int_en);

  BMI160_DBG("ACC_CONF = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_ACC_CONF));
  prv_enable_operating_mode(AccelOperatingModeDoubleTapDetection, BMI160SampleRate_200_HZ);
  BMI160_DBG("ACC_CONF = 0x%"PRIx8, bmi160_read_reg(BMI160_REG_ACC_CONF));
}

static void prv_disable_double_tap_detection(void) {
  uint8_t dis_mask = BMI160_INT_MAP_SINGLE_TAP_EN_MASK | BMI160_INT_MAP_DOUBLE_TAP_EN_MASK;
  prv_read_modify_write(BMI160_REG_INT_MAP_0, 0, dis_mask);

  dis_mask = BMI160_INT_EN_0_S_TAP_EN | BMI160_INT_EN_0_D_TAP_EN;
  prv_read_modify_write(BMI160_REG_INT_EN_0, 0x0, dis_mask);

  prv_disable_operating_mode(AccelOperatingModeDoubleTapDetection);
}

void accel_enable_shake_detection(bool on) {
  PBL_LOG(LOG_LEVEL_DEBUG, "enable shake detection %d", on);
  if (s_shake_detection_enabled == on) {
    // the requested change matches what we already have!
    return;
  }

  prv_update_accel_interrupts(on);
  if (on) {
    prv_enable_shake_detection();
  } else {
    prv_disable_shake_detection();
  }

  s_shake_detection_enabled = on;
}

void accel_enable_double_tap_detection(bool on) {
  PBL_LOG(LOG_LEVEL_DEBUG, "enable double tap detection %d", on);
  if (s_double_tap_detection_enabled == on) {
    // the requested change matches what we already have!
    return;
  }

  prv_update_accel_interrupts(on);
  if (on) {
    prv_enable_double_tap_detection();
  } else {
    prv_disable_double_tap_detection();
  }

  s_double_tap_detection_enabled = on;
}

bool accel_get_shake_detection_enabled(void) {
  return s_shake_detection_enabled;
}

bool accel_get_double_tap_detection_enabled(void) {
  return s_double_tap_detection_enabled;
}

void accel_toggle_power_mode(void) {
  static bool enable_low_power = false;
  prv_accel_enable_undersampling(enable_low_power);
  enable_low_power = !enable_low_power;
}
