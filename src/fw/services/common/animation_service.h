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
#include <stdint.h>
#include "kernel/pebble_tasks.h"

//! @file animation_service.h
//! Manage the system resources used by the applib/animation module.

//! Register the timer to fire in N ms. When it fires, the animation_private_timer_callback()
//! will be called and passed the AnimationState for that task.
void animation_service_timer_schedule(uint32_t ms);

//! Acknowledge that we received an event sent by the animation timer
void animation_service_timer_event_received(void);

//! Destroy the animation resources used by the given task. Called by the process_manager when a
// process exits
void animation_service_cleanup(PebbleTask task);
