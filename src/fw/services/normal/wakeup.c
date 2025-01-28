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

#include "wakeup.h"

#include "popups/wakeup_ui.h"

#include "os/mutex.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "process_management/app_storage.h"
#include "services/common/clock.h"
#include "services/common/event_service.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/system_task.h"
#include "services/normal/settings/settings_file.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/units.h"

#include "kernel/pbl_malloc.h"

#define SETTINGS_FILE_NAME "wakeup"
// settings file => 29 bytes * 30 apps * 8 wakeup events = ~7000 bytes
// This should be more than enough space to store all the wakeup events we will ever want.
#define SETTINGS_FILE_SIZE KiBYTES(8)
// This represents the size of the buffer that is allocated to pass into the wakeup_ui
// to show that an app's wakeup event had triggered while the watch was off. To reduce
// complexity, I have hard coded this buffer to a max size instead of going the linked_list
// route. 16 apps should be more than enough.
#define NUM_APPS_ALERT_ON_BOOT 16

static PebbleMutex *s_mutex;

//! Settings entries == WakeupId are stored by timestamp,
//! duplicate timestamps not allowed (can't have 2 wakeup events at same time)
//! repeating and repeat_hours_offset were included for future use
//! and use in repeat support for alarms
typedef struct PACKED {
  Uuid uuid; //!< UUID of app that scheduled the wakeup event
  int32_t reason; //!< App provided value to differentiate wakeup event
  bool repeating; //!< Enable event repetition
  uint16_t repeat_hours_offset; //!< repeat hour interval
  bool notify_if_missed; //!< Notify user if wakeup event has been missed
  time_t timestamp; //!< The time at which this entry will wake up at
  bool utc; //!< If timezone has been set, the this is UTC time
} WakeupEntry;

typedef struct PACKED {
  WakeupId current_wakeup_id;
  WakeupId next_wakeup_id;
  time_t timestamp;
} WakeupState;

struct prv_missed_events_s {
  uint8_t missed_apps_count;
  AppInstallId *missed_app_ids;
};

struct prv_check_app_and_wakeup_event_s {
  time_t wakeup_timestamp; //!< Timestamp of the WakeupEntry
  int wakeup_count; //!< wakeup event count for app, negative for error (StatusCode)
};

// Local prototypes
static WakeupEntry prv_wakeup_settings_get_entry(WakeupId wakeup_id);
static void prv_wakeup_settings_delete_entry(WakeupId wakeup_id);
static StatusCode prv_wakeup_settings_add_entry(WakeupId wakeup_id, WakeupEntry entry);
static void prv_wakeup_timer_next_pending(void);

static bool s_wakeup_enabled = false;
static TimerID s_current_timer_id = TIMER_INVALID_ID; // single timer reused for each event
// single structure containing the global wakeup state
static WakeupState s_wakeup_state = { -1, -1, 0 };
static bool s_catchup_enabled = false; // enables catching up with missed events

void wakeup_dispatcher_system_task(void *data){
  WakeupId wakeup_id = (WakeupId)data;
  WakeupEntry entry = prv_wakeup_settings_get_entry(wakeup_id);

  // Delete event from settings
  prv_wakeup_settings_delete_entry(wakeup_id);

  AppInstallId app_id = app_install_get_id_for_uuid(&entry.uuid);

  // If specified app isn't currently running, launch
  if (!(app_manager_get_current_app_id() == app_id)) {
    // Lookup app, and if installed, launch
    if (app_id != INSTALL_ID_INVALID) {
      PebbleLaunchAppEventExtended* data =
          kernel_malloc_check(sizeof(PebbleLaunchAppEventExtended));
      *data = (PebbleLaunchAppEventExtended) {
        .common.reason = APP_LAUNCH_WAKEUP,
        .wakeup.wakeup_id = wakeup_id,
        .wakeup.wakeup_reason = entry.reason,
      };
      data->common.args = &data->wakeup;

      PebbleEvent event = {
        .type = PEBBLE_APP_LAUNCH_EVENT,
        .launch_app = {
          .id = app_id,
          .data = data
        }
      };

      event_put(&event);
    }
  } else {
    // If app running, send event
    PebbleEvent event = {
      .type = PEBBLE_WAKEUP_EVENT,
      .wakeup = {
        .wakeup_info = {
          .wakeup_id = wakeup_id,
          .wakeup_reason = entry.reason
        }
      }
    };
    event_put(&event);
  }

  prv_wakeup_timer_next_pending();
}

// This callback is the system dispatcher that wakes up the application by AppInstallId
// OR queues a wakeup callback for that application
// and is triggered by NewTimer
static void prv_wakeup_dispatcher(void *data) {
  // Place actual work on system_task to unload it from new_timer task
  system_task_add_callback(wakeup_dispatcher_system_task, data);
}

static bool prv_find_next_wakeup_id_callback(SettingsFile *file,
    SettingsRecordInfo *info, void *context) {
  // Check if valid entry
  if (info->key_len != sizeof(WakeupId) || info->val_len != sizeof(WakeupEntry)) {
    return true; // continue iterating
  }

  WakeupId wakeup_id;
  info->get_key(file, (uint8_t*)&wakeup_id, sizeof(WakeupId));

  WakeupEntry entry;
  info->get_val(file, (uint8_t*)&entry, sizeof(WakeupEntry));

  // If the wakeup_id is valid, and the timestamp of the entry is closer than
  // the timestamp of our global wakeup state, then set the next close wakeup event
  if (wakeup_id > 0 && (s_wakeup_state.current_wakeup_id == -1 ||
        entry.timestamp < s_wakeup_state.timestamp)) {
    s_wakeup_state.timestamp = entry.timestamp;
    s_wakeup_state.current_wakeup_id = wakeup_id;
  }

  return true; // continue iterating
}


// Checks for the next pending wakeup event and sets up a timer for the event
static void prv_wakeup_timer_next_pending(void) {
  if (!s_wakeup_enabled) {
    return;
  }

  // If there is already a wakeup timer scheduled, cancel it. There will be a
  // new timer scheduled for the soonest wakeup that is registered.
  if (new_timer_scheduled(s_current_timer_id, NULL)) {
    new_timer_stop(s_current_timer_id);
  }

  mutex_lock(s_mutex);
  {
    // Find the next event to occur
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      // Reset wakeup state to use for the search
      s_wakeup_state.current_wakeup_id = -1;
      s_wakeup_state.timestamp = 0;
      settings_file_each(&wakeup_settings, prv_find_next_wakeup_id_callback, NULL);
      settings_file_close(&wakeup_settings);
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Error: could not open APP_WAKEUP settings");
    }
  }
  mutex_unlock(s_mutex);

  // Create a timer for the found wakeup id, given it's valid
  if (s_wakeup_state.current_wakeup_id >= 0) {
    int32_t timestamp = s_wakeup_state.timestamp;
    time_t current_time = rtc_get_time();
    int32_t time_difference = timestamp - current_time;

    // If time_difference is negative, this was a missed past event due to set_time
    // changing current time beyond the event
    // Note: this catches the edge case that there are several wakeup events skipped
    // and avoids clobbering these events with a WAKEUP_CATCHUP_WINDOW gap,
    // including an event occurring after missed events
    if (time_difference < 0 || s_catchup_enabled) {
      // next catchup_enabled state before modifying time_difference
      s_catchup_enabled = (time_difference < 0) ? true : false;
      // Enforces the catchup gap to be at least WAKEUP_CATCHUP_WINDOW gap
      time_difference = MAX(time_difference, WAKEUP_CATCHUP_WINDOW);
    }

    // timers are in milliseconds, set main callback dispatch for wakeup
    // WakeupId is used to save/restore/lookup wakeup events
    new_timer_start(s_current_timer_id, (time_difference * 1000), prv_wakeup_dispatcher,
        (void*)((intptr_t)s_wakeup_state.current_wakeup_id), 0);
  }
}


static void prv_migrate_events_callback(SettingsFile *old_file, SettingsFile *new_file,
    SettingsRecordInfo *info, void *utc_diff) {
  if (!utc_diff || info->key_len != sizeof(WakeupId) || info->val_len != sizeof(WakeupEntry)) {
    return;
  }

  WakeupId wakeup_id;
  info->get_val(old_file, (uint8_t*)&wakeup_id, sizeof(WakeupId));

  WakeupEntry entry;
  info->get_val(old_file, (uint8_t*)&entry, sizeof(WakeupEntry));

  // Migrate the entries to the new timezone
  if (entry.utc == false) {
    entry.timestamp -= *((int*)utc_diff);
    entry.utc = true;
    if (wakeup_id == s_wakeup_state.current_wakeup_id) {
      s_wakeup_state.timestamp = entry.timestamp;
    }
  }

  // Write the new entry to the settings file.  We always write as there's no
  // chance of it being invalid.
  settings_file_set(new_file, (uint8_t*)&wakeup_id, sizeof(WakeupId),
      (uint8_t*)&entry, sizeof(WakeupEntry));
}

static bool prv_check_for_events(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  bool *event_found = (bool *)context;
  *event_found = true;
  return false; // stop iterating
}

static void prv_update_events_callback(SettingsFile *old_file, SettingsFile *new_file,
    SettingsRecordInfo *info, void *context) {
  // Check if valid entry
  if (!context || info->key_len != sizeof(WakeupId)) {
    return;
  }

  bool struct_size_mismatch = info->val_len != sizeof(WakeupEntry);
  bool struct_migration = !clock_is_timezone_set() && struct_size_mismatch;
  if (struct_size_mismatch && !struct_migration) {
    return;
  }

  struct prv_missed_events_s *missed_events = (struct prv_missed_events_s*)context;

  WakeupId wakeup_id;
  info->get_key(old_file, (uint8_t*)&wakeup_id, sizeof(WakeupId));

  WakeupEntry entry;
  info->get_val(old_file, (uint8_t*)&entry, info->val_len);
  if (struct_migration) {
    entry.timestamp = wakeup_id; // WakeupId (key) is a timestamp
    entry.utc = false; // If we're migrating, this has not been utc
  }

  int32_t timestamp = entry.timestamp;
  time_t current_time = rtc_get_time();
  int32_t time_difference = timestamp - current_time;

  if (timestamp >= s_wakeup_state.next_wakeup_id) {
    s_wakeup_state.next_wakeup_id = timestamp + 1;
  } else if (wakeup_id >= s_wakeup_state.next_wakeup_id) {
    s_wakeup_state.next_wakeup_id = wakeup_id + 1;
  }

  // schedule non-expired events
  if (time_difference > 0) {
    // Using settings_file_rewrite, need to write to keep key/value
    settings_file_set(new_file, (uint8_t*)&wakeup_id, sizeof(WakeupId),
        (uint8_t*)&entry, sizeof(WakeupEntry));
  } else {
    if (entry.notify_if_missed) {
      if (missed_events->missed_app_ids == NULL) {
        // This is allocated here, but free'd in the wakeup_ui.h module
        missed_events->missed_app_ids =
            kernel_malloc(NUM_APPS_ALERT_ON_BOOT * sizeof(AppInstallId));
      }
      if (missed_events->missed_apps_count > NUM_APPS_ALERT_ON_BOOT) {
        // We have more than NUM_APPS_ALERT_ON_BOOT apps that had events fire while the watch
        // was shut off. Very rare this will happen, but just down show that the apps > 16 missed
        // an event.
        return;
      }
      // Set the AppInstallId of the app that had an alert fired
      missed_events->missed_app_ids[missed_events->missed_apps_count++] =
          app_install_get_id_for_uuid(&entry.uuid);
    }
    // Deletes the entry automatically if not written
  }
}

void wakeup_init(void) {
  struct prv_missed_events_s missed_events = { 0, NULL };

  s_mutex = mutex_create();

  event_service_init(PEBBLE_WAKEUP_EVENT, NULL, NULL);

  // Create single reusable timer for wakeup events
  s_current_timer_id = new_timer_create();
  s_wakeup_state.next_wakeup_id = rtc_get_time();
  s_wakeup_state.timestamp = -1;

  SettingsFile wakeup_settings;
  if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) != S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error: could not open wakeup settings");
    return;
  }

  // Want to check if there are any events first to prevent us from rewriting the file on boot if
  // we don't need to.
  bool event_found = false;
  settings_file_each(&wakeup_settings, prv_check_for_events, &event_found);
  if (event_found) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Rewriting wakeup file");
    // Update settings file removing expired events
    settings_file_rewrite(&wakeup_settings, prv_update_events_callback, &missed_events);
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Not rewriting wakeup file because no entries were found");
  }
  settings_file_close(&wakeup_settings);

  // If wakeup events were missed by apps requesting notify_if_missed
  // popup a notification window displaying these apps
  if (missed_events.missed_apps_count) {
    wakeup_popup_window(missed_events.missed_apps_count, missed_events.missed_app_ids);
  }
}


static bool prv_compiled_without_utc_support(void) {
  static const Version first_utc_version = {
    // See list of changes in pebble_process_info.h. Apps compiled prior to this version will
    // get local time returned from the time() call.
    0x5,
    0x2f
  };
  Version app_sdk_version = process_metadata_get_sdk_version(
                                            sys_process_manager_get_current_process_md());

  if (version_compare(app_sdk_version, first_utc_version) < 0) {
    return true;
  }
  return false;
}


DEFINE_SYSCALL(WakeupId, sys_wakeup_schedule, time_t timestamp, int32_t reason,
                                              bool notify_if_missed) {

  if (prv_compiled_without_utc_support()) {
    // Legacy apps get local time returned from the time() call.
    timestamp = time_local_to_utc(timestamp);
  }

  time_t current_time = rtc_get_time();
  int32_t time_difference = timestamp - current_time;
  WakeupId wakeup_id = s_wakeup_state.next_wakeup_id++;

  // Disallow scheduling past events
  if (time_difference <= 0) {
    return E_INVALID_ARGUMENT;
  }

  Uuid uuid = app_manager_get_current_app_md()->uuid;

  WakeupEntry entry = {
    .uuid = uuid,
    .reason = reason,
    .notify_if_missed = notify_if_missed,
    .timestamp = timestamp,
    .utc = clock_is_timezone_set()
  };

  // Add to settings file
  StatusCode retval = prv_wakeup_settings_add_entry(wakeup_id, entry);

  // If there was an error adding the wakeup event, return the error
  if (retval < S_SUCCESS) {
    return retval;
  }

  // If this new event is sooner than the currently scheduled timer, make this
  // the current one
  prv_wakeup_timer_next_pending();
  return wakeup_id;
}


static void prv_wakeup_settings_delete_entry(WakeupId wakeup_id) {
  mutex_lock(s_mutex);
  {
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      settings_file_delete(&wakeup_settings, (uint8_t*)&wakeup_id, sizeof(WakeupId));
      settings_file_close(&wakeup_settings);
    }
  }
  mutex_unlock(s_mutex);
}

static WakeupEntry prv_wakeup_settings_get_entry(WakeupId wakeup_id) {
  WakeupEntry entry = {{0}};

  mutex_lock(s_mutex);
  {
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      settings_file_get(&wakeup_settings, (uint8_t*)&wakeup_id, sizeof(WakeupId),
                        (uint8_t*)&entry, sizeof(WakeupEntry));
      settings_file_close(&wakeup_settings);
    }
  }
  mutex_unlock(s_mutex);
  return entry;
}

DEFINE_SYSCALL(void, sys_wakeup_delete, WakeupId wakeup_id) {

  WakeupEntry entry = prv_wakeup_settings_get_entry(wakeup_id);

  // Only allow owner to delete its own wakeup events
  if (uuid_equal(&app_manager_get_current_app_md()->uuid, &entry.uuid)) {
    if (wakeup_id == s_wakeup_state.current_wakeup_id &&
        new_timer_scheduled(s_current_timer_id, NULL)) {
      new_timer_stop(s_current_timer_id);
    }
    prv_wakeup_settings_delete_entry(wakeup_id);
    prv_wakeup_timer_next_pending();
  }
}

static bool prv_check_count_and_availability_callback(SettingsFile *file,
    SettingsRecordInfo *info, void *context) {
  // Check if valid entry
  if (!context || info->key_len != sizeof(WakeupId) || info->val_len != sizeof(WakeupEntry)) {
    return true; // continue iterating
  }

  struct prv_check_app_and_wakeup_event_s *check = (struct prv_check_app_and_wakeup_event_s*)context;

  WakeupId wakeup_id;
  info->get_key(file, (uint8_t*)&wakeup_id, sizeof(WakeupId));

  WakeupEntry entry;
  info->get_val(file, (uint8_t*)&entry, sizeof(WakeupEntry));

  //If we have already flagged an error, just skip the rest
  if (check->wakeup_count < S_SUCCESS) {
    return true; // continue iterating
  }

  if (uuid_equal(&app_manager_get_current_app_md()->uuid, &entry.uuid)) {
    check->wakeup_count++;
  }
  // If the wakeup_id is with the same minute as another wakeup event
  if ((entry.timestamp - WAKEUP_EVENT_WINDOW < check->wakeup_timestamp) &&
      (check->wakeup_timestamp < (entry.timestamp + WAKEUP_EVENT_WINDOW))) {
    check->wakeup_count = E_RANGE;
  }

  return true; // continue iterating
}


static StatusCode prv_wakeup_settings_add_entry(WakeupId wakeup_id, WakeupEntry entry) {
  status_t status = S_SUCCESS;

  mutex_lock(s_mutex);
  {
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      // Check if current app already has MAX_WAKEUP_EVENTS_PER_APP scheduled
      // or if the minute event window is already occupied
      struct prv_check_app_and_wakeup_event_s check = {
        .wakeup_count = 0,
        .wakeup_timestamp = entry.timestamp
      };
      settings_file_each(&wakeup_settings, prv_check_count_and_availability_callback, &check);

      if (check.wakeup_count < S_SUCCESS) {
        status = check.wakeup_count;
      } else if (check.wakeup_count >= MAX_WAKEUP_EVENTS_PER_APP) {
        status = E_OUT_OF_RESOURCES;
      } else {
        settings_file_set(&wakeup_settings, (uint8_t*)&wakeup_id, sizeof(WakeupId),
                          (uint8_t*)&entry, sizeof(WakeupEntry));
      }
      settings_file_close(&wakeup_settings);
    } else {
      status = E_INTERNAL;
    }
  }
  mutex_unlock(s_mutex);

  return status;
}

static void prv_delete_events_by_uuid_callback(SettingsFile *old_file, SettingsFile *new_file,
    SettingsRecordInfo *info, void *context) {
  // Check if valid entry
  if (info->key_len != sizeof(WakeupId) || info->val_len != sizeof(WakeupEntry)) {
    return;
  }

  WakeupId wakeup_id;
  info->get_key(old_file, (uint8_t*)&wakeup_id, sizeof(WakeupId));

  WakeupEntry entry;
  info->get_val(old_file, (uint8_t*)&entry, sizeof(WakeupEntry));

  // If the UUID is equal, delete the entry
  if (uuid_equal(&app_manager_get_current_app_md()->uuid, &entry.uuid)) {
    // if this is the current timer event, cancel it
    if (wakeup_id == s_wakeup_state.current_wakeup_id &&
        new_timer_scheduled(s_current_timer_id, NULL)) {
      new_timer_stop(s_current_timer_id);
    }
    // Deletes the entry automatically if not written
  } else { // Keep the entry
    // Using settings_file_rewrite, need to write to keep key/value
    settings_file_set(new_file, (uint8_t*)&wakeup_id, sizeof(WakeupId),
        (uint8_t*)&entry, sizeof(WakeupEntry));
  }
}


DEFINE_SYSCALL(void, sys_wakeup_cancel_all_for_app, void) {
  mutex_lock(s_mutex);
  {
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      // Update settings file removing all events with UUID = uuid
      settings_file_rewrite(&wakeup_settings, prv_delete_events_by_uuid_callback, NULL);
      settings_file_close(&wakeup_settings);
    }
  }
  mutex_unlock(s_mutex);

  // We may have cancelled the timer, next_pending will check
  prv_wakeup_timer_next_pending();
}

DEFINE_SYSCALL(time_t, sys_wakeup_query, WakeupId wakeup_id) {
  status_t status = E_DOES_NOT_EXIST;
  WakeupEntry entry = {};

  if (wakeup_id < 0) {
    return status;
  }

  mutex_lock(s_mutex);
  {
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      // Check if the wakeup id is valid by seeing if it is in the wakeup settings_file
      status = settings_file_get(&wakeup_settings, (uint8_t*)&wakeup_id, sizeof(WakeupId),
                                 (uint8_t*)&entry, sizeof(WakeupEntry));
      settings_file_close(&wakeup_settings);
    } else {
      status = E_INTERNAL;
    }
  }
  mutex_unlock(s_mutex);

  if (status != S_SUCCESS) {
    return status;
  }

  // timer doesn't belong to this app
  if (!uuid_equal(&app_manager_get_current_app_md()->uuid, &entry.uuid)) {
    return E_DOES_NOT_EXIST;
  }

  time_t return_time = entry.timestamp;
  if (prv_compiled_without_utc_support()) {
    // Legacy apps expect everything in local time.
    return_time = time_utc_to_local(return_time);
  }
  return return_time;
}

void wakeup_enable(bool enable) {
  bool was_enabled = s_wakeup_enabled;
  s_wakeup_enabled = enable;
  if (enable && !was_enabled) {
    prv_wakeup_timer_next_pending();
  } else if (!enable && s_current_timer_id &&
             new_timer_scheduled(s_current_timer_id, NULL)) {
    new_timer_stop(s_current_timer_id);
  }
}

TimerID wakeup_get_current(void) {
  return s_current_timer_id;
}

WakeupId wakeup_get_next_scheduled(void) {
  return s_wakeup_state.current_wakeup_id;
}

void wakeup_migrate_timezone(int utc_diff) {
  mutex_lock(s_mutex);
  {
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      settings_file_rewrite(&wakeup_settings, prv_migrate_events_callback, (void*)&utc_diff);
      settings_file_close(&wakeup_settings);
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Error: could not open wakeup settings");
    }
  }
  mutex_unlock(s_mutex);
}

static void prv_wakeup_rewrite_kernel_bg_cb(void *data) {
  // Update each wakeup entry via prv_update_events_callback and record any missed events
  struct prv_missed_events_s missed_events = { 0, NULL };

  mutex_lock(s_mutex);
  {
    SettingsFile wakeup_settings;
    if (settings_file_open(&wakeup_settings, SETTINGS_FILE_NAME, SETTINGS_FILE_SIZE) == S_SUCCESS) {
      // Update each wakeup entry via prv_update_events_callback and record any missed events
      settings_file_rewrite(&wakeup_settings, prv_update_events_callback, &missed_events);
      settings_file_close(&wakeup_settings);
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Error: could not open wakeup settings");
    }
  }
  mutex_unlock(s_mutex);

  // If any events were missed due to time change display the wakeup popup
  if (missed_events.missed_apps_count) {
    wakeup_popup_window(missed_events.missed_apps_count, missed_events.missed_app_ids);
  }

  // Setup a timer for the next wakeup event; will return if wakeup is not enabled
  prv_wakeup_timer_next_pending();
}

void wakeup_handle_clock_change(void) {
  // Offload the rewrite of the wakeup file to KernelBG as it may take a while
  //
  // TODO: The flash burden of this routine could also be reduced by not doing
  // rewrites and instead updating records in place
  if (pebble_task_get_current() == PebbleTask_KernelBackground) {
    prv_wakeup_rewrite_kernel_bg_cb(NULL);
  } else {
    system_task_add_callback(prv_wakeup_rewrite_kernel_bg_cb, NULL);
  }
}
