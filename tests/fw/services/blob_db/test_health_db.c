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

#include "services/normal/filesystem/pfs.h"
#include "services/normal/blob_db/health_db.h"
#include "util/size.h"

#include <limits.h>

#include "fake_settings_file.h"

// Stubs
////////////////////////////////////////////////////////////////
#include "stubs_app_state.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_tasks.h"
#include "stubs_pbl_malloc.h"
#include "stubs_prompt.h"
#include "stubs_worker_state.h"

status_t pfs_remove(const char *name) {
  fake_settings_file_reset();
  return S_SUCCESS;
}

status_t blob_db_insert(BlobDBId db_id,
    const uint8_t *key, int key_len, const uint8_t *val, int val_len) {
  return settings_file_set(NULL, key, key_len, val, val_len);
}

RtcTicks rtc_get_ticks(void) {
  return 0;
}

// Fakes
////////////////////////////////////////////////////////////////
static const time_t NOW = 1471269600; // Mon, 15 Aug 2016 14:00:00 GMT
time_t rtc_get_time(void) {
  return NOW;
}

bool activity_get_metric(ActivityMetric metric, uint32_t history_len, int32_t *history) {
  *history = 0;
  return true;
}


int s_metric_updated_count = 0;
void activity_metrics_prv_set_metric(ActivityMetric metric, DayInWeek day, int32_t value) {
  printf("s_metric_updated_count: %d\n", s_metric_updated_count);
  s_metric_updated_count++;
}

// Setup
////////////////////////////////////////////////////////////////

void test_health_db__initialize(void) {
  fake_settings_file_reset();
  health_db_init();
  s_metric_updated_count = 0;
}


void test_health_db__cleanup(void) {
}

// Dummy Data
////////////////////////////////////////////////////////////////

#define NUM_CURRENT_MOVEMENT_METRICS 5
#define NUM_CURRENT_SLEEP_METRICS 4
#define NUM_CURRENT_HR_ZONE_METRICS 3

typedef enum MovementDataFields {
  MD_Version,
  MD_Timestamp,
  MD_Steps,
  MD_ActiveKCalories,
  MD_RestingKCalories,
  MD_Distance,
  MD_ActiveTime
} MovementDataFields;

static uint32_t s_movement_data[] = {
  1,            // Version
  NOW,          // Timestamp
  1234,         // Steps
  1111,         // Active K Calories
  2222,         // Resting K Calories
  3333,         // Distance
  4444,         // Active Time
};

static uint32_t s_old_movement_data[] = {
  1,            // Version
  NOW - (7 * SECONDS_PER_DAY),
  1234,         // Steps
  1111,         // Active K Calories
  2222,         // Resting K Calories
  3333,         // Distance
  4444,         // Active Time
};

static uint32_t s_future_movement_data[] = {
  1,            // Version
  NOW + SECONDS_PER_DAY,
  1234,         // Steps
  1111,         // Active K Calories
  2222,         // Resting K Calories
  3333,         // Distance
  4444,         // Active Time
};

typedef enum SleepDataFields {
  SD_Version,
  SD_Timestamp,
  SD_SleepDuration,
  SD_DeepSleepDuration,
  SD_FallAsleepTime,
  SD_WakeupTime,
  SD_TypicalSleepDuration,
  SD_TypicalDeepSleepDuration,
  SD_TypicalFallAsleepTime,
  SD_TypicalWakeupTime,
} SleepDataFields;

static uint32_t s_sleep_data[] = {
  1,            // Version
  NOW,          // Timestamp
  1234,         // Sleep Duration
  1111,         // Deep Sleep Duration
  2222,         // Fall Asleep Time
  3333,         // Wakeup Time
  4444,         // Active Time

  5555,         // Typical sleep duration
  6666,         // Typical deep sleep duration
  7777,         // Typical fall asleep time
  8888,         // Typical wakeup time
};

static uint32_t s_old_sleep_data[] = {
  1,            // Version
  NOW - (7 * SECONDS_PER_DAY),
  1234,         // Sleep Duration
  1111,         // Deep Sleep Duration
  2222,         // Fall Asleep Time
  3333,         // Wakeup Time
  4444,         // Active Time

  5555,         // Typical sleep duration
  6666,         // Typical deep sleep duration
  7777,         // Typical fall asleep time
  8888,         // Typical wakeup time
};

static uint32_t s_invalid_sleep_data[] = {
  5,            // Version
  NOW,          // Timestamp
  1234,         // Sleep Duration
  1111,         // Deep Sleep Duration
  2222,         // Fall Asleep Time
  3333,         // Wakeup Time
  4444,         // Active Time

  5555,         // Typical sleep duration
  6666,         // Typical deep sleep duration
  7777,         // Typical fall asleep time
  8888,         // Typical wakeup time
};

static uint32_t s_hr_zone_data[] = {
  1,            // Version
  NOW,          // Timestamp
  3,            // Number of zones
  60,           // Minutes in zone 1
  30,           // Minutes in zone 2
  15,           // Minutes in zone 3
};

// Tests
////////////////////////////////////////////////////////////////

void test_health_db__blob_db_api(void) {
  const char *key = "monday_sleepData";

  // insert one
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);

  // check
  int32_t val_out;
  cl_assert(health_db_get_typical_value(ActivityMetricSleepTotalSeconds, Monday, &val_out));
  cl_assert_equal_i(val_out, s_sleep_data[SD_TypicalSleepDuration]);

  // delete
  cl_assert_equal_i(health_db_delete((uint8_t *)key, strlen(key)),
                    S_SUCCESS);

  // check
  cl_assert(!health_db_get_typical_value(ActivityMetricSleepTotalSeconds, Monday, &val_out));

  // insert again
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);

  // check
  cl_assert(health_db_get_typical_value(ActivityMetricSleepTotalSeconds, Monday, &val_out));
  cl_assert_equal_i(val_out, s_sleep_data[SD_TypicalSleepDuration]);

  // flush
  cl_assert_equal_i(health_db_flush(), S_SUCCESS);

  // check
  cl_assert(!health_db_get_typical_value(ActivityMetricSleepTotalSeconds, Monday, &val_out));

  // insert something with an older version (this will succeed)
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_invalid_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);
  // check
  cl_assert(!health_db_get_typical_value(ActivityMetricSleepTotalSeconds, Monday, &val_out));
}

void test_health_db__movement_data(void) {
  const char *key = "monday_movementData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_movement_data, sizeof(s_movement_data)),
                    S_SUCCESS);

  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_MOVEMENT_METRICS);

  // check typical (not stored)
  int32_t val_out;

  cl_assert(!health_db_get_typical_value(ActivityMetricStepCount, Monday, &val_out));

  cl_assert(!health_db_get_typical_value(ActivityMetricActiveSeconds, Monday, &val_out));

  cl_assert(!health_db_get_typical_value(ActivityMetricRestingKCalories, Monday, &val_out));

  cl_assert(!health_db_get_typical_value(ActivityMetricActiveKCalories, Monday, &val_out));

  cl_assert(!health_db_get_typical_value(ActivityMetricDistanceMeters, Monday, &val_out));
}

void test_health_db__sleep_data(void) {
  const char *key = "monday_sleepData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);

  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_SLEEP_METRICS);

  // check typical
  int32_t val_out;

  cl_assert(health_db_get_typical_value(ActivityMetricSleepTotalSeconds, Monday, &val_out));
  cl_assert_equal_i(val_out, s_sleep_data[SD_TypicalSleepDuration]);

  cl_assert(health_db_get_typical_value(ActivityMetricSleepRestfulSeconds, Monday, &val_out));
  cl_assert_equal_i(val_out, s_sleep_data[SD_TypicalDeepSleepDuration]);

  cl_assert(health_db_get_typical_value(ActivityMetricSleepEnterAtSeconds, Monday, &val_out));
  cl_assert_equal_i(val_out, s_sleep_data[SD_TypicalFallAsleepTime]);

  cl_assert(health_db_get_typical_value(ActivityMetricSleepExitAtSeconds, Monday, &val_out));
  cl_assert_equal_i(val_out, s_sleep_data[SD_TypicalWakeupTime]);
}

void test_health_db__hr_zone_data(void) {
  const char *key = "monday_heartRateZoneData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_hr_zone_data, sizeof(s_hr_zone_data)),
                    S_SUCCESS);

  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_HR_ZONE_METRICS);

  // check typical (not stored)
  int32_t val_out;

  cl_assert(!health_db_get_typical_value(ActivityMetricHeartRateZone1Minutes, Monday, &val_out));
  cl_assert(!health_db_get_typical_value(ActivityMetricHeartRateZone2Minutes, Monday, &val_out));
  cl_assert(!health_db_get_typical_value(ActivityMetricHeartRateZone3Minutes, Monday, &val_out));
}

void test_health_db__step_averages(void) {
  const struct {
    const char *key;
    const char *val;
  } entries[] = {
    {
      .key = "sunday_steps",
      .val = "l4tHpFsFGE6UINneFPMnf2lgINlYuXlDS6xh6vizK9jbDen5mHQgWF6E8jOzBVnEdV0j2DNOzONfJbsWoSWH0QoQpPmm1NSW" \
             "l4tHpFsFGE6UINneFPMnf2lgINlYuXlDS6xh6vizK9jbDen5mHQgWF6E8jOzBVnEdV0j2DNOzONfJbsWoSWH0QoQpPmm1NSW",
    }, {
      .key = "monday_steps",
      .val = "Rhgc3Q7ajjydH8CA9qxVJH0FpVDjdGwwoKCLE2F55x62EZZ6MCIjUMynVq13U8vOHhaWoygDf0zwOIdAEUOrZRwvJmYVzW7J" \
             "Rhgc3Q7ajjydH8CA9qxVJH0FpVDjdGwwoKCLE2F55x62EZZ6MCIjUMynVq13U8vOHhaWoygDf0zwOIdAEUOrZRwvJmYVzW7J",
    }, {
      .key = "tuesday_steps",
      .val = "V6PrBVc4suqCYjLceUl6a1UXYO8qwL5w3WZY00KeGoHAcuST7OxGnMBVCEskty0q4OIdTeyyZOljrGif09kZOFldu3BjJqJO" \
             "V6PrBVc4suqCYjLceUl6a1UXYO8qwL5w3WZY00KeGoHAcuST7OxGnMBVCEskty0q4OIdTeyyZOljrGif09kZOFldu3BjJqJO",
    }, {
      .key = "wednesday_steps",
      .val = "wufD6hzhFUrkZkLObfn2dFKUDs0kNNWp6CFiS2XBS3spSFDQUnFLuxWPEq7Dql2HjdkVobMcOA8DiOcanhZvziN6hbteMbg8" \
             "wufD6hzhFUrkZkLObfn2dFKUDs0kNNWp6CFiS2XBS3spSFDQUnFLuxWPEq7Dql2HjdkVobMcOA8DiOcanhZvziN6hbteMbg8",
    }, {
      .key = "thursday_steps",
      .val = "FXKAfWwOueL4jLJfZRxzINDITxaThvFIpOrzYfgPVmqbbYoCZKkKkbgyvP1UaCEstr9WjptLszgMocgGSEsqmoipqqWdk7dq" \
             "FXKAfWwOueL4jLJfZRxzINDITxaThvFIpOrzYfgPVmqbbYoCZKkKkbgyvP1UaCEstr9WjptLszgMocgGSEsqmoipqqWdk7dq",
    }, {
      .key = "friday_steps",
      .val = "uxFhoWTzJxDOmyBX2g3n7wdoPKxeleBR7iwKGn7utn8qTEj0tB7aw65EEFZ5QldgAkg6lctSmamf2p95l2CpHXNgVL22hQFx" \
             "uxFhoWTzJxDOmyBX2g3n7wdoPKxeleBR7iwKGn7utn8qTEj0tB7aw65EEFZ5QldgAkg6lctSmamf2p95l2CpHXNgVL22hQFx",
    }, {
      .key = "saturday_steps",
      .val = "SSxw7WtwGnhobAOXwqbvGDDwElpRG6cll8CwM9Wysh01Mj0aFWxEVN0z5w7yQHt8bwiWVabrMeUUAek2J5zCoXiGIkav4cW8" \
             "SSxw7WtwGnhobAOXwqbvGDDwElpRG6cll8CwM9Wysh01Mj0aFWxEVN0z5w7yQHt8bwiWVabrMeUUAek2J5zCoXiGIkav4cW8",
    },
  };


  for (int i = 0; i < ARRAY_LENGTH(entries); ++i) {
    cl_assert_equal_i(health_db_insert((uint8_t *)entries[i].key,
                                       strlen(entries[i].key),
                                       (uint8_t *)entries[i].val,
                                       strlen(entries[i].val)),
                      S_SUCCESS);

    ActivityMetricAverages averages;
    health_db_get_typical_step_averages(i, &averages);

    int idx = i * 10;
    uint16_t val_expected = ((uint16_t *)entries[i].val)[idx];
    cl_assert_equal_i(averages.average[idx], val_expected);
  }
}

void test_health_db__monthly_averages(void) {
  int32_t val_out;

  const char *average_step_key = "average_dailySteps";
  int32_t average_steps_val = 123456;
  cl_assert_equal_i(health_db_insert((uint8_t *)average_step_key, strlen(average_step_key),
                                     (uint8_t *)&average_steps_val, sizeof(average_steps_val)),
                    S_SUCCESS);

  cl_assert(health_db_get_monthly_average_value(ActivityMetricStepCount, &val_out));
  cl_assert_equal_i(val_out, average_steps_val);

  const char *average_sleep_key = "average_sleepDuration";
  int32_t average_sleep_val = 654321;
  cl_assert_equal_i(health_db_insert((uint8_t *)average_sleep_key, strlen(average_sleep_key),
                                     (uint8_t *)&average_sleep_val, sizeof(average_sleep_val)),
                    S_SUCCESS);

  cl_assert(health_db_get_monthly_average_value(ActivityMetricSleepTotalSeconds, &val_out));
  cl_assert_equal_i(val_out, average_sleep_val);
}

void test_health_db__notify_listeners(void) {
  char *key;

  s_metric_updated_count = 0;
  key = "tuesday_sleepData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_SLEEP_METRICS);

  s_metric_updated_count = 0;
  key = "wednesday_movementData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_movement_data, sizeof(s_movement_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_MOVEMENT_METRICS);

  s_metric_updated_count = 0;
  key = "thursday_sleepData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_SLEEP_METRICS);

  s_metric_updated_count = 0;
  key = "friday_movementData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_movement_data, sizeof(s_movement_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_MOVEMENT_METRICS);

  s_metric_updated_count = 0;
  key = "saturday_sleepData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_SLEEP_METRICS);

  s_metric_updated_count = 0;
  key = "sunday_movementData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_movement_data, sizeof(s_movement_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_MOVEMENT_METRICS);


  s_metric_updated_count = 0;
  key = "monday_movementData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_movement_data, sizeof(s_movement_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_MOVEMENT_METRICS);

  s_metric_updated_count = 0;
  key = "monday_sleepData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_sleep_data, sizeof(s_sleep_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, NUM_CURRENT_SLEEP_METRICS);


  // Test inserting something which is more than a week old.
  // We shouldn't update our internal storage
  s_metric_updated_count = 0;
  key = "monday_movementData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_old_movement_data, sizeof(s_old_movement_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, 0);

  s_metric_updated_count = 0;
  key = "monday_sleepData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_old_sleep_data, sizeof(s_old_sleep_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, 0);

  // Insert something which has a future timestamp
  s_metric_updated_count = 0;
  key = "monday_movementData";
  cl_assert_equal_i(health_db_insert((uint8_t *)key, strlen(key),
                                     (uint8_t *)s_future_movement_data, sizeof(s_movement_data)),
                    S_SUCCESS);
  cl_assert_equal_i(s_metric_updated_count, 0);

}
