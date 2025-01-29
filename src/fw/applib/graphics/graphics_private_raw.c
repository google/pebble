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
#include "graphics_private_raw.h"
#include "graphics_private_raw_mask.h"
#include "gtypes.h"
#include "system/passert.h"
#include "util/bitset.h"
#include "util/graphics.h"
#include "util/math.h"

ALWAYS_INLINE void graphics_private_raw_blend_color_factor(const GContext *ctx, GColor *dst_color,
                                                           unsigned int data_offset,
                                                           GColor src_color, int x,
                                                           uint8_t factor) {
#if SCREEN_COLOR_DEPTH_BITS == 8
  src_color.a = (uint8_t)(factor * 3 / (FIXED_S16_3_ONE.raw_value - 1));

  const GColor blended_color = gcolor_alpha_blend(src_color, *dst_color);
#if CAPABILITY_HAS_MASKING
  const GDrawMask *mask = ctx->draw_state.draw_mask;
  graphics_private_raw_mask_apply(dst_color, mask, data_offset, x, 1, blended_color);
#else
  *dst_color = blended_color;
#endif // CAPABILITY_HAS_MASKING

#endif // (SCREEN_COLOR_DEPTH_BITS == 8)
}

static ALWAYS_INLINE void prv_set_color(const GContext *ctx, GColor *dst_color,
                                        unsigned int data_row_offset, int x, int width,
                                        GColor src_color) {
#if CAPABILITY_HAS_MASKING
  const GDrawMask *mask = ctx->draw_state.draw_mask;
  graphics_private_raw_mask_apply(dst_color, mask, data_row_offset, x, width, src_color);
#else
  memset(dst_color, src_color.argb, (size_t)width);
#endif // CAPABILITY_HAS_MASKING
}

// Plots row at given starting position and width, dithers grayscale colors
static void prv_assign_row_with_pattern_1bit(GBitmap *framebuffer, int16_t y, int16_t x,
                                             int32_t width, GColor color) {
  const uint32_t pattern = graphics_private_get_1bit_grayscale_pattern(color, (uint8_t) y);
  uint32_t left_edge_block, right_edge_block, mask;
  const uint32_t left_edge_bits_count = x % 32;
  const uint32_t right_edge_bits_count = (x + width) % 32;
  uint32_t *block = ((uint32_t*)framebuffer->addr) + (y * (framebuffer->row_size_bytes / 4))
                    + (x / 32);

  bool both_edges_in_same_block = (left_edge_bits_count + width) < 32;
  if (both_edges_in_same_block) {
    left_edge_block = (0xffffffff << left_edge_bits_count);
    right_edge_block = right_edge_bits_count ? (0xffffffff >> (32 - right_edge_bits_count)) : 0;
    mask = (left_edge_block & right_edge_block);
    *(block) = (*(block) & ~mask) | (pattern & mask);
  } else {
    if (left_edge_bits_count) {
      mask = 0xffffffff << left_edge_bits_count;
      *(block) = (*(block) & ~mask) | (pattern & mask);
      block++;
      width -= (32 - left_edge_bits_count);
    }
    if (right_edge_bits_count) {
      mask = 0xffffffff >> (32 - right_edge_bits_count);
      *(block + (width / 32)) = (*(block + (width / 32)) & ~mask) | (pattern & mask);
      width -= right_edge_bits_count;
    }
    if (width > 0) {
      memset(block, pattern, (width / 8));
    }
  }
}

// ## Line blending functions:

// This function draws horizontal line with AA edges, given values have to be adjusted for
//   screen coordinates and clipped according to the clip box, does not respect transparency
//   on the drawn line (beside edges)
T_STATIC void prv_assign_horizontal_line_raw(GContext *ctx, int16_t y, Fixed_S16_3 x1,
                                             Fixed_S16_3 x2, GColor color) {
  PBL_ASSERTN(ctx);
  GBitmap *framebuffer = &ctx->dest_bitmap;
  PBL_ASSERTN(framebuffer->bounds.origin.x == 0 && framebuffer->bounds.origin.y == 0);

  // Clip the line to the bitmap data row's range
  const GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(framebuffer, y);
  x1.raw_value = MAX(x1.raw_value, data_row_info.min_x << FIXED_S16_3_PRECISION);
  x2.raw_value = MIN(x2.raw_value, data_row_info.max_x << FIXED_S16_3_PRECISION);
  if (x1.integer > x2.integer) {
    return;
  }

#if PBL_COLOR
  GColor8 *output = (GColor8 *)(data_row_info.data + x1.integer);

  // first pixel with blending if fraction is different than 0
  const unsigned int data_row_offset = data_row_info.data - (uint8_t *)framebuffer->addr;
  if (x1.fraction != 0) {
    graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x1.integer,
                                            (uint8_t)(FIXED_S16_3_ONE.raw_value - x1.fraction));
    output++;
    x1.integer++;
  }

  // middle pixels
  const int16_t width = x2.integer - x1.integer + 1;
  if (width > 0) {
    prv_set_color(ctx, output, data_row_offset, x1.integer, width, color);
    output += width;
    // x1 doesn't need to be increased as it's not used anymore in this function
  }

  // last pixel with blending (don't render first *and* last pixel if line length is 1)
  if (x2.fraction != 0) {
    graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x2.integer,
                                            (uint8_t)x2.fraction);
  }
#else
  // TODO: as part of PBL-30849 make this a first-class function
  // also see prv_blend_horizontal_line_raw
  const int16_t x1_rounded = (x1.raw_value + FIXED_S16_3_HALF.raw_value) / FIXED_S16_3_FACTOR;
  const int16_t x2_rounded = (x2.raw_value + FIXED_S16_3_HALF.raw_value) / FIXED_S16_3_FACTOR;
  prv_assign_row_with_pattern_1bit(framebuffer, y, x1_rounded, x2_rounded - x1_rounded + 1, color);
#endif
}

// This function draws vertical line with AA edges, given values have to be adjusted for
// screen coordinates and clipped according to the clip box, does not respect transparency
// on the drawn line (beside edges)
T_STATIC void prv_assign_vertical_line_raw(GContext *ctx, int16_t x, Fixed_S16_3 y1,
                                           Fixed_S16_3 y2, GColor color) {
  PBL_ASSERTN(ctx);
  GBitmap *framebuffer = &ctx->dest_bitmap;
  PBL_ASSERTN(framebuffer->bounds.origin.x == 0 && framebuffer->bounds.origin.y == 0);

  GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(framebuffer, y1.integer);
  GColor8 *output = (GColor8 *)(data_row_info.data + x);

  // first pixel with blending
  const unsigned int data_row_offset = data_row_info.data - (uint8_t *)framebuffer->addr;
  if (y1.fraction != 0) {
    // Only draw the pixel if its within the bitmap data row range
    if (WITHIN(x, data_row_info.min_x, data_row_info.max_x)) {
      graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x,
                                              (uint8_t)(FIXED_S16_3_ONE.raw_value - y1.fraction));
    }
    y1.integer++;
    data_row_info = gbitmap_get_data_row_info(framebuffer, y1.integer);
    output = (GColor8 *)(data_row_info.data + x);
  }

  // middle pixels
  while (y1.integer <= y2.integer) {
    // Only draw the pixel if its within the bitmap data row range
    if (WITHIN(x, data_row_info.min_x, data_row_info.max_x)) {
      prv_set_color(ctx, output, data_row_offset, x, 1, color);
    }
    y1.integer++;
    data_row_info = gbitmap_get_data_row_info(framebuffer, y1.integer);
    output = (GColor8 *)(data_row_info.data + x);
  }

  // last pixel with blending (don't render first *and* last pixel if line length is 1)
  if (y2.fraction != 0) {
    // Only draw the pixel if its within the bitmap data row range
    if (WITHIN(x, data_row_info.min_x, data_row_info.max_x)) {
      graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x,
                                              (uint8_t)y2.fraction);
    }
  }
}

// This function draws horizontal line with blending, given values have to be clipped and adjusted
//   clip_box and draw_box respectively.
T_STATIC void prv_blend_horizontal_line_raw(GContext *ctx, int16_t y, int16_t x1, int16_t x2,
                                            GColor color) {
  PBL_ASSERTN(ctx);
  GBitmap *framebuffer = &ctx->dest_bitmap;
  // Clip the line to the bitmap data row's range
  const GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(framebuffer, y);
  x1 = MAX(x1, data_row_info.min_x);
  x2 = MIN(x2, data_row_info.max_x);

#if PBL_COLOR
  for (int i = x1; i <= x2; i++) {
    GColor *output = (GColor *)(data_row_info.data + i);
    const unsigned int data_row_offset = data_row_info.data - (uint8_t *)framebuffer->addr;
    prv_set_color(ctx, output, data_row_offset, i, 1, gcolor_alpha_blend(color, *output));
  }
#else
  // TODO: as part of PBL-30849 make this a first-class function
  // also see, prv_assign_horizontal_line_raw
  prv_assign_row_with_pattern_1bit(framebuffer, y, x1, x2 - x1 + 1, color);
#endif // SCREEN_COLOR_DEPTH_BITS == 8
}

// This function draws vertical line with blending, given values have to be clipped and adjusted
//   clip_box and draw_box respectively.
T_STATIC void prv_blend_vertical_line_raw(GContext *ctx, int16_t x, int16_t y1, int16_t y2,
                                          GColor color) {
  PBL_ASSERTN(ctx);
  GBitmap *framebuffer = &ctx->dest_bitmap;
#if SCREEN_COLOR_DEPTH_BITS == 8
  for (int i = y1; i < y2; i++) {
    // Skip over pixels outside the bitmap data row's range
    const GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(framebuffer, i);
    if (!WITHIN(x, data_row_info.min_x, data_row_info.max_x)) {
      continue;
    }
    GColor *output = (GColor *)(data_row_info.data + x);
    const unsigned int data_row_offset = data_row_info.data - (uint8_t *)framebuffer->addr;
    prv_set_color(ctx, output, data_row_offset, x, 1, gcolor_alpha_blend(color, *output));
  }
#else
  bool black = (gcolor_equal(color, GColorBlack));

  for (int i = y1; i < y2; i++) {
    uint8_t *line = ((uint8_t *)framebuffer->addr) + (framebuffer->row_size_bytes * i);
    bitset8_update(line, x, !black);
  }
#endif // SCREEN_COLOR_DEPTH_BITS == 8
}

// This function will draw a horizontal line with two gradients on side representing AA edges
T_STATIC void prv_assign_horizontal_line_delta_raw(GContext *ctx, int16_t y,
                                                   Fixed_S16_3 x1, Fixed_S16_3 x2,
                                                   uint8_t left_aa_offset, uint8_t right_aa_offset,
                                                   int16_t clip_box_min_x, int16_t clip_box_max_x,
                                                   GColor color) {
  PBL_ASSERTN(ctx);
  GBitmap *framebuffer = &ctx->dest_bitmap;
  PBL_ASSERTN(framebuffer->bounds.origin.x == 0 && framebuffer->bounds.origin.y == 0);

  // Clip the clip box to the bitmap data row's range
  const GBitmapDataRowInfo data_row_info = gbitmap_get_data_row_info(framebuffer, y);
  clip_box_min_x = MAX(clip_box_min_x, data_row_info.min_x);
  clip_box_max_x = MIN(clip_box_max_x, data_row_info.max_x);
  // If x1 is further outside the clip box than the left gradient width, we need to move x1 up
  // to clip_box_min_x and proceed such that we don't draw the left gradient
  int16_t x1_distance_outside_clip_box = clip_box_min_x - x1.integer;
  if (x1_distance_outside_clip_box > left_aa_offset) {
    left_aa_offset = 0;
    x1.integer += x1_distance_outside_clip_box;
  }

  // Clip x2 to clip_box_max_x
  x2.integer = MIN(clip_box_max_x, x2.integer);

  // Return early if there's nothing to draw
  if (x1.integer > x2.integer) {
    return;
  }

  GColor8 *output = (GColor8 *)(data_row_info.data + x1.integer);

  // first pixel with blending
  const unsigned int data_row_offset = data_row_info.data - (uint8_t *)framebuffer->addr;
  if (left_aa_offset == 1) {
    // To prevent bleeding of left-hand AA below clip_box
    if (x1.integer >= clip_box_min_x) {
      graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x1.integer,
                                              (uint8_t)(FIXED_S16_3_ONE.raw_value - x1.fraction));
    }
    output++;
    x1.integer++;
    // or first AA gradient with blending
  } else {
    for (int i = 0; i < left_aa_offset; i++) {
      // To preserve gradient with clipping:
      if (x1.integer < clip_box_min_x) {
        output++;
        x1.integer++;
        continue;
      }
      if (x1.integer > clip_box_max_x) {
        break;
      }
      graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x1.integer,
                                              (uint8_t)(FIXED_S16_3_ONE.raw_value * i /
                                                left_aa_offset));
      output++;
      x1.integer++;
    }
  }

  // middle pixels
  const int16_t width = x2.integer - x1.integer + 1;
  if (width > 0) {
    prv_set_color(ctx, output, data_row_offset, x1.integer, width, color);
    output += width;
    x1.integer += width;
  }

  // last pixel with blending (don't render first *and* last pixel if line length is 1)
  if (right_aa_offset <= 1) {
    if (x1.integer <= clip_box_max_x) {
      graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x1.integer,
                                              (uint8_t)x2.fraction);
    }
    // or last AA gradient with blending
  } else {
    for (int i = 0; i < right_aa_offset; i++) {
      if (x1.integer > clip_box_max_x) {
        break;
      }
      graphics_private_raw_blend_color_factor(ctx, output, data_row_offset, color, x1.integer,
                                              (uint8_t)(FIXED_S16_3_ONE.raw_value *
                                                (right_aa_offset - i) / right_aa_offset));
      output++;
      x1.integer++;
    }
  }
}

// TODO: Platform switches could happen here, too
const GDrawRawImplementation g_default_draw_implementation = {
  .assign_horizontal_line = prv_assign_horizontal_line_raw,
  .assign_vertical_line = prv_assign_vertical_line_raw,
  .blend_horizontal_line = prv_blend_horizontal_line_raw,
  .blend_vertical_line = prv_blend_vertical_line_raw,
  .assign_horizontal_line_delta = prv_assign_horizontal_line_delta_raw,
};
