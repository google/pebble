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

#include "services/common/shared_prf_storage/shared_prf_storage.h"
#include "services/common/shared_prf_storage/v3_sprf/shared_prf_storage_private.h"
#include "flash_region/flash_region.h"
#include "drivers/flash.h"
#include "util/size.h"

#include <bluetooth/sm_types.h>
#include <btutil/sm_util.h>
#include <os/mutex.h>

#include <string.h>

#include "clar.h"

// Fakes
//////////////////////////////////////////////////////////
#include "fake_spi_flash.h"
#include "stubs_pbl_malloc.h"
#include "stubs_passert.h"
#include "stubs_logging.h"

// Externs
//////////////////////////////////////////////////////////
extern uint32_t shared_prf_storage_get_valid_page_number(void);
extern void shared_prf_storage_set_valid_page_number(uint32_t page_num);

// Defines
//////////////////////////////////////////////////////////
#define SPRF_REGION_SIZE (FLASH_REGION_SHARED_PRF_STORAGE_END - \
                          FLASH_REGION_SHARED_PRF_STORAGE_BEGIN)
#define SPRF_NUM_PAGES (SPRF_REGION_SIZE / sizeof(SharedPRFData))

#define SPRF_PAGE_FLASH_OFFSET(idx) (FLASH_REGION_SHARED_PRF_STORAGE_BEGIN + \
                                     (idx * sizeof(SharedPRFData)))

// Stubs
//////////////////////////////////////////////////////////
static bool s_mutex_locked;
PebbleMutex * mutex_create(void) {
  return NULL;
}

void mutex_lock(PebbleMutex * handle) {
  cl_assert_equal_b(s_mutex_locked, false);
  s_mutex_locked = true;
}

void mutex_unlock(PebbleMutex * handle) {
  cl_assert_equal_b(s_mutex_locked, true);
  s_mutex_locked = false;
}

static const char DEVICE_NAME[BT_DEVICE_NAME_BUFFER_SIZE] = "ABCDEFGHIJKLMNOPQRS";
static const char *PAIRING_NAME = "Blah123";
static const BTDeviceAddress DEVICE_ADDR = {.octets = {0x88, 0x99, 0xaa, 0xbb, 0x00, 0x11}};

static const SMPairingInfo PAIRING_INFO = (const SMPairingInfo) {
  .local_encryption_info = {
    .ediv = 123,
    .ltk = (const SMLongTermKey) {
      .data = {
        0x44, 0x55, 0x66, 0x77, 0x00, 0x11, 0x22, 0x33,
        0xcc, 0xdd, 0xee, 0xff, 0x88, 0x99, 0xaa, 0xbb,
      },
    },
    .rand = 0x11223344,
  },

  .remote_encryption_info = {
    .ltk = (const SMLongTermKey) {
      .data = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
      },
    },
    .rand = 0x11223344,
    .ediv = 9876,
  },

  .irk = (const SMIdentityResolvingKey) {
    .data = {
      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    },
  },
  .identity = (const BTDeviceInternal) {
    .opaque.opaque_64 = 0x1122334455667788,
  },

  .csrk = {
    .data = {
      0xcc, 0xdd, 0xee, 0xff, 0x88, 0x99, 0xaa, 0xbb,
      0x44, 0x55, 0x66, 0x77, 0x00, 0x11, 0x22, 0x33,
    },
  },

  .is_local_encryption_info_valid = true,
  .is_remote_encryption_info_valid = true,
  .is_remote_identity_info_valid = true,
  .is_remote_signing_info_valid = true,
  .is_mitm_protection_enabled = true,
};

// Helpers
///////////////////////////////////////////////////////////
static void prv_fill_flash_random_data(void) {
  uint8_t *buf = kernel_malloc_check(SPRF_REGION_SIZE);
  fake_spi_flash_erase();
  memset(buf, 0x17, SPRF_REGION_SIZE);
  flash_write_bytes(buf, FLASH_REGION_SHARED_PRF_STORAGE_BEGIN, SPRF_REGION_SIZE);
  kernel_free(buf);
}

static void prv_assert_mutexes_unlocked(void) {
  cl_assert_equal_b(s_mutex_locked, false);
}

// Tests
///////////////////////////////////////////////////////////
void test_shared_prf_storage_v3__initialize(void) {
  fake_spi_flash_init(FLASH_REGION_SHARED_PRF_STORAGE_BEGIN,
                      SPRF_REGION_SIZE);
  shared_prf_storage_init();
}

void test_shared_prf_storage_v3__cleanup(void) {
  fake_spi_flash_cleanup();
  prv_assert_mutexes_unlocked();
}

void test_shared_prf_storage_v3__init_all_zeros(void) {
  uint8_t *flash_buf = kernel_zalloc(SPRF_REGION_SIZE);
  flash_write_bytes(flash_buf, FLASH_REGION_SHARED_PRF_STORAGE_BEGIN, SPRF_REGION_SIZE);
  shared_prf_storage_init();
  shared_prf_storage_set_getting_started_complete(true);
  cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), true);
}

void test_shared_prf_storage_v3__find_first_valid_sector(void) {
  SharedPRFData data;

  // These are the pages that this test will place a valid header. It will write invalid pages
  // before the valid one.
  uint8_t page_idx[] = {0, 1, SPRF_NUM_PAGES / 2, SPRF_NUM_PAGES - 1};

  for (uint32_t i = 0; i < ARRAY_LENGTH(page_idx); i++) {
    fake_spi_flash_erase();

    // Invalidate all entries before it to simulate logging style
    for (uint32_t j = 0; j < page_idx[i]; j++) {
      SprfMagic inv_magic = SprfMagic_InvalidatedEntry;
      flash_write_bytes((uint8_t *) &inv_magic, SPRF_PAGE_FLASH_OFFSET(j), sizeof(inv_magic));
    }

    // Write the valid page
    flash_read_bytes((uint8_t *) &data, SPRF_PAGE_FLASH_OFFSET(page_idx[i]), sizeof(data));
    data.magic = SprfMagic_ValidEntry;
    flash_write_bytes((uint8_t *) &data, SPRF_PAGE_FLASH_OFFSET(page_idx[i]), sizeof(data));

    // Call init and see if it found the valid page
    shared_prf_storage_init();
    uint32_t desired_page_idx = page_idx[i];
    if (page_idx[i] > SPRF_MAX_NUM_PAGES_MULT(SPRF_NUM_PAGES)) {
      desired_page_idx = 0;
    }
    cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), desired_page_idx);
  }

  // Erase the entire region and test that it picks the first empty page
  fake_spi_flash_erase();
  shared_prf_storage_init();
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);

  // Test that it sees all pages are invalid, rewrites everything, and picks the first empty page
  prv_fill_flash_random_data();
  shared_prf_storage_init();
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);
}

void test_shared_prf_storage_v3__wipe_all(void) {
  SMPairingInfo sm_pairing_info;
  memset(&sm_pairing_info, 0xaa, sizeof(sm_pairing_info));
  sm_pairing_info.is_local_encryption_info_valid = true;
  sm_pairing_info.is_remote_signing_info_valid = true;
  sm_pairing_info.is_remote_identity_info_valid = true;
  shared_prf_storage_store_ble_pairing_data(&sm_pairing_info, PAIRING_NAME,
                                            false /* requires_address_pinning */, 0 /* flags */);
  cl_assert_equal_b(shared_prf_storage_get_ble_pairing_data(NULL, NULL, NULL, NULL), true);
  shared_prf_storage_wipe_all();

  cl_assert_equal_b(shared_prf_storage_get_ble_pairing_data(NULL, NULL, NULL, NULL), false);
}

void test_shared_prf_storage_v3__getting_started_complete(void) {
  shared_prf_storage_wipe_all();
  cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), false);
  shared_prf_storage_set_getting_started_complete(true);
  cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), true);
  shared_prf_storage_wipe_all();
  cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), false);
}

void test_shared_prf_storage_v3__ble_pairing(void) {
  shared_prf_storage_wipe_all();
  cl_assert_equal_b(shared_prf_storage_get_ble_pairing_data(NULL, NULL, NULL, NULL), false);

  shared_prf_storage_store_ble_pairing_data(&PAIRING_INFO, DEVICE_NAME,
                                            false /* requires_address_pinning */, 0 /* flags */);

  char name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  memset(name_out, 0, sizeof(name_out));
  SMPairingInfo pairing_info_out = {};
  bool requires_address_pinning_out = true;
  uint8_t flags = 0;
  cl_assert_equal_b(shared_prf_storage_get_ble_pairing_data(&pairing_info_out, name_out,
                                                            &requires_address_pinning_out,
                                                            &flags), true);
  cl_assert_equal_b(requires_address_pinning_out, false);
  cl_assert_equal_i(flags, 0);
  cl_assert_equal_i(strcmp(DEVICE_NAME, name_out), 0);
  cl_assert_equal_b(PAIRING_INFO.is_mitm_protection_enabled,
                    pairing_info_out.is_mitm_protection_enabled);
  cl_assert_equal_b(PAIRING_INFO.is_remote_signing_info_valid,
                    pairing_info_out.is_remote_signing_info_valid);
  cl_assert_equal_b(PAIRING_INFO.is_remote_identity_info_valid,
                    pairing_info_out.is_remote_identity_info_valid);
  cl_assert_equal_b(PAIRING_INFO.is_remote_encryption_info_valid,
                    pairing_info_out.is_remote_encryption_info_valid);
  cl_assert_equal_b(PAIRING_INFO.is_local_encryption_info_valid,
                    pairing_info_out.is_local_encryption_info_valid);
  cl_assert_equal_i(PAIRING_INFO.local_encryption_info.ediv,
                    pairing_info_out.local_encryption_info.ediv);
  cl_assert_equal_i(PAIRING_INFO.local_encryption_info.div,
                    pairing_info_out.local_encryption_info.div);
  cl_assert_equal_i(PAIRING_INFO.identity.opaque.opaque_64,
                    pairing_info_out.identity.opaque.opaque_64);
  cl_assert_equal_i(PAIRING_INFO.remote_encryption_info.rand,
                    pairing_info_out.remote_encryption_info.rand);
  cl_assert_equal_i(PAIRING_INFO.remote_encryption_info.ediv,
                    pairing_info_out.remote_encryption_info.ediv);
  cl_assert_equal_i(memcmp(&PAIRING_INFO.remote_encryption_info.ltk,
                           &pairing_info_out.remote_encryption_info.ltk, sizeof(SMLongTermKey)), 0);
  cl_assert_equal_i(memcmp(&PAIRING_INFO.irk, &pairing_info_out.irk,
                           sizeof(SMIdentityResolvingKey)), 0);
  cl_assert_equal_i(memcmp(&PAIRING_INFO.csrk, &pairing_info_out.csrk,
                           sizeof(SM128BitKey)), 0);

  shared_prf_storage_erase_ble_pairing_data();
  cl_assert_equal_b(shared_prf_storage_get_ble_pairing_data(NULL, NULL, NULL, NULL), false);
}

void test_shared_prf_storage_v3__root_keys(void) {
  shared_prf_storage_wipe_all();

  cl_assert_equal_b(shared_prf_storage_get_root_key(SMRootKeyTypeIdentity, NULL), false);
  cl_assert_equal_b(shared_prf_storage_get_root_key(SMRootKeyTypeEncryption, NULL), false);

  SM128BitKey keys[2];
  for (int i = 0; i < sizeof(keys); ++i) {
    ((uint8_t *) keys)[i] = i;
  }

  shared_prf_storage_set_root_keys(keys);

  SM128BitKey keys_out[2];
  for (SMRootKeyType key_type = 0; key_type < SMRootKeyTypeNum; ++key_type) {
    cl_assert_equal_b(shared_prf_storage_get_root_key(key_type, &keys_out[key_type]), true);
    // It's a byte array inside, so memcmp should be OK to use:
    cl_assert_equal_i(memcmp(&keys[key_type], &keys_out[key_type], sizeof(keys[0])), 0);
  }
}

void test_shared_prf_storage_v3__local_device_name(void) {
  cl_assert_equal_b(shared_prf_storage_get_local_device_name(NULL, 0), false);

  shared_prf_storage_set_local_device_name(DEVICE_NAME);

  char device_name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  cl_assert_equal_b(shared_prf_storage_get_local_device_name(device_name_out,
                                                             sizeof(device_name_out)), true);
  cl_assert_equal_s(DEVICE_NAME, device_name_out);
}

// Test that setting a local_name to NULL will rewrite the field with 0xFF and allow it to
// be rewritten without causing a new page to be written
void test_shared_prf_storage_v3__local_device_name_NULL_new_erased_field(void) {
  shared_prf_storage_set_local_device_name(DEVICE_NAME);

  char device_name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  cl_assert_equal_b(shared_prf_storage_get_local_device_name(device_name_out,
                                                             sizeof(device_name_out)), true);
  cl_assert_equal_s(DEVICE_NAME, device_name_out);

  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);
  shared_prf_storage_set_local_device_name(NULL);
  cl_assert_equal_b(shared_prf_storage_get_local_device_name(device_name_out,
                                                             sizeof(device_name_out)), false);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 1);
  shared_prf_storage_set_local_device_name(DEVICE_NAME);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 1);
}

// Test that setting and retrieving a pinned address works.
void test_shared_prf_storage_v3__pinned_address(void) {
  shared_prf_storage_set_ble_pinned_address(&DEVICE_ADDR);
  BTDeviceAddress addr_buf;
  bool rv = shared_prf_storage_get_ble_pinned_address(&addr_buf);
  cl_assert_equal_b(rv, true);
  cl_assert_equal_m(&DEVICE_ADDR, &addr_buf, sizeof(DEVICE_ADDR));

  shared_prf_storage_set_ble_pinned_address(&DEVICE_ADDR);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);

  shared_prf_storage_set_ble_pinned_address(NULL);
  rv = shared_prf_storage_get_ble_pinned_address(NULL);
  cl_assert_equal_b(rv, false);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 1);
}

void test_shared_prf_storage_v3__rewrite_pages_and_wrap_around(void) {
  bool toggle = false;

  shared_prf_storage_wipe_all();

  // Make sure to wrap around a few times and confirm that works
  for (uint32_t iter = 0; iter < 3; iter++) {
    toggle = false;
    // Iterate through all possible pages and keep writing new data, confirm it's the right data.
    for (uint32_t i = 0; i < SPRF_NUM_PAGES; i++) {
      shared_prf_storage_set_getting_started_complete(toggle);
      cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), toggle);

      cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), i);

      toggle = !toggle;
    }
  }
}

void test_shared_prf_storage_v3__save_all_data_confirm_all_data_correct(void) {
  const bool GETTING_STARTED_COMPLETE = true;

  shared_prf_storage_store_ble_pairing_data(&PAIRING_INFO, DEVICE_NAME,
                                            true /* requires_address_pinning */, 0xff /* flags */);
  shared_prf_storage_set_getting_started_complete(GETTING_STARTED_COMPLETE);
  shared_prf_storage_set_local_device_name(DEVICE_NAME);

  char device_name_out[BT_DEVICE_NAME_BUFFER_SIZE];

  // Check pairing info
  SMPairingInfo pairing_info_out;
  bool requires_address_pinning = false;
  uint8_t flags = 0;
  shared_prf_storage_get_ble_pairing_data(&pairing_info_out, device_name_out,
                                          &requires_address_pinning,
                                          &flags);
  cl_assert_equal_b(requires_address_pinning, true);
  cl_assert_equal_i(flags, 0xff);
  cl_assert_equal_b(sm_is_pairing_info_equal_identity(&PAIRING_INFO, &pairing_info_out), true);
  cl_assert_equal_s(device_name_out, DEVICE_NAME);

  // Check getting started
  cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), GETTING_STARTED_COMPLETE);

  // Check local_name
  cl_assert_equal_b(shared_prf_storage_get_local_device_name(device_name_out,
                                                             sizeof(device_name_out)), true);
  cl_assert_equal_s(DEVICE_NAME, device_name_out);
}

void test_shared_prf_storage_v3__write_in_loop_getting_started_confirm_data_still_intact(void) {
  bool GETTING_STARTED_COMPLETE = true;

  shared_prf_storage_store_ble_pairing_data(&PAIRING_INFO, DEVICE_NAME,
                                            true /* requires_address_pinning */, 0xff /* flags */);
  shared_prf_storage_set_getting_started_complete(GETTING_STARTED_COMPLETE);
  shared_prf_storage_set_local_device_name(DEVICE_NAME);

  for (uint32_t i = 0; i < 50; i++) {
    GETTING_STARTED_COMPLETE = !GETTING_STARTED_COMPLETE;

    cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), i % SPRF_NUM_PAGES);
    shared_prf_storage_set_getting_started_complete(GETTING_STARTED_COMPLETE);
    cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), (i + 1) % SPRF_NUM_PAGES);
  }

  // Check if our old information is still in tact after looping and rewriting a ton of times

  char device_name_out[BT_DEVICE_NAME_BUFFER_SIZE];

  // Check pairing info
  SMPairingInfo pairing_info_out;
  bool requires_address_pinning = false;
  uint8_t flags = 0;

  shared_prf_storage_get_ble_pairing_data(&pairing_info_out, device_name_out,
                                          &requires_address_pinning,
                                          &flags);
  cl_assert_equal_b(requires_address_pinning, true);
  cl_assert_equal_i(flags, 0xff);
  cl_assert_equal_b(sm_is_pairing_info_equal_identity(&PAIRING_INFO, &pairing_info_out), true);
  cl_assert_equal_s(device_name_out, DEVICE_NAME);

  // Check local_name
  cl_assert_equal_b(shared_prf_storage_get_local_device_name(device_name_out,
                                                             sizeof(device_name_out)), true);
  cl_assert_equal_s(DEVICE_NAME, device_name_out);
}

// Sets the getting started field, then corrupts the getting_started crc
void test_shared_prf_storage_v3__handle_corrupt_field_same(void) {
  bool GETTING_STARTED_COMPLETE = true;
  shared_prf_storage_set_getting_started_complete(GETTING_STARTED_COMPLETE);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);

  SharedPRFData data;
  flash_read_bytes((uint8_t *)&data,
                   SPRF_PAGE_FLASH_OFFSET(shared_prf_storage_get_valid_page_number()),
                   sizeof(data));
  cl_assert(data.getting_started.crc != 0xFFFFFFFF);

  uint32_t new_crc = 0;
  flash_write_bytes((uint8_t *)&new_crc,
                    SPRF_PAGE_FLASH_OFFSET(shared_prf_storage_get_valid_page_number())
                                           + offsetof(SharedPRFData, getting_started)
                                           + offsetof(SprfGettingStarted, crc),
                    sizeof(new_crc));

  // Confirm new CRC was written
  flash_read_bytes((uint8_t *)&data,
                   SPRF_PAGE_FLASH_OFFSET(shared_prf_storage_get_valid_page_number()),
                   sizeof(data));
  cl_assert_equal_i(data.getting_started.crc, new_crc);

  // Should be corrupt, so it should return false
  cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), false);
  // Should have moved to the next page
  cl_assert_equal_b(shared_prf_storage_get_valid_page_number(), 1);

  // Let's do it again, but move the valid page to index NUM_PAGES - 1 so we force a wrap around
  fake_spi_flash_erase();
  shared_prf_storage_set_valid_page_number(SPRF_NUM_PAGES - 1);
  shared_prf_storage_set_getting_started_complete(GETTING_STARTED_COMPLETE);
  flash_write_bytes((uint8_t *)&new_crc,
                    SPRF_PAGE_FLASH_OFFSET(shared_prf_storage_get_valid_page_number())
                                           + offsetof(SharedPRFData, getting_started)
                                           + offsetof(SprfGettingStarted, crc),
                    sizeof(new_crc));
  // Should be corrupt, so it should return false
  cl_assert_equal_b(shared_prf_storage_get_getting_started_complete(), false);
  // Should have moved to the next page, which is ZERO since we had to wrap around.
  cl_assert_equal_b(shared_prf_storage_get_valid_page_number(), 0);
}

// Sets the getting started field, then corrupts the ble_pairing_data crc
// This tests that when setting a value, all fields in the struct must be valid.
void test_shared_prf_storage_v3__handle_corrupt_field_during_setting(void) {
  bool GETTING_STARTED_COMPLETE = true;
  shared_prf_storage_set_getting_started_complete(GETTING_STARTED_COMPLETE);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);

  SharedPRFData data;
  flash_read_bytes((uint8_t *)&data,
                   SPRF_PAGE_FLASH_OFFSET(shared_prf_storage_get_valid_page_number()),
                   sizeof(data));
  cl_assert(data.getting_started.crc != 0xFFFFFFFF);

  uint32_t new_crc = 0;
  flash_write_bytes((uint8_t *)&new_crc,
                    SPRF_PAGE_FLASH_OFFSET(shared_prf_storage_get_valid_page_number())
                    + offsetof(SharedPRFData, ble_pairing_data)
                    + offsetof(SprfBlePairingData, crc),
                    sizeof(new_crc));

  // Confirm new CRC was written
  flash_read_bytes((uint8_t *)&data,
                   SPRF_PAGE_FLASH_OFFSET(shared_prf_storage_get_valid_page_number()),
                   sizeof(data));
  cl_assert_equal_i(data.ble_pairing_data.crc, new_crc);

  // Should be corrupt, so after a 'set', the page number should increment even though we are
  // setting the same value
  shared_prf_storage_set_getting_started_complete(true);
  // Should have moved to the next page
  cl_assert_equal_b(shared_prf_storage_get_valid_page_number(), 1);
}

// Test that when we write the ble_data and the ble_name separately, a page rewrite isn't triggered
void test_shared_prf_storage_v3__write_ble_data_and_ble_name_separately(void) {
  shared_prf_storage_store_ble_pairing_data(&PAIRING_INFO, NULL,
                                            true /* requires_address_pinning */,
                                            true /* auto_accept_re_pairing */);
  shared_prf_storage_store_ble_pairing_data(&PAIRING_INFO, DEVICE_NAME,
                                            true /* requires_address_pinning */,
                                            true /* auto_accept_re_pairing */);
  // confirm we wrote to the same "Page"
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);
}

// Test that when deleting the ble_data, both the data and name are deleted.
// Test that when rewriting the ble_data, the same page is used (since they were previously
// marked with 0xFF's
void test_shared_prf_storage_v3__write_ble_data_name_delete_rewrite(void) {
  shared_prf_storage_store_ble_pairing_data(&PAIRING_INFO, DEVICE_NAME,
                                            true /* requires_address_pinning */,
                                            true /* auto_accept_re_pairing */);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);
  shared_prf_storage_erase_ble_pairing_data();
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 2);

  char device_name_out[BT_DEVICE_NAME_BUFFER_SIZE];
  SMPairingInfo pairing_info_out;
  const bool rv = shared_prf_storage_get_ble_pairing_data(&pairing_info_out, device_name_out, NULL,
                                                          NULL);
  cl_assert_equal_b(rv, false);

  shared_prf_storage_store_ble_pairing_data(&PAIRING_INFO, DEVICE_NAME,
                                            true /* requires_address_pinning */,
                                            true /* auto_accept_re_pairing */);
  // It should detect the fields were already blank in the current page so the index should not
  // increment
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 2);
}

// Test that if we try to write the same data, the system does not force a rewrite of the page
void test_shared_prf_storage_v3__write_repeated_data_same_page(void) {
  shared_prf_storage_set_getting_started_complete(false);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);
  shared_prf_storage_set_getting_started_complete(false);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 0);
  shared_prf_storage_set_getting_started_complete(true);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 1);
  shared_prf_storage_set_getting_started_complete(true);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 1);
  shared_prf_storage_set_getting_started_complete(false);
  cl_assert_equal_i(shared_prf_storage_get_valid_page_number(), 2);
}
