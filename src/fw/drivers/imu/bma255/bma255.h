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

#include <stdbool.h>
#include <stdint.h>

void bma255_init(void);
bool bma255_query_whoami(void);

//! Power Modes
//! These are the supported power modes, and some rough estimates on power consumption.
//! There is a small set of transitions between power modes that are supported. To make life
//! easy, we will always go through Normal Mode, which allows transition to/from all power modes.
//! Use this enum to index into the \ref s_power_mode table, which contains configurations for each.
typedef enum {
  BMA255PowerMode_Normal = 0,  // 130uA
  BMA255PowerMode_Suspend,     // 2.1uA
  BMA255PowerMode_Standby,     // 62uA
  BMA255PowerMode_DeepSuspend, // 1uA
  BMA255PowerMode_LowPower1,
  BMA255PowerMode_LowPower2,

  BMA255PowerModeCount
} BMA255PowerMode;

//! Tsleep values.
//! These are defined to the value we pur into the PMU_LPW register.
//! See Table 3 of datasheet: "Sleep Phase Duration"
typedef enum {
  BMA255SleepDuration_0p5ms  = 5,
  BMA255SleepDuration_1ms    = 6,
  BMA255SleepDuration_2ms    = 7,
  BMA255SleepDuration_4ms    = 8,
  BMA255SleepDuration_6ms    = 9,
  BMA255SleepDuration_10ms   = 10,
  BMA255SleepDuration_25ms   = 11,
  BMA255SleepDuration_50ms   = 12,
  BMA255SleepDuration_100ms  = 13,
  BMA255SleepDuration_500ms  = 14,
  BMA255SleepDuration_1000ms = 15,

  BMA255SleepDurationCount
} BMA255SleepDuration;

//! These are the natively supported filter bandwidths of the bma255.
//! Note that power consumption is tightly tied to the filter bandwidth. In
//! order to run acceptably, we need to keep a bandwidth up in the 500Hz ~ 1kHz range.
//! Please see discussion below for more information on Bandwith, TSleep and ODR.
typedef enum {
  BMA255Bandwidth_7p81HZ  = 8,
  BMA255Bandwidth_15p63HZ = 9,
  BMA255Bandwidth_31p25HZ = 10,
  BMA255Bandwidth_62p5HZ  = 11,
  BMA255Bandwidth_125HZ   = 12,
  BMA255Bandwidth_250HZ   = 13,
  BMA255Bandwidth_500HZ   = 14,
  BMA255Bandwidth_1000HZ  = 15,

  BMA255BandwidthCount
} BMA255Bandwidth;

//! In order to achieve low power consumptions, the BMA255 Output Data Rate (ODR)
//! is determined by a combination of:
//!    - high-bandwidth operating rate:
//!        Less filtering is done on the bma255, which has a direct impact on power consumption.
//!        This gives a lower "update time", which in turn means less "active time" of the device.
//!        The trade-off here is that accelerometer data is a bit more susceptible to noise.
//!    - sleep time:
//!        The longer the sleep duration, the less power the device consums.
//!        After tsleep ms, a sample is taken, and then the device goes back to sleep.
//!
//! Power measurements on the board have shown we ideally want to run at a BW of 500Hz or 1000Hz.
//! Unfortunately, there is an issue with data jumps when running in low power modes.
//! At 4G sensitivity, we need to run at a bandwidth lower than 500Hz in order to minimize
//! jitter in readings. This means we probably want to stay at 250Hz.
//!
//! We are using Equidistant Sampling Mode (EST) to ensure that samples are taken
//! at equal time distances. See Figure 4 in the datasheet for an explanation of this.
//! In EST, a sample is taken every tsample ms, where tsample = (tsleep + wkup_time) [1]
//!
//! We can _approximate_ actual ODR as the following: [2]
//!        ODR = (1000 / (tsleep + wkup_time))
//!   where tsleep holds the property that:
//!        N = (2 * bw) * (tsleep / 1000) such that N is an Integer. [3][4]
//!   and wkup_time is taken for the corresponding bandwidth from Table 4 of the datasheet.
//!
//! [1] This is the best we can gather as a good approximation after a meeting with Bosch.
//! [2] This is only an approximation as the BMA255 part is only guaranteed to have
//!     Bandwidth accuracy within +/- 10%
//! [3] See p.16 of datasheet. Note that the formula in the datasheet is confirmed wrong by Bosch.
//! [4] Take note that all tsleep values are supported when running at 500Hz
//!
typedef enum {
  BMA255ODR_1HZ = 0,
  BMA255ODR_10HZ,
  BMA255ODR_19HZ,
  BMA255ODR_83HZ,
  BMA255ODR_125HZ,
  BMA255ODR_166HZ,
  BMA255ODR_250HZ,

  BMA255ODRCount
} BMA255ODR;

//! Note that these sample intervals are approximations.
typedef enum {
  BMA255SampleInterval_1HZ   = (1000000 / 1),
  BMA255SampleInterval_10HZ  = (1000000 / 10),
  BMA255SampleInterval_19HZ  = (1000000 / 19),
  BMA255SampleInterval_83HZ  = (1000000 / 83),
  BMA255SampleInterval_125HZ = (1000000 / 125),
  BMA255SampleInterval_166HZ = (1000000 / 166),
  BMA255SampleInterval_250HZ = (1000000 / 250),
} BMA255SampleInterval;
