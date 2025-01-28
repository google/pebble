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

#include "util/math_fixed.h"


#include "clar.h"

#include <stdio.h>
#include <string.h>

// Helper Functions
////////////////////////////////////

// Stubs
////////////////////////////////////

// Tests
////////////////////////////////////

////////////////////////////////////////////////////////////////
/// Fixed_S16_3 = 1 bit sign, 12 bits integer, 3 bits fraction
////////////////////////////////////////////////////////////////
void test_math_fixed__S16_3_create(void) {
  Fixed_S16_3 num;
  int16_t test_num;

  cl_assert(FIXED_S16_3_PRECISION == 3);

  int Fixed_S16_3_size = sizeof(Fixed_S16_3);
  cl_assert(Fixed_S16_3_size == sizeof(int16_t));

  test_num = 1 << FIXED_S16_3_PRECISION;
  num = (Fixed_S16_3){ .integer = 1, .fraction = 0 };

  // Test number creation
  test_num = (int16_t)((float)1 * (1 << FIXED_S16_3_PRECISION));
  cl_assert(num.raw_value == test_num);
  cl_assert((FIXED_S16_3_ONE.raw_value == test_num));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);

  num = (Fixed_S16_3){ .raw_value = (int16_t)((float)3.5 * (1 << FIXED_S16_3_PRECISION)) };
  test_num = (int16_t)((float)3.5 * (1 << FIXED_S16_3_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);

  num = (Fixed_S16_3){ .raw_value = (int16_t)((float)-2 * (1 << FIXED_S16_3_PRECISION)) };
  test_num = (int16_t)((float)-2 * (1 << FIXED_S16_3_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);

  num = (Fixed_S16_3){ .raw_value = (int16_t)((float)-3.5 * (1 << FIXED_S16_3_PRECISION)) };
  test_num = (int16_t)((float)-3.5 * (1 << FIXED_S16_3_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);
}

void test_math_fixed__S16_3_fraction(void) {
  Fixed_S16_3 num;
  int16_t test_num;

  // This test shows how the values change across various fraction values

  test_num = (int16_t)((float)-1.125 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -2);
  cl_assert(num.fraction == 7);

  // This confirms that the fixed number is (2^FIXED_S16_3_PRECISION)*(float value)
  test_num = -9;
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);

  test_num = (int16_t)((float)-1 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 0);

  test_num = (int16_t)((float)-0.875 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 1);

  test_num = (int32_t)((float)-0.750 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 2);

  test_num = (int32_t)((float)-0.625 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 3);

  test_num = (int32_t)((float)-0.500 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 4);

  test_num = (int32_t)((float)-0.375 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 5);

  test_num = (int32_t)((float)-0.250 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 6);

  test_num = (int32_t)((float)-0.125 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 7);

  test_num = (int32_t)((float)-0     * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 0);

  test_num = (int32_t)((float)0      * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 0);

  test_num = (int32_t)((float)0.125  * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 1);

  test_num = (int32_t)((float)0.250  * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 2);

  test_num = (int32_t)((float)0.375  * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 3);

  test_num = (int32_t)((float)0.500  * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 4);

  test_num = (int32_t)((float)0.625  * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 5);

  test_num = (int32_t)((float)0.750  * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 6);

  test_num = (int32_t)((float)0.875  * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 7);

  test_num = (int16_t)((float)1 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 1);
  cl_assert(num.fraction == 0);

  test_num = (int16_t)((float)1.125 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 1);
  cl_assert(num.fraction == 1);
}

void test_math_fixed__S16_3_range(void) {
  Fixed_S16_3 num;
  int16_t test_num;

  // This equates to -0.125
  num = (Fixed_S16_3){ .raw_value = 0xFFFF };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 7);
  test_num = (int32_t)((float)-0.125  * (1 << FIXED_S16_3_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);
  num.raw_value++;
  cl_assert(num.integer == 0);
  cl_assert(num.fraction == 0);

  num = (Fixed_S16_3){ .raw_value = 0x8000 };
  cl_assert(num.integer == -4096);
  cl_assert(num.fraction == 0);
  test_num = (int32_t)((float)-4096  * (1 << FIXED_S16_3_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);
  // overflowing negatively from -4096 results in going to 4095.875
  num.raw_value--;
  cl_assert(num.integer == 4095);
  cl_assert(num.fraction == 7);

  num = (Fixed_S16_3){ .raw_value = 0x7FFF };
  cl_assert(num.integer == 4095);
  cl_assert(num.fraction == 7);
  test_num = (int32_t)((float)4095.875  * (1 << FIXED_S16_3_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S16_3)) == 0);
  // overflowing positively from 4095.875 results in going to -4096
  num.raw_value++;
  cl_assert(num.integer == -4096);
  cl_assert(num.fraction == 0);
}

void test_math_fixed__Fixed_S16_3_rounded_int(void) {
  cl_assert_equal_i(0, Fixed_S16_3_rounded_int(Fixed_S16_3(0)));
  cl_assert_equal_i(0, Fixed_S16_3_rounded_int(Fixed_S16_3(3)));
  cl_assert_equal_i(1, Fixed_S16_3_rounded_int(Fixed_S16_3(4)));
  cl_assert_equal_i(1, Fixed_S16_3_rounded_int(Fixed_S16_3(8)));
  cl_assert_equal_i(2, Fixed_S16_3_rounded_int(Fixed_S16_3(12)));
  cl_assert_equal_i(0, Fixed_S16_3_rounded_int(Fixed_S16_3(-3)));
  cl_assert_equal_i(-1, Fixed_S16_3_rounded_int(Fixed_S16_3(-4)));
  cl_assert_equal_i(-1, Fixed_S16_3_rounded_int(Fixed_S16_3(-5)));
  cl_assert_equal_i(-1, Fixed_S16_3_rounded_int(Fixed_S16_3(-8)));
  cl_assert_equal_i(-2, Fixed_S16_3_rounded_int(Fixed_S16_3(-12)));
}

void test_math_fixed__S16_3_rounding(void) {
  Fixed_S16_3 num;
  int16_t test_num;

  // This test shows how the in between fractional values evaluate to the fixed representation
  // Positive numbers round down to nearest fraction
  // Negative numbers round up to nearest fraction
  test_num = (int16_t)((float)-1.249 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -2);
  cl_assert(num.fraction == 7); // rounds up to -1.125

  test_num = (int16_t)((float)-1.126 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -2);
  cl_assert(num.fraction == 7); // rounds up to -1.125

  test_num = (int16_t)((float)-1.124 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == -1);
  cl_assert(num.fraction == 0); // rounds up to -1.000

  test_num = (int16_t)((float)1.124 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 1);
  cl_assert(num.fraction == 0); // rounds down to 1.000

  test_num = (int16_t)((float)1.126 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 1);
  cl_assert(num.fraction == 1); // rounds down to 1.125

  test_num = (int16_t)((float)1.249 * (1 << FIXED_S16_3_PRECISION));
  num = (Fixed_S16_3){ .raw_value = test_num };
  cl_assert(num.integer == 1);
  cl_assert(num.fraction == 1); // rounds down to 1.125

}

void test_math_fixed__S16_3_add(void) {
  Fixed_S16_3 num1, num2;
  Fixed_S16_3 sum, sum_c;

  // Test number addition
  num1 = FIXED_S16_3_ONE;
  num2 = FIXED_S16_3_ONE;
  sum = Fixed_S16_3_add(num1, num2);
  sum_c = (Fixed_S16_3){ .raw_value = (int16_t)((float)2 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = 3.5 + 1 = 4.5
  num1 = (Fixed_S16_3){ .raw_value = (int16_t)((float)3.5 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = FIXED_S16_3_ONE;
  sum = Fixed_S16_3_add(num1, num2);
  sum_c = (Fixed_S16_3){ .raw_value = (int16_t)((float)4.5 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = 1 + 3.5 = 4.5 - test commutative property of addition
  num1 = FIXED_S16_3_ONE;
  num2 = (Fixed_S16_3){ .raw_value = (int16_t)((float)3.5 * (1 << FIXED_S16_3_PRECISION)) };
  sum = Fixed_S16_3_add(num1, num2);
  sum_c = (Fixed_S16_3){ .raw_value = (int16_t)((float)4.5 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = -2 + -3 = -5
  num1 = (Fixed_S16_3){ .raw_value = (int16_t)((float)-2 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = (Fixed_S16_3){ .raw_value = (int16_t)((float)-3 * (1 << FIXED_S16_3_PRECISION)) };
  sum = Fixed_S16_3_add(num1, num2);
  sum_c = (Fixed_S16_3){ .raw_value = (int16_t)((float)-5 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = -2 + 5 = 3
  num1 = (Fixed_S16_3){ .raw_value = (int16_t)((float)-2 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = (Fixed_S16_3){ .raw_value = (int16_t)((float)5 * (1 << FIXED_S16_3_PRECISION)) };
  sum = Fixed_S16_3_add(num1, num2);
  sum_c = (Fixed_S16_3){ .raw_value = (int16_t)((float)3 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = -2.1 + 5.4 ~= 3.375 (nearest 1/8 fraction to the expected result)
  // math is as follows:
  // -2.1 * (1 << 8) = -16.8 = -16 (drop fraction) ==> -2
  // 5.4 * (1 << 8) = 43.2 = 43 (drop fraction) ==> 5.375
  // -16 + 43 = 27 = 3.375 * (1 << 8)
  num1 = (Fixed_S16_3){ .raw_value = (int16_t)((float)-2.1 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = (Fixed_S16_3){ .raw_value = (int16_t)((float)5.4 * (1 << FIXED_S16_3_PRECISION)) };
  sum = Fixed_S16_3_add(num1, num2);
  sum_c = (Fixed_S16_3){ .raw_value = (int16_t)((float)3.375 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = 2.1 - 5.4 ~= -3.375 (nearest 1/8 fraction to the expected result)
  // math is as follows:
  // 2.1 * (1 << 8) = 16.8 = 16 (drop fraction) ==> 2
  // -5.4 * (1 << 8) = -43.2 = -43 (drop fraction) ==> -5.375
  // 16 - 43 = -27 = -3.375 * (1 << 8)
  num1 = (Fixed_S16_3){ .raw_value = (int16_t)((float)2.1 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = (Fixed_S16_3){ .raw_value = (int16_t)((float)-5.4 * (1 << FIXED_S16_3_PRECISION)) };
  sum = Fixed_S16_3_add(num1, num2);
  sum_c = (Fixed_S16_3){ .raw_value = (int16_t)((float)-3.375 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);
}

////////////////////////////////////////////////////////////////
/// Fixed_S32_16 = 1 bit sign, 15 bits integer, 16 bits fraction
////////////////////////////////////////////////////////////////
void test_math_fixed__S32_16_create(void) {
  Fixed_S32_16 num;
  int32_t test_num;

  cl_assert(FIXED_S32_16_PRECISION == 16);

  test_num = 1 << FIXED_S32_16_PRECISION;
  num = (Fixed_S32_16){ .integer = 1, .fraction = 0 };

  test_num = (int32_t)((float)1 * (1 << FIXED_S32_16_PRECISION));
  cl_assert(num.raw_value == test_num);
  cl_assert((FIXED_S32_16_ONE.raw_value == test_num));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S32_16)) == 0);

  num = (Fixed_S32_16){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S32_16_PRECISION)) };
  test_num = (int32_t)((float)3.5 * (1 << FIXED_S32_16_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S32_16)) == 0);

  num = (Fixed_S32_16){ .raw_value = (int32_t)((float)-2 * (1 << FIXED_S32_16_PRECISION)) };
  test_num = (int32_t)((float)-2 * (1 << FIXED_S32_16_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S32_16)) == 0);

  num = (Fixed_S32_16){ .raw_value = (int32_t)((float)-3.5 * (1 << FIXED_S32_16_PRECISION)) };
  test_num = (int32_t)((float)-3.5 * (1 << FIXED_S32_16_PRECISION));
  cl_assert(memcmp(&num, &test_num, sizeof(Fixed_S32_16)) == 0);

}

void test_math_fixed__S32_16_add(void) {
  Fixed_S32_16 num1, num2;
  Fixed_S32_16 sum, sum_c;

  // Test number addition
  num1 = FIXED_S32_16_ONE;
  num2 = FIXED_S32_16_ONE;
  sum = Fixed_S32_16_add(num1, num2);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)2 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = 3.5 + 1 = 4.5
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = FIXED_S32_16_ONE;
  sum = Fixed_S32_16_add(num1, num2);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)4.5 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = 1 + 3.5 = 4.5 - test commutative property of addition
  num1 = FIXED_S32_16_ONE;
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S32_16_PRECISION)) };
  sum = Fixed_S32_16_add(num1, num2);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)4.5 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = -2 + -3 = -5
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-2 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-3 * (1 << FIXED_S32_16_PRECISION)) };
  sum = Fixed_S32_16_add(num1, num2);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)-5 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = -2 + 5 = 3
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-2 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)5 * (1 << FIXED_S32_16_PRECISION)) };
  sum = Fixed_S32_16_add(num1, num2);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)3 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = -2.1 + 5.4 = 3.3
  // math is as follows:
  // -2.1 * (1 << 16) = -137625.6 = -137625 (drop fraction)
  // 5.4 * (1 << 16) = 353894.4 = 353894 (drop fraction)
  // -137625 + 353894 = 216269 ~= 3.3
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-2.1 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)5.4 * (1 << FIXED_S32_16_PRECISION)) };
  sum = Fixed_S32_16_add(num1, num2);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)(216269) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = 2.1 - 5.4 = -3.3
  // math is as follows:
  // 2.1 * (1 << 16) = 137625.6 = 137625 (drop fraction)
  // -5.4 * (1 << 16) = -353894.4 = -353894 (drop fraction)
  // 137625 - 353894 = -216269 ~= -3.3
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)2.1 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-5.4 * (1 << FIXED_S32_16_PRECISION)) };
  sum = Fixed_S32_16_add(num1, num2);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)(-216269) };
  cl_assert(sum.raw_value == sum_c.raw_value);
}

void test_math_fixed__S32_16_add3(void) {
  Fixed_S32_16 num1, num2, num3;
  Fixed_S32_16 sum, sum_c;

  // Test number addition
  num1 = FIXED_S32_16_ONE;
  num2 = FIXED_S32_16_ONE;
  num3 = FIXED_S32_16_ONE;
  sum = Fixed_S32_16_add3(num1, num2, num3);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)3 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(sum.raw_value == sum_c.raw_value);

  // sum = 3.7 + 2.3 + 1.1 = 242483.2 + 150732.8 + 72089.6
  //                       ~= 242483 + 150732 + 72089 = 465304 ~= 7.1
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3.7 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)2.3 * (1 << FIXED_S32_16_PRECISION)) };
  num3 = (Fixed_S32_16){ .raw_value = (int32_t)((float)1.1 * (1 << FIXED_S32_16_PRECISION)) };
  sum = Fixed_S32_16_add3(num1, num2, num3);
  sum_c = (Fixed_S32_16){ .raw_value = (int32_t)(465304) };
  cl_assert(sum.raw_value == sum_c.raw_value);
}

void test_math_fixed__S32_16_mul(void) {
  Fixed_S32_16 num1, num2;
  Fixed_S32_16 mul, mul_c;

  // Test number multiplication
  num1 = FIXED_S32_16_ONE;
  num2 = FIXED_S32_16_ONE;
  mul = Fixed_S32_16_mul(num1, num2);
  mul_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)1 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);

  // 2*3 = 6
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)2 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3 * (1 << FIXED_S32_16_PRECISION)) };
  mul = Fixed_S32_16_mul(num1, num2);
  mul_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)6 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);

  // -2*3 = -6
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-2 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3 * (1 << FIXED_S32_16_PRECISION)) };
  mul = Fixed_S32_16_mul(num1, num2);
  mul_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)-6 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);

  // -2*-3 = 6
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-2 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-3 * (1 << FIXED_S32_16_PRECISION)) };
  mul = Fixed_S32_16_mul(num1, num2);
  mul_c = (Fixed_S32_16){ .raw_value = (int32_t)((float)6 * (1 << FIXED_S32_16_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);

  // -2.5*-3.3 ==> 163840 * 216268.8 = 163840 * 216268 ==> 35433349120 / 65536 ==> 540670 ~= 8.25
  num1 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-2.5 * (1 << FIXED_S32_16_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)-3.3 * (1 << FIXED_S32_16_PRECISION)) };
  mul = Fixed_S32_16_mul(num1, num2);
  mul_c = (Fixed_S32_16){ .raw_value = (int32_t)(540670) };
  cl_assert(mul.raw_value == mul_c.raw_value);
}

////////////////////////////////////////////////////////////////
/// Mixed operations
////////////////////////////////////////////////////////////////
void test_math_fixed__S16_3_S32_16_mul(void) {
  Fixed_S16_3 num1;
  Fixed_S32_16 num2;
  Fixed_S16_3 mul, mul_c;

  // 1*1  = 1
  num1 = FIXED_S16_3_ONE;
  num2 = FIXED_S32_16_ONE;
  mul = Fixed_S16_3_S32_16_mul(num1, num2);
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)1 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);

  // 3.5*1 = 3.5
  num1 = (Fixed_S16_3){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = FIXED_S32_16_ONE;
  mul = Fixed_S16_3_S32_16_mul(num1, num2);
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);

  // 1*3.5 = 3.5
  num1 = FIXED_S16_3_ONE;
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S32_16_PRECISION)) };
  mul = Fixed_S16_3_S32_16_mul(num1, num2);
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);

  // 2.25*3.5 = 7.875
  num1 = (Fixed_S16_3){ .raw_value = (int32_t)((float)2.25 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3.5 * (1 << FIXED_S32_16_PRECISION)) };
  mul = Fixed_S16_3_S32_16_mul(num1, num2);
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)7.875 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);
  // check surrounding values
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)7.750 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value != mul_c.raw_value);
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)8.0 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value != mul_c.raw_value);


  // 2.25*3.3 = 7.425
  num1 = (Fixed_S16_3){ .raw_value = (int32_t)((float)2.25 * (1 << FIXED_S16_3_PRECISION)) };
  num2 = (Fixed_S32_16){ .raw_value = (int32_t)((float)3.3 * (1 << FIXED_S32_16_PRECISION)) };
  mul = Fixed_S16_3_S32_16_mul(num1, num2);
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)7.425 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);
  // check surrounding values
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)7.375 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value == mul_c.raw_value);
  mul_c = (Fixed_S16_3){ .raw_value = (int32_t)((float)7.5 * (1 << FIXED_S16_3_PRECISION)) };
  cl_assert(mul.raw_value != mul_c.raw_value);
}
