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

#include "health.h"
#include "health_card_view.h"
#include "health_data.h"

#include "applib/app.h"
#include "applib/ui/dialogs/expandable_dialog.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/modals/modal_manager.h"
#include "popups/health_tracking_ui.h"
#include "process_state/app_state/app_state.h"
#include "services/normal/activity/activity.h"
#include "services/normal/activity/activity_private.h"
#include "services/normal/timeline/timeline.h"
#include "system/logging.h"

// Health app versions
// 0: Invalid (app was never opened)
// 1: Initial version
// 2: Graphs moved to mobile apps
// 3: 4.0 app redesign
#define CURRENT_HEALTH_APP_VERSION 3

////////////////////////////////////////////////////////////////////////////////////////////////////
// Main Structures
//

//! Main structure for application
typedef struct HealthAppData {
  HealthCardView *health_card_view;
  HealthData *health_data;
} HealthAppData;


////////////////////////////////////////////////////////////////////////////////////////////////////
// Callbacks
//

//! Tick timer service callback
//! @param tick_time Pointer to time structure
//! @param units_changed The time units changed
static void prv_tick_timer_handler(struct tm *tick_time, TimeUnits units_changed) {
  HealthAppData *health_app_data = app_state_get_user_data();
  health_data_update_step_derived_metrics(health_app_data->health_data);
  health_card_view_mark_dirty(health_app_data->health_card_view);
}

// Activity change callback
static void prv_health_service_event_handler(HealthEventType event, void *context) {
  HealthAppData *health_app_data = context;
  if (event == HealthEventMovementUpdate) {
    const uint32_t steps_today = health_service_sum_today(HealthMetricStepCount);
    health_data_update_steps(health_app_data->health_data, steps_today);
  } else if (event == HealthEventSleepUpdate) {
    const uint32_t seconds_sleep_today = health_service_sum_today(HealthMetricSleepSeconds);
    const uint32_t seconds_restful_sleep_today =
      health_service_sum_today(HealthMetricSleepRestfulSeconds);
    health_data_update_sleep(health_app_data->health_data, seconds_sleep_today,
                             seconds_restful_sleep_today);
  } else if (event == HealthEventHeartRateUpdate) {
    health_data_update_current_bpm(health_app_data->health_data);
  } else {
    health_data_update(health_app_data->health_data);
  }
  health_card_view_mark_dirty(health_app_data->health_card_view);
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Initialization and Termination
//

//! Initialize application
static void prv_finish_initialization_cb(bool in_focus) {
  if (in_focus) {
    HealthAppData *health_app_data = app_state_get_user_data();

    tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_timer_handler);

    health_service_set_heart_rate_sample_period(1 /* interval_s */);

    // Subscribing to health events causes a `HealthEventSignificantUpdate` which
    // will trigger us to update our health data
    health_service_events_subscribe(prv_health_service_event_handler, health_app_data);

    // Unsubscribe, we only want to do this on the initial appearance (opening the app)
    app_focus_service_unsubscribe();
  }
}

static void prv_initialize(void) {
  if (!activity_prefs_tracking_is_enabled()) {
    /// Health disabled text
    static const char *msg = i18n_noop("Track your steps, sleep, and more!"
                                       " Enable Pebble Health in the mobile app.");
    health_tracking_ui_show_message(RESOURCE_ID_HEART_TINY, msg, true);
    return;
  }

  activity_prefs_set_health_app_opened_version(CURRENT_HEALTH_APP_VERSION);

  HealthAppData *health_app_data = app_zalloc_check(sizeof(HealthAppData));

  app_state_set_user_data(health_app_data);

  health_app_data->health_data = health_data_create();
  health_data_update_quick(health_app_data->health_data);

  health_app_data->health_card_view = health_card_view_create(health_app_data->health_data);

  health_card_view_push(health_app_data->health_card_view);

  // Finish up initializing the app a bit later. This helps reduce lag when opening the app
  app_focus_service_subscribe_handlers((AppFocusHandlers){
    .did_focus = prv_finish_initialization_cb,
  });
}

//! Terminate application
static void prv_terminate(void) {
  HealthAppData *health_app_data = app_state_get_user_data();

  // cancel explicit hr sample period
  health_service_set_heart_rate_sample_period(0 /* interval_s */);

  if (health_app_data) {
    health_card_view_destroy(health_app_data->health_card_view);

    health_data_destroy(health_app_data->health_data);

    app_free(health_app_data);
  }
}

//! Main entry point
static void prv_main(void) {
  prv_initialize();
  app_event_loop();
  prv_terminate();
}

const PebbleProcessMd *health_app_get_info(void) {
  static const PebbleProcessMdSystem s_health_app_info = {
    .common = {
      .main_func = &prv_main,
      .uuid = UUID_HEALTH_DATA_SOURCE,
#if CAPABILITY_HAS_CORE_NAVIGATION4
      .visibility = ProcessVisibilityHidden,
#endif
    },
    .name = i18n_noop("Health"),
  };
  return (const PebbleProcessMd*) &s_health_app_info;
}
