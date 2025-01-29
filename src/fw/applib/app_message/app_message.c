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

#include "applib/app_message/app_message.h"
#include "applib/app_message/app_message_internal.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "services/common/comm_session/protocol.h"
#include "syscall/syscall.h"
#include "system/logging.h"

// -------- Initialization ---------------------------------------------------------------------- //

void app_message_init(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  *app_message_ctx = (const AppMessageCtx) {};
}

// -------- Pebble Protocol Handlers ------------------------------------------------------------ //

static bool prv_has_invalid_header_length(size_t length) {
  if (length < sizeof(AppMessageHeader)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Too short");
    return true;
  }
  return false;
}

//! The new implementation uses up to 72 bytes more stack space than the previous implementation.
//! Might need to do some extra work to get a "thinner" stack, if this causes issues.
//! Executes on App task.
void app_message_app_protocol_msg_callback(CommSession *session,
                                           const uint8_t* data, size_t length,
                                           AppInboxConsumerInfo *consumer_info) {
  if (prv_has_invalid_header_length(length)) {
    return;
  }

  AppMessageHeader *message = (AppMessageHeader *) data;
  switch (message->command) {

    case CMD_PUSH:
      // Incoming message:
      app_message_inbox_receive(session, (AppMessagePush *) message, length, consumer_info);
      return;

    case CMD_REQUEST:
      // Incoming request for an update push:
      // TODO PBL-1636: decide to implement CMD_REQUEST, or remove it
      return;

    case CMD_ACK:
    case CMD_NACK:
      // Received ACK/NACK in response to previously pushed update:
      app_message_out_handle_ack_nack_received(message);
      return;

    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Unknown Cmd 0x%x", message->command);
      return;
  }
}

//! Executes on KernelBG, sends back NACK on behalf of the app if it is not able to do so.
//! Note that app_message_receiver_dropped_handler will also get called on the App task,
//! to report the number of missed messages.
void app_message_app_protocol_system_nack_callback(CommSession *session,
                                                   const uint8_t* data, size_t length) {
  if (prv_has_invalid_header_length(length)) {
    return;
  }
  AppMessageHeader *message = (AppMessageHeader *) data;
  if (message->command != CMD_PUSH) {
    return;
  }
  app_message_inbox_send_ack_nack_reply(session, message->transaction_id, CMD_NACK);
}

// -------- Developer Interface ----------------------------------------------------------------- //

void *app_message_get_context(void) {
  return app_state_get_app_message_ctx()->inbox.user_context;
}

void *app_message_set_context(void *context) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  void *retval = app_message_ctx->inbox.user_context;
  app_message_ctx->inbox.user_context = context;
  app_message_ctx->outbox.user_context = context;
  return retval;
}

AppMessageInboxReceived app_message_register_inbox_received(
    AppMessageInboxReceived received_callback) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  AppMessageInboxReceived retval = app_message_ctx->inbox.received_callback;
  app_message_ctx->inbox.received_callback = received_callback;
  return retval;
}

AppMessageInboxDropped app_message_register_inbox_dropped(AppMessageInboxDropped dropped_callback) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  AppMessageInboxDropped retval = app_message_ctx->inbox.dropped_callback;
  app_message_ctx->inbox.dropped_callback = dropped_callback;
  return retval;
}

AppMessageOutboxSent app_message_register_outbox_sent(AppMessageOutboxSent sent_callback) {
  AppMessageOutboxSent retval = app_state_get_app_message_ctx()->outbox.sent_callback;
  app_state_get_app_message_ctx()->outbox.sent_callback = sent_callback;
  return retval;
}

AppMessageOutboxFailed app_message_register_outbox_failed(AppMessageOutboxFailed failed_callback) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  AppMessageOutboxFailed retval = app_message_ctx->outbox.failed_callback;
  app_message_ctx->outbox.failed_callback = failed_callback;
  return retval;
}

void app_message_deregister_callbacks(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  app_message_ctx->inbox.received_callback = NULL;
  app_message_ctx->inbox.dropped_callback = NULL;
  app_message_ctx->inbox.user_context = NULL;
  app_message_ctx->outbox.sent_callback = NULL;
  app_message_ctx->outbox.failed_callback = NULL;
  app_message_ctx->outbox.user_context = NULL;
}

static bool prv_supports_8k(void) {
  if (!sys_app_pp_has_capability(CommSessionAppMessage8kSupport)) {
    return false;
  }
  const Version app_sdk_version = sys_get_current_app_sdk_version();
  const Version sdk_version_8k_messages_enabled = (const Version) { 0x05, 0x3f };
  return (version_compare(sdk_version_8k_messages_enabled, app_sdk_version) <= 0);
}

uint32_t app_message_inbox_size_maximum(void) {
  if (prv_supports_8k()) {
    // New behavior, allow up to one large 8K byte array per message:
    return (APP_MSG_8K_DICT_SIZE);
  } else {
    // Legacy behavior:
    if (sys_get_current_app_is_js_allowed()) {
      return (COMM_PRIVATE_MAX_INBOUND_PAYLOAD_SIZE - APP_MSG_HDR_OVRHD_SIZE);
    } else {
      return (COMM_PUBLIC_MAX_INBOUND_PAYLOAD_SIZE - APP_MSG_HDR_OVRHD_SIZE);
    }
  }
}

uint32_t app_message_outbox_size_maximum(void) {
  if (prv_supports_8k()) {
    return (APP_MSG_8K_DICT_SIZE);
  } else {
    // Legacy behavior:
    return (APP_MESSAGE_OUTBOX_SIZE_MINIMUM + APP_MSG_HDR_OVRHD_SIZE);
  }
}

AppMessageResult app_message_open(const uint32_t size_inbound, const uint32_t size_outbound) {
  // We're making this assumption in this file; here's as good a place to check it as any.
  // It's probably not super-bad if this isn't true, but we'll have type casts between different
  // sizes without over/underflow verification.
#ifndef UNITTEST
  _Static_assert(sizeof(size_t) == sizeof(uint32_t), "sizeof(size_t) != sizeof(uint32_t)");
#endif

  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  if (app_message_ctx->outbox.phase != OUT_CLOSED ||
      app_message_ctx->inbox.is_open) {
    return APP_MSG_INVALID_STATE; // Already open
  }

  AppMessageResult result = app_message_outbox_open(&app_message_ctx->outbox, size_outbound);
  if (APP_MSG_OK != result) {
    return result;
  }

  result = app_message_inbox_open(&app_message_ctx->inbox, size_inbound);
  if (APP_MSG_OK != result) {
    app_message_outbox_close(&app_message_ctx->outbox);
    return result;
  }

  return APP_MSG_OK;
}

void app_message_close(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();

  // TODO PBL-1634: handle the return status when this function returns status.
  // For now, continue to ignore failure.
  app_message_outbox_close(&app_message_ctx->outbox);
  app_message_inbox_close(&app_message_ctx->inbox);

  app_message_deregister_callbacks();
}

// -------- Testing Interface (only) ------------------------------------------------------------ //

AppTimer *app_message_ack_timer_id(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  return app_message_ctx->outbox.ack_nack_timer;
}

bool app_message_is_accepting_inbound(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  return app_message_ctx->inbox.is_open;
}

bool app_message_is_accepting_outbound(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  return (app_message_ctx->outbox.phase == OUT_ACCEPTING);
}

bool app_message_is_closed_inbound(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  return (!app_message_ctx->inbox.is_open);
}

bool app_message_is_closed_outbound(void) {
  AppMessageCtx *app_message_ctx = app_state_get_app_message_ctx();
  return (app_message_ctx->outbox.phase == OUT_CLOSED);
}
