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

#include "comm/ble/kernel_le_client/ams/ams_util.h"

#include "clar.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_logging.h"

// Helpers
///////////////////////////////////////////////////////////

static char s_results[10][32];
static uint32_t s_results_lengths[10];
static uint8_t s_results_count;
static uint32_t s_max_results_count;

static bool prv_result_callback(const char *value, uint32_t value_length,
                                uint32_t index, void *context) {
  strcpy(s_results[s_results_count], value);
  s_results_lengths[s_results_count] = value_length;
  cl_assert_equal_i(index, s_results_count);
  ++s_results_count;

  return (s_max_results_count > s_results_count);
}

// Tests
///////////////////////////////////////////////////////////

void test_ams_util__initialize(void) {
  memset(s_results, 0, sizeof(s_results));
  s_results_count = 0;
  s_max_results_count = ~0;
}

void test_ams_util__cleanup(void) {
}

// ams_util_float_string_parse() tests
///////////////////////////////////////////////////////////

#define assert_float_parse(in, mul, succeeds, expected_result) \
  { \
    const char *input = (const char *)in; \
    const uint32_t input_length = input ? sizeof(in) : 0; \
    const uint32_t multiplier = mul; \
    int32_t result = 0; \
    cl_assert_equal_b(succeeds, \
          ams_util_float_string_parse((const char *)in, input_length, multiplier, &result)); \
    if (succeeds) { \
      cl_assert_equal_i(result, expected_result); \
    } \
  }

void test_ams_util__float_string_parse_negative_number(void) {
  // "-1" * 3
  assert_float_parse("-1", 3, true, -1 * 3);
}

void test_ams_util__float_string_parse_only_minus_sign(void) {
  // "-" * 3
  assert_float_parse("-", 3, false, 0);
}

void test_ams_util__float_string_parse_negative_number_nothing_before_separator(void) {
  // "-.1" * 30
  assert_float_parse("-.1", 30, true, -3);
}

void test_ams_util__float_string_parse_multiple_minusses(void) {
  // "--.1" * 30
  assert_float_parse("--.1", 30, false, 0);
}

void test_ams_util__float_string_parse_null(void) {
  // NULL * 3
  assert_float_parse(NULL, 3, false, 0);
}

void test_ams_util__float_string_parse_not_zero_terminated(void) {
  uint8_t buffer[] = {'1'};
  assert_float_parse(buffer, 3, true, 3);
}

void test_ams_util__float_string_parse_null_in_the_middle(void) {
  uint8_t buffer[] = {'1', 0, '2'};
  assert_float_parse(buffer, 3, true, 3);
}

void test_ams_util__float_string_parse_empty_string(void) {
  // "" * 3
  assert_float_parse("", 3, false, 0);
}

void test_ams_util__float_string_parse_not_a_number(void) {
  // "hello" * 3
  assert_float_parse("hello", 3, false, 0);
  // " " * 3
  assert_float_parse(" ", 3, false, 0);
}

void test_ams_util__float_string_parse_no_fraction(void) {
  // "42" * 3
  assert_float_parse("42", 3, true, 42 * 3);
}

void test_ams_util__float_string_parse_separator_but_no_fraction(void) {
  // "21." * 3
  assert_float_parse("21.", 3, true, 21 * 3);
}

void test_ams_util__float_string_parse_comma_decimal_separator(void) {
  // "1.23456" * 3 is approx 3.7036800000000003, round up to 4
  assert_float_parse("1,23456", 3, true, 4);
}

void test_ams_util__float_string_parse_period_decimal_separator(void) {
  // "1.23456" * 3 is approx 3.7036800000000003, round up to 4
  assert_float_parse("1.23456", 3, true, 4);
}

void test_ams_util__float_string_parse_round_down(void) {
  // "0.74995" * 2 is approx 1.4999, round down to 1
  assert_float_parse("0.74995", 2, true, 1);
}

void test_ams_util__float_string_parse_multiple_separators(void) {
  // "1.2.2" * 3
  assert_float_parse("1.2.2", 2, false, 0);
}

void test_ams_util__float_string_positive_multiplier(void) {
  // "1.654321" * 33 is approx 54.592593, round up to 55
  assert_float_parse("1.654321", 33, true, 55);
}

void test_ams_util__float_string_negative_multiplier(void) {
  // "1.987622" * -33 is approx -65.591526, round to -66
  assert_float_parse("1.987622", -33, true, -66);
}

void test_ams_util__float_string_parse_overflow_positive(void) {
  // fails:
  // "2147483648" * 1
  assert_float_parse("2147483648", 1, false, 0);

  // succeeds:
  // "2147483647" * 1
  assert_float_parse("2147483647", 1, true, 2147483647);
}

void test_ams_util__float_string_parse_overflow_negative(void) {
  // fails:
  // "-2147483649" * 1
  assert_float_parse("-2147483649", 1, false, 0);

  // succeeds:
  // "-2147483648" * 1
  assert_float_parse("-2147483648", 1, true, -2147483648);
  assert_float_parse("2147483648", -1, true, -2147483648);
}

// ams_util_csv_parse() tests
///////////////////////////////////////////////////////////

#define assert_result(idx, val, len); \
  cl_assert_equal_i(len, s_results_lengths[idx]); \
  cl_assert_equal_i(memcmp(val, s_results[idx], len), 0);

void test_ams_util__csv_empty_string(void) {
  const char empty_string[] = "";
  const uint8_t count = ams_util_csv_parse(empty_string, sizeof(empty_string), NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 1);
  cl_assert_equal_i(count, 1);
  cl_assert_equal_s(empty_string, s_results[0]);
}

void test_ams_util__csv_empty_values(void) {
  const char one_value[] = ",";
  const uint8_t count = ams_util_csv_parse(one_value, sizeof(one_value), NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 2);
  cl_assert_equal_i(count, 2);
  assert_result(0, "", 0);
  assert_result(1, "", 0);
}

void test_ams_util__csv_null(void) {
  const uint8_t count = ams_util_csv_parse(NULL, 0, NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 0);
  cl_assert_equal_i(count, 0);
}

void test_ams_util__csv_one_value(void) {
  const char one_value[] = "A";
  const uint8_t count = ams_util_csv_parse(one_value, sizeof(one_value), NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 1);
  cl_assert_equal_i(count, 1);
  cl_assert_equal_s(one_value, s_results[0]);
}

void test_ams_util__csv_multiple_values(void) {
  const char multi_values[] = "A,B,C";
  const uint8_t count = ams_util_csv_parse(multi_values, sizeof(multi_values), NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 3);
  cl_assert_equal_i(count, 3);
  assert_result(0, "A", 1);
  assert_result(1, "B", 1);
  assert_result(2, "C", 1);
}

void test_ams_util__csv_stop_after_one_value(void) {
  const char multi_values[] = "A,B,C";
  s_max_results_count = 1;
  const uint8_t count = ams_util_csv_parse(multi_values, sizeof(multi_values), NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 1);
  cl_assert_equal_i(count, 1);
  assert_result(0, "A", 1);
}

void test_ams_util__csv_null_in_the_middle(void) {
  const char null_middle_value[] = "A\x00 BCD,1234";
  cl_assert(sizeof(null_middle_value) > 2);
  const uint8_t count = ams_util_csv_parse(null_middle_value, sizeof(null_middle_value), NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 1);
  cl_assert_equal_i(count, 1);
  assert_result(0, "A", 1);
}

void test_ams_util__csv_buffer_not_zero_terminated(void) {
  const char one_value[] = "ABCDEF";
  cl_assert(sizeof(one_value) > 2);
  const uint8_t count = ams_util_csv_parse(one_value,
                                           sizeof(one_value) - 1 /* omit zero terminator */, NULL,
                                           prv_result_callback);
  cl_assert_equal_i(s_results_count, 1);
  cl_assert_equal_i(count, 1);
  assert_result(0, "ABCDEF", 6);
}

