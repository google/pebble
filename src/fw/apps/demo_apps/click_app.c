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

#include "click_app.h"

#include "applib/app.h"
#include "process_state/app_state/app_state.h"
#include "applib/ui/ui.h"
#include "applib/ui/window.h"
#include "kernel/pbl_malloc.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"

#include <stdio.h>

#define TEXT_BUFFER_SIZE 64

typedef struct {
  Window window;
  TextLayer text;
  char text_buffer[TEXT_BUFFER_SIZE];
} ClickAppData;

////////////////////////////
// Click app's main window

//! Toggle the colors of the label, so we can see change even if the text stayed the same:
static void toggle_color(Window *window) {
  ClickAppData *data = window_get_user_data(window);
  TextLayer *text = &data->text;
  const GColor bg_color = text->background_color;
  if (gcolor_equal(bg_color, GColorBlack)) {
    text_layer_set_background_color(text, GColorWhite);
    text_layer_set_text_color(text, GColorBlack);
  } else {
    text_layer_set_background_color(text, GColorBlack);
    text_layer_set_text_color(text, GColorWhite);
  }
}

static void raw_click_handler(ClickRecognizerRef recognizer, Window *window, const bool up) {
  ClickAppData *data = window_get_user_data(window);
  sniprintf(data->text_buffer, TEXT_BUFFER_SIZE, up ? "Raw UP" : "Raw DOWN");
  if (up) {  // PBL_LOG requires a fixed const string, so can't use ternary
    PBL_LOG(LOG_LEVEL_DEBUG, "Raw UP");
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Raw DOWN");
  }
  text_layer_set_text(&data->text, data->text_buffer);
  toggle_color(window);
  (void)recognizer;
}

static void raw_up_click_handler(ClickRecognizerRef recognizer, Window *window) {
  raw_click_handler(recognizer, window, true);
}

static void raw_down_click_handler(ClickRecognizerRef recognizer, Window *window) {
  raw_click_handler(recognizer, window, false);
}

static void select_multi_click_handler(ClickRecognizerRef recognizer, Window *window) {
  ClickAppData *data = window_get_user_data(window);
  const uint16_t count = click_number_of_clicks_counted(recognizer);
  sniprintf(data->text_buffer, TEXT_BUFFER_SIZE, "Multi Click! (%u)\nMin: 2, Max: 10", count);
  PBL_LOG(LOG_LEVEL_DEBUG, "Multi Click! (%u)", click_number_of_clicks_counted(recognizer));
  text_layer_set_text(&data->text, data->text_buffer);
  toggle_color(window);
}

static void select_single_click_handler(ClickRecognizerRef recognizer, Window *window) {
  ClickAppData *data = window_get_user_data(window);
  const uint16_t count = click_number_of_clicks_counted(recognizer);
  sniprintf(data->text_buffer, TEXT_BUFFER_SIZE, "Single Click! (%u)", count);
  PBL_LOG(LOG_LEVEL_DEBUG, "Single Click! (%u)", click_number_of_clicks_counted(recognizer));
  text_layer_set_text(&data->text, data->text_buffer);
  toggle_color(window);

  // Let's try shortening the repeat interval as we go:
  ClickConfig *config = click_recognizer_get_config(recognizer);
  config->click.repeat_interval_ms = MAX((config->click.repeat_interval_ms / 2), 100);
}

static void select_long_click_handler(ClickRecognizerRef recognizer, Window *window) {
  ClickAppData *data = window_get_user_data(window);
  sniprintf(data->text_buffer, TEXT_BUFFER_SIZE, "Long Click!");
  PBL_LOG(LOG_LEVEL_DEBUG, "Long Click!");
  text_layer_set_text(&data->text, data->text_buffer);
  toggle_color(window);
  (void)recognizer;
}

static void select_long_click_release_handler(ClickRecognizerRef recognizer, Window *window) {
  ClickAppData *data = window_get_user_data(window);
  sniprintf(data->text_buffer, TEXT_BUFFER_SIZE, "Long Click Released!");
  PBL_LOG(LOG_LEVEL_DEBUG, "Long Click Released!");
  text_layer_set_text(&data->text, data->text_buffer);
  toggle_color(window);
  (void)recognizer;
}

static void config_provider(Window *window) {
  // See ui/click.h for more information and default values.

  // single click / repeat-on-hold config:
  window_single_repeating_click_subscribe(BUTTON_ID_SELECT, 1000, (ClickHandler)select_single_click_handler); // "hold-to-repeat" gets overridden if there's a long click handler configured!

  // multi click config:
  window_multi_click_subscribe(BUTTON_ID_SELECT, 2, 10, 0, false, (ClickHandler) select_multi_click_handler);

  // long click config:
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, (ClickHandler) select_long_click_handler, (ClickHandler) select_long_click_release_handler);

  // single click / repeat-on-hold config:
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 1000, (ClickHandler) select_single_click_handler); // "hold-to-repeat" gets overridden if there's a long click handler configured!

  // multi click config:
  window_multi_click_subscribe(BUTTON_ID_UP, 2, 10, 0, true, (ClickHandler) select_multi_click_handler);

  // raw:
  window_raw_click_subscribe(BUTTON_ID_DOWN, (ClickHandler) raw_down_click_handler, (ClickHandler) raw_up_click_handler, NULL);
}

static void prv_window_load(Window *window) {
  ClickAppData *data = window_get_user_data(window);
  TextLayer *text = &data->text;
  text_layer_init(text, &window->layer.bounds);
  text_layer_set_text(text, "Use select button and try different clicks: single, hold-to-repeat, multiple, long press, etc.\n\nNOTE: a long click config will override hold-to-repeat config. Comment out the long_click section of the config to enable hold-to-repeat.");
  layer_add_child(&window->layer, &text->layer);
}

static void push_window(ClickAppData *data) {
  Window *window = &data->window;
  window_init(window, WINDOW_NAME("Click Demo"));
  window_set_user_data(window, data);
  window_set_window_handlers(window, &(WindowHandlers) {
    .load = prv_window_load,
  });
  window_set_click_config_provider(window, (ClickConfigProvider) config_provider);
  const bool animated = true;
  app_window_stack_push(window, animated);
}

////////////////////
// App boilerplate

static void handle_init(void) {
  ClickAppData *data = (ClickAppData*) app_malloc_check(sizeof(ClickAppData));
  if (data == NULL) {
    PBL_CROAK("Out of memory");
  }
  app_state_set_user_data(data);
  push_window(data);
}

static void handle_deinit(void) {
  ClickAppData *data = app_state_get_user_data();
  app_free(data);
}

static void s_main(void) {
  handle_init();

  app_event_loop();

  handle_deinit();
}

const PebbleProcessMd* click_app_get_info() {
  static const PebbleProcessMdSystem s_click_app_info = {
    .common.main_func = s_main,
    .name = "Clicks"
  };
  return (const PebbleProcessMd*) &s_click_app_info;
}

#undef TEXT_BUFFER_SIZE
