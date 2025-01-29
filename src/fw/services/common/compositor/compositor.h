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

#include "applib/ui/property_animation.h"
#include "applib/ui/window.h"

//! @file kernel/services/compositor.h
//! This file manages what's currently shown on the screen of your Pebble!
//! There are two main things that are managed by the compositor...
//!
//! <h3>The App Framebuffer</h3>
//! This is the framebuffer the app is currently drawing into. The compositor
//! handles animating between app framebuffers when the app changes and window
//! animations requested by the window stack. The compositor will also draw in
//! the status bar when the app is in fullscreen, and the app will adjust its
//! framebuffer's destination frame vertically. The framebuffer is simply
//! bitblt'ed into the appropriate position whenever compositor_flush is called.
//!
//! @see \ref compositor_get_child_entity
//! @see \ref compositor_set_entity_framebuffer
//! @see \ref compositor_transition
//! @see \ref window_stack_animation_schedule
//! @see \ref app_emit_render_ready_event
//!
//! <h3>Modal Window</h3>
//! A modal window is a Window that can be rendered on top of an app without
//! interrupting it. The modal window can only be supplied by the kernel, so
//! we can trust its contents. The modal window is animated up and down the
//! screen when its pushed and popped. Since the window doesn't have a framebuffer
//! of its own, we render it to the main framebuffer on top of everything else
//! whenever compositor_flush is called.
//!

//! Transition direction, from the current position to the next.
//! For example, Up is a transition to some item that is upwards of the current screen.
typedef enum {
  CompositorTransitionDirectionUp,
  CompositorTransitionDirectionDown,
  CompositorTransitionDirectionLeft,
  CompositorTransitionDirectionRight,
  CompositorTransitionDirectionNone,
} CompositorTransitionDirection;

typedef void (*CompositorTransitionInitFunc)(Animation *animation);

// TODO: PBL-31460 Change compositor transitions to use AnimationProgress
// This would enable time-based bounce back transitions
typedef void (*CompositorTransitionUpdateFunc)(GContext *ctx, Animation *animation,
                                              uint32_t distance_normalized);

typedef void (*CompositorTransitionTeardownFunc)(Animation *animation);

typedef struct CompositorTransition {
  CompositorTransitionInitFunc init;          //!< Mandatory initialization function
  CompositorTransitionUpdateFunc update;      //!< Mandatory update function
  CompositorTransitionTeardownFunc teardown;  //!< Optional teardown function
  //! If false, modals are rendered after the update function, otherwise they are skipped
  bool skip_modal_render_after_update;
} CompositorTransition;

typedef struct FrameBuffer FrameBuffer;

void compositor_init(void);

//! Kick off a transition using the given CompositorTransition implementation. If a transition is
//! already underway the transition will be immediately cancelled and this one will be scheduled
//! in its place.
//!
//! For modal windows the new app we're animating to should already be on top of the modal window
//! stack. For apps the new app we're animating to should always be running. For apps, the
//! animation won't begin until the app has already started rendering itself.
void compositor_transition(const CompositorTransition *impl);

//! Perform the compositor transition rendering steps for a given update function.
//! Normally you will not call this, as the assigned transition animation automatically runs this.
//! However, if an animation needs to be scheduled inside of the compositor transition animation,
//! the new animation will need to call this in order to properly render.
//! A good use-case for this is when the transition needs to use an animation_sequence.
//!
//! For an example of this being used, check out compositor_shutter_transitions.c
void compositor_transition_render(CompositorTransitionUpdateFunc func, Animation *animation,
                                  const AnimationProgress distance_normalized);

//! Writes the app framebuffer to either the system framebuffer or display directly.
//! Calls compositor_render_modal if all modals are transparent as well.
void compositor_render_app(void);

//! Renders modals using the kernel graphics context
void compositor_render_modal(void);

//! The modal needs to redraw its buffer to the display.
void compositor_modal_render_ready(void);

//! The app needs to copy its framebuffer to the display.
void compositor_app_render_ready(void);

FrameBuffer* compositor_get_framebuffer(void);

GBitmap compositor_get_framebuffer_as_bitmap(void);

//! Gets the app framebuffer as a bitmap. The bounds of the bitmap will be set based on
//! app_manager_get_framebuffer_size() rather than the app's framebuffer size to protect against
//! malicious apps changing it.
GBitmap compositor_get_app_framebuffer_as_bitmap(void);

//! @return True if we're currently mid-animation between apps or modal windows
bool compositor_is_animating(void);

//! Sets the modal draw offset for transitions that redraw the modal
void compositor_set_modal_transition_offset(GPoint modal_offset);

//! Stops an existing transition in its tracks.
void compositor_transition_cancel(void);

//! Don't allow new frames to be pushed to the compositor from either the app or the modal.
void compositor_freeze(void);

//! Resuming allowing new frames to be pushed to the compositor, undoes the effects of
//! compositor_freeze.
void compositor_unfreeze(void);
