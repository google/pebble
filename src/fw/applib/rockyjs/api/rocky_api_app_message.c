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

#include "jerry-api.h"
#include "rocky_api_app_message.h"
#include "rocky_api_errors.h"
#include "rocky_api_global.h"
#include "rocky_api_util.h"

#include "applib/app_logging.h"
#include "applib/app_message/app_message.h"
#include "applib/app_timer.h"
#include "applib/event_service_client.h"
#include "kernel/pbl_malloc.h"
#include "services/common/comm_session/session.h"
#include "system/passert.h"
#include "syscall/syscall.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/string.h"

#define DEBUG_ROCKY_APPMESSAGE 1
#if DEBUG_ROCKY_APPMESSAGE
#define DBG(fmt, ...) PBL_LOG(LOG_LEVEL_DEBUG, fmt, ## __VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif

#define DEBUG_VERBOSE_ROCKY_APPMESSAGE 1
#if DEBUG_VERBOSE_ROCKY_APPMESSAGE
#define DBG_VERBOSE(fmt, ...) PBL_LOG(LOG_LEVEL_DEBUG, fmt, ## __VA_ARGS__)
#else
#define DBG_VERBOSE(fmt, ...)
#endif


#define ROCKY_EVENT_MESSAGE      "message"
#define ROCKY_EVENT_MESSAGE_DATA "data"
#define ROCKY_EVENT_CONNECTED    "postmessageconnected"
#define ROCKY_EVENT_DISCONNECTED "postmessagedisconnected"
#define ROCKY_EVENT_ERROR        "postmessageerror"
#define ROCKY_POSTMESSAGE        "postMessage"

#define GLOBAL_JSON              "JSON"
#define GLOBAL_JSON_STRINGIFY    "stringify"
#define GLOBAL_JSON_PARSE        "parse"

#define CONTROL_MESSAGE_MAX_FAILURES (3)
#define CHUNK_MESSAGE_MAX_FAILURES (3)
#define RETRY_DELAY_MS (1000)
#define SESSION_CLOSED_TIMEOUT_MS (3000)

typedef struct {
  ListNode node;
  uint32_t key;
  size_t length;
  uint8_t data[0];
} MessageNode;

typedef struct OutgoingObject {
  ListNode node;

  //! Working buffer containing the JSON string representation of the object.
  char *data_buffer;

  //! The next offset in bytes, into the JSON string (excluding the PostMessageChunkPayload
  //! header) that the next chunk's payload will start at.
  uint32_t offset_bytes;
} OutgoingObject;

typedef enum {
  OutboxMsgTypeNone,
  OutboxMsgTypeControl,
  OutboxMsgTypeChunk
} OutboxMsgType;

// TODO: PBL-35780 make this part of app_state_get_rocky_runtime_context()
SECTION(".rocky_bss") static struct {
  PostMessageState state;
  EventServiceInfo comm_session_event_info;

  //! NOTE: Negotiated values are only valid if state == PostMessageStateSessionOpen
  uint8_t protocol_version;     //!< Negotiated protocol version being used
  uint16_t tx_chunk_size_bytes; //!< Negotiated outgoing chunk size
  uint16_t rx_chunk_size_bytes; //!< Negotiated incoming chunk size

  struct {
    //! Queue with pending control messages (head == oldest message)
    MessageNode *control_msg_queue;

    //! Queue with objects to send
    OutgoingObject *object_queue;

    //! This timer runs when not in SessionOpen.
    //! Upon timeout, the head of the object queue is error'd out.
    AppTimer *session_closed_object_queue_timer;

    //! Type of message that is currently in the outbox, being sent out.
    OutboxMsgType msg_type;

    //! Number of failures for the current AppMessage.
    int failure_count;

    AppTimer *app_msg_retry_timer;
  } out;

  struct {
    uint8_t *reassembly_buffer;
    size_t received_size_bytes;
    size_t total_size_bytes;
  } in;
} s_state;


////////////////////////////////////////////////////////////////////////////////
// Forward Declarations
////////////////////////////////////////////////////////////////////////////////

static void prv_session_open__enter(const PostMessageResetCompletePayload *rc);
static void prv_awaiting_reset_request__enter(void);
static void prv_awaiting_reset_complete_remote_initiated_enter(void);
static void prv_awaiting_reset_complete_local_initiated__enter(bool should_send_reset_request);
static void prv_send_reset_request(void);
static void prv_session_open__after_exit(void);
static void prv_object_queue_pop_head_and_emit_error_event_and_own_json_buffer(void);
static void prv_start_session_closed_object_queue_timer(void);
static void prv_stop_session_closed_object_queue_timer(void);

static bool prv_is_outbox_busy(void);
static void prv_outbox_try_send_next(void);
T_STATIC jerry_value_t prv_json_parse(const char *object);

////////////////////////////////////////////////////////////////////////////////
// Comm Session Handling
////////////////////////////////////////////////////////////////////////////////

T_STATIC void prv_handle_connection(void) {
  if (s_state.state != PostMessageStateDisconnected) {
    // Handle the race. See comment in prv_init_apis().
    return;
  }
  DBG("Transport Connected");
  prv_awaiting_reset_request__enter();
}

T_STATIC void prv_handle_disconnection(void) {
  if (s_state.state == PostMessageStateDisconnected) {
    // Handle the race. See comment in prv_init_apis().
    return;
  }
  DBG("Transport Disconnected");
  const bool did_exit_session_open = (s_state.state == PostMessageStateSessionOpen);
  s_state.state = PostMessageStateDisconnected;
  if (did_exit_session_open) {
    prv_session_open__after_exit();
  }
}

static void prv_handle_comm_session_event(PebbleEvent *e, void *unused) {
  PebbleCommSessionEvent *pcse = &e->bluetooth.comm_session_event;
  if (!pcse->is_system) { // Need pkjs, which runs inside the Pebble app, so need system session.
    return;
  }
  if (pcse->is_open) { // Connection event
    prv_handle_connection();
  } else { // Disconnect event
    prv_handle_disconnection();
  }
}

////////////////////////////////////////////////////////////////////////////////
// Outbound Object Queue
////////////////////////////////////////////////////////////////////////////////

static void prv_object_queue_send_current_chunk(void);

static void prv_object_queue_pop_head(bool should_free_data_buffer) {
  OutgoingObject *obj = s_state.out.object_queue;
  list_remove((ListNode *)obj, (ListNode **)&s_state.out.object_queue, NULL);

  if (should_free_data_buffer) {
    task_free(obj->data_buffer);
  }
  task_free(obj);
}

static void prv_calc_current_chunk_size(size_t *out_bytes_remaining,
                                         size_t *out_chunk_payload_size) {
  OutgoingObject *obj = s_state.out.object_queue;
  const size_t bytes_remaining = strlen(obj->data_buffer + obj->offset_bytes) + 1;
  *out_bytes_remaining = bytes_remaining;
  *out_chunk_payload_size = MIN(bytes_remaining, s_state.tx_chunk_size_bytes);
}

static void prv_object_queue_handle_chunk_sent(void) {
  DBG("Sent Chunk Successfully.");

  size_t bytes_remaining_before_sent_chunk;
  size_t sent_chunk_payload_size;
  prv_calc_current_chunk_size(&bytes_remaining_before_sent_chunk, &sent_chunk_payload_size);

  const bool is_object_complete = (sent_chunk_payload_size == bytes_remaining_before_sent_chunk);
  if (is_object_complete) {
    DBG("Object Send Complete.");
    prv_object_queue_pop_head(true /* should_free_data_buffer */);
  } else {
    OutgoingObject * const obj = s_state.out.object_queue;
    obj->offset_bytes += sent_chunk_payload_size;
  }
}

static void prv_object_queue_send_current_chunk(void) {
  PBL_ASSERTN(s_state.out.object_queue);

  DictionaryIterator *it = NULL;
  app_message_outbox_begin(&it);
  if (!it) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to outbox_begin");
    return;
  }

  size_t bytes_remaining;
  size_t payload_size;
  prv_calc_current_chunk_size(&bytes_remaining, &payload_size);

  OutgoingObject *obj = s_state.out.object_queue;

  // There is no dict_write_... API that lets us write a Tuple's byte array in multiple calls,
  // so we're just going to poke in the data "manually" here:
  it->dictionary->count = 1;
  const size_t tuple_data_length = (sizeof(PostMessageChunkPayload) + payload_size);
  *it->dictionary->head = (Tuple) {
    .key = PostMessageKeyChunk,
    .type = TUPLE_BYTE_ARRAY,
    .length = tuple_data_length,
  };

  // Write the PostMessageChunkPayload header into obj->data_buffer (overwriting part of the JSON
  // string that has been sent out):
  const bool is_first = (obj->offset_bytes == 0);
  PostMessageChunkPayload *next_chunk =
      (PostMessageChunkPayload *) it->dictionary->head->value[0].data;
  if (is_first) {
    *next_chunk = (PostMessageChunkPayload) {
      .total_size_bytes = bytes_remaining,
      .is_first = true,
    };
  } else {
    *next_chunk = (PostMessageChunkPayload) {
      .offset_bytes = obj->offset_bytes,
      .continuation_is_first = false,
    };
  }
  // Copy the JSON fragment:
  memcpy(next_chunk->chunk_data, &obj->data_buffer[obj->offset_bytes], payload_size);

  // Move the cursor just like a dict_write_data() call would.
  // app_message_outbox_send() is expecting this!
  it->cursor = (Tuple *)((uint8_t *)it->cursor + sizeof(Tuple) + tuple_data_length);

  PBL_ASSERTN(s_state.out.msg_type == OutboxMsgTypeNone);
  s_state.out.msg_type = OutboxMsgTypeChunk;

  DBG("Sending Chunk (%"PRIu32" bytes remaining)", (uint32_t) bytes_remaining);
  PBL_ASSERTN(APP_MSG_OK == app_message_outbox_send());
}

////////////////////////////////////////////////////////////////////////////////
// Handling Inbound Object Chunks
////////////////////////////////////////////////////////////////////////////////

static void prv_cleanup_inbound_reassembly_buffer(void) {
  task_free(s_state.in.reassembly_buffer);
  s_state.in.reassembly_buffer = NULL;
}

static bool prv_handle_chunk_received(Tuple *tuple) {
  if (tuple->type != TUPLE_BYTE_ARRAY) {
    PBL_LOG(LOG_LEVEL_ERROR, "Chunk tuple not a byte array!");
    return false;
  }

  const PostMessageChunkPayload *const chunk = (const PostMessageChunkPayload *) tuple->value;
  if (tuple->length <= sizeof(PostMessageChunkPayload)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Chunk tuple too short to be valid!");
    return false;
  }
  const size_t payload_size = (tuple->length - sizeof(PostMessageChunkPayload));

  const bool is_expecting_first = (s_state.in.reassembly_buffer == NULL);
  if (chunk->is_first != is_expecting_first) {
    PBL_LOG(LOG_LEVEL_ERROR, "Chunk reassembly out of sync! is_first=%u, is_expecting_first=%u",
            chunk->is_first, is_expecting_first);
    return false;
  }

  if (chunk->is_first) {
    // If this is the first message, allocate buffer:
    uint8_t *const buffer = task_malloc(chunk->total_size_bytes);
    if (!buffer) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Not enough mem to recv postMessage() of %"PRIu32" bytes",
              (uint32_t) chunk->total_size_bytes);
      // https://pebbletechnology.atlassian.net/browse/PBL-42466
      // TODO: AppMessage NACK the message so the other side can retry later. Not doing this will
      // derail the protocol and thus cause a reset of the session.
      return true;  // false would close the session!
    }
    s_state.in.reassembly_buffer = buffer;
    s_state.in.total_size_bytes = chunk->total_size_bytes;
    s_state.in.received_size_bytes = 0;
  } else {
    // If this is not the first message, sanity check the chunk:
    if (s_state.in.received_size_bytes != chunk->offset_bytes) {
      PBL_LOG(LOG_LEVEL_ERROR,
              "Chunk reassembly out of sync! received_size_bytes=%"PRIu32", offset_bytes=%"PRIu32,
              (uint32_t) s_state.in.received_size_bytes, (uint32_t) chunk->offset_bytes);
      return false;
    }
    if (s_state.in.received_size_bytes + payload_size > s_state.in.total_size_bytes) {
      PBL_LOG(LOG_LEVEL_ERROR,
              "Chunk reassembly out of sync! recv_size=%"PRIu32", payload_size=%"PRIu32
              ", total_size=%"PRIu32,
              (uint32_t) s_state.in.received_size_bytes, (uint32_t) payload_size,
              (uint32_t) s_state.in.total_size_bytes);
      return false;
    }
  }

  // Copy the received buffer over:
  memcpy(s_state.in.reassembly_buffer + s_state.in.received_size_bytes,
         chunk->chunk_data, payload_size);
  s_state.in.received_size_bytes += payload_size;

  DBG("Received (%"PRIu32" / %"PRIu32" bytes)",
      (uint32_t) s_state.in.received_size_bytes, (uint32_t) s_state.in.total_size_bytes);
  DBG("Payload Size: %"PRIu32, (uint32_t) payload_size);

  const bool is_last_chunk = (s_state.in.received_size_bytes == s_state.in.total_size_bytes);
  if (is_last_chunk) {
    if (rocky_global_has_event_handlers(ROCKY_EVENT_MESSAGE)) {
      // Last chunk MUST be zero terminated:
      if (s_state.in.reassembly_buffer[s_state.in.total_size_bytes - 1] != '\0') {
        PBL_LOG(LOG_LEVEL_ERROR, "Last Chunk MUST be zero-terminated! Dropping msg.");
      } else {
        // Try to parse the received JSON string:
        JS_VAR object = prv_json_parse((const char *)s_state.in.reassembly_buffer);
        if (jerry_value_has_error_flag(object)) {
          rocky_error_print(object);
        } else {
          // Call the app's "message" handler:
          JS_VAR event = rocky_global_create_event(ROCKY_EVENT_MESSAGE);
          jerry_set_object_field(event, ROCKY_EVENT_MESSAGE_DATA, object);
          rocky_global_call_event_handlers(event);
        }
      }
    } else {
      DBG("No 'message' event handlers");
    }

    prv_cleanup_inbound_reassembly_buffer();
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
// Control Message Queue
////////////////////////////////////////////////////////////////////////////////

static void prv_control_message_queue_pop_head(void) {
  MessageNode *old_head = s_state.out.control_msg_queue;
  list_remove(&old_head->node, (ListNode **)&s_state.out.control_msg_queue, NULL);
  task_free(old_head);
}

static void prv_control_message_queue_send_head(void) {
  MessageNode *node = s_state.out.control_msg_queue;
  DictionaryIterator *it = NULL;
  app_message_outbox_begin(&it);
  if (!it) {
    // FIXME: Handle not being able to open inbox ??
    WTF;
  }
  dict_write_data(it, node->key, node->data, node->length);
  dict_write_end(it);

  PBL_ASSERTN(s_state.out.msg_type == OutboxMsgTypeNone);
  s_state.out.msg_type = OutboxMsgTypeControl;

  PBL_ASSERTN(APP_MSG_OK == app_message_outbox_send());
}

static void prv_control_message_queue_add(uint32_t key, const void *data, const size_t length) {
  MessageNode *node = (MessageNode *) task_zalloc_check(sizeof(MessageNode) + length);
  node->key = key;
  node->length = length;
  memcpy(node->data, data, length);
  if (s_state.out.control_msg_queue) {
    list_append((ListNode *)s_state.out.control_msg_queue, (ListNode *)node);
  } else {
    s_state.out.control_msg_queue = node;
  }

  prv_outbox_try_send_next();
}

////////////////////////////////////////////////////////////////////////////////
// Generic outbox handlers
////////////////////////////////////////////////////////////////////////////////

static bool prv_is_outbox_busy(void) {
  return (s_state.out.msg_type != OutboxMsgTypeNone ||
          s_state.out.app_msg_retry_timer != EVENTED_TIMER_INVALID_ID);
}

static void prv_outbox_try_send_next(void) {
  if (prv_is_outbox_busy()) {
    return;
  }

  // Send out the next message. Prioritize control messages over chunk messages:
  if (s_state.out.control_msg_queue) {
    prv_control_message_queue_send_head();
  } else if ((s_state.state == PostMessageStateSessionOpen) &&
             s_state.out.object_queue) {
    prv_object_queue_send_current_chunk();
  }
}

static void prv_outbox_try_send_next_timer_cb(void *unused) {
  s_state.out.app_msg_retry_timer = EVENTED_TIMER_INVALID_ID;
  prv_outbox_try_send_next();
}

static void prv_handle_outbox_result(AppMessageResult reason) {
  // https://pebbletechnology.atlassian.net/browse/PBL-42467
  // TODO: check reason and act upon it

  const OutboxMsgType sent_msg_type = s_state.out.msg_type;
  s_state.out.msg_type = OutboxMsgTypeNone;

  const bool is_sent_successfully = (reason == APP_MSG_OK);

  // Process the (N)ACK:
  switch (sent_msg_type) {
    case OutboxMsgTypeControl: {
      if (is_sent_successfully) {
        prv_control_message_queue_pop_head();
      } else {
        if (s_state.out.failure_count >= CONTROL_MESSAGE_MAX_FAILURES) {
          MessageNode *node = s_state.out.control_msg_queue;
          PBL_LOG(LOG_LEVEL_ERROR, "Failed to send msg with key %"PRIu32, node ? node->key : 0);
          prv_control_message_queue_pop_head();
        } else {
          // Retry happens below by calling prv_control_message_queue_send_head()
        }
      }
      break;
    }

    case OutboxMsgTypeChunk: {
      if (is_sent_successfully) {
        prv_object_queue_handle_chunk_sent();
      } else {
        if (s_state.out.failure_count >= CHUNK_MESSAGE_MAX_FAILURES) {
          APP_LOG(APP_LOG_LEVEL_WARNING, "Dropping Message.");
          prv_object_queue_pop_head_and_emit_error_event_and_own_json_buffer();
        } else {
          // Retry happens below by calling prv_object_queue_send_current_chunk()
        }
      }
      break;
    }

    case OutboxMsgTypeNone: {
      PBL_LOG(LOG_LEVEL_WARNING, "Got (N)ACK while not expecting any. %u", reason);
      break;
    }

    default:
      WTF;
      break;
  }

  // https://pebbletechnology.atlassian.net/browse/PBL-42468
  // Send next, or in case of error reason, delay the retry instead of sending immediately:
  if (is_sent_successfully) {
    prv_outbox_try_send_next();
  } else {
    PBL_ASSERTN(s_state.out.app_msg_retry_timer == EVENTED_TIMER_INVALID_ID);
    s_state.out.app_msg_retry_timer =
        app_timer_register(RETRY_DELAY_MS, prv_outbox_try_send_next_timer_cb, NULL);
  }
}

static void prv_handle_outbox_sent(DictionaryIterator *it, void *context) {
  s_state.out.failure_count = 0;
  prv_handle_outbox_result(APP_MSG_OK);
}

static void prv_handle_outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Failed to send message: Reason %d", reason);
  ++s_state.out.failure_count;
  prv_handle_outbox_result(reason);
}

////////////////////////////////////////////////////////////////////////////////
// Unsupported Protocol
////////////////////////////////////////////////////////////////////////////////

static bool prv_is_version_supported(const PostMessageResetCompletePayload *rc) {
  const bool is_unsupported = (rc->min_supported_version > POSTMESSAGE_PROTOCOL_MAX_VERSION ||
                               rc->max_supported_version < POSTMESSAGE_PROTOCOL_MIN_VERSION);
  if (is_unsupported) {
    // We don't support any of the same versions
    PBL_LOG(LOG_LEVEL_ERROR, "Protocol version unsupported! min=%"PRIu8", max=%"PRIu8,
            rc->min_supported_version, rc->max_supported_version);
    return false;
  }
  return true;
}

static void prv_send_unsupported_protocol_error_and_enter_await_reset_req(PostMessageError error) {
  PBL_ASSERTN(s_state.state == PostMessageStateAwaitingResetCompleteRemoteInitiated ||
              s_state.state == PostMessageStateAwaitingResetCompleteLocalInitiated);

  const PostMessageUnsupportedErrorPayload error_payload = {
    .error_code = error,
  };
  prv_control_message_queue_add(PostMessageKeyUnsupportedError,
                                &error_payload, sizeof(error_payload));

  prv_awaiting_reset_request__enter();
}

////////////////////////////////////////////////////////////////////////////////
// Session Open
////////////////////////////////////////////////////////////////////////////////

static void prv_emit_post_message_connection_event(bool is_connected) {
  const char *event_type = is_connected ? ROCKY_EVENT_CONNECTED : ROCKY_EVENT_DISCONNECTED;
  if (rocky_global_has_event_handlers(event_type)) {
    JS_VAR event = rocky_global_create_event(event_type);
    rocky_global_call_event_handlers(event);
  } else {
    DBG("No handler registered for %s", event_type);
  }
}

static void prv_session_open__enter(const PostMessageResetCompletePayload *rc) {
  PBL_ASSERTN(s_state.state == PostMessageStateAwaitingResetCompleteRemoteInitiated ||
              s_state.state == PostMessageStateAwaitingResetCompleteLocalInitiated);

  s_state.protocol_version = MIN(rc->max_supported_version, POSTMESSAGE_PROTOCOL_MAX_VERSION);
  // NOTE: Each end communicates it's OWN TX/RX max values.
  //       This means that TX max on one end is bound by RX max on the other, and vice versa.
  s_state.tx_chunk_size_bytes = MIN(rc->max_rx_chunk_size, POSTMESSAGE_PROTOCOL_MAX_TX_CHUNK_SIZE);
  s_state.rx_chunk_size_bytes = MIN(rc->max_tx_chunk_size, POSTMESSAGE_PROTOCOL_MAX_RX_CHUNK_SIZE);

  s_state.state = PostMessageStateSessionOpen;

  prv_stop_session_closed_object_queue_timer();

  prv_emit_post_message_connection_event(true /* is_connected */);

  // Kick the object queue upon entering SessionOpen:
  prv_outbox_try_send_next();

  DBG("SessionOpen enter");
}

static void prv_session_open__after_exit(void) {
  DBG("After SessionOpen exit");
  prv_cleanup_inbound_reassembly_buffer();

  if (s_state.out.object_queue) {
    // Make sure we'll re-transfer the object from the start when re-opening:
    OutgoingObject *head = s_state.out.object_queue;
    head->offset_bytes = 0;
    prv_start_session_closed_object_queue_timer();
  }

  PBL_ASSERTN(s_state.state != PostMessageStateSessionOpen);
  prv_emit_post_message_connection_event(false /* is_connected */);
}

static void prv_session_open__exit_and_initiate_reset(void) {
  prv_awaiting_reset_complete_local_initiated__enter(true /* should_send_reset_request */);
  prv_session_open__after_exit();
}

static void prv_session_open__inbox_received(DictionaryIterator *it, void *context) {
  Tuple *tuple = dict_find(it, PostMessageKeyChunk);
  if (tuple) {
    if (!prv_handle_chunk_received(tuple)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Resetting because bad Chunk!");
      prv_session_open__exit_and_initiate_reset();
    }
  } else if (dict_find(it, PostMessageKeyResetRequest)) {
    prv_awaiting_reset_complete_remote_initiated_enter();
    prv_session_open__after_exit();
  } else if (dict_find(it, PostMessageKeyResetComplete)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Resetting because got RC while open");
    prv_session_open__exit_and_initiate_reset();
  }
}

////////////////////////////////////////////////////////////////////////////////
// Awaiting Reset Complete Local Initiated
////////////////////////////////////////////////////////////////////////////////

static void prv_send_reset_request(void) {
  prv_control_message_queue_add(PostMessageKeyResetRequest, NULL, 0);
}

static void prv_awaiting_reset_complete_local_initiated__enter(bool should_send_reset_request) {
  PBL_ASSERTN(s_state.state == PostMessageStateAwaitingResetRequest ||
              s_state.state == PostMessageStateAwaitingResetCompleteRemoteInitiated ||
              s_state.state == PostMessageStateSessionOpen);
  if (should_send_reset_request) {
    prv_send_reset_request();
  }
  s_state.state = PostMessageStateAwaitingResetCompleteLocalInitiated;
}

////////////////////////////////////////////////////////////////////////////////
// Awaiting Reset Complete Remote Initiated
////////////////////////////////////////////////////////////////////////////////

static void prv_send_reset_complete(void) {
  const PostMessageResetCompletePayload payload = {
    .min_supported_version = POSTMESSAGE_PROTOCOL_MIN_VERSION,
    .max_supported_version = POSTMESSAGE_PROTOCOL_MAX_VERSION,
    .max_tx_chunk_size = POSTMESSAGE_PROTOCOL_MAX_TX_CHUNK_SIZE,
    .max_rx_chunk_size = POSTMESSAGE_PROTOCOL_MAX_RX_CHUNK_SIZE,
  };
  prv_control_message_queue_add(PostMessageKeyResetComplete, &payload, sizeof(payload));
}

static void prv_awaiting_reset_complete_remote_initiated_enter(void) {
  PBL_ASSERTN(s_state.state == PostMessageStateAwaitingResetRequest ||
              s_state.state == PostMessageStateAwaitingResetCompleteLocalInitiated ||
              s_state.state == PostMessageStateSessionOpen);
  s_state.state = PostMessageStateAwaitingResetCompleteRemoteInitiated;
  prv_send_reset_complete();
}

static bool prv_is_tuple_valid_reset_complete(Tuple *reset_complete) {
  if (reset_complete->type != TUPLE_BYTE_ARRAY) {
    PBL_LOG(LOG_LEVEL_ERROR, "ResetComplete not a byte array! %u", reset_complete->type);
    return false;
  }
  if (reset_complete->length >= sizeof(PostMessageResetCompletePayload)) {
    return true;
  }
  PBL_LOG(LOG_LEVEL_ERROR, "ResetComplete too small! %"PRIu16, reset_complete->length);
  return false;
}

static void prv_awaiting_reset_complete_remote_initiated__inbox_received(DictionaryIterator *it,
                                                                         void *context) {
  Tuple *tuple = dict_find(it, PostMessageKeyResetComplete);
  if (tuple) {
    if (!prv_is_tuple_valid_reset_complete(tuple)) {
      // TODO: document this in statechart
      prv_send_unsupported_protocol_error_and_enter_await_reset_req(PostMessageErrorMalformedResetComplete);
      return;
    }
    const PostMessageResetCompletePayload *rc =
        (const PostMessageResetCompletePayload *) tuple->value[0].data;
    // Check overlap in supported versions
    if (!prv_is_version_supported(rc)) {
      // Don't send an error here! The initiating side is supposed have detected the version
      // incompatibility and not sent the ResetComplete (and send an Error message), but apparently
      // we did get the ResetComplete somehow?
      prv_awaiting_reset_request__enter();
      return;
    }
    prv_session_open__enter(rc);
  } else if (dict_find(it, PostMessageKeyResetRequest)) {
    prv_send_reset_complete();
  } else {
    // Anything else (i.e. Chunk), initiate Reset:
    prv_awaiting_reset_complete_local_initiated__enter(true /* should_send_reset_request */);
  }
}

////////////////////////////////////////////////////////////////////////////////
// Awaiting Reset Complete Local Initiated
////////////////////////////////////////////////////////////////////////////////


static void prv_awaiting_reset_complete_local_initiated__inbox_received(DictionaryIterator *it,
                                                                        void *context) {
  Tuple *tuple = dict_find(it, PostMessageKeyResetComplete);
  if (tuple) {
    if (!prv_is_tuple_valid_reset_complete(tuple)) {
      // TODO: document this in statechart
      prv_send_unsupported_protocol_error_and_enter_await_reset_req(PostMessageErrorMalformedResetComplete);
      return;
    }
    const PostMessageResetCompletePayload *rc =
        (const PostMessageResetCompletePayload *) tuple->value[0].data;
    // Check overlap in supported versions
    if (!prv_is_version_supported(rc)) {
      prv_send_unsupported_protocol_error_and_enter_await_reset_req(PostMessageErrorUnsupportedVersion);
      return;
    }
    prv_send_reset_complete();
    prv_session_open__enter(rc);
  } else if (dict_find(it, PostMessageKeyResetRequest)) {
    prv_awaiting_reset_complete_remote_initiated_enter();
  } else if (dict_find(it, PostMessageKeyChunk)) {
    // Ignore it.
    // https://pebbletechnology.atlassian.net/browse/PBL-42466
    // TODO: NACK the Chunk.
  }
}

////////////////////////////////////////////////////////////////////////////////
// Awaiting Reset Request
////////////////////////////////////////////////////////////////////////////////

static void prv_awaiting_reset_request__enter(void) {
  PBL_ASSERTN(s_state.state == PostMessageStateDisconnected ||
              s_state.state == PostMessageStateAwaitingResetCompleteLocalInitiated ||
              s_state.state == PostMessageStateAwaitingResetCompleteRemoteInitiated);

  s_state.state = PostMessageStateAwaitingResetRequest;
}

static void prv_awaiting_reset_request__inbox_received(DictionaryIterator *it, void *context) {
  if (dict_find(it, PostMessageKeyResetRequest)) {
    prv_awaiting_reset_complete_remote_initiated_enter();
  } else {
    // This is not a request message. Drop it and initiate a request
    // https://pebbletechnology.atlassian.net/browse/PBL-42466
    // TODO: This should indicate to the AppMessage layer that it should NACK.
    prv_awaiting_reset_complete_local_initiated__enter(true /* should_send_reset_request */);
  }
}

////////////////////////////////////////////////////////////////////////////////
// App Message Handling
////////////////////////////////////////////////////////////////////////////////

static const struct {
  AppMessageInboxReceived inbox_received;
} s_app_message_handlers[] = {
  [PostMessageStateDisconnected] = {
    .inbox_received = NULL,
  },
  [PostMessageStateAwaitingResetRequest] = {
    .inbox_received = prv_awaiting_reset_request__inbox_received,
  },
  [PostMessageStateAwaitingResetCompleteRemoteInitiated] = {
    .inbox_received = prv_awaiting_reset_complete_remote_initiated__inbox_received,
  },
  [PostMessageStateAwaitingResetCompleteLocalInitiated] = {
    .inbox_received = prv_awaiting_reset_complete_local_initiated__inbox_received,
  },
  [PostMessageStateSessionOpen] = {
    .inbox_received = prv_session_open__inbox_received,
  },
};

static void prv_inbox_received(DictionaryIterator *it, void *context) {
  const AppMessageInboxReceived inbox_rcv = s_app_message_handlers[s_state.state].inbox_received;
  if (inbox_rcv) {
    inbox_rcv(it, context);
  } else {
    DBG_VERBOSE("No inbox_received handler for state %d", s_state.state);
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  // Q: We don't know what got dropped here. Should we send/initiate a ResetRequest?
  // A: No, a drop will be a NACK to the other side, so the other side should retry.
  PBL_LOG(LOG_LEVEL_WARNING, "inbox dropped msg in state %u because %u", s_state.state, reason);
}


////////////////////////////////////////////////////////////////////////////////
// Object (de)serialization and (de)chunking
////////////////////////////////////////////////////////////////////////////////

// Call the JSON.<function_name> function with the given args, and return the result.
// Returned jerry_value_t must be released after use.
static jerry_value_t prv_call_json_function(const char *function_name,
                                            const jerry_value_t * const args,
                                            jerry_size_t args_count) {
  JS_VAR JSON = jerry_get_global_builtin((const jerry_char_t *)GLOBAL_JSON);
  PBL_ASSERTN(!jerry_value_is_undefined(JSON));

  JS_VAR func = jerry_get_object_field(JSON, function_name);
  PBL_ASSERTN(jerry_value_is_function(func));

  return jerry_call_function(func, JSON, args, args_count);
}

T_STATIC jerry_value_t prv_json_stringify(jerry_value_t object) {
  return prv_call_json_function(GLOBAL_JSON_STRINGIFY, &object, 1);
}

T_STATIC jerry_value_t prv_json_parse(const char *object) {
  JS_VAR string_obj = jerry_create_string_utf8((const jerry_char_t *)object);
  return prv_call_json_function(GLOBAL_JSON_PARSE, &string_obj, 1);
}

#define BOX_SIZE (APP_MESSAGE_OUTBOX_SIZE_MINIMUM)
#define postMessage_HEADER_SIZE_BYTES (2 * sizeof(Tuple))

////////////////////////////////////////////////////////////////////////////////
// API: "postmessageerror" event
////////////////////////////////////////////////////////////////////////////////

static void prv_free_json_buffer_associated_with_postmessageerror_event(const uintptr_t ptr) {
  task_free((char *)ptr);
}

JERRY_FUNCTION(prv_postmessageerror_data_getter) {
  char *json_buffer = NULL;
  PBL_ASSERTN(jerry_get_object_native_handle(this_val, (uintptr_t *)&json_buffer));
  PBL_ASSERTN(json_buffer);
  return prv_json_parse(json_buffer);
}

static void prv_object_queue_pop_head_and_emit_error_event_and_own_json_buffer(void) {
  DBG("postmessageerror event");
  OutgoingObject *old_head = s_state.out.object_queue;
  char *const json_data_buffer = old_head->data_buffer;

  // Don't free the JSON data buffer, ownership is about to be passed to the error event.
  prv_object_queue_pop_head(false /* should_free_data_buffer */);

  JS_VAR event = rocky_global_create_event(ROCKY_EVENT_ERROR);
  jerry_set_object_native_handle(event, (uintptr_t)json_data_buffer,
                                 prv_free_json_buffer_associated_with_postmessageerror_event);
  rocky_define_property(event, ROCKY_EVENT_MESSAGE_DATA, prv_postmessageerror_data_getter, NULL);
  rocky_global_call_event_handlers(event);
}

////////////////////////////////////////////////////////////////////////////////
// API: postMessage()
////////////////////////////////////////////////////////////////////////////////

static void prv_start_session_closed_object_queue_timer(void);

static void prv_stop_session_closed_object_queue_timer(void) {
  if (EVENTED_TIMER_INVALID_ID != s_state.out.session_closed_object_queue_timer) {
    AppTimer *t = s_state.out.session_closed_object_queue_timer;
    s_state.out.session_closed_object_queue_timer = EVENTED_TIMER_INVALID_ID;
    app_timer_cancel(t);
    DBG("Cancelled 3s timeout");
  }
}

static void prv_session_closed_object_queue_timer_cb(void *unused) {
  if (EVENTED_TIMER_INVALID_ID == s_state.out.session_closed_object_queue_timer) {
    // handle race: timer was cancelled but event was already in the queue
    // Unfortunately, app_timer_cancel() doesn't tell us about this.
    return;
  }

  PBL_ASSERTN(s_state.state != PostMessageStateSessionOpen);

  DBG("Erroring out head object, 3s passed!");

  s_state.out.session_closed_object_queue_timer = EVENTED_TIMER_INVALID_ID;
  prv_object_queue_pop_head_and_emit_error_event_and_own_json_buffer();

  if (s_state.out.object_queue) {
    // Still not open and still things in the object queue, restart the timer:
    prv_start_session_closed_object_queue_timer();
  }
}

static void prv_start_session_closed_object_queue_timer(void) {
  PBL_ASSERTN(EVENTED_TIMER_INVALID_ID == s_state.out.session_closed_object_queue_timer);

  DBG("Starting 3s timeout...");

  s_state.out.session_closed_object_queue_timer =
      app_timer_register(SESSION_CLOSED_TIMEOUT_MS, prv_session_closed_object_queue_timer_cb, NULL);
}

static jerry_value_t prv_create_oom_error(void) {
  return rocky_error_oom("can't postMessage() -- object too large");
}

static bool prv_object_queue_add(OutgoingObject *msg) {
  if (s_state.out.object_queue) {
    list_append((ListNode *)s_state.out.object_queue, (ListNode *)msg);
    return false;
  } else {
    s_state.out.object_queue = msg;
    return true;
  }
}

JERRY_FUNCTION(prv_post_message) {
  if (argc < 1) {
    return rocky_error_arguments_missing();
  }

  const jerry_value_t js_msg = argv[0];

  JS_VAR json_string = prv_json_stringify(js_msg);
  if (jerry_value_has_error_flag(json_string)) {
    return jerry_acquire_value(json_string);
  }
  if (jerry_value_is_undefined(json_string)) {
    // ECMA v5.1, 15.12.3, Note 5: Values that do not have a JSON representation (such as undefined
    // and functions) do not produce a String. Instead they produce the undefined value.
    return rocky_error_unexpected_type(0, "JSON.stringify()-able object");
  }

  const uint32_t str_size = jerry_get_utf8_string_size(json_string) + 1 /* trailing zero */;
  char *const data_buffer = task_zalloc(str_size);
  if (!data_buffer) {
    return prv_create_oom_error();
  }
  jerry_string_to_utf8_char_buffer(json_string, (jerry_char_t *)data_buffer, str_size);

  OutgoingObject * const obj = task_zalloc(sizeof(*obj));
  if (!obj) {
    task_free(data_buffer);
    return prv_create_oom_error();
  }
  *obj = (OutgoingObject) {
    .data_buffer = data_buffer,
  };

  const bool is_first = prv_object_queue_add(obj);
  if (is_first) {
    if (s_state.state == PostMessageStateSessionOpen) {
      prv_outbox_try_send_next();
    } else {
      prv_start_session_closed_object_queue_timer();
    }
  }

  return jerry_create_undefined();
}

////////////////////////////////////////////////////////////////////////////////
// Rocky boilerplate
////////////////////////////////////////////////////////////////////////////////

static void prv_init_apis(void) {
  memset(&s_state, 0, sizeof(s_state));

  // Pebble comm session events to transition in & out of PostMessageStateDisconnected
  s_state.comm_session_event_info = (EventServiceInfo) {
    .type = PEBBLE_COMM_SESSION_EVENT,
    .handler = prv_handle_comm_session_event,
  };
  event_service_client_subscribe(&s_state.comm_session_event_info);
  if (sys_app_pp_get_comm_session()) {
    // There is a _small_ race here if a connection occurs async on
    // the BT thread after subscribing but before we check the current state.
    // This could result in us transitioning to a connected state, and then
    // getting an event to transition again.
    // We're guarding against this by ignoring the "duplicate" state change.
    prv_handle_connection();
  }

  // FIXME: this call can fail if there's not enough memory!
  // Probably best fix this by doing https://pebbletechnology.atlassian.net/browse/PBL-42250
  const size_t overhead = (sizeof(Dictionary) + sizeof(Tuple) + sizeof(PostMessageChunkPayload));
  app_message_open(overhead + POSTMESSAGE_PROTOCOL_MAX_TX_CHUNK_SIZE,
                   overhead + POSTMESSAGE_PROTOCOL_MAX_RX_CHUNK_SIZE);

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_sent(prv_handle_outbox_sent);
  app_message_register_outbox_failed(prv_handle_outbox_failed);

  JS_VAR rocky = rocky_get_rocky_singleton();
  rocky_add_function(rocky, ROCKY_POSTMESSAGE, prv_post_message);
}

static bool prv_free_control_msg_for_each_cb(ListNode *node, void *context) {
  task_free(node);
  return true;
}

static bool prv_free_outbound_object_for_each_cb(ListNode *node, void *context) {
  OutgoingObject *object = (OutgoingObject *)node;
  task_free(object->data_buffer);
  task_free(object);
  return true;
}

static void prv_deinit_apis(void) {
  event_service_client_unsubscribe(&s_state.comm_session_event_info);

  list_foreach((ListNode *)s_state.out.control_msg_queue, prv_free_control_msg_for_each_cb, NULL);
  list_foreach((ListNode *)s_state.out.object_queue, prv_free_outbound_object_for_each_cb, NULL);
}

//! Unfortunately, we can't use the same path as when normally calling these
//! handlers since we haven't added it to the event listeners list yet.
static void prv_call_handler_when_registering(const char *event_name, jerry_value_t handler) {
  JS_VAR event = rocky_global_create_event(event_name);
  rocky_util_call_user_function_and_log_uncaught_error(handler, jerry_create_undefined(),
                                                       &event, 1);
}

static bool prv_handle_callback_registration(const char *event_name, jerry_value_t handler) {
  const bool is_connected = (s_state.state == PostMessageStateSessionOpen);
  bool call_handler = false;
  if (strcmp(ROCKY_EVENT_CONNECTED, event_name) == 0) {
    call_handler = is_connected;
  } else if (strcmp(ROCKY_EVENT_DISCONNECTED, event_name) == 0) {
    call_handler = !is_connected;
  } else {
    return false;
  }

  if (call_handler) {
    prv_call_handler_when_registering(event_name, handler);
  }

  return true;
}

static bool prv_add_handler(const char *event_name, jerry_value_t handler) {
  if (strcmp(ROCKY_EVENT_MESSAGE, event_name) == 0 || strcmp(ROCKY_EVENT_ERROR, event_name) == 0) {
    return true;
  }

  return prv_handle_callback_registration(event_name, handler);
}

const RockyGlobalAPI APP_MESSAGE_APIS = {
  .init = prv_init_apis,
  .deinit = prv_deinit_apis,
  .add_handler = prv_add_handler,
};


////////////////////////////////////////////////////////////////////////////////
// Unit Test Helpers
////////////////////////////////////////////////////////////////////////////////

PostMessageState rocky_api_app_message_get_state(void) {
  return s_state.state;
}

AppTimer *rocky_api_app_message_get_app_msg_retry_timer(void) {
  return s_state.out.app_msg_retry_timer;
}

AppTimer *rocky_api_app_message_get_session_closed_object_queue_timer(void) {
  return s_state.out.session_closed_object_queue_timer;
}
