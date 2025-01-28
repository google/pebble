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

#include "stationary.h"

#include "applib/accel_service_private.h"
#include "applib/battery_state_service.h"
#include "applib/ui/dialogs/dialog_private.h"
#include "applib/ui/dialogs/simple_dialog.h"
#include "comm/bt_lock.h"
#include "drivers/battery.h"
#include "kernel/event_loop.h"
#include "kernel/ui/modals/modal_manager.h"
#include "resource/resource_ids.auto.h"
#include "services/common/accel_manager.h"
#include "services/common/analytics/analytics.h"
#include "services/common/analytics/analytics_event.h"
#include "services/common/i18n/i18n.h"
#include "services/common/regular_timer.h"
#include "services/common/system_task.h"
#include "services/runlevel.h"
#include "shell/prefs.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"

#include <stdlib.h>

#define STATIONARY_PEEKING_TIME_MINS 5
#define STATIONARY_WAIT_BEFORE_ENGAGING_TIME_MINS 30
#define STATIONARY_ENABLED_DIALOG_TIMEOUT_MS 1800000
#define STATIONARY_WELCOME_BACK_DIALOG_TIMEOUT_MS 2000
#define ACCEL_MAX_IDLE_DELTA 100

#define DEBUG_STATIONARY 0

//! Used for describing the stationary event reason in analytics
typedef enum {
  StationaryAnalyticsEnterNormally,
  StationaryAnalyticsEnterFromPeek,
  StationaryAnalyticsExitNormally,
  StationaryAnalyticsExitToPeek,
  StationaryAnalyticsEnterCharging,
  StationaryAnalyticsExitCharging,
  StationaryAnalyticsEnableStationaryMode,
  StationaryAnalyticsDisableStationaryMode
} StationaryAnalytics;

//! The possible states that the watch can be in regarding stationary mode
typedef enum {
  StationaryStateAwake,
  StationaryStateStationary,
  StationaryStatePeeking,
  StationaryStateDisabled,
} StationaryState;

//! The actions we take upon state transitions
typedef enum {
  StationaryActionGoToSleep,
  StationaryActionWakeUp,
  StationaryActionEnableStationary,
  StationaryActionDisableStationary
} StationaryAction;

static AccelData s_last_accel_data;
static AccelServiceState *s_accel_session;
static EventServiceInfo s_button_event_info;
static RegularTimerInfo s_accel_stationary_timer_info;
static StationaryState s_current_state = StationaryStateDisabled;
static uint8_t s_stationary_count_down = STATIONARY_WAIT_BEFORE_ENGAGING_TIME_MINS;
static bool s_stationary_mode_inhibit = true;

static void prv_handle_action(StationaryAction action);

// Compute and return the device's delta position to help determine movement as idle.
static uint32_t prv_compute_delta_pos(AccelData *cur_pos, AccelData *last_pos) {
  return (abs(last_pos->x - cur_pos->x) + abs(last_pos->y - cur_pos->y) +
          abs(last_pos->z - cur_pos->z));
}

//! The orientation of the accelerometer is checked every minute. If the orientation
//! has not changed by a significant amount, we consider it as stationary.
static bool prv_update_and_check_accel_is_stationary(void) {
  AccelData last_accel_data = s_last_accel_data;
  sys_accel_manager_peek(&s_last_accel_data);
  return prv_compute_delta_pos(&last_accel_data, &s_last_accel_data) < ACCEL_MAX_IDLE_DELTA;
}

static bool prv_is_allowed_to_run(void) {
  return (stationary_get_enabled() && !s_stationary_mode_inhibit);
}

static void prv_update_stationary_enabled(void *data) {
  if (!battery_is_usb_connected() && prv_is_allowed_to_run()) {
    prv_handle_action(StationaryActionEnableStationary);
  } else {
    prv_handle_action(StationaryActionDisableStationary);
  }
}

void stationary_handle_battery_connection_change_event(void) {
  PBL_LOG_D(DEBUG_STATIONARY, LOG_LEVEL_DEBUG,
            "Stationary mode battery state change event received");
  if (battery_is_usb_connected()) {
    analytics_event_stationary_state_change(rtc_get_time(), StationaryAnalyticsEnterCharging);
  } else {
    analytics_event_stationary_state_change(rtc_get_time(), StationaryAnalyticsExitCharging);
  }
  prv_update_stationary_enabled(NULL);
}

//! A movement of the watch will make the watch wake up. In this state, the watch
//! will have a low tap threshold, so it will be sensitive to motion
static void prv_accel_tap_handler(AccelAxisType axis, int32_t direction) {
  prv_handle_action(StationaryActionWakeUp);
}

static void prv_button_down_handler(PebbleEvent *event, void *data) {
  prv_handle_action(StationaryActionWakeUp);
}

//! If the watch is determined to be motionless for 30 minutes, it will go to sleep
static void prv_watch_is_motionless(void) {
  // Check if we should enable stationary mode and disabled unneeded features
  if (s_stationary_count_down > 0) {
    PBL_LOG_D(DEBUG_STATIONARY, LOG_LEVEL_DEBUG,
              "Countdown to stationary: %d", s_stationary_count_down);
    s_stationary_count_down--;
  } else {
    analytics_inc(ANALYTICS_DEVICE_METRIC_STATIONARY_TIME_MINUTES, AnalyticsClient_System);
    prv_handle_action(StationaryActionGoToSleep);
  }
}

//! The orientation of the accelerometer is checked every minute. If the orientation has
//! changed by a significant amount, we consider the watch as in motion, and restart the
//! stationary counter
static void prv_watch_is_in_motion(void) {
  prv_handle_action(StationaryActionWakeUp);
}

static void prv_stationary_check_launcher_task_cb(void *unused_data) {
  if (prv_update_and_check_accel_is_stationary()) {
    prv_watch_is_motionless();
  } else {
    prv_watch_is_in_motion();
  }
}

//! Called every minute to determine whether any motion has occurred since the last time
//! the call was made. The current position is updated at this time
static void prv_stationary_check_timer_cb(void *unused_data) {
  //! All stationary events need to be handled by kernel main
  launcher_task_add_callback(prv_stationary_check_launcher_task_cb, NULL);
}

bool stationary_get_enabled(void) {
  return shell_prefs_get_stationary_enabled();
}

void stationary_set_enabled(bool enabled) {
  if (enabled == stationary_get_enabled()) {
    return;
  }
  shell_prefs_set_stationary_enabled(enabled);

  if (enabled) {
    analytics_event_stationary_state_change(rtc_get_time(),
                                            StationaryAnalyticsEnableStationaryMode);
  } else {
    analytics_event_stationary_state_change(rtc_get_time(),
                                            StationaryAnalyticsDisableStationaryMode);
  }

  launcher_task_add_callback(prv_update_stationary_enabled, NULL);
}

void stationary_run_level_enable(bool enable) {
#if !STATIONARY_MODE
  return;
#endif

  const bool inhibit = !enable;
  if (inhibit == s_stationary_mode_inhibit) {
    return;
  }
  s_stationary_mode_inhibit = inhibit;
  launcher_task_add_callback(prv_update_stationary_enabled, NULL);
}

void stationary_wake_up(void) {
  if (!prv_is_allowed_to_run()) {
    return;
  }
  prv_handle_action(StationaryActionWakeUp);
}

static void prv_reset_stationary_counter(void) {
  s_stationary_count_down = STATIONARY_WAIT_BEFORE_ENGAGING_TIME_MINS;
}

static void prv_enter_awake_state(void) {
  PBL_LOG(LOG_LEVEL_INFO, "Exiting stationary: Setting run level to normal");
  analytics_event_stationary_state_change(rtc_get_time(), StationaryAnalyticsExitNormally);
  prv_reset_stationary_counter();
  s_current_state = StationaryStateAwake;
}

//! The accelerometer tap threshold will be set very low, so a small motion will wake
//! the watch back up
static void prv_enter_stationary_state(void) {
  PBL_LOG(LOG_LEVEL_INFO, "Entering stationary: Changing run level");
  if (s_current_state == StationaryStatePeeking) {
    analytics_event_stationary_state_change(rtc_get_time(), StationaryAnalyticsEnterFromPeek);
  } else if (s_current_state == StationaryStateAwake) {
    analytics_event_stationary_state_change(rtc_get_time(), StationaryAnalyticsEnterNormally);
  }
  services_set_runlevel(RunLevel_Stationary);
  accel_enable_high_sensitivity(true);
  s_current_state = StationaryStateStationary;
}

static void prv_exit_stationary(void) {
  accel_enable_high_sensitivity(false);
  services_set_runlevel(RunLevel_Normal);
}

static void prv_enter_peek_state(void) {
  analytics_event_stationary_state_change(rtc_get_time(), StationaryAnalyticsExitToPeek);
  //! When exiting out of stationary, we aren't certain that this wasn't caused by noise yet
  //! we set the counter to a small value in case there is no motion right after
  s_stationary_count_down = STATIONARY_PEEKING_TIME_MINS;
  prv_exit_stationary();
  s_current_state = StationaryStatePeeking;
}

static void prv_enter_disabled_state(void) {
  PBL_ASSERTN(s_accel_session);
  accel_session_shake_unsubscribe(s_accel_session);
  accel_session_delete(s_accel_session);
  s_accel_session = NULL;

  event_service_client_unsubscribe(&s_button_event_info);
  regular_timer_remove_callback(&s_accel_stationary_timer_info);
  s_current_state = StationaryStateDisabled;
}

static void prv_exit_disabled_state(void) {
  s_stationary_count_down = STATIONARY_WAIT_BEFORE_ENGAGING_TIME_MINS;
  PBL_ASSERTN(s_accel_session == NULL);
  s_accel_session = accel_session_create();
  accel_session_shake_subscribe(s_accel_session, prv_accel_tap_handler);
  event_service_client_subscribe(&s_button_event_info);

#if DEBUG_STATIONARY
  regular_timer_add_seconds_callback(&s_accel_stationary_timer_info);
#else
  regular_timer_add_minutes_callback(&s_accel_stationary_timer_info);
#endif
  s_current_state = StationaryStateAwake;
}

static void prv_handle_awake_action(StationaryAction action) {
  switch (action) {
    case StationaryActionGoToSleep:
      prv_enter_stationary_state();
      break;
    case StationaryActionWakeUp:
      prv_reset_stationary_counter();
      break;
    case StationaryActionEnableStationary:
      break;
    case StationaryActionDisableStationary:
      prv_enter_disabled_state();
      break;
    default:
      WTF;
  }
}

static void prv_handle_stationary_action(StationaryAction action) {
  switch (action) {
    case StationaryActionGoToSleep:
      break;
    case StationaryActionWakeUp:
      prv_enter_peek_state();
      break;
    case StationaryActionEnableStationary:
      break;
    case StationaryActionDisableStationary:
      prv_exit_stationary();
      prv_enter_disabled_state();
      break;
    default:
      WTF;
  }
}

static void prv_handle_peeking_action(StationaryAction action) {
  switch (action) {
    case StationaryActionGoToSleep:
      prv_enter_stationary_state();
      break;
    case StationaryActionWakeUp:
      prv_enter_awake_state();
      break;
    case StationaryActionEnableStationary:
      break;
    case StationaryActionDisableStationary:
      prv_enter_disabled_state();
      break;
    default:
      WTF;
  }
}

static void prv_handle_disabled_action(StationaryAction action) {
  switch (action) {
    case StationaryActionGoToSleep:
      break;
    case StationaryActionWakeUp:
      // No-op here. Awake gives us the same runlevel as Disabled, so no harm in just staying in
      // the disabled state. Could potentially be caused in races where we tap or press a button
      // to wake us up from stationary while we're being disabled.
      break;
    case StationaryActionEnableStationary:
      prv_exit_disabled_state();
      break;
    case StationaryActionDisableStationary:
      break;
    default:
      WTF;
  }
}

typedef void (*StationaryActionHandler)(StationaryAction action);

static void prv_handle_action(StationaryAction action) {
  // we need to be on kernel main so that we subscribe to event services
  // for kernel main
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  PBL_LOG_D(DEBUG_STATIONARY, LOG_LEVEL_DEBUG, "Stationary: state %d action %d",
            s_current_state, action);

  static StationaryActionHandler const prv_action_jump_table[] = {
    [StationaryStateAwake] = prv_handle_awake_action,
    [StationaryStateStationary] = prv_handle_stationary_action,
    [StationaryStatePeeking] = prv_handle_peeking_action,
    [StationaryStateDisabled] = prv_handle_disabled_action
  };
  PBL_ASSERTN(s_current_state < ARRAY_LENGTH(prv_action_jump_table));
  prv_action_jump_table[s_current_state](action);
}

static void prv_setup_callback_info(void) {
  //! Timer callback to check whether the watch is stationary every minute
  s_accel_stationary_timer_info = (RegularTimerInfo) {
    .cb = prv_stationary_check_timer_cb
  };

  //! Button press events
  s_button_event_info = (EventServiceInfo) {
    .type = PEBBLE_BUTTON_DOWN_EVENT,
    .handler = prv_button_down_handler,
  };
}

void stationary_init(void) {
  prv_setup_callback_info();
}
