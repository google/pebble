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
#include <stdbool.h>
#include <sys/types.h> // time_t and size_t

#define TM_YEAR_ORIGIN  1900
#define EPOCH_YEAR      1970
#define EPOCH_WDAY      4

#define DAYS_PER_WEEK  7
#define MONTHS_PER_YEAR 12

#define MS_PER_SECOND (1000)

#define SECONDS_PER_MINUTE (60)
#define MS_PER_MINUTE (MS_PER_SECOND * SECONDS_PER_MINUTE)
#define MINUTES_PER_HOUR (60)
#define SECONDS_PER_HOUR (SECONDS_PER_MINUTE * MINUTES_PER_HOUR)

#define HOURS_PER_DAY (24)
#define MINUTES_PER_DAY (HOURS_PER_DAY * MINUTES_PER_HOUR)
#define SECONDS_PER_DAY (MINUTES_PER_DAY * SECONDS_PER_MINUTE)
#define SECONDS_PER_WEEK (SECONDS_PER_DAY * DAYS_PER_WEEK)

#define YEAR_IS_LEAP(y) ((((y) % 4) == 0 && ((y) % 100) != 0) || ((y) % 400) == 0)

#define IS_WEEKDAY(d) (d >= Monday && d <= Friday)
#define IS_WEEKEND(d) (d == Saturday || d == Sunday)

#define TZ_LEN 6

// DST special cases. These map to indexes in the tools/timezones.py script that handles parsing
// the olson database into a compressed form. Don't change these without changing the script.
//
// Note that we don't correctly handle Morroco's DST rules, they're incredibly complex due to them
// suspending DST each year for Ramadan, resulting in 4 DST transitions each year.
//
// Any DST ids that aren't listed below have sane DST rules, where they change to DST in the
// spring on the same day by 1 hour each year and change from DST on a later day each year.
#define DSTID_BRAZIL 6
#define DSTID_LORDHOWE 20


//! @file time.h

//! @addtogroup StandardC Standard C
//! @{
//!   @addtogroup StandardTime Time
//! \brief Standard system time functions
//!
//! This module contains standard time functions and formatters for printing.
//! Note that Pebble now supports both local time and UTC time
//! (including timezones and daylight savings time).
//! Most of these functions are part of the C standard library which is documented at
//! https://sourceware.org/newlib/libc.html#Timefns
//! @{


//! structure containing broken-down time for expressing calendar time
//! (ie. Year, Month, Day of Month, Hour of Day) and timezone information
struct tm {
  int tm_sec;     /*!< Seconds. [0-60] (1 leap second) */
  int tm_min;     /*!< Minutes. [0-59] */
  int tm_hour;    /*!< Hours.  [0-23] */
  int tm_mday;    /*!< Day. [1-31] */
  int tm_mon;     /*!< Month. [0-11] */
  int tm_year;    /*!< Years since 1900 */
  int tm_wday;    /*!< Day of week. [0-6] */
  int tm_yday;    /*!< Days in year.[0-365] */
  int tm_isdst;   /*!< DST. [-1/0/1] */

  int tm_gmtoff;  /*!< Seconds east of UTC */
  char tm_zone[TZ_LEN]; /*!< Timezone abbreviation */
};

//! Enumeration of each day of the week.
typedef enum DayInWeek {
  Sunday = 0,
  Monday,
  Tuesday,
  Wednesday,
  Thursday,
  Friday,
  Saturday,
} DayInWeek;

//! Obtain the number of seconds and milliseconds part since the epoch.
//!   This is a non-standard C function provided for convenience.
//! @param tloc Optionally points to an address of a time_t variable to store the time in.
//!   You may pass NULL into tloc if you don't need a time_t variable to be set
//!   with the seconds since the epoch
//! @param out_ms Optionally points to an address of a uint16_t variable to store the
//!   number of milliseconds since the last second in.
//!   If you only want to use the return value, you may pass NULL into out_ms instead
//! @return The number of milliseconds since the last second
uint16_t time_ms(time_t *tloc, uint16_t *out_ms);

//!   @} // end addtogroup StandardTime
//! @} // end addtogroup StandardC


// The below standard c time functions are documented for the SDK
// by their applib pbl_override wrappers in pbl_std.h

struct tm *localtime(const time_t *timep);

struct tm *gmtime(const time_t *timep);

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

time_t mktime(struct tm *tb);

struct tm *gmtime_r(const time_t *timep, struct tm *result);

struct tm *localtime_r(const time_t *timep, struct tm *result);

//! Minimal struct to store timezone info in RTC registers
typedef struct TimezoneInfo {
  char tm_zone[TZ_LEN - 1];   //!< Up to 5 character (no null terminator) timezone abbreviation
  uint8_t dst_id;         //!< Daylight savings time zone index
  int16_t timezone_id;    //!< Olson index of timezone
  int32_t tm_gmtoff;      //!< GMT time offset
  time_t dst_start;   //!< timestamp of start of daylight savings period (0 if none)
  time_t dst_end;     //!< timestamp of end of daylight savings period (0 if none)
} TimezoneInfo;

//! Provides the timezone abbreviation string for the given time. Uses the utc_time provided
//! to correct the abbreviation for daylight savings time if applicable
//! @param out_buf should have length TZ_LEN
void time_get_timezone_abbr(char *out_buf, time_t utc_time);

//! Provides the gmt offset
int32_t time_get_gmtoffset(void);

//! Returns true if the UNIX time provided falls within DST
bool time_get_isdst(time_t utc_time);

//  0 = no transition
// <0 = dst begins between prev and next
// >0 = dst ends between prev and next
// returns difference in seconds of DST
int time_will_transition_dst(time_t prev, time_t next);

//! Returns the DST offset
int32_t time_get_dstoffset(void);

//! Returns the DST start timestamp
time_t time_get_dst_start(void);

//! Returns the DST end timestamp
time_t time_get_dst_end(void);

//! Convert UTC time, as returned by rtc_get_time() into local time
time_t time_utc_to_local(time_t utc_time);

//! Convert local time to UTC time
time_t time_local_to_utc(time_t local_time);

void time_util_split_seconds_into_parts(uint32_t seconds, uint32_t *day_part,
    uint32_t *hour_part, uint32_t *minute_part, uint32_t *second_part);

int time_util_get_num_hours(int hours, bool is24h);

int time_util_get_seconds_until_daily_time(struct tm *time, int hour, int minute);

//! Set the timezone
void time_util_update_timezone(const TimezoneInfo *tz_info);

time_t time_util_get_midnight_of(time_t ts);

bool time_util_range_spans_day(time_t start, time_t end, time_t start_of_day);

//! User-mode access calls
time_t sys_time_utc_to_local(time_t t);

time_t time_util_utc_to_local_offset(void);

time_t time_utc_to_local_using_offset(time_t utc_time, int16_t utc_offset_min);

time_t time_local_to_utc_using_offset(time_t local_time, int16_t utc_offset_min);

//! Computes the day index from UTC seconds. This index should change every day at midnight local
//! time
//! @param utc_sec Time to retrieve index for
//! @return Day index
uint16_t time_util_get_day(time_t utc_sec);

DayInWeek time_util_get_day_in_week(time_t utc_sec);

//! Computes the minute of the day
//! @param utc_sec Time to retrieve minute for
//! @return Minute of day
int time_util_get_minute_of_day(time_t utc_sec);

//! Adds a delta to the minute of the day and will wrap to the next or previous day if the
//! resulting minutes would have been out of bounds
//! @param minute Minute to adjust
//! @param delta Minutes to add to \ref minute
//! @return Adjusted minutes
int time_util_minute_of_day_adjust(int minute, int delta);

//! Return the UTC time that corresponds to the start of today (midnight).
//! @return the UTC time corresponding to the start of today (midnight)
time_t time_start_of_today(void);

//! Return the number of seconds since the system was restarted. This time is based on the
//! tickcount and so, unlike rtc_get_time(), it won't be affected if the phone changes the UTC
//! time on the watch.
uint32_t time_get_uptime_seconds(void);
