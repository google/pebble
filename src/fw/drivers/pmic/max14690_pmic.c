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
#include "console/prompt.h"
#include "drivers/battery.h"
#include "drivers/gpio.h"
#include "drivers/i2c.h"
#include "drivers/exti.h"
#include "drivers/periph_config.h"
#include "system/logging.h"
#include "os/mutex.h"
#include "system/passert.h"
#include "kernel/util/delay.h"
#include "util/size.h"
#include "kernel/util/sleep.h"

#include "kernel/events.h"
#include "services/common/system_task.h"
#include "services/common/new_timer/new_timer.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#include <stdint.h>

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
  PmicRegisters_CHG_CNTL_B = 0x0B,
  PmicRegisters_CH_TMR = 0x0C,
  PmicRegisters_BUCK1_CONFIG = 0x0D,
  PmicRegisters_BUCK1_VSET = 0x0E,
  PmicRegisters_BUCK2_CONFIG = 0x0F,
  PmicRegisters_LDO1_CONFIG = 0x12,
  PmicRegisters_LDO2_CONFIG = 0x14,
  PmicRegisters_LDO3_CONFIG = 0x16,
  PmicRegisters_MON_CFG = 0x19,
  PmicRegisters_HAND_SHK = 0x1D,
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
  { "+VBAT",        3, 0b001 }, // 3:1
  { "+VSYS",        4, 0b010 }, // 4:1

// We only care about non-battery rails in MFG where we have the command_pmic_rails function.
#ifdef RECOVERY_FW
  { "+1V2",         1, 0b011 }, // 1:1, BUCK1
  { "+1V8",         2, 0b100 }, // 2:1, BUCK2
  { "+2V0_RTC",     2, 0b101 }, // 2:1, LDO1
  { "+3V2",         2, 0b110 }, // 2:1, LDO2
#ifdef BOARD_SNOWY_BB
  { "+2V5",         2, 0b111 }, // 2:1, LDO3
#else
  { "+1V8_MFI_MIC", 2, 0b111 }, // 2:1, LDO3
#endif // BOARD_SNOWY_BB
#endif // RECOVERY_FW
};

//! Mutex to make sure two threads aren't working with the PMIC mon value at the same time.
static PebbleMutex *s_mon_config_mutex;

static const int PMIC_MON_CONFIG_VBAT_INDEX = 0;
static const int PMIC_MON_CONFIG_VSYS_INDEX = 1;

//! Debounce timer for USB connections
static TimerID s_debounce_usb_conn_timer = TIMER_INVALID_ID;
static const int USB_CONN_DEBOUNCE_MS = 1000;
static volatile int s_interrupt_bounce_count;

/* Private Function Definitions */
static bool prv_is_alive(void);
static bool prv_set_pin_config(void);
static void prv_register_dump(int start_reg, int stop_reg);
static void prv_initialize_interrupts(void);

//! Request that the rail be used or released. Internally refcounted per rail so you don't have
//! to worry about turning this off on another client.
static bool prv_update_rail_state(PmicRail rail, bool enable);

static void prv_mon_config_lock(void) {
  mutex_lock(s_mon_config_mutex);
}

static void prv_mon_config_unlock(void) {
  mutex_unlock(s_mon_config_mutex);
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

// Configure PMIC's charger settings (different from defaults -
// see https://pebbletechnology.atlassian.net/browse/PBL-15134)
static bool prv_config_charger(void) {
  const uint8_t CHARGE_VOLTAGE_4300 = 0b101;
  const uint8_t CHARGE_VOLTAGE_4200 = 0b011;

  // [AS] HACK alert! (see PBL-19186)
  // The MAX14690 state machine is stupid and kicks us into a charge complete state when the charger
  // is connected and the battery voltage is within the range VBATREG < x < VBATREG - VBATRECHG
  // (where VBATREG = 4.30V and VBATRECHG = 70mV (previously 170mV) for our setup). This is quite
  // a likely situation because the DC internal resistance of the battery is quite high (~1Ω) and
  // we reach the termination voltage at around 70% SOC. To workaround this, we set VBATREG to 4.35V
  // and VBATRECHG to 70mV, turn the charger off and on again, then configure the charger to our
  // desired settings. The PMIC then recovers into a charge state. This will hopefully work for
  // most watches.
  prv_write_register(PmicRegisters_CHG_CNTL_A, 0xCD);
  pmic_set_charger_state(false);
  pmic_set_charger_state(true);

  const uint8_t bat_reg = (BOARD_CONFIG_POWER.charging_cutoff_voltage == 4300) ?
      CHARGE_VOLTAGE_4300 : CHARGE_VOLTAGE_4200;
  uint8_t chg_ctrl_a = 1 << 7 |       // 1: Enable Auto-stop (default)
                       1 << 6 |       // 1: Enable Auto-restart (default)
                       0 << 4 |       // 0: Set battery recharge threshold to 70mV
                       bat_reg << 1 | // bat_Reg: Set battery charge complete voltage
                       1 << 0;        // 1: Enable charger (default)
  if (!prv_write_register(PmicRegisters_CHG_CNTL_A, chg_ctrl_a)) {
    return false;
  }

  uint8_t chg_ctrl_b = 6 << 4 | // 6: Set precharge voltage threshold to 3.00V (default)
                       1 << 2 | // 1: Set precharge current to 0.1C
                       1 << 0;  // 1: Set charge done current to 0.1C (default)
  if (!prv_write_register(PmicRegisters_CHG_CNTL_B, chg_ctrl_b)) {
    return false;
  }

  uint8_t ch_tmr = 1 << 4 | // 1: Set maintain charge timeout to 15 min
                   2 << 2 | // 2: Set fast charge timeout to 300 min
                   0 << 0;  // 0: Set precharge timeout to 30 min

  return prv_write_register(PmicRegisters_CH_TMR, ch_tmr);
}

/* Public Functions */

uint32_t pmic_get_last_reset_reason(void) {
  // TODO: Look into if this pmic has a reset reason register which would be useful for debug
  return 0;
}

bool pmic_init(void) {
  s_mon_config_mutex = mutex_create();

  s_debounce_usb_conn_timer = new_timer_create();

  if (!prv_set_pin_config()) {
    return false;
  }

  if (!prv_is_alive()) {
    return false;
  }

  prv_config_charger();

  prv_initialize_interrupts();

  prv_update_rail_state(PmicRail_LDO2, true);  // FW should bring this up
#if BOARD_ROBERT_BB2
  // On Robert BB2, the BLE chip is behind LDO3, which should always be on.
  prv_update_rail_state(PmicRail_LDO3, true);
#endif

  if (BOARD_CONFIG.mfi_reset_pin.gpio) {
    // We have access to the reset pin on the MFi. Need to hold it low before powering the 2V5
    // rail in order to get the MFi into a working state.
    // In the future if the MFi becomes janky again we can use this to later pull the power.
    gpio_use(BOARD_CONFIG.mfi_reset_pin.gpio);

    GPIO_InitTypeDef gpio_cfg;
    gpio_cfg.GPIO_OType = GPIO_OType_PP;
    gpio_cfg.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio_cfg.GPIO_Mode = GPIO_Mode_OUT;
    gpio_cfg.GPIO_Speed = GPIO_Speed_25MHz;
    gpio_cfg.GPIO_Pin = BOARD_CONFIG.mfi_reset_pin.gpio_pin;
    GPIO_Init(BOARD_CONFIG.mfi_reset_pin.gpio, &gpio_cfg);

    GPIO_WriteBit(BOARD_CONFIG.mfi_reset_pin.gpio, BOARD_CONFIG.mfi_reset_pin.gpio_pin, Bit_RESET);

    gpio_release(BOARD_CONFIG.mfi_reset_pin.gpio);
  }

  return true;
}

static bool prv_update_rail_state(PmicRail rail, bool enable) {
  static int8_t s_ldo2_ref_count = 0;
  static int8_t s_ldo3_ref_count = 0;

  int8_t *ref_count;
  uint8_t rail_control_reg;

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
        psleep(3);

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

    while(1);
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

uint16_t pmic_get_vsys(void) {
  prv_mon_config_lock();
  const PmicMonConfig *mon_config = &MON_CONFIG[PMIC_MON_CONFIG_VSYS_INDEX];
  prv_set_mon_config(mon_config);

  ADCVoltageMonitorReading reading = battery_read_voltage_monitor();
  uint32_t millivolts = battery_convert_reading_to_millivolts(reading, mon_config->ratio, 1);

  prv_set_mon_config_register(0);
  prv_mon_config_unlock();

  return (uint16_t)millivolts;
}

bool pmic_set_charger_state(bool enable) {
  // Defaults to ON
  // LSB is enable bit
  uint8_t register_value;
  if (!prv_read_register(PmicRegisters_CHG_CNTL_A, &register_value)) {
    return false;
  }
  if (enable) {
    register_value |= 0x01;
  } else {
    register_value &= ~0x01;
  }

  bool result = prv_write_register(PmicRegisters_CHG_CNTL_A, register_value);

  return result;
}


bool pmic_is_charging(void) {
  uint8_t val;

  if (!prv_read_register(PmicRegisters_STATUSA, &val)) {
#if defined (TARGET_QEMU)
    // NOTE: When running on QEMU, i2c reads return false. For now, just assume a failed
    // i2c read means we are charging
    return true;
#else
    PBL_LOG(LOG_LEVEL_DEBUG, "Failed to read charging status A register");
    return false;
#endif
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
#if defined (TARGET_QEMU)
    // NOTE: When running on QEMU, i2c reads return false. For now, just assume a failed
    // i2c read means we are connected to a USB cable
    return true;
#else
    PBL_LOG(LOG_LEVEL_DEBUG, "Failed to read charging status B register");
    return false;
#endif
  }

  bool usb_connected = (val >> 3) & 1;

  return usb_connected;
}

void pmic_read_chip_info(uint8_t *chip_id, uint8_t *chip_revision, uint8_t *buck1_vset) {
  prv_read_register(PmicRegisters_CHIP_ID, chip_id);
  prv_read_register(PmicRegisters_CHIP_REV, chip_revision);
  prv_read_register(PmicRegisters_BUCK1_VSET, buck1_vset);
}

static void prv_clear_any_pending_interrupts(void) {
  // Read the Int status registers to clear any pending bits.
  // An interrupt wont fire if the matching bit is already set.
  uint8_t throwaway_read_result;
  prv_read_register(PmicRegisters_INTA, &throwaway_read_result);
  prv_read_register(PmicRegisters_INTB, &throwaway_read_result);
}

static void prv_log_status_registers(const char *preamble) {
  uint8_t status_a;
  uint8_t status_b;

  if (!prv_read_register(PmicRegisters_STATUSA, &status_a) ||
      !prv_read_register(PmicRegisters_STATUSB, &status_b)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Failed to read status registers");
    return;
  }

  PBL_LOG(LOG_LEVEL_INFO, "%s: StatusA = 0x%"PRIx8"; StatusB = 0x%"PRIx8, preamble, status_a,
      status_b);
}

static void prv_debounce_callback(void* data) {
  bool is_connected = pmic_is_usb_connected();

  PBL_LOG(LOG_LEVEL_DEBUG, "Got PMIC debounced interrupt, plugged?: %s bounces: %u",
          is_connected ? "YES" : "NO", s_interrupt_bounce_count);
  s_interrupt_bounce_count = 0;

  if (is_connected) {
    // Configure our charging parameters when the charging cable is connected
    prv_config_charger();
    prv_log_status_registers("PMIC charger configured after charger connected");
  } else {
    prv_log_status_registers("PMIC charge/connection status changed");
  }

  PebbleEvent event = {
    .type = PEBBLE_BATTERY_CONNECTION_EVENT,
    .battery_connection = {
      .is_connected = is_connected,
    }
  };

  event_put(&event);
}

static void prv_handle_pmic_interrupt(void *data) {
  prv_clear_any_pending_interrupts();

  ++s_interrupt_bounce_count;

  new_timer_start(s_debounce_usb_conn_timer, USB_CONN_DEBOUNCE_MS, prv_debounce_callback,
                                                                                NULL, 0 /*flags*/);
}

static void pmic_interrupt_handler(bool *should_context_switch) {
  system_task_add_callback_from_isr(prv_handle_pmic_interrupt, NULL, should_context_switch);
}


/* Private Function Implementations */
static bool prv_is_alive(void) {
  uint8_t val;
  prv_read_register(0x00, &val);

  if (val == 0x01) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Found the max14690");
    return true;
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG,
            "Error: read max14690 whomai byte 0x%x, expecting 0x%x", val, 0x01);
    return false;
  }
}

static bool prv_set_pin_config(void) {
  periph_config_acquire_lock();

  // Initialize the GPIOs for the 4V5 & 6V6 rails
  gpio_output_init(&BOARD_CONFIG_POWER.rail_4V5_ctrl, GPIO_OType_OD, GPIO_Speed_50MHz);
  if (BOARD_CONFIG_POWER.rail_6V6_ctrl.gpio) {
    gpio_output_init(&BOARD_CONFIG_POWER.rail_6V6_ctrl, BOARD_CONFIG_POWER.rail_6V6_ctrl_otype,
                     GPIO_Speed_50MHz);
  }

  // Interrupt config
  gpio_input_init_pull_up_down(&BOARD_CONFIG_POWER.pmic_int_gpio, GPIO_PuPd_UP);

  periph_config_release_lock();

  return true;
}

static void prv_initialize_interrupts(void) {
  exti_configure_pin(BOARD_CONFIG_POWER.pmic_int, ExtiTrigger_Falling, pmic_interrupt_handler);
  exti_enable(BOARD_CONFIG_POWER.pmic_int);

  // Enable the UsbOk interrupt in the IntMaskA register
  prv_write_register(PmicRegisters_INT_MASK_A, 0x08);

  prv_clear_any_pending_interrupts();
}

static void register_dump(int start_reg, int stop_reg) {
  int reg;
  uint8_t val;
  char buffer[16];
  for (reg = start_reg; reg <= stop_reg; reg ++) {
    prv_read_register(reg, &val);
    prompt_send_response_fmt(buffer, sizeof(buffer), "Reg 0x%02X: 0x%02X", reg, val);
  }
}

void command_pmic_read_registers(void) {
  register_dump(0x00, 0x1F);
}

#ifdef RECOVERY_FW
void command_pmic_rails(void) {
  prv_mon_config_lock();

  // Make sure the LDO3 rail is on before measuring it.
  set_ldo3_power_state(true);

  for (size_t i = 0; i < ARRAY_LENGTH(MON_CONFIG); ++i) {
    prv_set_mon_config(&MON_CONFIG[i]);
    ADCVoltageMonitorReading reading = battery_read_voltage_monitor();
    uint32_t millivolts = battery_convert_reading_to_millivolts(reading, MON_CONFIG[i].ratio, 1);

    char buffer[40];
    prompt_send_response_fmt(buffer, sizeof(buffer), "%-15s: %"PRIu32" mV",
                             MON_CONFIG[i].name, millivolts);
  }

  // Turn this off again now that we're done measuring. This is refcounted so there's no concern
  // that we may be turning it off if it was on before we started measuring.
  set_ldo3_power_state(false);

  prv_mon_config_unlock();
}
#endif // RECOVERY_FW

void set_ldo3_power_state(bool enabled) {
  i2c_use(I2C_MAX14690);
  prv_update_rail_state(PmicRail_LDO3, enabled);
  i2c_release(I2C_MAX14690);
}

void set_4V5_power_state(bool enabled) {
  gpio_output_set(&BOARD_CONFIG_POWER.rail_4V5_ctrl, enabled);
}

void set_6V6_power_state(bool enabled) {
  PBL_ASSERTN(BOARD_CONFIG_POWER.rail_6V6_ctrl.gpio);
  gpio_output_set(&BOARD_CONFIG_POWER.rail_6V6_ctrl, enabled);
}

