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

#pragma once

#include "applib/bluetooth/ble_client.h"
#include "util/attributes.h"

#include "gap_le_task.h"

#include <bluetooth/mtu.h>

struct GAPLEConnection;

#define MAX_ATT_WRITE_PAYLOAD_SIZE (ATT_MAX_SUPPORTED_MTU - 3)
#ifdef RECOVERY_FW
// In PRF, we use a very high connection interval to make the FW update go as fast as possible.
// We're requesting between 11-21ms (see gap_le_connect_params.c). It ultimately depends on the
// master device (iOS) to decide which value to pick. So far the shortest I've seen is 15ms (not
// using the 'hack' to act like a HID device/keyboard/mouse, which will make it go down to 11ms).
// However, at 15ms notifications are getting dropped regularly already. Just to be safe, make the
// buffer in PRF really big:
#define GATT_CLIENT_SUBSCRIPTIONS_BUFFER_SIZE ((MAX_ATT_WRITE_PAYLOAD_SIZE + \
sizeof(GATTBufferedNotificationHeader)) * 6)
#else
// FIXME: https://pebbletechnology.atlassian.net/browse/PBL-11671
#define GATT_CLIENT_SUBSCRIPTIONS_BUFFER_SIZE ((MAX_ATT_WRITE_PAYLOAD_SIZE + \
sizeof(GATTBufferedNotificationHeader)) * 4)
#endif

//! Data structure representing a subscription of a specific client for
//! notifications or indications of a GATT characteristic for a specific
//! client (GAPLEClientApp or GAPLEClientKernel). The GAPLEConnection struct has
//! the head for each BLE connection.
typedef struct {
  ListNode node;

  //! The characteristic to which the client is subscribed
  BLECharacteristic characteristic;

  //! Cached ATT handle of the characteristic
  uint16_t att_handle;

  //! Array of subscription types for each client
  BLESubscription subscriptions[GAPLEClientNum];

  //! For each client, whether it is waiting for an event to confirm the subscription
  bool pending_confirmation[GAPLEClientNum];
} GATTClientSubscriptionNode;

//! Data structure representing a serialized GATT notification header.
typedef struct PACKED {
  BLECharacteristic characteristic;
  uint16_t value_length;
  uint8_t value[];
} GATTBufferedNotificationHeader;

BTErrno gatt_client_subscriptions_subscribe(BLECharacteristic characteristic,
                                            BLESubscription subscription_type,
                                            GAPLEClient client);

//! Gets the length of the next notification in the buffer that was received.
//! @param[out] header_out The header of the notification, containing the value length and the
//! characteristic reference.
//! @return True if there is a notification in the buffer, false if not
bool gatt_client_subscriptions_get_notification_header(GAPLEClient client,
                                                       GATTBufferedNotificationHeader *header_out);

//! Copies the data of the next notification and marks it as "consumed".
//! The client *MUST* keep on calling gatt_client_subscriptions_consume_notification() in a loop
//! until 0 is returned.
//! @see gatt_client_subscriptions_get_notification_value_length() to get the length of the next
//! notification.
//! @param[in,out] value_length_in_out Cannot be NULL. In: the size of the value_out buffer.
//! Out: the number of bytes copied into the value_out buffer.
//! @param[out] has_more_out Cannot be NULL. Will be set to true if there are more notifications
//! in the buffer, or to false if there are no more notifications in the buffer.
//! @return The length of the next notification's payload, if there is any (has_more_out is true),
//! undefined otherwise.
uint16_t gatt_client_subscriptions_consume_notification(BLECharacteristic *characteristic_ref_out,
                                                        uint8_t *value_out,
                                                        uint16_t *value_length_in_out,
                                                        GAPLEClient client, bool *has_more_out);

//! Indicates that the client wants to pause processing notifications and yield to keep the system
//! responsive. This puts a new event on the queue so the client can continue processing later on.
void gatt_client_subscriptions_reschedule(GAPLEClient c);

//! Unsubscribes all subscriptions associated with the client. This function
//! assumes the connection is still alive and will write to the CCCD to
//! "unsubscribe" from the remote as well, if the specified client was the
//! last one to be registered for a particular characteristic.
void gatt_client_subscriptions_cleanup_by_client(GAPLEClient client);

//! Frees the GATTClientSubscriptionNode nodes that might have been associated
//! with the connection as result of gatt_client_subscriptions_subscribe calls.
//! @param should_unsubscribe If true, the current subscriptions will be unsubscribed before
//! cleanup. If false, the current subscriptions will not be unsubscribed (this is useful when
//! the connection is already severed.) No unsubscription events will be emitted regardless of the
//! value of this argument.
void gatt_client_subscriptions_cleanup_by_connection(struct GAPLEConnection *connection,
                                                     bool should_unsubscribe);

//! Called once at boot.
void gatt_client_subscription_boot(void);
