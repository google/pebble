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

#include "health_hr_summary_card.h"
#include "health_hr_summary_card_segments.h"
#include "health_hr_detail_card.h"
#include "health_progress.h"
#include "services/normal/activity/health_util.h"

#include "applib/pbl_std/pbl_std.h"
#include "applib/ui/kino/kino_reel.h"
#include "applib/ui/text_layer.h"
#include "apps/system_apps/timeline/text_node.h"
#include "kernel/pbl_malloc.h"
#include "resource/resource_ids.auto.h"
#include "services/common/clock.h"
#include "services/common/i18n/i18n.h"
#include "system/logging.h"
#include "util/size.h"
#include "util/string.h"

typedef struct HealthHrSummaryCardData {
  HealthData *health_data;
  HealthProgressBar progress_bar;
  GDrawCommandSequence *pulsing_heart;
  uint32_t pulsing_heart_frame_index;
  AppTimer *pulsing_heart_timer;
  uint32_t num_heart_beats;
  uint32_t now_bpm;
  uint32_t resting_bpm;
  time_t last_updated;
  GFont bpm_font;
  GFont timestamp_font;
  GFont units_font;
} HealthHrSummaryCardData;

#define PULSING_HEART_TIMEOUT (30 * MS_PER_SECOND)

#define PROGRESS_BACKGROUND_COLOR (PBL_IF_COLOR_ELSE(GColorRoseVale, GColorBlack))
#define PROGRESS_OUTLINE_COLOR (PBL_IF_COLOR_ELSE(GColorClear, GColorBlack))

#define TEXT_COLOR (PBL_IF_COLOR_ELSE(GColorSunsetOrange, GColorBlack))
#define CARD_BACKGROUND_COLOR (PBL_IF_COLOR_ELSE(GColorBulgarianRose, GColorWhite))


static void prv_pulsing_heart_timer_cb(void *context) {
  Layer *base_layer = context;
  HealthHrSummaryCardData *data = layer_get_data(base_layer);

  const uint32_t duration = gdraw_command_sequence_get_total_duration(data->pulsing_heart);
  const uint32_t num_frames = gdraw_command_sequence_get_num_frames(data->pulsing_heart);
  const uint32_t timer_duration = duration / num_frames;
  const uint32_t max_heart_beats = PULSING_HEART_TIMEOUT / duration;

  data->pulsing_heart_frame_index++;

  if (data->pulsing_heart_frame_index >= num_frames) {
    data->pulsing_heart_frame_index = 0;
    data->num_heart_beats++;
  }

  if (data->num_heart_beats < max_heart_beats) {
    data->pulsing_heart_timer = app_timer_register(timer_duration,
                                                   prv_pulsing_heart_timer_cb,
                                                   base_layer);
  }

  layer_mark_dirty(base_layer);
}

static void prv_render_progress_bar(GContext *ctx, Layer *base_layer) {
  HealthHrSummaryCardData *data = layer_get_data(base_layer);

  health_progress_bar_fill(ctx, &data->progress_bar, PROGRESS_BACKGROUND_COLOR,
                           0, HEALTH_PROGRESS_BAR_MAX_VALUE);
}

static void prv_render_icon(GContext *ctx, Layer *base_layer) {
  HealthHrSummaryCardData *data = layer_get_data(base_layer);

  GDrawCommandFrame *frame = gdraw_command_sequence_get_frame_by_index(
      data->pulsing_heart, data->pulsing_heart_frame_index);
  if (frame) {
    const GPoint offset = GPoint(-1, -23);
    gdraw_command_frame_draw(ctx, data->pulsing_heart, frame, offset);
    return;
  }
}

static void prv_render_bpm(GContext *ctx, Layer *base_layer) {
  HealthHrSummaryCardData *data = layer_get_data(base_layer);

  const int units_offset_y =
      fonts_get_font_height(data->bpm_font) - fonts_get_font_height(data->units_font);

  GTextNodeHorizontal *horiz_container = graphics_text_node_create_horizontal(MAX_TEXT_NODES);
  GTextNodeContainer *container = &horiz_container->container;
  horiz_container->horizontal_alignment = GTextAlignmentCenter;

  if (data->now_bpm == 0) {
    health_util_create_text_node_with_text(
        EM_DASH, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD), TEXT_COLOR, container);
  } else {
    const size_t bpm_size = sizeof("000");

    GTextNodeText *number_text_node =
        health_util_create_text_node(bpm_size, data->bpm_font, TEXT_COLOR, container);
    snprintf((char *)number_text_node->text, bpm_size, "%"PRIu32, data->now_bpm);

    GTextNodeText *units_text_node = health_util_create_text_node_with_text(
        i18n_get("BPM", base_layer), data->units_font, TEXT_COLOR, container);
    units_text_node->node.offset.x += 2;
    units_text_node->node.offset.y = units_offset_y;
  }

  const int offset_y = PBL_IF_RECT_ELSE(101, 109);

  graphics_text_node_draw(&container->node, ctx,
      &GRect(0, offset_y, base_layer->bounds.size.w,
             fonts_get_font_height(data->bpm_font)), NULL, NULL);
  graphics_text_node_destroy(&container->node);
}

static void prv_render_timestamp(GContext *ctx, Layer *base_layer) {
  HealthHrSummaryCardData *data = layer_get_data(base_layer);

  if (data->last_updated <= 0 || data->now_bpm == 0) {
    return;
  }

  const size_t buffer_size = 32;
  char buffer[buffer_size];

  clock_get_until_time_without_fulltime(buffer, buffer_size, data->last_updated, HOURS_PER_DAY);

  const int y = PBL_IF_RECT_ELSE(130, 136);
  GRect rect = GRect(0, y, base_layer->bounds.size.w, 35);
#if PBL_RECT
  rect = grect_inset(rect, GEdgeInsets(0, 18));
#endif

  graphics_context_set_text_color(ctx, TEXT_COLOR);
  graphics_draw_text(ctx, buffer, data->timestamp_font,
                     rect, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void prv_render_hrm_disabled(GContext *ctx, Layer *base_layer) {
  HealthHrSummaryCardData *data = layer_get_data(base_layer);

  const int y = PBL_IF_RECT_ELSE(100, 109);

  GRect rect = GRect(0, y, base_layer->bounds.size.w, 52);

  /// HRM disabled
  const char *text = i18n_get("Enable heart rate monitoring in the mobile app", base_layer);

  graphics_context_set_text_color(ctx, TEXT_COLOR);
  graphics_draw_text(ctx, text, data->timestamp_font,
                     rect, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void prv_base_layer_update_proc(Layer *base_layer, GContext *ctx) {
  HealthHrSummaryCardData *data = layer_get_data(base_layer);

  data->now_bpm = health_data_hr_get_current_bpm(data->health_data);
  data->last_updated = health_data_hr_get_last_updated_timestamp(data->health_data);

  prv_render_icon(ctx, base_layer);

  prv_render_progress_bar(ctx, base_layer);

  if (!activity_prefs_heart_rate_is_enabled()) {
    prv_render_hrm_disabled(ctx, base_layer);
    return;
  }

  prv_render_bpm(ctx, base_layer);

  prv_render_timestamp(ctx, base_layer);
}

static void prv_hr_detail_card_unload_callback(Window *window) {
  health_hr_detail_card_destroy(window);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// API Functions
//

Layer *health_hr_summary_card_create(HealthData *health_data) {
  // create base layer
  Layer *base_layer = layer_create_with_data(GRectZero, sizeof(HealthHrSummaryCardData));
  HealthHrSummaryCardData *data = layer_get_data(base_layer);
  layer_set_update_proc(base_layer, prv_base_layer_update_proc);
  // set health data
  *data = (HealthHrSummaryCardData) {
    .health_data = health_data,
    .pulsing_heart =
        gdraw_command_sequence_create_with_resource(RESOURCE_ID_HEALTH_APP_PULSING_HEART),
    .progress_bar = {
      .num_segments = ARRAY_LENGTH(s_hr_summary_progress_segments),
      .segments = s_hr_summary_progress_segments,
    },
    .now_bpm = health_data_hr_get_current_bpm(health_data),
    .resting_bpm = health_data_hr_get_resting_bpm(health_data),
    .last_updated = health_data_hr_get_last_updated_timestamp(health_data),
    .bpm_font = fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM),
    .timestamp_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    .units_font = fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS),
  };

  data->pulsing_heart_timer = app_timer_register(0, prv_pulsing_heart_timer_cb, base_layer);

  return base_layer;
}

void health_hr_summary_card_select_click_handler(Layer *layer) {
  HealthHrSummaryCardData *data = layer_get_data(layer);
  HealthData *health_data = data->health_data;
  Window *window = health_hr_detail_card_create(health_data);
  window_set_window_handlers(window, &(WindowHandlers) {
    .unload = prv_hr_detail_card_unload_callback,
  });
  app_window_stack_push(window, true);
}

void health_hr_summary_card_destroy(Layer *base_layer) {
  HealthHrSummaryCardData *data = layer_get_data(base_layer);
  app_timer_cancel(data->pulsing_heart_timer);
  gdraw_command_sequence_destroy(data->pulsing_heart);
  i18n_free_all(base_layer);
  layer_destroy(base_layer);
}

GColor health_hr_summary_card_get_bg_color(Layer *layer) {
  return CARD_BACKGROUND_COLOR;
}

bool health_hr_summary_show_select_indicator(Layer *layer) {
  return true;
}
