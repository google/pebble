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

#include "applib/app_timer.h"

#include "applib/app_logging.h"
#include "services/common/evented_timer.h"
#include "syscall/syscall_internal.h"

//! @file fw/applib/app_timer.c
//!
//! Surprise! All this is is a dumb wrapper around evented_timer!

DEFINE_SYSCALL(AppTimer*, app_timer_register, uint32_t timeout_ms,
                                              AppTimerCallback callback,
                                              void* callback_data) {
  // No need to check callback_data, we only dereference it in userspace anyway.
  return (AppTimer*)(uintptr_t)evented_timer_register(timeout_ms, false, callback, callback_data);
}

DEFINE_SYSCALL(AppTimer*, app_timer_register_repeatable, uint32_t timeout_ms,
                                                         AppTimerCallback callback,
                                                         void* callback_data,
                                                         bool repeating) {
  // No need to check callback_data, we only dereference it in userspace anyway.
  return (AppTimer*)(uintptr_t)evented_timer_register(timeout_ms,
                                                      repeating,
                                                      callback,
                                                      callback_data);
}

DEFINE_SYSCALL(bool, app_timer_reschedule, AppTimer *timer, uint32_t new_timeout_ms) {
  if (PRIVILEGE_WAS_ELEVATED) {
    if (!evented_timer_exists((EventedTimerID)timer)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Timer %u does not exist", (unsigned)timer);
      return (false);
    }
    if (!evented_timer_is_current_task((EventedTimerID)timer)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid timer %u used in app_timer_reschedule", (unsigned)timer);
      syscall_failed();
    }
  }
  return evented_timer_reschedule((EventedTimerID)timer, new_timeout_ms);
}

DEFINE_SYSCALL(void, app_timer_cancel, AppTimer *timer) {
  if (PRIVILEGE_WAS_ELEVATED) {
    if (!evented_timer_exists((EventedTimerID)timer)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Timer %u does not exist", (unsigned)timer);
      return;
    }
    if (!evented_timer_is_current_task((EventedTimerID)timer)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid timer %u used in app_timer_reschedule", (unsigned)timer);
      syscall_failed();
    }
  }
  evented_timer_cancel((EventedTimerID)timer);
}

DEFINE_SYSCALL(void *, app_timer_get_data, AppTimer *timer) {
  if (PRIVILEGE_WAS_ELEVATED) {
    if (!evented_timer_exists((EventedTimerID)timer)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Timer %u does not exist", (unsigned)timer);
      return NULL;
    }
    if (!evented_timer_is_current_task((EventedTimerID)timer)) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid timer %u used in app_timer_reschedule", (unsigned)timer);
      syscall_failed();
    }
  }

  return evented_timer_get_data((EventedTimerID)timer);
}
