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

#include "applib/accel_service.h"
#include "applib/data_logging.h"
#include "applib/health_service.h"
#include "applib/health_service_private.h"
#include "drivers/rtc.h"
#include "drivers/vibe.h"
#include "kernel/events.h"
#include "services/common/hrm/hrm_manager_private.h"
#include "services/normal/activity/activity.h"
#include "services/normal/activity/activity_algorithm.h"
#include "services/normal/activity/activity_private.h"
#include "services/normal/activity/kraepelin/activity_algorithm_kraepelin.h"
#include "services/normal/data_logging/data_logging_service.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/protobuf_log/protobuf_log.h"
#include "shell/prefs.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/size.h"

#include <sys/stat.h>

#include "clar.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif


// Stubs
#include "stubs_activity_insights.h"
#include "stubs_alarm.h"
#include "stubs_app_manager.h"
#include "stubs_analytics.h"
#include "stubs_app_install_manager.h"
#include "stubs_battery.h"
#include "stubs_freertos.h"
#include "stubs_health_db.h"
#include "stubs_hexdump.h"
#include "stubs_i18n.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_process_info.h"
#include "stubs_prompt.h"
#include "stubs_sleep.h"
#include "stubs_system_theme.h"
#include "stubs_task_watchdog.h"
#include "stubs_worker_manager.h"
#include "stubs_workout_service.h"

// Fakes
#include "fake_accel_service.h"
#include "fake_cron.h"
#include "fake_events.h"
#include "fake_new_timer.h"
#include "fake_pbl_std.h"
#include "fake_rtc.h"
#include "fake_spi_flash.h"
#include "fake_system_task.h"


// We start time out at 5pm on Jan 1, 2015 for all of these tests
static const  struct tm s_init_time_tm = {
    // Thursday, Jan 1, 2015, 5:pm
    .tm_hour = 17,
    .tm_mday = 1,
    .tm_mon = 0,
    .tm_year = 115
  };


#define ACTIVITY_FIXTURE_PATH "activity"

// The expected resting kcalories is determined empirically from a known good commit and
// is based on the current time of day and the user's weight, age etc.
const int s_exp_5pm_resting_kcalories = 1031;
const int s_exp_full_day_resting_kcalories = 1455;

// Stub for health tracking disabled UI
void health_tracking_ui_feature_show_disabled(void) { }
void health_tracking_ui_app_show_disabled(void) { }

// These are declared as T_STATIC in activity.c
void prv_hrm_subscription_cb(PebbleHRMEvent *hrm_event, void *context);
void prv_minute_system_task_cb(void *data);

bool mfg_info_is_hrm_present(void) {
  return true;
}

void hrm_manager_handle_prefs_changed(void) { }

#define ASSERT_EQUAL_I(i1,i2,file,line) \
        clar__assert_equal_i((i1),(i2),file,line,#i1 " != " #i2, 1)

// ======================================================================================
// Misc stubs
static HealthServiceState s_health_service;

HealthServiceState *app_state_get_health_service_state(void) {
  return &s_health_service;
}

HealthServiceState *worker_state_get_health_service_state(void) {
  cl_fail("should never be called");
  return NULL;
}

void event_service_client_subscribe(EventServiceInfo * service_info) {}
void event_service_client_unsubscribe(EventServiceInfo * service_info) {}

void sys_send_pebble_event_to_kernel(PebbleEvent* event) {}


static UnitsDistance s_units_distance_result;

UnitsDistance sys_shell_prefs_get_units_distance(void) {
  return s_units_distance_result;
}

int32_t vibes_get_vibe_strength(void) {
  return VIBE_STRENGTH_OFF;
}

HRMSessionRef s_hrm_next_session_ref = 1;
static uint32_t s_hrm_manager_update_interval;
static int s_hrm_manager_num_update_interval_changes;
static uint16_t s_hrm_manager_expire_s;
HRMSessionRef hrm_manager_subscribe_with_callback(AppInstallId app_id, uint32_t update_interval_s,
                                         uint16_t expire_s, HRMFeature features,
                                         HRMSubscriberCallback callback, void *context) {
  s_hrm_manager_update_interval = update_interval_s;
  s_hrm_manager_expire_s = expire_s;
  return s_hrm_next_session_ref++;
}

bool sys_hrm_manager_unsubscribe(HRMSessionRef session) {
  cl_assert(session < s_hrm_next_session_ref);
  return true;
}

bool sys_hrm_manager_set_update_interval(HRMSessionRef session, uint32_t update_interval_s,
                                         uint16_t expire_s) {
  cl_assert(session < s_hrm_next_session_ref);
  s_hrm_manager_update_interval = update_interval_s;
  s_hrm_manager_expire_s = expire_s;
  s_hrm_manager_num_update_interval_changes++;
  return true;
}

HRMSessionRef sys_hrm_manager_app_subscribe(AppInstallId app_id, uint32_t update_interval_s,
                                            uint16_t expire_s, HRMFeature features) {
  return HRM_INVALID_SESSION_REF;
}

HRMSessionRef sys_hrm_manager_get_app_subscription(AppInstallId app_id) {
  return HRM_INVALID_SESSION_REF;
}

bool sys_hrm_manager_get_subscription_info(HRMSessionRef session, AppInstallId *app_id,
                                           uint32_t *update_interval_s, uint16_t *expire_s,
                                           HRMFeature *features) {
  return false;
}

AppInstallId app_get_app_id(void) {
  return 1;
}


// ======================================================================================
// Queue stubs, to support the semaphore that activity.c uses to block on a kernel BG callback
#define QUEUE_HANDLE  ((QueueHandle_t)0x11)
int s_queue_value = 0;

signed portBASE_TYPE xQueueGenericReceive(QueueHandle_t xQueue, void * const pvBuffer,
                                          TickType_t xTicksToWait, portBASE_TYPE xJustPeeking) {
  while (s_queue_value <= 0) {
    fake_system_task_callbacks_invoke_pending();
  }
  s_queue_value--;
  return true;
}

signed portBASE_TYPE xQueueGenericSend(QueueHandle_t xQueue, const void * const pvItemToQueue,
                                       TickType_t xTicksToWait, portBASE_TYPE xCopyPosition) {
  PBL_ASSERTN(xQueue == QUEUE_HANDLE);
  s_queue_value++;
  return true;
}

QueueHandle_t xQueueGenericCreate(unsigned portBASE_TYPE uxQueueLength,
                                 unsigned portBASE_TYPE uxItemSize, unsigned char ucQueueType) {
  return QUEUE_HANDLE;
}



// =============================================================================================
// Data logging stubs
typedef enum {
  DataLoggingSession_AccelSamples = 1,
  DataLoggingSession_ActivitySessions = 2,
} DataLoggingSession_ID;


// Logged items
static bool s_dls_accel_samples_created;
static int s_num_dls_accel_records;
static ActivityRawSamplesRecord s_dls_accel_records[100];

static bool s_dls_activity_sessions_created;
static int s_num_dls_activity_records;
static ActivitySessionDataLoggingRecord s_dls_activity_records[100];

static void prv_reset_captured_dls_data(void) {
  s_num_dls_accel_records = 0;
  s_num_dls_activity_records = 0;
}

DataLoggingResult dls_log(DataLoggingSession *logging_session, const void *data,
                          uint32_t num_items) {
  if (logging_session == (DataLoggingSession *)DataLoggingSession_AccelSamples) {
    cl_assert(s_dls_accel_samples_created);

    ActivityRawSamplesRecord *records = (ActivityRawSamplesRecord *)data;
    for (int i = 0; i < num_items; i++) {
      cl_assert(s_num_dls_accel_records < ARRAY_LENGTH(s_dls_accel_records));
      s_dls_accel_records[s_num_dls_accel_records++] = records[i];
    }

  } else if (logging_session == (DataLoggingSession *) DataLoggingSession_ActivitySessions) {
    cl_assert(s_dls_activity_sessions_created);

    ActivitySessionDataLoggingRecord  *records = (ActivitySessionDataLoggingRecord *)data;
    for (int i = 0; i < num_items; i++) {
      if (s_num_dls_activity_records >= 100) {
        cl_assert(false);
      }
      cl_assert(s_num_dls_activity_records < ARRAY_LENGTH(s_dls_activity_records));
      s_dls_activity_records[s_num_dls_activity_records++] = records[i];
    }

  } else {
    return DATA_LOGGING_INVALID_PARAMS;
  }

  return DATA_LOGGING_SUCCESS;
}

DataLoggingSession *dls_create(uint32_t tag, DataLoggingItemType item_type, uint16_t item_size,
                               bool buffered, bool resume, const Uuid *uuid) {
  if (tag == DlsSystemTagActivityAccelSamples) {
    s_dls_accel_samples_created = true;
    cl_assert_equal_i(item_size, sizeof(ActivityRawSamplesRecord));
    return (DataLoggingSession *)DataLoggingSession_AccelSamples;

  } else if (tag == DlsSystemTagActivitySession) {
    s_dls_activity_sessions_created = true;
    cl_assert_equal_i(item_size, sizeof(ActivitySessionDataLoggingRecord));
    return (DataLoggingSession *) DataLoggingSession_ActivitySessions;

  } else {
    return NULL;
  }
}

void dls_finish(DataLoggingSession *logging_session) {
  if (logging_session == (DataLoggingSession *)DataLoggingSession_AccelSamples) {
    s_dls_accel_samples_created = false;
  } else if (logging_session == (DataLoggingSession *) DataLoggingSession_ActivitySessions) {
    s_dls_activity_sessions_created = false;
  } else {
    cl_assert(false);
  }
}


// =================================================================================
// Measurement logging stubs
ProtobufLogRef protobuf_log_hr_create(void) {
  return (ProtobufLogRef)1;
}

bool protobuf_log_session_delete(ProtobufLogRef session) {
  return true;
}

bool protobuf_log_hr_add_sample(ProtobufLogRef ref, time_t now_utc, uint8_t bpm,
                               HRMQuality quality) {
  return true;
}


// =============================================================================================
// Assertion utilities
// --------------------------------------------------------------------------------------
static void prv_assert_equal_metric_history(ActivityMetric metric,
                                            const uint32_t expected[ACTIVITY_HISTORY_DAYS],
                                            char* file, int line) {
  int32_t actual[ACTIVITY_HISTORY_DAYS];
  activity_get_metric(metric, ACTIVITY_HISTORY_DAYS, actual);
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++) {
    ASSERT_EQUAL_I(actual[i], expected[i], file, line);
  }
}

#define ASSERT_EQUAL_METRIC_HISTORY(metric, expected) \
        prv_assert_equal_metric_history((metric), (expected), __FILE__, __LINE__)


static void prv_assert_dls_activity_record_present(ActivitySessionDataLoggingRecord *record,
                                                   char *file, int line) {
  for (int i = 0; i < s_num_dls_activity_records; i++) {
    if (!memcmp(record, &s_dls_activity_records[i], sizeof(*record))) {
      return;
    }
  }
  printf("\nFound records:");
  for (int i = 0; i < s_num_dls_activity_records; i++) {
    printf("\ntype: %d, start_utc: %"PRIu32", elapsed: %"PRIu32", utc_to_local: %"PRIu32" ",
           (int)s_dls_activity_records[i].activity, (uint32_t)s_dls_activity_records[i].start_utc,
           s_dls_activity_records[i].elapsed_sec, s_dls_activity_records[i].utc_to_local);
  }
  printf("\nLooking for: type: %d, start_utc: %"PRIu32", elapsed: %"PRIu32", "
           "utc_to_local: %"PRIu32" ", (int)record->activity, (uint32_t)record->start_utc,
         record->elapsed_sec, record->utc_to_local);
  clar__assert(false, file, line, "Missing activity record", "", true);
}

#define ASSERT_ACTIVITY_DLS_RECORD_PRESENT(record) \
        prv_assert_dls_activity_record_present((record), __FILE__, __LINE__)


// Assert that given number of activity sessions are present
static void prv_assert_num_activities(uint32_t num_expected, char *file, int line) {
  ActivitySession sessions[ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT];
  uint32_t num_sessions = ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT;
  activity_get_sessions(&num_sessions, sessions);
  if (num_sessions != num_expected) {
    printf("Expected %"PRIu32" activities, but found %"PRIu32".\n", num_expected, num_sessions);
  }
  clar__assert(num_sessions == num_expected, file, line, "wrong number of activities", "", true);
}

// Assert that a particular step activity session is present in the sessions list
static void prv_assert_step_activity_present(ActivitySession *exp_session, char *file, int line) {
  ActivitySession sessions[ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT];
  uint32_t num_sessions = ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT;
  activity_get_sessions(&num_sessions, sessions);
  for (int i = 0; i < num_sessions; i++) {
    if (sessions[i].type == exp_session->type
      && sessions[i].start_utc == exp_session->start_utc
      && sessions[i].length_min == exp_session->length_min
      && sessions[i].step_data.active_kcalories == exp_session->step_data.active_kcalories
      && sessions[i].step_data.resting_kcalories == exp_session->step_data.resting_kcalories
      && sessions[i].step_data.distance_meters == exp_session->step_data.distance_meters
      && sessions[i].step_data.steps == exp_session->step_data.steps) {
      return;
    }
  }
  printf("\nFound activities:");
  for (int i = 0; i < num_sessions; i++) {
    printf("\nFound:       type: %d, start_utc: %d, len: %"PRIu16", steps: %"PRIu16", "
             "rest_cal: %"PRIu32", active_cal: %"PRIu32", dist: %"PRIu32" ",
           (int)sessions[i].type, (int)sessions[i].start_utc, sessions[i].length_min,
           sessions[i].step_data.steps, sessions[i].step_data.resting_kcalories,
           sessions[i].step_data.active_kcalories,
           sessions[i].step_data.distance_meters);
  }
  printf("\nLooking for: type: %d, start_utc: %d, len: %"PRIu16", steps: %"PRIu16", "
           "rest_cal: %"PRIu32", active_cal: %"PRIu32", dist: %"PRIu32" ",
         (int)exp_session->type, (int)exp_session->start_utc,
         exp_session->length_min, exp_session->step_data.steps,
         exp_session->step_data.resting_kcalories,
         exp_session->step_data.active_kcalories, exp_session->step_data.distance_meters);
  clar__assert(false, file, line, "Missing activity record", "", true);
}

// Assert that a particular sleep activity session is present in the sessions list
static void prv_assert_sleep_activity_present(ActivitySession *exp_session, char *file, int line) {
  ActivitySession sessions[ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT];
  uint32_t num_sessions = ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT;
  activity_get_sessions(&num_sessions, sessions);
  for (int i = 0; i < num_sessions; i++) {
    if (sessions[i].type == exp_session->type
        && sessions[i].start_utc == exp_session->start_utc
        && sessions[i].length_min == exp_session->length_min) {
      return;
    }
  }
  printf("\nFound activities:");
  for (int i = 0; i < num_sessions; i++) {
    printf("\nFound:       type: %d, start_utc: %d, len: %"PRIu16" ",
           (int)sessions[i].type, (int)sessions[i].start_utc, sessions[i].length_min);
  }
  printf("\nLooking for: type: %d, start_utc: %d, len: %"PRIu16" ",
         (int)exp_session->type, (int)exp_session->start_utc,
         exp_session->length_min);
  clar__assert(false, file, line, "Missing sleep activity record", "", true);
}

#define ASSERT_STEP_ACTIVITY_SESSION_PRESENT(session) \
        prv_assert_step_activity_present((session), __FILE__, __LINE__)

#define ASSERT_SLEEP_ACTIVITY_SESSION_PRESENT(session) \
        prv_assert_sleep_activity_present((session), __FILE__, __LINE__)

#define ASSERT_NUM_ACTIVITY_SESSIONS(num_sessions) \
        prv_assert_num_activities((num_sessions), __FILE__, __LINE__)



// =============================================================================================
// Activity algorithm stub
// For each accel sample that is fed in, it updates the metrics as follows:
//  x: increment step count by this much
//  y: sleep state
//

#define ALGORITHM_SAMPLING_RATE ACCEL_SAMPLING_25HZ
#define TEST_ACTIVITY_MAX_SESSIONS 24
typedef struct {
  // Captured sessions
  ActivitySession sessions[TEST_ACTIVITY_MAX_SESSIONS];
  int num_sessions_created;
  time_t last_captured_utc;

  int sleep_current_container_idx;    // >=0 if we have a container in progress
  ActivitySleepState sleep_state;   // Our current sleep state
} AlgorithmStateMinuteData;

typedef struct {
  uint16_t steps;

  // Captured sessions
  AlgorithmStateMinuteData minute_data;

  // Rate info
  uint16_t rate_last_steps;
  time_t rate_last_update_time;
  uint16_t rate_steps;
  uint32_t rate_elapsed_ms;

  time_t last_sleep_utc;

  uint8_t orientation;

} AlgorithmState;
static AlgorithmState s_test_alg_state;


bool activity_algorithm_init(AccelSamplingRate *sampling_rate) {
  *sampling_rate = ALGORITHM_SAMPLING_RATE;
  AlgorithmStateMinuteData minute_data = s_test_alg_state.minute_data;
  // Preserve the minute data from the last boot
  s_test_alg_state = (AlgorithmState) {
    .minute_data = minute_data,
    .rate_last_update_time = rtc_get_time(),
  };

  return true;
}

// Call from unit tests to clear out the "minute data" that might have been left over
// from last time
static void prv_activity_algorithm_erase_minute_data(void) {
  s_test_alg_state.minute_data = (AlgorithmStateMinuteData){ };
  s_test_alg_state.minute_data.sleep_current_container_idx = -1;
  s_test_alg_state.minute_data.sleep_state = ActivitySleepStateAwake;
}

void activity_algorithm_early_deinit(void) {
}

bool activity_algorithm_deinit(void) {
  return true;
}

void activity_algorithm_handle_accel(AccelRawData *data, uint32_t num_samples, uint64_t timestamp) {
  // For testing purposes, we'll use the x movement as the steps and y as the sleep state
  ActivitySleepState prior_state = s_test_alg_state.minute_data.sleep_state;
  time_t now_secs = rtc_get_time();
  s_test_alg_state.minute_data.last_captured_utc = now_secs;

  for (int i = 0; i < num_samples; i++) {
    s_test_alg_state.steps += data[i].x;
    s_test_alg_state.minute_data.sleep_state = data[i].y;

    // Update the length of the current sleep container if we have one
    if (s_test_alg_state.minute_data.sleep_current_container_idx >= 0) {
      cl_assert(prior_state != ActivitySleepStateAwake);
      ActivitySession *session = &s_test_alg_state.minute_data.sessions
                                      [s_test_alg_state.minute_data.sleep_current_container_idx];
      session->length_min = ROUND(now_secs - session->start_utc, SECONDS_PER_MINUTE);
      // Inform the activity service of the new state
      activity_sessions_prv_add_activity_session(session);
    }
    // If we were in restful sleep, update that session as well
    if (prior_state == ActivitySleepStateRestfulSleep) {
      cl_assert(s_test_alg_state.minute_data.num_sessions_created > 0);
      ActivitySession *session = &s_test_alg_state.minute_data.sessions
            [s_test_alg_state.minute_data.num_sessions_created - 1];
      session->length_min = ROUND(now_secs - session->start_utc, SECONDS_PER_MINUTE);
      // Inform the activity service of the new state
      activity_sessions_prv_add_activity_session(session);
    }

    if (s_test_alg_state.minute_data.sleep_state == prior_state) {
      // No change in state, continue
      continue;
    }

    switch (s_test_alg_state.minute_data.sleep_state) {
      // We are waking --------------------
      case ActivitySleepStateAwake:
        // End the container
        s_test_alg_state.minute_data.sleep_current_container_idx = -1;

        // Send all stored sleep sessions to the activity service now that sleep is over
        for (int i = 0; i < s_test_alg_state.minute_data.num_sessions_created; i++) {
          s_test_alg_state.minute_data.sessions[i].ongoing = false;
          activity_sessions_prv_add_activity_session(&s_test_alg_state.minute_data.sessions[i]);
        }
        s_test_alg_state.minute_data.num_sessions_created = 0;
        break;

      // We are entering light sleep ------------------
      case ActivitySleepStateLightSleep:
        // Start a light sleep session if we were awake before. If we were in restful sleep,
        // we should already have one
        if (prior_state == ActivitySleepStateAwake) {
          cl_assert(s_test_alg_state.minute_data.num_sessions_created < TEST_ACTIVITY_MAX_SESSIONS);
          cl_assert(s_test_alg_state.minute_data.sleep_current_container_idx < 0);
          s_test_alg_state.minute_data.sleep_current_container_idx =
              s_test_alg_state.minute_data.num_sessions_created;
          s_test_alg_state.minute_data.sessions[s_test_alg_state.minute_data.num_sessions_created++]
            = (ActivitySession) {
            .type = ActivitySessionType_Sleep,
            .start_utc = now_secs,
            .length_min = 0,
            .ongoing = true,
          };
        } else {
          // We were in restful sleep before, we should already have a container
          cl_assert(s_test_alg_state.minute_data.sleep_current_container_idx >= 0);
        }
        break;

      // We are entering restful sleep ------------------
      case ActivitySleepStateRestfulSleep:
        // Start a container session if we don't have already
        if (s_test_alg_state.minute_data.sleep_current_container_idx < 0) {
          cl_assert(s_test_alg_state.minute_data.num_sessions_created < TEST_ACTIVITY_MAX_SESSIONS);
          s_test_alg_state.minute_data.sleep_current_container_idx =
              s_test_alg_state.minute_data.num_sessions_created;
          s_test_alg_state.minute_data.sessions[s_test_alg_state.minute_data.num_sessions_created++]
            = (ActivitySession) {
            .type = ActivitySessionType_Sleep,
            .start_utc = now_secs,
            .length_min = 0,
            .ongoing = true,
          };
        }

        // Start a restful sleep session
        cl_assert(s_test_alg_state.minute_data.num_sessions_created < TEST_ACTIVITY_MAX_SESSIONS);
        s_test_alg_state.minute_data.sessions[s_test_alg_state.minute_data.num_sessions_created++]
          = (ActivitySession) {
          .type = ActivitySessionType_RestfulSleep,
          .start_utc = now_secs,
          .length_min = 0,
          .ongoing = true,
        };
        break;

      case ActivitySleepStateUnknown:
        break;
    }

    prior_state = s_test_alg_state.minute_data.sleep_state;
  }

  // Update the rate info
  // The actual implementation only sends a rate update once every epoch (5 seconds), so
  // emulate that
  if ((now_secs - s_test_alg_state.rate_last_update_time) >= 5) {
    s_test_alg_state.rate_steps = s_test_alg_state.steps - s_test_alg_state.rate_last_steps;
    s_test_alg_state.rate_elapsed_ms = (now_secs - s_test_alg_state.rate_last_update_time)
                                        * MS_PER_SECOND;

    s_test_alg_state.rate_last_update_time = now_secs;
    s_test_alg_state.rate_last_steps = s_test_alg_state.steps;
  }
}

bool activity_algorithm_set_user(uint32_t height_mm, uint32_t weight_g, ActivityGender gender,
                                 uint32_t age_years) {
  return true;
}

bool activity_algorithm_get_steps(uint16_t *steps) {
  *steps = s_test_alg_state.steps;
  return true;
}

bool activity_algorithm_get_step_rate(uint16_t *steps, uint32_t *elapsed_ms, time_t *end_sec) {
  *steps = s_test_alg_state.rate_steps;
  *elapsed_ms = s_test_alg_state.rate_elapsed_ms;
  *end_sec = s_test_alg_state.rate_last_update_time;
  return true;
}

bool activity_algorithm_metrics_changed_notification(void) {
  s_test_alg_state.steps = 0;
  s_test_alg_state.rate_last_steps = 0;
  s_test_alg_state.rate_last_update_time = rtc_get_time();
  return true;
}

bool activity_algorithm_get_sleep_sessions(time_t sleep_earliest_end_utc,
                                           time_t *last_processed_utc) {
  *last_processed_utc = s_test_alg_state.minute_data.last_captured_utc;
  for (uint32_t i = 0; i < s_test_alg_state.minute_data.num_sessions_created; i++) {
    ActivitySession *session = &s_test_alg_state.minute_data.sessions[i];
    int start_minute = time_util_get_minute_of_day(session->start_utc);
    ACTIVITY_LOG_DEBUG("Found session %d: start_min: %d, len_min: %"PRIu16" ", session->type,
                       start_minute, session->length_min);
    if (!activity_sessions_prv_is_sleep_activity(session->type)) {
      continue;
    }
    if (s_test_alg_state.minute_data.sessions[i].start_utc
          + (s_test_alg_state.minute_data.sessions[i].length_min * SECONDS_PER_MINUTE)
        < sleep_earliest_end_utc) {
      continue;
    }
    ACTIVITY_LOG_DEBUG("Returning session %d: start_min: %d, len_min: %"PRIu16" ", session->type,
                       start_minute, session->length_min);
    activity_sessions_prv_add_activity_session(&s_test_alg_state.minute_data.sessions[i]);
  }
  return true;
}

void activity_algorithm_post_process_sleep_sessions(uint16_t num_input_sessions,
                                                    ActivitySession *sessions) {
}

void activity_algorithm_minute_handler(time_t utc_sec, AlgMinuteRecord *record_out) {
  s_test_alg_state.last_sleep_utc = utc_sec;

  record_out->data.base.orientation = s_test_alg_state.orientation;
}

bool activity_algorithm_dump_minute_data_to_log(void) {
  return false;
}

bool activity_algorithm_minute_file_info(bool compact_first, uint32_t *num_records,
                                         uint32_t *data_bytes, uint32_t *minutes) {
  *num_records = 0;
  *data_bytes = 0;
  *minutes = 0;
  return true;
}

bool activity_algorithm_test_fill_minute_file(void) {
  return true;
}

// We simulate the activity_algorithm_get_minute_history() call to return data that reflects
// that we record chunks of ALG_MINUTES_PER_RECORD minutes at a time. If we don't ask on a
// ALG_MINUTES_PER_RECORD minute boundary, we will have up to ALG_MINUTES_PER_RECORD minutes
// of data still unavailable before the current time. The data that we do return, we will set
// the number of steps equal to (% 255) of the timestamp of that minute.
bool activity_algorithm_get_minute_history(HealthMinuteData *minute_data, uint32_t *num_records,
                                           time_t *utc_start) {
  // Get the current time
  time_t now = rtc_get_time();

  // Get the minute index
  uint32_t minute_idx = now / SECONDS_PER_MINUTE;

  // Compute the timestamp of the end of the last record we would have available
  uint32_t last_minute_avail = minute_idx - (minute_idx % ALG_MINUTES_PER_FILE_RECORD);
  time_t last_second_available = last_minute_avail * SECONDS_PER_MINUTE;

  // Return the data now
  uint32_t num_records_requested = *num_records;

  // Start on next minute boundary
  *utc_start = ((*utc_start + SECONDS_PER_MINUTE - 1) / SECONDS_PER_MINUTE) * SECONDS_PER_MINUTE;

  int num_records_returned;
  time_t record_start_time = *utc_start;
  for (num_records_returned = 0; num_records_returned < num_records_requested;
       num_records_returned++, record_start_time += SECONDS_PER_MINUTE) {
    if (record_start_time + SECONDS_PER_MINUTE > last_second_available) {
      // This record not available yet.
      break;
    }

    minute_data[num_records_returned] = (HealthMinuteData) {
      .steps = record_start_time % 255,
    };
  }

  *num_records = num_records_returned;
  return true;
}

time_t activity_algorithm_get_last_sleep_utc(void) {
  return s_test_alg_state.last_sleep_utc;
}

bool activity_algorithm_test_send_fake_minute_data_dls_record(void) {
  return true;
}


// =========================================================================================
// Tests

// ---------------------------------------------------------------------------------------
// Feed in X seconds of data with the given statistics.
// The fake algorithm we plug in assumes that each accel sample contains the following:
// .x : the number of steps to increment by (either 0 or 1)
// .y : the current sleep state
// .z : 0
static void prv_feed_canned_accel_data(uint32_t num_sec, uint32_t steps_per_minute,
                                ActivitySleepState sleep_state) {
  uint32_t num_steps = (steps_per_minute * num_sec + 30) / 60;
  uint32_t num_samples = num_sec * ALGORITHM_SAMPLING_RATE;
  uint32_t samples_per_step = 0;
  if (num_steps > 0) {
    samples_per_step = num_samples / num_steps;
  }
  int need_step_ctr = samples_per_step;

  time_t utc_secs;
  uint16_t ms;
  rtc_get_time_ms(&utc_secs, &ms);
  uint64_t start_ms = utc_secs * 1000 + ms;
  uint64_t ms_per_sample = 1000 / ALGORITHM_SAMPLING_RATE;

  for (int i = 0; i < num_samples; ) {
    AccelData accel_data[ALGORITHM_SAMPLING_RATE];

    for (int j = 0; j < ALGORITHM_SAMPLING_RATE; j++, i++) {
      need_step_ctr -= 1;
      accel_data[j] = (AccelData) {
        .x = (num_steps > 0) && (need_step_ctr <= 0),
        .y = sleep_state,
        .z = 0,
        .timestamp = start_ms,
      };
      start_ms += ms_per_sample;
      if (need_step_ctr <= 0) {
        need_step_ctr = samples_per_step;
        if (num_steps > 0) {
          num_steps--;
        }
      }
    }

    fake_accel_service_invoke_callbacks(accel_data, ALGORITHM_SAMPLING_RATE);

    // Advance time
    fake_rtc_increment_time(1);
    fake_rtc_increment_ticks(configTICK_RATE_HZ);

    // Is it time to call the minute callback?
    utc_secs += 1;
    if ((utc_secs % 60) == 0) {
      fake_cron_job_fire();
      fake_system_task_callbacks_invoke_pending();
    }

  }
  PBL_ASSERTN(num_steps == 0);
}


// ---------------------------------------------------------------------------------------
// Feed in X seconds of raw data
static void prv_feed_raw_accel_data(AccelRawData *samples, uint32_t num_samples) {
  time_t utc_secs;
  uint16_t ms;
  rtc_get_time_ms(&utc_secs, &ms);
  uint64_t start_ms = utc_secs * 1000 + ms;

  for (int i = 0; i < num_samples; ) {
    AccelData accel_data[ALGORITHM_SAMPLING_RATE];

    int j;
    for (j = 0; j < ALGORITHM_SAMPLING_RATE && i < num_samples; j++, i++) {
      accel_data[j] = (AccelData) {
        .x = samples[i].x,
        .y = samples[i].y,
        .z = samples[i].z,
        .did_vibrate = false,
        .timestamp = start_ms
      };
    }

    fake_accel_service_invoke_callbacks(accel_data, j);

    // Advance time
    fake_rtc_increment_time(1);
    fake_rtc_increment_ticks(configTICK_RATE_HZ);

    // Is it time to call the minute callback?
    utc_secs += 1;
    if ((utc_secs % 60) == 0) {
      fake_cron_job_fire();
      fake_system_task_callbacks_invoke_pending();
    }

  }
}


// --------------------------------------------------------------------------------
// Fast forward time, one minute at a time, calling all minute callbacks along the way.
// This does not feed in any accel data
static void prv_advance_by_days(uint32_t num_days) {
  for (int i = 0; i < num_days; i++) {
    // Advance time
    fake_rtc_increment_time(SECONDS_PER_DAY);
    fake_rtc_increment_ticks(configTICK_RATE_HZ * SECONDS_PER_DAY);

    fake_cron_job_fire();
    fake_system_task_callbacks_invoke_pending();
  }
}


// ---------------------------------------------------------------------------------------
// Uncompress data stored in the raw accel DLS records
static void prv_uncompress_captured_data(AccelRawData *data, uint32_t num_samples) {
  for (int i = 0; i < s_num_dls_accel_records; i++) {
    ActivityRawSamplesRecord *record = &s_dls_accel_records[i];

    // Verify the header info
    cl_assert_equal_i(record->version, ACTIVITY_RAW_SAMPLES_VERSION);
    cl_assert_equal_i(record->len, sizeof(ActivityRawSamplesRecord));
    if (i == 0) {
      cl_assert(record->flags & ACTIVITY_RAW_SAMPLE_FLAG_FIRST_RECORD);
    } else {
      cl_assert(!(record->flags & ACTIVITY_RAW_SAMPLE_FLAG_FIRST_RECORD));
    }
    if (i == s_num_dls_accel_records - 1) {
      cl_assert(record->flags & ACTIVITY_RAW_SAMPLE_FLAG_LAST_RECORD);
    } else {
      cl_assert(!(record->flags & ACTIVITY_RAW_SAMPLE_FLAG_LAST_RECORD));
    }


    // Uncompress the entries into samples
    uint32_t num_samples_seen = 0;
    for (int j = 0; j < record->num_entries; j++) {
      uint32_t encoded = record->entries[j];
      uint32_t run_size = ACTIVITY_RAW_SAMPLE_GET_RUN_SIZE(encoded);
      AccelRawData sample = (AccelRawData) {
        .x = ACTIVITY_RAW_SAMPLE_GET_X(encoded),
        .y = ACTIVITY_RAW_SAMPLE_GET_Y(encoded),
        .z = ACTIVITY_RAW_SAMPLE_GET_Z(encoded),
      };
      while (run_size--) {
        cl_assert(num_samples > 0);
        *data++ = sample;
        num_samples--;
        num_samples_seen++;
      }
    }
    cl_assert_equal_i(num_samples_seen, record->num_samples);
  }
  cl_assert_equal_i(num_samples, 0);
}


// ---------------------------------------------------------------------------------------
// Init and enable the activity service
static void prv_activity_init_and_set_enabled(bool enable) {
  activity_init();
  activity_set_enabled(enable);
  fake_system_task_callbacks_invoke_pending();
}



// -----------------------------------------------------------------------------------------
// Fetch sleep sessions using the health_service API
static uint32_t s_health_sessions_count;
static uint32_t s_health_sessions_max;
static ActivitySession *s_health_sessions;
static time_t s_health_sessions_sleep_time;
static time_t s_health_sessions_awake_time;

static bool prv_activity_iterate_cb(HealthActivity activity, time_t time_start, time_t time_end,
                                    void *context) {
  if (s_health_sessions_count >= s_health_sessions_max) {
    return false;
  }

  // Update bed and awake time if appropriate
  if (activity == HealthActivitySleep) {
    if (s_health_sessions_sleep_time == 0) {
      s_health_sessions_sleep_time = time_start;
    }
    if ((s_health_sessions_awake_time == 0) || (time_end > s_health_sessions_awake_time)) {
      s_health_sessions_awake_time = time_end;
    }
  }

  char time_start_text[64];
  struct tm *local_tm = localtime(&time_start);
  strftime(time_start_text, sizeof(time_start_text),  "%F %r", local_tm);

  char time_end_text[64];
  local_tm = localtime(&time_end);
  strftime(time_end_text, sizeof(time_end_text),  "%F %r", local_tm);

  PBL_LOG(LOG_LEVEL_DEBUG, "Got activity: %d %s to %s (%d min)", (int)activity, time_start_text,
          time_end_text, (int)((time_end - time_start) / SECONDS_PER_MINUTE));


  // Save the session info
  ActivitySessionType session_type = ActivitySessionType_Sleep;
  if (activity == HealthActivitySleep) {
    session_type = ActivitySessionType_Sleep;
  } else if (activity == HealthActivityRestfulSleep) {
    session_type = ActivitySessionType_RestfulSleep;
  } else {
    cl_assert(false);
  }

  s_health_sessions[s_health_sessions_count] = (ActivitySession) {
    .type = session_type,
    .start_utc = time_start,
    .length_min = ROUND(time_end - time_start, SECONDS_PER_MINUTE),
    };
  s_health_sessions_count += 1;

  return true;
}

static void prv_sleep_sessions_using_health_service(uint32_t *session_entries,
                                                    ActivitySession *sessions,
                                                    HealthIterationDirection direction) {
  time_t now = rtc_get_time();
  s_health_sessions_count = 0;
  s_health_sessions_max = *session_entries;
  s_health_sessions = sessions;
  s_health_sessions_awake_time = 0;
  s_health_sessions_sleep_time = 0;
  health_service_activities_iterate(HealthActivityMaskAll, now - (2 * SECONDS_PER_DAY), now,
                                    direction, prv_activity_iterate_cb,
                                    NULL);
  PBL_LOG(LOG_LEVEL_DEBUG, "Found %"PRIu32" activities", s_health_sessions_count);
  *session_entries = s_health_sessions_count;
}


static void prv_assert_equal_activity_and_health_sleep_sessions(int exp_num_sessions) {
  // Get the sleep sessions and make sure we get the expected ones
  stub_pebble_tasks_set_current(PebbleTask_App);
  uint32_t session_entries = 24;
  ActivitySession sessions[session_entries];
  activity_get_sessions(&session_entries, sessions);
  cl_assert_equal_i(session_entries, exp_num_sessions);

  // Get the sleep sessions using the health API
  uint32_t health_session_entries = 24;
  ActivitySession health_sessions[session_entries];
  prv_sleep_sessions_using_health_service(&health_session_entries, health_sessions,
                                          HealthIterationDirectionFuture);
  cl_assert_equal_i(health_session_entries, exp_num_sessions);

  for (int i = 0; i < exp_num_sessions; i++) {
    cl_assert_equal_i(sessions[i].type, health_sessions[i].type);
    cl_assert_equal_i(sessions[i].start_utc, health_sessions[i].start_utc);
    cl_assert_equal_i(sessions[i].length_min, health_sessions[i].length_min);
  }
}



// =============================================================================================
// Start of unit tests
void test_activity__initialize(void) {
  TimezoneInfo tz_info = {
    .tm_zone = "UTC",
    .tm_gmtoff = 0,
  };
  time_util_update_timezone(&tz_info);

  struct tm time_tm = s_init_time_tm;
  time_t utc_sec = mktime(&time_tm);
  fake_rtc_init(100 /*initial_ticks*/, utc_sec);
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  pfs_format(false);

  prv_activity_algorithm_erase_minute_data();
  prv_activity_init_and_set_enabled(true);

  // Set default user settings
  activity_prefs_set_height_mm(ACTIVITY_DEFAULT_HEIGHT_MM);
  activity_prefs_set_weight_dag(ACTIVITY_DEFAULT_WEIGHT_DAG);
  activity_prefs_set_gender(ACTIVITY_DEFAULT_GENDER);
  activity_prefs_set_age_years(ACTIVITY_DEFAULT_AGE_YEARS);
}


// ---------------------------------------------------------------------------------------
void test_activity__cleanup(void) {
  activity_stop_tracking();
  fake_system_task_callbacks_invoke_pending();
}


// ---------------------------------------------------------------------------------------
// Test that we correctly initialize the history upon startup based on stored settings
void test_activity__init_history(void) {
  uint32_t exp_resting_kcalories[ACTIVITY_HISTORY_DAYS];
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++) {
    if (i == 0) {
      exp_resting_kcalories[i] = s_exp_5pm_resting_kcalories;
    } else {
      exp_resting_kcalories[i] = s_exp_full_day_resting_kcalories;
    }
  }

  // Should start out with 0 in the history
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricDistanceMeters,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepRestfulSeconds,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricActiveKCalories,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricRestingKCalories, exp_resting_kcalories);


  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // Feed in 100 steps/min over 1 min, 1 minute of deep and 1 minute of light sleep
  prv_feed_canned_accel_data(60, 100, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(60, 0, ActivitySleepStateLightSleep);
  prv_feed_canned_accel_data(60, 0, ActivitySleepStateRestfulSleep);

  // Put in a stepping activity
  time_t day_start = time_util_get_midnight_of(rtc_get_time());
  ActivitySession walk_activity = {
    .start_utc = day_start + 12 * SECONDS_PER_HOUR,
    .length_min = 120,
    .type = ActivitySessionType_Walk,
    .step_data = {
      .steps = 100,
      .active_kcalories = 200,
      .resting_kcalories = 300,
      .distance_meters = 400,
    },
  };
  activity_sessions_prv_add_activity_session(&walk_activity);

  // Capture the resting kcalories now, It is time dependent and we're not sure exactly which time
  // of day it will be saved to storage
  int32_t min_resting_kcalories;
  activity_get_metric(ActivityMetricRestingKCalories, 1, &min_resting_kcalories);

  // Wait long enough for our recompute sleep and periodic update logic to run.
  uint32_t wait_min = MAX(ACTIVITY_SESSION_UPDATE_MIN, ACTIVITY_SETTINGS_UPDATE_MIN);
  prv_feed_canned_accel_data(SECONDS_PER_MINUTE * wait_min, 0, ActivitySleepStateAwake);
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){100, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){2 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepRestfulSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){1 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0, 0}));

  // Check that we have the expected # of activities
  ASSERT_NUM_ACTIVITY_SESSIONS(3);  // 2 sleep sessions + 1 activity sessions
  ASSERT_STEP_ACTIVITY_SESSION_PRESENT(&walk_activity);

  // The expected resting calories
  int minutes_today = 17 * MINUTES_PER_HOUR + 3 + wait_min;
  const int exp_resting_kcalories_now = ROUND(s_exp_full_day_resting_kcalories * minutes_today,
                                              MINUTES_PER_DAY);
  exp_resting_kcalories[0] = exp_resting_kcalories_now;
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricRestingKCalories, exp_resting_kcalories);

  // See what distance we walked
  int32_t exp_distance;
  activity_get_metric(ActivityMetricDistanceMeters, 1, &exp_distance);
  cl_assert(exp_distance > 0);

  // Read the active calories
  int32_t exp_active_kcalories;
  activity_get_metric(ActivityMetricActiveKCalories, 1, &exp_active_kcalories);
  cl_assert(exp_active_kcalories > 0);


  // If we init again, we should start out with the same metrics because we
  // would have retrieved them from settings
  prv_activity_init_and_set_enabled(true);
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){100, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricDistanceMeters,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){exp_distance, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricActiveKCalories,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){exp_active_kcalories, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){2 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepRestfulSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){1 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0, 0}));

  // The actual resting calories must be in the range from min_resting_kcalories to
  // exp_resting_kcalories_now because we don't know at exactly which time settings were saved to
  // storage
  int32_t actual_resting_kcalories[ACTIVITY_HISTORY_DAYS];
  activity_get_metric(ActivityMetricRestingKCalories, ACTIVITY_HISTORY_DAYS,
                      actual_resting_kcalories);
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++) {
    if (i == 0) {
      cl_assert(actual_resting_kcalories[i] >= min_resting_kcalories
                  && actual_resting_kcalories[i] <= exp_resting_kcalories_now);
    } else {
      cl_assert_equal_i(actual_resting_kcalories[i], exp_resting_kcalories[i]);
    }
  }

  // Make sure all of our activities persisted
  ASSERT_NUM_ACTIVITY_SESSIONS(3);  // 2 sleep sessions + 1 activity sessions
  ASSERT_STEP_ACTIVITY_SESSION_PRESENT(&walk_activity);


  // Pretend that 24 hours has elapsed since we saved prefs. This should put both the step and
  // sleep history 1 day behind
  struct tm time_tm = s_init_time_tm;
  time_tm.tm_mday += 1;
  time_t utc_sec = mktime(&time_tm);
  rtc_set_time(utc_sec);
  prv_activity_init_and_set_enabled(true);
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 100, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricDistanceMeters,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, exp_distance, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricActiveKCalories,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, exp_active_kcalories, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 2 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepRestfulSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 1 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0}));

  activity_get_metric(ActivityMetricRestingKCalories, ACTIVITY_HISTORY_DAYS,
                      actual_resting_kcalories);
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++) {
    if (i == 0) {
      cl_assert_equal_i(actual_resting_kcalories[i], s_exp_5pm_resting_kcalories);
    } else if (i == 1) {
      cl_assert(actual_resting_kcalories[i] >= min_resting_kcalories
                  && actual_resting_kcalories[i] <= exp_resting_kcalories_now);
    } else {
      cl_assert_equal_i(actual_resting_kcalories[i], exp_resting_kcalories[i]);
    }
  }
}


// ---------------------------------------------------------------------------------------
// Test that we correctly initialize the setting upon startup based on the stored settings file
void test_activity__settings(void) {
  // Should start out with defaults
  uint16_t height_mm;
  uint16_t weight_dag;
  ActivityGender gender;
  uint8_t age_years;

  height_mm = activity_prefs_get_height_mm();
  cl_assert_equal_i(height_mm, ACTIVITY_DEFAULT_HEIGHT_MM);
  weight_dag = activity_prefs_get_weight_dag();
  cl_assert_equal_i(weight_dag, ACTIVITY_DEFAULT_WEIGHT_DAG);
  gender = activity_prefs_get_gender();
  cl_assert_equal_i(gender, ACTIVITY_DEFAULT_GENDER);
  age_years = activity_prefs_get_age_years();
  cl_assert_equal_i(age_years, ACTIVITY_DEFAULT_AGE_YEARS);

  // Set the settings, re-init, and make sure they stick
  height_mm += 10;
  weight_dag += 11;
  gender = ActivityGenderOther;
  age_years += 10;
  activity_prefs_set_height_mm(height_mm);
  activity_prefs_set_weight_dag(weight_dag);
  activity_prefs_set_gender(gender);
  activity_prefs_set_age_years(age_years);

  // Re-init
  prv_activity_init_and_set_enabled(true);

  // Check settings
  uint32_t value;
  value = activity_prefs_get_height_mm();
  cl_assert_equal_i(height_mm, value);
  value = activity_prefs_get_weight_dag();
  cl_assert_equal_i(weight_dag, value);
  value = activity_prefs_get_gender();
  cl_assert_equal_i(gender, value);
  value = activity_prefs_get_age_years();
  cl_assert_equal_i(age_years, value);

  // Reset settings
  activity_prefs_set_height_mm(ACTIVITY_DEFAULT_HEIGHT_MM);
  activity_prefs_set_weight_dag(ACTIVITY_DEFAULT_WEIGHT_DAG);
  activity_prefs_set_gender(ACTIVITY_DEFAULT_GENDER);
  activity_prefs_set_age_years(ACTIVITY_DEFAULT_AGE_YEARS);
}


// ---------------------------------------------------------------------------------------
// Test that our periodic minute callback correctly detects the midnight rollover
void test_activity__day_rollover(void) {
  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // Feed in 100 steps/min over 1 min, 1 minute of deep and 1 minute of light sleep
  prv_feed_canned_accel_data(60, 100, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(60, 0, ActivitySleepStateLightSleep);
  prv_feed_canned_accel_data(60, 0, ActivitySleepStateRestfulSleep);

  // Wait long enough for our recompute sleep logic to run.
  prv_feed_canned_accel_data(SECONDS_PER_MINUTE * ACTIVITY_SESSION_UPDATE_MIN, 0,
                              ActivitySleepStateAwake);
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){100, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){2 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepRestfulSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){1 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0, 0}));

  // Expected resting calories
  uint32_t exp_resting_kcalories[ACTIVITY_HISTORY_DAYS];
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++) {
    if (i == 0) {
      // All tests start at 5pm, we we just entered 3 minutes of data.
      uint32_t minutes_today = 17 * MINUTES_PER_HOUR + 3 + ACTIVITY_SESSION_UPDATE_MIN;
      exp_resting_kcalories[i] = ROUND(s_exp_full_day_resting_kcalories * minutes_today,
                                       MINUTES_PER_DAY);
    } else {
      exp_resting_kcalories[i] = s_exp_full_day_resting_kcalories;
    }
  }
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricRestingKCalories, exp_resting_kcalories);

  // Put in 2 activities, one of which should drop off on a new day because it's old and the
  // other which drop off because it is in the future (invalid)
  time_t day_start = time_util_get_midnight_of(rtc_get_time());
  ActivitySession old_activity = {
    .start_utc = day_start + 12 * SECONDS_PER_HOUR,
    .length_min = 120,
    .type = ActivitySessionType_Walk,
    .step_data = {
      .steps = 100,
      .active_kcalories = 200,
      .resting_kcalories = 300,
      .distance_meters = 400,
    },
  };
  ActivitySession new_activity = {
    .start_utc = day_start + 23 * SECONDS_PER_HOUR,
    .length_min = 120,
    .type = ActivitySessionType_Run,
    .step_data = {
      .steps = 1000,
      .active_kcalories = 300,
      .resting_kcalories = 400,
      .distance_meters = 500,
    },
  };
  activity_sessions_prv_add_activity_session(&old_activity);
  activity_sessions_prv_add_activity_session(&new_activity);
  ASSERT_NUM_ACTIVITY_SESSIONS(4);  // 2 sleep sessions + 2 activity sessions
  ASSERT_STEP_ACTIVITY_SESSION_PRESENT(&old_activity);
  ASSERT_STEP_ACTIVITY_SESSION_PRESENT(&new_activity);

  // Wait long enough for our midnight rollover to occur. We init time at 5pm, so we need to wait
  // for at least 7 hours.
  const int minutes_till_midnight = (7 * MINUTES_PER_HOUR) - ACTIVITY_SESSION_UPDATE_MIN - 3;
  prv_feed_canned_accel_data(SECONDS_PER_MINUTE * (minutes_till_midnight + 1), 0,
                              ActivitySleepStateAwake);
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 100, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 2 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepRestfulSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 1 * SECONDS_PER_MINUTE, 0, 0, 0, 0, 0}));
  for (int i = 0; i < ACTIVITY_HISTORY_DAYS; i++) {
    if (i == 0) {
      exp_resting_kcalories[i] = 1;
    } else if (i == 1) {
      exp_resting_kcalories[i] = ROUND(s_exp_full_day_resting_kcalories * (MINUTES_PER_DAY - 1),
                                       MINUTES_PER_DAY);
    } else {
      exp_resting_kcalories[i] = s_exp_full_day_resting_kcalories;
    }
  }
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricRestingKCalories, exp_resting_kcalories);

  // Verify that the expired and invalid activity session have been removed
  ASSERT_NUM_ACTIVITY_SESSIONS(0);

  // Verify that we have the right history capacity
  uint32_t exp_history[ACTIVITY_HISTORY_DAYS];
  for (int i = 1; i < ACTIVITY_HISTORY_DAYS; i++) {
    memset(exp_history, 0, sizeof(exp_history));
    exp_history[i] = 100;
    ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount, exp_history);

    prv_advance_by_days(1);
  }

}


// ---------------------------------------------------------------------------------------
// Derived metrics like distance, calories, and walking minutes that are based on steps
void test_activity__step_derived_metrics(void) {
  int32_t value;

  // All tests start at 5pm, which is 1020 minutes into the day
  const int k_minute_start = 1020;

  // Set the user's dimensions
  const int k_height_mm = 1630;
  activity_prefs_set_height_mm(k_height_mm);
  activity_prefs_set_weight_dag(6800);
  activity_prefs_set_gender(ActivityGenderFemale);
  activity_prefs_set_age_years(30);

  // The health_service calls expect to be in the app or worker task
  stub_pebble_tasks_set_current(PebbleTask_App);

  // Advance to a new day to give a chance for the new resting metabolism to be incorporated
  struct tm time_tm = s_init_time_tm;
  time_tm.tm_mday += 1;
  time_t utc_sec = mktime(&time_tm);
  rtc_set_time(utc_sec);
  prv_activity_init_and_set_enabled(true);

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // All tests start at 5pm, which is 1020 minutes into a 1440 minute day. The BMR for
  // the above user is 1388 kcalories per day, so we expect to get:
  //    1388 * 1020/1440 = 1023 kcalories
  activity_get_metric(ActivityMetricRestingKCalories, 1, &value);
  const int k_bmr_cal = 1388 * ACTIVITY_CALORIES_PER_KCAL;
  cl_assert_equal_i(value, ROUND(k_bmr_cal * k_minute_start / MINUTES_PER_DAY,
                                 ACTIVITY_CALORIES_PER_KCAL));
  cl_assert_equal_i(health_service_sum_today(HealthMetricRestingKCalories), value);

  // Feed in 100 steps/minute over 1 hour (walking rate)
  prv_feed_canned_accel_data(SECONDS_PER_HOUR, 100, ActivitySleepStateAwake);
  const int k_exp_steps = 100 * MINUTES_PER_HOUR;

  // Test the derived metrics
  activity_get_metric(ActivityMetricStepCount, 1, &value);
  cl_assert_equal_i(value, k_exp_steps);
  cl_assert_equal_i(health_service_sum_today(HealthMetricStepCount), k_exp_steps);

  activity_get_metric(ActivityMetricActiveSeconds, 1, &value);
  cl_assert_equal_i(value, SECONDS_PER_HOUR);
  cl_assert_equal_i(health_service_sum_today(HealthMetricActiveSeconds), SECONDS_PER_HOUR);

  activity_get_metric(ActivityMetricActiveKCalories, 1, &value);
  // The following determined from a known good commit
  int32_t exp_active_kcalories = 152;
  cl_assert_equal_i(value, exp_active_kcalories);
  cl_assert_equal_i(health_service_sum_today(HealthMetricActiveKCalories), exp_active_kcalories);

  // We now expect to get the following resting calories since we are now 1025 minutes into the day:
  const int exp_resting_calories = k_bmr_cal * (k_minute_start + MINUTES_PER_HOUR)
                                   / MINUTES_PER_DAY;
  activity_get_metric(ActivityMetricRestingKCalories, 1, &value);
  cl_assert_equal_i(value, ROUND(exp_resting_calories, ACTIVITY_CALORIES_PER_KCAL));


  // Test that ActivityMetricStepMinutes responds correctly
  prv_feed_canned_accel_data(1 * SECONDS_PER_MINUTE, 100, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(1 * SECONDS_PER_MINUTE, 10, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(1 * SECONDS_PER_MINUTE, 100, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(1 * SECONDS_PER_MINUTE, 10, ActivitySleepStateAwake);
  activity_get_metric(ActivityMetricActiveSeconds, 1, &value);
  cl_assert_equal_i(value, SECONDS_PER_HOUR + (2 * SECONDS_PER_MINUTE));
  cl_assert_equal_i(health_service_sum_today(HealthMetricActiveSeconds),
                    SECONDS_PER_HOUR + (2 * SECONDS_PER_MINUTE));


  // ----------------------------------------------------------------------------------
  // Reset and try another case. Faster pace and taller person
  activity_stop_tracking();
  fake_system_task_callbacks_invoke_pending();

  const int k_height_mm_2 = 1830;
  activity_prefs_set_height_mm(k_height_mm_2);
  activity_prefs_set_weight_dag(9100);
  activity_prefs_set_gender(ActivityGenderMale);
  activity_prefs_set_age_years(40);

  // Another day
  utc_sec += SECONDS_PER_DAY;
  rtc_set_time(utc_sec);
  prv_activity_init_and_set_enabled(true);

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // All tests start at 5pm, which is 1020 minutes into a 1440 minute day. The BMR for
  // the above user is 1859 kcalories per day, so we expect to get:
  //    1859 * 1020/1440 = 1328 kcalories
  activity_get_metric(ActivityMetricRestingKCalories, 1, &value);
  const int k_bmr_cal_2 = 1859 * ACTIVITY_CALORIES_PER_KCAL;
  cl_assert_equal_i(value, ROUND(k_bmr_cal_2 * k_minute_start / MINUTES_PER_DAY,
                                 ACTIVITY_CALORIES_PER_KCAL));

  // Feed in 125 steps/minute over 60 minutes
  prv_feed_canned_accel_data(60 * SECONDS_PER_MINUTE, 125, ActivitySleepStateAwake);
  const int k_exp_steps_2 = 125 * MINUTES_PER_HOUR;

  // Test the derived metrics
  activity_get_metric(ActivityMetricActiveSeconds, 1, &value);
  cl_assert_equal_i(value, SECONDS_PER_HOUR);

  activity_get_metric(ActivityMetricStepCount, 1, &value);
  cl_assert_equal_i(value, k_exp_steps_2);

  activity_get_metric(ActivityMetricActiveKCalories, 1, &value);
  // The following determined from a known good commit
  int32_t exp_active_kcalories_2 = 486;
  cl_assert_equal_i(value, exp_active_kcalories_2);

  // We now expect to get the following resting calories
  const int exp_resting_calories_2 = k_bmr_cal_2 * (k_minute_start + MINUTES_PER_HOUR)
                                     / MINUTES_PER_DAY;
  activity_get_metric(ActivityMetricRestingKCalories, 1, &value);
  cl_assert_equal_i(value, ROUND(exp_resting_calories_2, ACTIVITY_CALORIES_PER_KCAL));
}


// ---------------------------------------------------------------------------------------
// Test derived metrics based on sleep data
void test_activity__sleep_derived_metrics(void) {
  int32_t value;

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // All of our tests start at 5pm. Let's enter a sleep cycle where the user gets into bed
  // at 10pm, takes 30 minutes to fall asleep, and wakes up at 6am.

  // Light walking, 50 steps/minute, until 10pm
  prv_feed_canned_accel_data(5 * SECONDS_PER_HOUR, 50, ActivitySleepStateAwake);

  // Falling asleep for 30 minutes
  prv_feed_canned_accel_data(30 * SECONDS_PER_MINUTE, 5, ActivitySleepStateAwake);

  // Starting at 10:30pm: 2 Cycles of light (60 min), deep (50 min), awake (10 min)
  for (int i = 0; i < 2; i++) {

    prv_feed_canned_accel_data(60 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);
    activity_get_metric(ActivityMetricSleepState, 1, &value);
    cl_assert_equal_i(value, ActivitySleepStateLightSleep);

    prv_feed_canned_accel_data(50 * SECONDS_PER_MINUTE, 0, ActivitySleepStateRestfulSleep);
    activity_get_metric(ActivityMetricSleepState, 1, &value);
    cl_assert_equal_i(value, ActivitySleepStateRestfulSleep);

    prv_feed_canned_accel_data(10 * SECONDS_PER_MINUTE, 20, ActivitySleepStateAwake);
  }

  // 30 minute "morning walk" 4 hours later at 2:30am
  prv_feed_canned_accel_data(30 * SECONDS_PER_MINUTE, 50, ActivitySleepStateAwake);
  activity_get_metric(ActivityMetricSleepState, 1, &value);
  cl_assert_equal_i(value, ActivitySleepStateAwake);
  cl_assert_equal_i(health_service_peek_current_activities(), HealthActivityNone);

  int exp_value = 22 * SECONDS_PER_HOUR + 30 * SECONDS_PER_MINUTE; // 10:30pm in minutes
  activity_get_metric(ActivityMetricSleepEnterAtSeconds, 1, &value);
  cl_assert_equal_i(value, exp_value);

  activity_get_metric(ActivityMetricSleepStateSeconds, 1, &value);
  // Ideally it would show 40 minutes, but we only sample once every ACTIVITY_SESSION_UPDATE_MIN minutes
  cl_assert(value <= 40 * SECONDS_PER_MINUTE
            && value >= (40 - ACTIVITY_SESSION_UPDATE_MIN) * SECONDS_PER_MINUTE);

  // Verify the root metrics. Since we also verify these using the health_service api, set
  // the task to the app task now
  stub_pebble_tasks_set_current(PebbleTask_App);
  activity_get_metric(ActivityMetricSleepTotalSeconds, 1, &value);
  cl_assert_equal_i(value, 220 * SECONDS_PER_MINUTE);
  cl_assert_equal_i(health_service_sum_today(HealthMetricSleepSeconds), 220 * SECONDS_PER_MINUTE);

  activity_get_metric(ActivityMetricSleepRestfulSeconds, 1, &value);
  cl_assert_equal_i(value, 100 * SECONDS_PER_MINUTE);
  cl_assert_equal_i(health_service_sum_today(HealthMetricSleepRestfulSeconds),
                    100 * SECONDS_PER_MINUTE);

  activity_get_metric(ActivityMetricSleepExitAtSeconds, 1, &value);
  cl_assert_equal_i(value, 2 * SECONDS_PER_HOUR + 20 * SECONDS_PER_MINUTE
                    /* 2:20am in minutes */);
}


// ---------------------------------------------------------------------------------------
// Test that sleep sessions get registered in the correct day
void test_activity__sleep_history(void) {
  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // All of our tests start at 5pm. Let's enter a sleep cycle where the user has a sleep session
  // before the cut-off for the new day
  // Light walking, 50 steps/minute, until 6pm
  prv_feed_canned_accel_data(1 * SECONDS_PER_HOUR, 50, ActivitySleepStateAwake);

  // 2.5 hours of sleep, put's us at 8:30pm. The cut-off for the next day is
  // ACTIVITY_LAST_SLEEP_MINUTE_OF_DAY, currently set for 9pm so this session should be
  // registered for today
  prv_feed_canned_accel_data(150 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);

  // Awake for 30 minutes which puts us at 9pm.
  prv_feed_canned_accel_data(30 * SECONDS_PER_MINUTE, 20, ActivitySleepStateAwake);

  // Another 2 hour sleep session starting at 9pm. This will leave us at 11pm. Since this
  // session ends after the the cutoff, it should be registered for the next day
  prv_feed_canned_accel_data(120 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);

  // Awake for 2 hours which puts us at 1am
  prv_feed_canned_accel_data(120 * SECONDS_PER_MINUTE, 20, ActivitySleepStateAwake);

  // Now if we get sleep history, we should have 2.5 hours yesterday, and 2 hours today
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){120 * SECONDS_PER_MINUTE,
                                                150 * SECONDS_PER_MINUTE}));

  // Another 2 hour sleep session starting at 1am. This will leave us at 3am.
  prv_feed_canned_accel_data(120 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);

  // Awake for 1 hour which puts us at 4am
  prv_feed_canned_accel_data(60 * SECONDS_PER_MINUTE, 20, ActivitySleepStateAwake);

  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){240 * SECONDS_PER_MINUTE,
                                                150 * SECONDS_PER_MINUTE}));
}


// ---------------------------------------------------------------------------------------
// Test raw sample capturing
void test_activity__raw_sample_collection(void) {
  bool enabled;
  uint32_t session_id;
  uint32_t num_samples;
  uint32_t seconds;

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // ---------------------------------------------------------------------------------------
  // Feed in some raw samples where every sample is unique
  {
    prv_reset_captured_dls_data();
    activity_raw_sample_collection(true, false, &enabled, &session_id, &num_samples, &seconds);
    cl_assert(enabled);
    cl_assert_equal_i(num_samples, 0);

    // Feed in 510 values to test entire dynamic range
    const int k_raw_samples = 510;
    AccelRawData raw_data[k_raw_samples];
    for (int i = 0; i < k_raw_samples; i++) {
      // We store multiples of 8 because the compression algorithm divides by 8.
      raw_data[i].x = i * 8;
      raw_data[i].y = -i * 8;
      raw_data[i].z = (i + 1) * 8;
    }
    prv_feed_raw_accel_data(raw_data, k_raw_samples);

    // Stop collection
    activity_raw_sample_collection(false, true, &enabled, &session_id, &num_samples, &seconds);
    cl_assert(!enabled);
    cl_assert_equal_i(num_samples, k_raw_samples);
    cl_assert_equal_i(seconds,
                      (k_raw_samples + ALGORITHM_SAMPLING_RATE - 1) / ALGORITHM_SAMPLING_RATE);

    // Verify the collected data
    AccelRawData captured_data[k_raw_samples];
    prv_uncompress_captured_data(captured_data, k_raw_samples);
    cl_assert_equal_m(raw_data, captured_data, k_raw_samples * sizeof(AccelRawData));
  }


  // ---------------------------------------------------------------------------------------
  // Feed in some raw samples with some runs
  {
    prv_reset_captured_dls_data();
    activity_raw_sample_collection(true, false, &enabled, &session_id, &num_samples, &seconds);
    cl_assert(enabled);
    cl_assert_equal_i(num_samples, 0);

    // Feed in 510 values to test entire dynamic range
    const int k_raw_samples = 510;
    AccelRawData raw_data[k_raw_samples];
    int value = 0;
    for (int i = 0; i < k_raw_samples; i++) {
      // We store multiples of 8 because the compression algorithm divides by 8.
      raw_data[i].x = value * 8;
      raw_data[i].y = -value * 8;
      raw_data[i].z = (value + 1) * 8;
      if ((i % 7) == 0) {
        value += 1;
      }
    }
    prv_feed_raw_accel_data(raw_data, k_raw_samples);

    // Stop collection
    activity_raw_sample_collection(false, true, &enabled, &session_id, &num_samples, &seconds);
    cl_assert(!enabled);
    cl_assert_equal_i(num_samples, k_raw_samples);
    cl_assert_equal_i(seconds,
                      (k_raw_samples + ALGORITHM_SAMPLING_RATE - 1) / ALGORITHM_SAMPLING_RATE);

    // Verify the collected data
    AccelRawData captured_data[k_raw_samples];
    prv_uncompress_captured_data(captured_data, k_raw_samples);
    cl_assert_equal_m(raw_data, captured_data, k_raw_samples * sizeof(AccelRawData));
  }
}


// ---------------------------------------------------------------------------------------
// Test getting the sleep sessions
void test_activity__get_sleep_sessions(void) {
  int32_t value;

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // Light walking, 50 steps/minute, until 10pm
  prv_feed_canned_accel_data(5 * SECONDS_PER_HOUR, 50, ActivitySleepStateAwake);

  // Falling asleep for 30 minutes
  prv_feed_canned_accel_data(30 * SECONDS_PER_MINUTE, 5, ActivitySleepStateAwake);

  // Starting at 10:30pm: 2 Cycles of light (60 min), deep (50 min), awake (10 min)
  for (int i = 0; i < 2; i++) {
    prv_feed_canned_accel_data(60 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);

    prv_feed_canned_accel_data(50 * SECONDS_PER_MINUTE, 0, ActivitySleepStateRestfulSleep);

    prv_feed_canned_accel_data(10 * SECONDS_PER_MINUTE, 20, ActivitySleepStateAwake);
  }

  // 30 minute "morning walk" 4 hours later at 2:30am
  prv_feed_canned_accel_data(30 * SECONDS_PER_MINUTE, 50, ActivitySleepStateAwake);
  activity_get_metric(ActivityMetricSleepState, 1, &value);
  cl_assert_equal_i(value, ActivitySleepStateAwake);

  // Assert that we got the same sleep sessions using the activity service as we do using
  // the health API
  prv_assert_equal_activity_and_health_sleep_sessions(4);
}


// ---------------------------------------------------------------------------------------
// Test getting the minute history
void test_activity__get_minute_history(void) {

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  const uint32_t exp_num_records = 10;
  HealthMinuteData minutes[exp_num_records];

  // The last ALG_MINUTES_PER_RECORD of minutes may not be available yet, so start
  // well enough before that
  time_t utc_start = rtc_get_time() - ((ALG_MINUTES_PER_FILE_RECORD * 2) * SECONDS_PER_MINUTE);
  const time_t exp_utc_start = utc_start;

  stub_pebble_tasks_set_current(PebbleTask_App);
  uint32_t num_records = exp_num_records;
  activity_get_minute_history(minutes, &num_records, &utc_start);
  cl_assert_equal_i(num_records, exp_num_records);
  cl_assert_equal_i(utc_start, exp_utc_start);
  cl_assert_equal_i(minutes[0].steps, exp_utc_start % 255);


  // ---------------------------------------------------------------------------------------
  // Once a minute, retrieve the last ALG_MINUTES_PER_RECORD minutes of data. We should
  // get 1 fewer record each time because we know that the activity algorithm code only
  // writes a new minute data record once every ALG_MINUTES_PER_RECORD minutes.

  // Start on a ALG_MINUTES_PER_RECORD minute boundary so that we know we have
  // ALG_MINUTES_PER_RECORD records available up to the current time
  struct tm start_tm = {
    // Jan 1, 2015, 5am
    .tm_hour = 5,
    .tm_mday = 1,
    .tm_mon = 0,
    .tm_year = 115
  };
  time_t utc_sec = mktime(&start_tm);
  rtc_set_time(utc_sec);

  time_t oldest_to_fetch = rtc_get_time() - (ALG_MINUTES_PER_FILE_RECORD * SECONDS_PER_MINUTE);
  for (int i = 0; i < ALG_MINUTES_PER_FILE_RECORD; i++) {

    // Ask for the last ALG_MINUTES_PER_RECORD minutes of data
    num_records = ALG_MINUTES_PER_FILE_RECORD;
    time_t start_time = oldest_to_fetch + (i * SECONDS_PER_MINUTE);
    time_t end_time = utc_sec;
    HealthMinuteData received_records[ALG_MINUTES_PER_FILE_RECORD];
    num_records = health_service_get_minute_history(received_records, num_records, &start_time,
                                                    &end_time);

    cl_assert_equal_i(num_records, ALG_MINUTES_PER_FILE_RECORD - i);
    cl_assert_equal_i(start_time, oldest_to_fetch + (i * SECONDS_PER_MINUTE));

    printf("\nReceived %d minute records", (int)num_records);
    for (int j = 0; j < num_records; j++) {
      printf("\nRecord:%d, steps: %d", j, (int)received_records[j].steps);
    }

    // Verify the contents of the records
    for (int j = 0; j < num_records; j++) {
      cl_assert_equal_i(received_records[j].steps,
                        (start_time + (j * SECONDS_PER_MINUTE)) % 255);
    }

    // Advance another minute.
    rtc_set_time(utc_sec + (i * SECONDS_PER_MINUTE));
  }
}


// ---------------------------------------------------------------------------------------
// Return the index of the step averages slot that contains the given minute
static uint16_t prv_step_avg_slot(int hour, int min) {
  int minutes = hour * MINUTES_PER_HOUR + min;
  return minutes / (MINUTES_PER_DAY / ACTIVITY_NUM_METRIC_AVERAGES);
}

// Used by the test_activity__step_averages() method to figure out what steps/min we should
// feed in for the given 15-minute time slot
int prv_expected_steps_per_min(int slot, int multiplier) {
  if (multiplier == 1) {
    // The slot % 50 was chosen so that the total # of steps per day does not exceed 2^16
    return ((slot % 50) + 1);
  } else if (multiplier == 2) {
    // The slot % 30 was chosen so that the total # of steps per day does not exceed 2^16
    return 2 * ((slot % 30) + 1);
  } else {
    cl_assert(false);
    return 0;
  }
}


// ------------------------------------------------------------------------------------
// Verify that the settings are what we expected from prv_save_known_settings()
void prv_assert_known_settings(void) {
  struct tm time_tm = s_init_time_tm;
  time_t utc_sec = mktime(&time_tm);
  rtc_set_time(utc_sec);

  prv_activity_init_and_set_enabled(true);
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){300, 200, 100, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepTotalSeconds,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS])
                                  {6 * SECONDS_PER_MINUTE, 4 * SECONDS_PER_MINUTE,
                                   2 * SECONDS_PER_MINUTE, 0, 0, 0, 0}));
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricSleepRestfulSeconds,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS])
                                  {3 * SECONDS_PER_MINUTE, 2 * SECONDS_PER_MINUTE,
                                   1 * SECONDS_PER_MINUTE, 0, 0, 0, 0}));
}


// --------------------------------------------------------------------------------------
// Save the current settings file format with known data to the local file system so that it can
// be checked in and used for migration tests.
static void prv_save_known_settings_file(const char *filename) {

  // Let's include 3 days of history by start at s_init_time_tm - 3 days
  struct tm time_tm = s_init_time_tm;
  time_t utc_sec = mktime(&time_tm);
  utc_sec -= 2 * SECONDS_PER_DAY;
  rtc_set_time(utc_sec);

  prv_activity_init_and_set_enabled(true);
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // Feed in 100 steps/min over 1 min, 1 minute of deep and 1 minute of light sleep
  prv_feed_canned_accel_data(60, 100, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(60, 0, ActivitySleepStateRestfulSleep);
  prv_feed_canned_accel_data(60, 0, ActivitySleepStateLightSleep);

  // Wait long enough for our recompute sleep logic to run.
  prv_feed_canned_accel_data(SECONDS_PER_MINUTE * ACTIVITY_SESSION_UPDATE_MIN, 0,
                              ActivitySleepStateAwake);

  // Advance to next day
  prv_feed_canned_accel_data(SECONDS_PER_HOUR * 24, 0, ActivitySleepStateAwake);

  // Feed in 100 steps/min over 2 min, 2 minute of deep and 2 minute of light sleep
  prv_feed_canned_accel_data(120, 100, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(120, 0, ActivitySleepStateRestfulSleep);
  prv_feed_canned_accel_data(120, 0, ActivitySleepStateLightSleep);

  // Wait long enough for our recompute sleep logic to run.
  prv_feed_canned_accel_data(SECONDS_PER_MINUTE * ACTIVITY_SESSION_UPDATE_MIN, 0,
                              ActivitySleepStateAwake);

  // Advance to next day
  prv_feed_canned_accel_data(SECONDS_PER_HOUR * 24, 0, ActivitySleepStateAwake);

  // Feed in 100 steps/min over 3 min, 3 minute of deep and 3 minute of light sleep
  prv_feed_canned_accel_data(180, 100, ActivitySleepStateAwake);
  prv_feed_canned_accel_data(180, 0, ActivitySleepStateRestfulSleep);
  prv_feed_canned_accel_data(180, 0, ActivitySleepStateLightSleep);

  // Wait long enough for our recompute sleep logic to run.
  prv_feed_canned_accel_data(SECONDS_PER_MINUTE * ACTIVITY_SESSION_UPDATE_MIN, 0,
                              ActivitySleepStateAwake);

  // Make sure they are what we expected
  prv_assert_known_settings();


  // Extract activity settings file from PFS and save to the local file system
  char out_path[strlen(CLAR_FIXTURE_PATH) + strlen(ACTIVITY_FIXTURE_PATH)  + strlen(filename) + 3];
  sprintf(out_path, "%s/%s/%s", CLAR_FIXTURE_PATH, ACTIVITY_FIXTURE_PATH, filename);

  // Open and read the settings file from PFS
  int fd = pfs_open(ACTIVITY_SETTINGS_FILE_NAME, OP_FLAG_READ, FILE_TYPE_STATIC,
                    ACTIVITY_SETTINGS_FILE_LEN);
  cl_assert(fd >= S_SUCCESS);
  size_t size = pfs_get_file_size(fd);
  uint8_t *buf = malloc(size);
  cl_assert(buf != NULL);
  cl_assert(pfs_read(fd, buf, size) == size);
  pfs_close(fd);

  // Save it to the local file system
  FILE *file = fopen(out_path, "wb");
  cl_assert(file != NULL);
  cl_assert_equal_i(fwrite(buf, size, 1, file), 1);
  fclose(file);
  free(buf);

  printf("\nSaved current settings file to %s", out_path);
}


// ---------------------------------------------------------------------------------------
// Create the settings file in PFS from a file saved in the local file system
static void prv_load_settings_file_onto_pfs(const char *filename, const char *pfs_name) {
  char in_path[strlen(CLAR_FIXTURE_PATH) + strlen(ACTIVITY_FIXTURE_PATH)  + strlen(filename) + 3];
  sprintf(in_path, "%s/%s/%s", CLAR_FIXTURE_PATH, ACTIVITY_FIXTURE_PATH, filename);

  // check that file exists and fits in buffer
  struct stat st;
  cl_assert(stat(in_path, &st) == 0);

  FILE *file = fopen(in_path, "r");
  cl_assert(file);

  uint8_t buf[st.st_size];
  // copy file to fake flash storage
  cl_assert(fread(buf, 1, st.st_size, file) > 0);

  pfs_remove(pfs_name);
  int fd = pfs_open(pfs_name, OP_FLAG_WRITE, FILE_TYPE_STATIC, st.st_size);
  cl_assert(fd >= 0);
  int bytes_written = pfs_write(fd, buf, st.st_size);
  cl_assert(st.st_size == bytes_written);
  pfs_close(fd);
}


// ---------------------------------------------------------------------------------------
// Test that we correctly migrate older versions of activity settings files
void test_activity__migrate_settings(void) {

  // Uncomment this call to prv_save_known_settings_file() in order to save the current version
  // of settings to the fixture directory. After doing this, you will need to git add it and modify
  // this migration test to read it in and verify its contents after migration.
  // prv_save_known_settings_file("activity_settings.v1");

  // Load the v1 settings format.
  prv_load_settings_file_onto_pfs("activity_settings.v1", ACTIVITY_SETTINGS_FILE_NAME);

  // Make sure it got migrated correctly.
  prv_activity_init_and_set_enabled(true);
  prv_assert_known_settings();
}


// ----------------------------------------------------------------------------
// fake_event callback used to look for sleep events generated by the health_events test
static PebbleEvent s_captured_sleep_event = { };
static int s_num_captured_sleep_events = 0;
static void prv_fake_sleep_event_cb(PebbleEvent *event) {
  if ((event->type == PEBBLE_HEALTH_SERVICE_EVENT)
      && (event->health_event.type == HealthEventSleepUpdate)) {
    s_captured_sleep_event = *event;
    s_num_captured_sleep_events++;
  }
}


// ----------------------------------------------------------------------------
// fake_event callback used to look for history update events generated by the health_events test
static PebbleEvent s_captured_history_event = { };
static int s_num_captured_history_events = 0;
static void prv_fake_history_event_cb(PebbleEvent *event) {
  if ((event->type == PEBBLE_HEALTH_SERVICE_EVENT)
      && (event->health_event.type == HealthEventSignificantUpdate)) {
    s_captured_history_event = *event;
    s_num_captured_history_events++;
  }
}


// ---------------------------------------------------------------------------------------
// Test that we generate health events at the appropriate time
void test_activity__health_events(void) {
  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // -----------------------------------
  // Test that we receive step update events
  fake_event_reset_count();
  // Feed in 100 steps/minute over 1 minute. We should get some step update events
  prv_feed_canned_accel_data(1 * SECONDS_PER_MINUTE, 100, ActivitySleepStateAwake);

  uint32_t event_count = fake_event_get_count();
  // Our fake algorithm generates a step update once a second
  cl_assert_equal_i(event_count, 1 * SECONDS_PER_MINUTE);

  PebbleEvent event = fake_event_get_last();
  cl_assert_equal_i(event.type, PEBBLE_HEALTH_SERVICE_EVENT);
  cl_assert_equal_i(event.health_event.type, HealthEventMovementUpdate);

  // -----------------------------------
  // Test that we receive sleep update events
  prv_reset_captured_dls_data();

  // Falling asleep for 30 minutes
  prv_feed_canned_accel_data(30 * SECONDS_PER_MINUTE, 5, ActivitySleepStateAwake);

  // Starting at 10:31pm: 1 Cycle of light (60 min), deep (50 min)
  fake_event_reset_count();
  fake_event_set_callback(prv_fake_sleep_event_cb);
  s_captured_sleep_event = (PebbleEvent) { };
  s_num_captured_sleep_events = 0;
  prv_feed_canned_accel_data(60 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);
  prv_feed_canned_accel_data(50 * SECONDS_PER_MINUTE, 0, ActivitySleepStateRestfulSleep);

  prv_feed_canned_accel_data(15 * SECONDS_PER_MINUTE, 0, ActivitySleepStateAwake);

  prv_feed_canned_accel_data(60 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);
  prv_feed_canned_accel_data(50 * SECONDS_PER_MINUTE, 0, ActivitySleepStateRestfulSleep);

  // Wait long enough for our recompute sleep logic to run.
  prv_feed_canned_accel_data(SECONDS_PER_MINUTE * ACTIVITY_SESSION_UPDATE_MIN, 60,
                              ActivitySleepStateAwake);

  // See if we got the expected sleep events
  cl_assert(s_num_captured_sleep_events > 0);

  event = s_captured_sleep_event;
  cl_assert_equal_i(event.type, PEBBLE_HEALTH_SERVICE_EVENT);
  cl_assert_equal_i(event.health_event.type, HealthEventSleepUpdate);

  // -----------------------------------
  // Test that we receive history update events
  fake_event_reset_count();
  fake_event_set_callback(prv_fake_history_event_cb);
  s_captured_history_event = (PebbleEvent) { };
  s_num_captured_history_events = 0;

  // Get the current day_id
  int32_t actual;
  activity_get_metric(ActivityMetricStepCount, 1, &actual);

  // Wait long enough for a midnight rollover. All tests start at 5pm, so if we wait
  // 7 hours, we should get a midnight rollover
  prv_feed_canned_accel_data(7 * SECONDS_PER_HOUR, 0, ActivitySleepStateAwake);

  // See if we got the expected history events
  cl_assert_equal_i(s_num_captured_history_events, 1);

  event = s_captured_history_event;
  cl_assert_equal_i(event.type, PEBBLE_HEALTH_SERVICE_EVENT);
  cl_assert_equal_i(event.health_event.type, HealthEventSignificantUpdate);
}


// ---------------------------------------------------------------------------------------
// Test derived sleep metrics after the watch goes through a timezone change.
void test_activity__sleep_after_timezone_change(void) {
  int32_t value;

  // ----------------------------------------------------------------------------
  // Let's start out in EST time when tracking starts. All of our tests start at 5pm UTC, which is
  // 12pm EST. Let's start out in this time zone then switch back to PST right before we fall
  // asleep. This replicates the conditions that resulted in PBL-24823
  TimezoneInfo tz_info = {
    .tm_zone = "EST",
    .tm_gmtoff = -5 * SECONDS_PER_HOUR,
  };
  time_util_update_timezone(&tz_info);

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // Advance to 6pm EST
  prv_feed_canned_accel_data(6 * SECONDS_PER_HOUR, 50, ActivitySleepStateAwake);

  // switch into PST (which would be 3pm)
  tz_info = (TimezoneInfo) {
    .tm_zone = "PST",
    .tm_gmtoff = -8 * SECONDS_PER_HOUR,
  };
  time_util_update_timezone(&tz_info);

  // Walk some more until 11pm PST
  prv_feed_canned_accel_data(8 * SECONDS_PER_HOUR, 50, ActivitySleepStateAwake);

  // Starting at 11pm: 2 Cycles of 3 hrs each light (165 min), awake (15 min)
  for (int i = 0; i < 2; i++) {
    prv_feed_canned_accel_data(165 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);

    prv_feed_canned_accel_data(15 * SECONDS_PER_MINUTE, 20, ActivitySleepStateAwake);
  }

  activity_get_metric(ActivityMetricSleepEnterAtSeconds, 1, &value);
  cl_assert_equal_i(value, 23 * SECONDS_PER_HOUR /* 11pm */);

  activity_get_metric(ActivityMetricSleepTotalSeconds, 1, &value);
  cl_assert_equal_i(value, 330 * SECONDS_PER_MINUTE);

  activity_get_metric(ActivityMetricSleepExitAtSeconds, 1, &value);
  cl_assert_equal_i(value, 4 * SECONDS_PER_HOUR + 45 * SECONDS_PER_MINUTE /* 4:45am */);

  // Assert that we got the same sleep sessions using the activity service as we do using
  // the health API
  prv_assert_equal_activity_and_health_sleep_sessions(2);

  // ----------------------------------------------------------------------------
  // The previous test left us at 5am PST. Let's try going the other way and switch from PST to
  // EST right before we fall asleep
  // Advance to 11pm PST
  prv_feed_canned_accel_data(18 * SECONDS_PER_HOUR, 50, ActivitySleepStateAwake);

  // It is now 11pm PST. Switch to EST, which would be 2am
  tz_info = (TimezoneInfo) {
    .tm_zone = "EST",
    .tm_gmtoff = -5 * SECONDS_PER_HOUR,
  };
  time_util_update_timezone(&tz_info);

  // Starting at 2am EST: 2 Cycles of 3 hrs each light (165 min), awake (15 min)
  for (int i = 0; i < 2; i++) {
    prv_feed_canned_accel_data(165 * SECONDS_PER_MINUTE, 0, ActivitySleepStateLightSleep);

    prv_feed_canned_accel_data(15 * SECONDS_PER_MINUTE, 20, ActivitySleepStateAwake);
  }

  activity_get_metric(ActivityMetricSleepEnterAtSeconds, 1, &value);
  cl_assert_equal_i(value, 2 * SECONDS_PER_HOUR /* 2am */);

  activity_get_metric(ActivityMetricSleepTotalSeconds, 1, &value);
  cl_assert_equal_i(value, 330 * SECONDS_PER_MINUTE);

  activity_get_metric(ActivityMetricSleepExitAtSeconds, 1, &value);
  cl_assert_equal_i(value, 7 * SECONDS_PER_HOUR + 45 * SECONDS_PER_MINUTE /* 7:45am */);

  // Assert that we got the same sleep sessions using the activity service as we do using
  // the health API
  prv_assert_equal_activity_and_health_sleep_sessions(2);
}


// ---------------------------------------------------------------------------------------
// Test that the health service correctly interpolates when asked for a metric over partial days
void test_activity__health_service_interpolation(void) {
  // Let's start out in PST time when tracking starts. All of our tests start at 5pm UTC, which is
  // 9am PST.
  TimezoneInfo tz_info = {
    .tm_zone = "PST",
    .tm_gmtoff = -8 * SECONDS_PER_HOUR,
  };
  time_util_update_timezone(&tz_info);

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // Feed in 100 steps/min over 10 minutes, for a total of 1000 steps for today
  prv_feed_canned_accel_data(10 * SECONDS_PER_MINUTE, 100, ActivitySleepStateAwake);

  // Wait long enough until we start the next day (15 hours)
  prv_feed_canned_accel_data(SECONDS_PER_HOUR * 15, 0,
                              ActivitySleepStateAwake);
  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
                              ((const uint32_t [ACTIVITY_HISTORY_DAYS]){0, 1000, 0, 0, 0, 0, 0}));


  // Feed in 100 steps/min over 20 minutes, for a total of 2000 steps for today
  prv_feed_canned_accel_data(20 * SECONDS_PER_MINUTE, 100, ActivitySleepStateAwake);

  ASSERT_EQUAL_METRIC_HISTORY(ActivityMetricStepCount,
      ((const uint32_t [ACTIVITY_HISTORY_DAYS]){2000, 1000, 0, 0, 0, 0, 0}));


  // If we ask for the sum of the latter half of yesterday, we should get 500
  HealthValue steps = health_service_sum(
      HealthMetricStepCount, time_start_of_today() - (12 * SECONDS_PER_HOUR),
      time_start_of_today());
  cl_assert_equal_i(steps, 500);

  // If we ask for the sum from latter half of yesterday till now, we should get 2500
  steps = health_service_sum(
      HealthMetricStepCount, time_start_of_today() - (12 * SECONDS_PER_HOUR), rtc_get_time());
  cl_assert_equal_i(steps, 2500);

  // If we ask for the sum from latter half of yesterday till half of today, we should get 1500
  time_t elapsed_today = rtc_get_time() - time_start_of_today();
  steps = health_service_sum(
    HealthMetricStepCount, time_start_of_today() - (12 * SECONDS_PER_HOUR),
    time_start_of_today() + (elapsed_today / 2));
  cl_assert_equal_i(steps, 1500);
}


// ---------------------------------------------------------------------------------------
// Test distance using various speeds and user dimensions
typedef struct {
  int height_in;
  int gender;
  int steps;
  float seconds;
  int exp_distance_m;  // expected distance
} DistanceTestParams;

void test_activity__distance(void) {
  int32_t value;

  // The health_service calls expect to be in the app or worker task
  stub_pebble_tasks_set_current(PebbleTask_App);

  DistanceTestParams tests[] = {
    {69, ActivityGenderMale, 19177, 6360, 23352},
    {69, ActivityGenderMale, 10351, 3600, 11764},
    {69, ActivityGenderMale, 3003, 1560, 2398},
    {69, ActivityGenderMale, 3423, 2100, 2881},
    {65, ActivityGenderFemale, 6940, 3120, 8047},
    {65, ActivityGenderFemale, 4577, 2460, 3508},
    {63, ActivityGenderFemale, 4738, 1860, 4989},
    {63, ActivityGenderFemale, 4799, 1860, 5134},
    {63, ActivityGenderFemale, 2896, 1500, 2334},
    {71, ActivityGenderMale, 7529, 4020, 5568},
    {67, ActivityGenderMale, 6592, 3960, 6067},
    {73, ActivityGenderMale, 4467, 1740, 5118},
    {73, ActivityGenderMale, 4080, 1800, 5102},
    {73, ActivityGenderMale, 2890, 1680, 2382},
    {73, ActivityGenderMale, 4143, 2400, 3251},
    {64, ActivityGenderMale, 4373, 1823, 4168},
    {64, ActivityGenderMale, 642, 384, 483},
    {64, ActivityGenderMale, 4455, 1819, 4072},
    {64, ActivityGenderMale, 2008, 1229, 1448},
    {64, ActivityGenderMale, 2217, 1302, 1674},
    {64, ActivityGenderMale, 4568, 1820, 4152},
  };

  // Init the time
  struct tm time_tm = s_init_time_tm;
  time_tm.tm_mday += 1;
  time_t utc_sec = mktime(&time_tm);
  rtc_set_time(utc_sec);
  fake_system_task_callbacks_invoke_pending();

  int act_distance[ARRAY_LENGTH(tests)];
  const int k_elapsed_sec = 2 * SECONDS_PER_MINUTE;

  // Evaluate each test case
  for (int i = 0; i < ARRAY_LENGTH(tests); i++) {
    DistanceTestParams *params = &tests[i];

    // Advance to new day to reset the distance
    utc_sec += SECONDS_PER_DAY;
    rtc_set_time(utc_sec);
    prv_activity_init_and_set_enabled(true);

    // Set the user's dimensions
    activity_prefs_set_height_mm((int)(params->height_in * 25.4));
    activity_prefs_set_gender(params->gender);

    // Start activity tracking. This method assumes it can be called from any task, so we must
    // invoke system callbacks to handle its KernelBG callback.
    activity_start_tracking(false /*test_mode*/);
    fake_system_task_callbacks_invoke_pending();

    // Feed in the test cadence for 2 minutes. Compute the expected distance in 2 minutes
    // as well
    int steps_per_minute = (int)((float)params->steps / params->seconds * SECONDS_PER_MINUTE);
    int exp_distance_m = ROUND(params->exp_distance_m * k_elapsed_sec, params->seconds);

    // Feed in the test cadence for the given amount of time
    prv_feed_canned_accel_data(k_elapsed_sec, steps_per_minute, ActivitySleepStateAwake);

    activity_get_metric(ActivityMetricStepCount, 1, &value);
    cl_assert_near(value, ROUND(steps_per_minute * k_elapsed_sec, SECONDS_PER_MINUTE), 5);

    activity_get_metric(ActivityMetricDistanceMeters, 1, &value);
    act_distance[i] = value;
    float err = abs(exp_distance_m - value);
    float pct_err = err * 100.0 / exp_distance_m;
    printf("\nTest %d: height:%d, steps:%d, seconds:%.1f, exp_distance:%d, exp_distance_2min:%d, "
           "act_distance_2min:%"PRIu32", pct_err: %.2f%% \n", i, params->height_in, params->steps,
           params->seconds, params->exp_distance_m, exp_distance_m, value, pct_err);

    // Check the percent error
    cl_assert(pct_err < 25);
    cl_assert_equal_i(value, health_service_sum_today(HealthMetricWalkedDistanceMeters));

    activity_stop_tracking();
    fake_system_task_callbacks_invoke_pending();
  }

  // Print summary of results
  printf("\ntest  height  steps  seconds  cadence  exp_dist  exp_dist_2min  act_dist_2min   %%err");
  printf("\n------------------------------------------------------------------------------------");
  float pct_err_sum = 0;
  for (int i = 0; i < ARRAY_LENGTH(tests); i++) {
    DistanceTestParams *params = &tests[i];

    int steps_per_minute = (int)((float)params->steps / params->seconds * SECONDS_PER_MINUTE);
    int exp_distance_m = ROUND(params->exp_distance_m * k_elapsed_sec, params->seconds);
    float err = act_distance[i] - exp_distance_m;
    float pct_err = err * 100.0 / exp_distance_m;
    printf("\n%4d  %5d   %4d   %7.2f  %7d   %7d  %13d  %13d    %+.2f",
           i, params->height_in, params->steps, params->seconds, steps_per_minute,
           params->exp_distance_m, exp_distance_m, act_distance[i], pct_err);
    pct_err_sum += pct_err >= 0 ? pct_err : -pct_err;
  }

  printf("\n--------------------------");
  float avg_pct_err = pct_err_sum / ARRAY_LENGTH(tests);
  printf("\nAVERAGE PCT ERROR: %.2f", avg_pct_err);

  // Check the overall percent error
  cl_assert(avg_pct_err < 10);
}


// --------------------------------------------------------------------------------------------
// Advance through time simulating the heart rate manager calls
static int s_num_hrm_callbacks;
static void prv_advance_time_hr(uint32_t num_sec, uint8_t bpm, HRMQuality quality,
                                bool force_continuous) {
  // Call the minute handler, which computes the minute stats and saves them to data logging
  // as well as the sleep PFS file.
  for (int i = 0; i < num_sec; i++) {
    fake_rtc_set_ticks(rtc_get_ticks() + configTICK_RATE_HZ);
    rtc_set_time(rtc_get_time() + 1);

    if ((s_hrm_manager_update_interval == 1) || force_continuous) {
      PebbleHRMEvent hrm_event = {
        .event_type = HRMEvent_BPM,
        .bpm.bpm = bpm,
        .bpm.quality = quality,
      };
      prv_hrm_subscription_cb(&hrm_event, NULL);
      s_num_hrm_callbacks++;
    }
    if ((rtc_get_time() % SECONDS_PER_MINUTE) == 0) {
      prv_minute_system_task_cb(NULL);
    }
  }
}


// ---------------------------------------------------------------------------------------
// Test that we subscribe to the HR events at the expected times
void test_activity__hrm_sampling_period(void) {
  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();
  s_test_alg_state.orientation = 0x11; // Not flat

  prv_advance_time_hr(ACTIVITY_DEFAULT_HR_PERIOD_SEC, 100 /*bpm*/, HRMQuality_Good, false /*force_continuous*/);

  // Should be 1 second sampling when we start up
  cl_assert_equal_i(s_hrm_manager_update_interval, 1);

  // The last update time should be 0
  int32_t last_update_utc;
  activity_get_metric(ActivityMetricHeartRateRawUpdatedTimeUTC, 1, &last_update_utc);
  cl_assert_equal_i(last_update_utc, 0);

  // Simulate callbacks one second away from turning down the sampling rate
  // Use Acceptable because of the short circuiting in `prv_heart_rate_subscription_update`
  prv_advance_time_hr(ACTIVITY_DEFAULT_HR_ON_TIME_SEC - 1, 100 /*bpm*/,
                      HRMQuality_Acceptable, false /*force_continuous*/);

  // The last update time should be within a second
  activity_get_metric(ActivityMetricHeartRateRawUpdatedTimeUTC, 1, &last_update_utc);
  cl_assert(last_update_utc >= rtc_get_time() - 1);
  cl_assert(last_update_utc <= rtc_get_time());

  // Should still be sampling every 1 second
  cl_assert_equal_i(s_hrm_manager_update_interval, 1);

  // Tick one more second, should trigger slow sampling
  prv_advance_time_hr(1, 100 /*bpm*/, HRMQuality_Good, false /*force_continuous*/);
  // Should be back to no sampling by now (very large sampling period)
  cl_assert(s_hrm_manager_update_interval > SECONDS_PER_HOUR);

  // Advance to our next sampling period, but the watch is flat so we shouldn't start sampling
  s_test_alg_state.orientation = 0x00; // Flat
  prv_advance_time_hr(ACTIVITY_DEFAULT_HR_PERIOD_SEC, 100 /*bpm*/, HRMQuality_Good, false /*force_continuous*/);
  cl_assert(s_hrm_manager_update_interval > SECONDS_PER_HOUR);

  // Advance to our next sampling period, the watch is no longer flat so we should be sampling
  s_test_alg_state.orientation = 0x22; // Not flat
  prv_advance_time_hr(ACTIVITY_DEFAULT_HR_PERIOD_SEC, 100 /*bpm*/, HRMQuality_Good, false /*force_continuous*/);
  cl_assert_equal_i(s_hrm_manager_update_interval, 1);
}


// ---------------------------------------------------------------------------------------
// Test that average heart rate is reported correctly
void test_activity__hrm_median(void) {
  int32_t median, total_weight;

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // Reset the median
  activity_metrics_prv_reset_hr_stats();

  // Our previous median should be since we have no data
  int32_t last_median;
  int32_t last_update_utc;
  activity_get_metric(ActivityMetricHeartRateFilteredBPM, 1, &last_median);
  activity_get_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC, 1, &last_update_utc);
  cl_assert_equal_i(last_median, 0);
  cl_assert_equal_i(last_update_utc, 0);

  // Simulate some HRM callbacks with no heart rate, should get 0 median
  prv_advance_time_hr(10 /*sec*/, 0 /*hr*/, HRMQuality_Good, true /*force_continuous*/);
  activity_metrics_prv_get_median_hr_bpm(&median, &total_weight);
  cl_assert_equal_i(median, 0);
  cl_assert_equal_i(total_weight, 0);

  // Our previous median should be since we have no data (valid data)
  activity_get_metric(ActivityMetricHeartRateFilteredBPM, 1, &last_median);
  activity_get_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC, 1, &last_update_utc);
  cl_assert_equal_i(last_median, 0);
  cl_assert_equal_i(last_update_utc, 0);

  // Simulate some HRM callbacks with non-zero heart rate
  prv_advance_time_hr(3 /*sec*/, 50 /*hr*/, HRMQuality_Good, true /*force_continuous*/);
  prv_advance_time_hr(3 /*sec*/, 100 /*hr*/, HRMQuality_Good, true /*force_continuous*/);
  prv_advance_time_hr(1 /*sec*/, 51 /*hr*/, HRMQuality_Good, true /*force_continuous*/);
  prv_advance_time_hr(8 /*sec*/, 120 /*hr*/, HRMQuality_Worst, true /*force_continuous*/);
  prv_minute_system_task_cb(NULL);
  activity_metrics_prv_get_median_hr_bpm(&median, &total_weight);
  cl_assert_equal_i(median, 51);

  // The last median should be stored and accessable via the LastStableBPM metric
  activity_get_metric(ActivityMetricHeartRateFilteredBPM, 1, &last_median);
  activity_get_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC, 1, &last_update_utc);
  cl_assert_equal_i(last_median, 51);
  cl_assert(last_update_utc >= rtc_get_time() - 1);
  cl_assert(last_update_utc <= rtc_get_time());

  // Reset the stats, the median should be 0
  activity_metrics_prv_reset_hr_stats();
  activity_metrics_prv_get_median_hr_bpm(&median, &total_weight);
  cl_assert_equal_i(median, 0);

  // But the last stable BPM shouldn't get wiped
  activity_get_metric(ActivityMetricHeartRateFilteredBPM, 1, &last_median);
  activity_get_metric(ActivityMetricHeartRateFilteredUpdatedTimeUTC, 1, &last_update_utc);
  cl_assert_equal_i(last_median, 51);
  cl_assert(last_update_utc >= rtc_get_time() - 1);
  cl_assert(last_update_utc <= rtc_get_time());

}

static uint32_t s_num_hr_events;
static PebbleHealthEvent s_last_hr_event;
static void prv_fake_hr_event_handler(PebbleEvent *e) {
  s_num_hr_events++;
  s_last_hr_event = e->health_event;
}

// ---------------------------------------------------------------------------------------
// Test that some HRM events aren't passed on from activity service
void test_activity__hrm_ignore(void) {
  int32_t median, total_weight;

  s_num_hr_events = 0;

  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  fake_event_reset_count();
  fake_event_set_callback(prv_fake_hr_event_handler);

  // Should not fire off an event. Bad HR reading
  prv_advance_time_hr(1 /*sec*/, 0 /*hr*/, HRMQuality_Good, true /*force_continuous*/);
  cl_assert_equal_i(s_num_hr_events, 0);

  // Should fire off an event. Good HR and Good quality
  prv_advance_time_hr(1 /*sec*/, 120 /*hr*/, HRMQuality_Good, true /*force_continuous*/);
  cl_assert_equal_i(s_num_hr_events, 1);

  // Should fire off an event. OffWrist, tell clients
  prv_advance_time_hr(1 /*sec*/, 120 /*hr*/, HRMQuality_OffWrist, true /*force_continuous*/);
  cl_assert_equal_i(s_num_hr_events, 2);
  cl_assert_equal_i(s_last_hr_event.data.heart_rate_update.current_bpm, 0);
  cl_assert_equal_i(s_last_hr_event.data.heart_rate_update.quality, HRMQuality_OffWrist);

  // Should fire off an event. OffWrist, tell clients
  prv_advance_time_hr(1 /*sec*/, 0 /*hr*/, HRMQuality_OffWrist, true /*force_continuous*/);
  cl_assert_equal_i(s_num_hr_events, 3);
  cl_assert_equal_i(s_last_hr_event.data.heart_rate_update.current_bpm, 0);
  cl_assert_equal_i(s_last_hr_event.data.heart_rate_update.quality, HRMQuality_OffWrist);

  // Should fire off an event. Good HR and Good Quality
  prv_advance_time_hr(1 /*sec*/, 120 /*hr*/, HRMQuality_Excellent, true /*force_continuous*/);
  cl_assert_equal_i(s_num_hr_events, 4);
  cl_assert_equal_i(s_last_hr_event.data.heart_rate_update.current_bpm, 120);
  cl_assert_equal_i(s_last_hr_event.data.heart_rate_update.quality, HRMQuality_Excellent);

  // Should not fire off an event. Bad HR reading
  prv_advance_time_hr(1 /*sec*/, 20 /*hr*/, HRMQuality_Excellent, true /*force_continuous*/);
  cl_assert_equal_i(s_num_hr_events, 4);
}

// ---------------------------------------------------------------------------------------
// Today is Thursday
void test_activity__prv_set_metric(void) {
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  int32_t metric_values[ACTIVITY_HISTORY_DAYS] = {0};

  // Set today's value
  activity_metrics_prv_set_metric(ActivityMetricStepCount, Thursday, 1111);

  // Set yesterday's value
  activity_metrics_prv_set_metric(ActivityMetricStepCount, Wednesday, 2222);

  // Set last friday value
  activity_metrics_prv_set_metric(ActivityMetricStepCount, Friday, 3333);
  activity_get_metric(ActivityMetricStepCount, 7, metric_values);
  cl_assert_equal_i(metric_values[0], 1111);
  cl_assert_equal_i(metric_values[1], 2222);
  cl_assert_equal_i(metric_values[2], 0000);
  cl_assert_equal_i(metric_values[3], 0000);
  cl_assert_equal_i(metric_values[4], 0000);
  cl_assert_equal_i(metric_values[5], 0000);
  cl_assert_equal_i(metric_values[6], 3333);

  // Set the current value to something larger
  activity_metrics_prv_set_metric(ActivityMetricStepCount, Thursday, 4444);
  activity_get_metric(ActivityMetricStepCount, 1, metric_values);
  cl_assert_equal_i(metric_values[0], 4444);

  // Set the current value to something smaller (will be ignored)
  activity_metrics_prv_set_metric(ActivityMetricStepCount, Thursday, 1);
  activity_get_metric(ActivityMetricStepCount, 1, metric_values);
  cl_assert_equal_i(metric_values[0], 4444);

  // Verify some other metrics work
  activity_metrics_prv_set_metric(ActivityMetricActiveSeconds, Thursday, 60);
  activity_get_metric(ActivityMetricActiveSeconds, 1, metric_values);
  cl_assert_equal_i(metric_values[0], 60);

  activity_metrics_prv_set_metric(ActivityMetricDistanceMeters, Thursday, 66);
  activity_metrics_prv_set_metric(ActivityMetricDistanceMeters, Wednesday, 22);
  activity_get_metric(ActivityMetricDistanceMeters, 2, metric_values);
  cl_assert_equal_i(metric_values[0], 66);
  cl_assert_equal_i(metric_values[1], 22);
  cl_assert_equal_i(activity_metrics_prv_get_distance_mm(), 66 * MM_PER_METER);

  activity_metrics_prv_set_metric(ActivityMetricActiveKCalories, Thursday, 22);
  activity_metrics_prv_set_metric(ActivityMetricActiveKCalories, Wednesday, 33);
  activity_get_metric(ActivityMetricActiveKCalories, 2, metric_values);
  cl_assert_equal_i(metric_values[0], 22);
  cl_assert_equal_i(metric_values[1], 33);
  cl_assert_equal_i(activity_metrics_prv_get_active_calories(), 22 * ACTIVITY_CALORIES_PER_KCAL);

  activity_metrics_prv_set_metric(ActivityMetricRestingKCalories, Thursday, 2000);
  activity_metrics_prv_set_metric(ActivityMetricRestingKCalories, Wednesday, 44);
  activity_get_metric(ActivityMetricRestingKCalories, 2, metric_values);
  cl_assert_equal_i(metric_values[0], 2000);
  cl_assert_equal_i(metric_values[1], 44);
  cl_assert_equal_i(activity_metrics_prv_get_resting_calories(), 2000 * ACTIVITY_CALORIES_PER_KCAL);

  activity_metrics_prv_set_metric(ActivityMetricSleepTotalSeconds, Thursday, 60);
  activity_get_metric(ActivityMetricSleepTotalSeconds, 1, metric_values);
  cl_assert_equal_i(metric_values[0], 60);

  activity_metrics_prv_set_metric(ActivityMetricSleepRestfulSeconds, Wednesday, 60);
  activity_get_metric(ActivityMetricSleepRestfulSeconds, 2, metric_values);
  cl_assert_equal_i(metric_values[1], 60);

  activity_metrics_prv_set_metric(ActivityMetricSleepEnterAtSeconds, Thursday, 60);
  activity_get_metric(ActivityMetricSleepEnterAtSeconds, 1, metric_values);
  cl_assert_equal_i(metric_values[0], 60);

  activity_metrics_prv_set_metric(ActivityMetricSleepExitAtSeconds, Wednesday, 60);
  activity_get_metric(ActivityMetricSleepExitAtSeconds, 2, metric_values);
  cl_assert_equal_i(metric_values[1], 60);

  activity_stop_tracking();
  fake_system_task_callbacks_invoke_pending();

  activity_metrics_prv_set_metric(ActivityMetricStepCount, Thursday, 5555);
  activity_get_metric(ActivityMetricStepCount, 1, metric_values);
  cl_assert_equal_i(metric_values[0], 4444);
}

// Test that we report the that a run session is ongoing.
void test_activity__activity_sessions_run_ongoing_then_end(void) {
  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // No sessions active, ensure that asking if a run is ongoing returns false
  cl_assert_equal_b(false, activity_sessions_is_session_type_ongoing(ActivitySessionType_Run));
  cl_assert_equal_i(0, health_service_peek_current_activities());

  // Start on known boundary
  struct tm start_tm = {
    // Jan 1, 2015, 5am
    .tm_hour = 5,
    .tm_mday = 1,
    .tm_mon = 0,
    .tm_year = 115
  };
  time_t utc_sec = mktime(&start_tm);
  rtc_set_time(utc_sec);

  // Add a run session
  const time_t time_elapsed = (20 * SECONDS_PER_MINUTE);
  ActivitySession run_activity = {
    .start_utc = utc_sec - time_elapsed,
    .length_min = time_elapsed,
    .type = ActivitySessionType_Run,
    .ongoing = true,
  };
  activity_sessions_prv_add_activity_session(&run_activity);

  // Run session active, ensure that asking if a run is ongoing returns true
  cl_assert_equal_b(true, activity_sessions_is_session_type_ongoing(ActivitySessionType_Run));
  cl_assert_equal_i(HealthActivityRun, health_service_peek_current_activities());

  // Finish the run session
  utc_sec += (10 * SECONDS_PER_MINUTE);
  rtc_set_time(utc_sec);
  run_activity.ongoing = false;

  // Update session
  activity_sessions_prv_add_activity_session(&run_activity);

  // Run session ended, ensure that asking if a run is ongoing returns false
  cl_assert_equal_b(false, activity_sessions_is_session_type_ongoing(ActivitySessionType_Run));
  cl_assert_equal_i(0, health_service_peek_current_activities());
}

// ---------------------------------------------------------------------------------------
// Test that we report the that a Sleep session is ongoing.
void test_activity__activity_sessions_sleep_ongoing_then_delete(void) {
  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // No sessions active, ensure that asking if a sleep session is ongoing returns false
  cl_assert_equal_b(false, activity_sessions_is_session_type_ongoing(ActivitySessionType_Sleep));
  cl_assert_equal_i(0, health_service_peek_current_activities());

  // Start on known boundary
  struct tm start_tm = {
    // Jan 1, 2015, 5am
    .tm_hour = 5,
    .tm_mday = 1,
    .tm_mon = 0,
    .tm_year = 115
  };
  time_t utc_sec = mktime(&start_tm);
  rtc_set_time(utc_sec);

  // Add a Sleep session
  const time_t time_elapsed = (120 * SECONDS_PER_MINUTE);
  ActivitySession sleep_session = {
    .start_utc = utc_sec - time_elapsed,
    .length_min = time_elapsed,
    .type = ActivitySessionType_Sleep,
    .ongoing = true,
  };
  activity_sessions_prv_add_activity_session(&sleep_session);

  // Flip the switch to say we are in light sleep.
  activity_private_state()->sleep_data.cur_state = ActivitySleepStateLightSleep;

  // Sleep session active, ensure that asking if a Sleep is ongoing returns true
  cl_assert_equal_b(true, activity_sessions_is_session_type_ongoing(ActivitySessionType_Sleep));
  cl_assert_equal_i(HealthActivitySleep, health_service_peek_current_activities());

  // Delete session
  activity_sessions_prv_delete_activity_session(&sleep_session);

  // Flip the switch to say we are in an awake state.
  activity_private_state()->sleep_data.cur_state = ActivitySleepStateAwake;

  // Sleep session ended, ensure that asking if a Sleep is ongoing returns false
  cl_assert_equal_b(false, activity_sessions_is_session_type_ongoing(ActivitySessionType_Sleep));
  cl_assert_equal_i(0, health_service_peek_current_activities());
}

// ---------------------------------------------------------------------------------------
// Test that we report the that multiple sessions are ongoing.
void test_activity__activity_sessions_ongoing_multiple(void) {
  // Start activity tracking. This method assumes it can be called from any task, so we must
  // invoke system callbacks to handle its KernelBG callback.
  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();

  // No sessions active, ensure that asking for run,walk,sleep returns false
  cl_assert_equal_b(false, activity_sessions_is_session_type_ongoing(ActivitySessionType_Run));
  cl_assert_equal_b(false, activity_sessions_is_session_type_ongoing(ActivitySessionType_Walk));
  cl_assert_equal_b(false, activity_sessions_is_session_type_ongoing(ActivitySessionType_Sleep));
  cl_assert_equal_i(0, health_service_peek_current_activities());

  // Start on known boundary
  struct tm start_tm = {
    // Jan 1, 2015, 5am
    .tm_hour = 5,
    .tm_mday = 1,
    .tm_mon = 0,
    .tm_year = 115
  };
  time_t utc_sec = mktime(&start_tm);
  rtc_set_time(utc_sec);

  const time_t time_elapsed = (20 * SECONDS_PER_MINUTE);

  // Add a run session
  ActivitySession run_activity = {
    .start_utc = utc_sec - time_elapsed,
    .length_min = time_elapsed,
    .type = ActivitySessionType_Run,
    .ongoing = true,
  };
  activity_sessions_prv_add_activity_session(&run_activity);

  // Add a walk session
  ActivitySession walk_activity = {
    .start_utc = utc_sec - time_elapsed,
    .length_min = time_elapsed,
    .type = ActivitySessionType_Walk,
    .ongoing = true,
  };
  activity_sessions_prv_add_activity_session(&walk_activity);

  // Add a sleep session
  ActivitySession sleep_activity = {
    .start_utc = utc_sec - time_elapsed,
    .length_min = time_elapsed,
    .type = ActivitySessionType_Sleep,
    .ongoing = true,
  };
  activity_sessions_prv_add_activity_session(&sleep_activity);

  // Flip the switch to say we are in light sleep.
  activity_private_state()->sleep_data.cur_state = ActivitySleepStateLightSleep;

  // Run,Walk,Sleep sessions active, ensure that asking if they are ongoing, it returns true
  cl_assert_equal_b(true, activity_sessions_is_session_type_ongoing(ActivitySessionType_Run));
  cl_assert_equal_b(true, activity_sessions_is_session_type_ongoing(ActivitySessionType_Walk));
  cl_assert_equal_b(true, activity_sessions_is_session_type_ongoing(ActivitySessionType_Sleep));
  cl_assert_equal_i(HealthActivityRun | HealthActivityWalk | HealthActivitySleep , health_service_peek_current_activities());
}

static void prv_set_median_hr_for_minutes(int bpm, int num_minutes) {
  const int num_samples = 15;
  activity_private_state()->hr.num_samples = num_samples;
  memset(activity_private_state()->hr.samples, bpm, num_samples);
  memset(activity_private_state()->hr.weights, 100, num_samples);

  for (int i = 0; i < num_minutes; i++) {
    prv_minute_system_task_cb(NULL);
  }
}

static bool prv_is_hr_elevated(void) {
  return activity_private_state()->hr.metrics.is_hr_elevated;
}

void test_activity__update_time_in_hr_zones(void) {
  int32_t zone1_minutes, zone2_minutes, zone3_minutes;

  activity_start_tracking(false /*test_mode*/);
  fake_system_task_callbacks_invoke_pending();
  activity_metrics_prv_reset_hr_stats();

  cl_assert_equal_b(prv_is_hr_elevated(), false);
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 0);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 0);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 0);

  // Add some "regular" heart rates. This shouldn't affect our zone counts
  prv_set_median_hr_for_minutes(70 /* BPM */, 3 /* minutes */);
  cl_assert_equal_b(prv_is_hr_elevated(), false);
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 0);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 0);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 0);

  // Add some "very elevated" heart rates.
  // The zone should wait 1 minute, move up 1 zone per minute, stop at the top
  prv_set_median_hr_for_minutes(185 /* BPM */, 5 /* minutes */);
  cl_assert_equal_b(prv_is_hr_elevated(), true);
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 1);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 1);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 2);

  // Add some "regular" heart rates.
  // The zone should move down 1 zone per minute
  prv_set_median_hr_for_minutes(70 /* BPM */, 4 /* minutes */);
  cl_assert_equal_b(prv_is_hr_elevated(), false);
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 2);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 2);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 2);

  // Add some more "regular" heart rates.
  // This shouldn't affect our zone counts
  prv_set_median_hr_for_minutes(70 /* BPM */, 3 /* minutes */);
  cl_assert_equal_b(prv_is_hr_elevated(), false);
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 2);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 2);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 2);

  // Add a "blip" which shouldn't affect our zone counts.
  // This shouldn't affect our zone counts
  prv_set_median_hr_for_minutes(180 /* BPM */, 1 /* minutes */);
  cl_assert_equal_b(prv_is_hr_elevated(), true);
  prv_set_median_hr_for_minutes(70 /* BPM */, 1 /* minutes */);
  cl_assert_equal_b(prv_is_hr_elevated(), false);
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 2);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 2);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 2);

  // Ad some "Semi-active" heart rates
  prv_set_median_hr_for_minutes(130 /* BPM */, 3 /* minutes */);
  cl_assert_equal_b(prv_is_hr_elevated(), true);
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 4);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 2);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 2);

  // Advance to a new day. The HR zone stats should get reset
  time_t utc_sec = rtc_get_time();
  utc_sec += SECONDS_PER_DAY;
  rtc_set_time(utc_sec);
  prv_minute_system_task_cb(NULL);
  cl_assert_equal_b(prv_is_hr_elevated(), true); // stays elevated
  activity_get_metric(ActivityMetricHeartRateZone1Minutes, 1, &zone1_minutes);
  cl_assert_equal_i(zone1_minutes, 0);
  activity_get_metric(ActivityMetricHeartRateZone2Minutes, 1, &zone2_minutes);
  cl_assert_equal_i(zone2_minutes, 0);
  activity_get_metric(ActivityMetricHeartRateZone3Minutes, 1, &zone3_minutes);
  cl_assert_equal_i(zone3_minutes, 0);
}

// ---------------------------------------------------------------------------------------
// Test that we can add / delete an activity session
void test_activity__activity_sessions_add_delete_sessions(void) {
  ActivitySession empty_session = {};

  ActivitySession walk_activity = {
    .start_utc = 1,
    .length_min = 5,
    .type = ActivitySessionType_Walk,
    .ongoing = true,
  };

  // Add then delete
  activity_sessions_prv_add_activity_session(&walk_activity);
  cl_assert_equal_i(activity_private_state()->activity_sessions_count, 1);
  cl_assert_equal_m(&activity_private_state()->activity_sessions[0], &walk_activity,
                    sizeof(ActivitySession));

  activity_sessions_prv_delete_activity_session(&walk_activity);
  cl_assert_equal_i(activity_private_state()->activity_sessions_count, 0);
  cl_assert_equal_m(&activity_private_state()->activity_sessions[0], &empty_session,
                    sizeof(ActivitySession));


  // Add lots of sessions then delete from the front
  for (int i = 0; i < ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT; i++) {
    ActivitySession activity = walk_activity;
    activity.start_utc = i;
    activity_sessions_prv_add_activity_session(&activity);
    cl_assert_equal_i(activity_private_state()->activity_sessions_count, i + 1);
    cl_assert_equal_m(&activity_private_state()->activity_sessions[i], &activity,
                     sizeof(ActivitySession));
  }

  for (int i = 0; i < ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT; i++) {
    ActivitySession activity = walk_activity;
    activity.start_utc = i;

    ActivitySession next_activity = activity;
    next_activity.start_utc = i + 1;

    activity_sessions_prv_delete_activity_session(&activity);
    cl_assert_equal_i(activity_private_state()->activity_sessions_count,
                      ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT - 1 - i);
    if (activity_private_state()->activity_sessions_count) {
      cl_assert_equal_m(&activity_private_state()->activity_sessions[0], &next_activity,
                        sizeof(ActivitySession));
    }
  }
  cl_assert_equal_m(&activity_private_state()->activity_sessions[0], &empty_session,
                    sizeof(ActivitySession));


  // Add lots of sessions then delete from the back
  for (int i = 0; i < ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT; i++) {
    ActivitySession activity = walk_activity;
    activity.start_utc = i;
    activity_sessions_prv_add_activity_session(&activity);
    cl_assert_equal_i(activity_private_state()->activity_sessions_count, i + 1);
    cl_assert_equal_m(&activity_private_state()->activity_sessions[i], &activity,
                     sizeof(ActivitySession));
  }

  for (int i = ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT - 1; i >= 0; i--) {
    ActivitySession activity = walk_activity;
    activity.start_utc = i;

    ActivitySession next_activity = activity;
    next_activity.start_utc = i - 1;

    activity_sessions_prv_delete_activity_session(&activity);
    cl_assert_equal_i(activity_private_state()->activity_sessions_count, i);
    if (activity_private_state()->activity_sessions_count) {
      cl_assert_equal_m(&activity_private_state()->activity_sessions[i], &empty_session,
                        sizeof(ActivitySession));
    }
  }
  cl_assert_equal_m(&activity_private_state()->activity_sessions[0], &empty_session,
                    sizeof(ActivitySession));


  // Add 3 sessions and delete from the middle
  ActivitySession a1 = walk_activity;
  a1.start_utc = 1;
  ActivitySession a2 = walk_activity;
  a2.start_utc = 2;
  ActivitySession a3 = walk_activity;
  a3.start_utc = 3;
  activity_sessions_prv_add_activity_session(&a1);
  activity_sessions_prv_add_activity_session(&a2);
  activity_sessions_prv_add_activity_session(&a3);
  cl_assert_equal_i(activity_private_state()->activity_sessions_count, 3);

  activity_sessions_prv_delete_activity_session(&a2);
  cl_assert_equal_i(activity_private_state()->activity_sessions_count, 2);
  cl_assert_equal_m(&activity_private_state()->activity_sessions[0], &a1,
                    sizeof(ActivitySession));
  cl_assert_equal_m(&activity_private_state()->activity_sessions[1], &a3,
                    sizeof(ActivitySession));

}
