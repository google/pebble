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

#include "util/ihex.h"

#include <string.h>


static uint8_t s_result[300];


void test_ihex__initialize(void) {
  // Set the result buffer to a known value so that it can be checked
  // later to see if the code under test wrote more than it should have.
  memset(s_result, 0x20, sizeof(s_result));
}

static void prv_assert_ihex(const char *expected) {
  int len = strlen(expected);
  // Check that bytes aren't touched past the end of the record.
  for (int i=len; i < sizeof(s_result); ++i) {
    cl_assert_equal_i(0x20, s_result[i]);
  }
  // NULL-terminate the result so that it can be compared as a string.
  s_result[len] = '\0';
  cl_assert_equal_s(expected, (char *)s_result);
}

void test_ihex__eof_record(void) {
  ihex_encode(s_result, IHEX_TYPE_EOF, 0, NULL, 0);
  prv_assert_ihex(":00000001FF");
}

void test_ihex__data_record(void) {
  uint8_t data[7] = { 1, 2, 3, 4, 5, 6, 7 };
  ihex_encode(s_result, IHEX_TYPE_DATA, 0xABCD, data, sizeof(data));
  prv_assert_ihex(":07ABCD000102030405060765");
}

void test_ihex__empty_record_length(void) {
  cl_assert_equal_i(11, IHEX_RECORD_LENGTH(0));
}

void test_ihex__record_length(void) {
  cl_assert_equal_i(15, IHEX_RECORD_LENGTH(2));
}
