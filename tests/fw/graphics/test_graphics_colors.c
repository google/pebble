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

#include "applib/graphics/gtypes.h"

#include "clar.h"
#include "stubs_app_state.h"
#include "stubs_heap.h"
#include "stubs_passert.h"
#include "stubs_process_manager.h"

void test_graphics_colors__black(void) {
  cl_assert(GColorBlack.r == 0b00);
  cl_assert(GColorBlack.g == 0b00);
  cl_assert(GColorBlack.b == 0b00);
  cl_assert(GColorBlack.a == 0b11);
}

void test_graphics_colors__white(void) {
  cl_assert(GColorWhite.r == 0b11);
  cl_assert(GColorWhite.g == 0b11);
  cl_assert(GColorWhite.b == 0b11);
  cl_assert(GColorWhite.a == 0b11);
}

void test_graphics_colors__red(void) {
  cl_assert(GColorRed.r == 0b11);
  cl_assert(GColorRed.g == 0b00);
  cl_assert(GColorRed.b == 0b00);
  cl_assert(GColorRed.a == 0b11);
}

void test_graphics_colors__green(void) {
  cl_assert(GColorGreen.r == 0b00);
  cl_assert(GColorGreen.g == 0b11);
  cl_assert(GColorGreen.b == 0b00);
  cl_assert(GColorGreen.a == 0b11);
}

void test_graphics_colors__blue(void) {
  cl_assert(GColorBlue.r == 0b00);
  cl_assert(GColorBlue.g == 0b00);
  cl_assert(GColorBlue.b == 0b11);
  cl_assert(GColorBlue.a == 0b11);
}

void test_graphics_colors__equal(void) {
  cl_assert(gcolor_equal(GColorBlue, GColorBlue));
  cl_assert(gcolor_equal(GColorRed, GColorRed));
  cl_assert(!gcolor_equal(GColorRed, GColorBlue));

  // Two colors with zero alpha values should be equal regardless of what their RGB channels are
  GColor8 color1 = GColorBlue;
  color1.a = 0;
  GColor8 color2 = GColorRed;
  color2.a = 0;
  cl_assert(gcolor_equal(color1, color2));

  // But two colors with semi-transparent alpha values should not be equal if their RGB channels
  // don't match
  color1.a = 1;
  color2.a = 1;
  cl_assert(!gcolor_equal(color1, color2));
}

void test_graphics_colors__equal__deprecated(void) {
  cl_assert(gcolor_equal__deprecated(GColorBlue, GColorBlue));
  cl_assert(gcolor_equal__deprecated(GColorRed, GColorRed));
  cl_assert(!gcolor_equal__deprecated(GColorRed, GColorBlue));

  // Check the behavior of the deprecated function (which is incorrect) that two colors with
  // zero alpha values but different RGB values are not considered equal
  GColor8 color1 = GColorBlue;
  color1.a = 0;
  GColor8 color2 = GColorRed;
  color2.a = 0;
  cl_assert(!gcolor_equal__deprecated(color1, color2));
}

void test_graphics_colors__inverted_readable_color(void) {
  GColor8 (*fun)(GColor8) = gcolor_legible_over;

  // transparent colors result in transparent - who has a better idea?
  cl_assert_equal_i(GColorClearARGB8, fun(GColorClear).argb);

//  // obvious cases
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorBlack).argb);
  cl_assert_equal_i(GColorBlackARGB8, fun(GColorWhite).argb);

//  // expectation as derived from Appfaces_all.psd
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0xff0000)).argb);
  cl_assert_equal_i(GColorBlackARGB8, fun(GColorFromHEX(0x00aaff)).argb);
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0xaa0055)).argb);
  cl_assert_equal_i(GColorBlackARGB8, fun(GColorFromHEX(0x55aa55)).argb);
  cl_assert_equal_i(GColorBlackARGB8, fun(GColorFromHEX(0xff5555)).argb);
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0x0055aa)).argb);
  cl_assert_equal_i(GColorBlackARGB8, fun(GColorFromHEX(0xff5500)).argb);
  cl_assert_equal_i(GColorBlackARGB8, fun(GColorFromHEX(0xaaaaaa)).argb);

//  // expectation as derived from Appfaces_generic_colors.psd
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0x0000aa)).argb);
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0x005500)).argb);
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0x550055)).argb);
  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0xaa0000)).argb);

  // contradiction with previous case - oh dear...
//  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0xff5500)).argb);

  cl_assert_equal_i(GColorWhiteARGB8, fun(GColorFromHEX(0xaa5500)).argb);
}

void test_graphics_colors__grayscale_colors(void) {
  cl_assert_equal_i(GColorClearARGB8, gcolor_get_grayscale(GColorClear).argb);
  cl_assert_equal_i(GColorWhiteARGB8, gcolor_get_grayscale(GColorWhite).argb);
  cl_assert_equal_i(GColorBlackARGB8, gcolor_get_grayscale(GColorBlack).argb);
  cl_assert_equal_i(GColorLightGrayARGB8, gcolor_get_grayscale(GColorLightGray).argb);
  cl_assert_equal_i(GColorDarkGrayARGB8, gcolor_get_grayscale(GColorDarkGray).argb);
  cl_assert_equal_i(GColorWhiteARGB8, gcolor_get_grayscale(GColorYellow).argb);
  cl_assert_equal_i(GColorBlackARGB8, gcolor_get_grayscale(GColorBlue).argb);
  cl_assert_equal_i(GColorLightGrayARGB8, gcolor_get_grayscale(GColorTiffanyBlue).argb);
  cl_assert_equal_i(GColorDarkGrayARGB8, gcolor_get_grayscale(GColorOrange).argb);
}

void test_graphics_colors__bw_colors(void) {
  cl_assert_equal_i(GColorClearARGB8, gcolor_get_bw(GColorClear).argb);
  cl_assert_equal_i(GColorWhiteARGB8, gcolor_get_bw(GColorWhite).argb);
  cl_assert_equal_i(GColorBlackARGB8, gcolor_get_bw(GColorBlack).argb);
  cl_assert_equal_i(GColorWhiteARGB8, gcolor_get_bw(GColorLightGray).argb);
  cl_assert_equal_i(GColorBlackARGB8, gcolor_get_bw(GColorDarkGray).argb);
  cl_assert_equal_i(GColorWhiteARGB8, gcolor_get_bw(GColorYellow).argb);
  cl_assert_equal_i(GColorBlackARGB8, gcolor_get_bw(GColorBlue).argb);
  cl_assert_equal_i(GColorWhiteARGB8, gcolor_get_bw(GColorTiffanyBlue).argb);
  cl_assert_equal_i(GColorBlackARGB8, gcolor_get_bw(GColorOrange).argb);
}

static void prv_test_get_luminance(GColor8 color_to_test, GColor8Component expected_luminance) {
  cl_assert_equal_i(gcolor_get_luminance(color_to_test), expected_luminance);
  // Test that alpha doesn't affect luminance
  GColor8 semitransparent_color = color_to_test;
  semitransparent_color.a = 1;
  cl_assert_equal_i(gcolor_get_luminance(semitransparent_color), expected_luminance);
  GColor8 transparent_color = color_to_test;
  transparent_color.a = 0;
  cl_assert_equal_i(gcolor_get_luminance(transparent_color), expected_luminance);
}

void test_graphics_colors__get_luminance(void) {
  prv_test_get_luminance(GColorBlack, 0);
  prv_test_get_luminance(GColorWhite, 3);
  prv_test_get_luminance(GColorYellow, 3);
  prv_test_get_luminance(GColorRed, 2);
  prv_test_get_luminance(GColorBlue, 1);
}

void test_graphics_colors__tint_luminance_lookup_table_init(void) {
  GColor8 tint_color = GColorClear;
  GColor8 lookup_table_out[GCOLOR8_COMPONENT_NUM_VALUES];

  // Passing in NULL for lookup_table_out should assert
  cl_assert_passert(gcolor_tint_luminance_lookup_table_init(tint_color, NULL));

  // Setting the tint color to black should result in a gradient from black to white
  tint_color = GColorBlack;
  gcolor_tint_luminance_lookup_table_init(tint_color, lookup_table_out);
  cl_assert(gcolor_equal(lookup_table_out[0], GColorBlack));
  cl_assert(gcolor_equal(lookup_table_out[1], GColorDarkGray));
  cl_assert(gcolor_equal(lookup_table_out[2], GColorLightGray));
  cl_assert(gcolor_equal(lookup_table_out[3], GColorWhite));

  // Setting the tint color to blue should result in a gradient from blue to yellow
  tint_color = GColorBlue;
  gcolor_tint_luminance_lookup_table_init(tint_color, lookup_table_out);
  cl_assert(gcolor_equal(lookup_table_out[0], GColorBlue));
  cl_assert(gcolor_equal(lookup_table_out[1], GColorLiberty));
  cl_assert(gcolor_equal(lookup_table_out[2], GColorBrass));
  cl_assert(gcolor_equal(lookup_table_out[3], GColorYellow));

  // lookup_table_out's entries should all preserve the alpha of tint_color
  tint_color = GColorBlack;
  tint_color.a = 1;
  gcolor_tint_luminance_lookup_table_init(tint_color, lookup_table_out);
  for (GColor8Component i = 0; i < GCOLOR8_COMPONENT_NUM_VALUES; i++) {
    cl_assert_equal_i(lookup_table_out[i].a, tint_color.a);
  }
}

static void prv_test_tint_using_luminance_and_perform_lookup_using_color_luminance(
    GColor8 src_color, GColor8 tint_color, GColor8 expected_result_color) {
  GColor8 lookup_table[GCOLOR8_COMPONENT_NUM_VALUES];
  gcolor_tint_luminance_lookup_table_init(tint_color, lookup_table);
  cl_assert(gcolor_equal(
      gcolor_perform_lookup_using_color_luminance_and_multiply_alpha(src_color, lookup_table),
      expected_result_color));
  cl_assert(gcolor_equal(gcolor_tint_using_luminance_and_multiply_alpha(src_color, tint_color),
                         expected_result_color));
}

void test_graphics_colors__tint_using_luminance_and_perform_lookup_using_color_luminance(void) {
  // Passing in NULL for lookup_table should assert
  cl_assert_passert(gcolor_perform_lookup_using_color_luminance_and_multiply_alpha(GColorRed,
                                                                                   NULL));

  // A src_color of yellow should have a luminance that picks white from lookup_table
  // initialized with a tint_color of black
  prv_test_tint_using_luminance_and_perform_lookup_using_color_luminance(GColorYellow,
                                                                         GColorBlack,
                                                                         GColorWhite);

  // A src_color of red should have a luminance that picks light gray from lookup_table initialized
  // with a tint_color of black
  prv_test_tint_using_luminance_and_perform_lookup_using_color_luminance(GColorRed,
                                                                         GColorBlack,
                                                                         GColorLightGray);

  // A src_color of blue should have a luminance that picks dark gray from lookup_table
  // initialized with a tint_color of black
  prv_test_tint_using_luminance_and_perform_lookup_using_color_luminance(GColorBlue,
                                                                         GColorBlack,
                                                                         GColorDarkGray);

  // A src_color of black should have a luminance that picks black from lookup_table initialized
  // with a tint_color of black
  prv_test_tint_using_luminance_and_perform_lookup_using_color_luminance(GColorBlack,
                                                                         GColorBlack,
                                                                         GColorBlack);
}

void test_graphics_colors__component_multiply(void) {
  const GColor8Component max_component_value = GCOLOR8_COMPONENT_NUM_VALUES - 1;

  // 0% multiplied by anything is 0%
  for (GColor8Component i = 0; i <= max_component_value; i++) {
    cl_assert_equal_i(gcolor_component_multiply(i, 0), 0);
    cl_assert_equal_i(gcolor_component_multiply(0, i), 0);
  }

  // Associative property (excluding 0% since we already test it above)
  for (GColor8Component i = 1; i <= max_component_value; i++) {
    for (GColor8Component j = 1; j <= max_component_value; j++) {
      cl_assert_equal_i(gcolor_component_multiply(i, j), gcolor_component_multiply(j, i));
    }
  }

  // A few specific combinations
  cl_assert_equal_i(gcolor_component_multiply(3, 3), 3);
  cl_assert_equal_i(gcolor_component_multiply(3, 2), 2);
  cl_assert_equal_i(gcolor_component_multiply(2, 2), 1);
  cl_assert_equal_i(gcolor_component_multiply(1, 3), 1);
  cl_assert_equal_i(gcolor_component_multiply(2, 1), 1);
}
