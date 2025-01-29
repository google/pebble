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
#include <stdint.h>
#include <stdbool.h>
#include "util/list.h"
#include "drivers/rtc.h"
#include "applib/ui/animation.h"

//! @file animation.h
//! @addtogroup UI
//! @{
//!   @addtogroup AnimationLegacy2
//!   \brief Abstract framework to create arbitrary animations
//!
//! The AnimationLegacy2 framework provides your Pebble app with an base layer to create arbitrary
//! animations. The simplest way to work with animations is to use the layer frame
//! \ref PropertyAnimationLegacy2, which enables you to move a Layer around on the screen.
//! Using animation_legacy2_set_implementation(), you can implement a custom animation.
//!


//!   @{

///////////////////
// Base AnimationLegacy2
//

struct AnimationLegacy2;
struct AnimationLegacy2Implementation;
struct AnimationLegacy2Handlers;


//! Creates a new AnimationLegacy2 on the heap and initializes it with the default values.
//!
//! * Duration: 250ms,
//! * Curve: \ref AnimationCurveEaseInOut (ease-in-out),
//! * Delay: 0ms,
//! * Handlers: `{NULL, NULL}` (none),
//! * Context: `NULL` (none),
//! * Implementation: `NULL` (no implementation),
//! * Scheduled: no
//! @return A pointer to the animation. `NULL` if the animation could not
//! be created
struct AnimationLegacy2 *animation_legacy2_create(void);

//! Destroys an AnimationLegacy2 previously created by animation_legacy2_create.
void animation_legacy2_destroy(struct AnimationLegacy2 *animation);

//! @internal
//! resets animation to default values (see animation_create)
void animation_legacy2_init(struct AnimationLegacy2 *animation);

//! Sets the time in milliseconds that an animation takes from start to finish.
//! @param animation The animation for which to set the duration.
//! @param duration_ms The duration in milliseconds of the animation. This excludes
//! any optional delay as set using \ref animation_legacy2_set_delay().
void animation_legacy2_set_duration(struct AnimationLegacy2 *animation, uint32_t duration_ms);

//! Sets an optional delay for the animation.
//! @param animation The animation for which to set the delay.
//! @param delay_ms The delay in milliseconds that the animation system should
//! wait from the moment the animation is scheduled to starting the animation.
void animation_legacy2_set_delay(struct AnimationLegacy2 *animation, uint32_t delay_ms);

//! Sets the animation curve for the animation.
//! @param animation The animation for which to set the curve.
//! @param curve The type of curve.
//! @see AnimationCurve
void animation_legacy2_set_curve(struct AnimationLegacy2 *animation, AnimationCurve curve);

//! Sets a custom animation curve function.
//! @param animation The animation for which to set the curve.
//! @param curve_function The custom animation curve function.
//! @see AnimationCurveFunction
void animation_legacy2_set_custom_curve(struct AnimationLegacy2 *animation,
        AnimationCurveFunction curve_function);

//! Gets the custom animation curve function.
//! @param animation The animation for which to get the curve.
//! @return The custom animation curve function for the given animation. NULL if not set.
//! @see AnimationCurveFunction
AnimationCurveFunction animation_legacy2_get_custom_curve(struct AnimationLegacy2 *animation);

//! The function pointer type of the handler that will be called when an animation is started,
//! just before updating the first frame of the animation.
//! @param animation The animation that was started.
//! @param context The pointer to custom, application specific data, as set using
//! \ref animation_legacy2_set_handlers()
//! @note This is called after any optional delay as set by \ref animation_legacy2_set_delay()
//! has expired.
//! @see animation_legacy2_set_handlers
typedef void (*AnimationLegacy2StartedHandler)(struct AnimationLegacy2 *animation, void *context);

//! The function pointer type of the handler that will be called when the animation is stopped.
//! @param animation The animation that was stopped.
//! @param finished True if the animation was stopped because it was finished normally,
//! or False if the animation was stopped prematurely, because it was unscheduled before finishing.
//! @param context The pointer to custom, application specific data, as set using
//! \ref animation_legacy2_set_handlers()
//! @see animation_legacy2_set_handlers
typedef void (*AnimationLegacy2StoppedHandler)(struct AnimationLegacy2 *animation, bool finished,
              void *context);

//! The handlers that will get called when an animation starts and stops.
//! See documentation with the function pointer types for more information.
//! @see animation_legacy2_set_handlers
typedef struct AnimationLegacy2Handlers {
  //! The handler that will be called when an animation is started.
  AnimationLegacy2StartedHandler started;
  //! The handler that will be called when an animation is stopped.
  AnimationLegacy2StoppedHandler stopped;
} AnimationLegacy2Handlers;

//! Sets the callbacks for the animation.
//! Often an application needs to run code at the start or at the end of an animation.
//! Using this function is possible to register callback functions with an animation,
//! that will get called at the start and end of the animation.
//! @param animation The animation for which to set up the callbacks.
//! @param callbacks The callbacks.
//! @param context A pointer to application specific data, that will be passed as an argument by
//! the animation subsystem when a callback is called.
void animation_legacy2_set_handlers(struct AnimationLegacy2 *animation,
              AnimationLegacy2Handlers callbacks, void *context);

//! Gets the application-specific callback context of the animation.
//! This `void` pointer is passed as an argument when the animation system calls AnimationHandlers
//! callbacks. The context pointer can be set to point to any application specific data using
//! \ref animation_legacy2_set_handlers().
//! @param animation The animation.
//! @see animation_legacy2_set_handlers
void *animation_legacy2_get_context(struct AnimationLegacy2 *animation);

//! Schedules the animation. Call this once after configuring an animation to get it to
//! start running.
//!
//! If the animation's implementation has a `.setup` callback it will get called before
//! this function returns.
//!
//! @note If the animation was already scheduled,
//! it will first unschedule it and then re-schedule it again.
//! Note that in that case, the animation's `.stopped` handler, the implementation's
//! `.teardown` and `.setup` will get called, due to the unscheduling and scheduling.
//! @param animation The animation to schedule.
//! @see \ref animation_legacy2_unschedule()
void animation_legacy2_schedule(struct AnimationLegacy2 *animation);

//! Unschedules the animation, which in effect stops the animation.
//! @param animation The animation to unschedule.
//! @note If the animation was not yet finished, unscheduling it will
//! cause its `.stopped` handler to get called, with the "finished" argument set to false.
//! @see \ref animation_legacy2_schedule()
void animation_legacy2_unschedule(struct AnimationLegacy2 *animation);

//! Unschedules all animations of the application.
//! @see animation_legacy2_unschedule
void animation_legacy2_unschedule_all(void);

//! @return True if the animation was scheduled, or false if it was not.
//! @note An animation will be scheduled when it is running and not finished yet.
//! An animation that has finished is automatically unscheduled.
//! @param animation The animation for which to get its scheduled state.
//! @see animation_legacy2_schedule
//! @see animation_legacy2_unschedule
bool animation_legacy2_is_scheduled(struct AnimationLegacy2 *animation);

//! The data structure of an animation.
typedef struct AnimationLegacy2 {
  ListNode list_node;
  const struct AnimationLegacy2Implementation *implementation;
  AnimationLegacy2Handlers handlers;
  void *context;
  //! Absolute time when the animation got scheduled, in ms since system start.
  uint32_t abs_start_time_ms;
  uint32_t delay_ms;
  uint32_t duration_ms;
  AnimationCurve curve:3;
  bool is_completed:1;
  //! Pointer to a custom curve. Unfortunately, due to backward-compatibility
  //! constraints, it must fit into 28 bits.
  //! It is only valid when curve == AnimationCurveCustomFunction.
  //! The mapping from 28-bit field to pointer is unpublished. Call
  //! animation_legacy2_set_custom_curve() to ensure your app continues to run
  //! after future Pebble updates.
  uintptr_t custom_curve_function:28;
} AnimationLegacy2;

///////////////////////////////////////
// Implementing custom animation types

//! Pointer to function that (optionally) prepares the animation for running.
//! This callback is called when the animation is added to the scheduler.
//! @param animation The animation that needs to be set up.
//! @see animation_legacy2_schedule
//! @see AnimationTeardownImplementation
typedef void (*AnimationLegacy2SetupImplementation)(struct AnimationLegacy2 *animation);

//! Pointer to function that updates the animation according to the given normalized distance.
//! This callback will be called repeatedly by the animation scheduler whenever the animation needs
//! to be updated.
//! @param animation The animation that needs to update; gets passed in by the animation framework.
//! @param distance_normalized The current normalized distance; gets passed in by the animation
//! framework for each animation frame.
//! This is a value between \ref ANIMATION_NORMALIZED_MIN and \ref ANIMATION_NORMALIZED_MAX.
//! At the start of the animation, the value will be \ref ANIMATION_NORMALIZED_MIN.
//! At the end of the animation, the value will be \ref ANIMATION_NORMALIZED_MAX.
//! For each frame during the animation, the value will be the distance along the
//! animation path, mapped between \ref ANIMATION_NORMALIZED_MIN and
//! \ref ANIMATION_NORMALIZED_MAX based on the animation duration and the
//! \ref AnimationCurve set.
//! For example, say an animation was scheduled at t = 1.0s, has a delay of 1.0s,
//! a duration of 2.0s and a curve of AnimationCurveLinear.
//! Then the .update callback will get called on t = 2.0s with
//! distance_normalized = \ref ANIMATION_NORMALIZED_MIN. For each frame
//! thereafter until t = 4.0s, the update callback will get called where
//! distance_normalized is
//! (\ref ANIMATION_NORMALIZED_MIN + (((\ref ANIMATION_NORMALIZED_MAX -
//! \ref ANIMATION_NORMALIZED_MIN) * t) / duration)).
//! Other animation curves will result in a non-linear relation between
//! distance_normalized and time.
//! @internal
//! @see animation_legacy2_timing.h
typedef void (*AnimationLegacy2UpdateImplementation)(struct AnimationLegacy2 *animation,
                const uint32_t distance_normalized);

//! Pointer to function that (optionally) cleans up the animation.
//! This callback is called when the animation is removed from the scheduler.
//! In case the `.setup` implementation
//! allocated any memory, this is a good place to release that memory again.
//! @param animation The animation that needs to be teared down.
//! @see animation_legacy2_unschedule
//! @see AnimationSetupImplementation
typedef void (*AnimationLegacy2TeardownImplementation)(struct AnimationLegacy2 *animation);

//! The 3 callbacks that implement a custom animation.
//! Only the `.update` callback is mandatory, `.setup` and `.teardown` are optional.
//! See the documentation with the function pointer typedefs for more information.
//!
//! @note The `.setup` callback is called immediately after scheduling the animation,
//! regardless if there is a delay set for that animation using \ref animation_legacy2_set_delay().
//!
//! The diagram below illustrates the order in which callbacks can be expected to get called
//! over the life cycle of an animation. It also illustrates where the implementation of
//! different animation callbacks are intended to be “living”.
//! ![](animations.png)
//!
//! @see AnimationLegacy2SetupImplementation
//! @see AnimationLegacy2UpdateImplementation
//! @see AnimationLegacy2TeardownImplementation
typedef struct AnimationLegacy2Implementation {
  //! Called by the animation system when an animation is scheduled, to prepare it for running.
  //! This callback is optional and can be left `NULL` when not needed.
  AnimationLegacy2SetupImplementation setup;
  //! Called by the animation system when the animation needs to calculate the next animation frame.
  //! This callback is mandatory and should not be left `NULL`.
  AnimationLegacy2UpdateImplementation update;
  //! Called by the animation system when an animation is unscheduled, to clean up after it has run.
  //! This callback is optional and can be left `NULL` when not needed.
  AnimationLegacy2TeardownImplementation teardown;
} AnimationLegacy2Implementation;

//! Sets the implementation of the custom animation.
//! When implementing custom animations, use this function to specify what functions need to be called to
//! for the setup, frame update and teardown of the animation.
//! @param animation The animation for which to set the implementation.
//! @param implementation The structure with function pointers to the implementation of the setup, update and teardown functions.
//! @see AnimationImplementation
void animation_legacy2_set_implementation(struct AnimationLegacy2 *animation,
      const AnimationLegacy2Implementation *implementation);

//!   @} // group AnimationLegacy2
//! @} // group UI
