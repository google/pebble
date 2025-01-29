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
#include "animation_interpolate.h"
#include "drivers/rtc.h"
#include "util/list.h"

//! @file animation.h
//! @addtogroup UI
//! @{
//!   @addtogroup Animation
//!   \brief Abstract framework to create arbitrary animations
//!
//! The Animation framework provides your Pebble app with an base layer to create arbitrary
//! animations. The simplest way to work with animations is to use the layer frame
//! \ref PropertyAnimation, which enables you to move a Layer around on the screen.
//! Using animation_set_implementation(), you can implement a custom animation.
//!
//! Refer to the \htmlinclude UiFramework.html (chapter "Animation") for a conceptual overview
//! of the animation framework and on how to write custom animations.


//!   @{

///////////////////
// Base Animation
//

struct Animation;
typedef struct Animation Animation;
struct AnimationImplementation;
struct AnimationHandlers;

//! @internal
//! Immutable animations are animations whose properties cannot be changed, such as animations that
//! have been already scheduled. They can be typecasted to Animations for use, but keep in mind not
//! all animation methods will have an effect.
struct ImmutableAnimation;
typedef struct ImmutableAnimation ImmutableAnimation;

//! The normalized distance at the start of the animation.
#define ANIMATION_NORMALIZED_MIN 0

//! The normalized distance at the end of the animation.
#define ANIMATION_NORMALIZED_MAX 65535

//! Constant to indicate infinite play count.
//! Can be passed to \ref animation_set_play_count() to repeat indefinitely.
//! @note This can be returned by \ref animation_get_play_count().
#define ANIMATION_PLAY_COUNT_INFINITE UINT32_MAX

//! Constant to indicate "infinite" duration.
//! This can be used with \ref animation_set_duration() to indicate that the animation
//! should run indefinitely. This is useful when implementing for example a frame-by-frame
//! simulation that does not have a clear ending (e.g. a game).
//! @note Note that `distance_normalized` parameter that is passed
//! into the `.update` implementation is meaningless in when an infinite duration is used.
//! @note This can be returned by animation_get_duration (if the play count is infinite)
#define ANIMATION_DURATION_INFINITE UINT32_MAX

//! @internal
//! The default animation duration in milliseconds
#define ANIMATION_DEFAULT_DURATION_MS 250

//! @internal
//! aimed to duration of a single frame
//! 1000ms / 30 Hz
#define ANIMATION_TARGET_FRAME_INTERVAL_MS 33


//! The type used to represent how far an animation has progressed. This is passed to the
//! animation's update handler
typedef int32_t AnimationProgress;

//! Values that are used to indicate the different animation curves,
//! which determine the speed at which the animated value(s) change(s).
typedef enum {
  //! Linear curve: the velocity is constant.
  AnimationCurveLinear = 0,
  //! Bicubic ease-in: accelerate from zero velocity
  AnimationCurveEaseIn = 1,
  //! Bicubic ease-in: decelerate to zero velocity
  AnimationCurveEaseOut = 2,
  //! Bicubic ease-in-out: accelerate from zero velocity, decelerate to zero velocity
  AnimationCurveEaseInOut = 3,
  AnimationCurveDefault = AnimationCurveEaseInOut,
  //! Custom (user-provided) animation curve
  AnimationCurveCustomFunction = 4,
  //! User-provided interpolation function
  AnimationCurveCustomInterpolationFunction = 5,
  // Two more Reserved for forward-compatibility use.
  AnimationCurve_Reserved1 = 6,
  AnimationCurve_Reserved2 = 7,
} AnimationCurve;


//! Creates a new Animation on the heap and initializes it with the default values.
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
Animation * animation_create(void);

//! Destroys an Animation previously created by animation_create.
//! @return true if successful, false on failure
bool animation_destroy(Animation *animation);

// Clone an existing animation. Especially useful when it will be used in 2 or more other
// sequence or spawn animations.
Animation *animation_clone(Animation *from);


//! Create a new sequence animation from a list of 2 or more other animations. The returned
//! animation owns the animations that were provided as arguments and no further write operations
//! on those handles are allowed. The variable length argument list must be terminated with a NULL
//! ptr
//! @note the maximum number of animations that can be supplied to this method is 20
//! @param animation_a the first required component animation
//! @param animation_b the second required component animation
//! @param animation_c either the third component, or NULL if only adding 2 components
//! @return The newly created sequence animation
Animation *animation_sequence_create(Animation *animation_a, Animation *animation_b,
                                     Animation *animation_c, ...);

//! An alternate form of animation_sequence_create() that accepts an array of other animations.
//! @note the maximum number of elements allowed in animation_array is 256
//! @param animation_array an array of component animations to include
//! @param array_len the number of elements in the animation_array
//! @return The newly created sequence animation
Animation *animation_sequence_create_from_array(Animation **animation_array, uint32_t array_len);

//! @internal
//! An alternate form of animation_sequence_create() that accepts an array of other animations.
//! It also takes an animation that will be converted into the animation sequence.
//! @note the maximum number of elements allowed in animation_array is 256
//! @param parent a freshly created animation to convert into a sequence
//! @param animation_array an array of component animations to include
//! @param array_len the number of elements in the animation_array
//! @return The initialized sequence animation
Animation *animation_sequence_init_from_array(Animation *parent, Animation **animation_array,
                                              uint32_t array_len);

//! Create a new spawn animation from a list of 2 or more other animations. The returned
//! animation owns the animations that were provided as arguments and no further write operations
//! on those handles are allowed. The variable length argument list must be terminated with a NULL
//! ptr
//! @note the maximum number of animations that can be supplied to this method is 20
//! @param animation_a the first required component animation
//! @param animation_b the second required component animation
//! @param animation_c either the third component, or NULL if only adding 2 components
//! @return The newly created spawn animation or NULL on failure
Animation *animation_spawn_create(Animation *animation_a, Animation *animation_b,
                                  Animation *animation_c, ...);

//! An alternate form of animation_spawn_create() that accepts an array of other animations.
//! @note the maximum number of elements allowed in animation_array is 256
//! @param animation_array an array of component animations to include
//! @param array_len the number of elements in the animation_array
//! @return The newly created spawn animation or NULL on failure
Animation *animation_spawn_create_from_array(Animation **animation_array, uint32_t array_len);

//! @internal
//! Sets an animation as immutable. An immutable animation cannot have its properties changed.
//! Useful for animations that are meant to be passed publicly, but have special handlers that
//! must not be overwritten.
//! @return true if successful, false on failure
bool animation_set_immutable(Animation *animation);

//! @internal
bool animation_is_immutable(Animation *animation);

//! @internal
//! Set the auto-destroy flag for this animation. If set on, then the animation will be
//! automatically destroyed if/when the animation finishes after being scheduled or if
//! animation_unschedule() or animation_unschedule_all() are called.
//! @param animation the animation for which to set the auto_destroy setting
//! @param auto_destroy the new setting
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @return true if successful, false on failure
bool animation_set_auto_destroy(Animation *animation, bool auto_destroy);

//! Seek to a specific location in the animation. Only forward seeking is allowed. Returns true
//! if successful, false if the passed in seek location is invalid.
//! @param animation the animation for which to set the elapsed.
//! @param elapsed_ms the new elapsed time in milliseconds
//! @return true if successful, false if the requested elapsed is invalid.
bool animation_set_elapsed(Animation *animation, uint32_t elapsed_ms);

//! Get the current location in the animation.
//! @note The animation must be scheduled to get the elapsed time. If it is not schedule,
//! this method will return false.
//! @param animation The animation for which to fetch the elapsed.
//! @param[out] elapsed_ms pointer to variable that will contain the elapsed time in milliseconds
//! @return true if successful, false on failure
bool animation_get_elapsed(Animation *animation, int32_t *elapsed_ms);

//! @internal
//! Get the current progress of the animation.
//! @note The animation must be scheduled to get the progress time. If it is not scheduled,
//! this method will return false.
//! @param animation The animation for which to fetch the progress.
//! @param[out] progress_out Pointer to variable that will contain the progress.
//! @return true if successful, false on failure
bool animation_get_progress(Animation *animation, AnimationProgress *progress_out);

//! Set an animation to run in reverse (or forward)
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation the animation to operate on
//! @param reverse set to true to run in reverse, false to run forward
//! @return true if successful, false on failure
bool animation_set_reverse(Animation *animation, bool reverse);

//! Get the reverse setting of an animation
//! @param animation The animation for which to get the setting
//! @return the reverse setting
bool animation_get_reverse(Animation *animation);

//! Set an animation to play N times. The default is 1.
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation the animation to set the play count of
//! @param play_count number of times to play this animation. Set to ANIMATION_PLAY_COUNT_INFINITE
//! to make an animation repeat indefinitely.
//! @return true if successful, false on failure
bool animation_set_play_count(Animation *animation, uint32_t play_count);

//! Get the play count of an animation
//! @param animation The animation for which to get the setting
//! @return the play count
uint32_t animation_get_play_count(Animation *animation);

//! Sets the time in milliseconds that an animation takes from start to finish.
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation The animation for which to set the duration.
//! @param duration_ms The duration in milliseconds of the animation. This excludes
//! any optional delay as set using \ref animation_set_delay().
//! @return true if successful, false on failure
bool animation_set_duration(Animation *animation, uint32_t duration_ms);

//! Get the static duration of an animation from start to end (ignoring how much has already
//! played, if any).
//! @param animation The animation for which to get the duration
//! @param include_delay if true, include the delay time
//! @param include_play_count if true, incorporate the play_count
//! @return the duration, in milliseconds. This includes any optional delay a set using
//! \ref animation_set_delay.
uint32_t animation_get_duration(Animation *animation, bool include_delay, bool include_play_count);

//! Sets an optional delay for the animation.
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation The animation for which to set the delay.
//! @param delay_ms The delay in milliseconds that the animation system should
//! wait from the moment the animation is scheduled to starting the animation.
//! @return true if successful, false on failure
bool animation_set_delay(Animation *animation, uint32_t delay_ms);

//! Get the delay of an animation in milliseconds
//! @param animation The animation for which to get the setting
//! @return the delay in milliseconds
uint32_t animation_get_delay(Animation *animation);

//! Sets the animation curve for the animation.
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation The animation for which to set the curve.
//! @param curve The type of curve.
//! @see AnimationCurve
//! @return true if successful, false on failure
bool animation_set_curve(Animation *animation, AnimationCurve curve);

//! Gets the animation curve for the animation.
//! @param animation The animation for which to get the curve.
//! @return The type of curve.
AnimationCurve animation_get_curve(Animation *animation);

//! The function pointer type of a custom animation curve.
//! @param linear_distance The linear normalized animation distance to be curved.
//! @see animation_set_custom_curve
typedef AnimationProgress (*AnimationCurveFunction)(AnimationProgress linear_distance);

//! Sets a custom animation curve function.
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation The animation for which to set the curve.
//! @param curve_function The custom animation curve function.
//! @see AnimationCurveFunction
//! @return true if successful, false on failure
bool animation_set_custom_curve(Animation *animation, AnimationCurveFunction curve_function);

//! Gets the custom animation curve function for the animation.
//! @param animation The animation for which to get the curve.
//! @return The custom animation curve function for the given animation. NULL if not set.
AnimationCurveFunction animation_get_custom_curve(Animation *animation);

//! @internal
//! Sets the custom interpolation function for the animation to override the underlying behavior
//! of \ref interpolate_int64 and related functions. This can be used to implement spatial easing.
//! Animation curve and interpolation function are mutually exclusive.
//! @param animation The animation for which to set the interpolation function.
//! @param interpolate_function The custom interpolation function to use.
bool animation_set_custom_interpolation(Animation *animation_h,
                                        InterpolateInt64Function interpolate_function);

//! @internal
//! Get the custom interpolation function for the animation.
//! @param animation The animation for which to get the interpolation function.
//! @return The custom interpolation function for the given animation. NULL if not used.
InterpolateInt64Function animation_get_custom_interpolation(Animation *animation);

//! The function pointer type of the handler that will be called when an animation is started,
//! just before updating the first frame of the animation.
//! @param animation The animation that was started.
//! @param context The pointer to custom, application specific data, as set using
//! \ref animation_set_handlers()
//! @note This is called after any optional delay as set by \ref animation_set_delay() has expired.
//! @see animation_set_handlers
typedef void (*AnimationStartedHandler)(Animation *animation, void *context);

//! The function pointer type of the handler that will be called when the animation is stopped.
//! @param animation The animation that was stopped.
//! @param finished True if the animation was stopped because it was finished normally,
//! or False if the animation was stopped prematurely, because it was unscheduled before finishing.
//! @param context The pointer to custom, application specific data, as set using
//! \ref animation_set_handlers()
//! @see animation_set_handlers
//! \note
//! This animation (i.e.: the `animation` parameter) may be destroyed here.
//! It is not recommended to unschedule or destroy a **different** Animation within this
//! Animation's `stopped` handler.
typedef void (*AnimationStoppedHandler)(Animation *animation, bool finished, void *context);

//! The handlers that will get called when an animation starts and stops.
//! See documentation with the function pointer types for more information.
//! @see animation_set_handlers
typedef struct AnimationHandlers {
  //! The handler that will be called when an animation is started.
  AnimationStartedHandler started;
  //! The handler that will be called when an animation is stopped.
  AnimationStoppedHandler stopped;
} AnimationHandlers;

//! Sets the callbacks for the animation.
//! Often an application needs to run code at the start or at the end of an animation.
//! Using this function is possible to register callback functions with an animation,
//! that will get called at the start and end of the animation.
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation The animation for which to set up the callbacks.
//! @param callbacks The callbacks.
//! @param context A pointer to application specific data, that will be passed as an argument by
//! the animation subsystem when a callback is called.
//! @return true if successful, false on failure
bool animation_set_handlers(Animation *animation, AnimationHandlers callbacks, void *context);

//! Gets the callbacks for the animation.
//! @param animation The animation for which to set up the callbacks.
//! @return the callbacks in use by this animation
AnimationHandlers animation_get_handlers(Animation *animation_h);

//! Gets the application-specific callback context of the animation.
//! This `void` pointer is passed as an argument when the animation system calls AnimationHandlers
//! callbacks. The context pointer can be set to point to any application specific data using
//! \ref animation_set_handlers().
//! @param animation The animation.
//! @see animation_set_handlers
void *animation_get_context(Animation *animation);

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
//! @see \ref animation_unschedule()
//! @return true if successful, false on failure
bool animation_schedule(Animation *animation);

//! Unschedules the animation, which in effect stops the animation.
//! @param animation The animation to unschedule.
//! @note If the animation was not yet finished, unscheduling it will
//! cause its `.stopped` handler to get called, with the "finished" argument set to false.
//! @note If the animation is not scheduled or NULL, calling this routine is
//! effectively a no-op
//! @see \ref animation_schedule()
//! @return true if successful, false on failure
bool animation_unschedule(Animation *animation);

//! Unschedules all animations of the application.
//! @see animation_unschedule
void animation_unschedule_all(void);

//! @return True if the animation was scheduled, or false if it was not.
//! @note An animation will be scheduled when it is running and not finished yet.
//! An animation that has finished is automatically unscheduled.
//! For convenience, passing in a NULL animation argument will simply return false
//! @param animation The animation for which to get its scheduled state.
//! @see animation_schedule
//! @see animation_unschedule
bool animation_is_scheduled(Animation *animation);

///////////////////////////////////////
// Implementing custom animation types

//! Pointer to function that (optionally) prepares the animation for running.
//! This callback is called when the animation is added to the scheduler.
//! @param animation The animation that needs to be set up.
//! @see animation_schedule
//! @see AnimationTeardownImplementation
typedef void (*AnimationSetupImplementation)(Animation *animation);

//! Pointer to function that updates the animation according to the given normalized progress.
//! This callback will be called repeatedly by the animation scheduler whenever the animation needs
//! to be updated.
//! @param animation The animation that needs to update; gets passed in by the animation framework.
//! @param progress The current normalized progress; gets passed in by the animation
//! framework for each animation frame.
//! The value \ref ANIMATION_NORMALIZED_MIN represents the start and \ref ANIMATION_NORMALIZED_MAX
//! represents the end. Values outside this range (generated by a custom curve function) can be used
//! to implement features like a bounce back effect, where the progress exceeds the desired final
//! value before returning to complete the animation.
//! When using a system provided curve function, each frame during the animation will have a
//! progress value between \ref ANIMATION_NORMALIZED_MIN and \ref ANIMATION_NORMALIZED_MAX based on
//! the animation duration and the \ref AnimationCurve.
//! For example, say an animation was scheduled at t = 1.0s, has a delay of 1.0s, a duration of 2.0s
//! and a curve of AnimationCurveLinear. Then the .update callback will get called on t = 2.0s with
//! distance_normalized = \ref ANIMATION_NORMALIZED_MIN. For each frame thereafter until t = 4.0s,
//! the update callback will get called where distance_normalized is (\ref ANIMATION_NORMALIZED_MIN
//! + (((\ref ANIMATION_NORMALIZED_MAX - \ref ANIMATION_NORMALIZED_MIN) * t) / duration)).
//! Other system animation curve functions will result in a non-linear relation between
//! distance_normalized and time.
//! @internal
//! @see animation_timing.h
typedef void (*AnimationUpdateImplementation)(Animation *animation,
              const AnimationProgress progress);

//! Pointer to function that (optionally) cleans up the animation.
//! This callback is called when the animation is removed from the scheduler.
//! In case the `.setup` implementation
//! allocated any memory, this is a good place to release that memory again.
//! @param animation The animation that needs to be teared down.
//! @see animation_unschedule
//! @see AnimationSetupImplementation
typedef void (*AnimationTeardownImplementation)(Animation *animation);

//! The 3 callbacks that implement a custom animation.
//! Only the `.update` callback is mandatory, `.setup` and `.teardown` are optional.
//! See the documentation with the function pointer typedefs for more information.
//!
//! @note The `.setup` callback is called immediately after scheduling the animation,
//! regardless if there is a delay set for that animation using \ref animation_set_delay().
//!
//! The diagram below illustrates the order in which callbacks can be expected to get called
//! over the life cycle of an animation. It also illustrates where the implementation of
//! different animation callbacks are intended to be “living”.
//! ![](animations.png)
//!
//! @see AnimationSetupImplementation
//! @see AnimationUpdateImplementation
//! @see AnimationTeardownImplementation
typedef struct AnimationImplementation {
  //! Called by the animation system when an animation is scheduled, to prepare it for running.
  //! This callback is optional and can be left `NULL` when not needed.
  AnimationSetupImplementation setup;
  //! Called by the animation system when the animation needs to calculate the next animation frame.
  //! This callback is mandatory and should not be left `NULL`.
  AnimationUpdateImplementation update;
  //! Called by the animation system when an animation is unscheduled, to clean up after it has run.
  //! This callback is optional and can be left `NULL` when not needed.
  AnimationTeardownImplementation teardown;
} AnimationImplementation;

//! Gets the implementation of the custom animation.
//! @param animation The animation for which to get the implementation.
//! @see AnimationImplementation
//! @return NULL if animation implementation has not been setup.
const AnimationImplementation* animation_get_implementation(Animation *animation);

//! Sets the implementation of the custom animation.
//! When implementing custom animations, use this function to specify what functions need to be
//! called to for the setup, frame update and teardown of the animation.
//! @note Trying to set an attribute when an animation is immutable will return false (failure). An
//! animation is immutable once it has been added to a sequence or spawn animation or has been
//! scheduled.
//! @param animation The animation for which to set the implementation.
//! @param implementation The structure with function pointers to the implementation of the setup,
//!  update and teardown functions.
//! @see AnimationImplementation
//! @return true if successful, false on failure
bool animation_set_implementation(Animation *animation,
                                  const AnimationImplementation *implementation);

//!   @} // group Animation
//! @} // group UI
