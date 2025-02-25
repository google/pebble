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

#include "util/time/time.h"

typedef uint64_t RtcTicks;

#define RTC_TICKS_HZ (1024u)

//! Initialize the RTC driver at startup. Note that this runs very early in the startup process
//! and very few other systems will be running when this is called.
void rtc_init(void);

//! Calibrate the RTC driver using the given crystal frequency (in mHz).
//! This is a separate step because rtc_init needs to run incredibly early in the startup process
//! and the manufacturing registry won't be initialized yet.
void rtc_calibrate_frequency(uint32_t frequency);

//! Initialize any timers the RTC driver may need. This is a separate step than rtc_init because
//! rtc_init needs to run incredibly early in the startup process and at that time the timer
//! system won't be initialized yet.
void rtc_init_timers(void);

// RTC time
///////////////////////////////////////////////////////////////////////////////

//! We only support keeping time in the range of the year 2000 to the year 2037. Call this
//! function to adjust a given time into this range by simply clamping the year value back
//! into range without adjusting any of the other fields.
//! @return True if a change had to be made
bool rtc_sanitize_struct_tm(struct tm* t);
//! Wrapper for rtc_sanitize_struct_tmtime
bool rtc_sanitize_time_t(time_t* t);

//! Updates the current time.
//! We only support times with years between 2000 and 2037. Attempting to set times outside of this
//! range will result in an assert being tripped.
void rtc_set_time(time_t time);
time_t rtc_get_time(void);

// Wrappers for the above functions that take struct tm instead of time_t
void rtc_set_time_tm(struct tm* time_tm);
void rtc_get_time_tm(struct tm* time_tm);

// FIXME: PBL-41066 this should just return a uint64_t
//! @return millisecond port of the current second.
void rtc_get_time_ms(time_t* out_seconds, uint16_t* out_ms);

//! Saves the timezone_info to RTC registers
void rtc_set_timezone(TimezoneInfo *tzinfo);

//! Returns timezone_info from RTC registers
void rtc_get_timezone(TimezoneInfo *tzinfo);

//! Returns timezone region id from RTC registers
uint16_t rtc_get_timezone_id(void);

//! Returns if the system has timezone set, RTC in UTC mode
bool rtc_is_timezone_set(void);

#define TIME_STRING_BUFFER_SIZE 26

//! @param buffer Buffer used to write the string into. Must be at least TIME_STRING_BUFFER_SIZE
const char* rtc_get_time_string(char* buffer);


// RTC ticks
///////////////////////////////////////////////////////////////////////////////

//! @return Absolute number of ticks since system start.
RtcTicks rtc_get_ticks(void);


// RTC Alarm
///////////////////////////////////////////////////////////////////////////////

//! Initializes the RTC alarm functionality. We use this for waking us out of stop mode.
void rtc_alarm_init(void);

//! Set the alarm to go off num_ticks from now.
void rtc_alarm_set(RtcTicks num_ticks);

//! Clear the timezone registers (as part of factory reset)
void rtc_timezone_clear(void);

//! @return the number of ticks that have elapsed since rtc_alarm_set was last called.
RtcTicks rtc_alarm_get_elapsed_ticks(void);

//! @return True if the RTC alarm functionality has been initialized. This can be used to prevent
//!     us from going into stop mode before we're ready to wake up from it.
bool rtc_alarm_is_initialized(void);


// Utility Functions
///////////////////////////////////////////////////////////////////////////////

//! @param buffer Buffer used to write the string into. Must be at least TIME_STRING_BUFFER_SIZE
const char* time_t_to_string(char* buffer, time_t t);

