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

#include "gap_le_connect.h"

#include "ble_log.h"
#include "comm/bluetooth_analytics.h"
#include "comm/bt_conn_mgr.h"
#include "comm/bt_lock.h"
#include "gap_le_advert.h"
#include "gap_le_connect_params.h"
#include "gap_le_connection.h"
#include "gap_le_task.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/normal/bluetooth/ble_hrm.h"
#include "system/hexdump.h"
#include "system/logging.h"
#include "system/passert.h"

#include <bluetooth/gap_le_connect.h>
#include <bluetooth/pebble_pairing_service.h>
#include <btutil/bt_device.h>
#include <btutil/sm_util.h>

#if BLE_MASTER_CONNECT_SUPPORT // FIXME: Shouldn't be needed after PBL-32761
extern unsigned int bt_stack_id(void);
#endif

//! About this module
//! -----------------
//! - Manages initiating connections to other BLE devices as a Master.
//! - Handles inbound connection events as Slave as well.
//! - Programs the Bluetooth controller's white-list with the device(s) to
//!   initiate connections to.
//! - Uses the Bluetooth controller operations "LE Create Connection" and
//!   "LE Create Connection Cancel" to start/stop initiating, using the
//!   white-list as set of devices to look out for.
//! - Exposes an internal API that lets clients register "connection intents".
//! - Connection intents survive airplane mode. This keeps the application logic
//!   simpler for developers. Otherwise they would have to watch the air-plane
//!   mode state and re-register the connection intent.
//! - Clients are currently identified by PebbleTask (later by app UUID?).
//! - Clients do not have to worry about connection intents from other clients.
//!   because the module virtualizes the connection events. For example, if
//!   a client uses the API to initiate a connection, but a connection has
//!   already been created (by another client), it will still get a connection
//!   event (pretty much immediately) as if the device just connected.
//!
//! BT 4.1 Questions
//! ----------------
//! - What happens when LE Create Connection is sent for device that is already
//!   connected as master?
//!
//! - What happens when whitelisting a resolvable address, then connecting and
//!   finding out the device is already connected?

//! Represents a client (task) that (co-)owns an intent to connect.
typedef struct {
  //! True if the client has registered this intent, false if not.
  //! Because we're using a fixed size array, this is needed to indicate whether
  //! the array element is used or not.
  bool is_used;

  //! True if the system should handle pairing / encryption reestablishment
  //! transparently first, before sending the connection event.
  bool is_pairing_required;

  //! True if the intent should be kept around until the client calls
  //! gap_le_connect_cancel(), false if the intent should be removed
  //! when the slave device disconnects.
  bool auto_reconnect;

  //! True if a connection event has been sent. False if a disconnection event
  //! has been sent or if an event has never been sent. In other words,
  //! clients start off as disconnected.
  //! For clients that have is_pairing_required set to true, the connection
  //! event only gets sent after pairing/encryption is established.
  bool connected;
} GAPLEConnectionClient;

//! Data structure to hold cached bonding info
typedef struct {
  BTBondingID id;
  BTDeviceInternal device;  // containing identity address, not connection address
  SMIdentityResolvingKey irk;
} GAPLEConnectionIntentBonding;

//! Intent to connect.
//! Each intent is "owned" by one or more clients.
typedef struct {
  ListNode node;

  //! The device to connect to.
  //! @note When using a bonding, its address will be set to the last known connection address.
  BTDeviceInternal device;

  //! Array of clients (tasks). It's fixed in size for simplicity.
  //! It's not using PebbleTask to save some RAM.
  GAPLEConnectionClient client[GAPLEClientNum];

  //! True when `bonding` exists
  bool is_bonding_based;

  //! The optional bonding info for the device to connect to. NULL when unused.
  GAPLEConnectionIntentBonding bonding[];
} GAPLEConnectionIntent;

_Static_assert(offsetof(GAPLEConnectionIntent, node) == 0,
               "ListNode must be the first field in GAPLEConnectionIntent");

typedef enum {
  GAPLEConnectionEventDisconnected,
  GAPLEConnectionEventConnectedNotEncrypted,
  GAPLEConnectionEventConnectedAndEncrypted,
} GAPLEConnectionEvent;

//! Value indicating the current BLE connectivity role to the phone, from Pebble's point of view.
typedef enum {
  GAPLERoleSlave,
  GAPLERoleMaster,
} GAPLERole;

// -------------------------------------------------------------------------------------------------
// Static Variables -- MUST be protected with bt_lock/unlock!

//! The list of connection intents.
static GAPLEConnectionIntent * s_intents;

//! True if there is a pending LE Create Connection call, false if not.
static bool s_has_pending_create_connection;

//! True if the device is currently connected as LE Slave (4.0)
static bool s_is_connected_as_slave;

//! TODO: Implement role-switching (PBL-20368)
//! This is just a placeholder / stop-gap for now that is always set to GAPLERoleSlave, so that we
//! don't accidentally act as a master (perform LE Create Connection).
static const GAPLERole s_current_role = GAPLERoleSlave;

// -------------------------------------------------------------------------------------------------
// Function prototypes

typedef void (*IntentApply)(GAPLEConnectionIntent *intent, void *data);
static void prv_apply_function_to_intents_matching_connection(const GAPLEConnection *connection,
                                                              IntentApply fp, void *data);

static bool prv_intent_matches_connection(const GAPLEConnectionIntent *intent,
                                          const GAPLEConnection *connection);
static void prv_start_connecting_if_needed(void);
static GAPLEConnectionIntent * prv_get_intent_by_device(const BTDeviceInternal *device);
static void prv_intent_remove_and_free(GAPLEConnectionIntent *intent);
static bool prv_is_intent_used(const GAPLEConnectionIntent *intent);
static bool prv_is_intent_requiring_encryption(const GAPLEConnectionIntent *intent);
static bool prv_is_intent_using_whitelist(const GAPLEConnectionIntent *intent);
static BTBondingID prv_get_bonding_id_for_intent(const GAPLEConnectionIntent *intent);
static void prv_mutate_whitelist(const BTDeviceInternal *device, bool is_adding);
static void prv_mutate_whitelist_safely(const BTDeviceInternal *device,
                                        bool is_adding);

// -------------------------------------------------------------------------------------------------

// TODO: This is basically only used by the Settings/Bluetooth UI to refresh the list.
// Need to fix this up when addressing https://pebbletechnology.atlassian.net/browse/PBL-5254
static void prv_put_legacy_connection_event(const BTDeviceInternal *device, bool is_connected) {
  PebbleEvent event = {
    .type = PEBBLE_BT_CONNECTION_EVENT,
    .bluetooth = {
      .connection = {
        .is_ble = true,
        .device = *device,
      },
    },
  };

  if (is_connected) {
    event.bluetooth.connection.state = PebbleBluetoothConnectionEventStateConnected;
  } else {
    event.bluetooth.connection.state = PebbleBluetoothConnectionEventStateDisconnected;
  }
  event_put(&event);
}

// -------------------------------------------------------------------------------------------------

static void prv_put_connection_event(PebbleTaskBitset task_mask,
                                     const BTDeviceInternal *device,
                                     uint8_t hci_reason,
                                     bool connected,
                                     BTBondingID bonding_id) {
  PebbleEvent pebble_event = {
    .type = PEBBLE_BLE_CONNECTION_EVENT,
    .task_mask = task_mask,
    .bluetooth = {
      .le = {
        .connection = {
          .bt_device_bits = device->opaque.opaque_64,
          .hci_reason = hci_reason,
          .connected = connected,
          .bonding_id = bonding_id,
        },
      },
    },
  };
  event_put(&pebble_event);
}

// -------------------------------------------------------------------------------------------------

static void prv_build_task_mask_cb(GAPLEConnectionIntent *intent, void *data) {
  PebbleTaskBitset *task_mask = (PebbleTaskBitset *) data;
  for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
    GAPLEConnectionClient *client = &intent->client[c];
    if (client->is_used) {
      *task_mask &= ~gap_le_pebble_task_bit_for_client(c);
    }
  }
}

//! extern'd for gatt.c, used to determine to what tasks a "Buffer Empty" event should be sent.
//! Helper function to build a PebbleTaskBitset task_mask of the clients' tasks that are virtually
//! connected to specified real connection and therefore need to receive events for it.
//! bt_lock is assumed to be taken before calling this function.
PebbleTaskBitset gap_le_connect_task_mask_for_connection(const GAPLEConnection *connection) {
  const PebbleTaskBitset task_mask_none = ~0;
  PebbleTaskBitset task_mask = task_mask_none;
  prv_apply_function_to_intents_matching_connection(connection, prv_build_task_mask_cb, &task_mask);
  return task_mask;
}

// -------------------------------------------------------------------------------------------------
//! Updates the state of the client (as kept by this module) and
//! sends an event to notify client tasks of any state change. Client tasks
//! that have already been notified, will not be notified again.
//! @note Upon disconnection, this function also removes and free's the intent
//! if there are no more clients that want to auto-reconnect. The caller of this
//! function should therefore not attempt to access the intent after this
//! function returns.
//! bt_lock is assumed to be taken before calling this function.
//! @return false if the intent has been cleaned-up by this function and should
//! not be accessed any longer after returning.
static bool prv_update_clients(GAPLEConnectionIntent *intent,
                               uint8_t hci_reason,
                               GAPLEConnectionEvent event) {
  const BTDeviceInternal *device = &intent->device;
  const bool connected = (event == GAPLEConnectionEventConnectedNotEncrypted ||
                          event == GAPLEConnectionEventConnectedAndEncrypted);

  // Mask to mask out all tasks
  const PebbleTaskBitset task_mask_none = ~0;

  // Un-mask tasks that not to be notified of the new state:
  PebbleTaskBitset task_mask = task_mask_none;
  for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
    GAPLEConnectionClient *client = &intent->client[c];

    if (!client->is_used) {
      continue;
    }

    // When auto-reconnection is disabled, the client is "done" after the
    // first disconnection.
    if (event == GAPLEConnectionEventDisconnected && !client->auto_reconnect) {
      // (One-shot) intents should survive air plane mode toggles:
      if (hci_reason != GAPLEConnectHCIReasonExtensionAirPlaneMode) {
        client->is_used = false;
      }
    }

    if (client->connected != connected) {
      if (client->is_pairing_required &&
          event == GAPLEConnectionEventConnectedNotEncrypted) {
        // If is_pairing_required is true, "connected & not encrypted" is an
        // in-between state that should not be reported to the client.
        continue;
      }
      // The new state needs to be communicated with this client.
      task_mask &= ~gap_le_pebble_task_bit_for_client(c);

      // Update the local state for the client. An event is sent shortly after.
      intent->client[c].connected = connected;
    }
  }

  if (task_mask != task_mask_none) {
    // Send event to the client(s) that need to be notified:
    const BTBondingID bonding_id = prv_get_bonding_id_for_intent(intent);
    prv_put_connection_event(task_mask, device, hci_reason, connected, bonding_id);
  }


  // Clean up unused intent:
  if (!prv_is_intent_used(intent)) {
    prv_intent_remove_and_free(intent);
    return false;
  }
  return true;
}

void bt_driver_handle_le_connection_handle_update_address_and_irk(const BleAddressAndIRKChange *e) {
  bt_lock();
  {
    GAPLEConnection *connection = gap_le_connection_by_device(&e->device);
    if (!connection) {
      PBL_LOG(LOG_LEVEL_ERROR,
              "Got address & IRK update for non-existent connection. "
              "Old addr:"BT_DEVICE_ADDRESS_FMT, BT_DEVICE_ADDRESS_XPLODE(e->device.address));
      goto unlock;
    }
    if (e->is_address_updated) {
      connection->device = e->new_device;
      PBL_LOG(LOG_LEVEL_INFO,
              "Updated address to "BT_DEVICE_ADDRESS_FMT". Updated IRK: %c",
              BT_DEVICE_ADDRESS_XPLODE(connection->device.address), e->is_resolved ? 'Y' : 'N');
    } else if (e->is_resolved) {
      PBL_LOG(LOG_LEVEL_INFO, "Updated IRK: %c", e->is_resolved ? 'Y' : 'N');
    }
    const SMIdentityResolvingKey *remote_irk = e->is_resolved ? &e->irk : NULL;
    if (connection->irk) {
      PBL_LOG(LOG_LEVEL_WARNING, "Connection already has IRK!?");
    }
    gap_le_connection_set_irk(connection, remote_irk);
  }
unlock:
  bt_unlock();
}

void bt_driver_handle_peer_version_info_event(const BleRemoteVersionInfoReceivedEvent *e) {
  bt_lock();

  GAPLEConnection *connection = gap_le_connection_by_device(&e->peer_address);
  if (connection) {
    const BleRemoteVersionInfo *info = &e->remote_version_info;
    connection->remote_version_info = *info;
    PBL_LOG(LOG_LEVEL_DEBUG, "Remote Vers Info: VersNr: %d, CompId: 0x%x, SubVersNr: 0x%x",
            (int)info->version_number, (int)info->company_identifier, (int)info->subversion_number);
  }

  bt_unlock();
}

//! bt_lock is assumed to be taken before calling this function.
void bt_driver_handle_le_connection_complete_event(const BleConnectionCompleteEvent *event) {
  bt_lock();
  const BleConnectionParams *params = &event->conn_params;
  PBL_LOG(LOG_LEVEL_INFO,
          "LE Conn Compl: addr="BT_DEVICE_ADDRESS_FMT", is_random_addr=%u,",
          BT_DEVICE_ADDRESS_XPLODE(event->peer_address.address),
          event->peer_address.is_random_address);
  PBL_LOG(LOG_LEVEL_INFO,
          "               hdl=%u, status=0x%02x, master=%u, %u, slave lat=%u, "
          "supervision timeout=%u, is_resolved=%c",
          event->handle, event->status, event->is_master, params->conn_interval_1_25ms,
          params->slave_latency_events, params->supervision_timeout_10ms,
          event->is_resolved ? 'Y' : 'N');

  // When an "LE Connection Complete" event is received, the
  // "LE Create Connection" operation is stopped, so update our state:
  s_has_pending_create_connection = false;

  switch (event->status) {
    case HciStatusCode_Success: {
      // New connection! Update our records:
      const bool local_is_master = event->is_master;

      if (!local_is_master) {
        s_is_connected_as_slave = true;
        gap_le_advert_handle_connect_as_slave();

        prv_put_legacy_connection_event(&event->peer_address, true /* connected */);
      }

      if (gap_le_connection_is_connected(&event->peer_address)) {
        // We have seen this crop up for cases where the phone has disconnected due to a timeout
        // but the watch has not yet. In practice, I think the only way it could happen is if a
        // user is sitting in the Bluetooth settings menu and walking in and out of range. If it
        // does take place, let's trigger a disconnect to try and put us back into a sane state
        PBL_LOG(LOG_LEVEL_ERROR,
                "Not adding connection for device. It is already connected .. disconnecting");
        bt_driver_gap_le_disconnect(&event->peer_address);
        break;
      }

      const SMIdentityResolvingKey *remote_irk = event->is_resolved ? &event->irk : NULL;
      GAPLEConnection *connection = gap_le_connection_add(&event->peer_address, remote_irk,
                                                          local_is_master);
      // Cache the BLE connection parameters
      connection->conn_params = *params;

      bool found_match = false;
      GAPLEConnectionIntent *intent = s_intents;
      while (intent) {
        GAPLEConnectionIntent *next = (GAPLEConnectionIntent *) intent->node.next;
        if (prv_intent_matches_connection(intent, connection)) {
          found_match = true;

          if (intent->is_bonding_based) {
            // Update connection address:
            intent->device = event->peer_address;

            // FIXME:
            // Find and assign bonding_id even if there is no intent.
            // https://pebbletechnology.atlassian.net/browse/PBL-20972
            connection->bonding_id = intent->bonding->id;
          }

          prv_update_clients(intent, HciStatusCode_Success,
                             GAPLEConnectionEventConnectedNotEncrypted);

          if (prv_is_intent_using_whitelist(intent)) {
            // Remove from white-list, because the device is connected now.
            prv_mutate_whitelist(&event->peer_address, false /* remove */);
          }

          if (local_is_master && prv_is_intent_requiring_encryption(intent)) {
            // TODO: kick off pairing
          }
        }
        intent = next;
      }

      if (!local_is_master) { // At the moment we don't grab analytics for connections we generate
        bluetooth_analytics_handle_connect(&event->peer_address, &event->conn_params);
      }


      if (!found_match) {
        // There is no connection intent from our end. This could be the phone that is connecting
        // for the first time. Let the connection watchdog (TODO: PBL-11236) take care of
        // disconnecting at some point, if the connection ends up being unused.
        PBL_LOG(LOG_LEVEL_INFO, "No intent for connection");
        bluetooth_analytics_handle_no_intent_for_connection();
      }

#if RECOVERY_FW
      // In PRF, stick to shortest connection interval indefinitely:
      conn_mgr_set_ble_conn_response_time(connection, BtConsumerPRF,
                                          ResponseTimeMin, MAX_PERIOD_RUN_FOREVER);
#endif
      break;
    }

    case HciStatusCode_UnknownConnectionIdentifier: {
      // Happens if "Connection Create" was cancelled.
      // See Bluetooth Spec 4.0, Volume 2, Part E, Chapter 7.8.13.
      break;
    }

    default: {
      PBL_LOG(LOG_LEVEL_ERROR, "Connection Complete Event status: 0x%x",
              event->status);
      break;
    }
  }

  // Continue initiating connections to disconnected devices:
  prv_start_connecting_if_needed();
  bt_unlock();
}

//! bt_lock is assumed to be taken before calling this function.
void bt_driver_handle_le_disconnection_complete_event(const BleDisconnectionCompleteEvent *event) {
  bt_lock();

  switch (event->status) {
    case HciStatusCode_Success: {
      // Disconnection! Update our records:
      GAPLEConnection *connection = gap_le_connection_by_device(&event->peer_address);
#if CAPABILITY_HAS_BUILTIN_HRM
      ble_hrm_handle_disconnection(connection);
#endif
      const bool local_is_master = connection->local_is_master;

      PBL_LOG(LOG_LEVEL_INFO, "LE Disconn: addr="BT_DEVICE_ADDRESS_FMT", is_random_addr=%u,",
              BT_DEVICE_ADDRESS_XPLODE(event->peer_address.address),
              event->peer_address.is_random_address);
      PBL_LOG(LOG_LEVEL_INFO, "            hdl=%u, status=0x%02x, reason=0x%02x, master=%u",
              event->handle, event->status, event->reason, local_is_master);

      bluetooth_analytics_handle_disconnect(local_is_master);

      bluetooth_analytics_handle_connection_disconnection_event(
          AnalyticsEvent_BtLeDisconnect, event->reason, &connection->remote_version_info);

      if (!local_is_master) {
        s_is_connected_as_slave = false;
        gap_le_advert_handle_disconnect_as_slave();

        prv_put_legacy_connection_event(&event->peer_address, false /* disconnected */);
      }

      GAPLEConnectionIntent *intent = s_intents;
      while (intent) {
        GAPLEConnectionIntent *next = (GAPLEConnectionIntent *) intent->node.next;
        if (prv_intent_matches_connection(intent, connection)) {
          // Notify clients:
          if (prv_update_clients(intent, event->reason,
                                 GAPLEConnectionEventDisconnected)) {
            // Only if the intent hasn't been cleaned up by now:
            if (prv_is_intent_using_whitelist(intent)) {
              // Add to white-list, because the device is disconnected now and we
              // need to start connecting again:
              prv_mutate_whitelist_safely(&event->peer_address, true /* add */);
            }

            if (intent->is_bonding_based) {
              // Clear out connection address (more for debugging than any else):
              intent->device = (const BTDeviceInternal) {};
            }
          }
        }
        intent = next;
      }

      gap_le_connection_remove(&event->peer_address);
      break;
    }

    default: {
      PBL_LOG(LOG_LEVEL_ERROR, "Disconnection Complete Event status: 0x%x",
              event->status);
      break;
    }
  }
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------
//! Convenience function to apply a function to each intent matching or resolving to a device
//! bt_lock is assumed to be taken before calling this function.
static void prv_apply_function_to_intents_matching_connection(const GAPLEConnection *connection,
                                                              IntentApply fp, void *data) {
  GAPLEConnectionIntent *intent = s_intents;
  while (intent) {
    GAPLEConnectionIntent *next = (GAPLEConnectionIntent *) intent->node.next;
    if (prv_intent_matches_connection(intent, connection)) {
      fp(intent, data);
    }
    intent = next;
  }
}

// -------------------------------------------------------------------------------------------------
//! Helper for prv_handle_encryption_change

static void prv_send_clients_encrypted_event(GAPLEConnectionIntent *intent, void *unused_data) {
  prv_update_clients(intent, HciStatusCode_Success,
                     GAPLEConnectionEventConnectedAndEncrypted);
}

//! bt_lock is assumed to be taken before calling this function.
void bt_driver_handle_le_encryption_change_event(const BleEncryptionChange *event) {
  bt_lock();
  const bool is_encrypted = (event->encryption_enabled);
  if (!is_encrypted) {
    // The "Encryption Change" event can only enable encryption, there's no inverse,
    // so there must be an error:
    PBL_LOG(LOG_LEVEL_ERROR, "Encryption change failed: %u", event->status);
    goto unlock;
  }

  // Bluetopia doesn't set the 'is_random_address' field in the encryption change event, so using
  // gap_le_connection_by_device() will fail.
  GAPLEConnection *connection = gap_le_connection_by_addr(&event->dev_address);
  if (connection->is_encrypted) {
    PBL_LOG(LOG_LEVEL_INFO, "Encryption refreshed!");
    goto unlock;
  }

  const bool local_is_master = connection->local_is_master;
  connection->is_encrypted = true;

  if (!local_is_master) {
    PBL_LOG(LOG_LEVEL_INFO, "Hurray! LE Security established.");
    bluetooth_analytics_handle_encryption_change();
    bt_driver_pebble_pairing_service_handle_status_change(connection);
  }

  prv_apply_function_to_intents_matching_connection(connection,
                                                    prv_send_clients_encrypted_event, NULL);
unlock:
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------
//! Wrappers around Bluetopia HCI / GAP calls - Not compiled at the moment, fix in PBL-32761
//! bt_lock is assumed to be taken before calling these functions.

static void prv_start_connecting(void) {
#if !BLE_MASTER_CONNECT_SUPPORT // PBL-32761
  PBL_LOG(LOG_LEVEL_WARNING, "Watch driven BLE connection unimplemented");
#else
  if (s_has_pending_create_connection) {
    PBL_LOG(LOG_LEVEL_ERROR, "Already connecting...");
    return;
  }

  BLE_GAP_LOG_DEBUG("Starting connecting..");
  unsigned int stack_id = bt_stack_id();
  // See Bluetooth Spec 4.0, Volume 2, Part E, Chapter 7.8.12:
  const GAP_LE_Address_Type_t local_addr_type = BleAddressType_Random;
  GAP_LE_Connection_Parameters_t connection_params = {
    .Connection_Interval_Min = 40,
    .Connection_Interval_Max = 60,
    .Slave_Latency = 0,
    .Supervision_Timeout = 6000,
    .Minimum_Connection_Length = 0,
    .Maximum_Connection_Length = 40950,
  };
  const int r = GAP_LE_Create_Connection(stack_id, 10240, 10240, fpWhiteList,
                                   0 /* fpWhiteList ignores remote addr type */,
                                   NULL /* fpWhiteList ignores remote addr */,
                                   local_addr_type, &connection_params,
                                   gap_le_connect_bluetopia_connection_callback,
                                   0 /* callback context: unused */);
  if (r) {
    PBL_LOG(LOG_LEVEL_ERROR, "GAP_LE_Create_Connection (r=%d)", r);
  } else {
    s_has_pending_create_connection = true;
  }
#endif
}

static void prv_stop_connecting(void) {
#if !BLE_MASTER_CONNECT_SUPPORT // PBL-32761
  PBL_LOG(LOG_LEVEL_WARNING, "Watch driven BLE connection cancel unimplemented");
#else
  if (!s_has_pending_create_connection) {
    return;
  }
  unsigned int stack_id = bt_stack_id();
  BLE_GAP_LOG_DEBUG("Stopping connecting...");
  // See Bluetooth Spec 4.0, Volume 2, Part E, Chapter 7.8.13:
  const int r = GAP_LE_Cancel_Create_Connection(stack_id);
  if (r) {
    PBL_LOG(LOG_LEVEL_ERROR, "GAP_LE_Cancel_Create_Connection (r=%d)", r);
  } else {
    // Update the state right away (don't wait for the Connection Complete event
    // with HCI_ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER status):
    s_has_pending_create_connection = false;
  }
#endif
}

static void prv_mutate_whitelist(const BTDeviceInternal *device, bool is_adding) {
#if !BLE_MASTER_CONNECT_SUPPORT // PBL-32761
  PBL_LOG(LOG_LEVEL_WARNING, "BLE whitelist mutation unimplemented");
#else
  unsigned int stack_id = bt_stack_id();
  BLE_GAP_LOG_DEBUG("Mutating white-list (adding=%u): " BD_ADDR_FMT,
                    is_adding, BT_DEVICE_ADDRESS_XPLODE(device->address));
  // See Bluetooth Spec 4.0, Volume 2, Part E, Chapter 7.8.15:
  uint8_t status = 0;
  const uint8_t addr_type = device->is_random_address ? 0x01 : 0x00;
  __typeof__(&HCI_LE_Add_Device_To_White_List) mutator =
        (is_adding ? HCI_LE_Add_Device_To_White_List : HCI_LE_Remove_Device_From_White_List);
  const int r = mutator(stack_id, addr_type,
                        BTDeviceAddressToBDADDR(device->address), &status);
  if (r) {
    PBL_LOG(LOG_LEVEL_ERROR,
            "HCI_LE_..._Device_To_White_List (is_adding=%u, r=%d, status=0x%x)",
            is_adding, r, status);
  }
#endif
}

// -------------------------------------------------------------------------------------------------
//! Helpers to manage the s_intents list.
//! bt_lock() is expected to be taken by the caller for each of these.

static bool prv_intent_matches_connection(const GAPLEConnectionIntent *intent,
                                          const GAPLEConnection *connection) {
  if (intent->is_bonding_based) {
    // If the bonding-based intent is connected, the `device` is set to the connection address,
    // if it's not connected, it's all zeroes.
    if (bt_device_equal(&connection->device.opaque, &intent->device.opaque)) {
      return true;
    }
    if (!connection->irk) {
      // are we looking for a bonding which did not exchange an irk?
      if (sm_is_pairing_info_irk_not_used(&intent->bonding->irk)) {
        PBL_LOG(LOG_LEVEL_DEBUG, "Bonding does not have irk ... comparing identity address");
        return (0 == memcmp(&connection->device.opaque, &intent->bonding->device.opaque,
                            sizeof(connection->device.opaque)));
      }

      return false;
    }
    return (0 == memcmp(connection->irk, &intent->bonding->irk, sizeof(*connection->irk)));
  } else {
    return bt_device_equal(&connection->device.opaque, &intent->device.opaque);
  }
}

static void prv_intent_remove_and_free(GAPLEConnectionIntent *intent) {
  list_remove(&intent->node, (ListNode **) &s_intents, NULL);
  kernel_free(intent);
}

static bool prv_intent_filter_by_device(ListNode *node, void *data) {
  const BTDeviceInternal *target_device = (const BTDeviceInternal *) data;
  const GAPLEConnectionIntent *intent = (const GAPLEConnectionIntent *) node;
  if (intent->is_bonding_based) {
    return false;
  }
  return bt_device_equal(&target_device->opaque, &intent->device.opaque);
}

static GAPLEConnectionIntent * prv_get_intent_by_device(const BTDeviceInternal *device) {
  return (GAPLEConnectionIntent *) list_find(&s_intents->node,
                                             prv_intent_filter_by_device,
                                             (void *) device);
}

static bool prv_intent_filter_by_bonding_id(ListNode *node, void *data) {
  const BTBondingID bonding_id = (BTBondingID) (uintptr_t) data;
  const GAPLEConnectionIntent *intent = (const GAPLEConnectionIntent *) node;
  if (!intent->is_bonding_based) {
    return false;
  }
  return (intent->bonding->id == bonding_id);
}

static GAPLEConnectionIntent * prv_get_intent_by_bonding_id(BTBondingID bonding_id) {
  return (GAPLEConnectionIntent *) list_find(&s_intents->node,
                                             prv_intent_filter_by_bonding_id,
                                             (void *) (uintptr_t) bonding_id);
}

static bool prv_intent_filter_disconnected(ListNode *node, void *data) {
  const GAPLEConnectionIntent *intent = (const GAPLEConnectionIntent *) node;
  return !gap_le_connection_is_connected(&intent->device);
}

static bool prv_has_intents_for_disconnected_devices(void) {
  return list_find(&s_intents->node, prv_intent_filter_disconnected, NULL);
}

static uint32_t prv_intents_count(void) {
  return list_count(&s_intents->node);
}

static bool prv_is_intent_used(const GAPLEConnectionIntent *intent) {
  return (intent->client[GAPLEClientKernel].is_used |
          intent->client[GAPLEClientApp].is_used);
}

// -------------------------------------------------------------------------------------------------
static bool prv_is_intent_requiring_encryption(const GAPLEConnectionIntent *intent) {
  return (intent->client[GAPLEClientKernel].is_pairing_required ||
          intent->client[GAPLEClientApp].is_pairing_required);
}

// -------------------------------------------------------------------------------------------------
static bool prv_is_intent_using_whitelist(const GAPLEConnectionIntent *intent) {
  // TODO: If the bonding does not contain a valid IRK, perhaps we should use and whitelist the
  // identity address and treat it as a normal connection intent?
  // See note in BT spec "Note: An all zero Identity Resolving Key data field indicates that a
  // device does not have a valid resolvable private address." in Security Manager chapter.
  return (!intent->is_bonding_based);
}

// -------------------------------------------------------------------------------------------------
static BTBondingID prv_get_bonding_id_for_intent(const GAPLEConnectionIntent *intent) {
  if (intent->is_bonding_based) {
    return intent->bonding->id;
  }
  return BT_BONDING_ID_INVALID;
}

// -------------------------------------------------------------------------------------------------
static void prv_start_connecting_if_needed(void) {
  if (s_current_role == GAPLERoleSlave) {
    return;
  }
  if (prv_has_intents_for_disconnected_devices()) {
    prv_start_connecting();
  }
}

// -------------------------------------------------------------------------------------------------
//! Adds or removes a device to/from the Bluetooth controller's whitelist.
//! Stops and (re)starts the LE Create Connection operation as necessary.
static void prv_mutate_whitelist_safely(const BTDeviceInternal *device,
                                        bool is_adding) {
  // If there are already connection intents, cancel connecting briefly,
  // otherwise it's illegal to modify the white-list.
  prv_stop_connecting();

  // Mutate white-list:
  prv_mutate_whitelist(device, is_adding);

  // Start/continue connecting:
  prv_start_connecting_if_needed();
}

// -------------------------------------------------------------------------------------------------
// Helper data structure for prv_register_intent

struct RegisterIntentRequest {
  bool is_bonding_based;
  union {
    const BTDeviceInternal *device;
    GAPLEConnectionIntentBonding bonding;
  };
};

// -------------------------------------------------------------------------------------------------
//! Registers a connection intent for a client task
//! bt_lock() is expected to be taken by the caller
static BTErrno prv_register_intent(struct RegisterIntentRequest *request,
                                   bool auto_reconnect,
                                   bool is_pairing_required,
                                   GAPLEClient c) {
  // Check if the max count wasn't exceeded:
  const uint32_t prev_num_intents = prv_intents_count();
  if (prev_num_intents >= GAP_LE_CONNECT_MASTER_MAX_CONNECTION_INTENTS) {
    return BTErrnoNotEnoughResources;
  }

  bool is_already_connected = false;
  bool is_already_encrypted = false;
  bool local_is_master = false;

  const BTDeviceInternal *connected_device = NULL;
  GAPLEConnectionIntent *intent;

  if (request->is_bonding_based) {
    const GAPLEConnection *connection = gap_le_connection_find_by_irk(&request->bonding.irk);
    if (!connection) {
      if (sm_is_pairing_info_irk_not_used(&request->bonding.irk)) {
        PBL_LOG(LOG_LEVEL_DEBUG, "register_intent: IRK not used, searching by addr");
        connection = gap_le_connection_by_device(&request->bonding.device);
      }
    }
    if (connection) {
      is_already_connected = true;
      is_already_encrypted = connection->is_encrypted;
      local_is_master = connection->local_is_master;

      connected_device = &connection->device;
    }
    intent = prv_get_intent_by_bonding_id(request->bonding.id);
  } else {
    is_already_connected = gap_le_connection_is_connected(request->device);
    intent = prv_get_intent_by_device(request->device);
  }

  const bool is_existing_intent = (intent != NULL);
  if (is_existing_intent) {
    if (intent->client[c].is_used) {
      return BTErrnoInvalidState;
    }
  } else {
    // Create intent for device and add to list:
    const size_t alloc_size = sizeof(GAPLEConnectionIntent) +
                             (request->is_bonding_based ? sizeof(GAPLEConnectionIntentBonding) : 0);
    intent = (GAPLEConnectionIntent *) kernel_malloc(alloc_size);
    if (!intent) {
      return BTErrnoNotEnoughResources;
    }
    memset(intent, 0, alloc_size);
    s_intents = (GAPLEConnectionIntent *) list_prepend(&s_intents->node,
                                                       &intent->node);

    if (request->is_bonding_based) {
      // Create bonding info cache if it has not been created yet:
      intent->is_bonding_based = true,
      *intent->bonding = request->bonding;
      if (connected_device) {
        intent->device = *connected_device;
      }
    } else {
      intent->device = *request->device;
      // Append to hardware white-list of BT chip if not connected:
      if (!is_already_connected) {
        prv_mutate_whitelist_safely(request->device, true /* add */);
      }
    }
  }

  intent->client[c].is_used = true;
  intent->client[c].auto_reconnect = auto_reconnect;
  intent->client[c].is_pairing_required = is_pairing_required;
  intent->client[c].connected = false;  // starting state

  if (!is_already_connected) {
    return BTErrnoOK;
  }

  if (is_pairing_required && !is_already_encrypted) {
    if (local_is_master) {
      // TODO:
      // - Check if pairing process is on-going, if so, do nothing
      // - If not on-going, kick it off (we're the master)
      // See https://pebbletechnology.atlassian.net/browse/PBL-6850
    } else {
      // Pebble is slave, the other side should start pairing.
      // Connection watchdog should take care of disconnecting after timeout in case pairing
      // does not happen in a timely manner
      // TODO: https://pebbletechnology.atlassian.net/browse/PBL-11236
    }
    return BTErrnoOK;
  }

  // Notify client of the virtual connection:
  prv_update_clients(intent, HciStatusCode_Success,
                     is_already_encrypted ? GAPLEConnectionEventConnectedAndEncrypted :
                                    GAPLEConnectionEventConnectedNotEncrypted);

  return BTErrnoOK;
}

// -------------------------------------------------------------------------------------------------
//! Unregisters a connection intent for a client task
//! bt_lock() is expected to be taken by the caller
static BTErrno prv_unregister_intent(GAPLEConnectionIntent *intent,
                                     GAPLEClient c,
                                     bool should_send_disconnection_event,
                                     uint8_t hci_reason) {
  if (!intent->client[c].is_used) {
    // No intent that is owned by the given client
    return BTErrnoInvalidParameter;
  }

  // Only send disconnection event if a connection event has been sent to the
  // client in the past:
  const bool is_connected_virtual = intent->client[c].connected;
  should_send_disconnection_event &= is_connected_virtual;

  const BTDeviceInternal *device = &intent->device;
  const bool is_connected_real = gap_le_connection_is_connected(device);

  const BTBondingID bonding_id = prv_get_bonding_id_for_intent(intent);

  // Flag as unused:
  intent->client[c].is_used = false;

  bool should_remove_and_free = false;

  if (!prv_is_intent_used(intent)) {
    should_remove_and_free = true;

    if (is_connected_real) {
      // Disconnect the device because no one is using it
      const int result = bt_driver_gap_le_disconnect(device);
      if (result != 0) {
        PBL_LOG(LOG_LEVEL_ERROR, "Ble disconnect failed: %d", result);
      }
    } else {
      if (prv_is_intent_using_whitelist(intent)) {
        // Remove from white-list:
        prv_mutate_whitelist_safely(device, false /* remove */);
      }
    }
  }

  if (should_send_disconnection_event) {
    // Send virtual disconnection event:
    const PebbleTaskBitset task_mask = ~gap_le_pebble_task_bit_for_client(c);
    prv_put_connection_event(task_mask, device, hci_reason, false /* connected */, bonding_id);
  }

  if (should_remove_and_free) {
    // Delete the intent:
    prv_intent_remove_and_free(intent);
  }

  return BTErrnoOK;
}

// -------------------------------------------------------------------------------------------------

void gap_le_connect_handle_bonding_change(BTBondingID bonding_id, BtPersistBondingOp op) {
  // Load from flash outside of the bt_lock() block:
  GAPLEConnectionIntentBonding updated_bonding;
  if (op == BtPersistBondingOpDidChange) {
    if (!bt_persistent_storage_get_ble_pairing_by_id(bonding_id, &updated_bonding.irk,
                                              &updated_bonding.device, NULL)) {
      WTF;
    }
  }

  bt_lock();
  GAPLEConnectionIntent *intent = prv_get_intent_by_bonding_id(bonding_id);
  if (!intent) {
    goto unlock;
  }
  switch (op) {
    case BtPersistBondingOpDidAdd:
      // FIXME:
      break;
    case BtPersistBondingOpDidChange:
      *intent->bonding = updated_bonding;
      break;
    case BtPersistBondingOpWillDelete:
      for (GAPLEClient c = GAPLEClientKernel; c < GAPLEClientNum; ++c) {
        if (!intent->client[c].is_used) {
          continue;
        }
        prv_unregister_intent(intent, c, true /* should_send_disconnection_event */,
                              GAPLEConnectHCIReasonExtensionUserRemovedBonding);
      }
      break;
    default:
      WTF;
  }
unlock:
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------

BTErrno gap_le_connect_connect(const BTDeviceInternal *device, bool auto_reconnect,
                       bool is_pairing_required, GAPLEClient client) {
  if (!device || client >= GAPLEClientNum) {
    return BTErrnoInvalidParameter;
  }
  bt_lock();
  struct RegisterIntentRequest request = {
    .is_bonding_based = false,
    .device = device,
  };
  BTErrno ret_value = prv_register_intent(&request, auto_reconnect, is_pairing_required, client);
  bt_unlock();
  return ret_value;
}

// -------------------------------------------------------------------------------------------------

BTErrno gap_le_connect_cancel(const BTDeviceInternal *device,
                                     GAPLEClient client) {
  if (!device || client >= GAPLEClientNum) {
    return BTErrnoInvalidParameter;
  }
  bt_lock();
  // Find the intent for the device:
  GAPLEConnectionIntent *intent = prv_get_intent_by_device(device);
  BTErrno ret_value;
  if (!intent) {
    // No intent for given device
    ret_value = BTErrnoInvalidParameter;
  } else {
    ret_value = prv_unregister_intent(intent, client, true /* should_send_disconnection_event */,
                                      GAPLEConnectHCIReasonExtensionCancelConnect);
  }
  bt_unlock();
  return ret_value;
}

// -------------------------------------------------------------------------------------------------

BTErrno gap_le_connect_connect_by_bonding(BTBondingID bonding_id, bool auto_reconnect,
                                          bool is_pairing_required, GAPLEClient client) {
  if (bonding_id == BT_BONDING_ID_INVALID || client >= GAPLEClientNum) {
    return BTErrnoInvalidParameter;
  }
  struct RegisterIntentRequest request = {
    .is_bonding_based = true,
    .bonding = {
      .id = bonding_id,
    },
  };
  // Get the IRK and device from the bonding storage,
  // outside of bt_lock(), because it uses flash.
  if (!bt_persistent_storage_get_ble_pairing_by_id(bonding_id, &request.bonding.irk,
                                             &request.bonding.device, NULL)) {
    return BTErrnoInvalidParameter;
  }
  bt_lock();
  BTErrno ret_value = prv_register_intent(&request, auto_reconnect, is_pairing_required, client);
  bt_unlock();
  return ret_value;
}

// -------------------------------------------------------------------------------------------------

BTErrno gap_le_connect_cancel_by_bonding(BTBondingID bonding_id, GAPLEClient client) {
  if (bonding_id == BT_BONDING_ID_INVALID || client >= GAPLEClientNum) {
    return BTErrnoInvalidParameter;
  }
  bt_lock();
  // Find the intent for the device:
  GAPLEConnectionIntent *intent = prv_get_intent_by_bonding_id(bonding_id);
  BTErrno ret_value;
  if (!intent) {
    // No intent for given device
    ret_value = BTErrnoInvalidParameter;
  } else {
    ret_value = prv_unregister_intent(intent, client, true /* should_send_disconnection_event */,
                                      GAPLEConnectHCIReasonExtensionCancelConnect);
  }
  bt_unlock();
  return ret_value;
}

// -------------------------------------------------------------------------------------------------

void gap_le_connect_cancel_all(GAPLEClient client) {
  bt_lock();
  {
    BLE_GAP_LOG_DEBUG("Cancel connecting all for client %u...", client);

    GAPLEConnectionIntent *intent = s_intents;
    while (intent) {
      GAPLEConnectionIntent *next = (GAPLEConnectionIntent *) intent->node.next;
      prv_unregister_intent(intent, client, false /* should_send_disconnection_event */,
                            GAPLEConnectHCIReasonExtensionCancelConnect);
      intent = next;
    }
  }
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------

bool gap_le_connect_is_connected_as_slave(void) {
  bool connected;
  bt_lock();
  {
    connected = s_is_connected_as_slave;
  }
  bt_unlock();
  return connected;
}


// -------------------------------------------------------------------------------------------------

void gap_le_connect_init(void) {
  bt_lock();
  {
    GAPLEConnectionIntent *intent = s_intents;
    while (intent) {
      GAPLEConnectionIntent *next = (GAPLEConnectionIntent *) intent->node.next;
      prv_mutate_whitelist(&intent->device, true /* add */);
      intent = next;
    }

    prv_start_connecting_if_needed();
  }
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------

void gap_le_connect_deinit(void) {
  bt_lock();
  {
    s_has_pending_create_connection = false;

    // Going into air-plane mode, send virtual disconnection events:
    GAPLEConnectionIntent *intent = s_intents;
    while (intent) {
      GAPLEConnectionIntent *next = (GAPLEConnectionIntent *) intent->node.next;
      prv_update_clients(intent, GAPLEConnectHCIReasonExtensionAirPlaneMode,
                         GAPLEConnectionEventDisconnected);
      intent = next;
    }

    if (s_is_connected_as_slave) {
      // The BT controller will not send an etLE_Disconnection_Complete event
      // when going to airplane mode while being connected.
      // Stop analytics stopwatches manually:
      bluetooth_analytics_handle_disconnect(false);
      s_is_connected_as_slave = false;
    }
  }
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------
// For unit testing

bool gap_le_connect_has_pending_create_connection(void) {
  bt_lock();
  const bool ret_value = s_has_pending_create_connection;
  bt_unlock();
  return ret_value;
}

bool gap_le_connect_has_connection_intent(const BTDeviceInternal *device,
                                         GAPLEClient c) {
  bt_lock();
  bool ret_value = true;
  const GAPLEConnectionIntent *intent = prv_get_intent_by_device(device);
  if (intent) {
    if (!intent->client[c].is_used) {
      // Not all specified clients own the intent
      ret_value = false;
    }
  } else {
    // Intent not found
    ret_value = false;
  }
  bt_unlock();
  return ret_value;
}

bool gap_le_connect_has_connection_intent_for_bonding(BTBondingID bonding_id,
                                                      GAPLEClient c) {
  bt_lock();
  bool ret_value = true;
  const GAPLEConnectionIntent *intent = prv_get_intent_by_bonding_id(bonding_id);
  if (intent) {
    if (!intent->client[c].is_used) {
      // Not all specified clients own the intent
      ret_value = false;
    }
  } else {
    // Intent not found
    ret_value = false;
  }
  bt_unlock();
  return ret_value;
}

uint32_t gap_le_connect_connection_intents_count(void) {
  bt_lock();
  const uint32_t count =  prv_intents_count();
  bt_unlock();
  return count;
}
