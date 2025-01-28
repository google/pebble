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

#include "graphics_line.h"
#include "graphics_private.h"
#include "graphics.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/swap.h"

#define MINIMUM_PRECISE_STROKE_WIDTH 2

// Precomputed lookup table with quadrant of the circle for the caps on stroked lines
// table of y-coordinates expressed as Fixed_S16_3.raw_value
// for each x-coordinate (array index) of first quadrant of unit circle
// see prv_calc_quadrant_lookup()
static const uint16_t s_circle_table[] = {
  8,
  16, 3,
  24, 7, 2,
  32, 11, 5, 2,
  40, 16, 8, 4, 1,
  48, 22, 13, 7, 3, 1,
  56, 28, 17, 11, 6, 3, 1,
  64, 34, 22, 15, 9, 5, 3, 1,
  72, 40, 27, 19, 13, 8, 5, 2, 1,
  80, 46, 32, 23, 16, 11, 7, 4, 2, 1,
  88, 52, 38, 28, 21, 15, 10, 7, 4, 2, 1,
  96, 58, 43, 33, 25, 19, 13, 9, 6, 4, 2, 1,
  104, 64, 49, 38, 29, 23, 17, 12, 8, 6, 3, 2, 1
};

MOCKABLE void graphics_line_draw_1px_non_aa(GContext* ctx, GPoint p0, GPoint p1) {
  p0.x += ctx->draw_state.drawing_box.origin.x;
  p1.x += ctx->draw_state.drawing_box.origin.x;
  p0.y += ctx->draw_state.drawing_box.origin.y;
  p1.y += ctx->draw_state.drawing_box.origin.y;

  int steep = abs(p1.y - p0.y) > abs(p1.x - p0.x);
  if (steep) {
    swap16(&p0.x, &p0.y);
    swap16(&p1.x, &p1.y);
  }

  if (p0.x > p1.x) {
    swap16(&p0.x, &p1.x);
    swap16(&p0.y, &p1.y);
  }

  int dx = p1.x - p0.x;
  int dy = abs(p1.y - p0.y);

  int16_t err = dx / 2;
  int16_t ystep;

  if (p0.y < p1.y) {
    ystep = 1;
  } else {
    ystep = -1;
  }

  for (; p0.x <= p1.x; p0.x++) {
    if (steep) {
      graphics_private_set_pixel(ctx, GPoint(p0.y, p0.x));
    } else {
      graphics_private_set_pixel(ctx, GPoint(p0.x, p0.y));
    }
    err -= dy;
    if (err < 0) {
      p0.y += ystep;
      err += dx;
    }
  }
}

#if PBL_COLOR
MOCKABLE void graphics_line_draw_1px_aa(GContext* ctx, GPoint p0, GPoint p1) {
  // Implementation of Wu-Xiang fast anti-aliased line drawing algorithm

  // Points over which we're going to iterate adjusted to drawing_box
  int16_t x1 = p0.x + ctx->draw_state.drawing_box.origin.x;
  int16_t y1 = p0.y + ctx->draw_state.drawing_box.origin.y;
  int16_t x2 = p1.x + ctx->draw_state.drawing_box.origin.x;
  int16_t y2 = p1.y + ctx->draw_state.drawing_box.origin.y;

  // Main loop helpers
  uint16_t intensity_shift, error_adj, error_acc;
  uint16_t error_acc_temp, weighting, weighting_complement_mask;
  int16_t dx, dy, tmp, xi;

  // Grabbing framebuffer for drawing and stroke color to blend
  GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);
  GColor stroke_color = ctx->draw_state.stroke_color;

  if (!framebuffer) {
    // Couldn't capture framebuffer
    return;
  }

  // Make sure the line runs top to bottom
  if (y1 > y2) {
    tmp = y1; y1 = y2; y2 = tmp;
    tmp = x1; x1 = x2; x2 = tmp;
  }

  // Draw the initial pixel
  // TODO: PBL-14743: Make a unit test that will test case of .frame.origin != {0,0}
  graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x1, y1, MAX_PLOT_OPACITY,
                              stroke_color);

  if ((dx = x2 - x1) >= 0) {
    xi = 1;
  } else {
    xi = -1;
    dx = -dx;
  }

  // If line is vertical, horizontal or diagonal we dont need to anti-alias it
  if ((dy = y2 - y1) == 0) {
    // Horizontal line
    int16_t start = x1;
    int16_t end = x1 + (dx * xi);

    if (end < start) {
      swap16(&start, &end);
    }

    graphics_private_draw_horizontal_line_prepared(ctx, framebuffer, &ctx->draw_state.clip_box, y1,
                                                   (Fixed_S16_3) {.integer = start},
                                                   (Fixed_S16_3) {.integer = end}, stroke_color);
  } else if (dx == 0) {
    // Vertical line
    graphics_private_draw_vertical_line_prepared(ctx, framebuffer, &ctx->draw_state.clip_box, x1,
                                                 (Fixed_S16_3){.integer = y1},
                                                 (Fixed_S16_3){.integer = y1 + dy}, stroke_color);
  } else if (dx == dy) {
    // Diagonal line
    while (dy-- != 0) {
      x1 += xi;
      y1++;
      graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x1, y1, MAX_PLOT_OPACITY,
                                  stroke_color);
    }
  } else {
    // Line is not horizontal, diagonal, or vertical

    // Error accumulator
    error_acc = 0;

    // # of bits by which to shift error_acc to get intensity level
    intensity_shift = 14;

    // Mask used to flip all bits in an intensity weighting
    // producing the result (1 - intensity weighting)
    weighting_complement_mask = MAX_PLOT_BRIGHTNESS;

    // Is this an X-major or Y-major line?
    if (dy > dx) {
      // Y-major line; calculate 16-bit fixed-point fractional part of a
      // pixel that X advances each time Y advances 1 pixel, truncating the
      // result so that we won't overrun the endpoint along the X axis
      error_adj = ((uint32_t)(dx) << 16) / (uint32_t) dy;

      // Draw all pixels other than the first and last
      while (--dy) {
        error_acc_temp = error_acc;
        error_acc += error_adj;
        if (error_acc <= error_acc_temp) {
          // The error accumulator turned over, so advance the X coord
          x1 += xi;
        }
        y1++;
        // The IntensityBits most significant bits of error_acc give us the
        // intensity weighting for this pixel, and the complement of the
        // weighting for the paired pixel
        weighting = error_acc >> intensity_shift;
        graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x1, y1, weighting,
                                    stroke_color);
        graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x1 + xi, y1,
                                    (weighting ^ weighting_complement_mask), stroke_color);
      }
      // Draw final pixel
      graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x2, y2, MAX_PLOT_OPACITY,
                                  stroke_color);
    } else {
      // It's an X-major line
      error_adj = ((uint32_t) dy << 16) / (uint32_t) dx;

      // Draw all pixels other than the first and last
      while (--dx) {
        error_acc_temp = error_acc;
        error_acc += error_adj;
        if (error_acc <= error_acc_temp) {
           // The error accumulator turned over, so advance the Y coord
           y1++;
        }
        x1 += xi;
        weighting = error_acc >> intensity_shift;
        graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x1, y1, weighting,
                                    stroke_color);
        graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x1, y1 + 1,
                                    (weighting ^ weighting_complement_mask), stroke_color);
      }
      // Draw the final pixel
      graphics_private_plot_pixel(framebuffer, &ctx->draw_state.clip_box, x2, y2, MAX_PLOT_OPACITY,
                                  stroke_color);
    }
  }

  // Release the framebuffer after we're done
  graphics_release_frame_buffer(ctx, framebuffer);
}
#endif // PBL_COLOR

static Fixed_S16_3 prv_get_circle_border_precise(int16_t y, uint16_t radius) {
  // This is so we operate in middle of the pixel, not on the edge
  y += FIXED_S16_3_ONE.raw_value / 2;

  return (Fixed_S16_3){.raw_value = radius - integer_sqrt(radius * radius - y * y)};
}

static void prv_calc_cap_prepared(Fixed_S16_3 cap_center, Fixed_S16_3 cap_center_offset,
          Fixed_S16_3 cap_radius, Fixed_S16_3 progress, Fixed_S16_3 *min, Fixed_S16_3 *max) {
  if (progress.raw_value >= cap_center.raw_value - cap_radius.raw_value &&
      progress.raw_value <= cap_center.raw_value + cap_radius.raw_value) {
    int16_t circle_min;
    int16_t circle_max;

    const int16_t p_offset = cap_center_offset.raw_value;
    const int16_t r8 = cap_radius.raw_value;

    if (progress.raw_value <= cap_center.raw_value) {
      // Top part of the circle
      Fixed_S16_3 lookup_val = prv_get_circle_border_precise(cap_center.raw_value -
                    progress.raw_value, cap_radius.raw_value + FIXED_S16_3_ONE.raw_value);

      circle_min = p_offset - r8 + lookup_val.raw_value;
      circle_max = p_offset + r8 - lookup_val.raw_value;
    } else {
      // Bottom part of the circle
      Fixed_S16_3 lookup_val = prv_get_circle_border_precise(progress.raw_value -
                    cap_center.raw_value, cap_radius.raw_value + FIXED_S16_3_ONE.raw_value);
      circle_min = p_offset - r8 + lookup_val.raw_value;
      circle_max = p_offset + r8 - lookup_val.raw_value;
    }

    min->raw_value = MIN(min->raw_value, circle_min);
    max->raw_value = MAX(max->raw_value, circle_max);
  }
}

static void prv_calc_cap_horiz(GPointPrecise *line_end_point, Fixed_S16_3 cap_radius,
                        int16_t y, Fixed_S16_3 *left_margin, Fixed_S16_3 *right_margin) {
  // This function will calculate edges of the cap for stroked line using horizontal lines
  Fixed_S16_3 progress = (Fixed_S16_3){.integer = y};

  prv_calc_cap_prepared(line_end_point->y, line_end_point->x,
                        cap_radius, progress, left_margin, right_margin);
}

static void prv_calc_cap_vert(GPointPrecise *line_end_point, Fixed_S16_3 cap_radius,
                       int16_t x, Fixed_S16_3 *top_margin, Fixed_S16_3 *bottom_margin) {
  // This function will calculate edges of the cap for stroked line using vertical lines
  Fixed_S16_3 progress = (Fixed_S16_3){.integer = x};

  prv_calc_cap_prepared(line_end_point->x, line_end_point->y,
                        cap_radius, progress, top_margin, bottom_margin);
}

// TODO: test me
static void prv_calc_quadrant_lookup(Fixed_S16_3 lookup[], uint8_t radius) {
  int n = ((radius - 1) * radius) / 2;

  for (int i=0; i < radius; i++) {
    lookup[i].raw_value = s_circle_table[n + i];
  }
}

// Finds edge points of the rectangle and returns true if line is vertically dominant
static bool prv_calc_far_points(GPointPrecise *p0, GPointPrecise *p1, Fixed_S16_3 radius,
                     GPointPrecise *far_top, GPointPrecise *far_bottom,
                     GPointPrecise *far_left, GPointPrecise *far_right) {
  // Increase precision for square root function so we wont lose results when p0 and p1
  //   are closer to each other than 1px on screen
  const int64_t fixed_precision = 4;

  // Delta for the orthogonal vector - its rotated by 90 degrees so we swap x/y
  // Those values are multiplied by sqrt_precision which later would be removed in line 297/298
  const int64_t dx_fixed = ((*p1).y.raw_value - (*p0).y.raw_value) * fixed_precision;
  const int64_t dy_fixed = ((*p0).x.raw_value - (*p1).x.raw_value) * fixed_precision;

  // Length of the line for orthogonal vector normalization
  const int32_t length_fixed = integer_sqrt(dx_fixed * dx_fixed + dy_fixed * dy_fixed);

  if (length_fixed == 0) {
    // In this case we skip middle part of the stroke to avoid division by zero
    GPointPrecise point;
    point.x.raw_value = (*p0).x.raw_value;
    point.y.raw_value = (*p0).y.raw_value;

    (*far_top) = point;
    (*far_bottom) = point;
    (*far_left) = point;
    (*far_right) = point;

    return false;
  }

  // Orthogonal vector for offset points
  GPointPrecise v1;
  v1.x.raw_value = (dx_fixed * radius.raw_value) / length_fixed;
  v1.y.raw_value = (dy_fixed * radius.raw_value) / length_fixed;

  // Calculate main body offset points
  GPointPrecise points[4];
  points[0].x.raw_value = (*p0).x.raw_value + v1.x.raw_value;
  points[0].y.raw_value = (*p0).y.raw_value + v1.y.raw_value;
  points[1].x.raw_value = (*p0).x.raw_value - v1.x.raw_value;
  points[1].y.raw_value = (*p0).y.raw_value - v1.y.raw_value;
  points[2].x.raw_value = (*p1).x.raw_value + v1.x.raw_value;
  points[2].y.raw_value = (*p1).y.raw_value + v1.y.raw_value;
  points[3].x.raw_value = (*p1).x.raw_value - v1.x.raw_value;
  points[3].y.raw_value = (*p1).y.raw_value - v1.y.raw_value;

  /* Finding out positions fo the points relatively to main body rectangle
   * Hardcoded approach since this is faster than extra logic for edge cases
   *
   * Example case:
   *
   *                . far_top
   *                 \
   *                 /\
   *                /  '  far_right
   *   far_left .  /
   *             \/
   *              \
   *               ' far_bottom
   */
  if (dx_fixed > 0) {
    if (dy_fixed > 0) {
      // Line heading down left
      (*far_top) = points[1];
      (*far_bottom) = points[2];
      (*far_left) = points[3];
      (*far_right) = points[0];
    } else {
      // Line heading down right
      (*far_top) = points[0];
      (*far_bottom) = points[3];
      (*far_left) = points[1];
      (*far_right) = points[2];
    }
  } else {
    if (dy_fixed > 0) {
      // Line heading up left
      (*far_top) = points[3];
      (*far_bottom) = points[0];
      (*far_left) = points[2];
      (*far_right) = points[1];
    } else {
      // Line heading up right
      (*far_top) = points[2];
      (*far_bottom) = points[1];
      (*far_left) = points[0];
      (*far_right) = points[3];
    }
  }

  // Since we already rotated the vector by 90 degrees, delta x is actually delta y
  // therefore if x is bigger than y we have vertical dominance
  if (ABS(dx_fixed) > ABS(dy_fixed)) {
    return true;
  }

  return false;
}

void prv_draw_stroked_line_precise(GContext* ctx, GPointPrecise p0, GPointPrecise p1,
                                    uint8_t width) {
  // This function will draw thick line on the screen using following technique:
  // - calculate offset points of the line
  // - calculate margin for the round caps at the end of the line
  // - proceed to fill stroke line by 1px lines vertically or horizontally based on steepness
  //   + find the right/top most edge by checking caps and offset points
  //   + find the left/bottom most edge by checking caps and offset points
  //   + draw line between left/top most edge and right/bottom most edge

  // This algorithm doesn't handle width smaller than 2
  PBL_ASSERTN(width >= MINIMUM_PRECISE_STROKE_WIDTH);

  Fixed_S16_3 radius = (Fixed_S16_3){.raw_value = ((width - 1) * FIXED_S16_3_ONE.raw_value) / 2};

  // Check if the line is in fact point and lies exactly on the pixel
  if (p0.x.raw_value == p1.x.raw_value && p0.y.raw_value == p1.y.raw_value &&
        p0.x.fraction == 0 && p0.y.fraction == 0) {
    // Color hack
    const GColor temp_color = ctx->draw_state.fill_color;

    ctx->draw_state.fill_color = ctx->draw_state.stroke_color;

    // If so, draw a circle with corresponding radius
    graphics_fill_circle(ctx, GPoint(p0.x.integer, p0.y.integer), radius.integer);

    // Finish color hack
    ctx->draw_state.fill_color = temp_color;

    // Return without drawing the line since its not necessary
    return;
  }

  GPointPrecise far_top;
  GPointPrecise far_bottom;
  GPointPrecise far_left;
  GPointPrecise far_right;

  bool vertical = prv_calc_far_points(&p0, &p1, radius,
                                      &far_top, &far_bottom,
                                      &far_left, &far_right);

  // To compensate for rounding errors we need to add half of the precision in specific places
  //   - we add on top if line is leaning backward
  //   - we add on bottom if line is leaning forward
  //   - for lines with perfect horizontal or vertical lines this fix doesnt matter
  //       same applies to same starting/ending points
  bool delta_x_is_positive = ((p1.x.raw_value - p0.x.raw_value) >= 0);
  bool delta_y_is_positive = ((p1.y.raw_value - p0.y.raw_value) >= 0);
  bool add_on_top = (delta_x_is_positive == delta_y_is_positive);

  uint8_t add_top = (add_on_top)? (FIXED_S16_3_ONE.raw_value / 2) : 0;
  uint8_t add_bottom = (!add_on_top)? (FIXED_S16_3_ONE.raw_value / 2) : 0;

  const int8_t fraction_mask = 0x7;

  if (vertical) {
    // Left and right most point helpers for main loop
    GPointPrecise lm_p0 = far_top;
    GPointPrecise lm_p1 = far_left;
    GPointPrecise rm_p0 = far_top;
    GPointPrecise rm_p1 = far_right;

    const int16_t top_point = MIN(p0.y.raw_value, p1.y.raw_value) - radius.raw_value;
    const int16_t bottom_point = MAX(p0.y.raw_value, p1.y.raw_value) + radius.raw_value;

    const int8_t fraction_for_top = top_point & fraction_mask;
    const int8_t fraction_for_bottom = bottom_point & fraction_mask;

    // Drawing loop: Iterates over horizontal lines
    // As part of optimisation, this algorithm is moving between drawing boundaries,
    // so drawing box has to be substracted from its clipping extremes
    const int16_t clip_min_y = ctx->draw_state.clip_box.origin.y
                               - ctx->draw_state.drawing_box.origin.y;
    const int16_t clip_max_y = clip_min_y + ctx->draw_state.clip_box.size.h;
    const int16_t y_min = CLIP(top_point >> FIXED_S16_3_PRECISION, clip_min_y, clip_max_y);
    const int16_t y_max = CLIP(bottom_point >> FIXED_S16_3_PRECISION, clip_min_y, clip_max_y);

    // Blending of first line
    if (fraction_for_top != 0) {
      int16_t y = y_min;

      if (y > lm_p1.y.integer) {
        // We're crossing far_left point, time to swap...
        lm_p0 = far_left;
        lm_p1 = far_bottom;
      }

      if (y > rm_p1.y.integer) {
        // We're crossing far_right point, time to swap...
        rm_p0 = far_right;
        rm_p1 = far_bottom;
      }

      // Starting and ending point of the line, initialized with extremes
      Fixed_S16_3 left_margin = {.raw_value = INT16_MAX};
      Fixed_S16_3 right_margin = {.raw_value = INT16_MIN};

      // Find edges for upper cap
      GPointPrecise top_point_tmp = (p0.y.raw_value < p1.y.raw_value) ? p0 : p1;
      Fixed_S16_3 progress_line = (Fixed_S16_3){.raw_value = (y * FIXED_S16_3_ONE.raw_value +
                                                              FIXED_S16_3_ONE.raw_value / 2)};
      prv_calc_cap_prepared(top_point_tmp.y, top_point_tmp.x, radius,
                            progress_line, &left_margin, &right_margin);

      // Finally draw line
      if (left_margin.raw_value <= right_margin.raw_value) {
        graphics_private_plot_horizontal_line(ctx, y, left_margin, right_margin,
                                              (fraction_for_top >> 1));
      }
    }

    for (int16_t y = (fraction_for_top ? y_min + 1 : y_min); y <= y_max; y++) {
      if (y > lm_p1.y.integer) {
        // We're crossing far_left point, time to swap...
        lm_p0 = far_left;
        lm_p1 = far_bottom;
      }

      if (y > rm_p1.y.integer) {
        // We're crossing far_right point, time to swap...
        rm_p0 = far_right;
        rm_p1 = far_bottom;
      }

      // Starting and ending point of the line, initialized with extremes
      Fixed_S16_3 left_margin = {.raw_value = INT16_MAX};
      Fixed_S16_3 right_margin = {.raw_value = INT16_MIN};

      // Find edges of the line's straigth part
      if (y >= far_top.y.integer && y <= far_bottom.y.integer) {
        // TODO: possible performance optimization: PBL-14744
        // TODO: ^^ also possible avoid of following logic to avoid division by zero
        // Main part of the stroked line
        if (lm_p1.y.raw_value != lm_p0.y.raw_value) {
          left_margin.raw_value = lm_p0.x.raw_value + ((lm_p1.x.raw_value - lm_p0.x.raw_value)
            * (y - ((lm_p0.y.raw_value + add_top) / FIXED_S16_3_ONE.raw_value)))
            * FIXED_S16_3_ONE.raw_value / (lm_p1.y.raw_value - lm_p0.y.raw_value);
        } else {
          left_margin.raw_value = lm_p0.x.raw_value;
        }

        if (rm_p1.y.raw_value != rm_p0.y.raw_value) {
          right_margin.raw_value = rm_p0.x.raw_value + ((rm_p1.x.raw_value - rm_p0.x.raw_value)
            * (y - ((rm_p0.y.raw_value + add_bottom) / FIXED_S16_3_ONE.raw_value)))
            * FIXED_S16_3_ONE.raw_value / (rm_p1.y.raw_value - rm_p0.y.raw_value);
        } else {
          right_margin.raw_value = rm_p0.x.raw_value;
        }
      }

      // Find edges for both caps
      prv_calc_cap_horiz(&p0, radius, y, &left_margin, &right_margin);
      prv_calc_cap_horiz(&p1, radius, y, &left_margin, &right_margin);

      // Finally draw line
      if (left_margin.raw_value <= right_margin.raw_value) {
        graphics_private_draw_horizontal_line(ctx, y, left_margin, right_margin);
      }
    }

    // Blending of last line
    if (fraction_for_bottom != 0) {
      int16_t y = y_max + 1;

      // Starting and ending point of the line, initialized with extremes
      Fixed_S16_3 left_margin = {.raw_value = INT16_MAX};
      Fixed_S16_3 right_margin = {.raw_value = INT16_MIN};

      // Find edges for bottom cap
      GPointPrecise bottom_point_tmp = (p0.y.raw_value > p1.y.raw_value) ? p0 : p1;
      Fixed_S16_3 progress_line = (Fixed_S16_3){.raw_value = (y * FIXED_S16_3_ONE.raw_value -
                                                              FIXED_S16_3_ONE.raw_value / 2)};
      prv_calc_cap_prepared(bottom_point_tmp.y, bottom_point_tmp.x, radius,
                            progress_line, &left_margin, &right_margin);

      // Finally draw line
      if (left_margin.raw_value <= right_margin.raw_value) {
        graphics_private_plot_horizontal_line(ctx, y, left_margin, right_margin,
                                              (fraction_for_bottom >> 1));
      }
    }
  } else {
    // PBL-14798: refactor this.
    // Top and bottom most point helpers for main loop
    GPointPrecise tm_p0 = far_left;
    GPointPrecise tm_p1 = far_top;
    GPointPrecise bm_p0 = far_left;
    GPointPrecise bm_p1 = far_bottom;

    const int8_t fraction_for_left = (MIN(p0.x.raw_value, p1.x.raw_value) - radius.raw_value)
                                      & fraction_mask;
    const int8_t fraction_for_right = (MAX(p0.x.raw_value, p1.x.raw_value) + radius.raw_value)
                                       & fraction_mask;

    // Drawing loop: Iterates over vertical lines from left to right
    // As part of optimisation, this algorithm is moving between drawing boundaries,
    // so drawing box has to be substracted from its clipping extremes
    const int16_t clip_min_x = ctx->draw_state.clip_box.origin.x
                               - ctx->draw_state.drawing_box.origin.x;
    const int16_t clip_max_x = clip_min_x + ctx->draw_state.clip_box.size.w;
    const int16_t x_min = CLIP((MIN(p0.x.raw_value, p1.x.raw_value) - radius.raw_value)
                                >> FIXED_S16_3_PRECISION, clip_min_x, clip_max_x);
    const int16_t x_max = CLIP((MAX(p0.x.raw_value, p1.x.raw_value) + radius.raw_value)
                                >> FIXED_S16_3_PRECISION, clip_min_x, clip_max_x);

    // Blending of first line
    if (fraction_for_left != 0) {
      int16_t x = x_min;

      if (x > tm_p1.x.integer) {
        // We're crossing far_top point, time to swap...
        tm_p0 = far_top;
        tm_p1 = far_right;
      }

      if (x > bm_p1.x.integer) {
        // We're crossing far_bottom point, time to swap...
        bm_p0 = far_bottom;
        bm_p1 = far_right;
      }

      // Starting and ending point of the line, initialized with extremes
      Fixed_S16_3 top_margin = {.raw_value = INT16_MAX};
      Fixed_S16_3 bottom_margin = {.raw_value = INT16_MIN};

      // Find edges for left cap
      GPointPrecise left_point_tmp = (p0.y.raw_value < p1.y.raw_value) ? p0 : p1;
      Fixed_S16_3 progress_line = (Fixed_S16_3){.raw_value = (x * FIXED_S16_3_ONE.raw_value +
                                                              FIXED_S16_3_ONE.raw_value / 2)};
      prv_calc_cap_prepared(left_point_tmp.x, left_point_tmp.y, radius,
                            progress_line, &top_margin, &bottom_margin);

      // Finally draw line
      if (top_margin.raw_value <= bottom_margin.raw_value) {
        graphics_private_plot_vertical_line(ctx, x, top_margin, bottom_margin,
                                            (fraction_for_left >> 1));
      }
    }

    for (int16_t x = (fraction_for_left ? x_min + 1 : x_min); x <= x_max; x++) {
      if (x > tm_p1.x.integer) {
        // We're crossing far_top point, time to swap...
        tm_p0 = far_top;
        tm_p1 = far_right;
      }

      if (x > bm_p1.x.integer) {
        // We're crossing far_bottom point, time to swap...
        bm_p0 = far_bottom;
        bm_p1 = far_right;
      }

      // Starting and ending point of the line, initialized with extremes
      Fixed_S16_3 top_margin = {.raw_value = INT16_MAX};
      Fixed_S16_3 bottom_margin = {.raw_value = INT16_MIN};

      // Find edges of the line's straigth part
      if (x >= far_left.x.integer && x <= far_right.x.integer) {
        // Main part of the stroked line
        if (tm_p1.x.raw_value != tm_p0.x.raw_value) {
          top_margin.raw_value = tm_p0.y.raw_value + ((tm_p1.y.raw_value - tm_p0.y.raw_value)
              * (x - ((tm_p0.x.raw_value + add_top) / FIXED_S16_3_ONE.raw_value)))
              * FIXED_S16_3_ONE.raw_value / (tm_p1.x.raw_value - tm_p0.x.raw_value);
        } else {
          top_margin.raw_value = tm_p0.y.raw_value;
        }

        if (bm_p1.x.raw_value != bm_p0.x.raw_value) {
          bottom_margin.raw_value =
            bm_p0.y.raw_value + ((bm_p1.y.raw_value - bm_p0.y.raw_value)
              * (x - ((bm_p0.x.raw_value + add_bottom) / FIXED_S16_3_ONE.raw_value)))
              * FIXED_S16_3_ONE.raw_value / (bm_p1.x.raw_value - bm_p0.x.raw_value);
        } else {
          bottom_margin.raw_value = bm_p0.y.raw_value;
        }
      }

      // Find edges for both caps
      prv_calc_cap_vert(&p0, radius, x, &top_margin, &bottom_margin);
      prv_calc_cap_vert(&p1, radius, x, &top_margin, &bottom_margin);

      // Finally draw line
      if (top_margin.raw_value <= bottom_margin.raw_value) {
        graphics_private_draw_vertical_line(ctx, x, top_margin, bottom_margin);
      }
    }

    // Blending of last line
    if (fraction_for_right != 0) {
      int16_t x = x_max + 1;

      // Starting and ending point of the line, initialized with extremes
      Fixed_S16_3 top_margin = {.raw_value = INT16_MAX};
      Fixed_S16_3 bottom_margin = {.raw_value = INT16_MIN};

      // Find edges for right cap
      GPointPrecise right_point_tmp = (p0.x.raw_value > p1.x.raw_value) ? p0 : p1;
      Fixed_S16_3 progress_line = (Fixed_S16_3){.raw_value = (x * FIXED_S16_3_ONE.raw_value -
                                                              FIXED_S16_3_ONE.raw_value / 2)};
      prv_calc_cap_prepared(right_point_tmp.x, right_point_tmp.y, radius,
                            progress_line, &top_margin, &bottom_margin);

      // Finally draw line
      if (top_margin.raw_value <= bottom_margin.raw_value) {
        graphics_private_plot_vertical_line(ctx, x, top_margin, bottom_margin,
                                            (fraction_for_right >> 1));
      }
    }
  }
}

static void prv_adjust_stroked_line_width(uint8_t *width) {
  PBL_ASSERTN(*width >= MINIMUM_PRECISE_STROKE_WIDTH);

  if (*width % 2 == 0) {
    (*width)++;
  }
}

static void prv_draw_stroked_line_override_aa(GContext* ctx, GPointPrecise p0, GPointPrecise p1,
                                              uint8_t width, bool anti_aliased) {
#if PBL_COLOR
  // Force antialiasing setting
  bool temp_anti_aliased = ctx->draw_state.antialiased;
  ctx->draw_state.antialiased = anti_aliased;
#endif

  // Call graphics line draw function
  prv_draw_stroked_line_precise(ctx, p0, p1, width);

#if PBL_COLOR
  // Restore previous antialiasing setting
  ctx->draw_state.antialiased = temp_anti_aliased;
#endif
}

#if PBL_COLOR
MOCKABLE void graphics_line_draw_stroked_aa(GContext* ctx, GPoint p0, GPoint p1,
                                            uint8_t stroke_width) {
  prv_adjust_stroked_line_width(&stroke_width);
  prv_draw_stroked_line_override_aa(ctx, GPointPreciseFromGPoint(p0), GPointPreciseFromGPoint(p1),
                                    stroke_width, true);
}
#endif // PBL_COLOR

MOCKABLE void graphics_line_draw_stroked_non_aa(GContext* ctx, GPoint p0, GPoint p1,
                                                uint8_t stroke_width) {
  prv_adjust_stroked_line_width(&stroke_width);
  prv_draw_stroked_line_override_aa(ctx, GPointPreciseFromGPoint(p0), GPointPreciseFromGPoint(p1),
                                    stroke_width, false);
}

#if PBL_COLOR
MOCKABLE void graphics_line_draw_precise_stroked_aa(GContext* ctx, GPointPrecise p0,
                                                    GPointPrecise p1, uint8_t stroke_width) {
  prv_draw_stroked_line_override_aa(ctx, p0, p1, stroke_width, true);
}
#endif // PBL_COLOR

MOCKABLE void graphics_line_draw_precise_stroked_non_aa(GContext* ctx, GPointPrecise p0,
                                                        GPointPrecise p1, uint8_t stroke_width) {
  prv_draw_stroked_line_override_aa(ctx, p0, p1, stroke_width, false);
}

void graphics_line_draw_precise_stroked(GContext* ctx, GPointPrecise p0, GPointPrecise p1) {
  if (ctx->draw_state.stroke_width >= MINIMUM_PRECISE_STROKE_WIDTH) {
    prv_draw_stroked_line_precise(ctx, p0, p1, ctx->draw_state.stroke_width);
  } else {
    graphics_draw_line(ctx, GPointFromGPointPrecise(p0), GPointFromGPointPrecise(p1));
  }
}

void graphics_draw_line(GContext* ctx, GPoint p0, GPoint p1) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    if (ctx->draw_state.stroke_width > 1) {
      // Antialiased and Stroke Width > 1
      graphics_line_draw_stroked_aa(ctx, p0, p1, ctx->draw_state.stroke_width);
      return;
    } else {
      // Antialiased and Stroke Width == 1 (not suppported on 1-bit color)
      graphics_line_draw_1px_aa(ctx, p0, p1);
      return;
    }
  }
#endif
  if (ctx->draw_state.stroke_width > 1) {
    // Non-Antialiased and Stroke Width > 1
    graphics_line_draw_stroked_non_aa(ctx, p0, p1, ctx->draw_state.stroke_width);
  } else {
    // Non-Antialiased and Stroke Width == 1
    graphics_line_draw_1px_non_aa(ctx, p0, p1);
  }
}

static void prv_draw_dotted_line(GContext* ctx, GPoint p0, uint16_t length, bool vertical) {
  PBL_ASSERTN(ctx);
  if (ctx->lock || (length == 0)) {
    return;
  }

  // Even columns start at pixel 0, odd columns start at pixel 1
  //   0  1  2  3  4  5
  // 0 X     X     X
  // 1    X     X     X
  // 2 X     X     X
  // 3    X     X     X
  // 4 X     X     X
  // 5    X     X     X

  // absolute coordinate
  GPoint abs_point = gpoint_add(p0, ctx->draw_state.drawing_box.origin);
  // is first pixel even?
  bool even = (abs_point.x + abs_point.y) % 2 == 0;
  // direction to travel
  const GPoint delta = vertical ? GPoint(0, 1) : GPoint(1, 0);

  while (length >= 1) {
    if (even) {
      graphics_private_set_pixel(ctx, abs_point);
    }
    even = !even;
    gpoint_add_eq(&abs_point, delta);
    length--;
  }
}

void graphics_draw_vertical_line_dotted(GContext* ctx, GPoint p0, uint16_t length) {
  prv_draw_dotted_line(ctx, p0, length, true);
}

void graphics_draw_horizontal_line_dotted(GContext* ctx, GPoint p0, uint16_t length) {
  prv_draw_dotted_line(ctx, p0, length, false);
}
