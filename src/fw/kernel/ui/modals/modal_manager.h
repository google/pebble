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

#include "modal_manager_private.h"

#include "applib/graphics/graphics.h"
#include "applib/ui/click_internal.h"
#include "applib/ui/window.h"
#include "applib/ui/window_stack_animation.h"
#include "applib/ui/window_stack_private.h"
#include "kernel/events.h"

struct ModalContext;
typedef struct ModalContext ModalContext;

typedef bool (*ModalContextFilterCallback)(ModalContext *context, void *data);

//! This defines the priorities for the various modals.  The order in which they are
//! defined is specified by the interruption policy and determine who is able to interrupt
//! whom.  If a modal has a higher priority than another modal, it is able to interrupt
//! that modal.  Before adding anything to here, consider how it would affect the interrupt
//! policy.
//! Interruption Policy:
//! https://docs.google.com/presentation/d/1xIjOR9kh4jcBYonzRstdtFQW0ZNMoFZZwYOUMN2JHNI
typedef enum ModalPriority {
  //! Invalid priority for a modal.
  ModalPriorityInvalid = -1,
  //! Min priority, all modals are below or equal to this priority.
  ModalPriorityMin = 0,
  //! Discreet mode for watchface overlay information such as Timeline Peek.
  //! This priority should be used for modals that meant to display above the watchface without
  //! completely obstructing the watchface. For this reason, Discreet modal windows do not have
  //! compositor transitions because partial obstruction requires notifying the app of the
  //! unobstructed regions which only the modal window is able to derive.
  ModalPriorityDiscreet = ModalPriorityMin,
  //! Priority for generic one-off windows such as the battery charging window.
  //! This priority should be used for any modals that don't necessarily care how
  //! they are shown, just that they are shown.
  ModalPriorityGeneric,
  //! Priority used for displaying the phone ui after a phone call has been answered.
  //! Note: Notifications should always be able to subvert this.
  ModalPriorityPhone,
  //! Priority used for displaying notifications.
  ModalPriorityNotification,
  //! Priority used for displaying alerts.  Alerts are important, but shouldn't
  //! impact the user of the watch.
  ModalPriorityAlert,
  //! Priority used for displaying the voice screen for recording.  Ensure this one is
  //! always kept second to last.
  ModalPriorityVoice,
  //! Priority for displaying time-sensitive/critical windows which provide information
  //! on things that may affect the user's watch experience.  However, these should never
  //! prevent an alarm from displaying.
  ModalPriorityCritical,
  //! Priority used for displaying wake up events such as alarms.
  ModalPriorityAlarm,
  //! Max priority, all modals are below this priority
  ModalPriorityMax,
  NumModalPriorities = ModalPriorityMax,
} ModalPriority;

typedef enum ModalProperty {
  ModalProperty_None = 0,
  //! Whether there exists a modal in a modal stack on screen.
  ModalProperty_Exists = (1 << 0),
  //! Whether there exists a modal in a modal stack on screen that uses compositor transitions.
  ModalProperty_CompositorTransitions = (1 << 1),
  //! Whether there exists a modal that requested to render.
  ModalProperty_RenderRequested = (1 << 2),
  //! Whether all modal stacks are transparent. Having no modal is treated as transparent.
  ModalProperty_Transparent = (1 << 3),
  //! Whether all modal stacks pass input through. Having no modal is treated as unfocused.
  ModalProperty_Unfocused = (1 << 4),
  //! The default properties equivalent to there being no modal windows.
  ModalPropertyDefault = (ModalProperty_Transparent | ModalProperty_Unfocused),
} ModalProperty;

//! Initializes the modal window state.  This should be called before any
//! Modal applications attempt to push windows.
void modal_manager_init(void);

//! Sets whether modal windows are enabled.
//! @param enabled Boolean indicating whether enabled or disabled
//! Note that this is usable before modal_manager_init is called and modal_manager_init will not
//! reset this state.
void modal_manager_set_min_priority(ModalPriority priority);

//! Gets whether modal windows are enabled.
// @returns boolean indicating if modals are enabled
bool modal_manager_get_enabled(void);

//! Returns the ClickManager for the modal windows.
//! @returns pointer to a \ref ClickManager
ClickManager *modal_manager_get_click_manager(void);

//! Returns the first \ref WindowStack to pass the given filter callback.
//! Iterates down from the highest priority to the lowest priority.
//! @param filter_cb The \ref ModalContextFilterCallback
//! @param context Context to pass to the callback
//! @returns pointer to a \ref WindowStack
WindowStack *modal_manager_find_window_stack(ModalContextFilterCallback filter_cb, void *ctx);

//! Returns the stack with the given window priority.
//! If the passed priority is invalid, raises an assertion.
//! @param priority The \ref ModalPriority of the desired \ref WindowStack
//! @returns pointer to a \ref WindowStack
WindowStack *modal_manager_get_window_stack(ModalPriority priority);

//! Returns the \ref Window of the current visible stack if there is one,
//! otherwise NULL.
//! @returns Pointer to a \ref Window
Window *modal_manager_get_top_window(void);

//! Handles a button press event for the Modal Window. Raises an
//! assertion if the event is not a click event.
//! @param event The \ref PebbleEvent to handle.
void modal_manager_handle_button_event(PebbleEvent *event);

//! Pops all windows from all modal stacks.
void modal_manager_pop_all(void);

//! Pops all windows from modal stacks with priorities less than the given priority
//! @param the max priority stack to pop all windows from
void modal_manager_pop_all_below_priority(ModalPriority priority);

//! Called from the kernel event loop between events to handle any changes that have been made
//! to the modal window stacks.
void modal_manager_event_loop_upkeep(void);

//! Enumerates through the modal stacks and returns the flattened properties of all stacks
//! combined. For example, if all modals are transparent, the Transparent property will be
//! returned. Flattened meaning the entire stack is considered in aggregate. For example, if any
//! one modal is opaque, the Transparent property won't be returned.
ModalProperty modal_manager_get_properties(void);

//! Renders the highest priority top opaque window and all windows with higher priority.
//! @param ctx The \ref GContext in which to render.
//! @return Modal properties such as whether the modals are transparent or unfocusable
void modal_manager_render(GContext *ctx);

//! Determines whether the given modal window is visible. Use window_manager_is_window_visible if
//! both app windows and modal windows need to be considered.
//! @param window The modal window to determine whether it is visible.
//! @return true if the modal window is visible, false otherwise
bool modal_manager_is_window_visible(Window *window);

//! Determines whether the given modal window is focused. Use window_manager_is_window_focused if
//! both app windows and modal windows need to be considered.
//! @param window The modal window to determine whether it is focused.
//! @return true if the modal window is focused, false otherwise
bool modal_manager_is_window_focused(Window *window);

//! Wrapper to call \ref window_stack_push() with the appropriate stack for
//! the given \ref ModalPriority
//! @param window The window to push onto the stack
//! @param priority The priority of the window stack to push to
//! @param animated `True` for animated, otherwise `False`
void modal_window_push(Window *window, ModalPriority priority, bool animated);

//! Reset the modal manager state. Useful for unit testing.
void modal_manager_reset(void);
