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

#include "pin_window.h"
#include "timeline.h"
#include "timeline_animations.h"
#include "timeline_model.h"

#include "applib/app.h"
#include "applib/ui/animation_interpolate.h"
#include "applib/ui/animation_timing.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/kino/kino_reel/scale_segmented.h"
#include "applib/ui/kino/kino_reel/unfold.h"
#include "applib/ui/ui.h"
#include "drivers/rtc.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "process_management/app_manager.h"
#include "resource/resource_ids.auto.h"
#include "resource/timeline_resource_ids.auto.h"
#include "services/common/analytics/analytics.h"
#include "services/common/compositor/compositor_transitions.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/blob_db/pin_db.h"
#include "services/normal/timeline/actions_endpoint.h"
#include "services/normal/timeline/attribute.h"
#include "shell/normal/watchface.h"
#include "syscall/syscall.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/array.h"
#include "util/attributes.h"
#include "util/size.h"
#include "util/uuid.h"

// This is used to determine whether this app was launched as Timeline or Timeline Past.
// See timeline_get_app_info, timeline_past_get_app_info, and the usage of sys_get_app_uuid.
// uuid: DAAE3686-BFF6-4BA5-921B-262F847BB6E8
#define TIMELINE_PAST_UUID_INIT {0xDA, 0xAE, 0x36, 0x86, 0xBF, 0xF6, 0x4B, 0xA5, \
                                 0x92, 0x1B, 0x26, 0x2F, 0x84, 0x7B, 0xB6, 0xE8}

#if PBL_ROUND || PLATFORM_TINTIN
// Tintin looks funny with the dot animation, but it results in less code space usage
#define ANIMATION_DOT 1
#define ANIMATION_SLIDE 0
#else
#define ANIMATION_DOT 0
#define ANIMATION_SLIDE 1
#endif

typedef struct TimelineAppStyle {
  int16_t peek_offset_y;
  int16_t peek_icon_offset_y;
} TimelineAppStyle;

static const TimelineAppStyle s_style_medium = {
  .peek_icon_offset_y = PEEK_LAYER_ICON_OFFSET_Y,
};

static const TimelineAppStyle s_style_large = {
  .peek_offset_y = -7,
  .peek_icon_offset_y = -16,
};

static const TimelineAppStyle * const s_styles[NumPreferredContentSizes] = {
  [PreferredContentSizeSmall] = &s_style_medium,
  [PreferredContentSizeMedium] = &s_style_medium,
  [PreferredContentSizeLarge] = &s_style_large,
  [PreferredContentSizeExtraLarge] = &s_style_large,
};

static TimelineAppData *s_app_data;

static const uint32_t TIMELINE_SLIDE_ANIMATION_MS = 150;
static const uint32_t PEEK_SLIDE_ANIMATION_MS = 300;
static const uint32_t PEEK_SHOW_TIME_MS = 660;


static const TimelineAppStyle *prv_get_style(void) {
  return s_styles[PreferredContentSizeDefault];
}

/////////////////////////////////////
// State Machine
/////////////////////////////////////

static bool prv_can_transition_state(TimelineAppData *data, TimelineAppState next_state) {
  // all non-exit states can transition to exit
  if (data->state != TimelineAppStateExit &&
      next_state == TimelineAppStateExit) {
    return true;
  }
  switch (data->state) {
    case TimelineAppStateNone:
      return (next_state == TimelineAppStatePeek ||
              next_state == TimelineAppStateHidePeek ||
              next_state == TimelineAppStateFarDayHidePeek ||
              next_state == TimelineAppStateNoEvents);
    case TimelineAppStatePeek:
      return (next_state == TimelineAppStateHidePeek);
    case TimelineAppStateHidePeek:
      return (next_state == TimelineAppStateStationary);
    case TimelineAppStateFarDayHidePeek:
      return (next_state == TimelineAppStateDaySeparator);
    case TimelineAppStateStationary:
      return (next_state == TimelineAppStateUpDown ||
              next_state == TimelineAppStatePushCard ||
              next_state == TimelineAppStateNoEvents ||
              next_state == TimelineAppStateInactive);
    case TimelineAppStateUpDown:
      return (next_state == TimelineAppStateUpDown ||
              next_state == TimelineAppStateShowDaySeparator ||
              next_state == TimelineAppStateStationary);
    case TimelineAppStateShowDaySeparator:
      return (next_state == TimelineAppStateDaySeparator);
    case TimelineAppStateDaySeparator:
      return (next_state == TimelineAppStateHideDaySeparator);
    case TimelineAppStateHideDaySeparator:
      return (next_state == TimelineAppStateStationary);
    case TimelineAppStatePushCard:
      return (next_state == TimelineAppStateCard ||
              next_state == TimelineAppStatePopCard);
    case TimelineAppStateCard:
      return (next_state == TimelineAppStatePopCard ||
              next_state == TimelineAppStateStationary);
    case TimelineAppStatePopCard:
      return (next_state == TimelineAppStateStationary ||
              next_state == TimelineAppStatePushCard);
    case TimelineAppStateNoEvents:
      return (next_state == TimelineAppStateInactive);
    case TimelineAppStateInactive:
    case TimelineAppStateExit:
      return false;
    default:
      WTF;
  }
}

static bool prv_set_state(TimelineAppData *data, TimelineAppState next_state) {
  const bool can_transition = prv_can_transition_state(data, next_state);
  PBL_LOG(LOG_LEVEL_DEBUG, "state transition %d->%d valid:%d",
          data->state, next_state, can_transition);
  if (can_transition) {
    data->state = next_state;
  }
  return can_transition;
}

/////////////////////////////////////
// Exit Animation & Inactivity Timer
/////////////////////////////////////

T_STATIC void prv_init_peek_layer(TimelineAppData *data);

static void prv_launch_watchface(void *data) {
#ifdef SHELL_SDK
  // FIXME: We don't want to show off our unfinished animations in the sdkshell
  watchface_launch_default(NULL);
#else

#if PLATFORM_TINTIN
  const CompositorTransition *transition = NULL;
#else
  const bool is_future = (s_app_data->timeline_model.direction == TimelineIterDirectionFuture);
  const bool to_timeline = false;
  const CompositorTransition *transition = PBL_IF_RECT_ELSE(
      compositor_slide_transition_timeline_get(is_future, to_timeline, timeline_model_is_empty()),
      compositor_dot_transition_timeline_get(is_future, to_timeline));
#endif

  watchface_launch_default(transition);
#endif
}

static void prv_cleanup_timer(EventedTimerID *timer) {
  if (evented_timer_exists(*timer)) {
    evented_timer_cancel(*timer);
    *timer = EVENTED_TIMER_INVALID_ID;
  }
}

static void prv_exit_timer_callback(void *context) {
  TimelineAppData *data = context;
  data->timeline_layer.animating_intro_or_exit = false;
  launcher_task_add_callback(prv_launch_watchface, data);
}

static void prv_intro_or_exit_anim_started(Animation *anim, void *context) {
  TimelineAppData *data = context;
  data->timeline_layer.animating_intro_or_exit = true;
}

static void prv_exit_anim_stopped(Animation *animation, bool finished, void *context) {
  // we must use a timer to allow the last frame to render
  const int exit_timeout_ms = 2 * ANIMATION_TARGET_FRAME_INTERVAL_MS;
  evented_timer_register(exit_timeout_ms, false, prv_exit_timer_callback, context);
}

//! Used for setting the animation frame source and/or destination of the peek layer.
//! If use_pin is true, the animation frame size and position will be that of the first pin icon.
//! If shift_offscreen is true, the frame will be shifted by the screen row amount in a direction
//! depending on the scroll direction.
static void prv_get_icon_animation_frame(TimelineAppData *data, GRect *icon_frame_out,
                                         bool use_pin, bool shift_offscreen) {
#if ANIMATION_DOT
  const GRect *layer_frame = &data->timeline_window.layer.frame;
  *icon_frame_out = (GRect) {
    .origin.x = layer_frame->origin.x + (layer_frame->size.w - UNFOLD_DOT_SIZE_PX) / 2,
    .origin.y = layer_frame->origin.y + (layer_frame->size.h - UNFOLD_DOT_SIZE_PX) / 2,
    .size = UNFOLD_DOT_SIZE,
  };
#elif ANIMATION_SLIDE
  GRect icon_frame;
  TimelineLayout *first_timeline_layout = timeline_layer_get_current_layout(&data->timeline_layer);
  if (first_timeline_layout && use_pin) {
    GRect frame;
    timeline_layer_get_layout_frame(&data->timeline_layer, TIMELINE_LAYER_FIRST_VISIBLE_LAYOUT,
                                    &frame);
    timeline_layout_get_icon_frame(&frame, data->timeline_layer.scroll_direction, &icon_frame);
  } else {
    // Since there is no pin, we need the peek size, which is the large size
    icon_frame = (GRect) { .size = TIMELINE_LARGE_RESOURCE_SIZE };
    grect_align(&icon_frame, &data->peek_layer.layer.frame, GAlignCenter, false);
    const TimelineAppStyle *style = prv_get_style();
    icon_frame.origin.y += style->peek_icon_offset_y;
  }
  if (shift_offscreen) {
    if (data->timeline_model.direction == TimelineIterDirectionPast) {
      icon_frame.origin.y -= DISP_ROWS;
    } else {
      icon_frame.origin.y += DISP_ROWS;
    }
  }
  *icon_frame_out = icon_frame;
#endif
}

static Animation *prv_create_peek_exit_anim(TimelineAppData *data, TimelineAppState prev_state,
                                            uint32_t duration) {
  PeekLayer *peek_layer = &data->peek_layer;
  if (prev_state == TimelineAppStateNoEvents ||
      prev_state == TimelineAppStatePeek ||
      prev_state == TimelineAppStateHidePeek) {
    prv_cleanup_timer(&data->intro_timer_id);
  } else if (prev_state == TimelineAppStateStationary ||
             prev_state == TimelineAppStateUpDown) {
    TimelineLayout *first_timeline_layout =
        timeline_layer_get_current_layout(&data->timeline_layer);
    if (!first_timeline_layout) {
      return NULL;
    }

    prv_init_peek_layer(data);

    GRect icon_from;
    layer_get_global_frame((Layer *)&first_timeline_layout->icon_layer, &icon_from);

    peek_layer_set_icon_with_size(peek_layer, &first_timeline_layout->icon_info,
                                  TimelineResourceSizeTiny, icon_from);
  } else {
    return NULL;
  }

  GRect icon_to;
  const bool use_pin = true;
  const bool shift_offscreen = true;
  prv_get_icon_animation_frame(data, &icon_to, use_pin, shift_offscreen);

  peek_layer_clear_fields(peek_layer);
  peek_layer_set_scale_to(peek_layer, icon_to);
  peek_layer_set_duration(peek_layer, duration);

#if PLATFORM_TINTIN
  return (Animation *)peek_layer_create_play_animation(&data->peek_layer);
#else
  // Play only a section to reduce the duration to the scaling, ignoring the PDCS duration
  return (Animation *)peek_layer_create_play_section_animation(&data->peek_layer, 0, duration);
#endif
}

static Animation *prv_create_sidebar_animation(TimelineAppData *data, bool open) {
  int16_t to_sidebar_width;
  if (open) {
    to_sidebar_width = timeline_layer_get_ideal_sidebar_width();
  } else {
    const GRect *layer_frame = &data->timeline_window.layer.frame;
    to_sidebar_width = layer_frame->size.w;
#if PBL_ROUND
    // Use a larger width to ensure we fill the entire screen since we use a circular background
    to_sidebar_width += 25;
#endif
  }
  return timeline_layer_create_sidebar_animation(&data->timeline_layer, to_sidebar_width);
}

static void prv_exit(TimelineAppData *data) {
  UNUSED const TimelineAppState prev_state = data->state;
  if (!prv_set_state(data, TimelineAppStateExit)) {
    return;
  }

#if ANIMATION_SLIDE
  prv_launch_watchface(data);
#elif ANIMATION_DOT
  const uint32_t duration = interpolate_moook_in_duration();

  animation_unschedule(data->current_animation);
  layer_remove_child_layers((Layer *)&data->timeline_layer);

  Animation *sidebar_slide = prv_create_sidebar_animation(data, false /* open */);
  animation_set_duration(sidebar_slide, duration);
  animation_set_handlers(sidebar_slide, (AnimationHandlers) {
    .started = prv_intro_or_exit_anim_started,
    .stopped = prv_exit_anim_stopped,
  }, data);

  Animation *peek_anim = prv_create_peek_exit_anim(data, prev_state, duration);

  // Just play them at the same time
  animation_schedule(sidebar_slide);
  if (peek_anim) {
    animation_schedule(peek_anim);
  }
#endif
}

static void prv_inactive_timer_callback(void *data) {
  prv_set_state(data, TimelineAppStateInactive);
  prv_exit(data);
}

static void prv_inactive_timer_refresh(TimelineAppData *data) {
  static const uint32_t INACTIVITY_TIMEOUT_MS = 30 * 1000;
  s_app_data->inactive_timer_id = evented_timer_register_or_reschedule(
      s_app_data->inactive_timer_id, INACTIVITY_TIMEOUT_MS, prv_inactive_timer_callback, data);
}

/////////////////////////////////////
// Pin View
/////////////////////////////////////

static void prv_move_timeline_layer_stopped(Animation *animation, bool finished, void *context) {
  TimelineAppData *data = context;

  // reset the timeline layer
  data->timeline_layer.layer.bounds.origin.x = 0;
  window_set_background_color(&data->timeline_window, GColorWhite);

  if (!finished) {
    return;
  }

  TimelineLayout *timeline_layout = timeline_layer_get_current_layout(&data->timeline_layer);
  if (!timeline_layout) {
    return;
  }

  // cut to the card window
  app_window_stack_push(&data->pin_window.window, false);
  prv_set_state(data, TimelineAppStateCard);

  TimelineIterState *state = timeline_model_get_current_state();
  Uuid app_uuid;
  timeline_get_originator_id(&state->pin, &app_uuid);
  analytics_event_pin_open(state->pin.header.timestamp, &app_uuid);
}

static Animation *prv_animate_to_pin_window(TimelineAppData *data) {
  Layer *layer = &data->timeline_layer.layer;
  GPoint to_origin = GPoint(-layer->bounds.size.w, 0);
  Animation *animation = (Animation *)property_animation_create_bounds_origin(layer, NULL,
                                                                              &to_origin);
  animation_set_handlers(animation, (AnimationHandlers) {
    .stopped = prv_move_timeline_layer_stopped,
  }, data);
  animation_set_duration(animation, TIMELINE_CARD_TRANSITION_MS / 2);
  animation_set_custom_interpolation(animation, interpolate_moook);
  animation_schedule(animation);
  return animation;
}

static void prv_push_pin_window(TimelineAppData *data, TimelineIterState *state, bool animated) {
  TimelineLayout *timeline_layout = timeline_layer_get_current_layout(&data->timeline_layer);
  if (!timeline_layout) {
    return;
  }

  // Animation structure:
  // - Scheduled simultaneously
  //   - Transition pin to card
  //   - Move timeline layer to the left

  animation_unschedule(data->current_animation);

  // initialize the pin window with the card layout
  timeline_pin_window_init(&data->pin_window, &state->pin, state->current_day);

  // match the card background color
  const LayoutColors *colors = layout_get_colors((LayoutLayer *)timeline_layout);
  window_set_background_color(&data->timeline_window, colors->bg_color);

  // animate the card from the right
#if !PLATFORM_TINTIN
  TimelineLayout *card_timeline_layout = data->pin_window.item_detail_layer.timeline_layout;
  timeline_layout_transition_pin_to_card(timeline_layout, card_timeline_layout);
#endif

  // animate the timeline to the left
  data->current_animation = prv_animate_to_pin_window(data);
}

static bool prv_pin_in_card(TimelineAppData *data, Uuid *uuid) {
  if (!app_window_stack_contains_window((Window *)&data->pin_window)) {
    return false;
  }

  TimelineIterState *current_state = timeline_model_get_current_state();
  if (current_state == NULL) {
    return false;
  }

  return uuid_equal(&current_state->pin.header.id, uuid);
}

static void prv_refresh_pin(TimelineAppData *data, int idx) {
  PBL_ASSERTN(idx >= 0);
  TimelineIterState *state = timeline_model_get_iter_state(idx);
  timeline_iter_refresh_pin(state);

  if (idx == 0 && prv_pin_in_card(data, &state->pin.header.id)) {
    timeline_pin_window_set_item(&data->pin_window, &state->pin, state->current_day);
  }
}

/////////////////////////////////////
// Timeline Controller
/////////////////////////////////////

T_STATIC void prv_setup_no_events_peek(TimelineAppData *data);

static void prv_update_timeline_layer(TimelineAppData *data) {
  TimelineLayer *timeline_layer = &data->timeline_layer;
  if (data->state != TimelineAppStateStationary &&
      data->state != TimelineAppStateUpDown &&
      data->state != TimelineAppStateCard) {
    return;
  }
  animation_unschedule(data->current_animation);
  data->current_animation = NULL;
  timeline_layer_reset(timeline_layer);

  if (timeline_model_is_empty() &&
      prv_set_state(data, TimelineAppStateNoEvents)) {
    // Hide layouts and animate to "No events"
    timeline_layer_set_layouts_hidden(&data->timeline_layer, true);

    prv_init_peek_layer(data);
    prv_setup_no_events_peek(data);
    peek_layer_play(&data->peek_layer);

    Animation *sidebar_slide = prv_create_sidebar_animation(data, false /* open */);
    data->current_animation = sidebar_slide;
    animation_schedule(sidebar_slide);
  }
}

static void prv_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  TimelineAppData *data = context;
  prv_exit(data);
}

static void prv_up_down_stopped(Animation *animation, bool finished, void *context) {
  TimelineAppData *data = context;
  if (finished) {
    prv_set_state(data, TimelineAppStateStationary);
  }
}

static void prv_hide_day_sep_stopped(Animation *animation, bool finished, void *context) {
  TimelineAppData *data = context;
  if (!finished || !prv_set_state(data, TimelineAppStateStationary)) {
    return;
  }

  data->current_animation = NULL;
  prv_update_timeline_layer(data);

  Animation *move_animation = timeline_layer_create_up_down_animation(
      &data->timeline_layer, TIMELINE_UP_DOWN_ANIMATION_DURATION_MS / 2,
      timeline_animation_interpolate_moook_second_half);
  animation_set_handlers(move_animation, (AnimationHandlers) {
    .stopped = prv_up_down_stopped,
  }, data);

  data->current_animation = move_animation;
  animation_schedule(move_animation);
}

static void prv_hide_day_sep(void *context) {
  TimelineAppData *data = context;
  data->day_separator_timer_id = EVENTED_TIMER_INVALID_ID;
  if (!prv_set_state(data, TimelineAppStateHideDaySeparator)) {
    return;
  }

  animation_unschedule(data->current_animation);

  Animation *day_sep_hide = timeline_layer_create_day_sep_hide(&data->timeline_layer);
  animation_set_handlers(day_sep_hide, (AnimationHandlers){
    .stopped = prv_hide_day_sep_stopped,
  }, data);
  data->current_animation = day_sep_hide;
  animation_schedule(day_sep_hide);
}

static bool prv_attempt_hide_day_sep(TimelineAppData *data) {
  if (data->state == TimelineAppStateDaySeparator) {
    prv_cleanup_timer(&data->day_separator_timer_id);
    prv_hide_day_sep(data);
    return true;
  }
  return false;
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  TimelineAppData *data = context;

  prv_attempt_hide_day_sep(data);

  if (!prv_set_state(data, TimelineAppStatePushCard)) {
    return;
  }

  TimelineIterState *state = timeline_model_get_current_state();
  if (state) {
    const bool animated = true;
    prv_push_pin_window(data, state, animated);
  }
}

static void prv_set_day_sep_timer(TimelineAppData *data) {
  const int DAY_SEP_TIMEOUT_MS = 1000;
  data->day_separator_timer_id = evented_timer_register(DAY_SEP_TIMEOUT_MS,
                                                        false,
                                                        prv_hide_day_sep,
                                                        data);
}

static void prv_day_sep_show_stopped(Animation *animation, bool finished, void *context) {
  TimelineAppData *data = context;
  if (!finished || !prv_set_state(data, TimelineAppStateDaySeparator)) {
    return;
  }

  // Pins will reappear after the day separator completes hiding in `prv_hide_day_sep_stopped`
  timeline_layer_set_layouts_hidden(&data->timeline_layer, true);

  prv_set_day_sep_timer(data);
}

static void prv_up_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  TimelineAppData *data = context;

  prv_inactive_timer_refresh(data);

  ButtonId button = click_recognizer_get_button_id(recognizer);
  const bool next = (button == BUTTON_ID_UP) ^
    (data->timeline_model.direction == TimelineIterDirectionFuture);

  // We want to know if it was stationary before transitioning
  const bool was_stationary = (data->state == TimelineAppStateStationary);

  if (data->state == TimelineAppStateNoEvents) {
    if (!next) {
      prv_exit(data);
    }
    return; // There are no events
  } else if (prv_attempt_hide_day_sep(data)) {
    return; // Successfully interrupted the day separator, let it hide
  } else if (!prv_set_state(data, TimelineAppStateUpDown)) {
    return; // Not in a state able to scroll at the moment
  }

  animation_unschedule(data->current_animation);

  int new_idx;
  bool has_new;
  if (next) {
    if (!timeline_model_iter_next(&new_idx, &has_new)) {
      prv_set_state(data, TimelineAppStateStationary);
      goto done;
    }
    if (has_new) {
      timeline_layer_set_next_item(&data->timeline_layer, new_idx);
    }
    timeline_layer_move_data(&data->timeline_layer, 1);
  } else {
    if (!timeline_model_iter_prev(&new_idx, &has_new)) {
      prv_exit(data);
      goto done;
    }
    if (has_new) {
      timeline_layer_set_prev_item(&data->timeline_layer, new_idx);
    }
    timeline_layer_move_data(&data->timeline_layer, -1);
  }

  // If we interrupted a previous scroll, hasten this scroll
  const bool is_hasted = !was_stationary;
  const uint32_t duration = TIMELINE_UP_DOWN_ANIMATION_DURATION_MS;
  const InterpolateInt64Function interpolate = is_hasted ?
      timeline_animation_interpolate_moook_second_half :
      timeline_animation_interpolate_moook_soft;
  Animation *move_animation = timeline_layer_create_up_down_animation(
      &data->timeline_layer, duration, interpolate);

  if (timeline_layer_should_animate_day_separator(&data->timeline_layer) &&
      prv_set_state(data, TimelineAppStateShowDaySeparator)) {
    Animation *day_sep_show = timeline_layer_create_day_sep_show(&data->timeline_layer);
    move_animation = animation_spawn_create(move_animation, day_sep_show, NULL);
    animation_set_handlers(move_animation, (AnimationHandlers) {
      .stopped = prv_day_sep_show_stopped,
    }, data);
  } else {
    animation_set_handlers(move_animation, (AnimationHandlers) {
      .stopped = prv_up_down_stopped,
    }, data);
  }

  data->current_animation = move_animation;
  animation_schedule(move_animation);

done:
  if (data->timeline_model.direction == TimelineIterDirectionPast) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_TIMELINE_PAST_NAVIGATION_COUNT, AnalyticsClient_System);
  } else {
    analytics_inc(ANALYTICS_DEVICE_METRIC_TIMELINE_FUTURE_NAVIGATION_COUNT, AnalyticsClient_System);
  }
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_up_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
}

static void prv_blobdb_event_handler(PebbleEvent *event, void *context) {
  TimelineAppData *data = context;
  PebbleBlobDBEvent *blobdb_event = &event->blob_db;
  if (blobdb_event->db_id != BlobDBIdPins) {
    // we only care about pins
    return;
  }

  BlobDBEventType type = blobdb_event->type;
  Uuid *id = (Uuid *)blobdb_event->key;
  if (type == BlobDBEventTypeDelete) {
    if (prv_pin_in_card(data, id)) {
      // remove the pin window if we just removed the pin
      app_window_stack_remove((Window *)&data->pin_window, false);
      prv_set_state(data, TimelineAppStateStationary);
    }
    timeline_model_remove(id);
    prv_update_timeline_layer(data);
  } else if (type == BlobDBEventTypeInsert) {
    for (int i = 0; i < TIMELINE_NUM_VISIBLE_ITEMS; i++) {
      if (timeline_model_get_iter_state(i)->node &&
          uuid_equal(&timeline_model_get_iter_state(i)->pin.header.id, id)) {
        prv_refresh_pin(data, i);
      }
    }
    prv_update_timeline_layer(data);
  }
}

/////////////////////////////////////
// Intro Animation
/////////////////////////////////////

static void prv_intro_anim_stopped(Animation *anim, bool finished, void *context) {
  TimelineAppData *data = context;
  i18n_free_all(&data->peek_layer);
  peek_layer_deinit(&data->peek_layer);
  window_set_click_config_provider_with_context(&data->timeline_window, prv_click_config_provider,
                                                data);
  data->timeline_layer.animating_intro_or_exit = false;

  if (!finished || (!prv_set_state(s_app_data, TimelineAppStateStationary) &&
                    !prv_set_state(s_app_data, TimelineAppStateDaySeparator))) {
    return;
  }

  data->current_animation = NULL;
  prv_update_timeline_layer(data);

  if (data->state == TimelineAppStateDaySeparator) {
    // Hidden until the day separator hide animation stops
    timeline_layer_set_layouts_hidden(&data->timeline_layer, true);
#if ANIMATION_DOT
    timeline_layer_unfold_day_sep(&data->timeline_layer);
#elif ANIMATION_SLIDE
    timeline_layer_slide_day_sep(&data->timeline_layer);
#endif
    prv_set_day_sep_timer(data);
  } else {
#if CAPABILITY_HAS_TIMELINE_PEEK
    GPoint direction = GPoint(0, -1);
#else
    GPoint direction = GPoint(1, 0);
#endif
    Animation *layer_bounce = timeline_layer_create_bounce_back_animation(&data->timeline_layer,
                                                                          direction);
    data->current_animation = layer_bounce;
    animation_schedule(layer_bounce);
  }
}

static Animation *prv_create_intro_animation(TimelineAppData *data, uint32_t duration,
                                             bool was_mini_peek) {
  // Animation structure:
  // - Scheduled simultaneously
  //   - Spawn
  //     - Move peek layer to right (frame)
  //     - Resize sidebar from fullscreen to thin
  //     - After completion
  //       - Bounce back timeline pin layouts
  //     - Speed lines (if launching into a deep pin)

  // animate the peek layer to the right
  GRect *start = &data->peek_layer.layer.frame;
  GRect stop = { { was_mini_peek ? 0 : start->size.w , 0 }, start->size };
  Animation *peek_out = (Animation *)property_animation_create_layer_frame(
      (Layer *)&data->peek_layer, start, &stop);
  animation_set_duration(peek_out, duration);
  animation_set_custom_interpolation(peek_out, interpolate_moook_in_only);

  // resize the sidebar from fullscreen to become thin on the right
  Animation *sidebar_slide = prv_create_sidebar_animation(data, true /* open */);
  animation_set_duration(sidebar_slide, duration);

  Animation *speed_lines = data->launch_into_deep_pin ?
      timeline_layer_create_speed_lines_animation(&data->timeline_layer) : NULL;

  return animation_spawn_create(peek_out, sidebar_slide, speed_lines, NULL);
}

static void prv_play_peek_in(TimelineAppData *data) {
  // Skip the first frame since the icon is offscreen
  const int num_frames_skip = 1;
  // The peek layer scale animation has a bounce back effect, so the icon reaches the destination
  // if set to exactly the short moook in duration, so extend with more frames
  const int num_frames_extend = 3;
  const uint32_t duration = interpolate_moook_in_duration();
  peek_layer_set_duration(&data->peek_layer, duration + ((num_frames_skip + num_frames_extend) *
                                                         ANIMATION_TARGET_FRAME_INTERVAL_MS));
  Animation *animation = (Animation *)peek_layer_create_play_animation(&data->peek_layer);
  animation_schedule(animation);
  animation_set_elapsed(animation, num_frames_skip * ANIMATION_TARGET_FRAME_INTERVAL_MS);
}

static void prv_scale_peek_to_first_pin_icon(TimelineAppData *data, uint32_t duration,
                                             bool was_mini_peek) {
  TimelineLayout *first_timeline_layout = timeline_layer_get_current_layout(&data->timeline_layer);
  if (!first_timeline_layout) {
    return;
  }

  // scale the peek layer icon to the pin position
  GRect frame;
  layer_get_global_frame((Layer *)first_timeline_layout, &frame);
  GRect icon_to;
  timeline_layout_get_icon_frame(&frame, data->timeline_layer.scroll_direction, &icon_to);
  const bool align_in_frame = true;
  PeekLayer *peek_layer = &data->peek_layer;
  peek_layer_set_scale_to_image(peek_layer, &first_timeline_layout->icon_info,
                                TimelineResourceSizeTiny, icon_to, align_in_frame);

#if ANIMATION_SLIDE
  if (was_mini_peek) {
    prv_play_peek_in(data);
    return;
  }
#endif

  peek_layer_set_duration(peek_layer, duration);
  peek_layer_play(peek_layer);
}

static void prv_intro_timer_callback(void *context) {
  TimelineAppData *data = context;
  data->intro_timer_id = EVENTED_TIMER_INVALID_ID;

  // if we are already hiding the peek, we were in a mini peek
  const bool was_mini_peek = (data->state == TimelineAppStateHidePeek);

  prv_set_state(data, TimelineAppStateHidePeek);

  if (data->state != TimelineAppStateHidePeek &&
      data->state != TimelineAppStateFarDayHidePeek) {
    return;
  }

  // hide the peek text
  PeekLayer *peek_layer = &data->peek_layer;
  peek_layer_clear_fields(peek_layer);

  animation_unschedule(data->current_animation);

  const uint32_t duration = was_mini_peek ? interpolate_moook_in_duration() :
                                            TIMELINE_SLIDE_ANIMATION_MS;
  Animation *intro = prv_create_intro_animation(data, duration, was_mini_peek);
  animation_set_handlers(intro, (AnimationHandlers) {
    .started = prv_intro_or_exit_anim_started,
    .stopped = prv_intro_anim_stopped,
  }, data);

  data->current_animation = intro;
  animation_schedule(intro);

  if (!layer_get_hidden((Layer *)&data->peek_layer)) {
    prv_scale_peek_to_first_pin_icon(data, duration, was_mini_peek);
  }
}

static void prv_open_did_focus_handler(PebbleEvent *e, void *context) {
  TimelineAppData *data = context;
  event_service_client_unsubscribe(&data->focus_event_info);
  prv_intro_timer_callback(data);
}

static void prv_peek_did_focus_handler(PebbleEvent *e, void *context) {
  TimelineAppData *data = context;
  event_service_client_unsubscribe(&data->focus_event_info);

#if ANIMATION_DOT
  peek_layer_play(&data->peek_layer);
#elif ANIMATION_SLIDE
  prv_play_peek_in(data);
#endif

  if (data->state == TimelineAppStateNoEvents) {
    window_set_click_config_provider_with_context(&data->timeline_window, prv_click_config_provider,
                                                  data);
  } else if (data->state == TimelineAppStatePeek &&
             data->intro_timer_id == EVENTED_TIMER_INVALID_ID) {
    data->intro_timer_id = evented_timer_register(PEEK_SHOW_TIME_MS,
                                                  false,
                                                  prv_intro_timer_callback,
                                                  data);
  }
}

static void prv_setup_peek_animation(TimelineAppData *data, TimelineResourceInfo *timeline_res,
                                     bool use_pin) {
  PeekLayer *peek_layer = &data->peek_layer;
#if ANIMATION_DOT
  peek_layer_set_icon(peek_layer, timeline_res);
#elif ANIMATION_SLIDE
  GRect icon_from;
  GRect icon_to;
  const bool shift_offscreen_from = true;
  const bool shift_offscreen_to = false;
  prv_get_icon_animation_frame(data, &icon_from, use_pin, shift_offscreen_from);
  prv_get_icon_animation_frame(data, &icon_to, use_pin, shift_offscreen_to);
  peek_layer_set_icon_with_size(peek_layer, timeline_res, TimelineResourceSizeLarge, icon_from);
  peek_layer_set_scale_to(peek_layer, icon_to);
  peek_layer_set_fields_hidden(peek_layer, true);
#endif
}

T_STATIC void prv_setup_no_events_peek(TimelineAppData *data) {
  PeekLayer *peek_layer = &data->peek_layer;
  // set the text
  peek_layer_set_fields(peek_layer, "", i18n_get("No events", peek_layer), "");
  // set the icon resource
  TimelineResourceInfo timeline_res = {
    .res_id = TIMELINE_RESOURCE_NO_EVENTS,
  };
  const bool use_pin = false;
  prv_setup_peek_animation(data, &timeline_res, use_pin);
}

static void prv_setup_first_pin_peek(TimelineAppData *data) {
  TimelineIterState *state = timeline_model_get_current_state();
  // TODO: PBL-22075 Refactor Timeline Model
  // timeline_model_get_current_state explicitly tries to return NULL when supposedly empty,
  // but this does not seem to actually happen
  if (!state) {
    return;
  }

  TimelineItem *first_pin = &state->pin;
  if (!first_pin) {
    return;
  }

  TimelineLayout *first_timeline_layout = timeline_layer_get_current_layout(&data->timeline_layer);
  if (!first_timeline_layout) {
    return;
  }

  PeekLayer *peek_layer = &s_app_data->peek_layer;

#if PLATFORM_TINTIN
  const bool is_mini_peek = false;
#else
  // if we are hiding the peek, we are in a mini peek
  const bool is_mini_peek = (data->state == TimelineAppStateHidePeek);
#endif

  // set the text
  char number_buffer[TIME_STRING_REQUIRED_LENGTH] = {}; // "11"
  char word_buffer[TIME_STRING_REQUIRED_LENGTH] = {}; // "min to"
  if (!is_mini_peek) {
    clock_get_event_relative_time_string(
        number_buffer, sizeof(number_buffer), word_buffer, sizeof(word_buffer),
        first_pin->header.timestamp, first_pin->header.duration, state->current_day,
        first_pin->header.all_day);
  }
  peek_layer_set_fields(peek_layer, number_buffer, word_buffer, "");

  // set the icon
  if (is_mini_peek) {
    GRect icon_from;
    const bool shift_offscreen = true;
    const bool use_pin = true;
    prv_get_icon_animation_frame(data, &icon_from, use_pin, shift_offscreen);
    peek_layer_set_icon_with_size(peek_layer, &first_timeline_layout->icon_info,
                                  TimelineResourceSizeTiny, icon_from);
  } else {
    const bool use_pin = false;
    prv_setup_peek_animation(data, &first_timeline_layout->icon_info, use_pin);
  }
}

static void NOINLINE prv_setup_peek(TimelineAppData *data) {
  TimelineIterState *state = timeline_model_get_current_state();
  TimelineItem *first_pin = state ? &state->pin : NULL;
  EventServiceEventHandler focus_handler = prv_open_did_focus_handler;

  // we'll only show the first pin peek if timeline peek (aka quick view) isn't enabled
  time_t now = rtc_get_time();
  if (!first_pin && prv_set_state(s_app_data, TimelineAppStateNoEvents)) {
    layer_set_hidden((Layer *)&data->peek_layer, false);
    prv_setup_no_events_peek(data);
    focus_handler = prv_peek_did_focus_handler;
#if !CAPABILITY_HAS_TIMELINE_PEEK
  } else if (first_pin && (first_pin->header.timestamp + SECONDS_PER_MINUTE *
              first_pin->header.duration >= now) &&
             (first_pin->header.timestamp - SECONDS_PER_HOUR <= now) &&
             prv_set_state(data, TimelineAppStatePeek)) {
    // ongoing or within the hour
    prv_setup_first_pin_peek(data);
    focus_handler = prv_peek_did_focus_handler;
#endif
  } else if (state && (state->current_day != time_util_get_midnight_of(now)) &&
             prv_set_state(data, TimelineAppStateFarDayHidePeek)) {
    // entering into a day that isn't today, setup the day separator
    layer_set_hidden((Layer *)&data->peek_layer, true);
#if ANIMATION_DOT
    timeline_layer_set_day_sep_frame(&data->timeline_layer, &data->timeline_layer.layer.frame);
#elif ANIMATION_SLIDE
    GRect frame;
    layer_get_frame(&data->timeline_layer.day_separator.layer, &frame);
    const bool is_future = (s_app_data->timeline_model.direction == TimelineIterDirectionFuture);
    frame.origin.y += is_future ? DISP_ROWS : -DISP_ROWS;
    timeline_layer_set_day_sep_frame(&data->timeline_layer, &frame);
#endif
    focus_handler = prv_open_did_focus_handler;
  } else if (prv_set_state(data, TimelineAppStateHidePeek)) {
    // setup mini-peek where the icon animates directly into the pin position
    prv_setup_first_pin_peek(data);
    focus_handler = prv_open_did_focus_handler;
  }

  // set the did_focus handler
  data->focus_event_info = (EventServiceInfo) {
    .type = PEBBLE_APP_DID_CHANGE_FOCUS_EVENT,
    .handler = focus_handler,
    .context = s_app_data,
  };
  event_service_client_subscribe(&s_app_data->focus_event_info);
}

static GColor prv_get_sidebar_color(TimelineAppData *data) {
  if (s_app_data->timeline_model.direction == TimelineIterDirectionPast) {
    return TIMELINE_PAST_COLOR;
  } else {
    return TIMELINE_FUTURE_COLOR;
  }
}

T_STATIC void prv_init_peek_layer(TimelineAppData *data) {
  Window *window = &data->timeline_window;
  PeekLayer *peek_layer = &data->peek_layer;
  const TimelineAppStyle *style = prv_get_style();
  const GRect frame = { .origin.y = style->peek_offset_y, .size = window->layer.bounds.size };
  peek_layer_init(peek_layer, &frame);
  peek_layer_set_icon_offset_y(peek_layer, style->peek_icon_offset_y);
  peek_layer_set_frame(peek_layer, &frame);
  peek_layer_set_background_color(peek_layer, GColorClear);
  layer_add_child(&window->layer, &peek_layer->layer);
}

static void prv_timeline_window_load(Window *window) {
  TimelineLayer *timeline_layer = &s_app_data->timeline_layer;
  TimelineScrollDirection scroll_direction;
  if (s_app_data->timeline_model.direction == TimelineIterDirectionPast) {
    scroll_direction = TimelineScrollDirectionUp;
  } else {
    scroll_direction = TimelineScrollDirectionDown;
  }

  window_set_background_color(window, GColorWhite);

  // timeline layer
  timeline_layer_init(timeline_layer, &window->layer.bounds, scroll_direction);
  timeline_layer_set_sidebar_color(timeline_layer,
                                   PBL_IF_COLOR_ELSE(prv_get_sidebar_color(s_app_data),
                                                     GColorLightGray));
  timeline_layer_set_layouts_hidden(timeline_layer, true); // hide until the peek is over
  layer_set_hidden((Layer *)&timeline_layer->day_separator, true);
  layer_add_child(&window->layer, (Layer *)timeline_layer);

  // peek layer
  prv_init_peek_layer(s_app_data);
  prv_setup_peek(s_app_data);
}

static void prv_timeline_window_appear(Window *window) {
  TimelineAppData *data = window_get_user_data(window);

  // re-enable the inactivity timer back in timeline view
  prv_inactive_timer_refresh(data);
}

static void prv_timeline_window_disappear(Window *window) {
  TimelineAppData *data = window_get_user_data(window);

  // disable the inactivity timer when the user leaves
  prv_cleanup_timer(&data->inactive_timer_id);
}

static void prv_timeline_window_unload(Window *window) {
  TimelineAppData *data = window_get_user_data(window);

  // clean up any running animations
  animation_unschedule(data->current_animation);
  prv_cleanup_timer(&data->day_separator_timer_id);
}

static void prv_back_from_card_stopped(Animation *animation, bool finished, void *context) {
  TimelineAppData *data = context;
  if (!finished || !prv_set_state(data, TimelineAppStateStationary)) {
    return;
  }

  window_set_background_color(&data->timeline_window, GColorWhite);

  data->current_animation = NULL;
  prv_update_timeline_layer(data);

  Animation *layer_bounce = timeline_layer_create_bounce_back_animation(&data->timeline_layer,
                                                                        GPoint(1, 0));

  data->current_animation = layer_bounce;
  animation_schedule(layer_bounce);
}

/////////////////////////////////////
// Public API
/////////////////////////////////////

Animation *timeline_animate_back_from_card(void) {
  TimelineAppData *data = s_app_data;
  PBL_ASSERTN(data);

  if (!prv_set_state(data, TimelineAppStatePopCard)) {
    return NULL;
  }

  // Animation structure:
  // - Scheduled simultaneously
  //   - Transition card to pin
  //   - Move timeline layer from the left
  //     - After completion
  //       - Bounce back the timeline layer
  //   - Move pin window to the right

  animation_unschedule(data->current_animation);

  timeline_layer_set_layouts_hidden(&data->timeline_layer, true);
  window_set_background_color(&data->timeline_window, GColorWhite);


#if !PLATFORM_TINTIN
  TimelineLayout *pin_timeline_layout = timeline_layer_get_current_layout(&data->timeline_layer);
  if (pin_timeline_layout) {
    // animation the pin icon
    TimelineItemLayer *item_layer = &data->pin_window.item_detail_layer;
    timeline_layout_transition_card_to_pin(item_layer->timeline_layout, pin_timeline_layout);
  }
#endif

  // animate the timeline layer from the left
  Layer *layer = &data->timeline_layer.layer;
  GPoint from_origin = { -layer->bounds.size.w, 0 };
  Animation *layer_in = (Animation *)property_animation_create_bounds_origin(layer, &from_origin,
                                                                             &GPointZero);
  animation_set_duration(layer_in, TIMELINE_CARD_TRANSITION_MS / 2);
  animation_set_custom_interpolation(layer_in, interpolate_moook);
  animation_set_handlers(layer_in, (AnimationHandlers) {
    .stopped = prv_back_from_card_stopped,
  }, data);

  data->current_animation = layer_in;
  animation_schedule(layer_in);

  // animate the card layout
  timeline_pin_window_pop(&data->pin_window);

  return layer_in;
}

/////////////////////////////////////
// App boilerplate
/////////////////////////////////////

static bool NOINLINE prv_setup_timeline_app(void) {
  TimelineAppData *data = app_malloc_check(sizeof(TimelineAppData));
  s_app_data = data;
  *data = (TimelineAppData){};

  data->blobdb_event_info = (EventServiceInfo) {
    .type = PEBBLE_BLOBDB_EVENT,
    .handler = prv_blobdb_event_handler,
    .context = data,
  };
  event_service_client_subscribe(&data->blobdb_event_info);

  const TimelineArgs *args = process_manager_get_current_process_args();
  Uuid app_uuid;
  sys_get_app_uuid(&app_uuid);
  if (uuid_equal(&app_uuid, &(Uuid)TIMELINE_PAST_UUID_INIT)) {
    data->timeline_model.direction = TimelineIterDirectionPast;
  } else if (args == NULL) {
    data->timeline_model.direction = TimelineIterDirectionFuture;
  } else {
    data->timeline_model.direction = args->direction;
  }

  // check if we were asked to launch into a specific item
  time_t now = rtc_get_time();
  TimelineItem pin;
  bool launch_into_pin = false;
  if (args && args->launch_into_pin &&
      !uuid_is_invalid(&args->pin_id) &&
      pin_db_get(&args->pin_id, &pin) == S_SUCCESS) {
    launch_into_pin = true;
    if (!args->stay_in_list_view) {
      // Launching directly into the pin, change the direction to match
      data->timeline_model.direction =
          timeline_direction_for_item(&pin, data->timeline_model.timeline, now);
    }
    timeline_item_free_allocated_buffer(&pin);
  }

  timeline_model_init(now, &data->timeline_model);

  // if we're launching into a particular item, we iterate to it now
  if (launch_into_pin) {
    while (!uuid_equal(&timeline_model_get_current_state()->pin.header.id, &args->pin_id)) {
      data->launch_into_deep_pin = true;
      if (!timeline_model_iter_next(NULL, NULL)) {
        // for some reason we can't find the pin we were asked to launch into
        char uuid_buffer[UUID_STRING_BUFFER_LENGTH];
        uuid_to_string(&args->pin_id, uuid_buffer);
        PBL_LOG(LOG_LEVEL_ERROR, "Asked to launch into pin but can't find it %s", uuid_buffer);
        launch_into_pin = false;
        data->launch_into_deep_pin = false;
        // we couldn't find the launch pin, go back to the present
        while (timeline_model_iter_prev(NULL, NULL)) {}
        break;
      }
    }
  }

  Window *window = &data->timeline_window;
  window_init(window, WINDOW_NAME("Timeline"));
  window_set_user_data(window, data);
  window_set_window_handlers(window, &(WindowHandlers) {
    .load = prv_timeline_window_load,
    .appear = prv_timeline_window_appear,
    .disappear = prv_timeline_window_disappear,
    .unload = prv_timeline_window_unload
  });

  return (launch_into_pin && !(args && args->stay_in_list_view));
}

T_STATIC void NOINLINE prv_init(void) {
  bool do_push_pin_window = prv_setup_timeline_app();

  app_window_stack_push(&s_app_data->timeline_window, true /* animated */);

  if (do_push_pin_window) {
    prv_push_pin_window(s_app_data, timeline_model_get_current_state(), false /* animated */);
  }

#if CAPABILITY_HAS_TIMELINE_PEEK
  if (!timeline_model_is_empty()) {
    timeline_layer_set_sidebar_width(&s_app_data->timeline_layer,
                                     timeline_layer_get_ideal_sidebar_width());
  }
#endif
}

static void NOINLINE prv_deinit(void) {
  prv_cleanup_timer(&s_app_data->intro_timer_id);
  prv_cleanup_timer(&s_app_data->inactive_timer_id);

  event_service_client_unsubscribe(&s_app_data->focus_event_info);
  event_service_client_unsubscribe(&s_app_data->blobdb_event_info);

  timeline_layer_deinit(&s_app_data->timeline_layer);
  timeline_model_deinit();
  app_free(s_app_data);
}

static void prv_main(void) {
  prv_init();

  app_event_loop();

  prv_deinit();
}

const PebbleProcessMd *timeline_get_app_info() {
  static const PebbleProcessMdSystem s_app_md = {
    .common = {
      .main_func = prv_main,
      // uuid: 79C76B48-6111-4E80-8DEB-3119EEBEF33E
      .uuid = {0x79, 0xC7, 0x6B, 0x48, 0x61, 0x11, 0x4E, 0x80,
               0x8D, 0xEB, 0x31, 0x19, 0xEE, 0xBE, 0xF3, 0x3E},
      .visibility = ProcessVisibilityHidden,
    },
    .name = "Timeline",
  };
  return &s_app_md.common;
}

const PebbleProcessMd *timeline_past_get_app_info() {
  static const PebbleProcessMdSystem s_app_md = {
    .common = {
      .main_func = prv_main,
      .uuid = TIMELINE_PAST_UUID_INIT,
      .visibility = ProcessVisibilityQuickLaunch,
    },
    /// The title of Timeline Past in Quick Launch. If the translation is too long, cut out
    /// Timeline and only translate "Past".
    .name = i18n_noop("Timeline Past"),
  };
  return &s_app_md.common;
}
