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

#include "delay.h"

#include <inttypes.h>

void delay_us(uint32_t us) {
  // Empirically (measured on a bb2), 1 loop = ~47ns. (sysclk @
  // 64MHz, prefetch disabled) Alignment of code will have some impact on how
  // long this actually takes
  uint32_t delay_loops = us * 22;

  __asm volatile (
      "spinloop:                             \n"
      "  subs %[delay_loops], #1             \n"
      "  bne spinloop                        \n"
      : [delay_loops] "+r" (delay_loops) // read-write operand
      :
      : "cc"
       );
}

void delay_ms(uint32_t millis) {
  // delay_us(millis*1000) is not used because a long delay could easily
  // overflow the variable. Without the outer loop, a delay of even five
  // seconds would overflow.
  while (millis--) {
    delay_us(1000);
  }
}
