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

#include "health_card_view.h"

#include "health.h"
#include "health_activity_summary_card.h"
#include "health_sleep_summary_card.h"
#include "health_hr_summary_card.h"

#include "applib/app_launch_reason.h"
#include "applib/ui/action_button.h"
#include "applib/ui/content_indicator.h"
#include "applib/ui/content_indicator_private.h"
#include "kernel/pbl_malloc.h"
#include "services/normal/activity/activity_private.h"
#include "services/normal/timeline/health_layout.h"
#include "system/logging.h"
#include "util/time/time.h"

#define SELECT_INDICATOR_COLOR (PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack))
#define BACK_TO_WATCHFACE (-1)

// Enum for different card types
typedef enum {
  Card_ActivitySummary,
#if CAPABILITY_HAS_BUILTIN_HRM
  Card_HrSummary,
#endif
  Card_SleepSummary,
  CardCount
} Card;

// Main structure for card view
typedef struct HealthCardView {
  Window window;
  HealthData *health_data;
  Card current_card_index;
  Layer *card_layers[CardCount];
  Animation *slide_animation;
  Layer select_indicator_layer;
  Layer down_arrow_layer;
  Layer up_arrow_layer;
  ContentIndicator down_indicator;
  ContentIndicator up_indicator;
} HealthCardView;

static Layer* (*s_card_view_create[CardCount])(HealthData *health_data) = {
  [Card_ActivitySummary] = health_activity_summary_card_create,
#if CAPABILITY_HAS_BUILTIN_HRM
  [Card_HrSummary] = health_hr_summary_card_create,
#endif
  [Card_SleepSummary] = health_sleep_summary_card_create,
};

static void (*s_card_view_select_click_handler[CardCount])(Layer *layer) = {
  [Card_ActivitySummary] = health_activity_summary_card_select_click_handler,
#if CAPABILITY_HAS_BUILTIN_HRM
  [Card_HrSummary] = health_hr_summary_card_select_click_handler,
#endif
  [Card_SleepSummary] = health_sleep_summary_card_select_click_handler,
};

static GColor (*s_card_view_get_bg_color[CardCount])(Layer *layer) = {
  [Card_ActivitySummary] = health_activity_summary_card_get_bg_color,
#if CAPABILITY_HAS_BUILTIN_HRM
  [Card_HrSummary] = health_hr_summary_card_get_bg_color,
#endif
  [Card_SleepSummary] = health_sleep_summary_card_get_bg_color,
};

static bool (*s_card_view_show_select_indicator[CardCount])(Layer *layer) = {
  [Card_ActivitySummary] = health_activity_summary_show_select_indicator,
#if CAPABILITY_HAS_BUILTIN_HRM
  [Card_HrSummary] = health_hr_summary_show_select_indicator,
#endif
  [Card_SleepSummary] = health_sleep_summary_show_select_indicator,
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Private Functions
//

static int prv_get_next_card_idx(Card current, bool up) {
  const int direction = up ? 1 : -1;
  int next = current + direction;

// Skip over the HR card if we don't support it
#if CAPABILITY_HAS_BUILTIN_HRM
  if (next == Card_HrSummary && !activity_is_hrm_present()) {
    next = next + direction;
  }
  // if heart rate is disabled, change the order of cards to Activity <-> Sleep <-> HR
  else if (activity_is_hrm_present() && !activity_prefs_heart_rate_is_enabled()) {
    if (current == Card_ActivitySummary) {
      next = up ? Card_SleepSummary : BACK_TO_WATCHFACE;
    } else if (current == Card_SleepSummary) {
      next = up ? Card_HrSummary : Card_ActivitySummary;
    } else if (current == Card_HrSummary) {
      next = up ? CardCount : Card_SleepSummary;
    }
  }
#endif

  return next;
}

static void prv_select_indicator_layer_update_proc(Layer *layer, GContext *ctx) {
  action_button_draw(ctx, layer, SELECT_INDICATOR_COLOR);
}

static void prv_refresh_select_indicator(HealthCardView *health_card_view) {
  Layer *card_layer = health_card_view->card_layers[health_card_view->current_card_index];

  const bool is_hidden = !s_card_view_show_select_indicator[health_card_view->current_card_index](
      card_layer);

  layer_set_hidden(&health_card_view->select_indicator_layer, is_hidden);
}

static void prv_content_indicator_setup_direction(HealthCardView *health_card_view,
                                                  ContentIndicator *content_indicator,
                                                  Layer *indicator_layer,
                                                  ContentIndicatorDirection direction) {
  Layer *card_layer = health_card_view->card_layers[health_card_view->current_card_index];

  GColor card_bg_color = s_card_view_get_bg_color[health_card_view->current_card_index](card_layer);

  content_indicator_configure_direction(content_indicator, direction, &(ContentIndicatorConfig) {
    .layer = indicator_layer,
    .colors.foreground = gcolor_legible_over(card_bg_color),
    .colors.background = card_bg_color,
  });
}

static void prv_refresh_content_indicators(HealthCardView *health_card_view) {
  prv_content_indicator_setup_direction(health_card_view,
                                        &health_card_view->up_indicator,
                                        &health_card_view->up_arrow_layer,
                                        ContentIndicatorDirectionUp);
  prv_content_indicator_setup_direction(health_card_view,
                                        &health_card_view->down_indicator,
                                        &health_card_view->down_arrow_layer,
                                        ContentIndicatorDirectionDown);

  bool is_up_visible = true;
  if (prv_get_next_card_idx(health_card_view->current_card_index, true) >= CardCount) {
    is_up_visible = false;
  }

  content_indicator_set_content_available(&health_card_view->up_indicator,
                                          ContentIndicatorDirectionUp,
                                          is_up_visible);

  // Down is always visible (the watchface is always an option)
  content_indicator_set_content_available(&health_card_view->down_indicator,
                                          ContentIndicatorDirectionDown,
                                          true);
}

static void prv_hide_content_indicators(HealthCardView *health_card_view) {
  content_indicator_set_content_available(&health_card_view->up_indicator,
                                          ContentIndicatorDirectionUp,
                                          false);
  content_indicator_set_content_available(&health_card_view->down_indicator,
                                          ContentIndicatorDirectionDown,
                                          false);
}

static void prv_set_window_background_color(HealthCardView *health_card_view) {
  Layer *card_layer = health_card_view->card_layers[health_card_view->current_card_index];
  window_set_background_color(&health_card_view->window,
      s_card_view_get_bg_color[health_card_view->current_card_index](card_layer));
}

#define NUM_MID_FRAMES 1

static void prv_bg_animation_update(Animation *animation, AnimationProgress normalized) {
  HealthCardView *health_card_view = animation_get_context(animation);
  const AnimationProgress bounce_back_length =
      (interpolate_moook_out_duration() * ANIMATION_NORMALIZED_MAX) /
       interpolate_moook_soft_duration(NUM_MID_FRAMES);
  if (normalized >= ANIMATION_NORMALIZED_MAX - bounce_back_length) {
    prv_set_window_background_color(health_card_view);
  }
}

static void prv_bg_animation_started_handler(Animation *animation, void *context) {
  HealthCardView *health_card_view = context;

  layer_set_hidden(health_card_view->card_layers[health_card_view->current_card_index], false);

  layer_set_hidden(&health_card_view->select_indicator_layer, true);

  prv_hide_content_indicators(health_card_view);
}

static void prv_bg_animation_stopped_handler(Animation *animation, bool finished, void *context) {
  HealthCardView *health_card_view = context;

  for (int i = 0; i < CardCount; i++) {
    if (i != health_card_view->current_card_index) {
      layer_set_hidden(health_card_view->card_layers[i], true);
    }
  }

  if (!finished) {
    prv_set_window_background_color(health_card_view);
  } else {
    prv_refresh_select_indicator(health_card_view);
    prv_refresh_content_indicators(health_card_view);
  }
}

static const AnimationImplementation prv_bg_animation_implementation = {
    .update = &prv_bg_animation_update
};

static int64_t prv_interpolate_moook_soft(int32_t normalized, int64_t from, int64_t to) {
  return interpolate_moook_soft(normalized, from, to, NUM_MID_FRAMES);
}

static Animation* prv_create_slide_animation(Layer *layer, GRect *from_frame, GRect *to_frame) {
  Animation *anim = (Animation *)property_animation_create_layer_frame(layer, from_frame, to_frame);
  animation_set_duration(anim, interpolate_moook_soft_duration(NUM_MID_FRAMES));
  animation_set_custom_interpolation(anim, prv_interpolate_moook_soft);
  return anim;
}

// Create animation
static void prv_schedule_slide_animation(HealthCardView *health_card_view,
                                         Card next_card_index, bool slide_up) {
  animation_unschedule(health_card_view->slide_animation);
  health_card_view->slide_animation = NULL;

  GRect window_bounds = window_get_root_layer(&health_card_view->window)->bounds;

  Layer *current_card_layer = health_card_view->card_layers[health_card_view->current_card_index];
  Layer *next_card_layer = health_card_view->card_layers[next_card_index];

  GRect curr_stop = window_bounds;
  curr_stop.origin.y = slide_up ? window_bounds.size.h : -window_bounds.size.h;
  GRect next_start = window_bounds;
  next_start.origin.y = slide_up ? -window_bounds.size.h : window_bounds.size.h;

  Animation *curr_out = prv_create_slide_animation(current_card_layer, &window_bounds, &curr_stop);
  Animation *next_in = prv_create_slide_animation(next_card_layer, &next_start, &window_bounds);

  Animation *bg_anim = animation_create();
  animation_set_duration(bg_anim, interpolate_moook_soft_duration(NUM_MID_FRAMES));
  animation_set_handlers(bg_anim, (AnimationHandlers){
      .started = prv_bg_animation_started_handler,
      .stopped = prv_bg_animation_stopped_handler,
  }, health_card_view);
  animation_set_implementation(bg_anim, &prv_bg_animation_implementation);

  health_card_view->slide_animation = animation_spawn_create(curr_out, next_in, bg_anim, NULL);
  animation_schedule(health_card_view->slide_animation);

  health_card_view->current_card_index = next_card_index;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Callback Functions
//

// Up/Down click handler
static void prv_up_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthCardView *health_card_view = context;
  const bool slide_up = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
  const int next_card_index = prv_get_next_card_idx(health_card_view->current_card_index, slide_up);

  if (next_card_index < 0) {
    // Exit
    app_window_stack_pop_all(true);
  } else if (next_card_index >= CardCount) {
    // Top of the list (no-op)
    // TODO: maybe we want an animation?
    return;
  } else {
    // animate the cards' positions
    prv_schedule_slide_animation(health_card_view, next_card_index, slide_up);
  }
}

// Select click handler
static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthCardView *health_card_view = context;
  Layer *layer = health_card_view->card_layers[health_card_view->current_card_index];
  s_card_view_select_click_handler[health_card_view->current_card_index](layer);
  health_card_view_mark_dirty(health_card_view);
}

// Click config provider
static void prv_click_config_provider(void *context) {
  window_set_click_context(BUTTON_ID_UP, context);
  window_set_click_context(BUTTON_ID_SELECT, context);
  window_set_click_context(BUTTON_ID_DOWN, context);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_up_down_click_handler);
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// API Functions
//

HealthCardView *health_card_view_create(HealthData *health_data) {
  HealthCardView *health_card_view = app_malloc_check(sizeof(HealthCardView));
  *health_card_view = (HealthCardView) {
    .health_data = health_data,
  };
  window_init(&health_card_view->window, "Health Card View");
  window_set_click_config_provider_with_context(&health_card_view->window,
                                                prv_click_config_provider, health_card_view);
  Layer *window_root = window_get_root_layer(&health_card_view->window);

  // create and add all card layers to window root layer
  for (int i = 0; i < CardCount; i++) {
    health_card_view->card_layers[i] = s_card_view_create[i](health_data);
    layer_add_child(window_root, health_card_view->card_layers[i]);
  }

  // set starting card based on launch args
  HealthLaunchArgs launch_args = { .args = app_launch_get_args() };

  health_card_view->current_card_index =
      (launch_args.card_type == HealthCardType_Sleep) ? Card_SleepSummary : Card_ActivitySummary;

  // set window background
  prv_set_window_background_color(health_card_view);

  // position current card
  layer_set_frame(health_card_view->card_layers[health_card_view->current_card_index],
                  &window_root->frame);

  // setup select indicator
  layer_init(&health_card_view->select_indicator_layer, &window_root->frame);
  layer_add_child(window_root, &health_card_view->select_indicator_layer);
  layer_set_update_proc(&health_card_view->select_indicator_layer,
                        prv_select_indicator_layer_update_proc);

  // setup content indicators
  const int content_indicator_height = PBL_IF_ROUND_ELSE(18, 11);
  const GRect down_arrow_layer_frame = grect_inset(window_root->frame,
      GEdgeInsets(window_root->frame.size.h - content_indicator_height, 0, 0));
  layer_init(&health_card_view->down_arrow_layer, &down_arrow_layer_frame);
  layer_add_child(window_root, &health_card_view->down_arrow_layer);
  content_indicator_init(&health_card_view->down_indicator);

  const GRect up_arrow_layer_frame = grect_inset(window_root->frame,
      GEdgeInsets(0, 0, window_root->frame.size.h - content_indicator_height));
  layer_init(&health_card_view->up_arrow_layer, &up_arrow_layer_frame);
  layer_add_child(window_root, &health_card_view->up_arrow_layer);
  content_indicator_init(&health_card_view->up_indicator);

  prv_refresh_content_indicators(health_card_view);

  return health_card_view;
}

void health_card_view_destroy(HealthCardView *health_card_view) {
  // destroy cards
  health_activity_summary_card_destroy(health_card_view->card_layers[Card_ActivitySummary]);
  health_sleep_summary_card_destroy(health_card_view->card_layers[Card_SleepSummary]);
  // destroy self
  window_deinit(&health_card_view->window);
  app_free(health_card_view);
}

void health_card_view_push(HealthCardView *health_card_view) {
  app_window_stack_push(&health_card_view->window, true);
}

void health_card_view_mark_dirty(HealthCardView *health_card_view) {
  layer_mark_dirty(health_card_view->card_layers[health_card_view->current_card_index]);
}
