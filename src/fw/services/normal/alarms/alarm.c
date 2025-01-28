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

#include "alarm.h"
#include "alarm_pin.h"

#include "apps/system_app_ids.h"
#include "drivers/rtc.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "os/mutex.h"
#include "process_management/app_install_manager.h"
#include "services/common/analytics/analytics.h"
#include "services/common/clock.h"
#include "services/common/i18n/i18n.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/system_task.h"
#include "services/normal/activity/activity.h"
#include "services/normal/settings/settings_file.h"
#include "services/normal/timeline/event.h"
#include "services/normal/timeline/timeline.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/list.h"
#include "util/string.h"
#include "util/units.h"

#include <pebbleos/cron.h>

#include <string.h>

#define DEFAULT_SNOOZE_DELAY_M (10)
#define MAX_CONFIGURED_ALARMS (10)

#define ALARM_FILE_NAME "alarms"
#define ALARM_MAX_FILE_SIZE KiBYTES(1) // ~50 alarms or so
#define NUM_ALARM_PINS_PER_ALARM (3)
#define ALARM_ENTRY_SIZE (UUID_SIZE * NUM_ALARM_PINS_PER_ALARM)

// All alarm preferences are saved in the file under separate keys to simplify
// backward compatibility. When a new preference is added, watches with older
// firmwares won't have that preference stored and the preference-loading code
// naturally falls back to setting the preference to its default. If the
// preferences were all stored as a struct under a single key, the structs would
// need to be versioned and newer firmwares would need to know how to parse or
// migrate older versions of the preference struct.
#define ALARM_PREF_KEY_SNOOZE_DELAY "SnoozeDelayM"

// Multiple pieces of data need to be stored for each alarm. These are split
// across multiple keys to keep the alarm configuration separate from
// bookkeeping data.
typedef enum AlarmDataType {
  ALARM_DATA_CONFIG = 0,
  ALARM_DATA_PINS = 1,
} AlarmDataType;

// Stored alarm data is keyed off a binary (AlarmId, AlarmDataType) tuple
// so that programmatic construction of a key is straightforward.
typedef struct PACKED AlarmStorageKey {
  AlarmId id;
  AlarmDataType type:8;
} AlarmStorageKey;

typedef struct PACKED {
  AlarmKind kind:8;
  //! Whether the alarm is disabled or not. This field cannot be updated to a bitfield because the
  //! compiler sets arbitrary bits to indicate true as an optimization.
  bool is_disabled;
  uint8_t hour;
  uint8_t minute;
  // 1 entry per week day. True if the alarm should go off on that week day. Sunday = 0.
  bool scheduled_days[DAYS_PER_WEEK];
  //! v3.12 alarm fields, compiled in even for unhealthy platforms to simplify compatibility
  union {
    //! These flags have the same value in memory and in flash.
    struct {
      //! Whether the alarm is a smart alarm or not. Smart alarms attempt to wake the user the
      //! first moment the user is not in deep sleep in the time range T-30min to T every 5 min.
      bool is_smart:1;
    };
    uint8_t flags;
  };
} AlarmConfig;

typedef struct Alarm {
  AlarmId id;
  AlarmConfig config;
} Alarm;

typedef bool (*AlarmOperationCallback)(AlarmId id, AlarmConfig *config, void *context);

// Forward declarations
static void prv_alarm_operation(AlarmId id, AlarmOperationCallback callback, void *context);
static bool prv_reload_alarms(SettingsFile *file);
static bool prv_alarm_get_config(SettingsFile *file, AlarmId id, AlarmConfig* config_out);
static void prv_alarm_set_config(SettingsFile *file, AlarmId id, const AlarmConfig* config);
static void prv_cron_callback(CronJob *job, void* data);
static void prv_snooze_alarm(int snooze_delay_s);
static bool prv_analytics_op(AlarmId id, AlarmConfig *config, void *context);
static bool prv_set_alarm_kind_op(AlarmId id, AlarmConfig *config, void *context);
static bool prv_set_alarm_custom_op(AlarmId id, AlarmConfig *config, void *context);

// Private globals
static bool s_alarms_enabled = false;
static time_t s_next_alarm_time;

//! This is only valid when s_next_alarm_time is not 0.
static Alarm s_next_alarm;
static CronJob s_next_alarm_cron;
static AlarmId s_most_recent_alarm_id = ALARM_INVALID_ID;
static AlarmConfig s_most_recent_alarm_config;
static bool s_most_recent_alarm_recorded;

static TimerID s_snooze_timer_id = TIMER_INVALID_ID;
static uint16_t s_snooze_delay_m = DEFAULT_SNOOZE_DELAY_M;

//! Number of times the most recent alarm was automatically smart snoozed.
//! Used to determine an expired smart alarm avoiding issues with midnight rollover and DST.
static int s_smart_snooze_counter;

//! Mutex used to guard our list of alarms
static PebbleMutex *s_mutex;

// ----------------------------------------------------------------------------------------------
static bool prv_file_open_and_lock(SettingsFile *file) {
  mutex_lock_with_timeout(s_mutex, portMAX_DELAY);

  status_t rv = settings_file_open(file, ALARM_FILE_NAME, ALARM_MAX_FILE_SIZE);
  bool success = rv == S_SUCCESS;
  if (!success) {
    mutex_unlock(s_mutex);
  }

  return success;
}

// ----------------------------------------------------------------------------------------------
static void prv_file_close_and_unlock(SettingsFile *file) {
  settings_file_close(file);
  mutex_unlock(s_mutex);
}

// ----------------------------------------------------------------------------------------------
static ActivitySleepState prv_get_sleep_state(void) {
#if CAPABILITY_HAS_HEALTH_TRACKING
  int32_t sleep_state;
  const bool rv = activity_get_metric(ActivityMetricSleepState, 1, &sleep_state);
  return rv ? sleep_state : ActivitySleepStateUnknown;
#else
  return ActivitySleepStateUnknown;
#endif
}

// ----------------------------------------------------------------------------------------------
static int prv_get_sleep_seconds(void) {
#if CAPABILITY_HAS_HEALTH_TRACKING
  int32_t sleep_seconds;
  const bool rv = activity_get_metric(ActivityMetricSleepStateSeconds, 1, &sleep_seconds);
  return rv ? sleep_seconds : 0;
#else
  return 0;
#endif
}

// ----------------------------------------------------------------------------------------------
static int32_t prv_get_vmc(void) {
#if CAPABILITY_HAS_HEALTH_TRACKING
  int32_t vmc;
  const bool rv = activity_get_metric(ActivityMetricLastVMC, 1, &vmc);
  return rv ? vmc : 0;
#else
  return 0;
#endif
}

// ----------------------------------------------------------------------------------------------
static bool prv_should_smart_alarm_trigger(const AlarmConfig *config) {
  if (s_smart_snooze_counter >= SMART_ALARM_MAX_SMART_SNOOZE) {
    // The smart alarm has reached the end of its time range
    return true;
  }
  const int sleep_seconds = prv_get_sleep_seconds();
  switch (prv_get_sleep_state()) {
    case ActivitySleepStateUnknown:
    case ActivitySleepStateAwake:
      // The user is awake, just trigger
      return true;
    case ActivitySleepStateLightSleep:
    case ActivitySleepStateRestfulSleep:
      // The user is asleep, trigger if VMC > 0
      return prv_get_vmc() > 0;
  }
  return false;
}

// ----------------------------------------------------------------------------------------------
//! Removes the alarm from the timeline. No-op if it doesn't exist.
//! @return true if an alarm pin was removed
static bool prv_timeline_remove_alarm(SettingsFile *fd, AlarmId id) {
  bool success = false;
  AlarmStorageKey key = { .id = id, .type = ALARM_DATA_PINS };
  int size = settings_file_get_len(fd, &key, sizeof(key));

  if (size == 0) { // empty (likely deleted) entry
    return false;
  } else if (size > ALARM_ENTRY_SIZE) { // malformed data
    settings_file_delete(fd, &key, sizeof(key));
    return false;
  } else { // size <= ALARM_ENTRY_SIZE
    uint8_t buffer[size];
    if (settings_file_get(fd, &key, sizeof(key), buffer, size) == S_SUCCESS) {
      for (int i = 0; i < size / UUID_SIZE; i++) {
        Uuid *pinid = (Uuid *) &buffer[UUID_SIZE * i];
        if (uuid_is_invalid(pinid)) {
          continue;
        }
        alarm_pin_remove(pinid);
        settings_file_delete(fd, &key, sizeof(key));
        success = true;
      }
    }
  }
  return success;
}

// ----------------------------------------------------------------------------------------------
//! Converts an alarm's cron time to its actual alarm time.
//! Smart alarms execute earlier than their actual alarm time in order to opportunistically alarm.
static time_t prv_get_alarm_time(const Alarm *alarm, time_t cron_time) {
  if (alarm->config.is_smart) {
    cron_time += SMART_ALARM_RANGE_S;
  }
  return cron_time;
}

// ----------------------------------------------------------------------------------------------
//! Pin add helper
static void prv_add_pin(AlarmId id, const AlarmConfig *config, time_t alarm_time, Uuid *uuid_out) {
  const AlarmType type = config->is_smart ? AlarmType_Smart : AlarmType_Basic;
  alarm_pin_add(alarm_time, id, type, config->kind, uuid_out);
}

// ----------------------------------------------------------------------------------------------
//! Pins alarm in the timeline for the next three days
static void prv_timeline_add_alarm(SettingsFile *file, const Alarm *alarm,
                                   const CronJob *cron, const time_t current_time) {
  // If an alarm was updated then remove all the pins with stale information
  // If an alarm was added then this has no effect
  bool updated = prv_timeline_remove_alarm(file, alarm->id);

  // We allocate some larger variables on the heap to reduce stack usage
  struct tm *local_alarm_time = kernel_malloc_check(sizeof(struct tm));
  uint8_t *settings_file_buffer = kernel_malloc_check(ALARM_ENTRY_SIZE);
  AppInstallEntry *entry = kernel_malloc_check(sizeof(AppInstallEntry));
  if (!app_install_get_entry_for_install_id(APP_ID_ALARMS, entry)) {
    goto cleanup;
  }

  int num_pin_adds = 0;
  time_t alarm_time = prv_get_alarm_time(alarm, cron_job_get_execute_time(cron));

  time_t last_alarm = 0;
  for (int i = 0; alarm_time <= current_time + SECONDS_PER_DAY * 3; i++) {
    if (last_alarm != alarm_time) {
      last_alarm = alarm_time;
      localtime_r(&alarm_time, local_alarm_time);
      if (alarm->config.scheduled_days[local_alarm_time->tm_wday]) {
        Uuid *pinid = (Uuid *)&settings_file_buffer[num_pin_adds++ * UUID_SIZE];
        prv_add_pin(alarm->id, &alarm->config, alarm_time, pinid);

        if (updated) {
          analytics_event_pin_updated(alarm_time, &entry->uuid);
        } else {
          analytics_event_pin_created(alarm_time, &entry->uuid);
        }
      }
    }
    alarm_time = prv_get_alarm_time(
        alarm, cron_job_get_execute_time_from_epoch(cron, current_time + (i * SECONDS_PER_DAY)));
  }

  AlarmStorageKey key = { .id = alarm->id, .type = ALARM_DATA_PINS };
  settings_file_set(file, &key, sizeof(key),
                    settings_file_buffer, UUID_SIZE * num_pin_adds);

cleanup:
  kernel_free(settings_file_buffer);
  kernel_free(local_alarm_time);
  kernel_free(entry);
}

// ----------------------------------------------------------------------------------------------
static time_t prv_build_cron(AlarmConfig *config, CronJob *cron) {
  *cron = (CronJob) {
    .cb = prv_cron_callback,
    .cb_data = (void*)0,

    .minute = config->minute,
    .hour = config->hour,
    .mday = CRON_MDAY_ANY,
    .month = CRON_MONTH_ANY,

    .offset_seconds = config->is_smart ? -SMART_ALARM_RANGE_S : 0,

    // Tolerate up to a 15 minute clock change before recalculating.
    .clock_change_tolerance = 15 * SECONDS_PER_MINUTE,
  };
  for (int i = 0; i < DAYS_PER_WEEK; i++) {
    cron->wday |= config->scheduled_days[i] ? (1 << i) : 0;
  }
  return cron_job_get_execute_time(cron);
}

// ----------------------------------------------------------------------------------------------
static void prv_assign_alarm(Alarm *alarm, CronJob *cron) {
  cron_job_unschedule(&s_next_alarm_cron);
  s_next_alarm_cron = *cron;
  s_next_alarm_cron.cb_data = (void*)(intptr_t)alarm->id;
  s_next_alarm = *alarm;
  s_next_alarm_time = cron_job_schedule(&s_next_alarm_cron);
  PBL_LOG(LOG_LEVEL_INFO, "Scheduling alarm %u to go off at %d:%d (%ld) (smart:%d)",
          alarm->id, alarm->config.hour, alarm->config.minute, s_next_alarm_time,
          alarm->config.is_smart);
}

// ----------------------------------------------------------------------------------------------
//! Checks to see if this alarm is the new next alarm. If it is, adjust the timer so it's scheduled.
static void prv_check_and_schedule_alarm(SettingsFile *fd, Alarm *alarm, bool refresh) {
  if (alarm->id == ALARM_INVALID_ID) {
    return;
  }

  if (alarm->config.is_disabled) {
    prv_timeline_remove_alarm(fd, alarm->id);
    return;
  }

  CronJob cron;
  time_t execute_time = prv_build_cron(&alarm->config, &cron);
  prv_timeline_add_alarm(fd, alarm, &cron, rtc_get_time());

  if (s_next_alarm_time == 0 || execute_time < s_next_alarm_time) {
    // This alarm is sooner than the previous one!
    prv_assign_alarm(alarm, &cron);
  }
  if (refresh) {
    timeline_event_refresh();
  }
}

// ----------------------------------------------------------------------------------------------
//! Scans the all the configured alarms and re-adds them all.
//! @return True if at least one alarm was found
static bool prv_reload_alarms(SettingsFile *file) {
  bool alarm_found = false;

  s_next_alarm_time = 0;
  cron_job_unschedule(&s_next_alarm_cron);

  for (int i = 0; i < MAX_CONFIGURED_ALARMS; ++i) {
    AlarmConfig config;
    if (prv_alarm_get_config(file, i, &config)) {
      Alarm alarm = { .id = i, .config = config };
      prv_check_and_schedule_alarm(file, &alarm, false /* refresh */);
      alarm_found = true;
    }
  }

  timeline_event_refresh();
  return alarm_found;
}

// ----------------------------------------------------------------------------------------------
static void prv_put_alarm_event(void) {
  if (!s_alarms_enabled) {
    return;
  }

  AlarmConfig *config = s_most_recent_alarm_id != ALARM_INVALID_ID ? &s_most_recent_alarm_config :
                                                                     NULL;
  const bool is_smart = (config && config->is_smart && CAPABILITY_HAS_HEALTH_TRACKING &&
                         activity_tracking_on());
  PebbleEvent e = (PebbleEvent) {
    .type = PEBBLE_ALARM_CLOCK_EVENT,
    .alarm_clock = {
      .alarm_time = rtc_get_time(),
      .alarm_label = is_smart ? i18n_noop("Smart Alarm") : i18n_noop("Alarm")
    }
  };

  event_put(&e);
}

// ----------------------------------------------------------------------------------------------
static bool prv_record_alarm_op(AlarmId id, AlarmConfig *config, void *context) {
  // Add a pin to the timeline (will show up in the past)
  prv_add_pin(id, config, rtc_get_time(), NULL);

  // Send one triggered event for the alarm
  prv_analytics_op(id, config, (void *)(uintptr_t)AnalyticsEvent_AlarmTriggered);
  return false;
}

// ----------------------------------------------------------------------------------------------
static void prv_clear_snooze_timer(void) {
  new_timer_stop(s_snooze_timer_id);
}

// ----------------------------------------------------------------------------------------------
// Put a trigger event if applicable based on the type of alarm.
static void prv_process_most_recent_alarm(void) {
  // Only processes the most recent alarm since it modifies the alarm config
  AlarmConfig *config = s_most_recent_alarm_id != ALARM_INVALID_ID ? &s_most_recent_alarm_config :
                                                                     NULL;
  bool trigger = true;
  const bool is_smart = (config && config->is_smart);
  if (is_smart) {
    trigger = prv_should_smart_alarm_trigger(config);
    if (!trigger) {
      // Not triggering an event, increment to signify elapsed time and snooze
      s_smart_snooze_counter++;
      prv_snooze_alarm(SMART_ALARM_SNOOZE_DELAY_S);
      return;
    }
  }
  if (trigger) {
    prv_put_alarm_event();
    // Alarm events are triggered for both the initial alarm time and subsequent user snoozes
    // Ensure that we only record the first alarm time
    if (!s_most_recent_alarm_recorded) {
      s_most_recent_alarm_recorded = true;
      // Read from flash since the in-memory cache can be modified
      prv_alarm_operation(s_most_recent_alarm_id, prv_record_alarm_op, NULL);
    }
  }
}

// ----------------------------------------------------------------------------------------------
static void prv_snooze_kernel_bg_callback(void *unused) {
  prv_process_most_recent_alarm();
}

// ----------------------------------------------------------------------------------------------
static void prv_snooze_timer_callback(void *unused) {
  PBL_LOG(LOG_LEVEL_INFO, "Snooze timeout");
  system_task_add_callback(prv_snooze_kernel_bg_callback, NULL);
}

// ----------------------------------------------------------------------------------------------
T_STATIC void prv_timer_kernel_bg_callback(void *data) {
  AlarmId id = (intptr_t) data;
  if (id == ALARM_INVALID_ID) {
    return;
  }

  // We allocate some larger variables on the heap to reduce stack usage
  AlarmConfig *config = &s_most_recent_alarm_config;
  SettingsFile *file = kernel_malloc_check(sizeof(SettingsFile));
  bool rv = prv_file_open_and_lock(file);
  if (!rv) {
    goto cleanup;
  }

  rv = prv_alarm_get_config(file, id, config);

#if CAPABILITY_HAS_HEALTH_TRACKING
  s_smart_snooze_counter = 0;
#endif

  // If this is a just once alarm, then disable it.
  if (rv && config->kind == ALARM_KIND_JUST_ONCE) {
    config->is_disabled = true;
    prv_alarm_set_config(file, id, config); // This will reload the alarms
  } else {
    prv_reload_alarms(file);
  }

cleanup:
  prv_file_close_and_unlock(file);
  kernel_free(file);


  PBL_LOG(LOG_LEVEL_INFO, "Alarm %u timeout", id);
  s_most_recent_alarm_recorded = false;
  s_most_recent_alarm_id = rv ? id : ALARM_INVALID_ID;
  prv_clear_snooze_timer();
  prv_process_most_recent_alarm();
}

static void prv_cron_callback(CronJob *job, void *data) {
  system_task_add_callback(prv_timer_kernel_bg_callback, data);
}

// ----------------------------------------------------------------------------------------------
static void prv_persist_alarm(SettingsFile *fd, Alarm *alarm) {
  PBL_ASSERT(alarm->id >= 0 && alarm->id < MAX_CONFIGURED_ALARMS, "Invalid id %d", alarm->id);

  AlarmStorageKey key = { .id = alarm->id, .type = ALARM_DATA_CONFIG };
  AlarmConfig config = {
    .kind = alarm->config.kind,
    .is_disabled = alarm->config.is_disabled,
    .hour = alarm->config.hour,
    .minute = alarm->config.minute,
    .is_smart = alarm->config.is_smart,
  };
  memcpy(&config.scheduled_days, alarm->config.scheduled_days, sizeof(config.scheduled_days));
  settings_file_set(fd, &key, sizeof(key), &config, sizeof(config));
}

// ----------------------------------------------------------------------------------------------
static void prv_add_and_schedule_alarm(SettingsFile *file, Alarm *alarm) {
  prv_check_and_schedule_alarm(file, alarm, true /* refresh */);
  prv_persist_alarm(file, alarm);
}

// ----------------------------------------------------------------------------------------------
static bool prv_alarm_get_config(SettingsFile *file, AlarmId id, AlarmConfig* config_out) {
  AlarmStorageKey key = { .id = id, .type = ALARM_DATA_CONFIG };
  const int size = settings_file_get_len(file, &key, sizeof(key));
  if (size <= 0) {
    return false;
  }

  AlarmConfig config = {};
  const int load_size = MIN(size, (int)sizeof(config));
  if (settings_file_get(file, &key, sizeof(key), &config, load_size) == S_SUCCESS) {
    if (config.hour > 23 || config.minute > 59) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Invalid config for id %u! Blowing it out! "
              "Hours %u Minutes %u Kind %u", id, config.hour, config.minute,
              config.kind);
      settings_file_delete(file, &key, sizeof(key));
      return false;
    }

    *config_out = (AlarmConfig) {
      .hour = config.hour,
      .minute = config.minute,
      .is_disabled = config.is_disabled,
      .kind = config.kind,
      .is_smart = config.is_smart,
    };
    memcpy(&config_out->scheduled_days, config.scheduled_days, sizeof(config.scheduled_days));
    return true;
  }

  return false;
}

// ----------------------------------------------------------------------------------------------
static void prv_alarm_set_config(SettingsFile *file, AlarmId id, const AlarmConfig* config) {
  PBL_ASSERTN(id >= 0 && id < MAX_CONFIGURED_ALARMS);

  Alarm alarm = { .id = id, .config = *config };
  prv_persist_alarm(file, &alarm);

  prv_reload_alarms(file);
}

// ----------------------------------------------------------------------------------------------
static AlarmId prv_get_next_free_alarm_id(SettingsFile *file) {
  AlarmId id_out = ALARM_INVALID_ID;

  for (int i = 0; i < MAX_CONFIGURED_ALARMS; ++i) {
    AlarmStorageKey key = { .id = i, .type = ALARM_DATA_CONFIG };
    if (settings_file_get_len(file, &key, sizeof(key)) == 0) {
      id_out = i;
      break;
    }
  }

  return id_out;
}

// ----------------------------------------------------------------------------------------------
static int prv_get_day_for_just_once_alarm(int hour, int minute) {
  // Figure out what day the alarm should happen
  struct tm local_time;
  const time_t current_time = rtc_get_time();
  localtime_r(&current_time, &local_time);

  if (hour < local_time.tm_hour || (hour == local_time.tm_hour && minute <= local_time.tm_min)) {
    // The time is before or equal to the current time. Sechedule the alarm for tomorrow
    return (local_time.tm_wday + 1) % DAYS_PER_WEEK;
  } else {
    // The time hasn't happened yet today. Schedule it for today
    return local_time.tm_wday;
  }
}

static void prv_set_day_for_just_once_alarm(AlarmConfig *config, int hour, int minute) {
  const bool scheduled_days[DAYS_PER_WEEK] = {false, false, false, false, false, false, false};
  memcpy(&config->scheduled_days, scheduled_days, sizeof(scheduled_days));

  int wday = prv_get_day_for_just_once_alarm(hour, minute);
  config->scheduled_days[wday] = true;
}

// ----------------------------------------------------------------------------------------------
static void prv_assert_alarm_params(int hour, int minute) {
  PBL_ASSERT(hour < 24 && hour >= 0, "Invalid hour value, %d", hour);
  PBL_ASSERT(minute < 60 && minute >= 0, "Invalid minute value, %d", minute);
}

// ----------------------------------------------------------------------------------------------
static void prv_enable_alarm_config(AlarmConfig *config, bool enable) {
  config->is_disabled = !enable;
  if (enable && config->kind == ALARM_KIND_JUST_ONCE) {
    // Update the day required for the alarm
    prv_set_day_for_just_once_alarm(config, config->hour, config->minute);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Public Functions

AlarmId alarm_create(const AlarmInfo *info) {
  prv_assert_alarm_params(info->hour, info->minute);
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return ALARM_INVALID_ID;
  }

  AlarmId id = prv_get_next_free_alarm_id(&file);
  AlarmConfig config = {
    .hour = info->hour,
    .minute = info->minute,
    .kind = info->kind,
    .is_disabled = false,
    .is_smart = info->is_smart,
  };
  if (info->kind == ALARM_KIND_CUSTOM && info->scheduled_days) {
    prv_set_alarm_custom_op(id, &config, (void *)info->scheduled_days);
  } else {
    prv_set_alarm_kind_op(id, &config, (void *)(uintptr_t)info->kind);
  }

  Alarm alarm = { .id = id, .config = config };
  prv_add_and_schedule_alarm(&file, &alarm);
  prv_file_close_and_unlock(&file);

  analytics_event_alarm(AnalyticsEvent_AlarmCreated, info);

  return id;
}

// ----------------------------------------------------------------------------------------------
typedef bool (*AlarmOperationCallback)(AlarmId id, AlarmConfig *config, void *context);

static void prv_alarm_operation(AlarmId id, AlarmOperationCallback callback, void *context) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return;
  }

  AlarmConfig config;
  bool rv = prv_alarm_get_config(&file, id, &config);
  if (!rv) {
    goto cleanup;
  }

  if (!callback(id, &config, context)) {
    goto cleanup;
  }

  prv_enable_alarm_config(&config, true /* enabled */);
  prv_alarm_set_config(&file, id, &config);

cleanup:
  prv_file_close_and_unlock(&file);
}

// ----------------------------------------------------------------------------------------------
typedef struct SetAlarmTimeContext {
  int hour;
  int minute;
} SetAlarmTimeContext;

static bool prv_set_alarm_time_op(AlarmId id, AlarmConfig *config, void *context) {
  SetAlarmTimeContext *ctx = context;
  if (config->kind == ALARM_KIND_JUST_ONCE) {
    // We have to re-calculate the correct day
    prv_set_day_for_just_once_alarm(config, ctx->hour, ctx->minute);
  }
  config->hour = ctx->hour;
  config->minute = ctx->minute;
  return true;
}

void alarm_set_time(AlarmId id, int hour, int minute) {
  prv_assert_alarm_params(hour, minute);
  prv_alarm_operation(id, prv_set_alarm_time_op, &(SetAlarmTimeContext) { hour, minute });
}

// ----------------------------------------------------------------------------------------------
static bool prv_set_alarm_smart_op(AlarmId id, AlarmConfig *config, void *context) {
  config->is_smart = (uintptr_t)context;
  return true;
}

void alarm_set_smart(AlarmId id, bool smart) {
  prv_alarm_operation(id, prv_set_alarm_smart_op, (void *)(uintptr_t)smart);
}

// ----------------------------------------------------------------------------------------------
static bool prv_set_alarm_kind_op(AlarmId id, AlarmConfig *config, void *context) {
  AlarmKind type = (uintptr_t)context;
  switch (type) {
    case ALARM_KIND_EVERYDAY:
      config->kind = ALARM_KIND_EVERYDAY;
      const bool everyday[DAYS_PER_WEEK] = { true, true, true, true, true, true, true };
      memcpy(&config->scheduled_days, everyday, sizeof(everyday));
      break;
    case ALARM_KIND_WEEKENDS:
      config->kind = ALARM_KIND_WEEKENDS;
      const bool weekends[DAYS_PER_WEEK] = { true, false, false, false, false, false, true };
      memcpy(&config->scheduled_days, weekends, sizeof(weekends));
      break;
    case ALARM_KIND_WEEKDAYS:
      config->kind = ALARM_KIND_WEEKDAYS;
      const bool weekdays[DAYS_PER_WEEK] = { false, true, true, true, true, true, false };
      memcpy(&config->scheduled_days, weekdays, sizeof(weekdays));
      break;
    case ALARM_KIND_JUST_ONCE:
      config->kind = ALARM_KIND_JUST_ONCE;
      const bool no_day[DAYS_PER_WEEK] = { false, false, false, false, false, false, false };
      memcpy(&config->scheduled_days, no_day, sizeof(no_day));
      prv_set_day_for_just_once_alarm(config, config->hour, config->minute);
      break;
    default:
      return false;
  }
  return true;
}

void alarm_set_kind(AlarmId id, AlarmKind kind) {
  prv_alarm_operation(id, prv_set_alarm_kind_op, (void *)(uintptr_t)kind);
}

// ----------------------------------------------------------------------------------------------
static bool prv_set_alarm_custom_op(AlarmId id, AlarmConfig *config, void *context) {
  const bool (*scheduled_days)[DAYS_PER_WEEK] = context;
  config->kind = ALARM_KIND_CUSTOM;
  memcpy(&config->scheduled_days, scheduled_days, sizeof(config->scheduled_days));
  return true;
}

void alarm_set_custom(AlarmId id, const bool scheduled_days[DAYS_PER_WEEK]) {
  prv_alarm_operation(id, prv_set_alarm_custom_op, (void *)scheduled_days);
}

// ----------------------------------------------------------------------------------------------
bool alarm_get_custom_days(AlarmId id, bool scheduled_days[DAYS_PER_WEEK]) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return false;
  }

  AlarmConfig config;
  bool rv = prv_alarm_get_config(&file, id, &config);
  if (!rv) {
    goto cleanup;
  }
  memcpy(scheduled_days, &config.scheduled_days, DAYS_PER_WEEK);

cleanup:
  prv_file_close_and_unlock(&file);
  return rv;
}

// ----------------------------------------------------------------------------------------------
void alarm_set_enabled(AlarmId id, bool enable) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return;
  }

  AlarmConfig config;
  bool rv = prv_alarm_get_config(&file, id, &config);
  if (!rv) {
    goto cleanup;
  }

  if (id == s_most_recent_alarm_id && !enable) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Canceling snooze timer because alarm was disabled");
    // Harmless if the alarm is not currently snoozing - the snooze timer still exists to be stopped
    prv_clear_snooze_timer();
  }

  prv_enable_alarm_config(&config, enable);
  prv_alarm_set_config(&file, id, &config);

cleanup:
  prv_file_close_and_unlock(&file);
}

// ----------------------------------------------------------------------------------------------
void alarm_delete(AlarmId id) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return;
  }

  if (id == s_most_recent_alarm_id) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Canceling snooze timer on delete");
    prv_clear_snooze_timer();
  }

  AlarmStorageKey key = { .id = id, .type = ALARM_DATA_CONFIG };
  settings_file_delete(&file, &key, sizeof(key));
  prv_timeline_remove_alarm(&file, id);
  prv_reload_alarms(&file);

  prv_file_close_and_unlock(&file);
}

// ----------------------------------------------------------------------------------------------
bool alarm_get_enabled(AlarmId id) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return false;
  }

  bool is_enabled = false;
  AlarmConfig config;
  bool rv = prv_alarm_get_config(&file, id, &config);
  if (!rv) {
    goto cleanup;
  }

  is_enabled = !config.is_disabled;

cleanup:
  prv_file_close_and_unlock(&file);
  return is_enabled;
}

// ----------------------------------------------------------------------------------------------
bool alarm_get_hours_minutes(AlarmId id, int *hour_out, int *minute_out) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return false;
  }

  AlarmConfig config;
  bool rv = prv_alarm_get_config(&file, id, &config);
  if (!rv) {
    goto cleanup;
  }

  if (hour_out) {
    *hour_out = config.hour;
  }
  if (minute_out) {
    *minute_out = config.minute;
  }

cleanup:
  prv_file_close_and_unlock(&file);
  return rv;
}

// ----------------------------------------------------------------------------------------------
bool alarm_get_next_enabled_alarm(time_t *next_alarm_time_out) {
  mutex_lock_with_timeout(s_mutex, portMAX_DELAY);

  const bool alarm_is_scheduled = s_next_alarm_time != 0;

  if (alarm_is_scheduled && next_alarm_time_out) {
    *next_alarm_time_out = prv_get_alarm_time(&s_next_alarm, s_next_alarm_time);
  }

  mutex_unlock(s_mutex);

  return alarm_is_scheduled;
}

// ----------------------------------------------------------------------------------------------
bool alarm_is_next_enabled_alarm_smart(void) {
  mutex_lock_with_timeout(s_mutex, portMAX_DELAY);

  const bool alarm_is_scheduled = (s_next_alarm_time != 0);

  bool is_next_alarm_smart = false;
  if (alarm_is_scheduled) {
    is_next_alarm_smart = s_next_alarm.config.is_smart;
  }

  mutex_unlock(s_mutex);

  return is_next_alarm_smart;
}

// ----------------------------------------------------------------------------------------------
bool alarm_get_time_until(AlarmId id, time_t *time_out) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return false;
  }

  bool alarm_is_scheduled = false;
  AlarmConfig config;
  bool rv = prv_alarm_get_config(&file, id, &config);
  if (!rv) {
    goto cleanup;
  }

  if (time_out) {
    CronJob cron;
    *time_out = prv_build_cron(&config, &cron) - rtc_get_time();
  }
  alarm_is_scheduled = true;

cleanup:
  prv_file_close_and_unlock(&file);
  return alarm_is_scheduled;
}

// ----------------------------------------------------------------------------------------------
bool alarm_get_kind(AlarmId id, AlarmKind *kind_out) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return false;
  }

  AlarmConfig config;
  bool rv = prv_alarm_get_config(&file, id, &config);
  if (!rv) {
    goto cleanup;
  }

  if (kind_out) {
    *kind_out = config.kind;
  }

cleanup:
  prv_file_close_and_unlock(&file);
  return rv;
}

static void prv_snooze_alarm(int snooze_delay_s) {
  prv_clear_snooze_timer();
  PBL_LOG(LOG_LEVEL_INFO, "Snoozing for %d minutes", snooze_delay_s / SECONDS_PER_MINUTE);
  bool success = new_timer_start(s_snooze_timer_id, snooze_delay_s * MS_PER_SECOND,
                                 prv_snooze_timer_callback, NULL, 0 /* flags*/);
  PBL_ASSERTN(success);
}

// ----------------------------------------------------------------------------------------------
void alarm_set_snooze_alarm(void) {
  prv_snooze_alarm(s_snooze_delay_m * SECONDS_PER_MINUTE);
}

// ----------------------------------------------------------------------------------------------
uint16_t alarm_get_snooze_delay(void) {
  return s_snooze_delay_m;
}

// ----------------------------------------------------------------------------------------------
void alarm_set_snooze_delay(uint16_t delay_m) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return;
  }

  s_snooze_delay_m = delay_m;

  settings_file_set(&file, ALARM_PREF_KEY_SNOOZE_DELAY,
                    strlen(ALARM_PREF_KEY_SNOOZE_DELAY),
                    &s_snooze_delay_m, sizeof(s_snooze_delay_m));

  prv_file_close_and_unlock(&file);
}

// ----------------------------------------------------------------------------------------------
static bool prv_analytics_op(AlarmId id, AlarmConfig *config, void *context) {
  const AlarmInfo info = {
    .hour = config->hour,
    .minute = config->minute,
    .kind = config->kind,
    .is_smart = config->is_smart,
    .scheduled_days = &config->scheduled_days,
  };

  analytics_event_alarm((intptr_t)context, &info);
  return false;
}

void alarm_dismiss_alarm(void) {
  prv_clear_snooze_timer();

  // Read from flash since the in-memory cache can be modified
  prv_alarm_operation(s_most_recent_alarm_id, prv_analytics_op,
                      (void *)(intptr_t)AnalyticsEvent_AlarmDismissed);
}

// ----------------------------------------------------------------------------------------------
typedef struct {
  AlarmForEach cb;
  void *cb_data;
} ForEachAlarmItrData;

static bool alarm_for_each_itr(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  // check entry is valid
  if (info->val_len == 0 || info->key_len != sizeof(AlarmStorageKey)) {
    return true; // continue iterating
  }

  ForEachAlarmItrData *itr_data = (ForEachAlarmItrData*) context;

  AlarmStorageKey key;
  info->get_key(file, (uint8_t*) &key, info->key_len);

  if (key.type != ALARM_DATA_CONFIG) {
    return true;
  }

  AlarmConfig config;
  bool rv = prv_alarm_get_config(file, key.id, &config);
  if (!rv) {
    return true;
  }

  AlarmInfo alarm_info = {
    .hour = config.hour,
    .minute = config.minute,
    .kind = config.kind,
    .enabled = !config.is_disabled,
    .is_smart = config.is_smart,
    .scheduled_days = &config.scheduled_days,
  };
  itr_data->cb(key.id, &alarm_info, itr_data->cb_data);

  return true;
}

void alarm_for_each(AlarmForEach cb, void *context) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return;
  }
  ForEachAlarmItrData itr_data = {
    .cb = cb,
    .cb_data = context,
  };
  settings_file_each(&file, alarm_for_each_itr, &itr_data);

  prv_file_close_and_unlock(&file);
}

// ----------------------------------------------------------------------------------------------
bool alarm_can_schedule(void) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return false;
  }

  bool rv = false;
  for (int i = 0; i < MAX_CONFIGURED_ALARMS; ++i) {
    AlarmConfig config;
    if (!prv_alarm_get_config(&file, i, &config)) {
      rv = true;
      break;
    }
  }

  prv_file_close_and_unlock(&file);
  return rv;
}

// ----------------------------------------------------------------------------------------------
void alarm_handle_clock_change(void) {
  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return;
  }

  // Update the day for any just once alarms
  for (int i = 0; i < MAX_CONFIGURED_ALARMS; ++i) {
    AlarmConfig config;
    if (prv_alarm_get_config(&file, i, &config)) {
      if (config.kind == ALARM_KIND_JUST_ONCE) {
        prv_set_day_for_just_once_alarm(&config, config.hour, config.minute);
        Alarm alarm = {
          .id = i,
          .config = config,
        };
        prv_persist_alarm(&file, &alarm);
      }
    }
  }

  prv_reload_alarms(&file);

  prv_file_close_and_unlock(&file);
}

// ----------------------------------------------------------------------------------------------
void alarm_init(void) {
  s_mutex = mutex_create();
  PBL_ASSERTN(s_mutex != NULL);

  s_next_alarm_time = 0;
  s_snooze_timer_id = new_timer_create();

  SettingsFile file;
  if (!prv_file_open_and_lock(&file)) {
    return;
  }

  uint16_t snooze_delay_value;
  if (settings_file_get(&file, ALARM_PREF_KEY_SNOOZE_DELAY,
                        strlen(ALARM_PREF_KEY_SNOOZE_DELAY),
                        &snooze_delay_value,
                        sizeof(snooze_delay_value)) == S_SUCCESS) {
    s_snooze_delay_m = snooze_delay_value;
  }

  prv_reload_alarms(&file);
  prv_file_close_and_unlock(&file);
}

// ----------------------------------------------------------------------------------------------
void alarm_service_enable_alarms(bool enable) {
  s_alarms_enabled = enable;
}


// ----------------------------------------------------------------------------------------------
const char *alarm_get_string_for_kind(AlarmKind kind, bool all_caps) {
  const char *alarm_day_text = NULL;
  switch (kind) {
    case ALARM_KIND_EVERYDAY:
      alarm_day_text = all_caps ?
      /// A frequency option for alarms, i.e. the alarm would go off every day. Respect
      /// capitalization!
                          i18n_noop("EVERY DAY") :
      /// A frequency option for alarms, i.e. the alarm would go off every day. Respect
      /// capitalization!
                          i18n_noop("Every Day");
      break;
    case ALARM_KIND_WEEKDAYS:
      alarm_day_text = all_caps ?
      /// A frequency option for alarms, i.e. the alarm would only go off every weekday. Respect
      /// capitalization!
                          i18n_noop("WEEKDAYS") :
      /// A frequency option for alarms, i.e. the alarm would only go off every weekday. Respect
      /// capitalization!
                          i18n_noop("Weekdays");
      break;
    case ALARM_KIND_WEEKENDS:
      alarm_day_text = all_caps ?
      /// A frequency option for alarms, i.e. the alarm would only go off each day on the weekend.
      /// Respect capitalization!
                          i18n_noop("WEEKENDS") :
      /// A frequency option for alarms, i.e. the alarm would only go off each day on the weekend.
      /// Respect capitalization!
                          i18n_noop("Weekends");
      break;
    case ALARM_KIND_JUST_ONCE:

      alarm_day_text = all_caps ?
      /// A frequency option for alarms, i.e. the alarm would only go off one time ever. Respect
      /// capitalization!
                          i18n_noop("ONCE") :
      /// A frequency option for alarms, i.e. the alarm would only go off one time ever. Respect
      /// capitalization!
                          i18n_noop("Once");
      break;
    case ALARM_KIND_CUSTOM:
      // TODO: Use selected days as the string
      alarm_day_text = all_caps ?
      /// A frequency option for alarms, i.e. the alarm would only go off on a custom choice of
      /// days. Respect capitalization!
                          i18n_noop("CUSTOM") :
      /// A frequency option for alarms, i.e. the alarm would only go off on a custom choice of
      /// days. Respect capitalization!
                          i18n_noop("Custom");
      break;
    default:
      alarm_day_text = "";
      break;
  }

  return alarm_day_text;
}

void alarm_get_string_for_custom(bool scheduled_days[DAYS_PER_WEEK], char *alarm_day_text) {
  // 4 chars per day, 3 for letters and 1 for comma
  // max length = 7 days in a week * 4 chars per day = 28
  static const char *day_strings[7] = { i18n_noop("Sun"),
                                        i18n_noop("Mon"),
                                        i18n_noop("Tue"),
                                        i18n_noop("Wed"),
                                        i18n_noop("Thu"),
                                        i18n_noop("Fri"),
                                        i18n_noop("Sat")
                                      };
  static const char *full_day_strings[7] = {
                                             i18n_noop("Sundays"),
                                             i18n_noop("Mondays"),
                                             i18n_noop("Tuesdays"),
                                             i18n_noop("Wednesdays"),
                                             i18n_noop("Thursdays"),
                                             i18n_noop("Fridays"),
                                             i18n_noop("Saturdays")
                                           };

  uint8_t num_days_scheduled = 0, latest_day_scheduled = 0;
  // Monday should come first in the list
  for (unsigned int i = 1; i < 8; i++) {
    if (scheduled_days[i % 7] == true) {
      num_days_scheduled++;
      latest_day_scheduled = i % 7;
      const char *day_string = i18n_get(day_strings[i % 7], day_strings);
      strcat(alarm_day_text, day_string);
      strcat(alarm_day_text, ",");
      i18n_free(day_strings[i % 7], day_strings); // copied in by strcat. We can free it
    }
  }
  if (num_days_scheduled == 1) {
    // Write the full day string
    const char *full_day_string = i18n_get(full_day_strings[latest_day_scheduled],
                                           full_day_strings);
    strcpy(alarm_day_text, full_day_string);
    i18n_free(full_day_strings[latest_day_scheduled], full_day_strings); // copied in above.
  } else if (num_days_scheduled > 1) {
    // Write the shortened day strings, remove last ','
    alarm_day_text[strnlen(alarm_day_text, 28) - 1] = 0;
  }
}

// ----------------------------------------------------------------------------------------------
void command_alarm(void) {
  prv_put_alarm_event();
}
