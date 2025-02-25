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
#include "animation_legacy2.h"
#include "applib/ui/property_animation.h"
#include "applib/graphics/gtypes.h"

//////////////////////
// Legacy2 Property Animations
//

//! @file property_animation_legacy2_legacy2.h

//! @addtogroup UI
//! @{
//!   @addtogroup AnimationLegacy2
//!   @{
//!     @addtogroup PropertyAnimationLegacy2
//! \brief Concrete animations to move a layer around over time
//!
//! Actually, property animations do more than just moving a Layer around over time.
//! PropertyAnimationLegacy2 is a concrete class of animations and is built around the Animation
//! subsystem, which covers anything timing related, but does not move anything around.
//! A PropertyAnimation animates a "property" of a "subject".
//!
//! <h3>Animating a Layer's frame property</h3>
//! Currently there is only one specific type of property animation offered off-the-shelf, namely
//! one to change the frame (property) of a layer (subject), see
//! \ref property_animation_legacy2_create_layer_frame().
//!
//! <h3>Implementing a custom PropertyAnimationLegacy2</h3>
//! It is fairly simple to create your own variant of a PropertyAnimationLegacy2.
//!
//! Please refer to \htmlinclude UiFramework.html (chapter "Property Animations") for a conceptual
//! overview
//! of the animation framework and make sure you understand the underlying \ref Animation, in case
//! you are not familiar with it, before trying to implement a variation on
//! PropertyAnimationLegacy2.
//!
//! To implement a custom property animation, use \ref property_animation_legacy2_create() and
//! provide a
//! function pointers to the accessors (getter and setter) and setup, update and teardown callbacks
//! in the implementation argument.
//! Note that the type of property to animate with \ref PropertyAnimationLegacy2 is limited to
//! int16_t, GPoint or GRect.
//!
//! For each of these types, there are implementations provided
//! for the necessary `.update` handler of the animation: see
//! \ref property_animation_legacy2_update_int16(), \ref property_animation_legacy2_update_gpoint()
//! and  \ref property_animation_legacy2_update_grect().
//! These update functions expect the `.accessors` to conform to the following interface:
//! Any getter needs to have the following function signature: `__type__ getter(void *subject);`
//! Any setter needs to have to following function signature: `void setter(void *subject,
//! __type__ value);`
//! See \ref Int16Getter, \ref Int16Setter, \ref GPointGetter, \ref GPointSetter, \ref GRectGetter,
//! \ref GRectSetter
//! for the typedefs that accompany the update functions.
//!
//! \code{.c}
//! static const PropertyAnimationLegacy2Implementation my_implementation = {
//!   .base = {
//!     // using the "stock" update callback:
//!     .update = (AnimationUpdateImplementation) property_animation_legacy2_update_gpoint,
//!   },
//!   .accessors = {
//!     // my accessors that get/set a GPoint from/onto my subject:
//!     .setter = { .gpoint = my_layer_set_corner_point, },
//!     .getter = { .gpoint = (const GPointGetter) my_layer_get_corner_point, },
//!   },
//! };
//! static PropertyAnimationLegacy2* s_my_animation_ptr = NULL;
//! static GPoint s_to_point = GPointZero;
//! ...
//! // Use NULL as 'from' value, this will make the animation framework call the getter
//! // to get the current value of the property and use that as the 'from' value:
//! s_my_animation_ptr = property_animation_legacy2_create(&my_implementation, my_layer, NULL,
//! &s_to_point);
//! animation_schedule(s_my_animation_ptr->animation);
//! \endcode
//!   @{

struct PropertyAnimationLegacy2;
struct Layer;
struct PropertyAnimationLegacy2Implementation;

//! @internal
void property_animation_legacy2_init_layer_frame(
    struct PropertyAnimationLegacy2 *property_animation, struct Layer *layer, GRect *from_frame,
    GRect *to_frame);

//! Convenience function to create and initialize a property animation that animates the frame of a
//! Layer. It sets up the PropertyAnimationLegacy2 to use \ref layer_set_frame() and
//! \ref layer_get_frame() as accessors and uses the `layer` parameter as the subject for the
//! animation. The same defaults are used as with \ref animation_create().
//! @param layer the layer that will be animated
//! @param from_frame the frame that the layer should animate from
//! @param to_frame the frame that the layer should animate to
//! @note Pass in `NULL` as one of the frame arguments to have it set automatically to the layer's
//! current frame. This will result in a call to \ref layer_get_frame() to get the current frame of
//! the layer.
//! @return A pointer to the property animation. `NULL` if animation could not
//! be created
struct PropertyAnimationLegacy2* property_animation_legacy2_create_layer_frame(struct Layer *layer,
                          GRect *from_frame, GRect *to_frame);

//////////////////////////////////////////
// Implementing custom Property Animations
//

//! @internal
//! See \ref property_animation_legacy2_create() for a description of the parameter list
void property_animation_legacy2_init(struct PropertyAnimationLegacy2 *property_animation,
      const struct PropertyAnimationLegacy2Implementation *implementation, void *subject,
      void *from_value, void *to_value);

//! Creates a new PropertyAnimationLegacy2 on the heap and initializes it with the specified
//! values. The same defaults are used as with \ref animation_create().
//! If the `from_value` or the `to_value` is `NULL`, the getter accessor will be called to get the
//! current value of the property and be used instead.
//! @param implementation Pointer to the implementation of the animation. In most cases, it makes
//! sense to pass in a `static const` struct pointer.
//! @param subject Pointer to the "subject" being animated. This will be passed in when the
//! getter/setter accessors are called,
//! see \ref PropertyAnimationLegacy2Accessors, \ref GPointSetter, and friends. The value of this
//! pointer will be copied into the `.subject` field of the PropertyAnimationLegacy2 struct.
//! @param from_value Pointer to the value that the subject should animate from
//! @param to_value Pointer to the value that the subject should animate to
//! @note Pass in `NULL` as one of the value arguments to have it set automatically to the
//! subject's current property value, as returned by the getter function. Also note that passing in
//! `NULL` for both `from_value` and `to_value`, will result in the animation having the same from-
//! and to- values, effectively not doing anything.
//! @return A pointer to the property animation. `NULL` if animation could not
//! be created
struct PropertyAnimationLegacy2* property_animation_legacy2_create(
      const struct PropertyAnimationLegacy2Implementation *implementation, void *subject,
      void *from_value, void *to_value);

//! Free a dynamically allocated property animation
//! @param property_animation The property animation to be freed.
void property_animation_legacy2_destroy(struct PropertyAnimationLegacy2* property_animation);

//! Default update callback for a property animations to update a property of type int16_t.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationLegacy2Implementation,
//! in combination with a `.getter` and `.setter` accessors of types \ref Int16Getter and
//! \ref Int16Setter.
//! The implementation of this function will calculate the next value of the animation and call the
//! setter to set the new value upon the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See
//! \ref AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_legacy2_update_int16(struct PropertyAnimationLegacy2 *property_animation,
      const uint32_t distance_normalized);

//! Default update callback for a property animations to update a property of type GPoint.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationLegacy2Implementation, in combination with a `.getter` and `.setter` accessors
//! of types \ref GPointGetter and \ref GPointSetter.
//! The implementation of this function will calculate the next point of the animation and call the
//! setter to set the new point upon the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See
//! \ref AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_legacy2_update_gpoint(struct PropertyAnimationLegacy2 *property_animation,
      const uint32_t distance_normalized);

//! Default update callback for a property animations to update a property of type GRect.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationLegacy2Implementation,
//! in combination with a `.getter` and `.setter` accessors of types \ref GRectGetter and
//! \ref GRectSetter.
//! The implementation of this function will calculate the next rectangle of the animation and call
//! the setter to set the new rectangle upon the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See
//! \ref AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_legacy2_update_grect(struct PropertyAnimationLegacy2 *property_animation,
      const uint32_t distance_normalized);

//! Data structure containing a collection of function pointers that form the implementation of the
//! property animation.
//! See the code example at the top (\ref PropertyAnimationLegacy2).
typedef struct PropertyAnimationLegacy2Implementation {
  //! The "inherited" fields from the Animation "base class".
  AnimationLegacy2Implementation base;
  //! The accessors to set/get the property to be animated.
  PropertyAnimationAccessors accessors;
} PropertyAnimationLegacy2Implementation;

//! The data structure of a property animation that contains all its state.
typedef struct PropertyAnimationLegacy2 {
  //! The "inherited" state from the "base class", \ref Animation.
  AnimationLegacy2 animation;
  //! The values of the property that the animation should animated from and to.
  struct {
    //! The value of the property that the animation should animate to.
    //! When the animation completes, this value will be the final value that is set.
    union {
      //! Valid when the property being animated is of type GRect
      GRect grect;
      //! Valid when the property being animated is of type GPoint
      GPoint gpoint;
      //! Valid when the property being animated is of type int16_t
      int16_t int16;
    } to;
    //! The value of the property that the animation should animate to.
    //! When the animation starts, this value will be the initial value that is set.
    union {
      //! Valid when the property being animated is of type GRect
      GRect grect;
      //! Valid when the property being animated is of type GPoint
      GPoint gpoint;
      //! Valid when the property being animated is of type int16_t
      int16_t int16;
    } from;
  } values; //!< See detail table
  void *subject; //!< The subject of the animation of which the property should be animated.
} PropertyAnimationLegacy2;

//!     @} // group PropertyAnimationLegacy2
//!   @} // group Animation
//! @} // group UI
