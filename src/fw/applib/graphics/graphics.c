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

#include "graphics.h"

#include "bitblt.h"
#include "bitblt_private.h"
#include "framebuffer.h"
#include "graphics_private.h"
#include "graphics_private_raw.h"
#include "gtransform.h"

#include "applib/app_logging.h"
#include "applib/applib_malloc.auto.h"
#include "kernel/ui/kernel_ui.h"
#include "process_management/process_manager.h"
#include "process_state/app_state/app_state.h"
#include "system/passert.h"
#include "system/logging.h"
#include "util/bitset.h"
#include "util/graphics.h"
#include "util/math.h"
#include "util/reverse.h"
#include "util/trig.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if !defined(__clang__)
#pragma GCC optimize ("O3")
#endif

void graphics_draw_pixel(GContext* ctx, GPoint point) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  point.x += ctx->draw_state.drawing_box.origin.x;
  point.y += ctx->draw_state.drawing_box.origin.y;

  graphics_private_set_pixel(ctx, point);
}

T_STATIC void prv_fill_rect_legacy2(GContext *ctx, GRect rect, uint16_t radius,
    GCornerMask corner_mask, GColor fill_color) {
  if (gcolor_is_transparent(fill_color)) {
    fill_color = GColorWhite;
  }

  // as this function will only be called with radius 0 to or
  // to support the legacy2 behavior (where the radius is clamped to 8) it's safe to assume 8px here
  PBL_ASSERTN(radius <= 8);
  GBitmap* bitmap = graphics_context_get_bitmap(ctx);

  // translate to absolute bitmap coordinates:
  rect.origin.x += ctx->draw_state.drawing_box.origin.x;
  rect.origin.y += ctx->draw_state.drawing_box.origin.y;

  // clip it to avoid drawing outside of the bitmap memory:
  GRect clipped_rect = rect;
  grect_standardize(&clipped_rect);
  grect_clip(&clipped_rect, &bitmap->bounds);
  grect_clip(&clipped_rect, &ctx->draw_state.clip_box);
  if (grect_is_empty(&clipped_rect)) {
    return;
  }

  // All the row insets are packed into an uint32, taking 4 bits per inset (hence the 8px radius limit):
  static const uint32_t round_top_corner_lookup[] = {
    0x0, 0x01, 0x01, 0x12, 0x113, 0x123, 0x1234, 0x11235, 0x112346,
  };
  static const uint32_t round_bottom_corner_lookup[] = {
    0x0, 0x01, 0x10, 0x210, 0x3110, 0x32100, 0x432100, 0x5321100, 0x64321100,
  };

  // Set up the insets for doing the top corners.
  uint32_t corner_insets_left = (corner_mask & GCornerTopLeft) ? round_top_corner_lookup[radius] : 0;
  uint32_t corner_insets_right = (corner_mask & GCornerTopRight) ? round_top_corner_lookup[radius] : 0;

  const unsigned int top_cropped_rows_count = clipped_rect.origin.y - rect.origin.y;
  const int32_t left_cropped_columns_count = MAX(0, clipped_rect.origin.x - rect.origin.x);
  const int32_t right_cropped_columns_count = MAX(0, rect.size.w - clipped_rect.size.w -
                                                  left_cropped_columns_count);

  if (top_cropped_rows_count) {
    // Skip over rows for each one that's cropped off the top.
    corner_insets_left >>= 4 * MIN(top_cropped_rows_count, 8);
    corner_insets_right >>= 4 * MIN(top_cropped_rows_count, 8);
  }

  // Mark the destination dirty before clipped_rect is modified.
  graphics_context_mark_dirty_rect(ctx, clipped_rect);

  // bit-block fiddling:
  const int16_t max_y = clipped_rect.origin.y + clipped_rect.size.h;
  for (; clipped_rect.origin.y < max_y; ++clipped_rect.origin.y) {

    if ((clipped_rect.origin.y == (rect.origin.y + rect.size.h) - radius) && (corner_mask & GCornersBottom)) {
      if (corner_mask & GCornerBottomLeft) {
        corner_insets_left = round_bottom_corner_lookup[radius];
      }
      if (corner_mask & GCornerBottomRight) {
        corner_insets_right = round_bottom_corner_lookup[radius];
      }
    }

    int32_t left_side = MAX((int32_t)(corner_insets_left & 0xf) - left_cropped_columns_count, 0);
    int32_t right_side = MAX((int32_t)(corner_insets_right & 0xf) - right_cropped_columns_count, 0);

    int32_t corner_insets = left_side + right_side;
    int32_t width = corner_insets < clipped_rect.size.w ? (clipped_rect.size.w - corner_insets) : 0;
    uint32_t x = clipped_rect.origin.x + left_side;
    corner_insets_left >>= 4;
    corner_insets_right >>= 4;

    PBL_ASSERTN(clipped_rect.origin.y < bitmap->bounds.size.h);
    PBL_ASSERTN(clipped_rect.origin.y >= 0);

    const uint16_t y = clipped_rect.origin.y;
    const uint16_t x_end = x + width;
    graphics_private_draw_horizontal_line_integral(ctx, &ctx->dest_bitmap, y, x, x_end, fill_color);
  }
}

//! Return the maximum rounded corner radius allowed for a given rectangle size
T_STATIC uint16_t prv_clamp_corner_radius(GSize size, GCornerMask corner_mask, uint16_t radius) {
  if (corner_mask == GCornerNone) {
    return 0;
  }

  int16_t min_size = MIN(size.w, size.h);

  if (min_size >= 2 * radius) {
    return radius;
  } else {
    return (min_size / 2);
  }
}

typedef void (*FillCircleImplFunc)(GContext *, GPoint pt, uint16_t radius, GCornerMask mask);

//! generic fill_rect implementation to avoid code-duplication between aa and non-aa fill_rect
void prv_fill_rect_internal(GContext *ctx, const GRect *rect, uint16_t radius,
                            GCornerMask corner_mask, GColor fill_color, uint16_t alt_radius,
                            FillCircleImplFunc circle_func) {
  // only draw if there is enough to cover the rounded edges - otherwise round down to largest
  // radius that can be drawn
  radius = prv_clamp_corner_radius(rect->size, corner_mask, radius);

  if (radius <= alt_radius) {
    prv_fill_rect_legacy2(ctx, *rect, radius, corner_mask, fill_color);
  } else {
    // These are used to optimize the rectangles that are drawn such that only three rectangles
    // are drawn always
    int16_t top_rect_origin_x = rect->origin.x;
    int16_t top_rect_size_w = rect->size.w;
    int16_t bottom_rect_origin_x = rect->origin.x;
    int16_t bottom_rect_size_w = rect->size.w;

    // Fill 3 rectangles and 4 quadrants
    if (corner_mask & GCornerTopLeft) {
      circle_func(ctx, GPoint(rect->origin.x + radius, rect->origin.y + radius),
                  radius, GCornerTopLeft);
      top_rect_origin_x += radius;
      top_rect_size_w -= radius;
    }
    if (corner_mask & GCornerBottomLeft) {
      circle_func(ctx, GPoint(rect->origin.x + radius, rect->origin.y + rect->size.h - radius - 1),
                  radius, GCornerBottomLeft);
      bottom_rect_origin_x += radius;
      bottom_rect_size_w -= radius;
    }
    if (corner_mask & GCornerTopRight) {
      circle_func(ctx, GPoint(rect->origin.x + rect->size.w - radius - 1, rect->origin.y + radius),
                  radius, GCornerTopRight);
      top_rect_size_w -= radius;
    }
    if (corner_mask & GCornerBottomRight) {
      circle_func(ctx, GPoint(rect->origin.x + rect->size.w - radius - 1,
                              rect->origin.y + rect->size.h - radius - 1),
                  radius, GCornerBottomRight);
      bottom_rect_size_w -= radius;
    }

    // Top Rect
    prv_fill_rect_legacy2(ctx, GRect(top_rect_origin_x, rect->origin.y, top_rect_size_w, radius),
                          0, GCornerNone, fill_color);

    // Middle Rect
    prv_fill_rect_legacy2(ctx, GRect(rect->origin.x, rect->origin.y + radius,
                                     rect->size.w, rect->size.h - 2 * radius),
                          0, GCornerNone, fill_color);

    // Bottom Rect
    prv_fill_rect_legacy2(ctx, GRect(bottom_rect_origin_x, rect->origin.y + rect->size.h - radius,
                                     bottom_rect_size_w, radius),
                          0, GCornerNone, fill_color);
  }
}

T_STATIC void prv_fill_rect_non_aa(GContext* ctx, const GRect *rect, uint16_t radius,
                                   GCornerMask corner_mask, GColor fill_color) {

  // for radii <= 8 we can safely use the legacy2 behavior
  const uint16_t alt_radius = 8;
  FillCircleImplFunc circle_func = graphics_circle_quadrant_fill_non_aa;
  prv_fill_rect_internal(ctx, rect, radius, corner_mask, fill_color, alt_radius, circle_func);
}

#if PBL_COLOR
T_STATIC void prv_fill_rect_aa(GContext* ctx, const GRect *rect, uint16_t radius,
                               GCornerMask corner_mask, GColor fill_color) {
  FillCircleImplFunc circle_func = graphics_internal_circle_quadrant_fill_aa;
  prv_fill_rect_internal(ctx, rect, radius, corner_mask, fill_color, 0, circle_func);
}
#endif // PBL_COLOR

void graphics_fill_round_rect(GContext* ctx, const GRect *rect, uint16_t radius,
                              GCornerMask corner_mask) {
  PBL_ASSERTN(ctx);
  if (!rect || ctx->lock) {
    return;
  }

#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    // Antialiased (not supported on 1-bit color)
    prv_fill_rect_aa(ctx, rect, radius, corner_mask, ctx->draw_state.fill_color);
    return;
  }
#endif
  prv_fill_rect_non_aa(ctx, rect, radius, corner_mask, ctx->draw_state.fill_color);
}

void graphics_fill_round_rect_by_value(GContext* ctx, GRect rect, uint16_t radius,
                                 GCornerMask corner_mask) {
  graphics_fill_round_rect(ctx, &rect, radius, corner_mask);
}

void graphics_fill_rect(GContext* ctx, const GRect *rect) {
  graphics_fill_round_rect(ctx, rect, 0, GCornerNone);
}

T_STATIC void prv_draw_rect(GContext *ctx, const GRect *rect) {
  GColor fill_color = ctx->draw_state.fill_color;
  ctx->draw_state.fill_color = ctx->draw_state.stroke_color;
  graphics_fill_rect(ctx, &GRect(rect->origin.x, rect->origin.y, rect->size.w, 1)); // top
  graphics_fill_rect(ctx, &GRect(rect->origin.x, rect->origin.y + rect->size.h - 1,
                                 rect->size.w, 1)); // bottom
  graphics_fill_rect(ctx, &GRect(rect->origin.x, rect->origin.y + 1, 1, rect->size.h - 2)); // left
  graphics_fill_rect(ctx, &GRect(rect->origin.x + rect->size.w - 1,
                                 rect->origin.y + 1, 1, rect->size.h - 2)); // right
  ctx->draw_state.fill_color = fill_color;
}

#if PBL_COLOR
T_STATIC void prv_draw_rect_aa_stroked(GContext *ctx, const GRect *rect, uint8_t stroke_width) {
  const GPoint tl = GPoint(rect->origin.x, rect->origin.y);
  const GPoint tr = GPoint(rect->origin.x + rect->size.w - 1, rect->origin.y);
  const GPoint bl = GPoint(rect->origin.x, rect->origin.y + rect->size.h - 1);
  const GPoint br = GPoint(rect->origin.x + rect->size.w - 1, rect->origin.y + rect->size.h - 1);

  graphics_line_draw_stroked_aa(ctx, tl, tr, stroke_width);
  graphics_line_draw_stroked_aa(ctx, tl, bl, stroke_width);
  graphics_line_draw_stroked_aa(ctx, tr, br, stroke_width);
  graphics_line_draw_stroked_aa(ctx, bl, br, stroke_width);
}
#endif // PBL_COLOR

T_STATIC void prv_draw_rect_stroked(GContext *ctx, const GRect *rect, uint8_t stroke_width) {
  const GPoint tl = GPoint(rect->origin.x, rect->origin.y);
  const GPoint tr = GPoint(rect->origin.x + rect->size.w - 1, rect->origin.y);
  const GPoint bl = GPoint(rect->origin.x, rect->origin.y + rect->size.h - 1);
  const GPoint br = GPoint(rect->origin.x + rect->size.w - 1, rect->origin.y + rect->size.h - 1);

  graphics_line_draw_stroked_non_aa(ctx, tl, tr, stroke_width);
  graphics_line_draw_stroked_non_aa(ctx, tl, bl, stroke_width);
  graphics_line_draw_stroked_non_aa(ctx, tr, br, stroke_width);
  graphics_line_draw_stroked_non_aa(ctx, bl, br, stroke_width);
}

void graphics_draw_rect(GContext* ctx, const GRect *rect) {
  PBL_ASSERTN(ctx);
  if (!rect || ctx->lock) {
    return;
  }

  if (ctx->draw_state.stroke_width <= 2) {
    // Note: stroke width == 2 is rounded down to stroke width of 1
    prv_draw_rect(ctx, rect);
    return;
  }
#if PBL_COLOR
  if (ctx->draw_state.antialiased) {
    // Antialiased and Stroke Width > 2
    prv_draw_rect_aa_stroked(ctx, rect, ctx->draw_state.stroke_width);
    return;
  }
#endif
  // Non-Antialiased and Stroke Width > 2
  // Note: stroke width must be odd and greater than 2
  prv_draw_rect_stroked(ctx, rect, ctx->draw_state.stroke_width);
}

void graphics_draw_rect_by_value(GContext* ctx, GRect rect) {
  graphics_draw_rect(ctx, &rect);
}

void graphics_draw_rect_precise(GContext* ctx, const GRectPrecise *rect) {
  const Fixed_S16_3 right = grect_precise_get_max_x(rect);
  const Fixed_S16_3 bottom = grect_precise_get_max_y(rect);

  const GPointPrecise top_left = rect->origin;
  const GPointPrecise top_right = {right, rect->origin.y};
  const GPointPrecise bottom_right = {right, bottom};
  const GPointPrecise bottom_left = {rect->origin.x, bottom};

  graphics_line_draw_precise_stroked(ctx, top_left, top_right);
  graphics_line_draw_precise_stroked(ctx, top_right, bottom_right);
  graphics_line_draw_precise_stroked(ctx, bottom_right, bottom_left);
  graphics_line_draw_precise_stroked(ctx, bottom_left, top_left);
}

// This takes care of all routines since it re-uses existing AA and SW functionality in draw line
// and draw circle
T_STATIC void prv_draw_round_rect(GContext* ctx, const GRect *rect, uint16_t radius) {
  const GPoint origin = rect->origin;
  const int16_t width = rect->size.w;
  const int16_t height = rect->size.h;

  // Subtract out twice the respective radius values to get the actual width and height of the
  // rectangle lines
  const int16_t width_actual = width - (2 * radius);
  const int16_t height_actual = height - (2 * radius);

  // Take into account the radius values to determine the eight points for each of the four lines
  const GPoint top_l = GPoint(origin.x + radius, origin.y);
  const GPoint top_r = GPoint(origin.x + radius + width_actual - 1, origin.y);

  const GPoint bottom_l = GPoint(origin.x + radius, origin.y + height - 1);
  const GPoint bottom_r = GPoint(origin.x + radius + width_actual - 1, origin.y + height - 1);

  const GPoint left_t = GPoint(origin.x, origin.y + radius);
  const GPoint left_b = GPoint(origin.x, origin.y + radius + height_actual - 1);

  const GPoint right_t = GPoint(origin.x + width - 1, origin.y + radius);
  const GPoint right_b = GPoint(origin.x + width - 1, origin.y + radius + height_actual - 1);

  // Draw lines between each transformed corner point
  graphics_draw_line(ctx, top_l, top_r);       // top
  graphics_draw_line(ctx, bottom_l, bottom_r); // bottom
  graphics_draw_line(ctx, left_t, left_b);     // left
  graphics_draw_line(ctx, right_t, right_b);   // right

  // Draw quadrants
  const GPoint tl = GPoint(origin.x + radius, origin.y + radius);
  const GPoint tr = gpoint_add(tl, GPoint(width_actual - 1, 0));
  const GPoint bl = gpoint_add(tl, GPoint(0, height_actual - 1));
  const GPoint br = gpoint_add(tl, GPoint(width_actual - 1, height_actual - 1));

  graphics_circle_quadrant_draw(ctx, tl, radius, GCornerTopLeft);
  graphics_circle_quadrant_draw(ctx, bl, radius, GCornerBottomLeft);
  graphics_circle_quadrant_draw(ctx, tr, radius, GCornerTopRight);
  graphics_circle_quadrant_draw(ctx, br, radius, GCornerBottomRight);
}

#if PBL_COLOR
T_STATIC void prv_draw_round_rect_aa(GContext* ctx, const GRect *rect, uint16_t radius) {
  // Assumes AA and stroke_width is set appropriately in ctx
  prv_draw_round_rect(ctx, rect, radius);
}

T_STATIC void prv_draw_round_rect_aa_stroked(GContext* ctx, const GRect *rect,
                                             uint16_t radius, uint8_t stroke_width) {
  // Assumes AA and stroke_width is set appropriately in ctx
  prv_draw_round_rect(ctx, rect, radius);
}
#endif // SCREEN_COLOR_DEPTH_BITS

T_STATIC void prv_draw_round_rect_stroked(GContext* ctx, const GRect *rect, uint16_t radius,
                                          uint8_t stroke_width) {
  // Assumes AA and stroke_width is set appropriately in ctx
  prv_draw_round_rect(ctx, rect, radius);
}

static void prv_graphics_convert_8_bit_to_1_bit(const GBitmap *from, GBitmap *to) {
  const GRect bounds = from->bounds;
  uint8_t *to_buffer = (uint8_t *) to->addr;

  const int y_start = bounds.origin.y;
  const int y_end = y_start + bounds.size.h;
  const int x_start = bounds.origin.x;
  const int x_end = x_start + bounds.size.w;

  for (int y = y_start; y < y_end; ++y) {
    int to_idx_base = y * to->row_size_bytes;
    uint8_t *line = to_buffer + to_idx_base;
    for (int x = x_start; x < x_end; ++x) {
      bitset8_clear(line, x);
    }
  }
}

void graphics_draw_round_rect(GContext* ctx, const GRect *rect, uint16_t radius) {
  PBL_ASSERTN(ctx);
  if (!rect || ctx->lock) {
    return;
  }

  // only draw if there is enough to cover the rounded edges - otherwise round down to largest
  // radius that can be drawn
  radius = prv_clamp_corner_radius(rect->size, GCornersAll, radius);

  if (radius == 0) {
    graphics_draw_rect(ctx, rect);
  } else {
#if PBL_COLOR
    if (ctx->draw_state.antialiased) {
      if (ctx->draw_state.stroke_width > 1) {
        // Antialiased and Stroke Width > 1
        // Note: stroke width == 2 is rounded down to stroke width of 1
        prv_draw_round_rect_aa_stroked(ctx, rect, radius, ctx->draw_state.stroke_width);
        return;
      } else {
        // Antialiased and Stroke Width == 1 (not supported on 1-bit color)
        // Note: stroke width == 2 is rounded down to stroke width of 1
        prv_draw_round_rect_aa(ctx, rect, radius);
        return;
      }
    }
#endif
    if (ctx->draw_state.stroke_width > 1) {
      // Non-Antialiased and Stroke Width > 1
      prv_draw_round_rect_stroked(ctx, rect, radius, ctx->draw_state.stroke_width);
    } else {
      // Non-Antialiased and Stroke Width == 1
      prv_draw_round_rect(ctx, rect, radius);
    }
  }
}

void graphics_draw_round_rect_by_value(GContext* ctx, GRect rect, uint16_t radius) {
  graphics_draw_round_rect(ctx, &rect, radius);
}

void graphics_context_init(GContext *context, FrameBuffer *framebuffer,
                           GContextInitializationMode init_mode) {
  PBL_ASSERTN(context);
  PBL_ASSERTN(framebuffer);

  *context = (GContext) {
    // For apps, this is run before the app has a chance to run, so there's no concern here of the
    // app changing its framebuffer size.
    .dest_bitmap = framebuffer_get_as_bitmap(framebuffer, &framebuffer->size),
    .parent_framebuffer = framebuffer,
    .parent_framebuffer_vertical_offset = 0,
    .lock = false
  };

  // init the font cache
  FontCache *font_cache = &context->font_cache;
  memset(font_cache->cache_keys, 0, sizeof(font_cache->cache_keys));
  memset(font_cache->cache_data, 0, sizeof(font_cache->cache_data));
  keyed_circular_cache_init(&font_cache->line_cache, font_cache->cache_keys,
                            font_cache->cache_data, sizeof(LineCacheData), LINE_CACHE_SIZE);

  graphics_context_set_default_drawing_state(context, init_mode);
}

void graphics_context_set_default_drawing_state(GContext *ctx,
                                                GContextInitializationMode init_mode) {
  PBL_ASSERTN(ctx);
  GBitmap* bitmap = graphics_context_get_bitmap(ctx);

  ctx->draw_state = (GDrawState) {
    .stroke_color = GColorBlack,
    .fill_color = GColorBlack,
    .text_color = GColorWhite,
    .tint_color = GColorWhite,
    .compositing_mode = GCompOpAssign,
    .clip_box = bitmap->bounds,
    .drawing_box = bitmap->bounds,
#if PBL_COLOR
    .antialiased = !process_manager_compiled_with_legacy2_sdk(),
#endif
    .stroke_width = 1,
    .draw_implementation = &g_default_draw_implementation,
    .avoid_text_orphans = (init_mode == GContextInitializationMode_System),
  };
}

GDrawState graphics_context_get_drawing_state(GContext* ctx) {
  PBL_ASSERTN(ctx);
  return ctx->draw_state;
}

void graphics_context_set_drawing_state(GContext* ctx, GDrawState draw_state) {
  PBL_ASSERTN(ctx);
  ctx->draw_state = draw_state;
}

void graphics_context_move_draw_box(GContext* ctx, GPoint offset) {
  PBL_ASSERTN(ctx);
  ctx->draw_state.drawing_box.origin = gpoint_add(ctx->draw_state.drawing_box.origin, offset);
}

void graphics_context_set_stroke_color(GContext* ctx, GColor color) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  #if PBL_BW
    color = gcolor_get_bw(color);
  #else
    color = gcolor_closest_opaque(color);
  #endif
  ctx->draw_state.stroke_color = color;
}

void graphics_context_set_stroke_color_2bit(GContext* ctx, GColor2 color) {
  graphics_context_set_stroke_color(ctx, get_native_color(color));
}

void graphics_context_set_fill_color(GContext* ctx, GColor color) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  #if PBL_BW
    color = gcolor_get_grayscale(color);
  #else
    color = gcolor_closest_opaque(color);
  #endif
  ctx->draw_state.fill_color = color;
}

void graphics_context_set_fill_color_2bit(GContext* ctx, GColor2 color) {
  graphics_context_set_fill_color(ctx, get_native_color(color));
}

void graphics_context_set_text_color(GContext* ctx, GColor color) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  #if PBL_BW
    color = gcolor_get_bw(color);
  #else
    color = gcolor_closest_opaque(color);
  #endif
  ctx->draw_state.text_color = color;
}

void graphics_context_set_text_color_2bit(GContext* ctx, GColor2 color) {
  graphics_context_set_text_color(ctx, get_native_color(color));
}

void graphics_context_set_tint_color(GContext *ctx, GColor color) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }
  ctx->draw_state.tint_color = gcolor_closest_opaque(color);
}

void graphics_context_set_compositing_mode(GContext* ctx, GCompOp mode) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  ctx->draw_state.compositing_mode = mode;
}

void graphics_context_set_antialiased(GContext* ctx, bool enable) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }
#if PBL_COLOR
  ctx->draw_state.antialiased = enable;
#endif
}

bool graphics_context_get_antialiased(GContext *ctx) {
  PBL_ASSERTN(ctx);
  return PBL_IF_COLOR_ELSE(ctx->draw_state.antialiased, false);
}

void graphics_context_set_stroke_width(GContext* ctx, uint8_t stroke_width) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    return;
  }

  // Ignore if stroke width == 0
  if (stroke_width >= 1) {
    ctx->draw_state.stroke_width = stroke_width;
  }
}

GSize graphics_context_get_framebuffer_size(GContext *ctx) {
  if (ctx && ctx->parent_framebuffer) {
    return ctx->parent_framebuffer->size;
  } else {
    return GSize(DISP_COLS, DISP_ROWS);
  }
}

GBitmap* graphics_context_get_bitmap(GContext* ctx) {
  PBL_ASSERTN(ctx);
  return &ctx->dest_bitmap;
}

void graphics_context_mark_dirty_rect(GContext* ctx, GRect rect) {
  PBL_ASSERTN(ctx);
  if (ctx->parent_framebuffer) {
    framebuffer_mark_dirty_rect(ctx->parent_framebuffer, rect);
  }
}

bool graphics_frame_buffer_is_captured(GContext* ctx) {
  PBL_ASSERTN(ctx);
  return ctx->lock;
}

GBitmap* graphics_capture_frame_buffer_format(GContext *ctx, GBitmapFormat format) {
  PBL_ASSERTN(ctx);
  if (ctx->lock) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "Frame buffer has already been captured; it cannot be captured again until "
            "graphics_release_frame_buffer has been called.");
    return NULL;
  }
  ctx->lock = true;

  GBitmap *native =  graphics_context_get_bitmap(ctx);

  if (format == native->info.format) {
    return native;
  }

  GBitmap *result = NULL;
  if (format == GBitmapFormat1Bit && native->info.format == GBitmapFormat8Bit) {
    // Create a new blank gbitmap in the correct format.
    const GBitmap *native_framebuffer = graphics_context_get_bitmap(ctx);

    if (process_manager_compiled_with_legacy2_sdk()) {
      result = app_state_legacy2_get_2bit_framebuffer();
    } else {
      result = gbitmap_create_blank(native_framebuffer->bounds.size, GBitmapFormat1Bit);
    }

    if (result) {
      prv_graphics_convert_8_bit_to_1_bit(native_framebuffer, result);
    }
  }

  if (!result) {
    ctx->lock = false;
  }

  return result;
}

GBitmap* graphics_capture_frame_buffer_2bit(GContext *ctx) {
  return graphics_capture_frame_buffer_format(ctx, GBitmapFormat1Bit);
}

MOCKABLE GBitmap *graphics_capture_frame_buffer(GContext *ctx) {
  PBL_ASSERTN(ctx);
  return graphics_capture_frame_buffer_format(ctx, GBITMAP_NATIVE_FORMAT);
}

#include "system/profiler.h"
MOCKABLE bool graphics_release_frame_buffer(GContext *ctx, GBitmap *buffer) {
  PBL_ASSERTN(ctx);
  GBitmap *native_framebuffer = graphics_context_get_bitmap(ctx);
  if (gbitmap_get_format(buffer) != GBITMAP_NATIVE_FORMAT) {
    ctx->lock = false;
    bitblt_bitmap_into_bitmap(native_framebuffer, buffer, GPointZero,
        GCompOpAssign, GColorWhite);
    framebuffer_dirty_all(ctx->parent_framebuffer);

    // Don't destroy the bitmap we got from app_state_legacy2_get_2bit_framebuffer()
    if (!process_manager_compiled_with_legacy2_sdk()) {
      gbitmap_destroy(buffer);
    }
    return true;
  }

  if (buffer == native_framebuffer) {
    ctx->lock = false;
    framebuffer_dirty_all(ctx->parent_framebuffer);
    return true;
  }

  return false;
}
