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

#include "watch_model.h"

#include "applib/app.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/gpath.h"
#include "applib/graphics/graphics_circle.h"
#include "applib/graphics/text.h"
#include "util/trig.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "services/common/clock.h"
#include "util/time/time.h"

typedef struct {
  Window window;
  GFont text_font;
  ClockModel clock_model;
  GBitmap *bg_bitmap;
  GPath *hour_path;
  GPath *minute_path;
} MultiWatchData;

static const GPathInfo HOUR_PATH_INFO = {
  .num_points = 9,
  .points = (GPoint[]) {
    {-5, 10}, {-2, 10}, {-2, 15}, {2, 15}, {2, 10}, {5, 10}, {5, -51}, {0, -56}, {-5, -51},
  },
};

static const GPathInfo MINUTE_PATH_INFO = {
  .num_points = 5,
  .points = (GPoint[]) {
    {-5, 10}, {5, 10}, {5, -61}, {0, -66}, {-5, -61},
  },
};

void watch_model_handle_change(ClockModel *model) {
  MultiWatchData *data = app_state_get_user_data();
  data->clock_model = *model;
  layer_mark_dirty(window_get_root_layer(&data->window));
}

static GPointPrecise prv_gpoint_from_polar(const GPointPrecise *center, uint32_t distance,
                                                int32_t angle) {
  return gpoint_from_polar_precise(center, distance << GPOINT_PRECISE_PRECISION, angle);
}

static void prv_graphics_draw_centered_text(GContext *ctx, const GSize *max_size,
                                            const GPoint *center, const GFont font,
                                            const GColor color, const char *text) {
  GSize text_size = app_graphics_text_layout_get_content_size(
      text, font, (GRect) { .size = *max_size }, GTextOverflowModeFill, GTextAlignmentCenter);
  GPoint text_center = *center;
  text_center.x -= text_size.w / 2 + 1;
  text_center.y -= text_size.h * 2 / 3;
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, (GRect) { .origin = text_center, .size = text_size },
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

static void prv_draw_watch_hand_rounded(GContext *ctx, ClockHand *hand, GPointPrecise center) {
  GPointPrecise watch_hand_end = prv_gpoint_from_polar(&center, hand->length, hand->angle);
  if (hand->style == CLOCK_HAND_STYLE_ROUNDED_WITH_HIGHLIGHT) {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_line_draw_precise_stroked_aa(ctx, center, watch_hand_end, hand->thickness + 2);
  }
  graphics_context_set_stroke_color(ctx, hand->color);
  graphics_line_draw_precise_stroked_aa(ctx, center, watch_hand_end, hand->thickness);
}

static void prv_draw_watch_hand_pointed(GContext *ctx, ClockHand *hand, GPoint center,
                                        GPath *path) {
  graphics_context_set_fill_color(ctx, hand->color);
  gpath_rotate_to(path, hand->angle);
  gpath_move_to(path, center);
  gpath_draw_filled(ctx, path);
}

static void prv_draw_watch_hand(GContext *ctx, ClockHand *hand, GPointPrecise center, GPath *path) {
  switch (hand->style) {
    case CLOCK_HAND_STYLE_POINTED:
      prv_draw_watch_hand_pointed(ctx, hand, GPointFromGPointPrecise(center), path);
    case CLOCK_HAND_STYLE_ROUNDED:
    case CLOCK_HAND_STYLE_ROUNDED_WITH_HIGHLIGHT:
    default:
      prv_draw_watch_hand_rounded(ctx, hand, center);
      break;
  }
}

static GPointPrecise prv_get_clock_center_point(ClockLocation location, const GRect *bounds) {
  GPoint imprecise_center_point = {0};
  switch (location) {
    case CLOCK_LOCATION_TOP:
      imprecise_center_point = (GPoint) {
        .x = bounds->size.w / 2,
        .y = bounds->size.h / 4,
      };
    case CLOCK_LOCATION_RIGHT:
      imprecise_center_point = (GPoint) {
        .x = bounds->size.w * 3 / 4 - 5,
        .y = bounds->size.h / 2,
      };
    case CLOCK_LOCATION_BOTTOM:
      imprecise_center_point = (GPoint) {
        .x = bounds->size.w / 2,
        .y = bounds->size.h * 3 / 4 + 6,
      };
    case CLOCK_LOCATION_LEFT:
      imprecise_center_point = (GPoint) {
        .x = bounds->size.w / 4 + 4,
        .y = bounds->size.h / 2,
      };
    default:
      // aiming for width / 2 - 0.5 to get the true center
      return (GPointPrecise) {
        .x = { .integer = bounds->size.w / 2 - 1, .fraction = 3 },
        .y = { .integer = bounds->size.h / 2 - 1, .fraction = 3 }
      };
  }
  return GPointPreciseFromGPoint(imprecise_center_point);
}

static void prv_draw_clock_face(GContext *ctx, ClockFace *face) {
  MultiWatchData *data = app_state_get_user_data();
  const GRect *bounds = &window_get_root_layer(&data->window)->bounds;
  const GPointPrecise center = prv_get_clock_center_point(face->location, bounds);

  // Draw hands.
  // TODO: Need to do something about the static GPaths used for watchbands. This is very inflexible.
  prv_draw_watch_hand(ctx, &face->hour_hand, center, data->hour_path);
  prv_draw_watch_hand(ctx, &face->minute_hand, center, data->minute_path);

  // Draw bob.
  GRect bob_rect = (GRect) {
      .size = GSize(face->bob_radius * 2, face->bob_radius * 2)
  };
  GRect bob_center_rect = (GRect) {
      .size = GSize(face->bob_center_radius * 2, face->bob_center_radius * 2)
  };
  grect_align(&bob_rect, bounds, GAlignCenter, false /* clips */);
  grect_align(&bob_center_rect, bounds, GAlignCenter, false /* clips */);
  graphics_context_set_fill_color(ctx, face->bob_color);
  graphics_fill_oval(ctx, bob_rect, GOvalScaleModeFitCircle);
  graphics_context_set_fill_color(ctx, face->bob_center_color);
  graphics_fill_oval(ctx, bob_center_rect, GOvalScaleModeFitCircle);
}

static void prv_draw_non_local_clock(GContext *ctx, NonLocalClockFace *clock) {
  // TODO: The non-local clock text is currently baked into the background image.
  prv_draw_clock_face(ctx, &clock->face);
}


static void prv_update_proc(Layer *layer, GContext *ctx) {
  MultiWatchData *data = app_state_get_user_data();

  const GRect *bounds = &layer->bounds;

  // Background.
  graphics_draw_bitmap_in_rect(ctx, data->bg_bitmap, bounds);

  ClockModel *clock_model = &data->clock_model;

  // Watch text. TODO: Locate the text properly, rather than hard-coding.
  if (clock_model->text.location == CLOCK_TEXT_LOCATION_BOTTOM) {
    const GPoint point = (GPoint) { 90, 140 };
    prv_graphics_draw_centered_text(ctx, &bounds->size, &point, data->text_font,
                                    clock_model->text.color, clock_model->text.buffer);
  } else if (clock_model->text.location == CLOCK_TEXT_LOCATION_LEFT) {
    const GRect box = (GRect) { .origin = GPoint(25, 78), .size = bounds->size };
    graphics_draw_text(ctx, clock_model->text.buffer, data->text_font, box,
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }

  // Draw the clocks.
  for (uint32_t i = 0; i < clock_model->num_non_local_clocks; ++i) {
    prv_draw_non_local_clock(ctx, &clock_model->non_local_clock[i]);
  }
  prv_draw_clock_face(ctx, &clock_model->local_clock);
}

static void prv_window_load(Window *window) {
  MultiWatchData *data = app_state_get_user_data();

  layer_set_update_proc(window_get_root_layer(window), prv_update_proc);

  watch_model_init();

  data->hour_path = gpath_create(&HOUR_PATH_INFO);
  data->minute_path = gpath_create(&MINUTE_PATH_INFO);
  data->bg_bitmap = gbitmap_create_with_resource(data->clock_model.bg_bitmap_id);
}

static void prv_window_unload(Window *window) {
  MultiWatchData *data = app_state_get_user_data();
  gpath_destroy(data->hour_path);
  gpath_destroy(data->minute_path);
  gbitmap_destroy(data->bg_bitmap);
}

static void prv_app_did_focus(bool did_focus) {
  if (!did_focus) {
    return;
  }
  app_focus_service_unsubscribe();
  watch_model_start_intro();
}

static void prv_init(void) {
  MultiWatchData *data = app_zalloc_check(sizeof(*data));
  app_state_set_user_data(data);

  data->text_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  window_init(&data->window, "TicToc");
  window_set_window_handlers(&data->window, &(WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  const bool animated = true;
  app_window_stack_push(&data->window, animated);

  app_focus_service_subscribe_handlers((AppFocusHandlers) {
    .did_focus = prv_app_did_focus,
  });
}

static void prv_deinit(void) {
  MultiWatchData *data = app_state_get_user_data();
  window_destroy(&data->window);
  watch_model_cleanup();
}

void tictoc_main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
