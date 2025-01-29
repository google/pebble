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

#include <stdbool.h>

#include "freertos_types.h"
#include "kernel/pebble_tasks.h"

#define TASK_WATCHDOG_PRIORITY 0x1

void task_watchdog_init(void);

//! Feed the watchdog for a particular task. If a task doesn't call this function frequently
//! enough and it's mask is set we will eventually trigger a reboot.
void task_watchdog_bit_set(PebbleTask task);

//! Feed all task watchdogs bits. Don't use this unless you have to, as ideally all tasks should be
//! managing their own bits. If you're using this you're probably hacking around something awful.
void task_watchdog_bit_set_all(void);

//! @return bool Whether this task is being tracked by the task watchdog.
bool task_watchdog_mask_get(PebbleTask task);

//! Starts tracking a particular task using the task watchdog. The task must regularly call
//! task_watchdog_bit_set if task_watchdog_mask_set is set.
void task_watchdog_mask_set(PebbleTask task);

//! Removes a task from the task watchdog. This task will no long need to call
//! task_watchdog_bit_set regularly.
void task_watchdog_mask_clear(PebbleTask task);

//! Should only be called if the task_watchdog timer has been halted for some reason
//! (For example, when we are in stop mode)
void task_watchdog_step_elapsed_time_ms(uint32_t elapsed_ms);
