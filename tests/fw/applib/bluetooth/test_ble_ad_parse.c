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

#include "applib/bluetooth/ble_ad_parse.h"

#include "system/hexdump.h"

#include "clar.h"

#include <btutil/bt_uuid.h>

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_ble_syscalls.h"
#include "stubs_ble_syscalls.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_print.h"
#include "stubs_prompt.h"
#include "stubs_rand_ptr.h"
#include "stubs_serial.h"

// The test data and descriptions in this file are captured using the FrontLine
// Bluetooth sniffer.

static const size_t s_buffer_size = sizeof(BLEAdData) +
                                    (2 * GAP_LE_AD_REPORT_DATA_MAX_LENGTH);
static uint8_t s_buffer[s_buffer_size];
static BLEAdData * const s_ad_data = (BLEAdData *)s_buffer;

static void set_ad_data(uint8_t *data, size_t length) {
  memcpy(s_ad_data->data, data, length);
  s_ad_data->ad_data_length = length;
}

void test_ble_ad_parse__initialize(void) {
  memset(s_ad_data, 0, sizeof(s_buffer_size));
}

// -----------------------------------------------------------------------------
// Consuming BLEAdData:
// -----------------------------------------------------------------------------

void test_ble_ad_parse__16_bit_uuid_and_device_name(void) {
  // AD Element, Length: 2, AD Type: Flags (0x1a)
  // AD Element, Length: 3, AD Type: Complete list of 16-bit UUID, [0x7b29]
  // AD Element, Length: 10, AD Type: Complete local name, Text: LightBlue
  uint8_t data[] =
    "\x02\x01\x1a\x03\x03\x29\x7b\x0a\x09\x4c\x69\x67\x68\x74\x42\x6c\x75\x65";
  set_ad_data(data, sizeof(data));

  // Test ble_ad_get_raw_data_size:
  cl_assert_equal_i(ble_ad_get_raw_data_size(s_ad_data), sizeof(data));

  // Test ble_ad_copy_raw_data:
  uint8_t buffer[GAP_LE_AD_REPORT_DATA_MAX_LENGTH * 2];
  size_t size = ble_ad_copy_raw_data(s_ad_data, buffer, sizeof(buffer));
  cl_assert_equal_i(size, sizeof(data));
  cl_assert_equal_i(memcmp(buffer, data, sizeof(data)), 0);

  // Test ble_ad_copy_local_name, destination buffer large enough:
  char local_name[64];
  size = ble_ad_copy_local_name(s_ad_data, local_name, 64);
  cl_assert_equal_s(local_name, "LightBlue");
  cl_assert_equal_i(size, strlen("LightBlue") + 1);

  // Test ble_ad_copy_local_name, destination buffer too small:
  size = ble_ad_copy_local_name(s_ad_data, local_name, 6);
  cl_assert_equal_s(local_name, "Light");
  cl_assert_equal_i(size, strlen("Light") + 1);

  // Test ble_ad_includes_service:
  Uuid included_uuid = bt_uuid_expand_16bit(0x7b29);
  cl_assert(ble_ad_includes_service(s_ad_data, &included_uuid));
  Uuid missing_uuid = bt_uuid_expand_16bit(0xabcd);
  cl_assert(!ble_ad_includes_service(s_ad_data, &missing_uuid));

  // Test ble_ad_copy_service_uuids, destination array sized large enough:
  const uint8_t count = 4;
  Uuid copied_uuids[count];
  uint8_t found = ble_ad_copy_service_uuids(s_ad_data, copied_uuids, count);
  cl_assert_equal_i(found, 1);

  // Test ble_ad_copy_service_uuids, destination array too small:
  found = ble_ad_copy_service_uuids(s_ad_data, copied_uuids, 0);
  cl_assert_equal_i(found, 1);

  // Test ble_ad_get_tx_power_level returns false, when no TX Power Level:
  int8_t tx_power_level_out;
  cl_assert(!ble_ad_get_tx_power_level(s_ad_data, &tx_power_level_out));
}

void test_ble_ad_parse__128_bit_uuid(void) {
  // AD Element, Length: 2, AD Type: Flags
  // AD Element, Length: 17, AD Type: More 128-bit UUIDs available,
  // Value: 0x68753a444d6f12269c600050e4c00067

  uint8_t data[GAP_LE_AD_REPORT_DATA_MAX_LENGTH] =
    "\x02\x01\x1a\x11\x06\x67\x00\xc0\xe4\x50\x00\x60\x9c\x26\x12\x6f\x4d\x44" \
    "\x3a\x75\x68";
  set_ad_data(data, sizeof(data));

  // Test ble_ad_includes_service:
  uint8_t uuid_bytes[] = "\x68\x75\x3a\x44\x4d\x6f\x12\x26\x9c\x60\x00\x50" \
  "\xe4\xc0\x00\x67";
  Uuid included_uuid = UuidMakeFromBEBytes(uuid_bytes);
  cl_assert(ble_ad_includes_service(s_ad_data, &included_uuid));
  Uuid missing_uuid = bt_uuid_expand_16bit(0xabcd);
  cl_assert(!ble_ad_includes_service(s_ad_data, &missing_uuid));
}

// -----------------------------------------------------------------------------
// Creating BLEAdData:
// -----------------------------------------------------------------------------

void test_ble_ad_parse__ad_and_scan_resp_boundaries(void) {
}

void test_ble_ad_parse__start_scan_response(void) {
  BLEAdData *ad = ble_ad_create();
  ble_ad_start_scan_response(ad);

  uint8_t expected_scan_resp_data[] = {
    1 /* +1 for Type byte */ + strlen("Pebble 1234"),
    0x09, // Local Name, Complete
    'P', 'e', 'b', 'b', 'l', 'e', ' ', '1', '2', '3', '4'
  };

  // Should fit fine, expect true:
  cl_assert_equal_b(ble_ad_set_local_name(ad, "Pebble 1234"), true);
  // Expect no advertisement data:
  cl_assert_equal_i(ad->ad_data_length, 0);
  // Expect scan response data:
  cl_assert_equal_i(ad->scan_resp_data_length, sizeof(expected_scan_resp_data));
  // Compare scan response data:
  cl_assert_equal_i(memcmp(expected_scan_resp_data,
                           ad->data + ad->ad_data_length,
                           ad->scan_resp_data_length), 0);

  ble_ad_destroy(ad);
}

void test_ble_ad_parse__set_service_uuids_128_bit(void) {
  BLEAdData *ad = ble_ad_create();

  uint8_t uuid_bytes[] = "\x97\x6e\xbb\x18\xd3\xe9\x43\xc0\x8a\x63\x8d\x2b" \
  "\x60\xd9\x04\x2a";
  Uuid uuid[2];
  uuid[0] = UuidMakeFromBEBytes(uuid_bytes);
  uuid[1] = UuidMakeFromLEBytes(uuid_bytes); // just reversed, laziness

  // 2x 128-bit UUIDs is not going to fit, expect false:
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 2), false);

  // Hand-construct expected raw advertisement data:
  uint8_t expected_ad_data[sizeof(Uuid) + 2 /* +1 Length, +1 Type bytes */] = {
    sizeof(Uuid) + 1 /* +1 for Type byte */,
    0x07, // Service UUIDs, 128-bit, Complete
  };
  memcpy(&expected_ad_data[2], uuid_bytes, sizeof(Uuid));

  // One should fit though:
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 1), true);

  cl_assert_equal_i(memcmp(expected_ad_data, ad->data,
                           sizeof(expected_ad_data)), 0);
  cl_assert_equal_i(ad->ad_data_length, sizeof(expected_ad_data));
  cl_assert_equal_i(ad->scan_resp_data_length, 0);

  ble_ad_destroy(ad);
}

void test_ble_ad_parse__set_service_uuids_32_bit(void) {
  BLEAdData *ad;

  Uuid uuid[8];
  for (int i = 0; i < 8; ++i) {
    uuid[i] = bt_uuid_expand_32bit(0x12346700 + i);
  }

  // Hand-construct expected raw advertisement data:
  uint8_t expected_ad_data[] = {
    (2 * sizeof(uint32_t)) + 1 /* +1 for Type byte */,
    0x05, // Service UUIDs, 32-bit, Complete
    0x00, 0x67, 0x34, 0x12, // Little endian
    0x01, 0x67, 0x34, 0x12,
  };

  // 2x 32-bit UUIDs should fit fine, expect true:
  ad = ble_ad_create();
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 2), true);
  cl_assert_equal_i(memcmp(expected_ad_data, ad->data,
                           sizeof(expected_ad_data)), 0);
  cl_assert_equal_i(ad->ad_data_length, sizeof(expected_ad_data));
  cl_assert_equal_i(ad->scan_resp_data_length, 0);
  ble_ad_destroy(ad);

  // 7x 32-bit UUIDs should fit, expect true:
  ad = ble_ad_create();
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 7), true);
  ble_ad_destroy(ad);

  // 8x 32-bit UUIDs does not fit, expect false:
  ad = ble_ad_create();
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 8), false);
  ble_ad_destroy(ad);
}

void test_ble_ad_parse__set_service_uuids_16_bit(void) {
  BLEAdData *ad;

  Uuid uuid[15];
  for (int i = 0; i < 15; ++i) {
    uuid[i] = bt_uuid_expand_16bit(0x1800 + i);
  }

  // Hand-construct expected raw advertisement data:
  uint8_t expected_ad_data[] = {
    (2 * sizeof(uint16_t)) + 1 /* +1 for Type byte */,
    0x03, // Service UUIDs, 16-bit, Complete
    0x00, 0x18, // Little endian
    0x01, 0x18,
  };

  // 2x 16-bit UUIDs should fit fine, expect true:
  ad = ble_ad_create();
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 2), true);
  cl_assert_equal_i(memcmp(expected_ad_data, ad->data,
                           sizeof(expected_ad_data)), 0);
  cl_assert_equal_i(ad->ad_data_length, sizeof(expected_ad_data));
  cl_assert_equal_i(ad->scan_resp_data_length, 0);
  ble_ad_destroy(ad);

  // 14x 16-bit UUIDs should fit, expect true:
  ad = ble_ad_create();
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 14), true);
  ble_ad_destroy(ad);

  // 15x 16-bit UUIDs does not fit, expect false:
  ad = ble_ad_create();
  cl_assert_equal_b(ble_ad_set_service_uuids(ad, uuid, 15), false);
  ble_ad_destroy(ad);
}

void test_ble_ad_parse__set_local_name(void) {
  BLEAdData *ad;
  ad = ble_ad_create();

  uint8_t expected_ad_data[] = {
    1 /* +1 for Type byte */ + strlen("Pebble 1234"),
    0x09, // Local Name, Complete
    'P', 'e', 'b', 'b', 'l', 'e', ' ', '1', '2', '3', '4'
  };

  // Should fit fine, expect true:
  cl_assert_equal_b(ble_ad_set_local_name(ad, "Pebble 1234"), true);
  cl_assert_equal_i(memcmp(expected_ad_data, ad->data, ad->ad_data_length), 0);

  ble_ad_destroy(ad);
}

void test_ble_ad_parse__set_tx_power_level(void) {
  BLEAdData *ad;
  ad = ble_ad_create();

  uint8_t expected_ad_data[] = {
    1 /* +1 for Type byte */ + 1 /* int8_t with value */,
    0x0a, // TX Power Level
    -55,
  };

  // Should fit fine, expect true:
  cl_assert_equal_b(ble_ad_set_tx_power_level(ad), true);
  cl_assert_equal_i(memcmp(expected_ad_data, ad->data, ad->ad_data_length), 0);

  ble_ad_destroy(ad);
}

void test_ble_ad_parse__set_manufacturer_specific_data(void) {
  BLEAdData *ad;
  ad = ble_ad_create();

  uint8_t expected_ad_data[] = {
    1 /* +1 for Type byte */ + 13 /* Company ID + data */,
    0xff, // Manufacturer Specific data
    0x34, 0x12,
    'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd',
  };

  // Should fit fine, expect true:
  cl_assert_equal_b(ble_ad_set_manufacturer_specific_data(ad,
                                                      0x1234,
                                                      (uint8_t *) "hello world",
                                                      11), true);
  cl_assert_equal_i(memcmp(expected_ad_data, ad->data, ad->ad_data_length), 0);

  ble_ad_destroy(ad);
}

void test_ble_ad_parse__set_flags(void) {
  BLEAdData *ad;
  ad = ble_ad_create();

  const uint8_t flags = 0x03;

  const uint8_t expected_ad_data[] = {
    1 /* +1 for Type byte */ + 1 /* uint8_t with value */,
    0x01, // Flags type
    flags,
  };

  // Should fit fine, expect true:
  cl_assert_equal_b(ble_ad_set_flags(ad, flags), true);
  cl_assert_equal_i(memcmp(expected_ad_data, ad->data, ad->ad_data_length), 0);

  ble_ad_destroy(ad);
}
