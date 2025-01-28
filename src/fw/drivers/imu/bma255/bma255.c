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

#include "bma255.h"
#include "bma255_regs.h"
#include "bma255_private.h"

#include "console/prompt.h"
#include "drivers/accel.h"
#include "drivers/exti.h"
#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "drivers/rtc.h"
#include "drivers/spi.h"
#include "kernel/util/delay.h"
#include "kernel/util/sleep.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/units.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#include <mcu.h>

#define BMA255_DEBUG 0

#if BMA255_DEBUG
#define BMA255_DBG(msg, ...) \
  do { \
    PBL_LOG(LOG_LEVEL_DEBUG, msg, __VA_ARGS__); \
  } while (0);
#else
#define BMA255_DBG(msg, ...)
#endif


#define SELFTEST_SIGN_POSITIVE (0x1 << 2)
#define SELFTEST_SIGN_NEGATIVE (0x0)

// The BMA255 is capable of storing up to 32 frames.
// Conceptually each frame consists of three 16-bit words corresponding to the x, y and z- axis.
#define BMA255_FIFO_MAX_FRAMES        (32)
#define BMA255_FIFO_FRAME_SIZE_BYTES  (3 * 2)
#define BMA255_FIFO_SIZE_BYTES        (BMA255_FIFO_MAX_FRAMES * BMA255_FIFO_FRAME_SIZE_BYTES)


// Driver state
static BMA255PowerMode s_accel_power_mode = BMA255PowerMode_Normal;
static bool s_fifo_is_enabled = false;
static bool s_shake_detection_enabled = false;
static bool s_accel_outstanding_motion_work = false;
static bool s_accel_outstanding_data_work = false;
static bool s_fifo_overrun_detected = false;


// Forward declarations
static void prv_configure_operating_mode(void);
static void prv_bma255_IRQ1_handler(bool *should_context_switch);
static void prv_bma255_IRQ2_handler(bool *should_context_switch);
static void prv_set_accel_power_mode(BMA255PowerMode mode);


// The BMA255 reports each G in powers of 2 with full deflection at +-2^11
// So scale all readings by (scale)/(2^11) to get G
// And scale the result by 1000 to allow for easier integer math
typedef enum {
  BMA255Scale_2G  = 980,  // 2000/2048
  BMA255Scale_4G  = 1953, // 4000/2048
  BMA255Scale_8G  = 3906, // 8000/2048
  BMA255Scale_16G = 7813, // 16000/2048
} BMA255Scale;

static int16_t s_raw_unit_to_mgs = BMA255Scale_2G;

typedef enum {
  AccelOperatingMode_Data,
  AccelOperatingMode_ShakeDetection,
  AccelOperatingMode_DoubleTapDetection,

  AccelOperatingModeCount,
} AccelOperatingMode;

static struct {
  bool enabled;
  bool using_interrupts;
  BMA255ODR odr;
} s_operating_states[AccelOperatingModeCount] = {
  [AccelOperatingMode_Data] = {
    .enabled = false,
    .using_interrupts = false,
    .odr = BMA255ODR_125HZ,
  },
  [AccelOperatingMode_ShakeDetection] = {
    .enabled = false,
    .using_interrupts = false,
    .odr = BMA255ODR_125HZ,
  },
  [AccelOperatingMode_DoubleTapDetection] = {
    .enabled = false,
    .using_interrupts = false,
    .odr = BMA255ODR_125HZ,
  },
};

void bma255_init(void) {
  bma255_gpio_init();
  if (!bma255_query_whoami()) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to query BMA255");
    return;
  }
  const bool pass = bma255_selftest();
  if (pass) {
    PBL_LOG(LOG_LEVEL_DEBUG, "BMA255 self test pass, all 3 axis");
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "BMA255 self test failed one or more axis");
  }

  // Workaround to fix FIFO Frame Leakage: Disable temperature sensor (we're not using it anyways)
  // See Section 2.2.1 of https://drive.google.com/a/pebble.com/file/d/0B9tTN3OlYns3bEZaczdoZUU3UEk/view
  bma255_write_register(BMA255Register_EXTENDED_MEMORY_MAP, BMA255_EXTENDED_MEMORY_MAP_OPEN);
  bma255_write_register(BMA255Register_EXTENDED_MEMORY_MAP, BMA255_EXTENDED_MEMORY_MAP_OPEN);
  bma255_write_register(BMA255Register_TEMPERATURE_SENSOR_CTRL, BMA255_TEMPERATURE_SENSOR_DISABLE);
  bma255_write_register(BMA255Register_EXTENDED_MEMORY_MAP, BMA255_EXTENDED_MEMORY_MAP_CLOSE);

  exti_configure_pin(BOARD_CONFIG_ACCEL.accel_ints[0], ExtiTrigger_Rising, prv_bma255_IRQ1_handler);
  exti_configure_pin(BOARD_CONFIG_ACCEL.accel_ints[1], ExtiTrigger_Rising, prv_bma255_IRQ2_handler);
}

bool bma255_query_whoami(void) {
  const uint8_t chip_id = bma255_read_register(BMA255Register_BGW_CHIP_ID);
  PBL_LOG(LOG_LEVEL_DEBUG, "Read BMA255 whoami byte 0x%"PRIx8", expecting 0x%"PRIx8,
          chip_id, BMA255_CHIP_ID);
  return (chip_id == BMA255_CHIP_ID);
}

static uint64_t prv_get_curr_system_time_ms(void) {
  time_t time_s;
  uint16_t time_ms;
  rtc_get_time_ms(&time_s, &time_ms);
  return (((uint64_t)time_s) * 1000 + time_ms);
}

void bma255_set_scale(BMA255Scale scale) {
  uint8_t value = 0;
  switch (scale) {
    case BMA255Scale_2G:
      value = 0x3;
      break;
    case BMA255Scale_4G:
      value = 0x5;
      break;
    case BMA255Scale_8G:
      value = 0x8;
      break;
    case BMA255Scale_16G:
      value = 0xc;
      break;
    default:
      WTF;
  }
  bma255_write_register(BMA255Register_PMU_RANGE, value);
  s_raw_unit_to_mgs = scale;
}

static int16_t prv_raw_to_mgs(int16_t raw_val) {
  int16_t mgs = ((int32_t)raw_val * s_raw_unit_to_mgs) / 1000;
  return mgs;
}

static int16_t prv_conv_raw_to_12bit(const uint8_t registers[2]) {
  int16_t reading = ((registers[0] >> 4) & 0x0F) | ((int16_t)registers[1] << 4);
  if (reading & 0x0800) {
    reading |= 0xF000;
  }
  return reading;
}

static void prv_convert_accel_raw_data_to_mgs(const uint8_t *buf, AccelDriverSample *data) {
  int16_t readings[3];
  for (int i = 0; i < 3; ++i) {
    readings[i] = prv_conv_raw_to_12bit(&buf[i*2]);
  }
  const AccelConfig *cfg = &BOARD_CONFIG_ACCEL.accel_config;
  *data = (AccelDriverSample) {
      .x = (cfg->axes_inverts[AXIS_X] ? -1 : 1) *
          prv_raw_to_mgs(readings[cfg->axes_offsets[AXIS_X]]),
      .y = (cfg->axes_inverts[AXIS_Y] ? -1 : 1) *
          prv_raw_to_mgs(readings[cfg->axes_offsets[AXIS_Y]]),
      .z = (cfg->axes_inverts[AXIS_Z] ? -1 : 1) *
          prv_raw_to_mgs(readings[cfg->axes_offsets[AXIS_Z]]),
  };
}

static void prv_read_curr_accel_data(AccelDriverSample *data) {
  uint8_t raw_buf[BMA255_FIFO_FRAME_SIZE_BYTES];
  bma255_burst_read(BMA255Register_ACCD_X_LSB, raw_buf, sizeof(raw_buf));

  prv_convert_accel_raw_data_to_mgs(raw_buf, data);
  // FIXME: assuming the timestamp on the samples is NOW! PBL-33765
  data->timestamp_us = prv_get_curr_system_time_ms() * 1000;

  BMA255_DBG("%"PRId16" %"PRId16" %"PRId16, data->x, data->y, data->z);
}

typedef enum {
  BMA255Axis_X = 0,
  BMA255Axis_Y,
  BMA255Axis_Z,
} BMA255Axis;

static void prv_drain_fifo(void) {
  // TODO: I think the ideal thing to do here would be to invoke the accel_cb_new_sample() while
  // the SPI transaction is in progress so we don't need a static ~500 byte buffer. (This is what
  // we do in the bmi160 driver) However, since we are oversampling super aggressively with the
  // bma255, I'm concerned about changing the timing of how fast we drain things. Thus, just use a
  // static buffer for now. This should be safe because only one thread should be draining the data.
  static AccelDriverSample data[BMA255_FIFO_MAX_FRAMES];
  const uint64_t timestamp_us = prv_get_curr_system_time_ms() * 1000;
  const uint32_t sampling_interval_us = accel_get_sampling_interval();

  uint8_t fifo_status = bma255_read_register(BMA255Register_FIFO_STATUS);
  BMA255_DBG("Drain %"PRIu8" samples", num_samples_available);

  const uint8_t num_samples_available = fifo_status & 0x3f;
  if (num_samples_available == 0) {
    return;
  }

  bma255_prepare_txn(BMA255Register_FIFO_DATA | BMA255_READ_FLAG);
  for (int i = 0; i < num_samples_available; ++i) {
    uint8_t buf[BMA255_FIFO_FRAME_SIZE_BYTES];
    for (int j = 0; j < BMA255_FIFO_FRAME_SIZE_BYTES; ++j) {
      buf[j] = bma255_send_and_receive_byte(0);
    }
    prv_convert_accel_raw_data_to_mgs(buf, &data[i]);
  }
  bma255_end_txn();

  // Timestamp & Dispatch data
  for (int i = 0; i < num_samples_available; ++i) {
    // Make a timestamp approximation based on the current time, the sample
    // being processed and the sampling interval.
    data[i].timestamp_us = timestamp_us - ((num_samples_available - i) * sampling_interval_us);
    BMA255_DBG("%2d: %"PRId16" %"PRId16" %"PRId16" %"PRIu32,
               i, data[i].x, data[i].y, data[i].z, (uint32_t)data[i].timestamp_us);
    accel_cb_new_sample(&data[i]);
  }

  // clear of fifo overrun flag must happen after draining samples, also the samples available will
  // get drained too!
  if ((fifo_status & 0x80) && !s_fifo_overrun_detected) {
    s_fifo_overrun_detected = true;
    // We don't clear the interrupt here because you are only supposed to touch the fifo config
    // registers while in standby mode.
    PBL_LOG(LOG_LEVEL_WARNING, "bma255 fifo overrun detected: 0x%x!", fifo_status);
  }
}

static void prv_handle_data(void) {
  s_accel_outstanding_data_work = false;
  if (s_fifo_is_enabled) {
    prv_drain_fifo();
    return;
  }

  AccelDriverSample data;
  prv_read_curr_accel_data(&data);
  accel_cb_new_sample(&data);
}

static void prv_handle_motion_interrupts(void) {
  s_accel_outstanding_motion_work = false;

  const uint8_t int0_status = bma255_read_register(BMA255Register_INT_STATUS_0);
  const uint8_t int2_status = bma255_read_register(BMA255Register_INT_STATUS_2);

  bool anymotion = (int0_status & BMA255_INT_STATUS_0_SLOPE_MASK);
  if (anymotion) {
    const AccelConfig *cfg = &BOARD_CONFIG_ACCEL.accel_config;
    IMUCoordinateAxis axis = AXIS_X;
    bool invert = false;

    if (int2_status & BMA255_INT_STATUS_2_SLOPE_FIRST_X) {
      axis = AXIS_X;
      invert = cfg->axes_inverts[AXIS_X];
    } else if (int2_status & BMA255_INT_STATUS_2_SLOPE_FIRST_Y) {
      axis = AXIS_Y;
      invert = cfg->axes_inverts[AXIS_Y];
    } else if (int2_status & BMA255_INT_STATUS_2_SLOPE_FIRST_Z) {
      axis = AXIS_Z;
      invert = cfg->axes_inverts[AXIS_Z];
    } else {
      BMA255_DBG("No Axis?: 0x%"PRIx8" 0x%"PRIx8, int0_status, int2_status);
    }
    int32_t direction = ((int2_status & BMA255_INT_STATUS_2_SLOPE_SIGN) == 0) ? 1 : -1;
    direction *= (invert ? -1 : 1);

    accel_cb_shake_detected(axis, direction);
  }
}

// Services tap/motion interrupts
static void prv_bma255_IRQ1_handler(bool *should_context_switch) {
  BMA255_DBG("Slope Int");
  if (!s_accel_outstanding_motion_work) {
    s_accel_outstanding_motion_work = true;
    accel_offload_work_from_isr(prv_handle_motion_interrupts, should_context_switch);
  }
}

// Services data / fifo interrupts.
// NOTE: The BMA255 Errata specifically states that we should keep the fifo /
//       data interrupt on INT2 to avoid "data inconsistencies" which arise when
//       we have it fifo / data interrupt on INT1.
static void prv_bma255_IRQ2_handler(bool *should_context_switch) {
  BMA255_DBG("Data Int");
  if (!s_accel_outstanding_data_work) {
    s_accel_outstanding_data_work = true;
    accel_offload_work_from_isr(prv_handle_data, should_context_switch);
  }
}

static void prv_update_accel_interrupts(bool enable, AccelOperatingMode mode) {
  s_operating_states[mode].using_interrupts = enable;

  bool enable_interrupts = false;
  for (uint32_t i = 0; i < ARRAY_LENGTH(s_operating_states); i++) {
    if (s_operating_states[i].using_interrupts) {
      enable_interrupts = true;
      break;
    }
  }

  if (enable_interrupts) {
    exti_enable(BOARD_CONFIG_ACCEL.accel_ints[0]);
    exti_enable(BOARD_CONFIG_ACCEL.accel_ints[1]);
  } else {
    exti_disable(BOARD_CONFIG_ACCEL.accel_ints[0]);
    exti_disable(BOARD_CONFIG_ACCEL.accel_ints[1]);
  }
}

uint32_t accel_get_sampling_interval(void) {
  BMA255ODR odr_max = 0;
  for (uint32_t i = 0; i < ARRAY_LENGTH(s_operating_states); i++) {
    if (s_operating_states[i].enabled) {
      odr_max = MAX(odr_max, s_operating_states[i].odr);
    }
  }

  uint32_t sample_interval = 0;
  switch (odr_max) {
    case BMA255ODR_1HZ:
      sample_interval = BMA255SampleInterval_1HZ;
      break;
    case BMA255ODR_10HZ:
      sample_interval = BMA255SampleInterval_10HZ;
      break;
    case BMA255ODR_19HZ:
      sample_interval = BMA255SampleInterval_19HZ;
      break;
    case BMA255ODR_83HZ:
      sample_interval = BMA255SampleInterval_83HZ;
      break;
    case BMA255ODR_125HZ:
      sample_interval = BMA255SampleInterval_125HZ;
      break;
    case BMA255ODR_166HZ:
      sample_interval = BMA255SampleInterval_166HZ;
      break;
    case BMA255ODR_250HZ:
      sample_interval = BMA255SampleInterval_250HZ;
      break;
    default:
      WTF;
  }
  return sample_interval;
}

//! Set the LOW_POWER and LPW registers as required.
//! The LPW register is masked because it contains the sleep duration for our desired ODR.
static void prv_enter_power_mode(BMA255PowerMode mode) {
  bma255_write_register(BMA255Register_PMU_LOW_POWER,
                           s_power_mode[mode].low_power << BMA255_LOW_POWER_SHIFT);
  bma255_read_modify_write(BMA255Register_PMU_LPW,
                           s_power_mode[mode].lpw << BMA255_LPW_POWER_SHIFT,
                           BMA255_LPW_POWER_MASK);

  // Workaround for error in transition to Suspend / Standby
  if (mode == BMA255PowerMode_Suspend || mode == BMA255PowerMode_Standby) {
      // Write to FIFO_CONFIG_1 to exit some unknown "intermittent state"
      // NOTE: This will clear the FIFO & FIFO status.
      bma255_read_modify_write(BMA255Register_FIFO_CONFIG_1, 0, 0);
  }
}

static void prv_set_accel_power_mode(BMA255PowerMode mode) {
  PBL_ASSERTN(mode == BMA255PowerMode_Normal ||
              mode == BMA255PowerMode_LowPower1 ||
              mode == BMA255PowerMode_Standby);

  // Workaround for entering Normal Mode
  // LPM1 => Normal requires us to go through Suspend mode
  // LPM2 => Normal requires us to go through Standby mode
  if (mode == BMA255PowerMode_Normal) {
    if (s_accel_power_mode == BMA255PowerMode_LowPower1) {
      prv_enter_power_mode(BMA255PowerMode_Suspend);
    } else if (s_accel_power_mode == BMA255PowerMode_LowPower2) {
      prv_enter_power_mode(BMA255PowerMode_Standby);
    }
  }

  prv_enter_power_mode(mode);

  BMA255_DBG("BMA555: power level set to: 0x%x and 0x%x",
             bma255_read_register(BMA255Register_PMU_LPW),
             bma255_read_register(BMA255Register_PMU_LOW_POWER));

  s_accel_power_mode = mode;
}

static BMA255ODR prv_get_odr(BMA255SampleInterval sample_interval) {
  BMA255ODR odr = 0;
  switch (sample_interval) {
    case BMA255SampleInterval_1HZ:
      odr = BMA255ODR_1HZ;
      break;
    case BMA255SampleInterval_10HZ:
      odr = BMA255ODR_10HZ;
      break;
    case BMA255SampleInterval_19HZ:
      odr = BMA255ODR_19HZ;
      break;
    case BMA255SampleInterval_83HZ:
      odr = BMA255ODR_83HZ;
      break;
    case BMA255SampleInterval_125HZ:
      odr = BMA255ODR_125HZ;
      break;
    case BMA255SampleInterval_166HZ:
      odr = BMA255ODR_166HZ;
      break;
    case BMA255SampleInterval_250HZ:
      odr = BMA255ODR_250HZ;
      break;
    default:
      WTF;
  }
  return odr;
}

static BMA255SampleInterval prv_get_supported_sampling_interval(uint32_t interval_us) {
  BMA255SampleInterval sample_interval;
  if (BMA255SampleInterval_1HZ <= interval_us) {
    sample_interval = BMA255SampleInterval_1HZ;
  } else if (BMA255SampleInterval_10HZ <= interval_us) {
    sample_interval = BMA255SampleInterval_10HZ;
  } else if (BMA255SampleInterval_19HZ <= interval_us) {
    sample_interval = BMA255SampleInterval_19HZ;
  } else if (BMA255SampleInterval_83HZ <= interval_us) {
    sample_interval = BMA255SampleInterval_83HZ;
  } else if (BMA255SampleInterval_125HZ <= interval_us) {
    sample_interval = BMA255SampleInterval_125HZ;
  } else if (BMA255SampleInterval_166HZ <= interval_us) {
    sample_interval = BMA255SampleInterval_166HZ;
  } else if (BMA255SampleInterval_250HZ <= interval_us) {
    sample_interval = BMA255SampleInterval_250HZ;
  } else {
    sample_interval = BMA255SampleInterval_250HZ;
  }
  return sample_interval;
}

static void prv_enable_operating_mode(AccelOperatingMode mode,
                                      BMA255SampleInterval sample_interval) {
  s_operating_states[mode].enabled = true;
  s_operating_states[mode].odr = prv_get_odr(sample_interval);
  prv_configure_operating_mode();
}

static void prv_disable_operating_mode(AccelOperatingMode mode) {
  s_operating_states[mode].enabled = false;
  prv_configure_operating_mode();
}

uint32_t accel_set_sampling_interval(uint32_t interval_us) {
  BMA255SampleInterval actual_interval = prv_get_supported_sampling_interval(interval_us);

  // FIXME: For now, tie us to 125Hz. 125Hz is a rate that is easy enough to
  // subsample to all of our supported accel service rates, and also cuts down power consumption
  // from the 140uA range to 100uA.
  // Being able to sample at a lower rate like 38Hz will be able to get us down into the 40uA range.
  //
  // By forcing a sample interval of 125Hz here, we will never use a different
  // rate, and the accelerometer service will be made aware of our running rate.
  actual_interval = BMA255SampleInterval_125HZ;

  prv_enable_operating_mode(AccelOperatingMode_Data, actual_interval);

  return accel_get_sampling_interval();
}

static void prv_configure_operating_mode(void) {
  BMA255SampleInterval interval_us = accel_get_sampling_interval();
  const uint8_t odr = (uint8_t)prv_get_odr(interval_us);
  const uint8_t bw = s_odr_settings[odr].bw;
  const uint8_t tsleep = s_odr_settings[odr].tsleep;

  // Set the BW and TSleep to get the ODR we expect.
  bma255_write_register(BMA255Register_PMU_BW, bw);
  bma255_read_modify_write(BMA255Register_PMU_LPW,
                           tsleep << BMA255_LPW_SLEEP_DUR_SHIFT,
                           BMA255_LPW_SLEEP_DUR_MASK);

  PBL_LOG(LOG_LEVEL_DEBUG, "Set sampling rate to %"PRIu32, (uint32_t)(1000000/interval_us));

  if (s_accel_power_mode == BMA255PowerMode_Normal) {
    // This should only execute on startup or if the power mode
    // is left in normal power mode for some reason
    PBL_LOG(LOG_LEVEL_DEBUG, "Enable low power mode");
    prv_set_accel_power_mode(BMA255PowerMode_LowPower1);
  }
}

int accel_peek(AccelDriverSample *data) {
  prv_read_curr_accel_data(data);
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// FIFO Support
////////////////////////////////////////////////////////////////////////////////

static void prv_program_fifo_register(uint8_t address, uint8_t data) {
  // To prevent lockups of the fifo, the fifo config registers should only be programmed
  // while in standby mode
  PBL_ASSERTN(s_accel_power_mode == BMA255PowerMode_Standby);
  const int retries = 2;
  uint8_t value;
  for (int i = 0; i <= retries; i++) {
    bma255_write_register(address, data);
    value = bma255_read_register(address);
    if (value == data) {
      return; // Write took, we are good
    }
    PBL_LOG(LOG_LEVEL_DEBUG, "FIFO config write failed, initiating workaround ...");

    // FIXME: Sometimes writes to the FIFO registers fail. I am suspicious that the bma255 enters
    // suspend mode instead of standby mode. (The datasheet states that FIFO_CONFIG registers
    // accesses fail in suspend mode). It seems like the issue can be worked around by attempting
    // to enter standby mode again. Hopefully, bosch can illuminate for us what is going on here
    // but in the meantime let's use this workaround.
    prv_set_accel_power_mode(BMA255PowerMode_Normal);
    prv_set_accel_power_mode(BMA255PowerMode_Standby);
  }

  PBL_LOG(LOG_LEVEL_WARNING, "Failed to program fifo reg, 0x%"PRIx8" = 0x%"PRIx8, address, data);
}

static void prv_set_fifo_mode(BMA255FifoMode mode) {
  BMA255_DBG("Set Fifo Mode: 0x%x", mode);
  const uint8_t out =
      (mode << BMA255_FIFO_MODE_SHIFT) | (BMA255FifoDataSel_XYZ << BMA255_FIFO_DATA_SEL_SHIFT);
  prv_program_fifo_register(BMA255Register_FIFO_CONFIG_1, out);
  // If the fifo had overflowed, the write above will have cleared the flag
  s_fifo_overrun_detected = false;
}

static void prv_configure_fifo_interrupts(bool enable_int, bool use_fifo) {
  BMA255_DBG("Enabling FIFO Interrupts: %d %d", (int)enable_int, (int)use_fifo);
  uint8_t map_value;
  uint8_t en_value;
  if (!enable_int) {
    map_value = 0;
    en_value = 0;
  } else if (!use_fifo) {
    map_value = BMA255_INT_MAP_1_INT2_DATA;
    en_value = BMA255_INT_EN_1_DATA;
  } else {
    map_value = BMA255_INT_MAP_1_INT2_FIFO_WATERMARK;
    en_value = BMA255_INT_EN_1_FIFO_WATERMARK;
  }

  bma255_write_register(BMA255Register_INT_MAP_1, map_value);
  bma255_write_register(BMA255Register_INT_EN_1, en_value);

  prv_update_accel_interrupts(enable_int, AccelOperatingMode_Data);
}

void accel_set_num_samples(uint32_t num_samples) {
  // Disable interrupts so they won't fire while we change sampling rate
  prv_configure_fifo_interrupts(false, false);

  // Workaround some bma255 issues:
  // Need to use Standby Mode to read/write the FIFO_CONFIG registers.
  prv_set_accel_power_mode(BMA255PowerMode_Normal); // Need to transition to Normal first
  prv_set_accel_power_mode(BMA255PowerMode_Standby);

  if (num_samples > BMA255_FIFO_MAX_FRAMES) {
    num_samples = BMA255_FIFO_MAX_FRAMES;
  }
  BMA255_DBG("Setting num samples to: %"PRIu32, num_samples);

  // Note that with the bma255, we do not want to use Bypass mode when
  // collecting a single sample as this will result in uneven sampling.
  // The accelerometer will wake up, provide several samples in quick
  // succession, and then go back to sleep for a period. Looking at the INT2
  // line shows similar to this:
  //           _   _   _                        _   _   _
  // .... ____| |_| |_| |______________________| |_| |_| |_________ .....
  //
  // By using a FIFO of depth 1, the bma255 respects EST mode and will provide
  // samples at a predictable interval and rate.
  const bool use_fifo = (num_samples > 0);

  if (use_fifo) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Enabling FIFO");
    // Watermark is the number of samples to be collected
    prv_program_fifo_register(BMA255Register_FIFO_CONFIG_0, num_samples);
    prv_set_fifo_mode(BMA255FifoMode_Fifo);
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Disabling FIFO");
    prv_set_fifo_mode(BMA255FifoMode_Bypass);
  }

  prv_set_accel_power_mode(BMA255PowerMode_Normal);
  prv_set_accel_power_mode(BMA255PowerMode_LowPower1);

  const bool enable_int = (num_samples != 0);
  prv_configure_fifo_interrupts(enable_int, use_fifo);

  s_fifo_is_enabled = use_fifo;
}

////////////////////////////////////////////////////////////////////////////////
// Shake Detection
////////////////////////////////////////////////////////////////////////////////

static void prv_enable_shake_detection(void) {
  bma255_write_register(BMA255Register_INT_EN_0, BMA255_INT_EN_0_SLOPE_X_EN |
                                                 BMA255_INT_EN_0_SLOPE_Y_EN |
                                                 BMA255_INT_EN_0_SLOPE_Z_EN);

  bma255_write_register(BMA255Register_INT_MAP_0, BMA255_INT_MAP_0_INT1_SLOPE);

  // configure the anymotion interrupt to fire after 4 successive
  // samples are over the threhold specified
  accel_set_shake_sensitivity_high(false /* sensitivity_high */);
  bma255_write_register(BMA255Register_INT_5,
                        BMA255_INT_5_SLOPE_DUR_MASK << BMA255_INT_5_SLOPE_DUR_SHIFT);

  prv_enable_operating_mode(AccelOperatingMode_ShakeDetection, BMA255SampleInterval_83HZ);
}

static void prv_disable_shake_detection(void) {
  // Don't worry about the configuration registers but disable interrupts from the accel
  bma255_write_register(BMA255Register_INT_EN_0, 0);

  prv_disable_operating_mode(AccelOperatingMode_ShakeDetection);
}

void accel_enable_shake_detection(bool enable) {
  if (s_shake_detection_enabled == enable) {
    // the requested change matches what we already have!
    return;
  }
  PBL_LOG(LOG_LEVEL_DEBUG, "%s shake detection", enable ? "Enabling" : "Disabling");

  prv_update_accel_interrupts(enable, AccelOperatingMode_ShakeDetection);
  if (enable) {
    prv_enable_shake_detection();
  } else {
    prv_disable_shake_detection();
  }

  s_shake_detection_enabled = enable;
}

void accel_set_shake_sensitivity_high(bool sensitivity_high) {
  // Configure the threshold level at which the BMI160 will think shake has occurred
  if (sensitivity_high) {
    bma255_write_register(BMA255Register_INT_6,
        BOARD_CONFIG_ACCEL.accel_config.shake_thresholds[AccelThresholdLow]);
  } else {
    bma255_write_register(BMA255Register_INT_6,
        BOARD_CONFIG_ACCEL.accel_config.shake_thresholds[AccelThresholdHigh]);
  }
}

bool accel_get_shake_detection_enabled(void) {
  return s_shake_detection_enabled;
}

////////////////////////////////////////////////////////////////////////////////
// Selftest Support
////////////////////////////////////////////////////////////////////////////////

static void prv_soft_reset(void) {
  bma255_write_register(BMA255Register_BGW_SOFTRESET, BMA255_SOFT_RESET_VALUE);
  psleep(4);
}

// Minimum thresholds for axis delta in mgs at 4G scale
static const uint16_t SELFTEST_THRESHOLDS[] = {
  [BMA255Axis_X] = 800,
  [BMA255Axis_Y] = 800,
  [BMA255Axis_Z] = 400,
};

static const char AXIS_NAMES[] = {
  [BMA255Axis_X] = 'X',
  [BMA255Axis_Y] = 'Y',
  [BMA255Axis_Z] = 'Z',
};

static const uint8_t AXIS_REGISTERS[] = {
  [BMA255Axis_X] = BMA255Register_ACCD_X_LSB,
  [BMA255Axis_Y] = BMA255Register_ACCD_Y_LSB,
  [BMA255Axis_Z] = BMA255Register_ACCD_Z_LSB,
};

static int16_t prv_read_axis(BMA255Axis axis, uint8_t *new_data) {
  uint8_t raw_buf[2];
  bma255_burst_read(AXIS_REGISTERS[axis], raw_buf, sizeof(raw_buf));
  int16_t reading = prv_conv_raw_to_12bit(raw_buf);
  if (new_data) {
    *new_data = raw_buf[0] & 0x01;
  }
  return reading;
}

static bool prv_selftest_axis(BMA255Axis axis) {
  uint8_t axis_bits;
  switch (axis) {
    case BMA255Axis_X:
      axis_bits = 0x01;
      break;
    case BMA255Axis_Y:
      axis_bits = 0x02;
      break;
    case BMA255Axis_Z:
      axis_bits = 0x03;
      break;
    default:
      WTF;
  }


  // g-range should be 4g for self-test
  bma255_set_scale(BMA255Scale_4G);

  psleep(2); // wait for a new sample

  uint8_t new_data;
  int16_t before = prv_read_axis(axis, &new_data);
  before = prv_raw_to_mgs(before);

  // Positive axis
  bma255_write_register(BMA255Register_PMU_SELFTEST, axis_bits | SELFTEST_SIGN_POSITIVE);
  psleep(50);
  uint8_t new_positive;
  int16_t positive = prv_read_axis(axis, &new_positive);
  positive = prv_raw_to_mgs(positive);

  prv_soft_reset();
  bma255_set_scale(BMA255Scale_4G);

  // Negative axis
  bma255_write_register(BMA255Register_PMU_SELFTEST, axis_bits | SELFTEST_SIGN_NEGATIVE);
  psleep(50);
  uint8_t new_negative;
  int16_t negative = prv_read_axis(axis, &new_negative);
  negative = prv_raw_to_mgs(negative);

  prv_soft_reset();

  int delta = positive - negative;
  delta = abs(delta);

  PBL_LOG(LOG_LEVEL_DEBUG,
          "Self test axis %c: %d Pos: %d Neg: %d Delta: %d (required %d)",
          AXIS_NAMES[axis], before, positive,
          negative, delta, SELFTEST_THRESHOLDS[axis]);

  if (delta < SELFTEST_THRESHOLDS[axis]) {
    PBL_LOG(LOG_LEVEL_ERROR, "Self test failed for axis %c: %d < %d",
            AXIS_NAMES[axis], delta, SELFTEST_THRESHOLDS[axis]);
    return false;
  }

  if ((new_data + new_negative + new_positive) != 3) {
    PBL_LOG(LOG_LEVEL_ERROR, "Self test problem? Not logging data? %d %d %d",
            new_data, new_positive, new_negative);
  }

  return true;
}

bool bma255_selftest(void) {
  // calling selftest_axis resets the device
  bool pass = true;
  pass &= prv_selftest_axis(BMA255Axis_X);
  pass &= prv_selftest_axis(BMA255Axis_Y);
  pass &= prv_selftest_axis(BMA255Axis_Z);

  // g-range should be 4g to copy the BMI160
  bma255_set_scale(BMA255Scale_4G);

  return pass;
}

bool accel_run_selftest(void) {
  return bma255_selftest();
}

////////////////////////////////////////////////////////////////////////////////
// Debug Commands
////////////////////////////////////////////////////////////////////////////////

void command_accel_status(void) {
  const uint8_t bw = bma255_read_register(BMA255Register_PMU_BW);
  const uint8_t lpw = bma255_read_register(BMA255Register_PMU_LPW);
  const uint8_t lp = bma255_read_register(BMA255Register_PMU_LOW_POWER);
  const uint8_t fifo_cfg0 = bma255_read_register(BMA255Register_FIFO_CONFIG_0);
  const uint8_t fifo_cfg1 = bma255_read_register(BMA255Register_FIFO_CONFIG_1);
  const uint8_t fifo_status = bma255_read_register(BMA255Register_FIFO_STATUS);
  const uint8_t int_map_0 = bma255_read_register(BMA255Register_INT_MAP_0);
  const uint8_t int_en_0 = bma255_read_register(BMA255Register_INT_EN_0);
  const uint8_t int_map_1 = bma255_read_register(BMA255Register_INT_MAP_1);
  const uint8_t int_en_1 = bma255_read_register(BMA255Register_INT_EN_1);
  const uint8_t int_map_2 = bma255_read_register(BMA255Register_INT_MAP_2);
  const uint8_t int_en_2 = bma255_read_register(BMA255Register_INT_EN_2);
  const uint8_t int_status_0 = bma255_read_register(BMA255Register_INT_STATUS_0);
  const uint8_t int_status_1 = bma255_read_register(BMA255Register_INT_STATUS_1);
  const uint8_t int_status_2 = bma255_read_register(BMA255Register_INT_STATUS_2);
  const uint8_t int_status_3 = bma255_read_register(BMA255Register_INT_STATUS_3);

  char buf[64];
  prompt_send_response_fmt(buf, 64, "(0x10) Bandwidth: 0x%"PRIx8, bw);

  prompt_send_response_fmt(buf, 64, "(0x11) LPW: 0x%"PRIx8, lpw);
  prompt_send_response_fmt(buf, 64, "  suspend: 0x%"PRIx8, (lpw & (1 << 7)) != 0);
  prompt_send_response_fmt(buf, 64, "  lowpower_en: 0x%"PRIx8, (lpw & (1 << 6)) != 0);
  prompt_send_response_fmt(buf, 64, "  deep_suspend: 0x%"PRIx8, (lpw & (1 << 5)) != 0);
  prompt_send_response_fmt(buf, 64, "  sleep_dur: 0x%"PRIx8, (lpw & 0b11110) >> 1);

  prompt_send_response_fmt(buf, 64, "(0x12) Low_Power: 0x%"PRIx8, lp);
  prompt_send_response_fmt(buf, 64, "  lowpower_mode: 0x%"PRIx8, (lp & (1 << 6)) != 0);
  prompt_send_response_fmt(buf, 64, "  sleeptimer_mode: 0x%"PRIx8, (lp & (1 << 5)) != 0);

  prompt_send_response_fmt(buf, 64, "(0x30) FIFO Config 0: 0x%"PRIx8, fifo_cfg0);
  prompt_send_response_fmt(buf, 64, "  Watermark: 0x%"PRIx8, fifo_cfg0 & 0b111111);

  prompt_send_response_fmt(buf, 64, "(0x3e) FIFO Config 1: 0x%"PRIx8, fifo_cfg1);
  prompt_send_response_fmt(buf, 64, "  Mode: 0x%"PRIx8, (fifo_cfg1 & (0x3 << 6)) >> 6);
  prompt_send_response_fmt(buf, 64, "  Data Select: 0x%"PRIx8, fifo_cfg1 & 0x3);

  prompt_send_response_fmt(buf, 64, "(0x0e) Fifo Status: 0x%"PRIx8, fifo_status);
  prompt_send_response_fmt(buf, 64, "  Num Samples: 0x%"PRIx8, (fifo_status & 0x3f));

  prompt_send_response_fmt(buf, 64, "(0x19) Int Map 0: 0x%"PRIx8, int_map_0);
  prompt_send_response_fmt(buf, 64, "(0x16) Int EN 0: 0x%"PRIx8, int_en_0);

  prompt_send_response_fmt(buf, 64, "(0x1a) Int Map 1: 0x%"PRIx8, int_map_1);
  prompt_send_response_fmt(buf, 64, "(0x17) Int EN 1: 0x%"PRIx8, int_en_1);

  prompt_send_response_fmt(buf, 64, "(0x1b) Int Map 2: 0x%"PRIx8, int_map_2);
  prompt_send_response_fmt(buf, 64, "(0x18) Int EN 2: 0x%"PRIx8, int_en_2);

  prompt_send_response_fmt(buf, 64, "(0x0a) Int Status 0: 0x%"PRIx8, int_status_0);
  prompt_send_response_fmt(buf, 64, "(0x0a) Int Status 1: 0x%"PRIx8, int_status_1);
  prompt_send_response_fmt(buf, 64, "(0x0b) Int Status 2: 0x%"PRIx8, int_status_2);
  prompt_send_response_fmt(buf, 64, "(0x0c) Int Status 3: 0x%"PRIx8, int_status_3);
}

void command_accel_selftest(void) {
  const bool success = accel_run_selftest();
  char *response = (success) ? "Pass" : "Fail";
  prompt_send_response(response);
}

void command_accel_softreset(void) {
  prv_soft_reset();
}
