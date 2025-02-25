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

#include "clar.h"

#include <bluetooth/bonding_sync.h>
#include <bluetooth/gap_le_connect.h>

#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/normal/bluetooth/bluetooth_persistent_storage_unittest_impl.h"

#include "services/normal/settings/settings_file.h"
#include "services/normal/filesystem/pfs.h"
#include "services/common/event_service.h"
#include "services/common/analytics/analytics.h"
#include "services/common/analytics/analytics_external.h"
#include "flash_region/flash_region.h"

// Stubs
////////////////////////////////////

typedef struct GAPLEConnection GAPLEConnection;

#include "fake_bonding_sync.h"
#include "fake_rtc.h"
#include "fake_spi_flash.h"
#include "fake_system_task.h"
#include "fake_events.h"
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_shared_prf_storage.h"

#include "stubs_bluetopia_interface.h"
#include "stubs_bluetooth_persistent_storage_debug.h"
#include "stubs_bt_driver.h"
#include "stubs_bt_lock.h"
#include "stubs_gap_le_advert.h"
#include "stubs_bluetooth_analytics.h"
#include "stubs_gatt_client_discovery.h"
#include "stubs_gatt_client_subscriptions.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_pairing_service.h"
#include "stubs_print.h"
#include "stubs_prompt.h"
#include "stubs_regular_timer.h"
#include "stubs_serial.h"
#include "stubs_sleep.h"
#include "stubs_system_reset.h"
#include "stubs_task_watchdog.h"

static int s_ble_bonding_change_add_count;
static int s_ble_bonding_change_update_count;
static int s_ble_bonding_change_delete_count;

static int s_analytics_ble_pairings_count;

typedef bool (*BondingSyncFilterCb)(const BleBonding *bonding, void *ctx);
const BleBonding *bonding_sync_find(BondingSyncFilterCb cb, void *ctx) {
  return NULL;
}

void bt_driver_pebble_pairing_service_handle_status_change(const GAPLEConnection *connection) {
}

bool bt_ctl_is_bluetooth_running(void) {
  return true;
}

void bt_driver_handle_le_conn_params_update_event(
    const BleConnectionUpdateCompleteEvent *event) {
}

typedef struct PairingUserConfirmationCtx PairingUserConfirmationCtx;

void bt_driver_cb_pairing_confirm_handle_request(const PairingUserConfirmationCtx *ctx,
                                                 const char *device_name,
                                                 const char *confirmation_token) {
}

void bt_driver_cb_pairing_confirm_handle_completed(const PairingUserConfirmationCtx *ctx,
                                                   bool success) {
}

void cc2564A_bad_le_connection_complete_handle(unsigned int stack_id,
                                             const GAP_LE_Current_Connection_Parameters_t *params) {
}

void gap_le_connect_handle_bonding_change(BTBondingID bonding_id, BtPersistBondingOp op) {
}

void gap_le_connection_handle_bonding_change(BTBondingID bonding, BtPersistBondingOp op) {
}

void gap_le_device_name_request(uintptr_t stack_id, GAPLEConnection *connection) {
}

void bt_pairability_update_due_to_bonding_change(void) {
}

void bt_local_addr_handle_bonding_change(BTBondingID bonding, BtPersistBondingOp op) {
}

void kernel_le_client_handle_bonding_change(BTBondingID bonding, BtPersistBondingOp op) {
  if (op == BtPersistBondingOpDidAdd) {
    s_ble_bonding_change_add_count++;
  } else if (op == BtPersistBondingOpDidChange) {
    s_ble_bonding_change_update_count++;
  } else if (op == BtPersistBondingOpWillDelete) {
    s_ble_bonding_change_delete_count++;
  }
  return;
}

void analytics_set(AnalyticsMetric metric, int64_t val, AnalyticsClient client) {
  if (metric == ANALYTICS_DEVICE_METRIC_BLE_PAIRING_RECORDS_COUNT) {
    s_analytics_ble_pairings_count = val;
  }
}

void analytics_event_bt_error(AnalyticsEvent type, uint32_t error) {
}

void analytics_inc(AnalyticsMetric metric, AnalyticsClient client) {
}

void gap_update_bt_classic_connectability(void) {
}

uint16_t gaps_get_starting_att_handle(void) {
  return 4;
}

void gatt_service_changed_server_cleanup_by_connection(GAPLEConnection *connection) {
}

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

// Tests
///////////////////////////////////////////////////////////

void test_bluetooth_persistent_storage__initialize(void) {
  bonding_sync_init();
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);

  s_ble_bonding_change_add_count = 0;
  s_ble_bonding_change_update_count = 0;
  s_ble_bonding_change_delete_count = 0;
  s_analytics_ble_pairings_count = 0;

  fake_shared_prf_storage_reset_counts();

  bt_persistent_storage_init();
}

void test_bluetooth_persistent_storage__cleanup(void) {
  bonding_sync_deinit();
}


///////////////////////////////////////////////////////////////////////////////////////////////////
//! BLE Pairing Info

void test_bluetooth_persistent_storage__ble_address_pinning(void) {
  cl_assert_equal_b(bt_persistent_storage_has_pinned_ble_pairings(), false);

  BTDeviceAddress address_out = {};
  cl_assert_equal_b(bt_persistent_storage_get_ble_pinned_address(&address_out), false);
  BTDeviceAddress address_out_expected = {};
  cl_assert_equal_m(&address_out_expected, &address_out, sizeof(address_out));

  BTDeviceAddress address = (BTDeviceAddress) {
    .octets = {
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16
    },
  };
  cl_assert_equal_b(bt_persistent_storage_set_ble_pinned_address(&address), true);

  SMPairingInfo pairing_1 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x11, 0x12, 0x13, 0x14, 0x15, 0x16
        },
      },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
    .is_mitm_protection_enabled = true,
  };

  BleBonding ble_bonding = (BleBonding) {
    .is_gateway = true,
    .pairing_info = pairing_1,
  };
  bonding_sync_add_bonding(&ble_bonding);
  BTBondingID id = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */, NULL,
                                                           true /* requires_address_pinning */,
                                                           false /* auto_accept_re_pairing */);
  cl_assert(id != BT_BONDING_ID_INVALID);

  cl_assert_equal_b(bt_persistent_storage_has_pinned_ble_pairings(), true);

  bt_persistent_storage_delete_ble_pairing_by_id(id);

  cl_assert_equal_b(bt_persistent_storage_has_pinned_ble_pairings(), false);

  cl_assert_equal_b(bt_persistent_storage_set_ble_pinned_address(NULL), true);
  cl_assert_equal_b(bt_persistent_storage_get_ble_pinned_address(NULL), false);
}

void test_bluetooth_persistent_storage__ble_store_and_get(void) {
  bool ret;

  // Output variables
  SMIdentityResolvingKey irk_out;
  BTDeviceInternal device_out;

  // Store a new pairing
  SMPairingInfo pairing_1;
  memset(&pairing_1, 0x00, sizeof(pairing_1));
  pairing_1 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x11, 0x12, 0x13, 0x14, 0x15, 0x16
        },
      },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
    .is_mitm_protection_enabled = true,
  };
  BTBondingID id_1 = bt_persistent_storage_store_ble_pairing(&pairing_1,
                                                             true /* is_gateway */, NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  cl_assert(id_1 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_ble_bonding_change_add_count, 1);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_store_count(), 1);
  cl_assert_equal_b(bt_persistent_storage_has_pinned_ble_pairings(), false);

  // Read it back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_1, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_1.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_1.identity, sizeof(device_out));

  // Store another pairing
  SMPairingInfo pairing_2;
  memset(&pairing_2, 0x00, sizeof(pairing_2));
  pairing_2 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x08,
        0x09, 0x02, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x20,
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x21, 0x22, 0x13, 0x14, 0x15, 0x26,
        },
      },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_2 = bt_persistent_storage_store_ble_pairing(&pairing_2, false /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  cl_assert(id_2 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_ble_bonding_change_add_count, 2);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_store_count(), 1); // This wasn't a gateway

  // Read both pairings back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_1, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_1.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_1.identity, sizeof(device_out));

  ret = bt_persistent_storage_get_ble_pairing_by_id(id_2, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_2.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_2.identity, sizeof(device_out));

  // Update first pairing (with the same data)
  BTBondingID id_X = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  cl_assert_equal_i(id_1, id_X);
  cl_assert_equal_i(s_ble_bonding_change_update_count, 1);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_store_count(), 1);

  // Read both pairings back again
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_1, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_1.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_1.identity, sizeof(device_out));

  ret = bt_persistent_storage_get_ble_pairing_by_id(id_2, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_2.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_2.identity, sizeof(device_out));

  // Add a third pairing
  SMPairingInfo pairing_3;
  memset(&pairing_3, 0x00, sizeof(pairing_3));
  pairing_3 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x91, 0x22, 0x73, 0x24, 0x25, 0x26, 0x27, 0x08,
        0x69, 0x02, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x99,
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x29, 0x92, 0x13, 0x99, 0x15, 0x96,
        },
      },
      .is_classic = true,
      .is_random_address = true,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_3 = bt_persistent_storage_store_ble_pairing(&pairing_3, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  cl_assert(id_3 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_ble_bonding_change_add_count, 3);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_store_count(), 2);

  // Read all three pairings back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_1, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_1.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_1.identity, sizeof(device_out));

  ret = bt_persistent_storage_get_ble_pairing_by_id(id_2, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_2.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_2.identity, sizeof(device_out));

  ret = bt_persistent_storage_get_ble_pairing_by_id(id_3, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_3.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_3.identity, sizeof(device_out));

  bt_persistent_storage_register_existing_ble_bondings();
  cl_assert_equal_b(bonding_sync_contains_pairing_info(&pairing_1, true), true);
  cl_assert_equal_b(bonding_sync_contains_pairing_info(&pairing_2, false), true);
  cl_assert_equal_b(bonding_sync_contains_pairing_info(&pairing_3, true), true);
}

 void test_bluetooth_persistent_storage__get_ble_by_addr(void) {
   bool ret;

   // Output variables
   SMIdentityResolvingKey irk_out;

   // Store a pairing
   SMPairingInfo pairing = (SMPairingInfo) {
     .irk = (SMIdentityResolvingKey) {
       .data = {
         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
         0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00,
       },
     },
     .identity = (BTDeviceInternal) {
       .address = (BTDeviceAddress) {
         .octets = {
           0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
         },
       },
       .is_classic = false,
       .is_random_address = false,
     },
     .is_remote_identity_info_valid = true,
   };

   BTBondingID id = bt_persistent_storage_store_ble_pairing(&pairing, true /* is_gateway */, NULL,
                                                            false /* requires_address_pinning */,
                                                            false /* auto_accept_re_pairing */);
   cl_assert(id != BT_BONDING_ID_INVALID);

   // Read it back
   ret = bt_persistent_storage_get_ble_pairing_by_addr(&pairing.identity, &irk_out, NULL);
   cl_assert(ret);
   cl_assert_equal_m(&irk_out, &pairing.irk, sizeof(irk_out));
}


void test_bluetooth_persistent_storage__delete_ble_pairing_by_id(void) {
  bool ret;

  // Output variables
  SMIdentityResolvingKey irk_out;
  BTDeviceInternal device_out;

  // Store a pairing
  SMPairingInfo pairing = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00,
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        },
      },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };

  BleBonding ble_bonding = (BleBonding) {
    .is_gateway = true,
    .pairing_info = pairing,
  };
  bonding_sync_add_bonding(&ble_bonding);
  BTBondingID id = bt_persistent_storage_store_ble_pairing(&pairing, true /* is_gateway */, NULL,
                                                           false /* requires_address_pinning */,
                                                           false /* auto_accept_re_pairing */);
  cl_assert(id != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_ble_bonding_change_add_count, 1);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_store_count(), 1);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_delete_count(), 1);

  // Delete the Pairing
  bt_persistent_storage_delete_ble_pairing_by_id(id);
  cl_assert_equal_i(s_ble_bonding_change_delete_count, 1);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_delete_count(), 2);

  // Try to read it back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id, &irk_out, &device_out, NULL);
  cl_assert(!ret);

  // Add the pairing again
  bonding_sync_add_bonding(&ble_bonding);
  id = bt_persistent_storage_store_ble_pairing(&pairing, true /* is_gateway */, NULL,
                                               false /* requires_address_pinning */,
                                               false /* auto_accept_re_pairing */);
  cl_assert(id != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_ble_bonding_change_add_count, 2);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_store_count(), 2);

  // Delete a pairing that doesn't exist. Delete count should stay at 1
  bt_persistent_storage_delete_ble_pairing_by_id(9);
  cl_assert_equal_i(s_ble_bonding_change_delete_count, 1);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_delete_count(), 3);

  // Make sure the pairing is actually still there
  ret = bt_persistent_storage_get_ble_pairing_by_id(id, &irk_out, &device_out, NULL);
  cl_assert(ret);

  // And delete is again
  bt_persistent_storage_delete_ble_pairing_by_id(id);
  cl_assert_equal_i(s_ble_bonding_change_delete_count, 2);
  cl_assert_equal_i(fake_shared_prf_storage_get_ble_delete_count(), 4);

  // Try to read it back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id, &irk_out, &device_out, NULL);
  cl_assert(!ret);
}


void test_bluetooth_persistent_storage__analytics_external_collect_ble_pairing_info(void) {
  // No pairings yet
  analytics_external_collect_ble_pairing_info();
  cl_assert_equal_i(s_analytics_ble_pairings_count, 0);

  // Store a pairing
  SMPairingInfo pairing = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00,
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        },
      },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BleBonding ble_bonding = (BleBonding) {
    .is_gateway = true,
    .pairing_info = pairing,
  };
  bonding_sync_add_bonding(&ble_bonding);
  BTBondingID id = bt_persistent_storage_store_ble_pairing(&pairing, true /* is_gateway */, NULL,
                                                           false /* requires_address_pinning */,
                                                           false /* auto_accept_re_pairing */);
  cl_assert(id != BT_BONDING_ID_INVALID);

  // We should now be at 1
  analytics_external_collect_ble_pairing_info();
  cl_assert_equal_i(s_analytics_ble_pairings_count, 1);

  // Delete the Pairing
  bt_persistent_storage_delete_ble_pairing_by_id(id);
  cl_assert_equal_i(s_ble_bonding_change_delete_count, 1);

  // We should now be at 0
  analytics_external_collect_ble_pairing_info();
  cl_assert_equal_i(s_analytics_ble_pairings_count, 0);
}

void test_bluetooth_persistent_storage__ble_ancs_bonding(void) {
  bool ret;

  SMPairingInfo pairing = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00,
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        },
      },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };

  // This pairing is a heart rate monitor or something similar
  BTBondingID id = bt_persistent_storage_store_ble_pairing(&pairing, false /* is_gateway */, NULL,
                                                           false /* requires_address_pinning */,
                                                           false /* auto_accept_re_pairing */);
  cl_assert(id != BT_BONDING_ID_INVALID);

  // No ANCS bonding yet
  BTBondingID ancs_id = bt_persistent_storage_get_ble_ancs_bonding();
  cl_assert_equal_i(ancs_id, BT_BONDING_ID_INVALID);
  ret = bt_persistent_storage_has_ble_ancs_bonding();
  cl_assert(!ret);
  ret = bt_persistent_storage_is_ble_ancs_bonding(id);
  cl_assert(!ret);

  // Store another pairing, this one is a gateway (supports ancs)
  pairing.identity.address.octets[0] = 0x12;
  BTBondingID id2 = bt_persistent_storage_store_ble_pairing(&pairing, true /* is_gateway */, NULL,
                                                            false /* requires_address_pinning */,
                                                            false /* auto_accept_re_pairing */);
  cl_assert(id2 != BT_BONDING_ID_INVALID);

  // Find it
  ancs_id = bt_persistent_storage_get_ble_ancs_bonding();
  cl_assert_equal_i(ancs_id, id2);
  ret = bt_persistent_storage_has_ble_ancs_bonding();
  cl_assert(ret);
  ret = bt_persistent_storage_is_ble_ancs_bonding(id2);
  cl_assert(ret);
}

void test_bluetooth_persistent_storage__ble_device_name(void) {
  SMPairingInfo pairing = {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00,
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {
        .octets = {
          0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        },
      },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  const char *device_name = "iPhone";
  BTBondingID id = bt_persistent_storage_store_ble_pairing(&pairing, false /* is_gateway */,
                                                           device_name,
                                                           false /* requires_address_pinning */,
                                                           false /* auto_accept_re_pairing */);

  cl_assert(id != BT_BONDING_ID_INVALID);

  char device_name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  bt_persistent_storage_get_ble_pairing_by_id(id, NULL, NULL, device_name_out);

  cl_assert_equal_i(strcmp(device_name, device_name_out), 0);

  // Update:
  const char *new_device_name = "New iPhone";
  bt_persistent_storage_update_ble_device_name(id, new_device_name);
  bt_persistent_storage_get_ble_pairing_by_id(id, NULL, NULL, device_name_out);

  cl_assert_equal_i(strcmp(new_device_name, device_name_out), 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! BT Classic Pairing Info

void test_bluetooth_persistent_storage__bt_classic_store_and_get(void) {
  bool ret;

  // Output variables
  BTDeviceAddress addr_out;
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_1 = {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_1 = {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                             0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_1[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_1 = 0x11;
  BTBondingID id_1 = bt_persistent_storage_store_bt_classic_pairing(&addr_1, &link_key_1,
                                                            name_1, &platform_bits_1);
  cl_assert(id_1 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_store_count(), 1);

  // Read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_1, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_1, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_1, name_out);
  cl_assert_equal_i(platform_bits_1, platform_bits_out);

  // Store another pairing
  BTDeviceAddress addr_2 = {{0x21, 0x22, 0x23, 0x24, 0x25, 0x26}};
  SM128BitKey link_key_2 = {{0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                             0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}};
  char name_2[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 2";
  uint8_t platform_bits_2 = 0x22;
  BTBondingID id_2 = bt_persistent_storage_store_bt_classic_pairing(&addr_2, &link_key_2,
                                                            name_2, &platform_bits_2);
  cl_assert(id_2 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_store_count(), 2);

  // Read both pairings back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_1, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_1, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_1, name_out);
  cl_assert_equal_i(platform_bits_1, platform_bits_out);

  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_2, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_2, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_2, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_2, name_out);
  cl_assert_equal_i(platform_bits_2, platform_bits_out);

  // Update first pairing (with the same data)
  BTBondingID id_X = bt_persistent_storage_store_bt_classic_pairing(&addr_1, &link_key_1,
                                                            name_1, &platform_bits_1);
  cl_assert_equal_i(id_1, id_X);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_store_count(), 2);

  // Read both pairings back again
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_1, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_1, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_1, name_out);
  cl_assert_equal_i(platform_bits_1, platform_bits_out);

  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_2, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_2, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_2, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_2, name_out);
  cl_assert_equal_i(platform_bits_2, platform_bits_out);

  // Add a third pairing
  BTDeviceAddress addr_3 = {{0x31, 0x32, 0x33, 0x34, 0x35, 0x36}};
  SM128BitKey link_key_3 = {{0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
                             0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30}};
  char name_3[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 3";
  uint8_t platform_bits_3 = 0x33;
  BTBondingID id_3 = bt_persistent_storage_store_bt_classic_pairing(&addr_3, &link_key_3,
                                                            name_3, &platform_bits_3);
  cl_assert(id_3 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_store_count(), 3);


  // Read all three pairings back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_1, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_1, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_1, name_out);
  cl_assert_equal_i(platform_bits_1, platform_bits_out);

  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_2, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_2, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_2, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_2, name_out);
  cl_assert_equal_i(platform_bits_2, platform_bits_out);

  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_3, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_3, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_3, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_3, name_out);
  cl_assert_equal_i(platform_bits_3, platform_bits_out);

  // Add a fourth pairing
  BTDeviceAddress addr_4 = {{0x41, 0x42, 0x43, 0x34, 0x35, 0x44}};
  SM128BitKey link_key_4 = {{0x40, 0x40, 0x30, 0x30, 0x30, 0x30, 0x30, 0x40,
                             0x40, 0x40, 0x30, 0x30, 0x30, 0x30, 0x30, 0x40}};
  char name_4[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 4";
  uint8_t platform_bits_4 = 0x44;

  // Don't add the platform bits
  BTBondingID id_4 = bt_persistent_storage_store_bt_classic_pairing(&addr_4, &link_key_4, name_4, NULL);
  cl_assert(id_4 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_store_count(), 4);

  // Update with platform bits
  id_4 = bt_persistent_storage_store_bt_classic_pairing(&addr_4, NULL, NULL, &platform_bits_4);
  cl_assert(id_4 != BT_BONDING_ID_INVALID);
}

void test_bluetooth_persistent_storage__get_bt_classic_pairing_by_addr(void) {
  // Output variables
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_in = {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_in = {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                              0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_in[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_in = 0x11;

  BTBondingID id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                                             name_in, &platform_bits_in);
  cl_assert(id != BT_BONDING_ID_INVALID);

  // Read it back
  BTBondingID id_out = bt_persistent_storage_get_bt_classic_pairing_by_addr(&addr_in, &link_key_out,
                                                                     name_out, &platform_bits_out);
  cl_assert_equal_i(id, id_out);
  cl_assert_equal_m(&link_key_in, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_in, name_out);
  cl_assert_equal_i(platform_bits_in, platform_bits_out);

  // Now try to read out a pairing that doesn't exist
  addr_in.octets[0] = 0xff;
  id_out = bt_persistent_storage_get_bt_classic_pairing_by_addr(&addr_in, &link_key_out,
                                                      name_out, &platform_bits_out);
  cl_assert(id_out == BT_BONDING_ID_INVALID);
}


void test_bluetooth_persistent_storage__delete_bt_classic_pairing_by_id(void) {
 bool ret;

  // Output variables
  BTDeviceAddress addr_out;
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_in = {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_in = {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                              0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_in[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_in = 0x11;

  BTBondingID id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                                             name_in, &platform_bits_in);
  cl_assert(id != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_store_count(), 1);

  // Delete the Pairing
  bt_persistent_storage_delete_bt_classic_pairing_by_id(id);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_delete_count(), 2);

  // Try to read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(!ret);

  // Add the pairing again
  id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                               name_in, &platform_bits_in);
  cl_assert(id != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_store_count(), 2);

  // Delete a pairing that doesn't exist
  bt_persistent_storage_delete_bt_classic_pairing_by_id(9);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_delete_count(), 3);

  // Make sure the pairing is actually still there
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);

  // And delete is again
  bt_persistent_storage_delete_bt_classic_pairing_by_id(id);
  cl_assert_equal_i(fake_shared_prf_storage_get_bt_classic_delete_count(), 4);

  // Try to read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(!ret);
}

void test_bluetooth_persistent_storage__delete_bt_classic_pairing_by_addr(void) {
 bool ret;

  // Output variables
  BTDeviceAddress addr_out;
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_in = {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_in = {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                              0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_in[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_in = 0x11;

  BTBondingID id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                                             name_in, &platform_bits_in);
  cl_assert(id != BT_BONDING_ID_INVALID);

  // Delete the Pairing
  bt_persistent_storage_delete_bt_classic_pairing_by_addr(&addr_in);

  // Try to read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(!ret);

  // Add the pairing again
  id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                               name_in, &platform_bits_in);
  cl_assert(id != BT_BONDING_ID_INVALID);

  // Delete a pairing that doesn't exist
  BTDeviceAddress dummy_addr = {{0xff, 0x11, 0x22, 0x14, 0x15, 0x16}};
  bt_persistent_storage_delete_bt_classic_pairing_by_addr(&dummy_addr);

  // Make sure the pairing is actually still there
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);

  // And delete is again
  bt_persistent_storage_delete_bt_classic_pairing_by_addr(&addr_in);

  // Try to read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(!ret);
}


///////////////////////////////////////////////////////////////////////////////////////////////////
//! Local Device Info

void test_bluetooth_persistent_storage__test_active_gateway(void) {
  bool ret;

  BtPersistBondingType type_out;
  BTBondingID id_out;

  // Nothing is stored, so no active gateways yet
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(!ret);

  // Store a new BT Classic pairing
  BTDeviceAddress addr_1 = {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_1 = {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                             0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_1[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_1 = 0x11;

  BTBondingID id_1 = bt_persistent_storage_store_bt_classic_pairing(&addr_1, &link_key_1,
                                                             name_1, &platform_bits_1);
  cl_assert(id_1 != BT_BONDING_ID_INVALID);

  // It should be the active gateway
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(ret);
  cl_assert_equal_i(id_out, id_1);
  cl_assert_equal_i(type_out, BtPersistBondingTypeBTClassic);

  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(!ret);

  // Store another BT Classic pairing
  BTDeviceAddress addr_2 = {{0x22, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_2 = {{0x22, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                             0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_2[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 2";
  uint8_t platform_bits_2 = 0x22;
  BTBondingID id_2 = bt_persistent_storage_store_bt_classic_pairing(&addr_2, &link_key_2,
                                                             name_2, &platform_bits_2);
  cl_assert(id_2 != BT_BONDING_ID_INVALID);

  // The new pairing should be the active gateway
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(ret);
  cl_assert_equal_i(id_out, id_2);
  cl_assert_equal_i(type_out, BtPersistBondingTypeBTClassic);

  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(!ret);

  // Delete the pairing.
  bt_persistent_storage_delete_bt_classic_pairing_by_id(id_2);

  // There should be no active gateway now
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(!ret);

  // Store a new BLE pairing
  SMPairingInfo pairing_1 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00}},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_3 = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  cl_assert(id_3 != BT_BONDING_ID_INVALID);

  // There should still be no active gateway
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(!ret);

  // Manually set the active gateway
  bt_persistent_storage_set_active_gateway(id_1);
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(ret);
  cl_assert_equal_i(id_out, id_1);
  cl_assert_equal_i(type_out, BtPersistBondingTypeBTClassic);

  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(!ret);

  // Manually set the active gateway again (to the ble pairing)
  bt_persistent_storage_set_active_gateway(id_3);
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(ret);
  cl_assert_equal_i(id_out, id_3);
  cl_assert_equal_i(type_out, BtPersistBondingTypeBLE);

  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(ret);
}

void test_bluetooth_persistent_storage__test_is_faithful(void) {
  bool ret;

  // No pairing yet, we should be unfaithful
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(ret);

  // Add a pairing, still unfaithful
  BTDeviceAddress addr_1 = {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_1 = {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                             0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_1[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_1 = 0x11;

  BTBondingID id_1 = bt_persistent_storage_store_bt_classic_pairing(&addr_1, &link_key_1,
                                                             name_1, &platform_bits_1);
  cl_assert(id_1 != BT_BONDING_ID_INVALID);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(ret);

  // A "sync" happened. We are now faithful
  bt_persistent_storage_set_unfaithful(false);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(!ret);

  // Add a new pairing, the active gateway should have changed making us unfaithful
  BTDeviceAddress addr_2 = {{0x22, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_2 = {{0x22, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                             0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_2[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 2";
  uint8_t platform_bits_2 = 0x22;
  BTBondingID id_2 = bt_persistent_storage_store_bt_classic_pairing(&addr_2, &link_key_2,
                                                             name_2, &platform_bits_2);
  cl_assert(id_2 != BT_BONDING_ID_INVALID);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(ret);

  // A "sync" happened. We are now faithful
  bt_persistent_storage_set_unfaithful(false);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(!ret);

  // Add a BLE pairing. We should still be faithful (no PPoGATT yet)
  SMPairingInfo pairing_1 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_3 = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  cl_assert(id_3 != BT_BONDING_ID_INVALID);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(!ret);

  // Manually set a new active gateway.
  bt_persistent_storage_set_active_gateway(id_3);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(ret);

  // We should be unfaithful
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(ret);

  // A "sync" happened. We are now faithful
  bt_persistent_storage_set_unfaithful(false);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(!ret);

  // Another "sync" happened. We should still be faithful
  bt_persistent_storage_set_unfaithful(false);
  ret = bt_persistent_storage_is_unfaithful();
  cl_assert(!ret);
}

void test_bluetooth_persistent_storage__test_root_keys(void) {
  SM128BitKey keys[2] = {
    {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16}},
    {{0x21, 0x22, 0x23, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x11, 0x12, 0x13, 0x24, 0x25, 0x26}},
  };

  SM128BitKey keys_out[2];

  bt_persistent_storage_set_root_keys(keys);
  bt_persistent_storage_get_root_key(0, &keys_out[0]);
  bt_persistent_storage_get_root_key(1, &keys_out[1]);
  cl_assert_equal_m(&keys[0], &keys_out[0], sizeof(SM128BitKey));
  cl_assert_equal_m(&keys[1], &keys_out[1], sizeof(SM128BitKey));

  bt_persistent_storage_init();

  bt_persistent_storage_get_root_key(0, &keys_out[0]);
  bt_persistent_storage_get_root_key(1, &keys_out[1]);
  cl_assert_equal_m(&keys[0], &keys_out[0], sizeof(SM128BitKey));
  cl_assert_equal_m(&keys[1], &keys_out[1], sizeof(SM128BitKey));
}


void test_bluetooth_persistent_storage__delete_all(void) {
  bool ret;

  // Add some pairings
  // BT Classic pairing 1
  BTDeviceAddress addr_1 = {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_1 = {{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                             0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_1[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_1 = 0x11;

  BTBondingID id_1 = bt_persistent_storage_store_bt_classic_pairing(&addr_1, &link_key_1,
                                                             name_1, &platform_bits_1);
  cl_assert(id_1 != BT_BONDING_ID_INVALID);

  // BT Classic pairing 2
  BTDeviceAddress addr_2 = {{0x22, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_2 = {{0x22, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                             0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}};
  char name_2[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 2";
  uint8_t platform_bits_2 = 0x22;
  BTBondingID id_2 = bt_persistent_storage_store_bt_classic_pairing(&addr_2, &link_key_2,
                                                             name_2, &platform_bits_2);
  cl_assert(id_2 != BT_BONDING_ID_INVALID);

  // BLE pairing 1
  SMPairingInfo pairing_1 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0x11, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_3 = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);

  // BLE pairing 2
  SMPairingInfo pairing_2 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {{
      0x02, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x02, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
    }},
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {{0x22, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_4 = bt_persistent_storage_store_ble_pairing(&pairing_2, false /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);

  // Delete all
  bt_persistent_storage_delete_all_pairings();

  // Try to get the pairings
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, NULL, NULL, NULL, NULL);
  cl_assert(!ret);
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_2, NULL, NULL, NULL, NULL);
  cl_assert(!ret);
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_3, NULL, NULL, NULL);
  cl_assert(!ret);
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_4, NULL, NULL, NULL);
  cl_assert(!ret);
}

// Test to make sure we don't accidentally change the serialized data formats.
void test_bluetooth_persistent_storage__ble_serialized_data(void) {
#if UNITTEST_BT_PERSISTENT_STORAGE_VERSION == 1

  //0000  01 00 69 50 68 6f 6e 65  20 4d 61 72 74 79 00 00   ..iPhone  Marty..
  //0010  00 00 00 00 00 00 3f f9  92 8a 00 00 00 00 75 36   ......?. ......u6
  //0020  9c 6e 1a 1b eb 5f fb 89  db 0b ec a5 95 7a 44 f6   .n..._.. .....zD.
  //0030  1c 47 90 53 43 18 f3 e7  00 00 00 00 00 00 d1 6d   .G.SC... .......m
  //0040  89 95 83 aa 5e 7f ff 39  b3 47 36 e4 37 7e 05 1b   ....^..9 .G6.7~..
  //0050  85 e3 b8 98 00 00 00 00  00 00 00 00 00 00 00 00   ........ ........
  //0060  00 00 00 00 00 00 07 00  00 00 00 00 00 00         ........ ......

  const uint8_t expected_raw_data[] = {
    0x01, 0x00, 0x69, 0x50, 0x68, 0x6f, 0x6e, 0x65,
    0x20, 0x4d, 0x61, 0x72, 0x74, 0x79, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xf9,
    0x92, 0x8a, 0x00, 0x00, 0x00, 0x00, 0x75, 0x36,
    0x9c, 0x6e, 0x1a, 0x1b, 0xeb, 0x5f, 0xfb, 0x89,
    0xdb, 0x0b, 0xec, 0xa5, 0x95, 0x7a, 0x44, 0xf6,
    0x1c, 0x47, 0x90, 0x53, 0x43, 0x18, 0xf3, 0xe7,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd1, 0x6d,
    0x89, 0x95, 0x83, 0xaa, 0x5e, 0x7f, 0xff, 0x39,
    0xb3, 0x47, 0x36, 0xe4, 0x37, 0x7e, 0x05, 0x1b,
    0x85, 0xe3, 0xb8, 0x98, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  size_t data_size = 110;

#elif UNITTEST_BT_PERSISTENT_STORAGE_VERSION == 2

  //0000  01 00 69 50 68 6f 6e 65 20 4d 61 72 74 79 00 00  ..iPhone Marty..
  //0010  00 00 00 00 00 00 90 36 9c 6e 1a 1b eb 5f fb 89  .......6.n..._..
  //0020  db 0b ec a5 95 ab 92 8a aa f6 1c 47 90 53 43 ff  ...........G.SC.
  //0030  75 36 9c 6e 1a 1b eb 5f fb 89 db 0b ec a5 95 7a  u6.n..._.......z
  //0040  f3 e7 44 f6 1c 47 90 53 43 18 d1 6d 89 95 83 aa  ..D..G.SC..m....
  //0050  5e 7f ff 39 b3 47 36 e4 37 7e 05 1b 85 e3 b8 98  ^..9.G6.7~......
  //0060  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
  //0070  00 00 17                                         ...                                       ...

  const uint8_t expected_raw_data[] = {
    0x01, 0x00, 0x69, 0x50, 0x68, 0x6f, 0x6e, 0x65,
    0x20, 0x4d, 0x61, 0x72, 0x74, 0x79, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x36,
    0x9c, 0x6e, 0x1a, 0x1b, 0xeb, 0x5f, 0xfb, 0x89,
    0xdb, 0x0b, 0xec, 0xa5, 0x95, 0xab, 0x92, 0x8a,
    0xaa, 0xf6, 0x1c, 0x47, 0x90, 0x53, 0x43, 0xff,
    0x75, 0x36, 0x9c, 0x6e, 0x1a, 0x1b, 0xeb, 0x5f,
    0xfb, 0x89, 0xdb, 0x0b, 0xec, 0xa5, 0x95, 0x7a,
    0xf3, 0xe7, 0x44, 0xf6, 0x1c, 0x47, 0x90, 0x53,
    0x43, 0x18, 0xd1, 0x6d, 0x89, 0x95, 0x83, 0xaa,
    0x5e, 0x7f, 0xff, 0x39, 0xb3, 0x47, 0x36, 0xe4,
    0x37, 0x7e, 0x05, 0x1b, 0x85, 0xe3, 0xb8, 0x98,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x17,
  };
  size_t data_size = 115;

#else
#  error "Unknown version!"
#endif

  const SMPairingInfo pairing_info = {
    .local_encryption_info = {
      .ltk = {
        .data = {
          0x90, 0x36, 0x9c, 0x6e, 0x1a, 0x1b, 0xeb, 0x5f,
          0xfb, 0x89, 0xdb, 0x0b, 0xec, 0xa5, 0x95, 0xab,
        },
      },
      .rand = 0xff435390471cf6aa,
      .div = 0xf93f,
      .ediv = 0x8a92,
    },
    .remote_encryption_info = {
      .ltk = {
        .data = {
          0x75, 0x36, 0x9c, 0x6e, 0x1a, 0x1b, 0xeb, 0x5f,
          0xfb, 0x89, 0xdb, 0x0b, 0xec, 0xa5, 0x95, 0x7a,
        },
      },
      .rand = 0x18435390471cf644,
      .ediv = 0xe7f3,
    },
    .irk = {
      .data = {
        0xd1, 0x6d, 0x89, 0x95, 0x83, 0xaa, 0x5e, 0x7f,
        0xff, 0x39, 0xb3, 0x47, 0x36, 0xe4, 0x37, 0x7e,
      }
    },
    .identity = {
      {
        {
          .address = {
            .octets = {0x5, 0x1b, 0x85, 0xe3, 0xb8, 0x98}
          },
          .is_classic = 0x0,
          .is_random_address = 0x0,
          .zero = 0x0,
        },
      }
    },
    .csrk = {},
    .is_local_encryption_info_valid = 0x1,
    .is_remote_encryption_info_valid = 0x1,
    .is_remote_identity_info_valid = 0x1,
    .is_remote_signing_info_valid = 0x0,
    .is_mitm_protection_enabled = 0x1,
  };
  BTBondingID key = bt_persistent_storage_store_ble_pairing(&pairing_info, false /* is_gateway */,
                                                            "iPhone Marty",
                                                            false /* requires_address_pinning */,
                                                            false /* auto_accept_re_pairing */);
  cl_assert(key != BT_BONDING_ID_INVALID);

  uint8_t data[data_size];
  memset(data, 0, sizeof(data));
  int data_len = bt_persistent_storage_get_raw_data(&key, sizeof(key), data, data_size);
  cl_assert_equal_i(data_len, data_size);
  cl_assert_equal_m(expected_raw_data, data, sizeof(expected_raw_data));
}

void test_bluetooth_persistent_storage__v1_bt_classic(void) {
#if UNITTEST_BT_PERSISTENT_STORAGE_VERSION != 1
  // We only care about BT Classic in v1.
  return;
#endif

  BTDeviceAddress address = {
    .octets = {0x5, 0x1b, 0x85, 0xe3, 0xb8, 0x98},
  };
  SM128BitKey link_key = {
    .data = {
      0xb5, 0xa8, 0x09, 0xcc, 0x1a, 0xdf, 0xfa, 0x8e,
      0x96, 0x87, 0x76, 0xac, 0xcf, 0xb8, 0x15, 0x12,
    },
  };
  uint8_t platform_bits = 0x01;
  BTBondingID key = bt_persistent_storage_store_bt_classic_pairing(&address, &link_key,
                                                                   "iPhone Marty", &platform_bits);
  cl_assert(key != BT_BONDING_ID_INVALID);
  size_t v1_size = 110;
  uint8_t v1_data[v1_size];
  memset(v1_data, 0, sizeof(v1_data));
  int data_len = bt_persistent_storage_get_raw_data(&key, sizeof(key), v1_data, v1_size);
  cl_assert_equal_i(data_len, v1_size);

  //  00000000: 0005 1b85 e3b8 98b5 a809 cc1a dffa 8e96  ................
  //  00000010: 8776 accf b815 1269 5068 6f6e 6520 4d61  .v.....iPhone Ma
  //  00000020: 7274 7900 0000 0000 0000 0001 0000 0000  rty.............
  //  00000030: 0000 0000 0000 0000 0000 0000 0000 0000  ................
  //  00000040: 0000 0000 0000 0000 0000 0000 0000 0000  ................
  //  00000050: 0000 0000 0000 0000 0000 0000 0000 0000  ................
  //  00000060: 0000 0000 0000 0000 0000 0000 0000       ..............

  const uint8_t expected_raw_data[] = {
    0x00, 0x05, 0x1b, 0x85, 0xe3, 0xb8, 0x98, 0xb5,
    0xa8, 0x09, 0xcc, 0x1a, 0xdf, 0xfa, 0x8e, 0x96,
    0x87, 0x76, 0xac, 0xcf, 0xb8, 0x15, 0x12, 0x69,
    0x50, 0x68, 0x6f, 0x6e, 0x65, 0x20, 0x4d, 0x61,
    0x72, 0x74, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  cl_assert_equal_m(expected_raw_data, v1_data, sizeof(expected_raw_data));
}
