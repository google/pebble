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

#include "bluetooth/gap_le_connect.h"
#include "comm/ble/gap_le_connect.h"
#include "comm/ble/gap_le_connection.h"
#include "comm/ble/gap_le_task.h"

#include "kernel/events.h"
#include "services/common/analytics/analytics.h"

#include "clar.h"

#include <bluetooth/bonding_sync.h>
#include <bluetooth/sm_types.h>
#include <btutil/bt_device.h>

// Fakes
///////////////////////////////////////////////////////////

#include "fake_events.h"
#include "fake_GAPAPI.h"
#include "fake_bluetooth_persistent_storage.h"
#include "fake_HCIAPI.h"
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_system_task.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_bluetopia_interface.h"
#include "stubs_bt_lock.h"
#include "stubs_gap_le_advert.h"
#include "stubs_bluetooth_analytics.h"
#include "stubs_gatt_client_discovery.h"
#include "stubs_gatt_client_subscriptions.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_pairing_service.h"
#include "stubs_regular_timer.h"
#include "stubs_shared_prf_storage.h"

// Note:
// The unit tests for "Pebble as Master" are disabled because role switching is not implemented yet,
// and the FW is currently "hard-wired" to be slave as a precautionary measure to prevent it from
// trying to connect as master. See PBL-20368.

void bt_driver_cb_handle_create_bonding(const BleBonding *bonding,
                                        const BTDeviceAddress *addr) {
}

void cc2564A_bad_le_connection_complete_handle(unsigned int stack_id,
                                             const GAP_LE_Current_Connection_Parameters_t *params) {
}

const GAP_LE_Pairing_Capabilities_t* gap_le_pairing_capabilities(void) {
  return NULL;
}

void gap_le_device_name_request(uintptr_t stack_id, GAPLEConnection *connection) {
}

void gatt_service_changed_server_cleanup_by_connection(GAPLEConnection *connection) {
}

void bt_driver_handle_le_conn_params_update_event(
    const BleConnectionUpdateCompleteEvent *event) {
}

typedef struct PairingUserConfirmationCtx PairingUserConfirmationCtx;

void bt_driver_pebble_pairing_service_handle_status_change(const GAPLEConnection *connection) {
}

void bt_driver_cb_pairing_confirm_handle_request(const PairingUserConfirmationCtx *ctx,
                                                 const char *device_name,
                                                 const char *confirmation_token) {
}

void bt_driver_cb_pairing_confirm_handle_completed(const PairingUserConfirmationCtx *ctx,
                                                   bool success) {
}

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

void analytics_event_bt_error(AnalyticsEvent type, uint32_t error) {
}

void bluetooth_analytics_handle_connection_disconnection_event(
    AnalyticsEvent type, uint8_t reason, const BleRemoteVersionInfo *vers_info) {
}

void analytics_inc(AnalyticsMetric metric, AnalyticsClient client) {
}

// Helpers
///////////////////////////////////////////////////////////

static BTDeviceInternal prv_dummy_device(uint8_t octet) {
  BTDeviceAddress address = {
    .octets = {
      [0] = octet,
      [1] = octet,
      [2] = octet,
      [3] = octet,
      [4] = octet,
      [5] = octet,
    },
  };
  BTDevice device = bt_device_init_with_address(address, true /* is_random */);
  return *(BTDeviceInternal *)(&device);
}

static BTBondingID prv_add_bonding_for_fake_resolvable_device(void) {
  BTDeviceInternal identity_device = {};
  const SMIdentityResolvingKey *irk = (const SMIdentityResolvingKey *) fake_GAPAPI_get_fake_irk();
  BleBonding bonding = {
    .pairing_info = {
      .identity = identity_device,
      .irk = *irk,
      .is_remote_identity_info_valid = true,
    },
    .is_gateway = true,
  };
  bt_driver_handle_host_added_bonding(&bonding);
  return fake_bt_persistent_storage_add(irk, &identity_device, "Dummy", true /* is_gateway */);
}

static void prv_assert_no_event(void) {
  PebbleEvent event = fake_event_get_last();
  cl_assert_equal_i(event.type, PEBBLE_NULL_EVENT);
}

static void prv_fake_connect(const BTDeviceInternal *device, bool is_master) {
  // Simulate getting a Connection Complete event for the device from Bluetopia:
  fake_gap_put_connection_event(HCI_ERROR_CODE_SUCCESS,
                                is_master, device);
  cl_assert_equal_b(gap_le_connection_is_connected(device), true);
}

static void prv_fake_disconnect(const BTDeviceInternal *device, bool is_master) {
  fake_gap_put_disconnection_event(HCI_ERROR_CODE_SUCCESS,
                                   HCI_ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST,
                                   is_master,
                                   device);
  cl_assert_equal_b(gap_le_connection_is_connected(device), false);
}

static void prv_assert_client_event(const BTDeviceInternal *device,
                                    bool connected,
                                    PebbleTaskBitset client_tasks,
                                    uint8_t hci_reason) {
  // Verify the Pebble event:
  PebbleEvent event = fake_event_get_last();
  cl_assert_equal_i(event.type, PEBBLE_BLE_CONNECTION_EVENT);
  // Event should only go to app:
  cl_assert_equal_i(event.task_mask, (PebbleTaskBitset) ~(client_tasks));
  const PebbleBLEConnectionEvent *conn_event = &event.bluetooth.le.connection;
  const BTDeviceInternal event_device = PebbleEventToBTDeviceInternal(conn_event);
  const bool is_same_device = bt_device_equal(&event_device.opaque,
                                              &device->opaque);
  cl_assert_equal_b(is_same_device, true);
  cl_assert_equal_b(conn_event->connected, connected);
  cl_assert_equal_i(conn_event->hci_reason, hci_reason);
}

// Tests
///////////////////////////////////////////////////////////
extern void gap_le_connect_bluetopia_connection_callback(
    unsigned int stack_id, GAP_LE_Event_Data_t* event_data, unsigned long CallbackParameter);
void test_gap_le_connect__initialize(void) {
  fake_GAPAPI_init();

  // Register slave connection event callback for tests involving Pebble as slave:
  // This normally happens in gap_le_advert.c. Taking a shortcut to avoid dragging in more code.
  GAP_LE_Advertising_Enable(1, TRUE, NULL, NULL, gap_le_connect_bluetopia_connection_callback, 0);
  fake_event_init();
  fake_bt_persistent_storage_reset();
  gap_le_connection_init();
  gap_le_connect_init();
}

void test_gap_le_connect__cleanup(void) {
  // Cancel all connection intents:
  for (GAPLEClient c = 0; c < GAPLEClientNum; ++c) {
    gap_le_connect_cancel_all(c);
  }

  gap_le_connect_deinit();

  cl_assert_equal_b(gap_le_connect_has_pending_create_connection(), false);
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 0);

  gap_le_connection_deinit();

  cl_assert_equal_b(fake_HCIAPI_whitelist_error_count(), 0);
  fake_HCIAPI_deinit();
}

// -----------------------------------------------------------------------------
// Parameter / Bounds checking

void test_gap_le_connect__register_max_intents(void) {
  for (int i = 0; i < GAP_LE_CONNECT_MASTER_MAX_CONNECTION_INTENTS + 1; ++i) {
    BTDeviceInternal device = prv_dummy_device(i);
    BTErrno e = gap_le_connect_connect(&device,
                                      true /* auto_reconnect */,
                                      false /* is_pairing_required */,
                                      GAPLEClientApp);

    if (i == GAP_LE_CONNECT_MASTER_MAX_CONNECTION_INTENTS) {
      // When the limit is reached, expect "not enough resources" error:
      cl_assert_equal_i(e, BTErrnoNotEnoughResources);
    } else {
      cl_assert_equal_i(e, BTErrnoOK);
      const bool registered = gap_le_connect_has_connection_intent(&device,
                                                                  GAPLEClientApp);
      cl_assert_equal_b(registered, true);
    }
  }
}

void test_gap_le_connect__register_null_device(void) {
  BTErrno e = gap_le_connect_connect(NULL,
                                     true /* auto_reconnect */,
                                     false /* is_pairing_required */,
                                     GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

void test_gap_le_connect__unregister_null_device(void) {
  BTErrno e = gap_le_connect_cancel(NULL, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

void test_gap_le_connect__register_invalid_bonding(void) {
  BTErrno e = gap_le_connect_connect_by_bonding(BT_BONDING_ID_INVALID,
                                                true /* auto_reconnect */,
                                                false /* is_pairing_required */,
                                                GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

void test_gap_le_connect__register_non_existing_bonding(void) {
  BTErrno e = gap_le_connect_connect_by_bonding(~0,
                                                true /* auto_reconnect */,
                                                false /* is_pairing_required */,
                                                GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

void test_gap_le_connect__unregister_invalid_bonding(void) {
  BTErrno e = gap_le_connect_cancel_by_bonding(BT_BONDING_ID_INVALID, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

void test_gap_le_connect__unregister_non_existing_bonding(void) {
  BTErrno e = gap_le_connect_cancel_by_bonding(~0, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

void test_gap_le_connect__register_is_already_registered_for_same_client(void) {
  BTErrno e;
  bool registered;
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent:
  e = gap_le_connect_connect(&device,
                            true /* auto_reconnect */,
                            false /* is_pairing_required */,
                            GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);
  registered = gap_le_connect_has_connection_intent(&device, GAPLEClientApp);
  cl_assert_equal_b(registered, true);

  // Try registering the device again as same client:
  e = gap_le_connect_connect(&device,
                            true /* auto_reconnect */,
                            false /* is_pairing_required */,
                            GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidState);

  // Should still be registered from the first call:
  registered = gap_le_connect_has_connection_intent(&device, GAPLEClientApp);
  cl_assert_equal_b(registered, true);
}

void test_gap_le_connect__register_same_device_and_bonding(void) {
  // Test that it is possible to have 2 intents for the same device, when registering one intent
  // using the resolvable address and one with a bonding. Pebble will not try to collate these,
  // because there are many addresses that resolve to the same bonding. The current implementation
  // uses one address or one bonding per intent.
  BTErrno e;
  bool registered;
  BTDeviceInternal device = *fake_GAPAPI_get_device_resolving_to_fake_irk();
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  // Register connection intent:
  e = gap_le_connect_connect(&device,
                             true /* auto_reconnect */,
                             false /* is_pairing_required */,
                             GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);
  registered = gap_le_connect_has_connection_intent(&device, GAPLEClientApp);
  cl_assert_equal_b(registered, true);

  // Register another connection intent using the bonding:
  e = gap_le_connect_connect_by_bonding(bonding_id,
                                        true /* auto_reconnect */,
                                        false /* is_pairing_required */,
                                        GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);
  registered = gap_le_connect_has_connection_intent_for_bonding(bonding_id, GAPLEClientApp);
  cl_assert_equal_b(registered, true);
}

void test_gap_le_connect__register_two_clients_same_device(void) {
  BTErrno e;
  bool registered;
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent:
  e = gap_le_connect_connect(&device,
                            true /* auto_reconnect */,
                            false /* is_pairing_required */,
                            GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);
  registered = gap_le_connect_has_connection_intent(&device, GAPLEClientApp);
  cl_assert_equal_b(registered, true);

  // Try registering the device again for different client:
  e = gap_le_connect_connect(&device,
                            true /* auto_reconnect */,
                            false /* is_pairing_required */,
                            GAPLEClientKernel);
  cl_assert_equal_i(e, BTErrnoOK);

  // Assert registrations:
  registered = gap_le_connect_has_connection_intent(&device, GAPLEClientApp);
  cl_assert_equal_b(registered, true);
  registered = gap_le_connect_has_connection_intent(&device, GAPLEClientKernel);
  cl_assert_equal_b(registered, true);

  // Only one registration (co-owned by the 2 clients):
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 1);
}

void test_gap_le_connect__unregister_unknown_device(void) {
  BTDeviceInternal device = prv_dummy_device(1);
  BTErrno e = gap_le_connect_cancel(&device, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

void test_gap_le_connect__unregister_unowned_intent(void) {
  BTDeviceInternal device = prv_dummy_device(1);
  BTErrno e;

  // Register connection intent owned by kernel:
  e = gap_le_connect_connect(&device,
                            true /* auto_reconnect */,
                            false /* is_pairing_required */,
                            GAPLEClientKernel);
  cl_assert_equal_i(e, BTErrnoOK);

  // Unregister connection intent owned by app:
  e = gap_le_connect_cancel(&device, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoInvalidParameter);
}

// -----------------------------------------------------------------------------
// Virtual (dis)connection events

void __disabled_test_gap_le_connect__connection_event_for_registered_client(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent:
  BTErrno e = gap_le_connect_connect(&device,
                                    true /* auto_reconnect */,
                                    false /* is_pairing_required */,
                                    GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  // Device isn't connected. Verify no event was caused as a result of the
  // registration.
  prv_assert_no_event();

  // Connect & verify the client task gets the event:
  prv_fake_connect(&device, true /* is_master*/);
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);

  // Disconnect & verify the client task gets the event:
  prv_fake_disconnect(&device, true /* is_master */);
  prv_assert_client_event(&device, false /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST);
}

void test_gap_le_connect__connection_event_for_registered_client_by_bonding(void) {
  BTDeviceInternal device = *fake_GAPAPI_get_device_resolving_to_fake_irk();
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  // Register connection intent:
  BTErrno e = gap_le_connect_connect_by_bonding(bonding_id,
                                                true /* auto_reconnect */,
                                                false /* is_pairing_required */,
                                                GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  // Device isn't connected. Verify no event was caused as a result of the
  // registration.
  prv_assert_no_event();

  // Connect & verify the client task gets the event:
  prv_fake_connect(&device, false /* is_master*/);
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);

  // Disconnect & verify the client task gets the event:
  prv_fake_disconnect(&device, false /* is_master */);
  prv_assert_client_event(&device, false /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST);
}

void __disabled_test_gap_le_connect__register_for_already_connected_device(void) {
  BTErrno e;
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent for kernel:
  e = gap_le_connect_connect(&device,
                            true /* auto_reconnect */,
                            false /* is_pairing_required */,
                            GAPLEClientKernel);
  cl_assert_equal_i(e, BTErrnoOK);

  // Simulate getting a Connection Complete event for the device from Bluetopia:
  prv_fake_connect(&device, true /* is_master*/);

  // Verify the kernel task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_KernelMain),
                          HCI_ERROR_CODE_SUCCESS);

  // Register connection intent for app:
  e = gap_le_connect_connect(&device,
                            true /* auto_reconnect */,
                            false /* is_pairing_required */,
                            GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  // Verify (only) the app task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);
}

void test_gap_le_connect__register_for_already_connected_bonding(void) {
  BTErrno e;
  BTDeviceInternal device = *fake_GAPAPI_get_device_resolving_to_fake_irk();
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  // Register connection intent for kernel:
  e = gap_le_connect_connect_by_bonding(bonding_id,
                                        true /* auto_reconnect */,
                                        false /* is_pairing_required */,
                                        GAPLEClientKernel);
  cl_assert_equal_i(e, BTErrnoOK);

  // Simulate getting a Connection Complete event for the device from Bluetopia:
  prv_fake_connect(&device, false /* is_master*/);

  // Verify the kernel task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_KernelMain),
                          HCI_ERROR_CODE_SUCCESS);

  // Register connection intent for app:
  e = gap_le_connect_connect_by_bonding(bonding_id,
                                        true /* auto_reconnect */,
                                        false /* is_pairing_required */,
                                        GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  // Verify (only) the app task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);
}

void __disabled_test_gap_le_connect__disconnection_event_upon_airplane_mode(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent:
  BTErrno e = gap_le_connect_connect(&device,
                                    true /* auto_reconnect */,
                                    false /* is_pairing_required */,
                                    GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  prv_fake_connect(&device, true /* is_master*/);

  // Air-plane mode:
  gap_le_connect_deinit();

  // Verify (only) the app task got a (virtual) connection event:
  prv_assert_client_event(&device, false /* connected */, (1 << PebbleTask_App),
                          GAPLEConnectHCIReasonExtensionAirPlaneMode);
}

// -----------------------------------------------------------------------------
// Auto-reconnect Tests

void __disabled_test_gap_le_connect__single_client_no_autoreconnect(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent for app:
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);

  prv_fake_connect(&device, true /* is_master*/);

  // Verify the app task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);

  prv_fake_disconnect(&device, true /* is_master */);

  // Verify the app task got a (virtual) disconnection event:
  prv_assert_client_event(&device, false /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST);

  // Verify that the connection intent has been removed after disconnection:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 0);
}

void __disabled_test_gap_le_connect__two_clients_one_without_autoreconnect(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register auto-reconnecting connection intent for kernel:
  gap_le_connect_connect(&device,
                        true /* auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientKernel);

  prv_fake_connect(&device, true /* is_master*/);

  // Register one-shot connection intent for app:
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);

  prv_fake_disconnect(&device, true /* is_master */);

  // Verify both app task and kernel got the (virtual) disconnection event:
  prv_assert_client_event(&device, false /* connected */,
                          (1 << PebbleTask_App) | (1 << PebbleTask_KernelMain),
                          HCI_ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST);

  // Verify that the connection intent is still there for the Kernel:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 1);
  cl_assert_equal_b(gap_le_connect_has_connection_intent(&device, GAPLEClientKernel), true);
  cl_assert_equal_b(gap_le_connect_has_connection_intent(&device, GAPLEClientApp), false);
  cl_assert_equal_b(gap_le_connect_has_pending_create_connection(), true);
}

// -----------------------------------------------------------------------------
// Cancel Connect (as Master)

void __disabled_test_gap_le_connect__cancel_connect(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent for app:
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);

  BTErrno e = gap_le_connect_cancel(&device, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  // The LE Cancel Create Connection command is always followed by an event that
  // is sent by the BT Controller. Simulate this event:
  fake_gap_le_put_cancel_create_event(&device, true /* is_master */);

  prv_assert_no_event();

  // Verify there are no more intents:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 0);
  cl_assert_equal_b(gap_le_connect_has_pending_create_connection(), false);
}

void __disabled_test_gap_le_connect__disconnection_event_upon_cancel_connect(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent for app:
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);

  prv_fake_connect(&device, false /* is_master*/);

  BTErrno e = gap_le_connect_cancel(&device, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  // The LE Cancel Create Connection command is always followed by an event that
  // is sent by the BT Controller. Simulate this event:
  fake_gap_le_put_cancel_create_event(&device, true /* is_master */);

  // Verify the app task got a (virtual) disconnection event:
  prv_assert_client_event(&device, false /* connected */, (1 << PebbleTask_App),
                          GAPLEConnectHCIReasonExtensionCancelConnect);
  // Verify there are no more intents:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 0);
}

// -----------------------------------------------------------------------------
// Cancel Connect by Bonding (as Slave)

void test_gap_le_connect__slave_cancel_connect_by_bonding(void) {
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  // Register connection intent for app:
  gap_le_connect_connect_by_bonding(bonding_id,
                                    false /* no auto_reconnect */,
                                    false /* is_pairing_required */,
                                    GAPLEClientApp);

  BTErrno e = gap_le_connect_cancel_by_bonding(bonding_id, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  prv_assert_no_event();

  // Verify there are no more intents:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 0);
  cl_assert_equal_b(gap_le_connect_has_pending_create_connection(), false);
}

void test_gap_le_connect__slave_disconnection_event_upon_cancel_connect_by_bonding(void) {
  BTDeviceInternal device = *fake_GAPAPI_get_device_resolving_to_fake_irk();
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  prv_fake_connect(&device, false /* is_master*/);

  // Register connection intent for app:
  gap_le_connect_connect_by_bonding(bonding_id,
                                    false /* no auto_reconnect */,
                                    false /* is_pairing_required */,
                                    GAPLEClientApp);

  BTErrno e = gap_le_connect_cancel_by_bonding(bonding_id, GAPLEClientApp);
  cl_assert_equal_i(e, BTErrnoOK);

  // Verify the app task got a (virtual) disconnection event:
  prv_assert_client_event(&device, false /* connected */, (1 << PebbleTask_App),
                          GAPLEConnectHCIReasonExtensionCancelConnect);
  // Verify there are no more intents:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 0);
}


// -------------------------------------------------------------------------------------------------
// Pairing

void __disabled_test_gap_le_connect__one_shot_intent_removed_when_disconnected_before_encrypt(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection one-shot intent, with pairing required:
  gap_le_connect_connect(&device,
                                false /* no auto_reconnect */,
                                true /* is_pairing_required */,
                                GAPLEClientKernel);

  // Expect intent:
  cl_assert_equal_b(gap_le_connect_has_connection_intent(&device, GAPLEClientKernel), true);

  prv_fake_connect(&device, true /* is_master*/);
  prv_fake_disconnect(&device, true /* is_master */);

  // Expect intent to be removed:
  cl_assert_equal_b(gap_le_connect_has_connection_intent(&device, GAPLEClientKernel), false);
}

void test_gap_le_connect__connection_event_only_after_encrypted_if_encryption_required(void) {
  BTDeviceInternal device = *fake_GAPAPI_get_device_resolving_to_fake_irk();
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  // Register connection intent for app:
  gap_le_connect_connect_by_bonding(bonding_id,
                                    true /* no auto_reconnect */,
                                    true /* is_pairing_required */,
                                    GAPLEClientApp);

  prv_fake_connect(&device, false /* is_master*/);

  // Verify the app task got NO (virtual) connection event, the link is not encrypted yet:
  // TODO: legacy PEBBLE_BT_CONNECTION_EVENT is still emitted, see gap_le_connect.c
  // prv_put_legacy_connection_event.
  //
  // prv_assert_no_event();

  fake_event_clear_last();
  fake_GAPAPI_set_encrypted_for_device(&device);
  fake_GAPAPI_put_encryption_change_event(true /* encrypted */, GAP_LE_PAIRING_STATUS_NO_ERROR,
                                          false /* is_master */, &device);

  // Verify the app task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);
}

void test_gap_le_connect__add_intent_requiring_pairing_after_connected_and_encrypted(void) {
  BTDeviceInternal device = *fake_GAPAPI_get_device_resolving_to_fake_irk();
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  prv_fake_connect(&device, false /* is_master*/);
  fake_GAPAPI_set_encrypted_for_device(&device);
  fake_GAPAPI_put_encryption_change_event(true /* encrypted */, GAP_LE_PAIRING_STATUS_NO_ERROR,
                                          false /* is_master */, &device);
  fake_event_clear_last();

  const BleAddressAndIRKChange e = {
    .device = device,
    .is_address_updated = true,
    .new_device = device,
    .is_resolved = true,
    .irk = *(const SMIdentityResolvingKey *) fake_GAPAPI_get_fake_irk(),
  };
  bt_driver_handle_le_connection_handle_update_address_and_irk(&e);

  gap_le_connection_by_device(&device);


  // Register connection intent for app:
  gap_le_connect_connect_by_bonding(bonding_id,
                                    true /* no auto_reconnect */,
                                    true /* is_pairing_required */,
                                    GAPLEClientApp);

  // Verify the app task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);
}


// -----------------------------------------------------------------------------
// Handling Bonding Changes

void test_gap_le_connect__removed_bonding_while_connected(void) {
  BTDeviceInternal device = *fake_GAPAPI_get_device_resolving_to_fake_irk();
  BTBondingID bonding_id = prv_add_bonding_for_fake_resolvable_device();

  // Register connection intent for app:
  gap_le_connect_connect_by_bonding(bonding_id,
                                    false /* no auto_reconnect */,
                                    false /* is_pairing_required */,
                                    GAPLEClientApp);

  prv_fake_connect(&device, false /* is_master*/);

  // Verify the app task got a (virtual) connection event:
  prv_assert_client_event(&device, true /* connected */, (1 << PebbleTask_App),
                          HCI_ERROR_CODE_SUCCESS);

  // Simulate "bonding will delete" callback:
  gap_le_connect_handle_bonding_change(bonding_id, BtPersistBondingOpWillDelete);

  // Verify the app task got a (virtual) disconnection event:
  prv_assert_client_event(&device, false /* connected */, (1 << PebbleTask_App),
                          GAPLEConnectHCIReasonExtensionUserRemovedBonding);
  // Verify there are no more intents:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 0);
}

// -----------------------------------------------------------------------------
// BT Controller White-list management

void __disabled_test_gap_le_connect__whitelist_add_when_disconnected(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent for app:
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);

  // Not connected yet, so expect to be added to white-list:
  cl_assert_equal_b(fake_HCIAPI_whitelist_contains(&device), true);

  prv_fake_connect(&device, true /* is_master*/);

  // Connected, so expect to be removed from white-list:
  cl_assert_equal_b(fake_HCIAPI_whitelist_contains(&device), false);
}

void __disabled_test_gap_le_connect__whitelist_add_when_connected(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent for app:
  gap_le_connect_connect(&device,
                         false /* no auto_reconnect */,
                         false /* is_pairing_required */,
                         GAPLEClientKernel);

  prv_fake_connect(&device, true /* is_master*/);

  // Register connection intent for app:
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);

  // Connected, so expect to be removed from white-list:
  cl_assert_equal_b(fake_HCIAPI_whitelist_contains(&device), false);
}

void __disabled_test_gap_le_connect__whitelist_remove_when_connected(void) {
  BTDeviceInternal device = prv_dummy_device(1);

  // Register connection intent for app:
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);
  prv_fake_connect(&device, true /* is_master*/);

  gap_le_connect_cancel(&device, GAPLEClientApp);

  // Connected, so expect to be removed from white-list:
  cl_assert_equal_b(fake_HCIAPI_whitelist_contains(&device), false);
}

void __disabled_test_gap_le_connect__whitelist_repopulated_on_init(void) {
  BTDeviceInternal device = prv_dummy_device(1);
  gap_le_connect_connect(&device,
                        false /* no auto_reconnect */,
                        false /* is_pairing_required */,
                        GAPLEClientApp);

  gap_le_connect_deinit();

  // Connection intents survive air-plane mode:
  cl_assert_equal_i(gap_le_connect_connection_intents_count(), 1);

  // "Reset" BT Controller:
  fake_HCIAPI_deinit();
  cl_assert_equal_i(fake_HCIAPI_whitelist_count(), 0);

  gap_le_connect_init();

  // Not connected yet, so expect to be added to white-list:
  cl_assert_equal_b(fake_HCIAPI_whitelist_contains(&device), true);
  cl_assert_equal_i(fake_HCIAPI_whitelist_count(), 1);

}
