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

#include "weather_app_layout.h"

#include "applib/fonts/fonts.h"
#include "applib/graphics/gcontext.h"
#include "applib/graphics/gdraw_command_image.h"
#include "applib/graphics/gpath.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/graphics_circle.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/text.h"
#include "applib/ui/animation.h"
#include "applib/ui/animation_interpolate.h"
#include "applib/ui/animation_timing.h"
#include "applib/ui/content_indicator.h"
#include "applib/ui/kino/kino_layer.h"
#include "applib/ui/kino/kino_reel/morph_square.h"
#include "applib/ui/kino/kino_reel/transform.h"
#include "applib/ui/window.h"
#include "apps/system_apps/timeline/text_node.h"
#include "font_resource_keys.auto.h"
#include "kernel/pbl_malloc.h"
#include "resource/resource_ids.auto.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/timeline/timeline_resources.h"
#include "services/normal/weather/weather_service.h"
#include "services/normal/weather/weather_types.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"
#include "util/trig.h"

#include <stdio.h>

#define WEATHER_APP_LAYOUT_ARROW_LAYER_HEIGHT (18)
#define WEATHER_APP_LAYOUT_TOP_PADDING PBL_IF_RECT_ELSE(0, 15)
#define WEATHER_APP_LAYOUT_TIMELINE_ICON_RESOURCE_SIZE (TimelineResourceSizeTiny)
#define WEATHER_APP_LAYOUT_CONTENT_LAYER_HORIZONTAL_INSET PBL_IF_RECT_ELSE(3, 23)

static int prv_draw_text(GPoint offset, int max_width, GContext *context,
                         const char *text, const GFont font,
                         GColor font_color, GTextAlignment alignment) {
  const int height = fonts_get_font_height(font);
  const GRect box = (GRect) {offset, GSize(max_width, height)};

  graphics_context_set_text_color(context, font_color);
  graphics_draw_text(context, text, font, box, GTextOverflowModeFill, alignment, NULL);

  return height;
}

static void prv_draw_weather_background(const GRect *circle_bounding_box, GContext *context,
                                        GColor background_color) {
  if (!gcolor_is_invisible(background_color)) {
    graphics_context_set_fill_color(context, background_color);
    graphics_fill_oval(context, *circle_bounding_box, GOvalScaleModeFitCircle);
  }
}

static void prv_fill_high_low_temp_buffer(const int high, const int low, char *buffer,
                                          const size_t buffer_size, const void *i18n_owner) {
  if ((high == WEATHER_SERVICE_LOCATION_FORECAST_UNKNOWN_TEMP) &&
      (low == WEATHER_SERVICE_LOCATION_FORECAST_UNKNOWN_TEMP)) {
    /// Shown when neither high nor low temperature is known
    const char *both_temps_no_data = i18n_get("--° / --°", i18n_owner);
    strncpy(buffer, both_temps_no_data, strlen(both_temps_no_data));
    buffer[buffer_size - 1] = '\0';
  } else if (low == WEATHER_SERVICE_LOCATION_FORECAST_UNKNOWN_TEMP) {
    /// Shown when only the day's high temperature is known, (e.g. "68° / --°")
    snprintf(buffer, buffer_size, i18n_get("%i° / --°", i18n_owner), high);
  } else if (high == WEATHER_SERVICE_LOCATION_FORECAST_UNKNOWN_TEMP) {
    /// Shown when only the day's low temperature is known (e.g. "--° / 52°")
    snprintf(buffer, buffer_size, i18n_get("--° / %i°", i18n_owner), low);
  } else {
    /// A day's high and low temperature, separated by a forward slash (e.g. "68° / 52°")
    snprintf(buffer, buffer_size, i18n_get("%i° / %i°", i18n_owner), high, low);
  }
}

#define GPS_ARROW_WIDTH (12)
#define GPS_ARROW_HEIGHT (14)

static const GPoint s_gps_arrow_path_points[] = {
  {0, GPS_ARROW_HEIGHT},
  {(GPS_ARROW_WIDTH / 2), 0},
  {GPS_ARROW_WIDTH, GPS_ARROW_HEIGHT},
  // This 6/7 height ratio for the arrow notch achieves the design spec
  {(GPS_ARROW_WIDTH / 2), (GPS_ARROW_HEIGHT * 6 / 7)}
};

static void prv_draw_gps_arrow_node_callback(GContext *ctx, const GRect *rect,
                                             UNUSED const GTextNodeDrawConfig *config, bool render,
                                             GSize *size_out, UNUSED void *user_data) {
  GPath gps_arrow_path = (GPath) {
      .num_points = ARRAY_LENGTH(s_gps_arrow_path_points),
      .points = (GPoint *)s_gps_arrow_path_points,
      .offset = rect->origin,
      // Ideal rotation would be 45 degrees, but the shape of the arrow matches the design best at
      // 38 degrees
      .rotation = DEG_TO_TRIGANGLE(38),
  };

  if (render) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    gpath_draw_filled(ctx, &gps_arrow_path);
  }
  if (size_out) {
    // Note that gpath_outer_rect() doesn't take into account the rotation; we'll add margin to the
    // location text node to account for it
    *size_out = gpath_outer_rect(&gps_arrow_path).size;
  }
}

static GTextNode *prv_create_location_name_area_node(const WeatherLocationForecast *forecast,
                                                     GFont location_font) {
  const GTextAlignment location_name_alignment = PBL_IF_RECT_ELSE(GTextAlignmentLeft,
                                                                  GTextAlignmentCenter);

  // One node for the location name text and one node for the possible GPS arrow
  const size_t max_nodes = 2;
  GTextNodeHorizontal *horizontal_node = graphics_text_node_create_horizontal(max_nodes);
  horizontal_node->horizontal_alignment = location_name_alignment;

  GTextNodeText *location_text_node = graphics_text_node_create_text(0);
  location_text_node->text = forecast->location_name;
  location_text_node->font = location_font;
  location_text_node->color = GColorBlack;
  location_text_node->overflow = GTextOverflowModeTrailingEllipsis;
  if (forecast->is_current_location) {
    // Horizontal spacing between location name and GPS arrow is spec'd by design to be 11 pixels
    location_text_node->node.margin = GSize(11, 0);
  }
  graphics_text_node_container_add_child(&horizontal_node->container, &location_text_node->node);

  if (forecast->is_current_location) {
    GTextNodeCustom *arrow_node = graphics_text_node_create_custom(prv_draw_gps_arrow_node_callback,
                                                                   NULL);
    arrow_node->node.offset =
        GPoint(0, (int16_t)(fonts_get_font_cap_offset(location_font) / 2));
    graphics_text_node_container_add_child(&horizontal_node->container, &arrow_node->node);
  }

  return &horizontal_node->container.node;
}

static GSize prv_draw_location_name_area(GPoint offset, int max_width, GContext *ctx,
                                         GFont location_font,
                                         const WeatherLocationForecast *forecast) {
  GTextNode *location_name_area_node = prv_create_location_name_area_node(forecast, location_font);

  GRect location_name_area_rect = (GRect) {
    .origin = offset,
    .size = GSize(max_width, fonts_get_font_height(location_font)),
  };

#if PBL_ROUND
  // On round the location name text and arrow can be obscured by the edges of the bezel, so we
  // horizontally inset the rectangle by a few pixels
  const int16_t horizontal_inset = 5;
  location_name_area_rect = grect_inset(location_name_area_rect,
                                        GEdgeInsets(0, horizontal_inset, 0));
#endif

  GSize location_name_area_size;
  graphics_text_node_draw(location_name_area_node, ctx, &location_name_area_rect, NULL,
                          &location_name_area_size);
  graphics_text_node_destroy(location_name_area_node);
  return location_name_area_size;
}
// All text before the separator
static void prv_draw_top_half_text(const WeatherAppLayout *layout, GPoint *current_offset,
                                   int content_width,
                                   GContext *context) {
  const WeatherLocationForecast *forecast = layout->forecast;

  current_offset->y += prv_draw_location_name_area(*current_offset, content_width, context,
                                                   layout->location_font, forecast).h;

  const int location_and_today_temperature_vertical_spacing = 7;
  current_offset->y += location_and_today_temperature_vertical_spacing;

  char text_buffer[15] = {0};
  const size_t max_text_buff_size = ARRAY_LENGTH(text_buffer);

  if (forecast->current_temp == WEATHER_SERVICE_LOCATION_FORECAST_UNKNOWN_TEMP) {
    /// Shown when today's current temperature is unknown
    const char *unknown_temp_string = i18n_get("--°", layout);
    strncpy(text_buffer, unknown_temp_string, strlen(unknown_temp_string));
    text_buffer[max_text_buff_size - 1] = '\0';
  } else {
    /// Today's current temperature (e.g. "68°")
    snprintf(text_buffer, max_text_buff_size, i18n_get("%i°", layout), forecast->current_temp);
  }
  current_offset->y += prv_draw_text(*current_offset, content_width, context, text_buffer,
                                     layout->temperature_font, GColorBlack, GTextAlignmentLeft);

  prv_fill_high_low_temp_buffer(forecast->today_high, forecast->today_low, text_buffer,
                                max_text_buff_size, layout);
  current_offset->y += prv_draw_text(*current_offset, content_width, context, text_buffer,
                                     layout->high_low_phrase_font, GColorBlack,
                                     GTextAlignmentLeft);
  const int today_high_low_gap_vertical_spacing_reduction = 2;
  current_offset->y -= today_high_low_gap_vertical_spacing_reduction;

  current_offset->y += prv_draw_text(*current_offset, content_width, context,
                                     forecast->current_weather_phrase, layout->high_low_phrase_font,
                                     GColorBlack, GTextAlignmentLeft);
}

// All text after the separator
static void prv_draw_bottom_half_text(const WeatherAppLayout *layout, GPoint *current_offset,
                                      int content_width, GContext *context) {
  const WeatherLocationForecast *forecast = layout->forecast;

  const int separator_tomorrow_title_vertical_spacing = 6;
  current_offset->y += separator_tomorrow_title_vertical_spacing;
  current_offset->y += prv_draw_text(*current_offset, content_width, context,
  /// Refers to the weather conditions for tomorrow
                                    i18n_get("TOMORROW", layout), layout->tomorrow_font,
                                    GColorBlack, GTextAlignmentLeft);
  char text_buffer[15] = {0};
  const size_t max_text_buff_size = ARRAY_LENGTH(text_buffer);
  prv_fill_high_low_temp_buffer(forecast->tomorrow_high, forecast->tomorrow_low, text_buffer,
                                max_text_buff_size, layout);
  prv_draw_text(*current_offset, content_width, context, text_buffer, layout->high_low_phrase_font,
                GColorBlack, GTextAlignmentLeft);
}

static void prv_draw_weather_icon_backgrounds(const WeatherAppLayout *layout,
                                              const GRect *content_bounds, GContext *context) {
  const WeatherLocationForecast *forecast = layout->forecast;
  // assume that both current and tomorrow weather icons are the same size
  const GSize icon_size = layout->current_weather_icon_layer.layer.bounds.size;
  const unsigned int weather_icon_bg_circle_diam = integer_sqrt(2 * icon_size.w * icon_size.h);

  const int today_weather_bg_circle_top_margin = 28;
  GRect bg_circle_bounding_box = GRect(grect_get_max_x(content_bounds) -
                                       weather_icon_bg_circle_diam,
                                       today_weather_bg_circle_top_margin,
                                       weather_icon_bg_circle_diam, weather_icon_bg_circle_diam);

  prv_draw_weather_background(&bg_circle_bounding_box, context,
                              weather_type_get_bg_color(forecast->current_weather_type));

  const int weather_bg_circle_vertical_spacing = 40;
  bg_circle_bounding_box.origin.y += weather_icon_bg_circle_diam +
                                     weather_bg_circle_vertical_spacing;
  prv_draw_weather_background(&bg_circle_bounding_box, context,
                              weather_type_get_bg_color(forecast->tomorrow_weather_type));
}


static void prv_render_layout(Layer *layer, GContext *context) {
  // "Content" refers to everything except the dot separator
  const GRect content_bounds =
      grect_inset(layer->bounds, GEdgeInsets(0, WEATHER_APP_LAYOUT_CONTENT_LAYER_HORIZONTAL_INSET,
                                             0));
  const int content_x_offset = content_bounds.origin.x;
  const int content_width = content_bounds.size.w;

  const WeatherAppLayout *layout = window_get_user_data(layer_get_window(layer));
  const WeatherLocationForecast *forecast = layout->forecast;

  if (!forecast) {
    // Nothing to draw.
    return;
  }
  // start at 1 from the top to match design docs
  GPoint current_offset = GPoint(content_x_offset, 1);
  GPoint *offset = &current_offset;

  prv_draw_top_half_text(layout, offset, content_width, context);

  // dotted separator
  const int phrase_separator_vertical_spacing = 10;
  current_offset.y += phrase_separator_vertical_spacing;

  const GPoint separator_start = GPoint(0, current_offset.y);
  graphics_context_set_stroke_width(context, 5);
  graphics_context_set_stroke_color(context, PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack));
  graphics_draw_horizontal_line_dotted(context, separator_start,
                                       layer->bounds.size.w);

  if (!layout->animation_state.hide_bottom_half_text) {
    prv_draw_bottom_half_text(layout, offset, content_width, context);
  }
  prv_draw_weather_icon_backgrounds(layout, &content_bounds, context);
}

static void prv_content_indicator_setup_direction(ContentIndicator *content_indicator,
                                                  Layer *indicator_layer,
                                                  ContentIndicatorDirection direction) {
  content_indicator_configure_direction(content_indicator, direction, &(ContentIndicatorConfig) {
    .layer = indicator_layer,
    .colors.foreground = GColorBlack,
    .colors.background = GColorLightGray,
  });
}

void weather_app_layout_init(WeatherAppLayout *layout, const GRect *frame) {
  layout->location_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  layout->temperature_font = fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM);
  layout->high_low_phrase_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  layout->tomorrow_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  Layer *root_layer = &layout->root_layer;
  layer_init(root_layer, frame);

  Layer *down_arrow_layer = &layout->down_arrow_layer;
  const GRect down_arrow_layer_frame = grect_inset(
      *frame, GEdgeInsets(frame->size.h - WEATHER_APP_LAYOUT_ARROW_LAYER_HEIGHT, 0, 0));
  layer_init(down_arrow_layer, &down_arrow_layer_frame);
  layer_add_child(root_layer, down_arrow_layer);

  const int content_layer_side_padding = PBL_IF_RECT_ELSE(5, 12);
  const GRect content_layer_frame = grect_inset(*frame,
                                                GEdgeInsets(WEATHER_APP_LAYOUT_TOP_PADDING,
                                                            content_layer_side_padding,
                                                            WEATHER_APP_LAYOUT_ARROW_LAYER_HEIGHT));

  Layer *content_layer = &layout->content_layer;
  layer_init(content_layer, &content_layer_frame);
  layer_set_update_proc(content_layer, prv_render_layout);
  layer_add_child(root_layer, content_layer);

  ContentIndicator *content_indicator = &layout->content_indicator;
  content_indicator_init(content_indicator);

  prv_content_indicator_setup_direction(content_indicator, down_arrow_layer,
                                        ContentIndicatorDirectionDown);

  const GSize icon_size =
      timeline_resources_get_gsize(WEATHER_APP_LAYOUT_TIMELINE_ICON_RESOURCE_SIZE);

  const int icon_layer_margin_top = PBL_IF_RECT_ELSE(33, 18);
  const int icon_layer_right_margin = 5;
  GRect icon_layer_frame = (GRect) {
    {
      content_layer_frame.size.w - icon_size.w - WEATHER_APP_LAYOUT_CONTENT_LAYER_HORIZONTAL_INSET -
      icon_layer_right_margin,
      content_layer_frame.origin.y + icon_layer_margin_top
    },
    icon_size
  };

  KinoLayer *current_weather_icon_layer  = &layout->current_weather_icon_layer;
  kino_layer_init(current_weather_icon_layer, &icon_layer_frame);
  layer_add_child(content_layer, kino_layer_get_layer(current_weather_icon_layer));

  const int icon_layer_spacing = 50;
  icon_layer_frame.origin.y += icon_size.h + icon_layer_spacing;

  KinoLayer *tomorrow_weather_icon_layer  = &layout->tomorrow_weather_icon_layer;
  kino_layer_init(tomorrow_weather_icon_layer, &icon_layer_frame);
  layer_add_child(content_layer, kino_layer_get_layer(tomorrow_weather_icon_layer));
}

static uint32_t prv_get_resource_id_for_weather_type(WeatherType type) {
  const TimelineResourceInfo timeline_res = {
    .res_id = weather_type_get_timeline_resource_id(type),
  };
  AppResourceInfo icon_res_info;
  timeline_resources_get_id(&timeline_res, WEATHER_APP_LAYOUT_TIMELINE_ICON_RESOURCE_SIZE,
                            &icon_res_info);
  return icon_res_info.res_id;
}

void weather_app_layout_set_data(WeatherAppLayout *layout,
                                     const WeatherLocationForecast *forecast) {
  layout->forecast = forecast;

  const uint32_t current_weather_res_id = forecast ?
      prv_get_resource_id_for_weather_type(forecast->current_weather_type) : RESOURCE_ID_INVALID;
  const uint32_t tomorrow_weather_res_id = forecast ?
      prv_get_resource_id_for_weather_type(forecast->tomorrow_weather_type) : RESOURCE_ID_INVALID;

  kino_layer_set_reel_with_resource(&layout->current_weather_icon_layer, current_weather_res_id);
  kino_layer_set_reel_with_resource(&layout->tomorrow_weather_icon_layer, tomorrow_weather_res_id);

  layer_mark_dirty(&layout->root_layer);
}

void weather_app_layout_set_down_arrow_visible(WeatherAppLayout *layout, bool is_down_visible) {
  content_indicator_set_content_available(&layout->content_indicator, ContentIndicatorDirectionDown,
                                          is_down_visible);
}

void weather_app_layout_deinit(WeatherAppLayout *layout) {
  i18n_free_all(layout);
  layer_deinit(&layout->root_layer);
}

// Down arrow layer grows until a point, after which the entire content teleports to a height
// slightly higher than its resting position, then relaxes into place
static void prv_down_animation_update(Animation *animation, AnimationProgress normalized) {
  WeatherAppLayout *layout = animation_get_context(animation);
  // Progress at which to switch from the down arrow growing to entire content relaxing downwards
  const AnimationProgress animation_cut_frame_progress =
      (interpolate_moook_in_duration() * ANIMATION_NORMALIZED_MAX) / interpolate_moook_duration();
  // Progress at which to hide "TOMORROW" and tomorrow high / low temperature text
  const AnimationProgress animation_hide_bottom_half_text_progress =
      (animation_cut_frame_progress * 2) / 3;

  int down_arrow_layer_height = WEATHER_APP_LAYOUT_ARROW_LAYER_HEIGHT;
  layout->animation_state.hide_bottom_half_text = false;

  if (normalized <= animation_cut_frame_progress) {
    if (normalized >= animation_hide_bottom_half_text_progress) {
      layout->animation_state.hide_bottom_half_text = true;
    }
    // renormalize the progress so that interpolate_moook_in_only works as expected
    int32_t new_normalized = animation_timing_scaled(normalized, ANIMATION_NORMALIZED_MIN,
                                                     animation_cut_frame_progress);
    const int additional_down_arrow_height = 25;
    // grow the down arrow layer
    down_arrow_layer_height += interpolate_moook_in_only(new_normalized, 0,
                                                         additional_down_arrow_height);
  } else {
    // We've cut, so display the next forecast's data
    if (layout->animation_state.next_forecast) {
      weather_app_layout_set_data(layout, layout->animation_state.next_forecast);
      layout->animation_state.next_forecast = NULL;
    }
    int32_t new_normalized = animation_timing_scaled(normalized, animation_cut_frame_progress,
                                                     ANIMATION_NORMALIZED_MAX);

    // Relax the content by changing its top margin
    const int animation_margin_top_from = WEATHER_APP_LAYOUT_TOP_PADDING - PBL_IF_RECT_ELSE(10, 15);
    const int animation_margin_top_to = WEATHER_APP_LAYOUT_TOP_PADDING;
    const int num_frames_from = 1;
    const bool bounce_back = false;
    int animation_margin_top = interpolate_moook_out(new_normalized, animation_margin_top_from,
                                                     animation_margin_top_to, num_frames_from,
                                                     bounce_back);
    layout->content_layer.frame.origin.y = animation_margin_top;

    // The down arrow's height follows the content margin. It starts off large, then goes back to
    // its original size, as the content relaxes into place
    down_arrow_layer_height += (-animation_margin_top + WEATHER_APP_LAYOUT_TOP_PADDING);
  }

  const GRect down_arrow_layer_frame = grect_inset(layout->root_layer.frame,
      GEdgeInsets(layout->root_layer.frame.size.h - down_arrow_layer_height, 0, 0));
  layer_set_frame(&layout->down_arrow_layer, &down_arrow_layer_frame);

  layer_mark_dirty(&layout->root_layer);
}

// moves the entire root layer up back into place
static void prv_up_animation_update(Animation *animation, AnimationProgress normalized) {
  const int root_layer_top_margin_from = (WEATHER_APP_LAYOUT_ARROW_LAYER_HEIGHT * 2) / 3;
  const int root_layer_top_margin_to = 0;
  const int num_frames_from = 1;
  const bool bounce_back = false;
  int root_layer_top_margin = interpolate_moook_out(normalized, root_layer_top_margin_from,
                              root_layer_top_margin_to, num_frames_from, bounce_back);

  WeatherAppLayout *layout = animation_get_context(animation);
  if (layout->animation_state.next_forecast) {
    layout->forecast = layout->animation_state.next_forecast;
    layout->animation_state.next_forecast = NULL;
  }

  layout->root_layer.frame.origin.y = root_layer_top_margin;
  layer_set_frame(&layout->root_layer, &layout->root_layer.frame);
}

static void prv_animation_stopped(Animation *animation, bool finished, void *context) {
  WeatherAppLayout *layout = context;
  if (layout->animation_state.next_forecast) {
    weather_app_layout_set_data(layout, layout->animation_state.next_forecast);
    layout->animation_state.next_forecast = NULL;
  }
  layout->animation_state.hide_bottom_half_text = false;

  GRect *root_layer_frame = &layout->root_layer.frame;
  root_layer_frame->origin.y = 0;
  layer_set_frame(&layout->root_layer, root_layer_frame);

  GRect *content_layer_frame = &layout->content_layer.frame;
  content_layer_frame->origin.y = WEATHER_APP_LAYOUT_TOP_PADDING;
  layer_set_frame(&layout->content_layer, content_layer_frame);

  const GRect down_arrow_layer_frame = grect_inset(*root_layer_frame,
      GEdgeInsets(root_layer_frame->size.h - WEATHER_APP_LAYOUT_ARROW_LAYER_HEIGHT, 0, 0));
  layer_set_frame(&layout->down_arrow_layer, &down_arrow_layer_frame);
}

static const AnimationImplementation s_down_animation_implementation = {
  .update = &prv_down_animation_update,
};

static const AnimationImplementation s_up_animation_implementation = {
  .update = &prv_up_animation_update,
};

static const AnimationHandlers s_animation_handlers = {
    .stopped = &prv_animation_stopped,
};

static void prv_morph_weather_icons(KinoLayer *icon_layer, WeatherType from, WeatherType to,
                                    uint32_t duration) {
  uint32_t from_image_res_id =
      prv_get_resource_id_for_weather_type(from);
  KinoReel *from_reel = kino_reel_create_with_resource(from_image_res_id);
  KinoReel *to_reel = kino_reel_create_with_resource(prv_get_resource_id_for_weather_type(to));

  KinoReel *icon_reel = kino_reel_morph_square_create(from_reel, true);
  kino_reel_transform_set_to_reel(icon_reel, to_reel, true);
  kino_reel_transform_set_transform_duration(icon_reel, duration);

  kino_layer_set_reel(icon_layer, icon_reel, true);
  kino_layer_play(icon_layer);
}

void weather_app_layout_animate(WeatherAppLayout *layout, WeatherLocationForecast *new_forecast,
                                bool animate_down) {
  animation_unschedule_all();

  const uint32_t anim_duration = animate_down ? interpolate_moook_duration() :
                                                interpolate_moook_out_duration();
  layout->animation_state.next_forecast = new_forecast;
  Animation *animation = animation_create();
  animation_set_duration(animation, anim_duration);
  InterpolateInt64Function interpolation = animate_down ? interpolate_moook :
                                                          interpolate_moook_in_only;
  animation_set_custom_interpolation(animation, interpolation);
  animation_set_handlers(animation, s_animation_handlers, layout);
  const AnimationImplementation *implementation = animate_down ? &s_down_animation_implementation :
                                                                 &s_up_animation_implementation;
  animation_set_implementation(animation, implementation);
  animation_schedule(animation);

  prv_morph_weather_icons(&layout->current_weather_icon_layer,
                          layout->forecast->current_weather_type,
                          new_forecast->current_weather_type, anim_duration);

  prv_morph_weather_icons(&layout->tomorrow_weather_icon_layer,
                          layout->forecast->tomorrow_weather_type,
                          new_forecast->tomorrow_weather_type, anim_duration);
}
