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

#include "actionable_dialog_private.h"

#include "applib/graphics/gtypes.h"
#include "applib/ui/action_bar_layer.h"
#include "applib/ui/click.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/window_stack.h"

//! Creates a new ActionableDialog on the heap.
//! @param dialog_name The debug name to give the \ref ActionableDialog
//! @return Pointer to a \ref ActionableDialog
ActionableDialog *actionable_dialog_create(const char *dialog_name);

//! @internal
//! Initializes the passed \ref ActionableDialog
//! @param actionable_dialog Pointer to a \ref ActionableDialog to initialize
//! @param dialog_name The debug name to give the \ref ActionableDialog
void actionable_dialog_init(ActionableDialog *actionable_dialog, const char *dialog_name);

//! Retrieves the internal Dialog object from the ActionableDialog.
//! @param actionable_dialog Pointer to a \ref ActionableDialog from which to grab it's dialog
//! @return The underlying \ref Dialog of the given \ref ActionableDialog
Dialog *actionable_dialog_get_dialog(ActionableDialog *actionable_dialog);

//! Sets the type of action bar to used to one of the pre-defined types or a custom one.
//! @param actionable_dialog Pointer to a \ref ActionableDialog whom which to set
//! @param action_bar_type The type of action bar to give the passed dialog
//! @param action_bar Pointer to an \ref ActionBarLayer to assign to the dialog
//! @note:  The pointer to an \ref ActionBarLayer is optional and only required when the
//!     the \ref DialogActionBarType is \ref DialogActionBarCustom.  If the type is not
//!     custom, then the given action bar will not be set on the dialog, regardless of if
//!     it is `NULL` or not.
void actionable_dialog_set_action_bar_type(ActionableDialog *actionable_dialog,
                                           DialogActionBarType action_bar_type,
                                           ActionBarLayer *action_bar);

//! Sets the ClickConfigProvider of the action bar. If the dialog has a custom action bar then
//! this function has no effect. The action bar is responsible for setting up its own click
//! config provider
//! @param actionable_dialog Pointer to a \ref ActionableDialog to which to set the provider on
//! @param click_config_provider The \ref ClickConfigProvider to set
void actionable_dialog_set_click_config_provider(ActionableDialog *actionable_dialog,
                                                 ClickConfigProvider click_config_provider);

//! @internal
//! Pushes the \ref ActionableDialog onto the given window stack.
//! @param actionable_dialog Pointer to a \ref ActionableDialog to push
//! @param window_stack Pointer to a \ref WindowStack to push the \ref ActionableDialog to
void actionable_dialog_push(ActionableDialog *actionable_dialog, WindowStack *window_stack);

//! Wrapper to call \ref actionable_dialog_push() for an app.
//! @note: Put a better comment here when we export
void app_actionable_dialog_push(ActionableDialog *actionable_dialog);

//! Pops the given \ref ActionableDialog from the window stack it was pushed to.
//! @param actionable_dialog Pointer to a \ref ActionableDialog to pop
void actionable_dialog_pop(ActionableDialog *actionable_dialog);
