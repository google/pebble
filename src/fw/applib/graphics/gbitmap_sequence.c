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

#include "gbitmap_sequence.h"

#include "gbitmap_png.h"
#include "util/graphics.h"
#include "util/net.h"
#include "util/time/time.h"
#include "applib/app_logging.h"
#include "applib/applib_malloc.auto.h"
#include "syscall/syscall.h"
#include "system/passert.h"
#include "util/bitset.h"
#include "util/math.h"

#define APNG_DECODE_ERROR "APNG decoding failed"
#define APNG_MEMORY_ERROR "APNG memory allocation failed"
#define APNG_FORMAT_ERROR "Unsupported APNG format, only APNG8 is supported!"
#define APNG_LOAD_ERROR "Failed to load APNG"
#define APNG_UPDATE_ERROR "gbitmap_sequence failed to update bitmap"
#define APNG_ELAPSED_WARNING "invalid elapsed_ms for gbitmap_sequence, forward progression only"

static bool prv_gbitmap_sequence_restart(GBitmapSequence *bitmap_sequence, bool reset_elapsed) {
  if (bitmap_sequence == NULL) {
    return false;
  }

  // can start seeking after SIG + IHDR
  int32_t metadata_bytes = png_seek_chunk_in_resource(bitmap_sequence->resource_id,
                                                      PNG_HEADER_SIZE, false, NULL);

  if (metadata_bytes <= 0) {
    return false;
  }
  metadata_bytes += PNG_HEADER_SIZE;
  bitmap_sequence->png_decoder_data.read_cursor = metadata_bytes;
  bitmap_sequence->current_frame = 0;
  bitmap_sequence->current_frame_delay_ms = 0;

  if (reset_elapsed) {
    bitmap_sequence->elapsed_ms = 0;
    bitmap_sequence->play_index = 0;
  }
  return true;
}

//! Directly modifies dst, blending src into dst using equation
//! dst = src * (alpha_normalized) + dst * (1 - alpha_normalized)
static ALWAYS_INLINE void prv_gbitmap_sequence_blend_over(GColor8 src_color, GColor8 *dst) {
  if (src_color.a == 3) {
// Fast path: 100% opacity
    *dst = src_color;
  } else if (src_color.a == 0) {
// Fast path: 0% opacity, no-op!
  } else {
    const GColor8 dest_color = *dst;
    const uint8_t f_src = src_color.a;
    const uint8_t f_dst = 3 - f_src;
    GColor8 final = {};
    final.r = (src_color.r * f_src + dest_color.r * f_dst) / 3;
    final.g = (src_color.g * f_src + dest_color.g * f_dst) / 3;
    final.b = (src_color.b * f_src + dest_color.b * f_dst) / 3;
    final.a = src_color.a;  // Different than bitblt, required for correct transparency
    *dst = final;
  }
}

GBitmapSequence *gbitmap_sequence_create_with_resource(uint32_t resource_id) {
  ResAppNum app_num = sys_get_current_resource_num();
  return gbitmap_sequence_create_with_resource_system(app_num, resource_id);
}

GBitmapSequence *gbitmap_sequence_create_with_resource_system(ResAppNum app_num,
                                                              uint32_t resource_id) {
  uint8_t *frame_data_buffer = NULL;

  // Allocate gbitmap
  GBitmapSequence* bitmap_sequence = applib_type_zalloc(GBitmapSequence);
  if (bitmap_sequence == NULL) {
    goto cleanup;
  }

  bitmap_sequence->resource_id = resource_id;
  bitmap_sequence->data_is_loaded_from_flash = true;

  if (!prv_gbitmap_sequence_restart(bitmap_sequence, true)) {
    goto cleanup;
  }
  int32_t frame_bytes = bitmap_sequence->png_decoder_data.read_cursor;

  frame_data_buffer = applib_zalloc(frame_bytes);
  if (frame_data_buffer == NULL) {
    goto cleanup;
  }

  const size_t bytes_read = sys_resource_load_range(app_num, resource_id,
                                                    0, frame_data_buffer, frame_bytes);
  if (bytes_read != (size_t)frame_bytes) {
    goto cleanup;
  }

  upng_t *upng = upng_create();
  if (upng == NULL) {
    goto cleanup;
  }

  bitmap_sequence->png_decoder_data.upng = upng;
  upng_load_bytes(upng, frame_data_buffer, frame_bytes);

  upng_error upng_state = upng_decode_metadata(upng);
  if (upng_state != UPNG_EOK) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            (upng_state == UPNG_ENOMEM) ? APNG_MEMORY_ERROR : APNG_DECODE_ERROR);
    goto cleanup;
  }
  // Save metadata to bitmap_sequence
  uint32_t play_count = 0;
  // If png is APNG, get num plays, otherwise play count is 0
  if (upng_is_apng(upng)) {
    play_count = upng_apng_num_plays(upng);
    // At the API level 0 is no loops vs APNG specification uses 0 for infinite
    play_count = (play_count == 0) ? PLAY_COUNT_INFINITE : play_count;
  }
  bitmap_sequence->play_count = play_count;
  bitmap_sequence->bitmap_size = (GSize){.w = upng_get_width(upng), .h = upng_get_height(upng)};
  bitmap_sequence->total_frames = upng_apng_num_frames(upng);

  if (!gbitmap_png_is_format_supported(upng)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, APNG_FORMAT_ERROR);
    goto cleanup;
  }

  // Create a color palette in RGBA8 format from RGB24 + ALPHA8 PNG Palettes
  upng_format png_format = upng_get_format(upng);
  if (png_format >= UPNG_INDEXED1 && png_format <= UPNG_INDEXED8) {
    bitmap_sequence->png_decoder_data.palette_entries =
        gbitmap_png_load_palette(upng, &bitmap_sequence->png_decoder_data.palette);
    if (bitmap_sequence->png_decoder_data.palette_entries == 0) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to load palette");
      goto cleanup;
    }
  }

  bitmap_sequence->header_loaded = true;

cleanup:
  applib_free(frame_data_buffer);  // Free compressed image buffer

  if (!bitmap_sequence || !bitmap_sequence->header_loaded) {
    APP_LOG(APP_LOG_LEVEL_ERROR, APNG_LOAD_ERROR);
    gbitmap_sequence_destroy(bitmap_sequence);
  }

  return bitmap_sequence;
}

bool gbitmap_sequence_restart(GBitmapSequence *bitmap_sequence) {
  return prv_gbitmap_sequence_restart(bitmap_sequence, true);
}

void gbitmap_sequence_destroy(GBitmapSequence *bitmap_sequence) {
  if (bitmap_sequence) {
    upng_destroy(bitmap_sequence->png_decoder_data.upng, true);
    applib_free(bitmap_sequence->png_decoder_data.palette);
    applib_free(bitmap_sequence);
  }
}

static ALWAYS_INLINE GColor8 *prv_target_pixel_addr(GBitmap *bitmap, apng_fctl *fctl,
                                                    uint32_t x, uint32_t y) {
  uint32_t offset = (fctl->y_offset + y + bitmap->bounds.origin.y) * bitmap->row_size_bytes +
      (fctl->x_offset + x + bitmap->bounds.origin.x);
  GColor8 *pixel_data = bitmap->addr;
  return &pixel_data[offset];
}

static void prv_set_pixel_in_row(uint8_t *row_data, GBitmapFormat bitmap_format,
                                 uint32_t x, GColor8 color) {
  if (bitmap_format == GBitmapFormat1Bit) {
    if (!gcolor_is_invisible(color)) {
      const bool pixel_is_white = !gcolor_equal(color, GColorBlack);
      bitset8_update(row_data, x, pixel_is_white);
    }
  } else if ((bitmap_format == GBitmapFormat8Bit) ||
             (bitmap_format == GBitmapFormat8BitCircular)) {
    GColor8 *const destination_pixel = (GColor8 *)(row_data + x);
    *destination_pixel = color;
  } else {
    WTF; // Unsupported destination type
  }
}

bool gbitmap_sequence_update_bitmap_next_frame(GBitmapSequence *bitmap_sequence,
                                               GBitmap *bitmap, uint32_t *delay_ms) {
  bool retval = false;
  uint8_t* buffer = NULL;

  // Disabled if play count is 0 and not the very first frame
  if (!bitmap_sequence ||
      (bitmap_sequence->play_count == 0 && bitmap_sequence->current_frame != 0)) {
    return false;
  }

  GBitmapSequencePNGDecoderData *png_decoder_data = &bitmap_sequence->png_decoder_data;
  upng_t *upng = png_decoder_data->upng;

  // Check bitmap_sequence metadata is loaded, bitmap_sequence size, type & memory constraints
  const GBitmapFormat bitmap_format = gbitmap_get_format(bitmap); // call is NULL-safe
  if (!bitmap_sequence->header_loaded || bitmap == NULL || bitmap->addr == NULL ||
      bitmap_sequence->bitmap_size.w > (bitmap->bounds.size.w) ||
      bitmap_sequence->bitmap_size.h > (bitmap->bounds.size.h)) {
    goto cleanup;
  }

  if (!((bitmap_format == GBitmapFormat1Bit) ||
        (bitmap_format == GBitmapFormat8Bit) ||
        (bitmap_format == GBitmapFormat8BitCircular))) {

    APP_LOG(APP_LOG_LEVEL_ERROR, "Invalid destination bitmap format for APNG");
    goto cleanup;
  }

  // Update current time elapsed using the previous frames current_frame_delay_ms
  bitmap_sequence->elapsed_ms += bitmap_sequence->current_frame_delay_ms;

  // Check if single animation loop is complete, and restart if there are more loops
  if (bitmap_sequence->current_frame >= bitmap_sequence->total_frames) {
    if ((++bitmap_sequence->play_index < bitmap_sequence->play_count) ||
        (bitmap_sequence->play_count == PLAY_COUNT_INFINITE)) {
      prv_gbitmap_sequence_restart(bitmap_sequence, false);
    } else {
      return false;  // animation complete
    }
  }

  const int32_t metadata_bytes =
     png_seek_chunk_in_resource(bitmap_sequence->resource_id,
                                png_decoder_data->read_cursor, true, NULL);

  if (metadata_bytes <= 0) {
    goto cleanup;
  }

  buffer = applib_zalloc(metadata_bytes);
  if (buffer == NULL) {
    goto cleanup;
  }

  ResAppNum app_num = sys_get_current_resource_num();
  const size_t bytes_read = sys_resource_load_range(
      app_num, bitmap_sequence->resource_id,
      png_decoder_data->read_cursor, buffer, metadata_bytes);

  if (bytes_read != (size_t)metadata_bytes) {
    goto cleanup;
  }

  png_decoder_data->read_cursor += metadata_bytes;

  upng_load_bytes(upng, buffer, metadata_bytes);
  upng_error upng_state = upng_decode_image(upng);
  if (upng_state != UPNG_EOK) {
    APP_LOG(APP_LOG_LEVEL_ERROR,
            (upng_state == UPNG_ENOMEM) ? APNG_MEMORY_ERROR : APNG_DECODE_ERROR);
    goto cleanup;
  }
  applib_free(buffer);

  bitmap_sequence->current_frame++;

  const uint32_t width = bitmap_sequence->bitmap_size.w;
  const uint32_t height = bitmap_sequence->bitmap_size.h;

  const bool bitmap_supports_transparency = (bitmap_format != GBitmapFormat1Bit);

  // DISPOSE_OP_BACKGROUND sets the background to black with transparency (0x00)
  // If we don't support transparency, just do nothing.
  if (bitmap_supports_transparency &&
      (png_decoder_data->last_dispose_op == APNG_DISPOSE_OP_BACKGROUND)) {
    const uint32_t y_origin = bitmap->bounds.origin.y + png_decoder_data->previous_yoffset;
    for (uint32_t y = y_origin; y < y_origin + png_decoder_data->previous_height; y++) {
      const GBitmapDataRowInfo row_info = gbitmap_get_data_row_info(bitmap, y);
      const uint32_t x_origin = bitmap->bounds.origin.x + png_decoder_data->previous_xoffset;
      const int16_t min_x = MAX((uint32_t)row_info.min_x, x_origin);
      const int16_t max_x = MIN((uint32_t)row_info.max_x,
                                (x_origin + png_decoder_data->previous_width - 1));

      const int16_t num_bytes = max_x - min_x + 1;
      if (num_bytes > 0) {
        memset(row_info.data + min_x, 0, num_bytes);
      }
    }
  }

  apng_fctl fctl = {0}; // Defaults work for IDAT frame without fctl data

  // If this frame doesn't have fctl, use the full width & height
  if (!upng_get_apng_fctl(upng, &fctl)) {
    fctl.width = width;
    fctl.height = height;
    // As a PNG image is only a single frame, display it forever
    bitmap_sequence->current_frame_delay_ms = PLAY_DURATION_INFINITE;
  } else {
    png_decoder_data->last_dispose_op = fctl.dispose_op;
    png_decoder_data->previous_xoffset = fctl.x_offset;
    png_decoder_data->previous_yoffset = fctl.y_offset;
    png_decoder_data->previous_width = fctl.width;
    png_decoder_data->previous_height = fctl.height;

    fctl.delay_den = (fctl.delay_den == 0) ? APNG_DEFAULT_DELAY_UNITS : fctl.delay_den;
    // Update the current_frame_delay_ms for this frame
    bitmap_sequence->current_frame_delay_ms =
        ((uint32_t)fctl.delay_num * MS_PER_SECOND) / fctl.delay_den;
  }

  // Return the delay_ms for the new frame
  if (delay_ms != NULL) {
    *delay_ms = bitmap_sequence->current_frame_delay_ms;
  }

  uint32_t bpp = upng_get_bpp(upng);
  upng_format png_format = upng_get_format(upng);
  uint8_t *upng_buffer = (uint8_t*)upng_get_buffer(upng);

  // Byte aligned rows for image at bpp
  uint16_t row_stride_bytes = (fctl.width *  bpp + 7) / 8;

  if (png_format >= UPNG_INDEXED1 && png_format <= UPNG_INDEXED8) {
    const GColor8 *palette = png_decoder_data->palette;

    for (uint32_t y = 0; y < fctl.height; y++) {
      const uint16_t corrected_dst_y = fctl.y_offset + y + bitmap->bounds.origin.y;
      const GBitmapDataRowInfo row_info = gbitmap_get_data_row_info(bitmap, corrected_dst_y);
      int16_t delta_x = fctl.x_offset + bitmap->bounds.origin.x;
      for (int32_t x = MAX(0, row_info.min_x - delta_x);
           x < MIN((int32_t)fctl.width, row_info.max_x - delta_x + 1);
           x++) {
        const uint32_t corrected_dst_x = x + delta_x;
        const uint8_t palette_index = raw_image_get_value_for_bitdepth(upng_buffer, x, y,
            row_stride_bytes, bpp);

        const GColor8 src = palette[palette_index];
        GColor8 *const dst = (GColor8 *)(row_info.data + corrected_dst_x);
        if (fctl.blend_op == APNG_BLEND_OP_OVER) {
          prv_gbitmap_sequence_blend_over(src, dst);
        } else {
          *dst = src;
        }
      }
    }
  } else if (png_format >= UPNG_LUMINANCE1 && png_format <= UPNG_LUMINANCE8) {
    const int32_t transparent_gray = gbitmap_png_get_transparent_gray_value(upng);

    for (uint32_t y = 0; y < fctl.height; y++) {
      const uint16_t corrected_y = fctl.y_offset + y + bitmap->bounds.origin.y;
      const GBitmapDataRowInfo row_info = gbitmap_get_data_row_info(bitmap, corrected_y);

      // delta_x is the first bit of data in this frame relative to the bitmap's coordinate system
      const int16_t delta_x = fctl.x_offset + bitmap->bounds.origin.x;

      // for each pixel in this frame, clipping to the bitmap geometry
      for (int32_t x = MAX(0, row_info.min_x - delta_x);
           x < MIN((int32_t)fctl.width, row_info.max_x - delta_x + 1);
           x++) {

        const uint32_t corrected_dst_x = x + delta_x;
        uint8_t channel = raw_image_get_value_for_bitdepth(upng_buffer, x, y,
                                                           row_stride_bytes, bpp);
        if (transparent_gray >= 0 && channel == transparent_gray) {
          // Grayscale only has fully transparent, so only modify pixels
          // during OP_SOURCE to make the area transparent
          if (fctl.blend_op == APNG_BLEND_OP_SOURCE) {
            prv_set_pixel_in_row(row_info.data, bitmap_format, corrected_dst_x, GColorClear);
          }
        } else {
          channel = (channel * 255) / ~(~0 << bpp);  // Convert to 8-bit value
          const GColor8 color = GColorFromRGB(channel, channel, channel);

          prv_set_pixel_in_row(row_info.data, bitmap_format, corrected_dst_x, color);
        }
      }
    }
  }

  // Successfully updated gbitmap from sequence
  retval = true;

cleanup:
  if (!retval) {
    APP_LOG(APP_LOG_LEVEL_ERROR, APNG_UPDATE_ERROR);
    applib_free(buffer);
  }

  return retval;
}

// total elapsed from start of animation
bool gbitmap_sequence_update_bitmap_by_elapsed(GBitmapSequence *bitmap_sequence,
                                               GBitmap *bitmap, uint32_t elapsed_ms) {
  if (!bitmap_sequence) {
    return false;
  }

  // Disabled if play count is 0 and not the very first frame
  if (bitmap_sequence->play_count == 0 && bitmap_sequence->current_frame != 0) {
    return false;
  }

  // If animation has started and specified time is in the past
  if (bitmap_sequence->current_frame_delay_ms != 0 && elapsed_ms <= bitmap_sequence->elapsed_ms) {
    APP_LOG(APP_LOG_LEVEL_WARNING, APNG_ELAPSED_WARNING);
    return false;
  }

  bool retval = false;
  bool frame_updated = true;
  while (frame_updated && ((elapsed_ms > bitmap_sequence->elapsed_ms) ||
                           (bitmap_sequence->current_frame_delay_ms == 0))) {
    frame_updated = gbitmap_sequence_update_bitmap_next_frame(bitmap_sequence, bitmap, NULL);
    // If frame is updated at least once, return true
    if (frame_updated) {
      retval = true;
    }
  }

  return retval;
}

// Helper functions
int32_t gbitmap_sequence_get_current_frame_idx(GBitmapSequence *bitmap_sequence) {
  if (bitmap_sequence) {
    return bitmap_sequence->current_frame;
  }
  return -1;
}

uint32_t gbitmap_sequence_get_current_frame_delay_ms(GBitmapSequence *bitmap_sequence) {
  if (bitmap_sequence) {
    return bitmap_sequence->current_frame_delay_ms;
  }
  return 0;
}

uint32_t gbitmap_sequence_get_total_num_frames(GBitmapSequence *bitmap_sequence) {
  if (bitmap_sequence) {
    return bitmap_sequence->total_frames;
  }
  return 0;
}

uint32_t gbitmap_sequence_get_play_count(GBitmapSequence *bitmap_sequence) {
  if (bitmap_sequence) {
    return bitmap_sequence->play_count;
  }
  return 0;
}

void gbitmap_sequence_set_play_count(GBitmapSequence *bitmap_sequence, uint32_t play_count) {
  // Loop count is not allowed to be set to 0
  if (bitmap_sequence && play_count) {
    bitmap_sequence->play_count = play_count;
  }
}

GSize gbitmap_sequence_get_bitmap_size(GBitmapSequence *bitmap_sequence) {
  GSize size = (GSize){0, 0};
  if (bitmap_sequence) {
    size = bitmap_sequence->bitmap_size;
  }
  return size;
}

uint32_t gbitmap_sequence_get_total_duration(GBitmapSequence *bitmap_sequence) {
  if (bitmap_sequence) {
    return bitmap_sequence->total_duration_ms;
  }
  return 0;
}
