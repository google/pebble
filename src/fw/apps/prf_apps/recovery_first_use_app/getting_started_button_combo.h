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

#include "kernel/events.h"
#include "services/common/new_timer/new_timer.h"
#include "util/bitset.h"

//! @file prf_button_combo.h
//!
//! This file implements a monitor that watches which buttons are held down. This file looks for
//! 3 combinations.
//!
//! 1) Select held for 5 seconds: Invoke a user provided callback.
//! 2) Down held for 5 seconds: factory reset
//! 3) (PRF ONLY) Up+Down held for 5 seconds: Enter mfg mode
//!
//! The reason it's not just a boring set of long click handlers is because we don't support
//! registering a long click handler for a combination of buttons like up+down.
//!
//! I tried to split this out from a separate file from the recovery_first_use.c file so I could
//! test this behaviour in a unit test independent in the UI. I think it turned out /okay/. The
//! callback specification is a little odd (only for select but not for the other ones, should we
//! be blowing memory on static behaviour like this?) but it was worth a shot.

typedef void (*GettingStartedButtonComboCallback)(void *data);

typedef struct {
  //! Track which buttons are held. Use this instead of the driver function button_get_state_bits
  //! because that value isn't debounced.
  uint8_t buttons_held_bitset;

  //! Timer for how long the combination has been held for. We use new_timer instead of app_timer
  //! even though it's a little more dangerous (doesn't automatically get cleaned up by the app)
  //! because the api is nicer for starting/stopping/rescheduling the same timer over and over
  //! again with different callbacks.
  TimerID combo_timer;

  //! The callback to call when select is held.
  GettingStartedButtonComboCallback select_callback;
} GettingStartedButtonComboState;

//! Initialize resources associated with the state
//! @param state the state to initialize
//! @param select_callback the function to call when select is held
void getting_started_button_combo_init(GettingStartedButtonComboState *state,
                                       GettingStartedButtonComboCallback select_callback);

//! Deallocate resources associated with the state
void getting_started_button_combo_deinit(GettingStartedButtonComboState *state);

void getting_started_button_combo_button_pressed(GettingStartedButtonComboState *state,
                                                 ButtonId button_id);

void getting_started_button_combo_button_released(GettingStartedButtonComboState *state,
                                                  ButtonId button_id);
