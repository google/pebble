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

#include "services/common/analytics/analytics.h"
#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/common/shared_prf_storage/shared_prf_storage.h"

#include "services/common/system_task.h"
#include "flash_region/flash_region_s29vs.h"
#include "util/size.h"

typedef struct GAPLEConnection GAPLEConnection;

#include "fake_bonding_sync.h"
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_spi_flash.h"
#include "fake_regular_timer.h"

#include "stubs_bluetopia_interface.h"
#include "stubs_bt_lock.h"
#include "stubs_gap_le_advert.h"
#include "stubs_bluetooth_analytics.h"
#include "stubs_gatt_client_discovery.h"
#include "stubs_gatt_client_subscriptions.h"
#include "stubs_hexdump.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_pairing_service.h"

bool bt_driver_supports_bt_classic(void) {
  return true;
}

void analytics_event_bt_error(AnalyticsEvent type, uint32_t error) {
}

void analytics_inc(AnalyticsMetric metric, AnalyticsClient client) {
}

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

void bt_local_addr_handle_bonding_change(BTBondingID bonding, BtPersistBondingOp op) {
}

extern RegularTimerInfo *shared_prf_storage_get_writeback_timer(void);
static void prv_fire_writeback_timer(void) {
  fake_regular_timer_trigger(shared_prf_storage_get_writeback_timer());
}

static int s_bonding_change_count;
static BtPersistBondingOp s_bonding_change_ops[2];
void kernel_le_client_handle_bonding_change(BTBondingID bonding, BtPersistBondingOp op) {
  if (s_bonding_change_count <= ARRAY_LENGTH(s_bonding_change_ops)) {
    s_bonding_change_ops[s_bonding_change_count] = op;
  }
  ++s_bonding_change_count;
}

static void prv_reset_change_op_tracking(void) {
  s_bonding_change_count = 0;
  for (int i = 0; i < ARRAY_LENGTH(s_bonding_change_ops); ++i) {
    s_bonding_change_ops[i] = BtPersistBondingOpInvalid;
  }
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

uint16_t gaps_get_starting_att_handle(void) {
  return 4;
}

void gatt_service_changed_server_cleanup_by_connection(GAPLEConnection *connection) {
}

void bt_pairability_update_due_to_bonding_change(void) {
}

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

bool system_task_add_callback(SystemTaskEventCallback cb, void *data) {
  cb(data);
  return true;
}

// Tests
///////////////////////////////////////////////////////////

void test_bluetooth_persistent_storage_prf__initialize(void) {
  bonding_sync_init();
  prv_reset_change_op_tracking();
  fake_spi_flash_init(FLASH_REGION_SHARED_PRF_STORAGE_BEGIN,
                      FLASH_REGION_SHARED_PRF_STORAGE_END - FLASH_REGION_SHARED_PRF_STORAGE_BEGIN);
  shared_prf_storage_init();
  bt_persistent_storage_init();
}

void test_bluetooth_persistent_storage_prf__cleanup(void) {
  fake_spi_flash_cleanup();
  bonding_sync_deinit();
}

void test_bluetooth_persistent_storage_prf__ble_store_and_get(void) {
  bool ret;

  // Output variables
  SMIdentityResolvingKey irk_out;
  BTDeviceInternal device_out;

  // Store a new pairing
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
  };
  BTBondingID id_1 = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  prv_fire_writeback_timer();
  cl_assert(id_1 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_bonding_change_count, 1);
  cl_assert_equal_i(s_bonding_change_ops[0], BtPersistBondingOpDidAdd);

  // Read it back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_1, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_1.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_1.identity, sizeof(device_out));

  // Re-pair device 1 again:
  // In case the device is the same as the existing pairing, make sure the operation is "change"
  // and not "delete"  to avoid disconnecting just because the existing pairing is deleted.
  // For bug details see https://pebbletechnology.atlassian.net/browse/PBL-24690
  prv_reset_change_op_tracking();
  id_1 = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */, NULL,
                                                 false /* requires_address_pinning */,
                                                 false /* auto_accept_re_pairing */);
  prv_fire_writeback_timer();
  cl_assert(id_1 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_bonding_change_count, 1);
  cl_assert_equal_i(s_bonding_change_ops[0], BtPersistBondingOpDidChange);

  // Store another pairing (different device):
  SMPairingInfo pairing_2 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x08,
        0x09, 0x02, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x20
      },
    },
    .identity = (BTDeviceInternal) {
                 .address = (BTDeviceAddress) {
                   .octets = {
                     0x21, 0x22, 0x13, 0x14, 0x15, 0x26
                   },
                 },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  prv_reset_change_op_tracking();
  BTBondingID id_2 = bt_persistent_storage_store_ble_pairing(&pairing_2, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  prv_fire_writeback_timer();
  cl_assert(id_2 != BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_bonding_change_count, 2);
  cl_assert_equal_i(s_bonding_change_ops[0], BtPersistBondingOpWillDelete);
  cl_assert_equal_i(s_bonding_change_ops[1], BtPersistBondingOpDidAdd);

  // Read it back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_2, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_2.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_2.identity, sizeof(device_out));

  // Store another pairing, this time it isn't a gateway
  SMPairingInfo pairing_3 = (SMPairingInfo) {
    .irk = (SMIdentityResolvingKey) {
      .data = {
        0x33, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x08,
        0x39, 0x02, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x20
      },
    },
    .identity = (BTDeviceInternal) {
                 .address = (BTDeviceAddress) {
                   .octets = {
                     0x33, 0x22, 0x13, 0x14, 0x15, 0x26
                   },
                 },
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  prv_reset_change_op_tracking();
  BTBondingID id_3 = bt_persistent_storage_store_ble_pairing(&pairing_2, false /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  prv_fire_writeback_timer();
  cl_assert(id_3 == BT_BONDING_ID_INVALID);
  cl_assert_equal_i(s_bonding_change_count, 0);

  // Read out the stored pairing (id_2 should still be stored)
  ret = bt_persistent_storage_get_ble_pairing_by_id(id_1, &irk_out, &device_out, NULL /* name */);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing_2.irk, sizeof(irk_out));
  cl_assert_equal_m(&device_out, &pairing_2.identity, sizeof(device_out));

  bt_persistent_storage_register_existing_ble_bondings();
  cl_assert_equal_b(bonding_sync_contains_pairing_info(&pairing_1, true), false);
  cl_assert_equal_b(bonding_sync_contains_pairing_info(&pairing_2, true), true);
  cl_assert_equal_b(bonding_sync_contains_pairing_info(&pairing_3, false), false);
}

void test_bluetooth_persistent_storage_prf__get_ble_by_address(void) {
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
  prv_fire_writeback_timer();
  cl_assert(id != BT_BONDING_ID_INVALID);

  // Read it back
  ret = bt_persistent_storage_get_ble_pairing_by_addr(&pairing.identity, &irk_out, NULL);
  cl_assert(ret);
  cl_assert_equal_m(&irk_out, &pairing.irk, sizeof(irk_out));
}


void test_bluetooth_persistent_storage_prf__delete_ble_pairing_by_id(void) {
  bool ret;

  // Output variables
  SMIdentityResolvingKey irk_out;
  BTDeviceInternal device_out;

  // Store a pairing
  SMPairingInfo pairing = (SMPairingInfo) {
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
  };

  BleBonding ble_bonding = (BleBonding) {
    .is_gateway = true,
    .pairing_info = pairing,
  };
  bonding_sync_add_bonding(&ble_bonding);
  BTBondingID id = bt_persistent_storage_store_ble_pairing(&pairing, true /* is_gateway */, NULL,
                                                           false /* requires_address_pinning */,
                                                           false /* auto_accept_re_pairing */);
  prv_fire_writeback_timer();
  cl_assert(id != BT_BONDING_ID_INVALID);

  // Delete the Pairing
  bt_persistent_storage_delete_ble_pairing_by_id(id);

  // Try to read it back
  ret = bt_persistent_storage_get_ble_pairing_by_id(id, &irk_out, &device_out, NULL);
  cl_assert(!ret);

  // Add the pairing again
  id = bt_persistent_storage_store_ble_pairing(&pairing, true /* is_gateway */, NULL,
                                               false /* requires_address_pinning */,
                                               false /* auto_accept_re_pairing */);
  prv_fire_writeback_timer();
  cl_assert(id != BT_BONDING_ID_INVALID);
}


// ///////////////////////////////////////////////////////////////////////////////////////////////////
// //! BT Classic Pairing Info

void test_bluetooth_persistent_storage_prf__bt_classic_store_and_get(void) {
  bool ret;

  // Output variables
  BTDeviceAddress addr_out;
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_1 = {.octets = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_1 = {
    .data = {
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}
  };
  char name_1[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_1 = 0x11;
  BTBondingID id_1 = bt_persistent_storage_store_bt_classic_pairing(&addr_1, &link_key_1,
                                                            name_1, &platform_bits_1);
  prv_fire_writeback_timer();
  cl_assert(id_1 != BT_BONDING_ID_INVALID);

  // Read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_1, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_1, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_1, name_out);
  cl_assert_equal_i(platform_bits_1, platform_bits_out);

  // Store another pairing
  BTDeviceAddress addr_2 = {.octets = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26}};
  SM128BitKey link_key_2 = {
    .data = {
      0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
      0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    },
  };
  char name_2[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 2";
  uint8_t platform_bits_2 = 0x22;
  BTBondingID id_2 = bt_persistent_storage_store_bt_classic_pairing(&addr_2, &link_key_2,
                                                            name_2, &platform_bits_2);
  prv_fire_writeback_timer();
  cl_assert(id_2 != BT_BONDING_ID_INVALID);

  // Read it pairings back (purposefully using the wrong bonding ID cause it doesn't matter)
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_2, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_2, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_2, name_out);
  cl_assert_equal_i(platform_bits_2, platform_bits_out);

  // Add a third pairing
  BTDeviceAddress addr_3 = {.octets = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36}};
  SM128BitKey link_key_3 = {
    .data = {
      0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
      0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    },
  };
  char name_3[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 3";
  uint8_t platform_bits_3 = 0x33;
  BTBondingID id_3 = bt_persistent_storage_store_bt_classic_pairing(&addr_3, &link_key_3,
                                                            name_3, &platform_bits_3);
  prv_fire_writeback_timer();
  cl_assert(id_3 != BT_BONDING_ID_INVALID);

  // Read all three pairings back (purposefully using the wrong bonding ID cause it doesn't matter)
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id_1, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(ret);
  cl_assert_equal_m(&addr_3, &addr_out, sizeof(addr_out));
  cl_assert_equal_m(&link_key_3, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_3, name_out);
  cl_assert_equal_i(platform_bits_3, platform_bits_out);
}

void test_bluetooth_persistent_storage_prf__get_bt_classic_pairing_by_addr(void) {
  // Output variables
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_in = {.octets = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_in = {
    .data = {
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    },
  };
  char name_in[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_in = 0x11;

  BTBondingID id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                                             name_in, &platform_bits_in);
  prv_fire_writeback_timer();
  cl_assert(id != BT_BONDING_ID_INVALID);

  // Read it back
  BTBondingID id_out = bt_persistent_storage_get_bt_classic_pairing_by_addr(&addr_in, &link_key_out,
                                                                     name_out, &platform_bits_out);
  cl_assert_equal_i(id, id_out);
  cl_assert_equal_m(&link_key_in, &link_key_out, sizeof(link_key_out));
  cl_assert_equal_s(name_in, name_out);
  cl_assert_equal_i(platform_bits_in, platform_bits_out);
}


void test_bluetooth_persistent_storage_prf__delete_bt_classic_pairing_by_id(void) {
 bool ret;

  // Output variables
  BTDeviceAddress addr_out;
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_in = {.octets = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_in = {
    .data = {
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    },
  };
  char name_in[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_in = 0x11;

  BTBondingID id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                                             name_in, &platform_bits_in);
  prv_fire_writeback_timer();
  cl_assert(id != BT_BONDING_ID_INVALID);

  // Delete the Pairing
  bt_persistent_storage_delete_bt_classic_pairing_by_id(id);

  // Try to read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(!ret);

  // Add the pairing again
  id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                               name_in, &platform_bits_in);
  prv_fire_writeback_timer();
  cl_assert(id != BT_BONDING_ID_INVALID);

  // And delete is again
  bt_persistent_storage_delete_bt_classic_pairing_by_id(id);

  // Try to read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(!ret);
}

void test_bluetooth_persistent_storage_prf__delete_bt_classic_pairing_by_addr(void) {
 bool ret;

  // Output variables
  BTDeviceAddress addr_out;
  SM128BitKey link_key_out;
  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  uint8_t platform_bits_out;

  // Store a new pairing
  BTDeviceAddress addr_in = {.octets = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_in = {
    .data = {
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    },
  };
  char name_in[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_in = 0x11;

  BTBondingID id = bt_persistent_storage_store_bt_classic_pairing(&addr_in, &link_key_in,
                                                             name_in, &platform_bits_in);
  prv_fire_writeback_timer();
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
  prv_fire_writeback_timer();
  cl_assert(id != BT_BONDING_ID_INVALID);

  // And delete is again
  bt_persistent_storage_delete_bt_classic_pairing_by_addr(&addr_in);

  // Try to read it back
  ret = bt_persistent_storage_get_bt_classic_pairing_by_id(id, &addr_out, &link_key_out,
                                                    name_out, &platform_bits_out);
  cl_assert(!ret);
}


// ///////////////////////////////////////////////////////////////////////////////////////////////////
// //! Local Device Info

void test_bluetooth_persistent_storage_prf__test_active_gateway(void) {
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
  BTDeviceAddress addr_1 = {.octets = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_1 = {
    .data = {
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    },
  };
  char name_1[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 1";
  uint8_t platform_bits_1 = 0x11;

  BTBondingID id_1 = bt_persistent_storage_store_bt_classic_pairing(&addr_1, &link_key_1,
                                                             name_1, &platform_bits_1);
  prv_fire_writeback_timer();
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
  BTDeviceAddress addr_2 = {.octets = {0x22, 0x12, 0x13, 0x14, 0x15, 0x16}};
  SM128BitKey link_key_2 = {
    .data = {
      0x22, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    },
  };
  char name_2[BT_DEVICE_NAME_BUFFER_SIZE] = "Device 2";
  uint8_t platform_bits_2 = 0x22;
  BTBondingID id_2 = bt_persistent_storage_store_bt_classic_pairing(&addr_2, &link_key_2,
                                                             name_2, &platform_bits_2);
  prv_fire_writeback_timer();
  cl_assert(id_2 != BT_BONDING_ID_INVALID);

  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(ret);
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
    .irk = (SMIdentityResolvingKey) {
      .data= {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00
      },
    },
    .identity = (BTDeviceInternal) {
      .address = (BTDeviceAddress) {.octets = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16}},
      .is_classic = false,
      .is_random_address = false,
    },
    .is_remote_identity_info_valid = true,
  };
  BTBondingID id_3 = bt_persistent_storage_store_ble_pairing(&pairing_1, true /* is_gateway */,
                                                             NULL,
                                                             false /* requires_address_pinning */,
                                                             false /* auto_accept_re_pairing */);
  prv_fire_writeback_timer();
  cl_assert(id_3 != BT_BONDING_ID_INVALID);

  // There should now be an active BLE gateway
  ret = bt_persistent_storage_get_active_gateway(&id_out, &type_out);
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_bt_classic_gateway_bonding();
  cl_assert(!ret);
  ret = bt_persistent_storage_has_active_ble_gateway_bonding();
  cl_assert(ret);
}
