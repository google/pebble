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

#include "statusbar_demo.h"
#include "kernel/pbl_malloc.h"
#include "services/common/i18n/i18n.h"
#include "applib/app.h"
#include "applib/ui/ui.h"
#include "util/size.h"

typedef struct StatusBarDemoWindow {
  Window window;
  TextLayer text;
  StatusBarLayer status_bar;
} StatusBarDemoWindow;

static Window * prv_window_create(void);

static void prv_handle_click(ClickRecognizerRef ref, void *context) {
  Window *window = prv_window_create();
  app_window_stack_push(window, true);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_handle_click);
}

static void prv_window_unload(Window *window) {
  StatusBarDemoWindow *demo_window = (StatusBarDemoWindow *)window;

  status_bar_layer_deinit(&demo_window->status_bar);
  text_layer_deinit(&demo_window->text);
  window_deinit(window);
}

static Window *prv_window_create(void) {
  typedef struct Description {
    char *debug_name;
    bool full_screen;
    uint8_t window_color;
    bool status_bar;
    uint8_t status_bar_color;
  } Description;

  static const Description descriptions[] = {
    {
        .debug_name = "non-full-screen (legacy status bar)",
        .window_color = GColorRedARGB8,
    },
    {
        .debug_name = "non-full-screen (legacy status bar)",
        .window_color = GColorBlueARGB8,
    },
    {
        .debug_name = "full-screen (transparent status bar)",
        .full_screen = true,
        .window_color = GColorRedARGB8,
        .status_bar = true,
    },
    {
        .debug_name = "full-screen (opaque status bar)",
        .full_screen = true,
        .window_color = GColorBlueARGB8,
        .status_bar = true,
        .status_bar_color = GColorOrangeARGB8,
    },
    {
        .debug_name = "full-screen (no status bar)",
        .full_screen = true,
        .window_color = GColorGreenARGB8,
    },
  };

  Description description = descriptions[app_window_stack_count() % ARRAY_LENGTH(descriptions)];
  const GColor8 window_color = (GColor8){.argb = description.window_color};
  const GColor8 status_bar_color = (GColor8){.argb = description.status_bar_color};

  StatusBarDemoWindow *window = task_zalloc_check(sizeof(*window));
  Window *result = &window->window;
  window_init(result, description.debug_name);
  window_set_fullscreen(result, description.full_screen);
  window_set_background_color(result, window_color);

  text_layer_init(&window->text, &GRect(0, 40, 144, 40));
  text_layer_set_text(&window->text, description.debug_name);
  layer_add_child(&window->window.layer, &window->text.layer);

  StatusBarLayer *status_bar = &window->status_bar;
  status_bar_layer_init(status_bar);
  if (description.status_bar) {
    status_bar_layer_set_colors(status_bar, status_bar_color,
                                gcolor_legible_over(status_bar_color));
    layer_add_child(&window->window.layer, &status_bar->layer);
  }

  window_set_click_config_provider(result, prv_click_config_provider);

  window_set_window_handlers(&window->window, &(WindowHandlers){
     .unload = prv_window_unload,
  });

  return result;
}

static void prv_handle_init(void) {
  Window *window = prv_window_create();
  app_window_stack_push(window, true);
}

static void prv_main(void) {
  prv_handle_init();
  app_event_loop();
}

const PebbleProcessMd* statusbar_demo_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
      .common = {
          .main_func = prv_main,
          // UUID: dfcafc64-0af1-4e4a-8e03-1901b54335c5
          .uuid = {0xdf, 0xca, 0xfc, 0x64, 0xa, 0xf1, 0x4e, 0x4a,
                   0x8e, 0x3, 0x19, 0x1, 0xb5, 0x43, 0x35, 0xc5},
      },
      .name = "StatusBar Demo",
  };
  return (const PebbleProcessMd*) &s_app_info;
}
