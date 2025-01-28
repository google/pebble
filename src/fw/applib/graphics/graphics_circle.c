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

#include "graphics_circle.h"
#include "graphics_circle_private.h"
#include "graphics.h"
#include "graphics_private.h"
#include "kernel/pbl_malloc.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/size.h"
#include "util/trig.h"

static Fixed_S16_3 prv_get_circle_border(int16_t y, uint16_t radius) {
  // Match to the precision we need here
  y *= FIXED_S16_3_ONE.raw_value;
  radius *= FIXED_S16_3_ONE.raw_value;

  return (Fixed_S16_3){.raw_value = radius - integer_sqrt(radius * radius - y * y)};
}

static Fixed_S16_3 prv_get_ellipsis_border(Fixed_S16_3 offset, uint32_t offset_radius_sq,
                                           uint32_t opposite_radius_sq) {
  if (offset_radius_sq == opposite_radius_sq) {
    // We're dealing with a circle
    return (Fixed_S16_3){.raw_value = integer_sqrt((offset_radius_sq << FIXED_S16_3_PRECISION) -
                                                    offset.raw_value * offset.raw_value)};
  }

  return (Fixed_S16_3){
    .raw_value = (integer_sqrt((opposite_radius_sq - opposite_radius_sq *
                                ((offset.raw_value * offset.raw_value) >> FIXED_S16_3_PRECISION) /
                                offset_radius_sq) << FIXED_S16_3_PRECISION))
  };
}

static GPointPrecise prv_get_rotated_precise_point(GPointPrecise center, uint16_t radius,
                                                   uint32_t angle) {
  return GPointPrecise((center.x.raw_value + radius * sin_lookup(angle) / TRIG_MAX_RATIO),
                       (center.y.raw_value - radius * cos_lookup(angle) / TRIG_MAX_RATIO));
}

static GPointPrecise prv_get_rotated_precise_point_for_ellipsis(GPointPrecise center,
                                                                uint16_t radius_x,
                                                                uint16_t radius_y,
                                                                uint32_t angle) {
  // PBL-25637: goes bonkers for ellipsis and angles around 90° and 270°
  if (radius_x == radius_y) {
    // We're dealing with circle here - theres an easier way...
    return prv_get_rotated_precise_point(center, radius_x, angle);
  }

  // This is an edge case due to fixedpoint math
  if (angle % QUADRANT_ANGLE == 0) {
    radius_x <<= FIXED_S16_3_PRECISION;
    radius_y <<= FIXED_S16_3_PRECISION;

    switch (angle / QUADRANT_ANGLE) {
      case 0:
      return GPointPrecise(center.x.raw_value, center.y.raw_value - radius_y);
      case 1:
      return GPointPrecise(center.x.raw_value + radius_x, center.y.raw_value);
      case 2:
      return GPointPrecise(center.x.raw_value, center.y.raw_value + radius_y);
      default:
      return GPointPrecise(center.x.raw_value - radius_x, center.y.raw_value);
    }
  }

  // This algorthm operates on angle starting at our 90° mark, so we add 90°
  // and flip x/y coordinates (see last line of this function)
  angle = (angle + (TRIG_MAX_ANGLE / 4)) % TRIG_MAX_ANGLE;

  // This is going to be divided by fixed-point precision number so division here is unnecessary
  int32_t radius_xy = ((int32_t)radius_x * radius_y);
  int32_t radius_xx = ((int32_t)radius_x * radius_x) >> FIXED_S16_3_PRECISION;
  int32_t radius_yy = ((int32_t)radius_y * radius_y) >> FIXED_S16_3_PRECISION;

  int64_t sin = sin_lookup(angle);
  int64_t cos = cos_lookup(angle);

  int64_t sin_sq = sin * sin / TRIG_MAX_RATIO;
  int64_t cos_sq = cos * cos / TRIG_MAX_RATIO;

  // We simulate tan(angle) by sin(angle)/cos(angle)
  int64_t rx_tan = radius_xx * sin_sq;
  if (cos_sq != 0) {
    rx_tan /= cos_sq;
  }

  int32_t sqrt_x = integer_sqrt((radius_yy + rx_tan) << FIXED_S16_3_PRECISION);

  int16_t x = 0;
  if (sqrt_x > 0) {
    x = radius_xy / sqrt_x;
  }

  // Between 90° and 270° we flip the x
  if (angle >= (TRIG_MAX_ANGLE / 4) && angle < (TRIG_MAX_ANGLE * 3 / 4)) {
    x *= -1;
  }

  // And y in this case is just x multiplied by tan(angle)
  int16_t y = x * sin / cos;

  // Flipping results by center point
  return GPointPrecise(center.x.raw_value - x, center.y.raw_value - y);
}

T_STATIC void graphics_circle_quadrant_draw_1px_non_aa(GContext* ctx, GPoint p, uint16_t radius,
                                                       GCornerMask quadrant) {
  int f = 1 - radius;
  int ddF_x = 1;
  int ddF_y = -2 * radius;
  int x = 0;
  uint16_t y = radius;

  p.x += ctx->draw_state.drawing_box.origin.x;
  p.y += ctx->draw_state.drawing_box.origin.y;

  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }

    x++;
    ddF_x += 2;
    f += ddF_x;

    if (quadrant & GCornerBottomRight) {
      graphics_private_set_pixel(ctx, GPoint(p.x + x, p.y + y));
      graphics_private_set_pixel(ctx, GPoint(p.x + y, p.y + x));
    }

    if (quadrant & GCornerTopRight) {
      graphics_private_set_pixel(ctx, GPoint(p.x + x, p.y - y));
      graphics_private_set_pixel(ctx, GPoint(p.x + y, p.y - x));
    }

    if (quadrant & GCornerBottomLeft) {
      graphics_private_set_pixel(ctx, GPoint(p.x - x, p.y + y));
      graphics_private_set_pixel(ctx, GPoint(p.x - y, p.y + x));
    }

    if (quadrant & GCornerTopLeft) {
      graphics_private_set_pixel(ctx, GPoint(p.x - x, p.y - y));
      graphics_private_set_pixel(ctx, GPoint(p.x - y, p.y - x));
    }
  }
}

static void prv_plot4(GBitmap *fb, GRect *clip_box, GPoint center, GPoint offset, int8_t brightness,
                      GColor stroke_color, GCornerMask quadrant) {
  /*
   * This will mirror given offset point over x and y coordinates of given center point
   *        |
   *   x1   |    x
   *        |
   *        |
   * -------+--------
   *        |
   *        |
   *   x2   |    x3
   *        |
   *
   *    +  center point
   *    -  x coordiante mirror line
   *    |  y coordinate mirror line
   *    x  given offset point
   *    xn mirrored points
   */

  for (unsigned int i=0; i < ARRAY_LENGTH(quadrant_mask_mul); i++) {
    if (quadrant_mask_mul[i].mask & quadrant) {
      int16_t x = center.x + (offset.x * quadrant_mask_mul[i].x_mul);
      int16_t y = center.y + (offset.y * quadrant_mask_mul[i].y_mul);
      graphics_private_plot_pixel(fb, clip_box, x, y, brightness, stroke_color);
    }
  }
}

static void prv_plot8(GBitmap *fb, GRect *clip_box, GPoint center, GPoint offset, int8_t brightness,
                      GColor stroke_color, GCornerMask quadrant) {
  /*
   * This will mirror given offset point over all eighths of the circle at given center
   *   \  x8| x  /
   *    \   |   /
   *     \  |  /
   *  x7  \ | /   x2
   * -------+--------
   *  x6  / | \   x3
   *     /  |  \
   *    /   |   \
   *   /  x5| x4 \
   *
   *    +  center point
   *    -  x coordiante mirror line
   *    |  y coordinate mirror line
   *    /  45 degree mirror line
   *    \  135 degree mirror line
   *    x  given offset point
   *    xn mirrored points
   */

  prv_plot4(fb, clip_box, center, offset, brightness, stroke_color, quadrant);

  // Swapping x and y for rest of the circle
  prv_plot4(fb, clip_box, center, GPoint(offset.y, offset.x), brightness, stroke_color, quadrant);
}

#if PBL_COLOR
T_STATIC void graphics_circle_quadrant_draw_1px_aa(GContext* ctx, GPoint p, uint16_t radius,
                                                   GCornerMask quadrant) {
  /* This will draw antialiased circle with width of 1px, can be drawn in quadrants
   * Based on wu-xiang line drawing, will draw circle in two steps
   * 1. Calculate point on the edge of eighth of the cricle and plot it around by mirroring
   *    - if point is matching pixel perfectly thats going to be on fully colored pixel
   *    - if theres fraction, two pixels will be colored accordingly
   * 2. Fill special case pixels (pixels that are between mirrored eighths)
   *    - special code to avoid overdrawing of those pixels and preserve antialiasing
   *    - three pixels calculated for circle with radius > 6
   *    - two pixels calculated for circle with radius < 6
   *
   * Theres also special case for the radius of 3, where algorithm couldnt stop at right
   *   point and wasn't drawing two pixels on each quadrant
   *
   * Here's quadrant example:
   *
   *             45 degree angle (radius * (sqrt(2)/2))
   *             |
   *             v
   *
   *   | | | |
   *       | | |
   *           | o
   *             o o        <- 45 degree angle (radius * (sqrt(2)/2))
   *               - -
   *                 - -
   *                 - -
   *                   -
   *   x               -
   *
   *      |  original calculated pixels for plotting
   *      -  mirrored eight of the circle (will mirror more of them if neccessary)
   *      o  special case pixels
   *      x  center of the circle
   */

  // To match whats being drawn by non-aa graphics_draw_circle...
  radius++;

  // Apply drawing_box
  p.x += ctx->draw_state.drawing_box.origin.x;
  p.y += ctx->draw_state.drawing_box.origin.y;

  // As close to sqrt(2)/2 as possible
  int stop_progress = radius * 707 / 1000;
  int progress;

  uint16_t radius_fixed = radius * FIXED_S16_3_ONE.raw_value;
  int16_t weighting_compliment_mask = MAX_PLOT_BRIGHTNESS;
  int16_t weighting = 0;

  // Locking framebuffer...
  GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);
  GColor stroke_color = ctx->draw_state.stroke_color;

  // Step 1
  for (progress = 0; progress < stop_progress; progress++) {
    Fixed_S16_3 edge = (Fixed_S16_3){.raw_value = radius_fixed -
                                      prv_get_circle_border(progress, radius).raw_value};

    if (edge.integer != 0) {
      weighting = (edge.fraction >> 1);

      prv_plot8(framebuffer, &ctx->draw_state.clip_box, p, GPoint(edge.integer - 1, progress),
                weighting, stroke_color, quadrant);

      prv_plot8(framebuffer, &ctx->draw_state.clip_box, p, GPoint(edge.integer, progress),
                weighting ^ weighting_compliment_mask, stroke_color, quadrant);
    } else {
      prv_plot8(framebuffer, &ctx->draw_state.clip_box, p, GPoint(edge.integer, progress),
                MAX_PLOT_OPACITY, stroke_color, quadrant);
    }
  }

  // Behold for magic number 3!!!
  // Note: magic numbers explained in main comment for this function
  int special_case_pixels = 3;

  // Accompanied by magic number 7 (not 6, we increased radius at beginning of this function)
  if (radius < 7) {
    // And sometimes magic number 2
    special_case_pixels = 2;
  }

  // Step 2
  // Special code for filling gap between mirrored parts in a manner that wont overdraw pixels
  for (; progress < stop_progress + special_case_pixels; progress++) {
    Fixed_S16_3 edge = (Fixed_S16_3){.raw_value = radius_fixed -
                                      prv_get_circle_border(progress, radius).raw_value};

    if (edge.integer != 0) {
      weighting = (edge.fraction >> 1);

      if (edge.integer - 1 > radius - stop_progress) {
        prv_plot4(framebuffer, &ctx->draw_state.clip_box, p, GPoint(edge.integer - 1, progress),
                  weighting, stroke_color, quadrant);
      }

      if (edge.integer > radius - stop_progress) {
        prv_plot4(framebuffer, &ctx->draw_state.clip_box, p, GPoint(edge.integer, progress),
                  weighting ^ weighting_compliment_mask, stroke_color, quadrant);
      }
    } else {
      if (edge.integer > radius - stop_progress) {
        prv_plot4(framebuffer, &ctx->draw_state.clip_box, p, GPoint(edge.integer, progress),
                  MAX_PLOT_OPACITY, stroke_color, quadrant);
      }
    }
  }

  // And for grand finale, super special case for radius of 4 (3 outside of this funct):
  if (radius == 4) {
    prv_plot4(framebuffer, &ctx->draw_state.clip_box, p, GPoint(2, 2), MAX_PLOT_OPACITY,
              stroke_color, quadrant);

    prv_plot4(framebuffer, &ctx->draw_state.clip_box, p, GPoint(2, 3), 2, stroke_color, quadrant);
  }

  // Releasing framebuffer...
  graphics_release_frame_buffer(ctx, framebuffer);
}
#endif // PBL_COLOR

static void prv_circle_arc_draw_1px(GContext* ctx, GPoint center, uint16_t radius,
                                    int32_t angle_start, int32_t angle_end) {
  // TODO: PBL-23119 Write non-aa function
  // TODO: PBL-24777 Write new 1px functions (with support for ellipsis)
//  prv_circle_quadrant_part_1px_aa(ctx, center, radius, config.start_quadrant.angle_start,
//                                  config.start_quadrant.angle_end);
//
//  prv_circle_quadrant_part_1px_aa(ctx, center, radius, config.end_quadrant.angle_start,
//                                  config.end_quadrant.angle_end);
//
//  if (config.full_quadrants != GCornerNone) {
//    graphics_circle_quadrant_draw_1px_aa(ctx, center, radius, config.full_quadrants);
//  }
}

inline void prv_vline_quadrant(GCornerMask quadrant, GCornerMask desired, GContext *ctx, int16_t x,
                               Fixed_S16_3 start, Fixed_S16_3 end) {
  if (quadrant & desired) {
    graphics_private_draw_vertical_line(ctx, x, start, end);
  }
}

inline void prv_hline_quadrant(GCornerMask quadrant, GCornerMask desired, GContext *ctx, int16_t y,
                               Fixed_S16_3 start, Fixed_S16_3 end) {
  if (quadrant & desired) {
    graphics_private_draw_horizontal_line(ctx, y, start, end);
  }
}

static void prv_stroke_circle_quadrant_full(GContext* ctx, GPoint p, uint16_t radius,
                                            uint8_t stroke_width, GCornerMask quadrant) {
  // This algorithm will draw stroked circle with vairable width (only odd numbers for now)
  const uint8_t half_stroke_width = stroke_width / 2;
  const int16_t inner_radius = radius - half_stroke_width;
  const uint8_t outer_radius = radius + half_stroke_width;

  if (inner_radius < 1) {
    // Hack for filling circles: filling is done by line primitives using stroke_color by default
    GColor temp_color = ctx->draw_state.fill_color;
    ctx->draw_state.fill_color = ctx->draw_state.stroke_color;

#if PBL_COLOR
    if (ctx->draw_state.antialiased) {
      graphics_internal_circle_quadrant_fill_aa(ctx, p, outer_radius, quadrant);
    } else {
      graphics_circle_quadrant_fill_non_aa(ctx, p, outer_radius, quadrant);
    }
#else
    graphics_circle_quadrant_fill_non_aa(ctx, p, outer_radius, quadrant);
#endif

    // Restore original status
    ctx->draw_state.fill_color = temp_color;
    return;
  }

  // Since fill_oval will use fill color, we have to swap those:
  const GColor fill_color = ctx->draw_state.fill_color;
  ctx->draw_state.fill_color = ctx->draw_state.stroke_color;

  // For pixel matching we need to decrease inner radius...
  prv_fill_oval_quadrant(ctx, p, outer_radius, outer_radius,
                            inner_radius - 1, inner_radius - 1, quadrant);

  ctx->draw_state.fill_color = fill_color;
}

static void prv_stroke_circle_quadrant_full_override_aa(GContext* ctx, GPoint p, uint16_t radius,
                                                        uint8_t stroke_width, GCornerMask quadrant,
                                                        bool anti_aliased) {
#if PBL_COLOR
  // Force antialiasing setting
  bool temp_anti_aliased = ctx->draw_state.antialiased;
  ctx->draw_state.antialiased = anti_aliased;
#endif

  // Call stroke circle quadrant function
  prv_stroke_circle_quadrant_full(ctx, p, radius, stroke_width, quadrant);

#if PBL_COLOR
  // Restore previous antialiasing setting
  ctx->draw_state.antialiased = temp_anti_aliased;
#endif
}

#if PBL_COLOR
//! Draws anit-aliased stroked quadrant of a circle
//! @internal
T_STATIC void graphics_circle_quadrant_draw_stroked_aa(GContext* ctx, GPoint p, uint16_t radius,
                                                       uint8_t stroke_width, GCornerMask quadrant) {
  prv_stroke_circle_quadrant_full_override_aa(ctx, p, radius, stroke_width, quadrant, true);
}
#endif // PBL_COLOR

//! Draws aliased stroked quadrant of a circle
//! @internal
T_STATIC void graphics_circle_quadrant_draw_stroked_non_aa(GContext* ctx, GPoint p, uint16_t radius,
                                                           uint8_t stroke_width,
                                                           GCornerMask quadrant) {
  prv_stroke_circle_quadrant_full_override_aa(ctx, p, radius, stroke_width, quadrant, false);
}

void graphics_circle_quadrant_draw(GContext* ctx, GPoint p, uint16_t radius, GCornerMask quadrant) {
  uint8_t stroke_width = ctx->draw_state.stroke_width;
#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    if (stroke_width > 1) {
      // Antialiased and Stroke Width > 1
      graphics_circle_quadrant_draw_stroked_aa(ctx, p, radius, stroke_width, quadrant);
      return;
    } else {
      // Antialiased and Stroke Width == 1 (not suppported on 1-bit color)
      graphics_circle_quadrant_draw_1px_aa(ctx, p, radius, quadrant);
      return;
    }
  }
#endif
  if (stroke_width > 1) {
    // Non-Antialiased and Stroke Width > 1
    graphics_circle_quadrant_draw_stroked_non_aa(ctx, p, radius, stroke_width, quadrant);
  } else {
    // Non-Antialiased and Stroke Width == 1
    graphics_circle_quadrant_draw_1px_non_aa(ctx, p, radius, quadrant);
  }
}

T_STATIC void graphics_circle_draw_1px_non_aa(GContext* ctx, GPoint p, uint16_t radius) {
  graphics_circle_quadrant_draw_1px_non_aa(ctx, p, radius, GCornersAll);

  p.x += ctx->draw_state.drawing_box.origin.x;
  p.y += ctx->draw_state.drawing_box.origin.y;

  graphics_private_set_pixel(ctx, GPoint(p.x, p.y + radius));
  graphics_private_set_pixel(ctx, GPoint(p.x, p.y - radius));
  graphics_private_set_pixel(ctx, GPoint(p.x + radius, p.y));
  graphics_private_set_pixel(ctx, GPoint(p.x - radius, p.y));
}

#if PBL_COLOR
T_STATIC void graphics_circle_draw_1px_aa(GContext* ctx, GPoint p, uint16_t radius) {
  graphics_circle_quadrant_draw_1px_aa(ctx, p, radius, GCornersAll);
}

//! Draws an antialiased circle of stroke width > 1
//! @note This only supports odd numbers for stroke_width - even numbers will be rounded up.
//! Minimal supported stroke_width is 3
T_STATIC void graphics_circle_draw_stroked_aa(GContext* ctx, GPoint p, uint16_t radius,
                                              uint8_t stroke_width) {
  graphics_circle_quadrant_draw_stroked_aa(ctx, p, radius, stroke_width, GCornersAll);
}
#endif // PBL_COLOR

//! Draws a non-antialiased circle of stroke width > 1
//! @note This only supports odd numbers for stroke_width - even numbers will be rounded up.
//! Minimal supported stroke_width is 3
T_STATIC void graphics_circle_draw_stroked_non_aa(GContext* ctx, GPoint p, uint16_t radius,
                                                  uint8_t stroke_width) {
  graphics_circle_quadrant_draw_stroked_non_aa(ctx, p, radius, stroke_width, GCornersAll);
}

// Lifted directly from: http://en.wikipedia.org/wiki/Midpoint_circle_algorithm
void graphics_draw_circle(GContext* ctx, GPoint p, uint16_t radius) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  if (radius == 0) {
    // Special case radius 0 to fill a circle with radius eqaul to half the stroke width
    // Backup the fill color and set that to the current stroke color since the fill color
    // is what is used for fill circle. Restore the fill color afterwards.
    GColor backup_fill_color = ctx->draw_state.fill_color;
    ctx->draw_state.fill_color = ctx->draw_state.stroke_color;
    graphics_fill_circle(ctx, p, ctx->draw_state.stroke_width / 2);
    ctx->draw_state.fill_color = backup_fill_color;
    return;
  }

#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    if (ctx->draw_state.stroke_width > 1) {
      // Antialiased and Stroke Width > 1
      graphics_circle_draw_stroked_aa(ctx, p, radius, ctx->draw_state.stroke_width);
      return;
    } else {
      // Antialiased and Stroke Width == 1
      graphics_circle_draw_1px_aa(ctx, p, radius);
      return;
    }
  }
#endif
  if (ctx->draw_state.stroke_width > 1) {
    // Non-Antialiased and Stroke Width > 1
    graphics_circle_draw_stroked_non_aa(ctx, p, radius, ctx->draw_state.stroke_width);
  } else {
    // Non-Antialiased and Stroke Width == 1
    graphics_circle_draw_1px_non_aa(ctx, p, radius);
  }
}

#if defined(PLATFORM_TINTIN)
NOINLINE // Save space on tintin
#else
ALWAYS_INLINE // Optimize for speed on other platforms
#endif
static void prv_fill_horizontal_line(GContext *ctx, GPoint p, int16_t width) {
  graphics_fill_rect(ctx, &(GRect) { .origin = p, .size = { width, 1 } });
}

void graphics_circle_quadrant_fill_non_aa(GContext* ctx, GPoint p, uint16_t radius,
                                          GCornerMask quadrant) {
  const int16_t x0 = p.x;
  const int16_t y0 = p.y;
  int f = 1 - radius;
  int ddF_x = 1;
  int ddF_y = -2 * radius;
  int x = 0;
  uint16_t y = radius;

  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }

    x++;
    ddF_x += 2;
    f += ddF_x;

    if (quadrant & GCornerBottomLeft) {
      if (x == 1) {
        prv_fill_horizontal_line(ctx, GPoint(x0 - radius, y0), radius + 1);
      }
      prv_fill_horizontal_line(ctx, GPoint(x0 - x, y0 + y), x + 1);
      prv_fill_horizontal_line(ctx, GPoint(x0 - y, y0 + x), y + 1);
    }

    if (quadrant & GCornerBottomRight) {
      if (x == 1) {
        prv_fill_horizontal_line(ctx, GPoint(x0, y0), radius + 1);
      }
      prv_fill_horizontal_line(ctx, GPoint(x0, y0 + y), x + 1);
      prv_fill_horizontal_line(ctx, GPoint(x0, y0 + x), y + 1);
    }

    if (quadrant & GCornerTopLeft) {
      if (x == 1) {
        prv_fill_horizontal_line(ctx, GPoint(x0 - radius, y0), radius + 1);
      }
      prv_fill_horizontal_line(ctx, GPoint(x0 - x, y0 - y), x + 1);
      prv_fill_horizontal_line(ctx, GPoint(x0 - y, y0 - x), y + 1);
    }

    if (quadrant & GCornerTopRight) {
      if (x == 1) {
        prv_fill_horizontal_line(ctx, GPoint(x0, y0), radius + 1);
      }
      prv_fill_horizontal_line(ctx, GPoint(x0, y0 - y), x + 1);
      prv_fill_horizontal_line(ctx, GPoint(x0, y0 - x), y + 1);
    }
  }
}

static void graphics_fill_half_circle(GContext* ctx, int x0, int y0, uint16_t radius,
                                      uint8_t section) {
  int f = 1 - radius;
  int ddF_x = 1;
  int ddF_y = -2 * radius;
  int x = 0;
  uint16_t y = radius;

  while (x < y) {
    if (f >= 0) {
      y--;
      ddF_y += 2;
      f += ddF_y;
    }

    x++;
    ddF_x += 2;
    f += ddF_x;

    // bottom
    if (section & GCornersBottom) {
      prv_fill_horizontal_line(ctx, GPoint(x0 - x, y0 + y), 2 * x + 1);
      prv_fill_horizontal_line(ctx, GPoint(x0 - y, y0 + x), 2 * y + 1);
    }

    // top
    if (section & GCornersTop) {
      prv_fill_horizontal_line(ctx, GPoint(x0 - x, y0 - y), 2 * x + 1);
      prv_fill_horizontal_line(ctx, GPoint(x0 - y, y0 - x), 2 * y + 1);
    }
  }
}

MOCKABLE void graphics_circle_fill_non_aa(GContext* ctx, GPoint p, uint16_t radius) {
  prv_fill_horizontal_line(ctx, GPoint(p.x - radius, p.y), 2 * radius + 1);
  graphics_fill_half_circle(ctx, p.x, p.y, radius, GCornersAll);
}

#if PBL_COLOR
MOCKABLE void graphics_internal_circle_quadrant_fill_aa(GContext* ctx, GPoint p, uint16_t radius,
                                                        GCornerMask quadrant) {
  // Radius cannot be smaller than 1
  PBL_ASSERTN(radius > 0);

  prv_fill_oval_quadrant(ctx, p, radius, radius, 0, 0, quadrant);
}
#endif // PBL_COLOR

// Returns x for f(x) = g(x) with f(x)=y and a line g(x) that goes through two given points a,b
static int16_t prv_intersection_between_horizontal_and_line(Fixed_S16_3 progress, GPointPrecise top,
                                                            GPointPrecise bottom) {
  if (bottom.y.raw_value - top.y.raw_value == 0) {
    return top.x.raw_value + (bottom.x.raw_value - top.x.raw_value);
  }

  return top.x.raw_value + (bottom.x.raw_value - top.x.raw_value) *
  (progress.raw_value - top.y.raw_value) /
  (bottom.y.raw_value - top.y.raw_value);
}

static void prv_swap_precise_points(GPointPrecise *p0, GPointPrecise *p1) {
  GPointPrecise tmp = *p0;
  *p0 = *p1;
  *p1 = tmp;
}

static void prv_draw_scanline_collision_points(GContext *ctx, int16_t y,
                                               int16_t left, int16_t right,
                                               int16_t starting_edge, int16_t ending_edge,
                                               bool ignore_close_angles) {
  if (starting_edge > ending_edge || (ignore_close_angles && starting_edge == ending_edge)) {
    // Two separate drawings...
    starting_edge = MAX(starting_edge, left);
    ending_edge = MIN(ending_edge, right);

    if (left <= ending_edge) {
      graphics_private_draw_horizontal_line(ctx, y,
                                            (Fixed_S16_3){.raw_value = left},
                                            (Fixed_S16_3){.raw_value = ending_edge -
                                              FIXED_S16_3_ONE.raw_value});
    }

    if (starting_edge <= right) {
      graphics_private_draw_horizontal_line(ctx, y,
                                            (Fixed_S16_3){.raw_value = starting_edge},
                                            (Fixed_S16_3){.raw_value = right -
                                              FIXED_S16_3_ONE.raw_value});
    }
  } else {
    starting_edge = (MAX(left, starting_edge));
    ending_edge = (MIN(right, ending_edge));

    if (starting_edge <= ending_edge) {
      graphics_private_draw_horizontal_line(ctx, y,
                                            (Fixed_S16_3){.raw_value = starting_edge},
                                            (Fixed_S16_3){.raw_value = ending_edge -
                                              FIXED_S16_3_ONE.raw_value});
    }
  }
}

static GCornerMask prv_get_full_quadrants(int8_t starting_quadrant, int8_t ending_quadrant) {
  GCornerMask quadrants_solid = GCornerNone;

  if (starting_quadrant >= ending_quadrant) {
    ending_quadrant += QUADRANTS_NUM;
  }

  for (int i = starting_quadrant + 1; i < ending_quadrant; i++) {
    quadrants_solid = quadrants_solid | radius_quadrants[i % QUADRANTS_NUM];
  }

  return quadrants_solid;
}

static void prv_get_angles_mask_edge(Fixed_S16_3 y, GPointPrecise center, GCornerMask quadrant,
                                     int16_t *top_edge, int16_t *bottom_edge,
                                     GPointPrecise top, GPointPrecise bottom) {
  // This function determines where scanline is in position to the angle line and sets
  //   angle masking values accordingly
  if (quadrant & GCornersTop) {
    if (center.y.raw_value - y.raw_value <= bottom.y.raw_value) {
      if (center.y.raw_value - y.raw_value >= top.y.raw_value) {
        *top_edge = prv_intersection_between_horizontal_and_line((Fixed_S16_3){.raw_value =
            (center.y.raw_value - y.raw_value)}, top, bottom);
      } else {
        *top_edge = top.x.raw_value;
      }
    } else {
      *top_edge = bottom.x.raw_value;
    }
  } else {
    if (center.y.raw_value + y.raw_value >= top.y.raw_value) {
      if (center.y.raw_value + y.raw_value <= bottom.y.raw_value) {
        *bottom_edge = prv_intersection_between_horizontal_and_line((Fixed_S16_3){.raw_value =
            (center.y.raw_value + y.raw_value)}, top, bottom);
      } else {
        *bottom_edge = bottom.x.raw_value;
      }
    } else {
      *bottom_edge = top.x.raw_value;
    }
  }
}

T_STATIC EllipsisDrawConfig prv_calc_draw_config_ellipsis(int32_t angle_start, int32_t angle_end) {
  PBL_ASSERTN(angle_start <= angle_end);

  EllipsisDrawConfig config = (EllipsisDrawConfig){(EllipsisPartDrawConfig){0, GCornerNone},
    GCornerNone,
    (EllipsisPartDrawConfig){0, GCornerNone}};
  // Nothing to draw case:
  if (angle_end == angle_start) {
    return config;
  }

  // Full circle case:
  if (angle_end - angle_start >= TRIG_MAX_ANGLE) {
    config.full_quadrants = GCornersAll;
    return config;
  }

  int32_t angle_start_normalized = normalize_angle(angle_start);
  int32_t angle_end_normalized = normalize_angle(angle_end);

  int8_t starting_quadrant_normalized = (angle_start_normalized / QUADRANT_ANGLE) % 4;
  int8_t ending_quadrant_normalized = (angle_end_normalized / QUADRANT_ANGLE) % 4;

  if (starting_quadrant_normalized == ending_quadrant_normalized) {
    // Both indicate same quadrant...
    config.start_quadrant.angle = angle_start_normalized;
    config.start_quadrant.quadrant = radius_quadrants[starting_quadrant_normalized];
    config.end_quadrant.angle = angle_end_normalized;
    config.end_quadrant.quadrant = radius_quadrants[ending_quadrant_normalized];

    if (angle_end - angle_start > QUADRANT_ANGLE) {
      // Full quadrants:
      config.full_quadrants = prv_get_full_quadrants(starting_quadrant_normalized,
                                                     ending_quadrant_normalized);
    }
  } else {
    // Angles in different quadrants
    config.start_quadrant.angle = angle_start_normalized;
    config.start_quadrant.quadrant = radius_quadrants[starting_quadrant_normalized];
    config.end_quadrant.angle = angle_end_normalized;
    config.end_quadrant.quadrant = radius_quadrants[ending_quadrant_normalized];

    if (angle_start % QUADRANT_ANGLE == 0) {
      starting_quadrant_normalized--;
    }

    // Full quadrants
    config.full_quadrants = prv_get_full_quadrants(starting_quadrant_normalized,
                                                   ending_quadrant_normalized);
  }

  return config;
}

static void prv_fill_oval_precise(GContext *ctx, GPointPrecise center,
                                  Fixed_S16_3 radius_outer_x, Fixed_S16_3 radius_outer_y,
                                  Fixed_S16_3 radius_inner_x, Fixed_S16_3 radius_inner_y,
                                  int32_t angle_start, int32_t angle_end) {

  // Drawing config calculation
  const EllipsisDrawConfig config = prv_calc_draw_config_ellipsis(angle_start, angle_end);

  // This flag will skip calculation of angles
  const bool is_full_circle = config.full_quadrants == GCornersAll;

  // This will indicate special line in the middle of the circle, when center of the circle
  //    lies in between lines
  const bool odd_line = (center.y.fraction == FIXED_S16_3_HALF.raw_value);

  // This will prevent rounding error from breaking the scanline when angles are on same side but
  //   in reversed order (scanline first hits end angle then hits start angle
  const bool ignore_close_angles = (angle_end - angle_start > (TRIG_MAX_ANGLE / 2));

  // Clipping insets to prevent negative values
  radius_inner_x.raw_value = MAX(radius_inner_x.raw_value, 0);
  radius_inner_y.raw_value = MAX(radius_inner_y.raw_value, 0);

  // This flag prevents from calculation of the inner circle (and bugs related to it)
  const bool no_innner_ellipsis = (radius_inner_x.raw_value == 0 || radius_inner_y.raw_value == 0);

  // Squared radiuses values - they're used a lot in some cases
  const uint32_t radius_outer_x_sq =
      (radius_outer_x.raw_value * radius_outer_x.raw_value) >> FIXED_S16_3_PRECISION;
  const uint32_t radius_outer_y_sq =
      (radius_outer_y.raw_value * radius_outer_y.raw_value) >> FIXED_S16_3_PRECISION;
  const uint32_t radius_inner_x_sq =
      (radius_inner_x.raw_value * radius_inner_x.raw_value) >> FIXED_S16_3_PRECISION;
  const uint32_t radius_inner_y_sq =
      (radius_inner_y.raw_value * radius_inner_y.raw_value) >> FIXED_S16_3_PRECISION;

  // Intersection points of angles and radiuses
  GPointPrecise start_top =
      prv_get_rotated_precise_point_for_ellipsis(center,
                                                 radius_outer_x.raw_value, radius_outer_y.raw_value,
                                                 config.start_quadrant.angle);
  GPointPrecise end_top =
      prv_get_rotated_precise_point_for_ellipsis(center,
                                                 radius_outer_x.raw_value, radius_outer_y.raw_value,
                                                 config.end_quadrant.angle);

  GPointPrecise start_bottom = (no_innner_ellipsis) ? center :
      prv_get_rotated_precise_point_for_ellipsis(center,
                                                 radius_inner_x.raw_value, radius_inner_y.raw_value,
                                                 config.start_quadrant.angle);
  GPointPrecise end_bottom = (no_innner_ellipsis) ? center :
      prv_get_rotated_precise_point_for_ellipsis(center,
                                                 radius_inner_x.raw_value, radius_inner_y.raw_value,
                                                 config.end_quadrant.angle);

  // Swapping top/bottom offset points if neccesary
  if (start_top.y.raw_value > start_bottom.y.raw_value) {
    prv_swap_precise_points(&start_top, &start_bottom);
  } else if (start_top.y.raw_value == start_bottom.y.raw_value &&
             (config.start_quadrant.quadrant & GCornersBottom)) {
    // Special case to make bottom edge to be on left side and keep masking algorithm happy
    prv_swap_precise_points(&start_top, &start_bottom);
  }

  if (end_top.y.raw_value > end_bottom.y.raw_value) {
    prv_swap_precise_points(&end_top, &end_bottom);
  } else if (end_top.y.raw_value == end_bottom.y.raw_value &&
             (config.end_quadrant.quadrant & GCornersBottom)) {
    // Special case to make bottom edge to be on left side and keep masking algorithm happy
    prv_swap_precise_points(&end_top, &end_bottom);
  }

  // Range for scanline, since scanlines are mirred from the middle of the circle this is also
  //   indicated from the middle, therefore initialised with 0 (as middle) and
  //   radius_y (as scalines are on y axis)
  int draw_min = 0;
  int draw_max = radius_outer_y.integer;

  // Adjust to drawing_box offset
  int adjusted_center = center.y.integer + ctx->draw_state.drawing_box.origin.y;
  // We add one to compenaste in case of odd line needs to be drawn
  int adjusted_top = adjusted_center - radius_outer_y.integer - 1;
  int adjusted_bottom = adjusted_center + radius_outer_y.integer + 1;

  // Clip_box
  GRect clip_box = ctx->draw_state.clip_box;

  // Clip adjusted values by clip_box
  adjusted_top = MAX(adjusted_top, clip_box.origin.y);
  adjusted_top = MIN(adjusted_top, clip_box.origin.y + clip_box.size.h);
  adjusted_bottom = MAX(adjusted_bottom, clip_box.origin.y);
  adjusted_bottom = MIN(adjusted_bottom, clip_box.origin.y + clip_box.size.h);

  // Remove offset from adjusted values
  adjusted_top -= ctx->draw_state.drawing_box.origin.y;
  adjusted_bottom -= ctx->draw_state.drawing_box.origin.y;

  // Calculate distances from the middle of the circle (discard negative values)
  int draw_max_top = MAX(center.y.integer - adjusted_top, 0);
  int draw_max_bottom = MAX(adjusted_bottom - center.y.integer, 0);
  int draw_min_top = MAX(center.y.integer - adjusted_bottom, 0);
  // In case of odd line, center is with half pixel so we have to subtract one more more full line
  int draw_min_bottom = MAX(adjusted_top - center.y.integer - 1, 0);

  // Apply clipped distances
  draw_max = MIN(draw_max, MAX(draw_max_top, draw_max_bottom));
  draw_min = MAX(draw_min, MAX(draw_min_top, draw_min_bottom));

  // Scanline offset in precise point for calculation of edges
  Fixed_S16_3 y = (Fixed_S16_3){.integer = draw_min};

  // Flags used for filling of solid parts of the circle, when angles are not intersecting them
  const bool draw_top = is_full_circle || (config.full_quadrants & GCornersTop ||
                                           config.start_quadrant.quadrant & GCornersTop ||
                                           config.end_quadrant.quadrant & GCornersTop);
  const bool draw_bottom = is_full_circle || (config.full_quadrants & GCornersBottom ||
                                              config.start_quadrant.quadrant & GCornersBottom ||
                                              (config.end_quadrant.quadrant & GCornersBottom &&
                                               config.end_quadrant.angle % QUADRANT_ANGLE != 0));

  // Offsets for mirroring of scanline (they differ for even and odd height of the circle)
  int special_line_offset_top = 1;
  int special_line_offset_bottom = 1;

  // Color hack
  const GColor stroke_color = ctx->draw_state.stroke_color;
  ctx->draw_state.stroke_color = ctx->draw_state.fill_color;

  // In case of odd line in the middle, this is where we draw it:
  if (odd_line) {
    int16_t starting_edge = center.x.raw_value - radius_outer_x.raw_value;
    int16_t ending_edge = center.x.raw_value + radius_outer_x.raw_value;

    if (!is_full_circle) {
      // Following code finds crossing points slightly above and below center point
      //   as the radial angles will appear only with one of these cases, other
      //   will return same starting/ending point, which might be invalid angle
      // We might also reject this data if delta of start/end angle is bigger than 180°
      Fixed_S16_3 y_middle = (Fixed_S16_3){.raw_value = y.raw_value - 4};
      prv_get_angles_mask_edge(y_middle, center, config.start_quadrant.quadrant,
                               &starting_edge, &ending_edge, start_top, start_bottom);
      prv_get_angles_mask_edge(y_middle, center, config.end_quadrant.quadrant,
                               &ending_edge, &starting_edge, end_top, end_bottom);

      // Now we use only one with valuable data - same starting/ending is rejected
      if (starting_edge == ending_edge) {
        y_middle.integer++;

        prv_get_angles_mask_edge(y_middle, center, config.start_quadrant.quadrant,
                                 &starting_edge, &ending_edge, start_top, start_bottom);
        prv_get_angles_mask_edge(y_middle, center, config.end_quadrant.quadrant,
                                 &ending_edge, &starting_edge, end_top, end_bottom);
      }
    }

    int16_t outer_edge = prv_get_ellipsis_border(y, radius_outer_y_sq, radius_outer_x_sq).raw_value;
    int16_t left = center.x.raw_value - outer_edge;
    int16_t right = center.x.raw_value + outer_edge;

    if (!no_innner_ellipsis && radius_inner_y.integer != 0) {
      // This complicates the situation
      int16_t inner_edge =
          prv_get_ellipsis_border(y, radius_inner_y_sq, radius_inner_x_sq).raw_value;

      int16_t inner_left = center.x.raw_value - inner_edge;
      int16_t inner_right = center.x.raw_value + inner_edge;

      prv_draw_scanline_collision_points(ctx, center.y.integer, left, inner_left,
                                         starting_edge, ending_edge, ignore_close_angles);
      prv_draw_scanline_collision_points(ctx, center.y.integer, inner_right, right,
                                         starting_edge, ending_edge, ignore_close_angles);
    } else {
      prv_draw_scanline_collision_points(ctx, center.y.integer, left, right,
                                         starting_edge, ending_edge, ignore_close_angles);
    }

    // After drawing the line we move scanline edge calculation offset
    y.integer++;
  } else {
    // If theres no line in the middle, we move edge calculation offset by half (to stay
    //   in the middle of the pixel) and change bottom offset to zero (to evenly mirror lines)
    y.fraction = 4;
    special_line_offset_bottom = 0;
  }

  // Main drawing loop
  for (int i=draw_min; i < draw_max; i++, y.integer++) {
    // Separate min/max offsets for angles masking for lines drawn above and below center point
    int16_t top_starting_edge = center.x.raw_value - radius_outer_x.raw_value;
    int16_t top_ending_edge = center.x.raw_value + radius_outer_x.raw_value;
    int16_t bottom_starting_edge = top_starting_edge;
    int16_t bottom_ending_edge = top_ending_edge;

    // If the circle is not full - calculate mask for angles
    if (!is_full_circle) {
      prv_get_angles_mask_edge(y, center, config.start_quadrant.quadrant,
                               &top_starting_edge, &bottom_ending_edge, start_top, start_bottom);
      prv_get_angles_mask_edge(y, center, config.end_quadrant.quadrant,
                               &top_ending_edge, &bottom_starting_edge, end_top, end_bottom);
    }

    // Calculate outer radius edges
    int16_t outer_edge = prv_get_ellipsis_border(y, radius_outer_y_sq, radius_outer_x_sq).raw_value;
    int16_t left = center.x.raw_value - outer_edge;
    int16_t right = center.x.raw_value + outer_edge;

    // If theres circle in the middle - calculate it:
    if (!no_innner_ellipsis && i < radius_inner_y.integer) {
      int16_t inner_edge =
          prv_get_ellipsis_border(y, radius_inner_y_sq, radius_inner_x_sq).raw_value;

      int16_t inner_left = center.x.raw_value - inner_edge;
      int16_t inner_right = center.x.raw_value + inner_edge;

      // Using top/bottom flags we make sure to not draw outside of given angles range,
      //   we also draw separately left and right side of the circle
      if (draw_top) {
        prv_draw_scanline_collision_points(ctx, center.y.integer - i - special_line_offset_top,
                                           left, inner_left, top_starting_edge, top_ending_edge,
                                           ignore_close_angles);
        prv_draw_scanline_collision_points(ctx, center.y.integer - i - special_line_offset_top,
                                           inner_right, right, top_starting_edge, top_ending_edge,
                                           ignore_close_angles);
      }

      if (draw_bottom) {
        prv_draw_scanline_collision_points(ctx, center.y.integer + i + special_line_offset_bottom,
                                           left, inner_left, bottom_starting_edge,
                                           bottom_ending_edge, ignore_close_angles);
        prv_draw_scanline_collision_points(ctx, center.y.integer + i + special_line_offset_bottom,
                                           inner_right, right, bottom_starting_edge,
                                           bottom_ending_edge, ignore_close_angles);
      }
    } else {
      // If theres nothing in the middle, draw top and bottom parts of the circle
      if (draw_top) {
        prv_draw_scanline_collision_points(ctx, center.y.integer - i - special_line_offset_top,
                                           left, right, top_starting_edge, top_ending_edge,
                                           ignore_close_angles);
      }

      if (draw_bottom) {
        prv_draw_scanline_collision_points(ctx, center.y.integer + i + special_line_offset_bottom,
                                           left, right, bottom_starting_edge, bottom_ending_edge,
                                           ignore_close_angles);
      }
    }
  }

  // Finish color hack
  ctx->draw_state.stroke_color = stroke_color;
}

void prv_fill_oval_in_rect(GContext *ctx, GRect rect, uint16_t inset_x, uint16_t inset_y,
                           int32_t angle_start, int32_t angle_end) {
  // Cast to precision point
  GPointPrecise center = GPointPrecise(((rect.origin.x << FIXED_S16_3_PRECISION) +
                                        (rect.size.w << FIXED_S16_3_PRECISION) / 2),
                                       ((rect.origin.y << FIXED_S16_3_PRECISION) +
                                        (rect.size.h << FIXED_S16_3_PRECISION) / 2));

  Fixed_S16_3 radius_outer_x = (Fixed_S16_3){.raw_value =
    ((rect.size.w << FIXED_S16_3_PRECISION) / 2)};
  Fixed_S16_3 radius_outer_y = (Fixed_S16_3){.raw_value =
    ((rect.size.h << FIXED_S16_3_PRECISION) / 2)};
  Fixed_S16_3 radius_inner_x = (Fixed_S16_3){.raw_value =
    (((rect.size.w - inset_x * 2) << FIXED_S16_3_PRECISION) / 2)};
  Fixed_S16_3 radius_inner_y = (Fixed_S16_3){.raw_value =
    (((rect.size.h - inset_y * 2) << FIXED_S16_3_PRECISION) / 2)};

  prv_fill_oval_precise(ctx, center, radius_outer_x, radius_outer_y,
                        radius_inner_x, radius_inner_y, angle_start, angle_end);
}

void prv_fill_oval(GContext *ctx, GPoint center, uint16_t outer_radius_x, uint16_t outer_radius_y,
                   uint16_t inner_radius_x, uint16_t inner_radius_y,
                   int32_t angle_start, int32_t angle_end) {
  const GPointPrecise center_precise = (GPointPrecise) {
    .x.integer = center.x, .x.fraction = FIXED_S16_3_HALF.raw_value,
    .y.integer = center.y, .y.fraction = FIXED_S16_3_HALF.raw_value,
  };

  const Fixed_S16_3 outer_x_precise = (Fixed_S16_3){
    .integer = outer_radius_x,
    .fraction = FIXED_S16_3_HALF.raw_value
  };
  const Fixed_S16_3 outer_y_precise = (Fixed_S16_3){
    .integer = outer_radius_y,
    .fraction = FIXED_S16_3_HALF.raw_value
  };
  const Fixed_S16_3 inner_x_precise = (Fixed_S16_3){
    .integer = inner_radius_x,
    .fraction = FIXED_S16_3_HALF.raw_value
  };
  const Fixed_S16_3 inner_y_precise = (Fixed_S16_3){
    .integer = inner_radius_y,
    .fraction = FIXED_S16_3_HALF.raw_value
  };

  prv_fill_oval_precise(ctx, center_precise, outer_x_precise, outer_y_precise,
                        inner_x_precise, inner_y_precise, angle_start, angle_end);
}

void prv_fill_oval_quadrant_precise(GContext *ctx, GPointPrecise point,
                                    Fixed_S16_3 outer_radius_x, Fixed_S16_3 outer_radius_y,
                                    Fixed_S16_3 inner_radius_x, Fixed_S16_3 inner_radius_y,
                                    GCornerMask quadrant) {
  // Translate quadrants to angles
  if (quadrant == GCornerNone) {
    return;
  }

  // Calculate quadrants...
  int32_t angle_start = 0;
  int32_t angle_end = 0;

  if (quadrant == GCornersAll) {
    angle_end = TRIG_MAX_ANGLE;
  } else {
    if (quadrant & radius_quadrants[0]) {
      for (int i=QUADRANTS_NUM - 1; i >= 0; i--) {
        if (!(quadrant & radius_quadrants[i])) {
          angle_start = ((i + 1) % 4) * (TRIG_MAX_ANGLE / 4);
          break;
        }
      }
    } else {
      for (int i=1; i <= QUADRANTS_NUM; i++) {
        if (quadrant & radius_quadrants[i % 4]) {
          angle_start = i * (TRIG_MAX_ANGLE / 4);
          break;
        }
      }
    }

    if (!(quadrant & radius_quadrants[0])) {
      for (int i=QUADRANTS_NUM - 1; i >= 0; i--) {
        if (quadrant & radius_quadrants[i]) {
          angle_end = ((i + 1) % 4) * (TRIG_MAX_ANGLE / 4);
          break;
        }
      }
    } else {
      for (int i=1; i <= QUADRANTS_NUM; i++) {
        if (!(quadrant & radius_quadrants[i % 4])) {
          angle_end = i * (TRIG_MAX_ANGLE / 4);
          break;
        }
      }
    }
  }

  if (angle_end <= angle_start) {
    angle_end += TRIG_MAX_ANGLE;
  }

  prv_fill_oval_precise(ctx, point, outer_radius_x, outer_radius_y,
                        inner_radius_x, inner_radius_y, angle_start, angle_end);
}

void prv_fill_oval_quadrant(GContext *ctx, GPoint point,
                            uint16_t outer_radius_x, uint16_t outer_radius_y,
                            uint16_t inner_radius_x, uint16_t inner_radius_y,
                            GCornerMask quadrant) {
  // Cast to precision point
  const GPointPrecise center_precise = (GPointPrecise) {
    .x.integer = point.x, .x.fraction = FIXED_S16_3_HALF.raw_value,
    .y.integer = point.y, .y.fraction = FIXED_S16_3_HALF.raw_value,
  };

  const Fixed_S16_3 outer_x_precise = (Fixed_S16_3){
    .integer = outer_radius_x,
    .fraction = FIXED_S16_3_HALF.raw_value
  };
  const Fixed_S16_3 outer_y_precise = (Fixed_S16_3){
    .integer = outer_radius_y,
    .fraction = FIXED_S16_3_HALF.raw_value
  };
  const Fixed_S16_3 inner_x_precise = (Fixed_S16_3){
    .integer = inner_radius_x,
    .fraction = FIXED_S16_3_HALF.raw_value
  };
  const Fixed_S16_3 inner_y_precise = (Fixed_S16_3){
    .integer = inner_radius_y,
    .fraction = FIXED_S16_3_HALF.raw_value
  };

  prv_fill_oval_quadrant_precise(ctx, center_precise, outer_x_precise, outer_y_precise,
                                 inner_x_precise, inner_y_precise, quadrant);
}

MOCKABLE void graphics_draw_arc_precise_internal(GContext *ctx, GPointPrecise center,
                                                 Fixed_S16_3 radius,
                                                 int32_t angle_start, int32_t angle_end) {
  uint16_t stroke_width = ctx->draw_state.stroke_width;

  // We accept only .0 and .5 precision for now:
  center.x.raw_value -= center.x.raw_value % (FIXED_S16_3_ONE.raw_value / 2);
  center.y.raw_value -= center.y.raw_value % (FIXED_S16_3_ONE.raw_value / 2);

  // To maintain compability we have to adjust from integral points where given point means
  //    center of the point
  center.x.raw_value += 4;
  center.y.raw_value += 4;
  radius.raw_value += 4;

  // Same for radius:
  radius.raw_value -= radius.raw_value % (FIXED_S16_3_ONE.raw_value / 2);

  if (stroke_width < 1 || angle_start > angle_end) {
    // Dont draw anything
    return;
  }
  // TODO: Adapt this to support new precision, for now this is faked by regular fill
  // This will be done with PBL-24777
/*
  else if (stroke_width == 1) {
    prv_circle_arc_draw_1px(ctx, center, radius, angle_start, angle_end);
    return;
  }
*/
  // Color hack to draw using stroke_color instead of fill_color
  GColor tmp_color = ctx->draw_state.fill_color;
  ctx->draw_state.fill_color = ctx->draw_state.stroke_color;

  Fixed_S16_3 half_stroke_width =
      (Fixed_S16_3){.raw_value = (ctx->draw_state.stroke_width << FIXED_S16_3_PRECISION) / 2};
  Fixed_S16_3 radius_inner =
      (Fixed_S16_3){.raw_value = MAX(0, radius.raw_value - half_stroke_width.raw_value)};
  Fixed_S16_3 radius_outer =
      (Fixed_S16_3){.raw_value = radius.raw_value + half_stroke_width.raw_value};

  if (radius_outer.integer > 0) {
    prv_fill_oval_precise(ctx, center, radius_outer, radius_outer, radius_inner, radius_inner,
                          angle_start, angle_end);

    if (half_stroke_width.integer >= 1) {
      GPointPrecise starting_point =
          prv_get_rotated_precise_point(center, radius.raw_value, angle_start);
      GPointPrecise ending_point =
          prv_get_rotated_precise_point(center, radius.raw_value, angle_end);

      prv_fill_oval_precise(ctx, starting_point, half_stroke_width, half_stroke_width,
                            FIXED_S16_3_ZERO, FIXED_S16_3_ZERO, 0, TRIG_MAX_ANGLE);
      prv_fill_oval_precise(ctx, ending_point, half_stroke_width, half_stroke_width,
                            FIXED_S16_3_ZERO, FIXED_S16_3_ZERO, 0, TRIG_MAX_ANGLE);
    }
  }

  // Restore color
  ctx->draw_state.fill_color = tmp_color;
}

void graphics_draw_arc_internal(GContext *ctx, GPoint center, uint16_t radius, int32_t angle_start,
                                int32_t angle_end) {
  // We're just casting this to precise points
  GPointPrecise fixed_center;

  // GPointPreciseFromGPoint doesnt work for unit tests (!!!)
  fixed_center.x.integer = center.x;
  fixed_center.y.integer = center.y;
  fixed_center.x.fraction = 0;
  fixed_center.y.fraction = 0;
  Fixed_S16_3 fixed_radius = (Fixed_S16_3){.integer = radius, .fraction = 0};

  graphics_draw_arc_precise_internal(ctx, fixed_center, fixed_radius, angle_start, angle_end);
}

void graphics_draw_arc(GContext *ctx, GRect rect, GOvalScaleMode scale_mode,
                       int32_t angle_start, int32_t angle_end) {
  GPointPrecise center;
  Fixed_S16_3 radius;
  grect_polar_calc_values(&rect, scale_mode, &center, &radius);
  graphics_draw_arc_precise_internal(ctx, center, radius, angle_start, angle_end);
}

MOCKABLE void graphics_fill_radial_precise_internal(GContext *ctx, GPointPrecise center,
                                                    Fixed_S16_3 radius_inner, Fixed_S16_3
                                                    radius_outer, int32_t angle_start,
                                                    int32_t angle_end) {
  // This function is going to be replaced with function that will support drawing of ellipsis
  //   as documented in PBL-23640

  // For now we only accept .0 and .5 radius precision
  radius_inner.raw_value -= radius_inner.raw_value % (FIXED_S16_3_ONE.raw_value / 2);
  radius_outer.raw_value -= radius_outer.raw_value % (FIXED_S16_3_ONE.raw_value / 2);

  // Same goes for coordinates of center point:
  center.x.raw_value -= center.x.raw_value % (FIXED_S16_3_ONE.raw_value / 2);
  center.y.raw_value -= center.y.raw_value % (FIXED_S16_3_ONE.raw_value / 2);

  // Move the values to match old precision with integral coordinate between pixels
  center.x.raw_value += 4;
  center.y.raw_value += 4;
  radius_inner.raw_value += 4;
  radius_outer.raw_value += 4;

  if (angle_start > angle_end || radius_outer.raw_value < radius_inner.raw_value) {
    // Nothing will be drawn...
    return;
  }

  // TODO: Adapt this to support new precision, for now this is faked by regular fill
  // This will be done with PBL-24777
/*
  if (radius_outer.raw_value - radius_inner.raw_value == FIXED_S16_3_ONE.raw_value) {
    // Color hack
    GColor tmp_color = ctx->draw_state.stroke_color;
    ctx->draw_state.stroke_color = ctx->draw_state.fill_color;

    // prv_circle_arc_draw_1px(ctx, center, radius_outer, angle_start, angle_end);

    // Restore color
    ctx->draw_state.stroke_color = tmp_color;

    // We're done here
    return;
  } else
*/
  if (radius_outer.raw_value - radius_inner.raw_value < FIXED_S16_3_ONE.raw_value) {
    // Abort, abort!
    return;
  }

  prv_fill_oval_precise(ctx, center, radius_outer, radius_outer,
                        radius_inner, radius_inner, angle_start, angle_end);

}

void graphics_fill_radial_internal(GContext *ctx, GPoint center, uint16_t radius_inner,
                                   uint16_t radius_outer, int32_t angle_start, int32_t angle_end) {
  // Cast given values to precise point
  GPointPrecise center_fixed;
  center_fixed.x.integer = center.x;
  center_fixed.y.integer = center.y;
  center_fixed.x.fraction = 0;
  center_fixed.y.fraction = 0;
  Fixed_S16_3 radius_inner_fixed = (Fixed_S16_3){.integer = radius_inner, .fraction = 0};
  Fixed_S16_3 radius_outer_fixed = (Fixed_S16_3){.integer = radius_outer, .fraction = 0};

  prv_fill_oval_precise(ctx, center_fixed, radius_outer_fixed, radius_outer_fixed,
                        radius_inner_fixed, radius_inner_fixed, angle_start, angle_end);
}

void graphics_fill_radial(GContext *ctx, GRect rect, GOvalScaleMode scale_mode,
                          uint16_t inset_thickness,
                          int32_t angle_start, int32_t angle_end) {
  GPointPrecise center;
  Fixed_S16_3 radius_outer;
  grect_polar_calc_values(&rect, scale_mode, &center, &radius_outer);
  const Fixed_S16_3 radius_inner =
    Fixed_S16_3(radius_outer.raw_value - inset_thickness * FIXED_S16_3_ONE.raw_value);
  graphics_fill_radial_precise_internal(ctx, center, radius_inner, radius_outer,
                                        angle_start, angle_end);
}

void graphics_fill_oval(GContext *ctx, GRect rect, GOvalScaleMode scale_mode) {
  const int32_t inset_thickness = MAX(ABS(rect.size.h), ABS(rect.size.w));
  // Fill radial doesn't mind overlarge inset thickness
  graphics_fill_radial(ctx, rect, scale_mode, inset_thickness, 0, TRIG_MAX_ANGLE);
}

void graphics_fill_circle(GContext* ctx, GPoint p, uint16_t radius) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  if (radius == 0) {
    // Filling a circle of radius zero should just draw a single pixel.
    // Backup the stroke color and set that to the current fill color since the stroke color
    // is what is used for draw pixel. Restore the stroke color afterwards.
    GColor backup_stroke_color = ctx->draw_state.stroke_color;
    ctx->draw_state.stroke_color = ctx->draw_state.fill_color;
    graphics_draw_pixel(ctx, p);
    ctx->draw_state.stroke_color = backup_stroke_color;
    return;
  }

#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    graphics_internal_circle_quadrant_fill_aa(ctx, p, radius, GCornersAll);
    return;
  }
#endif
  graphics_circle_fill_non_aa(ctx, p, radius);
}

GPointPrecise gpoint_from_polar_precise(const GPointPrecise *precise_center,
                                        uint16_t precise_radius, int32_t angle) {
  const uint32_t normalized_angle = normalize_angle(angle);
  const GPointPrecise precise_perimeter_point = prv_get_rotated_precise_point(*precise_center,
                                                                              precise_radius,
                                                                              normalized_angle);
  return precise_perimeter_point;
}

GPoint gpoint_from_polar_internal(const GPoint *center, uint16_t radius, int32_t angle) {
  if (!center) {
    return GPointZero;
  }

  const GPointPrecise precise_center = GPointPreciseFromGPoint((*center));
  const uint16_t precise_radius = (uint16_t)(radius << GPOINT_PRECISE_PRECISION);
  const GPointPrecise result = gpoint_from_polar_precise(&precise_center, precise_radius,
                                                             angle);
  return GPointFromGPointPrecise(result);
}


GPointPrecise prv_gpointprecise_from_polar(GRect *rect, GOvalScaleMode scale_mode,
                                                 int32_t angle) {
  GPointPrecise center;
  Fixed_S16_3 radius;
  grect_polar_calc_values(rect, scale_mode, &center, &radius);
  return gpoint_from_polar_precise(&center, radius.raw_value, angle);
}

GPoint gpoint_from_polar(GRect rect, GOvalScaleMode scale_mode, int32_t angle) {
  const GPointPrecise result = prv_gpointprecise_from_polar(&rect, scale_mode, angle);
  return GPointFromGPointPrecise(result);
}

GRect grect_centered_internal(const GPointPrecise *center, GSize size) {
  size.w = ABS(size.w);
  size.h = ABS(size.h);
  const int16_t FIXED_HALF = FIXED_S16_3_HALF.raw_value;
  return (GRect) {
    // Adding 0.5 to x and y here will ensure we have rounded up when we throw away the fraction
    .origin.x = (center->x.raw_value - (size.w * FIXED_HALF) + FIXED_HALF) >> FIXED_S16_3_PRECISION,
    .origin.y = (center->y.raw_value - (size.h * FIXED_HALF) + FIXED_HALF) >> FIXED_S16_3_PRECISION,
    .size = size,
  };
}

GRect grect_centered_from_polar(GRect rect, GOvalScaleMode scale_mode, int32_t angle, GSize size) {
  const GPointPrecise center = prv_gpointprecise_from_polar(&rect, scale_mode, angle);
  return grect_centered_internal(&center, size);
}

void grect_polar_calc_values(const GRect *r, GOvalScaleMode scale_mode, GPointPrecise *center,
                             Fixed_S16_3 *radius) {
  if (!r) {
    return;
  }

  GRect rect = *r;
  grect_standardize(&rect);

  const int16_t FIXED_ONE = FIXED_S16_3_ONE.raw_value;
  const int16_t FIXED_HALF = FIXED_S16_3_HALF.raw_value;

  if (radius) {
    int16_t side;
    switch (scale_mode) {
      case GOvalScaleModeFitCircle:
        side = grect_shortest_side(rect);
        break;
      case GOvalScaleModeFillCircle:
        side = grect_longest_side(rect);
        break;
      default:
        WTF;
    }
    const int16_t radius_value = ((side + 1) * FIXED_ONE) / 2 - FIXED_ONE;
    *radius = Fixed_S16_3(MAX(0, radius_value));
  }
  if (center) {
    // origin + (origin + len)/2 - 0.5 == (origin * 2 + len) / 2 - 0.5
    const int16_t x = (rect.size.w <= 0) ? rect.origin.x * FIXED_ONE :
        (((2 * rect.origin.x + rect.size.w) * FIXED_ONE) / 2 - FIXED_HALF);
    const int16_t y = (rect.size.h <= 0) ? rect.origin.y * FIXED_ONE :
                      (((2 * rect.origin.y + rect.size.h) * FIXED_ONE) / 2 - FIXED_HALF);
    *center = (GPointPrecise) {.x = Fixed_S16_3(x), .y = Fixed_S16_3(y)};
  }
}
