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

#include "pebble_colors.h"

#include "applib/app.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/text.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/text_layer.h"
#include "applib/ui/window.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "system/logging.h"

#include <stdio.h>

#define ALPHA_0   0x00
#define ALPHA_33  0x40
#define ALPHA_66  0x80
#define ALPHA_100 0xC0

static const int TARGET_FPS = 30;
static Window *s_window;
static Layer *s_canvas_layer;

static uint8_t *s_color_table = NULL;

// Sorted by Hue, Value, Saturation
static uint8_t color_table_hvs[] = {
  0x00,
  0x15,
  0x10,
  0x2a,
  0x25,
  0x20,
  0x3f,
  0x3a,
  0x35,
  0x30,
  0x34,
  0x24,
  0x39,
  0x38,
  0x14,
  0x29,
  0x28,
  0x3e,
  0x3d,
  0x3c,
  0x2c,
  0x18,
  0x2d,
  0x1c,
  0x04,
  0x19,
  0x08,
  0x2e,
  0x1d,
  0x0c,
  0x0d,
  0x09,
  0x1e,
  0x0e,
  0x05,
  0x1a,
  0x0a,
  0x2f,
  0x1f,
  0x0f,
  0x0b,
  0x06,
  0x1b,
  0x07,
  0x01,
  0x16,
  0x02,
  0x2b,
  0x17,
  0x03,
  0x13,
  0x12,
  0x27,
  0x23,
  0x11,
  0x26,
  0x22,
  0x3b,
  0x37,
  0x33,
  0x32,
  0x21,
  0x36,
  0x31,
};

// Sorted by Hue, Saturation, Value
static uint8_t color_table_hsv[] = {
  0x00,
  0x15,
  0x2a,
  0x3f,
  0x3a,
  0x25,
  0x35,
  0x10,
  0x20,
  0x30,
  0x34,
  0x39,
  0x24,
  0x38,
  0x3e,
  0x29,
  0x3d,
  0x14,
  0x28,
  0x3c,
  0x2c,
  0x2d,
  0x18,
  0x1c,
  0x2e,
  0x19,
  0x1d,
  0x04,
  0x08,
  0x0c,
  0x0d,
  0x1e,
  0x09,
  0x0e,
  0x2f,
  0x1a,
  0x1f,
  0x05,
  0x0a,
  0x0f,
  0x0b,
  0x1b,
  0x06,
  0x07,
  0x2b,
  0x16,
  0x17,
  0x01,
  0x02,
  0x03,
  0x13,
  0x27,
  0x12,
  0x23,
  0x3b,
  0x26,
  0x37,
  0x11,
  0x22,
  0x33,
  0x32,
  0x36,
  0x21,
  0x31,
};

typedef enum {
  PROPERTY_FG_COLOR,
  PROPERTY_BG_COLOR,
  PROPERTY_ALPHA,

  // Add more above here
  PROPERTY_MAX,
  PROPERTY_COLOR_TABLE, // Currently don't allow to switch the color table order
} ColorProperties;

typedef struct AppData {
  uint8_t property;
  GColor bg_color;
  GColor fg_color;
  uint8_t alpha;
  TextLayer *alpha_text;
  char alpha_text_buffer[30];

  TextLayer *fg_text;
  char fg_text_buffer[5];
  TextLayer *fg_color_text;
  char fg_color_text_buffer[20];
  uint8_t fg_color_index;

  TextLayer *bg_text;
  char bg_text_buffer[5];
  TextLayer *bg_color_text;
  char bg_color_text_buffer[20];
  uint8_t bg_color_index;
} AppData;

static void set_text_element(TextLayer *text_layer, char *text_buffer, bool highlight) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  text_layer_set_font(text_layer, font);
  if (highlight) {
    text_layer_set_background_color(text_layer, GColorWhite);
    text_layer_set_text_color(text_layer, GColorBlack);
  } else {
    text_layer_set_background_color(text_layer, GColorClear);
    text_layer_set_text_color(text_layer, GColorWhite);
  }
  text_layer_set_text_alignment(text_layer, GTextAlignmentLeft);
  text_layer_set_text(text_layer, text_buffer);
}

static void set_text_layers(AppData *data) {
  // Set Alpha Text
  uint8_t alpha_percent = 0;
  if (data->alpha == ALPHA_100) {
    alpha_percent = 100;
  } else if (data->alpha == ALPHA_66) {
    alpha_percent = 50; // FIXME: Currently don't support 66
  } else if (data->alpha == ALPHA_33) {
    alpha_percent = 25; // FIXME: Currently don't support 33
  } else if (data->alpha == ALPHA_0) {
    alpha_percent = 0;
  }
  snprintf(data->alpha_text_buffer, sizeof(data->alpha_text_buffer), " a = %d %%", alpha_percent);
  if (data->property == PROPERTY_ALPHA) {
    set_text_element(data->alpha_text, data->alpha_text_buffer, true);
  } else {
    set_text_element(data->alpha_text, data->alpha_text_buffer, false);
  }

  // Set FG Text
  strncpy(data->fg_text_buffer, "FG =", sizeof(data->fg_text_buffer));
  if (data->property == PROPERTY_FG_COLOR) {
    set_text_element(data->fg_text, data->fg_text_buffer, true);
  } else {
    set_text_element(data->fg_text, data->fg_text_buffer, false);
  }

  // Set BG Text
  strncpy(data->bg_text_buffer, "BG =", sizeof(data->bg_text_buffer));
  if (data->property == PROPERTY_BG_COLOR) {
    set_text_element(data->bg_text, data->bg_text_buffer, true);
  } else {
    set_text_element(data->bg_text, data->bg_text_buffer, false);
  }

  // Set FG Color Text
  uint8_t fg_color = data->fg_color.argb;
  snprintf(data->fg_color_text_buffer, sizeof(data->fg_color_text_buffer), "FG = 0x%02x", fg_color);
  if (data->property == PROPERTY_FG_COLOR) {
    set_text_element(data->fg_color_text, data->fg_color_text_buffer, true);
  } else {
    set_text_element(data->fg_color_text, data->fg_color_text_buffer, false);
  }

  // Set BG Color Text
  uint8_t bg_color = data->bg_color.argb;
  snprintf(data->bg_color_text_buffer, sizeof(data->bg_color_text_buffer), "BG = 0x%02x", bg_color);
  if (data->property == PROPERTY_BG_COLOR) {
    set_text_element(data->bg_color_text, data->bg_color_text_buffer, true);
  } else {
    set_text_element(data->bg_color_text, data->bg_color_text_buffer, false);
  }

  layer_mark_dirty(text_layer_get_layer(data->alpha_text));
  layer_mark_dirty(text_layer_get_layer(data->fg_text));
  layer_mark_dirty(text_layer_get_layer(data->bg_text));
}

static void draw_color_point(GContext* ctx, AppData *data, GPoint point) {
  GColor fg_color = (GColor){ .argb = (data->fg_color.argb | ALPHA_100) };
  GColor bg_color = (GColor){ .argb = (data->bg_color.argb | ALPHA_100) };
  uint8_t alpha = data->alpha;

  if (alpha == ALPHA_100) {
    graphics_context_set_stroke_color(ctx, fg_color);
    graphics_draw_pixel(ctx, point);
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y));
    graphics_draw_pixel(ctx, GPoint(point.x, point.y + 1));
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y + 1));
  } else if (alpha == ALPHA_66) {
    graphics_context_set_stroke_color(ctx, fg_color);
    graphics_draw_pixel(ctx, point);
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y + 1));
    graphics_context_set_stroke_color(ctx, bg_color);
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y));
    graphics_draw_pixel(ctx, GPoint(point.x, point.y + 1));
  } else if (alpha == ALPHA_33) {
    graphics_context_set_stroke_color(ctx, fg_color);
    graphics_draw_pixel(ctx, point);
    graphics_context_set_stroke_color(ctx, bg_color);
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y));
    graphics_draw_pixel(ctx, GPoint(point.x, point.y + 1));
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y + 1));
  } else if (alpha == ALPHA_0) {
    graphics_context_set_stroke_color(ctx, bg_color);
    graphics_draw_pixel(ctx, point);
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y));
    graphics_draw_pixel(ctx, GPoint(point.x, point.y + 1));
    graphics_draw_pixel(ctx, GPoint(point.x + 1, point.y + 1));
  }
}

static void draw_color_rect(GContext* ctx, AppData *data, GRect rect) {
  uint16_t width = rect.size.w;
  uint16_t height = rect.size.h;

  for (uint16_t row = 0; row < height; row += 2) {
    for (uint16_t col = 0; col < width; col += 2) {
      GPoint point = GPoint(rect.origin.x + col, rect.origin.y + row);
      draw_color_point(ctx, data, point);
    }
  }
}

static void draw_boxes(GContext* ctx, AppData *data) {
  // Draw border
  if (gcolor_equal(data->fg_color, GColorBlack)) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_round_rect(ctx, &GRect(35, 1, 32, 22), 4, GCornersAll);
  }
  // Draw foreground color
  GColor fg_color = (GColor){ .argb = (data->fg_color.argb | ALPHA_100) };
  graphics_context_set_fill_color(ctx, fg_color);
  graphics_fill_round_rect(ctx, &GRect(36, 2, 30, 20), 4, GCornersAll);

  // Draw border
  if (gcolor_equal(data->bg_color, GColorBlack)) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_round_rect(ctx, &GRect(35, 47, 32, 22), 4, GCornersAll);
  }
  // Draw background color
  GColor bg_color = (GColor){ .argb = (data->bg_color.argb | ALPHA_100) };
  graphics_context_set_fill_color(ctx, bg_color);
  graphics_fill_round_rect(ctx, &GRect(36, 48, 30, 20), 4, GCornersAll);
}

#define COLOR_BAR_WIDTH 4
#define COLOR_BAR_HEIGHT 24
#define ROW_LENGTH 32
static void draw_color_wheel_box(GContext* ctx, AppData *data) {
  GPoint origin = GPoint(8, 114);
  GColor compare_color = GColorClear;
  uint8_t color_index_match = 0;
  uint16_t height_offset = 0;
  bool compare = true;

  if (data->property == PROPERTY_FG_COLOR) {
    compare_color = data->fg_color;
  } else if (data->property == PROPERTY_BG_COLOR) {
    compare_color = data->bg_color;
  } else {
    compare = false;
  }

  for (uint8_t row = 0; row < 2; row++) {
    for (uint8_t color_index = row * ROW_LENGTH; color_index < (row + 1) * ROW_LENGTH;
          color_index++) {
      GColor color = (GColor){.argb = (s_color_table[color_index] | ALPHA_100)};

      if (compare && gcolor_equal(color, compare_color)) {
        color_index_match = color_index - (row * ROW_LENGTH);
        height_offset = row * (COLOR_BAR_HEIGHT + 4);
      }

      GRect box = GRect(origin.x + (4 * (color_index - (row * ROW_LENGTH))),
                        origin.y + (row * (COLOR_BAR_HEIGHT + 4)),
                        COLOR_BAR_WIDTH, COLOR_BAR_HEIGHT);
      graphics_context_set_fill_color(ctx, color);
      graphics_fill_rect(ctx, &box);
    }
  }

  // Draw border
  if (compare) {
    const GRect box = GRect(origin.x + 4*color_index_match - 1, origin.y - 1 + height_offset,
                            COLOR_BAR_WIDTH + 2, COLOR_BAR_HEIGHT + 2);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_rect(ctx, &box);
  }
}

static void up_handler(ClickRecognizerRef recognizer, void *context) {
  AppData *data = window_get_user_data(s_window);
  if (data->property == PROPERTY_FG_COLOR) {
    data->fg_color_index = (data->fg_color_index + 1) & 0x3F;
    data->fg_color.argb = (s_color_table[data->fg_color_index] | ALPHA_100);
  } else if (data->property == PROPERTY_BG_COLOR) {
    data->bg_color_index = (data->bg_color_index + 1) & 0x3F;
    data->bg_color.argb = (s_color_table[data->bg_color_index] | ALPHA_100);
  } else if (data->property == PROPERTY_ALPHA) {
    data->alpha = (data->alpha + 0x40) & ALPHA_100;
    set_text_layers(data);
  } else if (data->property == PROPERTY_COLOR_TABLE) {
    if (s_color_table == color_table_hsv) {
      s_color_table = color_table_hvs;
    } else if (s_color_table == color_table_hvs) {
      s_color_table = color_table_hsv;
    }
  }
  layer_mark_dirty(s_canvas_layer);
}

static void select_handler(ClickRecognizerRef recognizer, void *context) {
  AppData *data = window_get_user_data(s_window);
  data->property = (data->property + 1) % PROPERTY_MAX;
  layer_mark_dirty(s_canvas_layer);
}

static void down_handler(ClickRecognizerRef recognizer, void *context) {
  AppData *data = window_get_user_data(s_window);
  if (data->property == PROPERTY_FG_COLOR) {
    data->fg_color_index = (data->fg_color_index - 1) & 0x3F;
    data->fg_color.argb = (s_color_table[data->fg_color_index] | ALPHA_100);
  } else if (data->property == PROPERTY_BG_COLOR) {
    data->bg_color_index = (data->bg_color_index - 1) & 0x3F;
    data->bg_color.argb = (s_color_table[data->bg_color_index] | ALPHA_100);
  } else if (data->property == PROPERTY_ALPHA) {
    data->alpha = (data->alpha - 0x40) & ALPHA_100;
    set_text_layers(data);
  } else if (data->property == PROPERTY_COLOR_TABLE) {
    if (s_color_table == color_table_hsv) {
      s_color_table = color_table_hvs;
    } else if (s_color_table == color_table_hvs) {
      s_color_table = color_table_hsv;
    }
  }
  layer_mark_dirty(s_canvas_layer);
}

static void click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_SELECT, 100, select_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_handler);
}

static void layer_update_proc(Layer *layer, GContext* ctx) {
  AppData *data = window_get_user_data(s_window);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, &layer->bounds);
  draw_boxes(ctx, data);
  set_text_layers(data);

  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_rect(ctx, &GRect(71, 0, 73, 111));      // Border around BG
  graphics_context_set_fill_color(ctx, data->bg_color);
  graphics_fill_rect(ctx, &GRect(72, 0, 72, 110));
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "BG", font, GRect(72, 110 - 16, 20, 16),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  draw_color_rect(ctx, data, GRect(92, 0, 62, 90));
  graphics_context_set_text_color(ctx, GColorWhite);
  if (data->alpha < ALPHA_100) {
    graphics_draw_text(ctx, "FG+BG", font, GRect(92, 90 - 16, 62, 16),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
  else {
    graphics_draw_text(ctx, "FG", font, GRect(92, 90 - 16, 62, 16),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }

  if (data->alpha < ALPHA_100) {
    graphics_context_set_fill_color(ctx, data->fg_color);
    graphics_fill_rect(ctx, &GRect(124, 0, 20, 40));
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "FG", font, GRect(124, 40 - 16, 20, 16),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }

  draw_color_wheel_box(ctx, data);
}

static void main_window_load(Window *window) {
  AppData *data = window_get_user_data(s_window);
  Layer *window_layer = window_get_root_layer(window);
  GRect window_bounds = window_layer->bounds;

  // Create Layer
  s_canvas_layer = layer_create(GRect(0, 0, window_bounds.size.w, window_bounds.size.h));
  layer_add_child(window_layer, s_canvas_layer);

  // Set the update_proc
  layer_set_update_proc(s_canvas_layer, layer_update_proc);


  data->fg_text = text_layer_create(GRect(2, 2, 28, 20));
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(data->fg_text));

  data->fg_color_text = text_layer_create(GRect(2, 24, 64, 20));
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(data->fg_color_text));

  data->fg_color_index = 8;

  data->bg_text = text_layer_create(GRect(2, 48, 28, 20));
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(data->bg_text));

  data->bg_color_text = text_layer_create(GRect(2, 70, 64, 20));
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(data->bg_color_text));

  data->bg_color_index = 0;

  data->alpha_text = text_layer_create(GRect(2, 92, 64, 20));
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(data->alpha_text));

  // Other properties
  s_color_table = color_table_hsv;
  data->alpha = ALPHA_100;
  data->fg_color.argb = s_color_table[data->fg_color_index] | ALPHA_100;
  data->bg_color = GColorBlack;
  set_text_layers(data);
}

static void main_window_unload(Window *window) {
  // Destroy Layer
  layer_destroy(s_canvas_layer);
}

static void init(void) {
  AppData *data = task_malloc(sizeof(AppData));
  if (!data) {
    return;
  }
  memset(data, 0x00, sizeof(AppData));

  s_window = window_create();
  window_set_user_data(s_window, data);
  window_set_fullscreen(s_window, true);
  window_set_window_handlers(s_window, &(WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });

  window_set_click_config_provider(s_window, click_config_provider);

  const bool animated = true;
  app_window_stack_push(s_window, animated);
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

const PebbleProcessMd* pebble_colors_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common.main_func = s_main,
    .name = "Pebble Colors"
  };
  return (const PebbleProcessMd*) &s_app_info;
}
