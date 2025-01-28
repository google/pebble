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

#include "debug/power_tracking.h"
#include "drivers/mcu.h"
#include "drivers/rtc.h"
#include "drivers/task_watchdog.h"

#include "kernel/memory_layout.h"
#include "kernel/pbl_malloc.h"
#include "os/tick.h"
#include "kernel/util/stop.h"
#include "kernel/util/wfi.h"
#include "process_management/worker_manager.h"
#include "services/common/analytics/analytics.h"
#include "system/logging.h"
#include "util/math.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#include "FreeRTOS.h"
#include "task.h"
#include "freertos_application.h"

static uint64_t s_analytics_device_sleep_cpu_cycles = 0;
static RtcTicks s_analytics_device_stop_ticks = 0;

static uint64_t s_analytics_app_sleep_cpu_cycles = 0;
static RtcTicks s_analytics_app_stop_ticks = 0;

// We need different timings for our different platforms since we use different mechanisms to keep
// time and to wake us up out of stop mode. On stm32f2 we don't have a millisecond register so we
// use the "retina rtc" and a RTC Alarm peripheral. On stm32f4 we do have a millisecond register
// so use the RTC running at normal speed and a RTC Wakeup peripheral. These have different
// accuracies when going into and out of stop mode.
#if defined(MICRO_FAMILY_STM32F2)
//! Stop mode until this number of ticks before the next scheduled task
static const RtcTicks EARLY_WAKEUP_TICKS = 2;
// slightly larger than the 2 permitted by FreeRTOS in tasks.c
static const RtcTicks MIN_STOP_TICKS = 5;
#elif defined(MICRO_FAMILY_STM32F4) || defined(MICRO_FAMILY_STM32F7)
//! Stop mode until this number of ticks before the next scheduled task
static const RtcTicks EARLY_WAKEUP_TICKS = 4;
//! Stop mode until this number of ticks before the next scheduled task
static const RtcTicks MIN_STOP_TICKS = 8;
#endif


// 1024 ticks so that we only wake up once every regular timer interval.
static const RtcTicks MAX_STOP_TICKS = 1024;

extern void vPortSuppressTicksAndSleep( TickType_t xExpectedIdleTime ) {
  if (!rtc_alarm_is_initialized() || !sleep_mode_is_allowed()) {
    // the RTC is not yet initialized to the point where it can wake us from sleep or sleep/stop
    // is disabled. Just returning will cause a busy loop where the caller thought we slept for
    // 0 ticks and will reevaluate what to do next (probably just try again).
    return;
  }

  // Note: all tasks are suspended at this point, but we can still be interrupted
  // so the critical section is necessary. taskENTER_CRITICAL() is not used here
  // as that method would mask interrupts that should exit the low-power mode.
  // The __disable_irq() function sets the PRIMASK bit which globally prevents
  // interrupt execution while still allowing interrupts to wake the processor
  // from WFI.
  // Conversely, taskEnter_CRITICAL() sets the BASEPRI register, which masks
  // interrupts with priorities lower than configMAX_SYSCALL_INTERRUPT_PRIORITY
  // from executing and from waking the processor.
  // See: http://infocenter.arm.com/help/topic/com.arm.doc.dui0552a/BABGGICD.html#BGBHDHAI
  __disable_irq();

  power_tracking_stop(PowerSystemMcuCoreRun);

  if (eTaskConfirmSleepModeStatus() != eAbortSleep) {
    if (xExpectedIdleTime < MIN_STOP_TICKS || !stop_mode_is_allowed()) {
      // We assume that a WFI to trigger sleep mode will not last longer than 1
      // SysTick. (The SysTick INT doesn't automatically get suppressed) Thus,
      // we use the SysTick timer to get a better estimate of our sleep time
      //
      // TODO: It would be nice if there was a clean way to actually 'suppress
      // ticks' while in sleep mode. If we figure that out, we would likely
      // need to update how this calculation works
      uint32_t systick_start = SysTick->VAL;

      power_tracking_start(PowerSystemMcuCoreSleep);
      __DSB();  // Drain any pending memory writes before entering sleep.
      do_wfi();  // Wait for Interrupt (enter sleep mode). Work around F2/F4 errata.
      __ISB();  // Let the pipeline catch up (force the WFI to activate before moving on).
      power_tracking_stop(PowerSystemMcuCoreSleep);

      uint32_t systick_stop = SysTick->VAL;
      uint32_t cycles_elapsed;
      if (systick_stop < systick_start) {
        cycles_elapsed = systick_start - systick_stop;
      } else {
        cycles_elapsed = (SysTick->LOAD - systick_stop) + systick_start;
      }

      s_analytics_device_sleep_cpu_cycles += cycles_elapsed;
      s_analytics_app_sleep_cpu_cycles += cycles_elapsed;
    } else {
      const RtcTicks stop_duration = MIN(xExpectedIdleTime - EARLY_WAKEUP_TICKS, MAX_STOP_TICKS);

      // Go into stop mode until the wakeup_tick.
      rtc_alarm_set(stop_duration);
      enter_stop_mode();

      RtcTicks ticks_elapsed = rtc_alarm_get_elapsed_ticks();
      vTaskStepTick(ticks_elapsed);

      // Update the task watchdog every time we come out of STOP mode (which is
      // at least once/second) since the timer peripheral will not have been
      // incremented
      task_watchdog_step_elapsed_time_ms((ticks_elapsed * 1000) / RTC_TICKS_HZ);

      s_analytics_device_stop_ticks += ticks_elapsed;
      s_analytics_app_stop_ticks += ticks_elapsed;
    }
  }

  power_tracking_start(PowerSystemMcuCoreRun);

  __enable_irq();
}

void vApplicationStackOverflowHook(TaskHandle_t task_handle, signed char *name) {
  PebbleTask task = pebble_task_get_task_for_handle(task_handle);

  // If the task is application or worker, ignore this hook. We have a memory protection region
  // setup at the bottom of those stacks and the code that catches MPU violations to that
  // area in fault_handling.c has the logic to safely kill those user tasks without forcing
  // a reboot.
  if ((task != PebbleTask_App) && (task != PebbleTask_Worker)) {
    PBL_LOG_SYNC(LOG_LEVEL_ERROR, "Stack overflow [task: %s]", name);
    RebootReason reason = {
      .code = RebootReasonCode_StackOverflow,
      .data8[0] = task
    };
    reboot_reason_set(&reason);

    reset_due_to_software_failure();
  }
}

bool xApplicationIsAllowedToRaisePrivilege(uint32_t caller_pc) {
  // This function is called by portSVCHandler with the PC value of the
  // function which initiated the SVC call requesting privilege elevation.

  // The memory_region.c functions are not used for this check as this function
  // is in a hot code-path and needs to execute as quickly as possible.

  // All syscall functions are lumped together in one place in the firmware
  // image to reduce the attack surface. Don't allow privilege to be raised by
  // any code outside of that region, even if that code is in flash.
  // See WHT-114 and PBL-34044.
  extern const uint32_t __syscall_text_start__[];
  extern const uint32_t __syscall_text_end__[];
  const uint32_t priv_code_start = (uint32_t) __syscall_text_start__;
  const uint32_t priv_code_end = (uint32_t) __syscall_text_end__;
  return (caller_pc >= priv_code_start && caller_pc < priv_code_end);
}

#undef vPortFree
void vPortFree(void* pv) {
  kernel_free(pv);
}

#undef pvPortMalloc
void* pvPortMalloc(size_t xSize) {
  return kernel_malloc(xSize);
}

// Called from the SysTick handler ISR to adjust ticks for situations where the CPU might
// occasionally fall behind and miss some tick interrupts (like when running under emulation).
bool vPortCorrectTicks(void) {
  static uint8_t s_check_counter = 0;
  static int64_t s_rtc_ticks_to_rtos_ticks = 0;

  if (++s_check_counter < 10) {
    // Just check occasionally so we don't incur the overhead of reading the RTC on every
    // systick
    return false;
  }
  s_check_counter = 0;

  // Compute what ticks should be based on the real time clock.
  time_t seconds;
  uint16_t milliseconds;
  rtc_get_time_ms(&seconds, &milliseconds);
  int64_t rtc_ticks = ((((int64_t)seconds * 1000) + milliseconds) * RTC_TICKS_HZ) / 1000;
  uint32_t target_rtos_ticks = rtc_ticks + s_rtc_ticks_to_rtos_ticks;
  uint32_t act_ticks = xTaskGetTickCountFromISR();

  if (act_ticks > target_rtos_ticks + 100 || act_ticks < target_rtos_ticks - 100) {
    // If we are too far out of range of the target ticks, just reset our offsets. This could
    // be caused either by the RTC time being changed or by staying in the debugger too long
    s_rtc_ticks_to_rtos_ticks = (int64_t)act_ticks - rtc_ticks;
    return false;
  } else if (act_ticks >= target_rtos_ticks) {
    // No correction needed
    return false;
  }

  // Let's advance the RTOS ticks until we catch up
  bool need_context_switch = false;
  while (act_ticks < target_rtos_ticks) {
    /* Increment the RTOS ticks. */
    need_context_switch |= (xTaskIncrementTick() != 0);
    act_ticks++;
  }
  return need_context_switch;
}


// CPU analytics
///////////////////////////////////////////////////////////

static uint32_t s_last_ticks = 0;
void dump_current_runtime_stats(void) {
  uint32_t stop_ms = ticks_to_milliseconds(s_analytics_device_stop_ticks);
  uint32_t sleep_ms = mcu_cycles_to_milliseconds(s_analytics_device_sleep_cpu_cycles);

  uint32_t now_ticks = rtc_get_ticks();
  uint32_t running_ms =
      ticks_to_milliseconds(now_ticks - s_last_ticks) - stop_ms - sleep_ms;

  uint32_t tot_time = running_ms + sleep_ms + stop_ms;

  char buf[80];
  dbgserial_putstr_fmt(buf, sizeof(buf), "Run:   %"PRIu32" ms (%"PRIu32" %%)",
                       running_ms, (running_ms * 100) / tot_time);
  dbgserial_putstr_fmt(buf, sizeof(buf), "Sleep: %"PRIu32" ms (%"PRIu32" %%)",
                       sleep_ms, (sleep_ms * 100) / tot_time);
  dbgserial_putstr_fmt(buf, sizeof(buf), "Stop:  %"PRIu32" ms (%"PRIu32" %%)",
                       stop_ms, (stop_ms * 100) / tot_time);
  dbgserial_putstr_fmt(buf, sizeof(buf), "Tot:   %"PRIu32" ms", tot_time);
}

void analytics_external_collect_cpu_stats(void) {
  uint32_t stop_ms = ticks_to_milliseconds(s_analytics_device_stop_ticks);
  uint32_t sleep_ms = mcu_cycles_to_milliseconds(s_analytics_device_sleep_cpu_cycles);

  analytics_set(ANALYTICS_DEVICE_METRIC_CPU_STOP_TIME, stop_ms, AnalyticsClient_System);
  analytics_set(ANALYTICS_DEVICE_METRIC_CPU_SLEEP_TIME, sleep_ms, AnalyticsClient_System);

  uint32_t now_ticks = rtc_get_ticks();
  uint32_t ms_running =
      ticks_to_milliseconds(now_ticks - s_last_ticks) - stop_ms - sleep_ms;
  analytics_set(ANALYTICS_DEVICE_METRIC_CPU_RUNNING_TIME, ms_running, AnalyticsClient_System);

  s_last_ticks = now_ticks;
  s_analytics_device_sleep_cpu_cycles = 0;
  s_analytics_device_stop_ticks = 0;
}

void analytics_external_collect_app_cpu_stats(void) {
  static uint32_t s_last_ticks = 0;

  uint32_t sleep_ms = mcu_cycles_to_milliseconds(s_analytics_app_sleep_cpu_cycles);

  uint32_t now_ticks = rtc_get_ticks();
  uint32_t stop_ms = ticks_to_milliseconds(s_analytics_app_stop_ticks);
  uint32_t awake_ms = ticks_to_milliseconds(now_ticks - s_last_ticks) - stop_ms - sleep_ms;

  analytics_set(ANALYTICS_APP_METRIC_CPU_RUNNING_TIME, awake_ms, AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_CPU_SLEEP_TIME,
                mcu_cycles_to_milliseconds(s_analytics_app_sleep_cpu_cycles),
                AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_CPU_STOP_TIME, stop_ms, AnalyticsClient_App);

  // NOTE: When we are running, we can't really tell how much of the time was spent in each task, so
  // the best we can do as attribute the elapsed running time to both the foreground and background worker
  if (worker_manager_get_current_worker_md() != NULL) {
    analytics_set(ANALYTICS_APP_METRIC_BG_CPU_RUNNING_TIME, awake_ms, AnalyticsClient_Worker);
    analytics_set(ANALYTICS_APP_METRIC_BG_CPU_SLEEP_TIME, sleep_ms, AnalyticsClient_Worker);
    analytics_set(ANALYTICS_APP_METRIC_BG_CPU_STOP_TIME, stop_ms, AnalyticsClient_Worker);
  }

  s_last_ticks = now_ticks;
  s_analytics_app_sleep_cpu_cycles = 0;
  s_analytics_app_stop_ticks = 0;
}
