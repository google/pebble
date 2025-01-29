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

#include "applib/ui/window.h"
#include "applib/ui/window_stack.h"
#include "services/normal/timeline/item.h"

//! @file action_menu_window.h
//! @addtogroup UI
//! @{
//!   @addtogroup ActionMenu
//!
//! \brief Configurable menu that displays a hierarchy of selectable choices to the user
//!
//!   @{


typedef enum {
  ActionMenuAlignTop = 0,
  ActionMenuAlignCenter
} ActionMenuAlign;

struct ActionMenuItem;
//! An ActionMenuItem is an entry in the ActionMenu
//! You can think of it as a node in the action tree.
//! ActionMenuItems are either actions (i.e. leaf nodes) or levels.
typedef struct ActionMenuItem ActionMenuItem;

struct ActionMenuLevel;
typedef struct ActionMenuLevel ActionMenuLevel;

struct ActionMenu;
typedef struct ActionMenu ActionMenu;

//! Callback executed after the ActionMenu has closed, so memory may be freed.
//! @param root_level the root level passed to the ActionMenu
//! @param performed_action the ActionMenuItem for the action that was performed,
//! NULL if the ActionMenu is closing without an action being selected by the user
//! @param context the context passed to the ActionMenu
typedef void (*ActionMenuDidCloseCb)(ActionMenu *menu,
                                     const ActionMenuItem *performed_action,
                                     void *context);

//! Callback executed immediately before the ActionMenu closes.
//! @param root_level the root ActionMenuLevel passed to the ActionMenu
//! @param performed_action the ActionMenuItem for the action that was performed,
//! NULL if the ActionMenu is closing without an action being selected by the user
//! @param context the context passed to the ActionMenu
typedef void (*ActionMenuWillCloseCb)(ActionMenu *menu,
                                      const ActionMenuItem *performed_action,
                                      void *context);

//! Configuration struct for the ActionMenu
typedef struct {
  const ActionMenuLevel *root_level; //!< the root level of the ActionMenu
  void *context; //!< a context pointer which will be accessible when actions are performed
  struct {
    GColor background; //!< the color of the left column of the ActionMenu
    GColor foreground; //!< the color of the individual "crumbs" that indicate menu depth
  } colors;
  ActionMenuDidCloseCb will_close; //!< Called immediately before the ActionMenu closes
  ActionMenuDidCloseCb did_close; //!< a callback used to cleanup memory after the menu has closed
  ActionMenuAlign align;
} ActionMenuConfig;

//! Get the context pointer this ActionMenu was created with
//! @param action_menu A pointer to an ActionMenu
//! @return the context pointer initially provided in the \ref ActionMenuConfig.
//! NULL if none exists.
void *action_menu_get_context(ActionMenu *action_menu);

//! Get the root level of an ActionMenu
//! @param action_menu the ActionMenu you want to know about
//! @return a pointer to the root ActionMenuLevel for the given ActionMenu, NULL if invalid
ActionMenuLevel *action_menu_get_root_level(ActionMenu *action_menu);

//! @internal
//! Open a new ActionMenu.
//! The ActionMenu acts much like a window. It fills the whole screen and handles clicks.
//! @param window_stack The \ref WindowStack to push the ActionMenu to
//! @param config the configuration info for this new ActionMenu
//! @return the new ActionMenu
ActionMenu *action_menu_open(WindowStack *window_stack, ActionMenuConfig *config);

//! Open a new ActionMenu.
//! The ActionMenu acts much like a window. It fills the whole screen and handles clicks.
//! @param config the configuration info for this new ActionMenu
//! @return the new ActionMenu
ActionMenu *app_action_menu_open(ActionMenuConfig *config);

//! Freeze the ActionMenu. The ActionMenu will no longer respond to user input.
//! @note this API should be used when waiting for asynchronous operation.
//! @param action_menu the ActionMenu
void action_menu_freeze(ActionMenu *action_menu);

//! Unfreeze the ActionMenu previously frozen with \ref action_menu_freeze
//! @param action_menu the ActionMenu to unfreeze
void action_menu_unfreeze(ActionMenu *action_menu);

//! Check if an ActionMenu is frozen.
bool action_menu_is_frozen(ActionMenu *action_menu);

//! Set the result window for an ActionMenu. The result window will be
//! shown when the ActionMenu closes
//! @param action_menu the ActionMenu
//! @param result_window the window to insert, pass NULL to remove the current result window
//! @note repeated call will result in only the last call to be applied, i.e. only
//! one result window is ever set
void action_menu_set_result_window(ActionMenu *action_menu, Window *result_window);

//! @internal
void action_menu_set_align(ActionMenuConfig *config, ActionMenuAlign align);

//! Close the ActionMenu, whether it is frozen or not.
//! @note this API can be used on a frozen ActionMenu once the data required to
//! build the result window has been received and the result window has been set
//! @param action_menu the ActionMenu to close
//! @param animated whether or not show a close animation
void action_menu_close(ActionMenu *action_menu, bool animated);

//!   @} // end addtogroup ActionMenu
//! @} // end addtogroup UI
