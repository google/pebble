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

#include "app_run_state.h"
#include "launcher_app_message.h"

#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "process_management/process_manager.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "util/attributes.h"

#define PB_APP_STATE_ENDPOINT_ID       0x34

typedef struct PACKED {
  AppState state:8;
  Uuid uuid;
} AppRunState;


static void prv_send_response(void *data) {
  AppRunState *app_run_state = (AppRunState*)data;

  CommSession *session = comm_session_get_system_session();
  if (session) {
    if (comm_session_has_capability(session, CommSessionRunState)) {
      char uuid_buffer[UUID_STRING_BUFFER_LENGTH];
      bool success = comm_session_send_data(session, PB_APP_STATE_ENDPOINT_ID,
                                            (uint8_t*)app_run_state, sizeof(*app_run_state),
                                            COMM_SESSION_DEFAULT_TIMEOUT);

      uuid_to_string(&app_run_state->uuid, uuid_buffer);
      PBL_LOG(LOG_LEVEL_DEBUG, "AppRunState(0x34) %s sending status: %s - %u",
              (success ? "success" : "failed"), uuid_buffer, app_run_state->state);
    } else {
      PBL_LOG(LOG_LEVEL_DEBUG, "Using deprecated launcher_app_message");
      const bool is_running = ((app_run_state->state == RUNNING) ? true : false);
      launcher_app_message_send_app_state_deprecated(&app_run_state->uuid, is_running);
    }
  }

  kernel_free(app_run_state);
}

void app_run_state_command(CommSession *session, AppRunStateCommand cmd, const Uuid *uuid) {
  const AppInstallId install_id = app_install_get_id_for_uuid(uuid);

  // Log most recent communication timestamp:
  app_install_mark_prioritized(install_id, true /* can_expire */);

  if (install_id == INSTALL_ID_INVALID && cmd != APP_RUN_STATE_STATUS_COMMAND) {
    char uuid_buffer[UUID_STRING_BUFFER_LENGTH];
    uuid_to_string(uuid, uuid_buffer);
    PBL_LOG(LOG_LEVEL_DEBUG, "No app found with uuid %s", uuid_buffer);
    return;
  }

  switch (cmd) {
    case APP_RUN_STATE_RUN_COMMAND:
      // Launch the application provided it isn't running, otherwise this is a noop
      app_manager_put_launch_app_event(&(AppLaunchEventConfig) {
        .id = install_id,
        .common.reason = APP_LAUNCH_PHONE,
      });
      break;
    case APP_RUN_STATE_STOP_COMMAND:
      // Stop the application provided it is running, otherwise this is a noop
      app_install_unmark_prioritized(install_id);
      if (app_install_is_app_running(install_id)) {
        process_manager_put_kill_process_event(PebbleTask_App, true);
      }
      break;
    case APP_RUN_STATE_STATUS_COMMAND:
      // Determine the running application
      uuid = &app_manager_get_current_app_md()->uuid;
      if (session != NULL) {
        // We check the session here as to be backwards compatible with the 0x31 endpoint and
        // to avoid repeating code, the endpoint makes use of this function, but since it does
        // not have an active session (it's session is NULL), it will fall to the else case.
        AppRunState *app_run_state = kernel_malloc_check(sizeof(AppRunState));
        app_run_state->state = RUNNING;
        app_run_state->uuid = *uuid;
        prv_send_response(app_run_state);
      } else {
        launcher_app_message_send_app_state_deprecated(uuid, RUNNING);
      }
      break;
    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Unknown command: %d", cmd);
  }
}

void app_run_state_protocol_msg_callback(CommSession *session, const uint8_t *data, size_t length) {
  typedef struct PACKED {
    uint8_t command;
    Uuid uuid;
  } AppStateMessage;

  const AppStateMessage *msg = (const AppStateMessage*)data;

  if (msg->command != APP_RUN_STATE_STATUS_COMMAND) {
    if (length < sizeof(AppStateMessage)) {
      PBL_LOG(LOG_LEVEL_ERROR, "length mismatch, expected %"PRIu32" byte(s), got %"PRIu32" bytes",
              (uint32_t) sizeof(AppStateMessage), (uint32_t) length);
      return;
    }
  }
  app_run_state_command(session, msg->command, uuid_is_invalid(&msg->uuid) ? NULL : &msg->uuid);
}

void app_run_state_send_update(const Uuid *uuid, AppState app_state) {
#ifdef RECOVERY_FW
  // FIXME: Need to actually factor out this so it's totally removed from PRF, but for now just
  // pull it all. We can't use this here because we don't initialize app message at all.
  return;
#endif

  // This function deprecates the 0x31 launcher_app_message_send_app_state
  // providing a different method of interacting with the endpoint.  Calls the old
  // method if the mobile application does not support the new endpoint.

  CommSession *session = comm_session_get_system_session();
  if (!session) {
    // If we don't have a comm session open, don't bother sending application messages
    return;
  }

  // Offload to KernelBG, because this function is called 2x when switching apps and we want to
  // be sure not to block KernelMain for 2x 4000ms when the send buffer is full.
  AppRunState *app_run_state = kernel_malloc(sizeof(AppRunState));
  app_run_state->uuid = *uuid;
  app_run_state->state = app_state;
  system_task_add_callback(prv_send_response, app_run_state);
}
