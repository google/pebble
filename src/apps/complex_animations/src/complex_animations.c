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

#include "pebble.h"

#include <stdlib.h>

static Window *window;

static TextLayer *s_text_layer_a;
static TextLayer *s_text_layer_b;

static Animation *s_animation;
static int toggle;

#define DURATION 1000

static void animation_started(Animation *animation, void *data) {
  text_layer_set_text(s_text_layer_a, "Started.");
}

static void animation_stopped(Animation *animation, bool finished, void *data) {
  text_layer_set_text(s_text_layer_a, finished ? "Hi, I'm a TextLayer!" : "Just Stopped.");
}


// --------------------------------------------------------------------------------------
// setup handler
static void prv_setup_handler(Animation *animation) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Executing setup handler for %d", (int)animation);
}

// --------------------------------------------------------------------------------------
// teardown handler
static void prv_teardown_handler(Animation *animation) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Executing teardown handler for %d", (int)animation);
}

// --------------------------------------------------------------------------------------
// update handler
static void prv_update_handler(Animation *animation, const uint32_t distance) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Executing update handler for %d, distance: %d", (int)animation,
          (int)distance);
}

// --------------------------------------------------------------------------------------
static const AnimationImplementation s_custom_implementation = {
  .setup = prv_setup_handler,
  .update = prv_update_handler,
  .teardown = prv_teardown_handler
};

// --------------------------------------------------------------------------------------
static Animation *prv_create_custom_animation(void) {
  // Create a custom animation with a custom update procedure
  Animation *d = animation_create();
  animation_set_implementation(d, &s_custom_implementation);
  animation_set_duration(d, DURATION);
  return d;
}


static void click_handler(ClickRecognizerRef recognizer, Window *window) {
  // If the animation is still running, fast-forward to 300ms from the end
  if (animation_is_scheduled(s_animation)) {
    uint32_t duration = animation_get_duration(s_animation, true, true);
    animation_set_elapsed(s_animation, duration - 300);
    APP_LOG(APP_LOG_LEVEL_INFO, "Advancing to 300ms from the end of %d ms", (int)duration);
    return;
  }

  Layer *layer = text_layer_get_layer(s_text_layer_a);

  GRect from_rect_a = GRect(0, 0, 60, 60);
  GRect to_rect_a = GRect(84, 92, 60, 60);

  GRect from_rect_b = GRect(84, 0, 60, 60);
  GRect to_rect_b = GRect(0, 92, 60, 60);
  GRect tmp;
  if (toggle) {
    tmp = to_rect_b;
    to_rect_b = from_rect_b;
    from_rect_b = tmp;
  }
  toggle = !toggle;


  animation_destroy(s_animation);
  s_animation = NULL;

  PropertyAnimation *a = property_animation_create_layer_frame(layer, &from_rect_a, &to_rect_a);
  animation_set_duration((Animation*)a, DURATION);
  animation_set_handlers((Animation*) a, (AnimationHandlers) {
    .started = (AnimationStartedHandler) animation_started,
    .stopped = (AnimationStoppedHandler) animation_stopped,
  }, NULL /* callback data */);

  PropertyAnimation *a_rev = property_animation_clone(a);
  animation_set_handlers((Animation*) a_rev, (AnimationHandlers) {
    .started = (AnimationStartedHandler) animation_started,
    .stopped = (AnimationStoppedHandler) animation_stopped,
  }, NULL /* callback data */);
  animation_set_delay((Animation*)a_rev, 400);
  animation_set_duration((Animation*)a_rev, DURATION);
  animation_set_reverse((Animation *)a_rev, true);

  GRect test_rect;
  property_animation_get_to_grect(a, &test_rect);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "rect is %d, %d, %d, %d", test_rect.origin.x, test_rect.origin.y,
        test_rect.size.w, test_rect.size.h);


  switch (click_recognizer_get_button_id(recognizer)) {
    case BUTTON_ID_UP:
      animation_set_curve((Animation*) a, AnimationCurveEaseOut);
      animation_set_curve((Animation*) a_rev, AnimationCurveEaseOut);
      break;

    case BUTTON_ID_DOWN:
      animation_set_curve((Animation*) a, AnimationCurveEaseIn);
      animation_set_curve((Animation*) a_rev, AnimationCurveEaseIn);
      break;

    default:
    case BUTTON_ID_SELECT:
      animation_set_curve((Animation*) a, AnimationCurveEaseInOut);
      animation_set_curve((Animation*) a_rev, AnimationCurveEaseInOut);
      break;
  }

  /*
   // Example animation parameters:

   // Duration defaults to 250 ms
   animation_set_duration(&prop_animation->animation, 1000);

   // Curve defaults to ease-in-out
   animation_set_curve(&prop_animation->animation, AnimationCurveEaseOut);

   // Delay defaults to 0 ms
   animation_set_delay(&prop_animation->animation, 1000);
   */


  Animation *seq = animation_sequence_create((Animation *)a, (Animation *)a_rev, NULL);

  PropertyAnimation *c = property_animation_create_layer_frame(
                              text_layer_get_layer(s_text_layer_b), &from_rect_b, &to_rect_b);
  animation_set_duration((Animation*)c, DURATION);

  s_animation = animation_spawn_create(seq, (Animation *)c, prv_create_custom_animation());

  animation_schedule(s_animation);
}

static void config_provider(Window *window) {
  window_single_click_subscribe(BUTTON_ID_UP, (ClickHandler) click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, (ClickHandler) click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, (ClickHandler) click_handler);
}


static void init(void) {
  window = window_create();
  window_set_click_config_provider(window, (ClickConfigProvider) config_provider);
  window_stack_push(window, false);

  GRect from_rect_a = GRect(0, 0, 60, 60);
  GRect to_rect_a = GRect(84, 92, 60, 60);

  GRect from_rect_b = GRect(84, 0, 60, 60);
  GRect to_rect_b = GRect(0, 92, 60, 60);

  s_text_layer_a = text_layer_create(from_rect_a);
  text_layer_set_text(s_text_layer_a, "Started!");
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(s_text_layer_a));

  s_text_layer_b = text_layer_create(from_rect_b);
  text_layer_set_text(s_text_layer_b, "Spawned");
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(s_text_layer_b));


  // Animate text layer a from top-left to bottom right and back
  PropertyAnimation *a = property_animation_create_layer_frame(
                          text_layer_get_layer(s_text_layer_a), &from_rect_a, &to_rect_a);
  animation_set_duration((Animation*)a, DURATION);

  PropertyAnimation *a_rev = property_animation_clone(a);
  animation_set_delay((Animation*) a_rev, 400);
  animation_set_duration((Animation*)a_rev, DURATION);
  animation_set_reverse((Animation*) a_rev, true);
  Animation *seq = animation_sequence_create((Animation *)a, (Animation *)a_rev, NULL);


  // Animate text layer b from top-right to bottom-left
  PropertyAnimation *c = property_animation_create_layer_frame(
                            text_layer_get_layer(s_text_layer_b), &from_rect_b, &to_rect_b);
  animation_set_duration((Animation*)c, DURATION);
  toggle = !toggle;


  s_animation = animation_spawn_create(seq, (Animation *)c, prv_create_custom_animation(), NULL);
  animation_schedule(s_animation);
}

static void deinit(void) {
  animation_destroy(s_animation);

  window_stack_remove(window, false);
  window_destroy(window);
  text_layer_destroy(s_text_layer_a);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
