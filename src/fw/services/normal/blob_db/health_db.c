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

#include "health_db.h"

#include "console/prompt.h"
#include "kernel/pbl_malloc.h"
#include "os/mutex.h"
#include "services/normal/activity/activity_private.h"
#include "services/normal/activity/hr_util.h"
#include "services/normal/blob_db/api.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/settings/settings_file.h"
#include "system/hexdump.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/units.h"

#include <stdio.h>
#include <string.h>

#define HEALTH_DB_DEBUG 0
#define HEALTH_DB_MAX_KEY_LEN 30

static const char *HEALTH_DB_FILE_NAME = "healthdb";
static const int HEALTH_DB_MAX_SIZE = KiBYTES(12);
static PebbleMutex *s_mutex;

#define MOVEMENT_DATA_KEY_SUFFIX "_movementData"
#define SLEEP_DATA_KEY_SUFFIX "_sleepData"
#define STEP_TYPICALS_KEY_SUFFIX "_steps" // Not the best suffix, but we are stuck with it now...
#define STEP_AVERAGE_KEY_SUFFIX "_dailySteps"
#define SLEEP_AVERAGE_KEY_SUFFIX "_sleepDuration"
#define HR_ZONE_DATA_KEY_SUFFIX "_heartRateZoneData"

static const char *WEEKDAY_NAMES[] = {
  [Sunday] = "sunday",
  [Monday] = "monday",
  [Tuesday] = "tuesday",
  [Wednesday] = "wednesday",
  [Thursday] = "thursday",
  [Friday] = "friday",
  [Saturday] = "saturday",
};

#define CURRENT_MOVEMENT_DATA_VERSION 1
#define CURRENT_SLEEP_DATA_VERSION 1
#define CURRENT_HR_ZONE_DATA_VERSION 1

typedef struct PACKED MovementData {
  uint32_t version;
  uint32_t last_processed_timestamp;
  uint32_t steps;
  uint32_t active_kcalories;
  uint32_t resting_kcalories;
  uint32_t distance;
  uint32_t active_seconds;
} MovementData;
_Static_assert(offsetof(MovementData, version) == 0, "Version not at the start of MovementData");
_Static_assert(sizeof(MovementData) % sizeof(uint32_t) == 0, "MovementData size is invalid");

typedef struct PACKED SleepData {
  uint32_t version;
  uint32_t last_processed_timestamp;
  uint32_t sleep_duration;
  uint32_t deep_sleep_duration;
  uint32_t fall_asleep_time;
  uint32_t wakeup_time;
  uint32_t typical_sleep_duration;
  uint32_t typical_deep_sleep_duration;
  uint32_t typical_fall_asleep_time;
  uint32_t typical_wakeup_time;
} SleepData;
_Static_assert(offsetof(SleepData, version) == 0, "Version not at the start of SleepData");
_Static_assert(sizeof(SleepData) % sizeof(uint32_t) == 0, "SleepData size is invalid");

// The phone doesn't send us Zone0 minutes
typedef struct PACKED HeartRateZoneData {
  uint32_t version;
  uint32_t last_processed_timestamp;
  uint32_t num_zones;
  uint32_t minutes_in_zone[HRZone_Max];
} HeartRateZoneData;
_Static_assert(offsetof(HeartRateZoneData, version) == 0,
               "Version not at the start of HeartRateZoneData");
_Static_assert(sizeof(HeartRateZoneData) % sizeof(uint32_t) == 0,
               "HeartRateZoneData size is invalid");


static status_t prv_file_open_and_lock(SettingsFile *file) {
  mutex_lock(s_mutex);

  status_t rv = settings_file_open(file, HEALTH_DB_FILE_NAME, HEALTH_DB_MAX_SIZE);
  if (rv != S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to open settings file");
    mutex_unlock(s_mutex);
  }

  return rv;
}

static void prv_file_close_and_unlock(SettingsFile *file) {
  settings_file_close(file);
  mutex_unlock(s_mutex);
}

static bool prv_key_is_valid(const uint8_t *key, int key_len) {
  return key_len != 0 &&  // invalid length
         strchr((const char *)key, '_') != NULL; // invalid key
}

static bool prv_value_is_valid(const uint8_t *key,
                               int key_len,
                               const uint8_t *val,
                               int val_len) {
  return val_len && val_len % sizeof(uint32_t) == 0;
}

static bool prv_is_last_processed_timestamp_valid(time_t timestamp) {
  // We only store today + the last 6 days. Anything older than that should be ignored
  const time_t start_of_today = time_start_of_today();
  // This might not handle DST perfectly, but it should be good enough
  const time_t oldest_valid_timestamp = start_of_today - (SECONDS_PER_DAY * 6);

  if (timestamp < oldest_valid_timestamp || timestamp > start_of_today + SECONDS_PER_DAY) {
    return false;
  }

  return true;
}


//! Tell the activity service that it needs to update its "current" values (non typicals / averages)
static void prv_notify_health_listeners(const char *key,
                                        int key_len,
                                        const uint8_t *val,
                                        int val_len) {
  DayInWeek wday;
  for (wday = 0; wday < DAYS_PER_WEEK; wday++) {
    if (strstr(key, WEEKDAY_NAMES[wday])) {
      break;
    }
  }
  // For logging
  const DayInWeek cur_wday = time_util_get_day_in_week(rtc_get_time());

  if (strstr(key, MOVEMENT_DATA_KEY_SUFFIX)) {
    MovementData *data = (MovementData *)val;
    if (!prv_is_last_processed_timestamp_valid(data->last_processed_timestamp)) {
      return;
    }
    PBL_LOG(LOG_LEVEL_INFO, "Got MovementData for wday: %d, cur_wday: %d, steps: %"PRIu32"",
            wday, cur_wday, data->steps);
    activity_metrics_prv_set_metric(ActivityMetricStepCount, wday, data->steps);
    activity_metrics_prv_set_metric(ActivityMetricActiveSeconds, wday, data->active_seconds);
    activity_metrics_prv_set_metric(ActivityMetricRestingKCalories, wday, data->resting_kcalories);
    activity_metrics_prv_set_metric(ActivityMetricActiveKCalories, wday, data->active_kcalories);
    activity_metrics_prv_set_metric(ActivityMetricDistanceMeters, wday, data->distance);
  } else if (strstr(key, SLEEP_DATA_KEY_SUFFIX)) {
    SleepData *data = (SleepData *)val;
    if (!prv_is_last_processed_timestamp_valid(data->last_processed_timestamp)) {
      return;
    }
    PBL_LOG(LOG_LEVEL_INFO, "Got SleepData for wday: %d, cur_wday: %d, sleep: %"PRIu32"",
            wday, cur_wday, data->sleep_duration);
    activity_metrics_prv_set_metric(ActivityMetricSleepTotalSeconds, wday, data->sleep_duration);
    activity_metrics_prv_set_metric(ActivityMetricSleepRestfulSeconds, wday,
                                    data->deep_sleep_duration);
    activity_metrics_prv_set_metric(ActivityMetricSleepEnterAtSeconds, wday,
                                    data->fall_asleep_time);
    activity_metrics_prv_set_metric(ActivityMetricSleepExitAtSeconds, wday, data->wakeup_time);
  } else if (strstr(key, HR_ZONE_DATA_KEY_SUFFIX)) {
    HeartRateZoneData *data = (HeartRateZoneData *)val;
    if (!prv_is_last_processed_timestamp_valid(data->last_processed_timestamp)) {
      return;
    }
    if (data->num_zones != HRZone_Max) {
      return;
    }
    PBL_LOG(LOG_LEVEL_INFO, "Got HeartRateZoneData for wday: %d, cur_wday: %d, zone1: %"PRIu32"",
            wday, cur_wday, data->minutes_in_zone[0]);
    activity_metrics_prv_set_metric(ActivityMetricHeartRateZone1Minutes, wday,
                                    data->minutes_in_zone[0]);
    activity_metrics_prv_set_metric(ActivityMetricHeartRateZone2Minutes, wday,
                                    data->minutes_in_zone[1]);
    activity_metrics_prv_set_metric(ActivityMetricHeartRateZone3Minutes, wday,
                                    data->minutes_in_zone[2]);
  }
}


/////////////////////////
// Public API
/////////////////////////

bool health_db_get_typical_value(ActivityMetric metric,
                                 DayInWeek day,
                                 int32_t *value_out) {
  char key[HEALTH_DB_MAX_KEY_LEN];
  snprintf(key, HEALTH_DB_MAX_KEY_LEN, "%s%s", WEEKDAY_NAMES[day], SLEEP_DATA_KEY_SUFFIX);
  const int key_len = strlen(key);


  SettingsFile file;
  if (prv_file_open_and_lock(&file) != S_SUCCESS) {
    return false;
  }

  // We cheat a bit here because the only typical values we store are sleep related
  SleepData data;
  status_t s = settings_file_get(&file, key, key_len, (uint8_t *)&data, sizeof(data));

  prv_file_close_and_unlock(&file);

  if (s != S_SUCCESS || data.version != CURRENT_SLEEP_DATA_VERSION) {
    return false;
  }

  switch (metric) {
    case ActivityMetricSleepTotalSeconds:
      *value_out = data.typical_sleep_duration;
      break;
    case ActivityMetricSleepRestfulSeconds:
      *value_out = data.typical_deep_sleep_duration;
      break;
    case ActivityMetricSleepEnterAtSeconds:
      *value_out = data.typical_fall_asleep_time;
      break;
    case ActivityMetricSleepExitAtSeconds:
      *value_out = data.typical_wakeup_time;
      break;
    case ActivityMetricStepCount:
    case ActivityMetricActiveSeconds:
    case ActivityMetricRestingKCalories:
    case ActivityMetricActiveKCalories:
    case ActivityMetricDistanceMeters:
    case ActivityMetricSleepStateSeconds:
    case ActivityMetricLastVMC:
    case ActivityMetricHeartRateRawBPM:
    case ActivityMetricHeartRateRawQuality:
    case ActivityMetricHeartRateRawUpdatedTimeUTC:
    case ActivityMetricHeartRateFilteredBPM:
    case ActivityMetricHeartRateFilteredUpdatedTimeUTC:
    case ActivityMetricNumMetrics:
    case ActivityMetricSleepState:
    case ActivityMetricHeartRateZone1Minutes:
    case ActivityMetricHeartRateZone2Minutes:
    case ActivityMetricHeartRateZone3Minutes:
      PBL_LOG(LOG_LEVEL_WARNING, "Health DB doesn't know about typical metric %d", metric);
      return false;
  }

  return true;
}

bool health_db_get_monthly_average_value(ActivityMetric metric,
                                         int32_t *value_out) {
  if (metric != ActivityMetricStepCount && metric != ActivityMetricSleepTotalSeconds) {
    PBL_LOG(LOG_LEVEL_WARNING, "Health DB doesn't store an average for metric %d", metric);
    return false;
  }

  SettingsFile file;
  if (prv_file_open_and_lock(&file) != S_SUCCESS) {
    return false;
  }

  char key[HEALTH_DB_MAX_KEY_LEN];
  snprintf(key, HEALTH_DB_MAX_KEY_LEN, "average%s", (metric == ActivityMetricStepCount) ?
           STEP_AVERAGE_KEY_SUFFIX : SLEEP_AVERAGE_KEY_SUFFIX);
  const int key_len = strlen(key);

  status_t s = settings_file_get(&file, key, key_len, value_out, sizeof(uint32_t));

  prv_file_close_and_unlock(&file);
  return (s == S_SUCCESS);
}

bool health_db_get_typical_step_averages(DayInWeek day, ActivityMetricAverages *averages) {
  if (!averages) {
    return false;
  }

  // Default results
  _Static_assert(((ACTIVITY_METRIC_AVERAGES_UNKNOWN >> 8) & 0xFF)
                     == (ACTIVITY_METRIC_AVERAGES_UNKNOWN & 0xFF), "Cannot use memset");
  memset(averages->average, ACTIVITY_METRIC_AVERAGES_UNKNOWN & 0xFF, sizeof(averages->average));

  SettingsFile file;
  if (prv_file_open_and_lock(&file) != S_SUCCESS) {
    return false;
  }

  char key[HEALTH_DB_MAX_KEY_LEN];
  snprintf(key, HEALTH_DB_MAX_KEY_LEN, "%s%s", WEEKDAY_NAMES[day], STEP_TYPICALS_KEY_SUFFIX);
  const int key_len = strlen(key);

  status_t s = settings_file_get(&file, key, key_len, averages->average, sizeof(averages->average));

  prv_file_close_and_unlock(&file);
  return (s == S_SUCCESS);
}

//! For test / debug purposes only
bool health_db_set_typical_values(ActivityMetric metric,
                                  DayInWeek day,
                                  uint16_t *values,
                                  int num_values) {
  char key[HEALTH_DB_MAX_KEY_LEN];
  snprintf(key, HEALTH_DB_MAX_KEY_LEN, "%s%s", WEEKDAY_NAMES[day], STEP_TYPICALS_KEY_SUFFIX);
  const int key_len = strlen(key);

  return health_db_insert((uint8_t *)key, key_len, (uint8_t*)values, num_values * sizeof(uint16_t));
}

/////////////////////////
// Blob DB API
/////////////////////////

void health_db_init(void) {
  s_mutex = mutex_create();
  PBL_ASSERTN(s_mutex != NULL);
}

status_t health_db_insert(const uint8_t *key, int key_len, const uint8_t *val, int val_len) {
  if (!prv_key_is_valid(key, key_len)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid health db key");
    PBL_HEXDUMP(LOG_LEVEL_ERROR, key, key_len);
    return E_INVALID_ARGUMENT;
  } else if (!prv_value_is_valid(key, key_len, val, val_len)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid health db value. Length %d", val_len);
    return E_INVALID_ARGUMENT;
  }

#if HEALTH_DB_DEBUG
  PBL_LOG(LOG_LEVEL_DEBUG, "New health db entry key:");
  PBL_HEXDUMP(LOG_LEVEL_DEBUG, key, key_len);
  PBL_LOG(LOG_LEVEL_DEBUG, "val: ");
  PBL_HEXDUMP(LOG_LEVEL_DEBUG, val, val_len);
#endif

  // Only store typicals / averages in this settings file. "Current" values are stored in the
  // activity settings file.
  // Sleep data contains a mix of current and typical values. The current values are just stored
  // for convenience and can't be accessed from this settings file.
  status_t rv = S_SUCCESS;
  if (!strstr((char *)key, MOVEMENT_DATA_KEY_SUFFIX)) {
    SettingsFile file;
    rv = prv_file_open_and_lock(&file);
    if (rv != S_SUCCESS) {
      return rv;
    }

    rv = settings_file_set(&file, key, key_len, val, val_len);
    prv_file_close_and_unlock(&file);
  }

  prv_notify_health_listeners((const char *)key, key_len, val, val_len);

  return rv;
}

int health_db_get_len(const uint8_t *key, int key_len) {
  if (!prv_key_is_valid(key, key_len)) {
    return E_INVALID_ARGUMENT;
  }

  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return 0;
  }

  int length = settings_file_get_len(&file, key, key_len);

  prv_file_close_and_unlock(&file);

  return length;
}

status_t health_db_read(const uint8_t *key, int key_len, uint8_t *value_out, int value_out_len) {
  if (!prv_key_is_valid(key, key_len)) {
    return E_INVALID_ARGUMENT;
  }

  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  rv = settings_file_get(&file, key, key_len, value_out, value_out_len);

  prv_file_close_and_unlock(&file);

  return rv;
}

status_t health_db_delete(const uint8_t *key, int key_len) {
  if (!prv_key_is_valid(key, key_len)) {
    return E_INVALID_ARGUMENT;
  }

  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  rv = settings_file_delete(&file, key, key_len);

  prv_file_close_and_unlock(&file);

  return rv;
}

status_t health_db_flush(void) {
  mutex_lock(s_mutex);
  status_t rv = pfs_remove(HEALTH_DB_FILE_NAME);
  mutex_unlock(s_mutex);
  return rv;
}

