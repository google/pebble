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

#include "lis3dh.h"

#include "board/board.h"
#include "drivers/exti.h"
#include "drivers/gpio.h"
#include "drivers/i2c.h"
#include "drivers/legacy/accel.h"
#include "drivers/periph_config.h"
#include "drivers/vibe.h"
#include "kernel/events.h"
#include "pebble_errors.h"
#include "registers.h"
#include "services/common/accel_manager.h"
#include "services/common/analytics/analytics.h"
#include "services/common/vibe_pattern.h"
#include "services/imu/units.h"
#include "system/logging.h"
#include "os/mutex.h"
#include "system/passert.h"
#include "util/size.h"
#include "kernel/util/sleep.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#include <mcu.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define ACCEL_MAX_IDLE_DELTA 100

// State
static bool s_initialized = false;
static bool s_running = false;
static const Lis3dhScale s_accel_scale = LIS3DH_SCALE_4G;
static PebbleMutex * s_accel_mutex;
static uint64_t s_latest_timestamp;
static uint8_t s_pending_accel_event = false;
static bool s_is_idle = false;
static AccelRawData s_last_analytics_position;
static AccelRawData s_latest_reading;

// Buffer for holding the accel data
static SharedCircularBuffer s_buffer;
static uint8_t s_buffer_storage[50*sizeof(AccelRawData)]; // 400 bytes (~1s of data at 50Hz)

static void lis3dh_IRQ1_handler(bool *should_context_switch);
static void lis3dh_IRQ2_handler(bool *should_context_switch);

static void prv_accel_configure_interrupts(void) {
  exti_configure_pin(BOARD_CONFIG_ACCEL.accel_ints[0], ExtiTrigger_Rising, lis3dh_IRQ1_handler);
  exti_configure_pin(BOARD_CONFIG_ACCEL.accel_ints[1], ExtiTrigger_Rising, lis3dh_IRQ2_handler);
}

static void disable_accel_interrupts(void) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(BOARD_CONFIG_ACCEL.accel_ints); i++) {
    exti_disable(BOARD_CONFIG_ACCEL.accel_ints[i]);
  }
}

static void enable_accel_interrupts(void) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(BOARD_CONFIG_ACCEL.accel_ints); i++) {
    exti_enable(BOARD_CONFIG_ACCEL.accel_ints[i]);
  }
}

static void clear_accel_interrupts(void) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(BOARD_CONFIG_ACCEL.accel_ints); i++) {
    EXTI_ClearFlag(BOARD_CONFIG_ACCEL.accel_ints[i].exti_line);
    EXTI_ClearITPendingBit(BOARD_CONFIG_ACCEL.accel_ints[i].exti_line);
  }
}


static int16_t raw_to_mgs(int16_t raw, Lis3dhScale scale) {
  int16_t mgs_per_digit;
  switch (scale) {
    case LIS3DH_SCALE_2G:
      mgs_per_digit = 1;
      break;
    case LIS3DH_SCALE_4G:
      mgs_per_digit = 2;
      break;
    case LIS3DH_SCALE_8G:
      mgs_per_digit = 4;
      break;
    case LIS3DH_SCALE_16G:
      mgs_per_digit = 12;
      break;
    default:
      WTF;
  }

  // least significant 4 bits need to be removed
  return ((raw >> 4) * mgs_per_digit);
}

static int16_t get_axis_data(AccelAxisType axis, uint8_t *raw_data) {
  // each sample is 2 bytes for each axis
  int offset = 2 * BOARD_CONFIG_ACCEL.accel_config.axes_offsets[axis];
  int invert = BOARD_CONFIG_ACCEL.accel_config.axes_inverts[axis];
  int16_t raw = (((int16_t)raw_data[offset + 1]) << 8) | raw_data[offset];
  int16_t converted = (invert ? -1 : 1) * raw_to_mgs(raw, s_accel_scale);
  return (converted);
}

// Simple read register command with no error handling
static bool prv_read_register(uint8_t register_address, uint8_t *result) {
  return i2c_read_register(I2C_LIS3DH, register_address, result);
}

// Simple write register command with no error handling
static bool prv_write_register(uint8_t register_address, uint8_t value) {
  return i2c_write_register(I2C_LIS3DH, register_address, value);
}

static void prv_clear_fifo(void) {
  // Use I2C calls instead of accel wrappers to avoid recursion (reset called from lis3dh_read/accel_write)
  uint8_t mode;
  if (!prv_read_register(LIS3DH_FIFO_CTRL_REG, &mode)) {
    return;
  }
  if (mode != MODE_BYPASS) {
    uint8_t fifo_ctrl_reg = mode & ~MODE_MASK;
    fifo_ctrl_reg |= (MODE_BYPASS & MODE_MASK);
    if (!prv_write_register(LIS3DH_FIFO_CTRL_REG, fifo_ctrl_reg)) {
      return;
    }
    if (!prv_write_register(LIS3DH_FIFO_CTRL_REG, mode)) {
      return;
    }
  }

}

static void prv_reset(void) {
  lis3dh_lock();
  if (i2c_bitbang_recovery(I2C_LIS3DH)) {
    prv_clear_fifo();
  }
  lis3dh_unlock();
  analytics_inc(ANALYTICS_DEVICE_METRIC_ACCEL_RESET_COUNT, AnalyticsClient_System);
}

bool lis3dh_read(uint8_t register_address, uint8_t read_size, uint8_t *buffer) {
  bool running = s_running;

  if (!running) {
    if (!accel_start()) {
      // couldn't start the accel
      return (false);
    }
  }

  if (!i2c_read_register_block(I2C_LIS3DH, register_address, read_size, buffer)) {
    prv_reset();
    return (false);
  }

  if (!running) {
    accel_stop();
  }

  return (true);
}

bool lis3dh_write(uint8_t address, uint8_t write_size, const uint8_t *buffer) {
  bool running = accel_running();

  if (!running) {
    if (!accel_start()) {
      // couldn't start the accel
      return (false);
    }
  }

  if (!i2c_write_register_block(I2C_LIS3DH, address, write_size, buffer)) {
    prv_reset();
    return (false);
  }

  if (!running) {
    accel_stop();
  }

  return true;
}


AccelRawData s_accel_data;
void accel_get_last_data(AccelRawData* data) {
  *data = s_accel_data;
}

void accel_get_data(AccelRawData* data, int num_samples) {
  if (!s_running) {
    PBL_LOG(LOG_LEVEL_ERROR, "Accel Not Running");
    return;
  }

  // accel output registers have adjacent addresses
  // MSB enables address auto-increment
  int num_bytes = 6 * num_samples;

  // Overflow bit doesn't get cleared until number of samples in fifo goes below
  // the watermark. Read an extra item from the fifo and just throw it away.
  int read_num_bytes = num_bytes + 6;

  uint8_t start_addr = 1 << 7 | LIS3DH_OUT_X_L;
  uint8_t buffer[read_num_bytes];
  lis3dh_read(start_addr, read_num_bytes, buffer);
  for (uint8_t *ptr = buffer; ptr < (buffer + num_bytes); ptr+=6) {
    data->x = get_axis_data(ACCEL_AXIS_X, ptr);
    data->y = get_axis_data(ACCEL_AXIS_Y, ptr);
    data->z = get_axis_data(ACCEL_AXIS_Z, ptr);
    s_accel_data = *data;
    data++;
  }
}

void lis3dh_init(void) {
  PBL_ASSERTN(!s_initialized);

  lis3dh_init_mutex();

  s_initialized = true;

  if (!accel_start()) {
    s_initialized = false;
    return;
  }

  if (!lis3dh_config_set_defaults()) {
    // accel write will call reset if it fails, so just try again
    if (!lis3dh_config_set_defaults()) {
      s_initialized = false;
      return;
    }
  }
  shared_circular_buffer_init(&s_buffer, s_buffer_storage, sizeof(s_buffer_storage));

  // Test out the peripheral real quick
  if (!lis3dh_sanity_check()) {
    s_initialized = false;
    return;
  }

  accel_stop();

  prv_accel_configure_interrupts();
}

void lis3dh_power_up(void) {
  if (accel_start()) {
    uint8_t ctrl_reg1;
    if (prv_read_register(LIS3DH_CTRL_REG1, &ctrl_reg1)) {
      ctrl_reg1 &= ~LPen;
      if (prv_write_register(LIS3DH_CTRL_REG1, ctrl_reg1)) {
          // Write successful, low power mode disabled
          return;
      }
    }
  }
  PBL_LOG(LOG_LEVEL_ERROR, "Failed to exit low power mode");
}

void lis3dh_power_down(void) {
  if (accel_start()) {
    uint8_t ctrl_reg1;
    if (prv_read_register(LIS3DH_CTRL_REG1, &ctrl_reg1)) {
      ctrl_reg1 |= LPen;
      if (prv_write_register(LIS3DH_CTRL_REG1, ctrl_reg1)) {
        // Write successful, low power mode enabled
        accel_stop();
        return;
      }
    }
  }
  PBL_LOG(LOG_LEVEL_ERROR, "Failed to enter low power mode");
}

bool accel_running(void) {
  return (s_running);
}

bool accel_start(void) {
  if (!s_initialized) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to start accel, not yet initialized");
    return false;
  }

  if (s_running) {
    return true; // Already running
  }

  i2c_use(I2C_LIS3DH);

  s_running = true;

  return true;
}

void accel_stop(void) {
  PBL_ASSERTN(s_initialized);
  if (s_running) {
    disable_accel_interrupts();
    clear_accel_interrupts();

    i2c_release(I2C_LIS3DH);

    enable_accel_interrupts();

    s_running = false;
  }
}

void lis3dh_init_mutex(void) {
  s_accel_mutex = mutex_create();
}

void lis3dh_lock(void)  {
  mutex_lock(s_accel_mutex);
}

void lis3dh_unlock(void) {
  mutex_unlock(s_accel_mutex);
}

static void prv_handle_tap(void *data) {
  IMUCoordinateAxis axis;
  int direction;

  if (s_running) {
    uint8_t click_src;
    lis3dh_read(LIS3DH_CLICK_SRC, 1, &click_src);

    if (click_src & (1 << BOARD_CONFIG_ACCEL.accel_config.axes_offsets[AXIS_X])) {
      axis = AXIS_X;
    } else if (click_src & (1 << BOARD_CONFIG_ACCEL.accel_config.axes_offsets[AXIS_Y])) {
      axis = AXIS_Y;
    } else if (click_src & (1 << BOARD_CONFIG_ACCEL.accel_config.axes_offsets[AXIS_Z])) {
      axis = AXIS_Z;
    } else {
      // something has reset the register, ignore
      return;
    }
    // sign bit is zero if positive, 1 if negative
    direction = (click_src & Sign) ? -1 : 1;
  } else {
    // when no-one has subscribed, we only listen to the x axis
    axis = AXIS_X;
    // no sign info
    direction = 0;
  }

  PebbleEvent e = {
    .type = PEBBLE_ACCEL_SHAKE_EVENT,
    .accel_tap = {
      .axis = axis,
      .direction = direction,
    },
  };

  event_put(&e);
}

static void lis3dh_IRQ2_handler(bool *should_context_switch) {
  // vibe sometimes triggers the tap interrupt.
  // if vibe is on, we disregard the interrupt
  if (vibes_get_vibe_strength() == VIBE_STRENGTH_OFF) {
    PebbleEvent e = {
      .type = PEBBLE_CALLBACK_EVENT,
      .callback = {
        .callback = prv_handle_tap,
        .data = NULL
      }
    };

    *should_context_switch = event_put_isr(&e);
  }
}

void accel_set_running(bool running) {
  s_running = running;
}

void accel_set_num_samples(uint8_t num_samples) {
  if (num_samples == 0) {
    // peek mode, no FIFO
    lis3dh_set_fifo_mode(MODE_BYPASS);
    lis3dh_disable_fifo();
  } else {
    lis3dh_set_fifo_wtm(num_samples - 1);
    // clear fifo
    lis3dh_set_fifo_mode(MODE_BYPASS);
    // wait 1 ms
    psleep(10);
    lis3dh_set_fifo_mode(MODE_STREAM);
    lis3dh_enable_fifo();
  }

}

static void prv_read_samples(void *data) {
  uint8_t src_reg;
  lis3dh_read(LIS3DH_FIFO_SRC_REG, 1, &src_reg);
  uint8_t num_samples = src_reg & FSS_MASK;

  AccelRawData accel_raw_data[num_samples];
  if (src_reg & FIFO_OVRN) {
    PBL_LOG(LOG_LEVEL_ERROR, "Fifo overrun");
    analytics_inc(ANALYTICS_DEVICE_METRIC_ACCEL_FIFO_OVERRUN_COUNT, AnalyticsClient_System);
  }

  if (src_reg & FIFO_WTM) {
    accel_get_data(accel_raw_data, num_samples);
    if (num_samples > 0) {
      s_latest_reading = accel_raw_data[num_samples-1];
    }
    lis3dh_lock();
    if (s_buffer.clients) {
      // Only buffer the data if we have clients that are subscribed.
      if (!shared_circular_buffer_write(&s_buffer, (uint8_t *)accel_raw_data, num_samples * sizeof(AccelRawData),
                      false /*advance_slackers*/)) {
        // Buffer is full, one or more clients will get dropped data

        // We have one or more clients who fell behind reading out of the buffer. Try again, but this time
        // resetting the slowest clients until there is room.
        PBL_ASSERTN(shared_circular_buffer_write(&s_buffer, (uint8_t *)accel_raw_data, num_samples * sizeof(AccelRawData),
                        true /*advance_slackers*/));
      }
    }
    lis3dh_unlock();
  }

  // Record timestamp of newest data in the queue
  time_t time_s;
  uint16_t time_ms;
  rtc_get_time_ms(&time_s, &time_ms);
  s_latest_timestamp = ((uint64_t)time_s) * 1000 + time_ms;

  if (num_samples == 0) {
    accel_reset_pending_accel_event();
    return;
  }

  accel_manager_dispatch_data();
}

uint64_t accel_get_latest_timestamp(void) {
  return s_latest_timestamp;
}

static void lis3dh_IRQ1_handler(bool *should_context_switch) {
  // It's possible that this interrupt could be leftover after turning accel off.
  if (!s_running) {
    return;
  }

  // Only post a new event if the prior one has been picked up. This prevents us from flooding the KernelMain
  // queue
  if (!s_pending_accel_event) {
    s_pending_accel_event = true;
    PebbleEvent e = {
      .type = PEBBLE_CALLBACK_EVENT,
      .callback = {
        .callback = prv_read_samples,
        .data = NULL
      }
    };
    *should_context_switch = event_put_isr(&e);
  }
}

//! Returns the latest accel reading
void accel_get_latest_reading(AccelRawData *data) {
  *data = s_latest_reading;
}

//! Clears the pending accel event boolean. Called by KernelMain once it receives the accel_manager_dispatch_data
//! callback
void accel_reset_pending_accel_event(void) {
  s_pending_accel_event = false;
}

//! Adds a consumer to the circular buffer
//! @client which client to add
void accel_add_consumer(SharedCircularBufferClient *client) {
  lis3dh_lock();
  PBL_ASSERTN(shared_circular_buffer_add_client(&s_buffer, client));
  lis3dh_unlock();
}


//! Removes a consumer from the circular buffer
//! @client which client to remove
void accel_remove_consumer(SharedCircularBufferClient *client) {
  lis3dh_lock();
  shared_circular_buffer_remove_client(&s_buffer, client);
  lis3dh_unlock();
}


//! Returns number of samples actually read.
//! @param data The buffer to read the data into
//! @client which client is reading
//! @param max_samples Size of buffer in samples
//! @param subsample_num Subsampling numerator
//! @param subsample_den Subsampling denominator
//! @return The actual number of samples read
uint32_t accel_consume_data(AccelRawData *data, SharedCircularBufferClient *client, uint32_t max_samples,
            uint16_t subsample_num, uint16_t subsample_den) {
  uint16_t items_read;
  PBL_ASSERTN(accel_running());
  lis3dh_lock();
  {
    shared_circular_buffer_subsample_items(&s_buffer, client, sizeof(AccelRawData), max_samples, subsample_num,
                subsample_den, (uint8_t *)data, &items_read);
  }
  lis3dh_unlock();
  ACCEL_LOG_DEBUG("%"PRIu16" samples (from %"PRIu32" requested) were read for %p",
      items_read, max_samples, client);
  return (items_read);
}


int accel_peek(AccelData* data) {
  if (!s_running) {
    return (-1);
  }

  // No peeking if we're in FIFO mode.
  if (lis3dh_get_fifo_mode() == MODE_STREAM) {
    return (-2);
  }

  accel_get_data((AccelRawData*)data, 1);

  return (0);
}


// Compute and return the device's delta position to help determine movement as idle.
static uint32_t prv_compute_delta_pos(AccelRawData *cur_pos, AccelRawData *last_pos) {
  return abs(last_pos->x - cur_pos->x) + abs(last_pos->y - cur_pos->y) + abs(last_pos->z - cur_pos->z);
}


// Return true if we are "idle". We check for no movement for at least the last hour (the analytics snapshot
// position is updated once/hour).
bool accel_is_idle(void) {
  if (!s_is_idle) {
    return false;
  }

  // It was idle recently, see if it's still idle. Note we are avoiding reading the accel hardware again here
  // to keep this call as lightweight as possible. Instead we are just comparing the last read value with
  // the value last captured by analytics (which does so on an hourly heartbeat).
  AccelRawData accel_data;
  accel_get_last_data((AccelRawData*)&accel_data);
  s_is_idle = (prv_compute_delta_pos(&accel_data, &s_last_analytics_position) < ACCEL_MAX_IDLE_DELTA);
  return s_is_idle;
}


static bool prv_get_accel_data(AccelRawData *accel_data) {
  bool running = accel_running();
  if (!running) {
    if (!accel_start()) {
      return false;
    }
  }
  if (lis3dh_get_fifo_mode() != MODE_STREAM) {
    accel_get_data((AccelRawData*)accel_data, 1);
  } else {
    accel_get_last_data((AccelRawData*)accel_data);
  }
  if (!running) {
    accel_stop();
  }
  return true;
}

// Analytics Metrics
//////////////////////////////////////////////////////////////////////
void analytics_external_collect_accel_xyz_delta(void) {
  AccelRawData accel_data;
  if (prv_get_accel_data(&accel_data)) {
    uint32_t delta = prv_compute_delta_pos(&accel_data, &s_last_analytics_position);
    s_is_idle = (delta < ACCEL_MAX_IDLE_DELTA);
    s_last_analytics_position = accel_data;
    analytics_set(ANALYTICS_DEVICE_METRIC_ACCEL_XYZ_DELTA, delta, AnalyticsClient_System);
  }
}


// Self Test
//////////////////////////////////////////////////////////////////////

bool accel_self_test(void) {
  AccelRawData data;
  AccelRawData data_st;

  if (!accel_start()) {
    PBL_LOG(LOG_LEVEL_ERROR, "Self test failed, could not start accel");
    return false;
  }

  psleep(10);

  accel_get_data(&data, 1);

  lis3dh_enter_self_test_mode(SELF_TEST_MODE_ONE);
  // ST recommends sleeping for 1ms after programming the module to
  // enter self-test mode; a 100x factor of safety ought to be
  // sufficient
  psleep(100);

  accel_get_data(&data_st, 1);

  lis3dh_exit_self_test_mode();
  accel_stop();

  // [MJZ] I have no idea how to interpret the data coming out of the
  // accel's self-test mode. If I could make sense of the
  // incomprehensible datasheet, I would be able to check if the accel
  // output matches the expected values
  return ABS(data_st.x) > ABS(data.x);
}

void accel_set_shake_sensitivity_high(bool sensitivity_high) {
  // Configure the threshold level at which the LIS3DH will think motion has occurred
  if (sensitivity_high) {
    lis3dh_set_interrupt_threshold(
        BOARD_CONFIG_ACCEL.accel_config.shake_thresholds[AccelThresholdLow]);
  } else {
    lis3dh_set_interrupt_threshold(
        BOARD_CONFIG_ACCEL.accel_config.shake_thresholds[AccelThresholdHigh]);
  }
}
