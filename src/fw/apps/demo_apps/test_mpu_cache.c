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

#include "test_mpu_cache.h"

#include "applib/app.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/ui.h"
#include "font_resource_keys.auto.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "process_management/process_manager.h"
#include "process_state/app_state/app_state.h"

// This demo app tests that MPU reconfiguration whilst context switching preserves the coherency of
// the data cache across between privileged and unprivileged tasks.
// See PBL-38343 for details

typedef struct {
  Window window;
  TextLayer text;
  ALIGN(32) uint32_t test;   // Align on 32-bit boundary (D-cache line on M7 is 32 bytes)
} AppData;

static void prv_window_load(Window *window) {
  AppData *app_data = window_get_user_data(window);

  GRect frame;
  layer_get_frame(window_get_root_layer(&app_data->window), &frame);
  GRect text_frame = {
    .size.h = 48,
    .size.w = 100,
  };
  grect_align(&text_frame, &frame, GAlignCenter, false);
  text_layer_init(&app_data->text, &text_frame);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28);
  text_layer_set_font(&app_data->text, font);
  text_layer_set_text_alignment(&app_data->text, GTextAlignmentCenter);
  layer_add_child(window_get_root_layer(&app_data->window), (Layer*)&app_data->text);
}

static void prv_verify_modify_on_app_task(void *data) {
  AppData *app_data = data;
  if (app_data->test != 0x3C3C3C3C) {
    text_layer_set_text(&app_data->text, "FAILED");
  } else {
    text_layer_set_text(&app_data->text, "PASSED");
  }
}

static void prv_verify_modify_on_kernel_task(void *data) {
  AppData *app_data = data;
  if (app_data->test != 0xA5A5A5A5) {
    text_layer_set_text(&app_data->text, "FAILED");
  } else {
    app_data->test = 0x3C3C3C3C;
    process_manager_send_callback_event_to_process(PebbleTask_App, prv_verify_modify_on_app_task,
                                                   app_data);
  }
}

static void prv_handle_init(void) {
  AppData *app_data = app_malloc_check(sizeof(AppData));
  app_state_set_user_data(app_data);
  window_init(&app_data->window, WINDOW_NAME("test_mpu"));
  window_set_user_data(&app_data->window, app_data);
  window_set_window_handlers(&app_data->window, &(WindowHandlers) {
      .load = prv_window_load });

  const bool animated = true;
  app_window_stack_push(&app_data->window, animated);

  app_data->test = 0xA5A5A5A5;
  launcher_task_add_callback(prv_verify_modify_on_kernel_task, app_data);
}

static void prv_main(void) {
  prv_handle_init();
  app_event_loop();
}

const PebbleProcessMd* test_mpu_cache_get_info() {
  static const PebbleProcessMdSystem s_test_mpu_info = {
    .common.main_func = prv_main,
    .name = "Test MPU cache"
  };
  return (const PebbleProcessMd*) &s_test_mpu_info;
}
