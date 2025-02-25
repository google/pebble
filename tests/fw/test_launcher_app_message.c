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

#include "applib/app_message/app_message_internal.h"
#include "process_management/app_run_state.h"
#include "process_management/launcher_app_message.h"
#include "services/common/comm_session/session_internal.h"
#include "system/passert.h"
#include "util/dict.h"
#include "util/uuid.h"

extern void launcher_app_message_reset(void);
extern void launcher_app_message_protocol_msg_callback_deprecated(CommSession *session,
                                                                  const uint8_t* data,
                                                                  size_t length);

// Fakes
////////////////////////////////////
#include "fake_app_manager.h"
#include "fake_pbl_malloc.h"
#include "fake_session.h"
#include "fake_system_task.h"

// Stubs
////////////////////////////////////
#include "stubs_bt_lock.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_rand_ptr.h"

AppRunStateCommand s_last_cmd;

Transport *s_transport;
CommSession *s_session;

#define APP_UUID_RAW   0x13, 0xEC, 0xC6, 0x7C, 0xCC, 0xB4, 0x4A, 0x96, \
0x9E, 0xA7, 0x50, 0xE5, 0x09, 0xCA, 0xF7, 0x3A

#define LAUNCHER_MESSAGE_ENDPOINT_ID  (0x31)

#define RUN_STATE_KEY       (1)
#define STATE_FETCH_KEY     (2)
#define INVALID_KEY         (0xffffffff)

#define RUNNING             (1)
#define NOT_RUNNING         (0)

#define TRANSACTION_ID      (0xA5)

#define assert_ack(ack) \
  fake_comm_session_process_send_next(); \
  const AppMessageAck ack_message = { \
    .header = { \
      .command = ack ? CMD_ACK : CMD_NACK, \
      .transaction_id = TRANSACTION_ID, \
    }, \
  }; \
  fake_transport_assert_sent(s_transport, 0, LAUNCHER_MESSAGE_ENDPOINT_ID, \
                             (const uint8_t *)&ack_message, sizeof(ack_message));

void app_run_state_command(CommSession *session, AppRunStateCommand cmd, const Uuid *uuid) {
  s_last_cmd = cmd;
  Uuid expected_uuid = {APP_UUID_RAW};
  cl_assert_equal_b(uuid, &expected_uuid);
  if (cmd == APP_RUN_STATE_STATUS_COMMAND) {
    launcher_app_message_send_app_state_deprecated(uuid, RUNNING);
  }
}

// Helpers
////////////////////////////////////

static const uint8_t *prv_build_push_message(uint32_t key, uint8_t value, uint32_t *size) {
  // Using static buffer is OK because tests are single-threaded
  static uint8_t buffer[sizeof(AppMessagePush) + sizeof(Tuple) + sizeof(uint8_t)];
  AppMessagePush *push_message = (AppMessagePush *)buffer;

  *push_message = (const AppMessagePush) {
    .header = {
      .command = 0x01, // Push
      .transaction_id = TRANSACTION_ID,
    },
    .uuid = {
      APP_UUID_RAW
    },
  };

  *size = sizeof(Dictionary) + sizeof(Tuple) + sizeof(uint8_t);
  const Tuplet tuplet = TupletInteger(key, value);
  cl_assert_equal_i(DICT_OK, dict_serialize_tuplets_to_buffer(&tuplet, 1,
                                                              (uint8_t *)&push_message->dictionary,
                                                              size));

  // Including sizeof(AppMessagePush):
  *size = sizeof(buffer);
  return buffer;
}

static void prv_receive(uint32_t key, uint8_t value) {
  uint32_t length = 0;
  const uint8_t *msg= prv_build_push_message(key, value, &length);
  launcher_app_message_protocol_msg_callback_deprecated(s_session, msg, length);
}

// Tests
////////////////////////////////////


void test_launcher_app_message__initialize(void) {
  launcher_app_message_reset();
  s_last_cmd = APP_RUN_STATE_INVALID_COMMAND;
  fake_comm_session_init();
  s_transport = fake_transport_create(TransportDestinationSystem, NULL, NULL);
  s_session = fake_transport_set_connected(s_transport, true /* connected */);
}

void test_launcher_app_message__cleanup(void) {
  fake_transport_destroy(s_transport);
  s_transport = NULL;
  s_session = NULL;
  fake_comm_session_cleanup();
  fake_system_task_callbacks_cleanup();
}

void test_launcher_app_message__ignore_too_short_message(void) {
  uint8_t too_short = 0;
  launcher_app_message_protocol_msg_callback_deprecated(s_session, &too_short, sizeof(too_short));
  fake_comm_session_process_send_next();
  fake_transport_assert_nothing_sent(s_transport);
}

void test_launcher_app_message__receive_unknown_key(void) {
  prv_receive(INVALID_KEY, 0);
  assert_ack(false);
}

void test_launcher_app_message__receive_push_start(void) {
  prv_receive(RUN_STATE_KEY, RUNNING);
  assert_ack(true);
  cl_assert_equal_i(APP_RUN_STATE_RUN_COMMAND, s_last_cmd);
}

void test_launcher_app_message__receive_push_stop(void) {
  prv_receive(RUN_STATE_KEY, NOT_RUNNING);
  assert_ack(true);
  cl_assert_equal_i(APP_RUN_STATE_STOP_COMMAND, s_last_cmd);
}

void test_launcher_app_message__receive_push_fetch_request(void) {
  prv_receive(STATE_FETCH_KEY, RUNNING);
  assert_ack(true);
  cl_assert_equal_i(APP_RUN_STATE_STATUS_COMMAND, s_last_cmd);
}

void test_launcher_app_message__ignore_acks(void) {
  const AppMessageAck ack_message = {
    .header = {
      .command = CMD_ACK,
      .transaction_id = TRANSACTION_ID,
    },
  };
  launcher_app_message_protocol_msg_callback_deprecated(s_session, (const uint8_t *)&ack_message,
                                                        sizeof(ack_message));
  fake_comm_session_process_send_next();
  fake_transport_assert_nothing_sent(s_transport);
}

void test_launcher_app_message__send_app_state(void) {
  Uuid uuid = {APP_UUID_RAW};
  bool running = true;
  launcher_app_message_send_app_state_deprecated(&uuid, running);

  // Even though the Launcher App Message documentation states that the value is a uint8_t,
  // the original implementation used uint32_t for the outbound pushes... Let's keep that "bug":
  uint8_t buffer[sizeof(AppMessagePush) + sizeof(Tuple) + sizeof(uint32_t)];
  AppMessagePush *push_message = (AppMessagePush *)buffer;

  *push_message = (const AppMessagePush) {
    .header = {
      .command = CMD_PUSH,
      .transaction_id = 0,
    },
    .uuid = {APP_UUID_RAW},
  };

  uint32_t size = sizeof(Dictionary) + sizeof(Tuple) + sizeof(uint32_t);
  const Tuplet tuplet = TupletInteger(RUN_STATE_KEY, (uint32_t) (running ? RUNNING : NOT_RUNNING));
  PBL_ASSERTN(DICT_OK == dict_serialize_tuplets_to_buffer(&tuplet, 1,
                                                          (uint8_t *)&push_message->dictionary,
                                                          &size));
  fake_comm_session_process_send_next();
  fake_transport_assert_sent(s_transport, 0, LAUNCHER_MESSAGE_ENDPOINT_ID, buffer, sizeof(buffer));
}
