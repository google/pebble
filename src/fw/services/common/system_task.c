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

#include "services/common/system_task.h"

#include "system/logging.h"

#include "drivers/task_watchdog.h"
#include "kernel/pebble_tasks.h"
#include "kernel/util/task_init.h"
#include "mcu/fpu.h"
#include "os/tick.h"
#include "services/common/regular_timer.h"
#include "system/passert.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#define SYSTEM_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

typedef struct {
  SystemTaskEventCallback cb;
  void *data;
} SystemTaskEvent;

static QueueHandle_t s_system_task_queue;
static QueueHandle_t s_from_app_system_task_queue;

static QueueSetHandle_t s_system_task_queue_set;

static SystemTaskEventCallback s_current_cb;

static bool s_system_task_idle = true;
static bool s_should_block_callbacks = false;

static bool prv_is_accepting_callbacks() {
  return s_system_task_queue != 0 && !s_should_block_callbacks;
}

static void system_task_idle_timer_callback(void* data) {
  if (s_system_task_idle && uxQueueMessagesWaiting(s_system_task_queue_set) == 0) {
    system_task_watchdog_feed();
  }
}

static void system_task_main(void* parameter) {
  task_watchdog_mask_set(PebbleTask_KernelBackground);
  task_init();

  while (true) {
    s_system_task_idle = true;

    SystemTaskEvent event;

    QueueSetMemberHandle_t activated_queue = xQueueSelectFromSet(s_system_task_queue_set, portMAX_DELAY);

    // Get event from the activated queue
    portBASE_TYPE result = xQueueReceive(activated_queue, &event, 0);

    // I believe its possible that we just reset the queue and accidently
    // pended an extra event to the queue set so handle that case gracefully
    if (result) {
      s_system_task_idle = false;
      s_current_cb = event.cb;
      event.cb(event.data);
      mcu_fpu_cleanup();
      s_current_cb = NULL;
    }

    // Refresh the watchdog immediately, just in case that cb() took awhile to run.
    system_task_watchdog_feed();
  }
}

void system_task_init(void) {
  static const int SYSTEM_TASK_QUEUE_LENGTH = 30;
  static const int FROM_APP_SYSTEM_TASK_QUEUE_LENGTH = 8;

  s_system_task_queue = xQueueCreate(SYSTEM_TASK_QUEUE_LENGTH, sizeof(SystemTaskEvent));
  s_from_app_system_task_queue = xQueueCreate(FROM_APP_SYSTEM_TASK_QUEUE_LENGTH, sizeof(SystemTaskEvent));

  s_system_task_queue_set = xQueueCreateSet(SYSTEM_TASK_QUEUE_LENGTH + FROM_APP_SYSTEM_TASK_QUEUE_LENGTH);
  xQueueAddToSet(s_system_task_queue, s_system_task_queue_set);
  xQueueAddToSet(s_from_app_system_task_queue, s_system_task_queue_set);

  extern uint32_t __kernel_bg_stack_start__[];
  extern uint32_t __kernel_bg_stack_size__[];
  extern uint32_t __stack_guard_size__[];
  const uint32_t kernel_bg_stack_words = ( (uint32_t)__kernel_bg_stack_size__
                       - (uint32_t)__stack_guard_size__) / sizeof(portSTACK_TYPE);

  TaskParameters_t task_params = {
    .pvTaskCode = system_task_main,
    .pcName = "KernelBG",
    .usStackDepth = kernel_bg_stack_words,
    .uxPriority = SYSTEM_TASK_PRIORITY | portPRIVILEGE_BIT,
    .puxStackBuffer = (void*)(uintptr_t)((uint32_t)__kernel_bg_stack_start__
                                          + (uint32_t)__stack_guard_size__)
  };

  pebble_task_create(PebbleTask_KernelBackground, &task_params, NULL);
}

void system_task_timer_init(void) {
  // Register a regular timer to kick the watchdog while we're waiting for something
  // to do. The other way to do this is to have the xQueueReceive in system_task_main timeout
  // occasionally, but that isn't necessarily second aligned and will require the watch
  // to wakeup from sleep just to kick the watchdog. This way it's kicked at the same time as
  // all the other regular tasks. Note that the system_task_idle_timer_callback only kicks
  // the watchdog if we're currently waiting for work to do on the system_task. If we're in the
  // middle of something we won't kick it.
  static RegularTimerInfo idle_watchdog_timer = {
    .cb = system_task_idle_timer_callback
  };
  regular_timer_add_seconds_callback(&idle_watchdog_timer);
}

void system_task_watchdog_feed(void) {
  task_watchdog_bit_set(PebbleTask_KernelBackground);
}

static void handle_system_task_send_failure(SystemTaskEventCallback cb) {
  register uintptr_t lr __asm("lr");
  uintptr_t saved_lr = lr;

  PBL_LOG(LOG_LEVEL_ERROR, "System task queue full. Dropped cb: %p, current cb: %p", cb, s_current_cb);

  RebootReason reason = {
    .code = RebootReasonCode_EventQueueFull,
    .event_queue = {
      .destination_task = PebbleTask_KernelBackground,
      .push_lr = (uint32_t) saved_lr,
      .current_event = (uint32_t) s_current_cb,
      .dropped_event = (uint32_t) cb
    }
  };
  reboot_reason_set(&reason);

  reset_due_to_software_failure();
}

bool system_task_add_callback_from_isr(SystemTaskEventCallback cb, void *data, bool* should_context_switch) {
  if (!prv_is_accepting_callbacks()) {
    return false;
  }
  SystemTaskEvent event = {
    .cb = cb,
    .data = data,
  };

  signed portBASE_TYPE tmp;
  bool success = (xQueueSendToBackFromISR(s_system_task_queue, &event, &tmp) == pdTRUE);
  if (!success) {
    handle_system_task_send_failure(cb);
  }

  *should_context_switch = (tmp == pdTRUE);

  return success;
}

bool system_task_add_callback(SystemTaskEventCallback cb, void *data) {
  if (!prv_is_accepting_callbacks()) {
    return false;
  }

  SystemTaskEvent event = {
    .cb = cb,
    .data = data,
  };

  if (pebble_task_get_current() == PebbleTask_App) {
    // If we're the app and we've filled up our system task, the app just gets to wait.
    // FIXME: In the future when we want to bound the amount of time a syscall can take this will have to change.
    xQueueSendToBack(s_from_app_system_task_queue, &event, portMAX_DELAY);
    return true;
  } else {
    // Back ourselves up and wait a reasonable amount of time before failing. If the queue is really backed up
    // we want to fall through to the handle_system_task_send_failure and not just get killed by the watchdog.
    bool success = (xQueueSendToBack(s_system_task_queue, &event, milliseconds_to_ticks(3000)) == pdTRUE);
    if (!success) {
      handle_system_task_send_failure(cb);
    }
  }

  return true;
}

void system_task_block_callbacks(bool block) {
  s_should_block_callbacks = block;
}

uint32_t system_task_get_available_space(void) {
  const bool is_app = pebble_task_get_current() == PebbleTask_App;
  return uxQueueSpacesAvailable(is_app ? s_from_app_system_task_queue : s_system_task_queue);
}

void* system_task_get_current_callback(void) {
  return s_current_cb;
}

void system_task_enable_raised_priority(bool is_raised) {
  const uint32_t raised_priority_level = tskIDLE_PRIORITY + 3; // Same as KernelMain / BT tasks
  vTaskPrioritySet(pebble_task_get_handle_for_task(PebbleTask_KernelBackground),
                   (is_raised ? raised_priority_level : SYSTEM_TASK_PRIORITY) | portPRIVILEGE_BIT);
}

bool system_task_is_ready_to_run(void) {
  const eTaskState bg_task_state =
        eTaskGetState(pebble_task_get_handle_for_task(PebbleTask_KernelBackground));
  // check if system task is ready to go (instead of e.g. waiting for a mutex)
  return (bg_task_state == eReady);
}
