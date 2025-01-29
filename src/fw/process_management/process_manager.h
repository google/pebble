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

#pragma once

#include "launch_config.h"

#include "applib/app_exit_reason.h"
#include "applib/platform.h"
#include "kernel/events.h"
#include "process_management/app_install_types.h"
#include "process_management/pebble_process_md.h"
#include "services/common/accel_manager.h"
#include "services/common/compositor/compositor.h"

#include <stdbool.h>

// Used to identify invalid or system app when using app_bank calls
#define INVALID_BANK_ID ~0
#define SYSTEM_APP_BANK_ID (~0 - 1)

typedef enum ProcessRunState {
  ProcessRunState_Running,
  ProcessRunState_GracefullyClosing,
  ProcessRunState_ForceClosing
} ProcessRunState;

typedef struct ProcessContext {
  //! The app metadata structure if this process represents a running application (NULL otherwise). Describes the
  //! static information about the currently running app.
  const PebbleProcessMd* app_md;

  //! The app install id for this process if it represents an application.
  AppInstallId install_id;

  //! The FreeRTOS task we're using the run the app. See xTaskHandle
  void* task_handle;

  //! The address range the process was loaded into. It is used to
  //! convert physical addresses into relative addresses in order to
  //! assist developers when debugging their apps.
  void *load_start;
  void *load_end;

  //! Queue used to send events to the process. The process will read PebbleEvents from here using
  //! sys_get_pebble_event.
  void* to_process_event_queue;

  //! This bool indicates that we can safely stop and delete the process without causing
  //! any instability to the rest of the system. This is set in both graceful (process closing
  //! and returning) and non-graceful (force closes or crashes) cases. The task itself
  //! is the only task that's allowed to set this to true.
  volatile bool safe_to_kill;

  //! Used to provide the application the method used to launch the application
  AppLaunchReason launch_reason;

  //! The button information that launched the app used to provide the app this information
  ButtonId launch_button;

  //! Used to allow the application to specify the reason it exited
  AppExitReason exit_reason;

  //! Used to provide the application the wakeup_event that launched the application
  WakeupInfo wakeup_info;

  //! What state the process is currently running in. Managed by the app_manager/worker_manager
  ProcessRunState closing_state;

  //! Arguments passed to the process'. This is a pointer to a struct
  //! that is defined by the application.
  const void* args;

  //! Pointer to a piece of data that we hold on behalf of the process. This makes it so
  //! our first party apps don't have to keep declaring their own statics to hold a pointer
  //! to a struct to represent their app data. Third party apps don't have this issue as their
  //! globals are loaded and unloaded when they start and stop, where the globals for our first
  //! party apps are always present. See app_state_get_user_data/app_state_set_user_data
  void* user_data;
} ProcessContext;

typedef struct ProcessLaunchConfig {
  LaunchConfigCommon common;
  AppInstallId id;
  //! true if we're launching the worker for this app ID, false if we're launching the app.
  bool worker;
  //! true if the previous app should be closed forcefully, false otherwise.
  bool forcefully;
} ProcessLaunchConfig;


//! Init the process manager
void process_manager_init(void);

//! Init the context variables
void process_manager_init_context(ProcessContext* context,
                                  const PebbleProcessMd *app_md, const void *args);

//! Checks if the app refered to by the given AppInstallId is SDK compatible
//! Returns true if it is compatible, false otherwise
//! Shows an error dialog if it is not compatible
bool process_manager_check_SDK_compatible(const AppInstallId id);

//! Request that a process be launched.
//! @param id The app to launch. Note that this app may not be cached
//! @param args The args to the app
//! @param animation The animation that should be used to show the app
//! @param launch_reason The launch reason for the app starting
void process_manager_launch_process(const ProcessLaunchConfig *config);

//! Close the given process. This is the highest level call which transfers control to the manager of that type of
//! task. That manager will in turn call process_manager_make_process_safe_to_kill/process_manager_process_cleanup
//! If it's an app, this will cause the next app in the system app state machine to automatically get launched.
//! May only be called from the KernelMain task
void process_manager_close_process(PebbleTask task, bool gracefully);

//! Called from the task itself, as it exits and reenters privileged mode
NORETURN process_manager_task_exit(void);

//! Prod the given process into exiting. Returns true if it is safe to kill that task and clean it up using
//!  process_manager_process_cleanup
bool process_manager_make_process_safe_to_kill(PebbleTask task, bool gracefully);


//! Setup some required state for processes
void process_manager_process_setup(PebbleTask task);

//! Kills the task and cleans it up. Must only be called if process_manager_make_process_safe_to_kill() returns
//! true.
void process_manager_process_cleanup(PebbleTask task);

//! Get the args for the current process
const void* process_manager_get_current_process_args(void);

//! Send an event to the process' event loop
bool process_manager_send_event_to_process(PebbleTask task, PebbleEvent* e);

//! Get the number of messages currently waiting to be processed by the process
uint32_t process_manager_process_events_waiting(PebbleTask task);

//! Wrapper for \ref send_event_to_app to send callback events to the app
void process_manager_send_callback_event_to_process(PebbleTask task, void (*callback)(void *), void *data);

//! Send a kill event for the given process to KernelMain
void process_manager_put_kill_process_event(PebbleTask task, bool gracefully);

//! Right before a syscall function drops privilege, it calls this
//! function to check whether the kernel is trying to kill the process.
//! It offers us a place that we know is unprivileged for us to safely
//! suspend the task and shut it down if it is misbehaving.
void process_manager_handle_syscall_exit(void);

//! Return true if the current process was compiled with any 2.x SDK
bool process_manager_compiled_with_legacy2_sdk(void);

//! Return true if the current process was compiled with any 3.x SDK
bool process_manager_compiled_with_legacy3_sdk(void);

//! Returns platform the application was compiled against
//! Use this to test, e.g. if app is Aplite while being executed on Basalt
//! or Basalt while being executed on Emery
PlatformType process_manager_current_platform(void);

//! Check if address is between lower_bound and the end of task's memory region.
//! lower_bound is used by syscall_assert_userspace_buffer to ensure that
//! address doesn't point into the stack frame of the executing syscall
bool process_manager_is_address_in_region(PebbleTask task, const void *address,
                                          const void *lower_bound);

void *process_manager_address_to_offset(PebbleTask task, void *system_address);
