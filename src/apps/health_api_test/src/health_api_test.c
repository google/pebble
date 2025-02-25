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
#include <inttypes.h>


// -------------------------------------------------------------------------------
// Defines
// Rect: left, top, width, height
#define STEPS_HEIGHT 45
#define STEPS_TOP ((DISP_ROWS - 3 * STEPS_HEIGHT)/2)

#if PBL_RECT
  #define DISP_COLS 144
  #define DISP_ROWS 168
#elif PBL_ROUND
  #define DISP_COLS 180
  #define DISP_ROWS 180
#endif

#define CUR_STEP_RECT GRect(0, STEPS_TOP, DISP_COLS, STEPS_HEIGHT)
#define TIME_RECT GRect(0, STEPS_TOP + STEPS_HEIGHT, DISP_COLS, STEPS_HEIGHT)
#define DELTA_STEP_RECT GRect(0, STEPS_TOP + 2 * STEPS_HEIGHT, 144, STEPS_HEIGHT)
#define TEXT_RECT GRect(0, STEPS_TOP + 3 * STEPS_HEIGHT - 3, \
                        DISP_COLS, DISP_ROWS - STEPS_HEIGHT * 3 + 3)

#define CURRENT_STEP_AVG 500
#define DAILY_STEP_AVG 1000

#define HEART_RATE_THRESHOLD 80

// Persist keys
typedef enum {
  AppPersistKeyLapSteps = 0,
} AppPersistKey;


// -------------------------------------------------------------------------------
// Structures

typedef struct {
  TextLayer *text_layer;
  char text[256];
} ResultsCard;

typedef struct {
  char dialog_text[256];
  SimpleMenuItem *menu_items;
  SimpleMenuLayer *menu_layer;
} DebugCard;

typedef struct {
  TextLayer *cur_step_layer;
  TextLayer *time_layer;
  TextLayer *delta_step_layer;
  TextLayer *msg_layer;
  char cur_step_text[32];
  char time_text[32];
  char delta_step_text[32];
  char msg_text[256];
} StepsCard;

typedef struct {
  TextLayer *text_layer;
  char text[256];
} SleepCard;

typedef struct {
  TextLayer *text_layer;
  char text[256];
} HeartRateCard;

// App globals
typedef struct {
  Window *steps_window;
  Window *sleep_window;
  Window *debug_window;
  Window *results_window;
  Window *hr_window;
  StepsCard steps_card;
  SleepCard sleep_card;
  DebugCard debug_card;
  ResultsCard results_card;
  HeartRateCard hr_card;
  uint32_t steps_offset;
  uint32_t cur_steps;
  uint32_t lap_steps;
  time_t bed_time_utc;
  time_t awake_time_utc;
  uint32_t cur_hr_bpm;
  uint32_t resting_hr_bpm;
  uint32_t num_hr_alerts;
  HealthMetricAlert *hr_alert;
} HealthAPITestAppData;

static HealthAPITestAppData *s_data;

static void steps_update_text(HealthAPITestAppData *data);
static void prv_debug_cmd_sleep_sessions(int index, void *context);
static void results_update_text(HealthAPITestAppData *data, const char* text);
static void prv_hr_update_text(HealthAPITestAppData *data);


// -------------------------------------------------------------------------------
// Return current time in ms
static uint64_t prv_ms(void) {
  time_t cur_sec;
  uint16_t cur_ms = time_ms(&cur_sec, NULL);
  return ((uint64_t)cur_sec * 1000) + cur_ms;
}


// -------------------------------------------------------------------------------
static void prv_convert_seconds_to_time(uint32_t secs_after_midnight, char *text,
                                        int text_len) {
  uint32_t minutes_after_midnight = secs_after_midnight / SECONDS_PER_MINUTE;
  uint32_t hour = minutes_after_midnight / MINUTES_PER_HOUR;
  uint32_t minute = minutes_after_midnight % MINUTES_PER_HOUR;
  snprintf(text, text_len, "%d:%02d", (int)hour, (int)minute);
}


// -----------------------------------------------------------------------------------------
static void prv_display_alert(const char *text) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, text);
  window_stack_push(s_data->results_window, true /* Animated */);
  results_update_text(s_data, text);
}


// -----------------------------------------------------------------------------------------
static void prv_safe_strcat(char* dst, const char* src, int dst_space) {
  int remaining = dst_space - strlen(dst);
  if (dst_space > 0) {
    strncat(dst, src, remaining);
  }
  dst[dst_space-1] = 0;
}


// -----------------------------------------------------------------------------------------
static void prv_display_scalar_history_alert(HealthAPITestAppData *data, const char *title,
                                             HealthMetric metric) {
  strcpy(data->debug_card.dialog_text, title);

  // Get History
  time_t day_start = time_start_of_today();
  for (int i = 0; i < 30; i++) {
    HealthValue value = health_service_sum(metric, day_start, day_start + SECONDS_PER_DAY);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "%d: %d", i, (int)value);
    char temp[32];
    snprintf(temp, sizeof(temp), "\n%d: %d", i, (int)value);
    prv_safe_strcat(data->debug_card.dialog_text, temp, sizeof(data->debug_card.dialog_text));
    day_start -= SECONDS_PER_DAY;
  }

  prv_display_alert(data->debug_card.dialog_text);
}


// -----------------------------------------------------------------------------------------
static void prv_display_seconds_history_alert(HealthAPITestAppData *data, const char *title,
                                              HealthMetric metric) {
  strcpy(data->debug_card.dialog_text, title);

  // Get History
  time_t day_start = time_start_of_today();
  for (int i = 0; i < 30; i++) {
    HealthValue value = health_service_sum(metric, day_start, day_start + SECONDS_PER_DAY);
    char elapsed[8];
    prv_convert_seconds_to_time(value, elapsed, sizeof(elapsed));
    APP_LOG(APP_LOG_LEVEL_DEBUG, "%d: %s", i, elapsed);
    char temp[32];
    snprintf(temp, sizeof(temp), "\n%d: %s", i, elapsed);
    prv_safe_strcat(data->debug_card.dialog_text, temp, sizeof(data->debug_card.dialog_text));
    day_start -= SECONDS_PER_DAY;
  }

  prv_display_alert(data->debug_card.dialog_text);
}


// -------------------------------------------------------------------------------
static void sleep_select_click_handler(ClickRecognizerRef recognizer, void *context) {
}


// -------------------------------------------------------------------------------
static void sleep_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_pop(true);
  window_stack_push(data->hr_window, true);
}


// -------------------------------------------------------------------------------
static void sleep_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_pop(true);
  window_stack_push(data->steps_window, true);
}

// -------------------------------------------------------------------------------
static void sleep_down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_push(data->debug_window, true /* Animated */);
}


// -------------------------------------------------------------------------------
static void sleep_click_config_provider(void *context) {
  const int k_long_press_timeout_ms = 1000;
  window_single_click_subscribe(BUTTON_ID_SELECT, sleep_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, sleep_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, sleep_down_click_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, k_long_press_timeout_ms,
                              sleep_down_long_click_handler, NULL);
}


// -------------------------------------------------------------------------------
static void sleep_update_text(HealthAPITestAppData *data) {
  if (!data->sleep_card.text_layer) {
    return;
  }
  int32_t sleep_total_sec = health_service_sum_today(HealthMetricSleepSeconds);
  int32_t sleep_deep_sec = health_service_sum_today(HealthMetricSleepRestfulSeconds);

  // This updates s_data->bed_time_utc and s_data->awake_time_utc
  prv_debug_cmd_sleep_sessions(0 /*index*/, s_data);

  char bed_time_str[8];
  struct tm *local_tm = localtime(&s_data->bed_time_utc);
  strftime(bed_time_str, sizeof(bed_time_str),  "%H:%M", local_tm);

  char wake_time_str[8];
  local_tm = localtime(&s_data->awake_time_utc);
  strftime(wake_time_str, sizeof(wake_time_str),  "%H:%M", local_tm);


  char total_sleep_str[8];
  char deep_sleep_str[8];
  prv_convert_seconds_to_time(sleep_total_sec, total_sleep_str, sizeof(total_sleep_str));
  prv_convert_seconds_to_time(sleep_deep_sec, deep_sleep_str, sizeof(deep_sleep_str));

  snprintf(data->sleep_card.text, sizeof(data->sleep_card.text),
           "Zzz..\ntotal: %s\ndeep: %s\nenter: %s\nexit: %s",
           total_sleep_str, deep_sleep_str, bed_time_str, wake_time_str);
  text_layer_set_text(data->sleep_card.text_layer, data->sleep_card.text);
}


// -------------------------------------------------------------------------------
static void sleep_window_load(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  Layer *window_layer = window_get_root_layer(window);
  GRect root_bounds = layer_get_bounds(window_layer);

  data->sleep_card.text_layer = text_layer_create(root_bounds);
  text_layer_set_text_alignment(data->sleep_card.text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(data->sleep_card.text_layer, GColorClear);
  text_layer_set_font(data->sleep_card.text_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_color(data->sleep_card.text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(data->sleep_card.text_layer));

  // Update UI
  sleep_update_text(data);
}


// -------------------------------------------------------------------------------
static void sleep_window_unload(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  text_layer_destroy(data->sleep_card.text_layer);
  data->sleep_card.text_layer = NULL;
}


// -------------------------------------------------------------------------------
static void steps_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;

  data->lap_steps = data->cur_steps;
  persist_write_int(AppPersistKeyLapSteps, data->lap_steps);
  text_layer_set_text(data->steps_card.delta_step_layer, "0");
}

// -------------------------------------------------------------------------------
static void steps_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
#if TEST_MODE
  data->steps_offset += 5;
  data->cur_steps += 5;
  layer_mark_dirty(window_get_root_layer(data->steps_window));
  steps_update_text(data);
  return;
#endif
  window_stack_pop(true);
  window_stack_push(data->sleep_window, true /* Animated */);
}


// -------------------------------------------------------------------------------
static void steps_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_pop(true);
  window_stack_push(data->hr_window, true /* Animated */);
}


// -------------------------------------------------------------------------------
static void steps_down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_push(data->debug_window, true /* Animated */);
}


// -------------------------------------------------------------------------------
static void steps_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, steps_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, steps_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, steps_down_click_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, 1000, steps_down_long_click_handler, NULL);
}


// -------------------------------------------------------------------------------
static void steps_update_text(HealthAPITestAppData *data) {
  // Show total steps
  if (data->steps_card.cur_step_layer) {
    snprintf(data->steps_card.cur_step_text, sizeof(data->steps_card.cur_step_text),
             "%d", (int) data->cur_steps);
    text_layer_set_text(data->steps_card.cur_step_layer, data->steps_card.cur_step_text);
  }

  // Show time
  if (data->steps_card.time_layer) {
    time_t now = time(NULL);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "new time: %d", (int) now);
    struct tm *local_tm = localtime(&now);
    strftime(data->steps_card.time_text, sizeof(data->steps_card.time_text),
             "%I:%M", local_tm);
    text_layer_set_text(data->steps_card.time_layer, data->steps_card.time_text);
  }


  if (data->lap_steps > data->cur_steps) {
    // We probably encountered a midnight rollover, reset the persistent storage too
    data->lap_steps = data->cur_steps;
    persist_write_int(AppPersistKeyLapSteps, data->lap_steps);
  }
  if (data->steps_card.delta_step_layer) {
    if (data->lap_steps) {
      snprintf(data->steps_card.delta_step_text, sizeof(data->steps_card.delta_step_text),
               "%d", (int) (data->cur_steps - data->lap_steps));
      text_layer_set_text(data->steps_card.delta_step_layer, data->steps_card.delta_step_text);
    }
  }
}


// -------------------------------------------------------------------------------
static void prv_health_event_handler(HealthEventType event,
                                     void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;

  if (event == HealthEventMovementUpdate) {
    // Test the peek function
    int32_t peek_steps = health_service_sum_today(HealthMetricStepCount);

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Got steps update event. (peek value: %d)",
            (int)peek_steps);

    data->cur_steps = peek_steps + data->steps_offset;
    steps_update_text(data);

  } else if (event == HealthEventSignificantUpdate) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Got significant update event");
    steps_update_text(data);

  } else if (event == HealthEventSleepUpdate) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Got sleep update event");

  } else if (event == HealthEventHeartRateUpdate) {
    prv_hr_update_text(data);

  } else if (event == HealthEventMetricAlert) {
    HealthValue now_bpm = health_service_peek_current_value(HealthMetricHeartRateBPM);
    APP_LOG(APP_LOG_LEVEL_INFO, "Crossed HR threshold of %d. HR: %"PRIi32" ",
            (int)HEART_RATE_THRESHOLD, now_bpm);
    data->num_hr_alerts++;
    prv_hr_update_text(data);
  }
}


// -------------------------------------------------------------------------------
static void steps_base_layer_update_proc(Layer *layer, GContext* ctx) {
  const GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Show the battery level in the outer circle
  BatteryChargeState charge_state = battery_state_service_peek();
  const int percent = charge_state.charge_percent;

  graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, 15, 0, TRIG_MAX_ANGLE);

  graphics_context_set_stroke_color(ctx, GColorJaegerGreen);
  graphics_context_set_fill_color(ctx, GColorJaegerGreen);
  graphics_fill_radial(ctx, bounds, GOvalScaleModeFitCircle, 15, 0, TRIG_MAX_ANGLE * percent / 100);
}


// -------------------------------------------------------------------------------
static void handle_battery(BatteryChargeState charge_state) {
  layer_mark_dirty(window_get_root_layer(s_data->steps_window));
}


// -------------------------------------------------------------------------------
static void handle_minute_tick(struct tm* tick_time, TimeUnits units_changed) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Got minute update");
  steps_update_text(s_data);
}


// -------------------------------------------------------------------------------
static void steps_window_load(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  Layer *window_layer = window_get_root_layer(window);
  GRect root_bounds = layer_get_bounds(window_layer);

  layer_set_update_proc(window_layer, steps_base_layer_update_proc);

  // Total steps
  data->steps_card.cur_step_layer = text_layer_create(CUR_STEP_RECT);
  text_layer_set_text_alignment(data->steps_card.cur_step_layer, GTextAlignmentCenter);
  text_layer_set_font(data->steps_card.cur_step_layer,
                      fonts_get_system_font(FONT_KEY_LECO_38_BOLD_NUMBERS));
  text_layer_set_background_color(data->steps_card.cur_step_layer, GColorClear);
  text_layer_set_text_color(data->steps_card.cur_step_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(data->steps_card.cur_step_layer));

  // Time
  data->steps_card.time_layer = text_layer_create(TIME_RECT);
  text_layer_set_text_alignment(data->steps_card.time_layer, GTextAlignmentCenter);
  text_layer_set_font(data->steps_card.time_layer,
                      fonts_get_system_font(FONT_KEY_LECO_38_BOLD_NUMBERS));
  text_layer_set_background_color(data->steps_card.time_layer, GColorClear);
  text_layer_set_text_color(data->steps_card.time_layer, GColorElectricBlue);
  layer_add_child(window_layer, text_layer_get_layer(data->steps_card.time_layer));

  // Lap counter
  data->steps_card.delta_step_layer = text_layer_create(DELTA_STEP_RECT);
  text_layer_set_text_alignment(data->steps_card.delta_step_layer, GTextAlignmentCenter);
  text_layer_set_font(data->steps_card.delta_step_layer,
                      fonts_get_system_font(FONT_KEY_LECO_38_BOLD_NUMBERS));
  text_layer_set_background_color(data->steps_card.delta_step_layer, GColorClear);
  text_layer_set_text_color(data->steps_card.delta_step_layer, GColorLightGray);
  layer_add_child(window_layer, text_layer_get_layer(data->steps_card.delta_step_layer));

  // "Tracking disabled" message
  data->steps_card.msg_layer = text_layer_create(root_bounds);
  text_layer_set_text_alignment(data->steps_card.msg_layer, GTextAlignmentCenter);
  text_layer_set_font(data->steps_card.msg_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_background_color(data->steps_card.msg_layer, GColorClear);
  text_layer_set_text_color(data->steps_card.msg_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(data->steps_card.msg_layer));
  snprintf(data->steps_card.msg_text, sizeof(data->steps_card.msg_text),
           "Tracking disabled\n\nHold down button for settings menu");
  text_layer_set_text(data->steps_card.msg_layer, data->steps_card.msg_text);

  // Init step and sleep data
  int32_t peek_steps = health_service_sum_today(HealthMetricStepCount);
  data->cur_steps = peek_steps + data->steps_offset;
  data->lap_steps = persist_read_int(AppPersistKeyLapSteps);
  if (data->lap_steps > data->cur_steps) {
    // Probably left over since a midnight rollover, reset the persistent storage
    data->lap_steps = data->cur_steps;
    persist_write_int(AppPersistKeyLapSteps, data->lap_steps);
  }

  // Update UI
  steps_update_text(data);

  // Subscribe to health update events
  health_service_events_subscribe(prv_health_event_handler, data);

  // Set a heart rate alert
  HealthMetricAlert *alert = health_service_register_metric_alert(HealthMetricHeartRateBPM, 80);
  APP_LOG(APP_LOG_LEVEL_INFO, "health metric alert: %p", alert);
  if (alert == NULL) {
    prv_display_alert("Can't register HR alert");
  }
  data->hr_alert = alert;

  // Subscribe to time and battery updates
  tick_timer_service_subscribe(MINUTE_UNIT, handle_minute_tick);
  battery_state_service_subscribe(handle_battery);
}


// -------------------------------------------------------------------------------
static void steps_window_unload(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  text_layer_destroy(data->steps_card.cur_step_layer);
  data->steps_card.cur_step_layer = NULL;
  text_layer_destroy(data->steps_card.time_layer);
  data->steps_card.time_layer = NULL;
  text_layer_destroy(data->steps_card.delta_step_layer);
  data->steps_card.delta_step_layer = NULL;
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
}


// -------------------------------------------------------------------------------
static void steps_window_appear(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  layer_set_hidden(text_layer_get_layer(data->steps_card.msg_layer), true);
  layer_set_hidden(text_layer_get_layer(data->steps_card.cur_step_layer), false);
  layer_set_hidden(text_layer_get_layer(data->steps_card.time_layer), false);
  layer_set_hidden(text_layer_get_layer(data->steps_card.delta_step_layer), false);
}

// -------------------------------------------------------------------------------
static void prv_results_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop(true);
}


// -------------------------------------------------------------------------------
static void results_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, prv_results_back_click_handler);
}


// -------------------------------------------------------------------------------
static void results_update_text(HealthAPITestAppData *data, const char* text) {
  strncpy(data->results_card.text, text, sizeof(data->results_card.text));
  data->results_card.text[sizeof(data->results_card.text) - 1] = 0;
  text_layer_set_text(data->results_card.text_layer, data->results_card.text);
  layer_mark_dirty(text_layer_get_layer(data->results_card.text_layer));
}


// -------------------------------------------------------------------------------
static void results_window_load(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  Layer *window_layer = window_get_root_layer(window);
  GRect root_bounds = layer_get_bounds(window_layer);

  data->results_card.text_layer = text_layer_create(root_bounds);
  text_layer_set_text_alignment(data->results_card.text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(data->results_card.text_layer, GColorClear);
  text_layer_set_font(data->results_card.text_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_color(data->results_card.text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(data->results_card.text_layer));

  // Update UI
  results_update_text(data, " ");
}


// -------------------------------------------------------------------------------
static void results_window_unload(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  text_layer_destroy(data->results_card.text_layer);
}



// -------------------------------------------------------------------------------
static void prv_hr_select_click_handler(ClickRecognizerRef recognizer, void *context) {
}


// -------------------------------------------------------------------------------
static void prv_hr_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_pop(true);
  window_stack_push(data->steps_window, true /* Animated */);
}


// -------------------------------------------------------------------------------
static void prv_hr_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_pop(true);
  window_stack_push(data->sleep_window, true /* Animated */);
}

// -------------------------------------------------------------------------------
static void prv_hr_down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  HealthAPITestAppData *data = (HealthAPITestAppData *)context;
  window_stack_push(data->debug_window, true /* Animated */);
}


// -------------------------------------------------------------------------------
static void prv_hr_click_config_provider(void *context) {
  const int k_long_press_timeout_ms = 1000;
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_hr_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_hr_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_hr_down_click_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, k_long_press_timeout_ms,
                              prv_hr_down_long_click_handler, NULL);
}


// -------------------------------------------------------------------------------
static void prv_hr_update_text(HealthAPITestAppData *data) {
  if (!data->hr_card.text_layer) {
    return;
  }
  // Get the latest heart rate
  HealthValue now_bpm = health_service_peek_current_value(HealthMetricHeartRateBPM);
  HealthValue resting_bpm = health_service_peek_current_value(HealthMetricRestingHeartRateBPM);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Got HR data. Now: %"PRIi32", Resting: %"PRIi32"",
          now_bpm, resting_bpm);

  data->cur_hr_bpm = now_bpm;
  data->resting_hr_bpm = resting_bpm;

  snprintf(data->hr_card.text, sizeof(data->hr_card.text),
           "HR❤️\nNow: %"PRIu32"\nRest: %"PRIu32"\n Alerts: %"PRIu32" ",
           data->cur_hr_bpm, data->resting_hr_bpm, data->num_hr_alerts);
  text_layer_set_text(data->hr_card.text_layer, data->hr_card.text);
  layer_mark_dirty(text_layer_get_layer(data->hr_card.text_layer));
}


// -------------------------------------------------------------------------------
static void prv_hr_window_load(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  Layer *window_layer = window_get_root_layer(window);
  GRect root_bounds = layer_get_bounds(window_layer);

  data->hr_card.text_layer = text_layer_create(root_bounds);
  text_layer_set_text_alignment(data->hr_card.text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(data->hr_card.text_layer, GColorClear);
  text_layer_set_font(data->hr_card.text_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_color(data->hr_card.text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(data->hr_card.text_layer));

  // Update UI
  prv_hr_update_text(data);

  // Sample the Heart rate at a higher rate while in this view
  health_service_set_heart_rate_sample_period(1 /*interval_s*/);
}


// -------------------------------------------------------------------------------
static void prv_hr_window_unload(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  text_layer_destroy(data->hr_card.text_layer);
  data->hr_card.text_layer = NULL;

  // Cancel our shorter sample period
  health_service_set_heart_rate_sample_period(0 /*interval_s*/);
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_step_history(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_display_scalar_history_alert(data, "Steps", HealthMetricStepCount);
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_sleep_history(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_display_seconds_history_alert(data, "Sleep total", HealthMetricSleepSeconds);
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_active_time_history(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_display_seconds_history_alert(data, "Active Time", HealthMetricActiveSeconds);
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_distance_history(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_display_scalar_history_alert(data, "Distance(m)", HealthMetricWalkedDistanceMeters);
}


// -----------------------------------------------------------------------------------------
static bool prv_activity_iterate_cb(HealthActivity activity, time_t time_start, time_t time_end,
                                    void *context) {
  uint32_t *num_activities_found = (uint32_t *)context;
  *num_activities_found += 1;

  char *activity_name = "unknown";
  switch (activity) {
    case HealthActivityNone:
      activity_name = "none";
      break;
    case HealthActivitySleep:
      activity_name = "sleep";
      break;
    case HealthActivityRestfulSleep:
      activity_name = "restful";
      break;
  }

  // Update bed and awake time if appropriate
  if (activity == HealthActivitySleep) {
    if (s_data->bed_time_utc == 0) {
      s_data->bed_time_utc = time_start;
    }
    if ((s_data->awake_time_utc == 0) || (time_end > s_data->awake_time_utc)) {
      s_data->awake_time_utc = time_end;
    }
  }

  char time_start_text[64];
  struct tm *local_tm = localtime(&time_start);
  strftime(time_start_text, sizeof(time_start_text),  "%F %r", local_tm);

  char time_end_text[64];
  local_tm = localtime(&time_end);
  strftime(time_end_text, sizeof(time_end_text),  "%F %r", local_tm);

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Got activity: %s %s to %s (%d min)", activity_name, time_start_text,
          time_end_text, (int)((time_end - time_start) / SECONDS_PER_MINUTE));
  return true;
}

static void prv_debug_cmd_sleep_sessions(int index, void *context) {
  HealthAPITestAppData *data = context;

  // These will be filled in by the activity callback
  data->bed_time_utc = 0;
  data->awake_time_utc = 0;

  time_t now = time(NULL);
  uint32_t num_activities_found = 0;

  char time_now_text[64];
  struct tm *local_tm = localtime(&now);
  strftime(time_now_text, sizeof(time_now_text),  "%F %r", local_tm);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Current time: %s", time_now_text);


  // Check for correct operation of health_service_any_activity_accessible()
  time_t t_24_hrs_ago = now - SECONDS_PER_DAY;
  HealthServiceAccessibilityMask mask = health_service_any_activity_accessible(HealthActivitySleep,
                                                                               t_24_hrs_ago, now);
  if (mask != HealthServiceAccessibilityMaskAvailable) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Unexpected accessibility result: %d", (int)mask);
  }

  health_service_activities_iterate(HealthActivityMaskAll, now - (2 * SECONDS_PER_DAY), now,
                                    HealthIterationDirectionFuture, prv_activity_iterate_cb,
                                    &num_activities_found);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Found %"PRIu32" activities", num_activities_found);
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_minute_data(int index, void *context) {
  HealthAPITestAppData *data = context;

  const uint32_t k_size = 1000;
  HealthMinuteData *minute_data = malloc(k_size * sizeof(HealthMinuteData));
  if (!minute_data) {
    snprintf(data->debug_card.dialog_text, sizeof(data->debug_card.dialog_text),
             "Out of memory");
    prv_display_alert(data->debug_card.dialog_text);
    goto exit;
  }

  time_t now = time(NULL);
  char time_now_text[64];
  struct tm *local_tm = localtime(&now);
  strftime(time_now_text, sizeof(time_now_text),  "%F %r", local_tm);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Current time: %s", time_now_text);

  // Start as far back as 30 days ago
  time_t utc_start = time(NULL) - 30 * SECONDS_PER_DAY;
  time_t utc_end;
  uint32_t num_records = 0;
  int num_minutes = 0;
  while (true) {
    utc_end = time(NULL);
    num_minutes = health_service_get_minute_history(minute_data, k_size, &utc_start, &utc_end);

    char time_start_text[64];
    local_tm = localtime(&utc_start);
    strftime(time_start_text, sizeof(time_start_text), "%F %r", local_tm);

    char time_end_text[64];
    local_tm = localtime(&utc_end);
    strftime(time_end_text, sizeof(time_end_text), "%F %r", local_tm);

    if (num_minutes > 0) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Got %d minutes: %s to %s", num_minutes, time_start_text,
            time_end_text);
    } else {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "No more data");
    }

    num_records += num_minutes;
    utc_start = utc_end;
    if (num_minutes == 0) {
      break;
    }
  }

  // Print summary
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Retrieved %d minute data records", (int)num_records);


  // Print detail on the last few minutes
  const int k_print_batch_size = 30;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Fetching last %d minutes", k_print_batch_size);
  utc_start = time(NULL) - (k_print_batch_size * SECONDS_PER_MINUTE);
  utc_end = time(NULL);
  uint64_t start_ms = prv_ms();
  num_minutes = health_service_get_minute_history(minute_data, k_print_batch_size, &utc_start,
                                                  &utc_end);
  uint64_t elapsed_ms = prv_ms() - start_ms;

  char time_start_text[64];
  local_tm = localtime(&utc_start);
  strftime(time_start_text, sizeof(time_start_text),  "%F %r", local_tm);

  char time_end_text[64];
  local_tm = localtime(&utc_end);
  strftime(time_end_text, sizeof(time_end_text),  "%F %r", local_tm);

  if (num_minutes > 0) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Got %d minutes in %"PRIu32" ms: %s to %s", num_minutes,
            (uint32_t) elapsed_ms, time_start_text, time_end_text);
  } else {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "No data available in last %d minutes", k_print_batch_size);
  }

  const int k_num_last_minutes = 6;
  if (num_minutes >= k_num_last_minutes) {
    for (int i = num_minutes - k_num_last_minutes; i < num_minutes; i++) {
      HealthMinuteData *m_data = &minute_data[i];
      APP_LOG(APP_LOG_LEVEL_DEBUG, "%"PRId8", 0x%"PRIx8", %"PRIu16", %"PRId8" ",
               m_data->steps, m_data->orientation, m_data->vmc, m_data->light);
    }
  }

exit:
  free(minute_data);
}


// -----------------------------------------------------------------------------------------
static void prv_daily_metric_avg(HealthAPITestAppData *data, HealthMetric metric,
                                 const char *name) {
  strncpy(data->debug_card.dialog_text, name, sizeof(data->debug_card.dialog_text));
  time_t day_start = time_start_of_today();

  HealthServiceAccessibilityMask accessible;
  accessible = health_service_metric_averaged_accessible(
      metric, day_start, day_start + SECONDS_PER_DAY, HealthServiceTimeScopeDailyWeekdayOrWeekend);
  if (!(accessible & HealthServiceAccessibilityMaskAvailable)) {
    prv_display_alert("NOT ACCESSIBLE");
    return;
  }

  // Weekday/weekend avg
  HealthValue avg;
  char temp[64];
  avg = health_service_sum_averaged(metric, day_start, day_start + SECONDS_PER_DAY,
                                    HealthServiceTimeScopeDailyWeekdayOrWeekend);
  snprintf(temp, sizeof(temp), "\nwday/end: %d", (int)avg);
  prv_safe_strcat(data->debug_card.dialog_text, temp, sizeof(data->debug_card.dialog_text));

  // Weekly avg
  avg = health_service_sum_averaged(metric, day_start, day_start + SECONDS_PER_DAY,
                                    HealthServiceTimeScopeWeekly);
  snprintf(temp, sizeof(temp), "\nweekly: %d", (int)avg);
  prv_safe_strcat(data->debug_card.dialog_text, temp, sizeof(data->debug_card.dialog_text));

  // Daily avg
  avg = health_service_sum_averaged(metric, day_start, day_start + SECONDS_PER_DAY,
                                    HealthServiceTimeScopeDaily);
  snprintf(temp, sizeof(temp), "\ndaily: %d", (int)avg);
  prv_safe_strcat(data->debug_card.dialog_text, temp, sizeof(data->debug_card.dialog_text));


  prv_display_alert(data->debug_card.dialog_text);
}


// -----------------------------------------------------------------------------------------
static void prv_intraday_metric_avg(HealthAPITestAppData *data, HealthMetric metric,
                                    const char *name) {
  strncpy(data->debug_card.dialog_text, name, sizeof(data->debug_card.dialog_text));

  time_t day_start = time_start_of_today();
  time_t now = time(NULL);

  HealthServiceAccessibilityMask accessible;
  accessible = health_service_metric_averaged_accessible(
      metric, day_start, now, HealthServiceTimeScopeDailyWeekdayOrWeekend);
  if (!(accessible & HealthServiceAccessibilityMaskAvailable)) {
    prv_display_alert("NOT ACCESSIBLE");
    return;
  }

  // Weekday/weekend avg up to now
  char temp[64];
  HealthValue avg;
  avg = health_service_sum_averaged(metric, day_start, now,
                                    HealthServiceTimeScopeDailyWeekdayOrWeekend);
  snprintf(temp, sizeof(temp), "\ntypical: %d", (int)avg);
  prv_safe_strcat(data->debug_card.dialog_text, temp, sizeof(data->debug_card.dialog_text));

  prv_display_alert(data->debug_card.dialog_text);
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_daily_step_avg(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_daily_metric_avg(data, HealthMetricStepCount, "Steps:");
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_intraday_step_avg(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_intraday_metric_avg(data, HealthMetricStepCount, "Steps:");
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_daily_active_seconds_avg(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_daily_metric_avg(data, HealthMetricActiveSeconds, "Active seconds:");
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_intraday_active_seconds_avg(int index, void *context) {
  HealthAPITestAppData *data = context;
  prv_intraday_metric_avg(data, HealthMetricActiveSeconds, "Active seconds:");
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_heart_rate_api(int index, void *context) {
  HealthAPITestAppData *data = context;
  bool passed = true;

  // Getting resting calories with sum should return work (return non-zero)
  HealthValue value;
  value = health_service_aggregate_averaged(HealthMetricRestingKCalories, time_start_of_today(),
                                            time(NULL), HealthAggregationSum,
                                            HealthServiceTimeScopeOnce);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Result from aggregate_averaged resting cals: %"PRIi32" ", value);
  if (value == 0) {
    passed = false;
    goto exit;
  }

  // Getting resting calories with anything other than sum should fail
  value = health_service_aggregate_averaged(HealthMetricRestingKCalories, time_start_of_today(),
                                            time(NULL), HealthAggregationAvg,
                                            HealthServiceTimeScopeOnce);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Result from aggregate_averaged resting cals: %"PRIi32" ", value);
  if (value != 0) {
    passed = false;
    goto exit;
  }

  // Getting heart rate using sum should fail
  value = health_service_aggregate_averaged(HealthMetricHeartRateBPM, time_start_of_today(),
                                            time(NULL), HealthAggregationSum,
                                            HealthServiceTimeScopeOnce);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Result from aggregate_averaged heart-rate: %"PRIi32" ", value);
  if (value != 0) {
    passed = false;
    goto exit;
  }

  // accessibility with calories should work
  HealthServiceAccessibilityMask access;
  access = health_service_metric_aggregate_averaged_accessible(
      HealthMetricRestingKCalories, time_start_of_today(), time(NULL), HealthAggregationSum,
      HealthServiceTimeScopeOnce);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Result from aggregate_averaged_accessible calories, sum: 0x%x",
          access);
  if (access != HealthServiceAccessibilityMaskAvailable) {
    passed = false;
    goto exit;
  }

  // accessibility with heart rate should work
  access = health_service_metric_aggregate_averaged_accessible(HealthMetricHeartRateBPM,
                                                               time_start_of_today(),
                                                               time(NULL),
                                                               HealthAggregationAvg,
                                                               HealthServiceTimeScopeOnce);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Result from aggregate_averaged_accessible heart rate, sum: 0x%x",
          access);
  if (access != HealthServiceAccessibilityMaskAvailable) {
    passed = false;
    goto exit;
  }

  // Test registering and cancelling a metric alert
  HealthMetricAlert *alert = health_service_register_metric_alert(HealthMetricHeartRateBPM, 10);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Result from register_metric_alert: %p", alert);
  if (alert == NULL) {
    passed = false;
    goto exit;
  }

  bool success = health_service_cancel_metric_alert(alert);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Result from cancel_metric_alert: %d", (int)success);
  if (!success) {
    passed = false;
    goto exit;
  }

  exit:
  strncpy(data->debug_card.dialog_text, passed ? "PASS" : "FAIL",
          sizeof(data->debug_card.dialog_text));
  prv_display_alert(data->debug_card.dialog_text);
}


// -----------------------------------------------------------------------------------------
static void prv_debug_cmd_heart_rate_stats(int index, void *context) {
  HealthAPITestAppData *data = context;
  strcpy(data->debug_card.dialog_text, "HR stats");

  // Get various stats
  time_t end_time = time(NULL);

  // List of time ranges
  typedef struct {
    uint32_t seconds;
    char *desc;
  } TimeRange;

  TimeRange ranges[] = {
    {1 * SECONDS_PER_HOUR, "1 hour"},
    {30 * SECONDS_PER_MINUTE, "30 min"},
  };

  unsigned num_ranges = ARRAY_LENGTH(ranges);
  for (unsigned i = 0; i < num_ranges; i++) {
    HealthValue min = health_service_aggregate_averaged(
        HealthMetricHeartRateBPM, end_time - ranges[i].seconds, end_time, HealthAggregationMin,
        HealthServiceTimeScopeOnce);

    HealthValue max = health_service_aggregate_averaged(
      HealthMetricHeartRateBPM, end_time - ranges[i].seconds, end_time, HealthAggregationMax,
      HealthServiceTimeScopeOnce);

    HealthValue avg = health_service_aggregate_averaged(
      HealthMetricHeartRateBPM, end_time - ranges[i].seconds, end_time, HealthAggregationAvg,
      HealthServiceTimeScopeOnce);

    char temp[64];
    snprintf(temp, sizeof(temp), "%s: min: %"PRIi32", max: %"PRIi32", avg: %"PRIi32" \n",
             ranges[i].desc, min, max, avg);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "%s", temp);
    prv_safe_strcat(data->debug_card.dialog_text, temp, sizeof(data->debug_card.dialog_text));
  }
  prv_display_alert(data->debug_card.dialog_text);
}


// -----------------------------------------------------------------------------------------
static void debug_window_load(Window *window) {
  HealthAPITestAppData *data = window_get_user_data(window);
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  static SimpleMenuItem menu_items[] = {
    {
      .title = "Step History",
      .callback = prv_debug_cmd_step_history,
    }, {
      .title = "Active Minutes History",
      .callback = prv_debug_cmd_active_time_history,
    }, {
      .title = "Distance(m) History",
      .callback = prv_debug_cmd_distance_history,
    }, {
      .title = "Sleep History",
      .callback = prv_debug_cmd_sleep_history,
    }, {
      .title = "Sleep Sessions",
      .callback = prv_debug_cmd_sleep_sessions,
    }, {
      .title = "Read Minute data",
      .callback = prv_debug_cmd_minute_data,
    }, {
      .title = "Daily step avg",
      .callback = prv_debug_cmd_daily_step_avg,
    }, {
      .title = "Intraday step avg",
      .callback = prv_debug_cmd_intraday_step_avg,
    }, {
      .title = "Daily active sec. avg",
      .callback = prv_debug_cmd_daily_active_seconds_avg,
    }, {
      .title = "Intraday active sec. avg",
      .callback = prv_debug_cmd_intraday_active_seconds_avg,
    }, {
      .title = "Heart Rate Stats",
      .callback = prv_debug_cmd_heart_rate_stats,
    }, {
      .title = "Heart Rate API",
      .callback = prv_debug_cmd_heart_rate_api,
    }
  };
  static const SimpleMenuSection sections[] = {
    {
      .items = menu_items,
      .num_items = ARRAY_LENGTH(menu_items)
    }
  };

  data->debug_card.menu_items = menu_items;
  data->debug_card.menu_layer = simple_menu_layer_create(bounds, window, sections,
                                                         ARRAY_LENGTH(sections), data);
  layer_add_child(window_layer, simple_menu_layer_get_layer(data->debug_card.menu_layer));
}


// -------------------------------------------------------------------------------
static void debug_window_unload(Window *window) {
  simple_menu_layer_destroy(s_data->debug_card.menu_layer);
}


// -------------------------------------------------------------------------------
static void deinit(void) {
  window_destroy(s_data->steps_window);
  free(s_data);
  s_data = NULL;
}


// -------------------------------------------------------------------------------
static void init(void) {
  HealthAPITestAppData *data = malloc(sizeof(HealthAPITestAppData));
  s_data = data;
  memset(data, 0, sizeof(HealthAPITestAppData));
  data->steps_offset = 0;

  // Create our windows

  // Steps window
  data->steps_window = window_create();
  window_set_background_color(data->steps_window, GColorBlack);
  window_set_user_data(data->steps_window, data);
  window_set_click_config_provider_with_context(data->steps_window, steps_click_config_provider,
                                                data);
  window_set_window_handlers(data->steps_window, (WindowHandlers) {
    .load = steps_window_load,
    .unload = steps_window_unload,
    .appear = steps_window_appear,
  });

  // Sleep window
  data->sleep_window = window_create();
  window_set_background_color(data->sleep_window, GColorBlack);
  window_set_user_data(data->sleep_window, data);
  window_set_click_config_provider_with_context(data->sleep_window, sleep_click_config_provider,
                                                data);
  window_set_window_handlers(data->sleep_window, (WindowHandlers) {
    .load = sleep_window_load,
    .unload = sleep_window_unload,
  });

  // Debug window
  data->debug_window = window_create();
  window_set_user_data(data->debug_window, data);
  window_set_window_handlers(data->debug_window, (WindowHandlers) {
    .load = debug_window_load,
    .unload = debug_window_unload,
  });

  // Results window
  data->results_window = window_create();
  window_set_background_color(data->results_window, GColorBlack);
  window_set_user_data(data->results_window, data);
  window_set_click_config_provider_with_context(data->results_window, results_click_config_provider,
                                                data);
  window_set_window_handlers(data->results_window, (WindowHandlers) {
    .load = results_window_load,
    .unload = results_window_unload,
  });

  // Heart rate window
  data->hr_window = window_create();
  window_set_background_color(data->hr_window, GColorBlack);
  window_set_user_data(data->hr_window, data);
  window_set_click_config_provider_with_context(data->hr_window, prv_hr_click_config_provider,
                                                data);
  window_set_window_handlers(data->hr_window, (WindowHandlers) {
    .load = prv_hr_window_load,
    .unload = prv_hr_window_unload,
  });

  window_stack_push(data->steps_window, true /* Animated */);
}


// -------------------------------------------------------------------------------
int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
