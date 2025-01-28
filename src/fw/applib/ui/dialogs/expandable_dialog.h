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

#include "applib/graphics/gtypes.h"
#include "applib/ui/click.h"
#include "applib/ui/action_bar_layer.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/scroll_layer.h"
#include "applib/ui/window_stack.h"
#include "resource/resource_ids.auto.h"

#include <stdint.h>

#define DIALOG_MAX_HEADER_LEN 30

// An ExpandableDialog is dialog that contains a large amount of text that can be scrolled. It also
// contains an action bar which indicates which directions can currently be scrolled and optionally
// a SELECT button action.
typedef struct ExpandableDialog {
  Dialog dialog;

  bool show_action_bar;
  bool show_action_icon_animated;

  GColor action_bar_background_color;
  ActionBarLayer action_bar;
  ClickHandler select_click_handler;

  GBitmap *up_icon;
  GBitmap *select_icon;
  GBitmap *down_icon;

  GFont header_font;
  char header[DIALOG_MAX_HEADER_LEN + 1];

  TextLayer header_layer;
  ScrollLayer scroll_layer;
  Layer content_down_arrow_layer;
} ExpandableDialog;

//! Creates a new ExpandableDialog on the heap.
//! @param dialog_name The name to give the dialog
//! @return Pointer to an \ref ExpandableDialog
ExpandableDialog *expandable_dialog_create(const char *dialog_name);

//! Creates a new ExpandableDialog on the heap with additional parameters
//! @param dialog_name The name to give the dialog
//! @param icon The icon which appears at the top of the dialog
//! @param text The text to display in the dialog
//! @param text_color The color of the dialog's text
//! @param background_color The background color of the dialog
//! @param callbacks The \ref DialogCallbacks to assign to the dialog
//! @param select_icon The icon to assign to the select button on the action menu
//! @param select_click_handler The \ref ClickHandler to assign to the select button
//! @return Pointer to an \ref ExpandableDialog
ExpandableDialog *expandable_dialog_create_with_params(const char *dialog_name, ResourceId icon,
                                                       const char *text, GColor text_color,
                                                       GColor background_color,
                                                       DialogCallbacks *callbacks,
                                                       ResourceId select_icon,
                                                       ClickHandler select_click_handler);

//! Simple callback which closes the dialog when called
void expandable_dialog_close_cb(ClickRecognizerRef recognizer, void *e_dialog);

//! Initializes an ExpandableDialog
//! @param expandable_dialog Pointer to an \ref ExpandableDialog
//! param dialog_name The name to give the \ref ExpandableDialog
void expandable_dialog_init(ExpandableDialog *expandable_dialog, const char *dialog_name);

//! Retrieves the internal Dialog object of the Expandable Dialog.
//! @param expandable_dialog The \ref ExpandableDialog to retrieve from.
//! @return \ref Dialog
Dialog *expandable_dialog_get_dialog(ExpandableDialog *expandable_dialog);

//! Sets whether or not the expandable dialog should should show its action bar.
//! @param expandable_dialog Pointer to the \ref ExpandableDialog to set on
//! @param show_action_bar Boolean indicating whether to show the action bar
void expandable_dialog_show_action_bar(ExpandableDialog *expandable_dialog,
                                       bool show_action_bar);

//! Sets whether to animate the action bar items.
//! @param expandable_dialog Pointer to the \ref ExpandableDialog to set on
//! @param animated Boolean indicating whether or not to animate the icons
//! @note Unless \ref expandable_dialog_show_action_bar is called with true, this function
//!     will not have any noticeable change on the \ref ExpandableDialog
void expandable_dialog_set_action_icon_animated(ExpandableDialog *expandable_dialog,
                                                bool animated);

//! Sets the action bar background color
//! @param expandable_dialog Pointer to the \ref ExpandableDialog for which to set
//! @param background_color The background color of the dialog's action bar
void expandable_dialog_set_action_bar_background_color(ExpandableDialog *expandable_dialog,
                                                       GColor background_color);

//! Sets the text of the optional header text.  The header has a maximum length of
//! \ref DIALOG_MAX_HEADER_LEN and the text passed in will be clipped if it exceeds that
//! length.
//! @param expandable_dialog Pointer to the \ref ExpandableDialog on which to set the header
//! @param header Text to set as the header.
//! @note If set to NULL, the header will not appear.
void expandable_dialog_set_header(ExpandableDialog *expandable_dialog, const char *header);

//! Sets the header font
//! @param expandable_dialog Pointer to the \ref ExpandableDialog on which to set the header
//! @param header_font The font to use for the header text
void expandable_dialog_set_header_font(ExpandableDialog *expandable_dialog, GFont header_font);

//! Sets the icon and ClickHandler of the SELECT button on the action bar.
//! @param expandable_dialog Pointer to the \ref ExpandableDialog for which to set
//! @param resource_id The resource id of the resource to be used to create the select bitmap
//! @param select_click_handler Handler to call when the select handler is clicked in the
//!     Expandable Dialog's action bar layer.
//! @note Passing \ref RESOURCE_ID_INVALID as the resource_id to the function will allow you
//!     to set an action with no icon appearing in the \ref ActionBarLayer
void expandable_dialog_set_select_action(ExpandableDialog *expandable_dialog,
                                         uint32_t resource_id,
                                         ClickHandler select_click_handler);

//! Pushes the dialog onto the window stack.
//! @param expandable_dialog Pointer to the \ref ExpandableDialog to push.
//! @param window_stack Pointer to the \ref WindowStack to push the dialog to
void expandable_dialog_push(ExpandableDialog *expandable_dialog, WindowStack *window_stack);

//! Pushes the dialog onto the app's window stack
//! @param expandable_dialog Pointer to the \ref ExpandableDialog to push.
//! @note: Put a better comment here before exporting
void app_expandable_dialog_push(ExpandableDialog *expandable_dialog);

//! Wrapper for popping the underlying dialog off of the window stack.  Useful for when the
//! user overrides the default behaviour of the select action to allow them to pop the dialog.
//! @param expandable_dialog Pointer to the \ref ExpandableDialog to pop.
void expandable_dialog_pop(ExpandableDialog *expandable_dialog);
