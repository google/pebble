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

#include "applib/ui/animation.h"
#include "applib/ui/layer.h"
#include "applib/ui/property_animation_private.h"

typedef struct WindowTransitioningContext WindowTransitioningContext;

typedef Animation *(*WindowTransitionImplementationCreateAnimationFunc)(
  WindowTransitioningContext *context);
typedef void (*WindowTransitionImplementationRenderFunc)(WindowTransitioningContext *context,
                                                         GContext *ctx);

// NOTE: container layer not yet implemented - once there:
// can assume
//   context.window_from.layer.parent == context.container_layer &&
//   context.window_from.layer.parent == context.container_layer
// needs to
//   create an animation that drives the visible transition
//   (e.g. by moving context.window_to.layer.frame)
//   call context.window_from.handlers.disappear and context.window_to.handlers.appear et al.
// doesn't need to
//   restore context.window_from.layer.frame (framework will do that)
// if no animation is returned by .create_animation, the windows will be replaced immediately
typedef struct {
  WindowTransitionImplementationCreateAnimationFunc create_animation;
  WindowTransitionImplementationRenderFunc render;
} WindowTransitionImplementation;

typedef struct WindowTransitioningContext {
  Layer *container_layer;
  Window *window_from;
  Window *window_to;
  //! helper vars to patch dirty pixels in the framebuffer
  int16_t window_from_last_x;
  int16_t window_to_last_x;
  //! provide backwards compatibility for 2.x apps that take window.frame during a transition
  //! to position their UI elements
  GPoint window_to_displacement;
  //! Animation attached to the transitioning context
  Animation *animation;
  //! Window transition implementation
  const WindowTransitionImplementation *implementation;
} WindowTransitioningContext;

const WindowTransitionImplementation *window_transition_get_default_push_implementation(void);
const WindowTransitionImplementation *window_transition_get_default_pop_implementation(void);
extern const WindowTransitionImplementation g_window_transition_none_implementation;

void window_transition_context_appearance_call_all(WindowTransitioningContext *ctx);
