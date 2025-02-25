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

#include "system/reset.h"

#define CMSIS_COMPATIBLE
#include <mcu.h>

#include "board/board.h"
#include "drivers/pmic.h"
#include "system/bootbits.h"
#include "kernel/core_dump.h"
#include "kernel/util/fw_reset.h"
#include "mcu/interrupts.h"

#include "drivers/flash.h"
#include "system/reboot_reason.h"

#include "FreeRTOS.h"
#include "task.h"

void system_reset_prepare(bool unsafe_reset) {
  fw_prepare_for_reset(unsafe_reset);
  flash_stop();
}

NORETURN system_reset(void) {
  static bool failure_occurred = false;

  bool already_failed = failure_occurred;
  if (!failure_occurred) {
    // Don't overwrite failure_occurred if a failure has already occurred
    failure_occurred = boot_bit_test(BOOT_BIT_SOFTWARE_FAILURE_OCCURRED);
  }

  // Skip safe teardown if doing so the first time already caused a second reset attempt; or
  // if we're in a critical section, interrupt or if the scheduler has been suspended
  if (!already_failed && !mcu_state_is_isr() && !portIN_CRITICAL() &&
      (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)) {
    system_reset_prepare(failure_occurred /* skip BT teardown if failure occurred */);
    reboot_reason_set_restarted_safely();
  }

  // If a software failure occurred, do a core dump before resetting
  if (failure_occurred) {
    core_dump_reset(false /* don't force overwrite */);
  }

  system_hard_reset();
}

void system_reset_callback(void *data) {
  system_reset();
  (void)data;
}

NORETURN system_hard_reset(void) {
  // Don't do anything fancy here. We may be in a context where nothing works, not even
  // interrupts. Just reset us.

  NVIC_SystemReset();
  __builtin_unreachable();
}

