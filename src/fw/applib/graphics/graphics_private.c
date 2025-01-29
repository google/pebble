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

#include "bitblt_private.h"
#include "graphics.h"
#include "graphics_private.h"
#include "gtypes.h"
#include "system/passert.h"
#include "util/bitset.h"
#include "util/math.h"

// ## Point setting/blending functions

#if PBL_COLOR
T_STATIC inline void set_pixel_raw_8bit(GContext* ctx, GPoint point) {
  if (!grect_contains_point(&ctx->dest_bitmap.bounds, &point)) {
    return;
  }

  const GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(&ctx->dest_bitmap, point.y);
  if (!WITHIN(point.x, data_row_info.min_x, data_row_info.max_x)) {
    return;
  }
  uint8_t *line = data_row_info.data;
  GColor color = ctx->draw_state.stroke_color;
  if (!gcolor_is_transparent(color)) {
    // Force alpha to be opaque since that represents how framebuffer discards it in display.
    // Also needed for unit tests since PNG tests interpret alpha
    color.a = 3;
    line[point.x] = color.argb;
  }
}
#endif

#if PBL_BW
static inline void set_pixel_raw_2bit(GContext* ctx, GPoint point) {
  if (!grect_contains_point(&ctx->dest_bitmap.bounds, &point)) {
    return;
  }
  bool black = (gcolor_equal(ctx->draw_state.stroke_color, GColorBlack));

  uint8_t *line = ((uint8_t *)ctx->dest_bitmap.addr) + (ctx->dest_bitmap.row_size_bytes * point.y);
  bitset8_update(line, point.x, !black);
}
#endif

void graphics_private_set_pixel(GContext* ctx, GPoint point) {
  if (!grect_contains_point(&ctx->draw_state.clip_box, &point)) {
    return;
  }

#if PBL_BW
  set_pixel_raw_2bit(ctx, point);
#elif PBL_COLOR
  set_pixel_raw_8bit(ctx, point);
#endif

  const GRect dirty_rect = { point, { 1, 1 } };
  graphics_context_mark_dirty_rect(ctx, dirty_rect);
}

// ## Private blending wrapper functions for non-aa

uint32_t graphics_private_get_1bit_grayscale_pattern(GColor color, uint8_t row_number) {
  const GColor8Component luminance = (color.r + color.g + color.b) / 3;
  switch (luminance) {
    case 0:
      return 0x00000000;
    case 1:
    case 2:
      // This is done to create a checkerboard pattern for gray
      return (row_number % 2) ? 0xAAAAAAAA : 0x55555555;
    case 3:
      return 0xFFFFFFFF;
    default:
      WTF;
  }
}

void prv_assign_line_horizontal_non_aa(GContext* ctx, int16_t y, int16_t x1, int16_t x2) {
  y += ctx->draw_state.drawing_box.origin.y;
  x1 += ctx->draw_state.drawing_box.origin.x;
  x2 += ctx->draw_state.drawing_box.origin.x;

  // Clip results
  const int y_min = ctx->draw_state.clip_box.origin.y;
  const int y_max = grect_get_max_y(&ctx->draw_state.clip_box) - 1;
  const int x_min = ctx->draw_state.clip_box.origin.x;
  const int x_max = grect_get_max_x(&ctx->draw_state.clip_box) - 1;

  x1 = MAX(x1, x_min);
  x2 = MIN(x2, x_max);
  if (!WITHIN(y, y_min, y_max) || x1 > x2) {
    // Outside of drawing bounds..
    return;
  }

  // Capture framebuffer & pass it to drawing implementation
  GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);

  if (!framebuffer) {
    // Couldn't capture framebuffer
    return;
  }

  ctx->draw_state.draw_implementation->blend_horizontal_line(ctx, y, x1, x2,
                                                             ctx->draw_state.stroke_color);

  graphics_release_frame_buffer(ctx, framebuffer);
}

void prv_assign_line_vertical_non_aa(GContext* ctx, int16_t x, int16_t y1, int16_t y2) {
  x += ctx->draw_state.drawing_box.origin.x;
  y1 += ctx->draw_state.drawing_box.origin.y;
  y2 += ctx->draw_state.drawing_box.origin.y;

  // To preserve old behaviour we add one to the end of the line about to be drawn
  y2++;

  // Clip results
  const int y_min = ctx->draw_state.clip_box.origin.y;
  const int y_max = grect_get_max_y(&ctx->draw_state.clip_box) - 1;
  const int x_min = ctx->draw_state.clip_box.origin.x;
  const int x_max = grect_get_max_x(&ctx->draw_state.clip_box) - 1;

  y1 = MAX(y1, y_min);
  y2 = MIN(y2, y_max + 1); // Thats because we added one to end of the line
  if (!WITHIN(x, x_min, x_max) || y1 > y2) {
    // Outside of drawing bounds..
    return;
  }

  // Capture framebuffer & pass it to drawing implementation
  GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);

  if (!framebuffer) {
    // Couldn't capture framebuffer
    return;
  }

  ctx->draw_state.draw_implementation->blend_vertical_line(ctx, x, y1, y2,
                                                           ctx->draw_state.stroke_color);

  graphics_release_frame_buffer(ctx, framebuffer);
}

// ## Line blending wrappers:

void graphics_private_draw_horizontal_line_prepared(GContext *ctx, GBitmap *framebuffer,
                                                    GRect *clip_box, int16_t y, Fixed_S16_3 x1,
                                                    Fixed_S16_3 x2, GColor color) {
  if (gcolor_is_invisible(color)) {
    return;
  }

  // look for clipbox
  if (!WITHIN(y, clip_box->origin.y, grect_get_max_y(clip_box) - 1)) {
    return;
  }

  const int16_t min_valid_x = clip_box->origin.x;
  if (x1.integer < min_valid_x) {
    x1 = (Fixed_S16_3){.integer = min_valid_x, .fraction = 0};
  }

  const int16_t max_valid_x = grect_get_max_x(clip_box) - 1;
  if (x2.integer > max_valid_x) {
    x2 = (Fixed_S16_3){.integer = max_valid_x};
  }

  // last pixel with blending (don't render the pixel if it overflows the framebuffer/clip box)
  if (x2.integer >= max_valid_x) {
    x2.fraction = 0;
  }

  ctx->draw_state.draw_implementation->assign_horizontal_line(ctx, y, x1, x2, color);
}

void graphics_private_draw_horizontal_line_integral(GContext *ctx, GBitmap *framebuffer, int16_t y,
                                                    int16_t x1, int16_t x2, GColor color) {
  // This is a wrapper for prv_draw_horizontal_line_raw for integral coordinates

  // End of the line is inclusive so we subtract one
  x2--;

  const Fixed_S16_3 x1_fixed = Fixed_S16_3(x1 << FIXED_S16_3_PRECISION);
  const Fixed_S16_3 x2_fixed = Fixed_S16_3(x2 << FIXED_S16_3_PRECISION);

  ctx->draw_state.draw_implementation->assign_horizontal_line(ctx, y, x1_fixed, x2_fixed,
                                                              color);
}

void graphics_private_draw_vertical_line_prepared(GContext *ctx, GBitmap *framebuffer,
                                                  GRect *clip_box, int16_t x, Fixed_S16_3 y1,
                                                  Fixed_S16_3 y2, GColor color) {
  if (gcolor_is_invisible(color)) {
    return;
  }

  // look for clipbox
  if (!WITHIN(x, clip_box->origin.x, grect_get_max_x(clip_box) - 1)) {
    return;
  }

  const int16_t min_valid_y = clip_box->origin.y;
  if (y1.integer < min_valid_y) {
    y1 = (Fixed_S16_3){.integer = min_valid_y, .fraction = 0};
  }

  const int16_t max_valid_y = grect_get_max_y(clip_box) - 1;
  if (y2.integer > max_valid_y) {
    y2 = (Fixed_S16_3){.integer = max_valid_y};
  }

  if (y1.integer > y2.integer) {
    return;
  }

  // last pixel with blending (don't render the pixel if it overflows the framebuffer/clip box)
  if (y2.integer >= max_valid_y) {
    y2.fraction = 0;
  }

  ctx->draw_state.draw_implementation->assign_vertical_line(ctx, x, y1, y2, color);
}

void graphics_private_draw_horizontal_line(GContext *ctx, int16_t y, Fixed_S16_3 x1,
                                            Fixed_S16_3 x2) {
#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    // apply draw box and clipping
    x1.integer += ctx->draw_state.drawing_box.origin.x;
    x2.integer += ctx->draw_state.drawing_box.origin.x;
    y += ctx->draw_state.drawing_box.origin.y;
    GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);

    if (!framebuffer) {
      // Couldn't capture framebuffer
      return;
    }

    graphics_private_draw_horizontal_line_prepared(ctx, framebuffer, &ctx->draw_state.clip_box, y,
                                                   x1, x2, ctx->draw_state.stroke_color);

    graphics_release_frame_buffer(ctx, framebuffer);
    return;
  }
#endif // PBL_COLOR
  // since x1 is beginning of the line, rounding should work in favor of flooring the value
  //   therefore we substract one from the rounding addition to produce result similar to x2
  int16_t x1_rounded =
    (x1.raw_value + (FIXED_S16_3_ONE.raw_value / 2 - 1)) / FIXED_S16_3_ONE.raw_value;
  int16_t x2_rounded = (x2.raw_value + (FIXED_S16_3_ONE.raw_value / 2)) / FIXED_S16_3_ONE.raw_value;

  if (x1_rounded > x2_rounded) {
    // AA algorithm will draw lines in one way only, so non-AA should reject those too
    return;
  }

  prv_assign_line_horizontal_non_aa(ctx, y, x1_rounded, x2_rounded);
}

void graphics_private_draw_vertical_line(GContext *ctx, int16_t x, Fixed_S16_3 y1, Fixed_S16_3 y2) {
#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    // apply draw box and clipping
    y1.integer += ctx->draw_state.drawing_box.origin.y;
    y2.integer += ctx->draw_state.drawing_box.origin.y;
    x += ctx->draw_state.drawing_box.origin.x;
    GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);

    if (!framebuffer) {
      // Couldn't capture framebuffer
      return;
    }

    graphics_private_draw_vertical_line_prepared(ctx, framebuffer, &ctx->draw_state.clip_box, x, y1,
                                                 y2, ctx->draw_state.stroke_color);

    graphics_release_frame_buffer(ctx, framebuffer);
    return;
  }
#endif // PBL_COLOR
  // since y1 is beginning of the line, rounding should work in favor of flooring the value
  //   therefore we substract one from the rounding addition to produce result similar to y2
  int16_t y1_rounded =
    (y1.raw_value + (FIXED_S16_3_ONE.raw_value / 2 - 1)) / FIXED_S16_3_ONE.raw_value;
  int16_t y2_rounded = (y2.raw_value + (FIXED_S16_3_ONE.raw_value / 2)) / FIXED_S16_3_ONE.raw_value;

  if (y1_rounded > y2_rounded) {
    // AA algorithm will draw lines in one way only, so non-AA should reject those too
    return;
  }

  prv_assign_line_vertical_non_aa(ctx, x, y1_rounded, y2_rounded);
}

void graphics_private_plot_pixel(GBitmap *framebuffer, GRect *clip_box, int x, int y,
                                 uint16_t opacity, GColor color) {
  // Plots pixel directly to framebuffer
  // Pixel position have to be adjusted to drawing_box before calling this!

  // Checking for clip box
  const GPoint point = GPoint(x, y);
  if (!grect_contains_point(clip_box, &point)) {
    return;
  }

#if PBL_COLOR
  // Checking for data row min/max x
  const GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(framebuffer, y);
  if (!WITHIN(x, data_row_info.min_x, data_row_info.max_x)) {
    return;
  }
  GColor *output = (GColor *)(data_row_info.data + x);
  color.a = (uint8_t)(MAX_PLOT_BRIGHTNESS - opacity);
  output->argb = gcolor_alpha_blend(color, (*output)).argb;
#else
  if (opacity <= (MAX_PLOT_BRIGHTNESS / 2)) {
    bool black = (gcolor_equal(color, GColorBlack));
    uint8_t *line = ((uint8_t *)framebuffer->addr) + (framebuffer->row_size_bytes * y);
    bitset8_update(line, x, !black);
  }
#endif // PBL_COLOR
}

void graphics_private_plot_horizontal_line_prepared(GContext *ctx, GBitmap *framebuffer,
                                                    GRect *clip_box, int y, int x0, int x1,
                                                    uint16_t opacity, GColor color) {
  // Plots pixel directly to framebuffer
  // Pixel position have to be adjusted to drawing_box before calling this!

  // Checking for clip_box
  if (!WITHIN(y, clip_box->origin.y, grect_get_max_y(clip_box) - 1)) {
    return;
  }
  const int16_t x_min = MAX(MIN(x0, x1), clip_box->origin.x);
  const int16_t x_max = MIN(MAX(x0, x1), grect_get_max_x(clip_box));

#if PBL_COLOR
  color.a = (uint8_t)(MAX_PLOT_BRIGHTNESS - opacity);
#else
  if (opacity > (MAX_PLOT_BRIGHTNESS / 2)) {
    // We're not plotting anything, bail
    return;
  }
#endif // PBL_COLOR

  ctx->draw_state.draw_implementation->blend_horizontal_line(ctx, y, x_min, x_max, color);
}

void graphics_private_plot_vertical_line_prepared(GContext *ctx, GBitmap *framebuffer,
                                                  GRect *clip_box, int x, int y0, int y1,
                                                  uint16_t opacity, GColor color) {
  // Plots pixel directly to framebuffer
  // Pixel position have to be adjusted to drawing_box before calling this!

  // Checking for clip_box
  if (!WITHIN(x, clip_box->origin.x, grect_get_max_x(clip_box) - 1)) {
    return;
  }

  int16_t y_min = MAX(MIN(y0, y1), clip_box->origin.y);
  int16_t y_max = MIN(MAX(y0, y1), clip_box->origin.y + clip_box->size.h);

#if PBL_COLOR
  color.a = (uint8_t)(MAX_PLOT_BRIGHTNESS - opacity);
#else
  if (opacity > (MAX_PLOT_BRIGHTNESS / 2)) {
    // We're not plotting anything, bail
    return;
  }
#endif // PBL_COLOR

  ctx->draw_state.draw_implementation->blend_vertical_line(ctx, x, y_min, y_max, color);
}

void graphics_private_plot_horizontal_line(GContext *ctx, int16_t y, Fixed_S16_3 x1, Fixed_S16_3 x2,
                                           uint16_t opacity) {
#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    // apply draw box and clipping
    x1.integer += ctx->draw_state.drawing_box.origin.x;
    x2.integer += ctx->draw_state.drawing_box.origin.x;
    y += ctx->draw_state.drawing_box.origin.y;

    // round edges:
    x1.raw_value += (FIXED_S16_3_ONE.raw_value / 2);
    x2.raw_value += (FIXED_S16_3_ONE.raw_value / 2);
    if (x2.fraction > (opacity << 1)) {
      x2.integer++;
    }

    GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);

    if (!framebuffer) {
      // Couldn't capture framebuffer
      return;
    }

    graphics_private_plot_horizontal_line_prepared(ctx, framebuffer, &ctx->draw_state.clip_box, y,
                                                   x1.integer, x2.integer, opacity,
                                                   ctx->draw_state.stroke_color);

    graphics_release_frame_buffer(ctx, framebuffer);
    return;
  }
#endif // PBL_COLOR

  if (opacity <= (MAX_PLOT_BRIGHTNESS / 2)) {
    int16_t x1_rounded = (x1.raw_value + (FIXED_S16_3_ONE.raw_value / 2))
                          / FIXED_S16_3_ONE.raw_value;
    int16_t x2_rounded = (x2.raw_value + (FIXED_S16_3_ONE.raw_value / 2))
                          / FIXED_S16_3_ONE.raw_value;

    prv_assign_line_horizontal_non_aa(ctx, y, x1_rounded, x2_rounded);
  }
}

void graphics_private_plot_vertical_line(GContext *ctx, int16_t x, Fixed_S16_3 y1, Fixed_S16_3 y2,
                                         uint16_t opacity) {
#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    // apply draw box and clipping
    x += ctx->draw_state.drawing_box.origin.x;
    y1.integer += ctx->draw_state.drawing_box.origin.y;
    y2.integer += ctx->draw_state.drawing_box.origin.y;

    // round edges:
    y1.raw_value += (FIXED_S16_3_ONE.raw_value / 2);
    y2.raw_value += (FIXED_S16_3_ONE.raw_value / 2);
    if (y2.fraction > (opacity << 1)) {
      y2.integer++;
    }

    GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);

    if (!framebuffer) {
      // Couldn't capture framebuffer
      return;
    }

    graphics_private_plot_vertical_line_prepared(ctx, framebuffer, &ctx->draw_state.clip_box, x,
                                                 y1.integer, y2.integer, opacity,
                                                 ctx->draw_state.stroke_color);

    graphics_release_frame_buffer(ctx, framebuffer);
    return;
  }
#endif // PBL_COLOR

  if (opacity <= (MAX_PLOT_BRIGHTNESS / 2)) {
    int16_t y1_rounded = (y1.raw_value + (FIXED_S16_3_ONE.raw_value / 2))
                          / FIXED_S16_3_ONE.raw_value;
    int16_t y2_rounded = (y2.raw_value + (FIXED_S16_3_ONE.raw_value / 2))
                          / FIXED_S16_3_ONE.raw_value;

    prv_assign_line_vertical_non_aa(ctx, x, y1_rounded, y2_rounded);
  }
}

#if PBL_COLOR
void graphics_private_draw_horizontal_line_delta_prepared(GContext *ctx, GBitmap *framebuffer,
                                                          GRect *clip_box, int16_t y,
                                                          Fixed_S16_3 x1, Fixed_S16_3 x2,
                                                          Fixed_S16_3 delta1, Fixed_S16_3 delta2,
                                                          GColor color) {

  // Extended sides AA calculations
  uint8_t left_aa_offset = (delta1.integer > 1) ?
        ((delta1.raw_value + (FIXED_S16_3_ONE.raw_value / 2)) / FIXED_S16_3_ONE.raw_value) : 1;

  uint8_t right_aa_offset = (delta2.integer > 1) ?
        ((delta2.raw_value + (FIXED_S16_3_ONE.raw_value / 2)) / FIXED_S16_3_ONE.raw_value) : 1;

  x1.integer -= left_aa_offset / 2;
  x2.integer -= right_aa_offset / 2;

  // look for clipbox
  if (!WITHIN(y, clip_box->origin.y, grect_get_max_y(clip_box) - 1)) {
    return;
  }

  const int16_t min_valid_x = clip_box->origin.x;
  const int16_t max_valid_x = grect_get_max_x(clip_box) - 1;

  // x1/x2 clipping and verification happens in raw drawing function to preserve gradients

  ctx->draw_state.draw_implementation->assign_horizontal_line_delta(ctx, y, x1, x2,
                                                                    left_aa_offset, right_aa_offset,
                                                                    min_valid_x, max_valid_x,
                                                                    color);
}

void graphics_private_draw_horizontal_line_delta_aa(GContext *ctx, int16_t y, Fixed_S16_3 x1,
                                                    Fixed_S16_3 x2, Fixed_S16_3 delta1,
                                                    Fixed_S16_3 delta2) {
  // apply draw box and clipping
  x1.integer += ctx->draw_state.drawing_box.origin.x;
  x2.integer += ctx->draw_state.drawing_box.origin.x;
  y += ctx->draw_state.drawing_box.origin.y;
  GBitmap *framebuffer = graphics_capture_frame_buffer(ctx);

  if (!framebuffer) {
    // Couldn't capture framebuffer
    return;
  }

  graphics_private_draw_horizontal_line_delta_prepared(ctx, framebuffer, &ctx->draw_state.clip_box,
                                                       y, x1, x2, delta1, delta2,
                                                       ctx->draw_state.stroke_color);

  graphics_release_frame_buffer(ctx, framebuffer);
}
#endif // PBL_COLOR

void graphics_private_draw_horizontal_line_delta_non_aa(GContext *ctx, int16_t y, Fixed_S16_3 x1,
                                                    Fixed_S16_3 x2, Fixed_S16_3 delta1,
                                                    Fixed_S16_3 delta2) {
  int16_t x1_rounded = (x1.raw_value + (FIXED_S16_3_ONE.raw_value / 2)) / FIXED_S16_3_ONE.raw_value;
  int16_t x2_rounded = (x2.raw_value + (FIXED_S16_3_ONE.raw_value / 2)) / FIXED_S16_3_ONE.raw_value;

  if (x1_rounded > x2_rounded) {
    // AA algorithm will draw lines in one way only, so non-AA should reject those too
    return;
  }

  prv_assign_line_horizontal_non_aa(ctx, y, x1_rounded, x2_rounded);
}

// This function will replicate source column in given area
T_STATIC void prv_replicate_column_row_raw(GBitmap *framebuffer, int16_t src_x, int16_t dst_x1,
                                           int16_t dst_x2) {
  const GRect column_to_replicate = (GRect) {
    .origin = GPoint(src_x, framebuffer->bounds.origin.y),
    .size = GSize(1, framebuffer->bounds.size.h),
  };
  GBitmap column_to_replicate_sub_bitmap;
  gbitmap_init_as_sub_bitmap(&column_to_replicate_sub_bitmap, framebuffer, column_to_replicate);
  for (int16_t x = dst_x1; x <= dst_x2; x++) {
    bitblt_bitmap_into_bitmap(framebuffer, &column_to_replicate_sub_bitmap, GPoint(x, 0),
                              GCompOpAssign, GColorWhite);
  }
}

void graphics_patch_trace_of_moving_rect(GContext *ctx, int16_t *prev_x, GRect current) {
  const int16_t new_x = current.origin.x;
  int16_t src_x = 0; // just so that GCC accepts that src_x is always initialized
  int16_t dst_x1 = INT16_MAX;
  int16_t dst_x2 = INT16_MIN;
  if (*prev_x == INT16_MAX) {
    // do nothing
  } else if (*prev_x > new_x) {
    // move to left
    src_x = new_x + current.size.w - 1;
    dst_x1 = src_x + 1;
    dst_x2 = DISP_COLS - 1;
  } else if (*prev_x < new_x) {
    src_x = new_x;
    dst_x1 = 0;
    dst_x2 = src_x - 1;
  }

  *prev_x = new_x;

  if (dst_x1 > dst_x2) {
    return;
  }

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) {
    return;
  }

  prv_replicate_column_row_raw(fb, src_x, dst_x1, dst_x2);

  graphics_release_frame_buffer(ctx, fb);
}

void graphics_private_move_pixels_horizontally(GBitmap *bitmap, int16_t delta_x,
                                               bool patch_garbage) {
  if (!bitmap || delta_x == 0) {
    return;
  }

  const int bpp = gbitmap_get_bits_per_pixel(bitmap->info.format);

  const int16_t abs_delta = ABS(delta_x);
  const bool delta_neg = (delta_x < 0);
  const int16_t min_y = bitmap->bounds.origin.y;
  const int16_t max_y = grect_get_max_y(&bitmap->bounds) - 1;
  for (int16_t y = min_y; y <= max_y; y++) {
    const GBitmapDataRowInfo row_info = gbitmap_get_data_row_info(bitmap, y);
    const int16_t min_x = MAX(row_info.min_x, bitmap->bounds.origin.x);
    const int16_t max_x = MIN(row_info.max_x, grect_get_max_x(&bitmap->bounds) - 1);
    const int16_t num_pix_data_row = max_x - min_x + 1;
    const int16_t pixels_to_move = num_pix_data_row - abs_delta;
    switch (bpp) {
      case 1: {
        // Note: this doesn't care about the bounding, because we don't have any round 1bpp
        // devices to support, and it simplifies the code.
#if PBL_ROUND
        WTF;
#endif
        uint8_t *const buf = row_info.data;
        const int delta_bytes = abs_delta / 8;
        const int delta_bits = abs_delta % 8;
        // Subtract two bytes to account for the 16-bit padding at the end of each row
        const int bytes = bitmap->row_size_bytes - 2;

        uint8_t *const left_pixel = buf;
        uint8_t *const right_pixel = buf + delta_bytes;

        const uint8_t fill_byte = (delta_neg ? (buf[bytes - 1] & 0x80) : buf[0] & 1) ? 0xFF : 0;

        if (pixels_to_move <= 0) {
          // on this row, the delta is wider than the available pixels
          if (patch_garbage) {
            memset(left_pixel, fill_byte, bytes);
          }
          break;
        }

        uint8_t *const from = delta_neg ? right_pixel : left_pixel;
        uint8_t *const to = delta_neg ? left_pixel : right_pixel;
        uint8_t *const garbage_start = delta_neg ? left_pixel + bytes - delta_bytes : left_pixel;

        if (delta_bytes) {
          memmove(to, from, bytes - delta_bytes);
          if (patch_garbage) {
            memset(garbage_start, fill_byte, delta_bytes);
          }
        }

        if (delta_neg) {
          if (delta_bits) {
            const int rshift = delta_bits;
            const int lshift = 8 - rshift;
            for (int i = 0; i < bytes - 1; i++) {
              buf[i] = (buf[i] >> rshift) | (buf[i+1] << lshift);
            }
            if (patch_garbage) {
              buf[bytes - 1] >>= rshift;
              buf[bytes - 1] |= fill_byte << lshift;
            } else {
              // Leave shifted-out areas alone
              buf[bytes - 1] = (buf[bytes - 1] >> rshift) | (buf[bytes - 1] & (0xFF << lshift));
            }
          }
        } else {
          if (delta_bits) {
            const int lshift = delta_bits;
            const int rshift = 8 - lshift;
            for (int i = bytes - 1; i >= 1; i--) {
              buf[i] = (buf[i] << lshift) | (buf[i-1] >> rshift);
            }
            if (patch_garbage) {
              buf[0] <<= lshift;
              buf[0] |= fill_byte >> rshift;
            } else {
              // Leave shifted-out areas alone
              buf[0] = (buf[0] << lshift) | (buf[0] & (0xFF >> rshift));
            }
          }
        }
        break;
      }
      case 8: {
        uint8_t *const left_pixel = row_info.data + min_x;
        uint8_t *const right_pixel = left_pixel + abs_delta;

        if (pixels_to_move <= 0) {
          // on this row, the delta is wider than the available pixels
          if (patch_garbage) {
            const uint8_t fill_byte = delta_neg ? left_pixel[num_pix_data_row - 1] :
                                                  left_pixel[0];
            memset(left_pixel, fill_byte, num_pix_data_row);
          }
          break;
        }

        uint8_t *const from = delta_neg ? right_pixel : left_pixel;
        uint8_t *const to = delta_neg ? left_pixel : right_pixel;
        uint8_t *const garbage_start = delta_neg ? left_pixel + pixels_to_move : left_pixel;
        const uint8_t fill_byte = delta_neg ? right_pixel[pixels_to_move - 1] : left_pixel[0];

        memmove(to, from, (size_t)pixels_to_move);
        if (patch_garbage) {
          memset(garbage_start, fill_byte, abs_delta);
        }
        break;
      }
      default:
        WTF;
    }
  }
}

void graphics_private_move_pixels_vertically(GBitmap *bitmap, int16_t delta_y) {
  if (!bitmap || (delta_y == 0)) {
    return;
  }

  const int bpp = gbitmap_get_bits_per_pixel(bitmap->info.format);

  const bool delta_neg = (delta_y < 0);
  const int16_t abs_delta = ABS(delta_y);
  const int16_t min_y = bitmap->bounds.origin.y;
  const int16_t max_y = grect_get_max_y(&bitmap->bounds) - 1;
  const int16_t max_x = grect_get_max_x(&bitmap->bounds) - 1;
  const int16_t iterate_dir = delta_neg ? -1 : 1;
  const int16_t end_y = delta_neg ? max_y : min_y;

  const int16_t start_y = delta_neg ? min_y + abs_delta : max_y - abs_delta;
  if ((!delta_neg && (start_y < end_y)) || (delta_neg && (start_y > end_y))) {
    return;
  }
  for (int16_t y = start_y; y != end_y; y -= iterate_dir) {
    const GBitmapDataRowInfo dst_row_info = gbitmap_get_data_row_info(bitmap, y + delta_y);
    const GBitmapDataRowInfo src_row_info = gbitmap_get_data_row_info(bitmap, y);

    switch (bpp) {
      case 1: {
        // Note: this doesn't care about the bounding, because we don't have any round 1bpp
        // devices to support, and it simplifies the code.
#if PBL_ROUND
        WTF;
#endif
        memmove(dst_row_info.data, src_row_info.data, bitmap->row_size_bytes);
        break;
      }
      case 8: {
        const int16_t dst_min_x = MAX(dst_row_info.min_x, bitmap->bounds.origin.x);
        const int16_t dst_max_x = MIN(dst_row_info.max_x, max_x);
        const int16_t dst_pixels = dst_max_x - dst_min_x + 1;

        const int16_t src_min_x = MAX(src_row_info.min_x, bitmap->bounds.origin.x);
        const int16_t src_max_x = MIN(src_row_info.max_x, max_x);
        const int16_t src_pixels = src_max_x - src_min_x + 1;

        const int16_t x_offset = src_min_x - dst_min_x;
        const int16_t copy_pixels = MIN(src_pixels, dst_pixels);
        memmove(dst_row_info.data + dst_min_x + x_offset, src_row_info.data + src_min_x,
                (size_t)copy_pixels);
        break;
      }
      default:
        WTF;
    }
  }
}

GColor graphics_private_sample_line_color(const GBitmap *bitmap, GColorSampleEdge edge,
                                          GColor fallback) {
  if (!bitmap) {
    return fallback;
  }

  GColor color = fallback;

  const int bpp = gbitmap_get_bits_per_pixel(bitmap->info.format);

  const int16_t min_x = bitmap->bounds.origin.x;
  const int16_t min_y = bitmap->bounds.origin.y;
  const int16_t end_x = grect_get_max_x(&bitmap->bounds);
  const int16_t end_y = grect_get_max_y(&bitmap->bounds);

  const bool horiz_advance = (edge == GColorSampleEdgeUp) || (edge == GColorSampleEdgeDown);
  const bool edge_is_max_position = (edge == GColorSampleEdgeDown) ||
                                    (edge == GColorSampleEdgeRight);

  const int16_t length = horiz_advance ? (end_x - min_x) : (end_y - min_y);

  for (int16_t i = 0; i < length; i++) {
    const int16_t x = horiz_advance ? min_x + i : (edge_is_max_position ? end_x - 1 : min_x);
    const int16_t y = !horiz_advance ? min_y + i : (edge_is_max_position ? end_y - 1 : min_y);

    const GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(bitmap, y);
    GColor this_color;
    switch (bpp) {
      case 1:
        this_color = (data_row_info.data[x / 8] & (0x1 << (x % 8))) ? GColorWhite : GColorBlack;
        break;
      case 8:
        this_color.argb = data_row_info.data[x];
        break;
      default:
        WTF;
    }

    if (i == 0) {
      color.argb = this_color.argb;
    } else if (color.argb != this_color.argb) {
      return fallback;
    }
  }
  return color;
}
