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

#include "services/common/status_led.h"
#include "system/passert.h"
#include "services/common/battery/battery_curve.h"
#include "drivers/led_controller.h"
#include "board/board.h"
#include "system/passert.h"

#if CAPABILITY_HAS_LED

static uint32_t s_led_color = LED_BLACK;

void status_led_set(StatusLedState state) {
  PBL_ASSERTN(state < StatusLedStateCount);

  static const uint32_t STATE_COLOR_MAPPING[] = {
    [StatusLedState_Off] = LED_BLACK,
    [StatusLedState_Charging] = LED_DIM_ORANGE,
    [StatusLedState_FullyCharged] = LED_DIM_GREEN
  };

  const uint32_t new_color = STATE_COLOR_MAPPING[state];

  if (new_color == s_led_color) {
    return;
  }

  s_led_color = new_color;

  // Tell the battery curve service to account for the updated LED state.
  int compensation_mv = 0;
  if (s_led_color != LED_BLACK) {
    compensation_mv = BOARD_CONFIG_POWER.charging_status_led_voltage_compensation;
  }
  battery_curve_set_compensation(BATTERY_CURVE_COMPENSATE_STATUS_LED, compensation_mv);

  led_controller_rgb_set_color(s_led_color);
}

#else

void status_led_set(StatusLedState state) {
  // No LED present, do nothing!
}

#endif // CAPABILITY_HAS_LED
