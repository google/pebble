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

#include "util/circular_buffer.h"

#include "clar.h"

#include <string.h>

#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"

void test_circular_buffer__initialize(void) {
}

void test_circular_buffer__cleanup(void) {
}

void test_circular_buffer__circular_buffer(void) {
  CircularBuffer buffer;
  uint8_t storage[8];
  circular_buffer_init(&buffer, storage, sizeof(storage));

  const uint8_t* out_buffer;
  uint16_t out_length;

  // We should start out empty
  cl_assert(!circular_buffer_read(&buffer, 1, &out_buffer, &out_length));

  cl_assert(circular_buffer_write(&buffer, (uint8_t*) "123", 3));
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 5);
  cl_assert(circular_buffer_write(&buffer, (uint8_t*) "456", 3));
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 2);
  cl_assert(!circular_buffer_write(&buffer, (uint8_t*) "789", 3)); // too big
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 2);

  cl_assert(circular_buffer_read(&buffer, 4, &out_buffer, &out_length));
  cl_assert_equal_i(out_length, 4);
  cl_assert(memcmp(out_buffer, "1234", 4) == 0);
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 2);

  cl_assert(circular_buffer_consume(&buffer, 4));
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 6);

  // Now there's just 56 in the buffer. Fill it to the brim
  cl_assert(circular_buffer_write(&buffer, (uint8_t*) "789", 3));
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 3);
  cl_assert(circular_buffer_write(&buffer, (uint8_t*) "abc", 3));
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 0);
  cl_assert(!circular_buffer_write(&buffer, (uint8_t*) "d", 1)); // too full
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 0);

  // Try a wrapped read
  cl_assert(circular_buffer_read(&buffer, 6, &out_buffer, &out_length));
  cl_assert_equal_i(out_length, 4);
  cl_assert(memcmp(out_buffer, (uint8_t*) "5678", 4) == 0);
  cl_assert(circular_buffer_consume(&buffer, 4));

  // Get the rest of the wrapped read
  cl_assert(circular_buffer_read(&buffer, 2, &out_buffer, &out_length));
  cl_assert_equal_i(out_length, 2);
  cl_assert(memcmp(out_buffer, (uint8_t*) "9a", 2) == 0);
  cl_assert(circular_buffer_consume(&buffer, 2));

  // Consume one without reading it
  cl_assert(circular_buffer_consume(&buffer, 1));

  // Read the last little bit
  cl_assert(circular_buffer_read(&buffer, 1, &out_buffer, &out_length));
  cl_assert_equal_i(out_length, 1);
  cl_assert(memcmp(out_buffer, (uint8_t*) "c", 1) == 0);
  cl_assert(circular_buffer_consume(&buffer, 1));

  // And we should be empty
  cl_assert(!circular_buffer_read(&buffer, 1, &out_buffer, &out_length));
  cl_assert(!circular_buffer_consume(&buffer, 1));
}

void test_circular_buffer__copy(void) {
  CircularBuffer buffer;
  uint8_t storage[8];
  circular_buffer_init(&buffer, storage, sizeof(storage));

  const uint16_t data_out_size = 8;
  uint8_t data_out[data_out_size];

  // Test copy when there is nothing in the buffer:
  cl_assert_equal_i(circular_buffer_copy(&buffer, data_out, data_out_size), 0);

  // Write + consume, so read index is at 2:
  circular_buffer_write(&buffer, (uint8_t *)"0123", 4);
  circular_buffer_consume(&buffer, 2);

  // Write data that will be wrapped:
  circular_buffer_write(&buffer, (uint8_t *)"456789", 6);

  // Test copying the whole thing (providing buffer of 8 bytes):
  memset(data_out, 0, data_out_size);
  cl_assert_equal_i(circular_buffer_copy(&buffer, data_out, data_out_size), 8);
  cl_assert(memcmp("23456789", data_out, 8) == 0);

  // Test partial copy (providing buffer of 6 bytes):
  memset(data_out, 0, data_out_size);
  cl_assert_equal_i(circular_buffer_copy(&buffer, data_out, 6), 6);
  cl_assert(memcmp("234567", data_out, 6) == 0);
}

void test_circular_buffer__copy_offset(void) {
  CircularBuffer buffer;
  uint8_t storage[8];
  circular_buffer_init(&buffer, storage, sizeof(storage));

  const uint16_t data_out_size = 8;
  uint8_t data_out[data_out_size];

  // Assert zero bytes copied, empty buffer:
  cl_assert_equal_i(circular_buffer_copy_offset(&buffer, 0 /* start_offset */,
                                                data_out, data_out_size), 0);
  // Assert zero bytes copied, start offset > storage size:
  cl_assert_equal_i(circular_buffer_copy_offset(&buffer, sizeof(storage) + 1 /* start_offset */,
                                                data_out, data_out_size), 0);

  // Valid offset, non-wrapping copy:
  circular_buffer_write(&buffer, (uint8_t *)"0123", 4);
  cl_assert_equal_i(circular_buffer_copy_offset(&buffer, 3 /* start_offset */,
                                                data_out, data_out_size), 1);
  cl_assert(memcmp("3", data_out, 1) == 0);

  // Offset as long as the available data:
  cl_assert_equal_i(circular_buffer_copy_offset(&buffer, 4 /* start_offset */,
                                                data_out, data_out_size), 0);

  // Free up 2 bytes at the beginning:
  circular_buffer_consume(&buffer, 2);

  // Write data that will be wrapped:
  cl_assert_equal_b(circular_buffer_write(&buffer, (uint8_t *)"456789", 6), true);
  cl_assert_equal_i(circular_buffer_copy_offset(&buffer, 2 /* start_offset */,
                                                data_out, data_out_size), 6);
  cl_assert(memcmp("456789", data_out, 6) == 0);
}

void test_circular_buffer__direct_write(void) {
  CircularBuffer buffer;
  uint8_t storage[8];
  circular_buffer_init(&buffer, storage, sizeof(storage));

  circular_buffer_write(&buffer, (uint8_t *)"0123", 4);

  uint8_t *data_out;
  uint16_t contiguous_num_bytes_left = circular_buffer_write_prepare(&buffer, &data_out);
  cl_assert_equal_i(contiguous_num_bytes_left, 4);

  memcpy(data_out, "456", 3);
  circular_buffer_write_finish(&buffer, 3);
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 1);
  cl_assert_equal_i(circular_buffer_get_read_space_remaining(&buffer), 7);

  contiguous_num_bytes_left = circular_buffer_write_prepare(&buffer, &data_out);
  cl_assert_equal_i(contiguous_num_bytes_left, 1);

  memcpy(data_out, "7", 1);
  circular_buffer_write_finish(&buffer, 1);
  cl_assert_equal_i(circular_buffer_get_write_space_remaining(&buffer), 0);
  cl_assert_equal_i(circular_buffer_get_read_space_remaining(&buffer), 8);

  contiguous_num_bytes_left = circular_buffer_write_prepare(&buffer, &data_out);
  cl_assert_equal_i(contiguous_num_bytes_left, 0);
  cl_assert_equal_p(data_out, NULL);

  const uint16_t copy_out_size = 8;
  uint8_t copy_out[copy_out_size];
  cl_assert_equal_i(circular_buffer_copy(&buffer, copy_out, copy_out_size), 8);
  cl_assert_equal_i(memcmp(copy_out, "01234567", 8), 0);

  circular_buffer_consume(&buffer, 2);
  cl_assert_equal_i(circular_buffer_copy(&buffer, copy_out, copy_out_size), 6);
  cl_assert_equal_i(memcmp(copy_out, "234567", 6), 0);

  contiguous_num_bytes_left = circular_buffer_write_prepare(&buffer, &data_out);
  cl_assert_equal_i(contiguous_num_bytes_left, 2);
  memcpy(data_out, "AB", 2);
  circular_buffer_write_finish(&buffer, 2);

  cl_assert_equal_i(circular_buffer_copy(&buffer, copy_out, copy_out_size), 8);
  cl_assert_equal_i(memcmp(copy_out, "234567AB", 8), 0);

  contiguous_num_bytes_left = circular_buffer_write_prepare(&buffer, &data_out);
  cl_assert_equal_i(contiguous_num_bytes_left, 0);
  cl_assert_equal_p(data_out, NULL);
}

void test_circular_buffer__read_or_copy_returns_false_when_length_is_too_long(void) {
  CircularBuffer buffer;
  uint8_t storage[1];
  circular_buffer_init(&buffer, storage, sizeof(storage));
  uint8_t *data_out = NULL;
  bool caller_should_free = false;
  cl_assert_equal_b(false, circular_buffer_read_or_copy(&buffer, &data_out, sizeof(storage) + 1,
                                                        malloc, &caller_should_free));
}

void test_circular_buffer__read_or_copy_doesnt_copy_when_already_contiguously_stored(void) {
  CircularBuffer buffer;
  uint8_t storage[8];
  circular_buffer_init(&buffer, storage, sizeof(storage));
  circular_buffer_write(&buffer, (uint8_t *)"01234567", sizeof(storage));
  uint8_t *data_out = NULL;
  bool caller_should_free = true;
  cl_assert_equal_b(true, circular_buffer_read_or_copy(&buffer, &data_out, sizeof(storage),
                                                       malloc, &caller_should_free));
  cl_assert_equal_b(false, caller_should_free);
  cl_assert_equal_p(data_out, storage);
}

static void *prv_oom_malloc(size_t length) {
  return NULL;
}

void test_circular_buffer__read_or_copy_does_copy_when_not_contiguously_stored(void) {
  CircularBuffer buffer;
  uint8_t storage[8];
  circular_buffer_init(&buffer, storage, sizeof(storage));
  circular_buffer_write(&buffer, (uint8_t *)"01234567", sizeof(storage));
  circular_buffer_consume(&buffer, 1);
  circular_buffer_write(&buffer, (uint8_t *)"8", 1);

  uint8_t *data_out = NULL;
  bool caller_should_free = false;
  cl_assert_equal_b(true, circular_buffer_read_or_copy(&buffer, &data_out, sizeof(storage),
                                                       malloc, &caller_should_free));
  cl_assert_equal_b(true, caller_should_free);
  cl_assert_equal_m(data_out, "12345678", sizeof(storage));
  free(data_out);

  // Test OOM scenario:
  cl_assert_equal_b(false, circular_buffer_read_or_copy(&buffer, &data_out, sizeof(storage),
                                                        prv_oom_malloc, &caller_should_free));
  cl_assert_equal_p(data_out, NULL);
  cl_assert_equal_b(false, caller_should_free);
}

void test_circular_buffer__read_while_write_pending(void) {
  CircularBuffer buffer;
  uint8_t storage[8];
  circular_buffer_init(&buffer, storage, sizeof(storage));

  uint8_t letterA = 'A';
  cl_assert(circular_buffer_write(&buffer, &letterA, sizeof(letterA)));

  uint8_t *data_buf;
  uint16_t num_bytes = circular_buffer_write_prepare(&buffer, &data_buf);
  cl_assert_equal_i(sizeof(storage) - sizeof(letterA), num_bytes);

  uint8_t letterB = 'B';
  data_buf[0] = letterB;

  cl_assert(circular_buffer_read(
      &buffer, sizeof(letterA), (const uint8_t **)&data_buf, &num_bytes));
  cl_assert_equal_i(num_bytes, sizeof(letterA));
  cl_assert_equal_m(data_buf, &letterA, sizeof(letterA));

  cl_assert(circular_buffer_consume(&buffer, sizeof(letterA)));

  circular_buffer_write_finish(&buffer, sizeof(letterB));

  cl_assert(circular_buffer_read(
      &buffer, sizeof(letterB), (const uint8_t **)&data_buf, &num_bytes));
  cl_assert_equal_i(num_bytes, sizeof(letterB));
  cl_assert_equal_m(data_buf, &letterB, sizeof(letterB));
}
