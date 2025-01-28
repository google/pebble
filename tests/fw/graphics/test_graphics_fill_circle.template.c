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
#include "applib/graphics/graphics_circle_private.h"
#include "applib/graphics/framebuffer.h"
#include "util/trig.h"

#include "applib/ui/window_private.h"
#include "applib/ui/layer.h"

#include "util/size.h"

#include "clar.h"
#include "util.h"
#include "pebble_asserts.h"

#include <stdio.h>

// Helper Functions
////////////////////////////////////
#include "test_graphics.h"
#include "${BIT_DEPTH_NAME}/test_framebuffer.h"

// Stubs
////////////////////////////////////
#include "graphics_common_stubs.h"
#include "stubs_applib_resource.h"

static FrameBuffer *fb = NULL;

// Setup
void test_graphics_fill_circle_${BIT_DEPTH_NAME}__initialize(void) {
  fb = malloc(sizeof(FrameBuffer));
  framebuffer_init(fb, &(GSize) {DISP_COLS, DISP_ROWS});
}

// Teardown
void test_graphics_fill_circle_${BIT_DEPTH_NAME}__cleanup(void) {
  free(fb);
}

// Tests
////////////////////////////////////

void inside_layer_update_callback(Layer* me, GContext* ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_antialiased(ctx, false);
  graphics_fill_circle(ctx, GPoint(14, 14), 12);
}

void white_layer_update_callback(Layer* me, GContext* ctx) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_antialiased(ctx, false);
  graphics_fill_circle(ctx, GPoint(14, 14), 12);
}

void clear_layer_update_callback(Layer* me, GContext* ctx) {
  graphics_context_set_fill_color(ctx, GColorClear);
  graphics_context_set_antialiased(ctx, false);
  graphics_fill_circle(ctx, GPoint(14, 14), 12);
}

void across_x_layer_update_callback(Layer* me, GContext* ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_antialiased(ctx, false);
  graphics_fill_circle(ctx, GPoint(28, 14), 12);
}

void across_nx_layer_update_callback(Layer* me, GContext* ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_antialiased(ctx, false);
  graphics_fill_circle(ctx, GPoint(-14, 14), 12);
}

void across_y_layer_update_callback(Layer* me, GContext* ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_antialiased(ctx, false);
  graphics_fill_circle(ctx, GPoint(14, 28), 12);
}

void across_ny_layer_update_callback(Layer* me, GContext* ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_antialiased(ctx, false);
  graphics_fill_circle(ctx, GPoint(14, -14), 12);
}

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__origin_layer(void) {
  GContext ctx;
  Layer layer;
  test_graphics_context_init(&ctx, fb);
  layer_init(&layer, &GRect(0, 0, 28, 28));
  layer_set_update_proc(&layer, &inside_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_inside_origin_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_x_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_x_origin_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_nx_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_nx_origin_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_y_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_y_origin_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_ny_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_ny_origin_layer.${BIT_DEPTH_NAME}.pbi"));
}

#define RADIUS_BIG 15
#define RADIUS_MEDIUM 8
#define RADIUS_MIN_CALCULATED 3
#define RADIUS_MAX_PRECOMPUTED 2
#define RADIUS_SMALL 1
#define RADIUS_NONE 0

#define ORIGIN_RECT_NO_CLIP        GRect(0, 0, 144, 168)
#define ORIGIN_RECT_CLIP_XY        GRect(0, 0, 30, 40)
#define ORIGIN_RECT_CLIP_NXNY      GRect(0, 0, 30, 40)
#define CENTER_OF_ORIGIN_RECT      GPoint(20, 25)
#define CENTER_OF_ORIGIN_RECT_NXNY GPoint(10, 15)

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__origin_layer_aa(void) {
  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  // Big circles
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r16_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_XY, ORIGIN_RECT_CLIP_XY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r16_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_NXNY, ORIGIN_RECT_CLIP_NXNY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r16_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Medium circles
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r8_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_XY, ORIGIN_RECT_CLIP_XY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r8_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_NXNY, ORIGIN_RECT_CLIP_NXNY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r8_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Small circles
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r1_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_XY, ORIGIN_RECT_CLIP_XY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r1_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_NXNY, ORIGIN_RECT_CLIP_NXNY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r1_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Testing of the special cases for radius:

  // Radius of 3 - starting point for calculated edges
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_MIN_CALCULATED);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r3_no_clip.${BIT_DEPTH_NAME}.pbi"));

  // Radius of 2 - ending point for precomputed edges
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_MAX_PRECOMPUTED);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r2_no_clip.${BIT_DEPTH_NAME}.pbi"));

  // No circle
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_NONE);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_origin_aa_r0_no_clip.${BIT_DEPTH_NAME}.pbi"));
}

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__offset_layer(void) {
  GContext ctx;
  Layer layer;
  test_graphics_context_init(&ctx, fb);
  layer_init(&layer, &GRect(10, 15, 28, 28));
  layer_set_update_proc(&layer, &inside_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_inside_offset_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_x_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_x_offset_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_nx_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_nx_offset_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_y_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_y_offset_layer.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &across_ny_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_across_ny_offset_layer.${BIT_DEPTH_NAME}.pbi"));
}

#define OFFSET_RECT_NO_CLIP        GRect(10, 10, 40, 50)
#define OFFSET_RECT_CLIP_XY        GRect(10, 10, 30, 40)
#define OFFSET_RECT_CLIP_NXNY      GRect(0, 0, 30, 40)
#define CENTER_OF_OFFSET_RECT      GPoint(10, 15)
#define CENTER_OF_OFFSET_RECT_NXNY GPoint(0, 5)

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__offset_layer_aa(void) {
  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  // Big circles
  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_NO_CLIP, OFFSET_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r16_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_XY, OFFSET_RECT_CLIP_XY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r16_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_NXNY, OFFSET_RECT_CLIP_NXNY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT_NXNY, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r16_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Medium circles
  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_NO_CLIP, OFFSET_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r8_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_XY, OFFSET_RECT_CLIP_XY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r8_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_NXNY, OFFSET_RECT_CLIP_NXNY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT_NXNY, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r8_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Small circles
  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_NO_CLIP, OFFSET_RECT_NO_CLIP, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r1_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_XY, OFFSET_RECT_CLIP_XY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r1_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_NXNY, OFFSET_RECT_CLIP_NXNY, true, 1);
  graphics_fill_circle(&ctx, CENTER_OF_OFFSET_RECT_NXNY, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_offset_aa_r1_clip_nxny.${BIT_DEPTH_NAME}.pbi"));
}

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__quadrants_aa(void) {
  #if PBL_COLOR
  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  typedef struct {
    char *filename_part;
    GCornerMask mask;
  } TestConfig;

  TestConfig test_config[] = {
      {
          .filename_part = "quad_top_left",
          .mask = GCornerTopLeft,
      },
      {
          .filename_part = "quad_top_right",
          .mask = GCornerTopRight,
      },
      {
          .filename_part = "quad_bottom_right",
          .mask = GCornerBottomRight,
      },
      {
          .filename_part = "quad_bottom_left",
          .mask = GCornerBottomLeft,
      },
      {
          .filename_part = "quads_top",
          .mask = GCornersTop,
      },
      {
          .filename_part = "quads_bottom",
          .mask = GCornersBottom,
      },
      {
          .filename_part = "quads_right",
          .mask = GCornersRight,
      },
      {
          .filename_part = "quads_left",
          .mask = GCornersLeft,
      },
  };

  // note: not the prettiest, fast a quick way to render all the scenarios Nitin was interested in
  for (int i = 0; i < ARRAY_LENGTH(test_config); i++) {
    TestConfig c = test_config[i];
    setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
    GPoint pt = CENTER_OF_ORIGIN_RECT;
    // draw multiple quads with different radiuses
    for (int r = 1; r <= 15; r++) {
      graphics_internal_circle_quadrant_fill_aa(&ctx, pt, r, c.mask);

      // center point follows a grid
      pt.x += 30;
      if (pt.x > 120) {
        pt.x = CENTER_OF_ORIGIN_RECT.x;
        pt.y += 30;
      }
    }

    // construct file name and create meaningful assert description
    char filename[100];
    snprintf(filename, sizeof(filename),
        "fill_circle_offset_aa_%s.${BIT_DEPTH_NAME}.pbi", c.filename_part);
    cl_check_(gbitmap_pbi_eq(&ctx.dest_bitmap, filename), filename);
  }
  #endif
}

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__color(void) {
  GContext ctx;
  Layer layer;
  test_graphics_context_init(&ctx, fb);
  layer_init(&layer, &GRect(0, 0, 28, 28));
  layer_set_update_proc(&layer, &inside_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_inside_origin_layer.${BIT_DEPTH_NAME}.pbi"));
  layer_set_update_proc(&layer, &white_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(framebuffer_is_empty("white_over_black", ctx.parent_framebuffer, GColorWhite));

  test_graphics_context_reset(&ctx, fb);
  layer_set_update_proc(&layer, &inside_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_circle_inside_origin_layer.${BIT_DEPTH_NAME}.pbi"));
  layer_set_update_proc(&layer, &clear_layer_update_callback);
  layer_render_tree(&layer, &ctx);
  cl_check(framebuffer_is_empty("clear_over_black", ctx.parent_framebuffer, GColorWhite));
}

#define TO_TRIG(deg) (((deg) * TRIG_MAX_ANGLE) / 360)

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__radial(void){
  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  // Pacman
  uint32_t angle_end = TRIG_MAX_ANGLE + (TRIG_MAX_ANGLE / 8);
  uint32_t angle_start = (TRIG_MAX_ANGLE / 8) * 3;
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_radial_internal(&ctx, CENTER_OF_ORIGIN_RECT, 0, RADIUS_BIG, angle_start, angle_end);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_origin_aa_pacman.${BIT_DEPTH_NAME}.pbi"));

  // Letter C
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_radial_internal(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, RADIUS_BIG, angle_start,
                                angle_end);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_origin_aa_letter_c.${BIT_DEPTH_NAME}.pbi"));

  // Negative angles - uses same resource image as result should be identical
  angle_start -= TRIG_MAX_ANGLE;
  angle_end -= TRIG_MAX_ANGLE;

  // Pacman
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_radial_internal(&ctx, CENTER_OF_ORIGIN_RECT, 0, RADIUS_BIG, angle_start, angle_end);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_origin_aa_pacman.${BIT_DEPTH_NAME}.pbi"));

  // Letter C
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_radial_internal(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, RADIUS_BIG, angle_start,
                                angle_end);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_origin_aa_letter_c.${BIT_DEPTH_NAME}.pbi"));

  // table with most popular angles to test
  typedef struct {
    char *filename_part;
    uint32_t angle;
  } TestAngles;

  TestAngles test_angles[] = {
    {
      .filename_part = "__1_degrees",
      .angle = TRIG_MAX_ANGLE / 360,
    },
    {
      .filename_part = "__6_degrees",
      .angle = TRIG_MAX_ANGLE / 60,
    },
    {
      .filename_part = "_30_degrees",
      .angle = TRIG_MAX_ANGLE / 12,
    },
    {
      .filename_part = "_45_degrees",
      .angle = TRIG_MAX_ANGLE / 8,
    },
    {
      .filename_part = "_90_degrees",
      .angle = TRIG_MAX_ANGLE / 4,
    },
    {
      .filename_part = "181_degrees",
      .angle = TRIG_MAX_ANGLE / 2 + TRIG_MAX_ANGLE / 360,
    }
  };

  for (int i = 0; i < ARRAY_LENGTH(test_angles); i++) {
    setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
    GPoint pt = CENTER_OF_ORIGIN_RECT;
    uint16_t inner_radius = 0;
    uint16_t outer_radius = 10;

    for (int r = 1; r <= 8; r++) {
      graphics_fill_radial_internal(&ctx, pt, inner_radius, outer_radius, 0, test_angles[i].angle);

      inner_radius += 1;
      outer_radius += 3;
      pt.x += outer_radius * 2;
      if (pt.x > 120) {
        pt.x = CENTER_OF_ORIGIN_RECT.x;
        pt.y += outer_radius * 2;
      }
    }

    char filename[100];
    snprintf(filename, sizeof(filename),
             "fill_radial_offset_aa_end_angle_%s.${BIT_DEPTH_NAME}.pbi", test_angles[i].filename_part);
    cl_check_(gbitmap_pbi_eq(&ctx.dest_bitmap, filename), filename);
  }

  for (int i = 0; i < ARRAY_LENGTH(test_angles); i++) {
    setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
    GPoint pt = CENTER_OF_ORIGIN_RECT;
    uint16_t inner_radius = 0;
    uint16_t outer_radius = 10;

    for (int r = 1; r <= 8; r++) {
      graphics_fill_radial_internal(&ctx, pt, inner_radius, outer_radius, test_angles[i].angle,
                                    TRIG_MAX_ANGLE);

      inner_radius += 1;
      outer_radius += 3;
      pt.x += outer_radius * 2;
      if (pt.x > 120) {
        pt.x = CENTER_OF_ORIGIN_RECT.x;
        pt.y += outer_radius * 2;
      }
    }

    char filename[100];
    snprintf(filename, sizeof(filename),
             "fill_radial_offset_aa_start_angle_%s.${BIT_DEPTH_NAME}.pbi", test_angles[i].filename_part);
    cl_check_(gbitmap_pbi_eq(&ctx.dest_bitmap, filename), filename);
  }

  // table with radiuses
  typedef struct {
    char *filename_part;
    int radius;
  } TestRadiuses;

  TestRadiuses test_radiuses[] = {
    {
      .filename_part = "_inner_0",
      .radius = 0,
    },
    {
      .filename_part = "_inner_20",
      .radius = 15,
    }
  };

  // table with quadrants
  typedef struct {
    char *filename_part;
    int angle_start;
    int angle_end;
  } TestQuadrants;

  TestQuadrants test_quadrants[] = {
    {
      .filename_part = "_part",
      .angle_start = (TO_TRIG(-45)),
      .angle_end = (TO_TRIG(-45)),
    },
    {
      .filename_part = "_two_parts",
      .angle_start = 0,
      .angle_end = 0,
    },
    {
      .filename_part = "_quadrant_and_two_parts",
      .angle_start = 0,
      .angle_end = (TO_TRIG(90)),
    }
  };

#if PBL_COLOR
  //Colors table
  GColor colors[4] = {
    GColorBlack,
    GColorRed,
    GColorBlue,
    GColorGreen,
  };
#endif

  uint16_t outer_radius = 30;
  int32_t twelveth_of_angle = TRIG_MAX_ANGLE / 12;
  int32_t quarter_of_angle = TRIG_MAX_ANGLE / 4;
  GPoint center = GPoint(72, 84);

  // Cases for quadrant joints
  for (int i = 0; i < ARRAY_LENGTH(test_radiuses); i++) {
    for (int j = 0; j < ARRAY_LENGTH(test_quadrants); j++) {
      setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
      int offset_angle = 0;

      for (int r = 0; r < 4; r++) {
        int offset = (((r + 1) % 4 < 2) ? -20 : 20);
        GPoint pt = GPoint(center.x + ((r % 2 == 0) ? 0 : offset),
                           center.y + ((r % 2 == 0) ? offset * 2 : 0));

#if PBL_COLOR
        graphics_context_set_fill_color(&ctx, colors[r]);
#endif

        graphics_fill_radial_internal(&ctx, pt, test_radiuses[i].radius, outer_radius,
                                      test_quadrants[j].angle_start + offset_angle -
                                      twelveth_of_angle,
                                      test_quadrants[j].angle_end + offset_angle +
                                      twelveth_of_angle);

        offset_angle += quarter_of_angle;
      }

      char filename[100];
      snprintf(filename, sizeof(filename),
               "fill_radial_aa_joints_%s%s.${BIT_DEPTH_NAME}.pbi",
               test_radiuses[i].filename_part, test_quadrants[j].filename_part);
      cl_check_(gbitmap_pbi_eq(&ctx.dest_bitmap, filename), filename);
    }
  }
}

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__radial_precise(void){
  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  // letter C
  uint32_t angle_end = TRIG_MAX_ANGLE + (TRIG_MAX_ANGLE / 8);
  uint32_t angle_start = (TRIG_MAX_ANGLE / 8) * 3;
  GPointPrecise center = GPointPrecise(CENTER_OF_ORIGIN_RECT.x * 8, CENTER_OF_ORIGIN_RECT.y * 8);
  Fixed_S16_3 radius_inner = (Fixed_S16_3){.integer = RADIUS_MEDIUM};
  Fixed_S16_3 radius_outer = (Fixed_S16_3){.integer = RADIUS_BIG};

  // Drawing
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_radial_precise_internal(&ctx, center, radius_inner, radius_outer, angle_start, angle_end);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_origin_aa_precise_letter_c.${BIT_DEPTH_NAME}.pbi"));

  //Make the points utilise precision powers
  center.x.raw_value += 4;
  center.y.raw_value += 4;
  radius_inner.raw_value += 4;
  radius_outer.raw_value += 4;

  // Drawing
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_fill_radial_precise_internal(&ctx, center, radius_inner, radius_outer, angle_start, angle_end);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_origin_aa_precise_halves_letter_c.${BIT_DEPTH_NAME}.pbi"));
}

typedef struct {
  char *filename_part;
  int angle_start;
  int angle_end;
} TestRadialAnglesConfigs;

TestRadialAnglesConfigs test_radial_angles[] = {
  {
    .filename_part = "_part",
    .angle_start = (TO_TRIG(-45)),
    .angle_end = (TO_TRIG(-45)),
  },
  {
    .filename_part = "_two_parts",
    .angle_start = 0,
    .angle_end = 0,
  },
  {
    .filename_part = "_quadrant_and_two_parts",
    .angle_start = 0,
    .angle_end = (TO_TRIG(90)),
  },
  {
    .filename_part = "_two_quadrants_and_two_parts",
    .angle_start = 0,
    .angle_end = (TO_TRIG(180)),
  },
  {
    .filename_part = "_three_quadrants_and_two_parts",
    .angle_start = 0,
    .angle_end = (TO_TRIG(270)),
  },
  {
    .filename_part = "_full",
    .angle_start = 0,
    .angle_end = TRIG_MAX_ANGLE,
  }
};

typedef struct {
  char *filename_part;
  int16_t width;
  int16_t height;
  GOvalScaleMode scale_mode;
  int16_t inset;
} TestRadialGRectConfigs;

TestRadialGRectConfigs test_radial_rects[] = {
  {
    .filename_part = "_even_rect_fill",
    .width = 40,
    .height = 40,
    .scale_mode = GOvalScaleModeFillCircle,
    .inset = 10,
  },
  {
    .filename_part = "_even_rect_fit",
    .width = 40,
    .height = 40,
    .scale_mode = GOvalScaleModeFitCircle,
    .inset = 10,
  },
  {
    .filename_part = "_odd_rect_fill",
    .width = 41,
    .height = 41,
    .scale_mode = GOvalScaleModeFillCircle,
    .inset = 10,
  },
  {
    .filename_part = "_odd_rect_fit",
    .width = 41,
    .height = 41,
    .scale_mode = GOvalScaleModeFitCircle,
    .inset = 10,
  },
  {
    .filename_part = "_even_rect_fill_no_middle",
    .width = 40,
    .height = 40,
    .scale_mode = GOvalScaleModeFillCircle,
    .inset = 20,
  },
  {
    .filename_part = "_even_rect_fit_no_middle",
    .width = 40,
    .height = 40,
    .scale_mode = GOvalScaleModeFitCircle,
    .inset = 20,
  },
  {
    .filename_part = "_odd_rect_fill_no_middle",
    .width = 41,
    .height = 41,
    .scale_mode = GOvalScaleModeFillCircle,
    .inset = 21,
  },
  {
    .filename_part = "_odd_rect_fit_no_middle",
    .width = 41,
    .height = 41,
    .scale_mode = GOvalScaleModeFitCircle,
    .inset = 21,
  },
};

void prv_draw_radial_in_rect_debugged(GContext *ctx, int16_t width, int16_t height,
                                      GOvalScaleMode scale_mode, int16_t inset,
                                      int32_t angle_start, int32_t angle_end){

  int offset_angle = 0;
  int32_t twelveth_of_angle = TRIG_MAX_ANGLE / 12;
  GPoint center = GPoint(72, 84);

  for (int i=0; i<4; i++) {
    GRect rect = GRect(center.x - (width/2), center.y - (height / 2), width, height);
    int offset_x = (((i + 1) % 4 < 2) ? -(width * 2 / 3) : (width * 2 / 3));
    int offset_y = ((i % 4 < 2) ? -(height * 2 / 3) : (height * 2 / 3));
    rect.origin.x += offset_x;
    rect.origin.y += offset_y;

    const GRect bigger_rect = GRect(rect.origin.x - 1, rect.origin.y - 1,
                                    rect.size.w + 2, rect.size.h + 2);

    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorGreen, GColorWhite));
    graphics_draw_rect(ctx, &rect);
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
    graphics_draw_rect(ctx, &bigger_rect);
    graphics_context_set_stroke_color(ctx, GColorBlack);

    graphics_fill_radial(ctx, rect, scale_mode, inset,
                         angle_start + offset_angle - twelveth_of_angle,
                         angle_end + offset_angle + twelveth_of_angle);

    offset_angle += TRIG_MAX_ANGLE / 4;
  }
}

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__radial_grect(void) {
  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  for (int rect_id=0; rect_id < ARRAY_LENGTH(test_radial_rects); rect_id++) {
    for (int angle_id=0; angle_id < ARRAY_LENGTH(test_radial_angles); angle_id++) {
      setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);

      prv_draw_radial_in_rect_debugged(&ctx,
                                       test_radial_rects[rect_id].width,
                                       test_radial_rects[rect_id].height,
                                       test_radial_rects[rect_id].scale_mode,
                                       test_radial_rects[rect_id].inset,
                                       test_radial_angles[angle_id].angle_start,
                                       test_radial_angles[angle_id].angle_end);

      char filename[100];
      snprintf(filename, sizeof(filename),
               "fill_radial%s%s.${BIT_DEPTH_NAME}.pbi",
               test_radial_rects[rect_id].filename_part, test_radial_angles[angle_id].filename_part);
      cl_check_(gbitmap_pbi_eq(&ctx.dest_bitmap, filename), filename);
    }
  }
}

void prv_test_dithering_color(GContext *ctx, GColor color) {
  const uint32_t angle_end = DEG_TO_TRIGANGLE(405);
  const uint32_t angle_start = DEG_TO_TRIGANGLE(135);
  GRect rect = GRect(10, 10, 40, 40);

  setup_test_aa_sw(ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, 1);
  graphics_context_set_fill_color(ctx, color);

  // Circle
  graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, 50, 0, TRIG_MAX_ANGLE);

  // Pacman
  rect.origin.y += 50;
  graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, 50, angle_start, angle_end);

  // Letter C
  rect.origin.y += 50;
  graphics_fill_radial(ctx, rect, GOvalScaleModeFitCircle, 10, angle_start, angle_end);;

  // Following SHOULD NOT be dithered into grayscale:
  graphics_context_set_stroke_color(ctx, color);

  // Circle:
  GPoint point = GPoint(95, 56);
  graphics_draw_circle(ctx, point, 20);

  // Line:
  GPoint p1 = GPoint(75, 140);
  GPoint p2 = GPoint(115, 140);
  graphics_draw_line(ctx, p1, p2);

  // Stroked Circle:
  graphics_context_set_stroke_width(ctx, 12);
  point.y += 52;
  graphics_draw_circle(ctx, point, 20);

  // Stroked Line:
  p1.y += 10;
  p2.y += 10;
  graphics_draw_line(ctx, p1, p2);

  // Stroked line turning into circle:
  point.y = 20;
  graphics_draw_circle(ctx, point, 5);
}

void test_graphics_fill_circle_${BIT_DEPTH_NAME}__dithering_grayscale(void) {
  GContext ctx;

  test_graphics_context_init(&ctx, fb);
  prv_test_dithering_color(&ctx, GColorWhite);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_dither_GColorWhite.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_init(&ctx, fb);
  prv_test_dithering_color(&ctx, GColorLightGray);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_dither_GColorLightGray.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_init(&ctx, fb);
  prv_test_dithering_color(&ctx, GColorDarkGray);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_dither_GColorDarkGray.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_init(&ctx, fb);
  prv_test_dithering_color(&ctx, GColorBlack);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_dither_GColorBlack.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_init(&ctx, fb);
  prv_test_dithering_color(&ctx, GColorJaegerGreen);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_dither_GColorJaegerGreen.${BIT_DEPTH_NAME}.pbi"));

  test_graphics_context_init(&ctx, fb);
  prv_test_dithering_color(&ctx, GColorOrange);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "fill_radial_dither_GColorOrange.${BIT_DEPTH_NAME}.pbi"));
}
