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

#include "rtc_calibration.h"

#include "system/logging.h"
#include "util/math.h"

#define STM32F2_COMPATIBLE
#include <mcu.h>


RTCCalibConfig rtc_calibration_get_config(uint32_t frequency, uint32_t target) {
  if (frequency == 0) {
    PBL_LOG(LOG_LEVEL_DEBUG, "RTC frequency invalid - Skipping calibration");
    return (RTCCalibConfig) {
      .sign = RTC_CalibSign_Positive,
      .units = 0
    };
  }

  // Difference in frequency in mHz (ex. 224 = .224 Hz off from target frequency)
  const int32_t rtc_freq_diff = target - frequency;

  // RTC_CoarseCalibConfig uses units of +4.069ppm or -2.035ppm.
  // Formula:
  // ppm = 1e6(target - frequency)/(target)
  // positive units = ppm / 4.069
  // negative units = ppm / -2.035
  uint32_t rtc_calib_sign, rtc_calib_units;
  const uint64_t numerator = 1000000000 * (uint64_t)ABS(rtc_freq_diff);
  uint64_t divisor;

  if (rtc_freq_diff >= 0) {
    divisor = 4069;
    rtc_calib_sign = RTC_CalibSign_Positive;
  } else {
    divisor = 2035;
    rtc_calib_sign = RTC_CalibSign_Negative;
  }

  rtc_calib_units = ROUND(numerator, divisor * target);

  return (RTCCalibConfig) {
    .sign = rtc_calib_sign,
    // Coarse calibration has a range of -63ppm to 126ppm.
    .units = MIN(rtc_calib_units, 31)
  };
}

// For RTC calibration testing
#ifdef RTC_CALIBRATION_TESTING

#include "drivers/rtc.h"
#include "drivers/periph_config.h"
#include "system/passert.h"

void rtc_calibration_init_timer(void) {
  const uint32_t timer_clock_hz = 32000;

  // The timer is on ABP1 which is clocked by PCLK1
  RCC_ClocksTypeDef clocks;
  RCC_GetClocksFreq(&clocks);
  uint32_t timer_clock = clocks.PCLK1_Frequency; // Hz

  uint32_t prescale = RCC->CFGR & RCC_CFGR_PPRE1;
  if (prescale != RCC_CFGR_PPRE1_DIV1) {
    // per the stm32 'clock tree' diagram, if the prescaler for APBx is not 1, then
    // the timer clock is at double the APBx frequency
    timer_clock *= 2;
  }

  // Clock frequency to run the timer at
  uint32_t prescaler = timer_clock / timer_clock_hz;
  uint32_t period = timer_clock_hz;

  // period & prescaler values are 16 bits, check for configuration errors
  PBL_ASSERTN(period <= UINT16_MAX && prescaler <= UINT16_MAX);

  periph_config_enable(TIM7, RCC_APB1Periph_TIM7);

  NVIC_InitTypeDef NVIC_InitStructure;
  /* Enable the TIM7 global Interrupt */
  TIM_ClearITPendingBit(TIM7, TIM_IT_Update);
  NVIC_InitStructure.NVIC_IRQChannel = TIM7_IRQn;
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0x0b;
  NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
  NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
  NVIC_Init(&NVIC_InitStructure);

  // Set up a timer that runs at 1Hz and activates once every second
  TIM_TimeBaseInitTypeDef tim_config;
  TIM_TimeBaseStructInit(&tim_config);
  tim_config.TIM_Period = period;
  // The timer is on ABP1 which is clocked by PCLK1
  tim_config.TIM_Prescaler = prescaler;
  // tim_config.TIM_ClockDivision = TIM_CKD_DIV4;
  tim_config.TIM_CounterMode = TIM_CounterMode_Up;
  TIM_TimeBaseInit(TIM7, &tim_config);

  TIM_ITConfig(TIM7, TIM_IT_Update, ENABLE);
  TIM_Cmd(TIM7, ENABLE);
}

static void prv_delta_ticks(void) {
  static uint64_t last_tick = 0;

  uint64_t rtc_ticks = rtc_get_ticks();
  PBL_LOG(LOG_LEVEL_INFO, "RTC tick delta: %d", rtc_ticks - last_tick);

  last_tick = rtc_ticks;
}

void TIM7_IRQHandler(void) {
  static uint8_t count = 0;

  // Workaround M3 bug that causes interrupt to fire twice:
  // https://my.st.com/public/Faq/Lists/faqlst/DispForm.aspx?ID=143
  TIM_ClearITPendingBit(TIM7, TIM_IT_Update);

  if (count == 0) {
    prv_delta_ticks();
  }

  // Log delta ticks every ~60 seconds
  count++;
  count %= 60;
}

#else

void rtc_calibration_init_timer(void) {}

#endif
