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

#include "dls_private.h"
#include "dls_endpoint.h"
#include "dls_list.h"
#include "dls_storage.h"

#include "services/common/analytics/analytics.h"
#include "services/common/comm_session/protocol.h"
#include "services/common/comm_session/session_send_buffer.h"
#include "services/common/system_task.h"
#include "services/common/new_timer/new_timer.h"
#include "services/normal/data_logging/data_logging_service.h"
#include "kernel/pbl_malloc.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/legacy_checksum.h"
#include "util/math.h"

#include <inttypes.h>

#include "FreeRTOS.h"
#include "timers.h"

typedef struct {
  ListNode list_node;
  DataLoggingSession *session;
  // Session metadata to make sure the session pointer corresponds to
  // the same session that was added to the reopen list. This guards
  // against the session being destroyed and another getting allocated
  // to the same address.
  Uuid app_uuid;
  time_t timestamp;
  uint32_t tag;
} DataLoggingReopenEntry;

static struct {
  PebbleMutex * mutex;
  TimerID ack_timer;
  bool report_in_progress;
} s_endpoint_data;

typedef struct PACKED {
  uint8_t command;
  uint8_t session_id;
} DataLoggingCloseSessionMessage;

typedef struct PACKED {
   uint8_t command;
   uint8_t session_id;
   Uuid app_uuid;
   uint32_t timestamp;
   uint32_t logging_session_tag;
   DataLoggingItemType data_item_type:8;
   uint16_t data_item_size;
} DataLoggingOpenSessionMessage;

static const uint16_t ENDPOINT_ID_DATA_LOGGING = 0x1a7a;

#define ACK_NACK_TIMEOUT_TICKS (30 * RTC_TICKS_HZ)

static const uint8_t MAX_NACK_COUNT = 20;

static void reschedule_ack_timeout(void);

static void update_session_state(DataLoggingSession *session, DataLoggingSessionCommState new_state,
              bool reschedule) {
  session->comm.state = new_state;

  switch (new_state) {
  case DataLoggingSessionCommStateOpening:
  case DataLoggingSessionCommStateSending:
    // These states need an ack from the phone.
    session->comm.ack_timeout = rtc_get_ticks() + ACK_NACK_TIMEOUT_TICKS;
    break;
  case DataLoggingSessionCommStateIdle:
    session->comm.ack_timeout = 0;
    break;
  }

  if (reschedule) {
    reschedule_ack_timeout();
  }
}

static void send_timeout_msg(void *session_id_param) {
  uint8_t session_id = (uint8_t)(uintptr_t)session_id_param;
  CommSession *session = comm_session_get_system_session();
  if (!session) {
    // timed out because of lost connection
    return;
  }

  DataLoggingSession *logging_session = dls_list_find_by_session_id(session_id);

  struct PACKED {
    uint8_t command;
    uint8_t session_id;
  } msg = {
    .command = DataLoggingEndpointCmdTimeout,
    .session_id = logging_session->comm.session_id,
  };

  comm_session_send_data(session, ENDPOINT_ID_DATA_LOGGING, (uint8_t *)&msg,
                         sizeof(msg), COMM_SESSION_DEFAULT_TIMEOUT);
}

static bool check_ack_timeout_for_session(DataLoggingSession *session, void *data) {
  RtcTicks *current_ticks = (RtcTicks*) data;

  if (session->comm.ack_timeout != 0 && session->comm.ack_timeout <= *current_ticks) {
    PBL_LOG(LOG_LEVEL_DEBUG, "session %"PRIu8" timeout", session->comm.session_id);

    // Send timeout msg from system task because it could take a while and also require
    //  more stack space than provided by the timer task.
    system_task_add_callback(send_timeout_msg, (void*)(uintptr_t)(session->comm.session_id));

    // Set reschedule to false because: 1.) we don't need to reschedule the timer since all
    // we did was process one that already expired, 2.) it can cause an infinite recursion
    // because reschedule_ack_timeout() will call check_ack_timeout() (which we are already in) if
    // any other timers have already expired.
    update_session_state(session, DataLoggingSessionCommStateIdle, false /*reschedule*/);
  }

  return true;
}

static void check_ack_timeout(void) {
  RtcTicks current_ticks = rtc_get_ticks();

  dls_list_for_each_session(check_ack_timeout_for_session, &current_ticks);

  reschedule_ack_timeout();
}

static void ack_timer_cb(void *cb_data) {
  mutex_lock(s_endpoint_data.mutex);

  check_ack_timeout();

  mutex_unlock(s_endpoint_data.mutex);
}

static bool find_soonest_ack_timeout_cb(DataLoggingSession *session, void *data) {
  RtcTicks *soonest_ack_timeout = (RtcTicks*) data;
  if (session->comm.ack_timeout != 0
      && (session->comm.ack_timeout < *soonest_ack_timeout || *soonest_ack_timeout == 0)) {
    *soonest_ack_timeout = session->comm.ack_timeout;
  }
  return true;
}

static void reschedule_ack_timeout(void) {
  RtcTicks soonest_ack_timeout = 0;
  dls_list_for_each_session(find_soonest_ack_timeout_cb, &soonest_ack_timeout);

  if (soonest_ack_timeout == 0) {
    // No one is waiting for ack, just stop the timer
    new_timer_stop(s_endpoint_data.ack_timer);
    return;
  }

  RtcTicks current_ticks = rtc_get_ticks();
  if (soonest_ack_timeout < current_ticks) {
    // Handle the timeout immediately. This will result the in the timer being rescheduled if we're still
    // waiting for an ack.
    check_ack_timeout();
    return;
  }

  // Convert from ticks to ms for the timer
  RtcTicks ticks_until_timeout = soonest_ack_timeout - current_ticks;
  uint32_t ms_until_timeout = ((uint64_t) ticks_until_timeout * 1000) / RTC_TICKS_HZ;

  bool success = new_timer_start(s_endpoint_data.ack_timer, ms_until_timeout, ack_timer_cb, NULL, 0 /*flags*/);
  PBL_ASSERTN(success);
}

static void dls_endpoint_print_message(uint8_t *message, int num_bytes) {
  PBL_ASSERTN(message != NULL);

  switch (message[0]) {
    case DataLoggingEndpointCmdClose:
    {
      DataLoggingCloseSessionMessage *msg = (DataLoggingCloseSessionMessage *)message;
      PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Closing session %d", msg->session_id);
      break;
    }
    case DataLoggingEndpointCmdOpen:
    {
      DataLoggingOpenSessionMessage *msg = (DataLoggingOpenSessionMessage *)message;
      PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Opening session %u with tag %"PRIu32", type %u, size %hu",
          msg->session_id, msg->logging_session_tag, msg->data_item_type, msg->data_item_size);
      break;
    }
    case DataLoggingEndpointCmdData:
    {
      DataLoggingSendDataMessage *msg = (DataLoggingSendDataMessage *)message;
      PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Sending data with session_id %"PRIu8", items remaining %"PRIu32", crc 0x%"PRIx32", num_bytes %d",
        msg->session_id, msg->items_left_hereafter, msg->crc32, num_bytes);
      break;
    }
    default:
      PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Message type 0x%x not recognized", message[0]);
  }
}

bool dls_endpoint_open_session(DataLoggingSession *session) {
  CommSession *comm_session = comm_session_get_system_session();
  if (!session) {
    return false;
  }

  DataLoggingOpenSessionMessage msg = {
    .command = DataLoggingEndpointCmdOpen,
    .session_id = session->comm.session_id,
    .app_uuid = session->app_uuid,
    .timestamp = session->session_created_timestamp,
    .logging_session_tag = session->tag,
    .data_item_type = session->item_type,
    .data_item_size = session->item_size,
  };

  dls_endpoint_print_message((uint8_t *)&msg, 0);

  update_session_state(session, DataLoggingSessionCommStateOpening, true /*reschedule*/);

  return (comm_session_send_data(comm_session, ENDPOINT_ID_DATA_LOGGING,
                                 (uint8_t *)&msg, sizeof(DataLoggingOpenSessionMessage),
                                 COMM_SESSION_DEFAULT_TIMEOUT));
}

void dls_endpoint_close_session(uint8_t session_id) {
  CommSession *session = comm_session_get_system_session();
  if (!session) {
    return;
  }

  DataLoggingCloseSessionMessage msg = {
    .command = DataLoggingEndpointCmdClose,
    .session_id = session_id,
  };

  dls_endpoint_print_message((uint8_t *)&msg, 0);

  comm_session_send_data(session, ENDPOINT_ID_DATA_LOGGING,
                         (uint8_t *)&msg, sizeof(DataLoggingCloseSessionMessage),
                         COMM_SESSION_DEFAULT_TIMEOUT);
}

bool dls_endpoint_send_data(DataLoggingSession *logging_session, const uint8_t *data,
                            unsigned int num_bytes) {
  if (num_bytes < 1) {
    // not sending anything
    return true;
  }

  CommSession *session = comm_session_get_system_session();
  if (!session) {
    return false;
  }

  mutex_lock(s_endpoint_data.mutex);
  if (logging_session->comm.state != DataLoggingSessionCommStateIdle) {
    mutex_unlock(s_endpoint_data.mutex);
    // logging_session is waiting for an ack, we'll send next time around
    // don't return a failure, this is pretty innocuous.
    return true;
  }

  const uint32_t total_length = sizeof(DataLoggingSendDataMessage) + num_bytes;
  const uint32_t timeout_ms = 500;
  SendBuffer *sb = comm_session_send_buffer_begin_write(session, ENDPOINT_ID_DATA_LOGGING,
                                                        total_length, timeout_ms);
  if (!sb) {
    mutex_unlock(s_endpoint_data.mutex);
    return false;
  }

  analytics_inc(ANALYTICS_DEVICE_METRIC_DATA_LOGGING_ENDPOINT_SENDS,
                AnalyticsClient_System);

  const DataLoggingSendDataMessage header = (const DataLoggingSendDataMessage) {
    .command = DataLoggingEndpointCmdData,
    .session_id = logging_session->comm.session_id,
    .items_left_hereafter = 0xffff, // FIXME: logging_session->storage.num_bytes - num_bytes,
    .crc32 = legacy_defective_checksum_memory(data, num_bytes),
  };
  comm_session_send_buffer_write(sb, (const uint8_t *) &header, sizeof(header));
  comm_session_send_buffer_write(sb, data, num_bytes);
  comm_session_send_buffer_end_write(sb);

  dls_endpoint_print_message((uint8_t *) &header, num_bytes);
  DLS_HEXDUMP(data, MIN(num_bytes, 64));

  logging_session->comm.num_bytes_pending = num_bytes;

  update_session_state(logging_session, DataLoggingSessionCommStateSending, true /*reschedule*/);

  mutex_unlock(s_endpoint_data.mutex);

  unsigned int data_buffer_length = sizeof(DataLoggingSendDataMessage) + num_bytes;
  if (!uuid_is_system(&logging_session->app_uuid)) {
    analytics_inc_for_uuid(ANALYTICS_APP_METRIC_LOG_OUT_COUNT, &logging_session->app_uuid);
    analytics_add_for_uuid(ANALYTICS_APP_METRIC_LOG_BYTE_OUT_COUNT, data_buffer_length, &logging_session->app_uuid);
  }
  return true;
}

static void prv_dls_endpoint_handle_ack(uint8_t session_id) {
  DataLoggingSession *session = dls_list_find_by_session_id(session_id);
  if (session == NULL) {
    PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_WARNING, "Received ack for nonexistent session id: %"PRIu8, session_id);
    return;
  }

  mutex_lock(s_endpoint_data.mutex);

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Received ACK for id: %"PRIu8" state: %u", session->comm.session_id, session->comm.state);

  switch (session->comm.state) {
    case DataLoggingSessionCommStateIdle:
      PBL_LOG(LOG_LEVEL_ERROR, "Unexpected ACK");
      break;
    case DataLoggingSessionCommStateOpening:
      update_session_state(session, DataLoggingSessionCommStateIdle, true /*reschedule*/);
      break;
    case DataLoggingSessionCommStateSending:
      session->comm.nack_count = 0;
      update_session_state(session, DataLoggingSessionCommStateIdle, true /*reschedule*/);

      mutex_unlock(s_endpoint_data.mutex);

      // unlock for time consuming activities
      dls_storage_consume(session, session->comm.num_bytes_pending);
      session->comm.num_bytes_pending = 0;

      // the bt session is likely already active so continue to flush data
      dls_private_send_session(session, true);
      return;
  }

  mutex_unlock(s_endpoint_data.mutex);
}

static void prv_dls_endpoint_handle_nack(uint8_t session_id) {
  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Received NACK for id: %"PRIu8, session_id);

  DataLoggingSession *logging_session = dls_list_find_by_session_id(session_id);
  if (!logging_session) {
    PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_WARNING, "Received nack for nonexistent session id: %"PRIu8, session_id);
    return;
  }

  mutex_lock(s_endpoint_data.mutex);
  switch (logging_session->comm.state) {
    case DataLoggingSessionCommStateIdle:
    case DataLoggingSessionCommStateOpening:
      //Currently, these messages never get NACK'd
      PBL_LOG(LOG_LEVEL_ERROR, "Unexpected NACK");
      break;
    case DataLoggingSessionCommStateSending:
      //Maybe queue a resend
      logging_session->comm.num_bytes_pending = 0;
      if (++logging_session->comm.nack_count > MAX_NACK_COUNT) {
        PBL_LOG(LOG_LEVEL_ERROR, "Too many nacks. Flushing...");
        dls_storage_consume(logging_session, logging_session->storage.num_bytes);
        analytics_inc(ANALYTICS_DEVICE_METRIC_DATA_LOGGING_FLUSH_COUNT, AnalyticsClient_System);
        logging_session->comm.nack_count = 0;
      }
      break;
  }

  update_session_state(logging_session, DataLoggingSessionCommStateIdle, true /*reschedule*/);

  mutex_unlock(s_endpoint_data.mutex);

  // reopen the session that was NACK'ed
  dls_endpoint_open_session(logging_session);
}

//! System task callback executed which reopens the next session in the list built up by report_cmd_system_task_cb
static void prv_reopen_next_session_system_task_cb(void* data) {
  DataLoggingReopenEntry *entry = (DataLoggingReopenEntry *)data;
  if (!entry) {
    s_endpoint_data.report_in_progress = false;
    return;
  }
  DataLoggingReopenEntry *new_head = (DataLoggingReopenEntry *)list_pop_head((ListNode *)entry);

  // Try and reopen this session
  bool success = false;
  if (dls_list_is_session_valid(entry->session) &&
      uuid_equal(&entry->app_uuid, &entry->session->app_uuid) &&
      entry->timestamp == entry->session->session_created_timestamp &&
      entry->tag == entry->session->tag) {
    PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Reopening session %d",
              entry->session->comm.session_id);
    success = (dls_endpoint_open_session(entry->session)
               && dls_private_send_session(entry->session, false));
  } else {
    // Session has disappeared between the time that the reopen list was
    // created and now. This ideally shouldn't happen, but there's a lot
    // that's broken about datalogging. See PBL-37078.
    success = true;
  }
  kernel_free(entry);

  if (success) {
    // Schedule next one
    if (new_head) {
      bool result = system_task_add_callback(prv_reopen_next_session_system_task_cb, new_head);
      PBL_ASSERTN(result);
    } else {
      s_endpoint_data.report_in_progress = false;
    }
  } else {
    s_endpoint_data.report_in_progress = false;
    // If we failed, give up on the remaining ones
    PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Aborting all remaining open requests");
    while (new_head) {
      DataLoggingReopenEntry *entry = new_head;
      new_head = (DataLoggingReopenEntry *)list_pop_head((ListNode *)new_head);
      kernel_free(entry);
    }
  }
}

//! For use with dls_list_for_each_session. Appends this session to our list of sessions we need to open.
//! On entry, 'data' points to the variable holding the head of the list.
static bool dls_endpoint_add_reopen_sessions_cb(DataLoggingSession *session, void *data) {
  DataLoggingReopenEntry **head_ptr = (DataLoggingReopenEntry **)data;
  DataLoggingReopenEntry *entry = kernel_malloc_check(sizeof(DataLoggingReopenEntry));
  *entry = (DataLoggingReopenEntry) {
    .session = session,
    .app_uuid = session->app_uuid,
    .timestamp = session->session_created_timestamp,
    .tag = session->tag,
  };
  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "adding session %d to reopen list", session->comm.session_id);
  *head_ptr = (DataLoggingReopenEntry *)list_insert_before((ListNode *)(*head_ptr), &entry->list_node);
  return true;
}

static void prv_handle_report_cmd(const uint8_t *session_ids, size_t num_sessions) {
  for (size_t i = 0; i < num_sessions; ++i) {
    const uint8_t session_id = session_ids[i];

    DataLoggingSession *logging_session = dls_list_find_by_session_id(session_id);

    PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Phone reported session %u opened", session_id);

    // If the phone thinks we're open and we're not, send a close message.
    if (logging_session == NULL) {
      dls_endpoint_close_session(session_id);
    }
  }

  // If the bluetooth connection is flaky, a session reopen could take a few seconds, so we will chain them
  // and only do 1 re-open per system callback so that we don't trigger a watchdog timeout.
  DataLoggingReopenEntry *head = NULL;
  dls_list_for_each_session(dls_endpoint_add_reopen_sessions_cb, (void *)&head);

  // Re-open the first one and reschedule the next one
  prv_reopen_next_session_system_task_cb((void *)head);
}

//! Empty a session by session id
static void prv_empty_session(uint8_t session_id) {
  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Phone requested empty of session %u",
            session_id);
  DataLoggingSession *logging_session = dls_list_find_by_session_id(session_id);
  if (logging_session) {
    dls_private_send_session(logging_session, true /*empty_all_data*/);
  }
}


//! data_logging_protocol_msg_callback runs on Bluetooth task. Keep it quick.
void data_logging_protocol_msg_callback(CommSession *session, const uint8_t *data, size_t length) {
  // consume the first byte to read the command
  uint8_t command = data[0];

  --length; // the length now reflects the sizeof the payload

  // All commands from the phone have their high bit set.
  if ((command & ~DLS_ENDPOINT_CMD_MASK) == 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid data logging endpoint command 0x%x", command);
    // TODO: send some error code back?
    return;
  }

  switch (command & DLS_ENDPOINT_CMD_MASK) {
    case (DataLoggingEndpointCmdAck):
      prv_dls_endpoint_handle_ack(data[1]);
      break;

    case (DataLoggingEndpointCmdNack):
      prv_dls_endpoint_handle_nack(data[1]);
      break;

    case (DataLoggingEndpointCmdReport):
      if (s_endpoint_data.report_in_progress) {
        PBL_LOG(LOG_LEVEL_INFO, "Report already in progress");
      } else {
        s_endpoint_data.report_in_progress = true;
        prv_handle_report_cmd(&data[1], length);
      }
      break;

    case (DataLoggingEndpointCmdEmptySession):
      prv_empty_session(data[1]);
      break;

    case (DataLoggingEndpointCmdGetSendEnableReq):
      {
        bool enabled = dls_get_send_enable();
        struct PACKED {
          uint8_t command;
          uint8_t enabled;
        } msg = {
          .command = DataLoggingEndpointCmdGetSendEnableRsp,
          .enabled = enabled,
        };

        comm_session_send_data(session, ENDPOINT_ID_DATA_LOGGING, (uint8_t *)&msg,
                           sizeof(msg), COMM_SESSION_DEFAULT_TIMEOUT);
      }
      break;

    case (DataLoggingEndpointCmdSetSendEnable):
      dls_set_send_enable_pp(data[1]);
      break;
  }
}

void dls_endpoint_init(void) {
  s_endpoint_data.mutex = mutex_create();
  s_endpoint_data.ack_timer = new_timer_create();
}

static bool prv_handle_disconnect_cb(DataLoggingSession *session, void *data) {
  session->comm.state = DataLoggingSessionCommStateIdle;
  return true;
}

void dls_private_handle_disconnect(void *data) {
  mutex_lock(s_endpoint_data.mutex);
  dls_list_for_each_session(prv_handle_disconnect_cb, 0);
  mutex_unlock(s_endpoint_data.mutex);
}
