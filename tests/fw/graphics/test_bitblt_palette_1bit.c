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

#include "applib/graphics/graphics.h"
#include "applib/graphics/graphics_private.h"
#include "applib/graphics/bitblt.h"
#include "applib/graphics/bitblt_private.h"
#include "applib/graphics/1_bit/framebuffer.h"
#include "applib/graphics/gtypes.h"

#include "clar.h"
#include "util.h"

#include <string.h>
#include <stdio.h>

// Stubs
////////////////////////////////////
#include "graphics_common_stubs.h"
#include "stubs_applib_resource.h"
#include "test_graphics.h"

extern void prv_apply_tint_color(GColor *color, GColor tint_color);

static uint8_t s_dest_data[100 * 100];
static GBitmap s_dest_bitmap = {
    .addr = s_dest_data,
    .row_size_bytes = 16,
    .info.format = GBitmapFormat1Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = GRect(0, 0, 100, 100)
  };
// Tests
////////////////////////////////////

// setup and teardown
void test_bitblt_palette_1bit__initialize(void) {
  // Set dest bitmap to white
  memset(s_dest_data, 0b11111111, sizeof(s_dest_data));
}

void test_bitblt_palette_1bit__cleanup(void) {
}

// Test images reside in "tests/fw/graphics/test_images/".
// The wscript will convert them from PNGs in that directory to PBIs in the build directory.

// Tests assign, from same size to same size.
// Setup:
//   - Source is 50x50, the left half is semi transparent orange and the right half is orange.
//   - Dest is 100x100, white.
// Result:
//   - All dithered gray.
void test_bitblt_palette_1bit__1bit_palette_to_1bit_assign(void) {
  GBitmap *src_bitmap =
    get_gbitmap_from_pbi("test_bitblt_palette_1bit__1bit_palette_to_1bit.pbi");

  bitblt_bitmap_into_bitmap(&s_dest_bitmap, src_bitmap, GPointZero, GCompOpAssign, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&s_dest_bitmap,
                  "test_bitblt_palette_1bit__1bit_palette_to_1bit_assign-expect.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Test images reside in "tests/fw/graphics/test_images/".
// The wscript will convert them from PNGs in that directory to PBIs in the build directory.

// Tests assign, from same size to same size.
// Setup:
//   - Source is 50x50, the left half is semi transparent orange and the right half is orange.
//   - Dest is 100x100, white.
// Result:
//   - The left half will be white and the right half will be dithered gray.
void test_bitblt_palette_1bit__1bit_palette_to_1bit_set(void) {
  GBitmap *src_bitmap =
    get_gbitmap_from_pbi("test_bitblt_palette_1bit__1bit_palette_to_1bit.pbi");

  bitblt_bitmap_into_bitmap(&s_dest_bitmap, src_bitmap, GPointZero, GCompOpSet, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&s_dest_bitmap,
                  "test_bitblt_palette_1bit__1bit_palette_to_1bit_set-expect.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Tests assign, from same size to same size.
// Setup:
//   - Source is 50x50, alternating lines between orange and blue for the top half.
//     The bottom half is a diagonal orange line over blue.
//     The left half is semi-transparent and the right half is completely opaque
//   - Dest is 100x100, white.
// Result:
//   - The top half will be alternating between dithered gray and black lines
//     The bottom half consists of a diagonal white line  on a black background
void test_bitblt_palette_1bit__2bit_palette_to_1bit_assign(void) {
  GBitmap *src_bitmap =
    get_gbitmap_from_pbi("test_bitblt_palette_1bit__2bit_palette_to_1bit.pbi");

  bitblt_bitmap_into_bitmap(&s_dest_bitmap, src_bitmap, GPointZero, GCompOpAssign, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&s_dest_bitmap,
                  "test_bitblt_palette_1bit__2bit_palette_to_1bit-expect.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Tests assign, from same size to same size.
// Setup:
//   - Source is 50x50, alternating lines between orange and blue for the top half.
//     The bottom half is a diagonal orange line over blue.
//     The left half is semi-transparent and the right half is completely opaque
//   - Dest is 100x100, white.
// Result:
//   - The top right half will be alternating between dithered gray and black lines
//     The bottom right half consists of a diagonal white line  on a black background
//     The left half will be completely white
void test_bitblt_palette_1bit__2bit_palette_to_1bit_set(void) {
  GBitmap *src_bitmap =
    get_gbitmap_from_pbi("test_bitblt_palette_1bit__2bit_palette_to_1bit.pbi");

  bitblt_bitmap_into_bitmap(&s_dest_bitmap, src_bitmap, GPointZero, GCompOpSet, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&s_dest_bitmap,
                  "test_bitblt_palette_1bit__2bit_palette_to_1bit_set-expect.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Tests assign, from same size to same size.
// Setup:
//   - Source is 50x50, alternating lines between orange and blue for the top half.
//     The bottom half is a diagonal orange line over blue.
//     The left half is semi-transparent and the right half is completely opaque
//   - Dest is 50x50, white.
// Result:
//   - The image described will be tiled in each of the four corners
//     The top right half will be alternating between dithered gray and black lines
//     The bottom right half consists of a diagonal white line  on a black background
//     The left half will be completely white
void test_bitblt_palette_1bit__2bit_palette_to_1bit_wrap(void) {
  GBitmap *src_bitmap =
    get_gbitmap_from_pbi("test_bitblt_palette_1bit__2bit_palette_to_1bit.pbi");

  bitblt_bitmap_into_bitmap_tiled(&s_dest_bitmap, src_bitmap, s_dest_bitmap.bounds, GPointZero, GCompOpAssign, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&s_dest_bitmap,
                  "test_bitblt_palette_1bit__2bit_palette_to_1bit_wrap-expect.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Tests assign, from same size to same size.
// Setup:
//   - Source is 50x50, alternating lines between orange and blue for the top half.
//     The bottom half is a diagonal orange line over blue.
//     The left half is semi-transparent and the right half is completely opaque
//   - Dest is 100x100, white.
// Result:
//   - The image described below will be at an offset of 20, 20
//     The top right half will be alternating between dithered gray and black lines
//     The bottom right half consists of a diagonal white line  on a black background
//     The left half will be completely white
void test_bitblt_palette_1bit__2bit_palette_to_1bit_offset(void) {
  GBitmap *src_bitmap =
    get_gbitmap_from_pbi("test_bitblt_palette_1bit__2bit_palette_to_1bit.pbi");

  bitblt_bitmap_into_bitmap(&s_dest_bitmap, src_bitmap, GPoint(20,20), GCompOpAssign, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&s_dest_bitmap,
                  "test_bitblt_palette_1bit__2bit_palette_to_1bit_offset-expect.pbi"));

  gbitmap_destroy(src_bitmap);
}

void test_bitblt_palette_1bit__get_1bit_graphics_grayscale_pattern(void) {
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorWhite, 0), 0xFFFFFFFF);
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorWhite, 1), 0xFFFFFFFF);
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorLightGray, 0), 0x55555555);
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorLightGray, 1), 0xAAAAAAAA);
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorDarkGray, 0), 0x55555555);
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorDarkGray, 1), 0xAAAAAAAA);
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorBlack, 0), 0x00000000);
  cl_assert_equal_i(graphics_private_get_1bit_grayscale_pattern(GColorBlack, 1), 0x00000000);
}

void test_bitblt_palette_1bit__prv_apply_tint_color(void) {
  GColor color = GColorBlack;
  GColor tinted_color = GColorBlack;
  prv_apply_tint_color(&color, GColorClear);
  cl_assert_equal_i(color.argb, tinted_color.argb);
  tinted_color = GColorRed;
  prv_apply_tint_color(&color, GColorRed);
  cl_assert_equal_i(color.argb, tinted_color.argb);
  tinted_color.a = 2;
  color.a = 2;
  prv_apply_tint_color(&color, GColorRed);
  cl_assert_equal_i(color.argb, tinted_color.argb);
  tinted_color.a = 1;
  color.a = 1;
  prv_apply_tint_color(&color, GColorRed);
  cl_assert_equal_i(color.argb, tinted_color.argb);

}

