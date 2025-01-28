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

#include "gtypes.h"
#include "gbitmap_pbi.h"
#include "gbitmap_png.h"

#include "applib/applib_malloc.auto.h"
#include "applib/applib_resource_private.h"
#include "applib/graphics/graphics.h"
#include "process_state/app_state/app_state.h"
#include "system/logging.h"
#include "system/passert.h"
#include "syscall/syscall.h"

#include <string.h>
#include <stddef.h>

uint8_t gbitmap_get_bits_per_pixel(GBitmapFormat format) {
  switch (format) {
    case GBitmapFormat1Bit:
    case GBitmapFormat1BitPalette:
      return 1;
    case GBitmapFormat2BitPalette:
      return 2;
    case GBitmapFormat4BitPalette:
      return 4;
    case GBitmapFormat8Bit:
    case GBitmapFormat8BitCircular:
      return 8;
  }
  return 0;
}

//! @return the size in bytes of the palette for a given format
uint8_t gbitmap_get_palette_size(GBitmapFormat format) {
  switch (format) {
    case GBitmapFormat1Bit:
    case GBitmapFormat8Bit:
    case GBitmapFormat8BitCircular:
      return 0;
    default:
      return (1 << gbitmap_get_bits_per_pixel(format));
  }
  return 0;
}

uint16_t gbitmap_format_get_row_size_bytes(int16_t width, GBitmapFormat format) {
  switch (format) {
    case GBitmapFormat1Bit:
      return ((width + 31) / 32 ) * 4;  // word aligned bytes
    case GBitmapFormat8Bit:
      return width;
    case GBitmapFormat1BitPalette:
    case GBitmapFormat2BitPalette:
    case GBitmapFormat4BitPalette:
      return ((width * gbitmap_get_bits_per_pixel(format) + 7) / 8); // byte aligned
    case GBitmapFormat8BitCircular:
      return 0; // variable width
  }
  return 0;
}

static GBitmap* prv_allocate_gbitmap(void) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    return (GBitmap *) applib_type_zalloc(GBitmapLegacy2);
  }
  return applib_type_zalloc(GBitmap);
}

static size_t prv_gbitmap_size(void) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    return applib_type_size(GBitmapLegacy2);
  }
  return applib_type_size(GBitmap);
}

static void prv_init_gbitmap_version(GBitmap *bitmap) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    bitmap->info.version = GBITMAP_VERSION_0;
  }
  bitmap->info.version = GBITMAP_VERSION_CURRENT;
}

uint8_t gbitmap_get_version(const GBitmap *bitmap) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    return GBITMAP_VERSION_0;
  }
  return bitmap->info.version;
}

// indirection to allow conditional mocking in unit-tests
T_STATIC
#if !UNITTEST
// apparently, GCC doesn't inline this otherwise
// scary, I wonder how many more places like these aren't inlined
ALWAYS_INLINE
#endif
GBitmapDataRowInfo prv_gbitmap_get_data_row_info(const GBitmap *bitmap, uint16_t y) {
  if (bitmap->info.format == GBitmapFormat8BitCircular) {
    const GBitmapDataRowInfoInternal *info = &bitmap->data_row_infos[y];
    return (GBitmapDataRowInfo) {
        .data = (uint8_t *)bitmap->addr + info->offset,
        .min_x = info->min_x,
        .max_x = info->max_x,
    };
  } else {
    return (GBitmapDataRowInfo) {
        .data = (uint8_t*)bitmap->addr + y * bitmap->row_size_bytes,
        .min_x = 0,
        // while this is conceptually wrong for .max_x as it should be
        // (.row_size_bytes / .bytes_per_pixel) - 1
        // it's still a valid value as we assume grect_get_max_x(.bounds) < .row_size_bytes * bpp
        // that way this is an efficient implementation of this functions contract
        .max_x = grect_get_max_x(&bitmap->bounds) - 1,
    };
  }
}

MOCKABLE GBitmapDataRowInfo gbitmap_get_data_row_info(const GBitmap *bitmap, uint16_t y) {
  return prv_gbitmap_get_data_row_info(bitmap, y);
}

void gbitmap_init_with_data(GBitmap *bitmap, const uint8_t *data) {
  BitmapData* bitmap_data = (BitmapData*) data;

  memset(bitmap, 0, prv_gbitmap_size());

  bitmap->row_size_bytes = bitmap_data->row_size_bytes;
  bitmap->info_flags = bitmap_data->info_flags;
  // Force this to false, just in case someone passes us some funny looking data.
  bitmap->info.is_bitmap_heap_allocated = false;

  // Note that our container contains values for the origin, but we want to ignore them.
  // This is because originally we just serialized GBitmap to disk,
  // but these fields don't really make sense for static images.
  // These origin fields are only used when reusing a byte buffer in a sub bitmap.
  // This allows us to have a shallow copy of a portion of a parent bitmap.
  // See gbitmap_init_as_sub_bitmap.
  bitmap->bounds.origin.x = 0; //((int16_t*)data)[2];
  bitmap->bounds.origin.y = 0; //((int16_t*)data)[3];

  bitmap->bounds.size.w = bitmap_data->width;
  bitmap->bounds.size.h = bitmap_data->height;

  bitmap->info.format = gbitmap_get_format(bitmap);

  if (gbitmap_get_palette_size(gbitmap_get_format(bitmap)) > 0) {
    PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk());
    // Palette is positioned right after the pixel data
    bitmap->palette = (GColor*)(bitmap_data->data +
        (bitmap->row_size_bytes * bitmap->bounds.size.h));
    // Don't flag this as heap allocated, as it gets freed along with pixel data
    bitmap->info.is_palette_heap_allocated = false;
  }

  bitmap->addr = bitmap_data->data;

  // Anything (not Legacy2) being loaded in this manner is being converted to the latest version.
  prv_init_gbitmap_version(bitmap);
}

GBitmap* gbitmap_create_with_data(const uint8_t *data) {
  GBitmap* bitmap = prv_allocate_gbitmap();
  if (bitmap) {
    gbitmap_init_with_data(bitmap, data);
  }
  return bitmap;
}

void gbitmap_init_as_sub_bitmap(GBitmap *sub_bitmap, const GBitmap *base_bitmap, GRect sub_rect) {
  if (gbitmap_get_version(base_bitmap) == GBITMAP_VERSION_0) {
    GBitmapLegacy2 *legacy_bitmap = (GBitmapLegacy2 *) sub_bitmap;
    *legacy_bitmap = *(GBitmapLegacy2 *) base_bitmap;
    // it's the responsibility of the parent bitmap to free the underlying data
    legacy_bitmap->is_heap_allocated = false;
  } else {
    *sub_bitmap = *base_bitmap;
    // it's the responsibility of the parent bitmap to free the underlying data and palette
    sub_bitmap->info.is_palette_heap_allocated = false;
    sub_bitmap->info.is_bitmap_heap_allocated = false;
  }
  grect_clip(&sub_rect, &base_bitmap->bounds);
  sub_bitmap->bounds = sub_rect;
}

GBitmap* gbitmap_create_as_sub_bitmap(const GBitmap *base_bitmap, GRect sub_rect) {
  GBitmap *bitmap = prv_allocate_gbitmap();
  if (bitmap) {
    gbitmap_init_as_sub_bitmap(bitmap, base_bitmap, sub_rect);
  }
  return bitmap;
}

static GColor* prv_allocate_palette(GBitmapFormat format) {
  PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk());
  GColor *palette = NULL;
  uint8_t palette_size = gbitmap_get_palette_size(format);
  if (palette_size > 0) {
    palette = applib_zalloc(palette_size * sizeof(GColor));
  }
  return palette;
}

#define BITMAP_FORMAT_IS_CIRCULAR_FULL_SCREEN(size, format) \
  ((format) == GBitmapFormat8BitCircular && (size).w == DISP_COLS && (size).h == DISP_ROWS)

T_STATIC size_t prv_gbitmap_size_for_data(GSize size, GBitmapFormat format) {
#if PLATFORM_SPALDING
  if (BITMAP_FORMAT_IS_CIRCULAR_FULL_SCREEN(size, format)) {
    return DISPLAY_FRAMEBUFFER_BYTES;
  }
#endif
  return gbitmap_format_get_row_size_bytes(size.w, format) * size.h;
}

static bool prv_gbitmap_allocate_data_for_size(GBitmap *bitmap, GSize size, GBitmapFormat format) {
  if (!bitmap) {
    return false;
  }

  bitmap->row_size_bytes = gbitmap_format_get_row_size_bytes(size.w, format);
  bitmap->bounds.size.w = size.w;
  bitmap->bounds.size.h = size.h;
  prv_init_gbitmap_version(bitmap);
  bitmap->info.format = format;

  const size_t data_size = prv_gbitmap_size_for_data(size, format);
  bitmap->addr = applib_zalloc(data_size);
  if (bitmap->addr) {
    bitmap->info.is_bitmap_heap_allocated = true;
    return true;
  }

  return false;
}

static GBitmap* prv_gbitmap_create_blank(GSize size, GBitmapFormat format) {
  GBitmap *bitmap = prv_allocate_gbitmap();
  if (bitmap) {
    if (!prv_gbitmap_allocate_data_for_size(bitmap, size, format)) {
      applib_free(bitmap);
      return NULL;
    }

#ifdef PLATFORM_SPALDING
    if (BITMAP_FORMAT_IS_CIRCULAR_FULL_SCREEN(size, format)) {
      bitmap->data_row_infos = g_gbitmap_spalding_data_row_infos;
    }
#endif
  }

  return bitmap;
}

static bool prv_platform_supports_format(GSize size, GBitmapFormat format) {
  switch (format) {
#if PBL_BW
    case GBitmapFormat1Bit:
    case GBitmapFormat1BitPalette:
    case GBitmapFormat2BitPalette:
      return true;
#elif PBL_COLOR && PBL_RECT
    case GBitmapFormat1Bit:
    case GBitmapFormat8Bit:
    case GBitmapFormat1BitPalette:
    case GBitmapFormat2BitPalette:
    case GBitmapFormat4BitPalette:
      return true;
#elif PBL_COLOR && PBL_ROUND
    case GBitmapFormat1Bit:
    case GBitmapFormat8Bit:
    case GBitmapFormat1BitPalette:
    case GBitmapFormat2BitPalette:
    case GBitmapFormat4BitPalette:
      return true;
    case GBitmapFormat8BitCircular:
      return BITMAP_FORMAT_IS_CIRCULAR_FULL_SCREEN(size, format);
#endif
    default:
      return false;
  }
}

static bool prv_is_palettized_format(GBitmapFormat format) {
  return format >= GBitmapFormat1BitPalette && format <= GBitmapFormat4BitPalette;
}

T_STATIC GBitmap *prv_gbitmap_create_blank_internal_no_platform_checks(GSize size,
                                                                       GBitmapFormat format) {
  GBitmap* bitmap = prv_gbitmap_create_blank(size, format);

  // If bitmap allocated and format requires a palette
  if (bitmap && prv_is_palettized_format(format)) {
    bitmap->palette = prv_allocate_palette(format);
    if (bitmap->palette) {
      bitmap->info.is_palette_heap_allocated = true;
    } else {
      gbitmap_destroy(bitmap);
      bitmap = NULL;
    }
  }

  return bitmap;
}

GBitmap* gbitmap_create_blank(GSize size, GBitmapFormat format) {
  if (process_manager_compiled_with_legacy2_sdk() && format != GBitmapFormat1Bit) {
    return NULL;
  }

  if (!prv_platform_supports_format(size, format)) {
    return NULL;
  }

  return prv_gbitmap_create_blank_internal_no_platform_checks(size, format);
}

GBitmapLegacy2* gbitmap_create_blank_2bit(GSize size) {
  return (GBitmapLegacy2 *) gbitmap_create_blank(size, GBitmapFormat1Bit);
}

GBitmap* gbitmap_create_blank_with_palette(GSize size, GBitmapFormat format,
    GColor *palette, bool free_on_destroy) {
  PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk());

  if (!prv_platform_supports_format(size, format)) {
    return NULL;
  }

  if (!prv_is_palettized_format(format)) {
    return NULL;
  }

  GBitmap *bitmap = prv_gbitmap_create_blank(size, format);
  if (bitmap) {
    gbitmap_set_palette(bitmap, palette, free_on_destroy);
  }

  return bitmap;
}

// Adapted from http://aggregate.org/MAGIC/#Bit%20Reversal
T_STATIC uint8_t prv_byte_reverse(uint8_t b) {
  b = (b & 0xaa) >> 1 | (b & 0x55) << 1;
  b = (b & 0xcc) >> 2 | (b & 0x33) << 2;
  b = (b & 0xf0) >> 4 | (b & 0x0f) << 4;
  return b;
}

GBitmap* gbitmap_create_palettized_from_1bit(const GBitmap *src_bitmap) {
  PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk());
  GBitmap *bitmap = NULL;
  if (src_bitmap && gbitmap_get_format(src_bitmap) == GBitmapFormat1Bit) {
    // Allocate the full size of the image up until the end of the bounds.
    // This eliminates edge cases where the bounds may start within a byte,
    // and not enough space would be allocated. This allows us to do all copying
    // from { 0, 0 } and simplifies copy.
    GSize size = (GSize) {
      .w = src_bitmap->bounds.size.w + src_bitmap->bounds.origin.x,
      .h = src_bitmap->bounds.size.h + src_bitmap->bounds.origin.y
    };
    bitmap = gbitmap_create_blank(size, GBitmapFormat1BitPalette);
    if (bitmap) {
      // Perform conversion
      uint8_t *src_data = (uint8_t *)src_bitmap->addr;
      uint8_t *dest_data = (uint8_t *)bitmap->addr;
      for (int y = 0; y < bitmap->bounds.size.h; ++y) {
        for (int b = 0; b < bitmap->row_size_bytes; ++b) {
          int dest_idx = y * bitmap->row_size_bytes + b;
          int src_idx = y * src_bitmap->row_size_bytes + b;
          dest_data[dest_idx] = prv_byte_reverse(src_data[src_idx]);
        }
      }
      bitmap->bounds = src_bitmap->bounds;
      bitmap->palette[0] = GColorBlack;
      bitmap->palette[1] = GColorWhite;
    }
  }
  return bitmap;
}

bool gbitmap_init_with_resource(GBitmap* bitmap, uint32_t resource_id) {
  ResAppNum app_resource_bank = sys_get_current_resource_num();
  return gbitmap_init_with_resource_system(bitmap, app_resource_bank, resource_id);
}

GBitmap *gbitmap_create_with_resource(uint32_t resource_id) {
  ResAppNum app_num = sys_get_current_resource_num();
  return gbitmap_create_with_resource_system(app_num, resource_id);
}

GBitmap *gbitmap_create_with_resource_system(ResAppNum app_num, uint32_t resource_id) {
  GBitmap *bitmap = prv_allocate_gbitmap();
  if (!bitmap) {
    return NULL;
  }

  if (!gbitmap_init_with_resource_system(bitmap, app_num, resource_id)) {
    applib_free(bitmap);
    return NULL;
  }

  return bitmap;
}

static bool prv_init_with_pbi_data(GBitmap *bitmap, uint8_t *data, size_t data_size,
                                   bool is_builtin) {
  // Initialize our metadata
  gbitmap_init_with_data(bitmap, data);
  if (is_builtin) {
    // for builtin resources, we don't do the extra bitmap manipulation below.
    return true;
  }

  // Verify the metadata is valid
  const GBitmapFormat format = gbitmap_get_format(bitmap);
  const size_t addr_offset = offsetof(BitmapData, data);
  const uint32_t pixel_data_bytes = bitmap->row_size_bytes * bitmap->bounds.size.h;
  const uint32_t required_total_size_bytes =
      addr_offset + // header size
      pixel_data_bytes  + // pixel data
      gbitmap_get_palette_size(format); // palette data

  const uint32_t required_row_size_bits =
      (bitmap->bounds.size.w * gbitmap_get_bits_per_pixel(format));
  // Convert from 8 bits in a byte, taking care to round up to the next whole byte.
  const uint32_t required_row_size_bytes = (required_row_size_bits + 7) / 8;

  if (data_size != required_total_size_bytes ||
      required_row_size_bytes > bitmap->row_size_bytes) {
    PBL_LOG(LOG_LEVEL_WARNING, "Bitmap metadata is inconsistent! data_size %u",
            (unsigned int) data_size);
    PBL_LOG(LOG_LEVEL_WARNING, "format %u row_size_bytes %"PRIu16" width %"PRId16" height %"PRId16,
            format, bitmap->row_size_bytes, bitmap->bounds.size.w, bitmap->bounds.size.h);
    return false;
  }


  // Move the actual pixel data up to the front of the buffer.
  // This way bitmap->addr points to the start of the buffer and can be directly freed.
  memmove(data, data + addr_offset, data_size - addr_offset);
  bitmap->addr = data;
  bitmap->info.is_bitmap_heap_allocated = true;

  // Move where the palette now points to, palette is positioned right after the pixel data
  if (gbitmap_get_palette_size(format) > 0) {
    bitmap->palette = (GColor*)((uint8_t*)bitmap->addr +
        (bitmap->row_size_bytes * bitmap->bounds.size.h));
  }

  return true;
}

bool gbitmap_init_with_resource_system(GBitmap* bitmap, ResAppNum app_num, uint32_t resource_id) {
  if (!bitmap) {
    return false;
  }

  memset(bitmap, 0, prv_gbitmap_size());

  const size_t data_size = sys_resource_size(app_num, resource_id);
  uint8_t *data = applib_resource_mmap_or_load(app_num, resource_id, 0, data_size, false);
  if (!data) {
    return false;
  }

  // Scan the resource data to see if it contains PNG data
  if (gbitmap_png_data_is_png(data, data_size)) {
    const bool result = gbitmap_init_with_png_data(bitmap, data, data_size);
    // the actual pixels live uncompressed on the heap now, we can free the PNG data
    applib_resource_munmap_or_free(data);
    return result;
  }

  const bool mmapped = applib_resource_is_mmapped(data);
  if (prv_init_with_pbi_data(bitmap, data, data_size, mmapped)) {
    // in order to make memory-mapped bitmaps work, we need to decrement the reference counter
    // when we destroy it. This case is different from a sub-bitmap that shares the bitmap
    // data. We use .is_bitmap_heap_allocated=true here so that bitmap_deinit() can take care
    // of it.
    // As the pixel data is either memory-mapped or heap-allocated we always say "true"
    bitmap->info.is_bitmap_heap_allocated = true;
    return true;
  } else {
    applib_resource_munmap_or_free(data);
    return false;
  }
}

uint16_t gbitmap_get_bytes_per_row(const GBitmap *bitmap) {
  if (!bitmap) {
    return 0;
  }
  return bitmap->row_size_bytes;
}

static bool prv_gbitmap_is_context(const GBitmap *bitmap) {
  return (bitmap->addr == graphics_context_get_bitmap(app_state_get_graphics_context())->addr);
}

GBitmapFormat gbitmap_get_format(const GBitmap *bitmap) {
  if (!bitmap) {
    return GBitmapFormat1Bit;
  }

  if (process_manager_compiled_with_legacy2_sdk() ||
      gbitmap_get_version(bitmap) == GBITMAP_VERSION_0) {
    // If the bitmap is from the graphics context, return its format
    // otherwise return the Legacy2 default 1-Bit format
    // to support legacy applications that mis-set the format flags
    return (prv_gbitmap_is_context(bitmap)) ? bitmap->info.format : GBitmapFormat1Bit;
  }
  return bitmap->info.format;
}

uint8_t* gbitmap_get_data(const GBitmap *bitmap) {
  if (!bitmap) {
    return NULL;
  }
  return bitmap->addr;
}

void gbitmap_set_data(GBitmap *bitmap, uint8_t *data, GBitmapFormat format,
                      uint16_t row_size_bytes, bool free_on_destroy) {
  if (bitmap) {
    bitmap->addr = data;
    bitmap->info.format = format;
    bitmap->row_size_bytes = row_size_bytes;
    bitmap->info.is_bitmap_heap_allocated = free_on_destroy;
  }
}

GColor* gbitmap_get_palette(const GBitmap *bitmap) {
  PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk());
  if (!bitmap) {
    return NULL;
  }
  return bitmap->palette;
}

void gbitmap_set_palette(GBitmap *bitmap, GColor *palette, bool free_on_destroy) {
  PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk());
  if (bitmap && palette) {
    if (gbitmap_get_info(bitmap).is_palette_heap_allocated) {
      applib_free(bitmap->palette);
    }
    bitmap->palette = palette;
    bitmap->info.is_palette_heap_allocated = free_on_destroy;
  }
}

GRect gbitmap_get_bounds(const GBitmap *bitmap) {
  if (!bitmap) {
    return GRectZero;
  }
  return bitmap->bounds;
}

void gbitmap_set_bounds(GBitmap *bitmap, GRect bounds) {
  if (bitmap) {
    bitmap->bounds = bounds;
  }
}

void gbitmap_deinit(GBitmap* bitmap) {
  if (gbitmap_get_info(bitmap).is_bitmap_heap_allocated) {
    applib_resource_munmap_or_free(bitmap->addr);
  }
  bitmap->addr = NULL;

  if (!process_manager_compiled_with_legacy2_sdk()) {
    if (gbitmap_get_info(bitmap).is_palette_heap_allocated) {
      applib_free(bitmap->palette);
    }
    bitmap->palette = NULL;
  }
}

void gbitmap_destroy(GBitmap* bitmap) {
  if (!bitmap) {
    return;
  }
  gbitmap_deinit(bitmap);
  applib_free(bitmap);
}
