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

#include "clar.h"

#include "services/normal/timezone_database.h"
#include "services/common/clock.h"

#include "../timezone_fixture.auto.h"

#include "stubs_logging.h"
#include "stubs_passert.h"

#include <string.h>

//! Find a region ID for the given region name.
//! @return a valid, matching region ID, or -1 if no region was found
int timezone_database_find_region_by_name(const char *region_name, int region_name_length);

#include "resource/resource.h"
size_t resource_load_byte_range_system(ResAppNum app_num, uint32_t resource_id,
                                       uint32_t start_offset, uint8_t *data, size_t num_bytes) {
  memcpy(data, ((uint8_t*) s_timezone_database) + start_offset, num_bytes);
  return num_bytes;
}

#define FIND_REGION(name) timezone_database_find_region_by_name(name, strlen(name));

void test_timezone_database__get_region_count(void) {
  // Note this test will break every time we update the timezone database and that's ok. Just
  // make sure the new number is sane and update the expected number.
  cl_assert_equal_i(timezone_database_get_region_count(), 336);
}

void test_timezone_database__find_region_by_name_simple(void) {
  // Unfortunately we don't really care what the resulting region ids are, we should
  // just make sure the ones that exist are there and they're unique from each other.

  const int america_new_york_region = FIND_REGION("America/New_York");
  cl_assert(america_new_york_region != -1);

  const int europe_minsk_region = FIND_REGION("Europe/Minsk");
  cl_assert(europe_minsk_region != -1);

  // Make sure they're unique
  cl_assert(america_new_york_region != europe_minsk_region);

  // Look up one that doesn't exist
  const int america_waterloo_region = FIND_REGION("America/Waterloo");
  cl_assert(america_waterloo_region == -1);
}

void test_timezone_database__find_region_by_name_links(void) {
  // Look up America/Los_Angeles using the US/Pacific link
  const int us_pacific_region = FIND_REGION("US/Pacific");
  cl_assert(us_pacific_region != -1);

  // Look up the real America/Los_Angeles
  const int america_los_angeles_region = FIND_REGION("America/Los_Angeles");
  cl_assert(america_los_angeles_region != -1);

  // Verify that they're the same underlying region
  cl_assert_equal_i(us_pacific_region, america_los_angeles_region);

  const int america_new_york_region = FIND_REGION("America/New_York");
  cl_assert(america_new_york_region != -1);
  cl_assert(america_new_york_region != america_los_angeles_region);
}

void test_timezone_database__load_region_name(void) {
  const char america_los_angeles_region_name[] = "America/Los_Angeles";

  const int america_los_angeles_region = FIND_REGION(america_los_angeles_region_name);
  cl_assert(america_los_angeles_region != -1);

  char region_name[TIMEZONE_NAME_LENGTH];
  const bool result = timezone_database_load_region_name(america_los_angeles_region, region_name);
  cl_assert(result);
  cl_assert_equal_s(region_name, america_los_angeles_region_name);
}

void test_timezone_database__load_dst_rule_los_angeles(void) {
  const int america_los_angeles_region = FIND_REGION("America/Los_Angeles");
  cl_assert(america_los_angeles_region != -1);

  TimezoneInfo tz_info;
  bool result = timezone_database_load_region_info(america_los_angeles_region, &tz_info);
  cl_assert_equal_s("P*T", tz_info.tm_zone);
  cl_assert_equal_i(-8 * 60 * 60, tz_info.tm_gmtoff);

  TimezoneDSTRule start;
  TimezoneDSTRule end;
  result = timezone_database_load_dst_rule(tz_info.dst_id, &start, &end);

  cl_assert_equal_i(start.ds_label, 'D');
  cl_assert_equal_i(start.month, 2);
  cl_assert_equal_i(start.mday, 8);
  cl_assert_equal_i(start.hour, 2);

  cl_assert_equal_i(end.ds_label, 'S');
  cl_assert_equal_i(end.month, 10);
  cl_assert_equal_i(end.mday, 1);
  cl_assert_equal_i(end.hour, 2);
}

void test_timezone_database__kazakhstan(void) {
  {
    const int almaty_region = FIND_REGION("Asia/Almaty");

    TimezoneInfo tz_info;
    bool result = timezone_database_load_region_info(almaty_region, &tz_info);

    cl_assert(result);
    cl_assert_equal_i(tz_info.dst_id, 0); // No DST
    cl_assert_equal_i(tz_info.tm_gmtoff, 6 * 60 * 60); // +6 hours
  }
}
