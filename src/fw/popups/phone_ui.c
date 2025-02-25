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

#include "phone_ui.h"
#include "phone_formatting.h"

#include "applib/fonts/fonts.h"
#include "util/trig.h"
#include "applib/ui/action_bar_layer.h"
#include "applib/ui/kino/kino_layer.h"
#include "applib/ui/kino/kino_reel_pdci.h"
#include "applib/ui/kino/kino_reel/morph_square.h"
#include "applib/ui/kino/kino_reel/transform.h"
#include "applib/ui/kino/kino_reel/unfold.h"
#include "applib/ui/ui.h"
#include "applib/ui/window_private.h"
#include "applib/ui/window_stack.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/kernel_ui.h"
#include "kernel/ui/modals/modal_manager.h"
#include "kernel/ui/system_icons.h"
#include "kernel/pbl_malloc.h"
#include "popups/notifications/notifications_presented_list.h"
#include "resource/resource_ids.auto.h"
#include "services/common/analytics/analytics.h"
#include "services/common/i18n/i18n.h"
#include "services/common/evented_timer.h"
#include "services/common/regular_timer.h"
#include "services/common/light.h"
#include "services/normal/blob_db/ios_notif_pref_db.h"
#include "services/normal/notifications/alerts.h"
#include "services/normal/notifications/notification_constants.h"
#include "services/normal/phone_call.h"
#include "services/normal/timeline/timeline.h"
#include "services/normal/timeline/timeline_actions.h"
#include "services/normal/timeline/timeline_resources.h"
#include "shell/system_theme.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/time/time.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if CAPABILITY_HAS_VIBE_SCORES
#include "services/normal/vibes/vibe_client.h"
#include "services/normal/vibes/vibe_score.h"
#endif

#define DECLINE_DELAY_MS 2000
#define SMS_REPLY_DELAY_MS 1200
#define SMS_REPLY_IOS_DELAY_MS 600
#define ACCEPT_DELAY_MS 3000
#define CALL_END_DELAY_MS 5000
#define OUTGOING_CALL_DELAY_MS 5000
#define MISSED_CALL_DELAY_MS 180000

#define NAME_BUFFER_LENGTH 32
#define CALL_STATUS_BUFFER_LENGTH 32

#define DEFAULT_COLOR PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite)
#define ACCEPT_COLOR PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite)
#define DECLINE_COLOR PBL_IF_COLOR_ELSE(GColorRed, GColorWhite)

#define TEXT_MARGIN_WIDTH PBL_IF_RECT_ELSE(5, 10)
#define RIGHTSIDE_PADDING 18
#define TEXT_RIGHTSIDE_PADDING \
  PBL_IF_RECT_ELSE(ACTION_BAR_WIDTH, ACTION_BAR_WIDTH + RIGHTSIDE_PADDING - TEXT_MARGIN_WIDTH)
#define ICON_WIDTH 80
#define ICON_POSITION_X \
  PBL_IF_RECT_ELSE(18, DISP_ROWS - (ACTION_BAR_WIDTH + RIGHTSIDE_PADDING) - ICON_WIDTH)

#if PBL_ROUND
#define ICON_POSITION_CENTERED_X (DISP_ROWS / 2 - ICON_WIDTH / 2)
#endif

static const int16_t DOT_SIZE = 8;
static const uint32_t UNFOLD_DURATION = 300;
static const int16_t UNFOLD_EXPAND = 8;

static const uint32_t ANIMATION_FRAME_MS = 36;

static const uint32_t SQUARE_ANIMATION_FRAMES = 10;
static const uint32_t BOUNCEBACK_ANIMATION_FRAMES = 2;
static const uint32_t COLOUR_ANIMATION_FRAMES = 4;
static const uint32_t DURATION_APPEAR_ANIMATION_FRAMES = 4;
static const uint32_t ACTION_BAR_DISAPPEAR_ANIMATION_FRAMES = 2;

static const int16_t BOUNCEBACK_DISTANCE = 6;
static const int16_t DURATION_ANIMATION_START_OFFSET = 30;

static const int16_t SINGLE_LINE_BOUND_OFFSET = 5;
static const int16_t SINGLE_LINE_BOUND_HEIGHT = 30;
static const int16_t DOUBLE_LINE_BOUND_OFFSET = 0;
static const int16_t DOUBLE_LINE_BOUND_HEIGHT = 40;

//! Enumeration for the various action bar items in the phone ui
typedef enum {
  PhoneCallActions_None = 0,
  PhoneCallActions_Decline = 1 << 0,
  PhoneCallActions_Answer = 1 << 1,
  PhoneCallActions_Reply = 1 << 2,
} PhoneCallActions;

typedef struct {
  TimelineResourceSize icon_size;
  GPoint icon_pos;
  int16_t caller_id_pos_y;
  int16_t caller_id_height;
  int16_t status_pos_y;
  int16_t status_height;
  bool large_caller_id;
} PhoneStyle;

typedef enum {
  ACCEPTED,
  DECLINED,
  DISCONNECTED
} CallStatus;

typedef struct {
  Window window;
#if !PLATFORM_TINTIN
  struct {
    GColor left;
    GColor right;
    int16_t boundary;
  } bg_color;

  Animation *action_bar_animation;
  Animation *bg_color_animation;
  Animation *call_status_animation;
#endif

  ActionBarLayer action_bar;
  Layer core_ui_container;
  TextLayer caller_id_text_layer;
  TextLayer call_status_text_layer;
  StatusBarLayer status_bar;
  KinoLayer icon_layer;
  KinoReel *current_icon;
  ResourceId current_icon_id;
  bool hid_action_bar;

  GBitmap up_bitmap;
  GBitmap select_bitmap;
  GBitmap down_bitmap;
  ClickHandler up_action;
  ClickHandler select_action;
  ClickHandler down_action;

  const PhoneStyle *style;

  GFont name_font;
  GFont long_name_font;
  GFont status_font;

  char caller_id_text_buf[NAME_BUFFER_LENGTH];
  char call_status_text_buf[CALL_STATUS_BUFFER_LENGTH];
  EventedTimerID call_duration_timer;
  EventedTimerID window_pop_timer;
  time_t call_start_time;
#if CAPABILITY_HAS_VIBE_SCORES
  VibeScore *vibe_score;
#endif
  RegularTimerInfo ring_timer;
  bool show_ongoing_call_ui;

  // Incoming call reply data
  TimelineItem *call_response_item;
  bool waiting_for_action_result;
  bool open_reply_menu_on_pop;
  void *action_handle;
} PhoneUIData;

static const PhoneStyle s_phone_style_default = {
  .icon_size = TimelineResourceSizeLarge,
  .icon_pos = { ICON_POSITION_X, PBL_IF_RECT_ELSE(25, 22) },
  .caller_id_pos_y = PBL_IF_RECT_ELSE(102, 93),
  .caller_id_height = 50,
  .status_pos_y = PBL_IF_RECT_ELSE(142, 144),
  .status_height = 20,
};

static const PhoneStyle s_phone_style_large = {
  .icon_size = PBL_IF_RECT_ELSE(TimelineResourceSizeSmall, TimelineResourceSizeLarge),
  .icon_pos = { ICON_POSITION_X, PBL_IF_RECT_ELSE(11, 22) },
  .caller_id_pos_y = PBL_IF_RECT_ELSE(80, 88),
  .caller_id_height = 60,
  .status_pos_y = PBL_IF_RECT_ELSE(138, 144),
  .status_height = 20,
  .large_caller_id = true,
};

static const PhoneStyle *s_phone_styles[NumPreferredContentSizes] = {
  [PreferredContentSizeSmall] = &s_phone_style_default,
  [PreferredContentSizeMedium] = &s_phone_style_default,
  [PreferredContentSizeLarge] = &s_phone_style_large,
  [PreferredContentSizeExtraLarge] = &s_phone_style_large,
};

static PhoneUIData *s_phone_ui_data;
static void prv_window_pop(void);
static void prv_window_pop_with_delay(uint32_t delay_ms);
static void prv_action_bar_setup(PhoneCallActions actions);

static void prv_set_answer_window(void) {
  modal_window_push(&s_phone_ui_data->window, ModalPriorityPhone, false /* don't animate */);
}

static void prv_set_reply_window(void) {
  modal_window_push(&s_phone_ui_data->window, ModalPriorityNotification, false /* don't animate */);
}

//! Icon setters.
// This one will make sure the *previously* set icon resource is destroyed.
// The final icon set must still be destroyed alongside the reel.
static void prv_set_icon_resource(TimelineResourceId timeline_res_id) {
  TimelineResourceInfo timeline_res = {
    .res_id = timeline_res_id,
  };
  AppResourceInfo icon_res_info;
  timeline_resources_get_id(&timeline_res, s_phone_ui_data->style->icon_size, &icon_res_info);
  const ResourceId resource = icon_res_info.res_id;

  // Resetting the same icon shouldn't be animated.
  if (resource == s_phone_ui_data->current_icon_id) {
    return;
  }

  KinoReel *new_image = kino_reel_create_with_resource(resource);
#if !PLATFORM_TINTIN
  KinoReel *old_image = s_phone_ui_data->current_icon;
  kino_layer_pause(&s_phone_ui_data->icon_layer);

  KinoReel *icon_reel = kino_reel_morph_square_create(old_image, true);
  kino_reel_transform_set_to_reel(icon_reel, new_image, false);
  kino_reel_transform_set_transform_duration(icon_reel,
                                             SQUARE_ANIMATION_FRAMES * ANIMATION_FRAME_MS);
  kino_layer_set_reel(&s_phone_ui_data->icon_layer, icon_reel, true);
  kino_layer_play(&s_phone_ui_data->icon_layer);
  s_phone_ui_data->current_icon = new_image;
  s_phone_ui_data->current_icon_id = resource;
#else
  kino_layer_set_reel(&s_phone_ui_data->icon_layer, new_image, true);
  s_phone_ui_data->current_icon = new_image;
  s_phone_ui_data->current_icon_id = resource;
#endif
}

// This will do the wrong thing if called after the action bar is removed, due to the absolute
// coordinate scheme being offset.
static void prv_unfold_icon_resource(TimelineResourceId timeline_res_id) {
  TimelineResourceInfo timeline_res = {
    .res_id = timeline_res_id,
  };
  AppResourceInfo icon_res_info;
  timeline_resources_get_id(&timeline_res, s_phone_ui_data->style->icon_size, &icon_res_info);
  const ResourceId resource = icon_res_info.res_id;

  KinoReel *image = kino_reel_create_with_resource(resource);
#if !PLATFORM_TINTIN
  GRect layer_frame = s_phone_ui_data->icon_layer.layer.frame;
  GSize size = kino_reel_get_size(image);
  GRect icon_from = {
      .origin.x = layer_frame.origin.x + (layer_frame.size.w - DOT_SIZE) / 2,
      .origin.y = layer_frame.origin.y + (layer_frame.size.h - DOT_SIZE) / 2,
      .size = { DOT_SIZE, DOT_SIZE },
  };
  GRect icon_to = {
      .origin.x = layer_frame.origin.x + (layer_frame.size.w - size.w) / 2,
      .origin.y = layer_frame.origin.y + (layer_frame.size.h - size.h) / 2,
      .size = size,
  };
  KinoReel *kino_reel = kino_reel_unfold_create(image, false, layer_frame, 0,
                                                UNFOLD_DEFAULT_NUM_DELAY_GROUPS,
                                                UNFOLD_DEFAULT_GROUP_DELAY);
  kino_reel_transform_set_from_frame(kino_reel, icon_from);
  kino_reel_transform_set_to_frame(kino_reel, icon_to);
  kino_reel_transform_set_transform_duration(kino_reel, UNFOLD_DURATION);
  kino_reel_scale_segmented_set_deflate_effect(kino_reel, UNFOLD_EXPAND);
  kino_layer_set_reel(&s_phone_ui_data->icon_layer, kino_reel, true);
  kino_layer_play(&s_phone_ui_data->icon_layer);
  s_phone_ui_data->current_icon = image;
  s_phone_ui_data->current_icon_id = resource;
#else
  kino_layer_set_reel(&s_phone_ui_data->icon_layer, image, true);
  s_phone_ui_data->current_icon = image;
  s_phone_ui_data->current_icon_id = resource;
#endif
}

#if !PLATFORM_TINTIN
static void prv_update_color_boundary(void *subject, int16_t boundary) {
  s_phone_ui_data->bg_color.boundary = boundary;
  layer_mark_dirty(&s_phone_ui_data->window.layer);
}

static int16_t prv_get_color_boundary(void *subject) {
  return s_phone_ui_data->bg_color.boundary;
}

static const PropertyAnimationImplementation s_color_slide_animation_impl = {
    .base = {
        .update = (AnimationUpdateImplementation)property_animation_update_int16,
    },
    .accessors = {
        .getter = { .int16 = (const Int16Getter)prv_get_color_boundary },
        .setter = { .int16 = (const Int16Setter)prv_update_color_boundary, },
    },
};
#endif

static void prv_set_window_color(GColor color, bool left_to_right) {
#if !PLATFORM_TINTIN
  Animation *color_animation;
  int16_t width = s_phone_ui_data->window.layer.bounds.size.w;
  int16_t zero = 0;

  animation_unschedule(s_phone_ui_data->bg_color_animation);

  // Take whichever side is more complete as our starting colour.
  if (s_phone_ui_data->bg_color.boundary > width / 2) {
    s_phone_ui_data->bg_color.right = s_phone_ui_data->bg_color.left;
  } else {
    s_phone_ui_data->bg_color.left = s_phone_ui_data->bg_color.right;
  }

  if (left_to_right) {
    s_phone_ui_data->bg_color.left = color;
    color_animation = property_animation_get_animation(
        property_animation_create(&s_color_slide_animation_impl, NULL, &zero, &width));
    s_phone_ui_data->bg_color.boundary = 0;
  } else {
    s_phone_ui_data->bg_color.right = color;
    color_animation = property_animation_get_animation(
        property_animation_create(&s_color_slide_animation_impl, NULL, &width, &zero));
    s_phone_ui_data->bg_color.boundary = width;
  }
  s_phone_ui_data->bg_color_animation = color_animation;
  animation_set_duration(color_animation, COLOUR_ANIMATION_FRAMES * ANIMATION_FRAME_MS);
  animation_set_curve(color_animation, AnimationCurveEaseIn);
  animation_schedule(color_animation);
#else
  layer_mark_dirty(&s_phone_ui_data->window.layer);
#endif
}

// Names can sometimes actually be phone numbers. We're assuming that phone numbers will always
// match /^[() +0-9-]+$/
static bool prv_is_string_a_phone_number(const char *name) {
  size_t length = strlen(name);

  // Blocked/unknown numbers on Android start with a '-'
  if ((name[0] == '-') || (length == 0)) {
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    unsigned char chr = name[i];
    if ((chr != '(') && (chr != ')') && (chr != '+') && (chr != ' ') && (chr != '-') && (chr != '.')
        && !isdigit(chr)) {
      return false;
    }
  }
  return true;
}

static bool prv_has_long_name(GFont font) {
  // Figure out if it's a "long name"
  // (i.e. one that won't fit a single line at the default font size).
  const int16_t fudge_some_pixels = 30;
  const bool line_contains_newline = (strchr(s_phone_ui_data->caller_id_text_buf, '\n') != NULL);
  int16_t test_width = s_phone_ui_data->caller_id_text_layer.layer.bounds.size.w
                        + fudge_some_pixels;
  GSize text_size = graphics_text_layout_get_max_used_size(
      kernel_ui_get_graphics_context(),
      s_phone_ui_data->caller_id_text_buf,
      font,
      GRect(0, 0, test_width, SINGLE_LINE_BOUND_HEIGHT),
      s_phone_ui_data->caller_id_text_layer.overflow_mode,
      GTextAlignmentLeft, NULL);
  return (text_size.w > s_phone_ui_data->caller_id_text_layer.layer.bounds.size.w) ||
          line_contains_newline;
}

//! Text setters
static void prv_set_caller_id_text(PebblePhoneCaller *caller) {
  if (!caller->name && !caller->number) {
    return;
  }

  const char *caller_text = caller->name ?: caller->number;
  // Occasionally a name comes in as a number, and vice versa
  const bool is_phone_number = prv_is_string_a_phone_number(caller_text);
  GFont caller_id_font = NULL;
  int lines = 1;
  if (is_phone_number) {
    phone_format_phone_number(caller_text, s_phone_ui_data->caller_id_text_buf, NAME_BUFFER_LENGTH);
    text_layer_set_overflow_mode(&s_phone_ui_data->caller_id_text_layer, GTextOverflowModeWordWrap);
  } else {
    phone_format_caller_name(caller_text, s_phone_ui_data->caller_id_text_buf, NAME_BUFFER_LENGTH);
  }

  if (s_phone_ui_data->style->large_caller_id) {
    caller_id_font = s_phone_ui_data->name_font;
    lines++;
  } else if (prv_has_long_name(s_phone_ui_data->name_font)) {
    caller_id_font = s_phone_ui_data->long_name_font;
    lines++;
  } else {
    caller_id_font = s_phone_ui_data->name_font;
  }

  text_layer_set_font(&s_phone_ui_data->caller_id_text_layer, caller_id_font);
  if (lines == 1) {
    s_phone_ui_data->caller_id_text_layer.layer.bounds.origin.y = SINGLE_LINE_BOUND_OFFSET;
  } else {
    s_phone_ui_data->caller_id_text_layer.layer.bounds.origin.y = DOUBLE_LINE_BOUND_OFFSET;
  }
  s_phone_ui_data->caller_id_text_layer.layer.bounds.size.h =
      lines * fonts_get_font_height(caller_id_font);
  text_layer_set_text(&s_phone_ui_data->caller_id_text_layer, s_phone_ui_data->caller_id_text_buf);
}

//! Window background rendering
static void prv_window_update_proc(Layer *layer, GContext *ctx) {
#if !PLATFORM_TINTIN
  graphics_context_set_fill_color(ctx, s_phone_ui_data->bg_color.left);
  graphics_fill_rect(ctx, &GRect(0, 0, s_phone_ui_data->bg_color.boundary, layer->bounds.size.h));
  graphics_context_set_fill_color(ctx, s_phone_ui_data->bg_color.right);
  graphics_fill_rect(ctx, &GRect(s_phone_ui_data->bg_color.boundary, 0, layer->bounds.size.w,
                                 layer->bounds.size.h));
#else
  graphics_context_set_fill_color(ctx, DEFAULT_COLOR);
  graphics_fill_rect(ctx, &layer->bounds);
#endif
}

//! Ring functionality
static void prv_ring(void *unused) {
  PBL_LOG(LOG_LEVEL_DEBUG, "RING");
  if (alerts_should_vibrate_for_type(AlertPhoneCall)) {
#if CAPABILITY_HAS_VIBE_SCORES
    if (!s_phone_ui_data || !s_phone_ui_data->vibe_score) {
      // There is a mutex-related issue that can appear where the timer callback will execute after
      // phone_ui cancels the timer and frees the vibe_score / s_phone_ui_data. Thus, bail early
      // if we detect this bad state.
      // See PBL-35548
      return;
    }
    vibe_score_do_vibe(s_phone_ui_data->vibe_score);
#else
    vibes_long_pulse();
#endif
  }
  if (alerts_should_enable_backlight_for_type(AlertPhoneCall)) {
    light_enable_interaction();
  }
}

static void prv_start_ringing(void) {
  alerts_incoming_alert_analytics();
  s_phone_ui_data->ring_timer = (const RegularTimerInfo) {
    .cb = prv_ring,
  };
  unsigned int vibe_repeat_interval_sec;
#if CAPABILITY_HAS_VIBE_SCORES
  s_phone_ui_data->vibe_score = vibe_client_get_score(VibeClient_PhoneCalls);
  if (!s_phone_ui_data->vibe_score) {
    return;
  }
  unsigned int vibe_interval_ms = vibe_score_get_duration_ms(s_phone_ui_data->vibe_score) +
      vibe_score_get_repeat_delay_ms(s_phone_ui_data->vibe_score);
  vibe_repeat_interval_sec = DIVIDE_CEIL(vibe_interval_ms, MS_PER_SECOND);
#else
  vibe_repeat_interval_sec = 2;
#endif
  prv_ring(NULL);
  regular_timer_add_multisecond_callback(&s_phone_ui_data->ring_timer, vibe_repeat_interval_sec);
}

static void prv_stop_ringing(void) {
  regular_timer_remove_callback(&s_phone_ui_data->ring_timer);
#if CAPABILITY_HAS_VIBE_SCORES
  if (s_phone_ui_data->vibe_score) {
    vibe_score_destroy(s_phone_ui_data->vibe_score);
    s_phone_ui_data->vibe_score = NULL;
  }
#endif
  vibes_cancel();
}


//! Call duration related functions
static void prv_show_call_status(void) {
  layer_set_hidden(&s_phone_ui_data->call_status_text_layer.layer, false);
#if !PLATFORM_TINTIN
  s_phone_ui_data->call_status_text_layer.layer.bounds.origin.y = DURATION_ANIMATION_START_OFFSET;
  Animation *upward = property_animation_get_animation(
      property_animation_create_bounds_origin(&s_phone_ui_data->call_status_text_layer.layer,
                                              &GPoint(0, DURATION_ANIMATION_START_OFFSET),
                                              &GPoint(0, -BOUNCEBACK_DISTANCE)));
  animation_set_curve(upward, AnimationCurveEaseIn);
  animation_set_duration(upward, DURATION_APPEAR_ANIMATION_FRAMES * ANIMATION_FRAME_MS);

  Animation *bounceback = property_animation_get_animation(
      property_animation_create_bounds_origin(&s_phone_ui_data->call_status_text_layer.layer,
                                              &GPoint(0, -BOUNCEBACK_DISTANCE),
                                              &GPointZero));
  animation_set_curve(bounceback, AnimationCurveEaseOut);
  animation_set_duration(bounceback, BOUNCEBACK_ANIMATION_FRAMES * ANIMATION_FRAME_MS);

  Animation *animation = animation_sequence_create(upward, bounceback, NULL);
  s_phone_ui_data->call_status_animation = animation;
  animation_schedule(animation);
#else
  s_phone_ui_data->call_status_text_layer.layer.bounds.origin = GPointZero;
#endif
}

static void prv_update_call_time(void *unused) {
  if (s_phone_ui_data == NULL) {
    return;
  }

  if (layer_get_hidden(&s_phone_ui_data->call_status_text_layer.layer)) {
    prv_show_call_status();
  }
  const time_t duration = rtc_get_time() - s_phone_ui_data->call_start_time;
  const int seconds = duration % SECONDS_PER_MINUTE;
  int minutes = (duration - seconds) / SECONDS_PER_MINUTE;
  if (minutes >= MINUTES_PER_HOUR) {
    const int hours = minutes / MINUTES_PER_HOUR;
    minutes = minutes % MINUTES_PER_HOUR;
    sniprintf(s_phone_ui_data->call_status_text_buf, CALL_STATUS_BUFFER_LENGTH,
        "%u:%02u:%02u", hours, minutes, seconds);
  } else {
    sniprintf(s_phone_ui_data->call_status_text_buf, CALL_STATUS_BUFFER_LENGTH,
        "%u:%02u", minutes, seconds);
  }
  text_layer_set_text(&s_phone_ui_data->call_status_text_layer,
                      s_phone_ui_data->call_status_text_buf);
}

static void prv_start_call_duration_timer(void) {
  if (s_phone_ui_data->call_start_time == 0) {
    s_phone_ui_data->call_start_time = rtc_get_time();
  }

  s_phone_ui_data->call_duration_timer = evented_timer_register(
      1000, true /* repeating */, prv_update_call_time, NULL);

  // Update call time immediately
  prv_update_call_time(NULL);
}

static void prv_stop_call_duration_timer(void) {
  evented_timer_cancel(s_phone_ui_data->call_duration_timer);
  s_phone_ui_data->call_duration_timer = EVENTED_TIMER_INVALID_ID;
}

static void prv_set_status_text(CallStatus status) {
  if (status == ACCEPTED) {
    i18n_get_with_buffer("Call Accepted", s_phone_ui_data->call_status_text_buf,
                         CALL_STATUS_BUFFER_LENGTH);
  } else if (status == DISCONNECTED) {
    i18n_get_with_buffer("Disconnected", s_phone_ui_data->call_status_text_buf,
                         CALL_STATUS_BUFFER_LENGTH);
  } else {
    if (s_phone_ui_data->call_start_time) {
      i18n_get_with_buffer("Call Ended", s_phone_ui_data->call_status_text_buf,
                           CALL_STATUS_BUFFER_LENGTH);
    } else {
      i18n_get_with_buffer("Call Declined", s_phone_ui_data->call_status_text_buf,
                           CALL_STATUS_BUFFER_LENGTH);
    }
  }

  text_layer_set_text(&s_phone_ui_data->call_status_text_layer,
                      s_phone_ui_data->call_status_text_buf);
  prv_show_call_status();
}

// Handles cleanup when the SMS reply menu closes
static void prv_action_menu_did_close(ActionMenu *action_menu, const ActionMenuItem *item,
                                      void *context) {
  timeline_item_destroy(context);
}

static void prv_ancs_response_action_result_handler(bool success, void *timeline_item) {
  timeline_item_destroy(timeline_item);

  // We got the action result for our iOS reply. We can now close the phone ui window because we
  // are displaying the action menu (but only if the original window hasn't already been torn down)
  if (s_phone_ui_data && s_phone_ui_data->waiting_for_action_result) {
    prv_window_pop();
  }
}

// Creates a new reply action menu and pushes it with notification modal priority
static void prv_open_reply_action_menu(void *unused) {
  // Drop the call window priority so we properly animate in the menu
  prv_set_reply_window();

  // The timeline item will be cleaned up by the action menu/action callbacks
  TimelineItem *item = s_phone_ui_data->call_response_item;
  s_phone_ui_data->call_response_item = NULL;

  TimelineItemAction *reply_action = timeline_item_find_reply_action(item);

  if (!reply_action) {
    return;
  }

  switch (reply_action->type) {
    case TimelineItemActionTypeResponse:
      timeline_actions_push_response_menu(item, reply_action, SMS_REPLY_COLOR,
                                          prv_action_menu_did_close,
                                          modal_manager_get_window_stack(ModalPriorityNotification),
                                          TimelineItemActionSourcePhoneUi,
                                          true /* standalone_reply */);
      break;
    case TimelineItemActionTypeAncsResponse:
      // Mark this window so we know to pop it when we get a response
      s_phone_ui_data->waiting_for_action_result = true;

      // Kick off the reply action automatically - we will pop the phone ui once we get an action
      // result and can show the action menu
      timeline_actions_invoke_action(reply_action, item, prv_ancs_response_action_result_handler,
                                     item);
      break;
    default:
      break;
  }
}

//! Action bar click handlers
static void prv_answer_click_handler(ClickRecognizerRef recognizer, void *unused) {
  prv_stop_ringing();
  phone_call_answer();

  // This must be called before prv_set_status_text, otherwise the text will not be centered
  prv_action_bar_setup(PhoneCallActions_None);
  prv_set_window_color(ACCEPT_COLOR, false);
  prv_set_icon_resource(TIMELINE_RESOURCE_DURING_PHONE_CALL);

  if (s_phone_ui_data->show_ongoing_call_ui) {
    prv_start_call_duration_timer();
  } else {
    prv_set_status_text(ACCEPTED);
    prv_window_pop_with_delay(ACCEPT_DELAY_MS);
  }

  prv_set_answer_window();
}

static void prv_decline_call(void) {
  prv_stop_ringing();
  phone_call_decline();

  prv_stop_call_duration_timer();
  prv_set_icon_resource(TIMELINE_RESOURCE_DISMISSED_PHONE_CALL);
  prv_set_window_color(DECLINE_COLOR, true);

  // This must be called before prv_set_status_text, otherwise the text will not be centered
  prv_action_bar_setup(PhoneCallActions_None);
  prv_set_status_text(DECLINED);
}

static void prv_decline_click_handler(ClickRecognizerRef recognizer, void *unused) {
  prv_decline_call();
  prv_window_pop_with_delay(DECLINE_DELAY_MS);
}

static void prv_sms_reply_click_handler(ClickRecognizerRef recognizer, void *unused) {
  prv_decline_call();
  s_phone_ui_data->open_reply_menu_on_pop = true;

  const TimelineItemAction *reply_action =
      timeline_item_find_reply_action(s_phone_ui_data->call_response_item);

  switch (reply_action->type) {
    case TimelineItemActionTypeResponse:
      // On Android, we just open the action menu at the same time we pop the window
      prv_window_pop_with_delay(SMS_REPLY_DELAY_MS);
      break;
    case TimelineItemActionTypeAncsResponse:
      // On iOS, show the "Call Declined" animation and send the AncsResponse message shortly after
      // We hold the phone UI up until timeline_actions responds or another call comes in
      s_phone_ui_data->window_pop_timer = evented_timer_register(SMS_REPLY_IOS_DELAY_MS,
          false /* repeating */, prv_open_reply_action_menu, NULL);
      break;
    default:
      break;
  }
}

static void prv_pop_click_handler(ClickRecognizerRef recognizer, void *unused) {
  analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_POP_COUNT, AnalyticsClient_System);
  prv_stop_ringing();
  prv_window_pop();
}

//! Action bar animation
static void prv_hide_action_bar(void) {
  if (s_phone_ui_data->hid_action_bar) {
    return;
  }
  s_phone_ui_data->hid_action_bar = true;

#if !PLATFORM_TINTIN
  const GRect window_bounds = s_phone_ui_data->window.layer.bounds;
  GRect offscreen = GRect(window_bounds.size.w, 0, PBL_IF_RECT_ELSE(ACTION_BAR_WIDTH, 0),
                          window_bounds.size.h);
  Animation *action_bar_animation = property_animation_get_animation(
      property_animation_create_layer_frame(&s_phone_ui_data->action_bar.layer, NULL, &offscreen));
  animation_set_duration(action_bar_animation, ACTION_BAR_DISAPPEAR_ANIMATION_FRAMES
                                               * ANIMATION_FRAME_MS);
  animation_set_curve(action_bar_animation, AnimationCurveEaseIn);
  GPoint overshoot = GPoint(PBL_IF_RECT_ELSE(ACTION_BAR_WIDTH / 2, 0) + BOUNCEBACK_DISTANCE, 0);
  Animation *ui_movement = property_animation_get_animation(
      property_animation_create_bounds_origin(&s_phone_ui_data->core_ui_container, NULL,
                                              &overshoot));
  animation_set_curve(ui_movement, AnimationCurveEaseIn);
  animation_set_duration(ui_movement, 3 * ANIMATION_FRAME_MS);
  Animation *ui_bounceback = property_animation_get_animation(
      property_animation_create_bounds_origin(
        &s_phone_ui_data->core_ui_container, &overshoot,
        &GPoint(PBL_IF_RECT_ELSE(ACTION_BAR_WIDTH / 2, 0), 0)));
  animation_set_curve(ui_bounceback, AnimationCurveEaseOut);
  animation_set_duration(ui_bounceback, 2 * ANIMATION_FRAME_MS);
  Animation *ui_animation = animation_sequence_create(ui_movement, ui_bounceback, NULL);
  Animation *combined = animation_spawn_create(action_bar_animation, ui_animation, NULL);
  s_phone_ui_data->action_bar_animation = combined;
  animation_schedule(combined);
#if PBL_ROUND
  // Extend the bounds to center the call text when the action bar is removed
  s_phone_ui_data->caller_id_text_layer.layer.bounds.size.w += TEXT_RIGHTSIDE_PADDING;
  text_layer_set_text_alignment(&s_phone_ui_data->caller_id_text_layer, GTextAlignmentCenter);
  s_phone_ui_data->call_status_text_layer.layer.bounds.size.w += TEXT_RIGHTSIDE_PADDING;
  text_layer_set_text_alignment(&s_phone_ui_data->call_status_text_layer, GTextAlignmentCenter);
  // Center the kino icon
  s_phone_ui_data->icon_layer.layer.frame.origin.x = ICON_POSITION_CENTERED_X;
#endif

#else
  const GRect container_bounds = s_phone_ui_data->core_ui_container.bounds;
  const GRect onscreen = GRect(ACTION_BAR_WIDTH / 2, 0,
                               container_bounds.size.w, container_bounds.size.h);
  layer_set_hidden(&s_phone_ui_data->action_bar.layer, true /* hide */);
  layer_set_bounds(&s_phone_ui_data->core_ui_container, &onscreen);
#endif
}

//! Action bar setup functions
static void prv_set_action_bar_icon(ButtonId button, ResourceId resource, GBitmap *storage) {
  if (resource == RESOURCE_ID_INVALID) {
    action_bar_layer_clear_icon(&s_phone_ui_data->action_bar, button);
    return;
  }

  gbitmap_deinit(storage);
  gbitmap_init_with_resource_system(storage, SYSTEM_APP, resource);
  action_bar_layer_set_icon(&s_phone_ui_data->action_bar, button, storage);
}

// Returns the appropriate app id for the given phone number and call source
static const char *prv_get_app_id(const char *number, PhoneCallSource source) {
  if (!number) {
    return NULL;
  }

  // Select appropriate app id
  switch (source) {
    case PhoneCallSource_PP:
      // We require the this to be a valid number when coming from PP
      if (prv_is_string_a_phone_number(number)) {
        return ANDROID_PHONE_KEY;
      }
      break;
    case PhoneCallSource_ANCS:
    case PhoneCallSource_ANCS_Legacy:
      return IOS_PHONE_KEY;
      break;
  }

  return NULL;
}

// Checks for the existence of a call reply action in the notif pref db and loads it into
// a timeline item
static bool prv_load_sms_reply_action(const char *number, PhoneCallSource source) {
  const char *app_id = prv_get_app_id(number, source);

  if (!app_id) {
    return false;
  }

  // Load actions from prefs db and determine if we have an SMS reply option
  iOSNotifPrefs *notif_prefs = ios_notif_pref_db_get_prefs((uint8_t *)app_id, strlen(app_id));
  if (!notif_prefs) {
    return false;
  }

  // Add attributes to the timeline item for contact lookup
  AttributeList attributes = {};

  attribute_list_add_cstring(&attributes, AttributeIdSender, number);
  attribute_list_add_cstring(&attributes, AttributeIdiOSAppIdentifier, app_id);

  TimelineItem *item = timeline_item_create_with_attributes(0, 0, TimelineItemTypeNotification,
                                                            LayoutIdUnknown,
                                                            &attributes,
                                                            &notif_prefs->action_group);
  bool rv = false;

  // Make sure we have a reply action (this properly handles NULL items)
  const TimelineItemAction *reply_action = timeline_item_find_reply_action(item);
  if (reply_action) {
    s_phone_ui_data->call_response_item = item;
    rv = true;

    if (reply_action->type == TimelineItemActionTypeResponse) {
      item->header.id = (Uuid)UUID_SEND_SMS;
    }
  } else {
    timeline_item_destroy(item);
  }

  attribute_list_destroy_list(&attributes);
  ios_notif_pref_db_free_prefs(notif_prefs);

  return rv;
}

//! Action bar click configurations
static void prv_click_config_provider(void *context) {
  if (s_phone_ui_data->up_action) {
    window_single_click_subscribe(BUTTON_ID_UP, s_phone_ui_data->up_action);
  }

  if (s_phone_ui_data->select_action) {
    window_single_click_subscribe(BUTTON_ID_SELECT, s_phone_ui_data->select_action);
  }

  if (s_phone_ui_data->down_action) {
    window_single_click_subscribe(BUTTON_ID_DOWN, s_phone_ui_data->down_action);
  }

  window_single_click_subscribe(BUTTON_ID_BACK, prv_pop_click_handler);
}

static void prv_action_bar_setup(PhoneCallActions actions) {
  s_phone_ui_data->up_action = NULL;
  s_phone_ui_data->select_action = NULL;
  s_phone_ui_data->down_action = NULL;

  ResourceId up_icon = RESOURCE_ID_INVALID;
  ResourceId select_icon = RESOURCE_ID_INVALID;
  ResourceId down_icon = RESOURCE_ID_INVALID;

  if (actions) {
    if (actions & PhoneCallActions_Answer) {
      s_phone_ui_data->up_action = prv_answer_click_handler;
      up_icon = RESOURCE_ID_ACTION_BAR_ICON_CHECK;
    }

    if (actions & PhoneCallActions_Reply) {
      // Move to top if that place isn't taken
      if (!s_phone_ui_data->up_action) {
        s_phone_ui_data->up_action = prv_sms_reply_click_handler;
        up_icon = RESOURCE_ID_ACTION_BAR_ICON_SMS;
      } else {
        s_phone_ui_data->select_action = prv_sms_reply_click_handler;
        select_icon = RESOURCE_ID_ACTION_BAR_ICON_SMS;
      }
    }

    if (actions & PhoneCallActions_Decline) {
      s_phone_ui_data->down_action = prv_decline_click_handler;
      down_icon = RESOURCE_ID_ACTION_BAR_ICON_X;
    }

    prv_set_action_bar_icon(BUTTON_ID_UP, up_icon, &s_phone_ui_data->up_bitmap);
    prv_set_action_bar_icon(BUTTON_ID_SELECT, select_icon, &s_phone_ui_data->select_bitmap);
    prv_set_action_bar_icon(BUTTON_ID_DOWN, down_icon, &s_phone_ui_data->down_bitmap);
  } else {
    prv_hide_action_bar();
  }

  action_bar_layer_set_click_config_provider(&s_phone_ui_data->action_bar,
                                             prv_click_config_provider);
}

//! Put the correct data in the 3 text fields
static void prv_display_caller_info(PebblePhoneCaller *caller) {
  prv_set_caller_id_text(caller);
}

static void prv_phone_ui_deinit(void) {
  if (s_phone_ui_data == NULL) {
    return;
  }

  kino_layer_pause(&s_phone_ui_data->icon_layer);
  kino_layer_deinit(&s_phone_ui_data->icon_layer);
#if !PLATFORM_TINTIN
  // The reels will destroy intermediate images, but not the one currently on screen
  // clean it up here.  Note that we don't have to do this on Tintin/Bianca as we
  // do not create an intermediary reel for animating.
  kino_reel_destroy(s_phone_ui_data->current_icon);

  animation_unschedule(s_phone_ui_data->bg_color_animation);
  animation_unschedule(s_phone_ui_data->action_bar_animation);
  animation_unschedule(s_phone_ui_data->call_status_animation);
#endif
  s_phone_ui_data->current_icon = NULL;
  s_phone_ui_data->current_icon_id = 0;

  status_bar_layer_deinit(&s_phone_ui_data->status_bar);
  gbitmap_deinit(&s_phone_ui_data->up_bitmap);
  gbitmap_deinit(&s_phone_ui_data->select_bitmap);
  gbitmap_deinit(&s_phone_ui_data->down_bitmap);

  text_layer_deinit(&s_phone_ui_data->call_status_text_layer);
  text_layer_deinit(&s_phone_ui_data->caller_id_text_layer);

  evented_timer_cancel(s_phone_ui_data->call_duration_timer);
  evented_timer_cancel(s_phone_ui_data->window_pop_timer);

  action_bar_layer_deinit(&s_phone_ui_data->action_bar);

  i18n_free_all(s_phone_ui_data);

  prv_stop_ringing();

  window_deinit(&s_phone_ui_data->window);

  timeline_item_destroy(s_phone_ui_data->call_response_item);

  kernel_free(s_phone_ui_data);

  s_phone_ui_data = NULL;
}

static void prv_handle_window_unload(Window *window) {
  prv_phone_ui_deinit();
}

//! Window destroy functions
//! Currently only 1 call window can exist at a time
static void prv_window_pop(void) {
  if (s_phone_ui_data == NULL) {
    // Check to make sure we didn't get popped already.
    // There could possibly be 2 of these callback in the queue at time if this is called right after
    // a prv_pop_with_delay
    return;
  }

  if (s_phone_ui_data->open_reply_menu_on_pop) {
    prv_open_reply_action_menu(NULL);
  }

  window_stack_remove(&s_phone_ui_data->window, true /* animated */);

  // The window_stack_remove() call should run the unload handler (which deinits the ui),
  // but in the rare case that the window never loaded (i.e. a higher priority modal was up)
  // then we could leak the phone_ui data and assert on the next phone call.
  // Deinit again to cover this case (will be a no-op) if the window was already deinited.
  prv_phone_ui_deinit();
}

static void prv_window_pop_cb(void *unused) {
  s_phone_ui_data->window_pop_timer = EVENTED_TIMER_INVALID_ID;
  prv_window_pop();
}

static void prv_window_pop_with_delay(uint32_t delay_ms) {
  s_phone_ui_data->window_pop_timer = evented_timer_register(
      delay_ms, false /* repeating */, prv_window_pop_cb, NULL);
}

//! Window setup
//! Currently only 1 call window can exist at a time
static void prv_phone_ui_init(void) {
  PBL_ASSERTN(s_phone_ui_data == NULL);

  s_phone_ui_data = kernel_zalloc_check(sizeof(PhoneUIData));
  s_phone_ui_data->hid_action_bar = false;

  s_phone_ui_data->style = s_phone_styles[system_theme_get_content_size()];

  const PhoneStyle *style = s_phone_ui_data->style;
  s_phone_ui_data->name_font = system_theme_get_font(TextStyleFont_Title);
  s_phone_ui_data->long_name_font =
      system_theme_get_font(PBL_IF_RECT_ELSE(TextStyleFont_Header, TextStyleFont_Title));
  s_phone_ui_data->status_font =
      PBL_IF_RECT_ELSE(system_theme_get_font(TextStyleFont_Header),
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));

  Window *window = &s_phone_ui_data->window;

  window_init(window, WINDOW_NAME("Phone"));
  window_set_status_bar_icon(window, (GBitmap*)&s_status_icon_phone_bitmap);
  layer_set_update_proc(&window->layer, prv_window_update_proc);
  window_set_window_handlers(&s_phone_ui_data->window, &(WindowHandlers) {
    .unload = prv_handle_window_unload,
  });
  window_set_overrides_back_button(window, true);
#if !PLATFORM_TINTIN
  s_phone_ui_data->bg_color.left = DEFAULT_COLOR;
  s_phone_ui_data->bg_color.right = DEFAULT_COLOR;
  s_phone_ui_data->bg_color.boundary = 0;
#endif

  const int16_t width = window->layer.bounds.size.w - (TEXT_MARGIN_WIDTH * 2);

  // Container layer
  layer_init(&s_phone_ui_data->core_ui_container, &window->layer.bounds);
  layer_add_child(&window->layer, &s_phone_ui_data->core_ui_container);

  // Status bar
  status_bar_layer_init(&s_phone_ui_data->status_bar);
  layer_set_frame(&s_phone_ui_data->status_bar.layer,
                  &GRect(0, 0, window->layer.bounds.size.w - PBL_IF_RECT_ELSE(ACTION_BAR_WIDTH, 0),
                         STATUS_BAR_LAYER_HEIGHT));
  status_bar_layer_set_colors(&s_phone_ui_data->status_bar,
                              PBL_IF_COLOR_ELSE(GColorClear, GColorWhite),
                              GColorBlack);
  layer_add_child(&s_phone_ui_data->core_ui_container, &s_phone_ui_data->status_bar.layer);

  // Icon
  kino_layer_init(&s_phone_ui_data->icon_layer,
                  &(GRect){ style->icon_pos, { ICON_WIDTH, ICON_WIDTH } });
  kino_layer_set_alignment(&s_phone_ui_data->icon_layer, GAlignCenter);
  layer_add_child(&s_phone_ui_data->core_ui_container, &s_phone_ui_data->icon_layer.layer);

  // Caller ID text
  const GRect caller_id_text_rect = GRect(TEXT_MARGIN_WIDTH, style->caller_id_pos_y,
                                          width, style->caller_id_height);
  text_layer_init_with_parameters(&s_phone_ui_data->caller_id_text_layer,
                                  &caller_id_text_rect, NULL, NULL,
                                  GColorBlack,
                                  PBL_IF_COLOR_ELSE(GColorClear, GColorWhite),
                                  PBL_IF_RECT_ELSE(GTextAlignmentCenter, GTextAlignmentRight),
                                  GTextOverflowModeTrailingEllipsis);
  layer_add_child(&s_phone_ui_data->core_ui_container,
                  &s_phone_ui_data->caller_id_text_layer.layer);
  // Shrink the bounds but not the frame size to allow for centering when action bar removed
  s_phone_ui_data->caller_id_text_layer.layer.bounds.size.w = width - TEXT_RIGHTSIDE_PADDING;

  // Status text
  const GRect call_status_text_rect = GRect(TEXT_MARGIN_WIDTH, style->status_pos_y,
                                            width, style->status_height);
  text_layer_init_with_parameters(&s_phone_ui_data->call_status_text_layer,
                                  &call_status_text_rect, NULL, s_phone_ui_data->status_font,
                                  GColorBlack,
                                  PBL_IF_COLOR_ELSE(GColorClear, GColorWhite),
                                  PBL_IF_RECT_ELSE(GTextAlignmentCenter, GTextAlignmentRight),
                                  GTextOverflowModeTrailingEllipsis);
  layer_set_hidden(&s_phone_ui_data->call_status_text_layer.layer, true);
  layer_set_clips(&s_phone_ui_data->call_status_text_layer.layer, false);
  layer_add_child(&s_phone_ui_data->core_ui_container,
                  &s_phone_ui_data->call_status_text_layer.layer);
  // Shrink the bounds but not the frame size to allow for centering when action bar removed
  s_phone_ui_data->call_status_text_layer.layer.bounds.size.w = width - TEXT_RIGHTSIDE_PADDING;

  // Action bar
  action_bar_layer_init(&s_phone_ui_data->action_bar);
  action_bar_layer_add_to_window(&s_phone_ui_data->action_bar, window);

  modal_window_push(window, ModalPriorityCritical, true /* animated */);
}

static bool prv_check_popups_are_blocked(void) {
  if (launcher_popups_are_blocked()) {
    PBL_LOG(LOG_LEVEL_INFO, "Ignoring call event. Popups are blocked");
    return true;
  }
  return false;
}

//!
//! API for updating / creating the phone UI
//!
void phone_ui_handle_incoming_call(PebblePhoneCaller *caller, bool can_answer,
                                   bool show_ongoing_call_ui, PhoneCallSource source) {
  if (prv_check_popups_are_blocked()) {
    return;
  }

  if (s_phone_ui_data) {
    // In this case we are waiting to pop the window and a new event has come in.
    // Pop it immediately and then set up for the new event
    prv_window_pop();
  }

  prv_phone_ui_init();
  s_phone_ui_data->show_ongoing_call_ui = show_ongoing_call_ui;

  prv_unfold_icon_resource(TIMELINE_RESOURCE_INCOMING_PHONE_CALL);

  bool can_reply = false;
  if (caller) {
    prv_display_caller_info(caller);

    // Check if we support sms reply
    can_reply = prv_load_sms_reply_action(caller->number, source);
  }

  uint8_t actions = PhoneCallActions_Decline;
  if (can_reply) {
    actions |= PhoneCallActions_Reply;
  }
  if (can_answer) {
    actions |= PhoneCallActions_Answer;
  }
  prv_action_bar_setup(actions);

  prv_start_ringing();
}

void phone_ui_handle_outgoing_call(PebblePhoneCaller *caller) {
  if (s_phone_ui_data) {
    // In this case we are waiting to pop the window and a new event has come in.
    // Pop it immediately and then set up for the new event
    prv_window_pop();
  }

  prv_phone_ui_init();

  // FIXME: PBL-21570 Outgoing call small is missing
  prv_unfold_icon_resource(TIMELINE_RESOURCE_INCOMING_PHONE_CALL);

  if (caller) {
    prv_display_caller_info(caller);
  }

  prv_action_bar_setup(PhoneCallActions_None);

  prv_window_pop_with_delay(OUTGOING_CALL_DELAY_MS);
}

void phone_ui_handle_missed_call(void) {
  if (!s_phone_ui_data) {
    return;
  }

  prv_stop_ringing();

  prv_set_icon_resource(TIMELINE_RESOURCE_DISMISSED_PHONE_CALL);

  prv_action_bar_setup(PhoneCallActions_None);

  prv_window_pop_with_delay(MISSED_CALL_DELAY_MS);
}

void phone_ui_handle_call_start(bool can_decline) {
  if (!s_phone_ui_data) {
    PBL_LOG(LOG_LEVEL_ERROR, "Can't handle call start, UI isn't setup");
    return;
  }

  prv_stop_ringing();

#if PBL_RECT
  prv_set_icon_resource(TIMELINE_RESOURCE_DURING_PHONE_CALL);
#else
  // action bar requires right-aligned icon, otherwise centered icon
  prv_set_icon_resource((can_decline) ? TIMELINE_RESOURCE_DURING_PHONE_CALL :
                        TIMELINE_RESOURCE_DURING_PHONE_CALL_CENTERED);
#endif

  prv_set_window_color(ACCEPT_COLOR, false);

  prv_action_bar_setup(can_decline ? PhoneCallActions_Decline : PhoneCallActions_None);

  prv_start_call_duration_timer();
  prv_set_answer_window();
}

void phone_ui_handle_call_end(bool call_accepted, bool disconnected) {
  if (!s_phone_ui_data) {
    PBL_LOG(LOG_LEVEL_ERROR, "Can't handle call end, UI isn't setup");
    return;
  }

  prv_stop_ringing();

  prv_stop_call_duration_timer();

  // This must be called before prv_set_status_text, otherwise the text will not be centered
  prv_action_bar_setup(PhoneCallActions_None);

  if (call_accepted) {
    prv_set_icon_resource(TIMELINE_RESOURCE_DURING_PHONE_CALL);
    prv_set_window_color(ACCEPT_COLOR, true);
    prv_set_status_text(ACCEPTED);
  } else {
    prv_set_icon_resource(TIMELINE_RESOURCE_DISMISSED_PHONE_CALL);
    prv_set_window_color(DECLINE_COLOR, true);
    if (disconnected) {
      prv_set_status_text(DISCONNECTED);
    } else {
      prv_set_status_text(DECLINED);
    }
  }

  prv_window_pop_with_delay(CALL_END_DELAY_MS);
}

void phone_ui_handle_call_hide(void) {
  // Just pop the window - it'll handle all the cleanup
  prv_window_pop();
}

void phone_ui_handle_caller_id(PebblePhoneCaller *caller) {
  if (!s_phone_ui_data) {
    PBL_LOG(LOG_LEVEL_ERROR, "Can't update caller id, UI isn't setup");
    return;
  }

  if (caller) {
    prv_display_caller_info(caller);
  }
}
