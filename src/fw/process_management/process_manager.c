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

#include "process_manager.h"
#include "app_install_manager.h"
#include "app_manager.h"
#include "worker_manager.h"

#include "applib/app_logging.h"
#include "applib/accel_service_private.h"
#include "applib/platform.h"
#include "applib/rockyjs/rocky_res.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/expandable_dialog.h"

#include "process_state/app_state/app_state.h"
#include "process_state/worker_state/worker_state.h"

#include "pebble_process_md.h"
#include "kernel/pebble_tasks.h"
#include "os/tick.h"
#include "resource/resource_ids.auto.h"
#include "services/common/animation_service.h"
#include "services/common/analytics/analytics.h"
#include "services/common/evented_timer.h"
#include "services/common/event_service.h"
#include "services/common/hrm/hrm_manager.h"
#include "services/normal/filesystem/pfs.h"
#include "services/common/system_task.h"
#include "services/normal/accessory/smartstrap_attribute.h"
#include "services/normal/app_cache.h"
#include "services/normal/data_logging/data_logging_service.h"
#include "services/normal/persist.h"
#include "services/normal/voice/voice.h"
#include "shell/normal/watchface.h"

#include "syscall/syscall.h"
#include "system/logging.h"
#include "system/passert.h"

#include "kernel/pbl_malloc.h"
#include "kernel/ui/modals/modal_manager.h"
#include "util/heap.h"

#include "syscall/syscall_internal.h"

#include "apps/system_apps/app_fetch_ui.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"


static TimerID s_deinit_timer_id = TIMER_INVALID_ID;


// -------------------------------------------------------------------------------------------
static ProcessContext *prv_get_context_for_task(PebbleTask task) {
  if (task == PebbleTask_App) {
    return app_manager_get_task_context();
  } else {
    PBL_ASSERTN(task == PebbleTask_Worker);
    return worker_manager_get_task_context();
  }
}


// -------------------------------------------------------------------------------------------
static ProcessContext *prv_get_context(void) {
  return prv_get_context_for_task(pebble_task_get_current());
}


// --------------------------------------------------------------------------------------------------
// This timer callback gets called if the process doesn't finish it's deinit within the required timeout (currently
// 3 seconds).
static void prv_graceful_close_timer_callback(void* data) {
  PBL_LOG(LOG_LEVEL_DEBUG, "deinit timeout expired, killing app forcefully");
  PebbleTask task = (PebbleTask)data;

  process_manager_put_kill_process_event(task, false /*gracefully*/);
}


// ---------------------------------------------------------------------------------------------
static bool prv_force_stop_task_if_unprivileged(ProcessContext *context) {
  vTaskSuspend((TaskHandle_t) context->task_handle);

  uint32_t control_reg = ulTaskDebugGetStackedControl((TaskHandle_t) context->task_handle);
  if ((control_reg & 0x1) == 0) {
    // We're privileged, it's not safe to just kill the app task.
    vTaskResume((TaskHandle_t) context->task_handle);
    return false;
  }

  context->safe_to_kill = true;
  return true;
}


// --------------------------------------------------------------------------------------------------
static void prv_force_close_timer_callback(void* data) {
  PebbleTask task = (PebbleTask)data;
  ProcessContext *context = prv_get_context_for_task(task);

  if (!prv_force_stop_task_if_unprivileged(context)) {
    PBL_CROAK("task stuck inside privileged code!");
  }
  process_manager_put_kill_process_event(task, false /*graceful*/);
}


// ---------------------------------------------------------------------------------------------
EXTERNALLY_VISIBLE void process_manager_handle_syscall_exit(void) {
  PebbleTask task = pebble_task_get_current();
  ProcessContext *context = prv_get_context_for_task(task);

  if (context->closing_state == ProcessRunState_ForceClosing) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Hit syscall exit trap!");
    context->safe_to_kill = true;
    process_manager_put_kill_process_event(task, false);

    vTaskSuspend(xTaskGetCurrentTaskHandle());
  }
}


// ---------------------------------------------------------------------------------------------
void process_manager_init(void) {
  s_deinit_timer_id = new_timer_create();
}


// -----------------------------------------------------------------------------------------------------------
void process_manager_put_kill_process_event(PebbleTask task, bool gracefully) {
  PebbleEvent event = {
    .type = PEBBLE_PROCESS_KILL_EVENT,
    .kill = {
      .gracefully = gracefully,
      .task = task,
    },
  };

  // When we have decided to exit the app,
  // it doesn't need to process any queued accel data
  // or other services before exiting, so clear the to_process_event_queue
  ProcessContext *context = prv_get_context_for_task(task);
  if (context->to_process_event_queue) {
    event_queue_cleanup_and_reset(context->to_process_event_queue);
  }

  // Since the app is about to exit, make sure the next (only)
  // message on the from app queue is the PEBBLE_APP_KILL_EVENT
  // to expedite exiting the application
  event_reset_from_process_queue(task);

  event_put_from_process(task, &event);
}


// ---------------------------------------------------------------------------------------------
//! Init the context variables for a task.
void process_manager_init_context(ProcessContext* context,
                                  const PebbleProcessMd *app_md, const void *args) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);

  PBL_ASSERTN(context->task_handle == NULL);
  PBL_ASSERTN(context->to_process_event_queue == NULL);

  context->app_md = app_md;

  AppInstallId install_id = app_install_get_id_for_uuid(&app_md->uuid);
  context->install_id = install_id;

  // we are safe to kill until the app main starts
  context->safe_to_kill = true;
  context->closing_state = ProcessRunState_Running;
  context->args = args;
  context->user_data = 0;

  // get app launch reason and wakeup_info
  context->launch_reason = app_manager_get_launch_reason();
  context->launch_button = app_manager_get_launch_button();
  context->wakeup_info = app_manager_get_app_wakeup_state();

  // set the default exit reason
  context->exit_reason = APP_EXIT_NOT_SPECIFIED;
}

#if !defined(RECOVERY_FW)
bool process_manager_check_SDK_compatible(const AppInstallId id) {
  AppInstallEntry entry;
  if (!app_install_get_entry_for_install_id(id, &entry)) {
    return false;
  }

  if (app_install_entry_is_SDK_compatible(&entry)) {
    return true;
  }

  PBL_LOG(LOG_LEVEL_WARNING, "App requires support for SDK version (%"PRIu8".%"PRIu8"), "
                             "we only support version (%"PRIu8".%"PRIu8").",
          entry.sdk_version.major, entry.sdk_version.minor,
          (uint8_t) PROCESS_INFO_CURRENT_SDK_VERSION_MAJOR,
          (uint8_t) PROCESS_INFO_CURRENT_SDK_VERSION_MINOR);

  ExpandableDialog *expandable_dialog = expandable_dialog_create("Incompatible SDK");
  Dialog *dialog = expandable_dialog_get_dialog(expandable_dialog);
  const char *error_text = i18n_noop("This app requires a newer version of the Pebble firmware.");
  dialog_set_text(dialog, i18n_get(error_text, expandable_dialog));
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_WARNING_SMALL);
  i18n_free(error_text, expandable_dialog);

  if (pebble_task_get_current() == PebbleTask_KernelMain) {
    WindowStack *window_stack = modal_manager_get_window_stack(ModalPriorityAlert);
    expandable_dialog_push(expandable_dialog, window_stack);
  } else {
    app_expandable_dialog_push(expandable_dialog);
  }

  return false;
}

static bool prv_needs_fetch(AppInstallId id, const PebbleProcessMd **md, bool is_worker) {
  PBL_ASSERTN(md);

  if (!app_cache_entry_exists(id)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Cache entry did not exist on launch attempt");
    return true;
  }

  *md = app_install_get_md(id, is_worker);

  if (!is_worker && rocky_app_validate_resources(*md) == RockyResourceValidation_Invalid) {
    PBL_LOG(LOG_LEVEL_DEBUG, "App has incompatible JavaScript bytecode");
    //  TODO: do we need to purge the app cache here?
    return true;
  }

  return false;
}

#endif

void process_manager_launch_process(const ProcessLaunchConfig *config) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  const AppInstallId id = config->id;
  const bool is_worker = config->worker;

  if (id == INSTALL_ID_INVALID) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Invalid ID");
    return;
  }

  const PebbleProcessMd *md = NULL;
#if !RECOVERY_FW
  if (app_install_id_from_app_db(id)) {
      if (!process_manager_check_SDK_compatible(id)) {
        return;
      }

    // This is a third party flash 3.0 app install
    if (prv_needs_fetch(id, &md, is_worker)) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Cache entry did not exist on launch attempt");

      // Freed in app_fetch_ui.c
      AppFetchUIArgs *fetch_args = kernel_malloc_check(sizeof(AppFetchUIArgs));
      *fetch_args = (AppFetchUIArgs){};
      fetch_args->common = config->common;
      fetch_args->app_id = id;
      fetch_args->forcefully = config->forcefully;

      // if the data is wakeup info, then copy out that information.
      if ((config->common.reason == APP_LAUNCH_WAKEUP) && (config->common.args != NULL)) {
        fetch_args->wakeup_info = *(WakeupInfo *)config->common.args;
        fetch_args->common.args = &fetch_args->wakeup_info;
      }

      PebbleEvent e = {
        .type = PEBBLE_APP_FETCH_REQUEST_EVENT,
        .app_fetch_request = {
          .id = id,
          .with_ui = true,
          .fetch_args = fetch_args,
        },
      };
      event_put(&e);
      return;
    } else {
      // tell the app cache that we are launching this application.
      app_cache_app_launched(id);
    }
  }
#endif
  // we either came here if PRF or if we didn't start a fetch
  // md is either already initialized, or we took a code path that didn't try
  if (!md) {
    md = app_install_get_md(id, is_worker);
  }

  if (!md) {
    PBL_LOG(LOG_LEVEL_ERROR, "Tried to launch non-existant app!");
    return;
  }

#if !RECOVERY_FW
  if (!is_worker) {
    // Check if the app ram size is valid in order to determine if its SDK version is supported.
    if (!app_manager_is_app_supported(md)) {
      PBL_LOG(LOG_LEVEL_WARNING, "Tried to launch an app with an unsupported SDK version.");
      AppInstallEntry entry;
      if (!app_install_get_entry_for_install_id(id, &entry)) {
        // can't retrieve app install entry for id
        PBL_LOG(LOG_LEVEL_ERROR, "Failed to get entry for id %"PRId32, id);
      } else if (app_install_entry_is_watchface(&entry)) {
        // If the watchface is for an unsupported SDK version, we need to switch the default
        // watchface back to tictoc. Otherwise, we will be stuck in the launcher forever.
        watchface_set_default_install_id(INSTALL_ID_INVALID);
        watchface_launch_default(NULL);
      }

      // Not going to launch this, release the allocated memory
      app_install_release_md(md);

      return;
    }
  }
#endif

  if (is_worker) {
    worker_manager_launch_new_worker_with_args(md, NULL);
  } else {
    app_manager_launch_new_app(&(AppLaunchConfig) {
      .md = md,
      .common = config->common,
      .forcefully = config->forcefully,
    });
  }
}

// ---------------------------------------------------------------------------------------------
extern void analytics_external_collect_app_cpu_stats(void);
extern void analytics_external_collect_app_flash_read_stats(void);
static void prv_handle_app_stop_analytics(const ProcessContext *const context,
                                          PebbleTask task, bool gracefully) {
  if (!gracefully) {
    if (task == PebbleTask_App) {
      if (context->app_md->is_rocky_app) {
        analytics_inc(ANALYTICS_APP_METRIC_ROCKY_CRASHED_COUNT, AnalyticsClient_App);
      }
      analytics_inc(ANALYTICS_APP_METRIC_CRASHED_COUNT, AnalyticsClient_App);
    } else if (task == PebbleTask_Worker) {
      analytics_inc(ANALYTICS_APP_METRIC_BG_CRASHED_COUNT, AnalyticsClient_Worker);
    }
    if (context->app_md->is_rocky_app) {
      analytics_inc(ANALYTICS_DEVICE_METRIC_APP_ROCKY_CRASHED_COUNT, AnalyticsClient_System);
    }
    analytics_inc(ANALYTICS_DEVICE_METRIC_APP_CRASHED_COUNT, AnalyticsClient_System);
  }
  if (task == PebbleTask_App) {
    analytics_stopwatch_stop(ANALYTICS_APP_METRIC_FRONT_MOST_TIME);
  }
  analytics_external_collect_app_cpu_stats();
  analytics_external_collect_app_flash_read_stats();
}


// ---------------------------------------------------------------------------------------------
//! This method returns true if the process is safe to kill (it has exited out of it's main function). If the
//! the process is not already safe to kill, it will "prod" it to exit, set a timer, and return false.
//!
//! The app manager and worker manager MUST call this before they call the code to kill the task and clean it up
//! (most of that work is done by process_manager_process_cleanup()). If it returns false, they should abort the
//! current process exit operation and wait for another KILL event to get posted.
//!
//! If the task does eventually fall through it's main function, the exit handling code will set the safe to kill
//! boolean and post another KILL event to the KernelMain which will result in this method being called again, and
//! this time it will see the safe to kill is set and return true
//!
//! If the task does not exit by itself before the timer expires, then the timer will post another KILL event
//! with graceful set to false. This will result in this method being called again with gracefully = false. When
//! we see this, we just try and make sure the app is not stuck in privilege code. If it's not, we return true
//! and allow the caller to kill the task.
//!
//! If however, the task is in privilege mode, we tell the syscall machinery to set the safe to kill boolean as
//! soon as the current syscall returns and set another timer. Once that timer expires, if the task is no longer
//! in privilege mode we post another KILL event (graceful = false). If the task is still in privilege mode then,
//! we croak.
bool process_manager_make_process_safe_to_kill(PebbleTask task, bool gracefully) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  ProcessContext *context = prv_get_context_for_task(task);

  // If already safe to kill, we're done
  if (context->safe_to_kill) {
    prv_handle_app_stop_analytics(context, task, gracefully);
    return true;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "make %s process safe to kill: state %u", pebble_task_get_name(task),
                              context->closing_state);

  if (gracefully) {
    if (context->closing_state == ProcessRunState_Running) {
      context->closing_state = ProcessRunState_GracefullyClosing;

      PBL_LOG(LOG_LEVEL_DEBUG, "Attempting to gracefully deinit %s", pebble_task_get_name(task));

      // Send deinit event to app:
      PebbleEvent deinit_event = {
        .type = PEBBLE_PROCESS_DEINIT_EVENT,
      };
      process_manager_send_event_to_process(task, &deinit_event);

      // Set a timer to forcefully close the app in 3 seconds if it doesn't respond by then. The app can respond
      // within 3 seconds by posting a PEBBLE_APP_KILL_EVENT (graceful=true), which will result in
      // app_manager_close_current_app() being called, which in turn calls this method with graceful = true.
      bool success = new_timer_start(s_deinit_timer_id, 3 * 1000, prv_graceful_close_timer_callback, (void*)task,
                        0 /*flags*/);
      PBL_ASSERTN(success);
    }
    // Else we're already in the gracefully closing state, just let the timer run out or the
    // app to mark itself as safe_to_kill.
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Check if we can force stop the %s task", pebble_task_get_name(task));

    if (prv_force_stop_task_if_unprivileged(context)) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Got it");
      prv_handle_app_stop_analytics(context, task, gracefully);
      return true;
    }

    // Non-graceful close
    if (context->closing_state == ProcessRunState_Running ||
        context->closing_state == ProcessRunState_GracefullyClosing) {
      // Right before a syscall drops privilege, it calls
      // process_manager_force_close_syscall_exit_trap to check whether
      // it is about to return control to a misbehaving app. This
      // function checks the process context's closing state and makes
      // the process safe to kill if its state is set to ForceClosing.
      // All we have to do is set the state and wait.
      context->closing_state = ProcessRunState_ForceClosing;
      PBL_LOG(LOG_LEVEL_DEBUG, "task is privileged, setting the syscall exit trap");

      bool success = new_timer_start(s_deinit_timer_id, 3 * 1000, prv_force_close_timer_callback, (void*)task,
              0 /*flags*/);
      PBL_ASSERTN(success);
    }
  }
  return false;
}

// -----------------------------------------------------------------------------------------------------------
// This is designed to be called from the task itself, in privilege mode, after it exits. It is called from
// app_task_exit for app tasks and worker_task_exit from worker tasks
NORETURN process_manager_task_exit(void) {
  PebbleTask task = pebble_task_get_current();
  ProcessContext *context = prv_get_context_for_task(task);

  // If this is not a system app, output its heap usage stats.
  if (context->app_md->process_storage == ProcessStorageFlash) {
    const Heap *heap;
    if (task == PebbleTask_App) {
      heap = app_state_get_heap();
    } else if (task == PebbleTask_Worker) {
      heap = worker_state_get_heap();
    } else {
      WTF;
    }

    // FIXME: We cast heap_size's size_t result to int because for some reason our printf doesn't
    // like the %zd formatter
    APP_LOG(APP_LOG_LEVEL_INFO, "Heap Usage for %s: Total Size <%dB> Used <%uB> Still allocated <%uB>",
        pebble_task_get_name(task), (int) heap_size(heap), heap->high_water_mark, heap->current_size);
  }

  // Let the task manager know we're done cleaning up.
  context->safe_to_kill = true;

  // Tell the task manager that we want to be killed. This may be redundant if we're responding to a DEINIT
  // message, but just in case we're exiting on our own (someone found the sys_exit syscall and called in when
  // we weren't expecting it?) we should let the app manager know.
  process_manager_put_kill_process_event(task, true);

  // Better to die in our sleep ...
  vTaskSuspend(NULL /* self */);

  // We don't expect someone to resume us.
  PBL_CROAK("");

}

// ---------------------------------------------------------------------------------------------
// Get the args for the current process
const void *process_manager_get_current_process_args(void) {
  return prv_get_context()->args;
}


// ---------------------------------------------------------------------------------------------
// Setup the system services required for this process. Called by app_manager and worker_manager
// right before we launch the task for the new process.
void process_manager_process_setup(PebbleTask task) {
  ProcessContext *context = prv_get_context_for_task(task);
  persist_service_client_open(&context->app_md->uuid);
}


// ---------------------------------------------------------------------------------------------
//! Kills the process, giving it no chance to clean things up or exit gracefully. The process must already be in a
//! state where it's safe to exit, so the caller must call process_manager_make_process_safe_to_kill() first and only
//! call this method if process_manager_make_process_safe_to_kill() returns true;
void process_manager_process_cleanup(PebbleTask task) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);

  ProcessContext *context = prv_get_context_for_task(task);
  PBL_ASSERTN(context->safe_to_kill);

  PBL_LOG(LOG_LEVEL_DEBUG, "%s is getting cleaned up", pebble_task_get_name(task));

  // Shutdown services that may be running. Do this before we destroy the task and clear the queue
  // just in case other services are still in flight.
  accel_service_cleanup_task_session(task);
  animation_service_cleanup(task);
  persist_service_client_close(&context->app_md->uuid);
  event_reset_from_process_queue(task);
  evented_timer_clear_process_timers(task);
  event_service_clear_process_subscriptions(task);

#if CAPABILITY_HAS_BUILTIN_HRM
  hrm_manager_process_cleanup(task, context->install_id);
#endif

#ifndef RECOVERY_FW
#if CAPABILITY_HAS_MICROPHONE
  voice_kill_app_session(task);
#endif
  dls_inactivate_sessions(task);

  if (task == PebbleTask_App) {
#if CAPABILITY_HAS_ACCESSORY_CONNECTOR
    smartstrap_attribute_unregister_all();
#endif
  }
#endif // RECOVERY_FW

  // Unregister the task
  pebble_task_unregister(task);

  new_timer_stop(s_deinit_timer_id);

  if (context->task_handle) {
    vTaskDelete(context->task_handle);
    context->task_handle = NULL;
  }

  // cleanup memory that was used to store the Md, but only if it isn't a system application
  app_install_release_md(context->app_md);

  // Clear the old app metadata
  context->app_md = 0;
  context->install_id = INSTALL_ID_INVALID;

  if (context->to_process_event_queue &&
      pdFAIL == event_queue_cleanup_and_reset(context->to_process_event_queue)) {
    PBL_LOG(LOG_LEVEL_ERROR, "The to process queue could not be reset!");
  }
  context->to_process_event_queue = NULL;
}



// -----------------------------------------------------------------------------------------------------------
void process_manager_close_process(PebbleTask task, bool gracefully) {
  if (task == PebbleTask_App) {
    // This will tell the app manager to switch to the last registered app.
    app_manager_close_current_app(gracefully);

  } else if (task == PebbleTask_Worker) {
    worker_manager_close_current_worker(gracefully);

  } else {
    WTF;
  }
}


// ----------------------------------------------------------------------------------------------
bool process_manager_send_event_to_process(PebbleTask task, PebbleEvent* e) {
  ProcessContext *context = prv_get_context_for_task(task);

  if (context->to_process_event_queue == 0) {
    PBL_LOG(LOG_LEVEL_WARNING, "Dropped app event! Type: %u", e->type);
    return false;
  }

  // Put on app's own queue:
  if (!xQueueSend(context->to_process_event_queue, e, milliseconds_to_ticks(1000))) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to send event %u to app! Closing it!", e->type);
    // We could be called from a timer task callback, so post a kill event rather than call
    //  process_manager_close_process directly.
    process_manager_put_kill_process_event(task, false);
    return false;
  }

  return true;
}


// ----------------------------------------------------------------------------------------------
uint32_t process_manager_process_events_waiting(PebbleTask task) {
  ProcessContext *context = prv_get_context_for_task(task);

  if (context->to_process_event_queue == 0) {
    PBL_LOG(LOG_LEVEL_WARNING, "no event queue");
    return 0;
  }

  return uxQueueMessagesWaiting(context->to_process_event_queue);
}


// ----------------------------------------------------------------------------------------------
void process_manager_send_callback_event_to_process(PebbleTask task, void (*callback)(void *data), void *data) {

  PBL_ASSERTN(callback != NULL);
  PebbleEvent event = {
    .type = PEBBLE_CALLBACK_EVENT,
    .callback = {
      .callback = callback,
      .data = data,
    },
  };
  process_manager_send_event_to_process(task, &event);
}

// ----------------------------------------------------------------------------------------------
void *process_manager_address_to_offset(PebbleTask task, void *system_address) {
  ProcessContext *context = prv_get_context_for_task(task);
  if (system_address >= context->load_start &&
      system_address < context->load_end) {
    return (void*)((uintptr_t) system_address - (uintptr_t)context->load_start);
  }
  // Not in app space:
  return system_address;
}


// ----------------------------------------------------------------------------------------------

extern char __APP_RAM__[];
extern char __APP_RAM_end__[];
extern char __WORKER_RAM__[];
extern char __WORKER_RAM_end__[];

bool process_manager_is_address_in_region(PebbleTask task, const void *address,
                                          const void *lower_bound) {
  void *ram_start = NULL, *ram_end = NULL;
  if (task == PebbleTask_App) {
    ram_start = __APP_RAM__;
    ram_end = __APP_RAM_end__;
  } else if (task == PebbleTask_Worker) {
    ram_start = __WORKER_RAM__;
    ram_end = __WORKER_RAM_end__;
  } else {
    WTF;
  }

  // check for vulnerability: lower_bound outside of task's region
  PBL_ASSERTN(lower_bound >= ram_start);

  return (address >= lower_bound && address < ram_end);
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(void, sys_get_pebble_event, PebbleEvent *event) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(event, sizeof(*event));
  }

  xQueueReceive(prv_get_context()->to_process_event_queue, event, portMAX_DELAY);
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(AppLaunchReason, sys_process_get_launch_reason, void) {
  return prv_get_context()->launch_reason;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(ButtonId, sys_process_get_launch_button, void) {
  return prv_get_context()->launch_button;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(uint32_t, sys_process_get_launch_args, void) {
  if (sys_process_get_launch_reason() != APP_LAUNCH_TIMELINE_ACTION) {
    return 0;
  } else {
    return (uint32_t) process_manager_get_current_process_args();
  }
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(AppExitReason, sys_process_get_exit_reason, void) {
  return prv_get_context()->exit_reason;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(void, sys_process_set_exit_reason, AppExitReason exit_reason) {
  // Just return if exit_reason is invalid
  if (exit_reason >= NUM_EXIT_REASONS) {
    return;
  }
  prv_get_context()->exit_reason = exit_reason;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(void, sys_process_get_wakeup_info, WakeupInfo *info) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(info, sizeof(*info));
  }
  *info = prv_get_context()->wakeup_info;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(const PebbleProcessMd*, sys_process_manager_get_current_process_md, void) {
  return prv_get_context()->app_md;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(bool, sys_process_manager_get_current_process_uuid, Uuid *uuid_out) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(uuid_out, sizeof(*uuid_out));
  }

  const PebbleProcessMd* app_md = prv_get_context()->app_md;
  if (!app_md) {
    return false;
  }
  *uuid_out = app_md->uuid;
  return true;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(AppInstallId, sys_process_manager_get_current_process_id, void) {
  return prv_get_context()->install_id;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(bool, process_manager_compiled_with_legacy2_sdk, void) {
  PebbleTask task = pebble_task_get_current();
  if (task != PebbleTask_App && task != PebbleTask_Worker) {
    return false;
  }

  const ProcessAppSDKType sdk_type =
      process_metadata_get_app_sdk_type(sys_process_manager_get_current_process_md());

  return sdk_type == ProcessAppSDKType_Legacy2x;
}

// -------------------------------------------------------------------------------------------
DEFINE_SYSCALL(bool, process_manager_compiled_with_legacy3_sdk, void) {
  PebbleTask task = pebble_task_get_current();
  if (task != PebbleTask_App && task != PebbleTask_Worker) {
    return false;
  }

  const ProcessAppSDKType sdk_type =
      process_metadata_get_app_sdk_type(sys_process_manager_get_current_process_md());

  return sdk_type == ProcessAppSDKType_Legacy3x;
}

DEFINE_SYSCALL(Version, sys_get_current_process_sdk_version, void) {
  return process_metadata_get_sdk_version(sys_process_manager_get_current_process_md());
}

DEFINE_SYSCALL(PlatformType, process_manager_current_platform, void) {
  PebbleTask task = pebble_task_get_current();
  if (task != PebbleTask_App && task != PebbleTask_Worker) {
    return PBL_PLATFORM_TYPE_CURRENT;
  }

  const PebbleProcessMd *const md = sys_process_manager_get_current_process_md();
  return process_metadata_get_app_sdk_platform(md);
}
