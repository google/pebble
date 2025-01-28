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
#include "util/math_fixed.h"

#include <stdint.h>

//! @file interpolate.h
//! Routines for interpolating between values and points. Useful for animations.

typedef struct MoookConfig {
  const int32_t *frames_in; //!< In frame lookup table applied as delta * direction to the `from`
  size_t num_frames_in; //!< Number of in frames in the frame lookup table
  const int32_t *frames_out; //!< Out frame lookup table applied as delta * direction to the `to`
  size_t num_frames_out; //!< Number of out frames in the frame lookup table
  size_t num_frames_mid; //!< Number of soft mid frames to insert
  bool no_bounce_back; //!< Whether the direction should be reversed for out frames.
} MoookConfig;

#define INTERPOLATE_MOOOK_BOUNCE_BACK 4

//! Performs an interpolation between from and to.
//! Progress represents 0..1 as fixed point between 0..ANIMATION_NORMALIZED_MAX,
//! but can have values <0 and >1 as well to support overshooting.
//! Likewise, it can return values outside of the range from..to.
typedef int64_t (*InterpolateInt64Function)(int32_t progress, int64_t from, int64_t to);

//! @internal
//! Truly linear interpolation between two int64_t.
//! Does not consider any overriding of interpolation for spatial easing.
int64_t interpolate_int64_linear(int32_t normalized, int64_t from, int64_t to);

//! Interpolation between two int64_t.
//! In most cases, this is a linear interpolation but the behavior can vary if this function
//! is called from within an animation's update handler that uses
//! AnimationCurveCustomInterpolationFunction. This allows clients to transparently implement
//! effects such as spatial easing. See \ref animation_set_custom_interpolation().
int64_t interpolate_int64(int32_t normalized, int64_t from, int64_t to);

//! Interpolation between two int16_t.
//! See \ref interpolate_int64() for special cases.
int16_t interpolate_int16(int32_t normalized, int16_t from, int16_t to);

//! Interpolation between two uint32_t.
//! See \ref interpolate_int64() for special cases.
uint32_t interpolate_uint32(int32_t normalized, uint32_t from, uint32_t to);

//! Interpolation between two Fixed_S32_16.
//! See \ref interpolate_int64() for special cases.
Fixed_S32_16 interpolate_fixed32(int32_t normalized, Fixed_S32_16 from, Fixed_S32_16 to);

//! Interpolation between two GSize.
//! See \ref interpolate_int64() for special cases.
GSize interpolate_gsize(int32_t normalized, GSize from, GSize to);

//! Interpolation between two GPoint.
//! See \ref interpolate_int64() for special cases.
GPoint interpolate_gpoint(int32_t normalized, GPoint from, GPoint to);

//! linear scale a int16_t between two int16_t lengths
int16_t scale_int16(int16_t value, int16_t from, int16_t to);

//! linear scale a int32_t between two int32_t lengths
int32_t scale_int32(int32_t value, int32_t from, int32_t to);

uint32_t interpolate_moook_in_duration();
uint32_t interpolate_moook_out_duration();
uint32_t interpolate_moook_duration();

//! @param num_frames_mid Number of additional linearly interpolated middle frames
uint32_t interpolate_moook_soft_duration(int32_t num_frames_mid);

//! Calculates the duration of a given custom Moook curve configuration.
//! @param config Custom Moook curve configuration.
//! @return Duration of the custom Moook curve in milliseconds.
uint32_t interpolate_moook_custom_duration(const MoookConfig *config);

//! Moook ease in curve. Useful for composing larger interpolation curves.
//! @param normalized Time of the point in the ease curve
//! @param from Starting point in space of the animation
//! @param to Ending point in space of the animation
//! @param num_frames_to Remaining number of frames in the animation that do not consist of the
//! Moook ease in curve.
int64_t interpolate_moook_in(int32_t normalized, int64_t from, int64_t to,
                             int32_t num_frames_to);

//! Only the Moook ease in curve. Used for animations that only consist of the ease in.
//! @param normalized Time of the point in the ease curve
//! @param from Starting point in space of the animation
//! @param to Ending point in space of the animation
int64_t interpolate_moook_in_only(int32_t normalized, int64_t from, int64_t to);

//! Moook ease out curve. Useful for composing larger interpolation curves.
//! @param normalized Time of the point in the ease curve
//! @param from Starting point in space of the animation
//! @param to Ending point in space of the animation
//! @param bounce_back Whether to lead up to the end point from the opposite direction if we were
//! to lead up from the start poing, which a normal Moook curve would do.
int64_t interpolate_moook_out(int32_t normalized, int64_t from, int64_t to,
                              int32_t num_frames_from, bool bounce_back);

//! Moook curve. This is a ease in and ease out curve with a hard cut between the two easings.
//! When using this curve, the duration must be set to \ref interpolate_moook_duration()
//! @param normalized Time of the point in the ease curve
//! @param from Starting point in space of the animation
//! @param to Ending point in space of the animation
int64_t interpolate_moook(int32_t normalized, int64_t from, int64_t to);

//! Moook curve with additional linearly interpolated frames between the ease in and ease out.
//! When using this curve, the duration must be set to \ref interpolate_moook_soft_duration()
//! with the same number of frames in the parameter.
//! @param normalized Time of the point in the ease curve
//! @param from Starting point in space of the animation
//! @param to Ending point in space of the animation
//! @param num_frames_mid Number of additional linearly interpolated middle frames
int64_t interpolate_moook_soft(int32_t normalized, int64_t from, int64_t to,
                               int32_t num_frames_mid);

//! Custom Moook curve which supports arbitrary delta frame tables.
//! @param normalized Time of the point in the ease curve.
//! @param from Starting point in space of the animation.
//! @param to Ending point in space of the animation.
//! @param config Custom Moook curve configuration.
//! @return Moook interpolated spacial value.
int64_t interpolate_moook_custom(int32_t normalized, int64_t from, int64_t to,
                                 const MoookConfig *config);
