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

#include "layer.h"
#include "text_layer.h"
#include "action_bar_layer.h"
#include "window.h"

//! @file number_window.h
//! @addtogroup UI
//! @{
//!   @addtogroup Window
//!   @{
//!     @addtogroup NumberWindow
//! \brief A ready-made Window prompting the user to pick a number
//!
//! ![](number_window.png)
//!     @{

//! A ready-made Window prompting the user to pick a number
struct NumberWindow;

//! Function signature for NumberWindow callbacks.
typedef void (*NumberWindowCallback)(struct NumberWindow *number_window, void *context);

//! Data structure containing all the callbacks for a NumberWindow.
typedef struct {
  //! Callback that gets called as the value is incremented.
  //! Optional, leave `NULL` if unused.
  NumberWindowCallback incremented;
  //! Callback that gets called as the value is decremented.
  //! Optional, leave `NULL` if unused.
  NumberWindowCallback decremented;
  //! Callback that gets called as the value is confirmed, in other words the
  //! SELECT button is clicked.
  //! Optional, leave `NULL` if unused.
  NumberWindowCallback selected;
} NumberWindowCallbacks;

//! Data structure of a NumberWindow.
//! @note a `NumberWindow *` can safely be casted to a `Window *` and can thus
//! be used with all other functions that take a `Window *` as an argument.
//! <br/>For example, the following is legal:
//! \code{.c}
//! NumberWindow number_window;
//! ...
//! window_stack_push((Window *)&number_window, true);
//! \endcode
typedef struct NumberWindow {
  //! Make sure this is the first member of this struct, we use that to cast from Layer* all the
  //! way up to NumberWindow* in prv_update_proc.
  Window window;

  ActionBarLayer action_bar;

  const char *label;

  int32_t value;
  int32_t max_val;
  int32_t min_val;
  int32_t step_size;

  NumberWindowCallbacks callbacks;
  void *callback_context;
} NumberWindow;

//! Initializes the NumberWindow.
//! @param numberwindow Pointer to the NumberWindow to initialize
//! @param label The title or prompt to display in the NumberWindow. Must be long-lived and cannot be stack-allocated.
//! @param callbacks The callbacks
//! @param callback_context Pointer to application specific data that is passed
//! into the callbacks.
//! @note The number window is not pushed to the window stack. Use \ref window_stack_push() to do this.
//! See code fragment here: NumberWindow
void number_window_init(NumberWindow *numberwindow, const char *label, NumberWindowCallbacks callbacks, void *callback_context);

//! Creates a new NumberWindow on the heap and initializes it with the default values.
//!
//! @param label The title or prompt to display in the NumberWindow. Must be long-lived and cannot be stack-allocated.
//! @param callbacks The callbacks
//! @param callback_context Pointer to application specific data that is passed
//! @note The number window is not pushed to the window stack. Use \ref window_stack_push() to do this.
//! @return A pointer to the NumberWindow. `NULL` if the NumberWindow could not
//! be created
NumberWindow* number_window_create(const char *label, NumberWindowCallbacks callbacks, void *callback_context);

//! Destroys a NumberWindow previously created by number_window_create.
void number_window_destroy(NumberWindow* number_window);

//! Sets the text of the title or prompt label.
//! @param numberwindow Pointer to the NumberWindow for which to set the label
//! text
//! @param label The new label text. Must be long-lived and cannot be
//! stack-allocated.
void number_window_set_label(NumberWindow *numberwindow, const char *label);

//! Sets the maximum value this field can hold
//! @param numberwindow Pointer to the NumberWindow for which to set the maximum
//! value
//! @param max The maximum value
void number_window_set_max(NumberWindow *numberwindow, int32_t max);

//! Sets the minimum value this field can hold
//! @param numberwindow Pointer to the NumberWindow for which to set the minimum
//! value
//! @param min The minimum value
void number_window_set_min(NumberWindow *numberwindow, int32_t min);

//! Sets the current value of the field
//! @param numberwindow Pointer to the NumberWindow for which to set the current
//! value
//! @param value The new current value
void number_window_set_value(NumberWindow *numberwindow, int32_t value);

//! Sets the amount by which to increment/decrement by on a button click
//! @param numberwindow Pointer to the NumberWindow for which to set the step
//! increment
//! @param step The new step increment
void number_window_set_step_size(NumberWindow *numberwindow, int32_t step);

//! Gets the current value
//! @param numberwindow Pointer to the NumberWindow for which to get the current
//! value
//! @return The current value
int32_t number_window_get_value(const NumberWindow *numberwindow);

//! Gets the "root" Window of the number window
//! @param numberwindow Pointer to the NumberWindow for which to get the "root" Window
//! @return The "root" Window of the number window.
Window *number_window_get_window(NumberWindow *numberwindow);

//!     @} // end addtogroup NumberWindow
//!   @} // end addtogroup Window
//! @} // end addtogroup UI

