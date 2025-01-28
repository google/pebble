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

#include "app_manager.h"
#include "worker_manager.h"
#include "process_loader.h"

// Pebble stuff
#include "applib/app_launch_reason.h"
#include "applib/app_message/app_message_internal.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/simple_dialog.h"
#include "applib/ui/window_stack.h"
#include "apps/system_app_ids.h"
#include "console/prompt.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/kernel_ui.h"
#include "kernel/ui/modals/modal_manager.h"
#include "kernel/util/segment.h"
#include "kernel/util/task_init.h"
#include "mcu/cache.h"
#include "mcu/privilege.h"
#include "os/mutex.h"
#include "popups/health_tracking_ui.h"
#include "popups/timeline/peek.h"
#include "process_management/app_run_state.h"
#include "process_management/pebble_process_md.h"
#include "process_management/process_heap.h"
#include "process_management/sdk_memory_limits.auto.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource.h"
#include "resource/resource_ids.auto.h"
#include "resource/resource_mapped.h"
#include "services/common/analytics/analytics.h"
#include "services/common/compositor/compositor_transitions.h"
#include "services/common/i18n/i18n.h"
#include "services/common/light.h"
#include "services/normal/app_cache.h"
#include "services/normal/app_inbox_service.h"
#include "services/normal/app_outbox_service.h"
#include "shell/normal/app_idle_timeout.h"
#include "shell/normal/watchface.h"
#include "shell/shell.h"
#include "shell/system_app_state_machine.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"

// FreeRTOS stuff
#include "FreeRTOS.h"
#include "freertos_application.h"
#include "task.h"
#include "queue.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETURN_CRASH_TIMEOUT_TICKS  (60 * RTC_TICKS_HZ)

//! Behold! The file that manages applications!
//!
//! The code in this file applies to all apps, whether they're third party apps (stored in SPI flash) or first
//! party apps stored inside our firmware.
//!
//! Apps are only started and stopped on the launcher task (aka kernel main).

extern char __APP_RAM__[];
extern char __APP_RAM_end__[];
extern char __stack_guard_size__[];

//! Used by the "pebble gdb" command to locate the loaded app in memory.
void * volatile g_app_load_address;

static const int MAX_TO_APP_EVENTS = 32;
static QueueHandle_t s_to_app_event_queue;
static ProcessContext s_app_task_context;
static ProcessAppRunLevel s_minimum_run_level;

typedef struct NextApp {
  LaunchConfigCommon common;
  const PebbleProcessMd *md;
  WakeupInfo wakeup_info;
} NextApp;

typedef struct {
  AppInstallId install_id;
  RtcTicks crash_ticks;
} AppCrashInfo;

static NextApp s_next_app;

static void prv_handle_app_start_analytics(const PebbleProcessMd *app_md,
                                           const AppLaunchReason launch_reason);

// ---------------------------------------------------------------------------------------------
void app_manager_init(void) {
  s_to_app_event_queue = xQueueCreate(MAX_TO_APP_EVENTS, sizeof(PebbleEvent));

  s_app_task_context = (ProcessContext) { 0 };
}

// ---------------------------------------------------------------------------------------------
bool app_manager_is_initialized(void) {
  return s_to_app_event_queue != NULL;
}

static bool s_first_app_launched = false;
bool app_manager_is_first_app_launched(void) {
  return s_first_app_launched;
}

WakeupInfo app_manager_get_app_wakeup_state(void) {
  return s_next_app.wakeup_info;
}

// ---------------------------------------------------------------------------------------------
//! This is the wrapper function for all apps here. It's not allowed to return as it's
//! the top frame on the stack created for the application.
static void prv_app_task_main(void *entry_point) {
  app_state_init();
  task_init();

  // about to start the app in earnest. No longer safe to kill.
  s_app_task_context.safe_to_kill = false;

  // Enter unprivileged mode!
  const bool is_unprivileged = s_app_task_context.app_md->is_unprivileged;

  // There are currently no Rocky.js APIs that need to be called while in privileged mode, so run
  // in unprivileged mode for the built-in Rocky.js apps (Tictoc) as well:
  const bool is_rocky_app = s_app_task_context.app_md->is_rocky_app;

  if (is_unprivileged || is_rocky_app) {
    mcu_state_set_thread_privilege(false);
  }

  const PebbleMain main_func = entry_point;
  main_func();

  // Clean up after the app.  Remember to put only non-critical cleanup here,
  // as the app may crash or otherwise misbehave. If something really needs to
  // be cleaned up, make it so the kernel can do it on the apps behalf and put
  // the call at the bottom of prv_app_cleanup.
  app_state_deinit();
#ifndef RECOVERY_FW
  app_message_close();
#endif

  sys_exit();
}

//! Heap locking function for our app heap. Our process heaps don't actually
//! have to be locked because they're the sole property of the process and no
//! other tasks should be touching it. All this function does is verify that
//! this condition is met before continuing without locking.
static void prv_heap_lock(void* unused) {
  PBL_ASSERT_TASK(PebbleTask_App);
}

void prv_dump_start_app_info(const PebbleProcessMd *app_md) {
  char *app_type = "";
  switch (process_metadata_get_app_sdk_type(app_md)) {
    case ProcessAppSDKType_System:
      app_type = "system";
      break;
    case ProcessAppSDKType_Legacy2x:
      app_type = "legacy2";
      break;
    case ProcessAppSDKType_Legacy3x:
      app_type = "legacy3";
      break;
    case ProcessAppSDKType_4x:
      app_type = "4.x";
      break;
  }

  char *const sdk_platform = platform_type_get_name(process_metadata_get_app_sdk_platform(app_md));

  PBL_LOG(LOG_LEVEL_DEBUG, "Starting %s app <%s>", app_type, process_metadata_get_name(app_md));
  // new logging only allows for 2 %s per format string...
  PBL_LOG(LOG_LEVEL_DEBUG, "Starting app with sdk platform %s", sdk_platform);
}

#define APP_STACK_ROCKY_SIZE (8 * 1024)
#define APP_STACK_NORMAL_SIZE (2 * 1024)

static size_t prv_get_app_segment_size(const PebbleProcessMd *app_md) {
  switch (process_metadata_get_app_sdk_type(app_md)) {
    case ProcessAppSDKType_Legacy2x:
      return APP_RAM_2X_SIZE;
    case ProcessAppSDKType_Legacy3x:
      return APP_RAM_3X_SIZE;
    case ProcessAppSDKType_4x:
#if CAPABILITY_HAS_JAVASCRIPT
      if (app_md->is_rocky_app) {
        // on Spalding, we didn't have enough applib padding to guarantee both,
        // 4.x native app heap + JerryScript statis + increased stack for Rocky.
        // For now, we just decrease the amount of available heap as we don't use it.
        // In the future, we will move the JS stack to the heap PBL-35783,
        // make byte code swappable PBL-37937,and remove JerryScript's static PBL-40400.
        // All of the above will work to our advantage so it's safe to make this simple
        // change now.
        return APP_RAM_4X_SIZE - (APP_STACK_ROCKY_SIZE - APP_STACK_NORMAL_SIZE);
      }
#endif
      return APP_RAM_4X_SIZE;
    case ProcessAppSDKType_System:
      return APP_RAM_SYSTEM_SIZE;
    default:
      WTF;
  }
}

static size_t prv_get_app_stack_size(const PebbleProcessMd *app_md) {
#if CAPABILITY_HAS_JAVASCRIPT
  if (app_md->is_rocky_app) {
    return APP_STACK_ROCKY_SIZE;
  }
#endif
  return APP_STACK_NORMAL_SIZE;
}

T_STATIC MemorySegment prv_get_app_ram_segment(void) {
  return (MemorySegment) { __APP_RAM__, __APP_RAM_end__ };
}

T_STATIC size_t prv_get_stack_guard_size(void) {
  return (uintptr_t)__stack_guard_size__;
}

// ---------------------------------------------------------------------------------------------
//! @return True on success, False if:
//!     - We fail to start the app. No app is running and the caller is responsible for starting
//!       a different app.
//!
//! @note Side effects: trips assertions if:
//!     - The app manager was not init,
//!     - The app's task handle or event queue aren't null
//!     - The app's metadata is null
static bool prv_app_start(const PebbleProcessMd *app_md, const void *args,
    const AppLaunchReason launch_reason) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  PBL_ASSERTN(app_md);

  prv_dump_start_app_info(app_md);

  process_manager_init_context(&s_app_task_context, app_md, args);

  // Set up the app's memory and load the app into it.
  size_t app_segment_size = prv_get_app_segment_size(app_md);
  // The stack guard is counted as part of the app segment size...
  const size_t stack_guard_size = prv_get_stack_guard_size();
  // ...and is carved out of the stack.
  const size_t stack_size = prv_get_app_stack_size(app_md) - stack_guard_size;

  MemorySegment app_ram = prv_get_app_ram_segment();

#if !UNITTEST
  if (app_md->is_rocky_app) {
    /* PBL-40376: Temp hack: put .rocky_bss at end of APP_RAM:
       Interim solution until all statics are removed from applib & jerry.
       These statics are only used for rocky apps, so it's OK that this overlaps/overlays with the
       app heap for non-rocky apps.
     */
    extern char __ROCKY_BSS_size__[];
    extern char __ROCKY_BSS__[];
    memset(__ROCKY_BSS__, 0, (size_t)__ROCKY_BSS_size__);

    // ROCKY_BSS is inside APP_RAM to make the syscall buffer checks pass.
    // However, we want to avoid overlapping with any splits we're about to make:
    app_ram.end = __ROCKY_BSS__;

    // Reduce the size available for the code + app heap, on Spalding the "padding" we had left
    // isn't enough to fit Rocky + Jerry's .bss:
    app_segment_size -= 1400;
  }
#endif

  memset((char *)app_ram.start + stack_guard_size, 0,
         memory_segment_get_size(&app_ram) - stack_guard_size);

  MemorySegment app_segment;
  PBL_ASSERTN(memory_segment_split(&app_ram, &app_segment, app_segment_size));
  PBL_ASSERTN(memory_segment_split(&app_segment, NULL, stack_guard_size));
  // No (accessible) memory segments can be placed between the top of APP_RAM
  // and the end of stack. Stacks always grow towards lower memory addresses, so
  // we want a stack overflow to touch the stack guard region before it begins
  // to clobber actual data. And syscalls assume that the stack is always at the
  // top of APP_RAM; violating this assumption will result in syscalls sometimes
  // failing when the app hasn't done anything wrong.
  portSTACK_TYPE *stack = memory_segment_split(&app_segment, NULL, stack_size);
  PBL_ASSERTN(stack);
  s_app_task_context.load_start = app_segment.start;
  g_app_load_address = app_segment.start;
  void *entry_point = process_loader_load(app_md, PebbleTask_App, &app_segment);
  s_app_task_context.load_end = app_segment.start;
  if (!entry_point) {
    PBL_LOG(LOG_LEVEL_WARNING, "Tried to launch an invalid app in bank %u!",
        process_metadata_get_code_bank_num(app_md));
    return false;
  }

  const ResAppNum res_bank_num = process_metadata_get_res_bank_num(app_md);
  if (res_bank_num != SYSTEM_APP) {
    const ResourceVersion res_version = process_metadata_get_res_version(app_md);
    // for RockyJS apps, we initialize without checking the for a match between
    // binary's copy of the resource CRC and the actual CRC as it could be outdated
    const ResourceVersion *const res_version_ptr = app_md->is_rocky_app ? NULL : &res_version;
    if (!resource_init_app(res_bank_num, res_version_ptr)) {
      // The resources are busted! Abort starting this app.
      APP_LOG(APP_LOG_LEVEL_ERROR,
              "Checksum for resources differs or insufficient meta data for JavaScript app.");
      return false;
    }
  }

  // Synchronously handle process start since its new state is needed for app state initialization
  timeline_peek_handle_process_start();

  const ProcessAppSDKType sdk_type = process_metadata_get_app_sdk_type(app_md);

  // The rest of app_ram is available for app_state to use as it sees fit.
  if (!app_state_configure(&app_ram, sdk_type, timeline_peek_get_obstruction_origin_y())) {
    PBL_LOG(LOG_LEVEL_ERROR, "App state configuration failed");
    return false;
  }
  // The remaining space in app_segment is assigned to the app's heap.
  // app_state needs to be configured before initializing the app heap
  // as the AppState struct holds the app heap's Heap object.

  // Don't fuzz 3rd party app heaps because likely many of them rely on accessing free'd memory
  bool enable_heap_fuzzing = (sdk_type == ProcessAppSDKType_System);
  Heap *app_heap = app_state_get_heap();
  PBL_LOG(LOG_LEVEL_DEBUG, "App heap init %p %p",
          app_segment.start, app_segment.end);
  heap_init(app_heap, app_segment.start, app_segment.end, enable_heap_fuzzing);
  heap_set_lock_impl(app_heap, (HeapLockImpl) {
      .lock_function = prv_heap_lock,
  });
  process_heap_set_exception_handlers(app_heap, app_md);

  // We're now going to start the app. We can't abort the app now without calling prv_app_cleanup.

  // If it's a watchface and we were launched by the phone or the user, make it the new default.
  if ((s_app_task_context.install_id != INSTALL_ID_INVALID) &&
      ((launch_reason == APP_LAUNCH_PHONE) || (launch_reason == APP_LAUNCH_USER))) {
    AppInstallEntry entry;
    if (!app_install_get_entry_for_install_id(s_app_task_context.install_id, &entry)) {
      // cant retrieve app install entry for id
      PBL_LOG(LOG_LEVEL_ERROR, "Failed to get entry for id %"PRId32, s_app_task_context.install_id);
      return false;
    }
    if (app_install_entry_is_watchface(&entry) && !app_install_entry_is_hidden(&entry)) {
      watchface_set_default_install_id(entry.install_id);
    }
  }

  app_manager_set_minimum_run_level(process_metadata_get_run_level(app_md));

  // Use the static app event queue:
  s_app_task_context.to_process_event_queue = s_to_app_event_queue;

  // Init services required for this process before it starts to execute
  process_manager_process_setup(PebbleTask_App);

  char task_name[configMAX_TASK_NAME_LEN];
  snprintf(task_name, sizeof(task_name), "App <%s>", process_metadata_get_name(s_app_task_context.app_md));

  TaskParameters_t task_params = {
    .pvTaskCode = prv_app_task_main,
    .pcName = task_name,
    .usStackDepth = stack_size / sizeof(portSTACK_TYPE),
    .pvParameters = entry_point,
    .uxPriority = APP_TASK_PRIORITY | portPRIVILEGE_BIT,
    .puxStackBuffer = stack,
  };

  PBL_LOG(LOG_LEVEL_DEBUG, "Starting %s", task_name);

  // Store slot of launched app for reboot support (flash apps only)
  reboot_set_slot_of_last_launched_app(
      (app_md->process_storage == ProcessStorageFlash) ?
          process_metadata_get_code_bank_num(app_md) : SYSTEM_APP_BANK_ID);

  pebble_task_create(PebbleTask_App, &task_params, &s_app_task_context.task_handle);

  // Always notify the phone that the application is running
  app_run_state_send_update(&app_md->uuid, RUNNING);

  system_app_state_machine_register_app_launch(s_app_task_context.install_id);

  prv_handle_app_start_analytics(app_md, launch_reason);

#if CAPABILITY_HAS_HEALTH_TRACKING && !defined(RECOVERY_FW)
  health_tracking_ui_register_app_launch(s_app_task_context.install_id);
#endif

  return true;
}

// ---------------------------------------------------------------------------------------------
//! Kills the app, giving it no chance to clean things up or exit gracefully. The app must
//! already be in a state where it's safe to exit.
//! Note that the app may not have ever been successfully started when this is called, so check
//! your null pointers!
static void prv_app_cleanup(void) {
  // Back button may have been held down when this app quits.
  launcher_cancel_force_quit();

  // Always notify the phone that the application is not running
  app_run_state_send_update(&s_app_task_context.app_md->uuid, NOT_RUNNING);

  // Perform generic process cleanup. Note that s_app_task_context will be cleaned up and zero'd
  // by this.
  process_manager_process_cleanup(PebbleTask_App);

  // Perform app specific cleanup
  app_idle_timeout_stop();
#ifndef RECOVERY_FW
  app_inbox_service_unregister_all();
  app_outbox_service_cleanup_all_pending_messages();
#endif
  light_reset_user_controlled();
  sys_vibe_history_stop_collecting();
#if !defined(PLATFORM_TINTIN)
  ble_app_cleanup();
#endif
#if CAPABILITY_HAS_MAPPABLE_FLASH
  resource_mapped_release_all(PebbleTask_App);
#endif

  app_comm_set_sniff_interval(SNIFF_INTERVAL_NORMAL);

  app_manager_set_minimum_run_level(ProcessAppRunLevelNormal);
  app_install_cleanup_registered_app_callbacks();
  app_install_notify_app_closed();

  timeline_peek_handle_process_kill();
}

// ---------------------------------------------------------------------------------------------
//! On watchface crashes, we want to signal to the user that the watchface has crashed so that
//! they understand why are being jettisoned into the launcher.
static void prv_app_show_crash_ui(AppInstallId install_id) {
  AppInstallEntry entry;

  if (!app_install_get_entry_for_install_id(install_id, &entry)) {
    return;
  }

  if (!app_install_entry_is_watchface(&entry)) {
    return;
  }

#if !defined(RECOVERY_FW)
  static AppCrashInfo crash_info = { 0 };
  // If the same watchface crashes twice in one minute, then we show a dialog informing
  // the user that the watchface has crashed.  Any button press will dismiss
  // the dialog and show us the default system watch face.
  PBL_ASSERTN(install_id != INSTALL_ID_INVALID);
  if (crash_info.install_id != install_id ||
      (crash_info.crash_ticks + RETURN_CRASH_TIMEOUT_TICKS) < rtc_get_ticks()) {
    crash_info = (AppCrashInfo) {
      .install_id = install_id,
      .crash_ticks = rtc_get_ticks()
    };
    // Re-launch immediately
    watchface_launch_default(NULL);
    return;
  }

  SimpleDialog *crash_dialog = simple_dialog_create("Watchface crashed");
  Dialog *dialog = simple_dialog_get_dialog(crash_dialog);
  const char *text_fmt = i18n_get("%.*s is not responding", crash_dialog);
  unsigned int name_len = 15;
  char text[DIALOG_MAX_MESSAGE_LEN];
  sniprintf(text, DIALOG_MAX_MESSAGE_LEN, text_fmt, name_len, entry.name);

  dialog_set_text(dialog, text);
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_WARNING_LARGE);
  dialog_set_timeout(dialog, DIALOG_TIMEOUT_INFINITE /* no timeout */);

  // Any sort of application crash or window crash is a critical message as it
  // impacts the UX experience, so we want to push it to the forefront of the
  // window stack.
  WindowStack *window_stack = modal_manager_get_window_stack(ModalPriorityAlert);
  simple_dialog_push(crash_dialog, window_stack);

#if PBL_ROUND
  // For circular display, reduce app name length until message fits on the screen
  // This has to occur after the dialog window load has been called to provide
  // initial layout, text_layer flow and text_layer positions
  TextLayer *text_layer = &dialog->text_layer;
  const unsigned int min_text_len = 3;
  const int max_text_height = 2 * fonts_get_font_height(text_layer->font) + 8;
  GContext *ctx = graphics_context_get_current_context();
  int32_t text_height = text_layer_get_content_size(ctx, text_layer).h;

  // Until the text_height fits max_text_height or the app name is min_text_len
  while (text_height > max_text_height && name_len > min_text_len) {
    name_len--;
    sniprintf(text, DIALOG_MAX_MESSAGE_LEN, text_fmt, name_len, entry.name);
    dialog_set_text(dialog, text);
    text_height = text_layer_get_content_size(ctx, text_layer).h;
  }
#endif

  i18n_free_all(crash_dialog);

  PBL_LOG(LOG_LEVEL_DEBUG, "Watchface crashed, launching default.");

  crash_info = (AppCrashInfo) { 0 };

  watchface_set_default_install_id(INSTALL_ID_INVALID);
  watchface_launch_default(NULL);
#endif
}

// ---------------------------------------------------------------------------------------------
//! Switch to the app stored in the s_next_app global. The gracefully flag tells us whether to attempt a graceful
//! exit or not.
//!
//! For a graceful exit, if the app has not already finished it's de-init, we post a de_init event to the app, set
//! a 3 second timer, and return immediately to the caller. If/when the app finally finishes deinit, it will post a
//! PEBBLE_PROCESS_KILL_EVENT (graceful=true), which results in this method being again with graceful=true. We will then
//! see that the de_init already finished in that second invocation.
//!
//! If the app has finished its de-init, or graceful is false, we proceed to kill the app task and launch the next
//! app as stored in the s_next_app global.
//!
//! Returns true if new app was just switched in.
static bool prv_app_switch(bool gracefully) {
  ProcessContext *app_task_ctx = &s_app_task_context;

  PBL_LOG(LOG_LEVEL_DEBUG, "Switching from '%s' to '%s', graceful=%d...",
          process_metadata_get_name(app_task_ctx->app_md),
          process_metadata_get_name(s_next_app.md),
          (int)gracefully);

  // Shouldn't be called from app. Use app_manager_put_kill_app_event() instead.
  PBL_ASSERT_TASK(PebbleTask_KernelMain);

  // We have to call this here, in addition to calling it in prv_app_cleanup(),
  // because the timer could otherwise be triggered while waiting for the task
  // to exit, causing the app we land on to be killed when it shouldn't be.
  launcher_cancel_force_quit();

  // Make sure the process is safe to kill. If this method returns false, it will have set a timer to post
  //  another KILL event in a few seconds, thus giving the process a chance to clean up.
  if (!process_manager_make_process_safe_to_kill(PebbleTask_App, gracefully)) {
    // Maybe next time...
    return false;
  }

  AppInstallId old_install_id = s_app_task_context.install_id;

  // Kill the current app
  prv_app_cleanup();

  // If we had to ungracefully kill the current app, switch to the launcher app
  if (!gracefully) {
    app_install_release_md(s_next_app.md);
    s_next_app = (NextApp) {
      .md = system_app_state_machine_get_default_app(),
    };
  } else {
    // Get the next app to launch
    if (!s_next_app.md) {
      // There is no next app to launch? We're starting up, let's launch the startup app.
      app_install_release_md(s_next_app.md);
      s_next_app = (NextApp) {
        .md = system_app_state_machine_system_start(),
      };
    }
  }

  // Launch the new app
  if (!prv_app_start(s_next_app.md, s_next_app.common.args, s_next_app.common.reason)) {
    if (s_next_app.md->process_storage != ProcessStorageFlash) {
      PBL_CROAK("Failed to start system app <%s>!", process_metadata_get_name(s_next_app.md));
    }
    PBL_LOG(LOG_LEVEL_WARNING, "Failed to start app <%s>! Restarting launcher",
            process_metadata_get_name(s_next_app.md));

    prv_app_start(system_app_state_machine_system_start(), NULL, APP_LAUNCH_SYSTEM);
  }

  compositor_transition(s_next_app.common.transition);

  // Check if we've exited gracefully.  Otherwise, display the crash dialog if appropriate.
  if (!gracefully) {
    prv_app_show_crash_ui(old_install_id);
  }

  // Clear for next time.
  s_next_app = (NextApp) {};

  return true;
}


// ---------------------------------------------------------------------------------------------
void app_manager_start_first_app(void) {
  const PebbleProcessMd* app_md = system_app_state_machine_system_start();
  PBL_ASSERTN(prv_app_start(app_md, 0, APP_LAUNCH_SYSTEM));
  s_first_app_launched = true;
  compositor_transition(NULL);
}

static const CompositorTransition *prv_get_transition(const LaunchConfigCommon *config,
                                                      AppInstallId new_app_id) {
  return config->transition ?: shell_get_open_compositor_animation(s_app_task_context.install_id,
                                                                   new_app_id);
}

// ---------------------------------------------------------------------------------------------
void app_manager_put_launch_app_event(const AppLaunchEventConfig *config) {
  PBL_ASSERTN(config->id != INSTALL_ID_INVALID);

  PebbleLaunchAppEventExtended *data = kernel_malloc_check(sizeof(PebbleLaunchAppEventExtended));
  *data = (PebbleLaunchAppEventExtended) {
    .common = config->common
  };
  data->common.transition = prv_get_transition(&config->common, config->id);

  PebbleEvent e = {
    .type = PEBBLE_APP_LAUNCH_EVENT,
    .launch_app = {
      .id = config->id,
      .data = data
    },
  };

  event_put(&e);
}

// ---------------------------------------------------------------------------------------------
bool app_manager_launch_new_app(const AppLaunchConfig *config) {
  // Note that config has a dynamically allocated member that needs to be free'd with
  // app_install_release_md if we don't actually proceed with launching the app.

  const PebbleProcessMd *app_md = config->md;
  const AppInstallId new_app_id = app_install_get_id_for_uuid(&app_md->uuid);

  if (!config->restart && uuid_equal(&(app_md->uuid), &(s_app_task_context.app_md->uuid))) {
    PBL_LOG(LOG_LEVEL_WARNING, "Ignoring launch for app <%s>, app is already running",
            process_metadata_get_name(app_md));

    app_install_release_md(app_md);
    return false;
  }

  if (process_metadata_get_run_level(app_md) < s_minimum_run_level) {
    PBL_LOG(LOG_LEVEL_WARNING,
        "Ignoring launch for app <%s>, minimum run level %d, app run level %d",
        process_metadata_get_name(app_md), s_minimum_run_level,
        process_metadata_get_run_level(app_md));

    app_install_release_md(app_md);
    return false;
  }

  s_next_app = (NextApp) {
    .md = app_md,
    .common = config->common,
  };
  s_next_app.common.transition = prv_get_transition(&config->common, new_app_id);

  if ((config->common.reason == APP_LAUNCH_WAKEUP) && (config->common.args != NULL)) {
    WakeupInfo *wakeup_info = (WakeupInfo *)config->common.args;
    s_next_app.wakeup_info = *(WakeupInfo *)wakeup_info;

    // Stop pointing at the old storage location for wakeup_info so we don't keep the dangling
    // pointer around.
    s_next_app.common.args = NULL;
  }

  return prv_app_switch(!config->forcefully);
}

// ---------------------------------------------------------------------------------------------
void app_manager_handle_app_fetch_request_event(const PebbleAppFetchRequestEvent *const evt) {
  PBL_ASSERTN(evt);
  if (!evt->with_ui) {
    return;
  }
  const AppFetchUIArgs *const fetch_args = evt->fetch_args;
  app_manager_launch_new_app(&(AppLaunchConfig) {
    .md = app_fetch_ui_get_app_info(),
    .common.args = fetch_args,
    .common.transition = fetch_args->common.transition,
    .forcefully = fetch_args->forcefully,
  });
}

// -----------------------------------------------------------------------------------------
static AppInstallId prv_get_app_exit_reason_destination_install_id_override(void) {
  switch (s_app_task_context.exit_reason) {
    case APP_EXIT_NOT_SPECIFIED:
      return INSTALL_ID_INVALID;
    case APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY:
      PBL_LOG(LOG_LEVEL_INFO,
              "Next app overridden with watchface because action was performed successfully");
      return watchface_get_default_install_id();
    // Handling this case specifically instead of providing a default case ensures that the addition
    // of future exit reason values will cause compilation to fail until the new case is handled
    case NUM_EXIT_REASONS:
      break;
  }
  WTF;
}

// -----------------------------------------------------------------------------------------
void app_manager_close_current_app(bool gracefully) {
  // This method can be called as a result of receiving a PEBBLE_PROCESS_KILL_EVENT notification
  // from an app, telling us that it just finished it's deinit. Don't replace s_next_app.md if
  // perhaps it was already set by someone who called app_manager_launch_new_app or
  // app_manager_launch_new_app_with_args and asked the current app to exit.
  const AppInstallId current_app_id = s_app_task_context.install_id;
  AppInstallId destination_app_id = INSTALL_ID_INVALID;

#if !RECOVERY_FW
  destination_app_id = prv_get_app_exit_reason_destination_install_id_override();
#endif

  if (destination_app_id == INSTALL_ID_INVALID) {
    // If we get here, the app exit reason didn't override the destination app ID
    if (!s_next_app.md) {
      destination_app_id = system_app_state_machine_get_last_registered_app();
    } else {
      // If we get here, s_next_app is already setup and so we can call prv_app_switch() directly
      // and return
      prv_app_switch(gracefully);
      return;
    }
  }

  app_manager_set_minimum_run_level(ProcessAppRunLevelNormal);
  process_manager_launch_process(&(ProcessLaunchConfig) {
    .id = destination_app_id,
    .common.transition = shell_get_close_compositor_animation(current_app_id, destination_app_id),
    .forcefully = !gracefully,
  });
}

// -----------------------------------------------------------------------------------------
void app_manager_set_minimum_run_level(ProcessAppRunLevel run_level) {
  s_minimum_run_level = run_level;
}

// -----------------------------------------------------------------------------------------
void app_manager_force_quit_to_launcher(void) {
  const PebbleProcessMd *default_process = system_app_state_machine_get_default_app();
  const AppInstallId current_app_id = s_app_task_context.install_id;
  const AppInstallId new_app_id = app_install_get_id_for_uuid(&default_process->uuid);
  s_next_app = (NextApp) {
    .md = default_process,
  };
  s_next_app.common.transition = shell_get_close_compositor_animation(current_app_id, new_app_id);

  prv_app_switch(true /*gracefully*/);
}

const PebbleProcessMd* app_manager_get_current_app_md(void) {
  return s_app_task_context.app_md;
}

AppInstallId app_manager_get_current_app_id(void) {
  return s_app_task_context.install_id;
}

ProcessContext* app_manager_get_task_context(void) {
  return &s_app_task_context;
}

bool app_manager_is_watchface_running(void) {
  return (app_manager_get_current_app_md()->process_type == ProcessTypeWatchface);
}

ResAppNum app_manager_get_current_resource_num(void) {
  return process_metadata_get_res_bank_num(s_app_task_context.app_md);
}

AppLaunchReason app_manager_get_launch_reason(void) {
  return s_next_app.common.reason;
}

ButtonId app_manager_get_launch_button(void) {
  return s_next_app.common.button;
}

void app_manager_get_framebuffer_size(GSize *size) {
  if (size == NULL) {
    return;
  }

  if (!s_app_task_context.app_md) {
    // No app has been started yet, so just use the default system size
    *size = GSize(DISP_COLS, DISP_ROWS);
    return;
  }

  // Platform matches current platform
  const PlatformType sdk_platform =
    process_metadata_get_app_sdk_platform(s_app_task_context.app_md);

  if (sdk_platform == PBL_PLATFORM_TYPE_CURRENT) {
    *size = GSize(DISP_COLS, DISP_ROWS);
    return;
  }

  // We cannot use the SDK type for this compatibility check but there's
  // also no easy way to get the resolutions per platform.
  // so we re-use the suboptimal defines from each display_<model>.h
  switch (sdk_platform) {
    case PlatformTypeAplite:
      *size = GSize(LEGACY_2X_DISP_COLS, LEGACY_2X_DISP_ROWS);
      return;
    case PlatformTypeBasalt:
    case PlatformTypeChalk:
      // yes, this is misleading, e.g. on Spalding, these defines are always 180x180
      // oh dear...
      *size = GSize(LEGACY_3X_DISP_COLS, LEGACY_3X_DISP_ROWS);
      return;
    case PlatformTypeDiorite:
    case PlatformTypeEmery:
      *size = GSize(DISP_COLS, DISP_ROWS);
      return;
  }
  WTF;
}

bool app_manager_is_app_supported(const PebbleProcessMd *md) {
  // Get the app ram size depending on the SDK type.
  // Unsupported SDK types will have a size of 0.
  return prv_get_app_segment_size(md) > 0;
}


// Commands
///////////////////////////////////////////////////////////

void command_get_active_app_metadata(void) {
  char buffer[32];

  const PebbleProcessMd* app_metadata = app_manager_get_current_app_md();
  if (app_metadata != NULL) {
    prompt_send_response_fmt(buffer, sizeof(buffer), "app name: %s",
                             process_metadata_get_name(app_metadata));
    prompt_send_response_fmt(buffer, sizeof(buffer), "is watchface: %d",
                             (app_metadata->process_type == ProcessTypeWatchface));
    prompt_send_response_fmt(buffer, sizeof(buffer), "visibility: %u", app_metadata->visibility);
    prompt_send_response_fmt(buffer, sizeof(buffer), "bank: %d",
                             (uint8_t) process_metadata_get_res_bank_num(app_metadata));
  } else {
    prompt_send_response("metadata lookup failed: no app running");
  }
}


// Analytics
//////////////////////////////////////////////////////////////

static void prv_handle_app_start_analytics(const PebbleProcessMd *app_md,
    const AppLaunchReason launch_reason) {
  analytics_event_app_launch(&app_md->uuid);
  analytics_inc(ANALYTICS_APP_METRIC_LAUNCH_COUNT, AnalyticsClient_App);
  analytics_stopwatch_start(ANALYTICS_APP_METRIC_FRONT_MOST_TIME, AnalyticsClient_App);

  Version app_sdk_version = process_metadata_get_sdk_version(app_md);
  analytics_set(ANALYTICS_APP_METRIC_SDK_MAJOR_VERSION, app_sdk_version.major, AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_SDK_MINOR_VERSION, app_sdk_version.minor, AnalyticsClient_App);

  Version app_version = process_metadata_get_process_version(app_md);
  analytics_set(ANALYTICS_APP_METRIC_APP_MAJOR_VERSION, app_version.major, AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_APP_MINOR_VERSION, app_version.minor, AnalyticsClient_App);

  ResourceVersion resource_version = process_metadata_get_res_version(app_md);
  analytics_set(ANALYTICS_APP_METRIC_RESOURCE_TIMESTAMP, resource_version.timestamp, AnalyticsClient_App);

  if (app_md->is_rocky_app) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_APP_ROCKY_LAUNCH_COUNT, AnalyticsClient_System);
    analytics_inc(ANALYTICS_APP_METRIC_ROCKY_LAUNCH_COUNT, AnalyticsClient_App);
  }

  if (launch_reason == APP_LAUNCH_QUICK_LAUNCH) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_APP_QUICK_LAUNCH_COUNT, AnalyticsClient_System);
    analytics_inc(ANALYTICS_APP_METRIC_QUICK_LAUNCH_COUNT, AnalyticsClient_App);
  } else if (launch_reason == APP_LAUNCH_USER) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_APP_USER_LAUNCH_COUNT, AnalyticsClient_System);
    analytics_inc(ANALYTICS_APP_METRIC_USER_LAUNCH_COUNT, AnalyticsClient_App);
  }
}


// -------------------------------------------------------------------------------------------
/*!
  @brief User mode access to its UUID.
  @param[out] uuid The app's UUID.
 */
DEFINE_SYSCALL(void, sys_get_app_uuid, Uuid *uuid) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(uuid, sizeof(*uuid));
  }

  *uuid = app_manager_get_current_app_md()->uuid;
}

DEFINE_SYSCALL(Version, sys_get_current_app_sdk_version, void) {
  return process_metadata_get_sdk_version(app_manager_get_current_app_md());
}

DEFINE_SYSCALL(bool, sys_get_current_app_is_js_allowed, void) {
  return (app_manager_get_current_app_md()->allow_js);
}

DEFINE_SYSCALL(bool, sys_get_current_app_is_rocky_app, void) {
  return (app_manager_get_current_app_md()->is_rocky_app);
}

DEFINE_SYSCALL(PlatformType, sys_get_current_app_sdk_platform, void) {
  return process_metadata_get_app_sdk_platform(app_manager_get_current_app_md());
}

DEFINE_SYSCALL(bool, sys_app_is_watchface, void) {
  return app_manager_is_watchface_running();
}

DEFINE_SYSCALL(ResAppNum, sys_get_current_resource_num, void) {

  if (pebble_task_get_current() == PebbleTask_KernelMain) {
    return SYSTEM_APP;
  }

  return process_metadata_get_res_bank_num(app_manager_get_current_app_md());
}

DEFINE_SYSCALL(AppInstallId, sys_app_manager_get_current_app_id, void) {
  return app_manager_get_current_app_id();
}
