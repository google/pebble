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

#include "display.h"

#include "drivers/button_id.h"

#include "stm32f7xx.h"

#include <stdint.h>
#include <stdbool.h>

#define GPIO_Port_NULL ((GPIO_TypeDef *) 0)
#define GPIO_Pin_NULL ((uint16_t)0x0000)

// This is generated in order to facilitate the check within the IRQ_MAP macro below
enum {
#define IRQ_DEF(num, irq) IS_VALID_IRQ__##irq,
#include "irq_stm32f7.def"
#undef IRQ_DEF
};

//! Creates a trampoline to the interrupt handler defined within the driver
#define IRQ_MAP(irq, handler, device) \
  void irq##_IRQHandler(void) { \
    handler(device); \
  } \
  _Static_assert(IS_VALID_IRQ__##irq || true, "(See comment below)")
/*
 * The above static assert checks that the requested IRQ is valid by checking that the enum
 * value (generated above) is declared. The static assert itself will not trip, but you will get
 * a compilation error from that line if the IRQ does not exist within irq_stm32*.def.
 */

typedef struct {
  GPIO_TypeDef* const gpio; ///< One of GPIOX. For example, GPIOA.
  const uint32_t gpio_pin; ///< One of GPIO_Pin_X.
} InputConfig;

typedef struct {
  GPIO_TypeDef* const gpio; ///< One of GPIOX. For example, GPIOA.
  const uint32_t gpio_pin; ///< One of GPIO_Pin_X.
  bool active_high; ///< Pin is active high or active low
} OutputConfig;

//! Alternate function pin configuration
//! Used to configure a pin for use by a peripheral
typedef struct {
  GPIO_TypeDef* const gpio; ///< One of GPIOX. For example, GPIOA.
  const uint32_t gpio_pin; ///< One of GPIO_Pin_X.
  const uint16_t gpio_pin_source; ///< One of GPIO_PinSourceX.
  const uint8_t  gpio_af; ///< One of GPIO_AF_X
} AfConfig;

typedef struct {
  InputConfig input;
  GPIOPuPd_TypeDef pupd;
} ButtonConfig;

// Button Configuration
/////////////////////////////////////////////////////////////////////////////
typedef struct {
  const ButtonConfig buttons[NUM_BUTTONS];
} BoardConfigButton;

// Power Configuration
/////////////////////////////////////////////////////////////////////////////
typedef struct {
  //! Voltage rail control lines
  const OutputConfig rail_4V5_ctrl;
  const OutputConfig rail_6V6_ctrl;
} BoardConfigPower;

typedef struct {
  OutputConfig reset_gpio;
} BoardConfigFlash;

typedef struct {
  const OutputConfig power_en; //< Enable power supply to the accessory connector.
} BoardConfigAccessory;

typedef const struct SPIBus SPIBus;
typedef const struct SPISlavePort SPISlavePort;
typedef const struct I2CBus I2CBus;
typedef const struct I2CSlavePort I2CSlavePort;
typedef const struct ICE40LPDevice ICE40LPDevice;

void board_init(void);

#include "board_definitions.h"
