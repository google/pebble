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

#include "services/normal/settings/settings_file.h"
#include "services/normal/settings/settings_raw_iter.h"
#include "system/hexdump.h"

#include "clar.h"

#include "services/normal/filesystem/pfs.h"
#include "flash_region/flash_region.h"

#include <stdio.h>
#include <string.h>

// Stubs
////////////////////////////////////
#include "stubs_analytics.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_print.h"
#include "stubs_prompt.h"
#include "stubs_rand_ptr.h"
#include "stubs_serial.h"
#include "stubs_sleep.h"
#include "stubs_system_reset.h"
#include "stubs_task_watchdog.h"
#include "fake_rtc.h"
#include "fake_spi_flash.h"

// Tests
////////////////////////////////////
extern status_t settings_file_compact(SettingsFile *file);

void test_settings_file__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
}

void test_settings_file__cleanup(void) {}

#include <stdio.h>

#define PRIb8 "%d%d%d%d%d%d%d%d"
#define TO_BINARY(byte)  \
  (byte & 0x80 ? 1 : 0), \
  (byte & 0x40 ? 1 : 0), \
  (byte & 0x20 ? 1 : 0), \
  (byte & 0x10 ? 1 : 0), \
  (byte & 0x08 ? 1 : 0), \
  (byte & 0x04 ? 1 : 0), \
  (byte & 0x02 ? 1 : 0), \
  (byte & 0x01 ? 1 : 0)

extern void pfs_debug_dump(int fd, int num_bytes);
void settings_file_hexdump(SettingsFile *file) {
  pfs_seek(file->iter.fd, 0, FSeekSet);
  pfs_debug_dump(file->iter.fd, 4096);
}
void settings_file_dump(SettingsFile *file) {
  for (settings_raw_iter_begin(&file->iter); !settings_raw_iter_end(&file->iter);
       settings_raw_iter_next(&file->iter)) {
    SettingsRecordHeader hdr = file->iter.hdr;
    printf("Record { last_modified: %d, ", hdr.last_modified);
    printf("flags: "PRIb8", ", TO_BINARY(hdr.flags));
    printf("key_hash: %"PRIu8", key_len: %d, val_len: %d }\n", hdr.key_hash, hdr.key_len, hdr.val_len);
  }
}


static void verify(SettingsFile *file, uint8_t *key, int key_len,
                   uint8_t *val, int val_len) {
  int val_len_out = settings_file_get_len(file, key, key_len);
  cl_must_pass(val_len_out);
  cl_assert_equal_i(val_len, val_len_out);

  const bool is_immediate_delete = (val_len == 0 && DELETED_LIFETIME <= 0);
  uint8_t *val_out = malloc(val_len_out);
  cl_assert_equal_i(settings_file_get(file, key, key_len, val_out, val_len_out),
                    is_immediate_delete ? E_DOES_NOT_EXIST : S_SUCCESS);
  if (!is_immediate_delete) {
    cl_assert_equal_m(val, val_out, val_len);
  }
  free(val_out);
}

static void set_and_verify(SettingsFile *file, uint8_t *key, int key_len,
                           uint8_t *val, int val_len) {
  cl_must_pass(settings_file_set(file, key, key_len, val, val_len));
  verify(file, key, key_len, val, val_len);
}

void test_settings_file__set_get_one(void) {
  printf("\nTesting setting and retreiving a single key a single time...\n");
  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_set_get_one", 4096));
  uint8_t key[4];
  int key_len = sizeof(key) - 1;
  memcpy(key, "key", sizeof(key));
  uint8_t val[4];
  int val_len = sizeof(val) - 1;
  memcpy(val, "val", sizeof(val));

  set_and_verify(&file, key, key_len, val, val_len);
}

void test_settings_file__set_get_one_many_times(void) {
  printf("\nTesting setting and retreiving a key several times...\n");
  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_set_get_one_many_times", 4096));
  uint8_t key[4];
  int key_len = sizeof(key) - 1;
  memcpy(key, "key", sizeof(key));
  uint8_t val[7];
  int val_len = 6;
  printf("Setting key 10 times and verifying we get the same value back...\n");
  for (int i = 0; i < 10; i++) {
    snprintf((char *)val, 7, "val%03d", i);
    printf("Iteration %d val %s\n", i, val);

    set_and_verify(&file, key, key_len, val, val_len);
  }

  settings_file_close(&file);
  cl_must_pass(settings_file_open(&file, "test_file_set_get_one_many_times", 4096));

  printf("Making sure we still get the right value after closing & reopening the file...\n");
  verify(&file, key, key_len, val, val_len);
}

static bool prv_each_check_all_values(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  uint32_t desired_value = *(uint32_t *)context;

  uint32_t key;
  uint32_t value;
  info->get_key(file, (uint8_t *)&key, sizeof(uint32_t));
  info->get_val(file, (uint8_t *)&value, sizeof(uint32_t));
  cl_assert(value == desired_value);
  return true;
}

static void prv_test_settings_file_compaction(const bool manual) {
  // If manual is enabled, then the test will force a compaction every so often
  // If manual is disabled, then the test will only compact when it is required to do so.

  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_compaction", 2048));

  uint32_t value = 0;
  static const uint32_t LOOPS = 10;
  static const uint32_t NUMBER_ENTRIES = 50;

  // set all keys to 0
  for (uint32_t i = 0; i < NUMBER_ENTRIES; i++) {
    settings_file_set(&file, (uint8_t *)&i, sizeof(uint32_t), (uint8_t *)&value, sizeof(uint32_t));
  }

  for (uint32_t i = 0; i < LOOPS; i++) {
    // increment value of all NUMBER_ENTRIES entries
    for (uint32_t j = 0; j < NUMBER_ENTRIES; j++) {
      if (manual && (j % 10 == 0)) {
        cl_must_pass(settings_file_compact(&file));
      }

      settings_file_get(&file, (uint8_t *)&j, sizeof(uint32_t),
                        (uint8_t *)&value, sizeof(uint32_t));
      value += 1;
      set_and_verify(&file, (uint8_t *)&j, sizeof(uint32_t), (uint8_t *)&value, sizeof(uint32_t));
    }
  }

  // ensure all values of every entry is equal to LOOPS
  uint32_t desired_value = LOOPS;
  settings_file_each(&file, prv_each_check_all_values, (void *)&desired_value);
}

void test_settings_file__manual_compaction_increment(void) {
  printf("\nTesting manual file compaction...\n");
  const bool manual_compaction = true;
  prv_test_settings_file_compaction(manual_compaction);
}

void test_settings_file__automatic_compaction_increment(void) {
  printf("\nTesting automatic file compaction...\n");
  const bool manual_compaction = false;
  prv_test_settings_file_compaction(manual_compaction);
}

static void prv_print_stats(SettingsFile *file) {
  printf("file max used space = %d\n", file->max_used_space);
  printf("file max space total = %d\n", file->max_space_total);
  printf("file used space = %d\n", file->used_space);
  printf("file dead space = %d\n", file->dead_space);
}

void test_settings_file__compute_stats(void) {
  printf("\nTesting if compute stats is equal to live stats...\n");
  SettingsFile file;
  const uint32_t max_used_space = 4096;
  cl_must_pass(settings_file_open(&file, "test_file_max_storage", max_used_space));
  uint8_t key[5];
  int key_len = 4;
  uint8_t val[5];
  int val_len = 4;
  for (int i = 0; i < 100; i++) {
    prv_print_stats(&file);
    snprintf((char *)key, sizeof(key), "k%03d", i);
    snprintf((char *)val, sizeof(val), "v%03d", i);
    printf("Iteration %d key %s val %s\n", i, key, val);
    set_and_verify(&file, key, key_len, val, val_len);
    // delete the first 50
    if (i < 50) {
      settings_file_delete(&file, key, key_len);
    }
  }
  prv_print_stats(&file);
  settings_file_compact(&file);
  const SettingsFile file_copy = file;
  settings_file_close(&file);
  cl_must_pass(settings_file_open(&file, "test_file_max_storage", max_used_space));
  cl_assert_equal_i(file.max_used_space, file_copy.max_used_space);
  cl_assert_equal_i(file.max_space_total, file_copy.max_space_total);
  cl_assert_equal_i(file.used_space, file_copy.used_space);
  cl_assert_equal_i(file.dead_space, file_copy.dead_space);
}

void test_settings_file__max_storage(void) {
  printf("\nTesting what happens when we hit the storage limits...\n");
  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_max_storage", 4096));
  uint8_t key[5];
  int key_len = 4;
  uint8_t val[5];
  int val_len = 4;
  // 255 * (8 + 4 + 4) = 4080 + 8 + 8 = 4096
  for (int i = 0; i < 255; i++) {
    snprintf((char *)key, sizeof(key), "k%03d", i);
    snprintf((char *)val, sizeof(val), "v%03d", i);
    printf("Iteration %d key %s val %s\n", i, key, val);
    set_and_verify(&file, key, key_len, val, val_len);
  }
  prv_print_stats(&file);
  printf("Making sure we handle running out of storage gracefully...\n");
  memcpy(key, "k255", 5);
  memcpy(val, "v255", 5);
  cl_assert_equal_i(settings_file_set(&file, key, key_len, val, val_len), E_OUT_OF_STORAGE);
  int val_len_out = settings_file_get_len(&file, key, key_len);
  cl_must_pass(val_len_out);
  uint8_t *val_out = malloc(val_len_out);
  cl_assert_equal_i(settings_file_get(&file, key, key_len, val_out, val_len_out), E_DOES_NOT_EXIST);
  free(val_out);
  printf("Making sure we can delete when at max storage...\n");
  memcpy(key, "k000", 5);
  cl_must_pass(settings_file_delete(&file, key, key_len));
}

void test_settings_file__max_storage_with_delete(void) {
  printf("\nTesting what happens when we hit the storage limits with deletes...\n");
  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_max_storage", 4096));
  uint8_t key[5];
  int key_len = 4;
  uint8_t val[5];
  int val_len = 4;
  // 255 * (8 + 4 + 4) = 4080 + 8 + 8 = 4096
  for (int j = 0; j < 255 * 2; j++) {
    prv_print_stats(&file);
    int i = j % 255;
    snprintf((char *)key, sizeof(key), "k%03d", i);
    snprintf((char *)val, sizeof(val), "v%03d", i);
    printf("Iteration %d key %s val %s\n", i, key, val);
    set_and_verify(&file, key, key_len, val, val_len);
    // delete the first iteration
    if (j < 255) {
      settings_file_delete(&file, key, key_len);
    }
  }
  printf("Making sure we handle running out of storage gracefully...\n");
  memcpy(key, "k255", 5);
  memcpy(val, "v255", 5);
  cl_assert_equal_i(settings_file_set(&file, key, key_len, val, val_len), E_OUT_OF_STORAGE);
  int val_len_out = settings_file_get_len(&file, key, key_len);
  cl_must_pass(val_len_out);
  uint8_t *val_out = malloc(val_len_out);
  cl_assert_equal_i(settings_file_get(&file, key, key_len, val_out, val_len_out), E_DOES_NOT_EXIST);
  free(val_out);
}

void test_settings_file__used_space_tracking(void) {
  printf("\nTesting used space tracking...\n");
  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_used_space_tracking", 4096));
  uint8_t key[5];
  int key_len = 4;
  uint8_t val[5];
  int val_len = 4;

  // First, fill half the file up with random keys and values. This reduces
  // our usable space to slightly less than 2048 bytes.
  for (int i = 0; i < 128; i++) {
    snprintf((char *)key, sizeof(key), "k%03d", i);
    snprintf((char *)val, sizeof(val), "v%03d", i);
    printf("Iteration %d val %s\n", i, val);
    set_and_verify(&file, key, key_len, val, val_len);
  }

  // Then, write to the same key many, many, times. This should only use up 16
  // more bytes of the file if our used/free space tracking is working
  // correctly, but may end up being counted as more if it's broken.
  snprintf((char *)key, sizeof(key), "k%03d", 128);
  for (int i = 0; i < 128; i++) {
    snprintf((char *)val, sizeof(val), "v%03d", i);
    printf("Iteration %d val %s\n", i, val);
    set_and_verify(&file, key, key_len, val, val_len);
  }

  // Finally, try to set a key to a large value. This should succeed with
  // plenty of room to spare if our space tracking is functioning correctly.
  uint8_t big_val[SETTINGS_VAL_MAX_LEN] = {};
  set_and_verify(&file, key, key_len, big_val, 511);
}

typedef enum {
  RecordResultOld,
  RecordResultNew,
  RecordResultEnd,
} RecordResult;

static RecordResult write_and_change_record_aborting_after_bytes(int after_n_bytes) {
  fake_spi_flash_init(0, 0x1000000);
  // Wednesday (the 1st) at 00:00
  // date -d "2014/01/01 00:00:00" "+%s" ==> 1388563200
  fake_rtc_init(0, 1388563200);
  pfs_init(false);

  SettingsFile file_original;
  cl_must_pass(settings_file_open(&file_original, "test_file_atomic", 4096));

  uint8_t key[4];
  int key_len = sizeof(key) - 1;
  memcpy(key, "key", sizeof(key));

#define ORIGINAL_VALUE "original_value"
  uint8_t original_value[sizeof(ORIGINAL_VALUE)];
  int original_value_length = sizeof(ORIGINAL_VALUE) - 1;
  memcpy(original_value, ORIGINAL_VALUE, sizeof(ORIGINAL_VALUE));

  set_and_verify(&file_original, key, key_len, original_value, original_value_length);

#define NEW_VALUE "new_value"
  uint8_t new_value[sizeof(NEW_VALUE)];
  int new_value_length = sizeof(NEW_VALUE) - 1;
  memcpy(new_value, NEW_VALUE, sizeof(NEW_VALUE));

  jmp_buf jmp;
  if (setjmp(jmp)) {
    // If we end up here, that means we wrote at least after_n_bytes to the
    // SPI flash. Once we get here, we want to "pretend" that we rebooted,
    // and make sure that the contents of the settings_file are not corrupted
    // in any way.

    // Simulate a reboot by clearing out PFS's state.
    fake_spi_flash_force_future_failure(0, NULL);
    extern void pfs_reset_all_state(void);
    pfs_reset_all_state();
    pfs_init(false);

    // Reopen the file we were in the middle of writing.
    SettingsFile file_new;
    cl_must_pass(settings_file_open(&file_new, "test_file_atomic", 4096));

    settings_file_dump(&file_new);

    int val_len_out = settings_file_get_len(&file_new, key, key_len);
    cl_must_pass(val_len_out);

    uint8_t *val_out = malloc(val_len_out);
    cl_must_pass(settings_file_get(&file_new, key, key_len, val_out, val_len_out));

    if (val_len_out == original_value_length) {
      printf("original! %d\n", original_value_length);
      cl_assert_equal_m(original_value, val_out, val_len_out);
      free(val_out);
      return RecordResultOld;
    } else if (val_len_out == new_value_length) {
      printf("new! %d\n", new_value_length);
      cl_assert_equal_m(new_value, val_out, val_len_out);
      free(val_out);
      return RecordResultNew;
    }
    // Should not get here! This means that neither the old nor the new value
    // could be retreived, and thus the atomicity is broken! Aaaaaaaaaaaaaah!!
    cl_assert(false);
  }
  fake_spi_flash_force_future_failure(after_n_bytes, &jmp);

  // Setting this key will cause us to jump up to the above if statement if
  // we end up writing more than after_n_bytes in order to change the value
  // of the key. If we don't write more than after_n_bytes, we will continue
  // normally.
  set_and_verify(&file_original, key, key_len, new_value, new_value_length);

  printf("(never hit limit)\n");
  return RecordResultEnd;
}

void test_settings_file__atomic(void) {
  printf("\nTesting if we really are atomic...\n");
  bool have_hit_old_value = false;
  bool have_hit_new_value = false;
  bool have_hit_end = false;
  for (int i = 1; i < 50; i++) {
    printf("Iteration %3d... ", i);
    RecordResult result = write_and_change_record_aborting_after_bytes(i);
    switch (result) {
      case RecordResultOld:
        have_hit_old_value = true;
        break;
      case RecordResultNew:
        have_hit_new_value = true;
        break;
      case RecordResultEnd:
        have_hit_end = true;
        break;
    }
  }
  cl_assert(have_hit_old_value);
  cl_assert(have_hit_new_value);
  cl_assert(have_hit_end);
}

void test_settings_file__zero_length(void) {
  printf("\nTesting if we can set keys & values of zero length...\n");

  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_zero_length", 4096));

  uint8_t key[4];
  int key_len = sizeof(key) - 1;
  memcpy(key, "key", sizeof(key));
  uint8_t val[4];
  int val_len = sizeof(val) - 1;
  memcpy(val, "val", sizeof(val));

  set_and_verify(&file, key, key_len, val, val_len);
  set_and_verify(&file, key, key_len, val, 0);
  // This one's a little funny... We could disallow setting zero-length keys
  // because it's unlikely to be something people want, but it currently works
  // just fine, and is actually a fairly well-defined operation (after all,
  // keys & values are just binary data, so the zero-length key is just as
  // unique a key as any other).
  set_and_verify(&file, key, 0, val, val_len);
}

static bool prv_each_cb(SettingsFile *file, SettingsRecordInfo *info,
                        void *context) {
  uint8_t *key = malloc(info->key_len + 1);
  uint8_t *val = malloc(info->val_len + 1);
  info->get_key(file, key, info->key_len);
  info->get_val(file, val, info->val_len);
  key[info->key_len] = '\0';
  val[info->val_len] = '\0';

  printf("Read key of %s %d and val of %s %d\n", key, info->key_len, val, info->val_len);
  int key_i = atoi((char*)key + 1);
  cl_assert(key_i >= 0 && key_i < 255);
  uint8_t *counts = (uint8_t*)context;
  counts[key_i]++;

  status_t result = settings_file_get(file, key, info->key_len, val, info->val_len);
  cl_assert_equal_i(result, S_SUCCESS);

  int val_i = atoi((char*)val + 1);
  cl_assert_equal_i(key_i, val_i);
  return true;
}

void test_settings_file__each(void) {
  printf("\nTesting if we can use each...\n");

  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_each", 4096));

  uint8_t key[5];
  int key_len = 4;
  uint8_t val[5];
  int val_len = 4;
  for (int i = 0; i < 255; i++) {
    snprintf((char *)key, sizeof(key), "k%03d", i);
    snprintf((char *)val, sizeof(val), "v%03d", i);
    printf("Iteration %d key %s val %s\n", i, key, val);
    set_and_verify(&file, key, key_len, val, val_len);
  }

  uint8_t *counts = task_zalloc(255);
  settings_file_each(&file, prv_each_cb, counts);
  for (int i = 0; i < 255; i++) {
    cl_assert_equal_i(counts[i], 1);
  }
  task_free(counts);
}

static const uint8_t STOPPING_NUM = 117;
static bool prv_each_cb_quit_early(SettingsFile *file, SettingsRecordInfo *info,
                        void *context) {
  uint8_t *key = malloc(info->key_len + 1);
  uint8_t *val = malloc(info->val_len + 1);
  info->get_key(file, key, info->key_len);
  info->get_val(file, val, info->val_len);
  key[info->key_len] = '\0';
  val[info->val_len] = '\0';

  printf("Read key of %s %d and val of %s %d\n", key, info->key_len, val, info->val_len);
  int key_i = atoi((char*)key + 1);
  cl_assert(key_i >= 0 && key_i < 255);
  uint8_t *cur_val = (uint8_t*)context;
  *cur_val = key_i;

  int val_i = atoi((char*)val + 1);
  cl_assert_equal_i(key_i, val_i);

  if (key_i == STOPPING_NUM) {
    return false;
  }

  return true;
}

void test_settings_file__each_quit_early(void) {
  printf("\nTesting if we can use each and stop early at %d iterations...\n", STOPPING_NUM);

  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_each", 4096));

  uint8_t key[5];
  int key_len = 4;
  uint8_t val[5];
  int val_len = 4;
  for (int i = 0; i < 255; i++) {
    snprintf((char *)key, sizeof(key), "k%03d", i);
    snprintf((char *)val, sizeof(val), "v%03d", i);
    printf("Iteration %d key %s val %s\n", i, key, val);
    set_and_verify(&file, key, key_len, val, val_len);
  }

  uint8_t cur_val = 0;
  settings_file_each(&file, prv_each_cb_quit_early, (void *)&cur_val);

  cl_assert_equal_i(STOPPING_NUM, cur_val);
}

void test_settings_file__in_place(void) {
  printf("Testing that we can update a setting file in place\n");

  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_in_place", 4096));

  uint8_t key[4];
  int key_len = sizeof(key) - 1;
  memcpy(key, "key", sizeof(key));
  uint8_t val[4] = {0x11, 0x22, 0x33, 0x44};
  int val_len = sizeof(val);

  set_and_verify(&file, key, key_len, val, val_len);

  // setting a byte to all 0s should work
  settings_file_set_byte(&file, key, key_len, 2, 0x00);
  val[2] &= 0x00;
  verify(&file, key, key_len, val, val_len);

  // setting all 1s should do nothing - only reset bytes are applied
  settings_file_set_byte(&file, key, key_len, 2, 0xff);
  val[2] &= 0xff;
  verify(&file, key, key_len, val, val_len);

  // setting a mask should leave the other bytes untouched
  settings_file_set_byte(&file, key, key_len, 3, 0x40);
  val[3] &= 0x40;
  verify(&file, key, key_len, val, val_len);
}

// Test that we successfully detect and reallocate a settings file if it gets opened with
// a larger requested size.
void test_settings_file__reallocate_larger(void) {
  printf("\nTesting re-allocating a settings file to a larger size\n");

  const int k_initial_size = 0x1000;
  const int k_larger_size = 0x4000;
  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_file_reallocate_larger", k_initial_size));
  int orig_fd_size = pfs_get_file_size(file.iter.fd);

  uint8_t key[4];
  int key_len = sizeof(key) - 1;
  memcpy(key, "key", sizeof(key));
  uint8_t val[4];
  int val_len = sizeof(val) - 1;
  memcpy(val, "val", sizeof(val));

  set_and_verify(&file, key, key_len, val, val_len);

  // Now close the file and re-open it with a larger size
  settings_file_close(&file);
  cl_must_pass(settings_file_open(&file, "test_file_reallocate_larger", k_larger_size));
  int new_fd_size = pfs_get_file_size(file.iter.fd);
  cl_assert(new_fd_size > orig_fd_size);

  verify(&file, key, key_len, val, val_len);
}

// Test that we can start searching beginning at the record we previously found in a recent
// call into settings file. This makes sure we don't start searching at the beginning of a file
// each API call.
extern uint32_t settings_raw_iter_prv_get_num_record_searches(void);
void test_settings_file__iterator_wrapping(void) {
  printf("Testing that we can call get_len and get without iterator searching again\n");

  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "test_no_move", 4096));

  uint8_t key[5];
  int key_len = 4;
  uint8_t val[5];
  int val_len = 4;

  const int NUM_RECORDS = 128;

  // First, fill half the file up with random keys and values. This reduces
  // our usable space to slightly less than 2048 bytes.
  for (int i = 0; i < NUM_RECORDS; i++) {
    snprintf((char *)key, sizeof(key), "k%03d", i);
    snprintf((char *)val, sizeof(val), "v%03d", i);
    set_and_verify(&file, key, key_len, val, val_len);
  }

  const int search_for_idx = 57;
  snprintf((char *)key, sizeof(key), "k%03d", search_for_idx);

  // Move iter to the beginning to bring to a known state
  settings_raw_iter_begin(&file.iter);

  // Get the iter to the record we want
  cl_assert_equal_b(true, settings_file_exists(&file, key, key_len));

  // Check that we can do a get_len and we don't search past the current record
  int before_count = settings_raw_iter_prv_get_num_record_searches();
  cl_assert_equal_i(key_len, settings_file_get_len(&file, key, key_len));
  int after_count = settings_raw_iter_prv_get_num_record_searches();
  cl_assert_equal_i(before_count, after_count);

  // Check that we can do a get and we don't search past the current record
  before_count = settings_raw_iter_prv_get_num_record_searches();
  cl_must_pass(settings_file_get(&file, key, key_len, val, val_len));
  after_count = settings_raw_iter_prv_get_num_record_searches();
  cl_assert_equal_i(before_count, after_count);

  // Force us to move to the record `search_for_idx` + 1
  settings_raw_iter_next(&file.iter);

  // We now are forced to start searching in the middle, wrap around, and continue searching
  // from the beginning. This will result in us calling `settings_raw_iter_next` NUM_RECORDS - 1
  // times.
  before_count = settings_raw_iter_prv_get_num_record_searches();
  cl_must_pass(settings_file_get(&file, key, key_len, val, val_len));
  after_count = settings_raw_iter_prv_get_num_record_searches();
  cl_assert_equal_i(NUM_RECORDS - 1, after_count - before_count);
}
