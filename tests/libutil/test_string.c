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

#include "util/string.h"

#include "clar.h"

#include "stubs_logging.h"
#include "stubs_passert.h"

#include <string.h>

void test_string__initialize(void) {
}

void test_string__cleanup(void) {
}

void test_string__strip_leading_whitespace(void) {
  const char *with_whitespace = "   hello, world";
  const char *without_whitespace = "hello, world";
  cl_assert(strcmp(without_whitespace, string_strip_leading_whitespace(with_whitespace)) == 0);

  const char *with_newlines = "\n\n\nbonjour, monde";
  const char *without_newlines = "bonjour, monde";
  cl_assert(strcmp(without_newlines, string_strip_leading_whitespace(with_newlines)) == 0);

  const char *with_both = "\n\n  \n \nalbuquerque is a lovely town, not!\n";
  const char *with_neither = "albuquerque is a lovely town, not!\n";
  cl_assert(strcmp(with_neither, string_strip_leading_whitespace(with_both)) == 0);
}

void test_string__strip_trailing_whitespace(void) {
  char string_out[100];

  const char *with_whitespace = "hello, world   ";
  const char *without_whitespace = "hello, world";
  string_strip_trailing_whitespace(with_whitespace, string_out);
  cl_assert(strcmp(without_whitespace, string_out) == 0);

  const char *with_newlines = "bonjour, monde\n\n\n";
  const char *without_newlines = "bonjour, monde";
  string_strip_trailing_whitespace(with_newlines, string_out);
  cl_assert(strcmp(without_newlines, string_out) == 0);

  const char *with_both = "\n albuquerque is a lovely town, not!\n\n \n \n  ";
  const char *with_neither = "\n albuquerque is a lovely town, not!";
  string_strip_trailing_whitespace(with_both, string_out);
  cl_assert(strcmp(with_neither, string_out) == 0);
}

void test_string__test_concat_str_int(void) {
  char buf[32];

  concat_str_int("app", 1, buf, sizeof(buf));
  cl_assert_equal_s(buf, "app1");

  concat_str_int("app", 255, buf, sizeof(buf));
  cl_assert_equal_s(buf, "app255");

  concat_str_int("res_bank", 1, buf, sizeof(buf));
  cl_assert_equal_s(buf, "res_bank1");

  concat_str_int("res_bank", 255, buf, sizeof(buf));
  cl_assert_equal_s(buf, "res_bank255");
}

void test_string__test_itoa_int(void) {
  char buf[32];

  itoa_int(0, buf, 10);
  cl_assert(0 == strcmp(buf, "0"));

  itoa_int(-0, buf, 10);
  cl_assert(0 == strcmp(buf, "0"));

  itoa_int(1, buf, 10);
  cl_assert(0 == strcmp(buf, "1"));

  itoa_int(-1, buf, 10);
  cl_assert(0 == strcmp(buf, "-1"));

  itoa_int(365, buf, 10);
  cl_assert(0 == strcmp(buf, "365"));

  itoa_int(-365, buf, 10);
  cl_assert(0 == strcmp(buf, "-365"));

  // max int32
  itoa_int(2147483647, buf, 10);
  cl_assert(0 == strcmp(buf, "2147483647"));

  // min int32
  itoa_int(-2147483647, buf, 10);
  cl_assert(0 == strcmp(buf, "-2147483647"));
}

void test_string__test_byte_stream_to_hex_string(void) {
  char result_buf[256]; // arbitrarily large

  const uint8_t byte_stream[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  const char *expected_result_fwd = "00010203040506070809";
  const char *expected_result_bkwd = "09080706050403020100";

  // check that fwd decoding byte streams work
  byte_stream_to_hex_string(&result_buf[0], sizeof(result_buf),
      byte_stream, sizeof(byte_stream), false);
  int res = strcmp(&result_buf[0], expected_result_fwd);
  cl_assert(res == 0);

  // check that bkwd decoding bytes streams work
  byte_stream_to_hex_string(&result_buf[0], sizeof(result_buf),
      byte_stream, sizeof(byte_stream), true);
  res = strcmp(&result_buf[0], expected_result_bkwd);
  cl_assert(res == 0);

  // check that we truncate correctly if result buffer is too small
  // in this case lets make it so there is not enough space for the '\0' byte
  size_t truncated_size = sizeof(byte_stream) * 2;
  memset(result_buf, 0x00, sizeof(result_buf)); // reset buffer
  byte_stream_to_hex_string(&result_buf[0], truncated_size, byte_stream,
      sizeof(byte_stream), false);
  res = memcmp(&result_buf[0], expected_result_fwd, strlen(expected_result_fwd) - 2);
  cl_assert(res == 0 && result_buf[truncated_size - 1] == '\0');
}
