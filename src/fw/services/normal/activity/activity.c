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

#include "applib/data_logging.h"
#include "applib/health_service.h"
#include "drivers/battery.h"
#include "drivers/vibe.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "mfg/mfg_info.h"
#include "os/mutex.h"
#include "os/tick.h"
#include "popups/health_tracking_ui.h"
#include "process_management/app_manager.h"
#include "process_management/worker_manager.h"
#include "services/common/analytics/analytics_event.h"
#include "services/common/battery/battery_state.h"
#include "services/common/hrm/hrm_manager_private.h"
#include "services/common/system_task.h"
#include "services/common/vibe_pattern.h"
#include "services/normal/alarms/alarm.h"
#include "services/normal/blob_db/health_db.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/protobuf_log/protobuf_log.h"
#include "services/normal/protobuf_log/protobuf_log_hr.h"
#include "shell/prefs.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/hexdump.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/base64.h"
#include "util/math.h"
#include "util/size.h"
#include "util/units.h"

#include <pebbleos/cron.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "activity.h"
#include "activity_algorithm.h"
#include "activity_calculators.h"
#include "activity_insights.h"
#include "activity_private.h"

// Our globals
static ActivityState s_activity_state;
static bool s_hrm_present = false;

// ------------------------------------------------------------------------------------------------
ActivityState *activity_private_state(void) {
  return &s_activity_state;
}


// ------------------------------------------------------------------------------------------------
bool activity_is_hrm_present(void) {
  return s_hrm_present;
}

static bool prv_activity_allowed_to_be_enabled(void) {
  return s_activity_state.enabled_run_level && s_activity_state.enabled_charging_state;
}


// ------------------------------------------------------------------------------------------------
// If necessary, change the sampling period of our heart rate subscription
// @param[in] now_ts number of seconds the system has been running (from time_get_uptime_seconds())
static void prv_heart_rate_subscription_update(uint32_t now_ts) {
#if CAPABILITY_HAS_BUILTIN_HRM
  if (!s_hrm_present) {
    return;
  }

  const uint32_t last_toggled_ts = s_activity_state.hr.toggled_sampling_at_ts;

  bool should_toggle = false;
  if (s_activity_state.hr.currently_sampling) {
    // If we are currently sampling, turn off when:
    // - We reach the end of our maximum time on, ACTIVITY_DEFAULT_HR_ON_TIME_SEC
    // - We get ACTIVITY_MIN_NUM_SAMPLES_SHORT_CIRCUIT samples before the time runs out
    //     - e.g. We get X samples >= ACTIVITY_MIN_HR_QUALITY_THRESH in our current minute,
    //       go ahead and turn off the sensor
    const uint32_t turn_off_at = last_toggled_ts + ACTIVITY_DEFAULT_HR_ON_TIME_SEC;
    const bool samples_req_met =
        (s_activity_state.hr.num_quality_samples >= ACTIVITY_MIN_NUM_SAMPLES_SHORT_CIRCUIT);
    if ((turn_off_at <= now_ts) || samples_req_met) {
      should_toggle = true;
    }
  } else {
    // If we are not currently sampling, turn on after ACTIVITY_DEFAULT_HR_PERIOD_SEC
    const uint32_t turn_on_at = last_toggled_ts + ACTIVITY_DEFAULT_HR_PERIOD_SEC;
    if (turn_on_at <= now_ts) {
      should_toggle = true;
    }
  }

  if (should_toggle) {
    // Check to see if the watch is face up or face down. If it is assume the watch is off wrist
    // The z-axis is encoded in the 4 most significant bits of the orientation
    const uint8_t z_axis = s_activity_state.last_orientation >> 4;
    const bool watch_is_flat = z_axis == 0 || z_axis == 8;

    const bool should_be_sampling = !s_activity_state.hr.currently_sampling && !watch_is_flat;
    if (!s_activity_state.hr.currently_sampling && watch_is_flat) {
      PBL_LOG(LOG_LEVEL_INFO, "Not subscribing to HRM: watch is flat(ish)");
    }

    // Pick the subscription rate (essentially ON and OFF)
    const uint32_t desired_interval_sec = (should_be_sampling)
                                          ? ACTIVITY_HRM_SUBSCRIPTION_ON_PERIOD_SEC
                                          : ACTIVITY_HRM_SUBSCRIPTION_OFF_PERIOD_SEC;

    bool success = sys_hrm_manager_set_update_interval(s_activity_state.hr.hrm_session,
                                                       desired_interval_sec , 0 /*expire_sec*/);
    PBL_ASSERTN(success);
    // Update history
    s_activity_state.hr.currently_sampling = should_be_sampling;
    s_activity_state.hr.toggled_sampling_at_ts = now_ts;
    PBL_LOG(LOG_LEVEL_DEBUG, "Changed HR sampling period to %"PRIu32" sec", desired_interval_sec);
  }
#endif // CAPABILITY_HAS_BUILTIN_HRM
}


// ------------------------------------------------------------------------------------------------
// Kernel BG callback called by the Heart Rate Manager when new data arrives
T_STATIC void prv_hrm_subscription_cb(PebbleHRMEvent *hrm_event, void *context) {
#if CAPABILITY_HAS_BUILTIN_HRM
  if (!s_hrm_present) {
    return;
  }

  ACTIVITY_LOG_DEBUG("Got HR event: %d", (int) hrm_event->event_type);
  if (hrm_event->event_type == HRMEvent_BPM) {
    ACTIVITY_LOG_DEBUG("HR bpm: %"PRIu8", qual: %"PRId8" ", hrm_event->bpm.bpm,
                       (int8_t) hrm_event->bpm.quality);

    // Perform a basic validity check so we only proceed with reasonable data
    // TODO: Use quality to filter out some readings,
    // TODO PBL-40784: Use HRMQuality_OffWrist as a special case to slow down the HRM subscription
    bool valid_hr_reading = true;
    if (hrm_event->bpm.bpm < ACTIVITY_DEFAULT_MIN_HR ||
        hrm_event->bpm.bpm > ACTIVITY_DEFAULT_MAX_HR) {
      valid_hr_reading = false;
    }

    uint32_t now_uptime_ts = time_get_uptime_seconds();
    if (valid_hr_reading) {
      // Update the heart rate metrics
      time_t now_utc = rtc_get_time();
      activity_metrics_prv_add_median_hr_sample(hrm_event, now_utc, now_uptime_ts);

      // Log it to the mobile
      protobuf_log_hr_add_sample(s_activity_state.hr.log_session, now_utc,
                                hrm_event->bpm.bpm, hrm_event->bpm.quality);
    }

    if (valid_hr_reading || hrm_event->bpm.quality == HRMQuality_OffWrist) {
      mutex_lock_recursive(s_activity_state.mutex);
      {
        // Post a health service heart rate changed event
        PebbleEvent event = {
          .type = PEBBLE_HEALTH_SERVICE_EVENT,
          .health_event = {
            .type = HealthEventHeartRateUpdate,
            .data.heart_rate_update = {
              .current_bpm = (hrm_event->bpm.quality == HRMQuality_OffWrist) ? 0
                                                                             : hrm_event->bpm.bpm,
              .resting_bpm = s_activity_state.hr.metrics.resting_bpm,
              .quality = hrm_event->bpm.quality,
              .is_filtered = false,
            },
          },
        };
        event_put(&event);
      }
      mutex_unlock_recursive(s_activity_state.mutex);
    }

    // Modify our sampling period now if necessary
    // NOTE: Must be kept at the bottom of the function, or at least below
    //   `activity_metrics_prv_add_median_hr_sample`
    prv_heart_rate_subscription_update(now_uptime_ts);
  }
#endif // CAPABILITY_HAS_BUILTIN_HRM
}


// ---------------------------------------------------------------------------------------
// Init heart rate support
static void prv_heart_rate_init(void) {
#if CAPABILITY_HAS_BUILTIN_HRM
  s_hrm_present = mfg_info_is_hrm_present();
  if (!s_hrm_present) {
    return;
  }

  // Subscribe to HRM data
  s_activity_state.hr.currently_sampling = false;
  s_activity_state.hr.toggled_sampling_at_ts = time_get_uptime_seconds();
  s_activity_state.hr.hrm_session = hrm_manager_subscribe_with_callback(
      INSTALL_ID_INVALID, ACTIVITY_HRM_SUBSCRIPTION_OFF_PERIOD_SEC, 0 /*expire_s*/, HRMFeature_BPM,
      prv_hrm_subscription_cb, NULL);
  PBL_ASSERTN(s_activity_state.hr.hrm_session != HRM_INVALID_SESSION_REF);

  s_activity_state.hr.log_session = protobuf_log_hr_create(NULL);
  PBL_ASSERTN(s_activity_state.hr.log_session != NULL);
#endif // CAPABILITY_HAS_BUILTIN_HRM
}


// ---------------------------------------------------------------------------------------
// De-init heart rate support
static void prv_heart_rate_deinit(void) {
#if CAPABILITY_HAS_BUILTIN_HRM
  if (!s_hrm_present) {
    return;
  }

  sys_hrm_manager_unsubscribe(s_activity_state.hr.hrm_session);
  protobuf_log_session_delete(s_activity_state.hr.log_session);
  activity_metrics_prv_reset_hr_stats();
#endif // CAPABILITY_HAS_BUILTIN_HRM
}



// ----------------------------------------------------------------------------------------------
// Open the settings file and malloc space for the file struct
SettingsFile *activity_private_settings_open(void) {
  SettingsFile *file = kernel_malloc_check(sizeof(SettingsFile));
  if (settings_file_open(file, ACTIVITY_SETTINGS_FILE_NAME,
                         ACTIVITY_SETTINGS_FILE_LEN) != S_SUCCESS) {
    kernel_free(file);
    PBL_LOG(LOG_LEVEL_ERROR, "No settings file");
    return NULL;
  }
  return file;
}


// ------------------------------------------------------------------------------------------------
// Close the settings file and free the file struct
void activity_private_settings_close(SettingsFile *file) {
  settings_file_close(file);
  kernel_free(file);
}


// ----------------------------------------------------------------------------------------------
// Rewrite the settings file. Used when migrating from version 1 to version 2, where all we
// we need to do is recreate the file in a bigger size
static void prv_settings_rewrite_cb(SettingsFile *old_file, SettingsFile *new_file,
                                    SettingsRecordInfo *info, void *context) {
  if (info->key_len != sizeof(ActivitySettingsKey)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Unexpected key len: %"PRIu32" ", (uint32_t)info->key_len);
    return;
  }

  // rewrite this entry
  ActivitySettingsKey key;
  info->get_key(old_file, &key, info->key_len);
  void *data =  kernel_malloc_check(info->val_len);
  info->get_val(old_file, data, info->val_len);

  settings_file_set(new_file, &key, info->key_len, data, info->val_len);

  kernel_free(data);
}


// ----------------------------------------------------------------------------------------------
// Migrate settings from an earlier version now if necessary
static SettingsFile *prv_settings_migrate(SettingsFile *file, uint16_t *written_version) {
  status_t result = E_ERROR;

  ActivitySettingsKey key = ActivitySettingsKeyStepCountHistory;
  if (!settings_file_exists(file, &key, sizeof(key))) {
    // If this settings file is empty, no migration necessary
    return file;
  }

  // See which version we are on
  key = ActivitySettingsKeyVersion;
  result = settings_file_get(file, &key, sizeof(key), written_version, sizeof(*written_version));
  if (result != S_SUCCESS) {
    // Version 1 had no settings key in it.
    *written_version = 1;
  }

  const uint16_t version = *written_version;
  if (version == ACTIVITY_SETTINGS_CURRENT_VERSION) {
    // Current version, no migration necessary
    return file;
  }

  PBL_LOG(LOG_LEVEL_INFO, "Performing settings file migration from verison %"PRIu16"", version);

  // Perform migration
  if (version == 1) {
    // The only other version right now is version 1, which has the same format but the file
    // size is different. We need to re-create it using the new, bigger size.
    result = settings_file_rewrite(file, prv_settings_rewrite_cb, NULL);
    if (result != S_SUCCESS) {
      PBL_LOG(LOG_LEVEL_ERROR, "Failure %"PRIi32" while re-writing setting file", (int32_t)result);
    }
  } else {
    // If the version is totally unexpected, remove the file and create a new one
    PBL_LOG(LOG_LEVEL_ERROR, "Unknown settings file verison %"PRIu16"", version);
  }

  if (result != S_SUCCESS) {
    // Delete the old file and create a new one if migration failed
    activity_private_settings_close(file);
    pfs_remove(ACTIVITY_SETTINGS_FILE_NAME);
    file = activity_private_settings_open();
    *written_version = ACTIVITY_SETTINGS_CURRENT_VERSION;
  }
  return file;
}


// -----------------------------------------------------------------------------------------
// Called from the prv_minute_system_task_cb(). Determines if we should update storage.
static void NOINLINE prv_update_storage(time_t utc_sec) {
  // If no reason to update storage, we can bail immediately.
  s_activity_state.update_settings_counter -= 1;
  if (s_activity_state.update_settings_counter > 0) {
    return;
  }

  // The following sections of code can access the settings file and/or update globals,
  // so we need to surround it with mutex ownership
  mutex_lock_recursive(s_activity_state.mutex);
  {
    SettingsFile *file = activity_private_settings_open();

    if (file && (s_activity_state.update_settings_counter <= 0)) {
      // Periodically save current stats into settings, so that if watch resets or crashes we
      // don't lose too much info
      ACTIVITY_LOG_DEBUG("updating current stats in settings");

      for (ActivityMetric metric = ActivityMetricFirst; metric < ActivityMetricNumMetrics;
           metric++) {
        ActivityMetricInfo m_info;
        activity_metrics_prv_get_metric_info(metric, &m_info);

        if (m_info.has_history) {
          ActivitySettingsValueHistory history;
          settings_file_get(file, &m_info.settings_key, sizeof(m_info.settings_key), &history,
                            sizeof(history));
          history.values[0] = *m_info.value_p;
          settings_file_set(file, &m_info.settings_key, sizeof(m_info.settings_key), &history,
                            sizeof(history));
        } else {
          if (m_info.settings_key != ActivitySettingsKeyInvalid) {
            settings_file_set(file, &m_info.settings_key, sizeof(m_info.settings_key),
                              m_info.value_p, sizeof(*m_info.value_p));
          }
        }
      }

      if (s_activity_state.need_activities_saved) {
        // Save stored activities
        ActivitySettingsKey key = ActivitySettingsKeyStoredActivities;
        settings_file_set(file, &key, sizeof(key), s_activity_state.activity_sessions,
                          sizeof(s_activity_state.activity_sessions));
        s_activity_state.need_activities_saved = false;
      }

      s_activity_state.update_settings_counter = ACTIVITY_SETTINGS_UPDATE_MIN;
    }

    if (file) {
      activity_private_settings_close(file);
    }
  }
  mutex_unlock_recursive(s_activity_state.mutex);
}


// ------------------------------------------------------------------------------------------------
// Tail end of prv_process_minute_data, separated out to decrease stack requirements. This
// portion of the logic handles activity process, saving prefs to our backing store, and
// resetting metrics at midnight.
// We use NOINLINE to reduce the stack requirements during the minute handler (see PBL-38130)
static void NOINLINE prv_process_minute_data_tail(time_t utc_sec) {
  bool need_history_update_event;
  uint16_t cur_day_index;
  mutex_lock_recursive(s_activity_state.mutex);
  {
    cur_day_index = time_util_get_day(utc_sec);
    need_history_update_event = (cur_day_index != s_activity_state.cur_day_index);

    // Call the activity sessions minute handler
    activity_sessions_prv_minute_handler(utc_sec);

    // Update our backing store if necessary
    prv_update_storage(utc_sec);

    // If we are starting a new day, reset all metrics
    if (cur_day_index != s_activity_state.cur_day_index) {
      s_activity_state.step_data = (ActivityStepData) { 0 };
      s_activity_state.sleep_data = (ActivitySleepData) { 0 };
      memset(&s_activity_state.hr.metrics.minutes_in_zone, 0,
             sizeof(s_activity_state.hr.metrics.minutes_in_zone));
      s_activity_state.steps_per_minute_last_steps = 0;
      s_activity_state.distance_mm = 0;
      s_activity_state.active_calories = 0;
      s_activity_state.resting_calories = 0;
      activity_algorithm_metrics_changed_notification();
      s_activity_state.cur_day_index = cur_day_index;

      // Remove sessions that belong to the prior day
      activity_sessions_prv_remove_out_of_range_activity_sessions(utc_sec,
                                                                  false /*remove_ongoing*/);
      activity_insights_recalculate_stats();
    }

    // Update the heart rate sampling period if necessary
    prv_heart_rate_subscription_update(time_get_uptime_seconds());
  }
  mutex_unlock_recursive(s_activity_state.mutex);

  // Send the history update event now if history has changed
  if (need_history_update_event) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Sending history update event");
    PebbleEvent e = {
      .type = PEBBLE_HEALTH_SERVICE_EVENT,
      .health_event = {
        .type = HealthEventSignificantUpdate,
        .data.significant_update = {
          .day_id = cur_day_index,
        },
      },
    };
    event_put(&e);
  }
}


// ------------------------------------------------------------------------------------------------
// Takes care of updating the history when we reach midnight as well as checking for changes in
// sleep state. Returns true if the sleep metrics were updated
// We use NOINLINE to reduce the stack requirements during the minute handler (see PBL-38130)
static void NOINLINE prv_process_minute_data(time_t utc_sec) {
  // Update the metrics
  activity_metrics_prv_minute_handler(utc_sec);

  // Call the algorithm's minute handler. This gives it an opportunity to log minute data
  // to data logging. etc. In case the user settings have changed, pass the current ones in.
  ActivityGender gender = activity_prefs_get_gender();
  uint16_t weight_dag = activity_prefs_get_weight_dag();
  uint16_t height_mm = activity_prefs_get_height_mm();
  uint8_t age_years = activity_prefs_get_age_years();
  activity_algorithm_set_user(height_mm, weight_dag * 10, gender, age_years);

  AlgMinuteRecord minute_record = {};
  activity_algorithm_minute_handler(utc_sec, &minute_record);

  s_activity_state.last_vmc = minute_record.data.base.vmc;
  s_activity_state.last_orientation = minute_record.data.base.orientation;

  // The rest of the minute handling is separated into another method to decrease the stack
  // depth during the call to activity_algorithm_minute_handler() (above)
  prv_process_minute_data_tail(utc_sec);
}


// ------------------------------------------------------------------------------------------------
// This system task, triggered by a minute regular timer, takes care of updating the history
// when we reach midnight, checking for changes in sleep state, and updating insights
T_STATIC void prv_minute_system_task_cb(void *data) {
  if (!s_activity_state.started) {
    return;
  }
  ACTIVITY_LOG_DEBUG("running minute system task");

  // Get the current time
  time_t utc_sec = rtc_get_time();

  // Do our minute processing
  prv_process_minute_data(utc_sec);

  // Process insights
  mutex_lock_recursive(s_activity_state.mutex);
  {
    activity_insights_process_sleep_data(utc_sec);
    activity_insights_process_minute_data(utc_sec);
  }
  mutex_unlock_recursive(s_activity_state.mutex);
}


// ------------------------------------------------------------------------------------------------
// Runs on the timer task. Simply register a callback for the KernelBG task from here.
static void prv_minute_cb(CronJob *job, void *data) {
  system_task_add_callback(prv_minute_system_task_cb, data);
  cron_job_schedule(job);
}

static CronJob s_activity_job = {
  .minute = CRON_MINUTE_ANY,
  .hour = CRON_HOUR_ANY,
  .mday = CRON_MDAY_ANY,
  .month = CRON_MONTH_ANY,
  .cb = prv_minute_cb,
};


// ------------------------------------------------------------------------------------------------
// Capture raw accel data
// If finish is true, we will close out the current partially formed record and log it.
static void prv_collect_raw_samples(AccelRawData *accel_data, uint32_t num_samples,
                                    bool finish) {
  ActivitySampleCollectionData *data = s_activity_state.sample_collection_data;

  // Create the data logging session now, if needed
  if (data->dls_session == NULL) {
    Uuid system_uuid = UUID_SYSTEM;
    data->dls_session = dls_create(
        DlsSystemTagActivityAccelSamples, DATA_LOGGING_BYTE_ARRAY, sizeof(ActivityRawSamplesRecord),
        true /*buffered*/, false /*resume*/, &system_uuid);
    if (data->dls_session == NULL) {
      PBL_LOG(LOG_LEVEL_ERROR, "Unable to create DLS session");
      return;
    }
  }


  if (finish) {
    PBL_ASSERTN(num_samples == 0 && accel_data == NULL);
  }

  // Save the samples
  for (uint32_t i = 0; finish || i < num_samples; i++, accel_data++) {
    // Init the record header now if necessary
    if (data->record.num_samples == 0) {
      data->record = (ActivityRawSamplesRecord) {
        .version = ACTIVITY_RAW_SAMPLES_VERSION,
        .session_id = s_activity_state.sample_collection_session_id,
        .len = sizeof(ActivityRawSamplesRecord),
        .time_local = time_utc_to_local(rtc_get_time()),
        .num_samples = data->run_size,
      };
      if (data->first_record) {
        data->record.flags |= ACTIVITY_RAW_SAMPLE_FLAG_FIRST_RECORD;
        data->first_record = false;
      }
    }

    if (finish) {
      // Finishing up an existing record?
      if (data->run_size > 0) {
        // We started a run, finish it up now.
        ACTIVITY_RAW_SAMPLE_SET_RUN_SIZE(data->prev_sample, data->run_size);
        data->record.entries[data->record.num_entries++] = data->prev_sample;
        data->run_size = 0;
      }
    } else {
      // Add a new sample
      s_activity_state.sample_collection_num_samples++;
      data->record.num_samples++;

      // Encode this sample
      uint32_t encoded = ACTIVITY_RAW_SAMPLE_ENCODE(0, accel_data->x, accel_data->y, accel_data->z);
      if (data->run_size == 0) {
        // Start a new run
        data->run_size = 1;
        data->prev_sample = encoded;
      } else if (data->prev_sample == encoded) {
        // Continue a previous run
        data->run_size++;

        // If we've maxed out this run, terminate this run and start a new one
        if (data->run_size >= ACTIVITY_RAW_SAMPLE_MAX_RUN_SIZE) {
          ACTIVITY_RAW_SAMPLE_SET_RUN_SIZE(data->prev_sample, data->run_size);
          data->record.entries[data->record.num_entries++] = data->prev_sample;
          data->run_size = 0;
        }
      } else {
        // Finish the old run, start a new one
        ACTIVITY_RAW_SAMPLE_SET_RUN_SIZE(data->prev_sample, data->run_size);
        data->record.entries[data->record.num_entries++] = data->prev_sample;

        data->run_size = 1;
        data->prev_sample = encoded;
      }
    }

    // Save to data logging if the record is full now
    if (finish || data->record.num_entries >= ACTIVITY_RAW_SAMPLES_MAX_ENTRIES) {
      // Decrement num_samples if we already started building another run with the current sample
      data->record.num_samples -= data->run_size;
      if (finish) {
        data->record.flags |= ACTIVITY_RAW_SAMPLE_FLAG_LAST_RECORD;
      }
      DataLoggingResult result = dls_log(data->dls_session, &data->record, 1);
      if (result != DATA_LOGGING_SUCCESS) {
        PBL_LOG(LOG_LEVEL_WARNING, "Error %"PRIi32" while logging raw sample data",
                (int32_t)result);
      }

      // Generate a log message as well. This is temporary until we have better support to
      // send the DLS data to a server and retrieve it from there. The record itself
      // is about 112 bytes. Base64 encoded it becomes 112 * 4/3 = 150 bytes. That is too much
      // to fit in a single log line, so we split it into 2.
      uint32_t chunk_size = sizeof(data->record) / 2;
      uint8_t *binary_data = (uint8_t *)&data->record;
      int32_t num_chars = base64_encode(data->base64_buf, sizeof(data->base64_buf),
                                        binary_data, chunk_size);
      PBL_ASSERTN(num_chars + 1 < (int)sizeof(data->base64_buf));
      pbl_log(LOG_LEVEL_INFO, __FILE_NAME__, __LINE__, "RAW: %s", data->base64_buf);
      num_chars = base64_encode(data->base64_buf, sizeof(data->base64_buf),
                                binary_data + chunk_size, sizeof(data->record) - chunk_size);
      PBL_ASSERTN(num_chars + 1 < (int)sizeof(data->base64_buf));
      pbl_log(LOG_LEVEL_INFO, __FILE_NAME__, __LINE__, "RAW: %s", data->base64_buf);

      // Reset the stored record. 0 in num_samples causes it to be re-initialized
      // at the top of this loop.
      data->record.num_samples = 0;
    }

    if (finish) {
      break;
    }
  }
}


// ------------------------------------------------------------------------------------------------
// Accel callback. Called from KernelBG task. Feeds new samples into the algorithm, saves
// the updated step and sleep stats into our globals, and posts a service event if the steps
// have changed.
static void prv_accel_cb(AccelRawData *data, uint32_t num_samples, uint64_t timestamp) {
  // If the watch is vibrating, remove the movement
  if (vibes_get_vibe_strength() != VIBE_STRENGTH_OFF) {
    memset(data, 0, num_samples * sizeof(AccelRawData));
  }
  // Have the algorithm process the samples from KernelBG
  activity_algorithm_handle_accel(data, num_samples, timestamp);

  // Update our copy of the steps after grabbing the mutex. We guard these globals with a
  // mutex because activity_get_metric() provides access to the current metrics from any task.
  // The current sleep data is only recomputed every few minutes in order to reduce overhead and
  // is done so from prv_minute_system_task_cb()
  ActivityScalarStore prev_steps = s_activity_state.step_data.steps;
  mutex_lock_recursive(s_activity_state.mutex);
  {
    activity_algorithm_get_steps(&s_activity_state.step_data.steps);

    // Are we logging raw accel samples?
    if (s_activity_state.sample_collection_enabled) {
      prv_collect_raw_samples(data, num_samples, false /*finish*/);
    }

    // See if we have a stepping rate update from the algorithm. If so, accumulate the distance
    // covered.
    uint16_t rate_steps;
    uint32_t rate_elapsed_ms;
    time_t rate_update_time;
    activity_algorithm_get_step_rate(&rate_steps, &rate_elapsed_ms, &rate_update_time);
    if (rate_update_time != s_activity_state.rate_last_update_time) {
      s_activity_state.rate_last_update_time = rate_update_time;
      uint32_t distance_mm = activity_private_compute_distance_mm(rate_steps, rate_elapsed_ms);
      s_activity_state.distance_mm += distance_mm;
      s_activity_state.active_calories +=
          activity_private_compute_active_calories(distance_mm, rate_elapsed_ms);
    }
  }
  mutex_unlock_recursive(s_activity_state.mutex);

  if (s_activity_state.step_data.steps != prev_steps) {
    // Post a steps changed event
    PebbleEvent e = {
      .type = PEBBLE_HEALTH_SERVICE_EVENT,
      .health_event = {
        .type = HealthEventMovementUpdate,
        .data.movement_update = {
          .steps = s_activity_state.step_data.steps,
        },
      },
    };
    event_put(&e);
  }
}


// ------------------------------------------------------------------------------------------------
// Used by activity_test_feed_samples() to feed in accel samples manually for testing
static void prv_feed_samples_system_cb(void *context_in) {
  ActivityFeedSamples *context = (ActivityFeedSamples *)context_in;

  time_t time_s;
  uint16_t time_ms;
  rtc_get_time_ms(&time_s, &time_ms);
  uint64_t timestamp = ((uint64_t)time_s) * MS_PER_SECOND + time_ms;

  // Feed samples into the algorithm
  prv_accel_cb(context->data, context->num_samples, timestamp);
  kernel_free(context);

  s_activity_state.pending_test_cb = false;
}

// ------------------------------------------------------------------------------------------------
// NOTE: Caller must have lock
static void prv_stop_tracking_early(void) {
  // Don't do anything if we are already deinited.
  if (!s_activity_state.started) {
    return;
  }

  // Demands the underlying activity algorithms to clean up their act(ivity sessions) and save off
  // any new data they have (only to RAM...persisted to flash a few lines later).
  activity_algorithm_early_deinit();

  // Update storage before we close down
  s_activity_state.update_settings_counter = -1;
  prv_update_storage(rtc_get_time());

  PBL_LOG(LOG_LEVEL_DEBUG, "Updated and persisted sessions before stopping activity tracking");
}


// ------------------------------------------------------------------------------------------------
// Start activity tracking system callback
static void prv_start_tracking_cb(void *context) {
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);
  bool test_mode = (bool)context;
  activity_prefs_set_activated();

  s_activity_state.should_be_started = true;
  if (!prv_activity_allowed_to_be_enabled() || s_activity_state.started) {
    return;
  }

  AccelSamplingRate sampling_rate;
  if (activity_algorithm_init(&sampling_rate)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Starting activity tracking...");

    // Subscribe to the accelerometer from KernelBG
    s_activity_state.test_mode = test_mode;
    if (!test_mode) {
      PBL_ASSERTN(s_activity_state.accel_session == NULL);
      s_activity_state.accel_session = accel_session_create();
      accel_session_raw_data_subscribe(s_activity_state.accel_session, sampling_rate,
                                       ACTIVITY_ALGORITHM_MAX_SAMPLES, prv_accel_cb);

      // Subscribe to get heart rate updates and create our measurement logging
      // session if an hrm is present
      prv_heart_rate_init();
    }

    // Set the user data
    ActivityGender gender = activity_prefs_get_gender();
    uint16_t weight_dag = activity_prefs_get_weight_dag();
    uint16_t height_mm = activity_prefs_get_height_mm();
    uint8_t age_years = activity_prefs_get_age_years();
    activity_algorithm_set_user(height_mm,
                                weight_dag * 10,
                                gender,
                                age_years);
    activity_algorithm_metrics_changed_notification();

    // Register our minutes callback
    cron_job_schedule(&s_activity_job);
    s_activity_state.started = true;
    PBL_LOG(LOG_LEVEL_INFO, "Activity tracking started");

    PebbleEvent event = {
      .type = PEBBLE_ACTIVITY_EVENT,
      .activity_event = {
        .type = PebbleActivityEvent_TrackingStarted,
      },
    };
    event_put(&event);
  }
}


// ------------------------------------------------------------------------------------------------
// Stop activity tracking system callback
static void prv_stop_tracking_cb(void *context) {
  s_activity_state.should_be_started = false;
  if (!s_activity_state.started) {
    return;
  }

  cron_job_unschedule(&s_activity_job);
  if (s_activity_state.accel_session) {
    accel_session_data_unsubscribe(s_activity_state.accel_session);
    accel_session_delete(s_activity_state.accel_session);
    s_activity_state.accel_session = NULL;
  }

  // Close down heart rate support
  prv_heart_rate_deinit();

  PBL_ASSERTN(activity_algorithm_deinit());
  s_activity_state.started = false;
  PBL_LOG(LOG_LEVEL_INFO, "activity tracking stopped");

  PebbleEvent event = {
    .type = PEBBLE_ACTIVITY_EVENT,
    .activity_event = {
      .type = PebbleActivityEvent_TrackingStopped,
    },
  };
  event_put(&event);
}


// ------------------------------------------------------------------------------------------------
// Enable/disable activity service KernelBG callback. Used by activity_set_enabled().
static void prv_set_enable_cb(void *context) {
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);
  mutex_lock_recursive(s_activity_state.mutex);
  {
    const bool enable = prv_activity_allowed_to_be_enabled();

    if (enable == s_activity_state.started) {
      // No change in enabled state, we're done.
      goto cleanup;
    }

    if (enable) {
      // We just got enabled, re-start activity tracking if it should be in the started state
      if (s_activity_state.should_be_started) {
        prv_start_tracking_cb(NULL);
      }

    } else {
      // We just got disabled. Turn off activity tracking if necessary and set should_be_started
      // so that it gets restarted again once we get re-enabled.
      if (s_activity_state.started) {
        prv_stop_tracking_cb(NULL);
        // We want to turn tracking on again once we get re-enabled, so change the state of
        // should_be_started to true (prv_stop_tracking_cb() sets it to false).
        s_activity_state.should_be_started = true;
      }
    }
  }
cleanup:
  mutex_unlock_recursive(s_activity_state.mutex);
}

static void prv_handle_activity_enabled_change(void) {
  if (activity_tracking_on() && !prv_activity_allowed_to_be_enabled()) {
    prv_stop_tracking_early();
  }

  system_task_add_callback(prv_set_enable_cb, NULL);
}

static void prv_charger_event_cb(PebbleEvent *e, void *context) {
#if !IS_BIGBOARD
  // Since bigboards are usually plugged in, don't react to a battery connection event
  const PebbleBatteryStateChangeEvent *evt = &e->battery_state;
  mutex_lock_recursive(s_activity_state.mutex);
  {
    s_activity_state.enabled_charging_state = !evt->new_state.is_plugged;
  }
  mutex_unlock_recursive(s_activity_state.mutex);
  prv_handle_activity_enabled_change();
#endif
}

// -------------------------------------------------------------------------------------------
// Wait for an activity_algorithm call executed on KernelBG to complete
// The *context and *success booleans will be filled in by the callback.
static bool prv_wait_system_task(SystemTaskEventCallback cb, void *context, bool *cb_success,
                                 bool *cb_completed, uint32_t timeout_sec) {
  // This call blocks on KernelBG, so it should only be called from an app or worker
  PebbleTask task = pebble_task_get_current();
  PBL_ASSERTN(task == PebbleTask_App || task == PebbleTask_Worker);

  // Enqueue it for KernelBG to process
  bool success = system_task_add_callback(cb, context);
  if (!success) {
    return false;
  }

  RtcTicks end_ticks = rtc_get_ticks() + timeout_sec * configTICK_RATE_HZ;
  while (!(*cb_completed)) {
    // NOTE: we use while (!completed) and wait in 1 second chunks just in case the semaphore was
    // left set from an earlier call that timed out.
    if (rtc_get_ticks() > end_ticks) {
      return false;     // Timed out
    }
    const TickType_t k_timeout = configTICK_RATE_HZ;
    xSemaphoreTake(s_activity_state.bg_wait_semaphore, k_timeout);
  }

  return *cb_success;
}


// ------------------------------------------------------------------------------------------------
bool activity_init(void) {
  ACTIVITY_LOG_DEBUG("init");
  s_activity_state = (ActivityState) {};
  s_activity_state.mutex = mutex_create_recursive();

  // This semaphore used to wake up the calling task when it is waiting for KernelBG to
  // handle a request
  s_activity_state.bg_wait_semaphore = xSemaphoreCreateBinary();

    // Open up our settings file so that we can init our state
  SettingsFile *file = activity_private_settings_open();
  if (!file) {
    return false;
  }

  // Perform migration now if necessary
  uint16_t written_version;
  file = prv_settings_migrate(file, &written_version);
  if (!file) {
    return false;
  }

  // Write the new version
  ActivitySettingsKey key = ActivitySettingsKeyVersion;
  const uint16_t version = ACTIVITY_SETTINGS_CURRENT_VERSION;
  if (version != written_version) {
    settings_file_set(file, &key, sizeof(key), &version, sizeof(version));
  }

  // Init the current day index
  time_t utc_now = rtc_get_time();
  s_activity_state.cur_day_index = time_util_get_day(utc_now);

  // Roll back the history if needed and init each of the metrics for today
  activity_metrics_prv_init(file, utc_now);

  // Load in the saved activities
  activity_sessions_prv_init(file, utc_now);

  // Init variables used to compute the derived metrics
  s_activity_state.steps_per_minute_last_steps = s_activity_state.step_data.steps;
  s_activity_state.distance_mm = s_activity_state.step_data.distance_meters * MM_PER_METER;
  s_activity_state.active_calories = s_activity_state.step_data.active_kcalories
                                     * ACTIVITY_CALORIES_PER_KCAL;
  int minute_of_day = time_util_get_minute_of_day(utc_now);
  s_activity_state.resting_calories = activity_private_compute_resting_calories(minute_of_day);

  key = ActivitySettingsKeyLastSleepActivityUTC;
  settings_file_get(file, &key, sizeof(key),
                    &s_activity_state.logged_sleep_activity_exit_at_utc,
                    sizeof(s_activity_state.logged_sleep_activity_exit_at_utc));

  key = ActivitySettingsKeyLastRestfulSleepActivityUTC;
  settings_file_get(file, &key, sizeof(key),
                    &s_activity_state.logged_restful_sleep_activity_exit_at_utc,
                    sizeof(s_activity_state.logged_restful_sleep_activity_exit_at_utc));

  key = ActivitySettingsKeyLastStepActivityUTC;
  settings_file_get(file, &key, sizeof(key),
                    &s_activity_state.logged_step_activity_exit_at_utc,
                    sizeof(s_activity_state.logged_step_activity_exit_at_utc));

  // Clean up
  activity_private_settings_close(file);

  // Init insights
  activity_insights_init(utc_now);

  // Set up charger subscription and check right now if charger is connected
  s_activity_state.charger_subscription = (EventServiceInfo) {
    .type = PEBBLE_BATTERY_STATE_CHANGE_EVENT,
    .handler = prv_charger_event_cb,
  };
  event_service_client_subscribe(&s_activity_state.charger_subscription);
#if IS_BIGBOARD
  s_activity_state.enabled_charging_state = true;
#else
  s_activity_state.enabled_charging_state = !battery_is_usb_connected();
#endif


  return true;
}


// ------------------------------------------------------------------------------------------------
bool activity_start_tracking(bool test_mode) {
  return system_task_add_callback(prv_start_tracking_cb, (void *)test_mode);
}


// ------------------------------------------------------------------------------------------------
bool activity_stop_tracking(void) {
  mutex_lock_recursive(s_activity_state.mutex);
  {
    prv_stop_tracking_early();
  }
  mutex_unlock_recursive(s_activity_state.mutex);
  return system_task_add_callback(prv_stop_tracking_cb, NULL);
}


// ------------------------------------------------------------------------------------------------
bool activity_tracking_on(void) {
  bool result;
  mutex_lock_recursive(s_activity_state.mutex);
  result = s_activity_state.started;
  mutex_unlock_recursive(s_activity_state.mutex);
  return result;
}


// ------------------------------------------------------------------------------------------------
// Enable/disable this service. Used by the service manager's services_set_runlevel() call.
// Note that this can be called from a timer callback so we do all the heavy lifting from a
// kernel BG callback
void activity_set_enabled(bool enable) {
  mutex_lock_recursive(s_activity_state.mutex);
  {
    s_activity_state.enabled_run_level = enable;
  }
  mutex_unlock_recursive(s_activity_state.mutex);
  prv_handle_activity_enabled_change();
}


// ------------------------------------------------------------------------------------------------
bool activity_get_sessions(uint32_t *session_entries, ActivitySession *sessions) {
  if (sessions == NULL) {
    return false;
  }
  mutex_lock_recursive(s_activity_state.mutex);
  {
    uint32_t num_sessions_to_return = MIN(*session_entries,
                                          s_activity_state.activity_sessions_count);

    memcpy(sessions, s_activity_state.activity_sessions,
           num_sessions_to_return * sizeof(ActivitySession));
    *session_entries = num_sessions_to_return;
  }
  mutex_unlock_recursive(s_activity_state.mutex);
  return true;
}


// ------------------------------------------------------------------------------------------------
DEFINE_SYSCALL(bool, sys_activity_get_sessions, uint32_t *session_entries,
               ActivitySession *sessions) {
  if (PRIVILEGE_WAS_ELEVATED) {
    if (session_entries) {
      syscall_assert_userspace_buffer(session_entries, sizeof(*session_entries));
    }
    if (sessions) {
      syscall_assert_userspace_buffer(sessions, sizeof(*sessions) * (*session_entries));
    }
  }

  return activity_get_sessions(session_entries, sessions);
}


// ------------------------------------------------------------------------------------------------
DEFINE_SYSCALL(bool, sys_activity_prefs_heart_rate_is_enabled, void) {
  return activity_prefs_heart_rate_is_enabled();
}


// ------------------------------------------------------------------------------------------------
typedef struct {
  HealthMinuteData *minute_data;
  uint32_t *num_records;
  time_t *utc_start;
  bool success;
  bool completed;
} ActivityGetMinuteHistoryContext;

static void prv_get_minute_history_system_cb(void *context_param) {
  ActivityGetMinuteHistoryContext *context = (ActivityGetMinuteHistoryContext *)context_param;

  // Get the minute history
  if (s_activity_state.started) {
    context->success = activity_algorithm_get_minute_history(context->minute_data,
                                                             context->num_records,
                                                             context->utc_start);
  } else {
    context->success = false;
  }

  // Unblock the caller
  context->completed = true;
  xSemaphoreGive(s_activity_state.bg_wait_semaphore);
}

bool activity_get_minute_history(HealthMinuteData *minute_data, uint32_t *num_records,
                                 time_t *utc_start) {
  // Fill in the context
  ActivityGetMinuteHistoryContext context = (ActivityGetMinuteHistoryContext) {
    .minute_data = minute_data,
    .num_records = num_records,
    .utc_start = utc_start,
  };

  // Enqueue it for KernelBG to process
  bool success = prv_wait_system_task(prv_get_minute_history_system_cb, &context, &context.success,
                                      &context.completed, 30 /*timeout_sec*/);
  return success;
}


// ------------------------------------------------------------------------------------------------
DEFINE_SYSCALL(bool, sys_activity_get_minute_history, HealthMinuteData *minute_data,
               uint32_t *num_records, time_t *utc_start) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(utc_start, sizeof(*utc_start));
    syscall_assert_userspace_buffer(num_records, sizeof(*num_records));
    syscall_assert_userspace_buffer(minute_data, *num_records * sizeof(HealthMinuteData));
  }

  return activity_get_minute_history(minute_data, num_records, utc_start);
}


// ------------------------------------------------------------------------------------------------
bool activity_get_step_averages(DayInWeek day_of_week, ActivityMetricAverages *averages) {
  return health_db_get_typical_step_averages(day_of_week, averages);
}


// ------------------------------------------------------------------------------------------------
DEFINE_SYSCALL(bool, sys_activity_get_step_averages, DayInWeek day_of_week,
               ActivityMetricAverages *averages) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(averages, sizeof(*averages));
  }

  return activity_get_step_averages(day_of_week, averages);
}

// ------------------------------------------------------------------------------------------------
bool activity_get_metric_typical(ActivityMetric metric, DayInWeek day, int32_t *value_out) {
  *value_out = 0;
  return health_db_get_typical_value(metric, day, value_out);
}

// ------------------------------------------------------------------------------------------------
bool activity_get_metric_monthly_avg(ActivityMetric metric, int32_t *value_out) {
  *value_out = 0;
  return health_db_get_monthly_average_value(metric, value_out);
}

// ------------------------------------------------------------------------------------------------
bool activity_raw_sample_collection(bool enable, bool disable, bool *enabled,
                                    uint32_t *session_id, uint32_t *num_samples,
                                    uint32_t *seconds) {
  bool success = true;
  mutex_lock_recursive(s_activity_state.mutex);
  {
    if (enable && !s_activity_state.sample_collection_enabled) {
      ActivitySampleCollectionData *data = kernel_zalloc_check(
                                                sizeof(ActivitySampleCollectionData));
      s_activity_state.sample_collection_data = data;
      data->first_record = true;
      s_activity_state.sample_collection_session_id++;
      s_activity_state.sample_collection_seconds = rtc_get_time();
      s_activity_state.sample_collection_num_samples = 0;
      s_activity_state.sample_collection_enabled = true;
    }
    if (disable && s_activity_state.sample_collection_enabled) {
      // Finish up the current record
      s_activity_state.sample_collection_enabled = false;
      prv_collect_raw_samples(NULL, 0, true /*finish*/);
      ActivitySampleCollectionData *data = s_activity_state.sample_collection_data;
      if (data->dls_session) {
        dls_finish(data->dls_session);
      }
      kernel_free(data);
      s_activity_state.sample_collection_data = NULL;
      s_activity_state.sample_collection_seconds = rtc_get_time()
                                                 - s_activity_state.sample_collection_seconds;
    }
    *enabled = s_activity_state.sample_collection_enabled;
    *session_id = s_activity_state.sample_collection_session_id;
    *num_samples = s_activity_state.sample_collection_num_samples;
    if (*enabled) {
      *seconds = rtc_get_time() - s_activity_state.sample_collection_seconds;
    } else {
      *seconds = s_activity_state.sample_collection_seconds;
    }
  }
  mutex_unlock_recursive(s_activity_state.mutex);
  return success;
}


// ------------------------------------------------------------------------------------------------
// Get info on the sleep file
typedef struct {
  bool success;
  bool completed;
} ActivityDumpSleepLogContext;

static void prv_dump_sleep_log_system_cb(void *context_param) {
  ActivityDumpSleepLogContext *context = (ActivityDumpSleepLogContext *)context_param;

  // Get the sleep info
  if (s_activity_state.started) {
    context->success = activity_algorithm_dump_minute_data_to_log();
  } else {
    context->success = false;
  }

  // Unblock the caller
  context->completed = true;
  xSemaphoreGive(s_activity_state.bg_wait_semaphore);
}

bool activity_dump_sleep_log(void) {
  // Fill in the context
  ActivityDumpSleepLogContext context = (ActivityDumpSleepLogContext) { };

  // Enqueue it for KernelBG to process
  bool success = prv_wait_system_task(prv_dump_sleep_log_system_cb, &context, &context.success,
                                      &context.completed, 30 /*timeout_sec*/);
  return success;
}


// ------------------------------------------------------------------------------------------------
bool activity_test_feed_samples(AccelRawData *data, uint32_t num_samples) {
  if (!s_activity_state.test_mode) {
    PBL_LOG(LOG_LEVEL_ERROR, "not in test mode");
    return false;
  }

  PBL_ASSERT(s_activity_state.started, "not started");

  while (num_samples) {
    while (s_activity_state.pending_test_cb) {
      sys_psleep(1);         // Wait for kernelBG to process prior data
    }

    uint32_t chunk_size = MIN(ACTIVITY_ALGORITHM_MAX_SAMPLES, num_samples);

    // Allocate space for the samples
    uint16_t req_size = sizeof(ActivityFeedSamples) + chunk_size * sizeof(AccelRawData);
    ActivityFeedSamples *context = kernel_malloc(req_size);
    if (!context) {
      PBL_LOG(LOG_LEVEL_ERROR, "Not enough memory");
      return false;
    }

    context->num_samples = chunk_size;
    memmove(context->data, data, chunk_size * sizeof(AccelRawData));
    s_activity_state.pending_test_cb = true;
    system_task_add_callback(prv_feed_samples_system_cb, context);

    data += chunk_size;
    num_samples -= chunk_size;
  }
  return true;
}


// ------------------------------------------------------------------------------------------------
bool activity_test_run_minute_callback(void) {
  return system_task_add_callback(prv_minute_system_task_cb, NULL);
}


// ------------------------------------------------------------------------------------------------
// Writes history to settings file
static void prv_write_metric_history(ActivitySettingsKey key,
                                     const ActivitySettingsValueHistory *history) {
  SettingsFile *file = activity_private_settings_open();
  if (file) {
    settings_file_set(file, &key, sizeof(key), history, sizeof(*history));
    activity_private_settings_close(file);
  }
}


// ------------------------------------------------------------------------------------------------
// Used by unit tests to init state. Clears all stored data and re-initializes.
bool activity_test_reset(bool reset_settings, bool tracking_on,
                         const ActivitySettingsValueHistory *sleep_history,
                         const ActivitySettingsValueHistory *step_history) {
  bool tracking = s_activity_state.started || tracking_on;
  bool test_mode = s_activity_state.test_mode;

  activity_stop_tracking();
  while (s_activity_state.started) {
    // Wait for stop_tracking KernelBG callback to run
    sys_psleep(1);
  }
  cron_job_unschedule(&s_activity_job);
  mutex_destroy((PebbleMutex *)s_activity_state.mutex);
  if (reset_settings) {
    pfs_remove(ACTIVITY_SETTINGS_FILE_NAME);
  }

  if (sleep_history) {
    prv_write_metric_history(ActivitySettingsKeySleepTotalMinutesHistory, sleep_history);
  }
  if (step_history) {
    prv_write_metric_history(ActivitySettingsKeyStepCountHistory, step_history);
  }
  activity_init();
  activity_set_enabled(true);

  // Restart tracking
  if (tracking) {
    activity_start_tracking(test_mode);
    while (!s_activity_state.started) {
      sys_psleep(1);
    }
  }
  return true;
}


// ------------------------------------------------------------------------------------------------
// Get info on the sleep file
typedef struct {
  bool compact_first;
  uint32_t num_records;
  uint32_t data_bytes;
  uint32_t minutes;
  bool success;
  bool completed;
} ActivitySleepFileInfoContext;

static void prv_sleep_file_info_system_cb(void *context_param) {
  ActivitySleepFileInfoContext *context = (ActivitySleepFileInfoContext *)context_param;

  // Get the sleep info
  if (s_activity_state.started) {
    context->success = activity_algorithm_minute_file_info(
      context->compact_first, &context->num_records, &context->data_bytes, &context->minutes);
  } else {
    context->success = false;
  }

  // Unblock the caller
  context->completed = true;
  xSemaphoreGive(s_activity_state.bg_wait_semaphore);
}

bool activity_test_minute_file_info(bool compact_first, uint32_t *num_records, uint32_t *data_bytes,
                                    uint32_t *minutes) {
  // Fill in the context
  ActivitySleepFileInfoContext context = (ActivitySleepFileInfoContext) {
    .compact_first = compact_first,
  };

  // Enqueue it for KernelBG to process
  bool success = prv_wait_system_task(prv_sleep_file_info_system_cb, &context, &context.success,
                                      &context.completed, 30 /*timeout_sec*/);
  if (success) {
    *num_records = context.num_records;
    *data_bytes = context.data_bytes;
    *minutes = context.minutes;
  } else {
    *num_records = 0;
    *data_bytes = 0;
    *minutes = 0;
  }
  return success;
}


// ------------------------------------------------------------------------------------------------
// Fill the sleep file
typedef struct {
  bool success;
  bool completed;
} ActivityFillSleepFileContext;

static void prv_fill_minute_file_system_cb(void *context_param) {
  ActivityFillSleepFileContext *context = (ActivityFillSleepFileContext *)context_param;

  // Get the sleep info
  if (s_activity_state.started) {
    context->success = activity_algorithm_test_fill_minute_file();
  } else {
    context->success = false;
  }

  // Unblock the caller
  context->completed = true;
  xSemaphoreGive(s_activity_state.bg_wait_semaphore);
}

bool activity_test_fill_minute_file(void) {
  // Fill in the context
  ActivitySleepFileInfoContext context = (ActivitySleepFileInfoContext) { };

  // Enqueue it for KernelBG to process
  bool success = prv_wait_system_task(prv_fill_minute_file_system_cb, &context, &context.success,
                                      &context.completed, 300 /*timeout_sec*/);
  return success;
}


// ------------------------------------------------------------------------------------------------
// Send a fake data logging records
static void prv_send_fake_dls_records_system_cb(void *context_param) {
  // Send a fake legacy sleep logging record
  time_t utc_now = rtc_get_time();
  time_t session_start_utc = utc_now - (4 * SECONDS_PER_HOUR);

  // Send one of each activity type
  for (ActivitySessionType activity = ActivitySessionType_Sleep;
       activity < ActivitySessionTypeCount; activity++) {
    ActivitySession session = {
      .start_utc = session_start_utc,
      .length_min = 10,
      .type = activity,
    };
    activity_sessions_prv_send_activity_session_to_data_logging(&session);
    session_start_utc += 20 * SECONDS_PER_MINUTE;
  }

  // Send a fake minute-data record
  activity_algorithm_test_send_fake_minute_data_dls_record();
}

bool activity_test_send_fake_dls_records(void) {
  // Enqueue it for KernelBG to process
  return system_task_add_callback(prv_send_fake_dls_records_system_cb, NULL);
}


// ------------------------------------------------------------------------------------------------
void activity_test_set_steps_and_avg(int32_t new_steps, int32_t current_avg, int32_t daily_avg) {
  mutex_lock_recursive(s_activity_state.mutex);
  {
    // set the current steps to new_steps
    s_activity_state.step_data.steps = new_steps;
    activity_algorithm_metrics_changed_notification();

    // set all the step average values in the DB to 0 (except the first and last key)
    // The first key is set to the current_avg so that the current step average will always be at
    // current_avg. The last key is set to daily_avg - current_avg so that the total daily_avg will
    // always be at daily_avg
    const time_t now = rtc_get_time();
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    DayInWeek day_of_week = local_tm.tm_wday;

    uint16_t step_avg_array[ACTIVITY_STEP_AVERAGES_PER_KEY] = {};
    for (int i = 0; i < ACTIVITY_STEP_AVERAGES_PER_KEY; i++) {
      step_avg_array[i] = 0;
    }

    step_avg_array[0] = current_avg;
    step_avg_array[ACTIVITY_STEP_AVERAGES_PER_KEY - 1] = daily_avg - current_avg;
    health_db_set_typical_values(ActivityMetricStepCount,
                                 day_of_week,
                                 step_avg_array,
                                 ACTIVITY_STEP_AVERAGES_PER_KEY);
  }
  mutex_unlock_recursive(s_activity_state.mutex);
}


// ------------------------------------------------------------------------------------------------
void activity_test_set_steps_history() {
  ActivitySettingsValueHistory step_history = {
    .utc_sec = rtc_get_time(),
    .values = {
      0, // This ends up overwritten anyway by the current sleep value
      1000,
      750,
      1250,
      500,
      2000,
      3000
    }
  };

  prv_write_metric_history(ActivitySettingsKeyStepCountHistory, &step_history);
}


// ------------------------------------------------------------------------------------------------
void activity_test_set_sleep_history() {
  ActivitySettingsValueHistory sleep_history = {
    .utc_sec = rtc_get_time(),
    .values = {
      0, // This ends up overwritten anyway by the current sleep value
      400,
      500,
      400,
      500,
      400,
      500,
    }
  };

  prv_write_metric_history(ActivitySettingsKeySleepTotalMinutesHistory, &sleep_history);
}
