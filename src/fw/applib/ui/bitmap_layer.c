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

#include "bitmap_layer.h"

#include "applib/graphics/graphics.h"
#include "util/trig.h"
#include "applib/applib_malloc.auto.h"
#include "process_management/process_manager.h"

#include <string.h>

void bitmap_layer_update_proc(BitmapLayer *image, GContext* ctx) {
  const GColor bg_color = image->background_color;
  if (!gcolor_is_transparent(bg_color)) {
    graphics_context_set_fill_color(ctx, bg_color);
    graphics_fill_rect(ctx, &image->layer.bounds);
  }
  graphics_context_set_compositing_mode(ctx, image->compositing_mode);
  if (image->bitmap != NULL) {
    const GSize size = image->bitmap->bounds.size;
    const bool clips = true;  // bitmap layer not allowed to draw outside of its frame
    GRect rect = (GRect){{0, 0}, size};
    grect_align(&rect, &image->layer.bounds, image->alignment, clips);
    if (!process_manager_compiled_with_legacy2_sdk()) {
      // Dirty workaround for calculation of offset in graphics_draw_bitmap_in_rect
      // and preserving state of bitmap alignment in bitmap_layer
      // The previous behavior is relied on by some 2.x apps, and therefore we exclude
      // the fix for apps compiled with older SDKs. See PBL-19136 for details.
      rect.origin.x -= image->layer.bounds.origin.x;
      rect.origin.y -= image->layer.bounds.origin.y;
    }
    graphics_draw_bitmap_in_rect(ctx, image->bitmap, &rect);
  }
}

void bitmap_layer_init(BitmapLayer *image, const GRect *frame) {
  *image = (BitmapLayer){};
  image->layer.frame = *frame;
  image->layer.bounds = (GRect){{0, 0}, frame->size};
  image->layer.update_proc = (LayerUpdateProc)bitmap_layer_update_proc;
  layer_set_clips(&image->layer, true);
  image->background_color = GColorClear;
  image->compositing_mode = GCompOpAssign;
  layer_mark_dirty(&(image->layer));
}

BitmapLayer* bitmap_layer_create(GRect frame) {
  BitmapLayer* layer = applib_type_malloc(BitmapLayer);
  if (layer) {
    bitmap_layer_init(layer, &frame);
  }
  return layer;
}

void bitmap_layer_deinit(BitmapLayer *bitmap_layer) {
  layer_deinit(&bitmap_layer->layer);
}

void bitmap_layer_destroy(BitmapLayer* bitmap_layer) {
  if (bitmap_layer == NULL) {
    return;
  }
  bitmap_layer_deinit(bitmap_layer);
  applib_free(bitmap_layer);
}

Layer* bitmap_layer_get_layer(const BitmapLayer *bitmap_layer) {
  return &((BitmapLayer *)bitmap_layer)->layer;
}

const GBitmap* bitmap_layer_get_bitmap(BitmapLayer* bitmap_layer) {
  return bitmap_layer->bitmap;
}

void bitmap_layer_set_bitmap(BitmapLayer *image, const GBitmap *bitmap) {
  if (image == NULL) {
    return;
  }
  image->bitmap = bitmap;
  layer_mark_dirty(&(image->layer));
}

void bitmap_layer_set_alignment(BitmapLayer *image, GAlign alignment) {
  if (alignment == image->alignment) {
    return;
  }
  image->alignment = alignment;
  layer_mark_dirty(&(image->layer));
}

void bitmap_layer_set_background_color(BitmapLayer *image, GColor color) {
  const GColor image_color = image->background_color;
  if (gcolor_equal(color, image_color)) {
    return;
  }
  image->background_color = color;
  layer_mark_dirty(&(image->layer));
}

void bitmap_layer_set_background_color_2bit(BitmapLayer *bitmap_layer, GColor2 color) {
  bitmap_layer_set_background_color(bitmap_layer, get_native_color(color));
}

void bitmap_layer_set_compositing_mode(BitmapLayer *image, GCompOp mode) {
  if (image->compositing_mode == mode) {
    return;
  }
  image->compositing_mode = mode;
  layer_mark_dirty(&(image->layer));
}
