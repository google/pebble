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

#include "comm/ble/kernel_le_client/ppogatt/ppogatt.h"
#include "comm/ble/kernel_le_client/ppogatt/ppogatt_internal.h"
#include "services/common/comm_session/session_transport.h"
#include "services/common/regular_timer.h"

#include <util/size.h>

#include "clar.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_analytics.h"
#include "stubs_bt_conn_mgr.h"
#include "stubs_bt_lock.h"
#include "stubs_logging.h"
#include "stubs_mfg_info.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_print.h"
#include "stubs_prompt.h"
#include "stubs_rand_ptr.h"
#include "stubs_serial.h"

// Fakes
///////////////////////////////////////////////////////////

#include "fake_gatt_client_operations.h"
#include "fake_gatt_client_subscriptions.h"
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_session.h"
#include "fake_system_task.h"

#define MTU_SIZE (158)
#define MAX_PAYLOAD_SIZE (MTU_SIZE - 3 /* ATT Header size */ - 1 /* PPoGATT Packet Header */)

uint16_t s_mtu_size;

int bt_driver_gap_le_disconnect(const BTDeviceInternal *peer_address) {
  return 0;
}

uint16_t gap_le_connection_get_gatt_mtu(const BTDeviceInternal *device) {
  return s_mtu_size;
}

GAPLEConnection *gap_le_connection_get_gateway(void) {
  return NULL;
}

GAPLEConnection *gatt_client_characteristic_get_connection(BLECharacteristic characteristic_ref) {
  return NULL;
}

static BTDeviceInternal s_device = {};

BTDeviceInternal gatt_client_characteristic_get_device(BLECharacteristic characteristic_ref) {
  return s_device;
}

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

// Helpers
///////////////////////////////////////////////////////////

extern Transport *ppogatt_client_for_uuid(const Uuid *uuid);
extern bool ppogatt_has_client_for_uuid(const Uuid *uuid);
extern uint32_t ppogatt_client_count(void);
extern void ppogatt_trigger_rx_ack_send_timeout(void);
extern TransportDestination ppogatt_get_destination(Transport *transport);

static const uint8_t s_num_service_instances = 2;
static BLECharacteristic s_characteristics[s_num_service_instances][PPoGATTCharacteristicNum] = {
  [0] = {
    [PPoGATTCharacteristicData] = 01,
    [PPoGATTCharacteristicMeta] = 02,
  },
  [1] = {
    [PPoGATTCharacteristicData] = 11,
    [PPoGATTCharacteristicMeta] = 12,
  },
};

static const BLECharacteristic s_unknown_characteristics = 0x55;

static const PPoGATTMetaV0 s_meta_v0_app = {
  .ppogatt_min_version = PPOGATT_MIN_VERSION,
  .ppogatt_max_version = USE_PPOGATT_VERSION,
  .app_uuid = UuidMake(0xA4, 0x83, 0x2A, 0x0E, 0x74, 0x54, 0x45, 0x32,
                       0xB2, 0xA2, 0x4E, 0x6F, 0x8F, 0x7B, 0x68, 0x6F)
};

static const PPoGATTMetaV0 s_meta_v0_system = {
  .ppogatt_min_version = PPOGATT_MIN_VERSION,
  .ppogatt_max_version = USE_PPOGATT_VERSION,
  .app_uuid = (const Uuid) UUID_SYSTEM,
};

static const PPoGATTMetaV1 s_meta_v1_hybrid = {
  .ppogatt_min_version = 0,
  .ppogatt_max_version = 0,
  .app_uuid = (const Uuid) UUID_SYSTEM,
  .pp_session_type = PPoGATTSessionType_Hybrid,
};

static const PPoGATTMetaV1 s_meta_v1_system_inferred = {
  .ppogatt_min_version = 0,
  .ppogatt_max_version = 0,
  .app_uuid = (const Uuid) UUID_SYSTEM,
  .pp_session_type = PPoGATTSessionType_InferredFromUuid,
};

static const PPoGATTMetaV1 s_meta_v1_app_inferred = {
  .ppogatt_min_version = 0,
  .ppogatt_max_version = 0,
  .app_uuid = UuidMake(0xA4, 0x83, 0x2A, 0x0E, 0x74, 0x54, 0x45, 0x32,
                       0xB2, 0xA2, 0x4E, 0x6F, 0x8F, 0x7B, 0x68, 0x6F),
  .pp_session_type = PPoGATTSessionType_InferredFromUuid,
};

static PPoGATTPacket s_reset_complete = (const PPoGATTPacket) {
  .sn = 0,
  .type = PPoGATTPacketTypeResetComplete,
};

static PPoGATTPacket s_server_reset_request = (const PPoGATTPacket) {
  .sn = 0,
  .type = PPoGATTPacketTypeResetRequest,
};

static PPoGATTPacket * s_client_reset_request;
static uint16_t s_client_reset_request_size;

static PPoGATTPacket * s_client_reset_complete;
static uint16_t s_client_reset_complete_size;

static int s_ppogatt_version;
static int s_tx_window_size;
static int s_rx_window_size;

static void prv_create_expected_reset_request(void) {
  s_client_reset_request_size = sizeof(PPoGATTPacket) + sizeof(PPoGATTResetRequestClientIDPayload);
  s_client_reset_request = (PPoGATTPacket *) malloc(s_client_reset_request_size);
  *s_client_reset_request = (const PPoGATTPacket) {
    .sn = 0,
    .type = PPoGATTPacketTypeResetRequest,
  };
  PPoGATTResetRequestClientIDPayload *client_id_payload =
  (PPoGATTResetRequestClientIDPayload *)s_client_reset_request->payload;
  *client_id_payload = (const PPoGATTResetRequestClientIDPayload) {
    .ppogatt_version = s_ppogatt_version,
  };
  memcpy(client_id_payload->serial_number, mfg_get_serial_number(), MFG_SERIAL_NUMBER_SIZE);
}

static void prv_create_expected_reset_complete(void) {
  s_client_reset_complete_size = sizeof(PPoGATTPacket);
  if (s_ppogatt_version  > 0) {
    s_client_reset_complete_size += sizeof(PPoGATTResetCompleteClientIDPayloadV1);
  }

  s_client_reset_complete = (PPoGATTPacket *) malloc(s_client_reset_complete_size);
  *s_client_reset_complete = (const PPoGATTPacket) {
    .sn = 0,
    .type = PPoGATTPacketTypeResetComplete,
  };

  if (s_ppogatt_version > 0) {
    *((PPoGATTResetCompleteClientIDPayloadV1 *)s_client_reset_complete->payload) =
        (const PPoGATTResetCompleteClientIDPayloadV1) {
      .ppogatt_max_rx_window = s_rx_window_size,
      .ppogatt_max_tx_window = s_tx_window_size,
    };
  }
}

static void prv_receive_reset_request(BLECharacteristic characteristic) {
  ppogatt_handle_read_or_notification(characteristic, (const uint8_t *) &s_server_reset_request,
                                      sizeof(s_server_reset_request), BLEGATTErrorSuccess);
}
static void prv_receive_reset_complete(BLECharacteristic characteristic) {
  ppogatt_handle_read_or_notification(characteristic, (const uint8_t *)s_client_reset_complete,
                                      s_client_reset_complete_size, BLEGATTErrorSuccess);
}

static const uint8_t s_short_data_fragment[] = {
  0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
};

static void prv_receive_short_data_fragment(BLECharacteristic characteristic, uint8_t sn) {
  PPoGATTPacket *packet = malloc(sizeof(PPoGATTPacket) + sizeof(s_short_data_fragment));
  packet->sn = sn;
  packet->type = PPoGATTPacketTypeData;
  memcpy(packet->payload, s_short_data_fragment, sizeof(s_short_data_fragment));
  ppogatt_handle_read_or_notification(characteristic, (const uint8_t *) packet,
                                      sizeof(s_short_data_fragment), BLEGATTErrorSuccess);
  free(packet);
}

static void prv_receive_ack(BLECharacteristic characteristic, uint8_t sn) {
  const PPoGATTPacket ack = (const PPoGATTPacket) {
    .sn = sn,
    .type = PPoGATTPacketTypeAck,
  };
  ppogatt_handle_read_or_notification(characteristic, (const uint8_t *) &ack,
                                      sizeof(ack), BLEGATTErrorSuccess);
}

static void prv_assert_sent_reset_request(BLECharacteristic characteristic) {
  fake_gatt_client_op_assert_write(characteristic,
                                   (const uint8_t *) s_client_reset_request,
                                   s_client_reset_request_size,
                                   GAPLEClientKernel, false /* is_response_required */);
}

static void prv_assert_sent_reset_complete(BLECharacteristic characteristic) {
  struct PACKED {
    PPoGATTPacketType type:3;
    uint8_t sn:PPOGATT_SN_BITS;
    PPoGATTResetCompleteClientIDPayloadV1 payload;
  } expected_response = {
    .sn = 0,
    .type = PPoGATTPacketTypeResetComplete
  };

  if (s_ppogatt_version > 0) {
    expected_response.payload = (const PPoGATTResetCompleteClientIDPayloadV1) {
      .ppogatt_max_rx_window = PPOGATT_V1_DESIRED_RX_WINDOW_SIZE,
      .ppogatt_max_tx_window = PPOGATT_V0_WINDOW_SIZE,
    };
  }

  fake_gatt_client_op_assert_write(characteristic,
                                   (const uint8_t *) &expected_response, s_client_reset_complete_size,
                                   GAPLEClientKernel, false /* is_response_required */);

  if (s_ppogatt_version > 0) {
    s_tx_window_size = MIN(s_tx_window_size, expected_response.payload.ppogatt_max_tx_window);
    s_rx_window_size = MIN(s_rx_window_size, expected_response.payload.ppogatt_max_rx_window);
  } else {
    s_tx_window_size = s_rx_window_size =  PPOGATT_V0_WINDOW_SIZE;
  }
}

static void prv_assert_sent_ack(BLECharacteristic characteristic, uint8_t sn) {
  const PPoGATTPacket ack = (const PPoGATTPacket) {
    .sn = sn,
    .type = PPoGATTPacketTypeAck,
  };
  fake_gatt_client_op_assert_write(characteristic, (const uint8_t *) &ack, sizeof(ack),
                                   GAPLEClientKernel, false /* is_response_required */);
}

static void prv_assert_sent_data(BLECharacteristic characteristic, uint8_t sn,
                                 const uint8_t *data, size_t length) {
  cl_assert(length <= MAX_PAYLOAD_SIZE);
  PPoGATTPacket *packet = malloc(sizeof(PPoGATTPacket) + length);
  packet->sn = sn;
  packet->type = PPoGATTPacketTypeData;
  memcpy(packet->payload, data, length);
  fake_gatt_client_op_assert_write(characteristic, (const uint8_t *) packet,
                                   sizeof(PPoGATTPacket) + length,
                                   GAPLEClientKernel, false /* is_response_required */);
  free(packet);
}

// Tests
///////////////////////////////////////////////////////////

void test_ppogatt__initialize(void) {
  s_ppogatt_version = USE_PPOGATT_VERSION;
  s_tx_window_size = 25;
  s_rx_window_size = 25;
  prv_create_expected_reset_request();
  prv_create_expected_reset_complete();
  s_mtu_size = MTU_SIZE;
  fake_pbl_malloc_clear_tracking();
  fake_gatt_client_op_init();
  fake_gatt_client_subscriptions_init();
  regular_timer_init();
  fake_comm_session_init();
  ppogatt_create();
}

void test_ppogatt__cleanup(void) {
  ppogatt_destroy();
  cl_assert_equal_i(ppogatt_client_count(), 0);
  cl_assert_equal_i(regular_timer_seconds_count(), 0);
  regular_timer_deinit();
  fake_gatt_client_op_deinit();
  fake_gatt_client_subscriptions_deinit();

  // Check for leaks:
  fake_pbl_malloc_check_net_allocs();
  fake_pbl_malloc_clear_tracking();

  fake_comm_session_cleanup();
  free(s_client_reset_request);
  free(s_client_reset_complete);
}

void prv_notify_services_discovered(int num_services_to_register) {
  for (int i = 0; i < s_num_service_instances && i < num_services_to_register; i++) {
    ppogatt_handle_service_discovered(s_characteristics[i]);
  }
}

void test_ppogatt__find_pebble_app_and_3rd_party_app(void) {
  prv_notify_services_discovered(s_num_service_instances);

  // Assert GATT reads requests to Meta characteristics happened:
  fake_gatt_client_op_assert_read(s_characteristics[0][PPoGATTCharacteristicMeta],
                                  GAPLEClientKernel);
  fake_gatt_client_op_assert_read(s_characteristics[1][PPoGATTCharacteristicMeta],
                                  GAPLEClientKernel);

  // Simulate read responses:
  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system, sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), true);

  ppogatt_handle_read_or_notification(s_characteristics[1][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_app, sizeof(s_meta_v0_app),
                                      BLEGATTErrorSuccess);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_app.app_uuid), true);
}

void test_ppogatt__handles_unknown_read_response(void) {
  uint8_t data;
  ppogatt_handle_read_or_notification(s_unknown_characteristics,
                                      &data, sizeof(data), BLEGATTErrorSuccess);
  // No crashes / asserts etc.
}

void test_ppogatt__handles_too_short_meta_length(void) {
  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system) - 1 /* missing last byte */,
                                      BLEGATTErrorSuccess);
  // No client created:
  cl_assert_equal_i(ppogatt_client_count(), 0);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), false);
}

void test_ppogatt__handles_meta_v1(void) {
  struct {
    const PPoGATTMetaV1 *meta;
    TransportDestination expected_destination;
  } metas[] = {
    {
      .meta = &s_meta_v1_hybrid,
      .expected_destination = TransportDestinationHybrid,
    },
    {
      .meta = &s_meta_v1_system_inferred,
      .expected_destination = TransportDestinationSystem,
    },
    {
      .meta = &s_meta_v1_app_inferred,
      .expected_destination = TransportDestinationApp,
    },
  };

  for (int i = 0; i < ARRAY_LENGTH(metas); ++i) {
    prv_notify_services_discovered(1);
    ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                        (const uint8_t *) metas[i].meta,
                                        sizeof(PPoGATTMetaV1),
                                        BLEGATTErrorSuccess);
    // Client created:
    cl_assert_equal_i(ppogatt_client_count(), 1);

    Transport *client = ppogatt_client_for_uuid(&metas[i].meta->app_uuid);
    cl_assert_equal_i(ppogatt_get_destination(client), metas[i].expected_destination);
    ppogatt_close(client);
  }
}

void test_ppogatt__handles_unsupported_meta_ppogatt_version(void) {
  PPoGATTMetaV0 future_meta_non_compatible = s_meta_v0_system;
  future_meta_non_compatible.ppogatt_min_version = 0xaa;
  future_meta_non_compatible.ppogatt_max_version = 0xff;

  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &future_meta_non_compatible,
                                      sizeof(future_meta_non_compatible),
                                      BLEGATTErrorSuccess);
  // No client created:
  cl_assert_equal_i(ppogatt_client_count(), 0);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&future_meta_non_compatible.app_uuid), false);
}

void test_ppogatt__handles_invalid_uuid_meta(void) {
  PPoGATTMetaV0 meta_invalid_uuid = s_meta_v0_system;
  meta_invalid_uuid.app_uuid = UUID_INVALID;

  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &meta_invalid_uuid,
                                      sizeof(meta_invalid_uuid),
                                      BLEGATTErrorSuccess);
  // No client created:
  cl_assert_equal_i(ppogatt_client_count(), 0);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&meta_invalid_uuid.app_uuid), false);
}

void test_ppogatt__deletes_existing_client_after_rediscovery(void) {
  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  // Client created:
  cl_assert_equal_i(ppogatt_client_count(), 1);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), true);
  Transport *client = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Rediscovery:
  ppogatt_invalidate_all_references();
  prv_notify_services_discovered(1);

  // Still one client:
  cl_assert_equal_i(ppogatt_client_count(), 1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  // Still one client:
  cl_assert_equal_i(ppogatt_client_count(), 1);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), true);
  Transport *client2 = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);
}

void test_ppogatt__invalidate_characteristic_refs_immediately_after_update(void) {
  prv_notify_services_discovered(1);
  ppogatt_handle_service_removed(&s_characteristics[0][0], PPoGATTCharacteristicNum);
  const bool can_handle =
                 ppogatt_can_handle_characteristic(s_characteristics[0][PPoGATTCharacteristicData]);
  cl_assert_equal_b(can_handle, false);
}

void test_ppogatt__handle_subscribe_to_unknown_characteristic(void) {
  ppogatt_handle_subscribe(s_unknown_characteristics, BLESubscriptionNotifications,
                           BLEGATTErrorSuccess);

  // Expect to unsubscribe from the unknown characteristic:
  fake_gatt_client_subscriptions_assert_subscribe(s_unknown_characteristics, BLESubscriptionNone,
                                                  GAPLEClientKernel);
}

void test_ppogatt__cleanup_client_when_meta_read_fails(void) {
  fake_gatt_client_op_set_read_return_value(BTErrnoInvalidParameter);
  prv_notify_services_discovered(1);
  cl_assert_equal_i(ppogatt_client_count(), 0);
}

void test_ppogatt__cleanup_client_when_meta_read_gets_error_response(void) {
  fake_gatt_client_op_set_read_return_value(BTErrnoOK);
  prv_notify_services_discovered(1);
  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      NULL, 0,
                                      BLEGATTErrorInvalidHandle);
  cl_assert_equal_i(ppogatt_client_count(), 0);
}

void test_ppogatt__cleanup_client_when_data_subscription_cccd_write_failed(void) {
  fake_gatt_client_subscriptions_set_subscribe_return_value(BTErrnoInvalidParameter);

  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  cl_assert_equal_i(ppogatt_client_count(), 0);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), false);
}

void test_ppogatt__cleanup_client_when_data_subscription_error_response(void) {
  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  // Expect subscribe request was made:
  fake_gatt_client_subscriptions_assert_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                                                  BLESubscriptionNotifications,
                                                  GAPLEClientKernel);
  // Simulate getting the subscription failure:
  ppogatt_handle_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                           BLESubscriptionNotifications, BLEGATTErrorReadNotPermitted);
  cl_assert_equal_i(ppogatt_client_count(), 0);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), false);
}

static void prv_discover_and_read_meta_and_reset(void) {
  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  // Expect subscribe request was made:
  fake_gatt_client_subscriptions_assert_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                                                  BLESubscriptionNotifications,
                                                  GAPLEClientKernel);
  // Simulate getting the subscription confirmation:
  ppogatt_handle_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                           BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Expect Reset to be initiated ("Reset Request" sent by FW):
  prv_assert_sent_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);

  // Session should still no have opened yet:
  cl_assert_equal_i(fake_comm_session_open_call_count(), 0);
}

void test_ppogatt__open_session_when_found_pebble_app(void) {
  prv_discover_and_read_meta_and_reset();

  // Simulate getting "Reset Complete" from remote:
  prv_receive_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);

  // Expect "Reset Complete" to be sent by FW:
  prv_assert_sent_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);

  // Expect Session to be opened now:
  cl_assert_equal_i(fake_comm_session_open_call_count(), 1);
}

void test_ppogatt__start_reset_upon_out_of_range_ack(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  // Simulate getting an Ack that's outside of the window of outstanding SNs:
  prv_receive_ack(s_characteristics[0][PPoGATTCharacteristicData], PPOGATT_SN_MOD_DIV / 2);
  // Expect Reset to be initiated ("Reset Request" sent by FW):
  prv_assert_sent_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
}

void test_ppogatt__ignore_retransmitted_ack(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);
  for (uint8_t sn = 0; sn < 3; ++sn) {
    const bool success =
        fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                             s_short_data_fragment,
                                                             sizeof(s_short_data_fragment));
    cl_assert_equal_b(success, true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment));
  }

  // Receive ACK for first data packet with sn=0:
  prv_receive_ack(s_characteristics[0][PPoGATTCharacteristicData], 0);

  // Pretend data packets with sn=1 got lost in the ether, but data sn=2 was received...

  // Receive a retransmit for the ACK sn=0, to indicate data was missing.
  prv_receive_ack(s_characteristics[0][PPoGATTCharacteristicData], 0);

  // The retransmitted ACK should be ignored.
  fake_gatt_client_op_assert_no_write();

  // Session shouldn't get closed:
  cl_assert_equal_i(fake_comm_session_close_call_count(), 0);
}

void test_ppogatt__ignore_server_reset_request_while_resetting_due_to_server_reset_request(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  prv_receive_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
  prv_assert_sent_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);

  prv_receive_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
  fake_gatt_client_op_assert_no_write();
}

void test_ppogatt__ignore_server_reset_request_while_resetting_due_to_own_reset_request(void) {
  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  ppogatt_handle_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                           BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Expect Reset to be initiated ("Reset Request" sent by FW):
  prv_assert_sent_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);

  prv_receive_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
  fake_gatt_client_op_assert_no_write();
}

void test_ppogatt__timeout_waiting_for_reset_complete_remote_initiated(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  prv_receive_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
  prv_assert_sent_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);

  // Timeout waiting for "Reset Complete":
  for (int i = 0; i < PPOGATT_TIMEOUT_TICKS; ++i) {
    regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
  }

  // Expect "Reset Request" sent by FW:
  prv_assert_sent_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
}

void test_ppogatt__timeout_waiting_for_reset_complete_self_initiated(void) {
  prv_discover_and_read_meta_and_reset();

  // Timeout waiting for "Reset Complete":
  for (int i = 0; i < PPOGATT_TIMEOUT_TICKS; ++i) {
    regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
  }

  // Expect "Reset Request" sent by FW:
  prv_assert_sent_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
}

void test_ppogatt__server_reset_request_while_pending_ack(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  // Simulate outbound queue full, so ack will have to wait until there's buffer space:
  fake_gatt_client_op_set_write_return_value(BTErrnoNotEnoughResources);
  // Receive data (that needs to be ack'd):
  uint8_t sn = 0;
  prv_receive_short_data_fragment(s_characteristics[0][PPoGATTCharacteristicData], sn);
  fake_gatt_client_op_assert_no_write();

  // Receive Reset Request:
  prv_receive_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
  fake_gatt_client_op_assert_no_write();

  // Simulate outbound queue having space again:
  fake_gatt_client_op_set_write_return_value(BTErrnoOK);
  ppogatt_handle_buffer_empty();

  // Expect Reset Complete to be sent out, but nothing more than that:
  prv_assert_sent_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);
  fake_gatt_client_op_assert_no_write();

  // In the past we had a bug here where the pending ACK would get sent out.
  // See https://pebbletechnology.atlassian.net/browse/PBL-24651
}

void test_ppogatt__ignore_invalid_packet_type(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  PPoGATTPacket packet = {
    .sn = 0,
    .type = PPoGATTPacketTypeInvalidRangeStart,
  };
  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicData],
                                      (const uint8_t *) &packet, sizeof(packet),
                                      BLEGATTErrorSuccess);
  // No crash etc, client still alive:
  cl_assert_equal_i(ppogatt_client_count(), 1);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), true);
  cl_assert_equal_i(fake_comm_session_close_call_count(), 0);
}

void test_ppogatt__ignore_reset_complete_while_open(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  // Simulate getting "Reset Complete" from remote:
  prv_receive_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);
  // No crash etc, client still alive:
  cl_assert_equal_i(ppogatt_client_count(), 1);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), true);
  cl_assert_equal_i(fake_comm_session_close_call_count(), 0);
}

void test_ppogatt__ignore_data_during_reset(void) {
  prv_notify_services_discovered(1);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  ppogatt_handle_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                           BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Expect Reset to be initiated ("Reset Request" sent by FW):
  prv_assert_sent_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);

  // Receive data:
  prv_receive_short_data_fragment(s_characteristics[0][PPoGATTCharacteristicData], 3 /* sn */);

  // Simulate getting "Reset Complete" from remote:
  prv_receive_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);

  // Expect "Reset Complete" to be sent by FW:
  prv_assert_sent_reset_complete(s_characteristics[0][PPoGATTCharacteristicData]);

  // Expect Session to be opened now:
  cl_assert_equal_i(fake_comm_session_open_call_count(), 1);
}

void test_ppogatt__ignore_zero_length_notification(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicData], NULL, 0,
                                      BLEGATTErrorSuccess);
  // No crash etc, client still alive:
  cl_assert_equal_i(ppogatt_client_count(), 1);
  cl_assert_equal_b(ppogatt_has_client_for_uuid(&s_meta_v0_system.app_uuid), true);
  cl_assert_equal_i(fake_comm_session_close_call_count(), 0);
}

void test_ppogatt__ack_received_data(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  // Receive data:
  for (uint8_t i = 0; i < PPOGATT_SN_MOD_DIV + 1; ++i) {
    const uint8_t sn = i % PPOGATT_SN_MOD_DIV;
    ppogatt_trigger_rx_ack_send_timeout();
    prv_receive_short_data_fragment(s_characteristics[0][PPoGATTCharacteristicData], sn);
    prv_assert_sent_ack(s_characteristics[0][PPoGATTCharacteristicData], sn);
  }
}

void test_ppogatt__close(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  Transport *client = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);
  cl_assert(client);

  ppogatt_close(client);

  cl_assert_equal_p(NULL, ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid));
}

void test_ppogatt__missing_inbound_packet(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  // Receive data:
  prv_receive_short_data_fragment(s_characteristics[0][PPoGATTCharacteristicData],
                                  1 /* sn (expecting sn=0) */);
  // Expect nothing to be sent, rely on other end to hit time-out and retransmit
  fake_gatt_client_op_assert_no_write();
}

void test_ppogatt__send_data_max_payload_size(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  uint8_t *data = malloc(MAX_PAYLOAD_SIZE);
  memset(data, MAX_PAYLOAD_SIZE, 0x55);

  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);
  for (uint8_t sn = 0; sn < s_tx_window_size; ++sn) {
    cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport, data,
                                                                           MAX_PAYLOAD_SIZE), true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         data, MAX_PAYLOAD_SIZE);
  }

 free(data);
}

void test_ppogatt__cap_number_of_data_packets_in_flight(void) {
  test_ppogatt__open_session_when_found_pebble_app();

  uint8_t sn = 0;
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Get s_rx_window_size packets in flight:
  for (sn = 0; sn < s_tx_window_size; ++sn) {
    const bool success = fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                                    s_short_data_fragment,
                                                                    sizeof(s_short_data_fragment));
    printf("SEND %d %d\n", sn, success);
    cl_assert_equal_b(success, true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment));
  }

  printf("done\n");

  // Enqueue another:
  cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                             s_short_data_fragment,
                                                             sizeof(s_short_data_fragment)), true);
  ppogatt_send_next(transport);
  fake_gatt_client_op_assert_no_write();

  // Ack the first one (sn=0):
  prv_receive_ack(s_characteristics[0][PPoGATTCharacteristicData], 0 /* sn */);

  // The last enqueued one should now be sent out:
  prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                       s_short_data_fragment, sizeof(s_short_data_fragment));
}

void test_ppogatt__receive_ack_for_all_packets_in_flight(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  uint8_t sn = 0;
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Get s_tx_window_size packets in flight:
  for (sn = 0; sn < s_tx_window_size; ++sn) {
    cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                             s_short_data_fragment,
                                                             sizeof(s_short_data_fragment)), true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment));
  }

  // Ack the last one (sn == s_tx_window_size - 1), which will be interpreted as Ack'ing all
  // the packets before it too:
  prv_receive_ack(s_characteristics[0][PPoGATTCharacteristicData],
                  s_tx_window_size - 1 /* sn */);

  // We should now be able to submit s_tx_window_size packets again:
  for (sn = s_tx_window_size; sn < 2 * s_tx_window_size; ++sn) {
    cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                             s_short_data_fragment,
                                                             sizeof(s_short_data_fragment)), true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment));
  }
}

void test_ppogatt__handle_client_disappearing_for_send_callback(void) {
  // ppogatt_send_next() is called from the KernelBG task sometimes.
  // It's possible that the pointer is dangling by the time the callback executes.
  // Therefore ppogatt_send_next() should be able to handle this dangling pointer gracefully.
  uint8_t fake_client = 0;
  ppogatt_send_next((struct Transport *) &fake_client);
  // No crashes, no writes, etc.
  fake_gatt_client_op_assert_no_write();
}

void test_ppogatt__handle_bluetooth_stack_queue_full_and_empty_events(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  fake_gatt_client_op_set_write_return_value(BTErrnoNotEnoughResources);

  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);
  cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                             s_short_data_fragment,
                                                             sizeof(s_short_data_fragment)), true);
  ppogatt_send_next(transport);
  fake_gatt_client_op_assert_no_write();

  fake_gatt_client_op_set_write_return_value(BTErrnoOK);
  ppogatt_handle_buffer_empty();
  prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], 0 /* sn */,
                       s_short_data_fragment, sizeof(s_short_data_fragment));
}

void test_ppogatt__retransmit_timed_out_data_packets_all_at_once(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  uint8_t sn = 0;
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Get s_tx_window_size packets in flight:
  for (sn = 0; sn < s_tx_window_size; ++sn) {
    cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                         s_short_data_fragment,
                                                         sizeof(s_short_data_fragment) - sn), true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
  }

  // Simulate the regular timer firing a bunch of times to expire the timeout for all the packets:
  for (int i = 0; i < PPOGATT_TIMEOUT_TICKS; ++i) {
    regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
  }

  fake_comm_session_process_send_next();

  // The data should *NOT* get concatenated in a single packet, even though it might fit. The
  // fragmentation should be the same as the previous transmission pass, because there is a race
  // condition where there are Ack(s) in flight for the "original" data packets. Because we're
  // using the same SNs, we cannot change the fragmentation, because we cannot know whether they
  // would refer to the old or new fragmentation.

  for (sn = 0; sn < s_tx_window_size; ++sn) {
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn /* sn */,
                         s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
  }
}

void test_ppogatt__retransmit_timed_out_data_packets_first_but_not_later_ones(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  uint8_t sn = 0;
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Get s_tx_window_size packets in flight:
  uint8_t secs_passed = 0;
  for (sn = 0; sn < s_tx_window_size; ++sn) {
    cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                         s_short_data_fragment,
                                                         sizeof(s_short_data_fragment) - sn), true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
    if (sn == 0 || sn == 1) {
      // Make the first and second packet time out each, one second earlier
      // than the 3rd and 4rd packets:
      regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
      ++secs_passed;
    }
    cl_assert(secs_passed < PPOGATT_TIMEOUT_TICKS);
  }

  // Simulate the regular timer firing a bunch of times to expire the timeout for the in-flight packets
  // This will trigger a retransmit of the un-acked packets
  for (int i = 0; i < PPOGATT_TIMEOUT_TICKS - 1; ++i) {
    regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
  }

  fake_comm_session_process_send_next();

  for (sn = 0; sn < s_tx_window_size; ++sn) {
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn /* sn */,
                         s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
  }
}

void test_ppogatt__retransmit_timed_out_data_packets_race_everything_acked_at_once(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Get s_tx_window_size packets in flight:
  uint8_t sn = 0;
  for (; sn < s_tx_window_size; ++sn) {
    cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                         s_short_data_fragment,
                                                         sizeof(s_short_data_fragment) - sn), true);
    ppogatt_send_next(transport);
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
  }

  // Time-out all packets in flight, rolling back for retransmission:
  for (int i = 0; i < PPOGATT_TIMEOUT_TICKS; ++i) {
    regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
  }

  // Simulate receiving an ack for the last, after the roll-back, but before the packets are
  // retransmitted (the last part shouldn't matter much, but simplifies the test a bit)
  prv_receive_ack(s_characteristics[0][PPoGATTCharacteristicData],
                  (sn - 1) % PPOGATT_SN_MOD_DIV);

  // Some new data has been queued up in the mean time:
  cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                         s_short_data_fragment,
                                                         sizeof(s_short_data_fragment) - sn), true);
  // Only now the system task callback is fired (prv_send_next_packets_async):
  fake_comm_session_process_send_next();

  // Expect the new data to come through, no retransmissions at all.
  // (They all got considered Ack'd by the one Ack)
  prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn /* sn */,
                       s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
}

void test_ppogatt__retransmit_max_number_of_times(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Get a packet in flight:
  uint8_t sn = 0;
  cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                         s_short_data_fragment,
                                                         sizeof(s_short_data_fragment) - sn), true);
  ppogatt_send_next(transport);
  prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                       s_short_data_fragment, sizeof(s_short_data_fragment) - sn);

  for (int j = 0; j < PPOGATT_TIMEOUT_COUNT_MAX - 1; ++j) {
    // Time-out the packet over and over until (max - 1) is reached:
    for (int i = 0; i < PPOGATT_TIMEOUT_TICKS; ++i) {
      regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
    }
    fake_comm_session_process_send_next();
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
  }

  // The last straw:
  for (int i = 0; i < PPOGATT_TIMEOUT_TICKS; ++i) {
    regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
  }
  prv_assert_sent_reset_request(s_characteristics[0][PPoGATTCharacteristicData]);
}

void test_ppogatt__make_sure_timeout_reset_after_data_ack(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  uint8_t num_packets = s_tx_window_size;

  // Get a packet in flight:
  for (int sn = 0; sn < num_packets; sn++) {
    cl_assert_equal_b(
        fake_comm_session_send_buffer_write_raw_by_transport(
        transport, s_short_data_fragment, sizeof(s_short_data_fragment) - sn), true);
    ppogatt_send_next(transport);
  }

  for (int sn = 0; sn < num_packets; sn++) {
    for (int i = 0; i < (PPOGATT_TIMEOUT_TICKS - 1); ++i) {
      regular_timer_fire_seconds(PPOGATT_TIMEOUT_TICK_INTERVAL_SECS);
    }

    prv_receive_ack(s_characteristics[0][PPoGATTCharacteristicData], sn /* sn */);
  }

  fake_comm_session_process_send_next();

  for (int sn = 0; sn < num_packets; sn++) {
    prv_assert_sent_data(s_characteristics[0][PPoGATTCharacteristicData], sn,
                         s_short_data_fragment, sizeof(s_short_data_fragment) - sn);
  }

  // There should be no writes we haven't already checked for. That would only happen if we timed
  // out!
  fake_gatt_client_op_assert_no_write();
}

void test_ppogatt__mtu_zero_due_to_disconnection(void) {
  test_ppogatt__open_session_when_found_pebble_app();
  Transport *transport = ppogatt_client_for_uuid(&s_meta_v0_system.app_uuid);

  // Get a packet in flight:
  uint8_t sn = 0;
  cl_assert_equal_b(fake_comm_session_send_buffer_write_raw_by_transport(transport,
                                                         s_short_data_fragment,
                                                         sizeof(s_short_data_fragment) - sn), true);
  fake_malloc_set_largest_free_block(1000);
  s_mtu_size = 0;
  ppogatt_send_next(transport);
  // No crash
}

//! When client ID info got added to the Reset Packet (PBL-14099), a potential buffer overrun
//! situation got introduced accidentally. This test is a white-box test to catch this issue.
//! For the Reset Packet, a buffer needs to be allocated. The size of this buffer is based upon
//! the MTU of the connection. It's possible the lookup fails and returns 0. In this case, the
//! packet shouldn't be attempted to be written at all, because it will not fit and overrun the
//! buffer.
void test_ppogatt__mtu_zero_due_to_service_rediscovery_while_resetting(void) {
  ppogatt_handle_service_discovered(s_characteristics[0]);

  ppogatt_handle_read_or_notification(s_characteristics[0][PPoGATTCharacteristicMeta],
                                      (const uint8_t *) &s_meta_v0_system,
                                      sizeof(s_meta_v0_system),
                                      BLEGATTErrorSuccess);
  // Expect subscribe request was made:
  fake_gatt_client_subscriptions_assert_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                                                  BLESubscriptionNotifications,
                                                  GAPLEClientKernel);

  // During service re-discovery the cached characteristic handles will be stale for a brief period.
  // This will cause the gatt_client_characteristic_get_device to return BT_DEVICE_INTERNAL_INVALID
  // and eventually gap_le_connection_get_gatt_mtu call to return 0. See PBL-22038.
  s_mtu_size = 0;

  // Simulate getting the subscription confirmation, this will normally trigger PPoGATT to try to
  // write out the Reset packet, but because the MTU is couldn't be looked up, no packet should get
  // sent out:
  ppogatt_handle_subscribe(s_characteristics[0][PPoGATTCharacteristicData],
                           BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Expect nothing to be sent out by FW:
  fake_gatt_client_op_assert_no_write();

  // No crash nor DUMA failures
}

void test_ppogatt__unsubscribe_when_no_memory_for_comm_session(void) {
  // TODO
}
