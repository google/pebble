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

#include "animated_demo.h"

#include "applib/app.h"
#include "process_state/app_state/app_state.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "system/passert.h"

#include <string.h>
#include <stdio.h>

typedef struct AnimatedDemoData {
  Window window;
  TextLayer text_layer;
  PropertyAnimation *prop_animation;
  bool toggle;
} AnimatedDemoData;

static void animation_started(Animation *animation, AnimatedDemoData *data) {
  text_layer_set_text(&data->text_layer, "Started.");
  (void)animation;
}

static void animation_stopped(Animation *animation, bool finished, AnimatedDemoData *data) {
  text_layer_set_text(&data->text_layer, finished ? "Hi, I'm a TextLayer!" : "Just Stopped.");
  (void)animation;
}

AnimationProgress animation_bounce(AnimationProgress linear_distance) {
  // An awful linear "bounce-like" animation
  if (linear_distance < ANIMATION_NORMALIZED_MAX/2) {
    return linear_distance * 2;
  } else if (linear_distance < ANIMATION_NORMALIZED_MAX * 3/4) {
    return ANIMATION_NORMALIZED_MAX * 3/2 - linear_distance;
  } else {
    return linear_distance;
  }
}

static void click_handler(ClickRecognizerRef recognizer, Window *window) {
  AnimatedDemoData *data = window_get_user_data(window);
  Layer *layer = &data->text_layer.layer;

  GRect to_rect;
  if (data->toggle) {
    to_rect = GRect(4, 4, 120, 60);
  } else {
    to_rect = GRect(84, 92, 60, 60);
  }
  data->toggle = !data->toggle;

  // Does nothing if prop_animation is NULL
  if (data->prop_animation) {
    animation_unschedule(property_animation_get_animation(data->prop_animation));
    property_animation_init_layer_frame(data->prop_animation, layer, NULL, &to_rect);
  } else {
    data->prop_animation = property_animation_create_layer_frame(layer, NULL, &to_rect);
  }
  Animation *animation = property_animation_get_animation(data->prop_animation);
  PBL_ASSERTN(animation);
  animation_set_auto_destroy(animation, true);
  animation_set_duration(animation, 400);
  switch (click_recognizer_get_button_id(recognizer)) {
    case BUTTON_ID_UP:
      animation_set_curve(animation, AnimationCurveEaseOut);
      break;

    case BUTTON_ID_DOWN:
      // animation_set_curve(animation, AnimationCurveEaseIn);
      // animation_set_duration(animation, 2000);
      animation_set_custom_curve(animation, animation_bounce);
      break;

    default:
    case BUTTON_ID_SELECT:
      animation_set_curve(animation, AnimationCurveEaseInOut);
      break;
  }

  /*
  // Example animation parameters:

  // Duration defaults to 250 ms
  animation_set_duration(animation, 1000);

  // Curve defaults to ease-in-out
  animation_set_curve(animation, AnimationCurveEaseOut);

  // Delay defaults to 0 ms
  animation_set_delay(animation, 1000);
  */

  animation_set_handlers(animation, (AnimationHandlers) {
    .started = (AnimationStartedHandler) animation_started,
    .stopped = (AnimationStoppedHandler) animation_stopped,
  }, data);
  animation_schedule(animation);
}

void animated_demo_window_load(Window *window) {
  AnimatedDemoData *data = window_get_user_data(window);
  text_layer_init(&data->text_layer, &GRect(0, 0, 60, 60));
  text_layer_set_background_color(&data->text_layer, GColorBlack);
  text_layer_set_text_color(&data->text_layer, GColorWhite);
  text_layer_set_text(&data->text_layer, "Press Buttons");
  GFont gothic_24_norm = fonts_get_system_font(FONT_KEY_GOTHIC_24);
  text_layer_set_font(&data->text_layer, gothic_24_norm);
  text_layer_set_text_alignment(&data->text_layer, GTextAlignmentCenter);
  layer_add_child(&window->layer, &data->text_layer.layer);
}

static void config_provider(Window *window) {
  window_single_click_subscribe(BUTTON_ID_UP, (ClickHandler)click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, (ClickHandler)click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, (ClickHandler)click_handler);
  (void)window;
}

static void handle_init(void) {
  AnimatedDemoData *data = (AnimatedDemoData*) app_malloc_check(sizeof(AnimatedDemoData));
  data->toggle = false;
  app_state_set_user_data(data);

  Window *window = &data->window;
  window_init(window, WINDOW_NAME("Animated Demo"));
  window_set_user_data(window, data);
  window_set_click_config_provider(window, (ClickConfigProvider) config_provider);
  window_set_window_handlers(window, &(WindowHandlers){
    .load = animated_demo_window_load,
  });
  const bool animated = true;
  app_window_stack_push(window, animated);
}

static void handle_deinit(void) {
  AnimatedDemoData *data = app_state_get_user_data();
  app_free(data);
}

static void s_main(void) {
  handle_init();

  app_event_loop();

  handle_deinit();
}

const PebbleProcessMd* animated_demo_get_app_info() {
  static const PebbleProcessMdSystem animated_demo_app_info = {
    .common.main_func = s_main,
    .name = "Animation Demo"
  };
  return (const PebbleProcessMd*) &animated_demo_app_info;
}
