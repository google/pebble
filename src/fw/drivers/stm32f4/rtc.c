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

#include "drivers/rtc.h"
#include "drivers/rtc_private.h"

#include "drivers/clocksource.h"
#include "drivers/exti.h"
#include "drivers/periph_config.h"
#include "drivers/rtc.h"

#include "mcu/interrupts.h"
#include "system/logging.h"
#include "system/passert.h"

#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#include "FreeRTOS.h"
#include "task.h"

#define LSE_FREQUENCY_HZ 32768

static uint64_t s_alarm_set_time_milliseconds_since_epoch;

static const int RTC_CLOCK_ASYNC_PRESCALER = 127;
static const int RTC_CLOCK_SYNC_PRESCALER = 255;

void rtc_init(void) {
  periph_config_acquire_lock();
  rtc_enable_backup_regs();

  clocksource_lse_configure();

  // Only initialize the RTC peripheral if it's not already enabled.
  if (!(RCC->BDCR & RCC_BDCR_RTCEN)) {
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
    RCC_RTCCLKCmd(ENABLE);

    RTC_InitTypeDef rtc_init_struct;
    RTC_StructInit(&rtc_init_struct);
    rtc_init_struct.RTC_AsynchPrediv = RTC_CLOCK_ASYNC_PRESCALER;
    rtc_init_struct.RTC_SynchPrediv = RTC_CLOCK_SYNC_PRESCALER;
    RTC_Init(&rtc_init_struct);
  }

  RTC_WaitForSynchro();

  periph_config_release_lock();

#ifdef PBL_LOG_ENABLED
  char buffer[TIME_STRING_BUFFER_SIZE];
  PBL_LOG(LOG_LEVEL_DEBUG, "Current time is <%s>", rtc_get_time_string(buffer));
#endif
}

void rtc_calibrate_frequency(uint32_t frequency) {
  // Nothing to do here! (yet)
}

void rtc_init_timers(void) {
  // Nothing to do here!
}

RtcTicks rtc_get_ticks(void) {
  static TickType_t s_last_freertos_tick_count = 0;
  static RtcTicks s_coarse_ticks = 0;

  bool ints_enabled = mcu_state_are_interrupts_enabled();
  if (ints_enabled) {
    __disable_irq();
  }

  TickType_t freertos_tick_count = xTaskGetTickCount();
  if (freertos_tick_count < s_last_freertos_tick_count) {
    // We rolled over! Note this will happen every 2^32 / 1024 (tick rate) seconds, which is about
    // 49 days. If this function doesn't get called for 49 days we'll miss a rollover but that's
    // extremely unlikely.
    TickType_t rollover_amount = -1;
    s_coarse_ticks += rollover_amount;
  }

  s_last_freertos_tick_count = freertos_tick_count;
  RtcTicks ret_value = freertos_tick_count + s_coarse_ticks;

  if (ints_enabled) {
    __enable_irq();
  }
  return ret_value;
}

void rtc_set_time(time_t time) {
  struct tm t;
  gmtime_r(&time, &t);

  // Just die if sanitization is necessary.
  PBL_ASSERTN(!rtc_sanitize_struct_tm(&t));

  RTC_TimeTypeDef rtc_time_struct = {
    .RTC_Hours = t.tm_hour,
    .RTC_Minutes = t.tm_min,
    .RTC_Seconds = t.tm_sec
  };

  RTC_DateTypeDef rtc_date_struct = {
    .RTC_Month = t.tm_mon + 1, // RTC_Month is 1-12, tm_mon is 0-11
    .RTC_Date = t.tm_mday,
    .RTC_Year = (t.tm_year % 100) // tm_year is years since 1900, RTC_Year is just 0-99
  };

  RTC_SetTime(RTC_Format_BIN, &rtc_time_struct);
  RTC_SetDate(RTC_Format_BIN, &rtc_date_struct);
}

void rtc_get_time_ms(time_t* out_seconds, uint16_t* out_ms) {
  uint32_t sub_seconds = 0;
  RTC_DateTypeDef rtc_date;
  RTC_TimeTypeDef rtc_time;

  // NOTE: There is a tricky rollover situation that can occur here if the date rolls over
  // between when we read the date and time registers. For example:
  //    read date: 1/1/14  (actual time 11:59:59 PM)
  //    [date rolls over]
  //    read time: 12:00:00 AM (actual date now 1/2/14)
  // At the end of this, we would think the date and time is 1/1/14 12:00:00 AM and we
  // would be 24 hours behind the actual date and time.
  // A similar issue can occur if the seconds change right after we've read the time register
  // and before we've read the subsecond register
  // To eliminate these possibilities, we read the time register both before and after
  // we read the date and subsecond registers and only exit this method if we are in the same
  // second both before and after.
  int max_loops = 4;    // If we loop more than this many times, something is seriously wrong
  while (--max_loops) {
    RTC_TimeTypeDef rtc_time_before;
    RTC_GetTime(RTC_Format_BIN, &rtc_time_before);
    RTC_GetDate(RTC_Format_BIN, &rtc_date);

    // See reference manual section 26.6.11 for SSR to milliseconds conversion
    sub_seconds = RTC_GetSubSecond();

    // Make sure neither date nor time rolled over since we read them.
    RTC_GetTime(RTC_Format_BIN, &rtc_time);
    // we need to read the DR again because reading the RTC_TR or RTC_SSR locks the shadow register
    // for RTC_DR and leaves it in a stale state unless we read from it again
    // This causes time to go backwards once a day unless we unlock it after reading from RTC_TR
    RTC_GetDate(RTC_Format_BIN, &rtc_date);
    if (rtc_time.RTC_Seconds == rtc_time_before.RTC_Seconds) {
      break;
    }
  }
  PBL_ASSERTN(max_loops > 0);

  struct tm current_time = {
    .tm_sec = rtc_time.RTC_Seconds,
    .tm_min = rtc_time.RTC_Minutes,
    .tm_hour = rtc_time.RTC_Hours,
    .tm_mday = rtc_date.RTC_Date,
    .tm_mon = rtc_date.RTC_Month - 1, // RTC_Month is 1-12, tm_mon is 0-11
    .tm_year = rtc_date.RTC_Year + 100, // RTC_Year is 0-99, tm_year is years since 1900.
                                        // Assumes 2000+ year, we may guess 20th or 21st century.
    .tm_wday = rtc_date.RTC_WeekDay,
    .tm_yday = 0,
    .tm_isdst = 0
  };

  // Verify the registers have valid values. While rtc_set_time_ms above prevents us from setting
  // invalid values I want to be safe against other firmwares that we've upgraded from seeding
  // bad values in our RTC registers which could lead to a reboot loop.
  bool sanitization_done = rtc_sanitize_struct_tm(&current_time);

  // Calculate our results
  *out_seconds = mktime(&current_time);
  *out_ms = (uint16_t)(
      ((RTC_CLOCK_SYNC_PRESCALER - sub_seconds) * 1000) / (RTC_CLOCK_SYNC_PRESCALER + 1));

  if (sanitization_done) {
    // Fix up the underlying registers so we don't have to do this again.
    rtc_set_time(*out_seconds);
  }
}


time_t rtc_get_time(void) {
  time_t  seconds;
  uint16_t ms;

  rtc_get_time_ms(&seconds, &ms);
  return seconds;
}


//! Tracks whether we've successfully initialized the wakeup functionality
static bool s_rtc_wakeup_initialized = false;

static const int RTC_WAKEUP_HZ = LSE_FREQUENCY_HZ / 2;

void rtc_alarm_init(void) {
  RTC_ITConfig(RTC_IT_WUT, DISABLE);
  RTC_WakeUpCmd(DISABLE);

  // Make sure this in in sync with the definition of LSE_FREQUENCY_HZ. This is the lowest setting
  // for the highest frequency and therefore the highest accuracy. However, it limits us to only
  // setting wakeup timers for up to 4s~ (2^16 max counter value / (32768 / 2)) in the future.
  // This is fine for now as we have a regular timer register each second, so we'll never want to
  // stop for more than a single second.
  RTC_WakeUpClockConfig(RTC_WakeUpClock_RTCCLK_Div2);

  exti_configure_other(ExtiLineOther_RTCWakeup, ExtiTrigger_Rising);
  exti_enable_other(ExtiLineOther_RTCWakeup);

  s_rtc_wakeup_initialized = true;
}

static uint64_t prv_get_time_milliseconds_since_epoch(void) {
  time_t seconds;
  uint16_t milliseconds;
  rtc_get_time_ms(&seconds, &milliseconds);

  return ((uint64_t)seconds * 1000) + milliseconds;
}

void rtc_alarm_set(RtcTicks num_ticks) {
  PBL_ASSERTN(s_rtc_wakeup_initialized);

  uint32_t wakeup_counter = num_ticks * RTC_WAKEUP_HZ / RTC_TICKS_HZ;

  // From 26.6.6 of the STM32F4 reference manual.
  // "Note: The first assertion of WUTF occurs (WUT+1) ck_wut cycles after WUTE is set."
  wakeup_counter -= 1;

  // We can only count up to a certain number. If we need to set an alarm for a longer period
  // of time we need to decrease the RTC_WAKEUP_HZ value at the cost of some accuracy.
  PBL_ASSERTN(wakeup_counter < 65536);

  RTC_ITConfig(RTC_IT_WUT, DISABLE);

  RTC_WakeUpCmd(DISABLE);

  RTC_SetWakeUpCounter(wakeup_counter);

  RTC_ClearFlag(RTC_FLAG_WUTF);
  exti_clear_pending_other(ExtiLineOther_RTCWakeup);
  RTC_ClearITPendingBit(RTC_IT_WUT);

  RTC_WakeUpCmd(ENABLE);
  RTC_ITConfig(RTC_IT_WUT, ENABLE);

  s_alarm_set_time_milliseconds_since_epoch = prv_get_time_milliseconds_since_epoch();
}

RtcTicks rtc_alarm_get_elapsed_ticks(void) {
  uint64_t now = prv_get_time_milliseconds_since_epoch();
  PBL_ASSERTN(now >= s_alarm_set_time_milliseconds_since_epoch);
  uint64_t milliseconds_elapsed = now - s_alarm_set_time_milliseconds_since_epoch;
  return (milliseconds_elapsed * RTC_TICKS_HZ) / 1000;
}

bool rtc_alarm_is_initialized(void) {
  return s_rtc_wakeup_initialized;
}

void RTC_WKUP_IRQHandler(void) {
  if (RTC_GetITStatus(RTC_IT_WUT) != RESET) {
    RTC_WakeUpCmd(DISABLE);

    RTC_ClearITPendingBit(RTC_IT_WUT);
    exti_clear_pending_other(ExtiLineOther_RTCWakeup);
  }
}
