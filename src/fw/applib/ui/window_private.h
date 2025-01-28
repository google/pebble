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

#include "window.h"

#include <stddef.h>

//! Internal interface for layer to schedule a render for the window:
//! @param window Pointer to the window to schedule
void window_schedule_render(Window *window);

//! Setup the click config provider
//! @param window Pointer to the window to setup the click config provider
void window_setup_click_config_provider(Window *window);

//! Internal interface for window_stack to signal putting a window on/off screen:
//! @param window Pointer to the window to set on the screen
//! @param new_on_screen Boolean indicating if there is a new window or an old one
//!     If the window is on screen and this is true, or the window is off-screen and
//!     this is false, then this is a no-op
//! @param call_window_appear_handlers Boolean indicating whether or not to call the
//!     window appear/disappear handler
void window_set_on_screen(Window *window, bool new_on_screen, bool call_window_appear_handlers);

//! Internal helper to calculate the frame of a window (e.g. inside a transition container layer)
//! NOTE: even if window.fullscreen==false, it still returns result.origin.y == 0
//! When rendering, window_render() takes care of it
//! @param fullscreen boolean indicating if the window is full screen or not
//! @return \ref GRect
GRect window_calc_frame(bool fullscreen);

//! @internal
//! @param window Pointer to the \ref Window to check
//! @return boolean indicating if the passed window has a status bar
bool window_has_status_bar(Window *window);

//! @param window Pointer to the \ref Window to set
//! @param overrides_back_button Boolean indicating if the back button has been overridden
//!     in the \ref ClickConfigProvider of the passed \ref Window
void window_set_overrides_back_button(Window *window, bool overrides_back_button);

//! @internal
//! Called to unload a window.
//! @param window The \ref Window to unload
void window_unload(Window *window);
