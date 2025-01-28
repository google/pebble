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

#include "music_app.h"

#include "applib/app.h"
#include "applib/event_service_client.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/preferred_content_size.h"
#include "applib/tick_timer_service.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/property_animation.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/system_icons.h"
#include "services/common/clock.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/music.h"
#include "services/normal/vibes/vibe_score.h"
#include "shell/system_theme.h"
#include "process_management/app_manager.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum ActionBarState {
  ActionBarStateSkip,
  ActionBarStateVolume,
  ActionBarStateLongPress,
};

typedef struct MusicAppSizeConfig {
  const char *music_time_font_key;
  const char *no_music_font_key;
  int16_t horizontal_margin;

  GRangeVertical artist_field;
  GRangeVertical title_field;
  GRangeVertical time_field;

  GRect cassette_rect;
  int16_t cassette_animation_x;
  int32_t cassette_animation_time;

  GRangeVertical track_field;
  int16_t track_corner_radius;

  GPoint no_music_img_pos;
  GRangeVertical no_music_text_field;
} MusicAppSizeConfig;

// The reference animations ran at 28fps.
#define ANIMATION_FRAME_MS (1000 / 28)

#define CONTENT_VERTICAL_OFFSET PBL_IF_RECT_ELSE(0, 5)
static const MusicAppSizeConfig s_music_size_config_medium = {
  .music_time_font_key = FONT_KEY_GOTHIC_14,
  .no_music_font_key = FONT_KEY_GOTHIC_18_BOLD,
  .horizontal_margin = PBL_IF_RECT_ELSE(12, 25),

  .artist_field = {
    .origin_y = 31 + CONTENT_VERTICAL_OFFSET,
    .size_h = 21,
  },
  .title_field = {
    .origin_y = 53 + CONTENT_VERTICAL_OFFSET,
    .size_h = 44,
  },
  .time_field = {
    .origin_y = 106 + CONTENT_VERTICAL_OFFSET,
    .size_h = 14,
  },

  .cassette_rect = {{0, -1 + CONTENT_VERTICAL_OFFSET}, {43, 28}},
  .cassette_animation_x = 60,
  .cassette_animation_time = 1 * ANIMATION_FRAME_MS,

  .track_field = {
    .origin_y = 120 + CONTENT_VERTICAL_OFFSET,
    .size_h = 4,
  },
  .track_corner_radius = 1,

  .no_music_img_pos = {PBL_IF_RECT_ELSE(29, 53), PBL_IF_RECT_ELSE(25, 26)},
  .no_music_text_field = {
    .origin_y = PBL_IF_RECT_ELSE(107, 104),
    .size_h = 58,
  },
};

static const MusicAppSizeConfig s_music_size_config_large = {
  .music_time_font_key = FONT_KEY_GOTHIC_18_BOLD,
  .no_music_font_key = FONT_KEY_GOTHIC_28,
  .horizontal_margin = 10,

  .artist_field = {
    .origin_y = 30,
    .size_h = 21,
  },
  .title_field = {
    .origin_y = 60,
    .size_h = 80,
  },
  .time_field = {
    .origin_y = 146,
    .size_h = 20,
  },

  .cassette_rect = {{0, -8}, {43, 28}},
  .cassette_animation_x = 140,
  .cassette_animation_time = 3 * ANIMATION_FRAME_MS,

  .track_field = {
    .origin_y = 168,
    .size_h = 10,
  },
  .track_corner_radius = 4,

  .no_music_img_pos = {57, 46},
  .no_music_text_field = {
    .origin_y = 131,
    .size_h = 58,
  },
};

static const MusicAppSizeConfig * const s_music_size_configs[NumPreferredContentSizes] = {
  [PreferredContentSizeSmall] = &s_music_size_config_medium,
  [PreferredContentSizeMedium] = &s_music_size_config_medium,
  [PreferredContentSizeLarge] = &s_music_size_config_large,
  [PreferredContentSizeExtraLarge] = &s_music_size_config_large,
};

static const MusicAppSizeConfig *prv_config(void) {
  return s_music_size_configs[PreferredContentSizeDefault];
}

static int prv_content_width(void) {
  return DISP_COLS - ACTION_BAR_WIDTH - (prv_config()->horizontal_margin * 2);
}

static int prv_text_layer_width(void) {
  return prv_content_width() + PBL_IF_RECT_ELSE(prv_config()->horizontal_margin / 2, 0);
}

static GRect prv_artist_rect(void) {
  const MusicAppSizeConfig *config = prv_config();
  return GRect(config->horizontal_margin, config->artist_field.origin_y,
               prv_text_layer_width(), config->artist_field.size_h);
}

static GRect prv_title_rect(void) {
  const MusicAppSizeConfig *config = prv_config();
  return GRect(config->horizontal_margin, config->title_field.origin_y,
               prv_text_layer_width(), config->title_field.size_h);
}

static GRect prv_time_rect(void) {
  const MusicAppSizeConfig *config = prv_config();
  return GRect(config->horizontal_margin, config->time_field.origin_y,
               prv_content_width(), config->time_field.size_h);
}

static GRect prv_cassette_rect(void) {
  const MusicAppSizeConfig *config = prv_config();
  const int16_t cassette_x = config->horizontal_margin +
      PBL_IF_RECT_ELSE(0, prv_content_width() - config->cassette_rect.size.w);
  return GRect(cassette_x, config->cassette_rect.origin.y,
               config->cassette_rect.size.w, config->cassette_rect.size.h);
}

static GRect prv_track_rect(void) {
  const MusicAppSizeConfig *config = prv_config();
  return GRect(config->horizontal_margin, config->track_field.origin_y,
               prv_content_width(), config->track_field.size_h);
}

static const ButtonId BUTTON_FORWARD = BUTTON_ID_DOWN;
static const ButtonId BUTTON_BACKWARD = BUTTON_ID_UP;

static const int16_t BOUNCEBACK_OFFSET = 6;
// These offsets get rid of the empty space above the first line of text,
// enabling neater animations by clipping immediately at the top of the text.
static const int16_t TITLE_BOUNDS_OFFSET = 5;
static const int16_t ARTIST_BOUNDS_OFFSET = 3;
static const int16_t TIME_BOUNDS_OFFSET = 2;

static const uint32_t VOLUME_REPEAT_INTERVAL_MS = 400;
static const uint32_t ACTION_BAR_TIMEOUT_MS = 2000;
static const uint32_t VOLUME_ICON_TIMEOUT_MS = 2000;

typedef struct {
  Window window;
  BitmapLayer bitmap_layer;
  GBitmap bitmap;
  TextLayer text_layer;
} MusicNoMusicWindow;

typedef struct {
  Window window;
  ActionBarLayer action_bar;

  TextLayer artist_text_layer;
  char artist_buffer[MUSIC_BUFFER_LENGTH];

  TextLayer title_text_layer;
  char title_buffer[MUSIC_BUFFER_LENGTH];

  StatusBarLayer status_layer;

  TextLayer position_text_layer;
  char position_buffer[9]; // 9 will fit "00:00:00"

  TextLayer length_text_layer;
  char length_buffer[9];

  Animation *transition;
  AppTimer *volume_icon_timer;

  MusicPlayState current_play_state;

  GBitmap icon_skip_forward;
  GBitmap icon_skip_backward;
  GBitmap icon_ellipsis;
  GBitmap icon_pause;
  GBitmap icon_play;
  GBitmap icon_play_pause;
  GBitmap icon_volume_up;
  GBitmap icon_volume_down;
  GBitmap image_cassette;
  GBitmap image_pause;
  GBitmap image_volume_up;
  GBitmap image_volume_down;

  Layer cassette_container;
  BitmapLayer cassette_layer;
  GBitmap *cassette_current_icon;

  EventServiceInfo event_info;

  ProgressLayer track_pos_bar;
  uint32_t track_length;
  uint32_t track_pos;
  bool pause_track_pos_updates;

  enum ActionBarState action_bar_state;
  AppTimer *action_bar_revert_timer;
  AppTimer *volume_repeat_timer;
  bool volume_is_up;

  MusicNoMusicWindow *no_music_window;

#if CAPABILITY_HAS_VIBE_SCORES
  VibeScore *score;
#endif
} MusicAppData;

static void prv_set_action_bar_state(MusicAppData *data, enum ActionBarState state);

static void prv_trigger_cassette_icon_switch(GBitmap *bitmap, bool animated);
static void prv_update_cassette_icon(MusicAppData *data, bool animated);

static void prv_do_haptic_feedback_vibe(MusicAppData *data) {
#if CAPABILITY_HAS_VIBE_SCORES
  vibe_score_do_vibe(data->score);
#endif
}

static void prv_handle_volume_icon_timer(void *context) {
  MusicAppData *data = context;
  data->volume_icon_timer = NULL;
  prv_update_cassette_icon(data, true);
}

static void prv_show_volume_image(GBitmap *bitmap) {
  MusicAppData *data = app_state_get_user_data();
  if (data->volume_icon_timer) {
    app_timer_reschedule(data->volume_icon_timer, VOLUME_ICON_TIMEOUT_MS);
  } else {
    data->volume_icon_timer = app_timer_register(VOLUME_ICON_TIMEOUT_MS,
                                                 prv_handle_volume_icon_timer, data);
  }
  prv_trigger_cassette_icon_switch(bitmap, true);
}

static void prv_change_volume(bool volume_is_up) {
  if (!music_is_command_supported(MusicCommandVolumeUp) ||
      !music_is_command_supported(MusicCommandVolumeDown)) {
    return;
  }

  MusicAppData *data = app_state_get_user_data();
  prv_show_volume_image(volume_is_up ? &data->image_volume_up : &data->image_volume_down);
  music_command_send(volume_is_up ? MusicCommandVolumeUp : MusicCommandVolumeDown);
}

static Animation* prv_create_layer_upwards_animation(Layer *layer, int16_t offset) {
  GPoint target = GPoint(0, -layer->bounds.size.h - offset);
  GPoint origin = GPoint(0, -offset);
  Animation *animation = property_animation_get_animation(
      property_animation_create_bounds_origin(layer, &origin, &target));
  animation_set_duration(animation, 4 * ANIMATION_FRAME_MS);
  animation_set_curve(animation, AnimationCurveEaseIn);
  return animation;
}

static Animation* prv_create_upwards_animation(MusicAppData *data) {
  return animation_spawn_create(
      prv_create_layer_upwards_animation(&data->artist_text_layer.layer, ARTIST_BOUNDS_OFFSET),
      prv_create_layer_upwards_animation(&data->title_text_layer.layer, TITLE_BOUNDS_OFFSET),
      prv_create_layer_upwards_animation(&data->position_text_layer.layer, TIME_BOUNDS_OFFSET),
      prv_create_layer_upwards_animation(&data->length_text_layer.layer, TIME_BOUNDS_OFFSET),
      NULL);
}

static const PropertyAnimationImplementation s_frame_layer_implementation = {
    .base = {
        .update = (AnimationUpdateImplementation) property_animation_update_grect,
    },
    .accessors = {
        .setter = { .grect = (const GRectSetter) layer_set_frame_by_value, },
        .getter = { .grect = (const GRectGetter) layer_get_frame_by_value, },
    },
};

static Animation *prv_create_layer_bounceback_animation(Layer *layer, GRect origin) {
  GRect target = origin;
  origin.origin.y -= BOUNCEBACK_OFFSET;
  Animation *animation = property_animation_get_animation(
      property_animation_create(&s_frame_layer_implementation, layer, &origin, &target));
  animation_set_duration(animation, 4 * ANIMATION_FRAME_MS);
  animation_set_curve(animation, AnimationCurveEaseOut);
  return animation;
}

static Animation *prv_create_bounceback_animation(MusicAppData *data) {
  const GRect time_rect = prv_time_rect();
  return animation_spawn_create(
      prv_create_layer_bounceback_animation(&data->artist_text_layer.layer, prv_artist_rect()),
      prv_create_layer_bounceback_animation(&data->title_text_layer.layer, prv_title_rect()),
      prv_create_layer_bounceback_animation(&data->position_text_layer.layer, time_rect),
      prv_create_layer_bounceback_animation(&data->length_text_layer.layer, time_rect),
      NULL);
}

static void prv_update_track_progress(MusicAppData *data);

static void prv_flip_animated_text(Animation *animation, bool finished, void *context) {
  MusicAppData *data = context;
  data->pause_track_pos_updates = false;
  prv_update_track_progress(data);
  music_get_now_playing(data->title_buffer, data->artist_buffer, NULL);
  // Restore the layers to their original bounds for the next part of the animation.
  data->title_text_layer.layer.bounds.origin.y = -TITLE_BOUNDS_OFFSET;
  data->artist_text_layer.layer.bounds.origin.y = -ARTIST_BOUNDS_OFFSET;
  data->position_text_layer.layer.bounds.origin.y = -TIME_BOUNDS_OFFSET;
  data->length_text_layer.layer.bounds.origin.y = -TIME_BOUNDS_OFFSET;
}

static inline bool prv_should_animate_cassette(void) {
  return music_get_playback_state() != MusicPlayStatePaused;
}

static Animation *prv_create_cassette_animation(MusicAppData *data) {
  const MusicAppSizeConfig *config = prv_config();
  const GRect cassette_rect = prv_cassette_rect();
  GPoint left_target = GPoint(-cassette_rect.size.w - cassette_rect.origin.x, 0);
  Animation *cassette_left = property_animation_get_animation(
      property_animation_create_bounds_origin(&data->cassette_container, &GPointZero,
                                              &left_target));
  Animation *cassette_right = property_animation_get_animation(
      property_animation_create_bounds_origin(&data->cassette_container,
          &GPoint(config->cassette_animation_x, 0), &GPoint(-4, 0)));
  Animation *cassette_bounceback = property_animation_get_animation(
      property_animation_create_bounds_origin(&data->cassette_container, &GPoint(-4, 0),
                                              &GPointZero));
  animation_set_duration(cassette_left, 4 * ANIMATION_FRAME_MS);
  animation_set_curve(cassette_left, AnimationCurveEaseIn);
  animation_set_duration(cassette_right, config->cassette_animation_time);
  animation_set_curve(cassette_left, AnimationCurveLinear);
  animation_set_duration(cassette_bounceback, 4 * ANIMATION_FRAME_MS);
  animation_set_curve(cassette_bounceback, AnimationCurveEaseOut);
  Animation *sequence = animation_sequence_create(cassette_left, cassette_right,
                                                  cassette_bounceback, NULL);
  if (!prv_should_animate_cassette()) {
    animation_set_play_count(sequence, 0);
  }
  return sequence;
}

static void prv_trigger_track_change_animation(MusicAppData *data) {
  // Animation structure:
  // - Master animation
  //   - Cassette animation
  //     - Move to left
  //     - Move in from right
  //     - Bounceback
  //   - Upwards animation
  //     - Per-layer animations
  //   - (flip text)
  //   - Bounceback animation
  //     - Per-layer animations

  if (animation_is_scheduled(data->transition)) {
    return;
  }
  data->pause_track_pos_updates = true;
  Animation *scroll_up = prv_create_upwards_animation(data);
  Animation *bounceback = prv_create_bounceback_animation(data);
  animation_set_handlers(scroll_up, (AnimationHandlers) {
      .stopped = prv_flip_animated_text,
  }, data);

  Animation *complete;
  complete = animation_sequence_create(scroll_up, bounceback, NULL);
  data->transition = complete;
  animation_schedule(complete);
}

static void prv_update_icon(Animation *animation, bool finished, void *context) {
  MusicAppData *data = app_state_get_user_data();
  GBitmap *bitmap = context;
  bitmap_layer_set_bitmap(&data->cassette_layer, bitmap);
  data->cassette_layer.layer.bounds.origin.y = 0;
}

static void prv_trigger_cassette_icon_switch(GBitmap *new_bitmap, bool animated) {
  MusicAppData *data = app_state_get_user_data();

  if (!animated) {
    bitmap_layer_set_bitmap(&data->cassette_layer, new_bitmap);
    data->cassette_current_icon = new_bitmap;
    return;
  }
  // Never animate an icon to itself. We can't use the current value of the bitmap layer itself,
  // because that will cause false positives if an icon change is triggered, but a revert
  // is triggered before the first half of the icon animation completes (currently 107 ms).
  if (new_bitmap == data->cassette_current_icon) {
    return;
  }

  GRect cassette_rect = prv_cassette_rect();
  Animation *disappear_animation = property_animation_get_animation(
      property_animation_create_bounds_origin(&data->cassette_layer.layer, &GPointZero,
                                              &GPoint(0, -cassette_rect.size.h)));
  animation_set_duration(disappear_animation, 3 * ANIMATION_FRAME_MS);
  animation_set_curve(disappear_animation, AnimationCurveEaseIn);

  GRect origin = cassette_rect;
  origin.origin.y -= BOUNCEBACK_OFFSET;

  Animation *bounceback_animation = property_animation_get_animation(
      property_animation_create(&s_frame_layer_implementation, &data->cassette_layer.layer,
                                &origin, &cassette_rect));
  animation_set_duration(bounceback_animation, 4 * ANIMATION_FRAME_MS);
  animation_set_curve(bounceback_animation, AnimationCurveEaseOut);

  animation_set_handlers(disappear_animation, (AnimationHandlers) {
      .stopped = prv_update_icon,
  }, new_bitmap);

  Animation *sequence = animation_sequence_create(disappear_animation, bounceback_animation, NULL);

  data->cassette_current_icon = new_bitmap;
  animation_schedule(sequence);
}

static void prv_skipping_click_config_provider(void *data);
static void prv_volume_click_config_provider(void *data);

static void prv_update_cassette_icon(MusicAppData *data, bool animated) {
  if (music_get_playback_state() == MusicPlayStatePaused) {
    prv_trigger_cassette_icon_switch(&data->image_pause, animated);
  } else {
    prv_trigger_cassette_icon_switch(&data->image_cassette, animated);
  }
}

static void prv_update_ui_state_skipping(MusicAppData *data, bool animated) {
  action_bar_layer_set_click_config_provider(&data->action_bar,
                                             prv_skipping_click_config_provider);
  action_bar_layer_set_icon_animated(&data->action_bar, BUTTON_FORWARD,
                                     &data->icon_skip_forward, animated);
  action_bar_layer_set_icon_animated(&data->action_bar, BUTTON_BACKWARD,
                                     &data->icon_skip_backward, animated);
  if (music_get_playback_state() == MusicPlayStatePaused) {
    action_bar_layer_set_icon_animated(&data->action_bar, BUTTON_ID_SELECT, &data->icon_play,
                                       animated);
  } else {
    action_bar_layer_set_icon_animated(&data->action_bar, BUTTON_ID_SELECT, &data->icon_ellipsis,
                                       animated);
  }
}

static void prv_update_ui_state_volume(MusicAppData *data, bool animated) {
  if (data->action_bar_state == ActionBarStateVolume) {
    action_bar_layer_set_click_config_provider(&data->action_bar,
                                               prv_volume_click_config_provider);
  }
  action_bar_layer_set_icon_animated(&data->action_bar, BUTTON_ID_UP, &data->icon_volume_up,
                                     animated);
  action_bar_layer_set_icon_animated(&data->action_bar, BUTTON_ID_DOWN, &data->icon_volume_down,
                                     animated);
  GBitmap const *select_bitmap;
  switch (music_get_playback_state()) {
    case MusicPlayStatePlaying: select_bitmap = &data->icon_pause; break;
    case MusicPlayStatePaused: select_bitmap = &data->icon_play; break;
    default: select_bitmap = &data->icon_play_pause; break;
  }
  action_bar_layer_set_icon_animated(&data->action_bar, BUTTON_ID_SELECT, select_bitmap, animated);
}

static void prv_update_ui_state(MusicAppData *data, bool animated) {
  if (data->action_bar_state == ActionBarStateSkip) {
    prv_update_ui_state_skipping(data, animated);
  } else {
    prv_update_ui_state_volume(data, animated);
  }

  if (music_get_playback_state() != data->current_play_state) {
    data->current_play_state = music_get_playback_state();
    prv_update_cassette_icon(data, animated);
  }
}

static void prv_set_action_bar_state(MusicAppData *data, enum ActionBarState state) {
  data->action_bar_state = state;
  prv_update_ui_state(data, true);
}

static void prv_action_bar_revert(void *context) {
  MusicAppData *data = context;
  data->action_bar_revert_timer = NULL;
  prv_set_action_bar_state(data, ActionBarStateSkip);
}

static void reset_action_bar_revert_timer(MusicAppData *data) {
  app_timer_reschedule(data->action_bar_revert_timer, ACTION_BAR_TIMEOUT_MS);
}

static void prv_skip_click_handler(ClickRecognizerRef recognizer, void *context) {
  // no animations on tintin
  Animation *animation = prv_create_cassette_animation(context);
  animation_schedule(animation);
  if (click_recognizer_get_button_id(recognizer) == BUTTON_BACKWARD) {
    music_command_send(MusicCommandPreviousTrack);
  } else {
    music_command_send(MusicCommandNextTrack);
  }
}

static void prv_volume_click_handler(ClickRecognizerRef recognizer, void *context) {
  reset_action_bar_revert_timer(context);
  // TODO: absolute volume + volume indicator, when that information is available.
  bool is_volume_up = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
  prv_change_volume(is_volume_up);

  // Trigger haptic feedback only on repeat
  if (click_number_of_clicks_counted(recognizer) >= 2) {
    prv_do_haptic_feedback_vibe(context);
  }
}

static void prv_ellipsis_click_handler(ClickRecognizerRef recognizer, void *context) {
  MusicAppData *data = context;
  data->action_bar_revert_timer = app_timer_register(ACTION_BAR_TIMEOUT_MS, prv_action_bar_revert,
                                                     data);
  prv_set_action_bar_state(data, ActionBarStateVolume);
}

static void prv_toggle_playing(void) {
  music_command_send(MusicCommandTogglePlayPause);
}

static void prv_play_pause_click_handler(ClickRecognizerRef recognizer, void *context) {
  reset_action_bar_revert_timer(context);
  prv_toggle_playing();
}

static void prv_handle_volume_repeat(void *context) {
  MusicAppData *data = context;
  if (!data->volume_repeat_timer) {
    return;
  }
  data->volume_repeat_timer = app_timer_register(VOLUME_REPEAT_INTERVAL_MS,
                                                 prv_handle_volume_repeat, data);
  prv_change_volume(data->volume_is_up);
  prv_do_haptic_feedback_vibe(context);
}

static void prv_volume_long_click_start_handler(ClickRecognizerRef recognizer, void *context) {
  const bool volume_is_up = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
  prv_change_volume(volume_is_up);
  prv_set_action_bar_state(context, ActionBarStateLongPress);
  MusicAppData *data = context;
  data->volume_is_up = volume_is_up;
  data->volume_repeat_timer = app_timer_register(VOLUME_REPEAT_INTERVAL_MS,
                                                 prv_handle_volume_repeat, data);
  prv_do_haptic_feedback_vibe(data);
}

static void prv_volume_long_click_end_handler(ClickRecognizerRef recognizer, void *context) {
  MusicAppData *data = context;
  prv_set_action_bar_state(data, ActionBarStateSkip);
  app_timer_cancel(data->volume_repeat_timer);
  data->volume_repeat_timer = NULL;
}

static void prv_play_pause_long_click_start_handler(ClickRecognizerRef recognizer, void *context) {
  prv_toggle_playing();
  prv_set_action_bar_state(context, ActionBarStateLongPress);
  prv_do_haptic_feedback_vibe(context);
}

static void prv_play_pause_long_click_end_handler(ClickRecognizerRef recognizer, void *context) {
  prv_set_action_bar_state(context, ActionBarStateSkip);
}

static void prv_skipping_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_skip_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_skip_click_handler);
  if (music_get_playback_state() == MusicPlayStatePaused) {
    window_single_click_subscribe(BUTTON_ID_SELECT, prv_play_pause_click_handler);
  } else {
    window_single_click_subscribe(BUTTON_ID_SELECT, prv_ellipsis_click_handler);
  }
  window_long_click_subscribe(BUTTON_ID_UP, 0, prv_volume_long_click_start_handler,
                              prv_volume_long_click_end_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, 0, prv_volume_long_click_start_handler,
                              prv_volume_long_click_end_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, prv_play_pause_long_click_start_handler,
                              prv_play_pause_long_click_end_handler);
}

static void prv_volume_click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, VOLUME_REPEAT_INTERVAL_MS,
                                          prv_volume_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, VOLUME_REPEAT_INTERVAL_MS,
                                          prv_volume_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_play_pause_click_handler);
}

static void prv_update_layout(MusicAppData *data) {
  // Hide track position bar if progress reporting not supported
  bool hide_layer = !music_is_progress_reporting_supported();
  layer_set_hidden(&data->track_pos_bar.layer, hide_layer);
  layer_set_hidden(&data->position_text_layer.layer, hide_layer);
  layer_set_hidden(&data->length_text_layer.layer, hide_layer);
}

static void prv_unload_no_music_window(Window *window) {
  MusicNoMusicWindow *music_window = (MusicNoMusicWindow *)window;
  gbitmap_deinit(&music_window->bitmap);
  bitmap_layer_deinit(&music_window->bitmap_layer);
  text_layer_deinit(&music_window->text_layer);
  i18n_free_all(music_window);
}

static void prv_handle_no_music_back(ClickRecognizerRef recognizer, void *context) {
  app_window_stack_pop_all(true);
}

static void prv_no_music_window_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, prv_handle_no_music_back);
}

static MusicNoMusicWindow *prv_create_no_music_window(void) {
  MusicNoMusicWindow *window = app_malloc_check(sizeof(MusicNoMusicWindow));
  window_init(&window->window, WINDOW_NAME("NoMusicWindow"));
  window_set_background_color(&window->window, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
  window_set_window_handlers(&window->window, &(WindowHandlers) {
      .unload = prv_unload_no_music_window
  });

  const MusicAppSizeConfig *config = prv_config();

  gbitmap_init_with_resource(&window->bitmap, RESOURCE_ID_MUSIC_IMAGE_NO_MUSIC);
  const GSize NO_MUSIC_IMAGE_SIZE = window->bitmap.bounds.size;
  const GRect NO_MUSIC_IMAGE_RECT = GRect(config->no_music_img_pos.x,
                                          config->no_music_img_pos.y,
                                          NO_MUSIC_IMAGE_SIZE.w, NO_MUSIC_IMAGE_SIZE.h);
  bitmap_layer_init(&window->bitmap_layer, &NO_MUSIC_IMAGE_RECT);
  bitmap_layer_set_bitmap(&window->bitmap_layer, &window->bitmap);
  bitmap_layer_set_compositing_mode(&window->bitmap_layer, GCompOpSet);

  const GRect NO_MUSIC_TEXT_RECT = GRect(0, config->no_music_text_field.origin_y,
                                         DISP_COLS, config->no_music_text_field.size_h);

  text_layer_init_with_parameters(&window->text_layer,
                                  &NO_MUSIC_TEXT_RECT,
                                  i18n_get("START PLAYBACK\nON YOUR PHONE", window),
                                  fonts_get_system_font(config->no_music_font_key),
                                  GColorBlack, GColorClear, GTextAlignmentCenter,
                                  GTextOverflowModeTrailingEllipsis);
  layer_add_child(&window->window.layer, &window->bitmap_layer.layer);
  layer_add_child(&window->window.layer, &window->text_layer.layer);
  window_set_click_config_provider(&window->window, prv_no_music_window_click_config);
  return window;
}

static void prv_push_no_music_window(MusicAppData *data) {
  if (data->no_music_window) {
    return;
  }
  data->no_music_window = prv_create_no_music_window();
  app_window_stack_push(&data->no_music_window->window, false);
}

static void prv_pop_no_music_window(MusicAppData *data) {
  if (data->no_music_window) {
    app_window_stack_remove(&data->no_music_window->window, true);
    app_free(data->no_music_window);
    data->no_music_window = NULL;
  }
}

static void prv_update_now_playing(MusicAppData *data) {
  layer_set_hidden((Layer *)&data->track_pos_bar, !music_is_progress_reporting_supported());

  char artist_buffer[MUSIC_BUFFER_LENGTH];
  char title_buffer[MUSIC_BUFFER_LENGTH];
  music_get_now_playing(title_buffer, artist_buffer, NULL);

  if (music_needs_user_to_start_playback_on_phone()) {
    prv_push_no_music_window(data);
  } else {
    prv_pop_no_music_window(data);
  }

  bool title_changed = strncmp(data->title_buffer, title_buffer, MUSIC_BUFFER_LENGTH) != 0;
  bool artist_changed = strncmp(data->artist_buffer, artist_buffer, MUSIC_BUFFER_LENGTH) != 0;
  if (title_changed || artist_changed) {
    // Animating nothing looks weird, so don't do that.
    if (data->artist_buffer[0] == 0 && data->title_buffer[0] == 0) {
      strncpy(data->artist_buffer, artist_buffer, MUSIC_BUFFER_LENGTH);
      strncpy(data->title_buffer, title_buffer, MUSIC_BUFFER_LENGTH);
      // It is sufficient to mark one layer as dirty.
      layer_mark_dirty(&data->title_text_layer.layer);
    } else if (title_changed) {
      prv_trigger_track_change_animation(data);
    }
  }
  prv_update_layout(data);
}

static void prv_copy_time_period(char *buffer, size_t n, uint32_t period_s) {
  uint32_t hours = period_s / SECONDS_PER_HOUR;
  uint32_t minutes = (period_s % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
  uint32_t seconds = period_s % SECONDS_PER_MINUTE;
  if (hours > 0) {
    snprintf(buffer, n, "%"PRIu32":%02"PRIu32":%02"PRIu32, hours, minutes, seconds);
  } else {
    snprintf(buffer, n, "%"PRIu32":%02"PRIu32, minutes, seconds);
  }
}

static void prv_update_track_progress(MusicAppData *data) {
  if (data->pause_track_pos_updates) {
    return;
  }
  if (!music_is_progress_reporting_supported()) {
    progress_layer_set_progress(&data->track_pos_bar, 0);
  } else {
    unsigned int percent = MIN((data->track_pos * 100) / data->track_length, 100);
    progress_layer_set_progress(&data->track_pos_bar, percent);
    prv_copy_time_period(data->position_buffer, sizeof(data->position_buffer),
                         data->track_pos / 1000);
    prv_copy_time_period(data->length_buffer, sizeof(data->length_buffer),
                         data->track_length / 1000);
  }
}

static void prv_update_pos(void) {
  MusicAppData *data = app_state_get_user_data();
  music_get_pos(&data->track_pos, &data->track_length);
  prv_update_track_progress(data);
}

static void prv_handle_tick_time(struct tm *time, TimeUnits units_changed) {
  if (music_get_playback_state() == MusicPlayStatePlaying) {
    prv_update_pos();
  }
}

static void prv_set_pos_update_timer(MusicAppData* data, MusicPlayState playstate) {
  if (!music_is_progress_reporting_supported()) {
    return;
  }
  switch (playstate) {
    case MusicPlayStatePlaying:
      // We need to update the progress bar every second.
      tick_timer_service_subscribe(SECOND_UNIT, prv_handle_tick_time);
      break;
    default:
      // We're no longer updating the progress bar; unsubscribe.
      tick_timer_service_unsubscribe();
  }
}

static void prv_configure_music_text_layer(
    TextLayer *text_layer, char* text_buffer, const GRect *rect, int16_t y_offset,
    GTextAlignment align, GFont font) {
  text_layer_init_with_parameters(text_layer, rect, text_buffer, font,
                                  GColorBlack, GColorClear, align, GTextOverflowModeFill);
  layer_set_bounds(&text_layer->layer, &GRect(0, -y_offset,
                                              rect->size.w, rect->size.h + y_offset));
}

static void prv_init_ui(Window *window) {
  MusicAppData *data = window_get_user_data(window);

  window_set_background_color(window, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));

  const GSize WINDOW_SIZE = window->layer.bounds.size;

  const GTextAlignment ARTIST_TITLE_TEXT_ALIGNMENT = PBL_IF_RECT_ELSE(GTextAlignmentLeft,
                                                                      GTextAlignmentRight);

  const MusicAppSizeConfig *config = prv_config();

  const GRect artist_rect = prv_artist_rect();
  const GRect title_rect = prv_title_rect();
  const GRect time_rect = prv_time_rect();
  const GRect cassette_rect = prv_cassette_rect();
  const GRect track_rect = prv_track_rect();

  prv_configure_music_text_layer(&data->artist_text_layer, data->artist_buffer,
                                 &artist_rect, ARTIST_BOUNDS_OFFSET,
                                 ARTIST_TITLE_TEXT_ALIGNMENT,
                                 system_theme_get_font_for_default_size(TextStyleFont_Header));
  layer_add_child(&data->window.layer, &data->artist_text_layer.layer);

  prv_configure_music_text_layer(&data->position_text_layer, data->position_buffer, &time_rect,
                                 TIME_BOUNDS_OFFSET, GTextAlignmentLeft,
                                 fonts_get_system_font(config->music_time_font_key));
  layer_add_child(&data->window.layer, &data->position_text_layer.layer);

  prv_configure_music_text_layer(&data->length_text_layer, data->length_buffer, &time_rect,
                                 TIME_BOUNDS_OFFSET, GTextAlignmentRight,
                                 fonts_get_system_font(config->music_time_font_key));
  layer_add_child(&data->window.layer, &data->length_text_layer.layer);

  prv_configure_music_text_layer(&data->title_text_layer, data->title_buffer, &title_rect,
                                 TITLE_BOUNDS_OFFSET, ARTIST_TITLE_TEXT_ALIGNMENT,
                                 system_theme_get_font_for_default_size(TextStyleFont_Subtitle));
  text_layer_set_line_spacing_delta(&data->title_text_layer, -2);

  layer_add_child(&data->window.layer, &data->title_text_layer.layer);

  const int16_t horizontal_margin = config->horizontal_margin;
  layer_init(&data->cassette_container, &GRect(0, WINDOW_SIZE.h - horizontal_margin - 24,
                                               WINDOW_SIZE.w - ACTION_BAR_WIDTH, 24));
  layer_add_child(&data->window.layer, &data->cassette_container);
  layer_set_clips(&data->cassette_container, false);

  bitmap_layer_init(&data->cassette_layer, &cassette_rect);
  bitmap_layer_set_bitmap(&data->cassette_layer, &data->image_cassette);
  data->cassette_current_icon = &data->image_cassette;
  const GAlign CASSETTE_LAYER_ALIGNMENT = PBL_IF_RECT_ELSE(GAlignTopLeft, GAlignTopRight);
  bitmap_layer_set_alignment(&data->cassette_layer, CASSETTE_LAYER_ALIGNMENT);
  bitmap_layer_set_compositing_mode(&data->cassette_layer, GCompOpSet);
  layer_add_child(&data->cassette_container, &data->cassette_layer.layer);

  progress_layer_init(&data->track_pos_bar, &track_rect);
  progress_layer_set_background_color(&data->track_pos_bar,
                                      PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
  progress_layer_set_foreground_color(&data->track_pos_bar,
                                      PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
  progress_layer_set_corner_radius(&data->track_pos_bar, config->track_corner_radius);
  layer_add_child(&window->layer, (Layer *)&data->track_pos_bar);

  ActionBarLayer *action_bar = &data->action_bar;
  data->action_bar_state = ActionBarStateSkip;
  action_bar_layer_init(action_bar);
  action_bar_layer_set_context(action_bar, data);
  action_bar_layer_add_to_window(action_bar, window);

  StatusBarLayer *status_layer = &data->status_layer;
  status_bar_layer_init(status_layer);
  GRect status_layer_frame = status_layer->layer.frame;
  const int16_t STATUS_BAR_LAYER_WIDTH = PBL_IF_RECT_ELSE(WINDOW_SIZE.w - ACTION_BAR_WIDTH,
                                                          WINDOW_SIZE.w);
  status_layer_frame.size.w = STATUS_BAR_LAYER_WIDTH;
  layer_set_frame(&status_layer->layer, &status_layer_frame);
  status_bar_layer_set_colors(&data->status_layer, GColorClear, GColorBlack);
  layer_add_child(&data->window.layer, &status_layer->layer);

  music_get_pos(&data->track_pos, &data->track_length);

#if CAPABILITY_HAS_VIBE_SCORES
  data->score = vibe_score_create_with_resource(RESOURCE_ID_VIBE_SCORE_HAPTIC_FEEDBACK);
#endif

  prv_update_now_playing(data);
  prv_update_layout(data);
  prv_update_track_progress(data);
  prv_update_ui_state(data, false);
}

static void prv_push_window(MusicAppData *data) {
  Window *window = &data->window;
  window_init(window, WINDOW_NAME("Music"));
  window_set_user_data(window, data);
  window_set_status_bar_icon(window, (GBitmap*)&s_status_icon_music_bitmap);

  const bool animated = true;
  app_window_stack_push(window, animated);
  prv_init_ui(window);
}

static void prv_music_event_handler(PebbleEvent *event, void *context) {
  MusicAppData *data = app_state_get_user_data();
  switch (event->media.type) {
    case PebbleMediaEventTypeNowPlayingChanged:
      prv_update_now_playing(data);
      return;
    case PebbleMediaEventTypePlaybackStateChanged: {
      prv_set_pos_update_timer(data, event->media.playback_state);
      prv_update_ui_state(data, true);
      return;
    }
    case PebbleMediaEventTypeVolumeChanged:
    case PebbleMediaEventTypeServerConnected:
    case PebbleMediaEventTypeServerDisconnected:
    case PebbleMediaEventTypeTrackPosChanged:
      music_get_pos(&data->track_pos, &data->track_length);
      prv_update_track_progress(data);
      prv_update_layout(data);
      return;
    default: return;
  }
}

////////////////////
// App boilerplate

static void prv_handle_init(void) {
  MusicAppData *data = app_malloc_check(sizeof(MusicAppData));
  *data = (MusicAppData){};
  app_state_set_user_data(data);

  data->event_info = (EventServiceInfo){
    .type = PEBBLE_MEDIA_EVENT,
    .handler = prv_music_event_handler,
  };

  // TODO: Once we have some sort of system-wide "needs bluetooth" assertion, invoke that here.

  data->current_play_state = MusicPlayStateInvalid;

  gbitmap_init_with_resource(&data->icon_skip_backward, RESOURCE_ID_MUSIC_ICON_SKIP_BACKWARD);
  gbitmap_init_with_resource(&data->icon_skip_forward, RESOURCE_ID_MUSIC_ICON_SKIP_FORWARD);
  gbitmap_init_with_resource(&data->icon_ellipsis, RESOURCE_ID_MUSIC_ICON_ELLIPSIS);
  gbitmap_init_with_resource(&data->icon_play, RESOURCE_ID_MUSIC_ICON_PLAY);
  gbitmap_init_with_resource(&data->icon_pause, RESOURCE_ID_MUSIC_ICON_PAUSE);
  gbitmap_init_with_resource(&data->icon_play_pause, RESOURCE_ID_MUSIC_ICON_PLAY_PAUSE);
  gbitmap_init_with_resource(&data->icon_volume_up, RESOURCE_ID_MUSIC_ICON_VOLUME_UP);
  gbitmap_init_with_resource(&data->icon_volume_down, RESOURCE_ID_MUSIC_ICON_VOLUME_DOWN);
  gbitmap_init_with_resource(&data->image_cassette, RESOURCE_ID_MUSIC_LARGE_CASSETTE);
  gbitmap_init_with_resource(&data->image_pause, RESOURCE_ID_MUSIC_LARGE_PAUSED);
  gbitmap_init_with_resource(&data->image_volume_up, RESOURCE_ID_MUSIC_LARGE_VOLUME_UP);
  gbitmap_init_with_resource(&data->image_volume_down, RESOURCE_ID_MUSIC_LARGE_VOLUME_DOWN);

  event_service_client_subscribe(&data->event_info);
  prv_push_window(data);

  // Overall reduce the latency at the expense of some power...
  music_request_reduced_latency(true);

  // Give us a super responsive initial period:
  music_request_low_latency_for_period(5000);

  prv_set_pos_update_timer(data, music_get_playback_state());
}

static void prv_handle_deinit(void) {
  music_request_reduced_latency(false);

  MusicAppData *data = app_state_get_user_data();
  i18n_free_all(data);

  // we'll be cleaned up properly by the system
}

static void prv_main(void) {
  prv_handle_init();

  app_event_loop();

  prv_handle_deinit();
}

const PebbleProcessMd* music_app_get_info(void) {
  // [INTL] The app name should come from a standard app resource, so it's localizable.
  static const PebbleProcessMdSystem s_app_info = {
    .common = {
      .main_func = &prv_main,
      // UUID: 1f03293d-47af-4f28-b960-f2b02a6dd757
      .uuid = {0x1f, 0x03, 0x29, 0x3d, 0x47, 0xaf, 0x4f, 0x28,
               0xb9, 0x60, 0xf2, 0xb0, 0x2a, 0x6d, 0xd7, 0x57},
    },
    .name = i18n_noop("Music"),
#if CAPABILITY_HAS_APP_GLANCES
    .icon_resource_id = RESOURCE_ID_AUDIO_CASSETTE_TINY,
#endif
  };
  return (const PebbleProcessMd*) &s_app_info;
}

