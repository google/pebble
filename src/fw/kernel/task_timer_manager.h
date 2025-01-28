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

#include "task_timer.h"

//! Internal state object. Each task that wants to execute timers should allocate their own
//! instance of this object.
typedef struct TaskTimerManager {
  PebbleMutex *mutex;

  //! List of timers that are currently running
  ListNode *running_timers;
  //! List of timers that are allocated but unscheduled
  ListNode *idle_timers;

  //! The next ID to assign to a new timer.
  TaskTimerID next_id;

  //! Externally provided semaphore that is given whenever the next timer to expire has changed.
  SemaphoreHandle_t semaphore;

  //! The callback we're currently executing, useful for debugging.
  void *current_cb;
} TaskTimerManager;


//! Initialize a passed in manager object.
//! @param[in] semaphore a sempahore the TaskTimerManager should give if the next expiring timer
//!                      has changed. The task event loop should block on this same semphore to
//!                      handle timer updates in a timely fashion.
void task_timer_manager_init(TaskTimerManager *manager, SemaphoreHandle_t semaphore);

//! Execute any timers that are currently expired.
//! @return the number of ticks until the next timer expires. If there are no timers running,
//!         returns portMAX_DELAY.
TickType_t task_timer_manager_execute_expired_timers(TaskTimerManager *manager);

//! Debugging interface to help understand why the task_timer execution is stuck and what
//! its stuck on.
//! @return A pointer to the current callback that's running, NULL if no callback
//!         is currently running.
void* task_timer_manager_get_current_cb(const TaskTimerManager *manager);
