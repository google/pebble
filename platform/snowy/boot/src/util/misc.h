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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define ABS(a) (((a) > 0) ? (a) : -1 * (a))
#define CLIP(n, min, max) ((n)<(min)?(min):((n)>(max)?(max):(n)))

#define MHZ_TO_HZ(hz) (((uint32_t)(hz)) * 1000000)

#define KiBYTES(k) ((k) * 1024) // Bytes to Kibibytes
#define MiBYTES(m) ((m) * 1024 * 1024) // Bytes to Mebibytes
#define EiBYTES(e) ((e) * 1024 * 1024 * 1024 * 1024 * 1024 * 1024) // Bytes to Exbibytes

// Stolen from http://stackoverflow.com/a/8488201
#define GET_FILE_NAME(file) (strrchr(file, '/') ? (strrchr(file, '/') + 1) : (file))

#define ARRAY_LENGTH(array) (sizeof((array))/sizeof((array)[0]))

#define MEMBER_SIZE(type, member) sizeof(((type *)0)->member)

static inline void swap16(int16_t *a, int16_t *b) {
  int16_t t = *a;
  *a = *b;
  *b = t;
}

int32_t sign_extend(uint32_t a, int bits);

//! Calculates the distance (end - start), taking a roll-over into account as good as it can get.
int32_t serial_distance32(uint32_t start, uint32_t end);

//! Calculates the distance (end - start), taking a roll-over into account as good as it can get.
//! @param bits the number of bits that are valid in start and end.
int32_t serial_distance(uint32_t start, uint32_t end, int bits);

void itoa(uint32_t num, char *buffer, int buffer_length);

/*
 * find the log base two of a number rounded up
 */
int ceil_log_two(uint32_t n);

//! Count the number of bits that are set to 1 in a multi-byte bitset.
//! @param bitset_bytes The bytes of the bitset
//! @param num_bits The width of the bitset
//! @note this function zeroes out any bits in the last byte if there
//! are more bits than num_bits.
uint8_t count_bits_set(uint8_t *bitset_bytes, int num_bits);

uintptr_t str_to_address(const char *address_str);

uint32_t hash(const uint8_t *bytes, const uint32_t length);

const char *bool_to_str(bool b);

//! @param hex 12-digit hex string representing a BT address
//! @param addr Points to a SS1 BD_ADDR_t as defined in BTBTypes.h
//! @return True on success
bool convert_bt_addr_hex_str_to_bd_addr(const char *hex_str, uint8_t *bd_addr, const unsigned int bd_addr_size);

/**
 * Compute the next backoff interval using a bounded binary exponential backoff formula.
 *
 * @param[in,out] attempt The number of retries performed so far. This count will be incremented by
 *   the function.
 * @param[in] initial_value The inital backoff interval. Subsequent backoff attempts will be this
 *   number multiplied by a power of 2.
 * @param[in] max_value The maximum backoff interval that returned by the function.
 * @return The next backoff interval.
 */
uint32_t next_exponential_backoff(uint32_t *attempt, uint32_t initial_value, uint32_t max_value);

/*
 * The -Wtype-limits flag generated an error with the previous IS_SIGNED maco.
 * If an unsigned number was passed in the macro would check if the unsigned number was less than 0.
 */
#define IS_SIGNED(var) (__builtin_choose_expr( \
  __builtin_types_compatible_p(__typeof__(var), unsigned char), false, \
  __builtin_choose_expr( \
  __builtin_types_compatible_p(__typeof__(var), unsigned short), false, \
  __builtin_choose_expr( \
  __builtin_types_compatible_p(__typeof__(var), unsigned int), false, \
  __builtin_choose_expr( \
  __builtin_types_compatible_p(__typeof__(var), unsigned long), false, \
  __builtin_choose_expr( \
  __builtin_types_compatible_p(__typeof__(var), unsigned long long), false, true))))) \
)
