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

#include "kernel_applib_state.h"

#include "applib/ui/layer.h"
#include "mcu/interrupts.h"
#include "os/mutex.h"

#include "FreeRTOS.h"
#include "task.h"

static PebbleRecursiveMutex *s_log_state_mutex = INVALID_MUTEX_HANDLE;
static bool s_log_state_task_entered[NumPebbleTask];   // which tasks have entered


// ---------------------------------------------------------------------------------------------
CompassServiceConfig **kernel_applib_get_compass_config(void) {
  static CompassServiceConfig *s_compass_config;
  return &s_compass_config;
}

// --------------------------------------------------------------------------------------------
AnimationState* kernel_applib_get_animation_state(void) {
  static AnimationState s_kernel_animation_state;
  return &s_kernel_animation_state;
}

// Get the current task. If FreeRTOS has not been initialized yet, set to KernelMain
static PebbleTask prv_get_current_task(void) {
  if (pebble_task_get_handle_for_task(PebbleTask_KernelMain) == NULL) {
    return PebbleTask_KernelMain;
  } else {
    return pebble_task_get_current();
  }
}

// --------------------------------------------------------------------------------------------
// Return a pointer to the LogState to use for kernel (non app task) code. The LogState contains
// the buffers for formatting the log message.
// Returns NULL if a kernel logging operation is already in progress
LogState *kernel_applib_get_log_state(void) {
  static LogState sys_log_state;
  bool use_mutex;

  // Return right away if we re-entered from the same task For example, if we hit an assert while
  // trying to grab the s_log_state_mutex mutex below and tried to log an error.
  PebbleTask task = prv_get_current_task();
  if (s_log_state_task_entered[task]) {
      return NULL;
  }
  s_log_state_task_entered[task] = true;


  // We have 3 possible phases of operation:
  //   1.) Before FreeRTOS has been initialized - only 1 "task", no mutexes available
  //   2.) After FreeRTOS, but before our mutex has been created (via kernel_applib_init())
  //   3.) After our mutex has been created.
  // In phase 1, we don't bother taking the mutex but still log
  // In phase 2, we just return without logging. It is too dangerous to have
  //  possibly multiple tasks using logging without mutex support
  // In phase 3, we log after locking the mutex only.
  // Note, if we are in an ISR or critical section in any of these phases, we cannot use a mutex
  if ((pebble_task_get_handle_for_task(PebbleTask_KernelMain) == NULL) || mcu_state_is_isr()
        || portIN_CRITICAL() || (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)) {
    // phase 1 || in an ISR || in a critical section
    use_mutex = false;
  } else if (s_log_state_mutex == INVALID_MUTEX_HANDLE) {
    // phase 2
    dbgserial_putstr("LOGGING DISABLED");
    goto exit_fail;
  } else {
    // phase 3
    use_mutex = true;
  }

  if (use_mutex) {
    // Logging operations shouldn't take long to complete. Use a timeout in case we run into
    // an unlikely deadlock situation (one task doing a synchronous log to flash and another task
    // trying to log from flash code)
    bool success = mutex_lock_recursive_with_timeout(s_log_state_mutex, 1000);
    if (!success) {
      dbgserial_putstr("kernel_applib_get_log_state timeout error");
      goto exit_fail;
    }
  }

  // Return if re-entered (logging while logging). This can happen for example if one task
  // grabbed the context from an ISR or critical section and another grabbed it using the mutex
  if (sys_log_state.in_progress) {
    if (use_mutex) {
      mutex_unlock_recursive(s_log_state_mutex);
    }
    goto exit_fail;
  }

  sys_log_state.in_progress = true;
  return &sys_log_state;

exit_fail:
  s_log_state_task_entered[task] = false;
  return NULL;
}


// --------------------------------------------------------------------------------------------
// Release the LogState buffer obtained by kernel_applib_get_log_state()
void kernel_applib_release_log_state(LogState *state) {
  state->in_progress = false;

  // For phase 1 & when in an ISR, there is no mutex available
  if (!portIN_CRITICAL() && !mcu_state_is_isr()  &&
      (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) &&
      (s_log_state_mutex != INVALID_MUTEX_HANDLE)) {
    mutex_unlock_recursive(s_log_state_mutex);
  }

  // Clear the reentrancy flag for this task
  PebbleTask task = prv_get_current_task();
  s_log_state_task_entered[task] = false;
}


// ---------------------------------------------------------------------------------------------
EventServiceInfo* kernel_applib_get_event_service_state(void) {
  static EventServiceInfo s_event_service_state;
  return &s_event_service_state;
}

// --------------------------------------------------------------------------------------------
TickTimerServiceState* kernel_applib_get_tick_timer_service_state(void) {
  static TickTimerServiceState s_tick_timer_service_state;
  return &s_tick_timer_service_state;
}

// -----------------------------------------------------------------------------------------------------------
ConnectionServiceState* kernel_applib_get_connection_service_state(void) {
  static ConnectionServiceState s_connection_service_state;
  return &s_connection_service_state;
}

// -----------------------------------------------------------------------------------------------------------
BatteryStateServiceState* kernel_applib_get_battery_state_service_state(void) {
  static BatteryStateServiceState s_battery_state_service_state;
  return &s_battery_state_service_state;
}

Layer** kernel_applib_get_layer_tree_stack(void) {
  static Layer* layer_tree_stack[LAYER_TREE_STACK_SIZE];
  return layer_tree_stack;
}

// -------------------------------------------------------------------------------------------------------------
void kernel_applib_init(void) {
  s_log_state_mutex = mutex_create_recursive();
  connection_service_state_init(kernel_applib_get_connection_service_state());
  battery_state_service_state_init(kernel_applib_get_battery_state_service_state());
}


