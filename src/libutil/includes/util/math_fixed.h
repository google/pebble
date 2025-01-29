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

#include <inttypes.h>
#include <stdbool.h>

////////////////////////////////////////////////////////////////
/// Fixed_S16_3 = 1 bit sign, 12 bits integer, 3 bits fraction
////////////////////////////////////////////////////////////////
// Note the fraction is unsigned and represents a positive addition
// to the integer. So for example:
// The value -1.125 will be stored as (-2 + 7*0.125) ==> integer = -2, fraction = 7
// The value 1.125 will be stored as (1 + 1*0.125) ==> integer = 1, fraction = 1
// This representation allows for direct addition/multiplication between numbers to happen
// without any complicated logic.
// The same representation for negative numbers applies for all fixed point representations
// in this file (i.e. fraction component is a positive addition to the integer).
typedef union __attribute__ ((__packed__)) Fixed_S16_3 {
  int16_t raw_value;
  struct {
    uint16_t fraction:3;
    int16_t integer:13;
  };
} Fixed_S16_3;

#define Fixed_S16_3(raw) ((Fixed_S16_3){ .raw_value = (raw) })
#define FIXED_S16_3_PRECISION 3
#define FIXED_S16_3_FACTOR (1 << FIXED_S16_3_PRECISION)

#define FIXED_S16_3_ZERO ((Fixed_S16_3){ .integer = 0, .fraction = 0 })
#define FIXED_S16_3_ONE ((Fixed_S16_3){ .integer = 1, .fraction = 0 })
#define FIXED_S16_3_HALF ((Fixed_S16_3){ .raw_value = FIXED_S16_3_ONE.raw_value / 2 })

static __inline__ Fixed_S16_3 Fixed_S16_3_mul(Fixed_S16_3 a, Fixed_S16_3 b) {
  return Fixed_S16_3(((int32_t) a.raw_value * b.raw_value) >> FIXED_S16_3_PRECISION);
}

static __inline__ Fixed_S16_3 Fixed_S16_3_add(Fixed_S16_3 a, Fixed_S16_3 b) {
  return Fixed_S16_3(a.raw_value + b.raw_value);
}

static __inline__ Fixed_S16_3 Fixed_S16_3_sub(Fixed_S16_3 a, Fixed_S16_3 b) {
  return Fixed_S16_3(a.raw_value - b.raw_value);
}

static __inline__ Fixed_S16_3 Fixed_S16_3_add3(Fixed_S16_3 a, Fixed_S16_3 b, Fixed_S16_3 c) {
  return Fixed_S16_3(a.raw_value + b.raw_value + c.raw_value);
}

static __inline__ bool Fixed_S16_3_equal(Fixed_S16_3 a, Fixed_S16_3 b) {
  return (a.raw_value == b.raw_value);
}

static __inline__ int16_t Fixed_S16_3_rounded_int(Fixed_S16_3 a) {
  const int16_t delta = a.raw_value >= 0 ? FIXED_S16_3_HALF.raw_value : -FIXED_S16_3_HALF.raw_value;
  return (a.raw_value + delta) / FIXED_S16_3_FACTOR;
}

////////////////////////////////////////////////////////////////
/// Fixed_S32_16 = 1 bit sign, 15 bits integer, 16 bits fraction
////////////////////////////////////////////////////////////////
typedef union __attribute__ ((__packed__)) Fixed_S32_16 {
  int32_t raw_value;
  struct {
    uint16_t fraction:16;
    int16_t integer:16;
  };
} Fixed_S32_16;

//! Work-around for function pointer return type Fixed_S32_16 to avoid
//! tripping the pre-processor to use the equally named Fixed_S32_16 define
typedef Fixed_S32_16 Fixed_S32_16Return;

#define Fixed_S32_16(raw) ((Fixed_S32_16){ .raw_value = (raw) })
#define FIXED_S32_16_PRECISION 16

#define FIXED_S32_16_ONE ((Fixed_S32_16){ .integer = 1, .fraction = 0 })
#define FIXED_S32_16_ZERO ((Fixed_S32_16){ .integer = 0, .fraction = 0 })

static __inline__ Fixed_S32_16 Fixed_S32_16_mul(Fixed_S32_16 a, Fixed_S32_16 b) {
  Fixed_S32_16 x;

  x.raw_value = (int32_t)((((int64_t) a.raw_value * (int64_t) b.raw_value)) >>
                FIXED_S32_16_PRECISION);
  return x;
}

static __inline__ Fixed_S32_16 Fixed_S32_16_add(Fixed_S32_16 a, Fixed_S32_16 b) {
  return Fixed_S32_16(a.raw_value + b.raw_value);
}

static __inline__ Fixed_S32_16 Fixed_S32_16_add3(Fixed_S32_16 a, Fixed_S32_16 b, Fixed_S32_16 c) {
  return Fixed_S32_16(a.raw_value + b.raw_value + c.raw_value);
}

static __inline__ Fixed_S32_16 Fixed_S32_16_sub(Fixed_S32_16 a, Fixed_S32_16 b) {
  return Fixed_S32_16(a.raw_value - b.raw_value);
}


////////////////////////////////////////////////////////////////
/// Fixed_S64_32 = 1 bit sign, 31 bits integer, 32 bits fraction
////////////////////////////////////////////////////////////////
typedef union __attribute__ ((__packed__)) Fixed_S64_32 {
  int64_t raw_value;
  struct {
    uint32_t fraction:32;
    int32_t integer:32;
  };
} Fixed_S64_32;

#define FIXED_S64_32_PRECISION 32

#define FIXED_S64_32_ONE ((Fixed_S64_32){ .integer = 1, .fraction = 0 })
#define FIXED_S64_32_ZERO ((Fixed_S64_32){ .integer = 0, .fraction = 0 })

#define FIXED_S64_32_FROM_RAW(raw) ((Fixed_S64_32){ .raw_value = (raw) })
#define FIXED_S64_32_FROM_INT(x) ((Fixed_S64_32){ .integer = x, .fraction = 0 })
#define FIXED_S64_32_TO_INT(x) (x.integer)

static __inline__ Fixed_S64_32 Fixed_S64_32_mul(Fixed_S64_32 a, Fixed_S64_32 b) {
  Fixed_S64_32 result;
  result.raw_value = (((uint64_t)(a.integer * b.integer)) << 32)
                   + ((((uint64_t)a.fraction) * ((uint64_t)b.fraction)) >> 32)
                   + ((a.integer) * ((uint64_t)b.fraction))
                   + (((uint64_t)a.fraction) * (b.integer));
  return result;
}

static __inline__ Fixed_S64_32 Fixed_S64_32_add(Fixed_S64_32 a, Fixed_S64_32 b) {
  return FIXED_S64_32_FROM_RAW(a.raw_value + b.raw_value);
}

static __inline__ Fixed_S64_32 Fixed_S64_32_add3(Fixed_S64_32 a, Fixed_S64_32 b, Fixed_S64_32 c) {
  return FIXED_S64_32_FROM_RAW(a.raw_value + b.raw_value + c.raw_value);
}

static __inline__ Fixed_S64_32 Fixed_S64_32_sub(Fixed_S64_32 a, Fixed_S64_32 b) {
  return FIXED_S64_32_FROM_RAW(a.raw_value - b.raw_value);
}


////////////////////////////////////////////////////////////////
/// Mixed operations
////////////////////////////////////////////////////////////////
// This function multiplies a Fixed_S16_3 and Fixed_S32_16 and returns result in Fixed_S16_3 format
static __inline__ Fixed_S16_3 Fixed_S16_3_S32_16_mul(Fixed_S16_3 a, Fixed_S32_16 b) {
  return Fixed_S16_3( a.raw_value * b.raw_value >> FIXED_S32_16_PRECISION );
}


////////////////////////////////////////////////////////////////
/// High level math functions and filters
////////////////////////////////////////////////////////////////

//! Run x through a linear recursive filter. See https://en.wikipedia.org/wiki/Digital_biquad_filter
//! for example of a 2nd order recursive filter. This function implements a generic Nth order one.
//! @param[in] x the next input value, x[n]
//! @param[in] num_input_coefficients the number of taps on the input side
//! @param[in] num_output_coefficients the number of taps on the output side
//! @param[in] cb pointer to array of input side coefficients. Must be an array of size
//!   num_input_coefficients
//! @param[in] ca pointer to array of output side coefficients. Must be an array of size
//!   num_output_coefficients
//! @param[in|out] state_x pointer to array to hold the history of x. Must be an array
//!   of size num_input_coefficients
//! @param[in|out] state_y pointer to array to hold the history of y. Must be an array
//!   of size num_output_coefficients
//! @return the filtered output value, y[n]
Fixed_S64_32 math_fixed_recursive_filter(Fixed_S64_32 x,
                                         int num_input_coefficients, int num_output_coefficients,
                                         const Fixed_S64_32 *cb, const Fixed_S64_32 *ca,
                                         Fixed_S64_32 *state_x, Fixed_S64_32 *state_y);
