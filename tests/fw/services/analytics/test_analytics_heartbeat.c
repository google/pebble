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

#include "services/common/analytics/analytics_heartbeat.h"
#include "services/common/analytics/analytics_metric.h"
#include "clar.h"
#include "fake_kernel_malloc.h"
#include "fake_rtc.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_tasks.h"
#include "stubs_rand_ptr.h"

void test_analytics_heartbeat__initialize(void) {
  analytics_metric_init();
}

void test_analytics_heartbeat__cleanup(void) {

}

static Uuid test_uuid = (Uuid){0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
                               0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};

// A minimal, basic test that heartbeats don't overwrite adjacent data when
// fields next to each other are set. We set UUID first (well, create_app does),
// and then set the fields on either side, and verify that UUID remains
// unchanged.
//   struct AppHeartbeat {
//     [...]
//     uint32 TIME_INTERVAL
//     Uuid   UUID
//     uint8  SDK_MAJOR_VERSION
//     [...]
//   }
void test_analytics_heartbeat__test_read_write_sanity(void) {
  // Set Metrics
  AnalyticsHeartbeat *heartbeat = analytics_heartbeat_app_create(&test_uuid);
  int64_t time_interval = 0x10111213;
  analytics_heartbeat_set(heartbeat, ANALYTICS_APP_METRIC_TIME_INTERVAL, time_interval);
  int64_t sdk_major_version = 0x14;
  analytics_heartbeat_set(heartbeat, ANALYTICS_APP_METRIC_SDK_MAJOR_VERSION, sdk_major_version);

  // Verify that things were set as expected, and adjacent metrics were not
  // overwritten.
  int64_t got_time_interval = analytics_heartbeat_get(heartbeat, ANALYTICS_APP_METRIC_TIME_INTERVAL);
  cl_assert(time_interval == got_time_interval);
  int64_t got_sdk_major_version = analytics_heartbeat_get(heartbeat, ANALYTICS_APP_METRIC_SDK_MAJOR_VERSION);
  cl_assert(sdk_major_version == got_sdk_major_version);
  for (int i = 0; i < sizeof(test_uuid); i++) {
    int64_t expected_uuid_byte = i;
    int64_t got_uuid_byte = analytics_heartbeat_get_array(heartbeat, ANALYTICS_APP_METRIC_UUID, i);
    cl_assert(got_uuid_byte == expected_uuid_byte);
  }

  kernel_free(heartbeat);
}

// Repeat a given bit several times.
static int64_t pattern(uint8_t i, size_t n) {
  uint64_t pat = 0;
  uint64_t mask = i;
  for (size_t j = 0; j < n; j++) {
    pat |= mask << (j*8);
  }
  pat &= 0x7f; // truncate so there are no overflows
  return *(int64_t*)&pat;
}

static void verify_metric(AnalyticsHeartbeat *heartbeat, AnalyticsMetric metric, uint8_t i, size_t j) {
  uint32_t item_size = analytics_metric_element_size(metric);
  int64_t expected = pattern(i, item_size);
  int64_t got;
  if (j == -1) {
    got = analytics_heartbeat_get(heartbeat, metric);
  } else {
    got = analytics_heartbeat_get_array(heartbeat, metric, j);
  }
  printf("Expected %"PRIx64", got %"PRIx64" (item_size=%"PRIu32")\n",
      expected, got, item_size);
  cl_assert(got == expected);
}

void test_analytics_heartbeat__clipping(void) {
  AnalyticsHeartbeat *heartbeat = analytics_heartbeat_app_create(&test_uuid);
  int64_t time_interval = 0x10111213;

  // uint8_t overflow
  analytics_heartbeat_set(heartbeat, ANALYTICS_APP_METRIC_LAUNCH_COUNT, 300);
  uint8_t val8 = analytics_heartbeat_get(heartbeat,
      ANALYTICS_APP_METRIC_LAUNCH_COUNT);
  cl_assert(val8 == (uint8_t)0xff);
  analytics_heartbeat_set(heartbeat, ANALYTICS_APP_METRIC_LAUNCH_COUNT, 80);
  val8 = analytics_heartbeat_get(heartbeat,
      ANALYTICS_APP_METRIC_LAUNCH_COUNT);
  cl_assert(val8 == 80);

  // uint16_t overflow
  analytics_heartbeat_set(heartbeat, ANALYTICS_APP_METRIC_MSG_DROP_COUNT,
      70000);
  analytics_heartbeat_set(heartbeat, ANALYTICS_APP_METRIC_MSG_DROP_COUNT,
      70001);
  uint16_t val16 = analytics_heartbeat_get(heartbeat,
      ANALYTICS_APP_METRIC_MSG_DROP_COUNT);
  cl_assert(val16 == (uint16_t)0xffff);

  // uint32_t overflow
  analytics_heartbeat_set(heartbeat, ANALYTICS_APP_METRIC_MSG_BYTE_IN_COUNT,
      ((uint64_t)0x1) << 34);
  uint32_t val32 = analytics_heartbeat_get(heartbeat,
      ANALYTICS_APP_METRIC_MSG_BYTE_IN_COUNT);
  cl_assert(val32 == (uint32_t)0xffffffff);

  kernel_free(heartbeat);
}

// Set every single app metric defined in the app heartbeat, and verify they
// are read out correctly, without overwriting any adjacent fields. Our malloc()
// mock also verifies that we don't write past the end of the heartbeat.
void test_analytics_heartbeat__test_read_write_all_app_metrics(void) {
  printf("Starting test...\n");
  AnalyticsHeartbeat *heartbeat = analytics_heartbeat_app_create(&test_uuid);

  uint8_t i = 0x80;
  for (AnalyticsMetric metric = ANALYTICS_APP_METRIC_START + 1;
       metric < ANALYTICS_APP_METRIC_END; metric++) {
    if (analytics_metric_is_array(metric)) {
      for (int j = 0; j < analytics_metric_num_elements(metric); j++) {
        analytics_heartbeat_set_array(heartbeat, metric, j, pattern(i, 8));
        i++;
      }
    } else {
      analytics_heartbeat_set(heartbeat, metric, pattern(i, 8));
      i++;
    }
  }

  analytics_heartbeat_print(heartbeat);

  i = 0x80;
  for (AnalyticsMetric metric = ANALYTICS_APP_METRIC_START + 1;
       metric < ANALYTICS_APP_METRIC_END; metric++) {
    if (analytics_metric_is_array(metric)) {
      for (int j = 0; j < analytics_metric_num_elements(metric); j++) {
        verify_metric(heartbeat, metric, i, j);
        i++;
      }
    } else {
      verify_metric(heartbeat, metric, i, -1);
      i++;
    }
  }
  kernel_free(heartbeat);
}
