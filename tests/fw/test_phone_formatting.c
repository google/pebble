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

#include "popups/phone_formatting.h"

#include "clar.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Stubs
///////////////////////////////////////////////////////////
#include "stubs_logging.h"
#include "stubs_passert.h"


// Tests
///////////////////////////////////////////////////////////

#define NAME_LENGTH 32
#define E_ACUTE "\xc3\x89"

static const char GUARD_CHAR = 'F';
static const char *GUARD_REFERENCE = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";

void test_phone_formatting__name_single(void) {
  char dest[NAME_LENGTH];
  phone_format_caller_name("Katharine", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Katharine");
}

void test_phone_formatting__name_multiple(void) {
  char dest[NAME_LENGTH];
  phone_format_caller_name("Katharine Claire Berry", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Katharine C. B.");
}

void test_phone_formatting__name_double_space(void) {
  char dest[NAME_LENGTH];
  phone_format_caller_name("Katharine  Berry", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Katharine B.");
}

void test_phone_formatting__name_trailing_space(void) {
  char dest[NAME_LENGTH];
  phone_format_caller_name("Katharine Berry ", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Katharine B.");

  memset(dest, 0, NAME_LENGTH);
  phone_format_caller_name("Katharine Berry  ", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Katharine B.");

}

void test_phone_formatting__single_name_trailing_space(void) {
  char dest[NAME_LENGTH];
  phone_format_caller_name("Katharine ", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Katharine");

  memset(dest, 0, NAME_LENGTH);
  phone_format_caller_name("Katharine  ", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Katharine");
}

void test_phone_formatting__multibyte_initial(void) {
  char dest[NAME_LENGTH];

  phone_format_caller_name("Donut " E_ACUTE "clair", dest, NAME_LENGTH);

  cl_assert_equal_s(dest, "Donut " E_ACUTE ".");
}

void test_phone_formatting__overflowing_single_name(void) {
  const size_t buffer_length = 10;
  char dest[NAME_LENGTH];
  memset(dest, GUARD_CHAR, NAME_LENGTH);

  phone_format_caller_name("Pankajavalli", dest, buffer_length);

  cl_assert_equal_s(dest, "Pankajava");
  cl_assert_equal_m(dest + buffer_length, GUARD_REFERENCE, NAME_LENGTH - buffer_length);
}

void test_phone_formatting__overflowing_first_name(void) {
  const size_t buffer_length = 10;
  char dest[NAME_LENGTH];
  memset(dest, GUARD_CHAR, NAME_LENGTH);

  phone_format_caller_name("Pankajavalli Balamurugan", dest, buffer_length);

  cl_assert_equal_s(dest, "Pankajava");
  cl_assert_equal_m(dest + buffer_length, GUARD_REFERENCE, NAME_LENGTH - buffer_length);
}

void test_phone_formatting__overflowing_space(void) {
  const size_t buffer_length = 10;
  char dest[NAME_LENGTH];
  memset(dest, GUARD_CHAR, NAME_LENGTH);

  phone_format_caller_name("Katharine Berry", dest, buffer_length);

  cl_assert_equal_s(dest, "Katharine");
  cl_assert_equal_m(dest + buffer_length, GUARD_REFERENCE, buffer_length);
}

void test_phone_formatting__overflowing_initial(void) {
  const size_t buffer_length = 12;
  char dest[NAME_LENGTH];
  memset(dest, GUARD_CHAR, NAME_LENGTH);

  phone_format_caller_name("Katharine Berry", dest, buffer_length);

  cl_assert_equal_s(dest, "Katharine");
  cl_assert_equal_m(dest + buffer_length, GUARD_REFERENCE, buffer_length);
}

void test_phone_formatting__overflowing_multibyte_initial(void) {
  const size_t buffer_length = 9;
  char dest[NAME_LENGTH];
  memset(dest, GUARD_CHAR, NAME_LENGTH);

  // This wouldn't overflow if E_ACUTE was one byte.
  phone_format_caller_name("Donut " E_ACUTE "clair", dest, buffer_length);

  cl_assert_equal_s(dest, "Donut");
  cl_assert_equal_m(dest + buffer_length, GUARD_REFERENCE, NAME_LENGTH - buffer_length);
}

void test_phone_formatting__phone_number_intl_std(void) {
  char test_number[] = "+55 408-555-1212";
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "+55 408\n555-1212");
}

void test_phone_formatting__phone_number_intl_parens(void) {
  char test_number[] = "+55 (408) 555-1212";
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "+55 (408)\n555-1212");
}

void test_phone_formatting__phone_number_long_distance_parens(void) {
  char test_number[] = "(608) 555-1212";  // typical format on android
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "(608)\n555-1212");
}

void test_phone_formatting__phone_number_long_distance_parens_plus(void) {
  char test_number[] = "+1 (608) 555-1212";  // typical format on iOS
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "+1 (608)\n555-1212");
}

void test_phone_formatting__phone_number_long_distance_parens_plus_leading_ltor_ancs(void) {
  char test_number[] = "+1 (608) 555-1212";  // typical format on iOS
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "+1 (608)\n555-1212");
}

void test_phone_formatting__phone_number_long_distance_uk(void) {
  char test_number[] = "12345-123456";
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "12345\n123456");
}

void test_phone_formatting__phone_number_intl_germany(void) {
  char test_number[] = "+49 030 90 26 0";  // Berlin, Rotes Rathaus
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "+49 030\n90 26 0");
}

void test_phone_formatting__phone_number_std_germany(void) {
  char test_number[] = "030 90 26 0";  // Berlin, Rotes Rathaus
  int dest_len = sizeof(test_number) + 1;
  char *dest = malloc(dest_len);  // malloc'd memory is protected by DUMA

  phone_format_phone_number(test_number, dest, dest_len);

  cl_assert_equal_s(dest, "030 90 26 0");
}

