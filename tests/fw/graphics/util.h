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

#pragma once

#include "test_graphics.h"
#include "applib/graphics/gbitmap_png.h"
#include "util/graphics.h"
#include "util/math.h"

#include "clar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/wait.h>

#define PATH_STRING_LENGTH 512

extern GBitmapDataRowInfo prv_gbitmap_get_data_row_info(const GBitmap *bitmap, uint16_t y);

#include "util_pbi.h"

// The following macros append the ~platform (unless default) and the filetype extension
// unless file_name contains Xbit, then appends .(native_bitdepth)bit and filetype extension
#define TEST_NAMED_PBI_FILE(file_name) namecat(file_name, ".pbi")
#define TEST_NAMED_PNG_FILE(file_name) namecat(file_name, ".png")

// The following macros create a file name based on the function name plus extension
#define TEST_PBI_FILE namecat(__func__, ".pbi")
#define TEST_PBI_FILE_FMT(fmt) namecat(__func__, "." #fmt ".pbi")
#define TEST_PNG_FILE namecat(__func__, ".png")
#define TEST_PNG_FILE_FMT(fmt) namecat(__func__, "." #fmt ".png")
#define TEST_PDC_FILE namecat(__func__, ".pdc")
#define TEST_PDC_PBI_FILE namecat(__func__, ".pdc.pbi")
#define TEST_APNG_FILE namecat(__func__, ".apng")
#define TEST_PBI_FILE_X(x) namecat(__func__, "_" #x ".pbi")

bool tests_write_gbitmap_to_pbi(GBitmap *bmp, const char *filename) {
  char full_path[PATH_STRING_LENGTH];
  snprintf(full_path, sizeof(full_path), "%s/%s", TEST_OUTPUT_PATH, filename);
  return write_gbitmap_to_pbi(bmp, full_path, PBI2PNG_EXE);
}

// Used to work around __func__ not being a string literal (necessary for macro concatenation)
static const char *namecat(const char* str1, const char* str2){
  char *filename = malloc(PATH_STRING_LENGTH);
  filename[0] = '\0';
  strcat(filename, str1);

  char *filename_xbit = strstr(filename, ".Xbit");

  if (filename_xbit) {
    // Support using ".Xbit" for the native bitdepth
    filename_xbit[1] = (SCREEN_COLOR_DEPTH_BITS == 8) ? '8' : '1';
    printf("filename and filename_xbit %s : %s\n", filename, filename_xbit);
  } else {
#if !PLATFORM_DEFAULT
    // Add ~platform to files with unit-tests built for a specific platform
    strcat(filename, "~");
    strcat(filename, PLATFORM_NAME);
#endif
  }

  strcat(filename, str2);
  return filename;
}

static char get_terminal_color(uint8_t c) {
  switch (c) {
    case GColorBlackARGB8: // black
      return 'B';
    case GColorWhiteARGB8: // white
      return 'W';
    case GColorRedARGB8: // red
      return 'R';
    case GColorGreenARGB8: // green
      return 'G';
    case GColorBlueARGB8: // blue
      return 'b';
    default:
      return ' ';
  }
}

// A simple function for printing 8-bit gbitmaps to the console.
// Makes it easy to quickly review failing test cases.
void print_bitmap(const GBitmap *bmp) {
  printf("Row Size Bytes: %d\n", bmp->row_size_bytes);
  printf("Bounds: ");
  printf(GRECT_PRINTF_FORMAT, GRECT_PRINTF_FORMAT_EXPLODE(bmp->bounds));
  printf("\n");

  GSize size = bmp->bounds.size;
  uint8_t *data = (uint8_t *)bmp->addr;
  // Build a coordinate system, 3 rows up top for the col number
  for (uint8_t y = 0; y < 3; ++y) {
    printf("\t"); // leave space for row #
    for (uint8_t x = 0; x < size.w; ++x) {
      int num = -1;
      switch (y) {
        case 0: // hundreds
          if (x < 100) break;
          num = (x / 100) % 10;
          break;
        case 1: // tens
          if (x < 10) break;
          num = (x / 10) % 10;
          break;
        case 2: // ones
          num = x % 10;
          break;
      }
      if (num < 0) {
        printf(" ");
      } else {
        printf("%d", num);
      }
    }
    printf("\n");
  }
  const uint8_t start_x = bmp->bounds.origin.x;
  const uint8_t end_x = start_x + bmp->bounds.size.w;
  const uint8_t start_y = bmp->bounds.origin.y;
  const uint8_t end_y = start_y + bmp->bounds.size.h;
  for (uint8_t y = start_y; y < end_y; ++y) {
    printf("\n%d\t", y);
    for (uint8_t x = start_x; x < end_x; ++x) {
      uint8_t color = data[y * bmp->row_size_bytes + x];
      printf("%c", get_terminal_color(color));
    }
  }
  printf("\n\n\n\n");
}

// Will load the PBI at location $TEST_IMAGES_PATH/filename into a gbitmap.
GBitmap *get_gbitmap_from_pbi(const char *filename) {
  char full_path[PATH_STRING_LENGTH];
  snprintf(full_path, PATH_STRING_LENGTH, "%s/%s", TEST_IMAGES_PATH, filename);
 
  // Support using ".Xbit" for the native bitdepth
  char *filename_xbit = strstr(full_path, ".Xbit");
  if (filename_xbit) {
    filename_xbit[1] = (SCREEN_COLOR_DEPTH_BITS == 8) ? '8' : '1';
  }

  FILE *file = fopen(full_path, "r");
  if (!file) {
    printf("Unable to open file: %s\n", full_path);
    return NULL;
  }

  GBitmap *bmp = malloc(sizeof(*bmp));
  *bmp = (GBitmap){0};

  // Read bitmap header
  fread(&bmp->row_size_bytes, sizeof(bmp->row_size_bytes), 1, file);
  fread(&bmp->info_flags, sizeof(bmp->info_flags), 1, file);
  fread(&bmp->bounds, sizeof(bmp->bounds), 1, file);

  size_t data_size = bmp->row_size_bytes * bmp->bounds.size.h;

  bmp->addr = malloc(data_size);
  bmp->info.is_bitmap_heap_allocated = true;
  fread(bmp->addr, 1, data_size, file);

  uint8_t palette_size = gbitmap_get_palette_size(gbitmap_get_format(bmp));
  if (palette_size > 0) {
    // Allocate palette of GColor8 entries in ARGB8 format
    bmp->palette = malloc(palette_size * sizeof(GColor8));
    fread(bmp->palette, 1, palette_size * sizeof(GColor8), file);
  }

  fclose(file);

  return bmp;
}

#define ACTUAL_PBI_FILE_EXTENSION "-actual.pbi"
#define EXPECTED_PBI_FILE_EXTENSION "-expected.pbi"
#define DIFF_PBI_FILE_EXTENSION "-diff.pbi"
#define DIFF_COLOR GColorMagenta

static GColor8 prv_convert_to_gcolor8(GBitmapFormat format, uint8_t raw_value, GColor *palette) {
  uint8_t color8 = raw_value;
  switch (format) {
    case GBitmapFormat1Bit:
      color8 = (raw_value) ? GColorWhite.argb : GColorBlack.argb;
      break;
    case GBitmapFormat1BitPalette:
    case GBitmapFormat2BitPalette:
    case GBitmapFormat4BitPalette:
      color8 = palette[raw_value].argb;
      break;
    case GBitmapFormat8Bit:
    default:
      break;
  }
  return (GColor8)color8;
}

static uint8_t prv_raw_image_get_value_for_format(const uint8_t *raw_image_buffer,
    uint32_t x, uint32_t y, uint16_t row_stride_bytes, uint8_t bitdepth, GBitmapFormat format) {
  if (format == GBitmapFormat1Bit){
    // Retrieve the byte from the image buffer containing the requested pixel
    uint32_t pixel_in_byte = raw_image_buffer[y * row_stride_bytes + (x / 8)];
    // Find the index of the pixel in terms of coordinates and aligned_width
    uint32_t pixel_index =  y * (row_stride_bytes * 8) + x;
    // Shift and mask the requested pixel data from the byte containing it and return
    return (uint8_t)(pixel_in_byte >> ((pixel_index % 8)) & 1);
  } else {
    return raw_image_get_value_for_bitdepth(raw_image_buffer, x, y, row_stride_bytes, bitdepth);
  }
}

static void prv_write_diff_to_file(const char *filename, GBitmap *expected_bmp,
                                   GBitmap *actual_bmp, GBitmap *diff_bmp) {
  // Write the expected output to filename-expected.png
  char bmp_filename[PATH_STRING_LENGTH];
  if (expected_bmp) {
    strncpy(bmp_filename, filename, PATH_STRING_LENGTH - strlen(EXPECTED_PBI_FILE_EXTENSION) - 1);
    char *ext = strrchr(bmp_filename, '.');
    strncpy(ext, EXPECTED_PBI_FILE_EXTENSION, strlen(EXPECTED_PBI_FILE_EXTENSION) + 1);
    cl_assert(tests_write_gbitmap_to_pbi(expected_bmp, bmp_filename));

    // TODO: PBL-20932 Add 1-bit and palettized support
    if (actual_bmp->info.format == GBitmapFormat8Bit && diff_bmp) {
      // Only write the diff file if there is an expected image
      strncpy(bmp_filename, filename, PATH_STRING_LENGTH - strlen(DIFF_PBI_FILE_EXTENSION) - 1);
      ext = strrchr(bmp_filename, '.');
      strncpy(ext, DIFF_PBI_FILE_EXTENSION, strlen(DIFF_PBI_FILE_EXTENSION) + 1);
      cl_assert(tests_write_gbitmap_to_pbi(diff_bmp, bmp_filename));
    }
  }

  // Write the actual output to the filename-actual.png
  strncpy(bmp_filename, filename, PATH_STRING_LENGTH - strlen(ACTUAL_PBI_FILE_EXTENSION) - 1);
  char *ext = strrchr(bmp_filename, '.');
  strncpy(ext, ACTUAL_PBI_FILE_EXTENSION, strlen(ACTUAL_PBI_FILE_EXTENSION) + 1);
  cl_assert(tests_write_gbitmap_to_pbi(actual_bmp, bmp_filename));
}

// declared in gbitmap.c
GBitmap *prv_gbitmap_create_blank_internal_no_platform_checks(GSize size, GBitmapFormat format);


// Compare two bitmap and return whether or not they are the same
// Note that if both passed bitmaps are NULL, this test will succeed!
bool gbitmap_eq(GBitmap *actual_bmp, GBitmap *expected_bmp, const char *filename) {
  bool rc = false;
  GBitmap *diff_bmp = NULL;
  if (!actual_bmp && !expected_bmp) {
    return true;
  } else if (!actual_bmp || !expected_bmp) {
    goto done;
  }

  if (!grect_equal(&actual_bmp->bounds, &expected_bmp->bounds)) {
    printf("Unmatched bounds\n");
    printf("\tExpected: ");
    printf(GRECT_PRINTF_FORMAT, GRECT_PRINTF_FORMAT_EXPLODE(expected_bmp->bounds));
    printf("\n\tGot: ");
    printf(GRECT_PRINTF_FORMAT, GRECT_PRINTF_FORMAT_EXPLODE(actual_bmp->bounds));
    goto done;
  }

  uint8_t actual_bmp_palette_size = gbitmap_get_palette_size(gbitmap_get_format(actual_bmp));
  uint8_t expected_bmp_palette_size = gbitmap_get_palette_size(gbitmap_get_format(expected_bmp));

  uint8_t *expected_bmp_data = (uint8_t *)expected_bmp->addr;

  uint8_t actual_bmp_bpp = gbitmap_get_bits_per_pixel(gbitmap_get_format(actual_bmp));
  uint8_t expected_bmp_bpp = gbitmap_get_bits_per_pixel(gbitmap_get_format(expected_bmp));

  const int16_t start_y = actual_bmp->bounds.origin.y;
  const int16_t end_y = start_y + actual_bmp->bounds.size.h;
  rc = true;

  // Create a bitmap for the diff image - force 8-bit
  // The diff image contains first the actual image, then the diff image, and then the expected image
  // These images are separated by one pixel column (transparent)
  GSize diff_bmp_size = actual_bmp->bounds.size;
  diff_bmp_size.w = (3 * diff_bmp_size.w) + 2; // 2 pixels to divide the three images
  diff_bmp = prv_gbitmap_create_blank_internal_no_platform_checks(diff_bmp_size, GBitmapFormat8Bit);
  if (!diff_bmp) {
    printf("Unable to create diff bitmap\n");
    rc = false;
    goto done;
  }

  for (int y = start_y; y < end_y; ++y) {
    uint8_t *line = ((uint8_t*)diff_bmp->addr) + (diff_bmp->row_size_bytes * y);

    // TODO: PBL-20932 Add 1-bit and palettized support
    if (actual_bmp->info.format == GBitmapFormat8Bit) {
      line[(diff_bmp->row_size_bytes / 3) + 1] = GColorClear.argb;     // Separator pixel between images
      line[(2 * diff_bmp->row_size_bytes / 3) + 1] = GColorClear.argb; // Separator pixel between images
    }

    // Needs to be prv_gbitmap_get_data_row_info to avoid unit test mocked version
    const GBitmapDataRowInfo dest_row_info = prv_gbitmap_get_data_row_info(actual_bmp, y);
    const int16_t start_x = MAX(actual_bmp->bounds.origin.x, dest_row_info.min_x);
    const int16_t end_x = MIN(grect_get_max_x(&actual_bmp->bounds), dest_row_info.max_x + 1);
    const int16_t y_line = 0; // line is constant zero below now that we are retrieving row
    if (end_x < start_x) {
      continue;
    }

    for (int x = start_x; x < end_x; ++x) {
      uint8_t *actual_bmp_data = dest_row_info.data;
      uint8_t actual_bmp_val = prv_raw_image_get_value_for_format(actual_bmp_data, x, y_line,
                                                                  actual_bmp->row_size_bytes,
                                                                  actual_bmp_bpp,
                                                                  actual_bmp->info.format);
      uint8_t expected_bmp_val = prv_raw_image_get_value_for_format(expected_bmp_data, x, y,
                                                                    expected_bmp->row_size_bytes,
                                                                    expected_bmp_bpp,
                                                                    expected_bmp->info.format);
      GColor8 actual_bmp_color = prv_convert_to_gcolor8(actual_bmp->info.format,
                                                        actual_bmp_val, actual_bmp->palette);
      GColor8 expected_bmp_color = prv_convert_to_gcolor8(expected_bmp->info.format,
                                                          expected_bmp_val, expected_bmp->palette);

      if (!gcolor_equal(actual_bmp_color, expected_bmp_color)) {
        if (rc) {
          // Only print out the first mismatch
          printf("Mismatch at x: %d y: %d\n", x, y);
          printf("value for end_x was:%d\n", end_x);
          printf("format was %d\n", actual_bmp->info.format);
        }
        rc = false;
      }

      // TODO: PBL-20932 Add 1-bit and palettized support
      if (actual_bmp->info.format == GBitmapFormat8Bit) {
        if (actual_bmp_color.argb != expected_bmp_color.argb) {
          GColor8 diff_bmp_color = DIFF_COLOR;
          line[(diff_bmp->row_size_bytes / 3) + x + 1] = diff_bmp_color.argb;
        } else {
          line[(diff_bmp->row_size_bytes / 3) + x + 1] = actual_bmp_color.argb;
        }

        // Fill in the actual and expected pixels on either side of the diff image
        line[x] = actual_bmp_color.argb;
        line[(2 * diff_bmp->row_size_bytes / 3) + x + 1] = expected_bmp_color.argb;
      }
    }
  }

done:
  if (!rc) {
    prv_write_diff_to_file(filename, expected_bmp, actual_bmp, diff_bmp);
  }
  gbitmap_destroy(diff_bmp);
  return rc;
}

// Compare the given gbitmap to a gbmitmap loaded from a PBI in the filename given.
bool gbitmap_pbi_eq_with_bounds(GBitmap *bmp, const char *filename, const GRect *bounds) {
  GBitmap *pbi_bmp = get_gbitmap_from_pbi(filename);
  if (pbi_bmp && bounds) {
    pbi_bmp->bounds = *bounds;
  }
  bool rc = gbitmap_eq(bmp, pbi_bmp, filename);
  gbitmap_destroy(pbi_bmp);
  return rc;
}

bool gbitmap_pbi_eq(GBitmap *bmp, const char *filename) {
  return gbitmap_pbi_eq_with_bounds(bmp, filename, NULL);
}

size_t load_file(const char* filename, uint8_t** data) {
  char full_path[PATH_STRING_LENGTH];
  snprintf(full_path, sizeof(full_path), "%s/%s", TEST_IMAGES_PATH, filename);

  FILE *file = fopen(full_path,"rb");
  if(file == NULL){
    printf("Error: couldn't open file: %s\n", filename);
    cl_assert(false);
  }
  fseek(file,0,SEEK_END);
  int data_size = ftell(file);
  fseek(file,0,SEEK_SET);
  *data = (unsigned char*)malloc(data_size);
  fread(*data,1, data_size, file);
  fclose(file);
  return data_size;
}

// Mask flags to indicate what to setup in the draw_state of GContext within setup_test_context
#define CTX_FLAG_DS_ALL               0x00000010
#define CTX_FLAG_DS_CLIP_BOX          0x00000020
#define CTX_FLAG_DS_DRAWING_BOX       0x00000040
#define CTX_FLAG_DS_STROKE_COLOR      0x00000080
#define CTX_FLAG_DS_FILL_COLOR        0x00000100
#define CTX_FLAG_DS_TEXT_COLOR        0x00000200
#define CTX_FLAG_DS_COMPOSITING_MODE  0x00000400
#define CTX_FLAG_DS_ANTIALIASED       0x00000800
#define CTX_FLAG_DS_STROKE_WIDTH      0x00001000
void setup_test_context(GContext* ctx, uint32_t flags, GDrawState *draw_state, bool *lock) {
  if (draw_state) {
    if (flags & CTX_FLAG_DS_CLIP_BOX) {
      ctx->draw_state.clip_box = draw_state->clip_box;
    }
    if (flags & CTX_FLAG_DS_DRAWING_BOX) {
      ctx->draw_state.drawing_box = draw_state->drawing_box;
    }
    if (flags & CTX_FLAG_DS_STROKE_COLOR) {
      graphics_context_set_stroke_color(ctx, draw_state->stroke_color);
    }
    if (flags & CTX_FLAG_DS_FILL_COLOR) {
      graphics_context_set_fill_color(ctx, draw_state->fill_color);
    }
    if (flags & CTX_FLAG_DS_TEXT_COLOR) {
      graphics_context_set_text_color(ctx, draw_state->text_color);
    }
    if (flags & CTX_FLAG_DS_COMPOSITING_MODE) {
      graphics_context_set_compositing_mode(ctx, draw_state->compositing_mode);
    }
    if (flags & CTX_FLAG_DS_ANTIALIASED) {
#if PBL_COLOR
      graphics_context_set_antialiased(ctx, draw_state->antialiased);
#endif
    }
    if (flags & CTX_FLAG_DS_STROKE_WIDTH) {
      graphics_context_set_stroke_width(ctx, draw_state->stroke_width);
    }
  }

  if (lock) {
    ctx->lock = lock;
  }
}

GBitmap* setup_pbi_test(const char *filename) {
  uint8_t *pbi_data = NULL;
  size_t pbi_size = 0;
  pbi_size = load_file(filename, &pbi_data);
  cl_assert(pbi_size > 0);
  cl_assert(pbi_data);
  return gbitmap_create_with_data(pbi_data);
}

static GBitmap* setup_png_test(const char *filename) {
  uint8_t *png_data = NULL;
  size_t png_size = 0;
  png_size = load_file(filename, &png_data);
  cl_assert(png_size > 0);
  cl_assert(png_data);
  return gbitmap_create_from_png_data(png_data, png_size);
}

void setup_test_aa_sw(GContext *ctx, FrameBuffer *fb, GRect clip_box, GRect drawing_box,
                             bool antialiased, uint8_t stroke_width) {
  test_graphics_context_reset(ctx, fb);

  GDrawState draw_state = {
    .clip_box = clip_box,
    .drawing_box = drawing_box,
#if PBL_COLOR
    .antialiased = antialiased,
#endif
    .stroke_width = stroke_width
  };
  setup_test_context(ctx, 
                     (CTX_FLAG_DS_CLIP_BOX | CTX_FLAG_DS_DRAWING_BOX |
                      CTX_FLAG_DS_ANTIALIASED | CTX_FLAG_DS_STROKE_WIDTH),
                     &draw_state, NULL);
}

#if PLATFORM_SPALDING
bool gbitmap_8bit_to_8bit_circular(GBitmap *bitmap) {
  // Only allow conversion of 8Bit 180x180 rectangular bitmaps to circular
  if (!bitmap || (gbitmap_get_format(bitmap) != GBitmapFormat8Bit) ||
      (bitmap->bounds.size.w != DISP_COLS) || (bitmap->bounds.size.h != DISP_COLS)) {
    return false;
  }
  // Using realloc or copying to a new buffer has high memory overhead
  // attempt to shuffle bytes in place to allow 3rd party watchapps use
  uint8_t *data = gbitmap_get_data(bitmap);
  
  // Convert format and link to data_row_infos table
  bitmap->info.format = GBitmapFormat8BitCircular;
  bitmap->data_row_infos = g_gbitmap_spalding_data_row_infos;

  for (uint32_t y = 0; y < DISP_ROWS; y++) {
    GBitmapDataRowInfo row_info = gbitmap_get_data_row_info(bitmap, y);
    // copy (using memmove for overlapping region) valid bytes between min_x and max_x
    memmove(&row_info.data[row_info.min_x], &data[y * DISP_COLS + row_info.min_x],
            row_info.max_x - row_info.min_x + 1);
  }
  return true;
}
#endif
