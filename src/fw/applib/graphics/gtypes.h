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

//! @file graphics/gtypes.h
//!

#pragma once

#include "drivers/display/display.h" // FIXME: Need display dimensions
#include "resource/resource.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/math_fixed.h"

#if !(defined(SDK) || defined(UNITTEST))
#endif

#include <stdint.h>
#include <stdbool.h>

//! @addtogroup Graphics
//! @{
//!   @addtogroup GraphicsTypes Graphics Types
//! \brief Basic graphics types (point, rect, size, color, bitmaps, etc.) and utility functions.
//!
//!   @{

typedef struct GContext GContext;

//! Color values.
typedef enum GColor2 {
  //! Represents black.
  GColor2Black = 0,
  //! Represents white.
  GColor2White = 1,
  //! Represents "clear" or transparent.
  GColor2Clear = ~0,
} GColor2;

//! @internal
//! 2-bit channel value of a GColor8; used to increase expressiveness for some internal routines.
typedef uint8_t GColor8Component;

//! @internal
//! The number of possible values of \ref GColor8Component
#define GCOLOR8_COMPONENT_NUM_VALUES (4)

typedef union GColor8 {
  uint8_t argb;
  struct {
    uint8_t b:2; //!< Blue
    uint8_t g:2; //!< Green
    uint8_t r:2; //!< Red
    uint8_t a:2; //!< Alpha. 3 = 100% opaque, 2 = 66% opaque, 1 = 33% opaque, 0 = transparent.
  };
} GColor8;

typedef GColor8 GColor;

//! True if both colors are identical or both are invisible (i.e. both have alpha values of .a=0).
bool gcolor_equal(GColor8 x, GColor8 y);

//! Deprecated, doesn't check if both colors are invisible. Kept for backwards compatibility.
bool gcolor_equal__deprecated(GColor8 x, GColor8 y);

//! @internal
//! Returns true if the alpha channel of the given color is set to transparent.
bool gcolor_is_transparent(GColor8 color);

//! @internal
//! Returns GColorClear if gcolor_is_transparent and the passed color with .a=3 otherwise
GColor8 gcolor_closest_opaque(GColor8 color);

//! @internal
//! Returns Black, White or Clear depending on the luminance
GColor8 gcolor_get_bw(GColor8 color);

//! @internal
//! Returns Black, Dark Gray, Light Gray, White or Clear depending on the luminance
GColor8 gcolor_get_grayscale(GColor8 color);

//! This method assists in improving the legibility of text on various background colors.
//! It takes the background color for the region in question and computes a color for
//! maximum legibility.
//! @param background_color Background color for the region in question
//! @return A legible color for the given background color
GColor8 gcolor_legible_over(GColor8 background_color);

//! @internal
//! Returns true if the alpha channel of the given color is set to 0.
bool gcolor_is_invisible(GColor8 color);

//! @internal
//! Lookup table to map from a 6-bit color (GColor8.argb & 0b00111111) to a 2-bit luminance
//! Created in Photoshop by mapping the Pebble color palette to GColorBlack (0), GColorDarkGray (1),
//! GColorLightGray (2), and GColorWhite (3)
extern const GColor8Component g_color_luminance_lookup[];

//! @internal
//! Get the luminance of a color
static inline GColor8Component gcolor_get_luminance(GColor8 color) {
  return g_color_luminance_lookup[color.argb & 0b00111111];
}

//! @internal
// NOTE: This will be removed once alpha blending is fully supported.
GColor8 gcolor_blend(GColor8 src_color, GColor8 dest_color, uint8_t blending_factor);

//! @internal
// NOTE: This will be removed once alpha blending is fully supported.
GColor8 gcolor_alpha_blend(GColor8 src_color, GColor8 dest_color);

//! @internal
//! Initialize a lookup table for tinting with luminance based on the provided tint_color.
//! @note This function is extracted to optimize performing multiple lookups using the same tint
//! color via \ref gcolor_perform_lookup_using_color_luminance_and_multiply_alpha.
void gcolor_tint_luminance_lookup_table_init(
    GColor8 tint_color, GColor8 *lookup_table_out);

//! @internal
//! Lookup a color in the provided lookup_table using the luminance of the src_color and multiply
//! its alpha with the src_color's alpha to produce the result.
GColor8 gcolor_perform_lookup_using_color_luminance_and_multiply_alpha(
    GColor8 src_color, const GColor8 lookup_table[GCOLOR8_COMPONENT_NUM_VALUES]);

//! @internal
//! Tint the provided src_color using tint_color and the luminance of src_color, and then multiply
//! that color's alpha by the src_color's alpha to produce the result.
//! @note Calling this function is rather expensive because it initializes a lookup table for the
//! provided tint_color, so if you need to perform multiple lookups using the same tint_color you
//! should use \ref gcolor_tint_luminance_lookup_table_init to initialize the lookup table yourself
//! and then perform the lookups using
//! \ref gcolor_perform_lookup_using_color_luminance_and_multiply_alpha.
GColor8 gcolor_tint_using_luminance_and_multiply_alpha(GColor8 src_color, GColor8 tint_color);

//! @internal
//! Multiply the components of two GColor8 e.g. the alpha
GColor8Component gcolor_component_multiply(GColor8Component a, GColor8Component b);

// Define to describe the proper display format
// as we don't share platform defines between SDK (e.g. snowy vs. basalt)
// and display_snowy.h is not included in SDK generator this looks a bit ugly here
// TODO: PBL-21978 remove redundant comments as a workaround around for SDK generator
#if PBL_RECT && PBL_BW
#define GBITMAP_NATIVE_FORMAT GBitmapFormat1Bit
#elif PBL_RECT && PBL_COLOR
#define GBITMAP_NATIVE_FORMAT GBitmapFormat8Bit
#elif PBL_ROUND && PBL_COLOR
#define GBITMAP_NATIVE_FORMAT GBitmapFormat8BitCircular
#else
#warning "Unknown platform"
#endif

// convenient macros to distinguish between rect and round
// TODO: PBL-21978 remove redundant comments as a workaround around for SDK generator
#if PBL_RECT

//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On platforms with rectangular screen, the first expression will be chosen, the second otherwise.
#define PBL_IF_RECT_ELSE(if_true, if_false) (if_true)

//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On platforms with round screen, the first expression will be chosen, the second otherwise.
#define PBL_IF_ROUND_ELSE(if_true, if_false) (if_false)

#elif PBL_ROUND

//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On platforms with rectangular screen, the first expression will be chosen, the second otherwise.
#define PBL_IF_RECT_ELSE(if_true, if_false) (if_false)

//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On platforms with round screen, the first expression will be chosen, the second otherwise.
#define PBL_IF_ROUND_ELSE(if_true, if_false) (if_true)

#elif !defined(UNITTEST)
#warning "Unknown screen shape"
#endif

// Convenience macros to distinguish mask support
// TODO: PBL-21978 remove redundant comments as a workaround around for SDK generator
#if CAPABILITY_HAS_MASKING

//! Convenience macro to switch between two expressions depending on the platform's support of
//! masking.
//! On platforms that support masking, the first expression will be chosen, the second otherwise.
#define PBL_IF_MASK_ELSE(if_true, if_false) (if_true)

#else

//! Convenience macro to switch between two expressions depending on the platform's support of
//! masking.
//! On platforms that support masking, the first expression will be chosen, the second otherwise.
#define PBL_IF_MASK_ELSE(if_true, if_false) (if_false)

#endif

// convenient macros to distinguish between bw and color
// TODO: PBL-21978 remove redundant comments as a workaround around for SDK generator
#if PBL_BW

//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On black& white platforms, the first expression will be chosen, the second otherwise.
#define PBL_IF_BW_ELSE(if_true, if_false) (if_true)
//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On color platforms, the first expression will be chosen, the second otherwise.
#define PBL_IF_COLOR_ELSE(if_true, if_false) (if_false)

//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On color platforms, the first expression will be chosen, the second otherwise.
#define COLOR_FALLBACK(color, bw) (bw)

#elif PBL_COLOR

//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On black& white platforms, the first expression will be chosen, the second otherwise.
#define PBL_IF_BW_ELSE(if_true, if_false) (if_false)
//! Convenience macro to switch between two expression depending on the screen of the platform.
//! On color platforms, the first expression will be chosen, the second otherwise.
#define PBL_IF_COLOR_ELSE(if_true, if_false) (if_true)

//! Convenience macro allowing use of a fallback color for black and white platforms.
//! On color platforms, the first expression will be chosen, the second otherwise.
#define COLOR_FALLBACK(color, bw) (color)

#elif !defined(UNITTEST)
#warning "Unknown color depth"
#endif

//! Given a 2-bit color, get the system-native 8-bit equivalent.
GColor8 get_native_color(GColor2 color);

GColor2 get_closest_gcolor2(GColor8 color);

#include "gcolor_definitions.h"

//! Represents a point in a 2-dimensional coordinate system.
//! @note Conventionally, the origin of Pebble's 2D coordinate system is in the upper,
//! lefthand corner
//! its x-axis extends to the right and its y-axis extends to the bottom of the screen.
typedef struct GPoint {
  //! The x-coordinate.
  int16_t x;
  //! The y-coordinate.
  int16_t y;
} GPoint;

//! Work-around for function pointer return type GPoint to avoid
//! tripping the pre-processor to use the equally named GPoint define
typedef GPoint GPointReturn;

//! A GPoint Comparator returns the Order in which (a, b) occurs
//! @return negative int for a descending value (a > b), positive for an ascending value (b > a),
//! 0 for equal
typedef int (*GPointComparator)(const GPoint * const a, const GPoint * const b, void *context);

//! Convenience macro to make a GPoint.
#define GPoint(x, y) ((GPoint){(x), (y)})

//! Convenience macro to make a GPoint at (0, 0).
#define GPointZero GPoint(0, 0)

//! @internal
static inline GPoint gpoint_scalar_lshift(const GPoint point, int8_t s) {
  return GPoint(point.x << s, point.y << s);
}

//! @internal
static inline GPoint gpoint_scalar_rshift(const GPoint point, int8_t s) {
  return GPoint(point.x >> s, point.y >> s);
}

//! @internal
//! Returns the negation of a GPoint
//! @note In some cases `gpoint_sub(GPointZero, ...)` uses less code space, please try both
//! @see gpoint_sub
static inline GPoint gpoint_neg(const GPoint a) {
  return GPoint(-a.x, -a.y);
}

//! @internal
//! Adds two GPoints together
//! @note In some cases \ref gpoint_add_eq uses less code space, please try both functions
//! @see gpoint_add_eq
static inline GPoint gpoint_add(const GPoint a, const GPoint b) {
  return GPoint(a.x + b.x, a.y + b.y);
}

//! @internal
//! Mutably adds a GPoint to the first
//! @note In some cases \ref gpoint_add uses less code space, please try both functions
//! @see gpoint_add
static inline void gpoint_add_eq(GPoint *a, const GPoint b) {
  a->x += b.x;
  a->y += b.y;
}

//! @internal
//! Subtracts a GPoint from another
//! @note In some cases \ref gpoint_sub_eq or \ref gpoint_neg uses less code space, please try all
//! methods that apply
//! @see gpoint_sub_eq
static inline GPoint gpoint_sub(const GPoint a, const GPoint b) {
  return GPoint(a.x - b.x, a.y - b.y);
}

//! @internal
//! Mutably subtracts a GPoint to the first
//! @note In some cases \ref gpoint_sub or \ref gpoint_neg uses less code space, please try all
//! methods that apply
//! @see gpoint_sub
static inline void gpoint_sub_eq(GPoint *a, const GPoint b) {
  a->x -= b.x;
  a->y -= b.y;
}

//! @internal
//! Calculates the distance squared between two GPoints
static inline uint32_t gpoint_distance_squared(GPoint a, GPoint b) {
  int32_t x = b.x - a.x;
  int32_t y = b.y - a.y;
  return x * x + y * y;
}

//! Tests whether 2 points are equal.
//! @param point_a Pointer to the first point
//! @param point_b Pointer to the second point
//! @return `true` if both points are equal, `false` if not.
bool gpoint_equal(const GPoint * const point_a, const GPoint * const point_b);

//! Sorts an array of gpoints using a given GPointComparator
//! @param reverse false to maintain order, true to reverse the order
void gpoint_sort(GPoint *points, size_t num_points, GPointComparator comparator, void *context,
    bool reverse);

#define GPOINT_PRECISE_MAX        0x2000 // 12 bit resolution
#define GPOINT_PRECISE_PRECISION  FIXED_S16_3_PRECISION
#define GPOINT_PRECISE_FACTOR     FIXED_S16_3_FACTOR

//! Internal representation of a point
//! 1 bit for sign, 12 bits represent the coordinate, 3 bits represent the precision
//! Supports -4096.000 px to 4095.875 px resolution
typedef struct __attribute__ ((__packed__)) GPointPrecise {
  //! The x-coordinate.
  Fixed_S16_3 x;
  //! The y-coordinate.
  Fixed_S16_3 y;
} GPointPrecise;

//! Convenience macro to make a GPointPrecise.
#define GPointPrecise(x, y) ((GPointPrecise){{(x)}, {(y)}})

//! Convenience macro to convert from GPoint to GPointPrecise.
#define GPointPreciseFromGPoint(point) \
        GPointPrecise((point.x % GPOINT_PRECISE_MAX) * GPOINT_PRECISE_FACTOR, \
                      (point.y % GPOINT_PRECISE_MAX) * GPOINT_PRECISE_FACTOR)

//! Convenience macro to convert from GPointPrecise to GPoint.
#define GPointFromGPointPrecise(pointP) \
        GPoint(pointP.x.raw_value >> GPOINT_PRECISE_PRECISION, \
               pointP.y.raw_value >> GPOINT_PRECISE_PRECISION)

//! Tests whether 2 precise points are equal.
//! @param pointP_a Pointer to the first precise point
//! @param pointP_b Pointer to the second precise point
//! @return `true` if both points are equal, `false` if not.
bool gpointprecise_equal(const GPointPrecise * const pointP_a,
                         const GPointPrecise * const pointP_b);

GPointPrecise gpointprecise_midpoint(const GPointPrecise a,
                                     const GPointPrecise b);

GPointPrecise gpointprecise_add(const GPointPrecise a,
                                const GPointPrecise b);
GPointPrecise gpointprecise_sub(const GPointPrecise a,
                                const GPointPrecise b);

//! Represents a vector in a 2-dimensional coordinate system.
typedef struct GVector {
  //! The x-coordinate of the vector.
  int16_t dx;
  //! The y-coordinate of the vector.
  int16_t dy;
} GVector;

#define GVector(dx, dy) ((GVector){(dx), (dy)})

#define GVECTOR_PRECISE_MAX            GPOINT_PRECISE_MAX
#define GVECTOR_PRECISE_PRECISION      GPOINT_PRECISE_PRECISION

//! Represents a transformed vector in a 2-dimensional coordinate system.
typedef struct GVectorPrecise {
  //! The x-coordinate of the vector.
  Fixed_S16_3 dx;
  //! The y-coordinate of the vector.
  Fixed_S16_3 dy;
} GVectorPrecise;

//! Convenience macro to make a GVectorPrecise.
#define GVectorPrecise(dx, dy) ((GVectorPrecise){{(dx)}, {(dy)}})

//! Convenience macro to convert from GVector to GVectorPrecise.
#define GVectorPreciseFromGVector(vector) \
        GVectorPrecise((vector.dx % GVECTOR_PRECISE_MAX) << GVECTOR_PRECISE_PRECISION, \
                      (vector.dy % GVECTOR_PRECISE_MAX) << GVECTOR_PRECISE_PRECISION)

//! Convenience macro to convert from GVectorPrecise to GVector.
#define GVectorFromGVectorPrecise(vectorP) \
        GVector(vectorP.dx.raw_value >> GVECTOR_PRECISE_PRECISION, \
                vectorP.dy.raw_value >> GVECTOR_PRECISE_PRECISION)

//! Tests whether 2 precise vectors are equal.
//! @param vectorP_a Pointer to the first precise vector
//! @param vectorP_b Pointer to the second precise vector
//! @return `true` if both vectors are equal, `false` if not.
bool gvectorprecise_equal(const GVectorPrecise * const vectorP_a,
                          const GVectorPrecise * const vectorP_b);

typedef struct GSizePrecise {
  Fixed_S16_3 w;
  Fixed_S16_3 h;
} GSizePrecise;

typedef struct GRectPrecise {
  GPointPrecise origin;
  GSizePrecise size;
} GRectPrecise;

static inline Fixed_S16_3 grect_precise_get_max_x(const GRectPrecise *rect) {
  return (Fixed_S16_3) {.raw_value = rect->origin.x.raw_value + rect->size.w.raw_value};
}

static inline Fixed_S16_3 grect_precise_get_max_y(const GRectPrecise *rect) {
  return (Fixed_S16_3) {.raw_value = rect->origin.y.raw_value + rect->size.h.raw_value};
}

void grect_precise_standardize(GRectPrecise *rect);

//! Represents a 2-dimensional size.
typedef struct GSize {
  //! The width
  int16_t w;
  //! The height
  int16_t h;
} GSize;

//! @internal
//! Work-around for function pointer return type GSize to avoid
//! tripping the pre-processor to use the equally named GSize define
typedef GSize GSizeReturn;

//! Convenience macro to make a GSize.
#define GSize(w, h) ((GSize){(w), (h)})

//! Convenience macro to make a GSize of (0, 0).
#define GSizeZero GSize(0, 0)

//! @internal
//! Adds two GSizes together
//! @note In some cases \ref gsize_add_eq uses less code space, please try both functions
//! @see gsize_add_eq
static inline GSize gsize_add(const GSize a, const GSize b) {
  return GSize(a.w + b.w, a.h + b.h);
}

//! @internal
//! Mutably adds a GSize to the first
//! @note In some cases \ref gsize_add uses less code space, please try both functions
//! @see gsize_add
static inline void gsize_add_eq(GSize *a, const GSize b) {
  a->w += b.w;
  a->h += b.h;
}

//! @internal
//! Mutably subtracts a GSize from the first
static inline void gsize_sub_eq(GSize *a, const GSize b) {
  a->w -= b.w;
  a->h -= b.h;
}

//! @internal
static inline GSize gsize_scalar_lshift(const GSize size, int8_t s) {
  return GSize(size.w << s, size.h << s);
}

//! @internal
static inline GSize gsize_scalar_rshift(const GSize size, int8_t s) {
  return GSize(size.w >> s, size.h >> s);
}

//! Tests whether 2 sizes are equal.
//! @param size_a Pointer to the first size
//! @param size_b Pointer to the second size
//! @return `true` if both sizes are equal, `false` if not.
bool gsize_equal(const GSize *size_a, const GSize *size_b);

//! Represents a rectangle and defining it using the origin of
//! the upper-lefthand corner and its size.
typedef struct GRect {
  //! The coordinate of the upper-lefthand corner point of the rectangle.
  GPoint origin;
  //! The size of the rectangle.
  GSize size;
} GRect;

//! Work-around for function pointer return type GRect to avoid
//! tripping the pre-processor to use the equally named GRect define
typedef GRect GRectReturn;

//! Convenience macro to make a GRect
#define GRect(x, y, w, h) ((GRect){{(x), (y)}, {(w), (h)}})

//! Convenience macro to make a GRect of ((0, 0), (0, 0)).
#define GRectZero GRect(0, 0, 0, 0)

#define GRECT_PRINTF_FORMAT "{ %"PRId16", %"PRId16" } { %"PRId16", %"PRId16" }"
#define GRECT_PRINTF_FORMAT_EXPLODE(r) (r).origin.x, (r).origin.y, (r).size.w, (r).size.h

#define DISP_FRAME (GRect(0, 0, DISP_COLS, DISP_ROWS))

//! @internal
static inline GRect grect_scalar_lshift(const GRect rect, int8_t s) {
  return (GRect) {
    .origin.x = rect.origin.x << s,
    .origin.y = rect.origin.y << s,
    .size.w = rect.size.w << s,
    .size.h = rect.size.h << s,
  };
}

//! @internal
static inline GRect grect_scalar_rshift(const GRect rect, int8_t s) {
  return (GRect) {
    .origin.x = rect.origin.x >> s,
    .origin.y = rect.origin.y >> s,
    .size.w = rect.size.w >> s,
    .size.h = rect.size.h >> s,
  };
}

//! @internal
//! Resizes a GPoint from one GSize to another
static inline GPoint gpoint_scale_by_gsize(GPoint point, GSize from, GSize to) {
  return GPoint(
      (from.w != 0) ? (((int32_t) point.x * to.w) / from.w) : 0,
      (from.h != 0) ? (((int32_t) point.y * to.h) / from.h) : 0);
}

//! @internal
//! Expands a GRect in all directions by a given length
static inline GRect grect_scalar_expand(GRect box, int16_t x) {
  return GRect(box.origin.x - x, box.origin.y - x, box.size.w + 2 * x, box.size.h + 2 * x);
}

//! Tests whether 2 rectangles are equal.
//! @param rect_a Pointer to the first rectangle
//! @param rect_b Pointer to the second rectangle
//! @return `true` if both rectangles are equal, `false` if not.
bool grect_equal(const GRect* const rect_a, const GRect* const rect_b);

//! Tests whether the size of the rectangle is (0, 0).
//! @param rect Pointer to the rectangle
//! @return `true` if the rectangle its size is (0, 0), or `false` if not.
//! @note If the width and/or height of a rectangle is negative, this
//! function will return `true`!
bool grect_is_empty(const GRect* const rect);

//! Converts a rectangle's values so that the components of its size
//! (width and/or height) are both positive. In the width and/or height are negative,
//! the origin will offset, so that the final rectangle overlaps with the original.
//! For example, a GRect with size (-10, -5) and origin (20, 20), will be standardized
//! to size (10, 5) and origin (10, 15).
//! @param[in] rect The rectangle to convert.
//! @param[out] rect The standardized rectangle.
void grect_standardize(GRect *rect);

//! Trim one rectangle using the edges of a second rectangle.
//! @param[in] rect_to_clip The rectangle that needs to be clipped (in place).
//! @param[out] rect_to_clip The clipped rectangle.
//! @param rect_clipper The rectangle of which the edges will serve as "scissors"
//! in order to trim `rect_to_clip`.
void grect_clip(GRect * const rect_to_clip, const GRect * const rect_clipper);

//! Calculate the smallest rectangle that contains both r1 and r2.
GRect grect_union(const GRect *r1, const GRect *r2);

//! Tests whether a rectangle contains a point.
//! @param rect The rectangle
//! @param point The point
//! @return `true` if the rectangle contains the point, or `false` if it does not.
bool grect_contains_point(const GRect *rect, const GPoint *point);

//! Convenience function to compute the center-point of a given rectangle.
//! This is equal to `(rect->x + rect->width / 2, rect->y + rect->height / 2)`.
//! @param rect The rectangle for which to calculate the center point.
//! @return The point at the center of `rect`
GPoint grect_center_point(const GRect *rect);

//! Reduce the width and height of a rectangle by insetting each of the edges with
//! a fixed inset. The returned rectangle will be centered relative to the input rectangle.
//! @note The function will trip an assertion if the crop yields a rectangle with negative width or height.
//! @param rect The rectangle that will be inset
//! @param crop_size_px The inset by which each of the rectangle will be inset.
//! A positive inset value results in a smaller rectangle, while negative inset value results
//! in a larger rectangle.
//! @return The cropped rectangle.
GRect grect_crop(GRect rect, const int32_t crop_size_px);

//! @internal
//! Returns a rectangle that is smaller or larger than the source rectangle,
//! with the same center point.
//! @note The rectangle is standardized and then the inset parameters are applied.
//! If the resulting rectangle would have a negative height or width, a GRectZero is returned.
GRect grect_inset_internal(GRect rect, int16_t dx, int16_t dy);

//! Represents insets for four sides. Negative values mean a side extends.
//! @see \ref grect_inset
typedef struct {
  //! The inset at the top of an object.
  int16_t top;
  //! The inset at the right of an object.
  int16_t right;
  //! The inset at the bottom of an object.
  int16_t bottom;
  //! The inset at the left of an object.
  int16_t left;
} GEdgeInsets;

//! helper for \ref GEdgeInsets macro
#define GEdgeInsets4(t, r, b, l) \
  ((GEdgeInsets){.top = t, .right = r, .bottom = b, .left = l})

//! helper for \ref GEdgeInsets macro
#define GEdgeInsets3(t, rl, b) \
  ((GEdgeInsets){.top = t, .right = rl, .bottom = b, .left = rl})

//! helper for \ref GEdgeInsets macro
#define GEdgeInsets2(tb, rl) \
  ((GEdgeInsets){.top = tb, .right = rl, .bottom = tb, .left = rl})

//! helper for \ref GEdgeInsets macro
#define GEdgeInsets1(trbl) \
  ((GEdgeInsets){.top = trbl, .right = trbl, .bottom = trbl, .left = trbl})

//! helper for \ref GEdgeInsets macro
#define GEdgeInsetsN(_1, _2, _3, _4, NAME, ...) NAME

//! Convenience macro to make a GEdgeInsets
//! This macro follows the CSS shorthand notation where you can call it with
//!  - just one value GEdgeInsets(v1) to configure all edges with v1
//!    (GEdgeInsets){.top = v1, .right = v1, .bottom = v1, .left = v1}
//!  - two values v1, v2 to configure a vertical and horizontal inset as
//!    (GEdgeInsets){.top = v1, .right = v2, .bottom = v1, .left = v2}
//!  - three values v1, v2, v3 to configure it with
//!    (GEdgeInsets){.top = v1, .right = v2, .bottom = v3, .left = v2}
//!  - four values v1, v2, v3, v4 to configure it with
//!    (GEdgeInsets){.top = v1, .right = v2, .bottom = v3, .left = v4}
//! @see \ref grect_insets
#define GEdgeInsets(...) \
  GEdgeInsetsN(__VA_ARGS__, GEdgeInsets4, GEdgeInsets3, GEdgeInsets2, GEdgeInsets1)(__VA_ARGS__)

//! Returns a rectangle that is shrunk or expanded by the given edge insets.
//! @note The rectangle is standardized and then the inset parameters are applied.
//! If the resulting rectangle would have a negative height or width, a GRectZero is returned.
//! @param rect The rectangle that will be inset
//! @param insets The insets that will be applied
//! @return The resulting rectangle
//! @note Use this function in together with the \ref GEdgeInsets macro
//! \code{.c}
//! GRect r_inset_all_sides = grect_inset(r, GEdgeInsets(10));
//! GRect r_inset_vertical_horizontal = grect_inset(r, GEdgeInsets(10, 20));
//! GRect r_expand_top_right_shrink_bottom_left = grect_inset(r, GEdgeInsets(-10, -10, 10, 10));
//! \endcode
GRect grect_inset(GRect rect, GEdgeInsets insets);

//! Convenience function to compute the max x-coordinate of a given rectangle.
//! @param rect The rectangle for which to calculate the max x-coordinate.
//! @return The max x-coordinate of the rect
static inline int16_t grect_get_max_x(const GRect *rect) {
  return rect->origin.x + rect->size.w;
}

//! Convenience function to compute the max y-coordinate of a given rectangle.
//! @internal
//! @param rect The rectangle for which to calculate the max y-coordinate.
//! @return The max y-coordinate of the rect
static inline int16_t grect_get_max_y(const GRect *rect) {
  return rect->origin.y + rect->size.h;
}

//! @internal
//! Convenience function to return the length of the longest side of a rect
static inline int16_t grect_longest_side(const GRect rect) {
  return MAX(ABS(rect.size.w), ABS(rect.size.h));
}

//! @internal
//! Convenience function to return the length of the shortest side of a rect
static inline int16_t grect_shortest_side(const GRect rect) {
  return MIN(ABS(rect.size.w), ABS(rect.size.h));
}

//! @internal
//! GBoxModel represents a box model using a minimal amount of values, a width and height for the
//! size changes to the base entity size and offset for the change in position not affecting the
//! positioning or box model of any other box.
typedef struct GBoxModel {
  //! Offset relatively positions the box without affecting the layout of other boxes. The offset
  //! can be used in combination with margin to achieve top-left or all-side margins.
  GPoint offset;
  //! Margin affects the size of the box, increasing size if positive and decreasing if negative.
  //! Parent boxes will treat the size of a box as the raw size plus the margin. Used alone, it
  //! adjusts the bottom-right margin, causing extra space to be between this box and boxes to the
  //! bottom and/or right.
  GSize margin;
} GBoxModel;

//! The format of a GBitmap can either be 1-bit or 8-bit.
typedef enum GBitmapFormat {
  GBitmapFormat1Bit = 0, //<! 1-bit black and white. 0 = black, 1 = white.
  GBitmapFormat8Bit,      //<! 6-bit color + 2 bit alpha channel. See \ref GColor8 for pixel format.
  GBitmapFormat1BitPalette,
  GBitmapFormat2BitPalette,
  GBitmapFormat4BitPalette,
  GBitmapFormat8BitCircular,
} GBitmapFormat;

//! GBitmap implementation supported up to the end of 2.x
#define GBITMAP_VERSION_0 0
//! GBitmap Version 1:
//!  - .format:3 field in .info_flags
//!  - .is_palette_heap_allocated:1 in .info_flags
//!  - .palette support
//!  - 32 bits of padding at end
#define GBITMAP_VERSION_1 1
#define GBITMAP_VERSION_CURRENT GBITMAP_VERSION_1

typedef struct __attribute__ ((__packed__)) GBitmapLegacy2 {
  //! Pointer to the address where the image data lives
  void *addr;
  //! @note The number of bytes per row may have restrictions depending on the format:
  //! - \ref GBitmapFormat1Bit: Must be a multiple of 4 (eg. word-padded)
  //! - All palettized formats must have byte-aligned rows.
  //! Also, the following should (naturally) be true: `(row_size_bytes * 8 >= bounds.w)`
  uint16_t row_size_bytes;

  //! Private attributes used by the system.
  //! @internal
  //! This union is here to make it easy to copy in a full uint16_t of flags from the binary format
  union {
    //! Bitfields of metadata flags.
    uint16_t info_flags;

    struct {
      //! Is .addr heap allocated? Do we need to free .addr in gbitmap_deinit?
      bool is_heap_allocated:1;
      uint16_t reserved:11;
       //! Version of bitmap structure and image data.
      uint16_t version:4;
    };
  };

  //! The box of bits that the `addr` field is pointing to, that contains
  //! the actual image data to use. Note that this may be a subsection of the
  //! data with padding on all sides.
  GRect bounds;
} GBitmapLegacy2;


//! Description of a single data row in the pixel data of a bitmap
//! @note This data type describes the actual pixel data of a bitmap and does not respect the
//!       bitmap's bounds.
//! @see \ref gbitmap_get_data_row_info
//! @see \ref gbitmap_get_data
//! @see \ref gbitmap_get_bounds
typedef struct {
    //! Address of the byte at column 0 of a given data row in a bitmap. Use this to calculate the
    //! memory address of a pixel. For GBitmapFormat8BitCircular or GBitmapFormat8Bit this would
    //! be: `uint8_t *pixel_addr = row_info.addr + x`.
    //! Note that this byte can be outside of the valid range for this row.
    //! For example: The first valid pixel (`min_x=76`) of a row might start at 76 bytes after the
    //! given data pointer (assuming that 1 pixel is represented as 1 byte as in
    //! GBitmapFormat8BitCircular or GBitmapFormat8Bit).
    uint8_t *data;
    //! The absolute column of a first valid pixel for a given data row.
    int16_t min_x;
    //! The absolute column of the last valid pixel for a given data row.
    //! For optimization reasons the result can be anywhere between
    //! grect_get_max_x(bitmap_bounds) - 1 and the physical data boundary.
    int16_t max_x;
} GBitmapDataRowInfo;

typedef struct BitmapInfo {
  //! Is .addr heap allocated? Do we need to free .addr in gbitmap_deinit?
  bool is_bitmap_heap_allocated:1;

  GBitmapFormat format:3;

  bool is_palette_heap_allocated:1;

  uint16_t reserved:7;
  //! Version of bitmap structure and image data.
  uint8_t version:4;
} BitmapInfo;

typedef struct __attribute__ ((__packed__)) GBitmap {
  //! Pointer to the address where the image data lives
  void *addr;
  //! @note The number of bytes per row may have restrictions depending on the format:
  //! - \ref GBitmapFormat1Bit: Must be a multiple of 4 (eg. word-padded)
  //! - All palettized formats must have byte-aligned rows.
  //! Also, the following should (naturally) be true: `(row_size_bytes * 8 >= bounds.w)`
  //! 0, if bitmap has a variable row size (GBitmapFormat8BitCircular)
  uint16_t row_size_bytes;

  //! Private attributes used by the system.
  //! @internal
  //! This union is here to make it easy to copy in a full uint16_t of flags from the binary format
  union {
    //! Bitfields of metadata flags.
    uint16_t info_flags;
    BitmapInfo info;
  };

  //! The box of bits that the `addr` field is pointing to, that contains
  //! the actual image data to use. Note that this may be a subsection of the
  //! data with padding on all sides.
  GRect bounds;

  union {
    //! If the format field indicates a Palletized bitmap format, this palette must point to a
    //! palette of the appropriate size.
    //! Example, \ref GBitmapFormat4BitPalette must have room for at least 2^4 = 16 GColors.
    GColor *palette;

    //! On GBitmapFormat8BitCircular, this points to a circular_map that's addressed according to
    //! .bounds (so it must consist of at least .bounds.origin.y + .bounds.size.h entries)
    const GBitmapDataRowInfoInternal *data_row_infos;
  };

  //! Pad GBitmap to give some space for future expansion.
  int32_t padding;
} GBitmap;

typedef struct GBitmapProcessor GBitmapProcessor;

//! Callback for the user to modify the GContext, replace the bitmap to be drawn, modify the
//! rectangle the bitmap will be drawn in, or do any other drawing before the bitmap is drawn to the
//! screen. Any changes to the GContext must be reversed and any new bitmap created should be
//! destroyed in the companion `GBitmapProcessorPostFunc` function.
//! @param processor GBitmapProcessor that is currently being used
//! @param ctx GContext that will be used to draw the bitmap
//! @param bitmap_to_use Pointer to the GBitmap that will be drawn (can be overwritten)
//! @param global_grect_to_use GRect in global screen coordinate space in which the bitmap will be
//! drawn
typedef void (*GBitmapProcessorPreFunc)(GBitmapProcessor *processor, GContext *ctx,
                                        const GBitmap **bitmap_to_use, GRect *global_grect_to_use);

//! Callback for the user to restore any changed state in the GContext, destroy any swapped bitmap,
//! or do any other drawing after the bitmap has been drawn to the screen.
//! @param processor GBitmapProcessor that is currently being used
//! @param ctx GContext that was used to draw the bitmap
//! @param bitmap_used GBitmap drawn (or attempted to be drawn), must be casted to non-const to
//! be destroyed (if applicable)
//! @param global_clipped_grect_used Clipped GRect in global screen coordinate space in which the
//! bitmap was drawn (test for `!grect_is_empty(*rect)` to check if the bitmap was actually drawn)
typedef void (*GBitmapProcessorPostFunc)(GBitmapProcessor *processor, GContext *ctx,
                                         const GBitmap *bitmap_used,
                                         const GRect *global_clipped_grect_used);

//! @internal
//! Clients can "subclass" this struct to provide additional data to the processor's functions
typedef struct GBitmapProcessor {
  //! Called before the bitmap is drawn
  GBitmapProcessorPreFunc pre;
  //! Called after the bitmap is drawn
  GBitmapProcessorPostFunc post;
} GBitmapProcessor;

//! Provides information about a pixel data row
//! @param bitmap A pointer to the GBitmap to get row info
//! @param y Absolute row number in the pixel data, independent from the bitmap's bounds
//! @return Description of the row
//! @note This function does not respect the bitmap's bounds but purely operates on the pixel data.
//!       This function works with every bitmap format including GBitmapFormat1Bit.
//!       The result of the function for invalid rows is undefined.
//! @see \ref gbitmap_get_data
GBitmapDataRowInfo gbitmap_get_data_row_info(const GBitmap *bitmap, uint16_t y);

//! @internal
uint8_t gbitmap_get_bits_per_pixel(GBitmapFormat format);

//! @internal
uint8_t gbitmap_get_version(const GBitmap *bitmap);

//! Get the number of bytes per row in the bitmap data for the given \ref GBitmap.
//! On rectangular displays, this can be used as a safe way of iterating over the rows in the
//! bitmap, since bytes per row should be set according to format. On circular displays with pixel
//! format of \ref GBitmapFormat8BitCircular this will return 0, and should not be used for
//! iteration over frame buffer pixels. Instead, use \ref GBitmapDataRowInfo, which provides safe
//! minimum and maximum x values for a given row's y value.
//! @param bitmap A pointer to the GBitmap to get the bytes per row
//! @return The number of bytes per row of the GBitmap
//! @see \ref gbitmap_get_data
uint16_t gbitmap_get_bytes_per_row(const GBitmap *bitmap);

//! Get the \ref GBitmapFormat for the \ref GBitmap.
//! @param bitmap A pointer to the GBitmap to get the format
//! @return The format of the given \ref GBitmap.
GBitmapFormat gbitmap_get_format(const GBitmap *bitmap);

//! Get a pointer to the raw image data section of the given \ref GBitmap as specified by the format
//! of the bitmap.
//! @param bitmap A pointer to the GBitmap to get the data
//! @return pointer to the raw image data for the GBitmap
//! @see \ref gbitmap_get_bytes_per_row
//! @see \ref GBitmap
uint8_t* gbitmap_get_data(const GBitmap *bitmap);

//! Set the bitmap data for the given \ref GBitmap.
//! @param bitmap A pointer to the GBitmap to set data to
//! @param data A pointer to the bitmap data
//! @param format the format of the bitmap data. If this is a palettized format, make sure that
//! there is an accompanying call to \ref gbitmap_set_palette.
//! @param row_size_bytes How many bytes a single row takes. For example, bitmap data of format
//! \ref GBitmapFormat1Bit must have a row size as a multiple of 4 bytes.
//! @param free_on_destroy Set whether the data should be freed when the GBitmap is destroyed.
//! @see \ref gbitmap_destroy
void gbitmap_set_data(GBitmap *bitmap, uint8_t *data, GBitmapFormat format,
                     uint16_t row_size_bytes, bool free_on_destroy);

//! Get the palette for the given \ref GBitmap.
//! @param bitmap A pointer to the GBitmap to get the palette from.
//! @return Pointer to a \ref GColor array containing the palette colors.
//! @see \ref gbitmap_set_palette
GColor* gbitmap_get_palette(const GBitmap *bitmap);

//! Set the palette for the given \ref GBitmap.
//! @param bitmap A pointer to the GBitmap to set the palette to
//! @param palette The palette to be used. Make sure that the palette is large enough for the
//! bitmap's format.
//! @param free_on_destroy Set whether the palette data should be freed when the GBitmap is
//! destroyed or when another palette is set.
//! @see \ref gbitmap_get_format
//! @see \ref gbitmap_destroy
//! @see \ref gbitmap_set_palette
void gbitmap_set_palette(GBitmap *bitmap, GColor *palette, bool free_on_destroy);

//! Gets the bounds of the content for the \ref GBitmap. This is set when loading the image or
//! if changed by \ref gbitmap_set_bounds.
//! @param bitmap A pointer to the GBitmap to get the bounding box from.
//! @return The bounding box for the GBitmap.
//! @see \ref gbitmap_set_bounds
GRect gbitmap_get_bounds(const GBitmap *bitmap);

//! Set the bounds of the given \ref GBitmap.
//! @param bitmap A pointer to the GBitmap to set the bounding box.
//! @param bounds The bounding box to set.
//! @see \ref gbitmap_get_bounds
void gbitmap_set_bounds(GBitmap *bitmap, GRect bounds);

//! @internal
void gbitmap_init_with_data(GBitmap *bitmap, const uint8_t *data);

//! Creates a new GBitmap on the heap initialized with the provided Pebble image data.
//!
//! The resulting \ref GBitmap must be destroyed using \ref gbitmap_destroy() but the image
//! data will not be freed automatically. The developer is responsible for keeping the image
//! data in memory as long as the bitmap is used and releasing it after the bitmap is destroyed.
//! @note One way to generate Pebble image data is to use bitmapgen.py in the Pebble
//! SDK to generate a .pbi file.
//! @param data The Pebble image data. Must not be NULL. The function
//! assumes the data to be correct; there are no sanity checks performed on the
//! data. The data will not be copied and the pointer must remain valid for the
//! lifetime of this GBitmap.
//! @return A pointer to the \ref GBitmap. `NULL` if the \ref GBitmap could not
//! be created
GBitmap* gbitmap_create_with_data(const uint8_t *data);

//! @internal
uint16_t gbitmap_format_get_row_size_bytes(int16_t width, GBitmapFormat format);

//! @internal
void gbitmap_init_as_sub_bitmap(GBitmap *sub_bitmap, const GBitmap *base_bitmap, GRect sub_rect);

//! Create a new \ref GBitmap on the heap as a sub-bitmap of a 'base' \ref
//! GBitmap, using a GRect to indicate what portion of the base to use. The
//! sub-bitmap will just reference the image data and palette of the base bitmap.
//! No deep-copying occurs as a result of calling this function, thus the caller
//! is responsible for making sure the base bitmap and palette will remain available when
//! using the sub-bitmap. Note that you should not destroy the parent bitmap until
//! the sub_bitmap has been destroyed.
//! The resulting \ref GBitmap must be destroyed using \ref gbitmap_destroy().
//! @param[in] base_bitmap The bitmap that the sub-bitmap of which the image data
//! will be used by the sub-bitmap
//! @param sub_rect The rectangle within the image data of the base bitmap. The
//! bounds of the base bitmap will be used to clip `sub_rect`.
//! @return A pointer to the \ref GBitmap. `NULL` if the GBitmap could not
//! be created
GBitmap* gbitmap_create_as_sub_bitmap(const GBitmap *base_bitmap, GRect sub_rect);

//! Creates a new blank GBitmap on the heap initialized to zeroes.
//! In the case that the format indicates a palettized bitmap, a palette of appropriate size will
//! also be allocated on the heap.
//! The resulting \ref GBitmap must be destroyed using \ref gbitmap_destroy().
//! @param size The Pebble image dimensions as a \ref GSize.
//! @param format The \ref GBitmapFormat the created image should be in.
//! @return A pointer to the \ref GBitmap. `NULL` if the \ref GBitmap could not
//! be created
GBitmap* gbitmap_create_blank(GSize size, GBitmapFormat format);
GBitmapLegacy2* gbitmap_create_blank_2bit(GSize size);

//! Creates a new blank GBitmap on the heap, initialized to zeroes, and assigns it the given
//! palette.
//! No deep-copying of the palette occurs, so the caller is responsible for making sure the palette
//! remains available when using the resulting bitmap. Management of that memory can be handed off
//! to the system with the free_on_destroy argument.
//! @param size The Pebble image dimensions as a \ref GSize.
//! @param format the \ref GBitmapFormat the created image and palette should be in.
//! @param palette a pointer to a palette that is to be used for this GBitmap. The palette should
//! be large enough to hold enough colors for the specified format. For example,
//! \ref GBitmapFormat2BitPalette should have 4 colors, since 2^2 = 4.
//! @param free_on_destroy Set whether the palette data should be freed along with the bitmap data
//! when the GBitmap is destroyed.
//! @return A Pointer to the \ref GBitmap. `NULL` if the \ref GBitmap could not be created.
GBitmap* gbitmap_create_blank_with_palette(GSize size, GBitmapFormat format,
                                           GColor *palette, bool free_on_destroy);

//! Given a 1-bit GBitmap, create a new bitmap of format GBitmapFormat1BitPalette.
//! The new data buffer is allocated on the heap, and a 2-color palette is allocated as well.
//! @param src_bitmap A GBitmap of format GBitmapFormat1Bit which is to be copied into a newly
//! created GBitmap of format GBitmapFormat1BitPalettized.
//! @returns The newly created 1-bit palettized GBitmap, or NULL if there is not sufficient space.
//! @note The new bitmap does not depend on any data from src_bitmap, so src_bitmap can be freed
//! without worry.
GBitmap* gbitmap_create_palettized_from_1bit(const GBitmap *src_bitmap);

//! @internal
bool gbitmap_init_with_resource(GBitmap* bitmap, uint32_t resource_id);

//! Creates a new \ref GBitmap on the heap using a Pebble image file stored as a resource.
//! The resulting GBitmap must be destroyed using \ref gbitmap_destroy().
//! @param resource_id The ID of the bitmap resource to load
//! @return A pointer to the \ref GBitmap. `NULL` if the GBitmap could not
//! be created
GBitmap* gbitmap_create_with_resource(uint32_t resource_id);

//! @internal
GBitmap *gbitmap_create_with_resource_system(ResAppNum app_num, uint32_t resource_id);

//! @internal
//! @see gbitmap_init_with_resource
//! @param app_num The app's resource bank number
//! @return true if we were sucessful, false otherwise
bool gbitmap_init_with_resource_system(GBitmap* bitmap, ResAppNum app_num, uint32_t resource_id);

//! @internal
//! Deinitialize a bitmap structure. This must be called for every bitmap that's been created with gbitmap_init_*
void gbitmap_deinit(GBitmap* bitmap);

//! Destroy a \ref GBitmap.
//! This must be called for every bitmap that's been created with gbitmap_create_*
//!
//! This function will also free the memory of the bitmap data (bitmap->addr) if the bitmap was created with \ref gbitmap_create_blank()
//! or \ref gbitmap_create_with_resource().
//!
//! If the GBitmap was created with \ref gbitmap_create_with_data(), you must release the memory
//! after calling gbitmap_destroy().
void gbitmap_destroy(GBitmap* bitmap);

//! Values to specify how two things should be aligned relative to each other.
//! ![](galign.png)
//! @see \ref bitmap_layer_set_alignment()
typedef enum GAlign {
  //! Align by centering
  GAlignCenter,
  //! Align by making the top edges overlap and left edges overlap
  GAlignTopLeft,
  //! Align by making the top edges overlap and left edges overlap
  GAlignTopRight,
  //! Align by making the top edges overlap and centered horizontally
  GAlignTop,
  //! Align by making the left edges overlap and centered vertically
  GAlignLeft,
  //! Align by making the bottom edges overlap and centered horizontally
  GAlignBottom,
  //! Align by making the right edges overlap and centered vertically
  GAlignRight,
  //! Align by making the bottom edges overlap and right edges overlap
  GAlignBottomRight,
  //! Align by making the bottom edges overlap and left edges overlap
  GAlignBottomLeft
} GAlign;

//! Aligns one rectangle within another rectangle, using an alignment parameter.
//! The relative coordinate systems of both rectangles are assumed to be the same.
//! When clip is true, `rect` is also clipped by the constraint.
//! @param[in] rect The rectangle to align (in place)
//! @param[out] rect The aligned and optionally clipped rectangle
//! @param inside_rect The rectangle in which to align `rect`
//! @param alignment Determines the alignment of `rect` within `inside_rect` by
//! specifying what edges of should overlap.
//! @param clip Determines whether `rect` should be trimmed using the edges of `inside_rect`
//! in case `rect` extends outside of the area that `inside_rect` covers after the alignment.
void grect_align(GRect *rect, const GRect *inside_rect, const GAlign alignment, const bool clip);

#if defined(PUBLIC_SDK)
//! Values to specify how the source image should be composited onto the destination image.
//!
//! ![](compops.png)
//! Contrived example of how the different compositing modes affect drawing.
//! Often, the "destination image" is the render buffer and thus contains the image of
//! what has been drawn before or "underneath".
//!
//! For color displays, only two compositing modes are supported, \ref GCompOpAssign and
//! \ref GCompOpSet. The behavior of other compositing modes are undefined and may change in the
//! future. Transparency can be achieved using \ref GCompOpSet and requires pixel values with alpha
//! value .a < 3.
//! @see \ref bitmap_layer_set_compositing_mode()
//! @see \ref graphics_context_set_compositing_mode()
//! @see \ref graphics_draw_bitmap_in_rect()
//! @see \ref graphics_draw_rotated_bitmap()
typedef enum {
  //! Assign the pixel values of the source image to the destination pixels,
  //! effectively replacing the previous values for those pixels. For color displays, when drawing
  //! a palettized or 8-bit \ref GBitmap image, the opacity value is ignored.
  GCompOpAssign,
  //! Assign the **inverted** pixel values of the source image to the destination pixels,
  //! effectively replacing the previous values for those pixels.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpAssignInverted,
  //! Use the boolean operator `OR` to composite the source and destination pixels.
  //! The visual result of this compositing mode is the source's white pixels
  //! are painted onto the destination and the source's black pixels are treated
  //! as clear.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpOr,
  //! Use the boolean operator `AND` to composite the source and destination pixels.
  //! The visual result of this compositing mode is the source's black pixels
  //! are painted onto the destination and the source's white pixels are treated
  //! as clear.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpAnd,
  //! Clears the bits in the destination image, using the source image as mask.
  //! The visual result of this compositing mode is that for the parts where the source image is
  //! white, the destination image will be painted black. Other parts will be left untouched.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpClear,
  //! Sets the bits in the destination image, using the source image as mask.
  //! This mode is required to apply any transparency of your bitmap.
  //! @note For bitmaps of the format GBitmapFormat1Bit, the visual result of this compositing
  //!   mode is that for the parts where the source image is black, the destination image will be
  //!   painted white. Other parts will be left untouched.
  GCompOpSet,
} GCompOp;
#else
//! Values to specify how the source image should be composited onto the destination image.
//!
//! ![](compops.png)
//! Contrived example of how the different compositing modes affect drawing.
//! Often, the "destination image" is the render buffer and thus contains the image of
//! what has been drawn before or "underneath".
//!
//! For color displays, only two compositing modes are supported, \ref GCompOpAssign and
//! \ref GCompOpSet. The behavior of other compositing modes are undefined and may change in the
//! future. Transparency can be achieved using \ref GCompOpSet and requires pixel values with alpha
//! value .a < 3.
//! @see \ref bitmap_layer_set_compositing_mode()
//! @see \ref graphics_context_set_compositing_mode()
//! @see \ref graphics_draw_bitmap_in_rect()
//! @see \ref graphics_draw_rotated_bitmap()
typedef enum {
  //! Assign the pixel values of the source image to the destination pixels,
  //! effectively replacing the previous values for those pixels. For color displays, when drawing
  //! a palettized or 8-bit \ref GBitmap image, the opacity value is ignored.
  GCompOpAssign,
  //! Assign the **inverted** pixel values of the source image to the destination pixels,
  //! effectively replacing the previous values for those pixels.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpAssignInverted,
  //! Use the boolean operator `OR` to composite the source and destination pixels.
  //! The visual result of this compositing mode is the source's white pixels
  //! are painted onto the destination and the source's black pixels are treated
  //! as clear.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpOr,
  //! Use the boolean operator `AND` to composite the source and destination pixels.
  //! The visual result of this compositing mode is the source's black pixels
  //! are painted onto the destination and the source's white pixels are treated
  //! as clear.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpAnd,
  //! Clears the bits in the destination image, using the source image as mask.
  //! The visual result of this compositing mode is that for the parts where the source image is
  //! white, the destination image will be painted black. Other parts will be left untouched.
  //! @note For bitmaps with a format different from GBitmapFormat1Bit, this mode is not supported
  //!     and the resulting behavior is undefined.
  GCompOpClear,
  //! Sets the bits in the destination image, using the source image as mask.
  //! This mode is required to apply any transparency of your bitmap.
  //! @note For bitmaps of the format GBitmapFormat1Bit, the visual result of this compositing
  //!   mode is that for the parts where the source image is black, the destination image will be
  //!   painted white. Other parts will be left untouched.
  GCompOpSet,
  // TODO PBL-37523: Rename GCompOpTint to GCompOpTintAlpha
  //! Sets the bits in the destination image to the tint color using the source image for
  //! transparency. For GBitmapFormat1Bit, the visual result of this compositing mode is that for
  //! the parts where the source image is black, the destination image will be painted the tint
  //! color. Other parts will be left untouched.
  //! For any other bitmap format, the destination will colored with the tint color and the
  //! source image's transparency.
  GCompOpTint,
  //! Sets the bits in the destination image to a value in the linear range from the tint color to
  //! the inverse of the tint color based on the luminance of the source image while preserving the
  //! transparency of the tint color.
  //! For GBitmapFormat1Bit, the visual result is identical to that when using GCompOpTint.
  GCompOpTintLuminance
} GCompOp;
#endif

//! Repeat Sequence or animation indefinitely.
#define PLAY_COUNT_INFINITE UINT32_MAX
//! Duration of Sequence or animation is infinite.
#define PLAY_DURATION_INFINITE UINT32_MAX

//!   @} // end addtogroup GraphicsTypes

//!   @addtogroup Drawing Drawing Primitives
//!   @{

//! Bit mask values to specify the corners of a rectangle.
//! The values can be combines using binary OR (`|`),
//! For example: the mask to indicate top left and bottom right corners can:
//! be created as follows: `(GCornerTopLeft | GCornerBottomRight)`
typedef enum {
  //! No corners
  GCornerNone = 0,
  //! Top-Left corner
  GCornerTopLeft = 1 << 0,
  //! Top-Right corner
  GCornerTopRight = 1 << 1,
  //! Bottom-Left corner
  GCornerBottomLeft = 1 << 2,
  //! Bottom-Right corner
  GCornerBottomRight = 1 << 3,
  //! All corners
  GCornersAll = GCornerTopLeft | GCornerTopRight | GCornerBottomLeft | GCornerBottomRight,
  //! Top corners
  GCornersTop = GCornerTopLeft | GCornerTopRight,
  //! Bottom corners
  GCornersBottom = GCornerBottomLeft | GCornerBottomRight,
  //! Left corners
  GCornersLeft = GCornerTopLeft | GCornerBottomLeft,
  //! Right corners
  GCornersRight = GCornerTopRight | GCornerBottomRight,
} GCornerMask;

//!   @} // end addtogroup Drawing
//! @} // end addtogroup Graphics

typedef void (*GDrawRawAssignHorizontalLineFunc)(GContext *ctx, int16_t y, Fixed_S16_3 x1,
                                                 Fixed_S16_3 x2, GColor color);

typedef void (*GDrawRawAssignVerticalLineFunc)(GContext *ctx, int16_t x, Fixed_S16_3 y1,
                                               Fixed_S16_3 y2, GColor color);

typedef void (*GDrawRawBlendHorizontalLineFunc)(GContext *ctx, int16_t y, int16_t x1,
                                                int16_t x2, GColor color);

typedef void (*GDrawRawBlendVerticalLineFunc)(GContext *ctx, int16_t x, int16_t y1,
                                              int16_t y2, GColor color);

typedef void (*GDrawRawAssignHorizontalLineDeltaFunc)(GContext *ctx, int16_t y,
                                                      Fixed_S16_3 x1, Fixed_S16_3 x2,
                                                      uint8_t left_aa_offset,
                                                      uint8_t right_aa_offset,
                                                      int16_t clip_box_min_x,
                                                      int16_t clip_box_max_x, GColor color);

typedef struct {
  GDrawRawAssignHorizontalLineFunc assign_horizontal_line;
  GDrawRawAssignVerticalLineFunc assign_vertical_line;
  GDrawRawBlendHorizontalLineFunc blend_horizontal_line;
  GDrawRawAssignHorizontalLineDeltaFunc assign_horizontal_line_delta;
  GDrawRawBlendVerticalLineFunc blend_vertical_line;
} GDrawRawImplementation;

typedef struct GDrawMask GDrawMask;

//! @internal
//! Data structure that contains all kinds of drawing parameters, like the clipping box,
//! the drawing box, stroke, fill and text colors and bitmap compositing mode.
typedef struct PACKED {
  //! The box relative to bitmap's bounds, that graphics functions MUST use to clip what they draw
  GRect clip_box;
  //! The box relative to bitmap's bounds, that graphics functions MUST use as their coordinate space
  GRect drawing_box;
  //! Line drawing functions MUST use this as line color
  GColor stroke_color;
  //! Fill drawing functions MUST use this as fill color
  GColor fill_color;
  //! Text drawing functions MUST use this as text color
  GColor text_color;
  //! This color MUST be used as the tint color when using the drawing functions
  //! \ref graphics_draw_bitmap_in_rect and \ref graphics_draw_rotated_bitmap_in_rect
  //! on Basalt with the compositing mode as GCompOpOr.
  GColor tint_color;
  //! Bitmap compositing functions MUST use this as the compositing mode
  GCompOp compositing_mode:3;
#if PBL_COLOR
  //! Antialiasing stroke enabled or not; default value is false
  bool antialiased:1;
#endif
  //! When true, text rendering routines will try to avoid orphans
  //! This will be enabled for every non-external app in graphics_context_init()
  bool avoid_text_orphans:1;
  //! Stroke width applied to drawing routines; default value is 1; accepted range 1..255
  uint8_t stroke_width;
  //! Struct of raw drawing function pointers; default value is g_default_draw_implementation
  const GDrawRawImplementation *draw_implementation;
#if CAPABILITY_HAS_MASKING
  //! Optional draw mask
  //! Depending on the mask mode, (ignore, recording, use) the .draw_implementation will be
  //! set accordingly
  GDrawMask *draw_mask;
#endif // CAPABILITY_HAS_MASKING
} GDrawState;

//! @internal
//! Internal representation of a transformation matrix coefficient
typedef Fixed_S32_16 GTransformNumber;

//! @internal
//! Data structure that contains the internal representation of a 3x3 tranformation matrix
//! The transformation matrix will be expressed as follows:
//! [ a  b  0 ]
//! [ c  d  0 ]
//! [ tx ty 1 ]
//! However, internally we do not need to store the last row since we only support two
//! dimensions (x,y). Thus the last row is omitted from the internal storage.
//! Data values are in 16.16 fixed point representation
typedef struct __attribute__ ((__packed__)) GTransform {
  GTransformNumber a;
  GTransformNumber b;
  GTransformNumber c;
  GTransformNumber d;
  GTransformNumber tx;
  GTransformNumber ty;
} GTransform;

//! Work-around for function pointer return type GTransform to avoid
//! tripping the pre-processor to use the equally named GTransform define
typedef GTransform GTransformReturn;

//! @internal
//! Converts a GPoint from local drawing coordinates to global coordinates.
GPoint gpoint_to_global_coordinates(const GPoint point, GContext *ctx);

//! @internal
//! Converts a GPoint from global coordinates to local drawing coordinates.
GPoint gpoint_to_local_coordinates(const GPoint point, GContext *ctx);

//! @internal
//! Converts a GRect from local drawing coordinates to global coordinates.
//! The GRect size is unmodified. Use the gpoint version if size is not needed.
GRect grect_to_global_coordinates(const GRect rect, GContext *ctx);

//! @internal
//! Converts a GRect from global coordinates to local drawing coordinates.
//! The GRect size is unmodified. Use the gpoint version if size is not needed.
GRect grect_to_local_coordinates(const GRect rect, GContext *ctx);

//! @internal
//! Returns true if the two GRects overlap at all.
bool grect_overlaps_grect(const GRect *r1, const GRect *r2);

//! @internal
BitmapInfo gbitmap_get_info(const GBitmap *bitmap);

//! @internal
uint8_t gbitmap_get_palette_size(GBitmapFormat format);

//! @internal
typedef struct GRange {
  int16_t origin;
  int16_t size;
} GRange;

//! @internal
typedef struct GRangeHorizontal {
  int16_t origin_x;
  int16_t size_w;
} GRangeHorizontal;

//! @internal
typedef struct GRangeVertical {
  int16_t origin_y;
  int16_t size_h;
} GRangeVertical;

//! @internal
void grange_clip(GRange *range_to_clip, const GRange * const range_clipper);
