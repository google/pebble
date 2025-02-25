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
#include "kernel/events.h"
#include "services/common/comm_session/meta_endpoint.h"
#include "services/common/comm_session/session_receive_router.h"
#include "services/common/comm_session/session_remote_version.h"
#include "services/common/comm_session/session_transport.h"
#include "services/common/comm_session/test_endpoint_ids.h"
#include "system/logging.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_analytics.h"
#include "stubs_bt_lock.h"
#include "stubs_bt_stack.h"
#include "stubs_events.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_rand_ptr.h"
#include "stubs_syscall_internal.h"

void app_launch_trigger(void) {
}

bool bt_driver_comm_schedule_send_next_job(CommSession *data) {
  return true;
}

bool bt_driver_comm_is_current_task_send_next_task(void) {
  return false;
}

void comm_session_analytics_inc_bytes_received(CommSession *session, uint16_t length) {
}

void comm_session_analytics_open_session(CommSession *session) {
}

void comm_session_analytics_close_session(CommSession *session, CommSessionCloseReason reason) {
}

void comm_session_send_queue_cleanup(CommSession *session) {
}

size_t comm_session_send_queue_get_length(const CommSession *session) {
  return 0;
}

void dls_private_handle_disconnect(void *data) {
}

void session_remote_version_start_requests(CommSession *session) {
}

void bt_persistent_storage_set_cached_system_capabilities(
    const PebbleProtocolCapabilities *capabilities) {
}

// Fakes
///////////////////////////////////////////////////////////

#include "fake_kernel_malloc.h"
#include "fake_session_send_buffer.h"
#include "fake_system_task.h"
#include "fake_app_manager.h"

MetaResponseInfo s_last_meta_response_info;

void meta_endpoint_send_response_async(const MetaResponseInfo *meta_response_info) {
  s_last_meta_response_info = *meta_response_info;
}

#define assert_meta_response_sent(e) \
  cl_assert_equal_i(e, s_last_meta_response_info.payload.error_code)

// Helpers
///////////////////////////////////////////////////////////

#define PP_MSG_BYTES(endpoint_id, length, ...) \
  { \
    length & 0xff, length >> 8, \
    endpoint_id && 0xff, endpoint_id >> 8, \
    __VA_ARGS__ \
  }

#define RECEIVE(...) \
  { \
    const uint8_t partial_data[] = { __VA_ARGS__ }; \
    comm_session_receive_router_write(s_session, partial_data, sizeof(partial_data)); \
  }

static void prv_send_next(Transport *transport);
static void prv_reset(Transport *transport);
static void prv_set_connection_responsiveness(Transport *transport,
                                              BtConsumer consumer,
                                              ResponseTimeState state,
                                              uint16_t max_period_secs,
                                              ResponsivenessGrantedHandler granted_handler);

CommSession *s_session;

Transport *s_transport = (Transport *) ~0;

const TransportImplementation s_transport_implementation = {
  .send_next = prv_send_next,
  .reset = prv_reset,
  .set_connection_responsiveness = prv_set_connection_responsiveness,
};

static void prv_send_next(Transport *transport) {

}

static void prv_reset(Transport *transport) {

}

static void prv_set_connection_responsiveness(Transport *transport,
                                              BtConsumer consumer,
                                              ResponseTimeState state,
                                              uint16_t max_period_secs,
                                              ResponsivenessGrantedHandler granted_handler) {

}

// Referenced from protocol_endpoints_table.auto.h override header:
///////////////////////////////////////////////////////////

typedef enum {
  TestEndpointPrivate,
  TestEndpointPublic,
  TestEndpointAny,
  NumTestEndpoint,
} TestEndpoint;

static int s_protocol_callback_counts[NumTestEndpoint];

void private_test_protocol_msg_callback(CommSession *session,
                                        const uint8_t* data, size_t length) {
  ++s_protocol_callback_counts[TestEndpointPrivate];
}

void public_test_protocol_msg_callback(CommSession *session,
                                       const uint8_t* data, size_t length) {
  ++s_protocol_callback_counts[TestEndpointPublic];
}

void any_test_protocol_msg_callback(CommSession *session,
                                    const uint8_t* data, size_t length) {
  ++s_protocol_callback_counts[TestEndpointAny];
}

static struct {
  int foo;
} s_test_receiver_ctx;

static int s_prepare_count = 0;
static int s_finish_count = 0;
static int s_cleanup_count = 0;

static uint8_t s_write_buffer[1024];
static uint16_t s_write_length;

static bool s_prepare_return_null = false;

static Receiver * prv_system_test_receiver_prepare(CommSession *session,
                                                   const PebbleProtocolEndpoint *endpoint,
                                                   size_t total_msg_length) {
  ++s_prepare_count;
  if (s_prepare_return_null) {
    return NULL;
  }
  return (Receiver *) &s_test_receiver_ctx;
}

static void prv_system_test_receiver_write(Receiver *receiver,
                                           const uint8_t *data, size_t length) {
  cl_assert(s_write_length + length < sizeof(s_write_buffer));
  memcpy(s_write_buffer + s_write_length, data, length);
  s_write_length += length;

  PBL_LOG(LOG_LEVEL_DEBUG, "Wrote %zu bytes", length);
}

static void prv_system_test_receiver_finish(Receiver *receiver) {
  ++s_finish_count;
}

static void prv_system_test_receiver_cleanup(Receiver *receiver) {
  ++s_cleanup_count;
}

const ReceiverImplementation g_system_test_receiver_imp = {
  .prepare = prv_system_test_receiver_prepare,
  .write = prv_system_test_receiver_write,
  .finish = prv_system_test_receiver_finish,
  .cleanup = prv_system_test_receiver_cleanup,
};

// Tests
///////////////////////////////////////////////////////////


void test_session_receive_router__initialize(void) {
  s_session = comm_session_open(s_transport, &s_transport_implementation,
                                TransportDestinationSystem);
  memset(s_protocol_callback_counts, 0, sizeof(s_protocol_callback_counts));
  s_prepare_count = 0;
  s_finish_count = 0;
  s_cleanup_count = 0;
  s_write_length = 0;
  s_last_meta_response_info = (const MetaResponseInfo) {};
  s_prepare_return_null = false;
}

void test_session_receive_router__cleanup(void) {
  if (s_session) {
    comm_session_close(s_session, CommSessionCloseReason_UnderlyingDisconnection);
    s_session = NULL;
  }
}

void test_session_receive_router__header_byte_by_byte(void) {
  // Expect callback to "prepare" only after complete Pebble Protocol header has been received.

  // Length high byte (big endian!)
  RECEIVE(0x00);
  cl_assert_equal_i(s_prepare_count, 0);

  // Length low byte (big endian!)
  RECEIVE(0x01);
  cl_assert_equal_i(s_prepare_count, 0);

  // Endpoint low byte (big endian!)
  RECEIVE(PRIVATE_TEST_ENDPOINT_ID >> 8);
  cl_assert_equal_i(s_prepare_count, 0);

  // Endpoint low byte (big endian!)
  RECEIVE(PRIVATE_TEST_ENDPOINT_ID & 0xff);
  cl_assert_equal_i(s_prepare_count, 1);
}

void test_session_receive_router__unhandled_endpoint(void) {
  // Expect "Unhandled" meta message to be replied to a message for an unknown endpoint.
  // The message should get eaten and not interfere with whatever comes next.

  // Length: 1, Endpoint ID: NON_EXISTENT_ENDPOINT_ID, Payload: 0x55
  RECEIVE(0x00, 0x01, NON_EXISTENT_ENDPOINT_ID >> 8, NON_EXISTENT_ENDPOINT_ID & 0xff, 0x55);
  cl_assert_equal_i(s_prepare_count, 0);
  assert_meta_response_sent(MetaResponseCodeUnhandled);

  // Length: 1, Endpoint ID: OTHER_NON_EXISTENT_ENDPOINT_ID, Payload: 0x55
  RECEIVE(0x00, 0x01,
          OTHER_NON_EXISTENT_ENDPOINT_ID >> 8, OTHER_NON_EXISTENT_ENDPOINT_ID & 0xff, 0x55);
  cl_assert_equal_i(s_prepare_count, 0);
  assert_meta_response_sent(MetaResponseCodeUnhandled);

  // Length: 1, Endpoint ID: PRIVATE_TEST_ENDPOINT_ID, Payload: 0xaa
  RECEIVE(0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff, 0xaa);
  cl_assert_equal_i(s_prepare_count, 1);
}

void test_session_receive_router__unhandled_and_supported_concat(void) {
  // Expect "Unhandled" meta message to be replied to a message for an unknown endpoint,
  // the message should get eaten even if a supported message immediately follows.

  // Length: 1, Endpoint ID: NON_EXISTENT_ENDPOINT_ID, Payload: 0x55
  RECEIVE(0x00, 0x01, NON_EXISTENT_ENDPOINT_ID >> 8, NON_EXISTENT_ENDPOINT_ID & 0xff, 0x55,
          0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff, 0xaa);
  assert_meta_response_sent(MetaResponseCodeUnhandled);
  cl_assert_equal_i(s_prepare_count, 1);
}

void test_session_receive_router__system_disallowed_endpoint(void) {
  // Expect "Disallowed" meta message to be replied to a message for an endpoint that is disallowed
  // by use over a system session.
  // The message should get eaten and not interfere with whatever comes next.

  // Length: 1, Endpoint ID: PUBLIC_TEST_ENDPOINT_ID, Payload: 0x55
  RECEIVE(0x00, 0x01, PUBLIC_TEST_ENDPOINT_ID >> 8, PUBLIC_TEST_ENDPOINT_ID & 0xff, 0x55);
  cl_assert_equal_i(s_prepare_count, 0);
  assert_meta_response_sent(MetaResponseCodeDisallowed);

  // Length: 1, Endpoint ID: PRIVATE_TEST_ENDPOINT_ID, Payload: 0xaa
  RECEIVE(0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff, 0xaa);
  cl_assert_equal_i(s_prepare_count, 1);
}

void test_session_receive_router__app_disallowed_endpoint(void) {
  // Expect "Disallowed" meta message to be replied to a message for an endpoint that is disallowed
  // by use over a app session.
  // The message should get eaten and not interfere with whatever comes next.

  comm_session_close(s_session, CommSessionCloseReason_UnderlyingDisconnection);
  s_session = comm_session_open(s_transport, &s_transport_implementation,
                                TransportDestinationApp);

  // Length: 1, Endpoint ID: PUBLIC_TEST_ENDPOINT_ID, Payload: 0xaa
  RECEIVE(0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff, 0xaa);
  cl_assert_equal_i(s_prepare_count, 0);
  assert_meta_response_sent(MetaResponseCodeDisallowed);

  // Length: 1, Endpoint ID: PUBLIC_TEST_ENDPOINT_ID, Payload: 0x55
  RECEIVE(0x00, 0x01, PUBLIC_TEST_ENDPOINT_ID >> 8, PUBLIC_TEST_ENDPOINT_ID & 0xff, 0x55);
  cl_assert_equal_i(s_prepare_count, 1);
}

void test_session_receive_router__ignore_message_if_no_receiver_could_be_prepared(void) {
  // Expect an inbound message to be skipped/ignored if no Receiver could be prepared.
  // The message should get eaten and not interfere with whatever comes next.

  s_prepare_return_null = true;
  // Length: 1, Endpoint ID: PRIVATE_TEST_ENDPOINT_ID, Payload: 0xaa
  RECEIVE(0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff, 0xaa);
  cl_assert_equal_i(s_prepare_count, 1);
  cl_assert_equal_i(s_finish_count, 0);
  s_prepare_return_null = false;

  // Length: 1, Endpoint ID: PRIVATE_TEST_ENDPOINT_ID, Payload: 0xaa
  RECEIVE(0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff, 0xaa);
  cl_assert_equal_i(s_prepare_count, 2);
  cl_assert_equal_i(s_finish_count, 1);
}

void test_session_receive_router__cleanup_receiver_if_session_is_closed(void) {
  // Expect that when a partial message has been received, but then the session gets closed,
  // that "cleanup" is called.

  // Length: 1, Endpoint ID: PRIVATE_TEST_ENDPOINT_ID, Payload: not received yet
  RECEIVE(0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff);
  cl_assert_equal_i(s_prepare_count, 1);

  comm_session_close(s_session, CommSessionCloseReason_UnderlyingDisconnection);
  s_session = NULL;

  cl_assert_equal_i(s_cleanup_count, 1);
}

void test_session_receive_router__payload_in_pieces(void) {
  // Expect that when a message's payload is received in pieces, the complete payload will be
  // written and finish will only be called after the whole payload has been received.

  const uint8_t expected_payload[] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

  // Length: 5, Endpoint ID: PRIVATE_TEST_ENDPOINT_ID, Payload: 0xaa (byte 1)
  RECEIVE(0x00, sizeof(expected_payload),
          PRIVATE_TEST_ENDPOINT_ID >> 8, PRIVATE_TEST_ENDPOINT_ID & 0xff,
          0xaa);
  cl_assert_equal_i(s_write_length, 1);
  cl_assert_equal_i(s_finish_count, 0);

  // Payload bytes 2, 3 and 4
  RECEIVE(0xbb, 0xcc, 0xdd);
  cl_assert_equal_i(s_write_length, 4);
  cl_assert_equal_i(s_finish_count, 0);

  // Last payload byte
  RECEIVE(0xee,
  // New message partial header: Length: 5, Endpoint ID: PRIVATE_TEST_ENDPOINT_ID
          0x00, 0x01, PRIVATE_TEST_ENDPOINT_ID >> 8);
  cl_assert_equal_i(s_finish_count, 1);
  cl_assert_equal_i(s_write_length, sizeof(expected_payload));
  cl_assert_equal_m(s_write_buffer, expected_payload, sizeof(expected_payload));
}
