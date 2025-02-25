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

#include "app_fetch_endpoint.h"

#include <string.h>
#include <stdbool.h>

#include "applib/rockyjs/rocky_res.h"
#include "kernel/pbl_malloc.h"
#include "process_management/pebble_process_info.h"
#include "services/common/comm_session/session.h"
#include "services/common/put_bytes/put_bytes.h"
#include "services/common/system_task.h"
#include "services/normal/app_cache.h"
#include "services/normal/blob_db/app_db.h"
#include "services/normal/process_management/app_storage.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/uuid.h"

//! Used for keeping track of binaries that are loaded through put_bytes
typedef struct {
  AppInstallId app_id;
  uint32_t total_size;
  AppFetchResult prev_error;
  bool cancelling;
  bool in_progress;
  bool app;
  bool worker;
  bool resources;
} AppFetchState;

//! Command type
enum {
  APP_FETCH_INSTALL_COMMAND = 0x01,
} AppFetchCommand;

//! Possible results that come back from the INSTALL_COMMAND
enum {
  APP_FETCH_INSTALL_RESPONSE = 0x01,
} AppFetchResponse;

//! Possible results that come back from the INSTALL_COMMAND
enum {
  APP_FETCH_RESPONSE_STARTING = 0x01,
  APP_FETCH_RESPONSE_BUSY = 0x02,
  APP_FETCH_RESPONSE_UUID_INVALID = 0x03,
  APP_FETCH_RESPONSE_NO_DATA = 0x04,
} AppFetchInstallResult;

//! Data sent to mobile phone for an INSTALL_COMMAND
typedef struct PACKED {
  uint8_t command;
  Uuid uuid;
  AppInstallId app_id;
} AppFetchInstallRequest;

//! Timeout used to determine how long we should wait before the phone starts sending the app
//! we requested (by issuing a put_bytes request).
#define FETCH_TIMEOUT_MS 15000

//! State for the app fetch flow
static AppFetchState s_fetch_state;

//! Endpoint ID
static const uint16_t APP_FETCH_ENDPOINT_ID = 6001;

////////////////////////////
// Internal Helper Functions
////////////////////////////

//! Puts an error event with the given error code
static void prv_put_event_error(uint8_t error_code) {
  s_fetch_state.prev_error = error_code;
  PebbleEvent event = {
    .type = PEBBLE_APP_FETCH_EVENT,
    .app_fetch = {
      .type = AppFetchEventTypeError,
      .id = s_fetch_state.app_id,
      .error_code = error_code,
    }
  };
  event_put(&event);
}

//! Puts an event with the given progress
static void prv_put_event_progress(uint8_t percent) {
  PebbleEvent event = {
    .type = PEBBLE_APP_FETCH_EVENT,
    .app_fetch = {
      .type = AppFetchEventTypeProgress,
      .id = s_fetch_state.app_id,
      .progress_percent = percent,
    }
  };
  event_put(&event);
}

//! Simply posts the type of event given.
static void prv_put_event_simple(AppFetchEventType type) {
  PebbleEvent event = {
    .type = PEBBLE_APP_FETCH_EVENT,
    .app_fetch = {
      .type = type,
      .id = s_fetch_state.app_id,
    }
  };
  event_put(&event);
}


//! Recomputes and saves the progress percent for the current application fetch session
static uint8_t prv_compute_progress_percent(PutBytesObjectType type, unsigned int type_percent) {
  // Add 33(34) percent for each piece that has finished (or is unneeded)
  uint8_t percent = 0;
  if (s_fetch_state.app) {
    percent += 30;
  }
  if (s_fetch_state.worker) {
    percent += 10;
  }
  if (s_fetch_state.resources) {
    percent += 60;
  }

  // add in the progress for the currently transferring piece.
  percent += (type_percent / 3);

  // store value
  return MIN(100, percent);
}

//! Cleans up the state of the app fetch endpoint. Always called from the system task
static void prv_cleanup(AppFetchResult result) {
  if (result != AppFetchResultSuccess) {
    put_bytes_cancel();
    app_cache_remove_entry(s_fetch_state.app_id);
    prv_put_event_error(result);
  }

  s_fetch_state.in_progress = false;

  PBL_LOG(LOG_LEVEL_INFO, "App fetch cleanup with result %d", result);
}

//! System task callback triggered by app_fetch_put_bytes_event_handler() when we are receiving
//! put_bytes messages in response to a fetch request to the phone.
void prv_put_bytes_event_system_task_cb(void *data) {
  PebblePutBytesEvent *pb_event = (PebblePutBytesEvent *)data;

  if (!s_fetch_state.in_progress) {
    return;
  }

  // If put_bytes has failed, let's just say fail and stop everything.
  if (pb_event->failed == true) {
    AppFetchResult error;
    if (s_fetch_state.cancelling) {
      PBL_LOG(LOG_LEVEL_WARNING, "Put bytes cancelled by user");
      error = AppFetchResultUserCancelled;
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Put bytes failure");
      error = AppFetchResultPutBytesFailure;
    }

    prv_cleanup(error);
    goto finally;
  }

  if (pb_event->type == PebblePutBytesEventTypeInitTimeout) {
    PBL_LOG(LOG_LEVEL_WARNING, "Timed out waiting for putbytes request from phone");
    prv_cleanup(AppFetchResultTimeoutError);
  }

  // If this is an object that doesn't have a cookie, then we won't care about it.
  if (pb_event->has_cookie == false) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Ignoring non cookie put_bytes event");
    goto finally;
  }

  // Check for the different types of PutBytes events.
  if (pb_event->type == PebblePutBytesEventTypeProgress) {
    // compute and save the new progress, then show it on the progress bar
    uint8_t percent =
        prv_compute_progress_percent(pb_event->object_type, pb_event->progress_percent);
    prv_put_event_progress(percent);
  } else if (pb_event->type == PebblePutBytesEventTypeCleanup) {
    // Mark off each finishing put_bytes transaction in our progress struct

    switch (pb_event->object_type) {
      case ObjectWatchApp:
        s_fetch_state.app = true;
        break;
      case ObjectWatchWorker:
        s_fetch_state.worker = true;
        break;
      case ObjectAppResources:
        s_fetch_state.resources = true;
        break;
      default:
        PBL_LOG(LOG_LEVEL_ERROR, "Got a PutBytes Object that we shouldn't have gotten");
        prv_cleanup(AppFetchResultGeneralFailure);
        goto finally;
    }

    // add the size of the finished PutBytes transaction to the total size.
    s_fetch_state.total_size += pb_event->total_size;
  }

  if (s_fetch_state.app && s_fetch_state.worker && s_fetch_state.resources) {
    // if everything has finished being transferred
    PBL_LOG(LOG_LEVEL_DEBUG, "All pieces (%"PRIu32" bytes) have been sent over put_bytes",
        s_fetch_state.total_size);

    // signify in the app cache that the app binaries are now loaded
    status_t added = app_cache_add_entry(s_fetch_state.app_id, s_fetch_state.total_size);
    if (added == S_SUCCESS) {
      const PebbleProcessMd *md = app_install_get_md(s_fetch_state.app_id, false);
      if (rocky_app_validate_resources(md) == RockyResourceValidation_Invalid) {
        PBL_LOG(LOG_LEVEL_ERROR, "Received app contains invalid JS bytecode");
        prv_put_event_error(AppFetchResultIncompatibleJSFailure);
      } else {
        // Set prev_error as a Success.
        s_fetch_state.prev_error = AppFetchResultSuccess;
        prv_put_event_simple(AppFetchEventTypeFinish);
      }
      app_install_release_md(md);
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Failed to insert into app cache: %"PRId32, added);
      prv_put_event_error(AppFetchResultGeneralFailure);
    }
    prv_cleanup(AppFetchResultSuccess);

  } else if (pb_event->type == PebblePutBytesEventTypeCleanup) {
    // Start the timeout watchdog again so we can tell if things get hung up
    // before the phone starts sending the next putbytes object.
    // This will only trigger if we've completed a piece and are still
    // waiting for another one.
    put_bytes_expect_init(FETCH_TIMEOUT_MS);
  }

finally:
  kernel_free(pb_event);
}

//! Put Bytes handler. Used for keeping track of progress and cleanup events. This is called
//! from KernelMain's event handler when it receives a PEBBLE_PUT_BYTES_EVENT event. put_bytes
//! posts these events to inform clients of progress.
void app_fetch_put_bytes_event_handler(PebblePutBytesEvent *pb_event) {
  // If an app fetch isn't in progress, ignore it.
  if (!s_fetch_state.in_progress) {
    return;
  }

  PebblePutBytesEvent *pb_event_copy = kernel_malloc_check(sizeof(PebblePutBytesEvent));
  memcpy(pb_event_copy, pb_event, sizeof(PebblePutBytesEvent));
  system_task_add_callback(prv_put_bytes_event_system_task_cb, pb_event_copy);
}

//! Callback for the system task to fire off the fetch request. Triggered by a call to
//! app_fetch_binaries().
static void prv_app_fetch_binaries_system_task_cb(void *data) {
  AppFetchInstallRequest *request = (AppFetchInstallRequest *)data;

  // check if Bluetooth is active. If so, this will send.
  bool successful = comm_session_send_data(comm_session_get_system_session(), APP_FETCH_ENDPOINT_ID,
      (uint8_t*)request, sizeof(AppFetchInstallRequest), COMM_SESSION_DEFAULT_TIMEOUT);

  // log it
  char uuid_buffer[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(&request->uuid, uuid_buffer);
  PBL_LOG(LOG_LEVEL_INFO, "%s request for app with uuid: %s and app_id: %"PRIu32"",
      successful ? "Sent" : "Failed to send", uuid_buffer, request->app_id);

  // free before error checking
  kernel_free(request);

  // If Bluetooth wasn't active, then post the error and cleanup.
  if (!successful) {
    prv_cleanup(AppFetchResultNoBluetooth);
    return;
  }

  // We next expect app_fetch_put_bytes_event_handler() to be called when the phone
  // gets our fetch request and issues a putbytes request.
  // Start the timeout watchdog to catch us in case the phone never issues the putbytes request.
  put_bytes_expect_init(FETCH_TIMEOUT_MS);
}

//! Called from the system task. Translates an Endpoint error to an event error and sends
//! off the appropriate event.
void prv_handle_app_fetch_install_response(uint8_t result_code) {
  switch (result_code) {
    case APP_FETCH_RESPONSE_STARTING:
      PBL_LOG(LOG_LEVEL_INFO, "Phone confirmed it will start sending data");
      prv_put_event_simple(AppFetchEventTypeStart);
      put_bytes_expect_init(FETCH_TIMEOUT_MS);
      break;
    case APP_FETCH_RESPONSE_BUSY:
      PBL_LOG(LOG_LEVEL_WARNING, "Error: Phone is currently busy");
      prv_cleanup(AppFetchResultPhoneBusy);
      break;
    case APP_FETCH_RESPONSE_UUID_INVALID:
      PBL_LOG(LOG_LEVEL_WARNING, "Error: UUID Invalid");
      prv_cleanup(AppFetchResultUUIDInvalid);
      break;
    case APP_FETCH_RESPONSE_NO_DATA:
      PBL_LOG(LOG_LEVEL_WARNING, "Error: No data on phone");
      prv_cleanup(AppFetchResultNoData);
      break;
  }
}

/////////////////////////
// Exported App Fetch API
/////////////////////////

//! Called by the system that triggers an app fetch install request
void app_fetch_binaries(const Uuid *uuid, AppInstallId app_id, bool has_worker) {
  if (s_fetch_state.in_progress) {
    PBL_LOG(LOG_LEVEL_WARNING, "Already an app fetch in progress. Ignoring request");
    return;
  }

  AppFetchInstallRequest *request = kernel_malloc_check(sizeof(AppFetchInstallRequest));

  // reset all state
  s_fetch_state = (AppFetchState){};

  // Mark whether the worker needs to be sent over.
  s_fetch_state.worker = !has_worker;
  s_fetch_state.app_id = app_id;
  s_fetch_state.in_progress = true;

  // populate fields
  request->command = APP_FETCH_INSTALL_COMMAND;
  request->uuid    = *uuid;
  request->app_id  = app_id;

  // Start "warming up" the connection, this will cause the low-latency period to start ~1s sooner.
  // Put bytes will extend the low-latency period after this:
  comm_session_set_responsiveness(comm_session_get_system_session(), BtConsumerPpAppFetch,
                                  ResponseTimeMin, MIN_LATENCY_MODE_TIMEOUT_APP_FETCH_SECS);

  system_task_add_callback(prv_app_fetch_binaries_system_task_cb, request);
}

AppFetchError app_fetch_get_previous_error(void) {
  AppFetchError error = {
    .error = s_fetch_state.prev_error,
    .id = s_fetch_state.app_id,
  };

  return error;
}

static void prv_cancel_fetch_from_system_task(void *data) {
  AppInstallId app_id = (AppInstallId)data;

  if ((!s_fetch_state.in_progress) ||
      ((s_fetch_state.app_id != app_id) && (app_id != INSTALL_ID_INVALID))) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Attempted to cancel an app that is currently not being"
            " fetched: %"PRId32, app_id);
    return;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Cancelling app fetch from system task");
  s_fetch_state.cancelling = true;
  put_bytes_cancel();
}

void app_fetch_cancel_from_system_task(AppInstallId app_id) {
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);

  prv_cancel_fetch_from_system_task((void *)(uintptr_t)app_id);
}

void app_fetch_cancel(AppInstallId app_id) {
  // Everything within app fetch happens on the background task
  system_task_add_callback(prv_cancel_fetch_from_system_task, (void *)(uintptr_t)app_id);
}

bool app_fetch_in_progress(void) {
  return s_fetch_state.in_progress;
}

////////////////////////////
// Exported Callbacks
////////////////////////////

typedef struct __attribute__((__packed__)) {
  uint8_t command;
  uint8_t result_code;
} AppFetchResponseData;

//! System task callback triggered by app_fetch_protocol_msg_callback().
static void prv_app_fetch_protocol_handle_msg(AppFetchResponseData *response_data) {

  switch (response_data->command) {
    case APP_FETCH_INSTALL_RESPONSE:
      prv_handle_app_fetch_install_response(response_data->result_code);
      break;
    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Invalid message received, command: %u result: %u",
          response_data->command, response_data->result_code);
      prv_cleanup(AppFetchResultGeneralFailure);
      break;
  }
}

//! Callback that is placed in the endpoints table. As of now, only responses will come through this
//! callback as all commands are originally sent to the phone.
void app_fetch_protocol_msg_callback(CommSession *session, const uint8_t *data, size_t length) {
  if (length < sizeof(AppFetchResponseData)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid message length %"PRIu32"", (uint32_t)length);
    prv_cleanup(AppFetchResultGeneralFailure);
    return;
  }

  if (!s_fetch_state.in_progress) {
    PBL_LOG(LOG_LEVEL_WARNING, "Got a message but app fetch not in progress. Ignoring");
    return;
  }

  prv_app_fetch_protocol_handle_msg((AppFetchResponseData *)data);
}
