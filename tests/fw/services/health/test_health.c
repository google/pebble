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

#include "applib/health_service_private.h"
#include "services/normal/activity/activity.h"
#include "shell/prefs_syscalls.h"
#include "util/size.h"


// Stubs
#include "stubs_app_manager.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_worker_manager.h"

// Fakes
#include "fake_rtc.h"
#include "fake_pbl_std.h"

static HealthServiceState s_health_service;

// -----------------------------------
// T_STATIC functions from health_service.c
bool prv_calculate_time_range(time_t time_start, time_t time_end,
                              HealthServiceTimeRange *range);

void prv_adjust_value_boundaries(HealthValue *values, size_t num_values,
                                 const HealthServiceTimeRange *range);

bool prv_activity_session_matches(const ActivitySession *session, HealthActivityMask mask,
                                  time_t time_start, time_t time_end);

int64_t prv_session_compare(const ActivitySession *a, const ActivitySession *b,
                            HealthIterationDirection direction);

void prv_health_event_handler(PebbleEvent *e, void *context);


// -----------------------------------
// Stubs
AppInstallId app_get_app_id(void) {
  return 1;
}

HealthServiceState *app_state_get_health_service_state(void) {
  return &s_health_service;
}

PebbleTask pebble_task_get_current(void) {
  return PebbleTask_App;
}

HealthServiceState *worker_state_get_health_service_state(void) {
  cl_fail("should never be called");
  return NULL;
}

void sys_send_pebble_event_to_kernel(PebbleEvent* event) {}

HRMSessionRef sys_hrm_manager_get_app_subscription(AppInstallId app_id) {
  return HRM_INVALID_SESSION_REF;
}

static bool s_activity_prefs_heart_rate_enabled;
bool sys_activity_prefs_heart_rate_is_enabled(void) {
  return s_activity_prefs_heart_rate_enabled;
}

bool sys_hrm_manager_get_subscription_info(HRMSessionRef session, AppInstallId *app_id,
                                           uint32_t *update_interval_s, uint16_t *expire_s,
                                           HRMFeature *features) {
  return false;
}


typedef struct {
  struct {
    ActivityMetric metric;
    uint32_t history_len;
  } in;
  struct {
    HealthValue history[ACTIVITY_HISTORY_DAYS];
    bool result;
  } out;
} sys_activity_get_metric_values;

// Activity Metric Overrides
// Allows one to specify a return value for a specific metric
static struct {
  bool overridden;
  int32_t value;
} s_metric_overrides[ActivityMetricNumMetrics];

static bool prv_handle_override(ActivityMetric metric, uint32_t history_len, int32_t *history) {
  if (!s_metric_overrides[metric].overridden) {
    return false;
  }
  cl_assert_equal_i(1, history_len);
  *history = s_metric_overrides[metric].value;
  return true;
}

static void prv_override_metric(ActivityMetric metric, int32_t value) {
  s_metric_overrides[metric].value = value;
  s_metric_overrides[metric].overridden = true;
}
// End override code


static sys_activity_get_metric_values s_sys_activity_get_metric_values;

// mock that simply copies values from static vars and stores args for later inspection
bool sys_activity_get_metric(ActivityMetric metric, uint32_t history_len, int32_t *history) {
  cl_assert(history_len <= ARRAY_LENGTH(s_sys_activity_get_metric_values.out.history));

  s_sys_activity_get_metric_values.in.metric = metric;
  s_sys_activity_get_metric_values.in.history_len = history_len;

  // Check if this value is in our overrides. If it is return it and quit.
  if (prv_handle_override(metric, history_len, history)) {
    return true;
  }

  // yes, actual implementation can handle this
  if (history) {
    for (uint32_t i = 0; i < history_len; i++) {
      history[i] = s_sys_activity_get_metric_values.out.history[i];
    }
  }

  return s_sys_activity_get_metric_values.out.result;
}

void event_service_client_subscribe(EventServiceInfo * service_info) {}
void event_service_client_unsubscribe(EventServiceInfo * service_info) {}

static UnitsDistance s_units_distance_result;

UnitsDistance sys_shell_prefs_get_units_distance(void) {
  return s_units_distance_result;
}

typedef struct {
  struct {
    ActivitySession sessions[30];
    uint32_t num_sessions;
    bool result;
  } out;
} sys_activity_get_sessions_values;

static sys_activity_get_sessions_values s_sys_activity_get_sessions_values;

bool sys_activity_get_sessions(uint32_t *num_sessions, ActivitySession *sessions) {
  cl_assert(num_sessions);
  cl_assert(sessions);
  cl_assert(s_sys_activity_get_sessions_values.out.num_sessions <=
            ARRAY_LENGTH(s_sys_activity_get_sessions_values.out.sessions));

  for (uint32_t i = 0;
       i < MIN(*num_sessions,
               s_sys_activity_get_sessions_values.out.num_sessions);
       i++) {
    sessions[i] = s_sys_activity_get_sessions_values.out.sessions[i];
  }

  *num_sessions = s_sys_activity_get_sessions_values.out.num_sessions;
  return s_sys_activity_get_sessions_values.out.result;
}


typedef struct {
  struct {
    uint16_t day_of_week;
  } in;
  struct {
    ActivityMetricAverages averages;
    bool result;
  } out;
} sys_activity_get_step_averages_values;

static sys_activity_get_step_averages_values s_sys_activity_get_step_averages_values_weekday;
static sys_activity_get_step_averages_values s_sys_activity_get_step_averages_values_weekend;

bool sys_activity_get_step_averages(uint16_t day_of_week, ActivityMetricAverages *averages) {
  cl_assert(averages);
  if (day_of_week == Sunday || day_of_week == Saturday) {
    s_sys_activity_get_step_averages_values_weekend.in.day_of_week = day_of_week;
    memcpy(averages, &s_sys_activity_get_step_averages_values_weekend.out.averages,
           sizeof(*averages));
    return s_sys_activity_get_step_averages_values_weekend.out.result;
  } else {
    s_sys_activity_get_step_averages_values_weekday.in.day_of_week = day_of_week;
    memcpy(averages, &s_sys_activity_get_step_averages_values_weekday.out.averages,
           sizeof(*averages));
    return s_sys_activity_get_step_averages_values_weekday.out.result;
  }
}

typedef struct {
  HealthActivity activity;
  time_t time_start;
  time_t time_end;
  void *context;
} HealthActivityCBData;

static HealthActivityCBData s_prv_activity_cb__args[100];
static uint32_t s_prv_activity_cb__call_count;
static uint32_t s_prv_activity_cb__false_at_call_no = UINT32_MAX;

bool prv_activity_cb(HealthActivity activity,
                     time_t time_start, time_t time_end,
                     void *context) {
  s_prv_activity_cb__args[s_prv_activity_cb__call_count] = (HealthActivityCBData) {
    .activity = activity, .time_start = time_start, .time_end = time_end, .context = context,
  };
  s_prv_activity_cb__call_count++;

  cl_assert(s_prv_activity_cb__call_count <= s_prv_activity_cb__false_at_call_no);
  return s_prv_activity_cb__call_count < s_prv_activity_cb__false_at_call_no;
}

typedef struct {
  HealthMinuteData records[MINUTES_PER_DAY];
  uint32_t num_records;
  time_t utc_start;
  bool result;
  bool asserts;
} sys_activity_get_minute_history_out_values;

typedef struct {
  uint32_t num_records;
  time_t utc_start;
} sys_activity_get_minute_history_in_values;

typedef struct {
  // Each time sys_activity_get_minute_history is called, we return the next stage of data
  int stage;
  sys_activity_get_minute_history_in_values in[4];
  sys_activity_get_minute_history_out_values out[4];
} sys_activity_get_minute_history_values;

static sys_activity_get_minute_history_values s_sys_activity_get_minute_history_values;


bool sys_activity_get_minute_history(HealthMinuteData *minute_data, uint32_t *num_records,
                                     time_t *utc_start) {
  int stage = s_sys_activity_get_minute_history_values.stage++;
  cl_assert(stage < ARRAY_LENGTH(s_sys_activity_get_minute_history_values.out));
  sys_activity_get_minute_history_out_values *out =
      &s_sys_activity_get_minute_history_values.out[stage];

  cl_assert_equal_b(out->asserts, false);
  cl_assert(minute_data);
  cl_assert(num_records);
  cl_assert(utc_start);

  s_sys_activity_get_minute_history_values.in[stage].num_records = *num_records;
  s_sys_activity_get_minute_history_values.in[stage].utc_start = *utc_start;

  if (!out->result) {
    return false;
  }

  *num_records = MIN(out->num_records, *num_records);
  *utc_start = out->utc_start;
  for (uint32_t i = 0; i < *num_records; i++) {
    minute_data[i] = out->records[i];
  }

  return true;
}

static bool s_activity_sessions_ongoing[ActivitySessionTypeCount];

bool sys_activity_sessions_is_session_type_ongoing(ActivitySessionType type) {
  return s_activity_sessions_ongoing[type];
}


/////////////////////////////////////////

void test_health__initialize(void) {
  TimezoneInfo tz_info = {
    .tm_zone = "UTC",
    .tm_gmtoff = 0,
  };
  time_util_update_timezone(&tz_info);

  s_health_service = (HealthServiceState){};
  time_t utc_sec = 1451293942; // some constant time for this test
    // Mon, 28 Dec 2015 09:12:22 GMT
    // => 22+12*60+9*60*60 = 33142 seconds into this day
    // =>   24*60*60-33142 = 53258 seconds remaining this day
  fake_rtc_init(100 /*initial_ticks*/, utc_sec);

  s_sys_activity_get_metric_values = (sys_activity_get_metric_values) {
    .out.result = true,
    .in.metric = (ActivityMetric)-1,
  };

  memset(s_prv_activity_cb__args, 0, sizeof(s_prv_activity_cb__args));
  memset(s_metric_overrides, 0, sizeof(s_metric_overrides));
  s_prv_activity_cb__call_count = 0;
  s_prv_activity_cb__false_at_call_no = UINT32_MAX;

  s_sys_activity_get_minute_history_values = (sys_activity_get_minute_history_values) {
    // as all these values need to be configured in the test, we assert per default
    .out[0].asserts = true,
  };

  s_activity_prefs_heart_rate_enabled = true;
}

void test_health__sum_today_returns_0_on_failure(void) {
  s_sys_activity_get_metric_values.out.result = false;
  s_sys_activity_get_metric_values.out.history[0] = 456;
  HealthValue result = health_service_sum_today(HealthMetricStepCount);
  cl_assert_equal_i(0, result);
}

void test_health__sum_today(void) {
  s_sys_activity_get_metric_values.out.history[0] = 123;
  s_sys_activity_get_metric_values.out.history[1] = 456;
  HealthValue result = health_service_sum_today(HealthMetricStepCount);
  cl_assert_equal_i(123, result);
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.metric, ActivityMetricStepCount);
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.history_len, ACTIVITY_HISTORY_DAYS);
}

#define cl_assert_equal_range(a, b) \
  do { \
    HealthServiceTimeRange r_a = (a); \
    HealthServiceTimeRange r_b = (b); \
    bool success = memcmp(&r_a, &r_b, sizeof(r_a)) == 0; \
    if (!success) { \
      char error_msg[256] = {0}; \
      snprintf(error_msg, sizeof(error_msg), \
          "HealthServiceInternalTimeRange equal\n" \
          "    a: {last_day_idx:%d, num_days:%d, seconds_first_day:%d, seconds_last_day:%d, " \
                  "seconds_total_last_day: %d}\n" \
          "    b: {last_day_idx:%d, num_days:%d, seconds_first_day:%d, seconds_last_day:%d," \
                  "seconds_total_last_day: %d}\n", \
              (int)r_a.last_day_idx, (int)r_a.num_days, \
              (int)r_a.seconds_first_day, (int)r_a.seconds_last_day, \
              (int)r_a.seconds_total_last_day, \
              (int)r_b.last_day_idx, (int)r_b.num_days, \
              (int)r_b.seconds_first_day, (int)r_b.seconds_last_day, \
              (int)r_b.seconds_total_last_day); \
      clar__assert(0, __FILE__, __LINE__, "Expression is not true: ", error_msg, 1); \
    } \
  } while(0);


void test_health__range_to_day_id(void) {
  const time_t now = rtc_get_time();

  bool result;
  HealthServiceTimeRange range;

  // today
  result = prv_calculate_time_range(time_util_get_midnight_of(now), now, &range);
  cl_assert(result);
  cl_assert_equal_range(range, ((HealthServiceTimeRange){
    .last_day_idx = 0,
    .num_days = 1,
    .seconds_first_day = 33142,
    .seconds_last_day = 33142,
    .seconds_total_last_day = 33142,
  }));

  // yesterday
  result = prv_calculate_time_range(
    time_util_get_midnight_of(now - SECONDS_PER_DAY), time_util_get_midnight_of(now), &range);
  cl_assert(result);
  cl_assert_equal_range(range, ((HealthServiceTimeRange){
    .last_day_idx = 1,
    .num_days = 1,
    .seconds_first_day = 86400,
    .seconds_last_day = 86400,
    .seconds_total_last_day = 86400,
  }));

  // some time yesterday + today
  result = prv_calculate_time_range(now - SECONDS_PER_DAY, now, &range);
  cl_assert(result);
  cl_assert_equal_range(range, ((HealthServiceTimeRange){
    .last_day_idx = 0,
    .num_days = 2,
    .seconds_first_day = 53258,
    .seconds_last_day = 33142,
    .seconds_total_last_day = 33142,
  }));
}

void test_health__range_to_day_id_respects_local_time(void) {
  const time_t now = rtc_get_time();

  bool result;
  HealthServiceTimeRange range;

  // some time yesterday + today - as if UTC == localtime
  result = prv_calculate_time_range(now - SECONDS_PER_DAY, now, &range);
  cl_assert(result);
  cl_assert_equal_range(range, ((HealthServiceTimeRange){
    .last_day_idx = 0,
    .num_days = 2,
    .seconds_first_day = 53258,
    .seconds_last_day = 33142,
    .seconds_total_last_day = 33142,
  }));

  // shifted one hour
  time_t utc_to_local_delta = SECONDS_PER_HOUR;
  TimezoneInfo tz_info = {
    .tm_zone = "FOO",
    .tm_gmtoff = utc_to_local_delta,
  };
  time_util_update_timezone(&tz_info);

  result = prv_calculate_time_range(now - SECONDS_PER_DAY, now, &range);
  cl_assert(result);
  cl_assert_equal_range(range, ((HealthServiceTimeRange){
    .last_day_idx = 0,
    .num_days = 2,
    .seconds_first_day = 53258 - utc_to_local_delta,
    .seconds_last_day = 33142 + utc_to_local_delta,
    .seconds_total_last_day = 33142 + utc_to_local_delta,
  }));
}

void test_health__range_to_day_id_rejects_invalid_values(void) {
  const time_t now = rtc_get_time();
  bool result;

  // check that we *can* return success
  result = prv_calculate_time_range(now - 10, now, NULL);
  cl_assert_equal_b(result, true);

  // in the future
  result = prv_calculate_time_range(now + 10, now + 20, NULL);
  cl_assert_equal_b(result, false);

  // too far in the past
  result = prv_calculate_time_range(
    now - (ACTIVITY_HISTORY_DAYS + 10) * SECONDS_PER_DAY,
    now - (ACTIVITY_HISTORY_DAYS + 2) * SECONDS_PER_DAY, NULL);
  cl_assert_equal_b(result, false);


  // start after end
  result = prv_calculate_time_range(now - 100, now - 200, NULL);
  cl_assert_equal_b(result, false);
}

void test_health__range_to_day_id_clamps_values(void) {
  const time_t now = rtc_get_time();
  bool result;
  HealthServiceTimeRange range;

  // clamps value that goes into the future
  result = prv_calculate_time_range(now - 10, now + 11, &range);
  cl_assert_equal_b(result, true);
  cl_assert_equal_range(range, ((HealthServiceTimeRange){
    .last_day_idx = 0,
    .num_days = 1,
    .seconds_first_day = 10,
    .seconds_last_day = 10,
    .seconds_total_last_day = 33142,
  }));

  // clamps value that goes into the future
  const time_t first_valid_time =
    time_util_get_midnight_of(now - (ACTIVITY_HISTORY_DAYS - 1) * SECONDS_PER_DAY);
  result = prv_calculate_time_range(first_valid_time - 12, first_valid_time + 13,
                                    &range);
  cl_assert_equal_b(result, true);
  cl_assert_equal_range(range, ((HealthServiceTimeRange){
    .last_day_idx = ACTIVITY_HISTORY_DAYS - 1,
    .num_days = 1,
    .seconds_first_day = 13,
    .seconds_last_day = 13,
    .seconds_total_last_day = 86400,
  }));
}

void test_health__sum_full_days(void) {
  // use values structured as binary mask so we can detect if we sum up correct days
  s_sys_activity_get_metric_values.out.history[0] = 1000;
  s_sys_activity_get_metric_values.out.history[1] = 2000;
  s_sys_activity_get_metric_values.out.history[2] = 4000;
  s_sys_activity_get_metric_values.out.history[3] = 8000;
  s_sys_activity_get_metric_values.out.history[4] = 16000;

  const time_t now = rtc_get_time();
  HealthValue result;
  // today until now
  result = health_service_sum(HealthMetricStepCount,
                              time_util_get_midnight_of(now),
                              now);
  cl_assert_equal_i(result, 1000);
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.history_len, ACTIVITY_HISTORY_DAYS);

  // today into future
  result = health_service_sum(HealthMetricStepCount,
                              time_util_get_midnight_of(now),
                              now + 12345);
  cl_assert_equal_i(result, 1000);

  // yesterday
  result = health_service_sum(HealthMetricStepCount,
                              time_util_get_midnight_of(now) - SECONDS_PER_DAY,
                              time_util_get_midnight_of(now));
  cl_assert_equal_i(result, 2000);

  // yesterday and today
  result = health_service_sum(HealthMetricStepCount,
                              time_util_get_midnight_of(now) - SECONDS_PER_DAY,
                              now);
  cl_assert_equal_i(result, 1000 + 2000);
}

void test_health__process_range(void) {
  HealthValue values[4] = {1000, 1000, 1000, 1000};
  HealthServiceTimeRange range = {
    .num_days = 3,
    .seconds_first_day = SECONDS_PER_DAY / 10,
    .seconds_last_day = SECONDS_PER_DAY / 5,
    .seconds_total_last_day = SECONDS_PER_DAY,
  };

  // make sure we treat first and last day correctly (last == idx 0)
  prv_adjust_value_boundaries(values, ARRAY_LENGTH(values), &range);
  cl_assert_equal_i(values[0], 1000 / 5);
  cl_assert_equal_i(values[1], 1000);
  cl_assert_equal_i(values[2], 1000 / 10);

  // ensure we look at seconds_total_last_day
  values[0] = 1000;
  values[2] = 1000;
  range.seconds_total_last_day = SECONDS_PER_DAY / 4;
  prv_adjust_value_boundaries(values, ARRAY_LENGTH(values), &range);
  cl_assert_equal_i(values[0], 4 * 1000 / 5);
  cl_assert_equal_i(values[1], 1000);
  cl_assert_equal_i(values[2], 1000 / 10);

  // ensure we don't calculate a single day multiple times
  values[0] = 1000;
  values[2] = 1000;
  range.num_days = 1;
  prv_adjust_value_boundaries(values, ARRAY_LENGTH(values), &range);
  cl_assert_equal_i(values[0], 4 * 1000 / 5);
  cl_assert_equal_i(values[1], 1000);
  cl_assert_equal_i(values[2], 1000);

  // ensure we can handle smaller array than range - nothing will be processed
  values[0] = 1000;
  range.num_days = 2;
  prv_adjust_value_boundaries(values, 1, &range);
  cl_assert_equal_i(values[0], 1000);
  cl_assert_equal_i(values[1], 1000);
  cl_assert_equal_i(values[2], 1000);

  // ensure we can handle empty sets
  values[0] = 1000;
  prv_adjust_value_boundaries(values, 0, &range);
  cl_assert_equal_i(values[0], 1000);
  cl_assert_equal_i(values[1], 1000);
  cl_assert_equal_i(values[2], 1000);

  // ensure we can handle empty ranges
  range.num_days = 0;
  prv_adjust_value_boundaries(values, ARRAY_LENGTH(values), &range);
  cl_assert_equal_i(values[0], 1000);
  cl_assert_equal_i(values[1], 1000);
  cl_assert_equal_i(values[2], 1000);

  // ensure we correctly handle the day index
  range = (HealthServiceTimeRange){
    .num_days = 3,
    .last_day_idx = 1,
    .seconds_first_day = SECONDS_PER_DAY / 10,
    .seconds_last_day = SECONDS_PER_DAY / 5,
    .seconds_total_last_day = SECONDS_PER_DAY,
  };
  prv_adjust_value_boundaries(values, ARRAY_LENGTH(values), &range);
  cl_assert_equal_i(values[0], 1000);
  cl_assert_equal_i(values[1], 1000 / 5);
  cl_assert_equal_i(values[2], 1000);
  cl_assert_equal_i(values[3], 1000 / 10);
}

void test_health__sum_fraction_days(void) {
  // use values structured as binary mask so we can detect if we sum up correct days
  s_sys_activity_get_metric_values.out.history[0] = 1000;
  s_sys_activity_get_metric_values.out.history[1] = 2000;
  s_sys_activity_get_metric_values.out.history[2] = 4000;
  s_sys_activity_get_metric_values.out.history[3] = 8000;
  s_sys_activity_get_metric_values.out.history[4] = 16000;

  const time_t now = rtc_get_time();
  HealthValue result;
  // 3/4 of yesterday
  result = health_service_sum(HealthMetricStepCount,
                              time_util_get_midnight_of(now) - SECONDS_PER_DAY,
                              time_util_get_midnight_of(now) - SECONDS_PER_DAY / 4);
  cl_assert_equal_i(result, 1500);
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.history_len, ACTIVITY_HISTORY_DAYS);

  // 1/2 of today's captured seconds so far
  result = health_service_sum(HealthMetricStepCount,
                              time_util_get_midnight_of(now),
                              (time_util_get_midnight_of(now) + now) / 2);
  cl_assert_equal_i(result, 500);
}

void test_health__cache(void) {
  cl_assert_equal_p(s_health_service.cache, NULL);
  health_service_events_subscribe(NULL, NULL);
  HealthServiceCache *const cache = s_health_service.cache;
  cl_assert(cache != NULL);

  // cache is preserved
  health_service_events_subscribe(NULL, NULL);
  cl_assert_equal_p(s_health_service.cache, cache);

  health_service_events_unsubscribe();
  cl_assert_equal_p(s_health_service.cache, NULL);

  // multiple unsubscribe/empty cache doesn't cause a problem
  health_service_events_unsubscribe();
  cl_assert_equal_p(s_health_service.cache, NULL);
}

void test_health__metric_accessible(void) {
  const time_t now = rtc_get_time();

  HealthServiceAccessibilityMask accessible;
  // all value in the future
  accessible = health_service_metric_accessible(HealthMetricStepCount, now + 10, now + 20);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskNotAvailable);

  // normal value is available
  accessible = health_service_metric_accessible(HealthMetricStepCount, now - 10, now);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);

  // values that partly are in unsupported range are available
  accessible = health_service_metric_accessible(HealthMetricStepCount, now - 10, now + 20);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);

  // if all values are -1, data is not available
  s_sys_activity_get_metric_values.out.history[0] = -1;
  s_sys_activity_get_metric_values.out.history[1] = -1;
  accessible = health_service_metric_accessible(HealthMetricStepCount,
                                                now - SECONDS_PER_DAY, now);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskNotAvailable);

  // if some values are >= 0, data is not available (day at idx 2)
  accessible = health_service_metric_accessible(HealthMetricStepCount,
                                                now - 2 * SECONDS_PER_DAY, now);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);
}

void test_health__metric_hr_accessible(void) {
  const time_t now = rtc_get_time();

  HealthServiceAccessibilityMask accessible;
  // all value in the future
  accessible = health_service_metric_accessible(HealthMetricHeartRateBPM, now + 10, now + 20);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskNotAvailable);

  // normal value is available
  accessible = health_service_metric_accessible(HealthMetricHeartRateBPM, now - 10, now);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);

  // values that partly are in unsupported range are available
  accessible = health_service_metric_accessible(HealthMetricHeartRateBPM, now - 10, now + 20);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);

  // HR has a limit of two hours. Make sure if we are within that range it's available
  accessible = health_service_metric_accessible(HealthMetricHeartRateBPM,
                                                now - 2 * SECONDS_PER_HOUR, now);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);
}

void test_health__metric_hr_averaged_accessible(void) {
  const time_t now = rtc_get_time();

  typedef struct {
    const char *desc;
    struct {
      HealthMetric metric;
      time_t time_start;
      time_t time_end;
      HealthServiceTimeScope scope;
      bool hr_disabled;
    } in;
    struct {
      HealthServiceAccessibilityMask accessible;
    } out;
  } TestInputOutput;

  const TestInputOutput tests[] = {
    {
      .desc = "Valid time range with ScopeOnce",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskAvailable }
    },
    {
      .desc = "Valid time range with ScopeWeekly",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthServiceTimeScopeWeekly },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "Valid time range with ScopeDailyWeekdayOrWeekend",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthServiceTimeScopeDailyWeekdayOrWeekend },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "Valid time range with ScopeDaily",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthServiceTimeScopeDaily },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "Invalid future time range with ScopeOnce",
      .in =  { HealthMetricHeartRateBPM, now + 10, now + 20, HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskNotAvailable }
    },
    {
      .desc = "Time range that goes further back into history than BPM supports",
      .in =  { HealthMetricHeartRateBPM, now - 3 * SECONDS_PER_HOUR, now, HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "Time range that goes further back into history than BPM supports",
      .in =  { HealthMetricHeartRateBPM, now - 3 * SECONDS_PER_HOUR, now - 1 * SECONDS_PER_HOUR, HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "HR Disabled. Return NoPermission",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthServiceTimeScopeOnce, true},
      .out = { HealthServiceAccessibilityMaskNoPermission }
    },
  };

  // Run all the tests
  HealthServiceAccessibilityMask accessible;
  for (int i = 0; i < ARRAY_LENGTH(tests); i++) {
    const TestInputOutput *test = &tests[i];
    s_activity_prefs_heart_rate_enabled = !test->in.hr_disabled;
    accessible = health_service_metric_averaged_accessible(test->in.metric,
                                                           test->in.time_start,
                                                           test->in.time_end,
                                                           test->in.scope);
    PBL_LOG(LOG_LEVEL_DEBUG, "%s\nMetric: %d, start: %d, end: %d, Scope: %d",
            test->desc, (int)test->in.metric, (int)test->in.time_start,
            (int)test->in.time_end, (int)test->in.scope);
    cl_assert_equal_i(accessible, tests[i].out.accessible);
  }
}

void test_health__metric_hr_aggregate_averaged_accessible(void) {
  const time_t now = rtc_get_time();

  typedef struct {
    const char *desc;
    struct {
      HealthMetric metric;
      time_t time_start;
      time_t time_end;
      HealthAggregation aggregation;
      HealthServiceTimeScope scope;
      bool hr_disabled;
    } in;
    struct {
      HealthServiceAccessibilityMask accessible;
    } out;
  } TestInputOutput;

  const TestInputOutput tests[] = {
    {
      .desc = "Valid time range with ScopeDaily and Sum. Should be NotSupported",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthAggregationSum,
               HealthServiceTimeScopeDaily },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "Valid time range with ScopeDaily and Avg. Not available because Daily",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthAggregationAvg,
               HealthServiceTimeScopeDaily },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "Valid time range with ScopeOnce and Min. Available",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthAggregationMin,
               HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskAvailable }
    },
    {
      .desc = "Valid time range with ScopeOnce and Max. Available",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthAggregationMax,
               HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskAvailable }
    },
    {
      .desc = "Valid time range with ScopeDaily and Max. NotSupported",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthAggregationMax,
               HealthServiceTimeScopeDaily },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "Invalid time range with ScopeOnce and Max. NotSupported",
      .in =  { HealthMetricHeartRateBPM, now - 3 * SECONDS_PER_HOUR, now - 2 * SECONDS_PER_HOUR,
               HealthAggregationMax, HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
    {
      .desc = "HR Disabled. Return NoPermission",
      .in =  { HealthMetricHeartRateBPM, now - 10, now, HealthAggregationMax,
               HealthServiceTimeScopeOnce, true },
      .out = { HealthServiceAccessibilityMaskNoPermission }
    },
    {
      .desc = "Time range that goes further back into history than BPM supports",
      .in =  { HealthMetricHeartRateBPM, now - 3 * SECONDS_PER_HOUR, now - 1 * SECONDS_PER_HOUR,
               HealthAggregationAvg, HealthServiceTimeScopeOnce },
      .out = { HealthServiceAccessibilityMaskNotSupported }
    },
  };

  // Run all the tests
  HealthServiceAccessibilityMask accessible;
  for (int i = 0; i < ARRAY_LENGTH(tests); i++) {
    const TestInputOutput *test = &tests[i];
    s_activity_prefs_heart_rate_enabled = !test->in.hr_disabled;
    accessible = health_service_metric_aggregate_averaged_accessible(test->in.metric,
                                                                     test->in.time_start,
                                                                     test->in.time_end,
                                                                     test->in.aggregation,
                                                                     test->in.scope);
    PBL_LOG(LOG_LEVEL_DEBUG, "%s\nMetric: %d, start: %d, end: %d, Aggregation: %d, Scope: %d",
            test->desc, (int)test->in.metric, (int)test->in.time_start,
            (int)test->in.time_end, (int)test->in.aggregation, (int)test->in.scope);
    cl_assert_equal_i(accessible, tests[i].out.accessible);
  }
}

void test_health__sleep_session_matches(void) {
  const time_t now = rtc_get_time();
  ActivitySession session = {
    .type = ActivitySessionType_Sleep,
    .start_utc = now - (10 * SECONDS_PER_MINUTE),
    .length_min = 10,
  };
  bool (*fun)(const ActivitySession *, HealthActivityMask, time_t, time_t) =
    prv_activity_session_matches;

  // mask none matches nothing
  cl_assert_equal_b(false, fun(&session, HealthActivityNone, now - (10 * SECONDS_PER_MINUTE),
                               now));

  // mask restful doesn't match
  cl_assert_equal_b(false, fun(&session, HealthActivityRestfulSleep,
                               now - (10 * SECONDS_PER_MINUTE), now));

  // exact time range matches
  cl_assert_equal_b(true, fun(&session, HealthActivityMaskAll, now - (10 * SECONDS_PER_MINUTE),
                              now));

  // too large time range matches
  cl_assert_equal_b(true, fun(&session, HealthActivityMaskAll, now - (20 * SECONDS_PER_MINUTE),
                              now + (10 * SECONDS_PER_MINUTE)));

  // range before doesn't match, even if it touches
  cl_assert_equal_b(false, fun(&session, HealthActivityMaskAll, now - (20 * SECONDS_PER_MINUTE),
                               now - (10 * SECONDS_PER_MINUTE)));

  // range after doesn't match, even if it touches
  cl_assert_equal_b(false, fun(&session, HealthActivityMaskAll, now,
                               now + (10 * SECONDS_PER_MINUTE)));

  // range that starts before matches
  cl_assert_equal_b(true, fun(&session, HealthActivityMaskAll, now - (20 * SECONDS_PER_MINUTE),
                              now - (9 * SECONDS_PER_MINUTE)));

  // range that ends after matches
  cl_assert_equal_b(true, fun(&session, HealthActivityMaskAll, now - (1 * SECONDS_PER_MINUTE),
                              now + (10 * SECONDS_PER_MINUTE)));
}

void test_health__any_activity_accessible(void) {
  const time_t now = rtc_get_time();
  HealthServiceAccessibilityMask accessible;

  // empty mask => not available
  accessible = health_service_any_activity_accessible(HealthActivityNone,
                                                      now - (10 * SECONDS_PER_MINUTE), now);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskNotAvailable);

  accessible = health_service_any_activity_accessible(HealthActivityMaskAll,
                                                      now - (10 * SECONDS_PER_MINUTE), now);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);

  // too far in the past
  accessible = health_service_any_activity_accessible(HealthActivityMaskAll,
                                                      now - 10 * SECONDS_PER_DAY,
                                                      now - 9 * SECONDS_PER_DAY);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskNotAvailable);

  // range far into to past and future
  accessible = health_service_any_activity_accessible(HealthActivityMaskAll,
                                                      now - 10 * SECONDS_PER_DAY,
                                                      now + 10 * SECONDS_PER_DAY);
  cl_assert_equal_i(accessible, HealthServiceAccessibilityMaskAvailable);
}

void test_health__activities_iterate(void) {
  const time_t now = rtc_get_time();

  // start from oldest to most-recent - this is more or less an arbitrary order
  s_sys_activity_get_sessions_values.out.sessions[6] = (ActivitySession){
    .type = ActivitySessionType_Open,
    .start_utc = now - (95 * SECONDS_PER_MINUTE),
    .length_min = 15, // end = -80
  };
  s_sys_activity_get_sessions_values.out.sessions[5] = (ActivitySession){
    .type = ActivitySessionType_Run,
    .start_utc = now - (80 * SECONDS_PER_MINUTE),
    .length_min = 15, // end = -65
  };
  s_sys_activity_get_sessions_values.out.sessions[4] = (ActivitySession){
    .type = ActivitySessionType_Walk,
    .start_utc = now - (65 * SECONDS_PER_MINUTE),
    .length_min = 15, // end = -50
  };
  s_sys_activity_get_sessions_values.out.sessions[3] = (ActivitySession){
    .type = ActivitySessionType_Sleep,
    .start_utc = now - (50 * SECONDS_PER_MINUTE),
    .length_min = 20, // end = -30
  };
  s_sys_activity_get_sessions_values.out.sessions[2] = (ActivitySession){
    .type = ActivitySessionType_RestfulSleep,
    .start_utc = now - (45 * SECONDS_PER_MINUTE),
    .length_min = 10, // end = -35
  };
  s_sys_activity_get_sessions_values.out.sessions[1] = (ActivitySession){
    .type = ActivitySessionType_Sleep,
    .start_utc = now - (20 * SECONDS_PER_MINUTE),
    .length_min = 10, // end = -10
  };
  s_sys_activity_get_sessions_values.out.sessions[0] = (ActivitySession){
    .type = ActivitySessionType_RestfulSleep,
    .start_utc = now - (18 * SECONDS_PER_MINUTE),
    .length_min = 5, // end = -13
  };
  // oldest to most-recent (looking at each session's start): 3, 2, 1, 0
  // most-recent to oldest (looking at each session's end): 1, 0, 3, 2

  const int num_sleep_sessions = 4;
  const int num_restfulsleep_sessions = 2;
  const int num_run_sessions = 1;
  const int num_walk_sessions = 1;
  const int num_open_sessions = 1;
  const int num_sessions = num_sleep_sessions + num_restfulsleep_sessions +
                           num_run_sessions + num_walk_sessions + num_open_sessions;

  // result from mocked sys_activity_get_sessions_values is still false
  health_service_activities_iterate(HealthActivityMaskAll, now - (100 * SECONDS_PER_MINUTE), now,
                                    HealthIterationDirectionPast, prv_activity_cb, NULL);
  cl_assert_equal_i(0, s_prv_activity_cb__call_count);

  s_sys_activity_get_sessions_values.out.result = true;
  // result from mocked sys_activity_get_sessions_values is still 0 sessions
  health_service_activities_iterate(HealthActivityMaskAll, now - (100 * SECONDS_PER_MINUTE), now,
                                    HealthIterationDirectionPast, prv_activity_cb, NULL);
  cl_assert_equal_i(0, s_prv_activity_cb__call_count);

  // respect mask for RestfulSleep
  s_prv_activity_cb__call_count = 0;
  s_sys_activity_get_sessions_values.out.num_sessions = 7;
  health_service_activities_iterate(HealthActivityRestfulSleep, now - (100 * SECONDS_PER_MINUTE),
                                    now, HealthIterationDirectionPast, prv_activity_cb, NULL);
  cl_assert_equal_i(num_restfulsleep_sessions, s_prv_activity_cb__call_count);
  cl_assert_equal_b(s_prv_activity_cb__args[0].activity, HealthActivityRestfulSleep);

  // respect mask for Run/Walk
  s_prv_activity_cb__call_count = 0;
  s_sys_activity_get_sessions_values.out.num_sessions = 7;
  health_service_activities_iterate(HealthActivityRun | HealthActivityWalk |
                                    HealthActivityOpenWorkout,
                                    now - (100 * SECONDS_PER_MINUTE),
                                    now, HealthIterationDirectionPast, prv_activity_cb, NULL);
  cl_assert_equal_i(num_run_sessions + num_walk_sessions + num_open_sessions,
                    s_prv_activity_cb__call_count);
  cl_assert_equal_b(s_prv_activity_cb__args[0].activity, HealthActivityRun);

  // respect range
  s_prv_activity_cb__call_count = 0;
  s_sys_activity_get_sessions_values.out.num_sessions = 7;
  health_service_activities_iterate(HealthActivitySleep, now - (15 * SECONDS_PER_MINUTE), now,
                                    HealthIterationDirectionPast, prv_activity_cb, NULL);
  cl_assert_equal_i(1, s_prv_activity_cb__call_count);
  cl_assert_equal_b(s_prv_activity_cb__args[0].activity, HealthActivitySleep);

  // order direction past
  s_prv_activity_cb__call_count = 0;
  health_service_activities_iterate(HealthActivityMaskAll, now - (200 * SECONDS_PER_MINUTE), now,
                                    HealthIterationDirectionPast, prv_activity_cb, NULL);
  cl_assert_equal_i(7, s_prv_activity_cb__call_count);
  cl_assert_equal_i(s_prv_activity_cb__args[0].time_start,
                    s_sys_activity_get_sessions_values.out.sessions[1].start_utc);
  cl_assert_equal_i(s_prv_activity_cb__args[3].time_start,
                    s_sys_activity_get_sessions_values.out.sessions[2].start_utc);

  // order direction future
  s_prv_activity_cb__call_count = 0;
  health_service_activities_iterate(HealthActivityMaskAll, now - (200 * SECONDS_PER_MINUTE), now,
                                    HealthIterationDirectionFuture, prv_activity_cb, NULL);
  cl_assert_equal_i(7, s_prv_activity_cb__call_count);
  cl_assert_equal_i(s_prv_activity_cb__args[0].time_start,
                    s_sys_activity_get_sessions_values.out.sessions[6].start_utc);
  cl_assert_equal_i(s_prv_activity_cb__args[6].time_start,
                    s_sys_activity_get_sessions_values.out.sessions[0].start_utc);
}

void test_health__peek_current_activities(void) {
  HealthActivityMask activities;
  activities = health_service_peek_current_activities();
  cl_assert_equal_i(activities, HealthActivityNone);
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.history_len, 1);
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.metric, ActivityMetricSleepState);

  s_sys_activity_get_metric_values.out.history[0] = ActivitySleepStateLightSleep;
  activities = health_service_peek_current_activities();
  cl_assert_equal_i(activities, HealthActivitySleep);

  s_sys_activity_get_metric_values.out.history[0] = ActivitySleepStateRestfulSleep;
  activities = health_service_peek_current_activities();
  cl_assert_equal_i(activities, HealthActivitySleep | HealthActivityRestfulSleep);

  s_sys_activity_get_metric_values.out.history[0] = ActivitySleepStateAwake;
  s_activity_sessions_ongoing[ActivitySessionType_Run] = true;
  s_activity_sessions_ongoing[ActivitySessionType_Walk] = true;
  s_activity_sessions_ongoing[ActivitySessionType_Open] = true;
  activities = health_service_peek_current_activities();
  cl_assert_equal_i(activities, HealthActivityRun | HealthActivityWalk | HealthActivityOpenWorkout);
}

void test_health__session_compare(void) {
  const time_t now = rtc_get_time();

  // both start at the same time
  cl_assert(0 == prv_session_compare(
    &(ActivitySession) {.start_utc = now, .length_min = 10},
    &(ActivitySession) {.start_utc = now, .length_min = 5},
    HealthIterationDirectionFuture));

  // a starts earlier
  cl_assert(0 > prv_session_compare(
    &(ActivitySession) {.start_utc = now, .length_min = 10},
    &(ActivitySession) {.start_utc = now + (2 * SECONDS_PER_MINUTE), .length_min = 5},
    HealthIterationDirectionFuture));

  // b starts earlier
  cl_assert(0 < prv_session_compare(
    &(ActivitySession) {.start_utc = now, .length_min = 10},
    &(ActivitySession) {.start_utc = now - (2 * SECONDS_PER_MINUTE), .length_min = 5},
    HealthIterationDirectionFuture));

  // both end at the same time
  cl_assert(0 == prv_session_compare(
    &(ActivitySession) {.start_utc = now, .length_min = 10},
    &(ActivitySession) {.start_utc = now + (5 * SECONDS_PER_MINUTE), .length_min = 5},
    HealthIterationDirectionPast));

  // a ends later
  cl_assert(0 > prv_session_compare(
    &(ActivitySession) {.start_utc = now, .length_min = 10},
    &(ActivitySession) {.start_utc = now + (2 * SECONDS_PER_MINUTE), .length_min = 5},
    HealthIterationDirectionPast));

  // b ends later
  cl_assert(0 < prv_session_compare(
    &(ActivitySession) {.start_utc = now, .length_min = 5},
    &(ActivitySession) {.start_utc = now + (2 * SECONDS_PER_MINUTE), .length_min = 5},
    HealthIterationDirectionPast));
}

void test_health__get_minute_history_edge_case_args(void) {
  const time_t now = rtc_get_time();

  HealthMinuteData data[5] = {};
  uint32_t written;

  // null pointer
  time_t time_start = now - 10 * 60 - 30;
  time_t time_end = now - 20;
  written = health_service_get_minute_history(NULL, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(0, written);

  // empty boundary
  written = health_service_get_minute_history(data, 0, &time_start, &time_end);
  cl_assert_equal_i(0, written);

  // empty start
  written = health_service_get_minute_history(data, ARRAY_LENGTH(data), NULL, &time_end);
  cl_assert_equal_i(0, written);

  // empty end before start
  time_t early_end = time_start - 20 * SECONDS_PER_MINUTE;
  written = health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &early_end);
  cl_assert_equal_i(0, written);

  // empty end works just fine
  s_sys_activity_get_minute_history_values = (sys_activity_get_minute_history_values) {
    .out[0] = {
      .num_records = 2,
      .result = true,
    },
  };
  written = health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, NULL);
  cl_assert_equal_i(2, written);
}

void test_health__get_minute_history(void) {
  const time_t now = rtc_get_time();

  HealthMinuteData data[5] = {};
  uint32_t written;
  s_sys_activity_get_minute_history_values = (sys_activity_get_minute_history_values) {
    .out[0] = {
      .num_records = 3,
      .result = true,
      .utc_start = now - 10 * SECONDS_PER_MINUTE,
      .records = {
        {.is_invalid = false, .steps = 1},
        {.is_invalid = true, .steps = 2},
        {.is_invalid = false, .steps = 3},
      },
    },
  };

  // pass time that's not exactly on a boundary
  time_t time_start = now - 10 * SECONDS_PER_MINUTE - 30;
  time_t time_end = now - 20;
  written = health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(3, written);
  cl_assert_equal_i(now - 10 * SECONDS_PER_MINUTE, time_start);
  cl_assert_equal_i(time_start + written * SECONDS_PER_MINUTE, time_end);
  cl_assert_equal_i(1, data[0].steps);
  cl_assert_equal_i(2, data[1].steps);
  cl_assert_equal_i(3, data[2].steps);

  // if internal sys_activity returns false, no records were written
  s_sys_activity_get_minute_history_values.stage = 0;
  s_sys_activity_get_minute_history_values.out[0].result = false;
  written = health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(0, written);
}

void test_health__get_minute_history_respects_time_end(void) {
  s_sys_activity_get_minute_history_values.stage = 0;
  s_sys_activity_get_minute_history_values.out[0].asserts = false;

  HealthMinuteData data[5] = {};

  // pass time that's not exactly on a boundary
  time_t time_start;
  time_t time_end;

  ////////////
  // start time on boundary
  const time_t time_on_boundary = (rtc_get_time() / 60 * 60) - 10 * 60;

  // respects time_end, 2.5 minutes => 3 records
  time_start = time_on_boundary;
  time_end = time_start + (5 * SECONDS_PER_MINUTE / 2);
  health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(3, s_sys_activity_get_minute_history_values.in[0].num_records);

  // respects time_end, 1 minute => 1 records
  s_sys_activity_get_minute_history_values.stage = 0;
  time_start = time_on_boundary;
  time_end = time_start + SECONDS_PER_MINUTE;
  health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(1, s_sys_activity_get_minute_history_values.in[0].num_records);

  // respects time_end == time_start => 0 records
  s_sys_activity_get_minute_history_values.stage = 0;
  time_start = time_on_boundary;
  time_end = time_start;
  health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(0, s_sys_activity_get_minute_history_values.in[0].num_records);

  ////////////
  // start time almost on the next minute
  s_sys_activity_get_minute_history_values.stage = 0;
  const time_t time_almost_next_minute = time_on_boundary + 59;
  // respects time_end, 2.5 minutes => 3 records
  time_start = time_almost_next_minute;
  time_end = time_start + (5 * SECONDS_PER_MINUTE / 2);
  health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(4, s_sys_activity_get_minute_history_values.in[0].num_records);

  // respects time_end, 1 minute => 1 records
  s_sys_activity_get_minute_history_values.stage = 0;
  time_start = time_almost_next_minute;
  time_end = time_start + SECONDS_PER_MINUTE;
  health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(2, s_sys_activity_get_minute_history_values.in[0].num_records);

  // respects time_end == time_start => 0 records
  s_sys_activity_get_minute_history_values.stage = 0;
  time_start = time_almost_next_minute;
  time_end = time_start;
  health_service_get_minute_history(data, ARRAY_LENGTH(data), &time_start, &time_end);
  cl_assert_equal_i(1, s_sys_activity_get_minute_history_values.in[0].num_records);
}

void test_health__get_yesterdays_sleep_activity(void) {
  HealthValue start_sec;
  HealthValue end_sec;
  // our mock setup doesn't provide configurations for multiple calls
  // as health_service_private_get_yesterdays_sleep_activity() calls the function twice, both
  // values will have the same value

  s_sys_activity_get_metric_values.out.history[0] = 123;
  bool success = health_service_private_get_yesterdays_sleep_activity(&start_sec, &end_sec);
  cl_assert(success);
  cl_assert_equal_i(123, start_sec);
  cl_assert_equal_i(123, end_sec);
  cl_assert_equal_i(1, s_sys_activity_get_metric_values.in.history_len);
  cl_assert_equal_i(ActivityMetricSleepExitAtSeconds, s_sys_activity_get_metric_values.in.metric);
}

// Test that health_service_sum_averaged() returns the correct result when asked to get the
// average daily value of a metric
void test_health__avg_full_days(void) {
  // Get the current time and day
  const time_t now = rtc_get_time();
  struct tm local_tm;
  localtime_r(&now, &local_tm);
  DayInWeek day_in_week = local_tm.tm_wday;

  // ----------------------------------------
  // Let's fill in some known data for the daily totals and accumulate the totals and counts
  // for each day of the week.
  int day_totals[DAYS_PER_WEEK] = {};
  int day_counts[DAYS_PER_WEEK] = {};
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++, day_in_week--) {
    day_in_week = positive_modulo(day_in_week, DAYS_PER_WEEK);
    s_sys_activity_get_metric_values.out.history[i] = 1000 + (i * 50);

    // Day 0 is not included in the stats
    if (i == 0) {
      continue;
    }

    // Increment totals for each day of the week
    day_totals[day_in_week] += s_sys_activity_get_metric_values.out.history[i];
    day_counts[day_in_week] += 1;
  }

  // ----------------------------------------
  // Compute expected values
  int exp_weekly = day_totals[local_tm.tm_wday] / day_counts[local_tm.tm_wday];

  int exp_daily = 0;
  int count = 0;
  for (int i = 0; i < DAYS_PER_WEEK; i++) {
    exp_daily += day_totals[i];
    count += day_counts[i];
  }
  exp_daily /= count;

  int exp_weekend = (day_totals[Sunday] + day_totals[Saturday])
                    / (day_counts[Sunday] + day_counts[Saturday]);

  int exp_weekday = 0;
  count = 0;
  for (int i = Monday; i <= Friday; i++) {
    exp_weekday += day_totals[i];
    count += day_counts[i];
  }
  exp_weekday /= count;


  // ----------------------------------------
  // Compute each type of daily average using the API and compare to expected
  HealthValue result;
  result = health_service_sum_averaged(HealthMetricStepCount,
                                       time_util_get_midnight_of(now) - SECONDS_PER_DAY,
                                       time_util_get_midnight_of(now), HealthServiceTimeScopeDaily);
  cl_assert_equal_i(result, exp_daily);

  // All of our tests set "now" to Mon, 28 Dec 2015 09:12:22 GMT, so yesterday was a Sunday
  result = health_service_sum_averaged(HealthMetricStepCount,
                                       time_util_get_midnight_of(now) - SECONDS_PER_DAY,
                                       time_util_get_midnight_of(now),
                                       HealthServiceTimeScopeDailyWeekdayOrWeekend);
  cl_assert_equal_i(result, exp_weekend);

  // All of our tests set "now" to Mon, 28 Dec 2015 09:12:22 GMT, so today is a weekday
  result = health_service_sum_averaged(HealthMetricStepCount,
                                       time_util_get_midnight_of(now),
                                       time_util_get_midnight_of(now) + SECONDS_PER_DAY,
                                       HealthServiceTimeScopeDailyWeekdayOrWeekend);
  cl_assert_equal_i(result, exp_weekday);

  // Average weekly value
  result = health_service_sum_averaged(HealthMetricStepCount,
                                       time_util_get_midnight_of(now),
                                       time_util_get_midnight_of(now) + SECONDS_PER_DAY,
                                       HealthServiceTimeScopeWeekly);
  cl_assert_equal_i(result, exp_weekly);

  // Average weekly 48hr avg
  result = health_service_sum_averaged(HealthMetricStepCount,
                                       time_util_get_midnight_of(now),
                                       time_util_get_midnight_of(now) + 2 * SECONDS_PER_DAY,
                                       HealthServiceTimeScopeWeekly);
  cl_assert_equal_i(result, 2 *exp_weekly);

}

// Return the sum of a bunch of step average chunks that cover the given time range. The time
// range is given in the minute offsets from midnight. This logic is written to produce the
// same results as implemented in health_data_steps_get_current_average() of health_data.c
// Once the Health app is updated to also use the Health API, we can change this logic freely.
static uint32_t prv_averages_sum(uint32_t minute_start_idx, uint32_t minute_end_idx,
                                 const ActivityMetricAverages *avgs) {
  cl_assert(minute_start_idx < MINUTES_PER_DAY);
  cl_assert(minute_end_idx < MINUTES_PER_DAY);

  const int k_minutes_per_step_avg = MINUTES_PER_DAY / ACTIVITY_NUM_METRIC_AVERAGES;
  uint32_t chunk_start_idx = minute_start_idx / k_minutes_per_step_avg;
  uint32_t chunk_end_idx = minute_end_idx / k_minutes_per_step_avg;
  uint32_t sum = 0;
  for (int i = chunk_start_idx; i < chunk_end_idx; i++) {
    sum += avgs->average[i];
  }
  return sum;
}

// Test that health_service_sum_averaged() returns the correct result when asked to get the
// intraday average of a metric.
void test_health__avg_partial_days(void) {
  // Get the current time and day
  const time_t now = rtc_get_time();
  struct tm local_tm;
  localtime_r(&now, &local_tm);
  DayInWeek day_in_week = local_tm.tm_wday;

  // Our _initialize should set us to Monday, 9am UTC
  cl_assert_equal_i(day_in_week, Monday);

  // ----------------------------------
  // Let's fill in known data for the 15-minute step averages
  s_sys_activity_get_step_averages_values_weekday = (sys_activity_get_step_averages_values){
    .out.result = true,
  };
  s_sys_activity_get_step_averages_values_weekend = (sys_activity_get_step_averages_values){
    .out.result = true,
  };

  for (int i = 0;
       i < ARRAY_LENGTH(s_sys_activity_get_step_averages_values_weekday.out.averages.average);
       i++) {
    s_sys_activity_get_step_averages_values_weekday.out.averages.average[i] = i * 10;
    s_sys_activity_get_step_averages_values_weekend.out.averages.average[i] = i * 5;
  }


  // Let's fill in daily totals that will be used when 15-minute averages are not available
  // (i.e. for metrics other than step averages)
  const int k_daily_total = 960;
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++, day_in_week--) {
    s_sys_activity_get_metric_values.out.history[i] = k_daily_total;
  }

  // ---
  // Compute weekday step average from midnight to 9am. This should use the 15-minute
  // step averages that we stuffed in.
  uint32_t exp_value = prv_averages_sum(0, 9 * MINUTES_PER_HOUR,
      &s_sys_activity_get_step_averages_values_weekday.out.averages);

  time_t start_of_today = time_start_of_today();
  HealthValue value = health_service_sum_averaged(HealthMetricStepCount, start_of_today,
                                                  start_of_today + (9 * SECONDS_PER_HOUR),
                                                  HealthServiceTimeScopeDailyWeekdayOrWeekend);
  cl_assert_equal_i(value, exp_value);


  // ---
  // Compute weekday HealthMetricActiveSeconds from midnight to 9am. This should use the daily
  // totals since we don't have 15-minute averages maintained for this metric
  exp_value = (k_daily_total * 9 * MINUTES_PER_HOUR) / MINUTES_PER_DAY;

  start_of_today = time_start_of_today();
  value = health_service_sum_averaged(HealthMetricActiveSeconds, start_of_today,
                                                  start_of_today + (9 * SECONDS_PER_HOUR),
                                                  HealthServiceTimeScopeDailyWeekdayOrWeekend);
  cl_assert_equal_i(value, exp_value);


  // ---
  // Compute weekend step average from 4am to 9am. This should use the 15-minute
  // step averages that we stuffed in.
  exp_value = prv_averages_sum(4 * MINUTES_PER_HOUR, 9 * MINUTES_PER_HOUR,
                       &s_sys_activity_get_step_averages_values_weekend.out.averages);

  // Since "today" is Monday, going back 24 hours puts us on a weekend
  time_t start_time = time_start_of_today() - SECONDS_PER_DAY
                    + (4 * SECONDS_PER_HOUR);
  value = health_service_sum_averaged(HealthMetricStepCount, start_time,
                                                  start_time + (5 * SECONDS_PER_HOUR),
                                                  HealthServiceTimeScopeDailyWeekdayOrWeekend);
  cl_assert_equal_i(value, exp_value);

  // ---
  // Compute weekend HealthMetricActiveSeconds average from 4am to 9am. This should use the
  // daily totals since we don't have 15-minute averages maintained for this metric
  exp_value = (k_daily_total * 5 * MINUTES_PER_HOUR) / MINUTES_PER_DAY;

  // Since "today" is Monday, going back 24 hours puts us on a weekend
  value = health_service_sum_averaged(HealthMetricActiveSeconds, start_time,
                                      start_time + (5 * SECONDS_PER_HOUR),
                                      HealthServiceTimeScopeDailyWeekdayOrWeekend);
  cl_assert_equal_i(value, exp_value);


  // ---
  // Compute daily step average from midnight to 9am. This should use the 15-minute
  // step averages that we stuffed in.
  exp_value = 5 * prv_averages_sum(0, 9 * MINUTES_PER_HOUR,
                        &s_sys_activity_get_step_averages_values_weekday.out.averages);
  exp_value += 2 * prv_averages_sum(0, 9 * MINUTES_PER_HOUR,
                        &s_sys_activity_get_step_averages_values_weekend.out.averages);
  exp_value /= 7;

  start_of_today = time_start_of_today();
  value = health_service_sum_averaged(HealthMetricStepCount, start_of_today,
                                                  start_of_today + (9 * SECONDS_PER_HOUR),
                                                  HealthServiceTimeScopeDaily);
  cl_assert_equal_i(value, exp_value);
}

void test_health__get_measurement_system_for_display(void) {
  MeasurementSystem actual =
    health_service_get_measurement_system_for_display(HealthMetricSleepSeconds);
  cl_assert_equal_i(actual, MeasurementSystemUnknown);

  s_units_distance_result = UnitsDistance_Miles;
  actual = health_service_get_measurement_system_for_display(HealthMetricWalkedDistanceMeters);
  cl_assert_equal_i(actual, MeasurementSystemImperial);

  s_units_distance_result = UnitsDistance_KM;
  actual = health_service_get_measurement_system_for_display(HealthMetricWalkedDistanceMeters);
  cl_assert_equal_i(actual, MeasurementSystemMetric);
}

void test_health__peek_current_value(void) {
  const time_t now_utc = rtc_get_time();

  // Set the return value to a valid time (Less than HS_MAX_AGE_HR_SAMPLE from the current time)
  prv_override_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC, now_utc);

  s_sys_activity_get_metric_values.out.history[0] = 123;
  s_sys_activity_get_metric_values.out.history[1] = 456;
  HealthValue result = health_service_peek_current_value(HealthMetricHeartRateBPM);
  cl_assert_equal_i(123, result);
  const ActivityMetric IN_METRIC = ActivityMetricHeartRateFilteredBPM;
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.metric, IN_METRIC);
  cl_assert_equal_i(s_sys_activity_get_metric_values.in.history_len, 1);

  // This is the equivalent to `peek_value` with HeartRateBPM. Make sure it is equal.
  result = health_service_aggregate_averaged(HealthMetricHeartRateBPM,
                                             now_utc, now_utc,
                                             HealthAggregationAvg, HealthServiceTimeScopeOnce);
  cl_assert_equal_i(123, result);

  // This is the equivalent to `peek_value` with HeartRateBPM. Make sure it is equal.
  result = health_service_aggregate_averaged(HealthMetricHeartRateBPM,
                                             now_utc - 60, now_utc - 60,
                                             HealthAggregationAvg, HealthServiceTimeScopeOnce);
  cl_assert_equal_i(123, result);

  // The function call is the equivalent to `peek_value` with HeartRateBPM except for the
  // time stamps, make sure it returns 0 because we haven't filled in that data)
  result = health_service_aggregate_averaged(HealthMetricHeartRateBPM,
                                             now_utc - 61, now_utc - 61,
                                             HealthAggregationAvg, HealthServiceTimeScopeOnce);
  cl_assert_equal_i(0, result);

  // Set the return value to an invalid time (More than HS_MAX_AGE_HR_SAMPLE from the current time)
  prv_override_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC,
                      rtc_get_time() - 20 * SECONDS_PER_MINUTE);
  result = health_service_peek_current_value(HealthMetricHeartRateBPM);
  cl_assert_equal_i(0, result);

  // Asking for a cumulative metric should return error (0)
  result = health_service_peek_current_value(HealthMetricStepCount);
  cl_assert_equal_i(0, result);
}


static void prv_update_stats(HealthServiceStats *stats, HealthValue value) {
  stats->sum += value;
  stats->min = MIN(value, stats->min);
  stats->max = MAX(value, stats->max);
  stats->count++;
  stats->avg = stats->sum / stats->count;
}

// Test that health_service_aggregate_averaged() returns the correct result when asked to get the
// aggregates of rate metrics (like heart rate). For these metrics, only min, max, and avg are
// valid aggregation functions. The sum function is only applicable to cumulative metrics and is
// tested above in test_health__sum_full_days().

// DISABLED because the firmware doesn't actually store daily history of HRM values.
void DISABLED_test_health__min_max_avg_full_days(void) {
  // Get the current time and day
  const time_t now = rtc_get_time();
  const time_t yesterday_utc = now - SECONDS_PER_DAY;

  struct tm local_tm;
  localtime_r(&now, &local_tm);
  DayInWeek todays_day_in_week = local_tm.tm_wday;

  localtime_r(&yesterday_utc, &local_tm);
  DayInWeek yesterday_day_in_week = local_tm.tm_wday;
  bool yesterday_was_weekend = (yesterday_day_in_week == Sunday)
                               || (yesterday_day_in_week == Saturday);

  PBL_LOG(LOG_LEVEL_DEBUG, "yesterday day in week: %d", yesterday_day_in_week);

  // ----------------------------------------
  // Let's fill in some known data for the daily totals and accumulate the stats
  HealthServiceStats weekly_stats = {
    .min = INT32_MAX,
    .max = INT32_MIN,
  };
  HealthServiceStats daily_stats = {
    .min = INT32_MAX,
    .max = INT32_MIN,
  };
  HealthServiceStats weekday_stats = {
    .min = INT32_MAX,
    .max = INT32_MIN,
  };
  HealthServiceStats weekend_stats = {
    .min = INT32_MAX,
    .max = INT32_MIN,
  };
  HealthServiceStats yesterday_stats = {
    .min = INT32_MAX,
    .max = INT32_MIN,
  };

  DayInWeek day_in_week = todays_day_in_week;
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++, day_in_week--) {
    day_in_week = positive_modulo(day_in_week, DAYS_PER_WEEK);
    HealthValue value = 1000 + (i * 50);
    s_sys_activity_get_metric_values.out.history[i] = value;

    PBL_LOG(LOG_LEVEL_DEBUG, "Day #%d, day_of_week: %d, value: %"PRIi32" ", i, day_in_week,
             value);
    // Day 0 is not included in the stats
    if (i == 0) {
      continue;
    }

    // Store if this is yesterday
    if (i == 1) {
      yesterday_stats = (HealthServiceStats) {
        .max = value,
        .min = value,
        .avg = value,
        .sum = value,
        .count = 1,
      };
    }

    // Update stats
    if (day_in_week == yesterday_day_in_week) {
      prv_update_stats(&weekly_stats, value);
      PBL_LOG(LOG_LEVEL_DEBUG, "Updating weekly stats with %"PRIi32": sum: %"PRIi32", "
               "avg: %"PRIi32" ", value, weekly_stats.sum, weekly_stats.avg);
    }
    if (day_in_week == Sunday || day_in_week == Saturday) {
      prv_update_stats(&weekend_stats, value);
    } else {
      prv_update_stats(&weekday_stats, value);
    }
    prv_update_stats(&daily_stats, value);
  }


  // ----------------------------------------
  // Compute each combination of agg/stat and compare to expected
  for (HealthAggregation agg = HealthAggregationSum; agg <= HealthAggregationMax; agg++) {
    for (HealthServiceTimeScope scope = HealthServiceTimeScopeOnce;
         scope <= HealthServiceTimeScopeDaily; scope++) {

      // Figure out the expected value
      HealthServiceStats *stats = NULL;
      char *scope_str;
      switch (scope) {
        case HealthServiceTimeScopeOnce:
          stats = &yesterday_stats;
          scope_str = "once";
          break;
        case HealthServiceTimeScopeWeekly:
          stats = &weekly_stats;
          scope_str = "weekly";
          break;
        case HealthServiceTimeScopeDailyWeekdayOrWeekend:
          stats = yesterday_was_weekend ? &weekend_stats : &weekday_stats;
          scope_str = "weekday/weekend";
          break;
        case HealthServiceTimeScopeDaily:
          stats = &daily_stats;
          scope_str = "daily";
          break;
      }

      HealthValue exp_value = 0;
      char *agg_str;
      switch (agg) {
        case HealthAggregationSum:
          exp_value = 0;    // error case
          agg_str = "sum";
          break;
        case HealthAggregationAvg:
          exp_value = stats->avg;
          agg_str = "avg";
          break;
        case HealthAggregationMin:
          exp_value = stats->min;
          agg_str = "min";
          break;
        case HealthAggregationMax:
          exp_value = stats->max;
          agg_str = "max";
          break;
      }

      // Get the value. Since we are computing min, max, avg and we only store 1 value per day
      // in our history, passing in a time range less than a day should produce the same result
      // as passing in a full day
      time_t time_start = time_util_get_midnight_of(now) - SECONDS_PER_DAY;
      time_t time_end = time_start + 12 * SECONDS_PER_HOUR;  // partial day

      // Heart rate should return error if asked for min/max with scope since we can't
      // compute that.
      if (scope != HealthServiceTimeScopeOnce
        && (agg == HealthAggregationMax || agg == HealthAggregationMin)) {
        exp_value = 0;
      }

      HealthValue result;
      result = health_service_aggregate_averaged(HealthMetricHeartRateBPM,
                                                 time_start, time_end, agg, scope);
      PBL_LOG(LOG_LEVEL_INFO, "Testing %-16s %-16s exp_value: %5"PRIi32", act_value: "
              "%5"PRIi32" " , scope_str, agg_str, exp_value, result);

      if (scope != HealthServiceTimeScopeOnce) {
        // Only test scoped results. Non-scoped results for heart rate are computed using
        // minute data and verified in the test_health__heart_rate_scope_once()
        cl_assert_equal_i(result, exp_value);
      }
    }
  }
}

// --------------------------------------------------------------------------------------
// Test calls to compute heart rate stats with scope once. This ends up being implemented using
// the minute history
void test_health__heart_rate_scope_once(void) {
  const time_t now = rtc_get_time();

  const time_t time_start = now - 2 * SECONDS_PER_HOUR;
  const time_t time_end = now;

  // ----------------------------------------------------------------
  // Put in our minute history
  HealthServiceCache *cache = NULL;
  unsigned num_minutes_per_call = ARRAY_LENGTH(cache->minute_data);
  s_sys_activity_get_minute_history_values = (sys_activity_get_minute_history_values) {
    .out[0] = {
      .num_records = num_minutes_per_call,
      .result = true,
      .utc_start = time_start,
    },
    .out[1] = {
      .num_records = num_minutes_per_call,
      .result = true,
      .utc_start = time_start,
    },
  };
  int32_t min_value = INT32_MAX;
  int32_t max_value = INT32_MIN;
  int32_t sum = 0;
  int32_t count = 0;
  uint8_t value = 50;
  for (unsigned i = 0; i < num_minutes_per_call; i++) {
    min_value = MIN(min_value, value);
    max_value = MAX(max_value, value);
    sum += value;
    count++;
    s_sys_activity_get_minute_history_values.out[0].records[i].heart_rate_bpm = value;
    value++;
    if (value > 200) {
      value = 50;
    }
  }
  for (unsigned i = 0; i < num_minutes_per_call; i++) {
    min_value = MIN(min_value, value);
    max_value = MAX(max_value, value);
    sum += value;
    count++;
    s_sys_activity_get_minute_history_values.out[1].records[i].heart_rate_bpm = value;
    value++;
    if (value > 200) {
      value = 50;
    }
  }
  int32_t avg_value = ROUND(sum, count);

  // Test each aggregation
  for (HealthAggregation agg = HealthAggregationAvg; agg <= HealthAggregationMax; agg++) {

    s_sys_activity_get_minute_history_values.stage = 0;
    if (agg == HealthAggregationMin) {
      PBL_LOG(LOG_LEVEL_DEBUG, "foo");
    }
    HealthValue result = health_service_aggregate_averaged(HealthMetricHeartRateBPM,
                                                           time_start, time_end,
                                                           agg, HealthServiceTimeScopeOnce);

    cl_assert_equal_i(s_sys_activity_get_minute_history_values.in[0].utc_start, time_start);
    cl_assert_equal_i(s_sys_activity_get_minute_history_values.in[0].num_records,
                      num_minutes_per_call);
    cl_assert_equal_i(s_sys_activity_get_minute_history_values.in[1].utc_start,
                      time_start + SECONDS_PER_HOUR);
    cl_assert_equal_i(s_sys_activity_get_minute_history_values.in[1].num_records,
                      num_minutes_per_call);

    if (agg == HealthAggregationAvg) {
      cl_assert_equal_i(result, avg_value);
    } else if (agg == HealthAggregationMin) {
      cl_assert_equal_i(result, min_value);
    } else if (agg == HealthAggregationMax) {
      cl_assert_equal_i(result, max_value);
    } else {
      cl_assert(false);
    }
  }

}


// --------------------------------------------------------------------------------------
int s_metric_alert_count;
static void prv_test_event_handler(HealthEventType event, void *context) {
  if (event == HealthEventMetricAlert) {
    s_metric_alert_count++;
  }
}

// --------------------------------------------------------------------------------------
// Test the health metric alert generation
void test_health__metric_alert_generation(void) {
  health_service_events_subscribe(prv_test_event_handler, NULL);
  s_sys_activity_get_metric_values.out.result = true;

  PebbleEvent event = {
    .type = PEBBLE_HEALTH_SERVICE_EVENT,
    .health_event.type = HealthEventHeartRateUpdate,
  };

  // Process some events with various heart rates, should not get an alert
  s_metric_alert_count = 0;
  for (int i = 50; i < 60; i++) {
    s_sys_activity_get_metric_values.out.history[0] = i;
    prv_health_event_handler(&event, NULL);
  }
  // Should not get any metric alerts because none registered
  cl_assert_equal_i(s_metric_alert_count, 0);


  // Create an alert for heart rate 65
  HealthMetricAlert *alert = health_service_register_metric_alert(HealthMetricHeartRateBPM, 65);
  for (int i = 60; i < 70; i++) {
    s_sys_activity_get_metric_values.out.history[0] = i;
    prv_health_event_handler(&event, NULL);
  }
  // One alert on the way up
  cl_assert_equal_i(s_metric_alert_count, 1);

  for (int i = 70; i >= 60; i--) {
    s_sys_activity_get_metric_values.out.history[0] = i;
    PebbleEvent event = {
      .type = PEBBLE_HEALTH_SERVICE_EVENT,
      .health_event.type = HealthEventHeartRateUpdate,
    };
    prv_health_event_handler(&event, NULL);
  }
  // One alert on the way down
  cl_assert_equal_i(s_metric_alert_count, 2);


  // Remove the alert
  s_metric_alert_count = 0;
  health_service_cancel_metric_alert(alert);
  for (int i = 60; i < 70; i++) {
    s_sys_activity_get_metric_values.out.history[0] = i;
    prv_health_event_handler(&event, NULL);
  }
  // Should not get an alert
  cl_assert_equal_i(s_metric_alert_count, 0);
}

// --------------------------------------------------------------------------------------
// Test the health metric alert registration
void test_health__metric_alert_registration(void) {
  health_service_events_subscribe(prv_test_event_handler, NULL);

  time_t now = rtc_get_time();
  HealthServiceAccessibilityMask accessible = health_service_metric_aggregate_averaged_accessible(
    HealthMetricHeartRateBPM, now, now, HealthAggregationAvg, HealthServiceTimeScopeOnce);
  cl_assert(accessible & HealthServiceAccessibilityMaskAvailable);

  // Create an alert for heart rate
  HealthMetricAlert *alert = health_service_register_metric_alert(HealthMetricHeartRateBPM, 65);
  cl_assert(alert != NULL);

  // If we try to register another for heart rate, it should fail
  HealthMetricAlert *fail_alert = health_service_register_metric_alert(HealthMetricHeartRateBPM,
                                                                       65);
  cl_assert(fail_alert == NULL);

  // Cancel the original
  health_service_cancel_metric_alert(alert);

  // Should be able to register another now
  alert = health_service_register_metric_alert(HealthMetricHeartRateBPM, 65);
  cl_assert(alert != NULL);
}

