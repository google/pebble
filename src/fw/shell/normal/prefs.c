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

#include "shell/normal/quick_launch.h"
#include "shell/normal/watchface.h"
#include "shell/prefs.h"
#include "shell/prefs_private.h"
#include "shell/system_theme.h"

#include "apps/system_apps/toggle/quiet_time.h"
#include "board/board.h"
#include "drivers/backlight.h"
#include "mfg/mfg_info.h"
#include "os/mutex.h"
#include "popups/timeline/peek.h"
#include "process_management/app_install_manager.h"
#include "process_management/process_manager.h"
#include "services/common/hrm/hrm_manager.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/bluetooth/ble_hrm.h"
#include "services/normal/settings/settings_file.h"
#include "services/normal/timeline/peek.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"
#include "util/uuid.h"

#if CAPABILITY_HAS_HEALTH_TRACKING
#include "services/normal/activity/activity.h"
#include "services/normal/activity/activity_insights.h"
#endif

#include <stdbool.h>

static PebbleMutex *s_mutex;

#define PREF_KEY_CLOCK_24H "clock24h"
static bool s_clock_24h = false;

#define PREF_KEY_CLOCK_TIMEZONE_SOURCE_IS_MANUAL "timezoneSource"
static bool s_clock_timezone_source_is_manual = false;

#define PREF_KEY_CLOCK_PHONE_TIMEZONE_ID "automaticTimezoneID"
static int16_t s_clock_phone_timezone_id = -1;

#define PREF_KEY_UNITS_DISTANCE "unitsDistance"
static uint8_t s_units_distance = UnitsDistance_Miles;

#define PREF_KEY_BACKLIGHT_BEHAVIOUR_DEPRECATED "lightBehaviour"
#define PREF_KEY_BACKLIGHT_ENABLED "lightEnabled"
static bool s_backlight_enabled = true;
#define PREF_KEY_BACKLIGHT_AMBIENT_SENSOR_ENABLED "lightAmbientSensorEnabled"
static bool s_backlight_ambient_sensor_enabled = true;

#define PREF_KEY_BACKLIGHT_TIMEOUT_MS "lightTimeoutMs"
static uint32_t s_backlight_timeout_ms = DEFAULT_BACKLIGHT_TIMEOUT_MS;
#define PREF_KEY_BACKLIGHT_INTENSITY "lightIntensity"
static uint16_t s_backlight_intensity; // default pulled from BOARD_CONFIGs in shell_prefs_init()

#define PREF_KEY_BACKLIGHT_MOTION "lightMotion"
static bool s_backlight_motion_enabled = true;

#define PREF_KEY_STATIONARY "stationaryMode"
#if RELEASE && !PLATFORM_SPALDING
static bool s_stationary_mode_enabled = false;
#else
static bool s_stationary_mode_enabled = true;
#endif

#define PREF_KEY_DEFAULT_WORKER "workerId"
static Uuid s_default_worker = UUID_INVALID_INIT;

// We use "textStyle" to indicate the content size
#define PREF_KEY_TEXT_STYLE "textStyle"
static uint8_t s_text_style = PreferredContentSizeDefault;
#if !UNITTEST
_Static_assert(sizeof(PreferredContentSize) == sizeof(s_text_style),
               "sizeof(PreferredContentSize) grew, pref needs to be migrated!");
#endif

#define PREF_KEY_LANG_ENGLISH "langEnglish"
static bool s_language_english = false;

typedef struct QuickLaunchPreference {
  bool enabled;
  Uuid uuid;
} QuickLaunchPreference;

#define PREF_KEY_QUICK_LAUNCH_UP "qlUp"
#define PREF_KEY_QUICK_LAUNCH_DOWN "qlDown"
#define PREF_KEY_QUICK_LAUNCH_SELECT "qlSelect"
#define PREF_KEY_QUICK_LAUNCH_BACK "qlBack"

static QuickLaunchPreference s_quick_launch_up = {
  .enabled = true,
  .uuid = UUID_INVALID_INIT,
};

static QuickLaunchPreference s_quick_launch_down = {
  .enabled = true,
  .uuid = UUID_INVALID_INIT,
};

static QuickLaunchPreference s_quick_launch_select = {
  .enabled = true,
  .uuid = UUID_INVALID_INIT,
};

static QuickLaunchPreference s_quick_launch_back = {
  .enabled = true,
  .uuid = QUIET_TIME_TOGGLE_UUID,
};

#define PREF_KEY_QUICK_LAUNCH_SETUP_OPENED "qlSetupOpened"
static uint8_t s_quick_launch_setup_opened = 0;

#define PREF_KEY_DEFAULT_WATCHFACE "watchface"
static Uuid s_default_watchface = UUID_INVALID_INIT;

#define PREF_KEY_WELCOME_VERSION "welcomeVersion"
static uint8_t s_welcome_version = 0;

#if CAPABILITY_HAS_HEALTH_TRACKING
#define PREF_KEY_ACTIVITY_PREFERENCES "activityPreferences"
static ActivitySettings s_activity_preferences = ACTIVITY_DEFAULT_PREFERENCES;

#define PREF_KEY_ACTIVITY_ACTIVATED_TIMESTAMP "activityActivated"
static time_t s_activity_activation_timestamp = 0;

#define PREF_KEY_ACTIVITY_ACTIVATION_DELAY_INSIGHT "activityActivationDelayInsights"
static uint32_t s_activity_activation_delay_insight = 0;

#define PREF_KEY_ACTIVITY_HEALTH_APP_OPENED "activityHealthAppOpened"
static uint8_t s_activity_prefs_health_app_opened = 0;

#define PREF_KEY_ACTIVITY_WORKOUT_APP_OPENED "activityWorkoutAppOpened"
static uint8_t s_activity_prefs_workout_app_opened = 0;

#define PREF_KEY_ALARMS_APP_OPENED "alarmsAppOpened"
static uint8_t s_alarms_app_opened = 0;

#define PREF_KEY_ACTIVITY_HRM_PREFERENCES "hrmPreferences"
static ActivityHRMSettings s_activity_hrm_preferences = ACTIVITY_HRM_DEFAULT_PREFERENCES;

#define PREF_KEY_ACTIVITY_HEART_RATE_PREFERENCES "heartRatePreferences"
static HeartRatePreferences s_activity_hr_preferences = ACTIVITY_HEART_RATE_DEFAULT_PREFERENCES;
#endif // CAPABILITY_HAS_HEALTH_TRACKING

#if PLATFORM_SPALDING
#define PREF_KEY_DISPLAY_USER_OFFSET "displayUserOffset"
static GPoint s_display_user_offset = {0, 0};
#define PREF_KEY_SHOULD_PROMPT_DISPLAY_CALIBRATION "promptDisplayCal"
static bool s_should_prompt_display_calibration = true;
#endif

#if CAPABILITY_HAS_TIMELINE_PEEK
#define PREF_KEY_TIMELINE_SETTINGS_OPENED "timelineSettingsOpened"
static uint8_t s_timeline_settings_opened = 0;

#define PREF_KEY_TIMELINE_PEEK_ENABLED "timelineQuickViewEnabled"
static bool s_timeline_peek_enabled = true;

#define PREF_KEY_TIMELINE_PEEK_BEFORE_TIME_M "timelineQuickViewBeforeTimeMin"
static uint16_t s_timeline_peek_before_time_m =
    (TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S / SECONDS_PER_MINUTE);
#endif

// ============================================================================================
// Handlers for each pref that validate the new setting and store the new value in our globals.
// This handler will be called when the setting is changed from inside the firmware using one of
// the "set" calls or when a pref is changed via a blob_db insert operation from the mobile
// (after we receive the blob_db update event).
//
// If changing of the setting requires more than just setting a global, this handler is the
// place to perform those other actions.
//
// If the handler gets passed a invalid new value, set its s_* global to a default value,
// and return false. This will trigger a rewrite of the s_* global to the backing file.
//
// Each of these functions MUST be named using the following pattern because they are called
// programmatically via the PREFS_MACRO macro defined below:
//   static bool prv_set_<static_var_name>(<static_var_type> new_value)
static bool prv_set_s_clock_24h(bool *new_value) {
  s_clock_24h = *new_value;
  return true;
}

static bool prv_set_s_clock_timezone_source_is_manual(bool *new_value) {
  s_clock_timezone_source_is_manual = *new_value;
  return true;
}

static bool prv_set_s_clock_phone_timezone_id(int16_t *new_value) {
  s_clock_phone_timezone_id = *new_value;
  return true;
}

static bool prv_set_s_units_distance(uint8_t *new_unit) {
  if (*new_unit >= UnitsDistanceCount) {
    s_units_distance = UnitsDistance_Miles;
    return false;
  }
  s_units_distance = *new_unit;
  return true;
};

static bool prv_set_s_backlight_enabled(bool *enabled) {
  s_backlight_enabled = *enabled;
  return true;
}

static bool prv_set_s_backlight_ambient_sensor_enabled(bool *enabled) {
  s_backlight_ambient_sensor_enabled = *enabled;
  return true;
}

static bool prv_set_s_backlight_timeout_ms(uint32_t *timeout_ms) {
  if (*timeout_ms > 0) {
    s_backlight_timeout_ms = *timeout_ms;
    return true;
  }
  s_backlight_timeout_ms = DEFAULT_BACKLIGHT_TIMEOUT_MS;
  return false;
}

static uint16_t prv_convert_backlight_percent_to_intensity(uint32_t percent_intensity);

static bool prv_set_s_backlight_intensity(uint16_t *intensity) {
  if (*intensity > BACKLIGHT_BRIGHTNESS_OFF) {
    s_backlight_intensity = *intensity;
    return true;
  }
  s_backlight_intensity =
    prv_convert_backlight_percent_to_intensity(BOARD_CONFIG.backlight_on_percent);
  return false;
}

static bool prv_set_s_backlight_motion_enabled(bool *enabled) {
  s_backlight_motion_enabled = *enabled;
  return true;
}

static bool prv_set_s_stationary_mode_enabled(bool *enabled) {
  s_stationary_mode_enabled = *enabled;
  return true;
}

static bool prv_set_s_default_worker(Uuid *uuid) {
  s_default_worker = *uuid;
  return true;
}

static bool prv_set_s_text_style(uint8_t *style) {
  s_text_style = *style;
  return true;
}

static bool prv_set_s_language_english(bool *english) {
  s_language_english = *english;
  i18n_enable(!s_language_english);
  return true;
}

static bool prv_set_s_quick_launch_up(QuickLaunchPreference *pref) {
  s_quick_launch_up = *pref;
  return true;
}

static bool prv_set_s_quick_launch_down(QuickLaunchPreference *pref) {
  s_quick_launch_down = *pref;
  return true;
}

static bool prv_set_s_quick_launch_select(QuickLaunchPreference *pref) {
  s_quick_launch_select = *pref;
  return true;
}

static bool prv_set_s_quick_launch_back(QuickLaunchPreference *pref) {
  s_quick_launch_back = *pref;
  return true;
}

static bool prv_set_s_quick_launch_setup_opened(uint8_t *version) {
  s_quick_launch_setup_opened = *version;
  return true;
}

static bool prv_set_s_default_watchface(Uuid *uuid) {
  s_default_watchface = *uuid;
  return true;
}

static bool prv_set_s_welcome_version(uint8_t *version) {
  s_welcome_version = *version;
  return true;
}

#if CAPABILITY_HAS_HEALTH_TRACKING
static bool prv_set_s_activity_preferences(ActivitySettings *new_settings) {
  bool invalid_data = false;

  if (new_settings->height_mm <= 0) {
    new_settings->height_mm = ACTIVITY_DEFAULT_HEIGHT_MM;
    invalid_data = true;
  }

  if (new_settings->weight_dag <= 0) {
    new_settings->weight_dag = ACTIVITY_DEFAULT_WEIGHT_DAG;
    invalid_data = true;
  }

  if (!(new_settings->gender == ActivityGenderMale ||
       new_settings->gender == ActivityGenderFemale ||
       new_settings->gender == ActivityGenderOther)) {
    new_settings->gender = ACTIVITY_DEFAULT_GENDER;
    invalid_data = true;
  }

  if (new_settings->age_years <= 0) {
    new_settings->age_years = ACTIVITY_DEFAULT_AGE_YEARS;
    invalid_data = true;
  }

  if (new_settings->tracking_enabled) {
    activity_start_tracking(false);
  } else {
    activity_stop_tracking();
  }

  s_activity_preferences = *new_settings;

  // If we received invalid data, we return false, so that prefs_private_handle_blob_db_event
  // will rewrite s_activity_preferences to the backing file
  return !invalid_data;
}

static bool prv_set_s_activity_activation_timestamp(time_t *timestamp) {
  s_activity_activation_timestamp = *timestamp;
  return true;
}

static bool prv_set_s_activity_activation_delay_insight(uint32_t *insight_bitmask) {
  s_activity_activation_delay_insight = *insight_bitmask;
  return true;
}

static bool prv_set_s_activity_prefs_health_app_opened(uint8_t *version) {
  s_activity_prefs_health_app_opened = *version;
  return true;
}

static bool prv_set_s_activity_prefs_workout_app_opened(uint8_t *version) {
  s_activity_prefs_workout_app_opened = *version;
  return true;
}

static bool prv_set_s_alarms_app_opened(uint8_t *version) {
  s_alarms_app_opened = *version;
  return true;
}

static bool prv_set_s_activity_hr_preferences(HeartRatePreferences *new_settings) {
  if (new_settings->resting_hr > new_settings->elevated_hr ||
      new_settings->elevated_hr > new_settings->max_hr) {
    return false;
  }
  if (new_settings->zone1_threshold > new_settings->zone2_threshold ||
      new_settings->zone2_threshold > new_settings->zone3_threshold) {
    return false;
  }

  s_activity_hr_preferences = *new_settings;
  return true;
}

static bool prv_set_s_activity_hrm_preferences(ActivityHRMSettings *new_settings) {
  // Set the preference before calling `hrm_manager_enable` because it actually queries
  // for the setting
  s_activity_hrm_preferences = *new_settings;

#if CAPABILITY_HAS_BUILTIN_HRM
  hrm_manager_handle_prefs_changed();
#endif // CAPABILITY_HAS_BUILTIN_HRM
#if BLE_HRM_SERVICE
  ble_hrm_handle_activity_prefs_heart_rate_is_enabled(new_settings->enabled);
#endif // BLE_HRM_SERVICE
  return true;
}
#endif /* CAPABILITY_HAS_HEALTH_TRACKING */


#if PLATFORM_SPALDING
static bool prv_set_s_display_user_offset(GPoint *offset) {
  s_display_user_offset = *offset;
  return true;
}
static bool prv_set_s_should_prompt_display_calibration(bool should_prompt) {
  s_should_prompt_display_calibration = should_prompt;
  return true;
}
#endif

#if CAPABILITY_HAS_TIMELINE_PEEK
static uint8_t prv_set_s_timeline_settings_opened(uint8_t *version) {
  s_timeline_settings_opened = *version;
  return true;
}

static bool prv_set_s_timeline_peek_enabled(bool *enabled) {
  s_timeline_peek_enabled = *enabled;
  timeline_peek_set_enabled(*enabled);
  return true;
}

static bool prv_set_s_timeline_peek_before_time_m(uint16_t *before_time_m) {
  s_timeline_peek_before_time_m = *before_time_m;
  timeline_peek_set_show_before_time(*before_time_m * SECONDS_PER_MINUTE);
  return true;
}
#endif

// ------------------------------------------------------------------------------------
// Table of all prefs
typedef bool (*PrefSetHandler)(const void *value, size_t val_len);
typedef struct {
  const char *key;
  void *value;
  uint16_t value_len;
  PrefSetHandler handler;
} PrefsTableEntry;

// The PREFS_DECLARE_HANDLER creates a springboard function with a generic signature
// (of type PrefSetHandler) which simply calls into the specialized function prv_set_<var_name>
// after dereferencing the void* argument using the right type for that pref
#define PREFS_MACRO(name, var) \
  static bool prv_set_ ## var ## _cb(const void *value, size_t val_len) { \
    return prv_set_ ## var ((__typeof__(var) *)value); \
  }
#include "prefs_values.h.inc"
#undef PREFS_MACRO

// Create a time containing the key name and global variable name for each pref
#define PREFS_MACRO(key, var) {key, &var, sizeof(var), prv_set_ ## var ## _cb},
static const PrefsTableEntry s_prefs_table[] = {
#include "prefs_values.h.inc"
};
#undef PREFS_MACRO



// ------------------------------------------------------------------------------------
// FIXME PBL-22272. We back convert this value in
// settings_display.c:prv_get_scaled_brightness() We should really just store
// the percent intensity or a setting level name and leave it up to the light
// module to do the conversion
static uint16_t prv_convert_backlight_percent_to_intensity(uint32_t percent_intensity) {
  return (BACKLIGHT_BRIGHTNESS_MAX * (uint32_t)percent_intensity) / 100;
}


// ------------------------------------------------------------------------------------
static void prv_convert_deprecated_backlight_behaviour_key(SettingsFile *file) {
  // if present, convert deprecated BACKLIGHT_BEHAVIOUR key to the two new separate keys
  if (settings_file_exists(file, PREF_KEY_BACKLIGHT_BEHAVIOUR_DEPRECATED,
                           sizeof(PREF_KEY_BACKLIGHT_BEHAVIOUR_DEPRECATED))) {
    bool temp;
    BacklightBehaviour backlight_behaviour = BacklightBehaviour_Auto;
    settings_file_get(file, PREF_KEY_BACKLIGHT_BEHAVIOUR_DEPRECATED,
                      sizeof(PREF_KEY_BACKLIGHT_BEHAVIOUR_DEPRECATED),
                      &backlight_behaviour, sizeof(backlight_behaviour));
    temp = (backlight_behaviour != BacklightBehaviour_Off);
    settings_file_set(file, PREF_KEY_BACKLIGHT_ENABLED,
                      sizeof(PREF_KEY_BACKLIGHT_ENABLED), &temp, sizeof(temp));
    temp = (backlight_behaviour != BacklightBehaviour_On);
    settings_file_set(file, PREF_KEY_BACKLIGHT_AMBIENT_SENSOR_ENABLED,
                      sizeof(PREF_KEY_BACKLIGHT_AMBIENT_SENSOR_ENABLED), &temp, sizeof(temp));
    settings_file_delete(file, PREF_KEY_BACKLIGHT_BEHAVIOUR_DEPRECATED,
                         sizeof(PREF_KEY_BACKLIGHT_BEHAVIOUR_DEPRECATED));
  }
}


// ------------------------------------------------------------------------------------
void shell_prefs_init(void) {
  s_backlight_intensity =
      prv_convert_backlight_percent_to_intensity(BOARD_CONFIG.backlight_on_percent);
  s_mutex = mutex_create();

  SettingsFile file = {{0}};
  if (settings_file_open(&file, SHELL_PREFS_FILE_NAME, SHELL_PREFS_FILE_LEN) != S_SUCCESS) {
    return;
  }

  prv_convert_deprecated_backlight_behaviour_key(&file);

  // Init state for each pref from our backing store
  uint32_t num_entries = ARRAY_LENGTH(s_prefs_table);
  const PrefsTableEntry *entry = s_prefs_table;
  for (uint32_t i = 0; i < num_entries; i++, entry++) {
    // Keys in the backing store include the null terminator, so we add 1 to key_len
    size_t key_len = strlen(entry->key) + 1;
    if (settings_file_get_len(&file, entry->key, key_len) == entry->value_len) {
      settings_file_get(&file, entry->key, key_len, entry->value, entry->value_len);
    }
  }

  settings_file_close(&file);
}


// ------------------------------------------------------------------------------------
// Find the PrefsTableEntry for the given key
static const PrefsTableEntry *prv_prefs_entry(const uint8_t *key, size_t key_len) {
  uint32_t num_entries = ARRAY_LENGTH(s_prefs_table);
  const PrefsTableEntry *entry = s_prefs_table;
  for (uint32_t i = 0; i < num_entries; i++, entry++) {
    if (!strncmp((const char *)key, entry->key, key_len)) {
      return entry;
    }
  }
  PBL_LOG(LOG_LEVEL_WARNING, "Unrecognized key: %s", (const char *)key);
  return NULL;
}


// ------------------------------------------------------------------------------------
// Set the backing store for a pref
static bool prv_set_pref_backing(const PrefsTableEntry *entry, const void *value, int value_len) {
  if (value_len != entry->value_len) {
    PBL_LOG(LOG_LEVEL_WARNING, "Attempt to set %s using invalid value_len of %"PRIu32"",
            entry->key, (uint32_t)value_len);
    return false;
  }

  status_t rv = E_ERROR;
  mutex_lock(s_mutex);
  {
    SettingsFile file = {{0}};
    if (settings_file_open(&file, SHELL_PREFS_FILE_NAME, SHELL_PREFS_FILE_LEN) == S_SUCCESS) {
      // Keys in the backing store include the null terminator, so we add 1 to key_len
      rv = settings_file_set(&file, entry->key, strlen(entry->key) + 1, value, value_len);
      if (rv != S_SUCCESS) {
        PBL_LOG(LOG_LEVEL_WARNING, "Failed to set pref '%s' (%"PRIi32")", entry->key, (int32_t)rv);
      }
      settings_file_close(&file);
    }
  }
  mutex_unlock(s_mutex);
  return (rv == S_SUCCESS);
}


// ------------------------------------------------------------------------------------
// Convenience function used to update the state AND set the backing for a pref. This is
// used by the functions below that are called by the firmware to change prefs (i.e.
// shell_prefs_set.*, backlight_set.*, etc.).
static void prv_pref_set(const char* key, const void *value, size_t val_len) {
  // Find the entry for this key
  const PrefsTableEntry *entry = prv_prefs_entry((const uint8_t *)key, strlen(key));

  // validate the key and value length
  PBL_ASSERT(entry != NULL, "Key %s not found", key);
  PBL_ASSERT(val_len == entry->value_len, "Attempt to set %s using invalid value_len of %"PRIu32"",
             entry->key, (uint32_t)val_len);

  // Call the update handler
  bool success = entry->handler(value, val_len);
  PBL_ASSERT(success, "Failure to store new value for %s in settings file", key);

  // Update the backing store
  if (success) {
    prv_set_pref_backing(entry, value, val_len);
  }
}


// ------------------------------------------------------------------------------------
// Exported function used by blob_db API to set the backing store for a specific key
bool prefs_private_write_backing(const uint8_t *key, size_t key_len, const void *value,
                               int value_len) {
  const PrefsTableEntry *entry = prv_prefs_entry(key, key_len);
  if (!entry) {
    return false;
  }

  return prv_set_pref_backing(entry, value, value_len);
}


// ------------------------------------------------------------------------------------
// Exported function used by blob_db API to get the length of a value in our backing store
int prefs_private_get_backing_len(const uint8_t *key, size_t key_len) {
  const PrefsTableEntry *entry = prv_prefs_entry(key, key_len);
  if (!entry) {
    return 0;
  }
  return entry->value_len;
}


// ------------------------------------------------------------------------------------
// Exported function used by blob_db API to read our backing store
bool prefs_private_read_backing(const uint8_t *key, size_t key_len, void *value, int value_len) {
  const PrefsTableEntry *entry = prv_prefs_entry(key, key_len);
  if (!entry) {
    return false;
  }

  if (value_len != entry->value_len) {
    PBL_LOG(LOG_LEVEL_WARNING, "Attempt to read %s using invalid value_len of %"PRIu32"",
            entry->key, (uint32_t)value_len);
    return false;
  }

  bool success = false;
  mutex_lock(s_mutex);
  {
    SettingsFile file = {{0}};
    if (settings_file_open(&file, SHELL_PREFS_FILE_NAME, SHELL_PREFS_FILE_LEN) == S_SUCCESS) {
      // Keys in the backing store include the null terminator, so we add 1 to key_len
      success = (settings_file_get(&file, entry->key, key_len + 1, value, value_len)
                                   == S_SUCCESS);
      settings_file_close(&file);
    }
  }
  mutex_unlock(s_mutex);
  return success;
}


// ------------------------------------------------------------------------------------
// Called from KernelMain when we get a blob DB event. We take this opportunity to update the state
// of the given pref
void prefs_private_handle_blob_db_event(PebbleBlobDBEvent *event) {
  if (event->type != BlobDBEventTypeInsert) {
    return;
  }

  const PrefsTableEntry *entry = prv_prefs_entry(event->key, event->key_len);
  if (!entry) {
    return;
  }

  // Read in the updated value from the backing store
  bool success = prefs_private_read_backing(event->key, event->key_len, entry->value,
                                            entry->value_len);
  if (success) {
    // Call the state update handler in case this pref needs to take other action besides
    // just updating the global
    if (!entry->handler(entry->value, entry->value_len)) {
      // If the handler returns false, that means it reset the global back to the default,
      // so we should write the new value to the backing
      prefs_private_write_backing(event->key, event->key_len, entry->value, entry->value_len);
    }
  }
}

// ========================================================================================
// Exported functions used by the firmware to read/change a preference.
// IMPORTANT: When implementing the *set* call, be sure to call prv_pref_set(). This does
// two things:
//   1.) It validates that the stored global matches the type of the passed in argument
//   2.) It insures that the flow will also work correctly for setting a pref from the
//        mobile side using a blob_db insert operation.
bool shell_prefs_get_clock_24h_style(void) {
  return s_clock_24h;
}

UnitsDistance shell_prefs_get_units_distance(void) {
  return s_units_distance;
}

void shell_prefs_set_units_distance(UnitsDistance new_unit) {
  uint8_t uint_new_unit = new_unit;
  prv_pref_set(PREF_KEY_UNITS_DISTANCE, &uint_new_unit, sizeof(uint_new_unit));
}

void shell_prefs_set_clock_24h_style(bool is24h) {
  prv_pref_set(PREF_KEY_CLOCK_24H, &is24h, sizeof(is24h));
}

bool shell_prefs_is_timezone_source_manual(void) {
  return s_clock_timezone_source_is_manual;
}

void shell_prefs_set_timezone_source_manual(bool manual) {
  prv_pref_set(PREF_KEY_CLOCK_TIMEZONE_SOURCE_IS_MANUAL, &manual, sizeof(manual));
}

void shell_prefs_set_automatic_timezone_id(int16_t timezone_id) {
  prv_pref_set(PREF_KEY_CLOCK_PHONE_TIMEZONE_ID, &timezone_id, sizeof(timezone_id));
}

int16_t shell_prefs_get_automatic_timezone_id(void) {
  return s_clock_phone_timezone_id;
}

// Emulate the old BacklightBehaviour type for analytics.
// This is a deprecated method and should not be called by new code.
BacklightBehaviour backlight_get_behaviour(void) {
  if (s_backlight_enabled) {
    if (s_backlight_ambient_sensor_enabled) {
      return BacklightBehaviour_Auto;
    } else {
      return BacklightBehaviour_On;
    }
  } else {
    return BacklightBehaviour_Off;
  }
}

bool backlight_is_enabled(void) {
  return s_backlight_enabled;
}

void backlight_set_enabled(bool enabled) {
  prv_pref_set(PREF_KEY_BACKLIGHT_ENABLED, &enabled, sizeof(enabled));
}

bool backlight_is_ambient_sensor_enabled(void) {
#if INFINITE_BACKLIGHT
  return false;
#endif
  return s_backlight_ambient_sensor_enabled;
}

void backlight_set_ambient_sensor_enabled(bool enabled) {
  prv_pref_set(PREF_KEY_BACKLIGHT_AMBIENT_SENSOR_ENABLED, &enabled, sizeof(enabled));
}

uint32_t backlight_get_timeout_ms(void) {
#if INFINITE_BACKLIGHT
  return UINT32_MAX;
#endif
  return s_backlight_timeout_ms;
}

void backlight_set_timeout_ms(uint32_t timeout_ms) {
  prv_pref_set(PREF_KEY_BACKLIGHT_TIMEOUT_MS, &timeout_ms, sizeof(timeout_ms));
}

uint16_t backlight_get_intensity(void) {
  return s_backlight_intensity;
}

uint8_t backlight_get_intensity_percent(void) {
  return (backlight_get_intensity() * 100) / BACKLIGHT_BRIGHTNESS_MAX;
}

void backlight_set_intensity_percent(uint8_t percent_intensity) {
  PBL_ASSERTN(percent_intensity > 0 && percent_intensity <= 100);
  uint16_t intensity = prv_convert_backlight_percent_to_intensity(percent_intensity);
  PBL_ASSERTN(intensity > BACKLIGHT_BRIGHTNESS_OFF);
  prv_pref_set(PREF_KEY_BACKLIGHT_INTENSITY, &intensity, sizeof(intensity));
}

bool backlight_is_motion_enabled(void) {
  return s_backlight_motion_enabled;
}

void backlight_set_motion_enabled(bool enable) {
  prv_pref_set(PREF_KEY_BACKLIGHT_MOTION, &enable, sizeof(enable));
}

bool shell_prefs_get_stationary_enabled(void) {
  return s_stationary_mode_enabled;
}

void shell_prefs_set_stationary_enabled(bool enabled) {
  prv_pref_set(PREF_KEY_STATIONARY, &enabled, sizeof(enabled));
}

AppInstallId worker_preferences_get_default_worker(void) {
  return app_install_get_id_for_uuid(&s_default_worker);
}

void worker_preferences_set_default_worker(AppInstallId app_id) {
  Uuid uuid;
  app_install_get_uuid_for_install_id(app_id, &uuid);
  prv_pref_set(PREF_KEY_DEFAULT_WORKER, &uuid, sizeof(uuid));
}

bool quick_launch_is_enabled(ButtonId button) {
  switch (button) {
    case BUTTON_ID_UP:
      return s_quick_launch_up.enabled;
    case BUTTON_ID_DOWN:
      return s_quick_launch_down.enabled;
    case BUTTON_ID_SELECT:
      return s_quick_launch_select.enabled;
    case BUTTON_ID_BACK:
      return s_quick_launch_back.enabled;
    case NUM_BUTTONS:
      break;
  }
  return false;
}

AppInstallId quick_launch_get_app(ButtonId button) {
  Uuid *uuid = NULL;
  switch (button) {
    case BUTTON_ID_UP:
      uuid = &s_quick_launch_up.uuid;
      break;
    case BUTTON_ID_DOWN:
      uuid = &s_quick_launch_down.uuid;
      break;
    case BUTTON_ID_SELECT:
      uuid = &s_quick_launch_select.uuid;
      break;
    case BUTTON_ID_BACK:
      uuid = &s_quick_launch_back.uuid;
      break;
    case NUM_BUTTONS:
      break;
  }
  PBL_ASSERTN(uuid);
  return app_install_get_id_for_uuid(uuid);
}

void quick_launch_set_app(ButtonId button, AppInstallId app_id) {
  QuickLaunchPreference pref = (QuickLaunchPreference) {
    .enabled = true,
  };
  app_install_get_uuid_for_install_id(app_id, &pref.uuid);

  const char *key = NULL;
  switch (button) {
    case BUTTON_ID_UP:
      key = PREF_KEY_QUICK_LAUNCH_UP;
      break;
    case BUTTON_ID_DOWN:
      key = PREF_KEY_QUICK_LAUNCH_DOWN;
      break;
    case BUTTON_ID_SELECT:
      key = PREF_KEY_QUICK_LAUNCH_SELECT;
      break;
    case BUTTON_ID_BACK:
      key = PREF_KEY_QUICK_LAUNCH_BACK;
      break;
    case NUM_BUTTONS:
      break;
  }
  PBL_ASSERTN(key);
  prv_pref_set(key, &pref, sizeof(pref));
}

void quick_launch_set_enabled(ButtonId button, bool enabled) {
  QuickLaunchPreference pref;

  const char *key = NULL;
  switch (button) {
    case BUTTON_ID_UP:
      pref = s_quick_launch_up;
      key = PREF_KEY_QUICK_LAUNCH_UP;
      break;
    case BUTTON_ID_DOWN:
      pref = s_quick_launch_down;
      key = PREF_KEY_QUICK_LAUNCH_DOWN;
      break;
    case BUTTON_ID_SELECT:
      pref = s_quick_launch_select;
      key = PREF_KEY_QUICK_LAUNCH_SELECT;
      break;
    case BUTTON_ID_BACK:
      pref = s_quick_launch_back;
      key = PREF_KEY_QUICK_LAUNCH_BACK;
      break;
    case NUM_BUTTONS:
      break;
  }
  PBL_ASSERTN(key);
  pref.enabled = enabled;
  prv_pref_set(key, &pref, sizeof(pref));
}

void quick_launch_set_quick_launch_setup_opened(uint8_t version) {
  if (s_quick_launch_setup_opened != version) {
    s_quick_launch_setup_opened = version;
    prv_pref_set(PREF_KEY_QUICK_LAUNCH_SETUP_OPENED, &s_quick_launch_setup_opened,
                 sizeof(s_quick_launch_setup_opened));
  }
}

uint8_t quick_launch_get_quick_launch_setup_opened(void) {
  return s_quick_launch_setup_opened;
}

void watchface_set_default_install_id(AppInstallId app_id) {
  Uuid uuid;
  app_install_get_uuid_for_install_id(app_id, &uuid);
  prv_pref_set(PREF_KEY_DEFAULT_WATCHFACE, &uuid, sizeof(uuid));
}

void welcome_set_welcome_version(uint8_t version) {
  if (s_welcome_version != version) {
    s_welcome_version = version;
    prv_pref_set(PREF_KEY_WELCOME_VERSION, &version, sizeof(version));
  }
}

uint8_t welcome_get_welcome_version(void) {
  return s_welcome_version;
}

static bool prv_set_default_any_watchface_enumerate_callback(AppInstallEntry *entry, void *data) {
  if (!app_install_entry_is_watchface(entry)
      || app_install_entry_is_hidden(entry)) {
    return true; // continue search
  }

  watchface_set_default_install_id(entry->install_id);
  return false;
}

AppInstallId watchface_get_default_install_id(void) {
  AppInstallId app_id = app_install_get_id_for_uuid(&s_default_watchface);
  AppInstallEntry entry;
  if (app_id == INSTALL_ID_INVALID ||
      !app_install_get_entry_for_install_id(app_id, &entry) ||
      !app_install_entry_is_watchface(&entry)) {
    app_install_enumerate_entries(
        prv_set_default_any_watchface_enumerate_callback, NULL);
    app_id = app_install_get_id_for_uuid(&s_default_watchface);
  }
  return app_id;
}

void system_theme_set_content_size(PreferredContentSize content_size) {
  if (content_size >= NumPreferredContentSizes) {
    PBL_LOG(LOG_LEVEL_WARNING, "Ignoring attempt to set content size to invalid size %d",
            content_size);
    return;
  }
  const uint8_t content_size_uint = content_size;
  prv_pref_set(PREF_KEY_TEXT_STYLE, &content_size_uint, sizeof(content_size_uint));
}

PreferredContentSize system_theme_get_content_size(void) {
  return system_theme_convert_host_content_size_to_runtime_platform(
      (PreferredContentSize)s_text_style);
}

bool shell_prefs_get_language_english(void) {
  return s_language_english;
}

void shell_prefs_set_language_english(bool english) {
  prv_pref_set(PREF_KEY_LANG_ENGLISH, &english, sizeof(english));
}

void shell_prefs_toggle_language_english(void) {
  shell_prefs_set_language_english(!shell_prefs_get_language_english());
}

#if CAPABILITY_HAS_HEALTH_TRACKING
static void prv_activity_pref_set(void) {
  prv_pref_set(PREF_KEY_ACTIVITY_PREFERENCES, &s_activity_preferences,
               sizeof(s_activity_preferences));
}

time_t activity_prefs_get_activation_time(void) {
  return s_activity_activation_timestamp;
}

void activity_prefs_set_activated(void) {
  if (s_activity_activation_timestamp == 0) {
    s_activity_activation_timestamp = rtc_get_time();
    prv_pref_set(PREF_KEY_ACTIVITY_ACTIVATED_TIMESTAMP, &s_activity_activation_timestamp,
                 sizeof(s_activity_activation_timestamp));
  }
}

bool activity_prefs_has_activation_delay_insight_fired(ActivationDelayInsightType type) {
  return (s_activity_activation_delay_insight & (1 << type));
}

void activity_prefs_set_activation_delay_insight_fired(ActivationDelayInsightType type) {
  s_activity_activation_delay_insight |= (1 << type);
  prv_pref_set(PREF_KEY_ACTIVITY_ACTIVATION_DELAY_INSIGHT, &s_activity_activation_delay_insight,
               sizeof(s_activity_activation_delay_insight));
}

uint8_t activity_prefs_get_health_app_opened_version(void) {
  return s_activity_prefs_health_app_opened;
}

void activity_prefs_set_health_app_opened_version(uint8_t version) {
  if (s_activity_prefs_health_app_opened != version) {
    s_activity_prefs_health_app_opened = version;
    prv_pref_set(PREF_KEY_ACTIVITY_HEALTH_APP_OPENED, &s_activity_prefs_health_app_opened,
                 sizeof(s_activity_prefs_health_app_opened));
  }
}

uint8_t activity_prefs_get_workout_app_opened_version(void) {
  return s_activity_prefs_workout_app_opened;
}

void activity_prefs_set_workout_app_opened_version(uint8_t version) {
  if (s_activity_prefs_workout_app_opened != version) {
    s_activity_prefs_workout_app_opened = version;
    prv_pref_set(PREF_KEY_ACTIVITY_WORKOUT_APP_OPENED, &s_activity_prefs_workout_app_opened,
                 sizeof(s_activity_prefs_workout_app_opened));
  }
}

bool activity_prefs_activity_insights_are_enabled(void) {
  return s_activity_preferences.activity_insights_enabled;
}

void activity_prefs_activity_insights_set_enabled(bool enable) {
  s_activity_preferences.activity_insights_enabled = enable;
  prv_activity_pref_set();
}

bool activity_prefs_sleep_insights_are_enabled(void) {
  return s_activity_preferences.sleep_insights_enabled;
}

void activity_prefs_sleep_insights_set_enabled(bool enable) {
  s_activity_preferences.sleep_insights_enabled = enable;
  prv_activity_pref_set();
}

bool activity_prefs_tracking_is_enabled(void) {
  return s_activity_preferences.tracking_enabled;
}

void activity_prefs_tracking_set_enabled(bool enable) {
  s_activity_preferences.tracking_enabled = enable;
  prv_activity_pref_set();
}

void activity_prefs_set_height_mm(uint16_t height_mm) {
  s_activity_preferences.height_mm = height_mm;
  prv_activity_pref_set();
}

uint16_t activity_prefs_get_height_mm(void) {
  return s_activity_preferences.height_mm;
}

void activity_prefs_set_weight_dag(uint16_t weight_dag) {
  s_activity_preferences.weight_dag = weight_dag;
  prv_activity_pref_set();
}

uint16_t activity_prefs_get_weight_dag(void) {
  return s_activity_preferences.weight_dag;
}

void activity_prefs_set_gender(ActivityGender gender) {
  s_activity_preferences.gender = gender;
  prv_activity_pref_set();
}

ActivityGender activity_prefs_get_gender(void) {
  return s_activity_preferences.gender;
}

void activity_prefs_set_age_years(uint8_t age_years) {
  s_activity_preferences.age_years = age_years;
  prv_activity_pref_set();
}

uint8_t activity_prefs_get_age_years(void) {
  return s_activity_preferences.age_years;
}

uint8_t activity_prefs_heart_get_resting_hr(void) {
  return s_activity_hr_preferences.resting_hr;
}

uint8_t activity_prefs_heart_get_elevated_hr(void) {
  return s_activity_hr_preferences.elevated_hr;
}

uint8_t activity_prefs_heart_get_max_hr(void) {
  return s_activity_hr_preferences.max_hr;
}

uint8_t activity_prefs_heart_get_zone1_threshold(void) {
  return s_activity_hr_preferences.zone1_threshold;
}

uint8_t activity_prefs_heart_get_zone2_threshold(void) {
  return s_activity_hr_preferences.zone2_threshold;
}

uint8_t activity_prefs_heart_get_zone3_threshold(void) {
  return s_activity_hr_preferences.zone3_threshold;
}

bool activity_prefs_heart_rate_is_enabled(void) {
  return s_activity_hrm_preferences.enabled;
}

void alarm_prefs_set_alarms_app_opened(uint8_t version) {
  if (s_alarms_app_opened != version) {
    s_alarms_app_opened = version;
    prv_pref_set(PREF_KEY_ALARMS_APP_OPENED, &s_alarms_app_opened, sizeof(s_alarms_app_opened));
  }
}

uint8_t alarm_prefs_get_alarms_app_opened(void) {
  return s_alarms_app_opened;
}
#endif /* CAPABILITY_HAS_HEALTH_TRACKING */

#if PLATFORM_SPALDING
void shell_prefs_set_display_offset(GPoint offset) {
  const GPoint user_offset = gpoint_sub(offset, mfg_info_get_disp_offsets());
  prv_pref_set(PREF_KEY_DISPLAY_USER_OFFSET, &user_offset, sizeof(user_offset));
}

GPoint shell_prefs_get_display_offset(void) {
  return gpoint_add(s_display_user_offset, mfg_info_get_disp_offsets());
}

void shell_prefs_display_offset_init(void) {
  display_set_offset(shell_prefs_get_display_offset());
}

bool shell_prefs_should_prompt_display_calibration(void) {
  return s_should_prompt_display_calibration;
}

void shell_prefs_set_should_prompt_display_calibration(bool should_prompt) {
  s_should_prompt_display_calibration = should_prompt;
  prv_pref_set(PREF_KEY_SHOULD_PROMPT_DISPLAY_CALIBRATION, &s_should_prompt_display_calibration,
               sizeof(s_should_prompt_display_calibration));
}
#endif

#if CAPABILITY_HAS_TIMELINE_PEEK
void timeline_prefs_set_settings_opened(uint8_t version) {
  prv_pref_set(PREF_KEY_TIMELINE_SETTINGS_OPENED, &version, sizeof(version));
}

uint8_t timeline_prefs_get_settings_opened(void) {
  return s_timeline_settings_opened;
}

void timeline_peek_prefs_set_enabled(bool enabled) {
  prv_pref_set(PREF_KEY_TIMELINE_PEEK_ENABLED, &enabled, sizeof(enabled));
}

bool timeline_peek_prefs_get_enabled(void) {
  return s_timeline_peek_enabled;
}

void timeline_peek_prefs_set_before_time(uint16_t before_time_m) {
  prv_pref_set(PREF_KEY_TIMELINE_PEEK_BEFORE_TIME_M, &before_time_m, sizeof(before_time_m));
}

uint16_t timeline_peek_prefs_get_before_time(void) {
  return s_timeline_peek_before_time_m;
}
#else
uint16_t timeline_peek_prefs_get_before_time(void) {
  return TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
}
#endif
