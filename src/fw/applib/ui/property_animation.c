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

#include "property_animation_private.h"
#include "animation_interpolate.h"
#include "animation_private.h"
#include "animation_timing.h"
#include "applib/legacy2/ui/property_animation_legacy2.h"

#include "applib/app_logging.h"
#include "applib/applib_malloc.auto.h"
#include "system/passert.h"
#include "system/logging.h"
#include "util/size.h"
#include "layer.h"

/////////////////////
// Property Animation
//

static const PropertyAnimationImplementation s_frame_layer_implementation = {
  .base = {
    .update = (AnimationUpdateImplementation) property_animation_update_grect,
  },
  .accessors = {
    .setter = { .grect = (const GRectSetter) layer_set_frame_by_value, },
    .getter = { .grect = (const GRectGetter) layer_get_frame_by_value, },
  },
};

static const PropertyAnimationImplementation s_bounds_layer_implementation = {
  .base = {
    .update = (AnimationUpdateImplementation) property_animation_update_grect,
  },
  .accessors = {
    .setter = { .grect = (const GRectSetter) layer_set_bounds_by_value, },
    .getter = { .grect = (const GRectGetter) layer_get_bounds_by_value, },
  },
};

// -----------------------------------------------------------------------------------------
static inline PropertyAnimationPrivate *prv_find_property_animation(PropertyAnimation *handle) {
  return (PropertyAnimationPrivate *)animation_private_animation_find((Animation *)handle);
}


// -----------------------------------------------------------------------------------------
void property_animation_update_int16(PropertyAnimation *property_animation_h,
                                     const uint32_t distance_normalized) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like scroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    property_animation_legacy2_update_int16((PropertyAnimationLegacy2 *)property_animation_h,
                                              distance_normalized);
    return;
  }

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return;
  }

  int16_t result = interpolate_int16(distance_normalized,
                                     property_animation->values.from.int16,
                                     property_animation->values.to.int16);
  ((PropertyAnimationImplementation*)
   property_animation->animation.implementation)
    ->accessors.setter.int16(property_animation->subject, result);
}


// -----------------------------------------------------------------------------------------
void property_animation_update_uint32(PropertyAnimation *property_animation_h,
                                     const uint32_t distance_normalized) {
  PBL_ASSERTN(!animation_private_using_legacy_2(NULL));

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return;
  }

  uint32_t result = interpolate_uint32(distance_normalized,
                                       property_animation->values.from.uint32,
                                       property_animation->values.to.uint32);
  ((PropertyAnimationImplementation*)
    property_animation->animation.implementation)
      ->accessors.setter.uint32(property_animation->subject, result);
}


// -----------------------------------------------------------------------------------------
void property_animation_update_gpoint(PropertyAnimation *property_animation_h,
                                      const uint32_t distance_normalized) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    property_animation_legacy2_update_gpoint((PropertyAnimationLegacy2 *)property_animation_h,
                                              distance_normalized);
    return;
  }

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return;
  }

  GPoint result;
  result.x = interpolate_int16(distance_normalized,
                               property_animation->values.from.gpoint.x,
                               property_animation->values.to.gpoint.x);
  result.y = interpolate_int16(distance_normalized,
                               property_animation->values.from.gpoint.y,
                               property_animation->values.to.gpoint.y);
  ((PropertyAnimationImplementation*)
    property_animation->animation.implementation)
      ->accessors.setter.gpoint(property_animation->subject, result);
}


// -----------------------------------------------------------------------------------------
void property_animation_update_grect(PropertyAnimation *property_animation_h,
                                     const uint32_t distance_normalized) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    property_animation_legacy2_update_grect((PropertyAnimationLegacy2 *)property_animation_h,
                                              distance_normalized);
    return;
  }

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return;
  }

  GRect result;
  result.origin.x = interpolate_int16(
      distance_normalized,
      property_animation->values.from.grect.origin.x,
      property_animation->values.to.grect.origin.x);
  result.origin.y = interpolate_int16(
      distance_normalized,
      property_animation->values.from.grect.origin.y,
      property_animation->values.to.grect.origin.y);
  result.size.w = interpolate_int16(
      distance_normalized,
      property_animation->values.from.grect.size.w,
      property_animation->values.to.grect.size.w);
  result.size.h = interpolate_int16(
      distance_normalized,
      property_animation->values.from.grect.size.h,
      property_animation->values.to.grect.size.h);
  ((PropertyAnimationImplementation*)
    property_animation->animation.implementation)
      ->accessors.setter.grect(property_animation->subject, result);
}


// -----------------------------------------------------------------------------------------
#if !defined(PLATFORM_TINTIN)
void property_animation_update_gtransform(PropertyAnimation *property_animation_h,
                                          const uint32_t distance_normalized) {
  PBL_ASSERTN(!animation_private_using_legacy_2(NULL));

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return;
  }

  GTransform result;
  result.a = interpolate_fixed32(
      distance_normalized,
      property_animation->values.from.gtransform.a,
      property_animation->values.to.gtransform.a);
  result.b = interpolate_fixed32(
      distance_normalized,
      property_animation->values.from.gtransform.b,
      property_animation->values.to.gtransform.b);
  result.c = interpolate_fixed32(
      distance_normalized,
      property_animation->values.from.gtransform.c,
      property_animation->values.to.gtransform.c);
  result.d = interpolate_fixed32(
      distance_normalized,
      property_animation->values.from.gtransform.d,
      property_animation->values.to.gtransform.d);
  result.tx = interpolate_fixed32(
      distance_normalized,
      property_animation->values.from.gtransform.tx,
      property_animation->values.to.gtransform.tx);
  result.ty = interpolate_fixed32(
      distance_normalized,
      property_animation->values.from.gtransform.ty,
      property_animation->values.to.gtransform.ty);

  // NOTE: We are not exposing the GTransform in the public SDK, so the setter and getter
  // must be typecast
  GTransformSetter setter = (GTransformSetter)((PropertyAnimationImplementation*)
                              property_animation->animation.implementation)
                                ->accessors.setter.int16;

  setter(property_animation->subject, result);
}
#endif


// -----------------------------------------------------------------------------------------
void property_animation_update_gcolor8(PropertyAnimation *property_animation_h,
                                       const uint32_t distance_normalized) {
  PBL_ASSERTN(!animation_private_using_legacy_2(NULL));

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return;
  }

  GColor8 result;
  result.a = interpolate_int16(
      distance_normalized,
      property_animation->values.from.gcolor8.a,
      property_animation->values.to.gcolor8.a);
  result.r = interpolate_int16(
      distance_normalized,
      property_animation->values.from.gcolor8.r,
      property_animation->values.to.gcolor8.r);
  result.g = interpolate_int16(
      distance_normalized,
      property_animation->values.from.gcolor8.g,
      property_animation->values.to.gcolor8.g);
  result.b = interpolate_int16(
      distance_normalized,
      property_animation->values.from.gcolor8.b,
      property_animation->values.to.gcolor8.b);

  ((PropertyAnimationImplementation*)
    property_animation->animation.implementation)
      ->accessors.setter.gcolor8(property_animation->subject, result);
}


// -----------------------------------------------------------------------------------------
void property_animation_update_fixed_s32_16(PropertyAnimation *property_animation_h,
                                            const uint32_t distance_normalized) {
  PBL_ASSERTN(!animation_private_using_legacy_2(NULL));

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return;
  }

  Fixed_S32_16 result = interpolate_fixed32(distance_normalized,
                                                property_animation->values.from.fixed_s32_16,
                                                property_animation->values.to.fixed_s32_16);

  // NOTE: We are not exposing the Fixed_S32_16 in the public SDK, so the setter and getter
  // must be typecast
  Fixed_S32_16Setter setter = (Fixed_S32_16Setter)((PropertyAnimationImplementation*)
                              property_animation->animation.implementation)
                                ->accessors.setter.int16;

  setter(property_animation->subject, result);
}


// -----------------------------------------------------------------------------------------
static void prv_init(PropertyAnimationPrivate *property_animation,
                           const PropertyAnimationImplementation *implementation,
                           void *subject, void *from_value, void *to_value) {
  property_animation->animation.is_property_animation = true;
  memset(&property_animation->values, 0xff, sizeof(property_animation->values));
  property_animation->animation.implementation = (AnimationImplementation*) implementation;
  property_animation->subject = subject;
  if (implementation->accessors.getter.int16) {
    if (property_animation->animation.implementation->update
        == (AnimationUpdateImplementation) property_animation_update_int16) {
      property_animation->values.to.int16 = to_value ? *((int16_t *)to_value)
                                          : implementation->accessors.getter.int16(subject);
      property_animation->values.from.int16 = from_value ? *((int16_t*)from_value)
                                            : implementation->accessors.getter.int16(subject);

    } else if (property_animation->animation.implementation->update
                        == (AnimationUpdateImplementation) property_animation_update_uint32) {
      property_animation->values.to.uint32 = to_value ? *((uint32_t *)to_value)
                                           : implementation->accessors.getter.uint32(subject);
      property_animation->values.from.uint32 = from_value ? *((uint32_t *)from_value)
                                             : implementation->accessors.getter.uint32(subject);

    } else if (property_animation->animation.implementation->update
                        == (AnimationUpdateImplementation) property_animation_update_gpoint) {
      property_animation->values.to.gpoint = to_value ? *((GPoint*)to_value)
                                           : implementation->accessors.getter.gpoint(subject);
      property_animation->values.from.gpoint = from_value ? *((GPoint*)from_value)
                                             : implementation->accessors.getter.gpoint(subject);

    } else if (property_animation->animation.implementation->update
               == (AnimationUpdateImplementation)property_animation_update_grect) {
      property_animation->values.to.grect = to_value ? *((GRect*)to_value)
                                          : implementation->accessors.getter.grect(subject);
      property_animation->values.from.grect = from_value ? *((GRect*)from_value)
                                            : implementation->accessors.getter.grect(subject);

#if !PLATFORM_TINTIN
    } else if (property_animation->animation.implementation->update
               == (AnimationUpdateImplementation)property_animation_update_gtransform) {
      // NOTE: We are not exposing the GTransform in the public SDK, so the setter and getter
      // must be typecast
      GTransformGetter getter = (GTransformGetter)((PropertyAnimationImplementation*)
                                  property_animation->animation.implementation)
                                    ->accessors.getter.int16;
      property_animation->values.to.gtransform = to_value ? *((GTransform*)to_value)
                                          : getter(subject);
      property_animation->values.from.gtransform = from_value ? *((GTransform*)from_value)
                                            : getter(subject);
#endif

    } else if (property_animation->animation.implementation->update
               == (AnimationUpdateImplementation)property_animation_update_gcolor8) {
      property_animation->values.to.gcolor8 = to_value ? *((GColor8*)to_value)
                                          : implementation->accessors.getter.gcolor8(subject);
      property_animation->values.from.gcolor8 = from_value ? *((GColor8*)from_value)
                                            : implementation->accessors.getter.gcolor8(subject);

    } else if (property_animation->animation.implementation->update
               == (AnimationUpdateImplementation)property_animation_update_fixed_s32_16) {
      // NOTE: We are not exposing the Fixed_S32_16 in the public SDK, so the setter and getter
      // must be typecast
      Fixed_S32_16Getter getter = (Fixed_S32_16Getter)((PropertyAnimationImplementation*)
                                  property_animation->animation.implementation)
                                    ->accessors.getter.int16;
      property_animation->values.to.fixed_s32_16 = to_value ? *((Fixed_S32_16*)to_value)
                                          : getter(subject);
      property_animation->values.from.fixed_s32_16 = from_value ? *((Fixed_S32_16*)from_value)
                                          : getter(subject);
    }
  }
}


// -----------------------------------------------------------------------------------------
PropertyAnimation *property_animation_create(
                            const PropertyAnimationImplementation *implementation,
                            void *subject, void *from_value, void *to_value) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    return (PropertyAnimation *)property_animation_legacy2_create(
        (PropertyAnimationLegacy2Implementation *)implementation, subject, from_value, to_value);
  }

  PropertyAnimationPrivate* property_animation = applib_type_malloc(PropertyAnimationPrivate);
  if (!property_animation) {
    return NULL;
  }
  memset(property_animation, 0, sizeof(*property_animation));
  Animation *handle = animation_private_animation_init(&property_animation->animation);
  prv_init(property_animation, implementation, subject, from_value, to_value);
  return (PropertyAnimation *)handle;
}


// -----------------------------------------------------------------------------------------
// Create a new property animation structure, copying just the property animation unique fields
PropertyAnimationPrivate *property_animation_private_clone(PropertyAnimationPrivate *from) {
  PBL_ASSERTN(!animation_private_using_legacy_2(NULL));

  PropertyAnimationPrivate* property_animation = applib_type_malloc(PropertyAnimationPrivate);
  if (!property_animation) {
    return NULL;
  }
  memset(property_animation, 0, sizeof(*property_animation));
  uint8_t *dst = (uint8_t *)property_animation;
  uint8_t *src = (uint8_t *)from;
  uint32_t offset = sizeof(AnimationPrivate);       // Skip the base class fields
  uint32_t size = sizeof(PropertyAnimationPrivate) - offset;
  memcpy(dst + offset, src + offset, size);
  return property_animation;
}


// -----------------------------------------------------------------------------------------
bool property_animation_init(PropertyAnimation *animation_h,
                            const PropertyAnimationImplementation *implementation,
                            void *subject, void *from_value, void *to_value) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    property_animation_legacy2_init((PropertyAnimationLegacy2 *)animation_h,
                                    (PropertyAnimationLegacy2Implementation *)implementation,
                                    subject, from_value, to_value);
    return true;
  }

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(animation_h);
  if (!property_animation) {
    return false;
  }

  // It is an error to call this if the animation is scheduled
  PBL_ASSERTN(!animation_is_scheduled((Animation *)animation_h));
  prv_init(property_animation, implementation, subject, from_value, to_value);
  return true;
}

// -----------------------------------------------------------------------------------------
PropertyAnimation* property_animation_create_layer_frame(struct Layer *layer, GRect *from_frame,
                                                         GRect *to_frame) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    return (PropertyAnimation *)property_animation_legacy2_create_layer_frame(layer,
                from_frame, to_frame);
  }

  return property_animation_create(&s_frame_layer_implementation, layer, from_frame, to_frame);
}

// -----------------------------------------------------------------------------------------
PropertyAnimation* property_animation_create_layer_bounds(struct Layer *layer, GRect *from_bounds,
                                                          GRect *to_bounds) {
  // no legacy2 support as this was never exposed on 2.x
  return property_animation_create(&s_bounds_layer_implementation, layer, from_bounds, to_bounds);
}

// -----------------------------------------------------------------------------------------
bool property_animation_init_layer_frame(PropertyAnimation *animation_h,
                            struct Layer *layer, GRect *from_frame, GRect *to_frame) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    property_animation_legacy2_init_layer_frame((PropertyAnimationLegacy2 *)animation_h, layer,
                                                from_frame, to_frame);
    return true;
  }
  return property_animation_init(animation_h, &s_frame_layer_implementation, layer, from_frame,
                                  to_frame);
}

// -----------------------------------------------------------------------------------------

PropertyAnimation *property_animation_create_bounds_origin(Layer *layer, GPoint *from, GPoint *to) {
  // no legacy2 support as this was never exposed on 2.x

  PropertyAnimation *result = property_animation_create(&s_bounds_layer_implementation,
      layer, NULL, NULL);

  GRect value = layer->bounds;
  if (from) {
    value.origin = *from;
  }
  property_animation_set_from_grect(result, &value);

  value = layer->bounds;
  if (to) {
    value.origin = *to;
  }
  property_animation_set_to_grect(result, &value);

  return result;
}


// -----------------------------------------------------------------------------------------

static void property_animation_update_mark_dirty(Animation* animation,
                                                 const AnimationProgress normalized) {
  PropertyAnimation *prop_anim = (PropertyAnimation *)animation;
  Layer *subject;
  if (property_animation_get_subject(prop_anim, (void**)&subject) && subject) {
    layer_mark_dirty(subject);
  }
}

static const PropertyAnimationImplementation s_dirty_layer_implementation = {
    .base = {
        .update = property_animation_update_mark_dirty,
    },
};

PropertyAnimation *property_animation_create_mark_dirty(struct Layer *layer) {
  // no legacy2 support as this was never exposed on 2.x
  PropertyAnimation *result = property_animation_create(&s_dirty_layer_implementation,
                                                        layer, NULL, NULL);

  return result;
}

// -----------------------------------------------------------------------------------------
void property_animation_destroy(PropertyAnimation* property_animation_h) {
  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    property_animation_legacy2_destroy((PropertyAnimationLegacy2 *)property_animation_h);
    return;
  }
  animation_destroy((Animation *)property_animation_h);
}


// -----------------------------------------------------------------------------------------
Animation *property_animation_get_animation(PropertyAnimation *property_animation) {
  return (Animation *)property_animation;
}

// -----------------------------------------------------------------------------------------
bool property_animation_subject(PropertyAnimation *property_animation_h, void **value, bool set) {
  if (!value) {
    return false;
  }

  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    if (set) {
      ((PropertyAnimationLegacy2 *)property_animation_h)->subject = *value;
    } else {
      *value = ((PropertyAnimationLegacy2 *)property_animation_h)->subject;
    }
    return true;
  }

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return false;
  }
  if (set) {
    property_animation->subject = *value;
  } else {
    *value = property_animation->subject;
  }
  return true;
}


// -----------------------------------------------------------------------------------------
bool property_animation_from(PropertyAnimation *property_animation_h, void *value, size_t size,
                             bool set) {
  if (!value) {
    return false;
  }

  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    PropertyAnimationLegacy2 *legacy = (PropertyAnimationLegacy2 *)property_animation_h;
    if (set) {
      memcpy(&legacy->values.from, value, size);
    } else {
      memcpy(value, &legacy->values.from, size);
    }
    return true;
  }

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return false;
  }
  if (size > MEMBER_SIZE(PropertyAnimationPrivate, values.from)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "invalid size");
    return false;
  }

  if (set) {
    memcpy(&property_animation->values.from, value, size);
  } else {
    memcpy(value, &property_animation->values.from, size);
  }
  return true;
}

// -----------------------------------------------------------------------------------------
bool property_animation_to(PropertyAnimation *property_animation_h, void *value, size_t size,
                             bool set) {
  if (!value) {
    return false;
  }

  if (animation_private_using_legacy_2(NULL)) {
    // We need to enable other applib modules like sroll_layer, menu_layer, etc. which are
    // compiled to use the 3.0 animation API to work with 2.0 apps.
    PropertyAnimationLegacy2 *legacy = (PropertyAnimationLegacy2 *)property_animation_h;
    if (set) {
      memcpy(&legacy->values.to, value, size);
    } else {
      memcpy(value, &legacy->values.to, size);
    }
    return true;
  }

  PropertyAnimationPrivate *property_animation = prv_find_property_animation(property_animation_h);
  if (!property_animation) {
    return false;
  }
  if (size > MEMBER_SIZE(PropertyAnimationPrivate, values.to)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "invalid size");
    return false;
  }

  if (set) {
    memcpy(&property_animation->values.to, value, size);
  } else {
    memcpy(value, &property_animation->values.to, size);
  }
  return true;
}
