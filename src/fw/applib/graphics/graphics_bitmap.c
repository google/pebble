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

#include "graphics_bitmap.h"

#include "bitblt.h"
#include "bitblt_private.h"
#include "gcontext.h"
#include "graphics.h"
#include "graphics_private.h"

#include "system/passert.h"
#include "util/graphics.h"
#include "util/trig.h"

void graphics_draw_bitmap_in_rect_processed(GContext *ctx, const GBitmap *src_bitmap,
                                            const GRect *rect_ref, GBitmapProcessor *processor) {
  if (!ctx || ctx->lock || !rect_ref) {
    return;
  }

  // Make a copy of the rect and translate it to global screen coordinates
  GRect rect = *rect_ref;
  rect.origin = gpoint_add(rect.origin, ctx->draw_state.drawing_box.origin);

  // Store the bitmap to draw in a new pointer that the processor can modify if it wants to
  const GBitmap *bitmap_to_draw = src_bitmap;

  // Call the processor's pre function, if applicable
  if (processor && processor->pre) {
    processor->pre(processor, ctx, &bitmap_to_draw, &rect);
  }

  // Bail out early if the bitmap to draw is NULL
  if (!bitmap_to_draw) {
    // Set rect to GRectZero so the processor's .post function knows that nothing was drawn
    rect = GRectZero;
    goto call_processor_post_function_and_return;
  }

  // TODO PBL-35694: what if src_bitmap == dest_bitmap....
  // This currently works only if the regions are equal, or the dest region is
  // to the bottom/right of it, since we scan from left to right, top to bottom
  GBitmap *dest_bitmap = graphics_context_get_bitmap(ctx);
  PBL_ASSERTN(dest_bitmap);

  // Save the original origin to compensate the position within src when rect.origin is negative
  const GPoint unclipped_origin = rect.origin;

  // Clip the rect to avoid drawing outside of the bitmap memory
  grect_standardize(&rect);
  grect_clip(&rect, &dest_bitmap->bounds);
  grect_clip(&rect, &ctx->draw_state.clip_box);
  // Bail out early if the clipped drawing rectangle is empty
  if (grect_is_empty(&rect)) {
    goto call_processor_post_function_and_return;
  }

  // Calculate the offset of src_bitmap to use
  const GPoint src_offset = gpoint_sub(rect.origin, unclipped_origin);

  // Blit bitmap_to_draw (which might have been changed by the processor) into dest_bitmap
  bitblt_bitmap_into_bitmap_tiled(dest_bitmap, bitmap_to_draw, rect, src_offset,
                                  ctx->draw_state.compositing_mode, ctx->draw_state.tint_color);
  // Mark the region where the bitmap was drawn as dirty
  graphics_context_mark_dirty_rect(ctx, rect);

call_processor_post_function_and_return:
  // Call the processor's post function, if applicable
  if (processor && processor->post) {
    processor->post(processor, ctx, bitmap_to_draw, &rect);
  }
}

void graphics_draw_bitmap_in_rect(GContext *ctx, const GBitmap *src_bitmap, const GRect *rect_ref) {
  graphics_draw_bitmap_in_rect_processed(ctx, src_bitmap, rect_ref, NULL);
}

void graphics_draw_bitmap_in_rect_by_value(GContext *ctx, const GBitmap *src_bitmap, GRect rect) {
  graphics_draw_bitmap_in_rect_processed(ctx, src_bitmap, &rect, NULL);
}

typedef struct DivResult {
  int32_t quot;
  int32_t rem;
} DivResult;

//! a div and mod operation where any remainder will always be the same direction as the numerator
static DivResult polar_div(int32_t numer, int32_t denom) {
  DivResult res;
  res.quot = numer / denom;
  res.rem = numer % denom;
  if (numer < 0 && res.rem > 0) {
    res.rem -= denom;
    res.quot += denom;
  }
  return res;
}

#if PBL_BW
T_STATIC bool get_bitmap_bit(GBitmap *bmp, int x, int y) {
  int byte_num = y * bmp->row_size_bytes + x / 8;
  int bit_num = x % 8;
  uint8_t byte = ((uint8_t*)(bmp->addr))[byte_num];
  return (byte & (1 << bit_num)) ? 1 : 0;
}
#elif PBL_COLOR
T_STATIC GColor get_bitmap_color(GBitmap *bmp, int x, int y) {
  const GBitmapFormat format = gbitmap_get_format(bmp);
  const GBitmapDataRowInfo row_info = gbitmap_get_data_row_info(bmp, y);
  const uint8_t *src = row_info.data;
  const uint8_t src_bpp = gbitmap_get_bits_per_pixel(format);
  uint8_t cindex = raw_image_get_value_for_bitdepth(src, x,
                                                    0,  // y = 0 when using data_row
                                                    bmp->row_size_bytes,
                                                    src_bpp);
  // Default color to be the raw color index - update only if palletized
  GColor src_color = (GColor){.argb = cindex};
  bool palletized = ((format == GBitmapFormat1BitPalette) ||
                     (format == GBitmapFormat2BitPalette) ||
                     (format == GBitmapFormat4BitPalette));
  if (palletized) {
    // Look up color in palette if palletized
    const GColor *palette = bmp->palette;
    src_color = palette[cindex];
  }
  return src_color;
}
#endif

void graphics_draw_rotated_bitmap(GContext* ctx, GBitmap *src, GPoint src_ic, int rotation,
                                  GPoint dest_ic) {
  PBL_ASSERTN(ctx);
  if (rotation == 0) {
    graphics_draw_bitmap_in_rect(
      ctx, src, &(GRect){ .origin = { dest_ic.x - src_ic.x, dest_ic.y - src_ic.y },
        .size = src->bounds.size });
    return;
  }

  GBitmap *dest_bitmap = graphics_capture_frame_buffer(ctx);
  if (dest_bitmap == NULL) {
    return;
  }

  GRect dest_clip = ctx->draw_state.clip_box;
  dest_ic.x += ctx->draw_state.drawing_box.origin.x;
  dest_ic.y += ctx->draw_state.drawing_box.origin.y;

  GCompOp compositing_mode = ctx->draw_state.compositing_mode;
#if PBL_BW
  GColor foreground, background;
  switch (compositing_mode) {
    case GCompOpAssign:
      foreground = GColorWhite;
      background = GColorBlack;
      break;
    case GCompOpAssignInverted:
      foreground = GColorBlack;
      background = GColorWhite;
      break;
    case GCompOpOr:
      foreground = GColorWhite;
      background = GColorClear;
      break;
    case GCompOpAnd:
      foreground = GColorClear;
      background = GColorBlack;
      break;
    case GCompOpClear:
      foreground = GColorBlack;
      background = GColorClear;
      break;
    case GCompOpSet:
      foreground = GColorClear;
      background = GColorWhite;
      break;
    default:
      PBL_ASSERT(0, "unknown compositing mode %d", compositing_mode);
      return;
  }
#endif

  // Backup context color
  const GColor ctx_color = ctx->draw_state.stroke_color;

  if (grect_contains_point(&src->bounds, &src_ic)) {
    // TODO: Optimize further (PBL-15657)
    // If src_ic is within the bounds of the source image, do the following performance
    // optimization:
    // Create a clipping rectangle based on the max distance away from the pivot point
    // that the destination image could be located at:
    // max distance from the pivot point = sqrt(x^2 + y^2), where x and y are at max twice the width
    // and height of the source image
    // i.e. in case the anchor point is on the edge then it would be twice
    // Also need to account for the dest_ic offset

    const int16_t max_width = MAX(src->bounds.origin.x + src->bounds.size.w - src_ic.x,
                                  src_ic.x - src->bounds.origin.x);
    const int16_t max_height = MAX(src->bounds.origin.y + src->bounds.size.h - src_ic.y,
                                   src_ic.y - src->bounds.origin.y);
    const int32_t width = 2 * (max_width + 1);   // Add one more pixel in case on the edge
    const int32_t height = 2 * (max_height + 1); // Add one more pixel in case on the edge

    // add two pixels just in case of rounding issues
    const int32_t max_distance = integer_sqrt((width * width) + (height * height)) + 2;
    const int32_t min_x = src_ic.x - max_distance;
    const int32_t min_y = src_ic.y - max_distance;

    const int32_t size_x = max_distance*2;
    const int32_t size_y = size_x;

    const GRect dest_clip_min = GRect(dest_ic.x + min_x, dest_ic.y + min_y, size_x, size_y);
    grect_clip(&dest_clip, &dest_clip_min);
  }

  for (int y = dest_clip.origin.y; y < dest_clip.origin.y + dest_clip.size.h; ++y) {
    for (int x = dest_clip.origin.x; x < dest_clip.origin.x + dest_clip.size.w; ++x) {
      // only draw if within the dest range
      const GBitmapDataRowInfo dest_info = gbitmap_get_data_row_info(dest_bitmap, y);
      if (!WITHIN(x, dest_info.min_x, dest_info.max_x)) {
        continue;
      }

      const int32_t cos_value = cos_lookup(-rotation);
      const int32_t sin_value = sin_lookup(-rotation);
      const int32_t src_numerator_x = cos_value * (x - dest_ic.x) - sin_value * (y - dest_ic.y);
      const int32_t src_numerator_y = cos_value * (y - dest_ic.y) + sin_value * (x - dest_ic.x);

      const DivResult src_vector_x = polar_div(src_numerator_x, TRIG_MAX_RATIO);
      const DivResult src_vector_y = polar_div(src_numerator_y, TRIG_MAX_RATIO);

      const int32_t src_x = src_ic.x + src_vector_x.quot;
      const int32_t src_y = src_ic.y + src_vector_y.quot;

      // only draw if within the src range
      const GBitmapDataRowInfo src_info = gbitmap_get_data_row_info(src, src_y);
      if (!(WITHIN(src_x, 0, src->bounds.size.w - 1) &&
        WITHIN(src_y, 0, src->bounds.size.h - 1) &&
        WITHIN(src_x, src_info.min_x, src_info.max_x))) {
        continue;
      }

#if PBL_BW
      // dividing by 8 to avoid overflows of <thresh> in the next loop
      const int32_t horiz_contrib[3] = {
        src_vector_x.rem < 0 ? (-src_vector_x.rem) >> 3 : 0,
        src_vector_x.rem < 0 ? (TRIG_MAX_RATIO + src_vector_x.rem) >> 3 :
                               (TRIG_MAX_RATIO - src_vector_x.rem) >> 3,
        src_vector_x.rem < 0 ? 0 : (src_vector_x.rem) >> 3
      };

      const int32_t vert_contrib[3] = {
        src_vector_y.rem < 0 ? (-src_vector_y.rem) >> 3 : 0,
        src_vector_y.rem < 0 ? (TRIG_MAX_RATIO + src_vector_y.rem) >> 3 :
                               (TRIG_MAX_RATIO - src_vector_y.rem) >> 3,
        src_vector_y.rem < 0 ? 0 : (src_vector_y.rem) >> 3
      };

      int32_t thresh = 0;

      for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
          if (src_x + i >= 0 && src_x + i < src->bounds.size.w
              && src_y + j >= 0 && src_y + j < src->bounds.size.h) {
            // I'm within bounds
            if (get_bitmap_bit(src, src_x + i , src_y + j)) {
              // more color
              thresh += (horiz_contrib[i+1] * vert_contrib[j+1]);
            } else {
              // less color
              thresh -= (horiz_contrib[i+1] * vert_contrib[j+1]);
            }
          }
        }
      }

      if (thresh > 0) {
        ctx->draw_state.stroke_color = foreground;
      } else {
        ctx->draw_state.stroke_color = background;
      }

      if (!gcolor_is_transparent(ctx->draw_state.stroke_color)) {
        graphics_private_set_pixel(ctx, GPoint(x, y));
      }
#elif PBL_COLOR
      const GColor src_color = get_bitmap_color(src, src_x, src_y);
      const GColor tint_color = ctx->draw_state.tint_color;
      switch (compositing_mode) {
        case GCompOpSet: {
          const GColor dst_color = get_bitmap_color(dest_bitmap, x, y);
          ctx->draw_state.stroke_color = gcolor_alpha_blend(src_color, dst_color);
          break;
        }
        case GCompOpOr: {
          const GColor dst_color = get_bitmap_color(dest_bitmap, x, y);
          if (tint_color.a != 0) {
            GColor actual_color = tint_color;
            actual_color.a = src_color.a;
            ctx->draw_state.stroke_color = gcolor_alpha_blend(actual_color, dst_color);
            break;
          }
        }
        case GCompOpAssign:
        default:
          // Do assign by default
          ctx->draw_state.stroke_color = src_color;
          break;
      }
      ctx->draw_state.stroke_color.a = 3; // Force to be opaque

      graphics_private_set_pixel(ctx, GPoint(x, y));
#endif
    }
  }

  // Restore context color
  ctx->draw_state.stroke_color = ctx_color;
  graphics_release_frame_buffer(ctx, dest_bitmap);
}
