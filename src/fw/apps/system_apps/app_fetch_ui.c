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

#include "app_fetch_ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "applib/app.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/progress_window.h"
#include "applib/ui/ui.h"
#include "drivers/battery.h"
#include "kernel/pbl_malloc.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "process_management/worker_manager.h"
#include "process_state/app_state/app_state.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/app_fetch_endpoint.h"
#include "services/normal/timeline/timeline_resources.h"
#include "apps/system_apps/timeline/peek_layer.h"
#include "shell/normal/watchface.h"
#include "shell/shell.h"
#include "shell/system_app_state_machine.h"
#include "services/common/compositor/compositor_transitions.h"
#include "system/logging.h"
#include "system/passert.h"
#include "services/common/evented_timer.h"

#define FAIL_PAUSE_MS 1000
#define SCROLL_OUT_MS 250
#define BAR_HEIGHT 6
#define BAR_WIDTH 80
#define BAR_TO_TRANS_MS 160
#define TRANS_TO_DOT_MS 90
#define DOT_TRANSITION_RADIUS 13
#define DOT_COMPOSITOR_RADIUS 7
#define DOT_OFFSET 25
#define UPDATE_INTERVAL 200
#define UPDATE_AMOUNT 2
#define FAILURE_PERCENT 15
#define INITIAL_PERCENT 0

//! App data
typedef struct {
  //! UI
  ProgressWindow window;

  //! App fetch result
  AppFetchResult result;

  //! Data
  AppInstallEntry install_entry;
  AppFetchUIArgs next_app_args;
  EventServiceInfo fetch_event_info;
  EventServiceInfo connect_event_info;

  bool failed;
} AppFetchUIData;

static void prv_set_progress(AppFetchUIData *data, int16_t progress) {
  progress_window_set_progress(&data->window, progress);
}

// Launch the desired app
static void prv_app_fetch_launch_app(AppFetchUIData *data) {
  // Let's launch the application we just fetched.
  PBL_LOG(LOG_LEVEL_DEBUG, "App Fetch: Putting launch event");

  // if this was launched by the phone, it's probably a new install
  if ((data->next_app_args.common.reason == APP_LAUNCH_PHONE) &&
      !battery_is_usb_connected()) {
    vibes_short_pulse();
  }

  // Allocate and initialize the data that would have been sent to the app originally before the
  // fetch request.
  PebbleLaunchAppEventExtended *ext = kernel_malloc_check(sizeof(PebbleLaunchAppEventExtended));
  *ext = (PebbleLaunchAppEventExtended) {
    .common = data->next_app_args.common,
    .wakeup = data->next_app_args.wakeup_info
  };
#if PLATFORM_TINTIN
  ext->common.transition = compositor_app_slide_transition_get(true /* slide to right */);
#else
  ext->common.transition = compositor_dot_transition_app_fetch_get();
#endif
  if ((data->next_app_args.common.reason == APP_LAUNCH_WAKEUP) &&
      (data->next_app_args.common.args != NULL)) {
    ext->common.args = &data->next_app_args.wakeup_info;
  }

  PebbleEvent launch_event = {
    .type = PEBBLE_APP_LAUNCH_EVENT,
    .launch_app = {
      .id = data->next_app_args.app_id,
      .data = ext
    }
  };

  event_put(&launch_event);
}

///////////////////////////////
// Animation Related Functions
///////////////////////////////

static void prv_remote_comm_session_event_handler(PebbleEvent *event, void *context) {
  AppFetchUIData *data = app_state_get_user_data();
  if (event->bluetooth.comm_session_event.is_open &&
      event->bluetooth.comm_session_event.is_system) {
    progress_window_pop(&data->window);
  }
}

static void prv_set_progress_failure(AppFetchUIData *data) {
  uint32_t icon;
  const char *message;
  switch (data->result) {
    case AppFetchResultNoBluetooth:
      icon = TIMELINE_RESOURCE_WATCH_DISCONNECTED;
      message = i18n_get("Not connected", data);
      // Subscribe to the BT remote app connect event
      data->connect_event_info = (EventServiceInfo) {
        .type = PEBBLE_COMM_SESSION_EVENT,
        .handler = prv_remote_comm_session_event_handler
      };
      event_service_client_subscribe(&data->connect_event_info);
      break;
    case AppFetchResultNoData:
      icon = TIMELINE_RESOURCE_CHECK_INTERNET_CONNECTION;
#if PBL_ROUND
      // TODO PBL-28730: Fix peek layer so it does its own line wrapping
      message = i18n_get("No internet\nconnection", data);
#else
      message = i18n_get("No internet connection", data);
#endif
      break;
    case AppFetchResultIncompatibleJSFailure:
      // TODO: PBL-39752 make this a more expressive error message with a call to action
      icon = TIMELINE_RESOURCE_GENERIC_WARNING;
      message = i18n_get("Incompatible JS", data);
      break;
    case AppFetchResultGeneralFailure:
    case AppFetchResultUUIDInvalid:
    case AppFetchResultPutBytesFailure:
    case AppFetchResultTimeoutError:
    case AppFetchResultPhoneBusy:
    default:
      icon = TIMELINE_RESOURCE_GENERIC_WARNING;
      message = i18n_get("Failed", data);
      break;
  }

  progress_window_set_result_failure(&data->window, icon, message,
                                     PROGRESS_WINDOW_DEFAULT_FAILURE_DELAY_MS);

  if (!battery_is_usb_connected()) {
    vibes_short_pulse();
  }
}

static void prv_progress_window_finished(ProgressWindow *window, bool success, void *context) {
  AppFetchUIData *data = context;
  if (success) {
    prv_app_fetch_launch_app(data);
  }
}

////////////////////////////
// Internal Helper Functions
////////////////////////////

//! Used to clean up the application's data before exiting
static void prv_app_fetch_cleanup(AppFetchUIData *data) {
  PBL_LOG(LOG_LEVEL_DEBUG, "App Fetch: prv_app_fetch_cleanup");
  event_service_client_unsubscribe(&data->fetch_event_info);
  event_service_client_unsubscribe(&data->connect_event_info);
}

//! Used when the app fetch process has failed
static void prv_app_fetch_failure(AppFetchUIData *data, uint8_t error_code) {
  PBL_LOG(LOG_LEVEL_WARNING, "App Fetch: prv_app_fetch_failure: %d", error_code);

  if (error_code == AppFetchResultUserCancelled) {
    app_window_stack_pop(true);
  }
  data->result = error_code;

  if ((watchface_get_default_install_id() == data->install_entry.install_id) &&
      app_install_entry_is_watchface(&data->install_entry)) {
    // We failed to fetch a watchface and it was our default.
    // Invalidate it and it will be reassigned to one that exists next time around.
    PBL_LOG(LOG_LEVEL_WARNING, "Default watchface fetch failed, setting INVALID as default");
    watchface_set_default_install_id(INSTALL_ID_INVALID);
  } else if ((worker_manager_get_default_install_id() == data->install_entry.install_id) &&
             app_install_entry_has_worker(&data->install_entry)) {
    // We failed to fetch a worker and it was our default.
    // Invalidate it and it will be reassigned to one that is launched next.
    PBL_LOG(LOG_LEVEL_WARNING, "Default worker fetch failed, setting INVALID as default");
    worker_manager_set_default_install_id(INSTALL_ID_INVALID);
  }

  data->failed = true;
  prv_set_progress_failure(data);
  prv_app_fetch_cleanup(data);
}

//! App Fetch handler. Used for keeping track of progress and cleanup events
static void prv_app_fetch_event_handler(PebbleEvent *event, void *context) {
  AppFetchUIData *data = app_state_get_user_data();
  PebbleAppFetchEvent *af_event = (PebbleAppFetchEvent *) event;

  // We have starting the App Fetch Process
  if (af_event->type == AppFetchEventTypeStart) {
    PBL_LOG(LOG_LEVEL_DEBUG, "App Fetch: Got the start event");

  // We have received a new progress event
  } else if (af_event->type == AppFetchEventTypeProgress) {
    progress_window_set_progress(&data->window, af_event->progress_percent);

  // We have finished the app fetch. Launching
  } else if (af_event->type == AppFetchEventTypeFinish) {
    progress_window_set_result_success(&data->window);
    prv_app_fetch_cleanup(data);

  // We received an error. Fail
  } else if (af_event->type == AppFetchEventTypeError) {
    prv_app_fetch_failure(data, af_event->error_code);
  }
}

// TODO: Use appropriate transitions to and from watchfaces or apps
static void prv_click_handler(ClickRecognizerRef recognizer, Window *window) {
  AppFetchUIData *data = app_state_get_user_data();
  if (data->failed) {
    app_window_stack_pop(true);
  } else {
    app_fetch_cancel(data->install_entry.install_id);
  }
}

static void config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, (ClickHandler) prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, (ClickHandler) prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, (ClickHandler) prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, (ClickHandler) prv_click_handler);
}

static void handle_init(void) {
  AppFetchUIData* data = app_zalloc_check(sizeof(AppFetchUIData));
  app_state_set_user_data(data);

  // get app args, copy them to app memory, and free the kernel buffer
  AppFetchUIArgs *temp_fetch_args =
      (AppFetchUIArgs *)process_manager_get_current_process_args();
  memcpy(&data->next_app_args, temp_fetch_args, sizeof(AppFetchUIArgs));
  kernel_free(temp_fetch_args);

  // Create and set up window
  progress_window_init(&data->window);
  progress_window_set_callbacks(&data->window, (ProgressWindowCallbacks) {
    .finished = prv_progress_window_finished,
  }, data);
  window_set_click_config_provider((Window *)&data->window, config_provider);

  // retrieve data about the AppInstallId given
  if (!app_install_get_entry_for_install_id(data->next_app_args.app_id, &data->install_entry)) {
    PBL_LOG(LOG_LEVEL_ERROR, "App Fetch: Error getting entry for id: %"PRIu32"",
        data->next_app_args.app_id);
    return;
  }

  AppFetchError prev_error = app_fetch_get_previous_error();
  if ((prev_error.id == data->next_app_args.app_id) &&
      (prev_error.error != AppFetchResultSuccess))  {
    prv_app_fetch_failure(data, prev_error.error);
    prv_set_progress(data, FAILURE_PERCENT);
  }

  // subscribe to PutBytes events
  data->fetch_event_info = (EventServiceInfo) {
    .type = PEBBLE_APP_FETCH_EVENT,
    .handler = prv_app_fetch_event_handler
  };
  event_service_client_subscribe(&data->fetch_event_info);

  app_progress_window_push(&data->window);
}

static void handle_deinit(void) {
  AppFetchUIData* data = app_state_get_user_data();
  prv_app_fetch_cleanup(data);
  progress_window_deinit(&data->window);
  app_free(data);
  i18n_free_all(data);
}

static void s_main(void) {
  handle_init();

  app_event_loop();

  handle_deinit();
}

const PebbleProcessMd *app_fetch_ui_get_app_info() {
  static const PebbleProcessMdSystem s_app_md = {
    .common = {
      .main_func = s_main,
      .visibility = ProcessVisibilityHidden,
      // UUID: 674271bc-f4fa-4536-97f3-8849a5ba75a4
      .uuid = {0x67, 0x42, 0x71, 0xbc, 0xf4, 0xfa, 0x45, 0x36,
               0x97, 0xf3, 0x88, 0x49, 0xa5, 0xba, 0x75, 0xa4},
    },
    .name = "App Fetch",
  };
  return (const PebbleProcessMd*) &s_app_md;
}
