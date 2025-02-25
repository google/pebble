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


// This app only makes sense on Snowy, as it uses addresses and sector sizes that only make sense
// on our parallel flash hardware
#if CAPABILITY_USE_PARALLEL_FLASH

#include "flash_test.h"

#include "applib/app.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/simple_menu_layer.h"
#include "applib/ui/text_layer.h"
#include "applib/ui/window.h"
#include "applib/ui/window_stack.h"


#include "mfg/mfg_apps/mfg_flash_test.h"
#include "process_state/app_state/app_state.h"
#include "services/common/system_task.h"
#include "system/logging.h"

#include "kernel/pbl_malloc.h"

#include <stdio.h>

enum FlashTestCaseStatus {
  FLASH_TEST_STATUS_INIT = 0,
  FLASH_TEST_STATUS_RUNNING = 1,
  FLASH_TEST_STATUS_STOPPED = 2,
  FLASH_TEST_STATUS_PASSED = 3,
  FLASH_TEST_STATUS_FAILED = 4,
};

#define STATUS_TEXT_SIZE 18
struct FlashTestData {
  Window window;
  SimpleMenuLayer simple_menu_layer;
  SimpleMenuSection menu_sections[1];
  SimpleMenuItem menu_items[FLASH_TEST_CASE_NUM_MENU_ITEMS];
  Window test_window;
  TextLayer msg_text_layer[3];
  TextLayer status_text_layer;
  char status_text[STATUS_TEXT_SIZE];
  FlashTestCaseType test_case;
  uint8_t test_case_status;
};

/***********************************************************/
/******************* Window Related Functions **************/
/***********************************************************/
static void handle_timer(struct tm *tick_time, TimeUnits units_changed) {
  struct FlashTestData *data = app_state_get_user_data();
  // simply marking the window dirty will make everything update
  if (data && (data->test_case_status != FLASH_TEST_STATUS_INIT))
  {
    layer_mark_dirty(&data->test_window.layer);
  }
}

static void update_test_case_status(struct FlashTestData *data) {
  switch (data->test_case_status) {
    case FLASH_TEST_STATUS_INIT:
      snprintf(data->status_text, STATUS_TEXT_SIZE, "Test Initialized");
      break;
    case FLASH_TEST_STATUS_RUNNING:
      snprintf(data->status_text, STATUS_TEXT_SIZE, "Test Running");
      break;
    case FLASH_TEST_STATUS_STOPPED:
      snprintf(data->status_text, STATUS_TEXT_SIZE, "Test Stopped");
      break;
    case FLASH_TEST_STATUS_PASSED:
      snprintf(data->status_text, STATUS_TEXT_SIZE, "Test Passed");
      break;
    case FLASH_TEST_STATUS_FAILED:
      snprintf(data->status_text, STATUS_TEXT_SIZE, "Test Failed");
      break;
    default:
      snprintf(data->status_text, STATUS_TEXT_SIZE, "Unknown Status");
      PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Unknown Test Case Selected");
      break;
  }

  text_layer_set_text(&data->status_text_layer, data->status_text);
}

static void test_window_load(Window *window) {
  struct FlashTestData *data = app_state_get_user_data();

  window_set_background_color(window, GColorBlack);

  TextLayer *msg_text_layer = &data->msg_text_layer[0];
  text_layer_init(msg_text_layer, &GRect(0, 12, window->layer.bounds.size.w, 18));
  switch (data->test_case) {
    case FLASH_TEST_CASE_RUN_DATA_TEST:
      text_layer_set_text(msg_text_layer, "Data Bus Test");
      break;
    case FLASH_TEST_CASE_RUN_ADDR_TEST:
      text_layer_set_text(msg_text_layer, "Addr Bus Test");
      break;
    case FLASH_TEST_CASE_RUN_STRESS_ADDR_TEST:
      text_layer_set_text(msg_text_layer, "Stress Addr Test");
      break;
    case FLASH_TEST_CASE_RUN_PERF_DATA_TEST:
      text_layer_set_text(msg_text_layer, "Perf Data Test");
      break;
    case FLASH_TEST_CASE_RUN_SWITCH_MODE_ASYNC:
    case FLASH_TEST_CASE_RUN_SWITCH_MODE_SYNC_BURST:
      text_layer_set_text(msg_text_layer, "Switch Mode");
      break;
    default:
      text_layer_set_text(msg_text_layer, "Unknown Test");
      break;
  }

  text_layer_set_text_alignment(msg_text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(msg_text_layer, GColorBlack);
  text_layer_set_text_color(msg_text_layer, GColorWhite);
  text_layer_set_font(msg_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  layer_add_child(&window->layer, &msg_text_layer->layer);

  msg_text_layer = &data->msg_text_layer[1];
  text_layer_init(msg_text_layer, &GRect(0, 32, window->layer.bounds.size.w, 18));
  if (data->test_case >= FLASH_TEST_CASE_RUN_SWITCH_MODE_ASYNC) {
    text_layer_set_text(msg_text_layer, "Select To Confirm Switch");
  }
  else {
    text_layer_set_text(msg_text_layer, "Press Select To Start");
  }
  text_layer_set_text_alignment(msg_text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(msg_text_layer, GColorBlack);
  text_layer_set_text_color(msg_text_layer, GColorWhite);
  text_layer_set_font(msg_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  layer_add_child(&window->layer, &msg_text_layer->layer);

  msg_text_layer = &data->msg_text_layer[2];
  text_layer_init(msg_text_layer, &GRect(0, 64, window->layer.bounds.size.w, 40));
  if (data->test_case >= FLASH_TEST_CASE_RUN_SWITCH_MODE_ASYNC) {
    text_layer_set_text(msg_text_layer, "Press Back To Exit");
  }
  else {
    text_layer_set_text(msg_text_layer, "Press Up to Exit, Down to Stop Test");
  }
  text_layer_set_text_alignment(msg_text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(msg_text_layer, GColorBlack);
  text_layer_set_text_color(msg_text_layer, GColorWhite);
  text_layer_set_font(msg_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  layer_add_child(&window->layer, &msg_text_layer->layer);

  TextLayer *status_text_layer = &data->status_text_layer;
  text_layer_init(status_text_layer, &GRect(0, 106, window->layer.bounds.size.w, 40));
  text_layer_set_text_alignment(status_text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(status_text_layer, GColorBlack);
  text_layer_set_text_color(status_text_layer, GColorWhite);
  text_layer_set_font(status_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  layer_add_child(&window->layer, &status_text_layer->layer);

  update_test_case_status(data);
}

void flash_test_dismiss_window(void) {
  struct FlashTestData *data = app_state_get_user_data();

  const bool animated = true;
  window_stack_remove(&data->test_window, animated);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *unused) {
  struct FlashTestData *data = app_state_get_user_data();

  if (data->test_case_status != FLASH_TEST_STATUS_RUNNING) {
    flash_test_dismiss_window(); 
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *unused) {
  struct FlashTestData *data = app_state_get_user_data();

  // Only stop stress test
  if ((data->test_case == FLASH_TEST_CASE_RUN_STRESS_ADDR_TEST) && (data->test_case_status == FLASH_TEST_STATUS_RUNNING)) {
    data->test_case_status = FLASH_TEST_STATUS_STOPPED;
    stop_flash_test_case();
    update_test_case_status(data);
  }
}

static void run_test(void* context) {
  struct FlashTestData *data = app_state_get_user_data();
  FlashTestErrorType status = FLASH_TEST_SUCCESS;

  // Execute test - pass in 0 by default for iterations
  status = run_flash_test_case(data->test_case, 0);

  if (status == FLASH_TEST_SUCCESS) {
    data->test_case_status = FLASH_TEST_STATUS_PASSED;
  }
  else {
    PBL_LOG(LOG_LEVEL_DEBUG, ">>>>>FAILED TEST CASE<<<<<");
    data->test_case_status = FLASH_TEST_STATUS_FAILED;
  }

  update_test_case_status(data);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *unused) {
  struct FlashTestData *data = app_state_get_user_data();

  if ((data->test_case == FLASH_TEST_CASE_RUN_STRESS_ADDR_TEST) && (data->test_case_status == FLASH_TEST_STATUS_RUNNING)) {
    data->test_case_status = FLASH_TEST_STATUS_STOPPED;
    stop_flash_test_case();
    update_test_case_status(data);
  }
  else if (data->test_case_status == FLASH_TEST_STATUS_INIT) {
    data->test_case_status = FLASH_TEST_STATUS_RUNNING;
    update_test_case_status(data);
    system_task_add_callback(run_test, NULL);
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, (ClickHandler)up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, (ClickHandler)down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, (ClickHandler)select_click_handler);
}

static void flash_test_select_callback(int index, void *context) {
  struct FlashTestData *data = (struct FlashTestData *) context;

  data->test_case = index;
  data->test_case_status = FLASH_TEST_STATUS_INIT;

  // Display window for running test case
  // Init the window
  Window *test_window = &data->test_window;
  window_init(test_window, WINDOW_NAME("Test Case"));
  window_set_window_handlers(test_window, &(WindowHandlers) {
    .load = test_window_load,
  });
  window_set_user_data(test_window, data);
  window_set_click_config_provider(test_window, click_config_provider);

  const bool animated = true;
  app_window_stack_push(test_window, animated);
}

static void flash_test_window_load(Window *window) {
  struct FlashTestData *data = (struct FlashTestData*) window_get_user_data(window);

  // Configure menu items:
  uint8_t num_items = 0;
  data->menu_items[num_items++] = (SimpleMenuItem) { .title = "Run Data Test",
                                                     .callback = flash_test_select_callback};
  data->menu_items[num_items++] = (SimpleMenuItem) { .title = "Run Address Test",
                                                     .callback = flash_test_select_callback};
  data->menu_items[num_items++] = (SimpleMenuItem) { .title = "Run Stress Test",
                                                     .callback = flash_test_select_callback};
  data->menu_items[num_items++] = (SimpleMenuItem) { .title = "Run Perf Data Test",
                                                     .callback = flash_test_select_callback};
  data->menu_items[num_items++] = (SimpleMenuItem) { .title = "-->Async Mode",
                                                     .callback = flash_test_select_callback};
  data->menu_items[num_items++] = (SimpleMenuItem) { .title = "-->Sync Burst Mode",
                                                     .callback = flash_test_select_callback};

  data->menu_sections[0].num_items = num_items;
  data->menu_sections[0].items = data->menu_items;

  // Configure simple menu:
  const GRect *bounds = &data->window.layer.bounds;
  simple_menu_layer_init(&data->simple_menu_layer, bounds, window, data->menu_sections, 1, data);
  layer_add_child(&data->window.layer, simple_menu_layer_get_layer(&data->simple_menu_layer));
}

static void handle_init(void) {
  struct FlashTestData* data_ptr = app_malloc_check(sizeof(struct FlashTestData));

  memset(data_ptr, 0, sizeof(struct FlashTestData));

  app_state_set_user_data(data_ptr);

  window_init(&data_ptr->window, WINDOW_NAME("Flash Test"));
  window_set_user_data(&data_ptr->window, data_ptr);
  window_set_window_handlers(&data_ptr->window, &(WindowHandlers){
    .load = flash_test_window_load,
  });
  
  const bool animated = true;
  app_window_stack_push(&data_ptr->window, animated);

  tick_timer_service_subscribe(SECOND_UNIT, handle_timer);
}

static void handle_deinit(void) {
  struct FlashTestData *data = app_state_get_user_data();
  simple_menu_layer_deinit(&data->simple_menu_layer);
  app_free(data);
  
  stop_flash_test_case();
}


static void s_main(void) {
  handle_init();

  app_event_loop();

  handle_deinit();
}

const PebbleProcessMd* flash_test_demo_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common.main_func = s_main,
    .name = "Flash Test"
  };
  return (const PebbleProcessMd*) &s_app_info;
}

#endif // CAPABILITY_USE_PARALLEL_FLASH
