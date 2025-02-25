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


Window *window;
TextLayer *text_layer;
Layer *line_layer;

static char g_text[100];

// Return current time in ms
static uint64_t prv_ms(void) {
  time_t cur_sec;
  uint16_t cur_ms = time_ms(&cur_sec, NULL);
  return ((uint64_t)cur_sec * 1000) + cur_ms;
}


static void steps_event_handler(uint16_t type, AppWorkerMessage *data) {
  //APP_LOG(APP_LOG_LEVEL_DEBUG, "Received new worker event. type: %d, data: %d, %d, %d", (int)type, (int)data->data0,
  //        (int)data->data1, (int)data->data2);

  if (type == 0) {
    snprintf(g_text, sizeof(g_text), "%5d %5d %5d", (int)data->data0, (int)data->data1, (int)data->data2);
    text_layer_set_text(text_layer, g_text);
  } else if (type == 1) {
    snprintf(g_text, sizeof(g_text), "BAT: %d, %d, %d", (int)data->data0, (int)data->data1, (int)data->data2);
    text_layer_set_text(text_layer, g_text);
  }
}


static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  AppWorkerResult result = app_worker_launch();
  APP_LOG(APP_LOG_LEVEL_INFO, "launch result: %d", result);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  AppWorkerMessage m;
  app_worker_send_message('x', &m);
  APP_LOG(APP_LOG_LEVEL_INFO, "crashing worker");
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  AppWorkerResult result = app_worker_kill();
  APP_LOG(APP_LOG_LEVEL_INFO, "kill result: %d", result);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

static uint32_t s_seconds_count;
void handle_second_tick(struct tm *tick_time, TimeUnits units_changed) {

  bool running = app_worker_is_running();

  if (false) {
    const char* status = "not";
    if (running) {
      status = "is";
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "worker %s running", status);
  }

  s_seconds_count++;
  if ((s_seconds_count % 5) == 0) {
    int value = persist_read_int(42);
    // APP_LOG(APP_LOG_LEVEL_INFO, "Updating persist value from %d to %d", value, value + 1);
    persist_write_int(42, value + 1);
  }
}

static void health_event_handler(HealthEventType event, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "app: Got health event update. event_id: %"PRIu32"",
          (uint32_t) event);
  if (event == HealthEventMovementUpdate) {
    HealthValue steps = health_service_sum_today(HealthMetricStepCount);
    APP_LOG(APP_LOG_LEVEL_INFO, "app: movement event, steps: %"PRIu32"",
            (uint32_t)steps);

    // Test getting historical steps
    time_t day_start = time_start_of_today();
    for (int i = 0; i < 7; i++) {
      steps = health_service_sum(HealthMetricStepCount, day_start, day_start + SECONDS_PER_DAY);
      APP_LOG(APP_LOG_LEVEL_INFO, "%d days ago steps: %d", i, (int)steps);
      day_start -= SECONDS_PER_DAY;
    }

    // Test getting steps for part of a day
    day_start = time_start_of_today();
    time_t seconds_today_so_far = time(NULL) - day_start;
    steps = health_service_sum(HealthMetricStepCount, day_start,
                               day_start + (seconds_today_so_far / 2));
    APP_LOG(APP_LOG_LEVEL_INFO, "steps 1st half of today: %d", (int)steps);

    steps = health_service_sum(HealthMetricStepCount, day_start - (SECONDS_PER_DAY / 2), day_start);
    APP_LOG(APP_LOG_LEVEL_INFO, "steps 2nd half of yesterday: %d", (int)steps);


    // Test the get_minute_history call

    const int minute_data_len = 10;
    HealthMinuteData minute_data[minute_data_len];
    uint32_t num_records = minute_data_len;
    time_t utc_start = time(NULL) - 60 * 60 * 24;  // All records since 1 day ago
    time_t utc_end = time(NULL);

    uint64_t start_ms = prv_ms();
    health_service_get_minute_history(minute_data, num_records, &utc_start, &utc_end);
    uint64_t elapsed_ms = prv_ms() - start_ms;

    int num_records_returned = (utc_end - utc_start) / SECONDS_PER_MINUTE;
    APP_LOG(APP_LOG_LEVEL_INFO, "app: Retrieved %d minute records in %"PRIu32" ms:",
            num_records_returned, (uint32_t)elapsed_ms);
    for (int i = 0; i < num_records_returned; i++) {
      APP_LOG(APP_LOG_LEVEL_INFO, "  steps: %"PRIu8", orient: 0x%"PRIx8", vmc: %"PRIu16", "
              "light: %d, valid: %d", minute_data[i].steps, minute_data[i].orientation,
              minute_data[i].vmc, (int)minute_data[i].light, (int)(!minute_data[i].is_invalid));
    }


  } else if (event == HealthEventSleepUpdate) {
    HealthValue total_sleep = health_service_sum_today(HealthMetricSleepSeconds);
    HealthValue restful_sleep = health_service_sum_today(HealthMetricSleepRestfulSeconds);
    APP_LOG(APP_LOG_LEVEL_INFO, "app: New sleep event: total: %"PRIu32", restful: %"PRIu32" ",
            total_sleep / SECONDS_PER_MINUTE,  restful_sleep / SECONDS_PER_MINUTE);
  }
}

void handle_deinit(void) {
  tick_timer_service_unsubscribe();
  health_service_events_unsubscribe();
}

void handle_init(void) {
  window = window_create();
  window_set_click_config_provider(window, click_config_provider);

  window_stack_push(window, true /* Animated */);
  window_set_background_color(window, GColorBlack);

  Layer *window_layer = window_get_root_layer(window);

  text_layer = text_layer_create(GRect(7, 40, 144-7, 168-40));
  text_layer_set_text_color(text_layer, GColorWhite);
  text_layer_set_background_color(text_layer, GColorClear);
  text_layer_set_font(text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));

  layer_add_child(window_layer, text_layer_get_layer(text_layer));

  text_layer_set_text(text_layer, "? ? ?");

  // Subscribe to messages published by the worker
  app_worker_message_subscribe(steps_event_handler);

  // Subscribe to second ticks
  tick_timer_service_subscribe(SECOND_UNIT, handle_second_tick);

  // Launch the worker
  AppWorkerResult result = app_worker_launch();
  APP_LOG(APP_LOG_LEVEL_INFO, "launch result: %d", result);

  // Subscribe to health service
  health_service_events_subscribe(health_event_handler, NULL);
}


int main(void) {
  handle_init();

  app_event_loop();
  
  handle_deinit();
}
