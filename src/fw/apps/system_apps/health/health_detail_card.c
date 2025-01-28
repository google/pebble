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

#include "health_detail_card.h"

#include "applib/pbl_std/pbl_std.h"
#include "kernel/pbl_malloc.h"
#include "resource/resource_ids.auto.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/activity/health_util.h"
#include "util/size.h"

#include "system/logging.h"

#define CORNER_RADIUS (3)

static void prv_draw_headings(HealthDetailCard *detail_card, GContext *ctx, const Layer *layer) {
  const int16_t rect_padding = PBL_IF_RECT_ELSE(5, 22);
  const int16_t rect_height = 35;

  for (int i = 0; i < detail_card->num_headings; i++) {
    HealthDetailHeading *heading = &detail_card->headings[i];

    if (!heading->primary_label) {
      continue;
    }

#if PBL_ROUND
    int16_t header_y_origin = ((detail_card->num_headings > 1) ? 22 : 32) + (i * (rect_height + 5));
#endif

    GRect header_rect = grect_inset(layer->bounds, GEdgeInsets(rect_padding));
    header_rect.origin.y += PBL_IF_RECT_ELSE(detail_card->y_origin, header_y_origin);
    header_rect.size.h = rect_height;

    detail_card->y_origin += rect_height + rect_padding;

#if PBL_BW
    const GRect inner_rect = grect_inset(header_rect, GEdgeInsets(1));
    const uint16_t inner_corner_radius = CORNER_RADIUS - 1;
    graphics_context_set_stroke_color(ctx, heading->outline_color);
    graphics_draw_round_rect(ctx, &inner_rect, inner_corner_radius);
    graphics_draw_round_rect(ctx, &header_rect, CORNER_RADIUS);
#else
    graphics_context_set_fill_color(ctx, heading->fill_color);
    graphics_fill_round_rect(ctx, &header_rect, CORNER_RADIUS, GCornersAll);
#endif

    const bool has_secondary_heading = heading->secondary_label;

    GRect label_rect = header_rect;
    if (has_secondary_heading) {
      label_rect.size.w /= 2;
    }
    label_rect.size.h = 12; // Restrict to a single line

    graphics_context_set_text_color(ctx, gcolor_legible_over(heading->fill_color));

    graphics_draw_text(ctx, heading->primary_label, detail_card->heading_label_font,
                       label_rect, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    const int16_t value_rect_y_padding = 12;

    GRect value_rect = label_rect;
    value_rect.origin.y += value_rect_y_padding;

    graphics_draw_text(ctx, heading->primary_value, detail_card->heading_value_font,
                       value_rect, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    if (!heading->secondary_label) {
      continue;
    }

    const int separator_padding = 5;

    GPoint separator_top_point = GPoint(header_rect.origin.x + (header_rect.size.w / 2) - 1,
                                        header_rect.origin.y + separator_padding);
    GPoint separator_bot_point = GPoint(separator_top_point.x, separator_top_point.y +
                                        header_rect.size.h - (separator_padding * 2) - 1);

    graphics_draw_line(ctx, separator_top_point, separator_bot_point);

    // draw another line to make the width 2px
    separator_top_point.x++;
    separator_bot_point.x++;
    graphics_draw_line(ctx, separator_top_point, separator_bot_point);

    label_rect.origin.x += label_rect.size.w;
    value_rect.origin.x += value_rect.size.w;

    graphics_draw_text(ctx, heading->secondary_label, detail_card->heading_label_font,
                       label_rect, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    graphics_draw_text(ctx, heading->secondary_value, detail_card->heading_value_font,
                       value_rect, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void prv_draw_subtitles(HealthDetailCard *detail_card, GContext *ctx, const Layer *layer) {
  const int16_t rect_padding = PBL_IF_RECT_ELSE(5, 0);
  const int16_t rect_height = PBL_IF_RECT_ELSE(23, 36);

  for (int i = 0; i < detail_card->num_subtitles; i++) {
    HealthDetailSubtitle *subtitle = &detail_card->subtitles[i];

    if (!subtitle->label) {
      continue;
    }

    GRect subtitle_rect = grect_inset(layer->bounds, GEdgeInsets(rect_padding));
    subtitle_rect.origin.y += PBL_IF_RECT_ELSE(detail_card->y_origin, 125);
    subtitle_rect.size.h = rect_height;

    detail_card->y_origin += rect_height + rect_padding;

    graphics_context_set_fill_color(ctx, subtitle->fill_color);
    graphics_fill_round_rect(ctx, &subtitle_rect, CORNER_RADIUS, GCornersAll);

    if (!gcolor_equal(subtitle->outline_color, GColorClear)) {
      graphics_context_set_stroke_color(ctx, subtitle->outline_color);
      graphics_draw_round_rect(ctx, &subtitle_rect, CORNER_RADIUS);
    }

    // font offset
    subtitle_rect.origin.y -= PBL_IF_RECT_ELSE(1, 3);

    graphics_context_set_text_color(ctx, gcolor_legible_over(subtitle->fill_color));
    graphics_draw_text(ctx, subtitle->label, detail_card->subtitle_font, subtitle_rect,
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void prv_draw_progress_bar(GContext *ctx, HealthProgressBar *progress_bar, GColor bg_color,
    GColor fill_color, int current_progress, int typical_progress, int max_progress,
    bool hide_typical) {
  const GColor typical_color = PBL_IF_COLOR_ELSE(GColorYellow, GColorBlack);
  const GColor outline_color = PBL_IF_COLOR_ELSE(GColorClear, GColorBlack);

  health_progress_bar_fill(ctx, progress_bar, bg_color, 0, HEALTH_PROGRESS_BAR_MAX_VALUE);

  if (max_progress > 0) {
    const int current_fill = (current_progress * HEALTH_PROGRESS_BAR_MAX_VALUE) / max_progress;

    health_progress_bar_fill(ctx, progress_bar, fill_color, 0, current_fill);

    if (typical_progress > 0) {
      const int typical_fill = (typical_progress * HEALTH_PROGRESS_BAR_MAX_VALUE) / max_progress;
      health_progress_bar_mark(ctx, progress_bar, typical_color, typical_fill);
    }
  }

  health_progress_bar_outline(ctx, progress_bar, outline_color);
}

#if PBL_RECT
static void prv_draw_progress_bar_in_zone(GContext *ctx, const GRect *zone_rect, GColor fill_color,
    int current_progress, int typical_progress, int max_progress, bool hide_typical) {
  const int16_t progress_bar_x = zone_rect->origin.x + PBL_IF_BW_ELSE(0, -1);
  const int16_t progress_bar_y = zone_rect->origin.y + 22;
  const int16_t progress_bar_width = zone_rect->size.w + PBL_IF_BW_ELSE(-2, 1);
  const int16_t progress_bar_height = 10 + PBL_IF_BW_ELSE(-1, 0);

  HealthProgressSegment segments[] = {
    {
      // Left side vertical line (needed for the draw outline function to draw the vertical lines)
      .type = HealthProgressSegmentType_Corner,
      .points = {
        {progress_bar_x, progress_bar_y},
        {progress_bar_x, progress_bar_y + progress_bar_height},
        {progress_bar_x, progress_bar_y + progress_bar_height},
        {progress_bar_x, progress_bar_y},
      },
    },
    {
      // Right side vertical line (needed for the draw outline function to draw the vertical lines)
      .type = HealthProgressSegmentType_Corner,
      .points = {
        {progress_bar_x + progress_bar_width, progress_bar_y},
        {progress_bar_x + progress_bar_width, progress_bar_y + progress_bar_height},
        {progress_bar_x + progress_bar_width, progress_bar_y + progress_bar_height},
        {progress_bar_x + progress_bar_width, progress_bar_y},
      },
    },
    {
      // Horizontal bar from left line to right line
      .type = HealthProgressSegmentType_Horizontal,
      .amount_of_total = HEALTH_PROGRESS_BAR_MAX_VALUE,
      .mark_width = 124, // Arbitrarily chosen through trial and error
      .points = {
        {progress_bar_x, progress_bar_y + progress_bar_height},
        {progress_bar_x + progress_bar_width, progress_bar_y + progress_bar_height},
        {progress_bar_x + progress_bar_width, progress_bar_y},
        {progress_bar_x, progress_bar_y},
      },
    },
  };

  HealthProgressBar progress_bar = {
    .num_segments = ARRAY_LENGTH(segments),
    .segments = segments,
  };

  const GColor bg_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite);

  prv_draw_progress_bar(ctx, &progress_bar, bg_color, fill_color, current_progress,
                        typical_progress, max_progress, hide_typical);
}

static void prv_draw_zones(HealthDetailCard *detail_card, GContext *ctx) {
  if (detail_card->num_zones <= 0) {
    return;
  }

  const int16_t rect_padding = 5;
  const int16_t rect_height = 33;

  GRect zone_rect = grect_inset(detail_card->window.layer.bounds, GEdgeInsets(rect_padding));
  zone_rect.origin.y += detail_card->y_origin;
  zone_rect.size.h = rect_height;

  for (int i = 0; i < detail_card->num_zones; i++) {
    HealthDetailZone *zone = &detail_card->zones[i];

    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
    graphics_draw_text(ctx, zone->label, detail_card->subtitle_font, zone_rect,
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    if (zone->show_crown) {
      const GSize label_size = app_graphics_text_layout_get_content_size(zone->label,
          detail_card->subtitle_font, zone_rect, GTextOverflowModeWordWrap, GTextAlignmentLeft);
      GPoint icon_offset = zone_rect.origin;
      icon_offset.x += label_size.w + 4;
#if PBL_BW
      icon_offset.y += 2;
#endif
      gdraw_command_image_draw(ctx, detail_card->icon_crown, icon_offset);
    }

    prv_draw_progress_bar_in_zone(ctx, &zone_rect, zone->fill_color, zone->progress,
        detail_card->daily_avg, detail_card->max_progress, zone->hide_typical);

    zone_rect.origin.y += rect_height + rect_padding;
    detail_card->y_origin += rect_height + rect_padding;
  }

  detail_card->y_origin += rect_padding;
}
#endif // PBL_RECT

#if PBL_ROUND
static uint16_t prv_get_num_rows_callback(MenuLayer *menu_layer,
                                          uint16_t section_index, void *context) {
  HealthDetailCard *detail_card = (HealthDetailCard *)context;
  return detail_card->num_zones + 1;
}

static void prv_draw_row_callback(GContext *ctx, const Layer *cell_layer,
                                  MenuIndex *cell_index, void *context) {
  HealthDetailCard *detail_card = (HealthDetailCard *)context;

  MenuIndex selected_index = menu_layer_get_selected_index(&detail_card->menu_layer);

  if (cell_index->row == 0) {
    graphics_context_set_fill_color(ctx, detail_card->bg_color);
    graphics_fill_rect(ctx, &cell_layer->bounds);

    prv_draw_headings(detail_card, ctx, cell_layer);
    prv_draw_subtitles(detail_card, ctx, cell_layer);
    return;
  }

  HealthDetailZone *zone = &detail_card->zones[cell_index->row - 1];

  const int16_t rect_padding = 5;

  GRect label_rect = grect_inset(cell_layer->bounds, GEdgeInsets(rect_padding));

  if (!menu_layer_is_index_selected(&detail_card->menu_layer, cell_index)) {
    label_rect.origin.y = (cell_index->row < selected_index.row) ? 3 : 22;

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, zone->label, detail_card->subtitle_font, label_rect,
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  } else {
    const GRect cell_bounds = grect_inset(cell_layer->bounds, GEdgeInsets(0, -1));

    HealthProgressSegment segments[] = {
      {
        // Horizontal bar from left line to right line
        .type = HealthProgressSegmentType_Horizontal,
        .amount_of_total = HEALTH_PROGRESS_BAR_MAX_VALUE,
        .mark_width = 100, // Arbitrarily chosen through trial and error
        .points = {
          {cell_bounds.origin.x, cell_bounds.size.h},
          {cell_bounds.size.w, cell_bounds.size.h},
          {cell_bounds.size.w, cell_bounds.origin.y},
          {cell_bounds.origin.x, cell_bounds.origin.y},
        },
      },
    };

    HealthProgressBar progress_bar = {
      .num_segments = ARRAY_LENGTH(segments),
      .segments = segments,
    };

    prv_draw_progress_bar(ctx, &progress_bar, GColorLightGray, zone->fill_color, zone->progress,
                          detail_card->daily_avg, detail_card->max_progress, zone->hide_typical);

    label_rect.origin.y += 3;

    if (zone->show_crown) {
      const GSize icon_size = gdraw_command_image_get_bounds_size(detail_card->icon_crown);
      GPoint icon_offset = GPoint((cell_layer->bounds.size.w / 2) - (icon_size.w / 2), 4);
      gdraw_command_image_draw(ctx, detail_card->icon_crown, icon_offset);

      label_rect.origin.y += 8;
    }

    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, zone->label, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), label_rect,
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static int16_t prv_get_cell_height_callback(MenuLayer *menu_layer,
                                            MenuIndex *cell_index, void *context) {
  if (cell_index->row == 0) {
    return menu_layer_is_index_selected(menu_layer, cell_index) ? DISP_ROWS : 0;
  }

  return menu_layer_is_index_selected(menu_layer, cell_index) ? 50 : 54;
}

static void prv_refresh_content_indicators(HealthDetailCard *detail_card) {
  const bool is_up_visible = (menu_layer_get_selected_index(&detail_card->menu_layer).row > 0);
  const bool is_down_visible = (menu_layer_get_selected_index(&detail_card->menu_layer).row <
      prv_get_num_rows_callback(&detail_card->menu_layer, 0, detail_card) - 1);

  content_indicator_set_content_available(&detail_card->up_indicator,
                                          ContentIndicatorDirectionUp,
                                          is_up_visible);

  content_indicator_set_content_available(&detail_card->down_indicator,
                                          ContentIndicatorDirectionDown,
                                          is_down_visible);
}

static void prv_selection_changed_callback(struct MenuLayer *menu_layer, MenuIndex new_index,
                                           MenuIndex old_index, void *context) {
  HealthDetailCard *detail_card = (HealthDetailCard *)context;
  prv_refresh_content_indicators(detail_card);
}
#else // PBL_RECT
static void prv_health_detail_scroll_layer_update_proc(Layer *layer, GContext *ctx) {
  ScrollLayer *scroll_layer = (ScrollLayer *)layer->parent;
  HealthDetailCard *detail_card = (HealthDetailCard *)scroll_layer->context;

  detail_card->y_origin = 0;

  prv_draw_headings(detail_card, ctx, &detail_card->window.layer);
  prv_draw_subtitles(detail_card, ctx, &detail_card->window.layer);
  prv_draw_zones(detail_card, ctx);

  scroll_layer_set_content_size(&detail_card->scroll_layer,
                                GSize(layer->bounds.size.w, detail_card->y_origin));
}
#endif // PBL_RECT

HealthDetailCard *health_detail_card_create(const HealthDetailCardConfig *config) {
  HealthDetailCard *detail_card = app_zalloc_check(sizeof(HealthDetailCard));
  window_init(&detail_card->window, WINDOW_NAME("Health Detail Card"));
  health_detail_card_configure(detail_card, config);
  GRect window_frame = detail_card->window.layer.frame;
#if PBL_ROUND
  // setup menu layer
  MenuLayer *menu_layer = &detail_card->menu_layer;
  menu_layer_init(menu_layer, &window_frame);
  menu_layer_set_callbacks(menu_layer, detail_card, &(MenuLayerCallbacks) {
    .get_num_rows = prv_get_num_rows_callback,
    .get_cell_height = prv_get_cell_height_callback,
    .draw_row = prv_draw_row_callback,
    .selection_changed = prv_selection_changed_callback,
  });
  menu_layer_set_normal_colors(menu_layer, detail_card->bg_color, GColorWhite);
  menu_layer_set_highlight_colors(menu_layer, detail_card->bg_color, GColorBlack);
  menu_layer_set_click_config_onto_window(menu_layer, &detail_card->window);
  layer_add_child(&detail_card->window.layer, menu_layer_get_layer(menu_layer));

  // setup content indicators
  const int content_indicator_height = 15;
  const GRect down_arrow_layer_frame = grect_inset(window_frame,
      GEdgeInsets(window_frame.size.h - content_indicator_height, 0, 0));
  layer_init(&detail_card->down_arrow_layer, &down_arrow_layer_frame);
  layer_add_child(&detail_card->window.layer, &detail_card->down_arrow_layer);
  content_indicator_init(&detail_card->down_indicator);

  const GRect up_arrow_layer_frame = grect_inset(window_frame,
      GEdgeInsets(0, 0, window_frame.size.h - content_indicator_height));
  layer_init(&detail_card->up_arrow_layer, &up_arrow_layer_frame);
  layer_add_child(&detail_card->window.layer, &detail_card->up_arrow_layer);
  content_indicator_init(&detail_card->up_indicator);

  ContentIndicatorConfig content_indicator_config = (ContentIndicatorConfig) {
    .layer = &detail_card->up_arrow_layer,
    .colors.foreground = gcolor_legible_over(detail_card->bg_color),
    .colors.background = detail_card->bg_color,
  };
  content_indicator_configure_direction(&detail_card->up_indicator, ContentIndicatorDirectionUp,
                                        &content_indicator_config);
  content_indicator_config.layer = &detail_card->down_arrow_layer;
  content_indicator_configure_direction(&detail_card->down_indicator, ContentIndicatorDirectionDown,
                                        &content_indicator_config);
  prv_refresh_content_indicators(detail_card);
#else // PBL_RECT
  // setup scroll layer
  scroll_layer_init(&detail_card->scroll_layer, &window_frame);
  scroll_layer_set_click_config_onto_window(&detail_card->scroll_layer, &detail_card->window);
  scroll_layer_set_context(&detail_card->scroll_layer, detail_card);
  scroll_layer_set_shadow_hidden(&detail_card->scroll_layer, true);
  layer_add_child(&detail_card->window.layer, (Layer *)&detail_card->scroll_layer);
  layer_set_update_proc(&detail_card->scroll_layer.content_sublayer,
                        prv_health_detail_scroll_layer_update_proc);
#endif // PBL_RECT
  detail_card->icon_crown = gdraw_command_image_create_with_resource(RESOURCE_ID_HEALTH_APP_CROWN);
  detail_card->heading_label_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  detail_card->heading_value_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  detail_card->subtitle_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  return detail_card;
}

void health_detail_card_destroy(HealthDetailCard *detail_card) {
  if (!detail_card) {
    return;
  }
  gdraw_command_image_destroy(detail_card->icon_crown);
#if PBL_ROUND
  menu_layer_deinit(&detail_card->menu_layer);
  content_indicator_deinit(&detail_card->down_indicator);
  content_indicator_deinit(&detail_card->up_indicator);
#else
  scroll_layer_deinit(&detail_card->scroll_layer);
#endif
  i18n_free_all(detail_card);
  app_free(detail_card);
}

void health_detail_card_configure(HealthDetailCard *detail_card,
                                  const HealthDetailCardConfig *config) {
  if (!detail_card || !config) {
    return;
  }
  detail_card->bg_color = config->bg_color;
  window_set_background_color(&detail_card->window, detail_card->bg_color);
  if (config->num_headings) {
    detail_card->num_headings = config->num_headings;
    detail_card->headings = config->headings;
  }
  if (config->num_subtitles) {
    detail_card->num_subtitles = config->num_subtitles;
    detail_card->subtitles = config->subtitles;
  }
  detail_card->daily_avg = config->daily_avg;
  detail_card->max_progress = MAX(config->weekly_max, detail_card->daily_avg);
  detail_card->max_progress = detail_card->max_progress * 11 / 10; // add 10%;
  if (config->num_zones) {
    detail_card->num_zones = config->num_zones;
    detail_card->zones = config->zones;
  }
  if (config->data) {
    detail_card->data = config->data;
  }
}

void health_detail_card_set_render_day_zones(HealthDetailZone *zones, int16_t *num_zones,
    int32_t *weekly_max, bool format_hours_and_minutes, bool show_crown, GColor fill_color,
    GColor today_fill_color, int32_t *day_data, void *i18n_owner) {
  time_t time_utc = rtc_get_time();
  struct tm time_tm;

  int max_data = 0;
  int crown_index = 0;

  *num_zones = DAYS_PER_WEEK;

  for (int i = 0; i < *num_zones; i++) {
    localtime_r(&time_utc, &time_tm);

    const bool is_today = (i == 0);

    const size_t buffer_size = 32;
    zones[i] = (HealthDetailZone) {
      .label = app_zalloc_check(buffer_size),
      .progress = day_data[i],
      .fill_color = is_today ? PBL_IF_ROUND_ELSE(fill_color, today_fill_color) : fill_color,
      .hide_typical = is_today,
    };

    char *label_ptr = zones[i].label;

    int pos = 0;

    if (i == 0) {
      pos += snprintf(label_ptr, buffer_size, "%s ", i18n_get("Today", i18n_owner));
    } else {
      pos += strftime(label_ptr, buffer_size, "%a ", &time_tm);
    }

    if (day_data[i] > 0) {
      if (format_hours_and_minutes) {
        health_util_format_hours_and_minutes(label_ptr + pos, buffer_size - pos, day_data[i],
                                             i18n_owner);
      } else {
        snprintf(label_ptr + pos, buffer_size - pos, "%"PRId32, day_data[i]);
      }
    }

    if (day_data[i] > *weekly_max) {
      *weekly_max = day_data[i];
    }

    if (day_data[i] > max_data) {
      max_data = day_data[i];
      crown_index = i;
    }

    time_utc -= SECONDS_PER_DAY;
  }

  if (crown_index && show_crown) {
    zones[crown_index].show_crown = true;
  }
}
