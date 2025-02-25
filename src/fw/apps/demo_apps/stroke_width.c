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

#include "stroke_width.h"

#include <inttypes.h>
#include <stdbool.h>

#include "applib/app.h"
#include "applib/pbl_std/pbl_std.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "util/trig.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/window.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "system/logging.h"

#define STEP_ROTATION_ANGLE (TRIG_MAX_ANGLE / 360) // 1 degree
#define MIN_OPS 0
#define OP_ROTATE 0
#define OP_CHANGE_WIDTH 1
#define OP_MOVE_P1_X 2
#define OP_MOVE_P1_Y 3
#define OP_MOVE_P2_X 4
#define OP_MOVE_P2_Y 5
#define OP_TEST 6
#define OP_TEST2 7
#define OP_TEST3 8
#define OP_TEST4 9
#define OP_TEST5 10
#define OP_ROTATE2 11
#define OP_ROTATE3 12
#define MAX_OPS 13

#define MIN_STROKE 1
#define MAX_STROKE 100

static Window *s_window;

extern void prv_draw_stroked_line_precise(GContext* ctx, GPointPrecise p0, GPointPrecise p1,
                                          uint8_t width, bool anti_aliased);

typedef struct AppData {
  Layer *canvas_layer;
  Layer *debug_layer;

  int stroke_width;
  int size;
  uint32_t rotation_angle;
  GPoint p1;
  GPoint p2;

  int8_t operation;
} AppData;

static AppTimer *timer;

static void up_handler(ClickRecognizerRef recognizer, void *context) {
  AppData *data = window_get_user_data(s_window);

  switch (data->operation) {
    case OP_ROTATE:
      data->rotation_angle = (data->rotation_angle + STEP_ROTATION_ANGLE) % TRIG_MAX_ANGLE;
      break;
    case OP_CHANGE_WIDTH:
    case OP_TEST:
    case OP_TEST2:
    case OP_TEST3:
    case OP_TEST4:
    case OP_ROTATE2:
    case OP_ROTATE3:
      if (data->stroke_width < MAX_STROKE) data->stroke_width++;
      break;
    case OP_TEST5:
      if (data->size < 100) data->size++;
      break;
    case OP_MOVE_P1_X:
      data->p1.x++;
      break;
    case OP_MOVE_P1_Y:
      data->p1.y++;
      break;
    case OP_MOVE_P2_X:
      data->p2.x++;
      break;
    case OP_MOVE_P2_Y:
      data->p2.y++;
      break;
    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Invalid operation type: %d", data->operation);
      break;
  }

  layer_mark_dirty(data->canvas_layer);

  PBL_LOG(LOG_LEVEL_DEBUG, "line(p1(%d, %d), p2(%d, %d), width=%d), angle=%d)",
          data->p1.x, data->p1.y, data->p2.x, data->p2.y, data->stroke_width,
          (int)(data->rotation_angle * 360 / TRIG_MAX_ANGLE));
}

static void down_handler(ClickRecognizerRef recognizer, void *context) {
  AppData *data = window_get_user_data(s_window);

  switch (data->operation) {
    case OP_ROTATE:
      data->rotation_angle = (data->rotation_angle - STEP_ROTATION_ANGLE) % TRIG_MAX_ANGLE;
      break;
    case OP_CHANGE_WIDTH:
    case OP_TEST:
    case OP_TEST2:
    case OP_TEST3:
    case OP_TEST4:
    case OP_ROTATE2:
    case OP_ROTATE3:
      if (data->stroke_width > MIN_STROKE) data->stroke_width--;
      break;
    case OP_TEST5:
      if (data->size > 1) data->size--;
      break;
    case OP_MOVE_P1_X:
      data->p1.x--;
      break;
    case OP_MOVE_P1_Y:
      data->p1.y--;
      break;
    case OP_MOVE_P2_X:
      data->p2.x--;
      break;
    case OP_MOVE_P2_Y:
      data->p2.y--;
      break;
    default:
      PBL_LOG(LOG_LEVEL_ERROR, "Invalid operation type: %d", data->operation);
      break;
  }

  layer_mark_dirty(data->canvas_layer);

  PBL_LOG(LOG_LEVEL_DEBUG, "line(p1(%d, %d), p2(%d, %d), width=%d), angle=%d)",
          data->p1.x, data->p1.y, data->p2.x, data->p2.y, data->stroke_width,
          (int)(data->rotation_angle * 360 / TRIG_MAX_ANGLE));
}

static void select_handler(ClickRecognizerRef recognizer, void *context) {
  AppData *data = window_get_user_data(s_window);

  data->operation = (data->operation + 1) % MAX_OPS;

  if (data->operation < MIN_OPS) data->operation = MIN_OPS;

  layer_mark_dirty(data->canvas_layer);

  PBL_LOG(LOG_LEVEL_DEBUG, "current operation type: %d", data->operation);
}

static void click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_SELECT, 100, select_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_handler);
}

static void debug_layer_update_proc(Layer *layer, GContext* ctx) {
  // For frame/bounds/clipping rect debugging
  AppData *data = window_get_user_data(s_window);

  const GRect *bounds = &data->canvas_layer->bounds;
  const GRect *frame = &data->canvas_layer->frame;

  graphics_context_set_stroke_color(ctx, GColorGreen);
  graphics_draw_rect(ctx, &GRect(bounds->origin.x + frame->origin.x,
                                 bounds->origin.y + frame->origin.y,
                                 bounds->size.w, bounds->size.h));

  graphics_context_set_stroke_color(ctx, GColorRed);
  graphics_draw_rect(ctx, frame);
}

static void canvas_update_proc(Layer *layer, GContext* ctx) {
  AppData *data = window_get_user_data(s_window);
  GColor main_color = (GColor)GColorRoseVale;

  graphics_context_set_stroke_color(ctx, main_color);
  graphics_context_set_fill_color(ctx, main_color);

  if (data->operation == OP_TEST) {
    int x1 = 50,
    x2 = 100,
    y1 = 40,
    y2 = 120;

    GPoint p0 = GPoint(x1, y1);
    GPoint p1 = GPoint(x1, y2);
    GPoint p2 = GPoint(x2, y2);
    GPoint p3 = GPoint(x2, y1);

    graphics_context_set_antialiased(ctx, true);
    graphics_context_set_stroke_width(ctx, data->stroke_width);
    graphics_draw_line(ctx, p0, p1);
    graphics_draw_line(ctx, p1, p2);
    graphics_draw_line(ctx, p2, p3);
    graphics_draw_line(ctx, p3, p0);

    return;
  }

  if (data->operation == OP_TEST2) {
    int x1 = 50,
    x2 = 100,
    y1 = 40,
    y2 = 120;

    GPoint p0 = GPoint(x1, y1);
    GPoint p1 = GPoint(x1, y2);
    GPoint p2 = GPoint(x2, y2);
    GPoint p3 = GPoint(x2, y1);

    graphics_context_set_antialiased(ctx, true);
    graphics_context_set_stroke_width(ctx, data->stroke_width);
    graphics_draw_line(ctx, p0, p1);
    graphics_draw_line(ctx, p2, p3);

    return;
  }

  if (data->operation == OP_TEST3) {
    int x1 = 50,
    x2 = 100,
    y1 = 40,
    y2 = 120;

    GPoint p0 = GPoint(x1, y1);
    GPoint p1 = GPoint(x1, y2);
    GPoint p2 = GPoint(x2, y2);
    GPoint p3 = GPoint(x2, y1);

    // graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_antialiased(ctx, true);
    graphics_context_set_stroke_width(ctx, data->stroke_width);
    graphics_draw_line(ctx, p1, p2);
    graphics_draw_line(ctx, p3, p0);
    /*
     graphics_context_set_stroke_color(ctx, GColorWhite);
     graphics_context_set_antialiased(ctx, true);
     graphics_context_set_stroke_width(ctx, data->stroke_width);
     graphics_draw_line(ctx, GPoint(70, 100), GPoint(70, 101));
     */
    return;
  }

  if (data->operation == OP_TEST4 || data->operation == OP_TEST5) {
    /*
     GRect bounds = layer->bounds;
     GPoint center = GPoint(bounds.size.w/2, bounds.size.h/2);
     int radius = 40;

     graphics_context_set_stroke_color(ctx, GColorWhite);
     graphics_context_set_antialiased(ctx, true);
     graphics_context_set_stroke_width(ctx, data->stroke_width);
     graphics_draw_circle(ctx, center, data->size);
     */
    graphics_context_set_antialiased(ctx, true);
    graphics_context_set_stroke_width(ctx, data->stroke_width);
    graphics_draw_line(ctx, GPoint(70, 100), GPoint(70, 101));

    return;
  }

  if (data->operation == OP_ROTATE2) {
    int line_length = 60;
    time_t now;
    uint16_t now_ms = time_ms(&now, NULL);

    uint32_t seconds = pbl_override_localtime(&now)->tm_sec;
    uint32_t milliseconds = seconds * 1000 + now_ms;
    uint32_t rotation = milliseconds * TRIG_MAX_ANGLE / (60 * 1000);

    GPointPrecise p0;
    GPointPrecise p1;

    p0.x.raw_value = (data->canvas_layer->bounds.size.w / 2) * FIXED_S16_3_ONE.raw_value;
    p0.y.raw_value = (data->canvas_layer->bounds.size.h / 2) * FIXED_S16_3_ONE.raw_value;
    p1.x.raw_value = (sin_lookup(rotation) * (line_length * FIXED_S16_3_ONE.raw_value)
                      / TRIG_MAX_RATIO) + p0.x.raw_value;
    p1.y.raw_value = (-cos_lookup(rotation) * (line_length * FIXED_S16_3_ONE.raw_value)
                      / TRIG_MAX_RATIO) + p0.y.raw_value;

    graphics_context_set_antialiased(ctx, true);
    graphics_context_set_stroke_width(ctx, data->stroke_width);
    if (data->stroke_width >= 2) {
      graphics_line_draw_precise_stroked_aa(ctx, p0, p1, data->stroke_width);
    } else {
      graphics_draw_line(ctx, GPoint(p0.x.integer, p0.y.integer),
                         GPoint(p1.x.integer, p1.y.integer));
    }

    return;
  }

  if (data->operation == OP_ROTATE3) {
    int line_length = 60;
    time_t now;
    uint16_t now_ms = time_ms(&now, NULL);

    uint32_t seconds = pbl_override_localtime(&now)->tm_sec;
    uint32_t milliseconds = seconds * 1000 + now_ms;
    uint32_t rotation = milliseconds * TRIG_MAX_ANGLE / (60 * 1000);

    GPointPrecise p0;
    GPointPrecise p1;

    p0.x.raw_value = (data->canvas_layer->bounds.size.w / 2) * FIXED_S16_3_ONE.raw_value;
    p0.y.raw_value = (data->canvas_layer->bounds.size.h / 2) * FIXED_S16_3_ONE.raw_value;
    p1.x.raw_value = (sin_lookup(rotation) * (line_length * FIXED_S16_3_ONE.raw_value)
                      / TRIG_MAX_RATIO) + p0.x.raw_value;
    p1.y.raw_value = (-cos_lookup(rotation) * (line_length * FIXED_S16_3_ONE.raw_value)
                      / TRIG_MAX_RATIO) + p0.y.raw_value;

    graphics_context_set_antialiased(ctx, false);
    graphics_context_set_stroke_width(ctx, data->stroke_width);
    if (data->stroke_width >= 2) {
      graphics_line_draw_precise_stroked_non_aa(ctx, p0, p1, data->stroke_width);
    } else {
      graphics_draw_line(ctx, GPoint(p0.x.integer, p0.y.integer),
                         GPoint(p1.x.integer, p1.y.integer));
    }

    return;
  }

  int x1, y1, x2, y2;

  if (data->operation == OP_ROTATE) {
    int line_length = 60;

    x1 = data->canvas_layer->bounds.size.w / 2;
    y1 = data->canvas_layer->bounds.size.h / 2;
    x2 = (sin_lookup(data->rotation_angle) * line_length / TRIG_MAX_RATIO) + x1;
    y2 = (-cos_lookup(data->rotation_angle) * line_length / TRIG_MAX_RATIO) + y1;
  } else {
    x1 = data->p1.x;
    y1 = data->p1.y;
    x2 = data->p2.x;
    y2 = data->p2.y;
  }

  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_stroke_width(ctx, data->stroke_width);
  graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
}

static void main_window_load(Window *window) {
  AppData *data = window_get_user_data(s_window);
  Layer *window_layer = window_get_root_layer(window);
  window_set_background_color(window, GColorBlack);
  GRect window_bounds = window_layer->bounds;

  data->debug_layer = layer_create(window_bounds);
  layer_set_update_proc(data->debug_layer, debug_layer_update_proc);
  layer_add_child(window_layer, data->debug_layer);

  data->canvas_layer = layer_create(GRect(10, 10, window_bounds.size.w - 20,
                                          window_bounds.size.h - 20));
  layer_set_update_proc(data->canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, data->canvas_layer);

  // For drawing_rect testing...
  // layer_set_bounds(data->canvas_layer, &GRect(0, 0,
  //                    window_bounds.size.w - 20, window_bounds.size.h - 20));

  data->stroke_width = 10;
}

void timer_callback(void *d) {
  AppData *data = window_get_user_data(s_window);

  layer_mark_dirty(data->canvas_layer);

  timer = app_timer_register(30, timer_callback, NULL);
}

static void main_window_unload(Window *window) {
  AppData *data = window_get_user_data(s_window);
  layer_destroy(data->canvas_layer);
  layer_destroy(data->debug_layer);
}

static void init(void) {
  AppData *data = task_zalloc(sizeof(AppData));
  if (!data) {
    return;
  }

  s_window = window_create();
  window_set_user_data(s_window, data);
  window_set_fullscreen(s_window, true);
  window_set_window_handlers(s_window, &(WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });

  data->p1.x = 10;
  data->p1.y = 30;
  data->p2.x = 100;
  data->p2.y = 120;
  data->size = 40;
  data->operation = MIN_OPS;

  window_set_click_config_provider(s_window, click_config_provider);

  const bool animated = true;
  app_window_stack_push(s_window, animated);

  timer = app_timer_register(30, timer_callback, NULL);
}

static void deinit(void) {
  AppData *data = window_get_user_data(s_window);
  task_free(data);
  window_destroy(s_window);
}

static void s_main(void) {
  init();
  app_event_loop();
  deinit();
}

const PebbleProcessMd* stroke_width_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common.main_func = s_main,
    .name = "Stroke Width"
  };
  return (const PebbleProcessMd*) &s_app_info;
}
