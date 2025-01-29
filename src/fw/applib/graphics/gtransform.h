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

#include "gtypes.h"
#include "util/math_fixed.h"

//! @addtogroup Graphics
//! @{
//!   @addtogroup GraphicsTransforms Transformation Matrices
//! \brief Types for creating transformation matrices and utility functions to manipulate and apply
//! the transformations.
//!
//!   @{

//////////////////////////////////////
/// Creating Transforms
//////////////////////////////////////
//! Convenience macro for GTransformNumber equal to 0
#define GTransformNumberZero ((GTransformNumber){ .integer = 0, .fraction = 0 })

//! Convenience macro for GTransformNumber equal to 1
#define GTransformNumberOne  ((GTransformNumber){ .integer = 1, .fraction = 0 })

//! Convenience macro to convert from a number (i.e. char, int, float, etc.) to GTransformNumber
//! @param x The number to convert
#define GTransformNumberFromNumber(x) \
        ((GTransformNumber){ .raw_value = (int32_t)((x)*(GTransformNumberOne.raw_value)) })

//! This macro returns the transformation matrix for the corresponding input coefficients.
//! Below is the equivalent resulting matrix:
//! t = [ a   b   0 ]
//!     [ c   d   0 ]
//!     [ tx  ty  1 ]
//! @param a Coefficient corresponding to X scale (type is GTransformNumber)
//! @param b Coefficient corresponding to X shear (type is GTransformNumber)
//! @param c Coefficient corresponding to Y shear (type is GTransformNumber)
//! @param d Coefficient corresponding to Y scale (type is GTransformNumber)
//! @param tx Coefficient corresponding to X translation (type is GTransformNumber)
//! @param ty Coefficient corresponding to Y translation (type is GTransformNumber)
#define GTransform(a, b, c, d, tx, ty) (GTransform) { (a), (b), (c), (d), (tx), (ty) }
//! @param a Coefficient corresponding to X scale (type is char, int, float, etc)
//! @param b Coefficient corresponding to X shear (type is char, int, float, etc)
//! @param c Coefficient corresponding to Y shear (type is char, int, float, etc)
//! @param d Coefficient corresponding to Y scale (type is char, int, float, etc)
//! @param tx Coefficient corresponding to X translation (type is char, int, float, etc)
//! @param ty Coefficient corresponding to Y translation (type is char, int, float, etc)
#define GTransformFromNumbers(a, b, c, d, tx, ty) GTransform(GTransformNumberFromNumber(a),  \
                                                             GTransformNumberFromNumber(b),  \
                                                             GTransformNumberFromNumber(c),  \
                                                             GTransformNumberFromNumber(d),  \
                                                             GTransformNumberFromNumber(tx), \
                                                             GTransformNumberFromNumber(ty))

//! This macro returns the identity transformation matrix.
//! Below is the equivalent resulting matrix:
//! t = [ 1   0   0 ]
//!     [ 0   1   0 ]
//!     [ 0   0   1 ]
#define GTransformIdentity()           \
        (GTransform) { .a = GTransformNumberOne,   \
                       .b = GTransformNumberZero,  \
                       .c = GTransformNumberZero,  \
                       .d = GTransformNumberOne,   \
                       .tx = GTransformNumberZero, \
                       .ty = GTransformNumberZero }

//! This macro returns a scaling transformation matrix for the corresponding input coefficients.
//! Below is the equivalent resulting matrix:
//! t = [ sx  0   0 ]
//!     [ 0   sy  0 ]
//!     [ 0   0   1 ]
//! @param sx X scaling factor (type is GTransformNumber)
//! @param sy Y scaling factor (type is GTransformNumber)
#define GTransformScale(sx, sy)         \
        (GTransform) { .a = sx,         \
                       .b = GTransformNumberZero,   \
                       .c = GTransformNumberZero,   \
                       .d = sy,         \
                       .tx = GTransformNumberZero,  \
                       .ty = GTransformNumberZero }
//! @param sx X scaling factor (type is char, int, float, etc)
//! @param sy Y scaling factor (type is char, int, float, etc)
#define GTransformScaleFromNumber(sx, sy) \
        GTransformScale(GTransformNumberFromNumber(sx), GTransformNumberFromNumber(sy))

//! This macro returns a translation transformation matrix for the corresponding input coefficients.
//! Below is the equivalent resulting matrix:
//! t = [ 1   0   0 ]
//!     [ 0   1   0 ]
//!     [ tx  ty  1 ]
//! @param tx_v X translation factor (type is GTransformNumber)
//! @param ty_v Y translation factor (type is GTransformNumber)
#define GTransformTranslation(tx_v, ty_v) \
        (GTransform) { .a = GTransformNumberOne,      \
                       .b = GTransformNumberZero,     \
                       .c = GTransformNumberZero,     \
                       .d = GTransformNumberOne,      \
                       .tx = tx_v,        \
                       .ty = ty_v }
//! @param tx_v X translation factor (type is char, int, float, etc)
//! @param ty_v Y translation factor (type is char, int, float, etc)
#define GTransformTranslationFromNumber(tx, ty)  \
        GTransformTranslation(GTransformNumberFromNumber(tx), GTransformNumberFromNumber(ty))


//! @internal
//! Function that returns the rotation matrix as defined below by GTransformRotation
GTransform gtransform_init_rotation(int32_t angle);

//! This macro returns the transformation matrix for the corresponding rotation angle.
//! Below is the equivalent resulting matrix:
//! t = [ cos(angle)   -sin(angle)   0 ]
//!     [ sin(angle)   cos(angle)    0 ]
//!     [ 0            0             1 ]
//
//! The input angle corresponds to the rotation angle applied during transformation.
//! If this angle is set to 0, then the identity matrix is returned.
//! @param angle Rotation angle to apply (type is in same format as trig angle 0..TRIG_MAX_ANGLE)
#define GTransformRotation(angle) gtransform_init_rotation(angle)

//////////////////////////////////////
/// Evaluating Transforms
//////////////////////////////////////
//! Returns whether the input matrix is an identity matrix or not
//! @param t Pointer to transformation matrix to test
//! @return True if input matrix is identity; False if NULL or not identity.
bool gtransform_is_identity(const GTransform * const t);

//! Returns whether the input matrix is strictly a scaling matrix
//! @param t Pointer to transformation matrix to test
//! @return True if input matrix is only scaling X or Y; False if NULL or other coefficients set.
bool gtransform_is_only_scale(const GTransform * const t);

//! Returns whether the input matrix is strictly a translation matrix
//! @param t Pointer to transformation matrix to test
//! @return True if input matrix is only translating X or Y; False if NULL or other
//! coefficients set.
bool gtransform_is_only_translation(const GTransform * const t);

//! Returns whether the input matrix has coefficients b and c set to 0.
//! This does not check whether any other coefficients are set or not.
//! @param t Pointer to transformation matrix to test
//! @return True if input matrix is only scaling or translating X or Y; False if NULL or other.
//! coefficients set.
bool gtransform_is_only_scale_or_translation(const GTransform * const t);

//! Returns true if the two matrices are equal; false otherwise
//! Returns false if either parameter is NULL
//! @param t1 Pointer to first transformation matrix
//! @param t2 Pointer to second transformation matrix
//! @return True if both matrices are equal; False if any are NULL or if not equal.
bool gtransform_is_equal(const GTransform * const t1, const GTransform * const t2);

//////////////////////////////////////
/// Modifying Transforms
//////////////////////////////////////
//! Concatenates two transformation matrices and returns the resulting matrix in t1
//! The operation performed is t_new = t1*t2. This order is not commutative so be careful
//! when concatenating the matrices.
//! Note t_new can safely be be the same pointer as t1 or t2.
//! @param t_new Pointer to destination transformation matrix
//! @param t1 Pointer to transformation matrix to concatenate with t2 where t_new = t1*t2
//! @param t2 Pointer to transformation matrix to concatenate with t1 where t_new = t1*t2
void gtransform_concat(GTransform *t_new, const GTransform *t1, const GTransform * t2);

//! Updates the input transformation matrix by applying a translation.
//! This results in applying the following matrix below (i.e. t_new = t_scale*t):
//! t_scale = [ sx  0   0 ]
//!           [ 0   sy  0 ]
//!           [ 0   0   1  ]
//! Note t_new can safely be be the same pointer as t.
//! @param t_new Pointer to destination transformation matrix
//! @param t Pointer to transformation matrix that will be scaled
//! @param sx X scaling factor
//! @param sy Y scaling factor
void gtransform_scale(GTransform *t_new, GTransform *t, GTransformNumber sx, GTransformNumber sy);

//! Similar to gtransform_scale but with native number types (i.e. char, int, float, etc)
//! @param t_new Pointer to destination transformation matrix
//! @param t Pointer to transformation matrix that will be scaled
//! @param sx X scaling factor (type is char, int, float, etc)
//! @param sy Y scaling factor (type is char, int, float, etc)
#define gtransform_scale_number(t_new, t, sx, sy) \
        gtransform_scale(t_new, t,             \
                         GTransformNumberFromNumber(sx), GTransformNumberFromNumber(sy))

//! Updates the input transformation matrix by applying a translation.
//! This results in applying the following matrix below (i.e. t_new = t_translation*t):
//! t_translation = [ 1   0   0 ]
//!                 [ 0   1   0 ]
//!                 [ tx  ty  1 ]
//! Note t_new can safely be be the same pointer as t.
//! @param t_new Pointer to destination transformation matrix
//! @param t Pointer to transformation matrix that will be translated
//! @param tx X translation factor
//! @param ty Y translation factor
void gtransform_translate(GTransform *t_new, GTransform *t,
                          GTransformNumber tx, GTransformNumber ty);

//! Similar to gtransform_translate but with native number types (i.e. char, int, float, etc)
//! @param t_new Pointer to destination transformation matrix
//! @param t Pointer to transformation matrix that will be translated
//! @param tx X translation factor (type is char, int, float, etc)
//! @param ty Y translation factor (type is char, int, float, etc)
#define gtransform_translate_number(t_new, t, tx, ty) \
        gtransform_translate(t_new, t,             \
                             GTransformNumberFromNumber(tx), GTransformNumberFromNumber(ty))

//! Updates the input transformation matrix by applying a rotation of angle degrees.
//! This results in applying the following matrix below (i.e. t_new = tr*t):
//! tr = [ cos(angle)   -sin(angle)   0 ]
//!      [ sin(angle)   cos(angle)    0 ]
//!      [ 0            0             1 ]
//! Note t_new can safely be be the same pointer as t.
//! @param t_new Pointer to destination transformation matrix
//! @param t Pointer to transformation matrix that will be rotated
//! @param angle Rotation angle to apply (type is in same format as trig angle 0..TRIG_MAX_ANGLE)
void gtransform_rotate(GTransform *t_new, GTransform *t, int32_t angle);

//! Returns the inversion of a given transformation matrix t in t_new.
//! Function returns true if operation is successful; false if the matrix cannot be inverted
//! If the matrix cannot be inverted, then the contents of t will be copied to t_new.
//! Note t_new can safely be be the same pointer as t.
//! @param t_new Pointer to destination transformation matrix
//! @param t Pointer to transformation matrix that will be inverted
//! @return True if inversion of input t matrix exists; False otherwise or if t is NULL.
bool gtransform_invert(GTransform *t_new, GTransform *t);

//////////////////////////////////////
/// Applying Transformations
//////////////////////////////////////
//! Transforms a single GPoint (x,y) based on the transformation matrix
//! @param point GPoint to be transformed
//! @param t Pointer to transformation matrix to apply to the GPoint
//! @return GPointPrecise after transforming the GPoint; if t is NULL then just convert the
//! GPoint to a GPointPrecise.
GPointPrecise gpoint_transform(GPoint point, const GTransform * const t);

//! Transforms a single GVector (dx,dy) based on the transformation matrix
//! @param point GVector to be transformed
//! @param t Pointer to transformation matrix to apply to the GVector
//! @return GVectorPrecise after transforming the GVector; if t is NULL then just convert the
//! GVector to a GVectorPrecise.
GVectorPrecise gvector_transform(GVector vector, const GTransform * const t);

//!   @} // end addtogroup GraphicsTransforms
//! @} // end addtogroup Graphics

