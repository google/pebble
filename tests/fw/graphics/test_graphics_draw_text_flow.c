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
#include "fixtures/load_test_resources.h"

#include "applib/fonts/fonts_private.h"
#include "applib/graphics/framebuffer.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/text.h"
#include "applib/graphics/text_resources.h"
#include "applib/ui/layer.h"
#include "applib/ui/window_private.h"
#include "resource/resource_ids.auto.h"
#include "util/size.h"


// Helper Functions
////////////////////////////////////
#include "test_graphics.h"
#include "8bit/test_framebuffer.h"
#include "util.h"

///////////////////////////////////////////////////////////
// Stubs
#include "stubs_analytics.h"
#include "stubs_app_state.h"
#include "stubs_applib_resource.h"
#include "stubs_bootbits.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_memory_layout.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_print.h"
#include "stubs_prompt.h"
#include "stubs_serial.h"
#include "stubs_sleep.h"
#include "stubs_syscall_internal.h"
#include "stubs_syscalls.h"
#include "stubs_system_reset.h"
#include "stubs_task_watchdog.h"
#include "stubs_ui_window.h"
#include "stubs_unobstructed_area.h"

///////////////////////////////////////////////////////////
// Tests

static FrameBuffer *fb = NULL;
static GContext ctx;

#define NUM_STEPS (5)
#define DELTA 20
static FontInfo s_font_info;
static GBitmap *s_dest_bitmap;
static char *s_text = "A B C D E F G "
  "H I J K L M N O P Q R S T U V W X Y Z a b c d e f g h i j k l m n o "
  "p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R"
  "S T U V W X Y Z a b c d e f g h i j k l m n o p q r s t u v w x y z";

void prv_prepare_fb_steps_xy(GSize size, int16_t steps_x, int16_t steps_y) {
  gbitmap_destroy(s_dest_bitmap);
  s_dest_bitmap = gbitmap_create_blank(GSize(size.w * steps_x, size.h * steps_y),
                                       GBITMAP_NATIVE_FORMAT);
  ctx.dest_bitmap = *s_dest_bitmap;
  ctx.draw_state.clip_box = (GRect){.size = size};
  ctx.draw_state.drawing_box = ctx.draw_state.clip_box;
  graphics_context_set_text_color(&ctx, GColorBlack);
  graphics_context_set_fill_color(&ctx, GColorLightGray);
  memset(s_dest_bitmap->addr, 0xff, s_dest_bitmap->row_size_bytes * s_dest_bitmap->bounds.size.h);
}

void prv_prepare_fb_steps(GSize size) {
  prv_prepare_fb_steps_xy(size, NUM_STEPS, NUM_STEPS);
}

void test_graphics_draw_text_flow__initialize(void) {
  fb = malloc(sizeof(FrameBuffer));
  framebuffer_init(fb, &(GSize) {DISP_COLS, DISP_ROWS});
  ctx = (GContext){};

  // Setup resources
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
  pfs_format(true /* write erase headers */);
  load_resource_fixture_in_flash(RESOURCES_FIXTURE_PATH, SYSTEM_RESOURCES_FIXTURE_NAME, false /* is_next */);

  resource_init();

  memset(&s_font_info, 0, sizeof(s_font_info));

  cl_assert(text_resources_init_font(0, RESOURCE_ID_GOTHIC_18_BOLD, 0, &s_font_info));

  test_graphics_context_init(&ctx, fb);

  setup_test_aa_sw(&ctx, fb, ctx.dest_bitmap.bounds, ctx.dest_bitmap.bounds, false, 1);

  prv_prepare_fb_steps(GSize(DISP_COLS, DISP_ROWS));
}

void test_graphics_draw_text_flow__cleanup(void) {
  free(fb);
  gbitmap_destroy(s_dest_bitmap);
  s_dest_bitmap = NULL;
}


#define RECT_TEXT_0_0 GRect(0, 0, DISP_COLS, DISP_ROWS)
GRangeHorizontal perimeter_for_display_round(const GPerimeter *perimeter,
                                             const GSize *ctx_size,
                                             GRangeVertical vertical_range,
                                             uint16_t inset);

static uint8_t *prv_bitmap_offset_for_steps(GBitmap *bmp, int sx, int sy,
                                            int steps_x, int steps_y) {
  sx += (steps_x - 1) / 2;
  sy += (steps_y - 1) / 2;

  int16_t step_w = bmp->bounds.size.w / steps_x;
  int16_t step_h = bmp->bounds.size.h / steps_y;

  return ((uint8_t *)bmp->addr) + (sy * step_h * bmp->row_size_bytes) + (sx * step_w);
}

typedef enum {
  RenderMoveTextBox,
  RenderMoveDrawBox,
} RenderMoveMode;

void render_steps(TextLayoutExtended *layout, RenderMoveMode mode, int delta, int16_t height,
                  char **texts) {
  const GTextLayoutCacheRef layout_cache = (GTextLayoutCacheRef const)layout;

  int steps_x = ctx.dest_bitmap.bounds.size.w / ctx.draw_state.clip_box.size.w;
  int steps_y = ctx.dest_bitmap.bounds.size.h / ctx.draw_state.clip_box.size.h;

  int text_idx = 0;
  for (int sx = -((steps_x - 1) / 2) ; sx <= steps_x / 2; sx++) {
    for (int sy = -((steps_y - 1) / 2); sy <= steps_y / 2; sy++) {
      // as draw_text internally uses absolute coordinates to derive its state we cannot
      // simply adjust the draw_box to accomplish a side-by-side comparison
      ctx.dest_bitmap.addr = prv_bitmap_offset_for_steps(s_dest_bitmap, sx, sy, steps_x, steps_y);

      GRect box = {.size = GSize(DISP_COLS, height)};
      const GPoint origin = GPoint(delta * sx, delta * sy);
      if (mode == RenderMoveTextBox) {
        box.origin = origin;
        ctx.draw_state.drawing_box.origin = GPointZero;
      } else {
        ctx.draw_state.drawing_box.origin = origin;
      }

      graphics_fill_rect(&ctx, &box);
      graphics_draw_rect(&ctx, &(GRect){.origin = GPoint(-ctx.draw_state.drawing_box.origin.x,
                                                        -ctx.draw_state.drawing_box.origin.y),
                                       .size = ctx.draw_state.clip_box.size});

      char *text = texts ? texts[text_idx++] : s_text;
      graphics_draw_text(&ctx, text, &s_font_info, box,
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, layout_cache);
    }
  }
}

void test_graphics_draw_text_flow__flow_no_paging(void) {
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback=perimeter_for_display_round},
      .perimeter.inset = 8,
    },
  };
  render_steps(&layout, RenderMoveTextBox, DELTA, DISP_ROWS, NULL);
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));
}

void test_graphics_draw_text_flow__flow_no_paging_draw_box(void) {
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback=perimeter_for_display_round},
      .perimeter.inset = 8,
    },
  };
  render_steps(&layout, RenderMoveDrawBox, DELTA, DISP_ROWS, NULL);
  // should result in the very same output as if you did a placement via text box
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, "test_graphics_draw_text_flow__flow_no_paging.pbi"));
}

void test_graphics_draw_text_flow__with_origin_zero(void) {
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback=perimeter_for_display_round},
      .perimeter.inset = 8,
      .paging.page_on_screen.size_h = DISP_ROWS, // setting a page height != enables positioning
      .paging.origin_on_screen = {0, 0},
    },
  };
  render_steps(&layout, RenderMoveTextBox, DELTA, DISP_ROWS, NULL);
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));
}

void test_graphics_draw_text_flow__with_origin_non_zero(void) {
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback=perimeter_for_display_round},
      .perimeter.inset = 8,
      .paging.page_on_screen.size_h = DISP_ROWS, // setting a page height != enables positioning
      .paging.origin_on_screen = {DELTA, 2 * DELTA},
    },
  };
  render_steps(&layout, RenderMoveTextBox, DELTA, DISP_ROWS, NULL);
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));
}

void test_graphics_draw_text_flow__with_paging(void) {
  prv_prepare_fb_steps(GSize(DISP_COLS, 2 * DISP_ROWS));
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback=perimeter_for_display_round},
      .perimeter.inset = 8,
      .paging.page_on_screen = {
        .origin_y = 25,
        .size_h = 100
      }, // setting a page height != enables positioning
    },
  };
  render_steps(&layout, RenderMoveTextBox, DELTA, 1000, NULL);
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));
}

void test_graphics_draw_text_flow__avoid_repeat_text_to_avoid_orphans(void) {
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback=perimeter_for_display_round},
      .perimeter.inset = 8,
      .paging.page_on_screen = {
        .origin_y = 25,
        .size_h = 100
      }, // setting a page height != enables positioning
    },
  };

  char first_page_one_line[] = "A B C D E F G H I";
  char second_page_one_line[] = "A B C D E F G H I J K L M N";
  char second_page_two_lines[] = "A B C D E F G H I J K L M N O P Q R S T U V";
  char second_page_full[] = "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z "
                            "a b c d e f g h j k l m n o p q r s t u v w x y z "
                            "A";
  char third_page_one_line[] = "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z "
                               "a b c d e f g h j k l m n o p q r s t u v w x y z "
                               "A B C D E F G";
  char third_page_two_lines[] = "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z "
                                "a b c d e f g h j k l m n o p q r s t u v w x y z "
                                "A B C D E F G I J K L M N O P";
  char third_page_full[] = "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z "
                           "a b c d e f g h j k l m n o p q r s t u v w x y z "
                           "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z "
                           "a b c d e f g h j k l m n o p q r s t u";
  char fourth_page_one_line[] = "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z "
                                "a b c d e f g h j k l m n o p q r s t u v w x y z "
                                "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z "
                                "a b c d e f g h j k l m n o p q r s t u v w x y z";
  char *texts[] = {
    first_page_one_line,
    second_page_one_line, second_page_two_lines, second_page_full,
    third_page_one_line, third_page_two_lines, third_page_full, fourth_page_one_line,
  };

  const int16_t num_steps = ARRAY_LENGTH(texts);
  const GSize size = GSize(144, 300);

  prv_prepare_fb_steps_xy(size, num_steps, 1);
  ctx.draw_state.avoid_text_orphans = true;

  render_steps(&layout, RenderMoveDrawBox, 0, size.h, texts);

  // draw markers to visualize page breaks
  ctx.draw_state.clip_box = s_dest_bitmap->bounds;
  ctx.draw_state.drawing_box = s_dest_bitmap->bounds;
  ctx.dest_bitmap.addr = s_dest_bitmap->addr;
  graphics_context_set_stroke_color(&ctx, GColorDarkGray);
  int16_t y = layout.flow_data.paging.page_on_screen.origin_y;
  while (y < size.h) {
    graphics_draw_line(&ctx, GPoint(0, y), GPoint(s_dest_bitmap->bounds.size.w, y));
    y += layout.flow_data.paging.page_on_screen.size_h;
  }

  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));
}

GRangeHorizontal perimeter_for_circle(GRangeVertical vertical_range, GPoint center, int32_t radius);
GRangeHorizontal perimeter_for_display_rect(const GPerimeter *perimeter,
                                            const GSize *ctx_size,
                                            GRangeVertical vertical_range,
                                            uint16_t inset);

// easiest way to make these dimensions identical to spalding although the tests take
// defaults from basalt's screen resolution. The original `perimeter_for_display_round`
// uses the platform-specific `DISP_FRAME`
static GRangeHorizontal prv_perimeter_for_display_round(const GPerimeter *perimeter,
                                                        const GSize *ctx_size,
                                                        GRangeVertical vertical_range,
                                                        uint16_t inset) {
  const GRect disp_180_frame = GRect(0, 0, 180, 180);
  const GPoint center = grect_center_point(&disp_180_frame);
  const int32_t radius = grect_shortest_side(disp_180_frame) / 2 - inset;
  return perimeter_for_circle(vertical_range, center, radius);
}


void test_graphics_draw_text_flow__draw_text_doom(void) {
  // text and configuration we see in text_flow demo app
  cl_assert(text_resources_init_font(0, RESOURCE_ID_GOTHIC_24_BOLD, 0, &s_font_info));
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback = prv_perimeter_for_display_round},
      .perimeter.inset = 8,
      .paging.page_on_screen = {
        .origin_y = 48,
        .size_h = 85
      },
      .paging.origin_on_screen.y = 412,
    },
  };
  char text[] = "Dib: You're just jealous...\nZim: This has nothing to do with jelly!\n"
    "Zim: You dare agree with me? Prepare to meet your horrible doom!";

  prv_prepare_fb_steps_xy(GSize(180, 300), 1, 1);
  ctx.draw_state.avoid_text_orphans = true;
  graphics_draw_text(&ctx, text, &s_font_info, GRect(0, 0, 180, 1000),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     (GTextAttributes *const) &layout);
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));

  prv_prepare_fb_steps_xy(GSize(180, 300), 1, 1);
  ctx.draw_state.avoid_text_orphans = true;
  ctx.draw_state.clip_box.origin.y = 48;
  ctx.draw_state.clip_box.size.h = 85;
  ctx.draw_state.drawing_box.origin.y = -183;
  graphics_draw_text(&ctx, text, &s_font_info, GRect(0, 0, 180, 1000),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     (GTextAttributes *const) &layout);
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, namecat(namecat(__func__, "__clipped"), ".pbi")));

  prv_prepare_fb_steps_xy(GSize(180, 300), 1, 1);
  ctx.draw_state.avoid_text_orphans = false;
  graphics_draw_text(&ctx, text, &s_font_info, GRect(0, 0, 180, 1000),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     (GTextAttributes *const) &layout);
  cl_check(gbitmap_pbi_eq(s_dest_bitmap, namecat(namecat(__func__, "__with_orphan"), ".pbi")));
}

void test_graphics_draw_text_flow__max_used_size_draw_text_doom(void) {
  cl_assert(text_resources_init_font(0, RESOURCE_ID_GOTHIC_24_BOLD, 0, &s_font_info));

  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback = prv_perimeter_for_display_round},
      .perimeter.inset = 8,
      .paging.page_on_screen = {
        .origin_y = 48,
        .size_h = 85
      },
      .paging.origin_on_screen.y = 412,
    },
  };
  void *layout_ref = (void *)&layout;

  char text[] = "Dib: You're just jealous...\nZim: This has nothing to do with jelly!\n"
    "Zim: You dare agree with me? Prepare to meet your horrible doom!";

  const GFont font = &s_font_info;
  const GRect box = GRect(0, 0, 180, 1000);
  const GTextOverflowMode overflow_mode = GTextOverflowModeTrailingEllipsis;
  const GTextAlignment text_alignment = GTextAlignmentCenter;
  const GSize fb_size = GSize(180, 300);
  const int16_t steps_x = 1;
  const int16_t steps_y = 1;

  prv_prepare_fb_steps_xy(fb_size, steps_x, steps_y);
  ctx.draw_state.avoid_text_orphans = true;

  const GSize size_with_orphan_avoidance = graphics_text_layout_get_max_used_size(&ctx,
                                                                                  text, font, box,
                                                                                  overflow_mode,
                                                                                  text_alignment,
                                                                                  layout_ref);

  // TODO: PBL-34191 move .avoid_text_orphans from GContext to TextLayout so layout is invalidated
  // Invalidate the layout so it will be recalculated for the next step
  layout.hash = 0;

  prv_prepare_fb_steps_xy(fb_size, steps_x, steps_y);
  ctx.draw_state.avoid_text_orphans = false;

  const GSize size_without_orphan_avoidance = graphics_text_layout_get_max_used_size(&ctx,
                                                                                     text, font,
                                                                                     box,
                                                                                     overflow_mode,
                                                                                     text_alignment,
                                                                                     layout_ref);

  // We should get different heights because the orphan avoidance algorithm adds an extra line
  cl_assert_equal_i(size_with_orphan_avoidance.h, 279);
  cl_assert_equal_i(size_without_orphan_avoidance.h, 255);
}

void test_graphics_draw_text_flow__no_infinite_loop(void) {
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback = perimeter_for_display_rect},
    },
  };
  char text[] = "Prevent orphans for tall-enough pages.";
  const int16_t line_height = 22;
  // some more pixels to show that orphan prevention really only applies if there's enough space
  // for enough *full* lines
  const int16_t some = 5;
  prv_prepare_fb_steps_xy(GSize(180, 300), 3, 1);
  ctx.draw_state.avoid_text_orphans = true;

  for (int i = 0; i < 3; i++) {
    const int number_of_lines_per_page = i + 1;
    layout.flow_data.paging.page_on_screen.size_h = number_of_lines_per_page * line_height + some;
    layout.flow_data.paging.origin_on_screen.y =
      layout.flow_data.paging.page_on_screen.size_h - line_height;
    graphics_draw_text(&ctx, text, &s_font_info, GRect(0, 0, 180, 1000),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                       (GTextAttributes *const) &layout);

    const int16_t second_page_start_y =
      (layout.flow_data.paging.page_on_screen.size_h - layout.flow_data.paging.origin_on_screen.y);
    const int16_t second_page_end_y =
      (second_page_start_y + layout.flow_data.paging.page_on_screen.size_h);
    graphics_draw_line(&ctx, GPoint(0, second_page_start_y), GPoint(180, second_page_start_y));
    graphics_draw_line(&ctx, GPoint(0, second_page_end_y), GPoint(180, second_page_end_y));
    ctx.draw_state.drawing_box.origin.x += 180;
    ctx.draw_state.clip_box.origin.x += 180;
  }

  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));
}

void test_graphics_draw_text_flow__no_infinite_loop2(void) {
  // replicates the bug described in PBL-29267 noticed in the notification app
  // the following values are those we measured in GDB when it entered the infinite loop
  cl_assert(text_resources_init_font(0, RESOURCE_ID_GOTHIC_24_BOLD, 0, &s_font_info));
  TextLayoutExtended layout = {
    .flow_data = {
      .perimeter.impl = &(GPerimeter){.callback = prv_perimeter_for_display_round,},
      .perimeter.inset = 8,
      .paging = {
        .origin_on_screen = GPoint(12, 83),
        .page_on_screen.origin_y = 24,
        .page_on_screen.size_h = 140,
      }
    },
  };
  char text[] = "Late again? Can you be on time ever? Seriosly? Dude!!!";
  prv_prepare_fb_steps_xy(GSize(180, 360), 1, 1);
  ctx.draw_state.avoid_text_orphans = true;

  GRect box = (GRect){.origin = {.x = 12, .y = 59}, .size = {.w = 156, .h = 2480}};
  graphics_draw_text(&ctx, text, &s_font_info, box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     (GTextAttributes *const) &layout);

  cl_check(gbitmap_pbi_eq(s_dest_bitmap, TEST_PBI_FILE));
}
