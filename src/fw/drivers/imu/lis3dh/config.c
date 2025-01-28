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
#include "registers.h"

#include "drivers/i2c.h"
#include "drivers/legacy/accel.h"
#include "system/logging.h"
#include "util/math.h"
#include "util/size.h"

//! @file accel_config.c
//! procedures for dealing with I2C communication with the accel

struct I2CCommand {
  uint8_t register_address;
  uint8_t value;
};

//
// Boiler plate functions for talking over i2c
//

static uint8_t prv_read_reg(uint8_t address) {
  uint8_t reg;
  lis3dh_read(address, 1, &reg);

  return (reg);
}

static bool prv_write_reg(uint8_t address, uint8_t value) {
  return (lis3dh_write(address, 1, &value));
}

static bool send_i2c_commands(struct I2CCommand* commands, int num_commands) {
  for (int i = 0; i < num_commands; ++i) {
    bool result = prv_write_reg(commands[i].register_address, commands[i].value);

    if (!result) {
      return false;
    }
  }
  return true;
}

void lis3dh_enable_fifo(void) {
  uint8_t ctrl_reg5 = prv_read_reg(LIS3DH_CTRL_REG5);
  ctrl_reg5 |= FIFO_EN;
  prv_write_reg(LIS3DH_CTRL_REG5, ctrl_reg5);
}

void lis3dh_disable_fifo(void) {
  uint8_t ctrl_reg5 = prv_read_reg(LIS3DH_CTRL_REG5);
  ctrl_reg5 &= ~FIFO_EN;
  prv_write_reg(LIS3DH_CTRL_REG5, ctrl_reg5);
}

bool lis3dh_is_fifo_enabled(void) {
  uint8_t ctrl_reg5 = prv_read_reg(LIS3DH_CTRL_REG5);
  return (ctrl_reg5 & FIFO_EN);
}

void lis3dh_disable_click(void) {
  uint8_t ctrl_reg3 = prv_read_reg(LIS3DH_CTRL_REG3);
  ctrl_reg3 &= ~I1_CLICK;
  prv_write_reg(LIS3DH_CTRL_REG3, ctrl_reg3);
}

void lis3dh_enable_click(void) {
  uint8_t ctrl_reg3 = prv_read_reg(LIS3DH_CTRL_REG3);
  ctrl_reg3 |= I1_CLICK;
  prv_write_reg(LIS3DH_CTRL_REG3, ctrl_reg3);
}

//
// Accel config Getter/Setters
//

void lis3dh_set_interrupt_axis(AccelAxisType axis, bool double_click) {
  // get the current state of the registers
  uint8_t reg_1 = prv_read_reg(LIS3DH_CTRL_REG1);

  // clear the axis-enable bits
  reg_1 = reg_1 & ~(0x1 | 0x2 | 0x4);
  uint8_t click_cfg = 0;

  switch (axis) {
    case ACCEL_AXIS_X:
      reg_1 |= 0x01;
      click_cfg = 0x01;
      break;
    case ACCEL_AXIS_Y:
      reg_1 |= 0x02;
      click_cfg = 0x04;
      break;
    case ACCEL_AXIS_Z:
      reg_1 |= 0x04;
      click_cfg = 0x10;
      break;
    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Unknown axis");
  }

  if (double_click) {
    click_cfg <<= 1;
  }

  if ((!prv_write_reg(LIS3DH_CTRL_REG1, reg_1))
      || (!prv_write_reg(LIS3DH_CLICK_CFG, click_cfg))) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to write axis selection");
  }
}

uint8_t lis3dh_get_click_window() {
  return prv_read_reg(LIS3DH_TIME_WINDOW);
}
void lis3dh_set_click_window(uint8_t window) {
  if (!prv_write_reg(LIS3DH_TIME_WINDOW, MIN(window, LIS3DH_MAX_CLICK_WINDOW))) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to write click latency");
  }
}

uint8_t lis3dh_get_click_latency() {
  return prv_read_reg(LIS3DH_TIME_LATENCY);
}
void lis3dh_set_click_latency(uint8_t latency) {
  if (!prv_write_reg(LIS3DH_TIME_LATENCY, MIN(latency, LIS3DH_MAX_CLICK_LATENCY))) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to write click latency");
  }
}

uint8_t lis3dh_get_interrupt_threshold() {
  return prv_read_reg(LIS3DH_CLICK_THS);
}
void lis3dh_set_interrupt_threshold(uint8_t threshold) {
  if (!prv_write_reg(LIS3DH_CLICK_THS, MIN(threshold, LIS3DH_MAX_THRESHOLD))) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to set interrupt threshold");
  }
}

uint8_t lis3dh_get_interrupt_time_limit() {
  return prv_read_reg(LIS3DH_TIME_LIMIT);
}
void lis3dh_set_interrupt_time_limit(uint8_t time_limit) {
  if (!prv_write_reg(LIS3DH_TIME_LIMIT, MIN(time_limit, LIS3DH_MAX_TIME_LIMIT))) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to set interrupt time limit");
  }
}

bool lis3dh_set_fifo_wtm(uint8_t wtm) {
  uint8_t fifo_ctrl_reg = prv_read_reg(LIS3DH_FIFO_CTRL_REG);
  fifo_ctrl_reg &= ~THR_MASK;
  fifo_ctrl_reg |= (wtm & THR_MASK);

  return (prv_write_reg(LIS3DH_FIFO_CTRL_REG, fifo_ctrl_reg));
}

uint8_t lis3dh_get_fifo_wtm(void) {
  uint8_t fifo_ctrl_reg = prv_read_reg(LIS3DH_FIFO_CTRL_REG);
  return (fifo_ctrl_reg & THR_MASK);
}

AccelSamplingRate accel_get_sampling_rate(void) {
  uint8_t odr = ODR_MASK & prv_read_reg(LIS3DH_CTRL_REG1);

  if (odr == (ODR2 | ODR0)) {
    return (ACCEL_SAMPLING_100HZ);
  } else if (odr == ODR2) {
    return (ACCEL_SAMPLING_50HZ);
  } else if (odr == (ODR1 | ODR0)) {
    return (ACCEL_SAMPLING_25HZ);
  } else if (odr == (ODR1)) {
    return (ACCEL_SAMPLING_10HZ);
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "Unrecognized ODR value %d", odr);
    return (0);
  }
}

bool accel_set_sampling_rate(AccelSamplingRate rate) {
  uint8_t odr;

  switch (rate) {
    case ACCEL_SAMPLING_100HZ:
      odr = ODR2 | ODR0;
      break;
    case ACCEL_SAMPLING_50HZ:
      odr = ODR2;
      break;
    case ACCEL_SAMPLING_25HZ:
      odr = ODR1 | ODR0;
      break;
    case ACCEL_SAMPLING_10HZ:
      odr = ODR1;
      break;
    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Unsupported sampling rate %d", rate);
      return (false);
  }

  uint8_t ctrl_reg_1 = prv_read_reg(LIS3DH_CTRL_REG1);
  ctrl_reg_1 &= ~ODR_MASK;
  ctrl_reg_1 |= (odr & ODR_MASK);
  //TODO: fix hack below (enabling axes after lis3dh_power_down)
  ctrl_reg_1 |= (Xen | Yen | Zen); //enable x, y and z axis
  bool res = prv_write_reg(LIS3DH_CTRL_REG1, ctrl_reg_1);

  // Update the click limit based on sampling frequency
  uint8_t time_limit = rate * LIS3DH_TIME_LIMIT_MULT / LIS3DH_TIME_LIMIT_DIV;
  lis3dh_set_interrupt_time_limit(time_limit);
  PBL_LOG(LOG_LEVEL_DEBUG, "setting click time limit to 0x%x",
              lis3dh_get_interrupt_time_limit());

  // Update click latency
  uint8_t time_latency = rate * LIS3DH_TIME_LATENCY_MULT / LIS3DH_TIME_LATENCY_DIV;
  lis3dh_set_click_latency(time_latency);
  PBL_LOG(LOG_LEVEL_DEBUG, "setting click time latency to 0x%x",
              lis3dh_get_click_latency());
  
  // Update click window
  uint32_t time_window = rate * LIS3DH_TIME_WINDOW_MULT / LIS3DH_TIME_WINDOW_DIV;
  time_window = MIN(time_window, 0xff);
  lis3dh_set_click_window(time_window);
  PBL_LOG(LOG_LEVEL_DEBUG, "setting click time window to 0x%x",
              lis3dh_get_click_window());
  

  return (res);
}

Lis3dhScale accel_get_scale(void) {
  uint8_t fs = FS_MASK & prv_read_reg(LIS3DH_CTRL_REG4);

  if (fs == (FS0 | FS1)) {
    return (LIS3DH_SCALE_16G);
  } else if (fs == FS1) {
    return (LIS3DH_SCALE_8G);
  } else if (fs == FS0) {
    return (LIS3DH_SCALE_4G);
  } else if (fs == 0) {
    return (LIS3DH_SCALE_2G);
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "Unrecognized FS value %d", fs);
    return (LIS3DH_SCALE_UNKNOWN);
  }
}

bool accel_set_scale(Lis3dhScale scale) {
  uint8_t fs;

  switch (scale) {
    case LIS3DH_SCALE_16G:
      fs = (FS0 | FS1);
      break;
    case LIS3DH_SCALE_8G:
      fs = FS1;
      break;
    case LIS3DH_SCALE_4G:
      fs = FS0;
      break;
    case LIS3DH_SCALE_2G:
      fs = 0;
      break;
    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Unsupported scale %d", scale);
      return (false);
  }

  uint8_t ctrl_reg_4 = prv_read_reg(LIS3DH_CTRL_REG4);
  ctrl_reg_4 &= ~FS_MASK;
  ctrl_reg_4 |= (fs & FS_MASK);
  bool res = prv_write_reg(LIS3DH_CTRL_REG4, ctrl_reg_4);

  lis3dh_set_interrupt_threshold(scale * LIS3DH_THRESHOLD_MULT
                                / LIS3DH_THRESHOLD_DIV);
  PBL_LOG(LOG_LEVEL_DEBUG, "setting click threshold to 0x%x",
              lis3dh_get_interrupt_threshold());
  return (res);
}

bool lis3dh_set_fifo_mode(uint8_t mode) {
  uint8_t fifo_ctrl_reg = prv_read_reg(LIS3DH_FIFO_CTRL_REG);
  fifo_ctrl_reg &= ~MODE_MASK;
  fifo_ctrl_reg |= (mode & MODE_MASK);
  return (prv_write_reg(LIS3DH_FIFO_CTRL_REG, fifo_ctrl_reg));
}

uint8_t lis3dh_get_fifo_mode(void) {
  uint8_t fifo_ctrl_reg = prv_read_reg(LIS3DH_FIFO_CTRL_REG);
  return (fifo_ctrl_reg & MODE_MASK);
}

//! Configure the accel to run "Self Test 0". See S3.2.2 of the accel datasheet for more information
bool lis3dh_enter_self_test_mode(SelfTestMode mode) {
  uint8_t reg4 = 0x8;

  switch (mode) {
  case SELF_TEST_MODE_ONE:
    reg4 |= 0x2;
    break;
  case SELF_TEST_MODE_TWO:
    reg4 |= (0x2 | 0x4);
    break;
  default:
    break;
  }

  struct I2CCommand test_mode_config[] = {
    { LIS3DH_CTRL_REG1, 0x9f },
    { LIS3DH_CTRL_REG3, 0x00 },
    { LIS3DH_CTRL_REG4, reg4 }
  };

  return send_i2c_commands(test_mode_config, ARRAY_LENGTH(test_mode_config));
}

void lis3dh_exit_self_test_mode(void) {
  lis3dh_config_set_defaults();
}

//
// Boot-time config
//

//! Ask the accel for a 8-bit value that's programmed into the IC at the
//! factory. Useful as a sanity check to make sure everything came up properly.
bool lis3dh_sanity_check(void) {
  uint8_t whoami = prv_read_reg(LIS3DH_WHO_AM_I);
  PBL_LOG(LOG_LEVEL_DEBUG, "Read accel whomai byte 0x%x, expecting 0x%x", whoami, LIS3DH_WHOAMI_BYTE);
  return (whoami == LIS3DH_WHOAMI_BYTE);
}

bool lis3dh_config_set_defaults() {
  // Follow the startup sequence from AN3308
  struct I2CCommand accel_init_commands[] = {
    { LIS3DH_CTRL_REG1, (ODR1 | ODR0 | Zen | Yen | Xen) }, // 25MHz, Enable X,Y,Z Axes
    { LIS3DH_CTRL_REG2, 0x00 },
    { LIS3DH_CTRL_REG3, I1_WTM }, // FIFO Watermark on INT1
    { LIS3DH_CTRL_REG4, (BDU | FS0 | HR) }, // Block Read, +/- 4g sensitivity
    { LIS3DH_CTRL_REG6, I2_CLICK }, // Click on INT2

    { LIS3DH_INT1_THS, 0x20 }, // inertial threshold (MAX 0x7f)
    { LIS3DH_INT1_DURATION, 0x10 }, // interrupt duration (units of 1/(update frequency) [See CTRL_REG1])
    { LIS3DH_INT1_CFG, 0x00 }, // no inertial interrupts

    // click threshold (MAX 0x7f)
    { LIS3DH_CLICK_THS, LIS3DH_SCALE_4G * LIS3DH_THRESHOLD_MULT
                        / LIS3DH_THRESHOLD_DIV },
    
    // click time limit (units of 1/(update frequency) [See CTRL_REG1])
    { LIS3DH_TIME_LIMIT, ACCEL_DEFAULT_SAMPLING_RATE * LIS3DH_TIME_LIMIT_MULT
                          / LIS3DH_TIME_LIMIT_DIV},
    
    { LIS3DH_CLICK_CFG, (XS | YS | ZS) }, // single click detection on the X axis

    { LIS3DH_FIFO_CTRL_REG, MODE_BYPASS | 0x19 }, // BYPASS MODE and 25 samples per interrupt

    // time latency, ie "debounce time" after the first of a double click
    // (units of 1/(update frequency) [See CTRL_REG1])
    { LIS3DH_TIME_LATENCY, ACCEL_DEFAULT_SAMPLING_RATE * LIS3DH_TIME_LATENCY_MULT
                            / LIS3DH_TIME_LATENCY_DIV },

    // max time allowed between clicks for a double click (end to start)
    // (units of 1/(update frequency) [See CTRL_REG1])
    { LIS3DH_TIME_WINDOW, ACCEL_DEFAULT_SAMPLING_RATE *  LIS3DH_TIME_WINDOW_MULT
                          / LIS3DH_TIME_WINDOW_DIV}
  };

  if (!send_i2c_commands(accel_init_commands, ARRAY_LENGTH(accel_init_commands))) {
    accel_stop();
    PBL_LOG(LOG_LEVEL_WARNING, "Failed to initialize accelerometer");
    return false;
  }

  return true;
}
