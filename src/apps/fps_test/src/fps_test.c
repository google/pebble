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

#include "test_bitmaps.h"
#include <pebble.h>

typedef struct AppData {
  Window *window;
  BitmapLayer *background_layer;
  BitmapLayer *topleft_layer;
  MenuLayer *action_list1;
  MenuLayer *action_list2;

  int64_t time_started;
  uint32_t rendered_frames;
} AppData;

static int64_t prv_time_64(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (int64_t)s * 1000 + ms;
}

static void prv_redraw_timer_cb(void *cb_data) {
  AppData *data = (AppData *)cb_data;

  Layer *root_layer = window_get_root_layer(data->window);
  layer_mark_dirty(root_layer);
  app_timer_register(0, prv_redraw_timer_cb, data);
}

// The profiler functions are in the exported symbols table but not in the header
// because we aren't quite ready to expose them to 3rd party developers
extern void __profiler_init(void);
extern void __profiler_print_stats(void);
extern void __profiler_start(void);
extern void __profiler_stop(void);

/*****************************************************************************************
  Stop our timer and display results.

  A frame update consists of the following operations:
  op_1) App renders to its own frame buffer
  op_2) System copies the app frame buffer to the system frame buffer
  op_3) System sends the system frame buffer to the display hardware (using DMA).

  op_3 can happen in parallel with op_1, so the effective frame period is:
     frame_period = MAX(op_1_time + op_2_time, op2_time + op_3_time)

  This app measures op_1_time + op_2_time and does so by counting the number of times
  the app window's update callback got called within a set amount of time. The window update
  callback only does op1, but the app_render_handler() method in app.c insures that a window
  update is not called again until op_2 has completed for the previous update. This throttling
  if the app's window update also insures that:
      (op_1_time + op_2_time) is always >= (op_2_time + op_3_time)

  To measure op_1, we use a profiler timer node called "render". This timer
  measures the amount of time we spend in the window_render() method.

  To measure op_2, we use a profiler timer node called "framebuffer_prepare". This timer
  measures the amount of time we spend copying the app's frame buffer to the system framebuffer

  To measure op_3, we use a profiler timer node called "framebuffer_send". This profiler timer
  measures the amount of time we spend waiting for a display DMA to complete.

  op_1 can be computed from the app's update period - op_2_time
*/
static void prv_pop_all_windows_cb(void *cb_data) {
  AppData *data = (AppData *)cb_data;
  // Print profiler stats which include the time spent copying the app frame buffer to the
  // system frame buffer and the time spent sending the system frame buffer to the display.
  __profiler_stop();
  __profiler_print_stats();

  int64_t time_rendered = prv_time_64() - data->time_started;

  APP_LOG(APP_LOG_LEVEL_INFO, "## %d frames rendered", (int)data->rendered_frames);
  if (time_rendered) {
    int frame_period = time_rendered/(int64_t)data->rendered_frames;
    int fps = (int64_t)data->rendered_frames*1000/time_rendered;
    APP_LOG(APP_LOG_LEVEL_INFO, "## at %d FPS (%d ms/frame)", fps, frame_period);
  }

  window_stack_pop_all(false);
}

static const char *prv_row_texts[] = {
    "Row 1",
    "Row 2",
    "Row 3",
    "Row 4",
    "Row 5",
    "Row 6",
};

static uint16_t prv_get_num_rows(struct MenuLayer *menu_layer, uint16_t section_index,
    void *callback_context) {
  return ARRAY_LENGTH(prv_row_texts);
}

static void prv_draw_row(GContext* ctx, const Layer *cell_layer, char const *title,
    int16_t offset) {
  // mostly copied from menu_cell_basic_draw_with_value
  // (that unfortunately doesn't respect bounds.origin.x)
  const int16_t title_height = 24;
  GRect box = layer_get_bounds(cell_layer);
  box.origin.x += offset;
  box.origin.y = (box.size.h - title_height) / 2;
  box.size.w -= offset;
  box.size.h = title_height + 4;

  const GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  if (title) {
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, title, title_font, box,
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
}

void prv_draw_row_1(GContext* ctx, const Layer *cell_layer, MenuIndex *cell_index,
    void *callback_context) {
  const char *title = prv_row_texts[cell_index->row];
  GRect frame = layer_get_frame(cell_layer);
  prv_draw_row(ctx, cell_layer, title, -frame.origin.y/4);
}

static void prv_draw_row_2(GContext* ctx, const Layer *cell_layer, MenuIndex *cell_index,
    void *callback_context) {
  const char *title = prv_row_texts[cell_index->row];
  GRect frame = layer_get_frame(cell_layer);
  GRect bounds = layer_get_bounds(cell_layer);
  prv_draw_row(ctx, cell_layer, title, -frame.origin.y/4 + bounds.size.w);
}

static int16_t prv_get_separator_height(struct MenuLayer *menu_layer, MenuIndex *cell_index,
    void *callback_context) {
  return 0;
}

static void prv_window_update_proc(struct Layer *layer, GContext *ctx) {
  Window *window = layer_get_window(layer);
  AppData *data = window_get_user_data(window);
  if (data->rendered_frames == 0) {
    data->time_started = prv_time_64();
    __profiler_init();
    __profiler_start();
  }
  data->rendered_frames++;
}

void prv_syncing_selection_changed(MenuLayer *menu_layer, MenuIndex old_index,
    MenuIndex new_index, void *context) {
  ScrollLayer *scroll_layer = (ScrollLayer *)menu_layer;
  AppData *data = context;

  GPoint offset = scroll_layer_get_content_offset(scroll_layer);
  scroll_layer_set_content_offset((ScrollLayer *)data->action_list1, offset, false);
}

static void prv_window_load(Window *window) {
  // creates a structure as outlined at
  // https://pebbletechnology.atlassian.net/wiki/display/DEV/3.0+Notifications+UI+MVP

  // it's one full screen background image .background_layer,
  // one image at the top left .topleft_layer,
  // and two menu layers .action_list1 and .action_list2 that overlay each other

  // some hackery with the two menu layers goes on to keep their scroll offset in sync
  // and to have the inverter layer rendered only once

  const int16_t navbar_width = s_fps_topleft_bitmap.bounds.size.w;

  AppData *data = window_get_user_data(window);
  Layer *root_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root_layer);
  const GRect full_rect = bounds;

  data->background_layer = bitmap_layer_create(full_rect);
  bitmap_layer_set_background_color(data->background_layer, GColorBlack);
  bitmap_layer_set_bitmap(data->background_layer, &s_fps_background_bitmap);
  layer_add_child(root_layer, (Layer *)data->background_layer);

  data->topleft_layer = bitmap_layer_create(GRect(0, 0, navbar_width, navbar_width));
  bitmap_layer_set_background_color(data->topleft_layer, GColorWhite);
  bitmap_layer_set_bitmap(data->topleft_layer, &s_fps_topleft_bitmap);

  layer_add_child(root_layer, (Layer *)data->topleft_layer);

  const GRect menu_layer_rect =
      GRect(navbar_width, 0, full_rect.size.w - navbar_width, full_rect.size.h);
  data->action_list1 = menu_layer_create(menu_layer_rect);
  menu_layer_set_callbacks(data->action_list1, data, (MenuLayerCallbacks){
      .get_num_rows = prv_get_num_rows,
      .draw_row = prv_draw_row_1,
      .get_separator_height = prv_get_separator_height,
  });
  // FIXME layer_set_hidden(&data->action_list1.inverter.layer, true);

  scroll_layer_set_shadow_hidden((ScrollLayer *)data->action_list1, true);
  layer_add_child(root_layer, menu_layer_get_layer(data->action_list1));

  data->action_list2 = menu_layer_create(menu_layer_rect);
  menu_layer_set_callbacks(data->action_list2, data, (MenuLayerCallbacks){
      .get_num_rows = prv_get_num_rows,
      .draw_row = prv_draw_row_2,
      .get_separator_height = prv_get_separator_height,
      .selection_changed = prv_syncing_selection_changed,
  });
  scroll_layer_set_shadow_hidden((ScrollLayer *)data->action_list2, true);
  menu_layer_set_click_config_onto_window(data->action_list2, window);
  layer_add_child(root_layer, menu_layer_get_layer(data->action_list2));

  // start infinite update loop
  prv_redraw_timer_cb(data);
  // run application for a given time, than terminate
  app_timer_register(5000, prv_pop_all_windows_cb, data);
}

void prv_deinit(AppData *data) {
  menu_layer_destroy(data->action_list1);
  menu_layer_destroy(data->action_list2);
  bitmap_layer_destroy(data->background_layer);
  bitmap_layer_destroy(data->topleft_layer);
  window_destroy(data->window);
  free(data);
}

int main(void) {
  AppData *data = malloc(sizeof(AppData));
  memset(data, 0, sizeof(AppData));

  Window *window = window_create();
  data->window = window;
  window_set_user_data(window, data);
  window_set_fullscreen(window, true);
  Layer *root_layer = window_get_root_layer(window);
  layer_set_update_proc(root_layer, prv_window_update_proc);
  window_set_window_handlers(window, (WindowHandlers){
      .load = prv_window_load,
  });

  window_stack_push(window, true);

  __profiler_init();
  __profiler_start();
  app_event_loop();

  prv_deinit(data);
}
