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

#include "util/iterator.h"
#include "utf8_test_data.h"
#include "applib/graphics/utf8.h"

#include "clar.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Stubs
///////////////////////////////////////////////////////////
#include "stubs_logging.h"
#include "stubs_passert.h"

///////////////////////////////////////////////////////////
// Tests

void test_utf8_iterator__decode_test_string_empty(void) {
  bool success = false;
  utf8_get_bounds(&success, "");
  cl_assert(success);
}

//! Decode ASCII char
void test_utf8_iterator__decode_test_single_codepoint_string_single_byte(void) {
  // Mutable types
  Iterator utf8_iter;
  Utf8IterState utf8_iter_state;

  // Immutable types
  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, "A");
  cl_assert(utf8_bounds.end - utf8_bounds.start == 1);
  cl_assert(success);

  // Init mutable types
  utf8_iter_init(&utf8_iter, &utf8_iter_state, &utf8_bounds, utf8_bounds.start);

  // Tests
  cl_assert(!iter_next(&utf8_iter));
  cl_assert(!iter_next(&utf8_iter));
  cl_assert(!iter_next(&utf8_iter));
}

//! Decode multi-byte char
void test_utf8_iterator__decode_test_single_codepoint_string_multi_byte(void) {
  // Mutable types
  Iterator utf8_iter;
  Utf8IterState utf8_iter_state;

  // Immutable types
  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, "\xc3\xb0");
  cl_assert(utf8_bounds.end - utf8_bounds.start == 2);
  cl_assert(success);

  // Init mutable types
  utf8_iter_init(&utf8_iter, &utf8_iter_state, &utf8_bounds, utf8_bounds.start);

  // Tests
  cl_assert(!iter_next(&utf8_iter));
  cl_assert(!iter_next(&utf8_iter));
  cl_assert(!iter_next(&utf8_iter));
}

void test_utf8_iterator__decode_valid_string(void) {
  // Mutable types
  Iterator utf8_iter;
  Utf8IterState utf8_iter_state;

  // Immutable types
  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, s_valid_test_string);
  cl_assert(success);

  const int NUM_VALID_CODEPOINTS = sizeof(s_valid_test_codepoints) / sizeof(uint32_t);

  // Init mutable types
  utf8_iter_init(&utf8_iter, &utf8_iter_state, &utf8_bounds, utf8_bounds.start);

  // Tests
  int i = 0;
  do {
    cl_assert(utf8_iter_state.current < utf8_bounds.end);
    cl_assert(i < NUM_VALID_CODEPOINTS);

    uint32_t decoded_codepoint = utf8_iter_state.codepoint;
    uint32_t actual_codepoint = s_valid_test_codepoints[i];

    cl_assert_equal_i(decoded_codepoint, actual_codepoint);

    ++i;
  } while (iter_next(&utf8_iter));
  cl_assert(!iter_next(&utf8_iter));

  cl_assert(i == NUM_VALID_CODEPOINTS);
  cl_assert(utf8_iter_state.current == utf8_bounds.end);
  cl_assert(*utf8_iter_state.current == '\0');
  cl_assert(utf8_iter_state.codepoint == 0);
}

void test_utf8_iterator__decode_valid_string_backwards(void) {
  // Mutable types
  Iterator utf8_iter;
  Utf8IterState utf8_iter_state;

  // Immutable types
  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, s_valid_test_string);
  cl_assert(success);

  const int NUM_VALID_CODEPOINTS = sizeof(s_valid_test_codepoints) / sizeof(uint32_t);

  // Init mutable types
  utf8_iter_init(&utf8_iter, &utf8_iter_state, &utf8_bounds, utf8_bounds.end);

  // Tests
  int i = sizeof(s_valid_test_codepoints) / sizeof(s_valid_test_codepoints[0]);
  while (iter_prev(&utf8_iter)) {
    cl_assert(utf8_iter_state.current >= utf8_bounds.start);
    cl_assert(i > 0);

    uint32_t decoded_codepoint = utf8_iter_state.codepoint;
    uint32_t actual_codepoint = s_valid_test_codepoints[i - 1];

    cl_assert_equal_i(decoded_codepoint, actual_codepoint);
    --i;
  };

  cl_assert(i == 0);
  cl_assert(utf8_iter_state.current == utf8_bounds.start);
  cl_assert(utf8_iter_state.codepoint == 0);
}

static void *s_context;

static int s_each_count = 0;

static bool prv_each_codepoint(int index, Codepoint codepoint, void *context) {
  static int s_index = 0;
  static Codepoint s_codes[] = { 0xf0, 'a' };
  cl_assert_equal_i(s_codes[index], codepoint);
  cl_assert_equal_i(s_index++, index);
  cl_assert_equal_p(context, &s_context);
  s_each_count++;
  return true;
}

void test_utf8_iterator__each_codepoint(void) {
  void *context = &s_context;
  const char *str = "\xc3\xb0" "a";
  s_each_count = 0;
  cl_assert_equal_b(utf8_each_codepoint(str, prv_each_codepoint, context), true);
  cl_assert_equal_i(s_each_count, 2);
}

static bool prv_each_codepoint_break(int index, Codepoint codepoint, void *context) {
  static int s_index = 0;
  static Codepoint s_codes[] = { 'a', 'b', 'c', 'd', 'e' };
  cl_assert_equal_i(s_index++, index);
  cl_assert_equal_i(s_codes[index], codepoint);
  cl_assert_equal_p(context, &s_context);
  s_each_count++;
  return (index != 2);
}

void test_utf8_iterator__each_codepoint_break(void) {
  void *context = &s_context;
  const char *str = "abcde";
  s_each_count = 0;
  cl_assert_equal_b(utf8_each_codepoint(str, prv_each_codepoint_break, context), true);
  cl_assert_equal_i(s_each_count, 3);
}

void test_utf8_iterator__each_codepoint_invalid(void) {
  void *context = &s_context;
  const char *str = "\xc3\x28";
  s_each_count = 0;
  cl_assert_equal_b(utf8_each_codepoint(str, prv_each_codepoint, context), false);
  cl_assert_equal_i(s_each_count, 0);
}

void test_utf8_iterator__each_codepoint_empty_string(void) {
  void *context = (void *)(uintptr_t)0x42;
  const char *str = "";
  s_each_count = 0;
  cl_assert_equal_b(utf8_each_codepoint(str, prv_each_codepoint, context), true);
  cl_assert_equal_i(s_each_count, 0);
}
