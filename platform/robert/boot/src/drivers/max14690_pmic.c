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

/* This file should probably go in the stm32f4 folder */

#include "drivers/pmic.h"

#include "board/board.h"
#include "drivers/dbgserial.h"
#include "drivers/gpio.h"
#include "drivers/i2c.h"
#include "drivers/periph_config.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/delay.h"

#include "stm32f7xx.h"

#include <stdint.h>

#define MAX14690_ADDR (0x50)

#define MAX14690_WHOAMI (0x01)

//! The addresses of the registers that we can read using i2c
typedef enum PmicRegisters {
  PmicRegisters_CHIP_ID = 0x00,
  PmicRegisters_CHIP_REV = 0x01,
  PmicRegisters_STATUSA = 0x02,
  PmicRegisters_STATUSB = 0x03,
  PmicRegisters_INTA = 0x05,
  PmicRegisters_INTB = 0x06,
  PmicRegisters_INT_MASK_A = 0x07,
  PmicRegisters_INT_MASK_B = 0x08,
  PmicRegisters_CHG_CNTL_A = 0x0A,
  PmicRegisters_BUCK1_CONFIG = 0x0D,
  PmicRegisters_BUCK2_CONFIG = 0x0F,
  PmicRegisters_LDO1_CONFIG = 0x12,
  PmicRegisters_LDO2_CONFIG = 0x14,
  PmicRegisters_LDO3_CONFIG = 0x16,
  PmicRegisters_MON_CFG = 0x19,
  PmicRegisters_PWR_CFG = 0x1F
} PmicRegisters;

//! The different power rails that our PMIC controls
typedef enum PmicRail {
  PmicRail_BUCK1, //!< 1.2V
  PmicRail_BUCK2, //!< 1.8V
  PmicRail_LDO1, //!< 2.0V - Auto - RTC
  PmicRail_LDO2, //!< 3.2V - Manual - FPGA

  //! snowy_bb: 2.5V - Manual - MFi, Magnetometer
  //! snowy_evt: 1.8V - Manual - MFi
  PmicRail_LDO3
} PmicRail;

//! Gives configuration information for reading a given rail through the monitor pin.
typedef struct {
  const char* name; //!< Name for the rail.

  //! What ratio we need to divide by in order to bring it into the range we can sense. We can
  //! only read between 0 and 1.8Vs, so we need to use the PMIC hardware to divide it down before
  //! sending it to us. Valid values are 1-4.
  uint8_t ratio;

  //! The binary value we need to put in the register to select the rail.
  uint8_t source_config;
} PmicMonConfig;

// Using the Binary constants GCC extension here, supported in GCC and Clang
// https://gcc.gnu.org/onlinedocs/gcc/Binary-constants.html
static const PmicMonConfig MON_CONFIG[] = {
  { "+VBAT",    3, 0b001 }, // 3:1
};

static const int PMIC_MON_CONFIG_VBAT_INDEX = 0;

/* Private Function Definitions */
static bool prv_is_alive(void);
static void prv_set_pin_config(void);

//! Request that the rail be used or released. Internally refcounted per rail so you don't have
//! to worry about turning this off on another client.
static bool prv_update_rail_state(PmicRail rail, bool enable);

static void prv_mon_config_lock(void) {
}

static void prv_mon_config_unlock(void) {
}

static bool prv_read_register(uint8_t register_address, uint8_t *result) {
  i2c_use(I2C_MAX14690);
  bool rv = i2c_read_register(I2C_MAX14690, register_address, result);
  i2c_release(I2C_MAX14690);
  return (rv);
}


static bool prv_write_register(uint8_t register_address, uint8_t value) {
  i2c_use(I2C_MAX14690);
  bool rv = i2c_write_register(I2C_MAX14690, register_address, value);
  i2c_release(I2C_MAX14690);
  return (rv);
}

/* Public Functions */
bool pmic_init(void) {
  prv_set_pin_config();

  if (!prv_is_alive()) {
    return false;
  }

  // Power up 3.2V rail
  prv_update_rail_state(PmicRail_LDO2, true);

  return true;
}

static bool prv_update_rail_state(PmicRail rail, bool enable) {
  static int8_t s_ldo2_ref_count = 0;
  static int8_t s_ldo3_ref_count = 0;

  int8_t *ref_count;
  uint8_t rail_control_reg = 0;

  if (rail == PmicRail_LDO2) {
    rail_control_reg = PmicRegisters_LDO2_CONFIG;
    ref_count = &s_ldo2_ref_count;
  } else if (rail == PmicRail_LDO3) {
    rail_control_reg = PmicRegisters_LDO3_CONFIG;
    ref_count = &s_ldo3_ref_count;
  } else {
    WTF;
  }

  uint8_t register_value;
  bool success = prv_read_register(rail_control_reg, &register_value);

  if (!success) {
    // Failed to read the current register value
    return false;
  }

  if (enable) {
    if (*ref_count) {
      (*ref_count)++;
      return true;
    } else {
      // Set the register byte to XXXXX01X to enable the rail, mask and set
      register_value = (register_value & ~0x06) | 0x02;

      success = prv_write_register(rail_control_reg, register_value);

      if (success) {
        // We enabled the rail!
        *ref_count = 1;

        // We need to wait a bit for the rail to stabilize before continuing to use the device.
        // It takes 2.6ms for the LDO rails to ramp.
        delay_ms(3);

        return true;
      }
      return false;
    }
  } else {
    if (*ref_count <= 1) {
      // Set the register byte to XXXXX00X to disable the rail, just mask
      register_value = (register_value & ~0x06);

      success = prv_write_register(rail_control_reg, register_value);

      if (success) {
        // We disabled the rail!
        *ref_count = 0;
        return true;
      }
      return false;
    } else {
      (*ref_count)--;
      return true;
    }
  }
}

bool pmic_power_off(void) {
  bool ret = prv_write_register(PmicRegisters_PWR_CFG, 0xB2);

  if (ret) {
    // Goodbye cruel world. The PMIC should be removing our power at any time now.

    while (1) {}
    __builtin_unreachable();
  }

  return false;
}

static bool prv_set_mon_config_register(uint8_t value) {
  return prv_write_register(PmicRegisters_MON_CFG, value);
}

static bool prv_set_mon_config(const PmicMonConfig *config) {
  const uint8_t ratio_config = 4 - config->ratio; // 4:1 is 0b00, 1:1 is 0b11.

  const uint8_t register_value = (ratio_config << 4) | config->source_config;
  bool result = prv_set_mon_config_register(register_value);

  // Need to wait a short period of time for the reading to settle due to capacitance on the line.
  delay_us(200);

  return result;
}

bool pmic_enable_battery_measure(void) {
  prv_mon_config_lock();

  return prv_set_mon_config(&MON_CONFIG[PMIC_MON_CONFIG_VBAT_INDEX]);

  // Don't prv_unlock, we don't want anyone else mucking with the mon config until
  // pmic_disable_battery_measure is called.
}

bool pmic_disable_battery_measure(void) {
  bool result = prv_set_mon_config_register(0);

  // Releases the lock that was previously acquired in pmic_enable_battery_measure.
  prv_mon_config_unlock();

  return result;
}

bool pmic_set_charger_state(bool enable) {
  // Defaults to ON
  // Default value is 0xF7
  const uint8_t register_value = enable ? 0xf7 : 0xf6;

  bool result = prv_write_register(PmicRegisters_CHG_CNTL_A, register_value);

  return result;
}


bool pmic_is_charging(void) {
  uint8_t val;
  if (!prv_read_register(PmicRegisters_STATUSA, &val)) {
    // NOTE: When running on QEMU, i2c reads return false. For now, just assume a failed
    // i2c read means we are charging
    return true;
  }

  uint8_t chgstat = val & 0x07;   // Mask off only charging status

  if (chgstat == 0x02 ||  // Pre-charge in progress
      chgstat == 0x03 ||  // Fast charge, CC
      chgstat == 0x04 ||  // Fast charge, CV
      chgstat == 0x05) {  // Maintain charge
    return true;
  } else {
    return false;
  }
}

bool pmic_is_usb_connected(void) {
  uint8_t val;
  if (!prv_read_register(PmicRegisters_STATUSB, &val)) {
    return false;
  }

  bool usb_connected = (val >> 3) & 1;

  return usb_connected;
}

void pmic_read_chip_info(uint8_t *chip_id, uint8_t *chip_revision) {
  prv_read_register(PmicRegisters_CHIP_ID, chip_id);
  prv_read_register(PmicRegisters_CHIP_REV, chip_revision);
}


/* Private Function Implementations */
static bool prv_is_alive(void) {
  uint8_t val;
  prv_read_register(0x00, &val);

  if (val == MAX14690_WHOAMI) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Found the max14690");
    return true;
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Error reading max14690 WHOAMI byte");
    return false;
  }
}

static void prv_set_pin_config(void) {
  periph_config_acquire_lock();

  // Initialize the GPIOs for the 4V5 & 6V6 rails
  gpio_output_init(&BOARD_CONFIG_POWER.rail_4V5_ctrl, GPIO_OType_OD, GPIO_Speed_50MHz);
  if (BOARD_CONFIG_POWER.rail_6V6_ctrl.gpio) {
    gpio_output_init(&BOARD_CONFIG_POWER.rail_6V6_ctrl, GPIO_OType_OD, GPIO_Speed_50MHz);
  }
  gpio_output_init(&BOARD_CONFIG_ACCESSORY.power_en, GPIO_OType_OD, GPIO_Speed_50MHz);

  periph_config_release_lock();
}

void set_4V5_power_state(bool enabled) {
  gpio_output_set(&BOARD_CONFIG_POWER.rail_4V5_ctrl, enabled);
}

void set_6V6_power_state(bool enabled) {
  if (BOARD_CONFIG_POWER.rail_6V6_ctrl.gpio) {
    gpio_output_set(&BOARD_CONFIG_POWER.rail_6V6_ctrl, enabled);
  }
}
