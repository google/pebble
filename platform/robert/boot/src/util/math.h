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

#include <stdint.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define ABS(a) (((a) > 0) ? (a) : -1 * (a))
#define CLIP(n, min, max) ((n) < (min) ? (min) : ((n) > (max) ? (max) : (n)))
#define ROUND(num, denom) (((num) + ((denom) / 2))/(denom))
#define WITHIN(n, min, max) ((n) >= (min) && (n) <= (max))
#define RANGE_WITHIN(n_min, n_max, min, max) ((n_min) >= (min) && (n_max) <= (max))

// Divide num by denom, rounding up (ceil(0.5) is 1.0, and ceil(-0.5) is 0.0)
// ex. 3, 4 (ie. 3/4) : returns 1
// ex. -3, 4 : returns 0
#define DIVIDE_CEIL(num, denom) ((num + (denom - 1)) / denom)

// Round value up to the next increment of modulus
// ex. val = 152 mod = 32 : returns 160
// val = -32 mod = 90 : returns -90
#define ROUND_TO_MOD_CEIL(val, mod) \
  ((val >= 0) ? \
  ((((val) + ABS(ABS(mod) - 1)) / ABS(mod)) * ABS(mod)) : \
  -((((-val) + ABS(ABS(mod) - 1)) / ABS(mod)) * ABS(mod)))

/*
 * find the log base two of a number rounded up
 */
int ceil_log_two(uint32_t n);

/*
 * The -Wtype-limits flag generated an error with the previous IS_SIGNED maco.
 * If an unsigned number was passed in the macro would check if the unsigned number was less than 0.
 */
//! Determine whether a variable is signed or not.
//! @param var The variable to evaluate.
//! @return true if the variable is signed.
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

/**
 * Compute the next backoff interval using a bounded binary exponential backoff formula.
 *
 * @param[in,out] attempt The number of retries performed so far. This count will be incremented
 * by the function.
 * @param[in] initial_value The inital backoff interval. Subsequent backoff attempts will be this
 * number multiplied by a power of 2.
 * @param[in] max_value The maximum backoff interval that returned by the function.
 * @return The next backoff interval.
 */
uint32_t next_exponential_backoff(uint32_t *attempt, uint32_t initial_value, uint32_t max_value);

//! Find the greatest common divisor of two numbers.
uint32_t gcd(uint32_t a, uint32_t b);
