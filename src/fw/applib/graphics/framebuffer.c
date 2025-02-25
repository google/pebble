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

//! @file framebuffer.c
//! Bitdepth independent routines for framebuffer.h
//! Bitdepth dependent routines can be found in the 1_bit & 8_bit folders in their
//! respective framebuffer.c files.

#include "applib/graphics/framebuffer.h"
#include "system/passert.h"

void framebuffer_init(FrameBuffer *fb, const GSize *size) {
  PBL_ASSERTN(!gsize_equal(size, &GSizeZero));
  fb->size = *size;
  framebuffer_reset_dirty(fb);
  // make sure the size is not bigger than the actual buffer size
  PBL_ASSERTN(framebuffer_get_size_bytes(fb) <= FRAMEBUFFER_SIZE_BYTES);
}

GBitmap framebuffer_get_as_bitmap(FrameBuffer *fb, const GSize *size) {
  PBL_ASSERTN(!gsize_equal(size, &GSizeZero));
  const GBitmapDataRowInfoInternal *data_row_infos =
    PBL_IF_RECT_ELSE(NULL, g_gbitmap_spalding_data_row_infos);

  return (GBitmap) {
    .addr = fb->buffer,
    .row_size_bytes = gbitmap_format_get_row_size_bytes(size->w, GBITMAP_NATIVE_FORMAT),
    .info = (BitmapInfo) {.format = GBITMAP_NATIVE_FORMAT, .version = GBITMAP_VERSION_CURRENT},
    .bounds = (GRect) { GPointZero, *size },
    .data_row_infos = data_row_infos,
  };
}

void framebuffer_dirty_all(FrameBuffer *fb) {
  PBL_ASSERTN(!gsize_equal(&fb->size, &GSizeZero));
  fb->dirty_rect = (GRect) { GPointZero, fb->size };
  fb->is_dirty = true;
}

void framebuffer_reset_dirty(FrameBuffer *fb) {
  PBL_ASSERTN(!gsize_equal(&fb->size, &GSizeZero));
  fb->dirty_rect = GRectZero;
  fb->is_dirty = false;
}

bool framebuffer_is_dirty(FrameBuffer *fb) {
  PBL_ASSERTN(!gsize_equal(&fb->size, &GSizeZero));
  return fb->is_dirty;
}

GSize framebuffer_get_size(FrameBuffer *fb) {
  return fb->size;
}
