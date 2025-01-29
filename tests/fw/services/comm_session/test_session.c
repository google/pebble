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

#include "BSCAPI.h"
#include "clar.h"
#include "bluetooth/bt_driver_comm.h"
#include "services/common/comm_session/session.h"
#include "services/common/comm_session/session_remote_version.h"
#include "services/common/comm_session/session_send_buffer.h"
#include "services/common/comm_session/session_transport.h"
#include "kernel/events.h"

extern void comm_session_set_capabilities(CommSession *session,
                                          CommSessionCapability capability_flags);
extern bool comm_session_is_valid(const CommSession *session);
extern void comm_session_init(void);
extern void comm_session_deinit(void);
extern void comm_session_send_next_immediately(CommSession *session);

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_analytics.h"
#include "stubs_bt_lock.h"
#include "stubs_bt_stack.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_rand_ptr.h"
#include "stubs_syscall_internal.h"

void comm_session_analytics_open_session(CommSession *session) {
}

void comm_session_analytics_close_session(CommSession *session, CommSessionCloseReason reason) {
}

void comm_session_receive_router_cleanup(CommSession *session) {
}

void comm_session_send_queue_cleanup(CommSession *session) {
}

void bt_persistent_storage_set_cached_system_capabilities(
    const PebbleProtocolCapabilities *capabilities) {
}

static uint16_t s_send_queue_length;

size_t comm_session_send_queue_get_length(const CommSession *session) {
  return s_send_queue_length;
}

void fake_session_send_queue_set_length(uint32_t length) {
  s_send_queue_length = length;
}

static bool s_dls_private_handle_disconnect_called;
void dls_private_handle_disconnect(void *data) {
  s_dls_private_handle_disconnect_called = true;
}

static bool s_comm_session_event_put;
void event_put(PebbleEvent* event) {
  if (event->type == PEBBLE_COMM_SESSION_EVENT &&
      event->bluetooth.comm_session_event.is_system) {
    s_comm_session_event_put = true;
  };
}

void app_launch_trigger(void) {
}

void session_remote_version_start_requests(CommSession *session) {
}

static int s_send_next_count;

static void prv_send_next(Transport *transport) {
  ++s_send_next_count;
}

static int s_close_count;
Transport *s_last_closed_transport;

static void prv_close(Transport *transport) {
  ++s_close_count;
  s_last_closed_transport = transport;
}

static int s_reset_count;

static void prv_reset(Transport *transport) {
  ++s_reset_count;
}

typedef enum {
  TransportIDNull,
  TransportID1,
  TransportID2,
  TransportID3,
  NumTransportID,
} TransportID;

static Uuid *s_transport_uuid[NumTransportID];

static const Uuid *prv_get_uuid(Transport *transport) {
  TransportID i = (uintptr_t)transport;
  cl_assert(i >= TransportID1 && i <= TransportID3);
  return s_transport_uuid[i];
}

static CommSessionTransportType prv_get_type(struct Transport *transport) {
  return CommSessionTransportType_QEMU;
}

static const TransportImplementation s_transport_imp = {
  .send_next = prv_send_next,
  .close = prv_close,
  .reset = prv_reset,
  .get_uuid = prv_get_uuid,
  .get_type = prv_get_type,
};

// Fakes
///////////////////////////////////////////////////////////

#include "fake_kernel_malloc.h"
#include "fake_session_send_buffer.h"
#include "fake_system_task.h"
#include "fake_app_manager.h"

static void prv_system_task_cb(void *data) {
  CommSession *session = (CommSession *)data;
  bt_driver_run_send_next_job(session, true);
}

bool bt_driver_comm_schedule_send_next_job(CommSession *data) {
  // Implement this API in this test suite using the fake_system_task:
  system_task_add_callback(prv_system_task_cb, data);
  return true;
}

static bool s_bt_driver_comm_is_current_task_send_next_task;
bool bt_driver_comm_is_current_task_send_next_task(void) {
  return s_bt_driver_comm_is_current_task_send_next_task;
}

// Tests
///////////////////////////////////////////////////////////

void test_session__initialize(void) {
  stub_app_init();
  comm_session_init();
  fake_kernel_malloc_init();
  fake_kernel_malloc_mark();
  fake_session_send_buffer_init();
  s_send_queue_length = 0;
  memset(s_transport_uuid, 0, sizeof(s_transport_uuid));
  s_send_next_count = 0;
  s_reset_count = 0;
  s_close_count = 0;
  s_last_closed_transport = NULL;
  s_dls_private_handle_disconnect_called = false;
  s_comm_session_event_put = false;
  s_bt_driver_comm_is_current_task_send_next_task = false;
}

void test_session__cleanup(void) {
  // Check for leaks:
  fake_kernel_malloc_mark_assert_equal();
  fake_kernel_malloc_deinit();

  fake_system_task_callbacks_cleanup();
}

void test_session__get_system_session_disconnected_returns_null(void) {
  cl_assert_equal_p(comm_session_get_system_session(), NULL);
}

void test_session__get_app_session_disconnected_returns_null(void) {
  cl_assert_equal_p(comm_session_get_current_app_session(), NULL);
}

void test_session__send_data_returns_false_for_null_session(void) {
  const uint16_t endpoint_id = 1234;
  uint8_t data[] = {1, 2, 3, 4};
  cl_assert_equal_b(comm_session_send_data(NULL, endpoint_id,
                                           data, sizeof(data),
                                           COMM_SESSION_DEFAULT_TIMEOUT), false);
}

void test_session__basic_open_close(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);
  cl_assert(session);
  cl_assert_equal_b(comm_session_is_valid(session), true);
  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
  cl_assert_equal_b(comm_session_is_valid(session), false);
}

void test_session__get_type_system(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);
  cl_assert_equal_i(comm_session_get_type(session), CommSessionTypeSystem);
  cl_assert_equal_p(comm_session_get_system_session(), session);
  cl_assert_equal_p(comm_session_get_by_type(CommSessionTypeSystem), session);
  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
  fake_system_task_callbacks_invoke_pending(); // get dls called
  cl_assert_equal_b(s_dls_private_handle_disconnect_called, true);
  cl_assert_equal_b(s_comm_session_event_put, true);
}

void test_session__get_type_app(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationApp);
  cl_assert_equal_i(comm_session_get_type(session), CommSessionTypeApp);
  cl_assert_equal_p(comm_session_get_current_app_session(), session);
  cl_assert_equal_p(comm_session_get_by_type(CommSessionTypeApp), session);
  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
  fake_system_task_callbacks_invoke_pending(); // get dls called
  cl_assert_equal_b(s_dls_private_handle_disconnect_called, false);
  cl_assert_equal_b(s_comm_session_event_put, false);
}

void test_session__last_system_session_wins(void) {
  Transport *system_transport = (Transport *) TransportID1;
  CommSession *system_session = comm_session_open(system_transport, &s_transport_imp,
                                                  TransportDestinationSystem);

  cl_assert_equal_i(s_close_count, 0);

  Transport *system_transport2 = (Transport *) TransportID2;
  CommSession *system_session2 = comm_session_open(system_transport2, &s_transport_imp,
                                                   TransportDestinationSystem);

  cl_assert(system_session2);
  cl_assert_equal_p(s_last_closed_transport, system_transport);
  cl_assert_equal_i(s_close_count, 1);

  comm_session_close(system_session2, CommSessionCloseReason_UnderlyingDisconnection);

  // The transport's .close is supposed to call this.
  // The stub in this test doesn't, so clean up manually:
  comm_session_close(system_session, CommSessionCloseReason_UnderlyingDisconnection);
}

void test_session__get_app_session_multiple(void) {
  Transport *system_transport = (Transport *) TransportID1;
  CommSession *system_session = comm_session_open(system_transport, &s_transport_imp,
                                                  TransportDestinationSystem);
  Uuid legacy_app_uuid = {
    0xff, 0xc5, 0x24, 0x01, 0x4d, 0xbe, 0x40, 0x8b,
    0xb7, 0x3a, 0x0e, 0x80, 0xef, 0x09, 0xaf, 0x74};
  // Legacy transport (iAP) isn't aware of the app UUID, so don't set anything:
  Transport *legacy_transport = (Transport *) TransportID2;
  CommSession *legacy_app_session = comm_session_open(legacy_transport, &s_transport_imp,
                                                      TransportDestinationApp);
  Uuid modern_app_uuid = {
    0x04, 0xc5, 0x24, 0x01, 0x4d, 0xbe, 0x40, 0x8b,
    0xb7, 0x3a, 0x0e, 0x80, 0xef, 0x09, 0xaf, 0x74};
  Transport *modern_transport = (Transport *) TransportID3;
  s_transport_uuid[TransportID3] = &modern_app_uuid;
  CommSession *modern_app_session = comm_session_open(modern_transport, &s_transport_imp,
                                                      TransportDestinationApp);

  stub_app_set_uuid(legacy_app_uuid);
  cl_assert_equal_p(comm_session_get_current_app_session(), legacy_app_session);

  stub_app_set_uuid(modern_app_uuid);
  cl_assert_equal_p(comm_session_get_current_app_session(), modern_app_session);

  stub_app_set_uuid((Uuid)UUID_INVALID);
  cl_assert_equal_p(comm_session_get_current_app_session(), NULL);

  stub_app_set_uuid((Uuid)UUID_SYSTEM);
  cl_assert_equal_p(comm_session_get_current_app_session(), NULL);

  comm_session_close(system_session, CommSessionCloseReason_UnderlyingDisconnection);
  comm_session_close(legacy_app_session, CommSessionCloseReason_UnderlyingDisconnection);
  comm_session_close(modern_app_session, CommSessionCloseReason_UnderlyingDisconnection);
}

void test_session__assert_if_deinit_and_transport_did_not_clean_up_properly(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);
  cl_assert_equal_b(comm_session_is_valid(session), true);
  // Expect assert when Transport didn't clean up after itself:
  cl_assert_passert(comm_session_deinit());
  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
}

void test_session__send_next_deduping(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);
  cl_assert_equal_b(comm_session_is_valid(session), true);
  fake_session_send_queue_set_length(1234);

  cl_assert_equal_i(fake_system_task_count_callbacks(), 0);
  comm_session_send_next(session);
  cl_assert_equal_i(fake_system_task_count_callbacks(), 1);
  comm_session_send_next(session);
  cl_assert_equal_i(fake_system_task_count_callbacks(), 1);
  comm_session_send_next(session);
  cl_assert_equal_i(fake_system_task_count_callbacks(), 1);
  comm_session_send_next(session);
  cl_assert_equal_i(fake_system_task_count_callbacks(), 1);
  comm_session_send_next(session);
  cl_assert_equal_i(fake_system_task_count_callbacks(), 1);
  cl_assert_equal_i(s_send_next_count, 0);

  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(fake_system_task_count_callbacks(), 0);
  cl_assert_equal_i(s_send_next_count, 1);

  comm_session_send_next(session);
  cl_assert_equal_i(fake_system_task_count_callbacks(), 1);
  comm_session_send_next(session);
  cl_assert_equal_i(fake_system_task_count_callbacks(), 1);
  cl_assert_equal_i(s_send_next_count, 1);

  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_send_next_count, 2);

  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
}

void test_session__send_next_not_called_when_session_closed_in_mean_time(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);
  cl_assert_equal_b(comm_session_is_valid(session), true);
  fake_session_send_queue_set_length(1234);

  cl_assert_equal_i(fake_system_task_count_callbacks(), 0);
  comm_session_send_next(session);

  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(fake_system_task_count_callbacks(), 0);
  cl_assert_equal_i(s_send_next_count, 0);
}

static bool prv_schedule_send_next(CommSession *session) {
  return true;
}

static bool s_is_current_task_schedule_task = false;
static bool prv_is_current_task_schedule_task(Transport *transport) {
  return s_is_current_task_schedule_task;
}

void test_session__transport_send_next_task(void) {
  TransportImplementation transport_imp = s_transport_imp;
  transport_imp.schedule = prv_schedule_send_next;
  transport_imp.is_current_task_schedule_task = prv_is_current_task_schedule_task;

  extern bool comm_session_is_current_task_send_next_task(CommSession *session);

  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &transport_imp,
                                           TransportDestinationSystem);

  s_is_current_task_schedule_task = true;
  cl_assert_equal_b(comm_session_is_current_task_send_next_task(session), true);
  s_is_current_task_schedule_task = false;
  cl_assert_equal_b(comm_session_is_current_task_send_next_task(session), false);
  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);

  transport_imp.schedule = NULL;
  transport_imp.is_current_task_schedule_task = NULL;

  session = comm_session_open(transport, &transport_imp, TransportDestinationSystem);

  s_bt_driver_comm_is_current_task_send_next_task = true;
  cl_assert_equal_b(comm_session_is_current_task_send_next_task(session), true);
  s_bt_driver_comm_is_current_task_send_next_task = false;
  cl_assert_equal_b(comm_session_is_current_task_send_next_task(session), false);
  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
}

void test_session__reset_valid_session(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);
  cl_assert_equal_b(comm_session_is_valid(session), true);
  cl_assert_equal_i(s_reset_count, 0);
  comm_session_reset(session);
  cl_assert_equal_i(s_reset_count, 1);

  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
}

void test_session__reset_invalid_session(void) {
  CommSession *invalid_session = (CommSession *) TransportID1;
  cl_assert_equal_b(comm_session_is_valid(invalid_session), false);
  cl_assert_equal_i(s_reset_count, 0);
  comm_session_reset(invalid_session);
  cl_assert_equal_i(s_reset_count, 0);
}

extern bool comm_session_send_next_is_scheduled(CommSession *session);

void test_session__send_next_is_schedule_flag_not_unset_after_immediate_call(void){
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);
  cl_assert_equal_b(comm_session_is_valid(session), true);

  comm_session_send_next(session);
  cl_assert_equal_b(comm_session_send_next_is_scheduled(session), true);

  // Calling comm_session_send_next_immediately should NOT result in the flag to get unset
  comm_session_send_next_immediately(session);
  cl_assert_equal_b(comm_session_send_next_is_scheduled(session), true);

  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(comm_session_send_next_is_scheduled(session), false);

  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
}

void test_session__capabilities(void) {
  Transport *transport = (Transport *) TransportID1;
  CommSession *session = comm_session_open(transport, &s_transport_imp,
                                           TransportDestinationSystem);

  for (int i = 0; i < (sizeof(int) * 8); ++i) {
    CommSessionCapability capability = (1 << i);
    cl_assert_equal_b(false, comm_session_has_capability(session, capability));
  }

  comm_session_set_capabilities(session, ~0);

  for (int i = 0; i < (sizeof(int) * 8); ++i) {
    CommSessionCapability capability = (1 << i);
    cl_assert_equal_b(true, comm_session_has_capability(session, capability));
  }

  comm_session_close(session, CommSessionCloseReason_UnderlyingDisconnection);
}
