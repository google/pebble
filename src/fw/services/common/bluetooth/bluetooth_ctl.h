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

typedef enum {
  BtCtlModeOverrideNone,
  BtCtlModeOverrideStop,
  BtCtlModeOverrideRun
} BtCtlModeOverride;

void bt_ctl_init(void);

//! returns the airplane mode state
bool bt_ctl_is_airplane_mode_on(void);

//! Returns whether the bluetooth stack is supposed to be up and running (but might not because it's
//! still starting or in the middle of resetting).
bool bt_ctl_is_bluetooth_active(void);

//! Returns whether the bluetooth stack is up and running or not.
bool bt_ctl_is_bluetooth_running(void);

// The following three functions are used for setting the flags that define the state of
// the bluetooth stack.

//! Sets the airplane mode flag. The flag is persisted across reboots
void bt_ctl_set_airplane_mode_async(bool enabled);

//! Sets enable flag (used by the runlevel system).
void bt_ctl_set_enabled(bool enabled);

//! Sets the override mode used to stop and start the bluetooth independent of the airplane mode.
void bt_ctl_set_override_mode(BtCtlModeOverride override);

//! Reset bluetooth using sequential calls to comm_stop() and comm_start()
void bt_ctl_reset_bluetooth(void);
