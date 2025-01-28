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
#include "animation.h"
#include "applib/graphics/gtypes.h"

//////////////////////
// Property Animations
//

//! @file property_animation.h

//! @addtogroup UI
//! @{
//!   @addtogroup Animation
//!   @{
//!     @addtogroup PropertyAnimation
//! \brief A ProperyAnimation animates the value of a "property" of a "subject" over time.
//!
//! <h3>Animating a Layer's frame property</h3>
//! Currently there is only one specific type of property animation offered off-the-shelf, namely
//! one to change the frame (property) of a layer (subject), see \ref
//! property_animation_create_layer_frame().
//!
//! <h3>Implementing a custom PropertyAnimation</h3>
//! It is fairly simple to create your own variant of a PropertyAnimation.
//!
//! Please refer to \htmlinclude UiFramework.html (chapter "Property Animations") for a conceptual
//! overview of the animation framework and make sure you understand the underlying \ref Animation,
//! in case you are not familiar with it, before trying to implement a variation on
//! PropertyAnimation.
//!
//! To implement a custom property animation, use \ref property_animation_create() and provide a
//! function pointers to the accessors (getter and setter) and setup, update and teardown callbacks
//! in the implementation argument. Note that the type of property to animate with \ref
//! PropertyAnimation is limited to int16_t, GPoint or GRect.
//!
//! For each of these types, there are implementations provided for the necessary `.update` handler
//! of the animation: see \ref property_animation_update_int16(), \ref
//! property_animation_update_gpoint() and \ref property_animation_update_grect().
//! These update functions expect the `.accessors` to conform to the following interface:
//! Any getter needs to have the following function signature: `__type__ getter(void *subject);`
//! Any setter needs to have to following function signature: `void setter(void *subject,
//! __type__ value);`
//! See \ref Int16Getter, \ref Int16Setter, \ref GPointGetter, \ref GPointSetter,
//! \ref GRectGetter, \ref GRectSetter for the typedefs that accompany the update fuctions.
//!
//! \code{.c}
//! static const PropertyAnimationImplementation my_implementation = {
//!   .base = {
//!     // using the "stock" update callback:
//!     .update = (AnimationUpdateImplementation) property_animation_update_gpoint,
//!   },
//!   .accessors = {
//!     // my accessors that get/set a GPoint from/onto my subject:
//!     .setter = { .gpoint = my_layer_set_corner_point, },
//!     .getter = { .gpoint = (const GPointGetter) my_layer_get_corner_point, },
//!   },
//! };
//! static PropertyAnimation* s_my_animation_ptr = NULL;
//! static GPoint s_to_point = GPointZero;
//! ...
//! // Use NULL as 'from' value, this will make the animation framework call the getter
//! // to get the current value of the property and use that as the 'from' value:
//! s_my_animation_ptr = property_animation_create(&my_implementation, my_layer, NULL, &s_to_point);
//! animation_schedule(property_animation_get_animation(s_my_animation_ptr));
//! \endcode
//!   @{

struct PropertyAnimation;
typedef struct PropertyAnimation PropertyAnimation;
struct Layer;

//! Function signature of a setter function to set a property of type int16_t onto the subject.
//! @see \ref property_animation_update_int16()
//! @see \ref PropertyAnimationAccessors
typedef void (*Int16Setter)(void *subject, int16_t int16);

//! Function signature of a getter function to get the current property of type int16_t of the
//! subject.
//! @see \ref property_animation_create()
//! @see \ref PropertyAnimationAccessors
typedef int16_t (*Int16Getter)(void *subject);

//! Function signature of a setter function to set a property of type uint32_t onto the subject.
//! @see \ref property_animation_update_int16()
//! @see \ref PropertyAnimationAccessors
typedef void (*UInt32Setter)(void *subject, uint32_t uint32);

//! Function signature of a getter function to get the current property of type uint32_t of the
//! subject.
//! @see \ref property_animation_create()
//! @see \ref PropertyAnimationAccessors
typedef uint32_t (*UInt32Getter)(void *subject);

//! Function signature of a setter function to set a property of type GPoint onto the subject.
//! @see \ref property_animation_update_gpoint()
//! @see \ref PropertyAnimationAccessors
typedef void (*GPointSetter)(void *subject, GPoint gpoint);

//! Function signature of a getter function to get the current property of type GPoint of the subject.
//! @see \ref property_animation_create()
//! @see \ref PropertyAnimationAccessors
typedef GPointReturn (*GPointGetter)(void *subject);

//! Function signature of a setter function to set a property of type GRect onto the subject.
//! @see \ref property_animation_update_grect()
//! @see \ref PropertyAnimationAccessors
typedef void (*GRectSetter)(void *subject, GRect grect);

//! Function signature of a getter function to get the current property of type GRect of the subject.
//! @see \ref property_animation_create()
//! @see \ref PropertyAnimationAccessors
typedef GRectReturn (*GRectGetter)(void *subject);

//! Function signature of a setter function to set a property of type GTransform onto the subject.
//! @see \ref property_animation_update_gtransform()
//! @see \ref PropertyAnimationAccessors
typedef void (*GTransformSetter)(void *subject, GTransform gtransform);

//! Function signature of a getter function to get the current property of type GTransform of the
//! subject.
//! @see \ref property_animation_create()
//! @see \ref PropertyAnimationAccessors
typedef GTransformReturn (*GTransformGetter)(void *subject);

//! Function signature of a setter function to set a property of type GColor8 onto the subject.
//! @see \ref property_animation_update_gcolor8()
//! @see \ref PropertyAnimationAccessors
typedef void (*GColor8Setter)(void *subject, GColor8 gcolor);

//! Function signature of a getter function to get the current property of type GColor8 of the
//! subject.
//! @see \ref property_animation_create()
//! @see \ref PropertyAnimationAccessors
typedef GColor8 (*GColor8Getter)(void *subject);

//! Function signature of a setter function to set a property of type Fixed_S32_16 onto the subject.
//! @see \ref property_animation_update_fixed_s32_16()
//! @see \ref PropertyAnimationAccessors
typedef void (*Fixed_S32_16Setter)(void *subject, Fixed_S32_16 fixed_s32_16);

//! Function signature of a getter function to get the current property of type Fixed_S32_16 of the
//! subject.
//! @see \ref property_animation_create()
//! @see \ref PropertyAnimationAccessors
typedef Fixed_S32_16Return (*Fixed_S32_16Getter)(void *subject);

//! Data structure containing the setter and getter function pointers that the property animation
//! should use.
//! The specified setter function will be used by the animation's update callback. <br/> Based on
//! the type of the property (int16_t, GPoint or GRect), the accompanying update callback should be
//! used, see \ref property_animation_update_int16(), \ref property_animation_update_gpoint() and
//! \ref property_animation_update_grect(). <br/>
//! The getter function is used when the animation is initialized, to assign the current value of
//! the subject's property as "from" or "to" value, see \ref property_animation_create().
typedef struct PropertyAnimationAccessors {
  //! Function pointer to the implementation of the function that __sets__ the updated property
  //! value. This function will be called repeatedly for each animation frame.
  //! @see PropertyAnimationAccessors
  union {
    //! Use if the property to animate is of int16_t type
    Int16Setter int16;
    //! Use if the property to animate is of GPoint type
    GPointSetter gpoint;
    //! Use if the property to animate is of GRect type
    GRectSetter grect;
    //! Use if the property to animate is of GColor8 type
    GColor8Setter gcolor8;
    //! Use if the property to animate is of uint32_t type
    UInt32Setter uint32;
  } setter;
  //! Function pointer to the implementation of the function that __gets__ the current property
  //! value. This function will be called during \ref property_animation_create(), to get the current
  //! property value, in case the `from_value` or `to_value` argument is `NULL`.
  //! @see PropertyAnimationAccessors
  union {
    //! Use if the property to animate is of int16_t type
    Int16Getter int16;
    //! Use if the property to animate is of GPoint type
    GPointGetter gpoint;
    //! Use if the property to animate is of GRect type
    GRectGetter grect;
    //! Use if the property to animate is of GColor8 type
    GColor8Getter gcolor8;
    //! Use if the property to animate is of uint32_t type
    UInt32Getter uint32;
  } getter;
} PropertyAnimationAccessors;

//! Data structure containing a collection of function pointers that form the implementation of the
//! property animation.
//! See the code example at the top (\ref PropertyAnimation).
typedef struct PropertyAnimationImplementation {
  //! The "inherited" fields from the Animation "base class".
  AnimationImplementation base;
  //! The accessors to set/get the property to be animated.
  PropertyAnimationAccessors accessors;
} PropertyAnimationImplementation;


//! Convenience function to create and initialize a property animation that animates the frame of a
//! Layer. It sets up the PropertyAnimation to use \ref layer_set_frame() and \ref layer_get_frame()
//! as accessors and uses the `layer` parameter as the subject for the animation.
//! The same defaults are used as with \ref animation_create().
//! @param layer the layer that will be animated
//! @param from_frame the frame that the layer should animate from
//! @param to_frame the frame that the layer should animate to
//! @note Pass in `NULL` as one of the frame arguments to have it set automatically to the layer's
//! current frame. This will result in a call to \ref layer_get_frame() to get the current frame of
//! the layer.
//! @return A handle to the property animation. `NULL` if animation could not be created
PropertyAnimation *property_animation_create_layer_frame(struct Layer *layer, GRect *from_frame,
                                                         GRect *to_frame);

//! Convenience function to create and initialize a property animation that animates the bounds of a
//! Layer. It sets up the PropertyAnimation to use \ref layer_set_bounds()
//! and \ref layer_get_bounds() as accessors and uses the `layer` parameter
//! as the subject for the animation.
//! The same defaults are used as with \ref animation_create().
//! @param layer the layer that will be animated
//! @param from_bounds the bounds that the layer should animate from
//! @param to_bounds the bounds that the layer should animate to
//! @note Pass in `NULL` as one of the frame arguments to have it set automatically to the layer's
//! current bounds. This will result in a call to \ref layer_get_bounds() to get the current
//! frame of the layer.
//! @return A handle to the property animation. `NULL` if animation could not be created
PropertyAnimation *property_animation_create_layer_bounds(struct Layer *layer, GRect *from_bounds,
                                                         GRect *to_bounds);

//! Convenience function to create and initialize a property animation that animates the bound's
//! origin of a Layer. It sets up the PropertyAnimation to use layer_set_bounds() and
//! layer_get_bounds() as accessors and uses the `layer` parameter as the subject for the animation.
//! The same defaults are used as with \ref animation_create().
//! @param layer the layer that will be animated
//! @param from_origin the origin that the bounds should animate from
//! @param to_origin the origin that the layer should animate to
//! @return A handle to the property animation. `NULL` if animation could not be created
PropertyAnimation *property_animation_create_bounds_origin(struct Layer *layer, GPoint *from,
    GPoint *to);


PropertyAnimation *property_animation_create_mark_dirty(struct Layer *layer);


//! @internal
//! Convenience function to re-initialize an already instantiated layer frame animation.
//! @param layer the layer that will be animated
//! @param from_frame the frame that the layer should animate from
//! @param to_frame the frame that the layer should animate to
//! @note Pass in `NULL` as one of the frame arguments to have it set automatically to the layer's
//! current frame. This will result in a call to \ref layer_get_frame() to get the current frame of
//! the layer.
//! @return true if successful
bool property_animation_init_layer_frame(PropertyAnimation *animation_h,
                            struct Layer *layer, GRect *from_frame, GRect *to_frame);


//! Destroy a property animation allocated by property_animation_create() or relatives.
//! @param property_animation the return value from property_animation_create
void property_animation_destroy(PropertyAnimation* property_animation);


//! Convenience function to retrieve an animation instance from a property animation instance
//! @param property_animation The property animation
//! @return The \ref Animation within this PropertyAnimation
Animation *property_animation_get_animation(PropertyAnimation *property_animation);

//! Convenience function to clone a property animation instance
//! @param property_animation The property animation
//! @return A clone of the original Animation
#define property_animation_clone(property_animation) \
    (PropertyAnimation *)animation_clone((Animation *)property_animation)

//! Convenience function to retrieve the 'from' GRect value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_from_grect(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GRect), false)

//! Convenience function to set the 'from' GRect value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_from_grect(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GRect), true)

//! Convenience function to retrieve the 'from' GPoint value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_from_gpoint(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GPoint), false)

//! Convenience function to set the 'from' GPoint value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_from_gpoint(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GPoint), true)

//! Convenience function to retrieve the 'from' int16_t value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_from_int16(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(int16_t), false)

//! Convenience function to set the 'from' int16_t value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_from_int16(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(int16_t), true)

//! Convenience function to retrieve the 'from' uint32_t value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_from_uint32(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(uint32_t), false)

//! Convenience function to set the 'from' uint32_t value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_from_uint32(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(uint32_t), true)

//! Convenience function to retrieve the 'from' GTransform value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_from_gtransform(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GTransform), false)

//! Convenience function to set the 'from' GTransform value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_from_gtransform(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GTransform), true)

//! Convenience function to retrieve the 'from' GColor8 value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_from_gcolor8(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GColor8), false)

//! Convenience function to set the 'from' GColor8 value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_from_gcolor8(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(GColor8), true)

//! Convenience function to retrieve the 'from' Fixed_S32_16 value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_from_fixed_s32_16(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(Fixed_S32_16), false)

//! Convenience function to set the 'from' Fixed_S32_16 value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_from_fixed_s32_16(property_animation, value_ptr) \
    property_animation_from(property_animation, value_ptr, sizeof(Fixed_S32_16), true)



//! Convenience function to retrieve the 'to' GRect value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_to_grect(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GRect), false)

//! Convenience function to set the 'to' GRect value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_to_grect(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GRect), true)

//! Convenience function to retrieve the 'to' GPoint value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_to_gpoint(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GPoint), false)

//! Convenience function to set the 'to' GPoint value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_to_gpoint(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GPoint), true)

//! Convenience function to retrieve the 'to' int16_t value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_to_int16(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(int16_t), false)

//! Convenience function to set the 'to' int16_t value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_to_int16(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(int16_t), true)

//! Convenience function to retrieve the 'to' uint32_t value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_to_uint32(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(uint32_t), false)

//! Convenience function to set the 'to' uint32_t value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_to_uint32(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(uint32_t), true)

//! Convenience function to retrieve the 'to' GTransform value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_to_gtransform(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GTransform), false)

//! Convenience function to set the 'to' GTransform value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_to_gtransform(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GTransform), true)

//! Convenience function to retrieve the 'to' GColor8 value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_to_gcolor8(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GColor8), false)

//! Convenience function to set the 'to' GColor8 value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_to_gcolor8(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(GColor8), true)

//! Convenience function to retrieve the 'to' Fixed_S32_16 value from property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr The value will be retrieved into this pointer
//! @return true on success, false on failure
#define property_animation_get_to_fixed_s32_16(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(Fixed_S32_16), false)

//! Convenience function to set the 'to' Fixed_S32_16 value of property animation handle
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new value
//! @return true on success, false on failure
#define property_animation_set_to_fixed_s32_16(property_animation, value_ptr) \
    property_animation_to(property_animation, value_ptr, sizeof(Fixed_S32_16), true)


//! Retrieve the subject of a property animation
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer used to store the subject of this property animation
//! @return The subject of this PropertyAnimation
#define property_animation_get_subject(property_animation, value_ptr) \
    property_animation_subject(property_animation, value_ptr, false)

//! Set the subject of a property animation
//! @param property_animation The PropertyAnimation to be accessed
//! @param value_ptr Pointer to the new subject value
#define property_animation_set_subject(property_animation, value_ptr) \
    property_animation_subject(property_animation, value_ptr, true)


////////////////////////////////////////////////////////////////////////////////
// Primitive helper functions for the property_animation_get|set.* macros
//

//! Helper function used by the property_animation_get|set_subject macros
//! @param property_animation Handle to the property animation
//! @param subject The subject to get or set.
//! @param set true to set new subject, false to retrieve existing value
//! @return true if successful, false on failure (usually a bad animation_h)
bool property_animation_subject(PropertyAnimation *property_animation, void **subject, bool set);


//! Helper function used by the property_animation_get|set_from_.* macros
//! @param property_animation Handle to the property animation
//! @param from Pointer to the value
//! @param size Size of the from value
//! @param set true to set new value, false to retrieve existing one
//! @return true if successful, false on failure (usually a bad animation_h)
bool property_animation_from(PropertyAnimation *property_animation, void *from, size_t size,
                              bool set);

//! Helper function used by the property_animation_get|set_to_.* macros
//! @param property_animation handle to the property animation
//! @param to Pointer to the value
//! @param size Size of the to value
//! @param set true to set new value, false to retrieve existing one
//! @return true if successful, false on failure (usually a bad animation_h)
bool property_animation_to(PropertyAnimation *property_animation, void *to, size_t size,
                            bool set);


//////////////////////////////////////////
// Implementing custom Property Animations
//

//! Creates a new PropertyAnimation on the heap and initializes it with the specified values.
//! The same defaults are used as with \ref animation_create().
//! If the `from_value` or the `to_value` is `NULL`, the getter accessor will be called to get the
//! current value of the property and be used instead.
//! @param implementation Pointer to the implementation of the animation. In most cases, it makes
//! sense to pass in a `static const` struct pointer.
//! @param subject Pointer to the "subject" being animated. This will be passed in when the getter/
//! setter accessors are called,
//! see \ref PropertyAnimationAccessors, \ref GPointSetter, and friends. The value of this pointer
//! will be copied into the `.subject` field of the PropertyAnimation struct.
//! @param from_value Pointer to the value that the subject should animate from
//! @param to_value Pointer to the value that the subject should animate to
//! @note Pass in `NULL` as one of the value arguments to have it set automatically to the subject's
//! current property value, as returned by the getter function. Also note that passing in `NULL` for
//! both `from_value` and `to_value`, will result in the animation having the same from- and to-
//! values, effectively not doing anything.
//! @return A handle to the property animation. `NULL` if animation could not be created
PropertyAnimation* property_animation_create(const PropertyAnimationImplementation *implementation,
                      void *subject, void *from_value, void *to_value);

//! @internal
//! Convenience function to re-initialize an already instantiated property animation.
//! @param implementation Pointer to the implementation of the animation. In most cases, it makes
//! sense to pass in a `static const` struct pointer.
//! @param subject Pointer to the "subject" being animated. This will be passed in when the getter/
//! setter accessors are called,
//! see \ref PropertyAnimationAccessors, \ref GPointSetter, and friends. The value of this pointer
//! will be copied into the `.subject` field of the PropertyAnimation struct.
//! @param from_value Pointer to the value that the subject should animate from
//! @param to_value Pointer to the value that the subject should animate to
//! @note Pass in `NULL` as one of the value arguments to have it set automatically to the subject's
//! current property value, as returned by the getter function. Also note that passing in `NULL` for
//! both `from_value` and `to_value`, will result in the animation having the same from- and to-
//! values, effectively not doing anything.
//! @return true if successful
bool property_animation_init(PropertyAnimation *animation_h,
                            const PropertyAnimationImplementation *implementation,
                            void *subject, void *from_value, void *to_value);


//! Default update callback for a property animations to update a property of type int16_t.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationImplementation, in combination with a `.getter` and `.setter` accessors of
//! types \ref Int16Getter and \ref Int16Setter.
//! The implementation of this function will calculate the next value of the animation and call the
//! setter to set the new value upon the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See \ref
//! AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_update_int16(PropertyAnimation *property_animation,
                                     const uint32_t distance_normalized);

//! Default update callback for a property animations to update a property of type uint32_t.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationImplementation, in combination with a `.getter` and `.setter` accessors of
//! types \ref UInt32Getter and \ref UInt32Setter.
//! The implementation of this function will calculate the next value of the animation and call the
//! setter to set the new value upon the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See \ref
//! AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_update_uint32(PropertyAnimation *property_animation,
                                      const uint32_t distance_normalized);

//! Default update callback for a property animations to update a property of type GPoint.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationImplementation,
//! in combination with a `.getter` and `.setter` accessors of types \ref GPointGetter and \ref
//! GPointSetter.
//! The implementation of this function will calculate the next point of the animation and call the
//! setter to set the new point upon the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See \ref
//! AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_update_gpoint(PropertyAnimation *property_animation,
                                      const uint32_t distance_normalized);

//! Default update callback for a property animations to update a property of type GRect.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationImplementation, in combination with a `.getter` and `.setter` accessors of
//! types \ref GRectGetter and \ref GRectSetter. The implementation of this function will calculate
//! the next rectangle of the animation and call the setter to set the new rectangle upon the
//! subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See \ref
//! AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_update_grect(PropertyAnimation *property_animation,
                                     const uint32_t distance_normalized);

//! @internal
//! GTransform is not exported, so don't include it in tintin. When GTransform will become exported,
//! remove this from here, property_animation_update_gtransform and property_animation_create.
#if !defined(PLATFORM_TINTIN)
//! Default update callback for a property animations to update a property of type GTransform.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationImplementation, in combination with a `.getter` and `.setter` accessors of
//! types \ref GTransformGetter and \ref GTransformSetter. The implementation of this function will
//! calculate the next GTransform of the animation and call the setter to set the new value upon
//! the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See \ref
//! AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_update_gtransform(PropertyAnimation *property_animation,
                                          const uint32_t distance_normalized);
#endif

//! Default update callback for a property animations to update a property of type GColor8.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationImplementation, in combination with a `.getter` and `.setter` accessors of
//! types \ref GColor8Getter and \ref GColor8Setter. The implementation of this function will
//! calculate the next rectangle of the animation and call the setter to set the new value upon
//! the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See \ref
//! AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_update_gcolor8(PropertyAnimation *property_animation,
                                       const uint32_t distance_normalized);

//! Default update callback for a property animations to update a property of type Fixed_S32_16.
//! Assign this function to the `.base.update` callback field of your
//! PropertyAnimationImplementation, in combination with a `.getter` and `.setter` accessors of
//! types \ref Fixed_S32_16Getter and \ref Fixed_S32_16Setter. The implementation of this function
//! will calculate the next Fixed_S32_16 of the animation and call the setter to set the new value
//! upon the subject.
//! @param property_animation The property animation for which the update is requested.
//! @param distance_normalized The current normalized distance. See \ref
//! AnimationUpdateImplementation
//! @note This function is not supposed to be called "manually", but will be called automatically
//! when the animation is being run.
void property_animation_update_fixed_s32_16(PropertyAnimation *property_animation,
                                            const uint32_t distance_normalized);

//!     @} // group PropertyAnimation
//!   @} // group Animation
//! @} // group UI
