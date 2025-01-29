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
#include "applib/graphics/bitblt.h"
#include "applib/graphics/bitblt_private.h"
#include "applib/graphics/8_bit/framebuffer.h"

#include "clar.h"
#include "util.h"

#include <string.h>
#include <stdio.h>

// Stubs
////////////////////////////////////
#include "graphics_common_stubs.h"
#include "stubs_applib_resource.h"
#include "test_graphics.h"

static GContext ctx;
static FrameBuffer framebuffer;

// Helpers
////////////////////////////////////

static void prv_set_opacity(GBitmap *bmp, uint8_t opacity) {
  GRect bounds = bmp->bounds;
  for (uint32_t idx = 0; idx < bounds.size.w * bounds.size.h; idx++) {
    ((GColor *)(bmp->addr + idx))->a = opacity;
  }
}

static GColor prv_next_color(GColor color) {
  GColor8 result = (GColor8){.argb = (color.argb + 1) % 64};
  result.a = color.a;
  return result;
}

// Tests
////////////////////////////////////

// setup and teardown
void test_bitblt__initialize(void) {
  framebuffer_init(&framebuffer, &(GSize) {DISP_COLS, DISP_ROWS});
  test_graphics_context_init(&ctx, &framebuffer);
}

void test_bitblt__cleanup(void) {
}

// Test images reside in "tests/fw/graphics/test_images/".
// The wscript will convert them from PNGs in that directory to PBIs in the build directory.
// Naming conventions of these images tends to be '<test_name>.<bitdepth>.png'.
// For example:
//    test_bitblt__8bit_assign would have:
//      - test_bitblt__8bit_assign.8bit.png
//      - test_bitblt__8bit_assign-expect.8bit.png

// Tests assign, from same size to same size.
// Setup:
//   - Source is 10x10, white.
//   - Dest is 10x10, green.
// Result:
//   - All white.
void test_bitblt__8bit_compop(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__8bit_assign.8bit.pbi");
  uint8_t dest_data[src_bitmap->bounds.size.w*src_bitmap->bounds.size.h];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = src_bitmap->bounds.size.w,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = src_bitmap->bounds
  };
  // All compositing modes except GCompOpSet should be the same as GCompAssign
  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpAssign, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_assign-expect.8bit.pbi"));

  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpAssignInverted, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_assign-expect.8bit.pbi"));

  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpOr, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_assign-expect.8bit.pbi"));

  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpAnd, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_assign-expect.8bit.pbi"));

  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpClear, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_assign-expect.8bit.pbi"));

  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  // Update source bitmap to be semi-transparent
  uint8_t *src_bitmap_buff = src_bitmap->addr;
  for (int index = 0; index < src_bitmap->bounds.size.h * src_bitmap->row_size_bytes; index++) {
    src_bitmap_buff[index] = src_bitmap_buff[index] & 0xbf;
  }
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpSet, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_set-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Test GCompOpTint, from same size to size.
// Setup:
//   - Source is a 10x10 square, white.
//   - Destination is either a black or white square.
// Result:
//   - When source is transparent or tint color is clear, dest is black.
//   - When source is opaque, dest is blended blue
void test_bitblt__8bit_to_8bit_comptint(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__8bit_assign.8bit.pbi");
  GRect bounds = gbitmap_get_bounds(src_bitmap);

  uint8_t dest_data[bounds.size.w * bounds.size.h];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = bounds.size.w,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = src_bitmap->bounds
  };

  uint8_t expect_bmp_data[bounds.size.w * bounds.size.h];
  GBitmap expect_bmp = {
    .addr = expect_bmp_data,
    .row_size_bytes = bounds.size.w,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = src_bitmap->bounds
  };

  // Verify that the compositing mode is correctly applied when the source is opaque and
  // the tint color is not clear.
  memset(dest_data, GColorClear.argb, sizeof(dest_data));
  memset(expect_bmp_data, GColorBlue.argb, sizeof(expect_bmp_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpTint, GColorBlue);
  cl_assert(gbitmap_eq(&dest_bitmap, &expect_bmp, "test_bitblt__8bit_comptint-expect.8bit.pbi"));

  // Rewrite the destination bitmap to be all black.
  memset(dest_data, GColorBlack.argb, sizeof(dest_data));

  // Verify that if the tint color is clear, than the source bitmap should not affect the destination bitmap
  memset(expect_bmp_data, GColorBlack.argb, sizeof(expect_bmp_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpTint, GColorClear);
  cl_assert(gbitmap_eq(&dest_bitmap, &expect_bmp, "test_bitblt__8bit_comptint_clear-expect.8bit.pbi"));

  // Verify that if the source is transparent, it should not affect the destination bitmap
  prv_set_opacity(src_bitmap, 0);

  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpTint, GColorRed);
  cl_assert(gbitmap_eq(&dest_bitmap, &expect_bmp, "test_bitblt__8bit_comptint_clear-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Tests comptint, multiple tint colors and varying opacity of the source image.
// Setup:
//   - Destination is two 128 by 64 blocks expressing the 64 colors; each row expresses
//     one of the colors stack vertically.
//   - Source is a set of 4 by 4 blocks each with a 1 pixel wide vertical strip of a color
//     with an opacity in [0,3] inclusive.
// Result:
//   - Destination should be blended properly
void test_bitblt__8bit_comptint_blend(void) {
  const uint8_t NUM_COLORS = 64;
  const uint8_t WIDTH = NUM_COLORS * 2;
  const uint8_t LEGEND_WIDTH = 4;
  const uint8_t LEGEND_HEIGHT = 4;
  const uint8_t OFFSET = 1;
  const uint8_t HEIGHT = WIDTH;
  const uint8_t NUM_OPACITIES = 4;
  const uint8_t TOTAL_WIDTH = LEGEND_WIDTH * 2 + WIDTH;
  const uint8_t TOTAL_HEIGHT = HEIGHT + OFFSET + LEGEND_HEIGHT * 2;

  uint8_t dest_data[TOTAL_WIDTH * TOTAL_HEIGHT];
  memset(dest_data, GColorWhite.argb, sizeof(dest_data));

  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = TOTAL_WIDTH,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = (GRect){GPointZero, (GSize){TOTAL_WIDTH, TOTAL_HEIGHT}}
  };

  GColor color = (GColor){ .a = 3, .r = 0, .g = 0, .b = 0 };
  uint8_t src_data[] = { color.argb };
  GBitmap src_bmp = (GBitmap){
    .addr = src_data,
    .row_size_bytes = 1,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = GRect(0, 0, 1, 1)
  };

  for (uint8_t offset_y = 0; offset_y < NUM_COLORS; offset_y++) {
    const int16_t offset_x = OFFSET + LEGEND_WIDTH + WIDTH;
    const int16_t x = LEGEND_WIDTH;

    memset(src_data, color.argb, sizeof(src_data));
    color = prv_next_color(color);

    const int16_t y_upper = LEGEND_HEIGHT + offset_y;
    const GRect upper_rect = GRect(x, y_upper, WIDTH, 1);
    const GRect upper_left_legend_rect = GRect(0, y_upper, 3, 1);
    const GRect upper_right_legend_rect = GRect(offset_x, y_upper, 3, 1);
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &src_bmp, upper_rect,
                                    GPointZero, GCompOpAssign, GColorWhite);
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &src_bmp, upper_left_legend_rect,
                                    GPointZero, GCompOpAssign, GColorWhite);
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &src_bmp, upper_right_legend_rect,
                                    GPointZero, GCompOpAssign, GColorWhite);

    const int16_t y_lower = y_upper + LEGEND_HEIGHT + NUM_COLORS + OFFSET;
    const GRect lower_rect = GRect(x, y_lower, WIDTH, 1);
    const GRect lower_left_legend_rect = GRect(0, y_lower, 3, 1);
    const GRect lower_right_legend_rect = GRect(offset_x, y_lower, 3, 1);
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &src_bmp, lower_rect,
                                    GPointZero, GCompOpAssign, GColorWhite);
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &src_bmp, lower_left_legend_rect,
                                    GPointZero, GCompOpAssign, GColorWhite);
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &src_bmp, lower_right_legend_rect,
                                    GPointZero, GCompOpAssign, GColorWhite);
  }

  // RGB value should be discarded later on adding them here might relveal bugs.
  // .a is the important part
  GColor8 test_blend_colors[] = {
    (GColor8){.a = 0, .r = 3, .g = 2, .b = 1},
    (GColor8){.a = 1, .r = 0, .g = 3, .b = 2},
    (GColor8){.a = 2, .r = 1, .g = 0, .b = 3},
    (GColor8){.a = 3, .r = 2, .g = 1, .b = 0}
  };

  // Test image with four pixels of all our supported alpha values
  GBitmap test_bmp = (GBitmap){
    .addr = test_blend_colors,
    .row_size_bytes = 4,
    .bounds = GRect(0, 0, 4, 1),
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT
  };

  for (uint8_t rgb_half = 0; rgb_half < NUM_COLORS / 2; rgb_half++) {
    const int16_t x = rgb_half * NUM_OPACITIES + LEGEND_WIDTH;
    const int8_t legend_height = 3;

    // Upper row with destination colors from 0..31
    const GColor upper_tint_color = (GColor){.argb = 0b11000000 | rgb_half};
    const int16_t y_upper = LEGEND_HEIGHT;
    const GRect upper_rect = GRect(x, y_upper, test_bmp.bounds.size.w, NUM_COLORS);
    const GRect upper_legend_rect = GRect(x, y_upper - LEGEND_HEIGHT, test_bmp.bounds.size.w, legend_height);
    GColor8 upper_legend[4] = { [0 ... 3] = upper_tint_color };
    test_bmp.addr = upper_legend;
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &test_bmp, upper_legend_rect, GPointZero, GCompOpAssign, GColorWhite);
    test_bmp.addr = test_blend_colors;
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &test_bmp, upper_rect, GPointZero, GCompOpTint, upper_tint_color);

    // Lower row with destination colors from 31..63
    const int16_t y_lower = y_upper + NUM_COLORS + OFFSET + LEGEND_HEIGHT;
    const GRect lower_rect = GRect(x, y_lower, test_bmp.bounds.size.w, NUM_COLORS);
    const GColor lower_tint_color = (GColor){.argb = 0b11000000 | (rgb_half + (NUM_COLORS / 2))};
    const GRect lower_legend_rect = GRect(x, y_lower - LEGEND_HEIGHT, test_bmp.bounds.size.w, legend_height);
    GColor8 lower_legend[4] = { [0 ... 3] = lower_tint_color };
    test_bmp.addr = lower_legend;
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &test_bmp, lower_legend_rect, GPointZero, GCompOpAssign, GColorWhite);
    test_bmp.addr = test_blend_colors;
    bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, &test_bmp, lower_rect, GPointZero, GCompOpTint, lower_tint_color);
  }

  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_comptint_blend-expect.8bit.pbi"));
}

// Tests assign, clipping, makes sure in bottom right corner.
// Setup:
//   - Source is 10x15, black box ((0, 0), (5, 10)), rest is red.
//   - Dest is White, 50x50.
//   - Dest offset is set to 5x10 pixels in bottom right corner.
// Result:
//   - White, with 5x10 black box in bottom right corner.
void test_bitblt__8bit_clipping(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__8bit_clipping.8bit.pbi");
  uint8_t dest_data[50*50];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 50,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 50, 50 } }
  };
  memset(dest_data, GColorWhite.argb, sizeof(dest_data));

  GPoint dest_offset = { dest_bitmap.bounds.size.w-5, dest_bitmap.bounds.size.h-10 };
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, dest_offset, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_clipping-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Test horizontal wrapping when dest_rect wider than src_bitmap.
// Setup:
//   - Source 15 x 10, each row has the following pattern:
//       - 2px  Red
//       - 13px Black
//   - Dest Green 50x50
//   - Dest rect (17, 10) at (0, 0)
// Result:
//   - 2px  Red
//   - 13px Black
//   - 2px  Red
//   - Rest Blue
void test_bitblt__8bit_wrap_x(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__8bit_wrap_x.8bit.pbi");

  uint8_t dest_data[50*50];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 50,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 50, 50 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));

  // 2 wider than src_bitmap, so 2 columns of red will repeat again.
  GRect dest_rect = GRect(0, 0, src_bitmap->bounds.size.w + 2, src_bitmap->bounds.size.h);

  bitblt_bitmap_into_bitmap_tiled_8bit_to_8bit(
      &dest_bitmap, src_bitmap, dest_rect, GPointZero, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_wrap_x-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Test vertical wrapping when dest_rect taller than src_bitmap.
// Setup:
//   - Source is 25 x 10
//   - 4 rows red, 2 rows blue, 4 rows black.
//   - Dest is Green, 50 x 50
//   - Dest Rect is 10 x 24 at (0, 0)
// Result:
//   - Pattern repeated vertically x2, plus 4 rows of red.
void test_bitblt__8bit_wrap_y(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__8bit_wrap_y.8bit.pbi");

  uint8_t dest_data[50*50];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 50,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 50, 50 } }
  };
  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  GRect dest_rect = GRect(0, 0, src_bitmap->bounds.size.w, src_bitmap->bounds.size.h*2 + 4);

  bitblt_bitmap_into_bitmap_tiled_8bit_to_8bit(
      &dest_bitmap, src_bitmap, dest_rect, GPointZero, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__8bit_wrap_y-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Test src_origin_offset, shouldn't see any red in dest_bitmap.
// This covers src_origin_offset, y-axis and x-axis wraparound.
// Setup:
//   - Source 25x25, 2 columns, 2 rows red, rest is black.
//   - Source offset starts at (2, 2)
//   - Dest is blue, 100x100.
//   - Dest rect is 50x50 at (0,0).
// Result:
//   - No red in dest_bitmap.
//   - 50x50 black square at (0,0), rest is blue.
void test_bitblt__8bit_src_origin_offset_wrap(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__8bit_src_origin_offset_wrap.8bit.pbi");
  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  GRect dest_rect = GRect(0, 0, src_bitmap->bounds.size.w*2, src_bitmap->bounds.size.h*2);
  GPoint src_origin_offset = { 2, 2 }; // Offset past the 2 red rows

  bitblt_bitmap_into_bitmap_tiled_8bit_to_8bit(
      &dest_bitmap, src_bitmap, dest_rect, src_origin_offset, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
                           "test_bitblt__8bit_src_origin_offset_wrap-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}


//
// Test 1-bit to 8-bit blitting
///////////////////////////////

// Setup:
//   - Source is 25x25.
//   - Source has alternating white / black lines.
//   - Dest is Blue, 100x100.
//   - Dest offset is (0,0) to blit to top left corner.
// Result:
//   - 25x25 alternating black / white lines in top left corner.
void test_bitblt__1bit_to_8bit_compop(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_assign.1bit.pbi");
  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpAssign, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_assign-expect.8bit.pbi"));

  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpAssignInverted, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_assigninverted-expect.8bit.pbi"));

  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpOr, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_or-expect.8bit.pbi"));

  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpAnd, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_and-expect.8bit.pbi"));

  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpClear, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_clear-expect.8bit.pbi"));

  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, GPointZero, GCompOpSet, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_set-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source is an image of a white cross
//   - Dest is blue, same size as source
// Result:
//   - Destination should be written with a White cross
// Description:
//   - This test verifies that when the bitmap is 1-bit, we treat white as a
//     non-transparent color
void test_bitblt__1bit_to_8bit_compor(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_compor.1bit.pbi");
  GRect bounds = gbitmap_get_bounds(src_bitmap);

  cl_assert_equal_i(src_bitmap->info.format, GBitmapFormat1Bit);

  uint8_t dest_data[bounds.size.w * bounds.size.h];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = bounds.size.w,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = src_bitmap->bounds
  };

  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpOr, GColorLightGray);

  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_compor-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source is a 1bit image of a white cross with a black background
// Result:
//   - The image names describe the expected result of each destination color / tint color
//     combination
void test_bitblt__1bit_to_8bit_comptint(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_comptint.1bit.pbi");
  const GRect bounds = gbitmap_get_bounds(src_bitmap);

  cl_assert_equal_i(src_bitmap->info.format, GBitmapFormat1Bit);

  uint8_t dest_data[bounds.size.w * bounds.size.h];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = bounds.size.w,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = src_bitmap->bounds
  };

  // Image name descriptions
  // comptint_<cross_color>_on_<background_color>.8bit

  // Destination White
  memset(dest_data, GColorWhite.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_comptint_white_cross_white_corners-expect.8bit.pbi"));

  memset(dest_data, GColorWhite.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorBlack);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_comptint_white_cross_black_corners-expect.8bit.pbi"));

  memset(dest_data, GColorWhite.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorLightGray);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_comptint_white_cross_lightgray_corners-expect.8bit.pbi"));

  // Destination Black
  memset(dest_data, GColorBlack.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_comptint_black_cross_white_corners-expect.8bit.pbi"));

  memset(dest_data, GColorBlack.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorBlack);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_comptint_black_cross_black_corners-expect.8bit.pbi"));

  memset(dest_data, GColorBlack.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorLightGray);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_comptint_black_cross_lightgray_corners-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source is 25x25.
//   - Source has alternating white / black lines.
//   - Dest is Blue, 100x100.
//   - Dest offset is set to 8x10 clipped in the bottom right corner.
// Result:
//   - There should be an 8x10 alternating black & white lines in the bottom right corner.
void test_bitblt__1bit_to_8bit_clipping(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_clipping.1bit.pbi");

  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  GPoint dest_offset = { dest_bitmap.bounds.size.w-8, dest_bitmap.bounds.size.h-10 };
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, dest_offset, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_clipping-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source is 25 x 25.
//   - Source rows alternating: 1px black, 1px white.
//   - Dest is Blue, 100x100
//   - Dest rect is 50 x 25, at (0, 0)
// Result:
//   - 50x20 of alternating stripes, rest is blue.
void test_bitblt__1bit_to_8bit_wrap_x(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_wrap_x.1bit.pbi");

  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  GRect dest_rect = GRect(0, 0, src_bitmap->bounds.size.w*2, src_bitmap->bounds.size.h);

  bitblt_bitmap_into_bitmap_tiled_1bit_to_8bit(
      &dest_bitmap, src_bitmap, dest_rect, GPointZero, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_8bit_wrap_x-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source is 40 x 30.
//   - Source has 2 columns, 4 rows of black, rest is white.
//   - Dest is all blue.
//   - Source offset (2, 4) past black.
//   - Destination is 100 x 100
//   - Destination rect is size of white portion of source.
// Result:
//   - Blue bitmap with white square at 0,0 of dest_rect size.
void test_bitblt__1bit_to_8bit_src_origin_offset(void) {
  GBitmap *src_bitmap =
      get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_src_origin_offset.1bit.pbi");
  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  GRect dest_rect = GRect(0, 0, src_bitmap->bounds.size.w - 2, src_bitmap->bounds.size.h - 4);
  GPoint src_origin_offset = { 2, 4 }; // Offset past the black

  bitblt_bitmap_into_bitmap_tiled_1bit_to_8bit(
      &dest_bitmap, src_bitmap, dest_rect, src_origin_offset, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
                           "test_bitblt__1bit_to_8bit_src_origin_offset-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source is a 10x32 white square.
//   - Dest is all black.
//   - Dest origin offset set to 15, 18.
//   - Dest clipped to 10x10
// Result:
//   - Black bitmap with 10x10 white square starting at (15, 10)
void test_bitblt__1bit_to_8bit_dest_origin_offset_clip(void) {
  GBitmap *src_bitmap =
      get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_dest_origin_offset_clip.1bit.pbi");

  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlack.argb, sizeof(dest_data));
  GRect dest_rect = GRect(15, 10, 10, 10);
  GPoint src_origin_offset = { 0, 0 };

  bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, src_bitmap, dest_rect, src_origin_offset,
                                  GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
                           "test_bitblt__1bit_to_8bit_dest_origin_offset_clip-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}


// Setup:
//   - Source width is 32 pixels (ie. a word in source)
//   - Source starts with 2 rows and 4 columns of black pixels.
//   - Dest is all blue.
//   - Src origin is set to (4, 2)
//   - dest origin is set to 10, 25
//   - dest size is twice the height / width of the source.
void test_bitblt__1bit_to_8bit_src_origin_offset_wrap(void) {
  GBitmap *src_bitmap =
      get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_src_origin_offset_wrap.1bit.pbi");

  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  GRect dest_rect = GRect(10, 25, src_bitmap->bounds.size.w*2, src_bitmap->bounds.size.h*2);
  GPoint src_origin_offset = { .x = 4, .y = 2 }; // Offset past the black

  bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, src_bitmap, dest_rect, src_origin_offset,
                                  GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
                           "test_bitblt__1bit_to_8bit_src_origin_offset_wrap-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source width is not a multiple of 8 (ie. not byte aligned in source)
//   - Source starts with 2 rows and 4 columns of black pixels.
//   - Dest is all blue.
//   - Src origin is set to (4, 2)
//   - dest origin is set to 22, 6
//   - dest size is twice the height / width of the source.
void test_bitblt__1bit_to_8bit_src_origin_offset_wrap2(void) {
  GBitmap *src_bitmap =
      get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_src_origin_offset_wrap2.1bit.pbi");

  uint8_t dest_data[100*100];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 100,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 100, 100 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  GRect dest_rect = GRect(21, 6, src_bitmap->bounds.size.w*2, src_bitmap->bounds.size.h*2);
  GPoint src_origin_offset = { .x = 4, .y = 2 }; // Offset past the black

  bitblt_bitmap_into_bitmap_tiled(&dest_bitmap, src_bitmap, dest_rect, src_origin_offset,
                                  GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
                           "test_bitblt__1bit_to_8bit_src_origin_offset_wrap2-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source has 2 lines of white and the rest black.
//   - Destination all blue.
//   - Use gbitmap_init_as_sub_bitmap to get a sub-bitmap that starts at y = 2
// Result:
//   - A 48 x 50 black box starting at y=2 in dest, rest is blue.
void test_bitblt__bitmap_into_bitmap_sub_bitmap(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__bitmap_into_bitmap_sub_bitmap.8bit.pbi");

  GBitmap cropped_src_bitmap;
  gbitmap_init_as_sub_bitmap(&cropped_src_bitmap, src_bitmap,
      (GRect) { { 0, 2 }, src_bitmap->bounds.size });

  uint8_t dest_data[50*50];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 50,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 50, 50 } }
  };
  memset(dest_data, GColorBlue.argb, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, &cropped_src_bitmap, (GPoint) { 0, 2 },
                            GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
                           "test_bitblt__bitmap_into_bitmap_sub_bitmap-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Test:
//   - source origin offset
//   - source bounds origin and size
//   - wrapping into larger destination
//   - dest rect not at { 0, 0 }
// Setup:
//   - Source has non-zero bounds origin, { 5, 5 }, outside of this is red.
//   - Source has non-full bounds size, { 10, 10 }, outside of this is white.
//   - Source has an origin offset, { 3, 6 }, outside of this is blue.
//   - Rest is black
//
//   - Destination has non-zero dest_rect origin, { 4, 4 }
//   - Destination has non-full dest_rect size larger than source, { 10, 10 }
//   - Destination has full size of { 20, 20 }
// Result:
//   - Expect a 10 x 10 black box at { 4, 4 } that is of size { 10, 10 }
//   - Remainder should be green.
void test_bitblt__8bit_bounds_and_origin_offset(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__bounds_and_origin_offset.8bit.pbi");
  src_bitmap->bounds = (GRect) { { 5, 5}, { 10, 10 } };

  uint8_t dest_data[50*50];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 50,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 50, 50 } }
  };
  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  const GRect dest_rect = GRect(4, 4, 10, 10);
  const GPoint src_origin_offset = GPoint(3, 6);
  bitblt_bitmap_into_bitmap_tiled_8bit_to_8bit(&dest_bitmap, src_bitmap, dest_rect,
                                               src_origin_offset, GCompOpAssign, GColorWhite);
  cl_assert(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__bounds_and_origin_offset-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Setup:
//   - Source is a 1bit image of a white cross with a black background
// Result:
//   - The image names describe the expected result of each destination color / tint color
//     combination
void test_bitblt__1bit_to_1bit_comptint(void) {
  GBitmap *src_bitmap = get_gbitmap_from_pbi("test_bitblt__1bit_to_1bit_comptint.1bit.pbi");
  const GRect bounds = gbitmap_get_bounds(src_bitmap);

  cl_assert_equal_i(src_bitmap->info.format, GBitmapFormat1Bit);

  uint8_t dest_data[bounds.size.w * bounds.size.h];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = src_bitmap->row_size_bytes,
    .info.format = GBitmapFormat1Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = src_bitmap->bounds
  };

  // Image name descriptions
  // comptint_<cross_color>_on_<background_color>.1bit

  // Destination White
  memset(dest_data, 0b11111111, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorClear);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_1bit_comptint_white_on_white-expect.1bit.pbi"));

  memset(dest_data, 0b11111111, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_1bit_comptint_white_on_white-expect.1bit.pbi"));

  memset(dest_data, 0b11111111, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorBlack);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_1bit_comptint_black_on_white-expect.1bit.pbi"));

  // Destination Black
  memset(dest_data, 0b00000000, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorClear);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_1bit_comptint_black_on_black-expect.1bit.pbi"));

  memset(dest_data, 0b00000000, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorWhite);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_1bit_comptint_white_on_black-expect.1bit.pbi"));

  memset(dest_data, 0b00000000, sizeof(dest_data));
  bitblt_bitmap_into_bitmap(&dest_bitmap, src_bitmap, (GPoint){0}, GCompOpTint, GColorBlack);
  cl_check(gbitmap_pbi_eq(&dest_bitmap, "test_bitblt__1bit_to_1bit_comptint_black_on_black-expect.1bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// Test:
//   - source origin offset
//   - source bounds origin and size
//     - Source bounds origin is beyond (32, y) to pass word boundary.
//     - Source origin offset is beyond (32, y) to pass another word boundary.
//   - wrapping into larger destination
//   - dest rect not at { 0, 0 }
// Setup:
//   - Dest rect at {4, 4}, repeat twice and a bit: {140, 55}
//   - Source bounds origin at {37, 3), size {63, 23}
//   - Source origin offset (39, 11)
void DISABLED_test_bitblt__1bit_to_8bit_bounds_and_origin_offset(void) {
  GBitmap *src_bitmap =
      get_gbitmap_from_pbi("test_bitblt__1bit_to_8bit_bounds_and_origin_offset.1bit.pbi");

  uint8_t dest_data[144*168];
  GBitmap dest_bitmap = {
    .addr = dest_data,
    .row_size_bytes = 144,
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
    .bounds = { { 0, 0 }, { 144, 168 } }
  };
  memset(dest_data, GColorGreen.argb, sizeof(dest_data));
  const GRect dest_rect = GRect(4, 4, 140, 55);
  const GPoint src_origin_offset = GPoint(39, 11);
  src_bitmap->bounds = (GRect) { { 37, 3}, { 63, 23 } };

  bitblt_bitmap_into_bitmap_tiled_1bit_to_8bit(&dest_bitmap, src_bitmap, dest_rect,
                                               src_origin_offset, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
      "test_bitblt__1bit_to_8bit_bounds_and_origin_offset-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}

// This test is the same as test_bitblt__1bit_to_1bit_bounds_and_origin_offset, except it is for
// 1-bit to 1-bit.
// FIXME: This is a known legacy-broken case in 1-bit bitblt. See PBL-14671 for more information
void DISABLED__test_bitblt__1bit_to_1bit_bounds_and_origin_offset(void) {
  GBitmap *src_bitmap =
      get_gbitmap_from_pbi("test_bitblt__1bit_to_1bit_bounds_and_origin_offset.1bit.pbi");

  uint8_t dest_data[src_bitmap->row_size_bytes * src_bitmap->bounds.size.h];
  GBitmap dest_bitmap = *src_bitmap;
  dest_bitmap.addr = dest_data;
  dest_bitmap.bounds = src_bitmap->bounds;
  memset(dest_data, 0xff, sizeof(dest_data));

  const GRect dest_rect = GRect(4, 4, 140, 55);
  const GPoint src_origin_offset = GPoint(39, 11);

  src_bitmap->bounds = (GRect) { { 37, 3}, { 63, 23 } };
  bitblt_bitmap_into_bitmap_tiled_1bit_to_1bit(&dest_bitmap, src_bitmap, dest_rect,
                                               src_origin_offset, GCompOpAssign, GColorWhite);

  cl_assert(gbitmap_pbi_eq(&dest_bitmap,
      "test_bitblt__1bit_to_1bit_bounds_and_origin_offset-expect.8bit.pbi"));

  gbitmap_destroy(src_bitmap);
}
