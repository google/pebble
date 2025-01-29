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
#include "applib/accel_service.h"
#include "kernel/event_loop.h"

//! Set up a timer that will check the position of the watch every minute to see
//! if any motion has occurred
void stationary_init(void);

//! Stationary mode should only be enabled when the user settings allow for it and when
//! the charger is not connected
bool stationary_get_enabled(void);

//! Set whether the stationary module is enabled. When disabled, all operations will end, we ensure
//! that we are in a normal state, and the watch will not be able to enter stationary mode
void stationary_set_enabled(bool enabled);

//! Set whether the stationary service is allowed to be enabled for the current runlevel
void stationary_run_level_enable(bool allow);

//! If the stationary module is enabled and currently in stationary mode, then
//! we are put into a normal state. Call this if the system is about to do something that will
//! probably require user interaction, like an alarm going off.
void stationary_wake_up(void);

//! Called by our event service system when there is a battery connection change
void stationary_handle_battery_connection_change_event(void);
