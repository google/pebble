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

//! @file timezone_database.h
//!
//! Functionality for reading the timezone database that we have stored in resources.

//! FIXME: Rename and document values
typedef enum {
  TIMEZONE_FLAG_DAY_DECREMENT = 1 << 0,
  TIMEZONE_FLAG_STANDARD_TIME = 1 << 1,
  TIMEZONE_FLAG_UTC_TIME = 1 << 2,
} DSTRuleFlags;

//! A structure describing when a given DST rule transitions from DST to standard time or from
//! standard time to DST. Note that this struct matches our storage format exactly, so don't
//! change it without changing the underlying format.
typedef struct {
  //! Describes the type of DSTRule this is. Possible values are 'D' for entering daylight savings
  //! time, 'S' for leaving daylight savings time and entering standard time, or '\0' for timezones
  //! that don't observe DST.
  char ds_label;
  //! Which day of the week this rule is observed.
  //! 0-indexed, starting with Sunday (ie Monday is 1, Tuesday is 2...).
  //! A value of 255 indicates that this rule applies to any day of the week.
  uint8_t wday;
  //! A bitset of flags, see DSTRuleFlags.
  uint8_t flag;
  //! Month to make the transition
  //! 0 is January, 11 is December
  uint8_t month;
  //! Day of the month
  //! Not zero indexed, 1 is the first day of the month
  uint8_t mday;
  //! Hour of the day, range [0-23]
  uint8_t hour;
  //! Minute of the hour
  uint8_t minute;

  uint8_t padding;
} TimezoneDSTRule;


//! @return The number of timezone regions we have in our database
int timezone_database_get_region_count(void);

//! Load a timezone region for a given region id.
//! Note, this does not populate the actual bounds of the current DST period and instead leaves
//! the .dst_start and .dst_end members in tz_info uninitialized.
//!
//! @param The region ID to look up
//! @param tz_info[out] The TimezoneInfo structure to populate with the region
bool timezone_database_load_region_info(uint16_t region_id, TimezoneInfo *tz_info);

//! Load a timezone name for a given region ID.
//!
//! @param region_id The region ID to look up
//! @param region_name[out] The resulting null-terminated name, including both the continent and
//!                         the city name. This buffer must be at least TIMEZONE_NAME_LENGTH long
//!                         in bytes.
//! @return True if successful, false if the region ID was invalid.
bool timezone_database_load_region_name(uint16_t region_id, char *region_name);

//! Load a pair of DST rules for the given id.
//!
//! @param dst_id The DST rule ID to look up
//! @param start[out] a TimezoneDSTRule structure to populate with the rule to enter DST
//! @param start[out] a TimezoneDSTRule structure to populate with the rule to leave DST
//! @return true if successful, false if the dst_id is invalid or the database is malformed
bool timezone_database_load_dst_rule(uint8_t dst_id, TimezoneDSTRule *start, TimezoneDSTRule *end);

//! Find a region ID for the given region name.
//! @return a valid, matching region ID, or -1 if no region was found
int timezone_database_find_region_by_name(const char *region_name, int region_name_length);
