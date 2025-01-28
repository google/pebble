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

#include "clar.h"

//#include "comm/remote.h"
#include "process_management/app_run_state.h"
#include "services/common/comm_session/protocol.h"
#include "system/passert.h"

#include "util/attributes.h"
#include "util/list.h"

#include <stdlib.h>

// Stubs
///////////////////////////////////////
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_rand_ptr.h"

// Fakes
///////////////////////////////////////
#include "fake_app_manager.h"
#include "fake_pebble_tasks.h"

// Structures
///////////////////////////////////////
typedef struct PACKED {
  uint8_t command;
  Uuid uuid;
} AppStateMessage;

struct CommSession {
};

typedef struct PACKED {
  AppState state:8;
  Uuid uuid;
} AppRunState;

// Globals
///////////////////////////////////////
CommSession *s_session = NULL;

static uint8_t s_launcher_deprecated_messages = 0;

static uint8_t s_app_run_state_messages = 0;

static uint8_t s_free_count = 0;

static uint8_t s_malloc_count = 0;

static void *s_ptr = NULL;

static uint8_t s_app_state;

static uint64_t s_flags = 0;

static const Uuid s_app_uuid = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5
};

// Helpers
///////////////////////////////////////
static void prv_set_remote_active(void) {
  static const int deadbeef = 0xdeadbeef;
  s_session = (void*)&deadbeef;
  s_flags = 0;
}

static void prv_set_expected(AppState app_state) {
  s_app_state = app_state;
}

static void prv_set_remote_capability(CommSessionCapability c) {
  s_flags |= c;
}

bool comm_session_has_capability(CommSession *session, CommSessionCapability c) {
  return (s_flags & c) != 0;
}

void app_install_unmark_prioritized(const Uuid *uuid) {
  return;
}

bool app_install_is_app_running(AppInstallId id) {
  return true;
}

void app_install_mark_prioritized(AppInstallId install_id, bool can_expire) {
}

bool system_task_add_callback(void(*cb)(void *data), void *data) {
  cb(data);
  return true;
}

status_t app_cache_app_launched(AppInstallId id) {
  return 0;
}

void app_manager_put_launch_app_event(const AppLaunchEventConfig *config) {
  app_run_state_send_update(&app_manager_get_current_app_md()->uuid, RUNNING);
}

void process_manager_put_kill_process_event(PebbleTask task, bool gracefully) {
  app_run_state_send_update(&app_manager_get_current_app_md()->uuid, NOT_RUNNING);
}

CommSession *comm_session_get_system_session(void) {
  return s_session;
}

void launcher_app_message_send_app_state_deprecated(const Uuid *uuid, bool running) {
  s_launcher_deprecated_messages++;
  cl_assert(running == (s_app_state == RUNNING ? true : false));
}

bool comm_session_send_data(CommSession *session, uint16_t endpoint_id,
                            const uint8_t *data, size_t length, uint32_t timeout_ms) {
  AppRunState *state = (AppRunState*)data;
  s_app_run_state_messages++;
  cl_assert(state->state == s_app_state);
  return true;
}

void bt_lock(void) {
  return;
}

void bt_unlock(void) {
  return;
}


// Tests
///////////////////////////////////////

extern void app_run_state_protocol_msg_callback(CommSession*, const uint8_t*, size_t);

void test_app_run_state__initialize(void) {
  s_launcher_deprecated_messages = 0;
  s_app_run_state_messages = 0;
  s_session = NULL;
  s_flags = 0;

  s_malloc_count = 0;
  s_free_count = 0;

  stub_app_init();
}


void test_app_run_state__cleanup(void) {
  // Always ensure that after any test, all malloc'd data has been freed
  cl_assert_equal_i(s_malloc_count, s_free_count);
}


void test_app_run_state__send_update(void) {
  // Tests that when app_run_state_send_update is called, that the proper
  // callback is triggered depending on the device / version
  app_run_state_send_update(&s_app_uuid, RUNNING);

  // When no remote is set, this should just not run
  cl_assert_equal_i(s_launcher_deprecated_messages, 0);

  // Set the remote as being active
  prv_set_remote_active();
  stub_app_set_uuid(s_app_uuid);

  // When app_run_state is not supported, should use launcher_app_message
  prv_set_expected(RUNNING);
  app_run_state_send_update(&s_app_uuid, RUNNING);
  cl_assert_equal_i(s_launcher_deprecated_messages, 1);

  // When app_run_state is supported, should use app_run_state
  prv_set_expected(NOT_RUNNING);
  prv_set_remote_capability(CommSessionRunState);
  app_run_state_send_update(&s_app_uuid, NOT_RUNNING);
  cl_assert_equal_i(s_launcher_deprecated_messages, 1);
  cl_assert_equal_i(s_app_run_state_messages, 1);

  // Changing the remote should change the flags and use launcher_app_message
  // if app_run_state not supported
  prv_set_remote_active();
  app_run_state_send_update(&s_app_uuid, NOT_RUNNING);
  cl_assert_equal_i(s_launcher_deprecated_messages, 2);
  cl_assert_equal_i(s_app_run_state_messages, 1);
}

void test_app_run_state__protocol_msg_callback(void) {
  // Tests app_run_state_protocol_msg_callback which should take data
  // from a source and perform the appropriate command
  prv_set_remote_active();
  prv_set_remote_capability(CommSessionRunState);
  stub_app_set_uuid(s_app_uuid);
  stub_app_set_install_id(1337);

  CommSession session;
  AppStateMessage msg;

  msg.command = APP_RUN_STATE_INVALID_COMMAND;
  memcpy(&msg.uuid, &s_app_uuid, sizeof(s_app_uuid));

  app_run_state_protocol_msg_callback(&session, (uint8_t*)&msg, sizeof(msg));

  // This should be a noop since the key is invalid
  cl_assert_equal_i(s_launcher_deprecated_messages, 0);
  cl_assert_equal_i(s_app_run_state_messages, 0);

  AppRunStateCommand commands[] = {
    APP_RUN_STATE_INVALID_COMMAND,
    APP_RUN_STATE_RUN_COMMAND,
    APP_RUN_STATE_STOP_COMMAND,
    APP_RUN_STATE_STATUS_COMMAND
  };

  AppState expected[] = {
    RUNNING,
    RUNNING,
    NOT_RUNNING,
    RUNNING
  };

  // Since our version is >= 2.2, this should use the new endpoint
  // And check that we're getting back the right state
  for (int msg_count = 0; msg_count < 4; msg_count++) {
    msg.command = commands[msg_count];
    prv_set_expected(expected[msg_count]);
    app_run_state_protocol_msg_callback(&session, (uint8_t*)&msg, sizeof(msg));
    cl_assert_equal_i(s_launcher_deprecated_messages, 0);
    cl_assert_equal_i(s_app_run_state_messages, msg_count);
  }

  app_run_state_protocol_msg_callback(NULL, (uint8_t*)&msg, sizeof(msg));

  cl_assert_equal_i(s_launcher_deprecated_messages, 1);
}
