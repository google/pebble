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

#pragma once

#include "services/imu/units.h"
#include "util/size.h"

#define BOARD_LSE_MODE RCC_LSE_Bypass

static const BoardConfig BOARD_CONFIG = {
  .ambient_light_dark_threshold = 150,
  .ambient_k_delta_threshold = 50,
  .photo_en = { GPIOC, GPIO_Pin_0, true },
  .als_always_on = true,

  .dbgserial_int = { EXTI_PortSourceGPIOB, 5 },

  // new sharp display requires 30/60Hz so we feed it directly from PMIC
  .lcd_com = { 0 },

  .backlight_on_percent = 25,
  .backlight_max_duty_cycle_percent = 67,

  .power_5v0_options = OptionNotPresent,
  .power_ctl_5v0 = { 0 },

  .has_mic = true,
};

static const BoardConfigButton BOARD_CONFIG_BUTTON = {
  .buttons = {
    [BUTTON_ID_BACK] =
        { "Back",   GPIOC, GPIO_Pin_13, { EXTI_PortSourceGPIOC, 13 }, GPIO_PuPd_NOPULL },
    [BUTTON_ID_UP] =
        { "Up",     GPIOD, GPIO_Pin_2, { EXTI_PortSourceGPIOD, 2 }, GPIO_PuPd_DOWN },
    [BUTTON_ID_SELECT] =
        { "Select", GPIOH, GPIO_Pin_0, { EXTI_PortSourceGPIOH, 0 }, GPIO_PuPd_DOWN },
    [BUTTON_ID_DOWN] =
        { "Down",   GPIOH, GPIO_Pin_1, { EXTI_PortSourceGPIOH, 1 }, GPIO_PuPd_DOWN },
  },
  .button_com = { 0 },
  .active_high = true,
};

static const BoardConfigPower BOARD_CONFIG_POWER = {
  .pmic_int = { EXTI_PortSourceGPIOC, 7 },
  .pmic_int_gpio = {
    .gpio = GPIOC,
    .gpio_pin = GPIO_Pin_7,
  },

  .battery_vmon_scale = {
    // Battery voltage is scaled down by a pair of resistors:
    //  - R13 on the top @ 47k
    //  - R15 on the bottom @ 30.1k
    //   (R13 + R15) / R15 = 77.1 / 30.1
    .numerator = 771,
    .denominator = 301,
  },

  .vusb_stat = { .gpio = GPIO_Port_NULL, },
  .chg_stat = { GPIO_Port_NULL },
  .chg_fast = { GPIO_Port_NULL },
  .chg_en = { GPIO_Port_NULL },
  .has_vusb_interrupt = false,

  .wake_on_usb_power = false,

  .charging_status_led_voltage_compensation = 0,

#if defined(IS_BIGBOARD) && !defined(BATTERY_DEBUG)
  // We don't use the same batteries on all bigboards, so set a safe cutoff voltage of 4.2V.
  // Please do not change this!
  .charging_cutoff_voltage = 4200,
#else
  .charging_cutoff_voltage = 4300,
#endif

  .low_power_threshold = 5,

  // Based on measurements from v4.0-beta16.
  // Typical Connected Current at VBAT without HRM ~520uA
  // Added draw with HRM on : ~1.5mA ==> Average impact (5% per hour + 1 hour continuous / day)
  //    (.05 * 23/24 + 1.0 * 1/24) * 1.5mA = ~134uA
  // Assume ~150uA or so for notifications & user interaction
  // Total Hours = 125 mA * hr / (.520 + .134 + 150)mA = 155 hours
  .battery_capacity_hours = 155,
};

static const BoardConfigAccel BOARD_CONFIG_ACCEL = {
  .accel_config = {
    .axes_offsets[AXIS_X] = 0,
    .axes_offsets[AXIS_Y] = 1,
    .axes_offsets[AXIS_Z] = 2,
#if IS_BIGBOARD
    .axes_inverts[AXIS_X] = false,
    .axes_inverts[AXIS_Y] = false,
    .axes_inverts[AXIS_Z] = false,
#else
    .axes_inverts[AXIS_X] = true,
    .axes_inverts[AXIS_Y] = true,
    .axes_inverts[AXIS_Z] = true,
#endif
    // This is affected by the accelerometer's configured ODR, so this value
    // will need to be tuned again once we stop locking the BMA255 to an ODR of
    // 125 Hz.
    .shake_thresholds[AccelThresholdHigh] = 64,
    .shake_thresholds[AccelThresholdLow] = 0xf,
    .double_tap_threshold = 12500,
  },
  .accel_int_gpios = {
    [0] = { GPIOA, GPIO_Pin_6 },
    [1] = { GPIOA, GPIO_Pin_3 },
  },
  .accel_ints = {
    [0] = { EXTI_PortSourceGPIOA, 6 },
    [1] = { EXTI_PortSourceGPIOA, 3 }
  },
};

static const BoardConfigActuator BOARD_CONFIG_VIBE = {
  .options = ActuatorOptions_Pwm,
  .ctl = { 0 },
  .pwm = {
    .output = { GPIOA, GPIO_Pin_7, true },
    .timer = {
      .peripheral = TIM14,
      .config_clock = RCC_APB1Periph_TIM14,
      .init = TIM_OC1Init,
      .preload = TIM_OC1PreloadConfig
    },
    .afcfg = { GPIOA, GPIO_Pin_7, GPIO_PinSource7, GPIO_AF_TIM14 },
  },
  .vsys_scale = 3300,
};

static const BoardConfigActuator BOARD_CONFIG_BACKLIGHT = {
  .options = ActuatorOptions_Pwm | ActuatorOptions_Ctl,
  .ctl = { GPIOB, GPIO_Pin_13, true },
  .pwm = {
    .output = { GPIOC, GPIO_Pin_6, true },
    .timer = {
      .peripheral = TIM3,
      .config_clock = RCC_APB1Periph_TIM3,
      .init = TIM_OC1Init,
      .preload = TIM_OC1PreloadConfig
    },
    .afcfg = { GPIOC, GPIO_Pin_6, GPIO_PinSource6, GPIO_AF_TIM3 },
  },
};

#define ACCESSORY_UART_IS_SHARED_WITH_BT 1
static const BoardConfigAccessory BOARD_CONFIG_ACCESSORY = {
  .exti = { EXTI_PortSourceGPIOA, 11 },
};

static const BoardConfigBTCommon BOARD_CONFIG_BT_COMMON = {
  .controller = DA14681,
  .reset = { GPIOC, GPIO_Pin_5, true },
  .wakeup = {
    .int_gpio = { GPIOC, GPIO_Pin_4 },
    .int_exti = { EXTI_PortSourceGPIOC, 4 },
  },
};

static const BoardConfigBTSPI BOARD_CONFIG_BT_SPI = {
  .cs = { GPIOB, GPIO_Pin_1, false },
};

static const BoardConfigMCO1 BOARD_CONFIG_MCO1 = {
  .output_enabled = true,
  .af_cfg = {
    .gpio = GPIOA,
    .gpio_pin = GPIO_Pin_8,
    .gpio_pin_source = GPIO_PinSource8,
    .gpio_af = GPIO_AF_MCO,
  },
  .an_cfg = {
    .gpio = GPIOA,
    .gpio_pin = GPIO_Pin_8,
  },
};

static const BoardConfigSharpDisplay BOARD_CONFIG_DISPLAY = {
  .spi = SPI2,
  .spi_gpio = GPIOB,
  .spi_clk = RCC_APB1Periph_SPI2,
  .spi_clk_periph = SpiPeriphClockAPB1,

  .clk = { GPIOB, GPIO_Pin_10, GPIO_PinSource10, GPIO_AF_SPI2 },
  .mosi = { GPIOB, GPIO_Pin_15, GPIO_PinSource15, GPIO_AF_SPI2 },
  .cs = { GPIOB, GPIO_Pin_9, true },

  .on_ctrl = { GPIOA, GPIO_Pin_0, true },
  .on_ctrl_otype = GPIO_OType_PP,
};

#define DIALOG_TIMER_IRQ_HANDLER TIM6_IRQHandler
static const TimerIrqConfig BOARD_BT_WATCHDOG_TIMER = {
  .timer = {
    .peripheral = TIM6,
    .config_clock = RCC_APB1Periph_TIM6,
  },
  .irq_channel = TIM6_IRQn,
};

extern DMARequest * const COMPOSITOR_DMA;
extern DMARequest * const SHARP_SPI_TX_DMA;

extern UARTDevice * const QEMU_UART;
extern UARTDevice * const DBG_UART;
extern UARTDevice * const ACCESSORY_UART;

extern UARTDevice * const BT_TX_BOOTROM_UART;
extern UARTDevice * const BT_RX_BOOTROM_UART;

extern I2CSlavePort * const I2C_AS3701B;
extern I2CSlavePort * const I2C_AS7000;

extern const VoltageMonitorDevice * VOLTAGE_MONITOR_ALS;
extern const VoltageMonitorDevice * VOLTAGE_MONITOR_BATTERY;

extern const TemperatureSensor * const TEMPERATURE_SENSOR;

extern HRMDevice * const HRM;

extern QSPIPort * const QSPI;
extern QSPIFlash * const QSPI_FLASH;

extern MicDevice * const MIC;

extern SPISlavePort * const DIALOG_SPI;
