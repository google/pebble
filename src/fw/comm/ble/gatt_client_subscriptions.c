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

#include "gatt_client_subscriptions.h"
#include "gatt_client_accessors.h"
#include "gatt_client_operations.h"
#include "gatt_service_changed.h"

#include <bluetooth/gatt.h>

#include "gap_le_connection.h"

#include "comm/bt_lock.h"
#include "drivers/rtc.h"

#include "kernel/events.h"
#include "kernel/pbl_malloc.h"

#include "services/common/analytics/analytics.h"
#include "system/logging.h"
#include "system/passert.h"

#include "util/circular_buffer.h"
#include "util/likely.h"

#include <os/mutex.h>
#include <os/tick.h>

#include "FreeRTOS.h"
#include "semphr.h"

//! Time to wait/block for when the buffer is full and needs to be drained by the client.
//! Note that bt_lock() is held while waiting, so this has to be rather small.
#define GATT_CLIENT_SUBSCRIPTIONS_WRITE_TIMEOUT_MS (100)

// TODO:
// - Intercept "manual" CCCD writes from the app, error for now? or translate to
//   ble_client_subscribe calls?
// - Filter out ANCS / AMS services -- apps shouldn't be able to muck with these

// -------------------------------------------------------------------------------------------------
// Static variables

static PebbleRecursiveMutex *s_gatt_client_subscriptions_mutex;
static SemaphoreHandle_t s_gatt_client_subscriptions_semphr;

//! s_gatt_client_subscriptions_mutex must be taken when accessing these static variables below!

//! Circular buffer holding notifications/indications that still need to be
//! consumed by the client. One circular buffer is created for a client as soon
//! as it subscribes to one (or more) characteristic.
static CircularBuffer *s_circular_buffer[GAPLEClientNum];
static uint32_t s_circular_buffer_retain_count[GAPLEClientNum];

//! Whether a PEBBLE_BLE_GATT_CLIENT_EVENT has been scheduled for the particular GAPLEClient.
//! This is to bound the number of these events to one per queue.
static bool s_is_notification_event_pending[GAPLEClientNum];

// -------------------------------------------------------------------------------------------------
// The call below requires the caller to own the bt_lock while calling the
// function and for as long as the result is being used / accessed.
extern BLEDescriptor gatt_client_accessors_find_cccd_with_characteristic(
                                                            BLECharacteristic characteristic_ref,
                                                            uint8_t *characteristic_properties_out,
                                                            uint16_t *characteristic_att_handle_out,
                                                            GAPLEConnection **connection_out);

extern BLECharacteristic gatt_client_descriptor_get_characteristic_and_connection(
                                                                  BLEDescriptor descriptor_ref,
                                                                  GAPLEConnection **connection_out);

// -------------------------------------------------------------------------------------------------
// Function implemented by the gatt_client_operations module to write the CCCD (to alter the remote
// subscription state). The big difference with gatt_client_op_write_descriptor() is that this
// function calls back to the gatt_client_subscriptions module when the result of the write is
// received, so that module can take care of sending the appropriate events to the clients.
extern BTErrno gatt_client_op_write_descriptor_cccd(BLEDescriptor cccd_ref,
                                                    const uint16_t *cccd_value);

// -------------------------------------------------------------------------------------------------
// Static function prototypes

static GATTClientSubscriptionNode * prv_find_subscription_for_characteristic(
                                                               BLECharacteristic characteristic_ref,
                                                                       GAPLEConnection *connection);

static BLESubscription prv_prevailing_subscription_type(GATTClientSubscriptionNode *subscription);

static void prv_release_buffer(GAPLEClient client);

static void prv_remove_subscription(GAPLEConnection *connection,
                                    GATTClientSubscriptionNode *subscription);

// -------------------------------------------------------------------------------------------------

//! bt_lock() may only (optionally) be taken *before* prv_lock(), otherwise we'll deadlock.
static void prv_lock(void) {
  mutex_lock_recursive(s_gatt_client_subscriptions_mutex);
}

static void prv_unlock(void) {
  mutex_unlock_recursive(s_gatt_client_subscriptions_mutex);
}

static void prv_send_notification_event(PebbleTaskBitset task_mask) {
  PebbleEvent e = {
    .type = PEBBLE_BLE_GATT_CLIENT_EVENT,
    .task_mask = task_mask,
    .bluetooth = {
      .le = {
        .gatt_client = {
          .subtype = PebbleBLEGATTClientEventTypeNotification,
          .gatt_error = BLEGATTErrorSuccess,
        },
      },
    },
  };
  event_put(&e);
}

static void prv_send_subscription_event(BLECharacteristic characteristic_ref,
                                        PebbleTaskBitset task_mask, BLESubscription type,
                                        BLEGATTError gatt_error) {
  PebbleEvent e = {
    .type = PEBBLE_BLE_GATT_CLIENT_EVENT,
    .task_mask = task_mask,
    .bluetooth = {
      .le = {
        .gatt_client = {
          .subtype = PebbleBLEGATTClientEventTypeCharacteristicSubscribe,
          .object_ref = characteristic_ref,
          .subscription_type = type,
          .gatt_error = gatt_error,
        },
      },
    },
  };
  event_put(&e);
}

static bool prv_find_subscription_by_att_handle(ListNode *node, void *data) {
  const GATTClientSubscriptionNode *subscription = (const GATTClientSubscriptionNode *) node;
  const uint16_t att_handle = (const uint16_t)(uintptr_t) data;
  return (subscription->att_handle == att_handle);
}

static bool prv_retain_buffer(GAPLEClient client);

static bool prv_wait_until_write_space_available(const CircularBuffer *buffer,
                                                 size_t required_length, uint32_t timeout_ms) {
  bool did_stall = false;
  const RtcTicks timeout_end_ticks = rtc_get_ticks() + milliseconds_to_ticks(timeout_ms);
  while (true) {
    prv_lock();
    // bt_lock() is held when this function is called. Unsubscribing also requires taking bt_lock(),
    // therefore it can't have been released in the mean time and therefore no need to check whether
    // it still exists.
    const uint16_t write_space = circular_buffer_get_write_space_remaining(buffer);
    prv_unlock();
    if (LIKELY(write_space >= required_length)) {
      if (UNLIKELY(did_stall)) {
        PBL_LOG(LOG_LEVEL_DEBUG, "GATT notification stalled for %d ms...",
                (int)(timeout_ms - ticks_to_milliseconds(timeout_end_ticks - rtc_get_ticks())));
        analytics_inc(ANALYTICS_DEVICE_METRIC_BLE_GATT_STALLED_NOTIFICATIONS_COUNT,
                      AnalyticsClient_System);
      }
      return true;
    }

    const RtcTicks now_ticks = rtc_get_ticks();
    if (now_ticks > timeout_end_ticks) {
      // Timeout expired.
      return false;
    }
    // Wait until space is freed up:
    const uint32_t timeout_ticks = (timeout_end_ticks - now_ticks);
    if (pdFALSE == xSemaphoreTake(s_gatt_client_subscriptions_semphr, timeout_ticks)) {
      // Timeout expired while waiting for the semaphore.
      return false;
    }

    did_stall = true;
  }
}

//! Internally used by gatt.c, should not be called otherwise.
//! For some reason, Bluetopia considers server notifications / indications the
//! be "connection events", while they are really client events...
//! @note bt_lock may be held by the caller. If the bt_lock is not held we will block for a little
//! if the subscription buffer is full
void gatt_client_subscriptions_handle_server_notification(GAPLEConnection *connection,
                                                          uint16_t att_handle,
                                                          const uint8_t *value,
                                                          uint16_t length) {
  bt_lock();

  ListNode *head = (ListNode *) connection->gatt_subscriptions;
  const GATTClientSubscriptionNode *subscription =
          (const GATTClientSubscriptionNode *) list_find(head, prv_find_subscription_by_att_handle,
                                                         (void *)(uintptr_t) att_handle);
  if (UNLIKELY(!subscription)) {
    // MT: I suspect this can be hit when the remote remembers the CCCD subscription state across
    // disconnections (while we don't remember it across disconnections).
    // iOS 7 behaves like this. iOS 8 supposedly does not.
    static uint16_t s_last_logged_handle;
    if (s_last_logged_handle != att_handle) {
      // Only log the same handle once. Logging to flash adds enough of a delay to cause the
      // Bluetopia Mailbox to get backed up quicker when running at a 15ms connection interval.
      s_last_logged_handle = att_handle;
      PBL_LOG(LOG_LEVEL_ERROR, "No subscription found for ATT handle %u", att_handle);
    }
    goto unlock;
  }

  // Mask to mask out all tasks
  const PebbleTaskBitset task_mask_none = ~0;
  PebbleTaskBitset task_mask = task_mask_none;

  for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
    if (UNLIKELY(subscription->subscriptions[c] == BLESubscriptionNone)) {
      // Not subscribed, continue
      continue;
    }
    // Write the header first, then write the payload:
    GATTBufferedNotificationHeader header = {
      .characteristic = subscription->characteristic,
      .value_length = length,
    };
    CircularBuffer *buffer = s_circular_buffer[c];
    bt_unlock();

    // If we do not hold the bt_lock() at this point it's safe to block for a little bit waiting
    // for notifications to be consumed
    uint32_t write_timeout = bt_lock_is_held() ? 0 : GATT_CLIENT_SUBSCRIPTIONS_WRITE_TIMEOUT_MS;
    bool consumed = prv_wait_until_write_space_available(buffer, (sizeof(header) + length),
                                                         write_timeout);

    bt_lock();
    if (!consumed) {
      PBL_LOG(LOG_LEVEL_ERROR, "Subscription buffer full. Dropping GATT notification of %u bytes",
              length);
      analytics_inc(ANALYTICS_DEVICE_METRIC_BLE_GATT_DROPPED_NOTIFICATIONS_COUNT,
                    AnalyticsClient_System);
      continue;
    }
    prv_lock();
    {
      circular_buffer_write(buffer, (const uint8_t *) &header, sizeof(header));
      circular_buffer_write(buffer, value, length);
      if (UNLIKELY(!s_is_notification_event_pending[c])) {
        task_mask &= ~gap_le_pebble_task_bit_for_client(c);
        s_is_notification_event_pending[c] = true;
      }
    }
    prv_unlock();
  }

  if (UNLIKELY(task_mask != task_mask_none)) {
    prv_send_notification_event(task_mask);
  }
unlock:
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------

static GATTClientSubscriptionNode * prv_find_subscription_and_connection_for_cccd(
                                                                 BLEDescriptor cccd_ref,
                                                                 GAPLEConnection **connection_out) {
  BLECharacteristic characteristic_ref =
                    gatt_client_descriptor_get_characteristic_and_connection(cccd_ref,
                                                                             connection_out);
  if (!*connection_out) {
    return NULL;
  }
  return prv_find_subscription_for_characteristic(characteristic_ref, *connection_out);
}

//! Internally used by gatt_client_operations.c, should not be called otherwise.
//! This function handles the completion of pending (un)subscriptions (confirmations of the writing
//! to the remote CCCD).
//! @note bt_lock is assumed to be already been taken by the caller!
void gatt_client_subscriptions_handle_write_cccd_response(BLEDescriptor cccd, BLEGATTError error) {
  GAPLEConnection *connection;
  GATTClientSubscriptionNode *subscription =
                                   prv_find_subscription_and_connection_for_cccd(cccd, &connection);
  if (!subscription || !connection) {
    // FIXME: When unsubscribing, the GATTClientSubscriptionNode is already removed at this point
    PBL_LOG(LOG_LEVEL_DEBUG,
            "No subscription and/or connection found for CCCD write response (%u)", error);
    return;
  }

  // Mask to mask out all tasks
  const PebbleTaskBitset task_mask_none = ~0;

  PebbleTaskBitset task_mask = task_mask_none;
  const bool has_error = (error != BLEGATTErrorSuccess);
  const BLESubscription type = has_error ?
                               BLESubscriptionNone : prv_prevailing_subscription_type(subscription);
  for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
    if (subscription->pending_confirmation[c]) {
      subscription->pending_confirmation[c] = false;
      if (subscription->subscriptions[c] == BLESubscriptionNone) {
        // Client unsubscribed in the mean-time. Confirmation should already have been sent.
        continue;
      }
      if (has_error) {
        // Subscribe failed. Record that the client is not subscribed and release buffer:
        subscription->subscriptions[c] = BLESubscriptionNone;
        prv_release_buffer(c);
      }
      task_mask &= ~gap_le_pebble_task_bit_for_client(c);
    }
  }

  if (task_mask != task_mask_none) {
    prv_send_subscription_event(subscription->characteristic, task_mask, type, error);
  }

  // In the error case, clean up the subscription data structure, if no longer used:
  if (has_error && prv_prevailing_subscription_type(subscription) == BLESubscriptionNone) {
    prv_remove_subscription(connection, subscription);
  }
}

// -------------------------------------------------------------------------------------------------

static bool prv_check_buffer(GAPLEClient client) {
  if (s_circular_buffer[client] == NULL) {
    PBL_LOG(LOG_LEVEL_ERROR, "App attempted to consume notifications without buffer.");
    return false;
  }
  return true;
}

// -------------------------------------------------------------------------------------------------

bool prv_get_next_notification_header(GAPLEClient client,
                                      GATTBufferedNotificationHeader *header_out) {
  bool has_notification = false;
  GATTBufferedNotificationHeader header;
  const uint16_t copied_length = circular_buffer_copy(s_circular_buffer[client],
                                                      (uint8_t *) &header,
                                                      sizeof(header));
  if (copied_length == sizeof(header)) {
    has_notification = true;
    if (header_out) {
      *header_out = header;
    }
  }
  return has_notification;
}

// -------------------------------------------------------------------------------------------------

bool gatt_client_subscriptions_get_notification_header(GAPLEClient client,
                                                       GATTBufferedNotificationHeader *header_out) {
  bool has_notification = false;
  prv_lock();
  if (!prv_check_buffer(client)) {
    goto unlock;
  }
  has_notification = prv_get_next_notification_header(client, header_out);
  const uint16_t read_space = circular_buffer_get_read_space_remaining(s_circular_buffer[client]);
  if (has_notification && header_out) {
    // When tackling https://pebbletechnology.atlassian.net/browse/PBL-14151 this should probably
    // not be an assert, but just return 0, in case the app mucked with the storage
    PBL_ASSERTN(header_out->value_length <= read_space - sizeof(*header_out));
  }
unlock:
  prv_unlock();
  return has_notification;
}

// -------------------------------------------------------------------------------------------------

uint16_t gatt_client_subscriptions_consume_notification(BLECharacteristic *characteristic_ref_out,
                                                        uint8_t *value_out,
                                                        uint16_t *value_length_in_out,
                                                        GAPLEClient client, bool *has_more_out) {
  bool has_more = false;

  GATTBufferedNotificationHeader next_header = {};
  prv_lock();
  {
    if (!prv_check_buffer(client)) {
      has_more = false; // the client went away
      goto unlock;
    }

    GATTBufferedNotificationHeader header = {};
    const bool has_notification = prv_get_next_notification_header(client, &header);
    if (LIKELY(has_notification)) {
      if (LIKELY(*value_length_in_out >= header.value_length)) {
        const uint16_t copied_length =
              circular_buffer_copy_offset(s_circular_buffer[client],
                                          sizeof(header), /* skip header */
                                          value_out,
                                          header.value_length);
        if (UNLIKELY(copied_length != header.value_length)) {
          PBL_LOG(LOG_LEVEL_ERROR, "Couldn't copy the number of requested byes (%u vs %u)",
                  header.value_length, copied_length);
        }
        *characteristic_ref_out = header.characteristic;
        *value_length_in_out = copied_length;
      } else {
        PBL_LOG(LOG_LEVEL_ERROR, "Client didn't provide buffer that was big enough (%u vs %u)",
                *value_length_in_out, header.value_length);
        *characteristic_ref_out = BLE_CHARACTERISTIC_INVALID;
        *value_length_in_out = 0;
      }
      // Always eat the notification:
      circular_buffer_consume(s_circular_buffer[client],
                              sizeof(header) + header.value_length);
    } else {
      PBL_LOG(LOG_LEVEL_WARNING, "Consume called while no notifications in buffer");
      *characteristic_ref_out = BLE_CHARACTERISTIC_INVALID;
      *value_length_in_out = 0;
    }

    has_more = has_notification &&
                          prv_get_next_notification_header(client, &next_header);
  }
unlock:
    if (!has_more) {
      s_is_notification_event_pending[client] = false;
    }
    if (has_more_out) {
      *has_more_out = has_more;
    }

  prv_unlock();

  // In the interest of simplicity, just give unconditionally (regardless of the number of bytes
  // consumed and regardless of which buffer was freed) to make
  // prv_wait_until_write_space_available() "poll" once whether there's enough space. We could be
  // smarter about this and add additional book-keeping so the semaphore is only given if enough
  // bytes have been freed up in the buffer of interest.
  xSemaphoreGive(s_gatt_client_subscriptions_semphr);
  return next_header.value_length;
}

// -------------------------------------------------------------------------------------------------

void gatt_client_subscriptions_reschedule(GAPLEClient c) {
  prv_lock();
  const PebbleTaskBitset task_mask = ~gap_le_pebble_task_bit_for_client(c);
  prv_send_notification_event(task_mask);
  s_is_notification_event_pending[c] = true;
  prv_unlock();
}

// -------------------------------------------------------------------------------------------------

// Decrements ownership count
static void prv_release_buffer(GAPLEClient client) {
  prv_lock();
  {
    PBL_ASSERTN(s_circular_buffer_retain_count[client]);
    --s_circular_buffer_retain_count[client];
    if (s_circular_buffer_retain_count[client] == 0) {
      // Last subscription for this client to require the circular buffer, go ahead and clean it up:
      kernel_free(s_circular_buffer[client]);
      s_circular_buffer[client] = NULL;
      // if the buffer is destroyed, there are no more events
      s_is_notification_event_pending[client] = false;
    }
  }
  prv_unlock();
}

// Increments ownership count
static bool prv_retain_buffer(GAPLEClient client) {
  bool rv = true;
  prv_lock();
  {
    if (s_circular_buffer_retain_count[client] == 0) {
      // First subscription for this client to require the circular buffer, go ahead and create it:
      PBL_ASSERTN(s_circular_buffer[client] == NULL);
      const size_t size = sizeof(CircularBuffer) + GATT_CLIENT_SUBSCRIPTIONS_BUFFER_SIZE;
      // TODO: Use app_malloc for the storage when client is app
      // https://pebbletechnology.atlassian.net/browse/PBL-14151
      uint8_t *buffer = (uint8_t *) kernel_zalloc(size);
      if (!buffer) {
        rv = false;
        goto unlock;
      }
      CircularBuffer *circular_buffer = (CircularBuffer *) buffer;
      circular_buffer_init(circular_buffer, (uint8_t *) (circular_buffer + 1),
                           GATT_CLIENT_SUBSCRIPTIONS_BUFFER_SIZE);
      s_circular_buffer[client] = circular_buffer;
    }
    ++s_circular_buffer_retain_count[client];
  }
unlock:
  prv_unlock();
  return rv;
}

// -------------------------------------------------------------------------------------------------

static bool prv_find_subscription_cb(ListNode *node, void *data) {
  const GATTClientSubscriptionNode *subscription = (const GATTClientSubscriptionNode *) node;
  const BLECharacteristic characteristic_ref = (BLECharacteristic) data;
  return (subscription->characteristic == characteristic_ref);
}

static GATTClientSubscriptionNode * prv_find_subscription_for_characteristic(
                                                               BLECharacteristic characteristic_ref,
                                                               GAPLEConnection *connection) {
  ListNode *head = (ListNode *) connection->gatt_subscriptions;
  return (GATTClientSubscriptionNode *) list_find(head, prv_find_subscription_cb,
                                                  (void *) characteristic_ref);
}

// -------------------------------------------------------------------------------------------------

static bool prv_has_pending_cccd_write(GATTClientSubscriptionNode *subscription) {
  for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
    if (subscription->pending_confirmation[c]) {
      return true;
    }
  }
  return false;
}

static BLESubscription prv_prevailing_subscription_type(GATTClientSubscriptionNode *subscription) {
  const BLESubscription orred = subscription->subscriptions[GAPLEClientApp] |
                                subscription->subscriptions[GAPLEClientKernel];
  // Notifications wins over None and Indications:
  if (orred & BLESubscriptionNotifications) {
    return BLESubscriptionNotifications;
  }
  // None or Indications:
  return (orred & BLESubscriptionIndications);
}

//! Mask out unsupported subscription type bits based on the
//! supported_properties of a characteristic.
//! @return true if the subscription_type is supported, false if not.
static bool prv_sanitize_subscription_type(BLESubscription *subscription_type,
                                           uint8_t supported_properties) {
  if (*subscription_type == BLESubscriptionNone) {
    // None is always supported
    return true;
  }
  BLESubscription supported = BLESubscriptionNone;
  if (supported_properties & BLEAttributePropertyNotify) {
    supported |= BLESubscriptionNotifications;
  }
  if (supported_properties & BLEAttributePropertyIndicate) {
    supported |= BLESubscriptionIndications;
  }
  // Mask out the unsupported type bits:
  *subscription_type &= supported;
  return (*subscription_type != BLESubscriptionNone);
}

// -------------------------------------------------------------------------------------------------

static void prv_remove_subscription(GAPLEConnection *connection,
                                    GATTClientSubscriptionNode *subscription) {
  list_remove(&subscription->node,
              (ListNode **) &connection->gatt_subscriptions, NULL);
  kernel_free(subscription);
}

// -------------------------------------------------------------------------------------------------

static BTErrno prv_subscribe(BLECharacteristic characteristic_ref,
                      BLESubscription subscription_type,
                      GAPLEClient client, bool is_cleaning_up) {
  BLESubscription previous_prevailing_type = BLESubscriptionNone;
  GAPLEConnection *connection;
  uint8_t supported_properties;
  uint16_t att_handle;
  BLEDescriptor cccd_ref =
      gatt_client_accessors_find_cccd_with_characteristic(characteristic_ref, &supported_properties,
                                                          &att_handle, &connection);
  if (cccd_ref == BLE_DESCRIPTOR_INVALID || !connection) {
    // Invalid characteristic or characteristic does not have a CCCD
    return BTErrnoInvalidParameter;
  }

  if (!prv_sanitize_subscription_type(&subscription_type, supported_properties)) {
    // Unsupported subscription type
    return BTErrnoInvalidParameter;
  }

  // Try to find existing subscription
  GATTClientSubscriptionNode *subscription =
                           prv_find_subscription_for_characteristic(characteristic_ref, connection);
  bool did_create_new_subscription = false;
  if (subscription) {
    if (subscription->subscriptions[client] == subscription_type) {
      // Already subscribed
      return BTErrnoInvalidState;
    }
    if (subscription->pending_confirmation[client] && !is_cleaning_up) {
      // Already a pending subscription in flight...
      return BTErrnoInvalidState;
    }
    previous_prevailing_type = prv_prevailing_subscription_type(subscription);
  } else {
    if (subscription_type == BLESubscriptionNone) {
      // No subscription, so nothing to unsubscribe from...
      return BTErrnoInvalidState;
    }
    // No subscriptions for the characteristic yet, go create one:
    subscription = (GATTClientSubscriptionNode *) kernel_malloc(sizeof(GATTClientSubscriptionNode));
    if (!subscription) {
      // OOM
      return BTErrnoNotEnoughResources;
    }
    // Initialize it:
    *subscription = (const GATTClientSubscriptionNode) {
      .characteristic = characteristic_ref,
      .att_handle = att_handle,
    };
    // Prepend to the list of subscriptions of the connection:
    ListNode *head = &connection->gatt_subscriptions->node;
    connection->gatt_subscriptions =
                             (GATTClientSubscriptionNode *) list_prepend(head, &subscription->node);

    PBL_LOG(LOG_LEVEL_DEBUG, "Added BLE subscription for handle 0x%x", att_handle);
    did_create_new_subscription = true;
  }

  // Keeping this around in case the write fails:
  const BLESubscription previous_type = subscription->subscriptions[client];

  // Update the client state:
  subscription->subscriptions[client] = subscription_type;

  // Manage the GATT subscription state:
  BTErrno ret_val = BTErrnoOK;
  bool has_pending_write = prv_has_pending_cccd_write(subscription);
  const BLESubscription next_prevailing_type = prv_prevailing_subscription_type(subscription);
  if (next_prevailing_type != previous_prevailing_type) {
    // The subscription type changed for this characteristic:

    // Write to the Client Configuration Characteristic Descriptor on the
    // remote to change the subscription:
    const uint16_t value = subscription_type;
    ret_val = gatt_client_op_write_descriptor_cccd(cccd_ref, &value);

    if (ret_val != BTErrnoOK) {
      // Write failed, bail out!
      if (did_create_new_subscription) {
        // Clean up...
        prv_remove_subscription(connection, subscription);
      } else {
        // ... or restore previous state:
        subscription->subscriptions[client] = previous_type;
      }
      return ret_val;
    }

    has_pending_write = true;
  }

  // Manage the client buffer:
  if (subscription_type == BLESubscriptionNone) {
    // Decrement retain count, or free:
    prv_release_buffer(client);
  } else {
    // Increment retain count, or create buffer:
    if (!prv_retain_buffer(client)) {
      // Failed to create buffer, abort!
      if (did_create_new_subscription) {
        prv_remove_subscription(connection, subscription);
      }
      return BTErrnoNotEnoughResources;
    }
  }

  if (ret_val == BTErrnoOK && !is_cleaning_up) {
    if (subscription_type == BLESubscriptionNone || !has_pending_write) {
      // When unsubscribing or when Pebble was already subscribed,
      // immediately send unsubscription confirmation event to client:
      prv_send_subscription_event(characteristic_ref, ~gap_le_pebble_task_bit_for_client(client),
                                  subscription_type, BLEGATTErrorSuccess);
    } else {
      // When subscribing, wait for the CCCD Write Response before sending the confirmation event
      // to the client.
      subscription->pending_confirmation[client] = true;
    }
  }

  if (next_prevailing_type == BLESubscriptionNone) {
    // No more subscribers or CCCD write failed, free the node:
    prv_remove_subscription(connection, subscription);
  }

  return ret_val;
}

BTErrno gatt_client_subscriptions_subscribe(BLECharacteristic characteristic_ref,
                                            BLESubscription subscription_type,
                                            GAPLEClient client) {
  bt_lock();
  BTErrno ret_val = prv_subscribe(characteristic_ref, subscription_type, client,
                                  false /* is_cleaning_up */);
  bt_unlock();
  return ret_val;
}

// -------------------------------------------------------------------------------------------------

bool prv_cleanup_subscriptions_for_client(GAPLEConnection *connection, void *data) {
  const GAPLEClient client = (const GAPLEClient)(uintptr_t) data;
  GATTClientSubscriptionNode *subscription = connection->gatt_subscriptions;
  while (subscription) {
    GATTClientSubscriptionNode *next_subscription =
                                             (GATTClientSubscriptionNode *) subscription->node.next;
    // If subscribed, unsubscribe:
    if (subscription->subscriptions[client] != BLESubscriptionNone) {
      prv_subscribe(subscription->characteristic, BLESubscriptionNone, client,
                    true /* is_cleaning_up */);
    }
    subscription = next_subscription;
  }
  return false /* should_stop */;
}

void gatt_client_subscriptions_cleanup_by_client(GAPLEClient client) {
  bt_lock();
  {
    // Walk all the connections to find subscriptions to unsubscribe:
    gap_le_connection_find(prv_cleanup_subscriptions_for_client, (void *)(uintptr_t) client);
  }
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------

void gatt_client_subscriptions_cleanup_by_connection(struct GAPLEConnection *connection,
                                                     bool should_unsubscribe) {
  bt_lock();
  {
    GATTClientSubscriptionNode *node = connection->gatt_subscriptions;
    while (node) {
      GATTClientSubscriptionNode *next = (GATTClientSubscriptionNode *) node->node.next;
      // Decrement circular buffer retain count:
      for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
        if (node->subscriptions[c] != BLESubscriptionNone) {
          if (should_unsubscribe) {
            // The connection is not gone, so unsubscribe for this client, this will also
            // free the GATTClientSubscriptionNode when both clients are unsubscribed:
            prv_subscribe(node->characteristic, BLESubscriptionNone, c,
                          true /* is_cleaning_up */);
          } else {
            // Just release the buffer on behalf of the subscription
            prv_release_buffer(c);
          }
        }
      }

      if (!should_unsubscribe) {
        // Just free the node and don't bother unsubscribing:
        kernel_free(node);
      }
      node = next;
    }
    connection->gatt_subscriptions = NULL;
  }
  bt_unlock();
}

void gatt_client_subscription_cleanup_by_att_handle_range(
    struct GAPLEConnection *connection, ATTHandleRange *range) {

  bt_lock();
  {
    GATTClientSubscriptionNode *node = connection->gatt_subscriptions;

    while (node) {
      GATTClientSubscriptionNode *next = (GATTClientSubscriptionNode *) node->node.next;

      if (node->att_handle >= range->start && node->att_handle <= range->end) {
        for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
          prv_subscribe(node->characteristic, BLESubscriptionNone, c,
                      true);
        }
      }
      node = next;
    }
  }
  bt_unlock();
}

void gatt_client_subscription_boot(void) {
  s_gatt_client_subscriptions_mutex = mutex_create_recursive();
  s_gatt_client_subscriptions_semphr = xSemaphoreCreateBinary();
  PBL_ASSERTN(s_gatt_client_subscriptions_semphr);
}

//! Only for unit tests
T_STATIC bool gatt_client_get_event_pending_state(GAPLEClient client) {
  return s_is_notification_event_pending[client];
}

//! Only for unit tests
SemaphoreHandle_t gatt_client_subscription_get_semaphore(void) {
  return s_gatt_client_subscriptions_semphr;
}

//! Only for unit tests
void gatt_client_subscription_cleanup(void) {
  mutex_destroy((PebbleMutex *)s_gatt_client_subscriptions_mutex);
  s_gatt_client_subscriptions_mutex = NULL;
  vSemaphoreDelete(s_gatt_client_subscriptions_semphr);
  s_gatt_client_subscriptions_semphr = NULL;
}
