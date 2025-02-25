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

#include "console/dbgserial.h"
#include "console/dbgserial_input.h"
#include "drivers/flash.h"
#include "drivers/periph_config.h"
#include "drivers/rtc.h"
#include "drivers/task_watchdog.h"
#include "os/tick.h"
#include "kernel/util/stop.h"
#include "kernel/util/wfi.h"
#include "mcu/interrupts.h"
#include "services/common/analytics/analytics.h"
#include "system/passert.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#include <stdbool.h>
#include <inttypes.h>

static int s_num_items_disallowing_stop_mode = 0;

#ifdef PBL_NOSLEEP
static bool s_sleep_mode_allowed = false;
#else
static bool s_sleep_mode_allowed = true;
#endif

typedef struct {
  uint32_t active_count;
  RtcTicks ticks_when_stop_mode_disabled;
  RtcTicks total_ticks_while_disabled;
} InhibitorTickProfile;

// Note: These variables should be protected within a critical section since
// they are read and modified by multiple threads
static InhibitorTickProfile s_inhibitor_profile[InhibitorNumItems];

void enter_stop_mode(void) {
  // enable the interrupt on the debug RX line so that we can use the serial
  // console even when we are in stop mode.
  dbgserial_enable_rx_exti();

  flash_power_down_for_stop_mode();

  // Turn on the power control peripheral so that we can put the regulator into low-power mode
  periph_config_enable(PWR, RCC_APB1Periph_PWR);

  if (mcu_state_are_interrupts_enabled()) {
    // If INTs aren't disabled here, we would wind up servicing INTs
    // immediately after the WFI (while running at the wrong clock speed) which
    // can confuse peripherals in subtle ways
    WTF;
  }

  // Enter stop mode.
  //PWR_EnterSTOPMode(PWR_Regulator_LowPower, PWR_STOPEntry_WFI);
  // We don't use ^^ the above function because of a silicon bug which
  // causes the processor to skip some instructions upon wake from STOP
  // in certain situations. See the STM32F20x and STM32F21x Errata sheet
  // section 2.1.3 "Debugging Stop mode with WFE entry", or the erratum
  // of the same name in section 2.1.2 of the STM32F42x and STM32F43x
  // Errata sheet, for (misleading) details.
  // http://www.st.com/web/en/resource/technical/document/errata_sheet/DM00027213.pdf
  // http://www.st.com/web/en/resource/technical/document/errata_sheet/DM00068628.pdf

  // Configure the PWR peripheral to put us in low-power STOP mode when
  // the processor enters deepsleep.
#if defined(MICRO_FAMILY_STM32F7)
  uint32_t temp = PWR->CR1;
  temp &= ~PWR_CR1_PDDS;
  temp |= PWR_CR1_LPDS;
  PWR->CR1 = temp;
#else
  uint32_t temp = PWR->CR;
  temp &= ~PWR_CR_PDDS;
  temp |= PWR_CR_LPDS;
#if STM32F412xG
  // STM32F412xG supports a new "low-power regulator low voltage in deep sleep" mode.
  temp |= PWR_CR_LPLVDS;
#endif
  PWR->CR = temp;
#endif

  // Configure the processor core to enter deepsleep mode when we
  // execute a WFI or WFE instruction.
  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

  // Go stop now.
  __DSB(); // Drain any pending memory writes before entering sleep.
  do_wfi(); // Wait for Interrupt (enter sleep mode). Work around F2/F4 errata.
  __ISB(); // Let the pipeline catch up (force the WFI to activate before moving on).

  // Tell the processor not to enter deepsleep mode for future WFIs.
  SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;

  // Stop mode will change our system clock to the HSI. Move it back to the PLL.

  // Enable the PLL and wait until it's ready
  RCC_PLLCmd(ENABLE);
  while (RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET) {}

  // Select PLL as system clock source and wait until it's being used
  RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);
  while (RCC_GetSYSCLKSource() != 0x08) {}

  // No longer need the power control peripheral
  periph_config_disable(PWR, RCC_APB1Periph_PWR);

  flash_power_up_after_stop_mode();
}

void stop_mode_disable( StopModeInhibitor inhibitor ) {
  portENTER_CRITICAL();
  ++s_num_items_disallowing_stop_mode;

  ++s_inhibitor_profile[inhibitor].active_count;
  // TODO: We should probably check if s_inhibitor_profile.active_count == 1
  // before doing this assignment. We don't seem to ever run into this case
  // yet (i.e. active_count is never > 1), but when we do, this code would
  // report the wrong number of nostop ticks.
  s_inhibitor_profile[inhibitor].ticks_when_stop_mode_disabled = rtc_get_ticks();
  portEXIT_CRITICAL();
}

void stop_mode_enable( StopModeInhibitor inhibitor ) {
  portENTER_CRITICAL();
  PBL_ASSERTN(s_num_items_disallowing_stop_mode != 0);
  PBL_ASSERTN(s_inhibitor_profile[inhibitor].active_count != 0);

  --s_num_items_disallowing_stop_mode;
  --s_inhibitor_profile[inhibitor].active_count;
  if (s_inhibitor_profile[inhibitor].active_count == 0) {
    s_inhibitor_profile[inhibitor].total_ticks_while_disabled += rtc_get_ticks() -
        s_inhibitor_profile[inhibitor].ticks_when_stop_mode_disabled;
  }
  portEXIT_CRITICAL();
}

bool stop_mode_is_allowed(void) {
#if PBL_NOSTOP
  return false;
#else
  return s_num_items_disallowing_stop_mode == 0;
#endif
}

void sleep_mode_enable(bool enable) {
  s_sleep_mode_allowed = enable;
}

bool sleep_mode_is_allowed(void) {
#ifdef PBL_NOSLEEP
  return false;
#endif
  return s_sleep_mode_allowed;
}

static RtcTicks prv_get_nostop_ticks(StopModeInhibitor inhibitor, RtcTicks now_ticks) {
    RtcTicks total_ticks = s_inhibitor_profile[inhibitor].total_ticks_while_disabled;
    if (s_inhibitor_profile[inhibitor].active_count != 0) {
        total_ticks += (now_ticks - s_inhibitor_profile[inhibitor].ticks_when_stop_mode_disabled);
    }
    return total_ticks;
}

static void prv_collect(AnalyticsMetric metric, StopModeInhibitor inhibitor, RtcTicks now_ticks) {
  // operating on 64 bit values so the load/stores will _not_ be atomic
  portENTER_CRITICAL();
  RtcTicks ticks = prv_get_nostop_ticks(inhibitor, now_ticks);
  s_inhibitor_profile[inhibitor].total_ticks_while_disabled = 0;
  portEXIT_CRITICAL();
  analytics_set(metric, ticks_to_milliseconds(ticks), AnalyticsClient_System);
}

void analytics_external_collect_stop_inhibitor_stats(RtcTicks now_ticks) {
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_MAIN_TIME, InhibitorMain, now_ticks);
  // We don't care about the serial console nostop time, it should always
  // be zero on watches in the field anyway. (InhibitorDbgSerial skipped)
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_BUTTON_TIME, InhibitorButton, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_BLUETOOTH_TIME, InhibitorBluetooth, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_DISPLAY_TIME, InhibitorDisplay, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_BACKLIGHT_TIME, InhibitorBacklight, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_COMM_TIME, InhibitorCommMode, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_FLASH_TIME, InhibitorFlash, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_I2C1_TIME, InhibitorI2C1, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_ACCESSORY, InhibitorAccessory, now_ticks);
  prv_collect(ANALYTICS_DEVICE_METRIC_CPU_NOSTOP_MIC, InhibitorMic, now_ticks);
  // TODO PBL-37941: Add analytics for InhibitorDMA
}

void command_scheduler_force_active(void) {
  sleep_mode_enable(false);
}

void command_scheduler_resume_normal(void) {
  sleep_mode_enable(true);
}
