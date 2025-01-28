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

#include "util/assert.h"
#include "util/math_fixed.h"

Fixed_S64_32 math_fixed_recursive_filter(Fixed_S64_32 x,
                                         int num_input_coefficients, int num_output_coefficients,
                                         const Fixed_S64_32 *cb, const Fixed_S64_32 *ca,
                                         Fixed_S64_32 *state_x, Fixed_S64_32 *state_y) {
  UTIL_ASSERT(num_input_coefficients >= 1);

  // shift the input over by one
  for (int k = num_input_coefficients - 1; k > 0; k--) {
    state_x[k] = state_x[k - 1];
  }
  state_x[0] = x;

  // Factor in the x * b elements
  Fixed_S64_32 ytmp = Fixed_S64_32_mul(cb[0], state_x[0]);
  for (int i = 1; i < num_input_coefficients; i++) {
    ytmp = Fixed_S64_32_add(ytmp, Fixed_S64_32_mul(cb[i], state_x[i]));
  }

  // Factor in the y * a coefficients
  for (int i = 0; i < num_output_coefficients; i++) {
    ytmp = Fixed_S64_32_sub(ytmp, Fixed_S64_32_mul(ca[i], state_y[i]));
  }

  // shift the y output elements
  for (int k = num_output_coefficients - 1; k > 0; k--) {
    state_y[k] = state_y[k-1];
  }
  state_y[0] = ytmp;

  return ytmp;
}
