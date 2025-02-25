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

#include "kickstart.h"

#include "applib/app.h"
#include "applib/graphics/text.h"
#include "applib/tick_timer_service.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/ui.h"
#include "apps/system_apps/timeline/text_node.h"
#include "kernel/pbl_malloc.h"
#include "applib/pbl_std/pbl_std.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "services/common/clock.h"
#include "services/normal/activity/health_util.h"
#include "util/size.h"
#include "util/string.h"
#include "util/time/time.h"
#include "util/trig.h"

#include <string.h>

#define ROBERT_SCREEN_RES (PBL_DISPLAY_WIDTH == 200 && PBL_DISPLAY_HEIGHT == 228)
#define SNOWY_SCREEN_RES (PBL_DISPLAY_WIDTH == 144 && PBL_DISPLAY_HEIGHT == 168)
#define SPALDING_SCREEN_RES (PBL_DISPLAY_WIDTH == 180 && PBL_DISPLAY_HEIGHT == 180)

////////////////////////////////////////////////////////////////////////////////////////////////////
// UI Utils

#if UNITTEST
static int16_t s_unobstructed_area_height = 0;

T_STATIC void prv_set_unobstructed_area_height(int16_t height) {
  s_unobstructed_area_height = height;
}
#endif

#define MULT_X(a, b) (b ? (1000 * a / b) : 0)
#define DIV_X(a) (a / 1000)

static GPoint prv_steps_to_point(int32_t cur, int32_t total, GRect frame) {
#if PBL_RECT
  /*  e    0    b
   *   ---------
   *   |       |
   *   |       |
   *   |       |
   *   |       |
   *   |       |
   *   ---------
   *  d         c
   */

  const int32_t top_right = frame.size.w / 2;
  const int32_t bot_right = frame.size.h + top_right;
  const int32_t bot_left = frame.size.w + bot_right;
  const int32_t top_left = frame.size.h + bot_left;
  const int32_t rect_perimeter = top_left + top_right;

  // limits calculated from length along perimeter starting from '0'
  const int32_t limit_b = total * top_right / rect_perimeter;
  const int32_t limit_c = total * bot_right / rect_perimeter;
  const int32_t limit_d = total * bot_left / rect_perimeter;
  const int32_t limit_e = total * top_left / rect_perimeter;

  if (cur <= limit_b) {
    // zone 0 - b
    return GPoint(frame.origin.x + DIV_X(frame.size.w * (500 + (MULT_X(cur, limit_b) / 2))),
                  frame.origin.y);
  } else if (cur <= limit_c) {
    // zone b - c
    return GPoint(frame.origin.x + frame.size.w,
                  frame.origin.y +
                      DIV_X(frame.size.h * MULT_X((cur - limit_b), (limit_c - limit_b))));
  } else if (cur <= limit_d) {
    // zone c - d
    return GPoint(frame.origin.x +
                      DIV_X(frame.size.w * (1000 - MULT_X((cur - limit_c), (limit_d - limit_c)))),
                  frame.origin.y + frame.size.h);
  } else if (cur <= limit_e) {
    // zone d - e
    return GPoint(frame.origin.x,
                  frame.origin.y +
                      DIV_X(frame.size.h * (1000 - MULT_X((cur - limit_d), (limit_e - limit_d)))));
  } else {
    // zone e - 0
    return GPoint(frame.origin.x +
                      DIV_X(frame.size.w / 2 * MULT_X((cur - limit_e), (total - limit_e))),
                  frame.origin.y);
  }
#elif PBL_ROUND
  // Simply a calculated point on the circumference
  const int32_t angle = DIV_X(360 * MULT_X(cur, total));
  return gpoint_from_polar(frame, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(angle));
#endif
}

#if PBL_RECT
static GPoint prv_inset_point(GRect *frame, GPoint outer_point, int32_t inset_amount) {
  // Insets the given point by the specified amount
  return (GPoint) {
    .x = MAX(inset_amount - 1, MIN(outer_point.x, frame->size.w - inset_amount)),
    .y = MAX(inset_amount - 1, MIN(outer_point.y, frame->size.h - inset_amount))
  };
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// UI Drawing

static void prv_draw_outer_ring(GContext *ctx, int32_t current, int32_t total,
                                int32_t fill_thickness, GRect frame, GColor color) {
  graphics_context_set_fill_color(ctx, color);

  const GRect outer_bounds = grect_inset(frame, GEdgeInsets(-1));

#if PBL_RECT
  GPoint start_outer_point = prv_steps_to_point(0, total, outer_bounds);
  GPoint start_inner_point = prv_inset_point(&frame, start_outer_point, fill_thickness);
  GPoint end_outer_point = prv_steps_to_point(current, total, outer_bounds);
  GPoint end_inner_point = prv_inset_point(&frame, end_outer_point, fill_thickness);

#if PBL_BW
  // Make sure we draw something if we have any steps
  if ((start_outer_point.y == end_outer_point.y) && (end_outer_point.x > start_outer_point.x) &&
      (end_outer_point.x - start_outer_point.x < 3)) {
    end_outer_point.x = start_outer_point.x + 3;
    end_inner_point.x = start_inner_point.x + 3;
  }
#endif

  const int32_t max_points = 20;

  GPath path = (GPath) {
    .points = app_zalloc_check(sizeof(GPoint) * max_points),
    .num_points = 0,
  };

  const int32_t top_right = frame.size.w / 2;
  const int32_t bot_right = frame.size.h + top_right;
  const int32_t bot_left = frame.size.w + bot_right;
  const int32_t top_left = frame.size.h + bot_left;
  const int32_t rect_perimeter = top_left + top_right;

  const int32_t corners[] = {0,
                             total * top_right / rect_perimeter,
                             total * bot_right / rect_perimeter,
                             total * bot_left / rect_perimeter,
                             total * top_left / rect_perimeter,
                             total};

  // start the path with start_outer_point
  path.points[path.num_points++] = start_outer_point;
  // loop through and add all the corners b/w start and end
  for (uint16_t i = 0; i < ARRAY_LENGTH(corners); i++) {
    if (corners[i] > 0 && corners[i] < current) {
      path.points[path.num_points++] = prv_steps_to_point(corners[i], total, outer_bounds);
    }
  }
  // add end outer and inner points
  path.points[path.num_points++] = end_outer_point;
  path.points[path.num_points++] = end_inner_point;
  // loop though backwards and add all the corners b/w end and start
  for (int i = ARRAY_LENGTH(corners) - 1; i >= 0; i--) {
    if (corners[i] > 0 && corners[i] < current) {
      path.points[path.num_points++] = prv_inset_point(
          &frame, prv_steps_to_point(corners[i], total, outer_bounds), fill_thickness);
    }
  }
  // add start_inner_point
  path.points[path.num_points++] = start_inner_point;

  gpath_draw_filled(ctx, &path);

#if PBL_COLOR
  graphics_context_set_stroke_color(ctx, color);
  gpath_draw_outline(ctx, &path);
#else
  graphics_context_set_stroke_color(ctx, GColorWhite);
  GRect inner_bounds = grect_inset(outer_bounds, GEdgeInsets(fill_thickness));
  graphics_draw_rect(ctx, &inner_bounds);
  inner_bounds = grect_inset(inner_bounds, GEdgeInsets(-1));
  graphics_draw_rect(ctx, &inner_bounds);
#endif

  app_free(path.points);

#elif PBL_ROUND
  const int32_t degree = total ? (360 * current / total) : 0;
  const int32_t to_angle = DEG_TO_TRIGANGLE(degree);
  graphics_fill_radial(ctx, outer_bounds, GOvalScaleModeFitCircle, fill_thickness, 0, to_angle);
#endif
}

#if PBL_ROUND
static void prv_draw_outer_dots(GContext *ctx, GRect bounds) {
  const GRect inset_bounds = grect_inset(bounds, GEdgeInsets(6));

  // outer dots placed along inside circumference
  const int num_dots = 12;
  for (int i = 0; i < num_dots; i++) {
    GPoint pos = gpoint_from_polar(inset_bounds, GOvalScaleModeFitCircle,
                                   DEG_TO_TRIGANGLE(i * 360 / num_dots));

    const int dot_radius = 2;
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_circle(ctx, pos, dot_radius);
  }
}
#endif

static void prv_draw_goal_line(GContext *ctx, int32_t current_progress, int32_t total_progress,
                               int32_t line_length, int32_t line_width, GRect frame, GColor color) {
  const GPoint line_outer_point = prv_steps_to_point(current_progress, total_progress, frame);

#if PBL_RECT
  const GPoint line_inner_point = prv_inset_point(&frame, line_outer_point, line_length);
#elif PBL_ROUND
  const GRect inner_bounds = grect_inset(frame, GEdgeInsets(line_length));
  const GPoint line_inner_point = prv_steps_to_point(current_progress,
                                                     total_progress, inner_bounds);
#endif

  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, line_width);
  graphics_draw_line(ctx, line_inner_point, line_outer_point);
}

#if ROBERT_SCREEN_RES
static void prv_draw_separator(GContext *ctx, GRect bounds, GColor color) {
  bounds.origin.y += 111; // top offset

  GPoint p1 = bounds.origin;
  GPoint p2 = p1;
  p2.x += bounds.size.w;

  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, p1, p2);
}
#endif

static void prv_draw_steps_and_shoe(GContext *ctx, const char *steps_buffer, GFont font,
                                    GRect bounds, GColor color, GBitmap *shoe_icon,
                                    bool screen_is_obstructed, bool has_bpm) {
#if PBL_BW
  bounds.origin.y += screen_is_obstructed ? (has_bpm ? 74 : 66) : (has_bpm ? 114 : 96);
#elif ROBERT_SCREEN_RES
  bounds.origin.y += screen_is_obstructed ? 113 : 158;
#elif SNOWY_SCREEN_RES
  if (screen_is_obstructed) {
    bounds = grect_inset(bounds, GEdgeInsets(0, 20));
  }
#endif

  GRect icon_bounds = gbitmap_get_bounds(shoe_icon);
  icon_bounds.origin = bounds.origin;
#if PBL_BW
  icon_bounds.origin.x += 23; // icon left offset
  icon_bounds.origin.y += 9; // icon top offset
#elif ROBERT_SCREEN_RES
  icon_bounds.origin.y += (46 - icon_bounds.size.h); // icon top offset
#elif SNOWY_SCREEN_RES
  icon_bounds.origin.x = screen_is_obstructed ? bounds.origin.x // icon_left offset
                                              : (bounds.size.w / 2) - (icon_bounds.size.w / 2);
  icon_bounds.origin.y += screen_is_obstructed ? 84 : 22; // icon top offset
#elif SPALDING_SCREEN_RES
  icon_bounds.origin.x = (bounds.size.w / 2) - (icon_bounds.size.w / 2);
  icon_bounds.origin.y += 27; // icon top offset
#endif

  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, shoe_icon, &icon_bounds);

#if PBL_BW
  const GTextAlignment alignment = GTextAlignmentLeft;
  bounds.origin.x += 62; // steps text left offset
#elif ROBERT_SCREEN_RES
  const GTextAlignment alignment = GTextAlignmentRight;
#elif SNOWY_SCREEN_RES
  const GTextAlignment alignment = screen_is_obstructed ? GTextAlignmentRight: GTextAlignmentCenter;
  bounds.origin.y += screen_is_obstructed ? 65 : 108; // steps text top offset
#elif SPALDING_SCREEN_RES
  const GTextAlignment alignment = GTextAlignmentCenter;
  bounds.origin.y += 113; // steps text top offset
#endif

  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, steps_buffer, font, bounds, GTextOverflowModeFill, alignment, NULL);
}

static void prv_draw_time(GContext *ctx, GFont time_font, GFont am_pm_font, GRect bounds,
                          bool screen_is_obstructed, bool has_bpm) {
  GTextNodeHorizontal *horiz_container = graphics_text_node_create_horizontal(MAX_TEXT_NODES);
  GTextNodeContainer *container = &horiz_container->container;
  horiz_container->horizontal_alignment = GTextAlignmentCenter;

  char time_buffer[8];
  char am_pm_buffer[4];

  const time_t now = rtc_get_time();
  /// Current time in 24 or 12 hour
  const char *time_fmt = clock_is_24h_style() ? "%R" : "%l:%M";
  strftime(time_buffer, sizeof(time_buffer), time_fmt, pbl_override_localtime(&now));
  health_util_create_text_node_with_text(
      string_strip_leading_whitespace(time_buffer),
      time_font, GColorWhite, container);

  if (!clock_is_24h_style()) {
    /// AM/PM for the current time
    strftime(am_pm_buffer, sizeof(am_pm_buffer), "%p", pbl_override_localtime(&now));
    health_util_create_text_node_with_text(
        am_pm_buffer, am_pm_font, GColorWhite, container);
  }

#if PBL_BW
  bounds.origin.y = screen_is_obstructed ? (has_bpm ? 13 : 23) : (has_bpm ? 36 : 53);
#elif ROBERT_SCREEN_RES
  bounds.origin.y = screen_is_obstructed ? -12 : 6;
#elif SNOWY_SCREEN_RES
  bounds.origin.y = screen_is_obstructed ? 4 : 47;
#elif SPALDING_SCREEN_RES
  bounds.origin.y = 50;
#endif

  graphics_text_node_draw(&container->node, ctx, &bounds, NULL, NULL);
  graphics_text_node_destroy(&container->node);
}

#if PBL_BW || ROBERT_SCREEN_RES
static void prv_draw_bpm(GContext *ctx, int32_t current_bpm, GFont font, GBitmap *heart_icon,
                         GRect bounds, bool screen_is_obstructed, void *i18n_owner) {
#if PBL_BW
  bounds.origin.y += screen_is_obstructed ? 52 : 89;
#elif ROBERT_SCREEN_RES
  bounds.origin.y += screen_is_obstructed ? 80 : 123;
#endif

  GRect icon_bounds = gbitmap_get_bounds(heart_icon);
  icon_bounds.origin = bounds.origin;
#if PBL_BW
  icon_bounds.origin.x += 20; // icon left offset
#endif

  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, heart_icon, &icon_bounds);

  char bpm_text[16];
  snprintf(bpm_text, sizeof(bpm_text), i18n_get("%d BPM", i18n_owner), current_bpm);

#if PBL_BW
  bounds.origin.x += 62; // bpm text left offset
#endif
  bounds.origin.y -= PBL_IF_BW_ELSE(5, 8); // bpm text top offset

  const GTextAlignment alignment = PBL_IF_BW_ELSE(GTextAlignmentLeft, GTextAlignmentRight);

  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorWhite));
  graphics_draw_text(ctx, bpm_text, font, bounds, GTextOverflowModeFill, alignment, NULL);
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// Update Proc

static void prv_base_layer_update_proc(Layer *layer, GContext *ctx) {
  KickstartData *data = window_get_user_data(layer_get_window(layer));
  GRect bounds = layer->bounds;

  GRect unobstructed_bounds;
  layer_get_unobstructed_bounds(layer, &unobstructed_bounds);

#if UNITTEST
  unobstructed_bounds.size.h = bounds.size.h - s_unobstructed_area_height;
#endif

  const bool screen_is_obstructed = (unobstructed_bounds.size.h != bounds.size.h);

  bounds.size.h = unobstructed_bounds.size.h;

#if SNOWY_SCREEN_RES
  const int16_t fill_thickness = screen_is_obstructed ? 10 : 11;
#elif ROBERT_SCREEN_RES
  const int16_t fill_thickness = screen_is_obstructed ? 5 : 13;
#elif SPALDING_SCREEN_RES
  const int16_t fill_thickness = (bounds.size.h - grect_inset(bounds, GEdgeInsets(15)).size.h) / 2;
#endif

#if PBL_COLOR
  const bool has_passed_goal = (data->current_steps > data->typical_steps);
  const GColor fill_color = has_passed_goal ? GColorJaegerGreen : GColorVividCerulean;
  const GColor text_color = has_passed_goal ? GColorJaegerGreen : GColorVividCerulean;
#if SNOWY_SCREEN_RES
  GBitmap *shoe =
      has_passed_goal ? (screen_is_obstructed ? &data->shoe_green_small : &data->shoe_green)
                      : (screen_is_obstructed ? &data->shoe_blue_small : &data->shoe_blue);
#else
  GBitmap *shoe = has_passed_goal ? &data->shoe_green : &data->shoe_blue;
#endif // SNOWY_SCREEN_RES
#else
  const GColor fill_color = GColorDarkGray;
  const GColor text_color = GColorWhite;
  GBitmap *shoe = &data->shoe;
#endif // PBL_COLOR

#if PBL_ROUND
  prv_draw_outer_dots(ctx, bounds);
#endif

  // draw outer ring
  prv_draw_outer_ring(ctx, data->current_steps, data->daily_steps_avg,
                      fill_thickness, bounds, fill_color);

  const int goal_line_length = PBL_IF_COLOR_ELSE(fill_thickness + 3, 12);
  const int goal_line_width = 4;

  // draw yellow goal line
  prv_draw_goal_line(ctx, data->typical_steps, MAX(data->daily_steps_avg, data->typical_steps),
                     goal_line_length, goal_line_width, bounds, GColorYellow);

  const bool has_bpm = (data->current_bpm > 0);

  // draw time
  prv_draw_time(ctx, data->time_font, PBL_IF_COLOR_ELSE(data->am_pm_font, data->time_font),
                bounds, screen_is_obstructed, has_bpm);

#if ROBERT_SCREEN_RES
  bounds = grect_inset(bounds, GEdgeInsets(0, 25));

  // draw separator
  if (!screen_is_obstructed) {
    prv_draw_separator(ctx, bounds, GColorWhite);
  }
#endif

#if PBL_BW || ROBERT_SCREEN_RES
  // draw bpm and heart
  if (has_bpm) {
    prv_draw_bpm(ctx, data->current_bpm, data->steps_font, &data->heart_icon, bounds,
                 screen_is_obstructed, data);
  }
#endif

  // draw steps and shoe
  prv_draw_steps_and_shoe(ctx, data->steps_buffer, data->steps_font, bounds, text_color, shoe,
                          screen_is_obstructed, has_bpm);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Data

static void prv_update_steps_buffer(KickstartData *data) {
  const int thousands = data->current_steps / 1000;
  const int hundreds = data->current_steps % 1000;
  if (thousands) {
    /// Step count greater than 1000 with a thousands separator
    snprintf(data->steps_buffer, sizeof(data->steps_buffer), i18n_get("%d,%03d", data),
             thousands, hundreds);
  } else {
    /// Step count less than 1000
    snprintf(data->steps_buffer, sizeof(data->steps_buffer), i18n_get("%d", data),
             hundreds);
  }
  layer_mark_dirty(&data->base_layer);
}

static void prv_update_current_steps(KickstartData *data) {
  data->current_steps = health_service_sum_today(HealthMetricStepCount);
  prv_update_steps_buffer(data);
}

static void prv_update_typical_steps(KickstartData *data) {
  data->typical_steps = health_service_sum_averaged(HealthMetricStepCount,
                                                time_start_of_today(),
                                                rtc_get_time(),
                                                HealthServiceTimeScopeWeekly);
}

static void prv_update_daily_steps_avg(KickstartData *data) {
  data->daily_steps_avg = health_service_sum_averaged(HealthMetricStepCount,
                                                  time_start_of_today(),
                                                  time_start_of_today() + SECONDS_PER_DAY,
                                                  HealthServiceTimeScopeWeekly);
}

static void prv_update_hrm_bpm(KickstartData *data) {
  data->current_bpm = health_service_peek_current_value(HealthMetricHeartRateBPM);
}

static void prv_normalize_data(KickstartData *data) {
  // If the user's daily avg steps are very low (QA or a brand new pebble user), bump the value
  // to a slightly more reasonable number.
  // This fixes an integer rounding problem when the value is very small (PBL-43717)
  const int min_daily_steps_avg = 100;
  data->daily_steps_avg = MAX(data->daily_steps_avg, min_daily_steps_avg);

  // increase daily avg 5% more than current steps if current steps is more than 95% of daily avg
  if (data->current_steps >= (data->daily_steps_avg * 95 / 100)) {
    data->daily_steps_avg = data->current_steps * 105 / 100;
  }
}

static void prv_update_data(KickstartData *data) {
  prv_update_current_steps(data);
  prv_update_typical_steps(data);
  prv_update_daily_steps_avg(data);
  prv_update_hrm_bpm(data);
  prv_normalize_data(data);
  layer_mark_dirty(&data->base_layer);
}

#if UNITTEST
T_STATIC void prv_set_data(KickstartData *data, int32_t current_steps,
                           int32_t typical_steps, int32_t daily_steps_avg, int32_t current_bpm) {
  data->current_steps = current_steps;
  data->typical_steps = typical_steps;
  data->daily_steps_avg = daily_steps_avg;
  data->current_bpm = current_bpm;
  prv_normalize_data(data);
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// Handlers

static void prv_health_service_events_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate) {
    prv_update_current_steps(context);
  }
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits changed) {
  KickstartData *data = app_state_get_user_data();
  prv_update_data(data);
}

T_STATIC void prv_window_load_handler(Window *window) {
  KickstartData *data = window_get_user_data(window);

  // load resources
#if PBL_BW
  gbitmap_init_with_resource(&data->shoe, RESOURCE_ID_STRIDE_SHOE);
  gbitmap_init_with_resource(&data->heart_icon, RESOURCE_ID_WORKOUT_APP_HEART);
  data->steps_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  data->time_font = fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM);
#else
  gbitmap_init_with_resource(&data->shoe_blue, RESOURCE_ID_STRIDE_SHOE_BLUE);
  gbitmap_init_with_resource(&data->shoe_green, RESOURCE_ID_STRIDE_SHOE_GREEN);
#if ROBERT_SCREEN_RES
  gbitmap_init_with_resource(&data->heart_icon, RESOURCE_ID_STRIDE_HEART);
  data->steps_font = fonts_get_system_font(FONT_KEY_AGENCY_FB_46_NUMBERS_AM_PM);
  data->time_font = fonts_get_system_font(FONT_KEY_AGENCY_FB_88_NUMBERS_AM_PM);
  data->am_pm_font = fonts_get_system_font(FONT_KEY_AGENCY_FB_88_THIN_NUMBERS_AM_PM);
#elif SNOWY_SCREEN_RES || SPALDING_SCREEN_RES
#if PBL_RECT
  gbitmap_init_with_resource(&data->shoe_blue_small, RESOURCE_ID_STRIDE_SHOE_BLUE_SMALL);
  gbitmap_init_with_resource(&data->shoe_green_small, RESOURCE_ID_STRIDE_SHOE_GREEN_SMALL);
#endif // PBL_RECT
  data->steps_font = fonts_get_system_font(FONT_KEY_AGENCY_FB_36_NUMBERS_AM_PM);
  data->time_font = fonts_get_system_font(FONT_KEY_AGENCY_FB_60_NUMBERS_AM_PM);
  data->am_pm_font = fonts_get_system_font(FONT_KEY_AGENCY_FB_60_THIN_NUMBERS_AM_PM);
#else
#error "Undefined screen size"
#endif // ROBERT_SCREEN_RES
#endif // PBL_BW

  Layer *window_layer = window_get_root_layer(window);

  // set window background
  window_set_background_color(window, GColorBlack);

  // set up the base layer
  layer_init(&data->base_layer, &window_layer->bounds);
  layer_set_update_proc(&data->base_layer, prv_base_layer_update_proc);
  layer_add_child(window_layer, &data->base_layer);

  // update steps and time
  prv_update_steps_buffer(data);

  // subscribe to health service
  health_service_events_subscribe(prv_health_service_events_handler, data);

  // subscribe to tick timer for minute ticks
  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
}

T_STATIC void prv_window_unload_handler(Window *window) {
  KickstartData *data = window_get_user_data(window);

  // unsubscribe from service events
  health_service_events_unsubscribe();
  tick_timer_service_unsubscribe();

  // deinit everything
#if PBL_BW
  gbitmap_deinit(&data->shoe);
#else
  gbitmap_deinit(&data->shoe_blue);
  gbitmap_deinit(&data->shoe_green);
#endif
#if PBL_COLOR && SNOWY_SCREEN_RES
  gbitmap_deinit(&data->shoe_blue_small);
  gbitmap_deinit(&data->shoe_green_small);
#endif
  gbitmap_deinit(&data->heart_icon);
  layer_deinit(&data->base_layer);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// App Main

static void prv_main(void) {
  KickstartData *data = app_zalloc_check(sizeof(KickstartData));
  app_state_set_user_data(data);

  prv_update_data(data);

  window_init(&data->window, WINDOW_NAME("Kickstart"));
  window_set_user_data(&data->window, data);
  window_set_window_handlers(&data->window, &(WindowHandlers) {
    .load = prv_window_load_handler,
    .unload = prv_window_unload_handler,
  });
  app_window_stack_push(&data->window, true);

  app_event_loop();

  window_deinit(&data->window);
  i18n_free_all(data);
  app_free(data);
}

const PebbleProcessMd* kickstart_get_app_info() {
  static const PebbleProcessMdSystem s_app_md = {
    .common = {
      // UUID: 3af858c3-16cb-4561-91e7-f1ad2df8725f
      .uuid = {0x3a, 0xf8, 0x58, 0xc3, 0x16, 0xcb, 0x45, 0x61,
               0x91, 0xe7, 0xf1, 0xad, 0x2d, 0xf8, 0x72, 0x5f},
      .main_func = prv_main,
      .process_type = ProcessTypeWatchface,
    },
    .icon_resource_id = RESOURCE_ID_MENU_ICON_KICKSTART_WATCH,
    .name = "Kickstart"
  };
  return (const PebbleProcessMd*) &s_app_md;
}
