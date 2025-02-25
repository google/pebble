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

#include "clar.h"

#include "applib/graphics/gtypes.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gpath.h"
#include "util/trig.h"
#include "applib/ui/ui.h"

#include <string.h>

// Helper Functions
////////////////////////////////////
#include "util.h"
#include "test_graphics.h"
#include "${BIT_DEPTH_NAME}/test_framebuffer.h"

// Stubs
////////////////////////////////////
#include "graphics_common_stubs.h"
#include "stubs_applib_resource.h"

static const int16_t SCREEN_WIDTH = 144;
static const int16_t SCREEN_HEIGHT = 168;
static FrameBuffer *fb = NULL;

static const GPathInfo s_house_path_info = {
  .num_points = 11,
  .points = (GPoint []) {
    {-40, 0}, {0, -40}, {40, 0}, {28, 0}, {28, 40}, {10, 40},
    {10, 16}, {-10, 16}, {-10, 40}, {-28, 40}, {-28, 0},
  },
};

static const GPathInfo s_bolt_path_info = {
  .num_points = 6,
  .points = (GPoint []) {{21, 0}, {14, 26}, {28, 26}, {7, 60}, {14, 34}, {0, 34}}
};

static const GPathInfo s_duplicates_path_info = {
  6,
  (GPoint[]) {
    {40, 0},
    {40, 0},
    {0, 40},
    {0, 40},
    {80, 40},
    {80, 40}
  }
};

static const GPathInfo s_single_duplicate_path_info = {
  2,
  (GPoint[]) {
    {40, 0},
    {40, 0}
  }
};

static const GPathInfo s_crossing_path_info = {
  6,
  (GPoint[]) {
    {0, 40},
    {20, 20},
    {60, 60},
    {80, 40},
    {60, 20},
    {20, 60}
  }
};

static const GPathInfo s_infinite_path_info = {
  16,
  (GPoint[]) {
    {-50, 0},
    {-50, -60},
    {10, -60},
    {10, -20},
    {-10, -20},
    {-10, -40},
    {-30, -40},
    {-30, -20},
    {50, -20},
    {50, 40},
    {-10, 40},
    {-10, 0},
    {10, 0},
    {10, 20},
    {30, 20},
    {30, 0}
  }
};

static const GPathInfo s_aa_clipping_path_info = {
  .num_points = 4,
  .points = (GPoint []) {{0,0}, {200, 0}, {200, 30}, {0, 30}}
};

static bool s_outline_mode = false;
static int s_path_angle = 0;

static GPath *s_house_path = NULL;
static GPath *s_bolt_path = NULL;
static GPath *s_duplicates_path = NULL;
static GPath *s_single_duplicate_path = NULL;
static GPath *s_crossing_path = NULL;
static GPath *s_infinite_path = NULL;
static GPath *s_current_path = NULL;
static GPath *s_aa_clipping_path = NULL;

static void prv_filled_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_stroke_color(ctx, GColorBlack);

#if 0 // Guidelines
  int num_segments = 4;
  int segment_width = SCREEN_WIDTH / num_segments;
  int segment_height = SCREEN_HEIGHT / num_segments;
  for (int i = 1; i < num_segments; ++i) {
    graphics_draw_line(ctx,
        GPoint(segment_width * i, 0),
        GPoint(segment_width * i, SCREEN_HEIGHT));
    graphics_draw_line(ctx,
        GPoint(0, segment_height * i),
        GPoint(SCREEN_WIDTH, segment_height * i));
  }
#endif

  gpath_rotate_to(s_current_path, s_path_angle * TRIG_MAX_ANGLE / 360);

  if (s_outline_mode) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    gpath_draw_outline(ctx, s_current_path);
  } else {
    graphics_context_set_fill_color(ctx, GColorBlack);
    gpath_draw_filled(ctx, s_current_path);
  }
}

static void prv_reset(void) {
  gpath_move_to(s_house_path, GPoint(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2));
  s_outline_mode = false;
  s_path_angle = 0;
}

// setup and teardown
void test_graphics_gpath_${BIT_DEPTH_NAME}__initialize(void) {
  fb = malloc(sizeof(FrameBuffer));
  framebuffer_init(fb, &(GSize) {DISP_COLS, DISP_ROWS});
  s_house_path = gpath_create(&s_house_path_info);
  s_bolt_path = gpath_create(&s_bolt_path_info);
  s_duplicates_path = gpath_create(&s_duplicates_path_info);
  s_crossing_path = gpath_create(&s_crossing_path_info);
  s_infinite_path = gpath_create(&s_infinite_path_info);
  s_aa_clipping_path = gpath_create(&s_aa_clipping_path_info);
  prv_reset();
}

void test_graphics_gpath_${BIT_DEPTH_NAME}__cleanup(void) {
  free(fb);
  gpath_destroy(s_house_path);
  gpath_destroy(s_bolt_path);
  gpath_destroy(s_duplicates_path);
  gpath_destroy(s_infinite_path);
  gpath_destroy(s_crossing_path);
  gpath_destroy(s_aa_clipping_path);
}

// tests
void test_graphics_gpath_${BIT_DEPTH_NAME}__filled(void) {
  GContext ctx;
  Layer layer;

  prv_reset();
  s_current_path = s_house_path;
  test_graphics_context_init(&ctx, fb);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled.${BIT_DEPTH_NAME}.pbi"));
}

void test_graphics_gpath_${BIT_DEPTH_NAME}__filled_clipped(void) {
  GContext ctx;
  Layer layer;

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(0, SCREEN_HEIGHT / 2, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_top_clipped.${BIT_DEPTH_NAME}.pbi"));

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_bottom_clipped.${BIT_DEPTH_NAME}.pbi"));

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH / 2, SCREEN_HEIGHT);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_left_clipped.${BIT_DEPTH_NAME}.pbi"));

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(SCREEN_WIDTH / 2, 0, SCREEN_WIDTH / 2, SCREEN_HEIGHT);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_right_clipped.${BIT_DEPTH_NAME}.pbi"));
}

// outside with no clipping -- results should be identical to the regular filled test
void test_graphics_gpath_${BIT_DEPTH_NAME}__filled_outside(void) {
  GContext ctx;
  Layer layer;

  printf("-- top\n");
  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  gpath_move_to(s_house_path, GPoint(SCREEN_WIDTH / 2, 0));
  ctx.draw_state.drawing_box = GRect(0, SCREEN_HEIGHT / 2, SCREEN_WIDTH, SCREEN_HEIGHT);
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled.${BIT_DEPTH_NAME}.pbi"));

  printf("-- bottom\n");
  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  gpath_move_to(s_house_path, GPoint(SCREEN_WIDTH / 2, SCREEN_HEIGHT));
  ctx.draw_state.drawing_box = GRect(0, -SCREEN_HEIGHT / 2, SCREEN_WIDTH, SCREEN_HEIGHT);
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled.${BIT_DEPTH_NAME}.pbi"));

  printf("-- left\n");
  prv_reset();
  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  gpath_move_to(s_house_path, GPoint(0, SCREEN_HEIGHT / 2));
  ctx.draw_state.drawing_box = GRect(SCREEN_WIDTH / 2, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled.${BIT_DEPTH_NAME}.pbi"));

  printf("-- right\n");
  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  gpath_move_to(s_house_path, GPoint(SCREEN_WIDTH, SCREEN_HEIGHT / 2));
  ctx.draw_state.drawing_box = GRect(-SCREEN_WIDTH / 2, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled.${BIT_DEPTH_NAME}.pbi"));
}

// AA section
void test_graphics_gpath_${BIT_DEPTH_NAME}__filled_aa(void) {
  GContext ctx;
  Layer layer;

  // House path - tests horizontal line edge case
  prv_reset();
  s_current_path = s_house_path;
  test_graphics_context_init(&ctx, fb);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled_aa.${BIT_DEPTH_NAME}.pbi"));

  // Special case for two points that are duplicates...
  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_single_duplicate_path;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_single_duplicate_aa.${BIT_DEPTH_NAME}.pbi"));

}

void test_graphics_gpath_${BIT_DEPTH_NAME}__filled_clipped_aa(void) {
  GContext ctx;
  Layer layer;

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(0, SCREEN_HEIGHT / 2, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_top_clipped_aa.${BIT_DEPTH_NAME}.pbi"));

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_bottom_clipped_aa.${BIT_DEPTH_NAME}.pbi"));

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH / 2, SCREEN_HEIGHT);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_left_clipped_aa.${BIT_DEPTH_NAME}.pbi"));

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_house_path;
  ctx.draw_state.clip_box = GRect(SCREEN_WIDTH / 2, 0, SCREEN_WIDTH / 2, SCREEN_HEIGHT);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
        "gpath_filled_right_clipped_aa.${BIT_DEPTH_NAME}.pbi"));
}

// Additional test to check AA on edges - works only on 8bit
void test_graphics_gpath_8bit__filled_bolt_aa(void) {
  // NOTE: Those tests are being performed only in 8bits due to differences caused by AA,
  //   performing them in both 1bit and 8bit would create differences on the edges
  //   and fail unit tests as a result
  GContext ctx;
  Layer layer;

  // Bolt path - test antialiased edges
  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_bolt_path;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
                          "gpath_filled_bolt_aa.8bit.pbi"));

  // Duplicate points in GPath test - makes sure theres no division by zero ;)
  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_duplicates_path;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
                          "gpath_filled_duplicates_aa.8bit.pbi"));

  // Crossing path - makes sure algorithm works for path that crosses itself
  prv_reset();
  s_current_path = s_crossing_path;
  test_graphics_context_init(&ctx, fb);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled_crossing_aa.8bit.pbi"));

  // Infinite path - shows an example where path seems to be crossing itself but
  //   in fact it does not
  prv_reset();
  s_current_path = s_infinite_path;
  gpath_move_to(s_infinite_path, GPoint(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2));
  test_graphics_context_init(&ctx, fb);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled_infinite_aa.8bit.pbi"));

  // An angle of infinite path - here we see the spacing between the parts
  prv_reset();
  s_current_path = s_infinite_path;
  gpath_move_to(s_infinite_path, GPoint(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2));
  s_path_angle = 45;
  test_graphics_context_init(&ctx, fb);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled_infinite_45_aa.8bit.pbi"));

  // Another angle of infinite path
  prv_reset();
  s_current_path = s_infinite_path;
  gpath_move_to(s_infinite_path, GPoint(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2));
  s_path_angle = 70;
  test_graphics_context_init(&ctx, fb);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled_infinite_70_aa.8bit.pbi"));

  // House path - two edge cases for tipping points of the path
  prv_reset();
  s_current_path = s_house_path;
  gpath_move_to(s_house_path, GPoint(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2));
  s_path_angle = 20;
  test_graphics_context_init(&ctx, fb);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled_house_20_aa.8bit.pbi"));

  // This case demonstrates tipping point that is also the starting point of the path
  prv_reset();
  s_current_path = s_house_path;
  gpath_move_to(s_house_path, GPoint(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2));
  s_path_angle = 105;
  test_graphics_context_init(&ctx, fb);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "gpath_filled_house_105_aa.8bit.pbi"));
  
  // Safety
  s_path_angle = 0;
}

void test_graphics_gpath_8bit__clipping_aa(void) {
  // NOTE: This test verifies correct clipping of anti-aliased edges on gpaths, therefore
  //         it works only on 8bit
  GContext ctx;
  Layer layer;

  prv_reset();
  test_graphics_context_init(&ctx, fb);
  s_current_path = s_aa_clipping_path;
  s_path_angle = 17;
  ctx.draw_state.clip_box = GRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
  graphics_context_set_antialiased(&ctx, true);
  prv_filled_update_proc(NULL, &ctx);
  // NOTE: Expected result of this test is to have an antialiased stripe go through the screen,
  //         where antialiased edges are being nicely cut off on top and bottom of the stripe
  //         (antialiased gradient would dive into the stripe near screen edges), also top
  //         left corner is intentionally ending just before screen cuts it out to verify that
  //         fractional anti-aliasing is not bleeding into row before (would occur as pixels on
  //         right side of the screen)
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap,
                          "gpath_clipping_aa.8bit.pbi"));

  // Safety
  s_path_angle = 0;
}
