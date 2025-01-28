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

#include "applib/app_message/app_message_internal.h"
#include "kernel/pbl_malloc.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "services/common/analytics/analytics.h"
#include "services/common/comm_session/session.h"
#include "services/common/comm_session/session_receive_router.h"
#include "services/normal/app_inbox_service.h"
#include "system/logging.h"
#include "util/math.h"

#include <stdint.h>

extern const ReceiverImplementation g_default_kernel_receiver_implementation;
extern const ReceiverImplementation g_app_message_receiver_implementation;

////////////////////////////////////////////////////////////////////////////////////////////////////
// ReceiverImplementation that writes App Message PP messages to the app's memory space using
// app_inbox_service. It also forwards a copy of the header to the default system receiver, but
// with a special handler that will always send a nack reply. If all goes well, this forward is
// cancelled in the end and the nack does not get sent.

//! The maximum amount of header bytes that is needed in order to let the system nack it.
//! To nack an App Message push, only the transaction ID is needed. Therefore, only buffer the
//! AppMessageHeader of the incoming push:
#define MAX_HEADER_SIZE (sizeof(AppMessageHeader))

typedef struct {
  bool is_writing_to_app_inbox;

  CommSession *session;

  //! Used to keep track of how many header bytes are remaining to either forward to the default
  //! system receiver or to save them in the event the app inbox write fails in the end.
  //! We only want to write up to MAX_HEADER_SIZE, to keep the kernel heap impact to a minimum.
  size_t header_bytes_remaining;

  //! Pointer to the default system receiver context, to which we want to forward the header data.
  Receiver *kernel_receiver;
} AppMessageReceiver;

static bool prv_fwd_prepare(AppMessageReceiver *rcv, CommSession *session,
                            size_t header_bytes_remaining) {
  // Try to set up a forward to the default system receiver that will send a nack back, based
  // on the header of the message:
  static const PebbleProtocolEndpoint kernel_nack_endpoint = {
    .endpoint_id = APP_MESSAGE_ENDPOINT_ID,
    .handler = app_message_app_protocol_system_nack_callback,
    .access_mask = PebbleProtocolAccessAny,
    .receiver_imp = &g_default_kernel_receiver_implementation,
    .receiver_opt = NULL,
  };
  Receiver *kernel_receiver = g_default_kernel_receiver_implementation.prepare(session,
                                           &kernel_nack_endpoint,
                                           header_bytes_remaining);
  if (!kernel_receiver) {
    PBL_LOG(LOG_LEVEL_ERROR, "System receiver wasn't able to prepare");
    return false;
  }
  rcv->kernel_receiver = kernel_receiver;
  return true;
}

static void prv_write(const uint8_t *data, size_t length) {
  app_inbox_service_write(AppInboxServiceTagAppMessageReceiver, data, length);
}

static Receiver *prv_app_message_receiver_prepare(CommSession *session,
                                                  const PebbleProtocolEndpoint *endpoint,
                                                  size_t total_payload_size) {
  analytics_inc(ANALYTICS_APP_METRIC_MSG_IN_COUNT, AnalyticsClient_App);

  // FIXME: Find a better solution for this.
  // https://pebbletechnology.atlassian.net/browse/PBL-21538
  if (total_payload_size > 500) {
    comm_session_set_responsiveness(session, BtConsumerPpAppMessage, ResponseTimeMin,
                                    MIN_LATENCY_MODE_TIMEOUT_APP_MESSAGE_SECS);
  }

  AppMessageReceiver *rcv = (AppMessageReceiver *)kernel_zalloc(sizeof(AppMessageReceiver));
  if (!rcv) {
    return NULL;
  }
  rcv->session = session;

  const size_t header_bytes_remaining = MIN(MAX_HEADER_SIZE, total_payload_size);
  rcv->header_bytes_remaining = header_bytes_remaining;

  // Always forward the header to default system receiver as well, we'll cancel it later on if the
  // message was written successfully to the app inbox.
  if (!prv_fwd_prepare(rcv, session, header_bytes_remaining)) {
    kernel_free(rcv);
    return NULL;
  }

  const size_t total_size = sizeof(AppMessageReceiverHeader) + total_payload_size;

  // Reasons why app_inbox_service_begin() might fail:
  // - the watchapp does not have App Message context opened
  // - there is no more space in the buffer that the app had allocated for it,
  // - the inbox is already being written to (by another CommSession) -- should be very rare
  if (app_inbox_service_begin(AppInboxServiceTagAppMessageReceiver, total_size, session)) {
    rcv->is_writing_to_app_inbox = true;

    // Log most recent communication timestamp
    const AppInstallId app_id = app_manager_get_current_app_id();
    app_install_mark_prioritized(app_id, true /* can_expire */);

    // Write the header, this info is needed for the app to handle the message and reply:
    const AppMessageReceiverHeader header = (const AppMessageReceiverHeader) {
      .session = session,
    };
    prv_write((const uint8_t *)&header, sizeof(header));
  }

  return (Receiver *)rcv;
}

static void prv_app_message_receiver_write(Receiver *receiver, const uint8_t *data, size_t length) {
  AppMessageReceiver *rcv = (AppMessageReceiver *)receiver;

  // FIXME: Find a better solution for this.
  // https://pebbletechnology.atlassian.net/browse/PBL-21538
  comm_session_set_responsiveness(rcv->session, BtConsumerPpAppMessage, ResponseTimeMin,
                                  MIN_LATENCY_MODE_TIMEOUT_APP_MESSAGE_SECS);

  analytics_add(ANALYTICS_APP_METRIC_MSG_BYTE_IN_COUNT, length, AnalyticsClient_App);

  if (rcv->header_bytes_remaining > 0) {
    const size_t header_bytes_to_write = MIN(rcv->header_bytes_remaining, length);
    g_default_kernel_receiver_implementation.write(rcv->kernel_receiver,
                                                   data, header_bytes_to_write);
    rcv->header_bytes_remaining -= header_bytes_to_write;
  }

  if (rcv->is_writing_to_app_inbox) {
    prv_write(data, length);
  }
}

static void prv_finally(AppMessageReceiver *receiver,
                        void (*kernel_receiver_finally_cb)(Receiver *)) {
  kernel_receiver_finally_cb(receiver->kernel_receiver);
  kernel_free(receiver);
}

static void prv_app_message_receiver_finish(Receiver *receiver) {
  AppMessageReceiver *rcv = (AppMessageReceiver *)receiver;

  // Default to letting the system receiver process the message and thus nack it:
  void (*kernel_receiver_finally_cb)(Receiver *) = g_default_kernel_receiver_implementation.finish;

  if (rcv->is_writing_to_app_inbox) {
    if (app_inbox_service_end(AppInboxServiceTagAppMessageReceiver)) {
      // The write was successful, cancel processing the header for nacking:
      kernel_receiver_finally_cb = g_default_kernel_receiver_implementation.cleanup;
    } else {
      analytics_inc(ANALYTICS_APP_METRIC_MSG_DROP_COUNT, AnalyticsClient_App);
    }
  }

  prv_finally(rcv, kernel_receiver_finally_cb);
}

static void prv_app_message_receiver_cleanup(Receiver *receiver) {
  AppMessageReceiver *rcv = (AppMessageReceiver *)receiver;

  if (rcv->is_writing_to_app_inbox) {
    // Cancel the write, we don't want to deliver a broken message to the watchapp:
    app_inbox_service_cancel(AppInboxServiceTagAppMessageReceiver);
  }

  prv_finally(rcv, g_default_kernel_receiver_implementation.cleanup);
}

const ReceiverImplementation g_app_message_receiver_implementation = {
  .prepare = prv_app_message_receiver_prepare,
  .write = prv_app_message_receiver_write,
  .finish = prv_app_message_receiver_finish,
  .cleanup = prv_app_message_receiver_cleanup,
};
