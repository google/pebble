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

#include "drivers/rtc.h"
#include "system/logging.h"

#include <inttypes.h>

/** Power Profiling
 *  ===============
 *  There are two main types of power consumers on the Pebble Smartwatch:
 *  - Discrete systems   (one or more independent power states).
 *  - Continuous systems (a continuum of power draw eg. the PWM-ed backlight).
 *
 *  The discrete systems will have their power profiled in a time-binned manner
 *  where the on-time of each state is integrated over a pre-determined period.
 *  General rule of thumb is that non-quiescent states should be tracked.
 *      eg. don't track BT sniff mode, but do track Active mode because it is more
 *      of an unusual condition...
 *
 *  The continuous systems will just dump their current state whenever it
 *  is changed.
 */

// enum that most discrete consumers can fall into.

// TODO: implement tracking on these in decreasing priority order:
// Spi1, BtShutdown, BtDeepSleep
// Tim1, Tim3, Tim4, I2C1, I2C2
// AccelLowPower, AccelNormal, Mag,
// probably will not implement(either very low power, or constantly on):
// 5vReg, Pwr, Adc1, Adc2, Ambient, Usart3
typedef enum {
  PowerSystem2v5Reg = 0,
  PowerSystem5vReg,
  PowerSystemMcuCoreSleep,
  PowerSystemMcuCoreRun,
  PowerSystemMcuGpioA,
  PowerSystemMcuGpioB,
  PowerSystemMcuGpioC,
  PowerSystemMcuGpioD,
  PowerSystemMcuGpioH,
  PowerSystemMcuCrc,       // Flash
  PowerSystemMcuPwr,       // Everything
  PowerSystemMcuDma1,      // Display
  PowerSystemMcuDma2,      // BT
  PowerSystemMcuTim1,      // Future use for the vibe PWM
  PowerSystemMcuTim3,      // Used for the backlight PWM
  PowerSystemMcuTim4,      // Used for the button debouncer
  PowerSystemMcuUsart1,    // Used for BT
  PowerSystemMcuUsart3,    // dbgserial
  PowerSystemMcuI2C1,      // Main I2C
  PowerSystemMcuI2C2,      // 2V5 I2C
  PowerSystemMcuSpi1,      // FLASH
#if PLATFORM_TINTIN || PLATFORM_SILK
  PowerSystemMcuSpi2,      // LCD
#else
  PowerSystemMcuSpi6,      // LCD
#endif
  PowerSystemMcuAdc1,      // Voltage monitoring & ambient light sensing
  PowerSystemMcuAdc2,      // Voltage monitoring & ambient light sensing
  PowerSystemFlashRead,
  PowerSystemFlashWrite,
  PowerSystemFlashErase,
  PowerSystemAccelLowPower,
  PowerSystemAccelNormal,
  PowerSystemMfi,
  PowerSystemMag,
  PowerSystemBtShutdown,
  PowerSystemBtDeepSleep,
  PowerSystemBtActive,
  PowerSystemAmbient,       
  PowerSystemProfiling,     // So that we can diminish the effects that dumping the profile logs has

  num_power_systems,
} PowerSystem;

void power_tracking_init(void);
void power_tracking_start(PowerSystem system);
void power_tracking_stop(PowerSystem system);

#if defined(SW_POWER_TRACKING)
  #define PWR_TRACK(system, state_fmt, args...) \
      { \
        power_tracking_start(PowerSystemProfiling); \
        char buffer[64]; \
        dbgserial_putstr_fmt(buffer, sizeof(buffer), ">>>PWR:%"PRIu64",%s,"state_fmt"<", rtc_get_ticks(), system, ## args); \
        power_tracking_stop(PowerSystemProfiling); \
      }
#else
  #define PWR_TRACK(system, state_fmt, args...)
#endif


#define PWR_TRACK_BATT(chg_state, voltage)        PWR_TRACK("Battery", "%s,%u", chg_state, voltage)
#define PWR_TRACK_ACCEL(state, frequency)         PWR_TRACK("Accel", "%s,%u", state, frequency)
#define PWR_TRACK_MAG(state, adc_rate)            PWR_TRACK("Mag", "%s,%u", state, adc_rate)
#define PWR_TRACK_VIBE(state, freq, duty)         PWR_TRACK("Vibe", "%s,%u,%u", state, freq, duty)
#define PWR_TRACK_BACKLIGHT(state, freq, duty)    PWR_TRACK("Backlight", "%s,%"PRIu32",%u", state, freq, duty)
