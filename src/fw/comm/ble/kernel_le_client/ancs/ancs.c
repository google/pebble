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

#define FILE_LOG_COLOR LOG_COLOR_BLUE

#include "ancs.h"
#include "ancs_app_name_storage.h"
#include "ancs_types.h"
#include "ancs_util.h"
#include "ancs_definition.h"

#include "comm/ble/ble_log.h"
#include "comm/ble/gatt_client_subscriptions.h"
#include "comm/ble/gatt_client_operations.h"
#include "comm/ble/kernel_le_client/dis/dis.h"

#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"

#include "services/common/analytics/analytics.h"
#include "services/common/evented_timer.h"
#include "services/normal/notifications/ancs/ancs_notifications.h"
#include "services/common/regular_timer.h"
#include "services/normal/timeline/timeline.h"

#include "system/hexdump.h"
#include "system/passert.h"
#include "system/logging.h"

#include "util/attributes.h"
#include "util/buffer.h"
#include "util/size.h"

#include <string.h>

// -----------------------------------------------------------------------------
// Static function prototypes

static void prv_handle_notification_attributes_response(const uint8_t *data, size_t length);

static void prv_handle_app_attributes_response(const uint8_t *data, size_t length);

static void prv_get_notification_attributes(uint32_t uid);

static bool prv_write_control_point_request(const CPDSMessage *cmd, size_t size);

static void prv_reset_reassembly_context(void);

T_STATIC void prv_check_ancs_alive(void);

static void prv_perform_action(uint32_t notification_uid, ActionId action_id);

// -----------------------------------------------------------------------------
// Static variables
//
// All accesses to these variables should happen from the KernelMain task,
// therefore no concurrent accesses can happen and no lock is needed.
// The only exception is the s_ns_flags_used_bitset, which gets read/set in
// analytics_external_collect_ancs_info from KernelBG. Since it's only one byte
// it should be fine.

#define INVALID_NOTIFICATION_UID 0xFFFFFFFF

#define ANCS_RETRY_TIME_MS (5 * MS_PER_SECOND)

typedef struct {
  uint8_t command_id;
  union {
    Buffer buffer;
    // `Buffer` has a variable sized uint8_t at the end of the struct. `buffer_storage` adds
    // the required backing storage space right after it:
    uint8_t buffer_storage[sizeof(Buffer) + NOTIFICATION_ATTRIBUTES_MAX_BUFFER_LENGTH];
  };
} ReassemblyContext;


typedef enum {
  NotificationQueueOpGetAttributes = 0,
  NotificationQueueOpPerformAction,
} NotificationQueueOp;

typedef enum {
  ANCSVersion_Unknown,
  ANCSVersion_iOS9OrNewer,
} ANCSVersion;

typedef struct {
  ListNode list_node;
  NotificationQueueOp op;
  uint32_t uid;
  ActionId action_id; // Only valid if op == NotificationQueueOpPerformAction
  ANCSProperty properties;
} NotificationQueueNode;

typedef struct ANCSClient {
  ANCSClientState state;
  BLECharacteristic characteristics[NumANCSCharacteristic];
  RegularTimerInfo is_alive_timer;
  ReassemblyContext reassembly_ctx;
  ANCSAttribute *attributes[NUM_FETCHED_NOTIF_ATTRIBUTES];
  NotificationQueueNode *queue;
  bool alive_check_pending;
  ANCSVersion version;
} ANCSClient;

static ANCSClient *s_ancs_client;

// Keeps track of used NS flags for analytics purposes:
static uint8_t s_ns_flags_used_bitset;

// -----------------------------------------------------------------------------
// State Machine

static bool prv_can_transition_state(ANCSClientState new_state) {
  if (s_ancs_client->state == new_state) {
    return true;
  }

  switch (s_ancs_client->state) {
    case ANCSClientStateIdle:
      return (new_state == ANCSClientStateRequestedNotification ||
              new_state == ANCSClientStateRetrying ||
              new_state == ANCSClientStatePerformingAction ||
              new_state == ANCSClientStateAliveCheck);
    case ANCSClientStateRequestedNotification:
      return (new_state == ANCSClientStateReassemblingNotification ||
              new_state == ANCSClientStateRequestedApp ||
              new_state == ANCSClientStateRetrying ||
              new_state == ANCSClientStateIdle);
    case ANCSClientStateReassemblingNotification:
      return (new_state == ANCSClientStateRequestedApp ||
              new_state == ANCSClientStateIdle);
    case ANCSClientStatePerformingAction:
      return (new_state == ANCSClientStateIdle);
    case ANCSClientStateRequestedApp:
      return (new_state == ANCSClientStateIdle);
    case ANCSClientStateAliveCheck:
        return (new_state == ANCSClientStateIdle);
    case ANCSClientStateRetrying:
        return (new_state == ANCSClientStateRequestedNotification ||
                new_state == ANCSClientStateIdle);
    default:
      WTF;
  }
}

static void prv_set_state(ANCSClientState new_state) {
  PBL_ASSERTN(prv_can_transition_state(new_state));
  s_ancs_client->state = new_state;
}

T_STATIC ANCSClientState prv_get_state(void) {
  return s_ancs_client->state;
}

// -----------------------------------------------------------------------------
// Notification Queue Logic

static void prv_do_notif_queue_operation(void) {
  if (s_ancs_client->queue->op == NotificationQueueOpGetAttributes) {
    prv_get_notification_attributes(s_ancs_client->queue->uid);
  } else if (s_ancs_client->queue->op == NotificationQueueOpPerformAction) {
    prv_perform_action(s_ancs_client->queue->uid, s_ancs_client->queue->action_id);
  }
}

static bool prv_notif_queue_comparator(ListNode *found_node, void *data) {
  NotificationQueueNode *queue_node = (NotificationQueueNode *)found_node;
  NotificationQueueNode *key_node = (NotificationQueueNode *)data;
  return (queue_node->uid == key_node->uid) && (queue_node->op == key_node->op);
}

static NotificationQueueNode *prv_notif_queue_find(NotificationQueueNode *node) {
  return (NotificationQueueNode *)list_find((ListNode *)s_ancs_client->queue,
                                            prv_notif_queue_comparator,
                                            (void *)node);
}

static void prv_notif_queue_reset(void) {
  ListNode *head = (ListNode *)s_ancs_client->queue;
  ListNode *cur;
  while (head) {
    cur = head;
    list_remove(cur, &head, NULL);
    kernel_free(cur);
  }
  s_ancs_client->queue = NULL;
}

static void prv_notif_queue_push_common(NotificationQueueNode *node) {
  if (prv_notif_queue_find(node)) {
    // already in the queue
    PBL_LOG(LOG_LEVEL_WARNING, "ANCS item already in Queue");
    kernel_free(node);
    return;
  }

  if (s_ancs_client->state == ANCSClientStateIdle) {
    s_ancs_client->queue = (NotificationQueueNode *)list_prepend((ListNode *)s_ancs_client->queue,
                                                                 (ListNode *)node);
    prv_do_notif_queue_operation();
  } else {
    list_append((ListNode *)s_ancs_client->queue,
               (ListNode *)node);
  }
}

static void prv_notif_queue_push_action(uint32_t uid, ActionId action_id) {
  NotificationQueueNode *node = kernel_malloc_check(sizeof(NotificationQueueNode));
  *node = (NotificationQueueNode) {
    .op = NotificationQueueOpPerformAction,
    .uid = uid,
    .action_id = action_id
  };

  prv_notif_queue_push_common(node);
}

static void prv_notif_queue_push_attr_request(uint32_t uid, ANCSProperty properties) {
  NotificationQueueNode *node = kernel_malloc_check(sizeof(NotificationQueueNode));
  *node = (NotificationQueueNode) {
    .op = NotificationQueueOpGetAttributes,
    .uid = uid,
    .properties = properties,
  };

  prv_notif_queue_push_common(node);
}

static void prv_notif_queue_pop(void) {
  NotificationQueueNode *temp = s_ancs_client->queue;
  if (temp) {
    list_remove((ListNode *)s_ancs_client->queue,
                (ListNode **)&s_ancs_client->queue,
                NULL);
    kernel_free(temp);
  }
}

static void prv_notif_queue_next(void) {
  if (s_ancs_client->alive_check_pending) {
    prv_check_ancs_alive();
    return;
  }

  if (s_ancs_client->queue == NULL) {
    // empty
    return;
  }

  prv_do_notif_queue_operation();
}

// -----------------------------------------------------------------------------
// Reset & Error Handling

static void prv_reset_and_idle(void) {
  prv_set_state(ANCSClientStateIdle);
  prv_reset_reassembly_context();
}

static void prv_reset_and_retry(void *unused) {
  if (s_ancs_client == NULL) {
    return;
  }

  prv_reset_reassembly_context();
  prv_notif_queue_next();
}

static void prv_reset_and_next(void) {
  prv_reset_and_idle();
  prv_notif_queue_pop();
  prv_notif_queue_next();
}

static void prv_reset_and_flush(void) {
  prv_reset_and_idle();
  prv_notif_queue_reset();
}

static void prv_reset_due_to_parse_error(void) {
  analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_PARSE_ERROR_COUNT,
                AnalyticsClient_System);
  prv_reset_and_next();
}

static void prv_reset_due_to_bt_error(void) {
  prv_reset_and_flush();
}

// -----------------------------------------------------------------------------
// Is Alive Logic

#define ANCS_INVALID_PARAM 0xA2
#define ANCS_IS_ALIVE_NEXT_CHECK_TIME_MINUTES 60 // 1 hour (60 minutes)
#define ANCS_IS_ALIVE_RESPONSE_WAIT_TIME_SECONDS 5 // 5 seconds


static void prv_is_ancs_alive_cb(void *data);
static void prv_is_ancs_alive_response_timeout(void *data);

static void prv_ancs_is_alive_schedule_next_check(void) {
  s_ancs_client->is_alive_timer = (const RegularTimerInfo) {
    .cb = prv_is_ancs_alive_cb,
  };
  regular_timer_add_multiminute_callback(&s_ancs_client->is_alive_timer,
                                         ANCS_IS_ALIVE_NEXT_CHECK_TIME_MINUTES);
}

static void prv_ancs_is_alive_start_response_wait_timer(void) {
  s_ancs_client->is_alive_timer = (const RegularTimerInfo) {
    .cb = prv_is_ancs_alive_response_timeout,
  };
  regular_timer_add_multisecond_callback(&s_ancs_client->is_alive_timer,
                                         ANCS_IS_ALIVE_RESPONSE_WAIT_TIME_SECONDS);
}

static void prv_ancs_is_alive_stop_timer(void) {
  if (regular_timer_is_scheduled(&s_ancs_client->is_alive_timer)) {
    regular_timer_remove_callback(&s_ancs_client->is_alive_timer);
  }
}

static void prv_ancs_is_alive_start_tracking(void) {
  if (regular_timer_is_scheduled(&s_ancs_client->is_alive_timer)) {
    prv_ancs_is_alive_stop_timer();
  } else {
    // Not scheduled, so analytics stopwatch would have been stopped
    analytics_stopwatch_start(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_CONNECT_TIME,
                              AnalyticsClient_System);
  }
  prv_ancs_is_alive_schedule_next_check();
}

static void prv_is_ancs_alive_response_timeout_launcher_task_cb(void *data) {
  if (!s_ancs_client) {
    return;
  }

  prv_reset_due_to_bt_error();

  // Stop the wait for response timer
  prv_ancs_is_alive_stop_timer();
}

static void prv_is_ancs_alive_response_timeout(void *data) {
  PBL_LOG(LOG_LEVEL_DEBUG, "ANCS isn't alive");
  analytics_stopwatch_stop(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_CONNECT_TIME);

  launcher_task_add_callback(prv_is_ancs_alive_response_timeout_launcher_task_cb, data);
}

static void prv_ancs_is_alive(void) {
  PBL_LOG(LOG_LEVEL_DEBUG, "ANCS is alive!");

  // Restart analytics tracking (if it stopped) and the 'is alive' timer
  prv_ancs_is_alive_start_tracking();
}

T_STATIC void prv_check_ancs_alive(void) {
  // Stop the next check timer
  prv_ancs_is_alive_stop_timer();

  if (s_ancs_client) {
    s_ancs_client->alive_check_pending = false;
    prv_set_state(ANCSClientStateAliveCheck);
    //! Sends an ANCS attribute fetch (to the Control Point). The notification UID is invalid, ANCS
    //! will reply with 0xA2 (invalid param)
    const GetNotificationAttributesMsg dummy_cmd = {
      .command_id = CommandIDGetNotificationAttributes,
      .notification_uid = INVALID_NOTIFICATION_UID,
    };
    prv_write_control_point_request((const CPDSMessage *)&dummy_cmd, sizeof(dummy_cmd));
    prv_ancs_is_alive_start_response_wait_timer();
  }
}

static void prv_is_ancs_alive_launcher_task_cb(void *data) {
  if (!s_ancs_client) {
    return;
  }
  if (s_ancs_client->state == ANCSClientStateIdle) {
    prv_check_ancs_alive();
  } else {
    s_ancs_client->alive_check_pending = true;
  }
}

static void prv_is_ancs_alive_cb(void *data) {
  launcher_task_add_callback(prv_is_ancs_alive_launcher_task_cb, data);
}

// -----------------------------------------------------------------------------
//! With iOS 8.2 the preexisting flag seems to be broken. Don't allow notifications for a bit after
//! reconnection so that all the "real" preexisting notification don't come through again.
static RegularTimerInfo s_notification_connection_delay_timer;
static bool s_just_connected = false;

static void prv_set_no_longer_just_connected(void *data) {
  s_just_connected = false;
  regular_timer_remove_callback(&s_notification_connection_delay_timer);
}

static void prv_start_temp_notification_connection_delay_timer(void) {
  if (regular_timer_is_scheduled(&s_notification_connection_delay_timer)) {
    regular_timer_remove_callback(&s_notification_connection_delay_timer);
  }
  s_just_connected = true;

  const int post_connection_notification_ignore_seconds = 10;
  s_notification_connection_delay_timer = (const RegularTimerInfo) {
    .cb = prv_set_no_longer_just_connected,
  };
  regular_timer_add_multisecond_callback(&s_notification_connection_delay_timer,
                                         post_connection_notification_ignore_seconds);
}

// -----------------------------------------------------------------------------
// Data source (DS) notification reassembly logic

static void prv_reset_reassembly_context(void) {
  memset(s_ancs_client->attributes, 0, sizeof(s_ancs_client->attributes));
  buffer_clear(&s_ancs_client->reassembly_ctx.buffer);
}

static bool prv_is_reassembly_in_progress(void) {
  return (s_ancs_client->state == ANCSClientStateReassemblingNotification);
}

static bool prv_reassembly_start(const uint8_t* const data, const size_t length) {
  PBL_ASSERTN(!prv_is_reassembly_in_progress());

  ReassemblyContext *reassembly_ctx = &s_ancs_client->reassembly_ctx;

  // Check that command ID is valid to prevent first part of buffer being occupied by invalid data
  // when a new, valid message is received
  const CPDSMessage * cmd_header = (const CPDSMessage *) data;
  if (cmd_header->command_id < CommandIdInvalid) {
    prv_set_state(ANCSClientStateReassemblingNotification);

    // Keep around the command_id, we know what parser to call later on:
    reassembly_ctx->command_id = cmd_header->command_id;

    // Append the partial response to the reassembly buffer:
    const int bytes_written = buffer_add(&reassembly_ctx->buffer, data, length);
    // If this gets hit, NOTIFICATION_ATTRIBUTES_MAX_BUFFER_LENGTH is too small:
    PBL_ASSERTN(bytes_written);

    return true;
  }
  return false;
}

static bool prv_reassembly_append(const uint8_t* const data, const size_t length) {
  PBL_ASSERTN(s_ancs_client->state == ANCSClientStateReassemblingNotification);
  return (buffer_add(&s_ancs_client->reassembly_ctx.buffer, data, length) != 0);
}

static uint8_t prv_current_command_id(const uint8_t* data) {
  return ((const CPDSMessage *)data)->command_id;
}

static bool prv_reassembly_is_complete(const uint8_t* data, const size_t length, bool* out_error) {
  switch (prv_current_command_id(data)) {
    case CommandIDGetNotificationAttributes:
      return ancs_util_is_complete_notif_attr_response(data, length, out_error);
    case CommandIDGetAppAttributes:
      return ancs_util_is_complete_app_attr_dict(data, length, out_error);
    default:
      *out_error = false;
      break;
  }
  return false;
}

static void prv_reassembly_handle_complete_response(const uint8_t* data, const size_t length) {

  analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_COUNT, AnalyticsClient_System);

  switch (prv_current_command_id(data)) {
    case CommandIDGetNotificationAttributes:
      prv_handle_notification_attributes_response(data, length);
      return;
    default:
      // WTF;
      prv_reset_and_next();
      return;
  }
}

static void prv_reassemble_ds_notification(uint32_t length, const uint8_t *data) {
  const bool is_first_message = !prv_is_reassembly_in_progress();
  if (is_first_message) {
    if (s_ancs_client->state != ANCSClientStateRequestedNotification ||
        !prv_reassembly_start(data, length)) {
      // Discard data if data is not the start of a new message or we didn't request it.
      return;
    }
  } else {
    // We have stuff in sitting in the reassembly buffer; assume that this is
    // data we need to finish reassembling the message
    const bool is_success = prv_reassembly_append(data, length);

    // [RC] This failure could be programmer error (in the reassembly code), but
    // could also occur if the iPhone restarts after sending us an incomplete
    // message, then we re-subscribe and start over from a different state
    if (!is_success) {
      PBL_LOG(LOG_LEVEL_ERROR, "ANCS reassembly buffer overflow; resetting ctx");
      // TODO: separate analytics trackers instead of piling onto "parse error count"
      prv_reset_due_to_parse_error();
      return;
    }

  }

  const uint8_t *response_data = s_ancs_client->reassembly_ctx.buffer.data;
  int response_length = s_ancs_client->reassembly_ctx.buffer.bytes_written;

  // Is the response complete? Or do we need to wait for more DS notifications?
  bool parse_error = false;
  const bool is_complete = prv_reassembly_is_complete(response_data, response_length,
      &parse_error);

  if (parse_error) {
    PBL_HEXDUMP(LOG_LEVEL_INFO, response_data, response_length);
    prv_reset_due_to_parse_error();
    return;
  }

  if (!is_complete) {
    // Keep waiting
    BLE_LOG_DEBUG("Incomplete response. Waiting for another DS notification.");
    return;
  }

  // Got all the data, pass up to parser!
  prv_reassembly_handle_complete_response(response_data, response_length);
}

static void prv_put_ancs_message(ANCSAttribute **app_attrs) {
  ancs_notifications_handle_message(s_ancs_client->queue->uid,
                                    s_ancs_client->queue->properties,
                                    s_ancs_client->attributes,
                                    app_attrs);
}

// -----------------------------------------------------------------------------
// Get App Attributes request

static void prv_handle_app_attributes_response(const uint8_t *data, size_t length) {
  // Skip over the app id
  while (length > 0) {
    length--;
    if (*data++ == 0) {
      break;
    }
  }

  ANCSAttribute *app_attrs[NUM_FETCHED_APP_ATTRIBUTES] = {0};

  if (length == 0) {
    goto fail;
  }

  bool error = false;
  const bool complete = ancs_util_get_attr_ptrs(data,
                                                length,
                                                s_fetched_app_attributes,
                                                NUM_FETCHED_APP_ATTRIBUTES,
                                                app_attrs,
                                                &error);
  if (!complete || error) {
    PBL_LOG(LOG_LEVEL_WARNING, "Error parsing app attributes");
    goto fail;
  }

  // cache the app name
  ANCSAttribute *app_id = s_ancs_client->attributes[FetchedNotifAttributeIndexAppID];
  ANCSAttribute *app_name = app_attrs[FetchedAppAttributeIndexDisplayName];
  ancs_app_name_storage_store(app_id, app_name);

fail:
  prv_put_ancs_message(app_attrs);
  prv_reset_and_next();
}

// -----------------------------------------------------------------------------
// Get Notification Attributes request

static void prv_add_attributes_to_request(Buffer *request_buffer) {
  static const struct PACKED {
    NotificationAttributeID positive_action:8;
    NotificationAttributeID negative_action:8;
    NotificationAttributeID app_id:8;
    NotificationAttributeID title:8;
    uint16_t max_title_length;
    NotificationAttributeID subtitle:8;
    uint16_t max_subtitle_length;
    NotificationAttributeID message:8;
    uint16_t max_message_length;
    //! Finish with the Date because the response value for the Date is
    //! fixed-length which allows us to determine whether the total response is
    //! finished or whether we need to expect DS notifications with more data.
    NotificationAttributeID date:8;
  } finishing_attributes = {
    .positive_action = NotificationAttributeIDPositiveActionLabel,
    .negative_action = NotificationAttributeIDNegativeActionLabel,
    .app_id = NotificationAttributeIDAppIdentifier,
    .title = NotificationAttributeIDTitle,
    .max_title_length = TITLE_MAX_LENGTH,
    .subtitle = NotificationAttributeIDSubtitle,
    .max_subtitle_length = SUBTITLE_MAX_LENGTH,
    .message = NotificationAttributeIDMessage,
    .max_message_length = MESSAGE_MAX_LENGTH,
    .date = NotificationAttributeIDDate,
  };

  buffer_add(request_buffer,
             (const uint8_t *) &finishing_attributes,
             sizeof(finishing_attributes));
}

static void prv_get_app_attributes(const ANCSAttribute *app_id) {
  if (!app_id) {
    prv_reset_and_next();
    return;
  }

  const size_t request_size = sizeof(GetAppAttributesMsg) +
                              app_id->length +
                              1 + // NULL terminator
                              ARRAY_LENGTH(s_fetched_app_attributes);

  GetAppAttributesMsg *request = kernel_zalloc_check(request_size);
  *request = (GetAppAttributesMsg) {
    .command_id = CommandIDGetAppAttributes,
  };

  uint8_t *request_data_ptr = (uint8_t *)request + sizeof(GetAppAttributesMsg);
  // app id
  memcpy(request_data_ptr, app_id->value, app_id->length);
  request_data_ptr += app_id->length;
  // NULL terminator
  *request_data_ptr = '\0';
  request_data_ptr += 1;
  // Requested attribute id(s)
  for (unsigned i = 0; i < ARRAY_LENGTH(s_fetched_app_attributes); ++i) {
    *request_data_ptr = s_fetched_app_attributes[i].id;
    request_data_ptr++;
  }

  prv_set_state(ANCSClientStateRequestedApp);

  bool success = prv_write_control_point_request((const CPDSMessage *) request, request_size);

  kernel_free(request);

  if (!success) {
    PBL_LOG(LOG_LEVEL_WARNING, "Failed to fetch app attributes for notification");
    ANCSAttribute *empty_attrs[NUM_FETCHED_APP_ATTRIBUTES] = {0};
    // we failed to fetch the app, but we got a notification
    prv_put_ancs_message(empty_attrs);
    prv_reset_and_next();
  }
}

static void prv_get_notification_attributes(uint32_t uid) {
  const GetNotificationAttributesMsg cmd_header = {
    .command_id = CommandIDGetNotificationAttributes,
    .notification_uid = uid,
  };

  static const size_t request_max_size = 32;
  Buffer *request_buffer = buffer_create(request_max_size);
  const size_t written_size = buffer_add(request_buffer,
                                         (const uint8_t *) &cmd_header,
                                         sizeof(cmd_header));
  PBL_ASSERTN(written_size == sizeof(cmd_header));

  prv_add_attributes_to_request(request_buffer);

  bool retrying = (s_ancs_client->state == ANCSClientStateRetrying);
  prv_set_state(ANCSClientStateRequestedNotification);

  bool success = prv_write_control_point_request((const CPDSMessage *) request_buffer->data,
                                                 request_buffer->bytes_written);

  kernel_free(request_buffer);

  if (!success) {
    if (retrying) {
      prv_reset_and_flush();
    } else {
      prv_set_state(ANCSClientStateRetrying);
      evented_timer_register(ANCS_RETRY_TIME_MS, false, prv_reset_and_retry, NULL);
    }
  }
}

static void prv_handle_notification_attributes_response(const uint8_t *data, size_t length) {
  // Skip past the header, don't need it (for now):
  data += sizeof(GetNotificationAttributesMsg);
  length -= sizeof(GetNotificationAttributesMsg);

  bool error = false;
  const bool did_get_attrs = ancs_util_get_attr_ptrs(data, length,
                                                     s_fetched_notif_attributes,
                                                     NUM_FETCHED_NOTIF_ATTRIBUTES,
                                                     s_ancs_client->attributes,
                                                     &error);
  if (!did_get_attrs || error) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error parsing attributes: %u, %u", did_get_attrs, error);
    prv_reset_and_next();
    return;
  }

  const ANCSAttribute *app_id = s_ancs_client->attributes[FetchedNotifAttributeIndexAppID];
  ANCSAttribute *app_name = ancs_app_name_storage_get(app_id);
  if (app_name) {
    prv_put_ancs_message(&app_name);
    prv_reset_and_next();
  } else {
    prv_get_app_attributes(app_id);
  }
}

// -----------------------------------------------------------------------------
// GATT Characteristic update & subscribe

static ANCSCharacteristic prv_get_id_for_characteristic(BLECharacteristic characteristic_to_find) {
  const BLECharacteristic *characteristic = s_ancs_client->characteristics;
  for (ANCSCharacteristic id = 0; id < NumANCSCharacteristic; ++id, ++characteristic) {
    if (*characteristic == characteristic_to_find) {
      return id;
    }
  }
  return ANCSCharacteristicInvalid;
}

static void prv_put_ancs_disconnected_event(void) {
  PebbleEvent event = {
      .type = PEBBLE_ANCS_DISCONNECTED_EVENT,
  };
  event_put(&event);
}

// Catching the subscription (CCCD write) confirmation for analytics purposes:
void ancs_handle_subscribe(BLECharacteristic subscribed_characteristic,
                           BLESubscription subscription_type, BLEGATTError error) {
  ANCSCharacteristic characteristic_id = prv_get_id_for_characteristic(subscribed_characteristic);
  if (characteristic_id != ANCSCharacteristicNotification &&
      characteristic_id != ANCSCharacteristicData) {
    // Only Notification and Data characteristics are expected to be subscribed to
    WTF;
  }

  static const AnalyticsMetric metric_matrix[2][2] = {
    [ANCSCharacteristicNotification] = {
      [0] = ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_NS_SUBSCRIBE_COUNT,
      [1] = ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_NS_SUBSCRIBE_FAIL_COUNT,
    },
    [ANCSCharacteristicData] = {
      [0] = ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_DS_SUBSCRIBE_COUNT,
      [1] = ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_DS_SUBSCRIBE_FAIL_COUNT,
    }
  };

  const bool no_error = (error == BLEGATTErrorSuccess);
  AnalyticsMetric metric = metric_matrix[characteristic_id][no_error ? 0 : 1];
  analytics_inc(metric, AnalyticsClient_System);

  if (no_error) {
    PBL_LOG(LOG_LEVEL_INFO, "Hurray! ANCS subscribed: %u", characteristic_id);

    if (characteristic_id == ANCSCharacteristicData) {
      prv_ancs_is_alive_start_tracking();
      prv_start_temp_notification_connection_delay_timer();
    }
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to subscribe charx: %u (error=%u)", characteristic_id, error);
  }
}

void ancs_invalidate_all_references(void) {
  for (int c = 0; c < NumANCSCharacteristic; c++) {
    s_ancs_client->characteristics[c] = BLE_CHARACTERISTIC_INVALID;
  }

  prv_reset_and_flush();
  prv_put_ancs_disconnected_event();
}

void ancs_handle_service_removed(BLECharacteristic *characteristics, uint8_t num_characteristics) {
  // There should only be one ancs client
  ancs_invalidate_all_references();
}

void ancs_handle_service_discovered(BLECharacteristic *characteristics) {
  BLE_LOG_DEBUG("In ANCS service discovery CB");
  PBL_ASSERTN(characteristics); // should only be called if we found something!
  analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_DISCOVERED_COUNT, AnalyticsClient_System);

  // Pause while re-subscribing, it will be resumed when re-subscribed:
  prv_ancs_is_alive_stop_timer();

  if (s_ancs_client->characteristics[0] != BLE_CHARACTERISTIC_INVALID) {
    PBL_LOG(LOG_LEVEL_WARNING, "Multiple ANCS services registered?!");
    ancs_invalidate_all_references();
  }

  // Keep around the BLECharacteristic references:
  memcpy(s_ancs_client->characteristics, characteristics,
         sizeof(BLECharacteristic) * NumANCSCharacteristic);

  // Subscribe to Data, then to Notification characteristics:
  for (int c = ANCSCharacteristicData; c >= ANCSCharacteristicNotification; --c) {
    const BTErrno e = gatt_client_subscriptions_subscribe(characteristics[c],
                                                          BLESubscriptionNotifications,
                                                          GAPLEClientKernel);
    PBL_ASSERTN(e == BTErrnoOK);
  }
}

bool ancs_can_handle_characteristic(BLECharacteristic characteristic) {
  if (!s_ancs_client) {
    return false;
  }
  for (int c = 0; c < NumANCSCharacteristic; ++c) {
    if (s_ancs_client->characteristics[c] == characteristic) {
      return true;
    }
  }
  return false;
}

// -------------------------------------------------------------------------------------------------
// Handling inbound GATT Notifications

static void prv_handle_ns_notification(uint32_t length, const uint8_t *notification) {
  PBL_ASSERTN(notification != NULL);

  analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_NS_COUNT, AnalyticsClient_System);
  analytics_add(ANALYTICS_DEVICE_METRIC_NOTIFICATION_BYTE_IN_COUNT, length, AnalyticsClient_System);

  if (length != sizeof(NSNotification)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Received invalid ANCS NS Notification length=<%"PRIu32">", length);
    return;
  }

  NSNotification* nsnotification = (NSNotification*) notification;
  ANCSProperty properties = ANCSProperty_None;

  BLE_LOG_DEBUG("NSNotification: ");
  BLE_LOG_DEBUG("> EventID: %d", nsnotification->event_id);
  BLE_LOG_DEBUG("> EventFlags: <%d>", nsnotification->event_flags);
  BLE_LOG_DEBUG("> CategoryID: <%d>", nsnotification->category_id);
  BLE_LOG_DEBUG("> CategoryCount: <%d>", nsnotification->category_count);
  BLE_LOG_DEBUG("> NotificationUID: <%"PRIu32">", nsnotification->uid);
  BLE_HEXDUMP((uint8_t *)nsnotification, sizeof(NSNotification));

  // Handle the CategoryID
  if (nsnotification->category_id == CategoryIDMissedCall) {
    properties |= ANCSProperty_MissedCall;
  } else if (nsnotification->category_id == CategoryIDIncomingCall) {
    properties |= ANCSProperty_IncomingCall;
  } else if (nsnotification->category_id == CategoryIDVoicemail) {
    properties |= ANCSProperty_VoiceMail;
  }

  // Handle the EventFlags
  if (nsnotification->event_flags & EventFlagMultiMedia) {
    properties |= ANCSProperty_MultiMedia;
  }

  if (s_ancs_client->version >= ANCSVersion_iOS9OrNewer) {
    properties |= ANCSProperty_iOS9;
  }

  // Handle the EventID
  switch (nsnotification->event_id) {
    case EventIDNotificationAdded:
      // In iOS 8.2 several apps (especially mail.app) seem to be setting the preexisting flag
      // when they shouldn't. This appeared to be fixed in iOS 9 beta 1.
      // By skipping the preexisting check we will re-recieve all the notifications
      // we got in the past 2 hours. To get past this ignore notifications for the first couple
      // seconds after connecting
      if (s_just_connected && (nsnotification->event_flags & EventFlagPreExisting)) {
        BLE_LOG_DEBUG("Ignoring notification because we just connected and PreExisting");
      } else {
        BLE_LOG_DEBUG("Added ANCS notification!");
        prv_notif_queue_push_attr_request(nsnotification->uid, properties);
      }

      // See analytics_external_collect_ancs_info()
      s_ns_flags_used_bitset |= nsnotification->event_flags;

      break;
    case EventIDNotificationModified:
      BLE_LOG_DEBUG("Modified ANCS notification!");
      prv_notif_queue_push_attr_request(nsnotification->uid, properties);
      break;
    case EventIDNotificationRemoved:
      BLE_LOG_DEBUG("Removed ANCS notification");
      ancs_notifications_handle_notification_removed(nsnotification->uid, properties);
      break;
  }
}

static void prv_handle_ds_notification(uint32_t length, const uint8_t *data) {
  PBL_ASSERTN(data != NULL);

  analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_DS_COUNT, AnalyticsClient_System);

  if (length < 1) {
    PBL_LOG(LOG_LEVEL_ERROR, "Received ANCS DS notification of length 0");
    return;
  }

  analytics_add(ANALYTICS_DEVICE_METRIC_NOTIFICATION_BYTE_IN_COUNT, length, AnalyticsClient_System);

  if (s_ancs_client->state == ANCSClientStateRequestedApp) {
    prv_handle_app_attributes_response(data, length);
  } else if (s_ancs_client->state == ANCSClientStateRequestedNotification ||
             s_ancs_client->state == ANCSClientStateReassemblingNotification) {
    prv_reassemble_ds_notification(length, data);
  }
}

void ancs_handle_read_or_notification(BLECharacteristic characteristic, const uint8_t *value,
                                      size_t value_length, BLEGATTError error) {
  if (error != BLEGATTErrorSuccess) {
    PBL_LOG(LOG_LEVEL_ERROR, "Read or notification error: %d", error);
    prv_reset_due_to_bt_error();
    return;
  }

  ANCSCharacteristic characteristic_id = prv_get_id_for_characteristic(characteristic);
  void (*handler)(uint32_t, const uint8_t *);
  switch (characteristic_id) {
    case ANCSCharacteristicNotification:
      handler = prv_handle_ns_notification;
      break;
    case ANCSCharacteristicData:
      handler = prv_handle_ds_notification;
      break;
    default:
      WTF;
  }
  handler(value_length, value);
}

// -----------------------------------------------------------------------------
// Writing commands to the ANCS Control Point

void ancs_handle_write_response(BLECharacteristic characteristic, BLEGATTError error) {
  if (error == ANCS_INVALID_PARAM) {
    if (s_ancs_client->state == ANCSClientStateAliveCheck) {
      // We got a response so cancel the response wait timer and setup another check.
      prv_ancs_is_alive();
    }

    // We asked for a nonexistent notification, go to the next one
    prv_reset_and_next();
    return;
  }

  if (error != BLEGATTErrorSuccess) {
    PBL_LOG(LOG_LEVEL_ERROR, "Control point error response: %d", error);
    prv_reset_due_to_bt_error();
    return;
  }

  BLE_LOG_DEBUG("Got ACK for Control Point write");

  if (s_ancs_client->queue && (s_ancs_client->queue->op == NotificationQueueOpPerformAction)) {
    // The action was successful
    prv_reset_and_next();
  }
}

static bool prv_write_control_point_request(const CPDSMessage *cmd, size_t size) {
  const BLECharacteristic cp = s_ancs_client->characteristics[ANCSCharacteristicControl];
  const BTErrno error = gatt_client_op_write(cp, (const uint8_t *) cmd, size, GAPLEClientKernel);

  BLE_LOG_DEBUG("Writing to control point:");
  PBL_HEXDUMP(LOG_LEVEL_DEBUG, (const uint8_t *) cmd, size);

  if (error != BTErrnoOK) {
    BLE_LOG_DEBUG("Control point write error: %d", error);
    return false;
  }

  return true;
}

// -------------------------------------------------------------------------------------------------
// Performing ANCS Notification Actions

static void prv_perform_action(uint32_t notification_uid, ActionId action_id) {
  prv_set_state(ANCSClientStatePerformingAction);
  PerformNotificationActionMsg action_msg = {
    .command_id = CommandIDPerformNotificationAction,
    .notification_uid = notification_uid,
    .action_id = action_id,
  };

  BLE_LOG_DEBUG("Taking action <%u> upon UID: %"PRIu32, action_id,
      notification_uid);

  const bool success = prv_write_control_point_request((const CPDSMessage *) &action_msg,
                                                       sizeof(action_msg));
  if (!success) {
    prv_reset_and_next();
  }
}

static void prv_serialize_action(const PerformNotificationActionMsg *action_msg) {
  if (!s_ancs_client) {
    PBL_LOG(LOG_LEVEL_ERROR, "No ANCS client");
    return;
  }

  prv_notif_queue_push_action(action_msg->notification_uid, action_msg->action_id);
}

void prv_serialize_action_launcher_task_cb(void *data) {
  const PerformNotificationActionMsg *action_msg = (PerformNotificationActionMsg *) data;
  prv_serialize_action(action_msg);
  kernel_free(data);
}

void ancs_perform_action(uint32_t notification_uid, uint8_t action_id) {
  bool is_kernel_main = (pebble_task_get_current() == PebbleTask_KernelMain);
  // Avoid heap allocation when directly calling prv_serialize_action:
  PerformNotificationActionMsg action_msg;
  PerformNotificationActionMsg *action_msg_ptr = is_kernel_main ?
  &action_msg : kernel_malloc_check(sizeof(PerformNotificationActionMsg));
  *action_msg_ptr = (const PerformNotificationActionMsg) {
    .command_id = CommandIDPerformNotificationAction,
    .notification_uid = notification_uid,
    .action_id = action_id,
  };
  if (is_kernel_main) {
    prv_serialize_action(action_msg_ptr);
  } else {
    launcher_task_add_callback(prv_serialize_action_launcher_task_cb, action_msg_ptr);
  }
}

void ancs_handle_ios9_or_newer_detected(void) {
  // The ANCSClient is created as soon as the gateway is connected (see kernel_le_client.c).
  PBL_ASSERTN(s_ancs_client);
  s_ancs_client->version = ANCSVersion_iOS9OrNewer;
}

// -------------------------------------------------------------------------------------------------
// Lifecycle

void ancs_create(void) {
  PBL_ASSERTN(s_ancs_client == NULL);
  s_ancs_client = (ANCSClient *) kernel_zalloc_check(sizeof(ANCSClient));
  buffer_init(&s_ancs_client->reassembly_ctx.buffer,
              sizeof(s_ancs_client->reassembly_ctx.buffer_storage));
  ancs_app_name_storage_init();
}

void ancs_destroy(void) {
  if (!s_ancs_client) {
    return;
  }
  analytics_stopwatch_stop(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_CONNECT_TIME);
  prv_ancs_is_alive_stop_timer();

  ancs_app_name_storage_deinit();

  prv_reset_and_flush();
  kernel_free(s_ancs_client);
  s_ancs_client = NULL;
  prv_put_ancs_disconnected_event();
}

// -------------------------------------------------------------------------------------------------
// Analytics

void analytics_external_collect_ancs_info(void) {
  // Keep track of bits that are used by this version of ANCS, we log this to analytics so we get
  // an indication of upcoming extensions to ANCS early on:
  analytics_set(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_NS_FLAGS_BITSET,
                s_ns_flags_used_bitset, AnalyticsClient_System);
  s_ns_flags_used_bitset = 0;
}
