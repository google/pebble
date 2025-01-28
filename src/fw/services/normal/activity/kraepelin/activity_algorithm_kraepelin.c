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
#include "util/uuid.h"
#include "drivers/ambient_light.h"
#include "kernel/pbl_malloc.h"
#include "services/common/battery/battery_state.h"
#include "services/common/system_task.h"
#include "services/normal/activity/activity_algorithm.h"
#include "services/normal/activity/activity_private.h"
#include "services/normal/data_logging/data_logging_service.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/settings/settings_file.h"
#include "syscall/syscall.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/base64.h"
#include "util/math.h"
#include "util/shared_circular_buffer.h"
#include "util/size.h"
#include "util/time/time.h"
#include "util/units.h"

#include "activity_algorithm_kraepelin.h"
#include "kraepelin_algorithm.h"

// NOTE: This file is called "activity_sleep" for legacy reasons. A better name now would be
// something like "activity_minute_data", but we want to maintain compatibility with prior
// releases that only used it for sleep data.
#define ALG_MINUTE_DATA_FILE_NAME  "activity_sleep"


// How many records we need to store in our circular buffer
// +1 for mgmt overhead
#define ALG_MINUTE_CBUF_NUM_RECORDS  (MAX(ALG_MINUTES_PER_DLS_RECORD, ALG_MINUTES_PER_FILE_RECORD) \
                                       + KALG_MAX_UNCERTAIN_SLEEP_M + 1)

// ---------------------------------------------------------------------------------------------
// Globals
typedef struct {
  PebbleRecursiveMutex *mutex;

  KAlgState *k_state;  // Pointer to Kraepelin state variables

  // Accumulated steps
  int32_t steps;

  // Last computed step rate information
  uint8_t rate_steps;
  uint16_t rate_elapsed_ms;
  time_t  rate_computed_time_s;

  // Minute data
  uint16_t minute_steps;

  // The data logging session and record we use to send minute data to the phone
  DataLoggingSession *dls_session;
  AlgMinuteDLSRecord dls_record;
  AlgMinuteFileRecord file_record;

  // How many records we have in our minute data settings file
  uint16_t num_minute_records;

  // Metrics that we compute minute deltas of
  uint32_t prev_distance_mm;
  uint32_t prev_resting_calories;
  uint32_t prev_active_calories;

  // We hold the last N minutes of minute data in this circular buffer so that we can go
  // back and zero out the steps in older minutes once we determine that we were definitely
  // asleep for those minutes (there can be a KALG_MAX_AWAKE_LAG minute lag before we figure out
  // that we woke up).
  SharedCircularBuffer minute_data_cbuf;
  AlgMinuteRecord minute_data_storage[ALG_MINUTE_CBUF_NUM_RECORDS];

  SharedCircularBufferClient file_minute_data_client;
  SharedCircularBufferClient dls_minute_data_client;
  AlgMinuteRecord cbuf_record;  // space for tmp record here to decrease stack requirements
} AlgState;
static AlgState *s_alg_state = NULL;


// ----------------------------------------------------------------------------------------------
static bool prv_lock(void) {
  if (!s_alg_state) {
#if RELEASE
    PBL_LOG(LOG_LEVEL_ERROR, "Trying to use the activity algorithm but it hasn't been initialized");
    return false;
#else
    WTF;
#endif
  }
  mutex_lock_recursive(s_alg_state->mutex);
  return true;
}

static void prv_unlock(void) {
  mutex_unlock_recursive(s_alg_state->mutex);
}

// ----------------------------------------------------------------------------------------------
// Open the minute data settings file and malloc space for the file struct
// We use NOINLINE to reduce the stack requirements during the minute handler (see PBL-38130)
static NOINLINE SettingsFile *prv_minute_data_file_open(void) {
  SettingsFile *file = kernel_malloc_check(sizeof(SettingsFile));
  if (settings_file_open(file, ALG_MINUTE_DATA_FILE_NAME, ALG_MINUTE_DATA_FILE_LEN) != S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "No minute data file");
    return NULL;
  }
  return file;
}


// ------------------------------------------------------------------------------------------------
// Close the settings file and free the file struct
static void prv_minute_data_file_close(SettingsFile *file) {
  settings_file_close(file);
  kernel_free(file);
}


// --------------------------------------------------------------------------------------------
// Return the settings file key associated with a particular UTC timestamp. Each entry
// holds ALG_MINUTES_PER_RECORD minutes of data. To get the key index, we divide the UTC
// time by ALG_MINUTES_PER_RECORD.
static uint32_t prv_minute_file_get_settings_key(time_t utc) {
  uint32_t seconds_per_key = ALG_MINUTES_PER_FILE_RECORD * SECONDS_PER_MINUTE;
  return utc / seconds_per_key;
}


// ----------------------------------------------------------------------------------------------
// Callback provided to kalg_activities_update to create activity sessions.
static void prv_create_activity_session_cb(void *context, KAlgActivityType kalg_activity,
                                           time_t start_utc, uint32_t len_sec, bool ongoing,
                                           bool delete, uint32_t steps, uint32_t resting_calories,
                                           uint32_t active_calories, uint32_t distance_mm) {
  // Translate to one of the activity.h activity types
  ActivitySessionType activity = ActivitySessionTypeCount;
  switch (kalg_activity) {
    case KAlgActivityType_Walk:
      activity = ActivitySessionType_Walk;
      break;
    case KAlgActivityType_Run:
      activity = ActivitySessionType_Run;
      break;
    case KAlgActivityType_RestfulSleep:
      activity = ActivitySessionType_RestfulSleep;
      break;
    case KAlgActivityType_Sleep:
      activity = ActivitySessionType_Sleep;
      break;
    case KAlgActivityTypeCount:
      WTF;
      break;
  }
  PBL_ASSERTN(activity != ActivitySessionTypeCount);

  ActivitySession session = {
    .type = activity,
    .start_utc = start_utc,
    .length_min = len_sec / SECONDS_PER_MINUTE,
    .ongoing = ongoing,
    .step_data = {
      .steps = steps,
      .active_kcalories = ROUND(active_calories, ACTIVITY_CALORIES_PER_KCAL),
      .resting_kcalories = ROUND(resting_calories, ACTIVITY_CALORIES_PER_KCAL),
      .distance_meters = ROUND(distance_mm, MM_PER_METER),
    },
  };
  if (delete) {
    activity_sessions_prv_delete_activity_session(&session);
  } else {
    activity_sessions_prv_add_activity_session(&session);
  }
}


// ------------------------------------------------------------------------------------------
// Used from settings_file_each() callback to read in a chunk based on the SettingsRecordInfo
// given to the callback. Returns true if the chunk is within the designated key range and
// should be processed.
static bool prv_read_minute_file_record(SettingsFile *file, SettingsRecordInfo *info,
                                        uint32_t key_range_start, uint32_t key_range_end,
                                        AlgMinuteFileRecord *chunk) {
  // Get the key for this record and see if we want it
  uint32_t key;
  info->get_key(file, &key, sizeof(key));
  if (key < key_range_start || key > key_range_end) {
    return false;
  }

  // Check the contents and process it
  if (info->val_len != sizeof(*chunk)) {
    return false;
  }
  info->get_val(file, chunk, sizeof(*chunk));

  // Skip invalid keys
  if (chunk->hdr.version != ALG_MINUTE_FILE_RECORD_VERSION) {
    return false;
  }

  return true;
}


// ----------------------------------------------------------------------------------------------
// The callback we give to settings_file_each to send the minute data to the logs
typedef struct {
  uint32_t oldest_key;
  uint32_t newest_key;

  time_t oldest_valid_utc;
  time_t newest_valid_utc;
} AlgLogMinuteFileContext;

static bool prv_log_minute_file_minutes_cb(SettingsFile *file, SettingsRecordInfo *info,
                                            void *context_param) {
  AlgLogMinuteFileContext *context = (AlgLogMinuteFileContext *)context_param;

  AlgMinuteFileRecord chunk;
  if (!prv_read_minute_file_record(file, info, context->oldest_key, context->newest_key, &chunk)) {
    return true;
  }

  // if in the wrong time range, skip it
  if (chunk.hdr.time_utc < (uint32_t)context->oldest_valid_utc
      || chunk.hdr.time_utc > (uint32_t)context->newest_valid_utc) {
    ACTIVITY_LOG_DEBUG("Minute chunk time out of range, skipping it");
    return true;
  }

  // We need to make it 33% bigger for base64 encoding (3 binary -> 4 characters)
  // Enough for half the base64 encoded message
  char base64_buf[sizeof(AlgMinuteFileRecord)];
  uint32_t chunk_size = sizeof(AlgMinuteFileRecord) / 2;
  uint8_t *binary_data = (uint8_t *)&chunk;

  int32_t num_chars = base64_encode(base64_buf, sizeof(base64_buf), binary_data, chunk_size);
  PBL_ASSERTN(num_chars + 1 < (int)sizeof(base64_buf));
  // NOTE: we use pbl_log_sync instead of PBL_LOG because we don't want these messages
  // hashed. Hashing them doesn't save any space and requires that you unhash before you can
  // parse the data out of the logs.
  pbl_log_sync(LOG_LEVEL_INFO, __FILE_NAME__, __LINE__, "SLP: %s", base64_buf);

  num_chars = base64_encode(base64_buf, sizeof(base64_buf), binary_data + chunk_size,
                            sizeof(AlgMinuteFileRecord) - chunk_size);
  PBL_ASSERTN(num_chars + 1 < (int)sizeof(base64_buf));
  pbl_log_sync(LOG_LEVEL_INFO, __FILE_NAME__, __LINE__, "SLP: %s", base64_buf);
  return true;
}


// ----------------------------------------------------------------------------------------------
// Log minute data to PBL_LOG
// @param earliest_wake_time ignore any sleep cycles that end before this time.
bool activity_algorithm_dump_minute_data_to_log(void) {
  if (!prv_lock()) {
    return false;
  }

  bool success = false;
  SettingsFile *file = NULL;

  // Open the minute data file
  file = prv_minute_data_file_open();
  if (!file) {
    goto exit;
  }

  // Figure out the oldest and newest possible time stamp for chunks that go into these buffers
  time_t now = rtc_get_time();
  const time_t k_oldest_valid_utc = now
                                  - ALG_SLEEP_HISTORY_HOURS_FOR_TODAY * SECONDS_PER_HOUR;
  const time_t k_newest_valid_utc = now;

  AlgLogMinuteFileContext context = (AlgLogMinuteFileContext) {
    .oldest_key = prv_minute_file_get_settings_key(k_oldest_valid_utc) - 1,
    .newest_key = prv_minute_file_get_settings_key(k_newest_valid_utc) + 1,
    .oldest_valid_utc = k_oldest_valid_utc,
    .newest_valid_utc = k_newest_valid_utc,
  };

  // Feed in the saved data, reading chunks out of the saved minute data and compressing
  // it into algorithm sleep minute structures.
  status_t status = settings_file_each(file, prv_log_minute_file_minutes_cb, &context);
  success = (status == S_SUCCESS);

exit:
  if (file) {
    prv_minute_data_file_close(file);
  }
  prv_unlock();
  return success;
}


// ----------------------------------------------------------------------------------------------
// Settings file rewrite callback used by prv_validate_and_trim_minute_file() when trimming off
// old keys
typedef struct {
  uint32_t oldest_valid_key;
  uint32_t newest_valid_key;
  uint16_t num_keys_kept;
  int32_t watchdog_kicks_left;
} AlgMinuteFileRewriteContext;

static bool prv_minute_file_rewrite_cb(void *key_arg, size_t key_len, void *val_arg,
                                       size_t value_len, void *context_arg) {
  AlgMinuteFileRewriteContext *context = (AlgMinuteFileRewriteContext *)context_arg;
  AlgMinuteFileRecord *val = (AlgMinuteFileRecord *)val_arg;
  uint32_t key = *(uint32_t *)key_arg;

  if (val->hdr.version != ALG_MINUTE_FILE_RECORD_VERSION) {
    ACTIVITY_LOG_DEBUG("Dropping key %"PRIu32", invalid version of %"PRIu16"", key,
                       val->hdr.version);
    return false;
  }

  if (key < context->oldest_valid_key || key > context->newest_valid_key) {
    ACTIVITY_LOG_DEBUG("Dropping key %"PRIu32", record UTC of %"PRIu32"", key,
                       val->hdr.time_utc);
    return false;
  }

  // This can take a while, so periodically tickle the KernelBG watchdog.
  if (context->watchdog_kicks_left > 0) {
    // Check counter as insurance against getting into an infinite loop and still tickling the
    // watchdog.
    system_task_watchdog_feed();
    context->watchdog_kicks_left--;
  }

  context->num_keys_kept++;
  return true;
}


// ----------------------------------------------------------------------------------------------
// Scan the existing minute file, validate it, keep only the most recent 'max_records' records
// If the file handle is not NULL, it will be used. Otherwise, the file will be opened and then
// closed again before we exit.
static SettingsFile *prv_validate_and_trim_minute_file(SettingsFile *file, uint16_t max_records) {
  bool need_close = false;
  bool nuke_file = false;

  // Reset total # of records we have. We will update this after we scan the file
  s_alg_state->num_minute_records = 0;

  // Open settings file containing our minute data
  if (file == NULL) {
    need_close = true;
    file = prv_minute_data_file_open();
    if (file == NULL) {
      goto exit;
    }
  }

  // Figure out which keys we want to keep
  time_t utc = rtc_get_time();
  uint32_t newest_valid_key = prv_minute_file_get_settings_key(utc) + 1;
  int32_t oldest_valid_key = (int32_t)newest_valid_key - max_records;
  oldest_valid_key = MAX(0, oldest_valid_key);
  AlgMinuteFileRewriteContext context = (AlgMinuteFileRewriteContext) {
    .oldest_valid_key = oldest_valid_key,
    .newest_valid_key = newest_valid_key,
    .watchdog_kicks_left = max_records,
  };

  // Rewrite the settings file, keeping only the keys we need
  PBL_LOG(LOG_LEVEL_DEBUG, "Compacting minute file down to %"PRIu16" records", max_records);
  status_t status = settings_file_rewrite_filtered(file, prv_minute_file_rewrite_cb, &context);

  // Error re-writing?
  if (status != S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Encountered error %"PRIi32" rewriting settings file",
            (int32_t)status);
    nuke_file = true;
  } else {
    s_alg_state->num_minute_records = context.num_keys_kept;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Compaction done, ended up with %"PRIu16" records",
          s_alg_state->num_minute_records);

exit:
  if (file && (need_close || nuke_file)) {
    prv_minute_data_file_close(file);
    file = NULL;
  }

  if (nuke_file) {
    PBL_LOG(LOG_LEVEL_WARNING, "Detected invalid minute data file, deleting it");
    pfs_remove(ALG_MINUTE_DATA_FILE_NAME);
  }
  return file;
}


// -------------------------------------------------------------------------------------
static void prv_init_minute_record(AlgMinuteRecordHdr *hdr, time_t utc_sec, bool for_file) {
  time_t local_time = time_utc_to_local(utc_sec);
  int16_t local_time_offset_15_min = (local_time - utc_sec) / (15 * SECONDS_PER_MINUTE);

  *hdr = (AlgMinuteRecordHdr) {
    .version = for_file ? ALG_MINUTE_FILE_RECORD_VERSION : ALG_DLS_MINUTES_RECORD_VERSION,
    .time_utc = utc_sec,
    .time_local_offset_15_min = local_time_offset_15_min,
    .sample_size = for_file ? sizeof(AlgMinuteFileSample) : sizeof(AlgMinuteDLSSample),
  };
}


// -------------------------------------------------------------------------------------
// We use NOINLINE to reduce the stack requirements during the minute handler (see PBL-38130)
static void NOINLINE prv_set_file_minute_record_entry(AlgMinuteFileRecord *file_record,
                                             AlgMinuteDLSSample *data, uint16_t sample_idx,
                                             time_t sample_utc, bool was_sleeping) {
  if (sample_idx == 0) {
    // If first record, init the header
    prv_init_minute_record(&file_record->hdr, sample_utc, true /*to_file*/);
  }

  // Put in this minute
  file_record->samples[sample_idx].v5_fields = data->base;
  file_record->samples[sample_idx].heart_rate_bpm = data->heart_rate_bpm;
  file_record->hdr.num_samples = sample_idx + 1;

  if (was_sleeping) {
    // Zero out if we were sleeping in this minute
    file_record->samples[sample_idx].v5_fields.steps = 0;
    file_record->samples[sample_idx].v5_fields.active = false;
  }
}


// ------------------------------------------------------------------------------------
// Add a record to the minute file
static bool prv_write_minute_file_record(AlgMinuteFileRecord *file_record) {
  bool success = false;

  SettingsFile *minute_file = prv_minute_data_file_open();
  if (!minute_file) {
    PBL_LOG(LOG_LEVEL_ERROR, "Could not open minute file for saving minute stats");
    goto exit;
  }

  uint32_t key = prv_minute_file_get_settings_key(file_record->hdr.time_utc);
  status_t status = settings_file_set(minute_file, &key, sizeof(key), file_record,
                                      sizeof(*file_record));
  if (status == E_OUT_OF_STORAGE) {
    uint16_t max_records = s_alg_state->num_minute_records / 2;
    PBL_LOG(LOG_LEVEL_INFO, "Compacting minute file from %"PRIu16" records to %"PRIu16"",
            s_alg_state->num_minute_records, max_records);
    minute_file = prv_validate_and_trim_minute_file(minute_file, max_records);
    if (!minute_file) {
      goto exit;
    }

    status = settings_file_set(minute_file, &key, sizeof(key), file_record, sizeof(*file_record));
  }

  // Was there an error writing the value out?
  if (status != S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error %"PRIi32" writing out minute data to minute file",
            (int32_t)status);
  } else {
    s_alg_state->num_minute_records++;
    success = true;
  }

exit:
  if (minute_file) {
    prv_minute_data_file_close(minute_file);
  }
  return success;
}


// -------------------------------------------------------------------------------------
static DataLoggingSession *prv_get_dls_minute_session(void) {
  // Open up the data logging session if we don't have one
  if (s_alg_state->dls_session == NULL) {
    // We don't need to be buffered since we are logging from the KernelBG task and this
    // saves having to allocate another buffer from the kernel heap.
    const bool buffered = false;
    const bool resume = false;
    Uuid system_uuid = UUID_SYSTEM;
    s_alg_state->dls_session = dls_create(DlsSystemTagActivityMinuteData, DATA_LOGGING_BYTE_ARRAY,
                                          sizeof(AlgMinuteDLSRecord), buffered, resume,
                                          &system_uuid);
    if (!s_alg_state->dls_session) {
      // This can happen when you are not connected to the phone and have rebooted a number of
      // times because each time you reboot, you get new sessions created and reach the limit
      // of the max # of sessions allowed.
      PBL_LOG(LOG_LEVEL_WARNING, "Error creating activity logging session");
      return NULL;
    }
  }
  return s_alg_state->dls_session;
}


// -------------------------------------------------------------------------------------
// We use NOINLINE to reduce the stack requirements during the minute handler (see PBL-38130)
static void NOINLINE prv_set_dls_minute_record_entry(AlgMinuteDLSRecord *dls_record,
                                            AlgMinuteDLSSample *data, uint16_t sample_idx,
                                            time_t sample_utc, bool was_sleeping) {
  if (sample_idx == 0) {
    // If first record, init the header
    prv_init_minute_record(&dls_record->hdr, sample_utc, false /*to_file*/);
  }
  // Put in this minute
  dls_record->samples[sample_idx] = *data;
  dls_record->hdr.num_samples = sample_idx + 1;

  if (was_sleeping && (dls_record->samples[sample_idx].base.steps != 0)) {
    // Subtract from our total steps since we decided we were definitely sleeping during
    // this minute
    PBL_LOG(LOG_LEVEL_DEBUG, "Subtracting %"PRIu8" steps that occurred during sleep",
            dls_record->samples[sample_idx].base.steps);
    s_alg_state->steps -= dls_record->samples[sample_idx].base.steps;
    s_alg_state->steps = MAX(0, s_alg_state->steps);
    dls_record->samples[sample_idx].base.steps = 0;
    dls_record->samples[sample_idx].base.active = false;
    dls_record->samples[sample_idx].active_calories = 0;
    dls_record->samples[sample_idx].distance_cm = 0;
  }
}


// -------------------------------------------------------------------------------------
// Prepare a minute record for writing. Either file_record or dls_record should be non-NULL,
// never both.
// Returns true if we have enough data to prepare a record, false if not enough data.
// We use NOINLINE to reduce the stack requirements during the minute handler (see PBL-38130)
static bool NOINLINE prv_prepare_minute_data(uint16_t uncertain_m, time_t sleep_start_utc,
                                             uint16_t sleep_len_m, AlgMinuteFileRecord *file_record,
                                             AlgMinuteDLSRecord *dls_record, bool force_send) {
  // Get the circular buffer client we are working with
  SharedCircularBufferClient *cbuf_client = file_record ? &s_alg_state->file_minute_data_client
                                                        : &s_alg_state->dls_minute_data_client;
  const int16_t minutes_per_record = file_record ? ALG_MINUTES_PER_FILE_RECORD :
                                     ALG_MINUTES_PER_DLS_RECORD;

  // Empty the circular buffer while we have enough for a record
  time_t sleep_end_utc = sleep_start_utc + (sleep_len_m * SECONDS_PER_MINUTE);

  int16_t certain_m = (shared_circular_buffer_get_read_space_remaining(
      &s_alg_state->minute_data_cbuf, cbuf_client) / sizeof(AlgMinuteRecord)) - uncertain_m;
  int minutes_this_record = MIN(certain_m, minutes_per_record);
  if (minutes_this_record == 0) {
    // nothing to send, even if we really wanted to
    return false;
  } else if (!force_send && minutes_this_record < minutes_per_record) {
    // didn't collect enough data for our regularly scheduled program
    return false;
  }

  // Fill up the next record
  AlgMinuteRecord *cbuf_record = &s_alg_state->cbuf_record;
  for (int i = 0; i < minutes_this_record; i++) {
    uint16_t length_out;
    bool success = shared_circular_buffer_read_consume(
        &s_alg_state->minute_data_cbuf, cbuf_client, sizeof(*cbuf_record), (uint8_t *)cbuf_record,
        &length_out);
    PBL_ASSERTN(success);

    // See if we need to zero out steps in this record. We check that the start of the minute
    // is within the sleep bounds. The WITHIN macro returns true if the test value is
    // <= end_value, so we need to subract one minute from the end to see if the start of this
    // test minute is entirely within the sleep range.
    bool was_sleeping =  WITHIN(cbuf_record->utc_sec, sleep_start_utc,
                                sleep_end_utc - SECONDS_PER_MINUTE);

    // Handle writing the record out to PFS
    if (file_record) {
      prv_set_file_minute_record_entry(file_record, &cbuf_record->data, i, cbuf_record->utc_sec,
                                       was_sleeping);

      // Handle writing the record out to data logging
    } else {
      prv_set_dls_minute_record_entry(dls_record, &cbuf_record->data, i, cbuf_record->utc_sec,
                                      was_sleeping);
    }
  }
  return true;
}



// -------------------------------------------------------------------------------------
// If we have enough minute data in our circular buffer, write it out to either the minute
// file or to data logging
static void prv_send_minute_data(uint16_t uncertain_m, time_t sleep_start_utc,
                                 uint16_t sleep_len_m, bool to_file, bool force_send) {
  // If writing to DLS, make sure we can open up the session we need first
  DataLoggingSession *dls_session = NULL;
  AlgMinuteFileRecord *file_record = NULL;
  AlgMinuteDLSRecord *dls_record = NULL;

  if (to_file) {
    file_record = &s_alg_state->file_record;
  } else {
    dls_record = &s_alg_state->dls_record;

    // Open up the data logging session if we don't already have one
    dls_session = prv_get_dls_minute_session();
    if (!dls_session) {
      return;
    }
  }

  // While we have whole minute records available for sending, send them.
  while (prv_prepare_minute_data(uncertain_m,
                                 sleep_start_utc,
                                 sleep_len_m,
                                 file_record,
                                 dls_record,
                                 force_send)) {
    PBL_ASSERTN((file_record == NULL) != (dls_record == NULL));
    if (file_record) {
      prv_write_minute_file_record(file_record);
    }

    if (dls_record) {
      // Handle writing the record out to data logging
      DataLoggingResult result = dls_log(dls_session, dls_record, 1);
      // PBL-43622: Will revert later
      PBL_LOG(LOG_LEVEL_INFO, "Logging %"PRIu8" MLD Records, First UTC: %"PRIu32,
              dls_record->hdr.num_samples, dls_record->hdr.time_utc);
      if (result != DATA_LOGGING_SUCCESS) {
        PBL_LOG(LOG_LEVEL_WARNING, "Error %"PRIi32" while logging activity data", (int32_t) result);
        return;
      }
    }
  }
}


// -------------------------------------------------------------------------------------------
// Handle storage and logging of the minute data
static void prv_log_minute_data(time_t utc_now, AlgMinuteRecord *minute_rec) {
  // Store the minute data into our circular buffer. The only place we ever read from this buffer
  // is below in this same method (during prv_send_minute_data) only from the KernelBG task, so no
  // need for a lock.
  bool success = shared_circular_buffer_write(&s_alg_state->minute_data_cbuf, (uint8_t *)minute_rec,
                                              sizeof(*minute_rec), false /*advance_slackers*/);
  if (!success) {
    // Although unlikely, we could get buffer overruns if we failed to open up the data logging
    // session after a number of retries. In that case, we will start dropping the oldest
    // minute data from data logging.
    PBL_LOG(LOG_LEVEL_ERROR, "Circular buffer overrun");
    success = shared_circular_buffer_write(&s_alg_state->minute_data_cbuf, (uint8_t *)minute_rec,
                                           sizeof(*minute_rec), true /*advance_slackers*/);
  }
  PBL_ASSERTN(success);

  // Find the number of "certain" minutes we have in the buffer. When we are asleep, the
  // most recent N minutes in the buffer will be uncertain because we don't know that we woke
  // until we see at least M minutes in a row of activity.
  KAlgOngoingSleepStats sleep_stats;
  kalg_get_sleep_stats(s_alg_state->k_state, &sleep_stats);

  // If there are any uncertain minutes, they will always be at the end
  int16_t uncertain_m = 0;
  if (sleep_stats.uncertain_start_utc != 0) {
    uncertain_m = (utc_now - sleep_stats.uncertain_start_utc) / SECONDS_PER_MINUTE;
  }
  if (uncertain_m > KALG_MAX_UNCERTAIN_SLEEP_M) {
    PBL_LOG(LOG_LEVEL_ERROR, "Unexpectedly large number of uncertain minutes");
    uncertain_m = KALG_MAX_UNCERTAIN_SLEEP_M;
  }

  // Send whatever complete DLS records we have
  prv_send_minute_data(uncertain_m, sleep_stats.sleep_start_utc, sleep_stats.sleep_len_m,
                       false /*to_file*/, false /*force_send*/);

  // Send whatever complete minute file records we have
  prv_send_minute_data(uncertain_m, sleep_stats.sleep_start_utc, sleep_stats.sleep_len_m,
                       true /*to_file*/, false /*force_send*/);
}


// ------------------------------------------------------------------------------------
void activity_algorithm_send_minutes(void) {
  if (!prv_lock()) {
    return;
  }
  KAlgOngoingSleepStats sleep_stats;
  kalg_get_sleep_stats(s_alg_state->k_state, &sleep_stats);

  int16_t uncertain_m = 0;
  prv_send_minute_data(uncertain_m, sleep_stats.sleep_start_utc, sleep_stats.sleep_len_m,
                       false /*to_file*/, true /*force_send*/);
  prv_unlock();
}


// ------------------------------------------------------------------------------------
time_t activity_algorithm_get_last_sleep_utc(void) {
  if (!prv_lock()) {
    return 0;
  }
  time_t rv = kalg_activity_last_processed_time(s_alg_state->k_state, KAlgActivityType_Sleep);
  prv_unlock();
  return rv;
}


// ------------------------------------------------------------------------------------
// Post-process the passed in sleep sessions. This function identifies which sleep sessions are
// nap sessions and labels them as such
// @param num_input_sessions number of entries in the sessions array
// @param sessions array of activity sessions
void activity_algorithm_post_process_sleep_sessions(uint16_t num_input_sessions,
                                                    ActivitySession *sessions) {
  if (num_input_sessions == 0) {
    return;
  }
  if (!prv_lock()) {
    return;
  }

  // Now, go through and fix up the labels on the sleep sessions that should be categorized as
  // naps
  ActivitySession *session = sessions;
  ActivitySession *most_recent_nap_session = NULL;
  for (unsigned i = 0; i < num_input_sessions; i++, session++) {
    const unsigned start_minute = time_util_get_minute_of_day(session->start_utc);
    const time_t end_utc = session->start_utc + (session->length_min * SECONDS_PER_MINUTE);
    const unsigned end_minute = time_util_get_minute_of_day(end_utc);

    ACTIVITY_LOG_DEBUG("procesing activity %d, start_min: %u, len: %"PRIu16"",
                       (int)session->type, start_minute, session->length_min);

    // Skip if not a sleep session
    if (!activity_sessions_prv_is_sleep_activity(session->type)) {
      ACTIVITY_LOG_DEBUG("Not a sleep session");
      continue;
    }

    // Skip if still ongoing
    if (session->ongoing) {
      ACTIVITY_LOG_DEBUG("Still ongoing");
      continue;
    }

    // Skip if already labeled as a nap session
    if ((session->type == ActivitySessionType_Nap)
        || (session->type == ActivitySessionType_RestfulNap)) {
      if (session->type == ActivitySessionType_Nap) {
        most_recent_nap_session = session;
      }
      ACTIVITY_LOG_DEBUG("Already labeled as a nap");
      continue;
    }

    if ((session->length_min > ALG_MAX_NAP_MINUTES)
        || !WITHIN(start_minute, ALG_PRIMARY_MORNING_MINUTE, ALG_PRIMARY_EVENING_MINUTE)
        || !WITHIN(end_minute, ALG_PRIMARY_MORNING_MINUTE, ALG_PRIMARY_EVENING_MINUTE)) {
      // If too long, or not within the primary sleep range, can't be a nap
      ACTIVITY_LOG_DEBUG("Not within nap time bounds or duration");
      continue;
    }

    // If this is a restful session, it must be inside of the most recently labeled nap session
    // for us to re-label it. Note that this logic makes the assumption that restful sessions
    // always follow the container session that they belong to. This is assumption is valid given
    // the way that the algorithm identifies sleep sessions.
    if (session->type == ActivitySessionType_RestfulSleep) {
      if (most_recent_nap_session == NULL) {
        continue;
      }
      if ((session->start_utc < most_recent_nap_session->start_utc)
          || (session->start_utc > (most_recent_nap_session->start_utc
              + (most_recent_nap_session->length_min * SECONDS_PER_MINUTE)))) {
        continue;
      }
    }

    // Label it as a nap
    if (session->type == ActivitySessionType_Sleep) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Found nap - start_utc: %d, start_min: %u, len: %"PRIu16" ",
              (int)session->start_utc, start_minute, session->length_min);
      session->type = ActivitySessionType_Nap;
      most_recent_nap_session = session;
    } else if (session->type == ActivitySessionType_RestfulSleep) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Found restful nap - start_utc: %d, start_min: %u, len: %"PRIu16" ",
              (int)session->start_utc, start_minute, session->length_min);
      session->type = ActivitySessionType_RestfulNap;
    }
  }
  prv_unlock();
}


// ------------------------------------------------------------------------------------
bool activity_algorithm_init(AccelSamplingRate *sampling_rate) {
  // Allocate kraepelin state and our globals
  PBL_ASSERTN(s_alg_state == 0);
  KAlgState *k_state = kernel_zalloc(kalg_state_size());
  if (k_state) {
    s_alg_state = kernel_zalloc(sizeof(AlgState));
  }
  if (!s_alg_state) {
    PBL_LOG(LOG_LEVEL_ERROR, "Not enough memory");
    kernel_free(k_state);
    return false;
  }

  // Init globals
  *s_alg_state = (AlgState) {
    .mutex = mutex_create_recursive(),
    .k_state = k_state,
  };
  shared_circular_buffer_init(&s_alg_state->minute_data_cbuf,
                              (uint8_t *)s_alg_state->minute_data_storage,
                              sizeof(s_alg_state->minute_data_storage));
  shared_circular_buffer_add_client(&s_alg_state->minute_data_cbuf,
                                    &s_alg_state->file_minute_data_client);
  shared_circular_buffer_add_client(&s_alg_state->minute_data_cbuf,
                                    &s_alg_state->dls_minute_data_client);

  // Init the algorithm state
  kalg_init(k_state, NULL);

  // Count # of records in minute file
  uint32_t num_records;
  uint32_t data_bytes;
  uint32_t minutes;
  activity_algorithm_minute_file_info(false /*compact_first*/, &num_records, &data_bytes, &minutes);
  s_alg_state->num_minute_records = num_records;

  PBL_LOG(LOG_LEVEL_DEBUG, "Found %"PRIu16" records in minute file",
          s_alg_state->num_minute_records);

  // Reset all metrics
  activity_algorithm_metrics_changed_notification();

  // Return desired sampling rate
  *sampling_rate = KALG_SAMPLE_HZ;
  return true;
}

static void prv_activity_update_states(time_t utc_sec, AlgMinuteRecord *record_out,
                                       bool shutting_down);

// ------------------------------------------------------------------------------------
// This is called when the activity services is doing down. This tells all of our state machines
// that are going to be shut down, and to save off any unsaved data/sleep/step sessions.
void activity_algorithm_early_deinit(void) {
  if (!prv_lock()) {
    return;
  }

  time_t utc_sec = rtc_get_time();
  // AlgMinuteRecord data storage needed for underlying function
  AlgMinuteRecord record_out = {};
  prv_activity_update_states(utc_sec, &record_out, true /* shutting_down */);

  prv_unlock();
}

// ------------------------------------------------------------------------------------
bool activity_algorithm_deinit(void) {
  PBL_ASSERTN(s_alg_state);
  PBL_ASSERTN(s_alg_state->k_state);

  mutex_destroy((PebbleMutex *)s_alg_state->mutex);
  kernel_free(s_alg_state->k_state);

  kernel_free(s_alg_state);
  s_alg_state = NULL;
  return true;
}


// ------------------------------------------------------------------------------------
bool activity_algorithm_set_user(uint32_t height_mm, uint32_t weight_g, ActivityGender gender,
                                 uint32_t age_years) {
  return true;
}


// ------------------------------------------------------------------------------------
void activity_algorithm_handle_accel(AccelRawData *data, uint32_t num_samples,
                                     uint64_t timestamp_ms) {
  if (!prv_lock()) {
    return;
  }
  uint32_t consumed_samples;
  uint16_t new_steps = kalg_analyze_samples(s_alg_state->k_state, data, num_samples,
                                            &consumed_samples);
  s_alg_state->steps += new_steps;
  s_alg_state->minute_steps += new_steps;

  // Update our stepping rate if the algorithm just consumed samples
  if (consumed_samples != 0) {
    s_alg_state->rate_steps = new_steps;
    s_alg_state->rate_elapsed_ms = (consumed_samples *  MS_PER_SECOND) / KALG_SAMPLE_HZ;
    s_alg_state->rate_computed_time_s = timestamp_ms / MS_PER_SECOND;
  }
  prv_unlock();
}


// ------------------------------------------------------------------------------------
// We use NOINLINE to reduce the stack requirements during the minute handler (see PBL-38130)
// Returns distance we traveled in the last minute, in mm.
static uint32_t NOINLINE prv_fill_minute_record(time_t utc_sec, AlgMinuteDLSSample *m_rec) {
  bool still;
  kalg_minute_stats(s_alg_state->k_state, &m_rec->base.vmc,
                    &m_rec->base.orientation, &still);

  m_rec->base.steps = MIN(s_alg_state->minute_steps, UINT8_MAX);

  // The light level readings we get are from 0 to 4095 (12 bits). We only have 8 bits of storage,
  // so divide it down to fit into 8 bits.
  m_rec->base.light = ROUND(ambient_light_get_light_level(), ALG_RAW_LIGHT_SENSOR_DIVIDE_BY);

  // Are we connected to a charger?
  const BatteryChargeState charge_state = battery_get_charge_state();
  m_rec->base.plugged_in = charge_state.is_plugged;

  // Set active flag
  m_rec->base.active = (m_rec->base.steps >= ACTIVITY_ACTIVE_MINUTE_MIN_STEPS) ? 1 : 0;

  // Fill in resting calories, active calories, and distance covered in the previous minute
  const uint32_t resting_calories = activity_metrics_prv_get_resting_calories();
  m_rec->resting_calories = resting_calories - s_alg_state->prev_resting_calories;

  const uint32_t active_calories = activity_metrics_prv_get_active_calories();
  m_rec->active_calories = active_calories - s_alg_state->prev_active_calories;

  const uint32_t distance_mm = activity_metrics_prv_get_distance_mm();
  const uint32_t minute_distance_mm = distance_mm - s_alg_state->prev_distance_mm;
  const int k_mm_per_cm = 10;
  m_rec->distance_cm = ROUND(minute_distance_mm, k_mm_per_cm);

  // Fill in heart rate, heart rate sample weight, then reset it
  int32_t median, heart_rate_total_weight_x100;
  activity_metrics_prv_get_median_hr_bpm(&median, &heart_rate_total_weight_x100);
  m_rec->heart_rate_bpm = (uint8_t)median;
  m_rec->heart_rate_total_weight_x100 = (uint16_t)heart_rate_total_weight_x100;
  m_rec->heart_rate_zone = activity_metrics_prv_get_hr_zone();

  return minute_distance_mm;
}

static void NOINLINE prv_reset_state_minute_handler(const AlgMinuteDLSSample *m_rec) {
    s_alg_state->prev_resting_calories = activity_metrics_prv_get_resting_calories();
    s_alg_state->prev_active_calories = activity_metrics_prv_get_active_calories();
    s_alg_state->prev_distance_mm = activity_metrics_prv_get_distance_mm();
    activity_metrics_prv_reset_hr_stats();
}

static void prv_activity_update_states(time_t utc_sec, AlgMinuteRecord *record_out,
                                       bool shutting_down) {
  // Make sure each record gets time stamped exactly on a minute boundary.
  utc_sec -= (utc_sec % SECONDS_PER_MINUTE);

  // Fill in the minute data structure that we log
  record_out->utc_sec = utc_sec - SECONDS_PER_MINUTE;  // this data is for the previous minute
  AlgMinuteDLSSample *m_rec = &record_out->data;
  uint32_t minute_distance_mm = prv_fill_minute_record(utc_sec, m_rec);

  prv_reset_state_minute_handler(m_rec);

  ACTIVITY_LOG_DEBUG("minute handler: steps: %"PRIu8", orientation: 0x%"PRIx8", vmc: %"PRIu16", "
                     "light: %"PRIu8", plugged_in: %d",
                     m_rec->base.steps, m_rec->base.orientation, m_rec->base.vmc,
                     m_rec->base.light, (int) m_rec->base.plugged_in);

  // Pass the minute data onto the activity detection logic
  kalg_activities_update(s_alg_state->k_state, utc_sec, m_rec->base.steps, m_rec->base.vmc,
                         m_rec->base.orientation, m_rec->base.plugged_in, m_rec->resting_calories,
                         m_rec->active_calories, minute_distance_mm, shutting_down,
                         prv_create_activity_session_cb, NULL);
}

// ------------------------------------------------------------------------------------
void activity_algorithm_minute_handler(time_t utc_sec, AlgMinuteRecord *record_out) {
  if (!prv_lock()) {
    return;
  }

  prv_activity_update_states(utc_sec, record_out, false /* shutting_down */);

  // Handle storage and logging of the minute data
  prv_log_minute_data(utc_sec, record_out);

  // Reset the minute stats
  s_alg_state->minute_steps = 0;
  prv_unlock();
}


// ------------------------------------------------------------------------------------
bool activity_algorithm_get_steps(uint16_t *steps) {
  if (!prv_lock()) {
    return false;
  }
  *steps = s_alg_state->steps;
  prv_unlock();
  return true;
}


// ------------------------------------------------------------------------------------
bool activity_algorithm_get_step_rate(uint16_t *steps, uint32_t *elapsed_ms, time_t *end_sec) {
  if (!prv_lock()) {
    return false;
  }
  *steps = s_alg_state->rate_steps;
  *elapsed_ms = s_alg_state->rate_elapsed_ms;
  *end_sec = s_alg_state->rate_computed_time_s;
  prv_unlock();
  return true;
}


// ------------------------------------------------------------------------------------
bool activity_algorithm_metrics_changed_notification(void) {
  if (!prv_lock()) {
    return false;
  }
  s_alg_state->steps = activity_metrics_prv_get_steps();
  s_alg_state->prev_active_calories = activity_metrics_prv_get_active_calories();
  s_alg_state->prev_resting_calories = activity_metrics_prv_get_resting_calories();
  s_alg_state->prev_distance_mm = activity_metrics_prv_get_distance_mm();
  prv_unlock();
  return true;
}

// ------------------------------------------------------------------------------------
void activity_algorithm_enable_activity_tracking(bool enable) {
  if (!activity_tracking_on()) {
    return;
  }

  if (!prv_lock()) {
    return;
  }

  kalg_enable_activity_tracking(s_alg_state->k_state, enable);

  prv_unlock();
}

// ----------------------------------------------------------------------------------------------
// This structure contains the context used by the activity_algorithm_get_minute_history() call
typedef struct {
  HealthMinuteData *minute_data;
  uint32_t array_size;

  uint32_t oldest_key;
  uint32_t newest_key;

  time_t utc_start;

  time_t oldest_requested_utc;
  int32_t last_record_idx_written;
} AlgReadMinutesContext;


// ----------------------------------------------------------------------------------------------
// Insert a HealthMinuteRecord into the correct place in the activity_algorithm_get_minute_history
// caller's array. Returns true if we don't need to insert any more records (this one is already
// newer than the caller wanted).
static bool prv_insert_health_minute_record(AlgReadMinutesContext *context, time_t record_utc,
                                            AlgMinuteFileSampleV5 *base_fields,
                                            uint8_t heart_rate_bpm) {
  // Get the timestamp of the first minute we are returning in the caller's array
  time_t utc_start = context->utc_start;
  if (utc_start == 0) {
    // This is the first record we found
    if (context->oldest_requested_utc < record_utc) {
      // The caller requested an old timestamp we don't have. Return the oldest one we do have.
      utc_start = record_utc;
    } else {
      utc_start = context->oldest_requested_utc;
    }
  }

  // See where this minute should go in the caller's buffer
  int32_t dst_index = (record_utc - utc_start) / SECONDS_PER_MINUTE;
  if (dst_index < 0) {
    // This record older than the caller wanted. Return false to look for more
    return false;
  }

  // If this is newer than the caller wanted, we are done
  if (dst_index >= (int32_t)context->array_size) {
    return true;
  }

  // Is this the first one we found?
  if (context->utc_start == 0) {
    context->utc_start = record_utc;
  }
  context->last_record_idx_written = dst_index;

  // Put it in
  uint32_t raw_light = base_fields->light * ALG_RAW_LIGHT_SENSOR_DIVIDE_BY;
  AmbientLightLevel health_light_level = ambient_light_level_to_enum(raw_light);
  HealthMinuteData record = {
    .steps = base_fields->steps,
    .orientation = base_fields->orientation,
    .vmc = base_fields->vmc,
    .light = health_light_level,
    .heart_rate_bpm = heart_rate_bpm,
  };

  context->minute_data[dst_index] = record;
  // Return false to look for more
  return false;
}


// ----------------------------------------------------------------------------------------------
// The callback we give to settings_file_each to read in the minute data for
// activity_algorithm_get_minute_history()
static bool prv_read_minute_history_file_cb(SettingsFile *file, SettingsRecordInfo *info,
                                            void *context_param) {
  AlgReadMinutesContext *context = (AlgReadMinutesContext *)context_param;

  AlgMinuteFileRecord chunk;
  if (!prv_read_minute_file_record(file, info, context->oldest_key, context->newest_key, &chunk)) {
    // This record key not within the desired time range
    return true;
  }

  // Check the exact time range using the value
  const uint32_t k_seconds_per_chunk = ALG_MINUTES_PER_FILE_RECORD * SECONDS_PER_MINUTE;
  if (chunk.hdr.time_utc + k_seconds_per_chunk < (uint32_t)context->oldest_requested_utc) {
    ACTIVITY_LOG_DEBUG("Minute chunk time out of range, skipping it");
    return true;;
  }

  // Insert each of the minutes from this chunk into the caller's array
  time_t minute_utc = chunk.hdr.time_utc;
  for (uint32_t i = 0; i < ALG_MINUTES_PER_FILE_RECORD; i++, minute_utc += SECONDS_PER_MINUTE) {
    bool done = prv_insert_health_minute_record(context, minute_utc, &chunk.samples[i].v5_fields,
                                                chunk.samples[i].heart_rate_bpm);
    if (done) {
      // Already newer than we need, return false to stop the search
      return false;
    }
  }

  // Return true to continue searching
  return true;
}


// ----------------------------------------------------------------------------------------------
// Fetch whatever records we have in our minute history circular buffer (i.e. not yet written
// to flash) to satisfy a get_minute_history() request. The passed in context contains the info on
// the request and has already been updated to reflect the data we already fetched from flash.
static void prv_read_minute_history_buffer(AlgReadMinutesContext *context) {
  // Make a copy of the circular buffer client because we don't want to permanently consume data
  SharedCircularBufferClient *cbuf_client = &s_alg_state->file_minute_data_client;
  SharedCircularBufferClient cbuf_client_bck = *cbuf_client;

  AlgMinuteRecord *cbuf_record = &s_alg_state->cbuf_record;
  int16_t avail_minutes = (shared_circular_buffer_get_read_space_remaining(
      &s_alg_state->minute_data_cbuf, cbuf_client) / sizeof(*cbuf_record));

  // Insert data from ram into the caller's minute buffer
  while (avail_minutes--) {
    // Read the next minute out of the buffer
    uint16_t length_out;
    bool success = shared_circular_buffer_read_consume(
        &s_alg_state->minute_data_cbuf, cbuf_client, sizeof(*cbuf_record), (uint8_t *)cbuf_record,
        &length_out);
    PBL_ASSERTN(success);

    time_t record_utc = cbuf_record->utc_sec;
    bool done = prv_insert_health_minute_record(context, record_utc, &cbuf_record->data.base,
                                                     cbuf_record->data.heart_rate_bpm);
    if (done) {
      // we are done
      goto exit;
    }
  }

  // Finally, get the data for the partial last minute that has not even been saved to the circular
  // buffer yet
  time_t minute_utc = rtc_get_time();
  uint32_t seconds_into_minute = minute_utc % SECONDS_PER_MINUTE;
  if (seconds_into_minute > 0) {
    minute_utc -= seconds_into_minute;
    AlgMinuteDLSSample current_minute;
    prv_fill_minute_record(minute_utc, &current_minute);
    prv_insert_health_minute_record(context, minute_utc, &current_minute.base,
                                    current_minute.heart_rate_bpm);
  }

exit:
  // Restore the circular buffer client to where it was before
  *cbuf_client = cbuf_client_bck;
}


// -------------------------------------------------------------------------------
bool activity_algorithm_get_minute_history(HealthMinuteData *minute_data, uint32_t *num_records,
                                           time_t *utc_start) {
  if (!prv_lock()) {
    return false;
  }

  bool success = true;
  SettingsFile *file = NULL;
  uint32_t array_size = *num_records;

  // Open the minute data file
  file = prv_minute_data_file_open();
  if (!file) {
    success = false;
    goto exit;
  }

  // Init for missing records
  memset(minute_data, 0xFF, array_size * sizeof(HealthMinuteData));

  // Figure out the lowest key value for for chunks that go into this buffer
  time_t utc_now = rtc_get_time();
  const time_t oldest_possible = utc_now
      - ALG_MINUTE_FILE_MAX_ENTRIES * ALG_MINUTES_PER_FILE_RECORD * SECONDS_PER_MINUTE;
  time_t oldest_requested_utc = *utc_start;
  oldest_requested_utc = MAX(oldest_possible, oldest_requested_utc);

  // Create the context
  AlgReadMinutesContext context = (AlgReadMinutesContext) {
    .minute_data = minute_data,
    .array_size = array_size,
    .oldest_key = prv_minute_file_get_settings_key(oldest_requested_utc) - 1,
    .newest_key = prv_minute_file_get_settings_key(utc_now) + 1,
    .utc_start = 0,
    .oldest_requested_utc = oldest_requested_utc,
    .last_record_idx_written = -1,
  };

  // Read the minute data from flash
  status_t status = settings_file_each(file, prv_read_minute_history_file_cb, &context);
  if (status != S_SUCCESS) {
    success = false;
    goto exit;
  }

  // Fill in any data we have in RAM as well
  prv_read_minute_history_buffer(&context);

exit:
  if (file) {
    prv_minute_data_file_close(file);
  }
  prv_unlock();

  if (success) {
    // Return number of records that were written, including missing records in the middle
    *num_records = context.last_record_idx_written + 1;
    *utc_start = context.utc_start;
  } else {
    *num_records = 0;
    *utc_start = 0;
  }
  return success;
}


// -------------------------------------------------------------------------------
// Get info on the minute data file
typedef struct {
  uint32_t num_records;
} AlgMinuteFileInfoContext;

static bool prv_read_minute_file_info_cb(SettingsFile *file, SettingsRecordInfo *info,
                                         void *context_arg) {
  AlgMinuteFileInfoContext *context = (AlgMinuteFileInfoContext *)context_arg;
  context->num_records++;
  return true;
}

bool activity_algorithm_minute_file_info(bool compact_first, uint32_t *num_records,
                                         uint32_t *data_bytes, uint32_t *minutes) {
  if (!prv_lock()) {
    return false;
  }
  bool success = false;
  SettingsFile *file = NULL;

  file = prv_minute_data_file_open();
  if (file && compact_first) {
    file = prv_validate_and_trim_minute_file(file, ALG_MINUTE_FILE_MAX_ENTRIES);
  }
  if (!file) {
    goto exit;
  }

  // Count # of records in minute file
  AlgMinuteFileInfoContext context = (AlgMinuteFileInfoContext) {};

  status_t status = settings_file_each(file, prv_read_minute_file_info_cb, &context);
  if (status != S_SUCCESS) {
    goto exit;
  }
  success = true;

exit:
  if (file) {
    prv_minute_data_file_close(file);
  }
  prv_unlock();

  if (success) {
    *num_records = context.num_records;
    *minutes = context.num_records * ALG_MINUTES_PER_FILE_RECORD;
    *data_bytes = *minutes * sizeof(AlgMinuteFileSample);
  } else {
    *num_records = 0;
    *minutes = 0;
    *data_bytes = 0;
  }
  return success;
}


// -------------------------------------------------------------------------------
bool activity_algorithm_test_fill_minute_file(void) {
  bool success = false;
  time_t utc_sec = rtc_get_time() - SECONDS_PER_MINUTE;

  AlgMinuteFileRecord record = { };
  prv_init_minute_record(&record.hdr, utc_sec, true /*for_file*/);

  // Delete old file so this doesn't take forver, in case it's already got a lot of data in it
  pfs_remove(ALG_MINUTE_DATA_FILE_NAME);
  s_alg_state->num_minute_records = 0;

  uint32_t secs_per_record = ALG_MINUTES_PER_FILE_RECORD * SECONDS_PER_MINUTE;
  time_t start_utc = utc_sec - ALG_MINUTE_FILE_MAX_ENTRIES * secs_per_record;

  PBL_LOG(LOG_LEVEL_DEBUG, "Writing %"PRIu32" records", (uint32_t) ALG_MINUTE_FILE_MAX_ENTRIES);

  // Fill up the minute file to capacity, starting from back in time
  uint8_t heart_rate = 50;
  for (uint32_t i = 0; i < ALG_MINUTE_FILE_MAX_ENTRIES; i++, start_utc += secs_per_record) {
    record.hdr.time_utc = start_utc;
    record.hdr.num_samples = ALG_MINUTES_PER_FILE_RECORD;

    // Fill in some fake heart rate data and step for testing purposes
    for (uint32_t j = 0; j < ALG_MINUTES_PER_FILE_RECORD; j++) {
      if ((j % 5) == 0) {
        record.samples[j].heart_rate_bpm = heart_rate++;
        if (heart_rate > 150) {
          heart_rate = 50;
        }
      }
      else {
        record.samples[j].heart_rate_bpm = 0;
      }
      record.samples[j].v5_fields.steps = i + 10;
    }
    success = prv_write_minute_file_record(&record);
    if (!success) {
      break;
    }
    system_task_watchdog_feed();
    if ((i % 25) == 0) {
      PBL_LOG(LOG_LEVEL_DEBUG, "wrote %"PRIu32" records...", i);
    }
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Done. End # of records: %"PRIu16, s_alg_state->num_minute_records);
  return success;
}


// -------------------------------------------------------------------------------
// Immediately send a fake logging record to data logging. This aids in testing the mobile
// app
bool activity_algorithm_test_send_fake_minute_data_dls_record(void) {
  AlgMinuteDLSRecord record = { };
  prv_init_minute_record(&record.hdr,
                         rtc_get_time() - (ALG_MINUTES_PER_DLS_RECORD * SECONDS_PER_MINUTE),
                         false /*for_file*/);

  // Fill in fake data
  for (uint32_t i = 0; i < ALG_MINUTES_PER_FILE_RECORD; i++) {
    record.samples[i] = (AlgMinuteDLSSample) {
      .base = {
        .steps = i,
        .orientation = 20 + i,
        .vmc = 40 + i,
        .light = 60 + i,
      },
      .resting_calories = 1000 + i,
      .active_calories = 2000 + i,
      .distance_cm = 100 + i,
      .heart_rate_bpm = 60 + i,
      .heart_rate_total_weight_x100 = 100 + i,
      .heart_rate_zone = hr_util_get_hr_zone(60 + i),
    };
  }

  record.hdr.num_samples = ALG_MINUTES_PER_FILE_RECORD;

  // Send it to data logging
  DataLoggingSession *dls_session = prv_get_dls_minute_session();
  if (!dls_session) {
    return false;
  }
  DataLoggingResult result = dls_log(dls_session, &record, 1);
  bool success = (result == DATA_LOGGING_SUCCESS);

  // Force data logging to send to the phone
  if (success) {
    dls_send_all_sessions();
  }
  return success;
}
