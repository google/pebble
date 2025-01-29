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
#include "stm32f4xx.h"
#include "util/attributes.h"

#include <inttypes.h>

// Calculated using the formula from the firmware's delay_init:
// ceil( NS_PER_US / ( clock_period * INSTRUCTIONS_PER_LOOP )
// where NS_PER_US = 1000 and INSTRUCTIONS_PER_LOOP = 3,
// and clock_period = 62.5
#define LOOPS_PER_US (6)

void NOINLINE delay_us(uint32_t us) {
  uint32_t delay_loops = us * LOOPS_PER_US;

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
