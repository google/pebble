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

#include <stdint.h>

// This is always invalid because it includes a value being set in the reserved field.
#define I2C_TIMINGR_INVALID_VALUE (0xffffffff)

typedef enum I2CBusMode {
  I2CBusMode_Standard,  ///< I2C Standard Mode (up to 100 kHz)
  I2CBusMode_FastMode,  ///< I2C Fast Mode (up to 400 kHz)
  I2CBusMode_FastModePlus,  ///< I2C Fast Mode Plus (up to 1 MHz)
} I2CBusMode;

uint32_t i2c_timingr_calculate(uint32_t i2c_clk_frequency,
                               I2CBusMode bus_mode,
                               uint32_t target_bus_frequency,
                               uint32_t rise_time_ns,
                               uint32_t fall_time_ns);
