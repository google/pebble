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

#include <string.h>
#include <stdlib.h>

#include "drivers/flash.h"
#include "flash_region/flash_region.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/filesystem/flash_translation.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/size.h"

#include "clar.h"

#include "fake_spi_flash.h"
#include "fake_rtc.h"
#include "stubs_analytics.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_print.h"
#include "stubs_prompt.h"
#include "stubs_serial.h"
#include "stubs_sleep.h"
#include "stubs_task_watchdog.h"

#define PFS_SECTOR_SIZE 4096

// a - 4K file full of 1's
static const char *const TEST_FILE_A_NAME = "a";
static const size_t TEST_FILE_A_SIZE = 4096;
static char s_test_file_a[TEST_FILE_A_SIZE];

// b - 0K file for appending
static const char *const TEST_FILE_B_NAME = "b";
static const size_t TEST_FILE_B_SIZE = 0;
static const size_t TEST_FILE_B_APPEND_SIZE = 8000;
static char s_test_file_b[TEST_FILE_B_APPEND_SIZE];

// c - space to perform non-append writes
static const char *const TEST_FILE_C_NAME = "c";
static const size_t TEST_FILE_C_SIZE = 9001; // it's over 9000!
static char s_test_file_c[TEST_FILE_C_SIZE];

static uint32_t num_pages(void) {
  return pfs_get_size() / PFS_SECTOR_SIZE;
}

void test_pfs__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  pfs_format(true /* write erase headers */);

  // should have: this should be baked into an image.  Perhaps with a test run
  // that captures a gold flash image?
  int fd;

  memset(s_test_file_a, 0, sizeof(s_test_file_a));
  fd = pfs_open(TEST_FILE_A_NAME, OP_FLAG_WRITE, FILE_TYPE_STATIC,
      sizeof(s_test_file_a));
  pfs_write(fd, (uint8_t *)s_test_file_a, sizeof(s_test_file_a));
  pfs_close(fd);

  memset(s_test_file_b, 0, sizeof(s_test_file_b));
  fd = pfs_open(TEST_FILE_B_NAME, OP_FLAG_WRITE, FILE_TYPE_STATIC,
      sizeof(s_test_file_b));
  pfs_close(fd);

  fd = pfs_open(TEST_FILE_C_NAME, OP_FLAG_WRITE, FILE_TYPE_STATIC,
      sizeof (s_test_file_c));
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_test_file_c); ++i) {
    s_test_file_c[i] = 'c';
  }
  pfs_write(fd, (uint8_t *)s_test_file_c, sizeof(s_test_file_c));
  pfs_close(fd);
}

void test_pfs__cleanup(void) {
  fake_spi_flash_cleanup();
}

void test_pfs__create(void) {
  char hello[] = { 'h', 'e', 'l', 'l', 'o' };
  int fd_z = pfs_open("z", OP_FLAG_WRITE, FILE_TYPE_STATIC, sizeof(hello));
  cl_assert(fd_z >= 0);
  int bytes_written = pfs_write(fd_z, (uint8_t *)hello, sizeof(hello));
  cl_assert(bytes_written == sizeof(hello));
  pfs_close(fd_z);
}

extern void test_force_garbage_collection(uint16_t start_page);
extern uint16_t test_get_file_start_page(int fd);
void test_pfs__garbage_collection(void) {
  char file_small[10];
  uint16_t start_page = 0;
  // create a sectors worth of files
  for (int i = 0; i < 16; i++) {
    snprintf(file_small, sizeof(file_small), "file%d", i);

    uint8_t buf[PFS_SECTOR_SIZE * 2];
    memset(&buf[0], i, sizeof(buf));
    int fd = pfs_open(file_small, OP_FLAG_WRITE, FILE_TYPE_STATIC, sizeof(buf));
    printf("fd=%d\n", fd);
    cl_assert(fd >= 0);
    cl_assert_equal_i(pfs_write(fd, &buf[0], sizeof(buf)), sizeof(buf));

    if (i == 0) {
      start_page = test_get_file_start_page(fd);
    }

    if ((i % 2) != 0) {
      pfs_close_and_remove(fd);
    } else {
      pfs_close(fd);
    }
  }

  // force garbage collection
  test_force_garbage_collection(start_page);

  // now make sure the files are still there!
  for (int i = 0; i < 16; i+=2) {

    snprintf(file_small, sizeof(file_small), "file%d", i);

    uint8_t buf[PFS_SECTOR_SIZE * 2];
    uint8_t bufcmp[PFS_SECTOR_SIZE * 2];

    memset(&bufcmp[0], i, sizeof(bufcmp));

    int fd = pfs_open(file_small, OP_FLAG_READ, FILE_TYPE_STATIC, sizeof(buf));
    cl_assert(fd >= 0);
    cl_assert_equal_i(pfs_read(fd, &buf[0], sizeof(buf)), sizeof(buf));
    cl_assert(memcmp(buf, bufcmp, sizeof(buf)) == 0);
    cl_assert_equal_i(pfs_close(fd), S_SUCCESS);
  }
}

void test_pfs__garbage_collection_when_full(void) {
  char file_name[10];
  int num = 0;
  int fd;
  while (1) {
    snprintf(file_name, sizeof(file_name), "file%d", num++);
    fd = pfs_open(file_name, OP_FLAG_WRITE, FILE_TYPE_STATIC, sizeof(file_name));
    cl_assert((fd >= 0) || (fd == E_OUT_OF_STORAGE));
    if (fd == E_OUT_OF_STORAGE) {
      break;
    }
    cl_assert_equal_i(pfs_close(fd), S_SUCCESS);
  }

  // the file system is full, lets delete a file
  snprintf(file_name, sizeof(file_name), "file%d", num / 2);
  fd = pfs_open(file_name, OP_FLAG_READ, 0, 0);
  cl_assert(fd >= 0);
  uint16_t target_start_page = test_get_file_start_page(fd);
  pfs_close_and_remove(fd);

  // lets force garbage collection on every sector
  test_force_garbage_collection(target_start_page);

  // now lets try to create a file
  fd = pfs_open(file_name, OP_FLAG_WRITE, FILE_TYPE_STATIC, sizeof(file_name));
  cl_assert_equal_i(target_start_page, test_get_file_start_page(fd));
  cl_assert(fd >= 0);
  cl_assert_equal_i(pfs_close(fd), S_SUCCESS);
}

void test_pfs__open(void) {
  int fd = pfs_open("dne", OP_FLAG_READ, 0, 0);
  cl_assert(fd == E_DOES_NOT_EXIST);

  fd = pfs_open("dne", OP_FLAG_OVERWRITE, FILE_TYPE_STATIC, 0);
  cl_assert(fd == E_DOES_NOT_EXIST);

  char name[] = {'a'};
  int fds[100]; // arbitrarily large # of FDs
  int i;
  for (i = 0; i < (sizeof(fds) - 1); i++) {
    fds[i] = pfs_open(name, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
    name[0]++;
    if (fds[i] < 0) {
      break;
    }
  }
  cl_assert(fds[i] == E_OUT_OF_RESOURCES);
  for (int j = 0; j < i; j++) {
    cl_assert(pfs_close(fds[j]) == S_SUCCESS);
  }

  fd = pfs_open("newfile", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  pfs_close(fd);

  int fd2 = pfs_open("newfile", OP_FLAG_READ, 0, 0);
  cl_assert((fd2 >= 0) && (fd == fd2)); // should have a cache hit
  pfs_close(fd2);

  fd = pfs_open("toobig", OP_FLAG_WRITE, FILE_TYPE_STATIC, 256 * 1024 * 1024);
  cl_assert(fd == E_OUT_OF_STORAGE);

  fd = pfs_open("toobig", OP_FLAG_READ, 0, 0);
  cl_assert(fd == E_DOES_NOT_EXIST);

  fd = pfs_open(NULL, OP_FLAG_READ, 0, 0);
  cl_assert(fd == E_INVALID_ARGUMENT);

  fd = pfs_open("newfile2", OP_FLAG_WRITE, FILE_TYPE_STATIC, 8000);
  cl_assert(fd >= 0);
  cl_assert(pfs_close(fd) == S_SUCCESS);
}

void test_pfs__page_lookup_cache(void) {
  // create fragmentation in the filesystem
  char file_small[10];

  char buf_small[50];
  for (int i = 0; i < num_pages(); i++) {
    snprintf(file_small, sizeof(file_small), "file%d", i);
    snprintf(buf_small, sizeof(buf_small), "This is small buf_small %d!", i);
    int len = strlen(buf_small);
    int fd = pfs_open(file_small, OP_FLAG_WRITE, FILE_TYPE_STATIC, len);
    cl_assert(fd >= 0);
    cl_assert(pfs_write(fd, (uint8_t *)buf_small, len) == len);
    cl_assert(pfs_close(fd) == S_SUCCESS);
    // delete every few files and a bunch of pages near the end
    if (((i & 0x1) == 0) ||
        ((i > ((num_pages() * 7) / 10)) && (i < ((num_pages() * 8) / 10)))) {
      cl_assert(pfs_remove(file_small) == S_SUCCESS);
    }
  }

  // We limit this number because we would overflow our uint8_t.
  const int num_regions = MIN(UINT8_MAX, (num_pages() * 5) / 10);
  int fd = pfs_open("page_lookup", OP_FLAG_WRITE, FILE_TYPE_STATIC, PFS_SECTOR_SIZE * num_regions);
  cl_assert(fd >= 0);

  char buf[PFS_SECTOR_SIZE];
  for (int i = 0; i < num_regions; i++) {
    memset(buf, 0xff - i, sizeof(buf));
    cl_assert_equal_i(pfs_write(fd, (uint8_t *)&buf[0], sizeof(buf)), sizeof(buf));
  }
  pfs_close(fd);
  fd = pfs_open("page_lookup", OP_FLAG_READ | OP_FLAG_USE_PAGE_CACHE, FILE_TYPE_STATIC, 0);
  cl_assert(fd >= 0);

  for (int i = 0; i < num_regions; i++) {
    cl_assert_equal_i(pfs_seek(fd, i * sizeof(buf), FSeekSet), i * sizeof(buf));
    uint8_t read_byte;
    for (int j = 0; j < 16; j++) {
      cl_assert_equal_i(pfs_read(fd, &read_byte, sizeof(read_byte)), sizeof(read_byte));
      cl_assert_equal_i(read_byte, 0xff - i);
    }
  }

  pfs_close(fd);
}

void test_pfs__write(void) {
  int rv = pfs_write(-1, NULL, 0);
  cl_assert(rv == E_INVALID_ARGUMENT);

  rv = pfs_write(1000000, NULL, 0);
  cl_assert(rv == E_INVALID_ARGUMENT);

  rv = pfs_write(0, NULL, 0);
  cl_assert(rv == E_INVALID_ARGUMENT);

  uint8_t buf[10];
  int fd = pfs_open("newfile", OP_FLAG_WRITE | OP_FLAG_READ,
      FILE_TYPE_STATIC, sizeof(buf));
  for (int i = 0; i < sizeof(buf); i++) {
    buf[i] = i;
  }
  rv = pfs_write(fd, NULL, sizeof(buf));
  cl_assert(rv == E_INVALID_ARGUMENT);
  rv = pfs_write(fd, buf, sizeof(buf) / 2);
  int off = sizeof(buf) / 2;
  rv = pfs_write(fd, &buf[off], sizeof(buf) - off);
  cl_assert(rv == (sizeof(buf) - off));

  rv = pfs_seek(fd, 0, FSeekSet);
  cl_assert(rv == 0);
  memset(buf, 0x00, sizeof(buf));
  uint8_t bigbuf[11];
  rv = pfs_write(fd, bigbuf, sizeof(bigbuf));
  cl_assert(rv == E_RANGE);

  rv = pfs_read(fd, buf, sizeof(buf));
  cl_assert(rv == sizeof(buf));
  for (int i = 0; i < sizeof(buf); i++) {
    cl_assert(buf[i] == i);
  }
  pfs_close(fd);

  fd = pfs_open("newfile", OP_FLAG_READ, 0, 0);
  rv = pfs_write(fd, buf, sizeof(buf));
  cl_assert(rv == E_INVALID_ARGUMENT);
}

void test_pfs__overwrite(void) {
  const char *file = "testfile";
  const char *string = "original file!";
  const char *overwrite_string = "overwrite file!";
  int rv;

  int fd = pfs_open(file, OP_FLAG_WRITE, FILE_TYPE_STATIC, strlen(string));
  cl_assert(fd >= 0);
  rv = pfs_write(fd, (uint8_t *)string, strlen(string));
  cl_assert_equal_i(rv, strlen(string));

  int tmp_fd = pfs_open(file, OP_FLAG_OVERWRITE, FILE_TYPE_STATIC, strlen(overwrite_string));
  cl_assert(fd >= 0);
  pfs_init(false); // simulate a reboot

  uint8_t read_buf[strlen(string)];
  fd = pfs_open(file, OP_FLAG_READ, 0, 0);
  cl_assert(fd >= 0);
  rv = pfs_read(fd, &read_buf[0], strlen(string));
  cl_assert_equal_i(rv, strlen(string));
  cl_assert(memcmp(string, read_buf, strlen(string)) == 0);
  pfs_close(fd);

  tmp_fd = pfs_open(file, OP_FLAG_OVERWRITE, FILE_TYPE_STATIC, strlen(overwrite_string));
  cl_assert(tmp_fd >= 0);
  rv = pfs_write(tmp_fd, (uint8_t *)overwrite_string, strlen(overwrite_string));
  cl_assert_equal_i(rv, strlen(overwrite_string));
  cl_assert_equal_i(pfs_close(tmp_fd), S_SUCCESS);

  uint8_t new_buf[strlen(overwrite_string)];
  fd = pfs_open(file, OP_FLAG_READ, 0, 0);
  cl_assert(fd >= 0);
  rv = pfs_read(fd, &new_buf[0], strlen(overwrite_string));
  cl_assert_equal_i(rv, strlen(overwrite_string));
  cl_assert(memcmp(overwrite_string, new_buf, strlen(overwrite_string)) == 0);
  pfs_close(fd);
}

void test_pfs__seek(void) {
  int len = 10;
  int fd = pfs_open("newfile", OP_FLAG_WRITE, FILE_TYPE_STATIC, len);
  cl_assert(fd >= 0);

  int rv = pfs_seek(fd, len, FSeekSet);
  cl_assert(rv == len);
  rv = pfs_seek(fd, 0, FSeekCur);
  cl_assert(rv == len);

  rv = pfs_seek(fd, -5, FSeekCur);
  cl_assert(rv == (len - 5));

  rv = pfs_seek(fd, -6, FSeekCur);
  cl_assert(rv == E_RANGE);
  rv = pfs_seek(fd, 0, FSeekSet);
  cl_assert(rv == 0);
  rv = pfs_seek(fd, len + 1, FSeekCur);
  cl_assert(rv == E_RANGE);

  rv = pfs_seek(fd, len + 1, FSeekSet);
  cl_assert(rv == E_RANGE);
  rv = pfs_seek(fd, -1, FSeekSet);
  cl_assert(rv == E_RANGE);
}

void test_pfs__read(void) {
  const int rd_len = 10;

  int rv = pfs_read(-1, NULL, rd_len);
  cl_assert(rv == E_INVALID_ARGUMENT);

  rv = pfs_read(0, NULL, rd_len);
  cl_assert(rv == E_INVALID_ARGUMENT);

  int fd = pfs_open("newfile", OP_FLAG_WRITE, FILE_TYPE_STATIC, rd_len);
  cl_assert(fd >= 0);
  uint8_t buf[rd_len];
  rv = pfs_read(fd, buf, rd_len);
  cl_assert(rv == E_INVALID_ARGUMENT);
  pfs_close(fd);

  fd = pfs_open("newfile", OP_FLAG_READ, 0, 0);
  rv = pfs_read(fd, buf, rd_len);
  cl_assert(rv == rd_len);
  rv = pfs_read(fd, buf, 1);
  cl_assert(rv == E_RANGE);
  rv = pfs_seek(fd, 0, FSeekSet);
  cl_assert(rv == 0);
  rv = pfs_read(fd, NULL, rd_len);
  cl_assert(rv == E_INVALID_ARGUMENT);
  rv = pfs_read(fd, buf, rd_len + 1);
  cl_assert(rv == E_RANGE);
}

void test_pfs__close(void) {
  // shouldn't be able to close fds that are not open
  int rv = pfs_close(-1);
  cl_assert(rv == E_INVALID_ARGUMENT);
  rv = pfs_close(0);
  cl_assert(rv == E_INVALID_ARGUMENT);
  rv = pfs_close(1000000);
  cl_assert(rv == E_INVALID_ARGUMENT);

  int fd = pfs_open("file", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  rv = pfs_close(fd);
  cl_assert(rv == S_SUCCESS);
  rv = pfs_close(fd); // should not be able to double close an fd
  cl_assert(rv == E_INVALID_ARGUMENT);
}

void test_pfs__remove(void) {
  cl_assert(pfs_remove(NULL) == E_INVALID_ARGUMENT);

  char *fname = "abc";
  int fd = pfs_open(fname, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10 * 1024);
  cl_assert(fd >= 0);
  cl_assert(pfs_close(fd) == S_SUCCESS);
  cl_assert(pfs_remove(fname) == S_SUCCESS);

  cl_assert(pfs_remove(fname) == E_DOES_NOT_EXIST);
  cl_assert(pfs_open(fname, OP_FLAG_READ, 0, 0) == E_DOES_NOT_EXIST);
}

#define NUM_ENTRIES 3
void test_pfs__close_and_remove(void) {
  cl_assert(pfs_close_and_remove(-1) == E_INVALID_ARGUMENT);
  // there shouldn't be any fd open at this point
  cl_assert(pfs_close_and_remove(2) == E_INVALID_ARGUMENT);

  int fd[NUM_ENTRIES];
  char *fname[NUM_ENTRIES] = {"a", "b", "c" };

  // create several files
  for (int i = 0; i < NUM_ENTRIES; i++) {
    fd[i] = pfs_open(fname[i], OP_FLAG_WRITE, FILE_TYPE_STATIC, 3 * 4096);
    cl_assert(fd[i] >= 0);
  }

  // test close and remove
  for (int i = (NUM_ENTRIES - 1); i >= 0; i--) {
    cl_assert_equal_i(pfs_close_and_remove(fd[i]), S_SUCCESS);
  }

  // now make sure none of the files exist
  for (int i = 0; i < NUM_ENTRIES; i++) {
    int temp_fd = pfs_open(fname[i], OP_FLAG_READ, 0, 0);
    cl_assert(temp_fd == E_DOES_NOT_EXIST);
  }
}

void test_pfs__discontiguous_page_test(void) {

  pfs_format(false /* write erase headers */); // start with an empty flash
  pfs_init(false);

  char file_small[10], file_large[10];

  char buf[50];
  for (int i = 0; i < num_pages(); i++) {
    snprintf(file_small, sizeof(file_small), "file%d", i);
    snprintf(buf, sizeof(buf), "This is small buf %d!", i);
    int len = strlen(buf);
    int fd = pfs_open(file_small, OP_FLAG_WRITE, FILE_TYPE_STATIC, len);
    cl_assert(fd >= 0);
    cl_assert(pfs_write(fd, (uint8_t *)buf, len) == len);
    cl_assert(pfs_close(fd) == S_SUCCESS);
    if ((i % 2) == 0) {
      cl_assert(pfs_remove(file_small) == S_SUCCESS);
    }
  }

  // now write two large files that are interleaved between sectors
  // was negative
  int bytes_available = (num_pages() / 2) * 4000;
  int large_file_size = bytes_available / 2;

  printf("Space Avail = %d\n", bytes_available);

  uint8_t *bigbuf = (uint8_t *)calloc(large_file_size, 1);
  uint16_t curr = 0;
  for (int i = 0; i < 2; i++) {
    snprintf(file_large, sizeof(file_large), "large%d", i);
    for (int i = 0; i < large_file_size / 4; i++) {
      *(((uint32_t *)((uint8_t *)bigbuf)) + i) = curr | (curr << 16);
      curr++;
    }

    int fd = pfs_open(file_large, OP_FLAG_WRITE, FILE_TYPE_STATIC, large_file_size);
    printf("the fd is: %d\n", fd);
    cl_assert(fd >= 0);
    cl_assert(pfs_write(fd, bigbuf, large_file_size) == large_file_size);
    cl_assert(pfs_close(fd) == S_SUCCESS);
  }
  free(bigbuf);

  // now read back the large files
  curr = 0;
  for (int i = 0; i < 2; i++) {
    snprintf(file_large, sizeof(file_large), "large%d", i);
    int fd = pfs_open(file_large, OP_FLAG_READ, 0, 0);
    cl_assert(fd >= 0);
    size_t sz = pfs_get_file_size(fd);
    cl_assert(sz == large_file_size);

    uint8_t *b = calloc(sz, 1);
    cl_assert(pfs_read(fd, b, sz) == sz);

    for (int j = 0; j < large_file_size / 4; j++) {
      uint32_t *val = ((uint32_t *)((uint8_t *)b)) + j;
      cl_assert(*val == (curr | (curr << 16)));
      curr++;
    }

    cl_assert(pfs_close(fd) == S_SUCCESS);
    free(b);
  }
}

void test_pfs__file_span_regions(void) {
  pfs_format(false /* write erase headers */); // start with an empty flash
  pfs_init(false);

  char name[128];
  snprintf(name, sizeof(name), "bigfile");

  //Fill up entire memory section, subtract 32768 for header space.
  int fd = pfs_open(name, OP_FLAG_WRITE, FILE_TYPE_STATIC, pfs_get_size() - (num_pages()*128));

  printf("%d\n", pfs_get_size() - 1024);

  cl_assert(fd >= 0);
  cl_assert(pfs_close(fd) == S_SUCCESS);
  cl_assert(pfs_remove(name) == S_SUCCESS);
}

void test_pfs__active_regions(void) {
  pfs_format(false);
  cl_assert(!pfs_active_in_region(0, pfs_get_size()));

  // erase every page and make sure pfs is active
  pfs_format(true);
  cl_assert(pfs_active_in_region(0, pfs_get_size()));

  // write something on every page and make sure pfs is active
  char file_name[10];
  for (int i = 0; i < num_pages(); i++) {
    snprintf(file_name, sizeof(file_name), "file%d", i);
    int fd = pfs_open(file_name, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
    cl_assert(fd >= 0);
    cl_assert(pfs_close(fd) == S_SUCCESS);
  }

  cl_assert(pfs_active_in_region(0, pfs_get_size()));

  // delete every page and make sure pfs is active
  for (int i = 0; i < num_pages(); i++) {
    snprintf(file_name, sizeof(file_name), "file%d", i);
    cl_assert(pfs_remove(file_name) == S_SUCCESS);
  }
  cl_assert(pfs_active_in_region(0, pfs_get_size()));

  // continuation page on region and make sure pfs is active
  pfs_format(true);
  int fd = pfs_open("testfile", OP_FLAG_WRITE, FILE_TYPE_STATIC, 68000);
  cl_assert(fd >= 0);
  cl_assert(pfs_close(fd) == S_SUCCESS);
  cl_assert(pfs_active_in_region(32000, 68000));

}

int run_full_flash_region_test(void) {
  char name[128];

  // assumes # pages is a multiple of 2
  int st_val = 0;
  const int f_size = (((PFS_SECTOR_SIZE * 2) - 400) / sizeof(st_val)) * sizeof(st_val);
  int num_vals = f_size / sizeof(st_val);

  int idx = 0;
  int fd, rv;

  while (true) {
    snprintf(name, sizeof(name), "file%d", idx);
    if ((fd = pfs_open(name, OP_FLAG_WRITE, FILE_TYPE_STATIC, f_size)) < 0) {
      break;
    }
    for (int i = 0; i < num_vals; i++) {
      st_val = st_val + i;
      rv = pfs_write(fd, (uint8_t *)&st_val, sizeof(st_val));
      cl_assert_equal_i(rv, sizeof(st_val));
    }
    pfs_close(fd);
    idx++;
  }

  cl_assert_equal_i((status_t)fd, E_OUT_OF_STORAGE);

  // read back files to make sure they are all correct
  st_val = 0;
  for (int i = 0; i < idx; i++) {
    snprintf(name, sizeof(name), "file%d", i);
    if ((fd = pfs_open(name, OP_FLAG_READ, FILE_TYPE_STATIC, f_size)) < 0) {
      break;
    }
    for (int j = 0; j < num_vals; j++) {
      st_val = st_val + j;
      int out_val;
      rv = pfs_read(fd, (uint8_t *)&out_val, sizeof(out_val));
      cl_assert_equal_i(rv, sizeof(st_val));
      cl_assert_equal_i(out_val, st_val);
    }
    pfs_close(fd);
  }

  return (idx);
}

void test_pfs__out_of_space(void) {
  pfs_format(false /* write erase headers */); // start with an empty flash
  pfs_init(false);

  int num_iters = 30;

  for (int i = 0; i < num_iters; i++) {
    int files_written = run_full_flash_region_test();
    if ((i % 2) == 0) {
      pfs_init(true); // simulate a reboot
    }
    // delete all files
    for (int i = (files_written - 1); i >= 0; i--) {
      char name[128];
      snprintf(name, sizeof(name), "file%d", i);
      cl_assert_equal_i(pfs_remove(name), S_SUCCESS);
    }
  }
}

void test_pfs__active_in_region(void) {
  cl_assert(pfs_active_in_region(0, pfs_get_size()));
}

void test_pfs__get_size(void) {
  cl_assert(pfs_get_size() == (ftl_get_size() - SECTOR_SIZE_BYTES));
}

void test_pfs__migration(void) {
  // reset the flash
  fake_spi_flash_cleanup();
  fake_spi_flash_init(0, 0x1000000);

  extern void ftl_force_version(int version_idx);
  pfs_init(true);
  ftl_force_version(1);

  // simulate a migration by leaving files in various states
  // in the first region. Then try to add another region and confirm
  // none of the files have been corrupted
  char file_small[10];

  char buf[50];
  const int erase_count = 3;
  for (int num_erases = 0; num_erases < erase_count; num_erases++) {
    for (int i = 0; i < num_pages(); i++) {
      snprintf(file_small, sizeof(file_small), "file%d", i);
      snprintf(buf, sizeof(buf), "This is small buf %d!", i);
      int len = strlen(buf);
      int fd = pfs_open(file_small, OP_FLAG_WRITE, FILE_TYPE_STATIC, len);
      cl_assert(fd >= 0);
      cl_assert(pfs_write(fd, (uint8_t *)buf, len) == len);
      cl_assert(pfs_close(fd) == S_SUCCESS);
      if (num_erases != (erase_count - 1)) {
        cl_assert(pfs_remove(file_small) == S_SUCCESS);
      }
    }
  }

  int original_page_count = num_pages();
  ftl_populate_region_list();
  // make sure something was added
  PBL_LOG(LOG_LEVEL_DEBUG, "original pages %u, num pages: %u", original_page_count, num_pages());
  cl_assert(original_page_count < num_pages());

  for (int i = 0; i < original_page_count; i++) {
    snprintf(file_small, sizeof(file_small), "file%d", i);
    snprintf(buf, sizeof(buf), "This is small buf %d!", i);
    int len = strlen(buf);
    char rbuf[len];
    int fd = pfs_open(file_small, OP_FLAG_READ, FILE_TYPE_STATIC, 0);
    cl_assert(fd >= 0);
    cl_assert_equal_i(pfs_read(fd, (uint8_t *)rbuf, len), len);
    cl_assert(memcmp(rbuf, buf, len) == 0);
    cl_assert(fd >= 0);
    cl_assert(pfs_close(fd) == S_SUCCESS);
  }
}

static uint32_t s_watch_file_callback_called_count = 0;
static void prv_file_changed_callback(void *data) {
  s_watch_file_callback_called_count++;
}

void test_pfs__watch_file_callbacks(void) {
  const char* file_name = "newfile";

  PFSCallbackHandle cb_handle = pfs_watch_file(file_name, prv_file_changed_callback,
                                               FILE_CHANGED_EVENT_ALL, NULL);

  // Callback should get invoked if we close with write access
  s_watch_file_callback_called_count = 0;
  int fd = pfs_open(file_name, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  pfs_close(fd);
  cl_assert(s_watch_file_callback_called_count == 1);

  // Callback should not get invoked if we close with read access
  s_watch_file_callback_called_count = 0;
  fd = pfs_open(file_name, OP_FLAG_READ, 0, 0);
  cl_assert(fd >= 0);
  pfs_close(fd);
  cl_assert(s_watch_file_callback_called_count == 0);

  // Callback should get invoked if we remove the file
  s_watch_file_callback_called_count = 0;
  pfs_remove(file_name);
  cl_assert(s_watch_file_callback_called_count == 1);

  pfs_unwatch_file(cb_handle);

  // Callback should not get invoked anymore
  s_watch_file_callback_called_count = 0;
  fd = pfs_open(file_name, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  pfs_close(fd);
  cl_assert(s_watch_file_callback_called_count == 0);
}

extern status_t test_scan_for_last_written(void);
void test_pfs__last_written_page(void) {
  pfs_format(true);
  pfs_init(false);

  // we just formatted so we shouldn't have a last written page
  cl_assert(test_scan_for_last_written() < 0);

  // set up an environment that forces some garbage collection
  for (int i = 0; i < num_pages(); i++) {
    char name[10];
    snprintf(name, sizeof(name), "test%d", i);
    int fd = pfs_open(name, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);

    cl_assert(fd >= 0);
    if (((i % 2) == 0) || i > ((num_pages() * 2) / 10)) {
      cl_assert(pfs_close_and_remove(fd) >= 0);
    } else {
      pfs_close(fd);
    }

    cl_assert(test_scan_for_last_written() >= 0);
  }

  uint32_t size = ((num_pages() * 8) / 10) * PFS_SECTOR_SIZE;
  int fd = pfs_open("test", OP_FLAG_WRITE, FILE_TYPE_STATIC, size);
  cl_assert(fd >= 0);

  cl_assert(test_scan_for_last_written() >= 0);
}


extern void test_force_reboot_during_garbage_collection(uint16_t start_page);
extern void pfs_reset_all_state(void);
void test_pfs__reboot_during_gc(void) {
  pfs_format(true);
  pfs_reset_all_state();
  pfs_init(false);

  const int pages_to_write = 16;

  char file_small[10];
  uint16_t start_page;

  for (int i = 0; i < pages_to_write; i++) {
    snprintf(file_small, sizeof(file_small), "file%d", i);

    uint8_t buf[PFS_SECTOR_SIZE * 2];
    memset(&buf[0], i, sizeof(buf));
    int fd = pfs_open(file_small, OP_FLAG_WRITE, FILE_TYPE_STATIC, sizeof(buf));
    cl_assert(fd >= 0);
    cl_assert_equal_i(pfs_write(fd, &buf[0], sizeof(buf)), sizeof(buf));

    if (i == 0) {
      start_page = test_get_file_start_page(fd);
    }

    if ((i % 2) != 0) {
      pfs_close_and_remove(fd);
    } else {
      pfs_close(fd);
    }
  }

  // force partial garbage collection
  test_force_reboot_during_garbage_collection(start_page);

  // reset our state variables, there should be no files found
  pfs_reset_all_state();
  for (int i = 0; i < pages_to_write; i++) {
    snprintf(file_small, sizeof(file_small), "file%d", i);
    int fd = pfs_open(file_small, OP_FLAG_READ, FILE_TYPE_STATIC, 0);
    cl_assert(fd < 0);
  }

  // simulate a reboot, all files should now appear because the GC completes
  pfs_init(false);

    // now make sure the files are still there!
  for (int i = 0; i < pages_to_write; i+=2) {

    snprintf(file_small, sizeof(file_small), "file%d", i);

    uint8_t buf[PFS_SECTOR_SIZE * 2];
    uint8_t bufcmp[PFS_SECTOR_SIZE * 2];

    memset(&bufcmp[0], i, sizeof(bufcmp));

    int fd = pfs_open(file_small, OP_FLAG_READ, FILE_TYPE_STATIC, 0);
    cl_assert(fd >= 0);
    cl_assert_equal_i(pfs_read(fd, &buf[0], sizeof(buf)), sizeof(buf));
    cl_assert(memcmp(buf, bufcmp, sizeof(buf)) == 0);
    cl_assert_equal_i(pfs_close(fd), S_SUCCESS);
  }
}

static bool prv_filename_filter_a_prefix_cb(const char *name) {
  return (strncmp(name, "a_", 2) == 0);
}

static bool prv_find_name(ListNode *node, void *data) {
  PFSFileListEntry *entry = (PFSFileListEntry *)node;
  return (strcmp(entry->name, (char *)data) == 0);
}

void test_pfs__file_list(void) {
  pfs_format(true);
  pfs_init(false);

  // Create some files
  int fd;
  fd = pfs_open("a_test_0", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  fd = pfs_open("a_test_1", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  fd = pfs_open("b_test_0", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  fd = pfs_open("b_test_1", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);

  // Get a directory listing with no filtering
  PFSFileListEntry *dir_list;
  dir_list = pfs_create_file_list(NULL);

  // Should have 4 entries in it
  cl_assert_equal_i(list_count(&dir_list->list_node), 4);
  cl_assert(list_find(&dir_list->list_node, prv_find_name, "a_test_0"));
  cl_assert(list_find(&dir_list->list_node, prv_find_name, "a_test_1"));
  cl_assert(list_find(&dir_list->list_node, prv_find_name, "b_test_0"));
  cl_assert(list_find(&dir_list->list_node, prv_find_name, "b_test_1"));
  pfs_delete_file_list(dir_list);


  // Do another search using a filter
  dir_list = pfs_create_file_list(prv_filename_filter_a_prefix_cb);

  // Should have 2 entries in it
  cl_assert_equal_i(list_count(&dir_list->list_node), 2);
  cl_assert(list_find(&dir_list->list_node, prv_find_name, "a_test_0"));
  cl_assert(list_find(&dir_list->list_node, prv_find_name, "a_test_1"));
  pfs_delete_file_list(dir_list);
}

static bool prv_file_exists(char *name) {
  int fd = pfs_open(name, OP_FLAG_READ, FILE_TYPE_STATIC, 0);
  if (fd < 0) {
    return false;
  }
  pfs_close(fd);
  return true;
}

extern void test_override_last_written_page(uint16_t start_page);
extern void test_force_recalc_of_gc_region(void);
// PBL-20973
//
// On boot, we scan pfs for the last page which was written. We then scan for a garbage collection
// sector (requirement is that no files exist in the entire sector) & use the last page as a
// starting point for where we will create initialize new files.
//
// There is a perfect storm of events which can lead to corruption on reboot. The sequence is as follows:
// 1) The last written file is deleted right before a reboot
// 2) No other files exist in the same sector as the one where the last file was deleted
//
// Upon reboot, a file could be created in this region & then later deleted when a garbage
// collection was needed. In practice, I think this is most likely to happen after issuing a command like
// 'factory reset fast' which we rely on heavily for automated testing
void test_pfs__start_page_collides_with_gc_page(void) {
  pfs_format(true);

  const uint8_t pages_per_sector = SECTOR_SIZE_BYTES / PFS_SECTOR_SIZE;
  const uint8_t start_page_offset = pages_per_sector / 2;

  test_override_last_written_page(start_page_offset);
  test_force_recalc_of_gc_region();
  pfs_init(false);

  int expected_remaing_files = pages_per_sector - 1;

  // scatter files across two sectors
  for (int i = 0; i < (pages_per_sector + start_page_offset); i++) {
    char filename[20];
    sprintf(filename, "test%d", i + start_page_offset);
    int fd = pfs_open(filename, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
    cl_assert(fd >= 0);
    pfs_close(fd);

    // delete some files in the region so a garbage collection will do something
    if (i >= expected_remaing_files) {
      pfs_remove(filename);
    }
  }

  test_force_garbage_collection(pages_per_sector);

  for (int i = 0; i < expected_remaing_files; i++) {
    char filename[20];
    sprintf(filename, "test%d", i + start_page_offset);
    int fd = pfs_open(filename, OP_FLAG_READ, FILE_TYPE_STATIC, 10);
    cl_assert(fd >= 0);
    pfs_close(fd);
  }
}

void test_pfs__remove_files(void) {
  pfs_format(true);
  pfs_init(false);

  // Create some files
  int fd;
  fd = pfs_open("a_test_0", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  pfs_close(fd);
  fd = pfs_open("a_test_1", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  pfs_close(fd);
  fd = pfs_open("b_test_0", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  pfs_close(fd);
  fd = pfs_open("b_test_1", OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
  cl_assert(fd >= 0);
  pfs_close(fd);

  // Should have 4 entries in pfs
  cl_assert_equal_b(prv_file_exists("a_test_0"), true);
  cl_assert_equal_b(prv_file_exists("a_test_1"), true);
  cl_assert_equal_b(prv_file_exists("b_test_0"), true);
  cl_assert_equal_b(prv_file_exists("b_test_1"), true);

  pfs_remove_files(prv_filename_filter_a_prefix_cb);

  // Should have only files starting with b_
  cl_assert_equal_b(prv_file_exists("a_test_0"), false);
  cl_assert_equal_b(prv_file_exists("a_test_1"), false);
  cl_assert_equal_b(prv_file_exists("b_test_0"), true);
  cl_assert_equal_b(prv_file_exists("b_test_1"), true);
}

void test_pfs__doesnt_give_out_fd_zero(void) {
  for (int i = 5; i > 0; --i) {
    char filename[20];
    sprintf(filename, "test%d", i);
    int fd = pfs_open(filename, OP_FLAG_WRITE, FILE_TYPE_STATIC, 10);
    cl_assert(fd > 0);
  }
}
