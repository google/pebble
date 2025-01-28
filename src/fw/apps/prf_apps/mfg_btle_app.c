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

#include "mfg_btle_app.h"

#include "applib/app.h"
#include "applib/ui/ui.h"
#include "applib/ui/option_menu_window.h"
#include "bluetooth/bt_test.h"
#include "board/board.h"
#if CAPABILITY_HAS_MICROPHONE
#include "drivers/mic.h"
#endif
#include "services/common/bluetooth/bt_compliance_tests.h"
#if CAPABILITY_HAS_BUILTIN_HRM
#include "services/common/hrm/hrm_manager.h"
#endif
#include "kernel/pbl_malloc.h"
#include "process_management/app_manager.h"
#include "process_state/app_state/app_state.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"
#include "util/string.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <stdio.h>

#if BT_CONTROLLER_DA14681

#define TXRX_NUM_SUBTITLES   2
#define TXRX_SUBTITLE_LENGTH 8

#define STATUS_STRING_LENGTH 32

typedef enum {
  BTLETestType_None = 0,

  BTLETestType_TX,
  BTLETestType_RX,

  BTLETestTypeCount
} BTLETestType;

typedef enum {
  BTLETestStep_None = 0,

  BTLETestStep_BTStart,
  BTLETestStep_BTEnd,

  BTLETestStep_BTLETransmitStart,
  BTLETestStep_BTLEReceiverStart,
  BTLETestStep_BTLEStop,

  BTLETestStepCount
} BTLETestStep;

typedef enum {
  BTLEPayloadType_PRBS9 = 0,
  BTLEPayloadType_11110000,
  BTLEPayloadType_10101010,
  BTLEPayloadType_PRBS15,
  BTLEPayloadType_11111111,
  BTLEPayloadType_00000000,
  BTLEPayloadType_00001111,
  BTLEPayloadType_01010101,

  BTLEPayloadTypeCount
} BTLEPayloadType;

static const char *s_payload_names[BTLEPayloadTypeCount] = {
  [BTLEPayloadType_PRBS9]    = "PRBS9",
  [BTLEPayloadType_11110000] = "11110000",
  [BTLEPayloadType_10101010] = "10101010",
  [BTLEPayloadType_PRBS15]   = "PRBS15",
  [BTLEPayloadType_11111111] = "11111111",
  [BTLEPayloadType_00000000] = "00000000",
  [BTLEPayloadType_00001111] = "00001111",
  [BTLEPayloadType_01010101] = "01010101"
};

typedef struct {
  // Main Menu
  Window main_menu_window;
  SimpleMenuLayer *main_menu_layer;
  SimpleMenuSection main_menu_section;
  SimpleMenuItem *main_menu_items;

  // TX / RX Menu
  Window txrx_window;
  MenuLayer txrx_menu_layer;
  NumberWindow txrx_number_window;

  // Payload Selection
  Window payload_window;
  SimpleMenuLayer *payload_menu_layer;
  SimpleMenuSection payload_menu_section;
  SimpleMenuItem *payload_menu_items;

  // Status Window
  Window status_window;
  TextLayer status_text;
  char status_string[STATUS_STRING_LENGTH];

  // Testing State
  BTLETestType current_test;
  uint8_t channel;
  uint8_t payload_length;
  BTLEPayloadType payload_type;
  bool is_unmodulated_cw_enabled;

#if CAPABILITY_HAS_BUILTIN_HRM
  bool is_hrm_enabled;
#endif

#if CAPABILITY_HAS_MICROPHONE
  bool is_mic_enabled;
  int16_t *mic_buffer;
#endif

  BTLETestStep current_test_step;
  bool last_test_step_result;

  uint16_t rx_test_received_packets;

  SemaphoreHandle_t btle_test_semaphore;

  HRMSessionRef hrm_session;
} AppData;

// Forward declarations
static void prv_txrx_menu_update(AppData *data);

//--------------------------------------------------------------------------------
// Running Tests
//--------------------------------------------------------------------------------
// Running the actual test is an asynchronous operations which expects a
// callback to come from the bt test driver.
// We keep track of our current test progress with AppData.current_test_step,
// and use that to know how to proceed through.
//
// A BTLE test gets started, and needs to be manually stopped.
// This means that setup setup goes like this:
//
// 1. User Signals "RUN"
// 2. bt_test_start()
// 3. bt_driver_le_transmitter_test / bt_driver_le_receiver_test
// 4. User Signals "STOP"
// 5. bt_driver_le_test_end()
// 6. In case of RX test, gather results
// 7. bt_test_stop()

static void prv_response_cb(HciStatusCode status, const uint8_t *payload) {
  AppData *data = app_state_get_user_data();
  const bool success = (status == HciStatusCode_Success);

  PBL_LOG(LOG_LEVEL_DEBUG, "Step %d complete", data->current_test_step);
  if (data->current_test_step == BTLETestStep_BTLEStop && data->current_test == BTLETestType_RX) {
    // RX Test, need to keep track of received packets
    // Payload is as follows:
    // | 1 byte  | 2 bytes          |
    // | success | received packets |
    // So we want grab a uint16_t from 1 byte into the payload
    const uint16_t *received_packets = (uint16_t *)(payload + 1);
    data->rx_test_received_packets = *received_packets;
  }

  data->last_test_step_result = success;
  xSemaphoreGive(data->btle_test_semaphore);
}

#if CAPABILITY_HAS_MICROPHONE
static void prv_mic_cb(int16_t *samples, size_t sample_count, void *context) {
  // Just throw away the recorded samples.
}
#endif

static bool prv_run_test_step(BTLETestStep step, AppData *data) {
  data->current_test_step = step;
  PBL_LOG(LOG_LEVEL_DEBUG, "Run test step: %d", step);
  bool wait_for_result = false;
  switch (step) {
    case BTLETestStep_BTStart:
      bt_test_start();
      break;
    case BTLETestStep_BTEnd:
      bt_test_stop();
      break;

    case BTLETestStep_BTLETransmitStart:
      if (data->is_unmodulated_cw_enabled) {
        bt_driver_start_unmodulated_tx(data->channel);
        wait_for_result = false;
      } else {
        bt_driver_le_transmitter_test(data->channel, data->payload_length, data->payload_type);
        wait_for_result = true;
      }
      break;
    case BTLETestStep_BTLEReceiverStart:
      bt_driver_le_receiver_test(data->channel);
      wait_for_result = true;
      break;
    case BTLETestStep_BTLEStop:
      if (data->current_test == BTLETestType_TX && data->is_unmodulated_cw_enabled) {
        bt_driver_stop_unmodulated_tx();
        wait_for_result = false;
      } else {
        bt_driver_le_test_end();
        wait_for_result = true;
      }
      break;
    default:
      WTF;
  }

  // Waiting for results is OK because it should not block the app task for very long.
  // The result is not for the entire test, it is a result for the step itself.
  // The test result for an RX test is received in prv_response_cb after BTLETestStep_BTLEStop.
  if (wait_for_result) {
    xSemaphoreTake(data->btle_test_semaphore, portMAX_DELAY);
    return data->last_test_step_result;
  }
  return true;
}

static void prv_stop_mic_and_cleanup(AppData *data) {
  mic_stop(MIC);
  app_free(data->mic_buffer);
  data->mic_buffer = NULL;
}

static void prv_run_test(AppData *data) {
  PBL_ASSERTN(data->current_test_step == BTLETestStep_None);

  bool failed = false;

  bt_driver_register_response_callback(prv_response_cb);

#if CAPABILITY_HAS_BUILTIN_HRM
  if (data->is_hrm_enabled) {
    AppInstallId app_id = 1;
    uint16_t expire_s = SECONDS_PER_HOUR;
    data->hrm_session = sys_hrm_manager_app_subscribe(app_id, 1, expire_s, HRMFeature_LEDCurrent);
  }
#endif

#if CAPABILITY_HAS_MICROPHONE
  if (data->is_mic_enabled) {
    const size_t BUFFER_SIZE = 50;
    data->mic_buffer = app_malloc_check(BUFFER_SIZE * sizeof(int16_t));
    if (!mic_start(MIC, &prv_mic_cb, NULL, data->mic_buffer, BUFFER_SIZE)) {
      failed = true;
      goto cleanup;
    }
  }
#endif

  if (!prv_run_test_step(BTLETestStep_BTStart, data)) {
    failed = true;
    goto cleanup;
  }

  switch (data->current_test) {
    case BTLETestType_TX:
      if (!prv_run_test_step(BTLETestStep_BTLETransmitStart, data)) {
        failed = true;
        goto cleanup;
      }
      break;
    case BTLETestType_RX:
      data->rx_test_received_packets = 0;
      if (!prv_run_test_step(BTLETestStep_BTLEReceiverStart, data)) {
        failed = true;
        goto cleanup;
      }
      break;
    default:
      WTF;
  }

cleanup:
  prv_txrx_menu_update(data);

  if (failed) {
#if CAPABILITY_HAS_BUILTIN_HRM
    sys_hrm_manager_unsubscribe(data->hrm_session);
#endif
#if CAPABILITY_HAS_MICROPHONE
    prv_stop_mic_and_cleanup(data);
#endif

    bt_driver_register_response_callback(NULL);
    snprintf(data->status_string, STATUS_STRING_LENGTH, "Test Failed to Start");
    app_window_stack_push(&data->status_window, true);
    if (data->current_test_step != BTLETestStep_BTStart) {
      // A BTLE Test start failed, stop BT Test
      const bool success = prv_run_test_step(BTLETestStep_BTEnd, data);
      PBL_ASSERTN(success);
    }
    data->current_test_step = BTLETestStep_None;
  }
}

static void prv_stop_test(AppData *data) {
  bool failed = false;
  if (!prv_run_test_step(BTLETestStep_BTLEStop, data)) {
    failed = true;
    goto cleanup;
  } else if (data->current_test == BTLETestType_RX) {
    snprintf(data->status_string, STATUS_STRING_LENGTH,
             "Packets Received: %"PRIu16, data->rx_test_received_packets);
    app_window_stack_push(&data->status_window, true);
  }

  if (!prv_run_test_step(BTLETestStep_BTEnd, data)) {
    failed = true;
    goto cleanup;
  }

cleanup:
  data->current_test_step = BTLETestStep_None;
  bt_driver_register_response_callback(NULL);
  prv_txrx_menu_update(data);

#if CAPABILITY_HAS_BUILTIN_HRM
  sys_hrm_manager_unsubscribe(data->hrm_session);
#endif
#if CAPABILITY_HAS_MICROPHONE
  prv_stop_mic_and_cleanup(data);
#endif

  if (failed) {
    snprintf(data->status_string, STATUS_STRING_LENGTH, "Test Failed");
    app_window_stack_push(&data->status_window, true);
  }
}

static bool prv_test_is_running(AppData *data) {
  return (data->current_test_step != BTLETestStep_None);
}

//--------------------------------------------------------------------------------
// Number Windows
//--------------------------------------------------------------------------------
// Number window is used / reused for getting channel / payload length from the user.

static void prv_number_window_selected_cb(NumberWindow *number_window, void *context) {
  uint8_t *result = context;

  *result = (uint8_t)number_window_get_value(number_window);

  app_window_stack_pop(true);
}

//! Automatically uses 0 as min
//! @param max The max value that can be selected
//! @param value Pointer to the value data, which will be used to populate as well as return result.
static void prv_txrx_number_window(uint8_t max, uint8_t *value, const char *label, AppData *data) {
  NumberWindow *number_window = &data->txrx_number_window;
  number_window_init(number_window, label,
      (NumberWindowCallbacks){ .selected = prv_number_window_selected_cb, }, value);

  number_window_set_min(number_window, 0);
  number_window_set_max(number_window, max);
  number_window_set_value(number_window, *value);

  app_window_stack_push(number_window_get_window(number_window), true);
}

//--------------------------------------------------------------------------------
// Payload Selection Window
//--------------------------------------------------------------------------------
// Payload selection allows the user to select the payload type that should be
// used in the TX test.

static void prv_register_payload(int index, void *context) {
  PBL_ASSERTN(index < BTLEPayloadTypeCount);
  AppData *data = app_state_get_user_data();

  data->payload_type = (BTLEPayloadType)index;

  app_window_stack_pop(true);
}

static void prv_payload_window_load(Window *window) {
  AppData *data = app_state_get_user_data();
  data->payload_menu_items = app_malloc_check(BTLEPayloadTypeCount * sizeof(SimpleMenuItem));
  for (int i = 0; i < BTLEPayloadTypeCount; ++i) {
    data->payload_menu_items[i] = (SimpleMenuItem) {
      .title = s_payload_names[i],
      .callback = prv_register_payload,
    };
  }

  data->payload_menu_section = (SimpleMenuSection) {
    .num_items = BTLEPayloadTypeCount,
    .items = data->payload_menu_items,
  };

  Layer *window_layer = window_get_root_layer(&data->payload_window);
  const GRect bounds = window_layer->bounds;
  data->payload_menu_layer =
      simple_menu_layer_create(bounds, &data->payload_window, &data->payload_menu_section, 1, data);
  layer_add_child(window_layer, simple_menu_layer_get_layer(data->payload_menu_layer));
}

static void prv_payload_window_unload(Window *window) {
  AppData *data = app_state_get_user_data();
  layer_remove_child_layers(window_get_root_layer(&data->payload_window));
  simple_menu_layer_destroy(data->payload_menu_layer);

  app_free(data->payload_menu_items);
}

static void prv_payload_type_window(AppData *data) {
  window_set_window_handlers(&data->payload_window, &(WindowHandlers){
        .load = prv_payload_window_load,
        .unload = prv_payload_window_unload,
      });
  app_window_stack_push(&data->payload_window, true);
}

//--------------------------------------------------------------------------------
// Status Window
//--------------------------------------------------------------------------------
// Status window is just a simple window which will display to the user whatever
// data->status_string has been set to.

static void prv_status_window_init(AppData *data) {
  // Init window
  window_init(&data->status_window, "");

  Layer *window_layer = window_get_root_layer(&data->status_window);
  GRect bounds = window_layer->bounds;
  bounds.origin.y += 40;
  // Init text layer
  text_layer_init(&data->status_text, &bounds);
  text_layer_set_text(&data->status_text, data->status_string);
  text_layer_set_text_alignment(&data->status_text, GTextAlignmentCenter);
  text_layer_set_font(&data->status_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(&data->status_text));
}

//--------------------------------------------------------------------------------
// TX/RX Menus & Windows
//--------------------------------------------------------------------------------
// The same menu layer is reused for TX / RX, we just handle it differently
// based on whether we are currently executing a TX or RX test.

#define TX_MENU_NUM_PAYLOAD_ROWS (2)

enum {
  TXMenuIdx_Channel = 0,
  TXMenuIdx_UnmodulatedContinuousWave,
  TXMenuIdx_PayloadLength,
  TXMenuIdx_PayloadType,
#if CAPABILITY_HAS_BUILTIN_HRM
  TXMenuIdx_HRM,
#endif
#if CAPABILITY_HAS_MICROPHONE
  TXMenuIdx_Microphone,
#endif
  TXMenuIdx_RunStop,
  TXMenuIdx_Count,
};

enum {
  RXMenuIdx_Channel = 0,
#if CAPABILITY_HAS_BUILTIN_HRM
  RXMenuIdx_HRM,
#endif
#if CAPABILITY_HAS_MICROPHONE
  RXMenuIdx_Microphone,
#endif
  RXMenuIdx_RunStop,
  RXMenuIdx_Count,
};

static void prv_txrx_menu_update(AppData *data) {
  layer_mark_dirty(menu_layer_get_layer(&data->txrx_menu_layer));
}

static uint16_t prv_menu_get_num_rows(MenuLayer *menu_layer, uint16_t section, void *context) {
  AppData *data = context;
  if (data->current_test == BTLETestType_TX) {
    uint16_t count = TXMenuIdx_Count;
    if (data->is_unmodulated_cw_enabled) {
      count -= TX_MENU_NUM_PAYLOAD_ROWS; // Payload rows are hidden
    }
    return count;
  } else {
    return RXMenuIdx_Count;
  }
}

static uint16_t prv_compensated_tx_menu_row_idx(const MenuIndex *index, const AppData *data) {
  uint16_t row = index->row;
  if (data->is_unmodulated_cw_enabled && row > TXMenuIdx_UnmodulatedContinuousWave) {
    // Payload length and payload type rows are removed when unmodulated continuous wave is
    // enabled, compensate so the enum still matches:
    row += TX_MENU_NUM_PAYLOAD_ROWS;
  }
  return row;
}

static void prv_menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  AppData *data = context;

  char subtitle_buffer[TXRX_SUBTITLE_LENGTH];
  const char *title = NULL;
  const char *subtitle = NULL;
  if (data->current_test == BTLETestType_TX) {
    switch (prv_compensated_tx_menu_row_idx(index, data)) {
      case TXMenuIdx_Channel:
        title = "Channel";
        itoa_int(data->channel, subtitle_buffer, 10);
        subtitle = subtitle_buffer;
        break;
      case TXMenuIdx_UnmodulatedContinuousWave:
        title = "Unmodulated CW";
        subtitle = data->is_unmodulated_cw_enabled ? "Enabled" : "Disabled";
        break;
      case TXMenuIdx_PayloadLength:
        title = "Payload Length";
        itoa_int(data->payload_length, subtitle_buffer, 10);
        subtitle = subtitle_buffer;
        break;
      case TXMenuIdx_PayloadType:
        title = "Payload Type";
        subtitle = s_payload_names[data->payload_type];
        break;
#if CAPABILITY_HAS_BUILTIN_HRM
      case TXMenuIdx_HRM:
        title = "HRM";
        subtitle = data->is_hrm_enabled ? "Enabled" : "Disabled";
        break;
#endif
#if CAPABILITY_HAS_MICROPHONE
      case TXMenuIdx_Microphone:
        title = "Microphone";
        subtitle = data->is_mic_enabled ? "Enabled" : "Disabled";
        break;
#endif
      case TXMenuIdx_RunStop:
        title = (prv_test_is_running(data)) ? "Stop" : "Run";
        break;
    }
  } else if (data->current_test == BTLETestType_RX) {
    switch (index->row) {
      case RXMenuIdx_Channel:
        title = "Channel";
        itoa_int(data->channel, subtitle_buffer, 10);
        subtitle = subtitle_buffer;
        break;
#if CAPABILITY_HAS_BUILTIN_HRM
      case RXMenuIdx_HRM:
        title = "HRM";
        subtitle = data->is_hrm_enabled ? "Enabled" : "Disabled";
        break;
#endif
#if CAPABILITY_HAS_MICROPHONE
      case RXMenuIdx_Microphone:
        title = "Microphone";
        subtitle = data->is_mic_enabled ? "Enabled" : "Disabled";
        break;
#endif
      case RXMenuIdx_RunStop:
        title = (prv_test_is_running(data)) ? "Stop" : "Run";
        break;
    }
  } else {
    WTF;
  }
  menu_cell_basic_draw(ctx, cell, title, subtitle, NULL);
}

static void prv_menu_select_click(MenuLayer *menu_layer, MenuIndex *index, void *context) {
  AppData *data = context;

  if (data->current_test == BTLETestType_TX) {
    const uint16_t row = prv_compensated_tx_menu_row_idx(index, data);
    if (prv_test_is_running(data) &&
        row != TXMenuIdx_RunStop) { // Can't change params while running
      return;
    }
    switch (row) {
      case TXMenuIdx_Channel:
        prv_txrx_number_window(39, &data->channel, "Channel", data);
        break;
      case TXMenuIdx_UnmodulatedContinuousWave:
        data->is_unmodulated_cw_enabled = !data->is_unmodulated_cw_enabled;
        menu_layer_reload_data(menu_layer);
        break;
      case TXMenuIdx_PayloadLength:
        prv_txrx_number_window(255, &data->payload_length, "Payload Length", data);
        break;
      case TXMenuIdx_PayloadType:
        prv_payload_type_window(data);
        break;
#if CAPABILITY_HAS_BUILTIN_HRM
      case TXMenuIdx_HRM:
        data->is_hrm_enabled = !data->is_hrm_enabled;
        menu_layer_reload_data(menu_layer);
        break;
#endif
#if CAPABILITY_HAS_MICROPHONE
      case TXMenuIdx_Microphone:
        data->is_mic_enabled = !data->is_mic_enabled;
        menu_layer_reload_data(menu_layer);
        break;
#endif
      case TXMenuIdx_RunStop: // Run / Stop
        if (data->current_test_step == BTLETestStep_None) {
          prv_run_test(data);
        } else {
          prv_stop_test(data);
        }
        break;
    }
  } else if (data->current_test == BTLETestType_RX) {
    if (prv_test_is_running(data) &&
        index->row != RXMenuIdx_RunStop) { // Can't change params while running
      return;
    }
    switch (index->row) {
      case RXMenuIdx_Channel:
        prv_txrx_number_window(39, &data->channel, "Channel", data);
        break;
#if CAPABILITY_HAS_BUILTIN_HRM
      case RXMenuIdx_HRM:
        data->is_hrm_enabled = !data->is_hrm_enabled;
        menu_layer_reload_data(menu_layer);
        break;
#endif
#if CAPABILITY_HAS_MICROPHONE
      case RXMenuIdx_Microphone:
        data->is_mic_enabled = !data->is_mic_enabled;
        menu_layer_reload_data(menu_layer);
        break;
#endif
      case RXMenuIdx_RunStop:
        if (data->current_test_step == BTLETestStep_None) {
          prv_run_test(data);
        } else {
          prv_stop_test(data);
        }
        break;
    }
  } else {
    WTF;
  }
}

static void prv_txrx_window_load(Window *window) {
  AppData *data = app_state_get_user_data();

  Layer *window_layer = window_get_root_layer(&data->txrx_window);
  menu_layer_init(&data->txrx_menu_layer, &window_layer->bounds);
  menu_layer_set_callbacks(&data->txrx_menu_layer, data, &(MenuLayerCallbacks) {
        .get_num_rows = prv_menu_get_num_rows,
        .draw_row = prv_menu_draw_row,
        .select_click = prv_menu_select_click,
      });
  layer_add_child(window_layer, menu_layer_get_layer(&data->txrx_menu_layer));
  menu_layer_set_click_config_onto_window(&data->txrx_menu_layer, &data->txrx_window);
}

// Shared unload handler which destroys all common data
static void prv_txrx_window_unload(Window *window) {
  AppData *data = app_state_get_user_data();
  layer_remove_child_layers(window_get_root_layer(&data->txrx_window));
  menu_layer_deinit(&data->txrx_menu_layer);

  if (data->current_test_step != BTLETestStep_None) {
    // Currently outstanding test not complete, finish it.
    switch (data->current_test_step) {
      case BTLETestStep_BTStart:
      case BTLETestStep_BTEnd:
        prv_run_test_step(BTLETestStep_BTEnd, data);
        break;

      case BTLETestStep_BTLETransmitStart:
      case BTLETestStep_BTLEReceiverStart:
        prv_run_test_step(BTLETestStep_BTLEStop, data);
        // fallthrough
      case BTLETestStep_BTLEStop:
        prv_run_test_step(BTLETestStep_BTEnd, data);
        break;
      default:
        WTF;
    }
  }

  data->current_test_step = BTLETestStep_None;
  data->current_test = BTLETestType_None;
}

static void prv_enter_txrx_menu(AppData *data, BTLETestType test) {
  switch (test) {
    case BTLETestType_TX:
    case BTLETestType_RX:
      break; // OK
    default:
      WTF;
  }
  data->current_test = test;

  window_set_window_handlers(&data->txrx_window, &(WindowHandlers){
        .load = prv_txrx_window_load,
        .unload = prv_txrx_window_unload,
      });

  app_window_stack_push(&data->txrx_window, true);
}

//--------------------------------------------------------------------------------
// Main Menu
//--------------------------------------------------------------------------------

static void prv_enter_tx_menu(int index, void *context) {
  prv_enter_txrx_menu(context, BTLETestType_TX);
}

static void prv_enter_rx_menu(int index, void *context) {
  prv_enter_txrx_menu(context, BTLETestType_RX);
}

static void prv_init_main_menu(AppData *data) {
  const SimpleMenuItem menu_items[] = {
    { .title = "BTLE TX", .callback = prv_enter_tx_menu },
    { .title = "BTLE RX", .callback = prv_enter_rx_menu },
  };

  const size_t size = sizeof(menu_items);
  data->main_menu_items = app_malloc_check(size);
  memcpy(data->main_menu_items, menu_items, size);

  data->main_menu_section = (SimpleMenuSection) {
    .num_items = ARRAY_LENGTH(menu_items),
    .items = data->main_menu_items,
  };

  Layer *window_layer = window_get_root_layer(&data->main_menu_window);
  const GRect bounds = window_layer->bounds;
  data->main_menu_layer =
      simple_menu_layer_create(bounds, &data->main_menu_window, &data->main_menu_section, 1, data);
  layer_add_child(window_layer, simple_menu_layer_get_layer(data->main_menu_layer));
}

static void prv_main(void) {
  AppData *data = app_malloc_check(sizeof(*data));
  *data = (AppData) {
    .current_test = BTLETestType_None,
    .current_test_step = BTLETestStep_None,
    .btle_test_semaphore = xSemaphoreCreateBinary(),
  };

  app_state_set_user_data(data);

  window_init(&data->main_menu_window, "");
  window_init(&data->txrx_window, "");
  window_init(&data->payload_window, "");
  prv_status_window_init(data);

  prv_init_main_menu(data);

  app_window_stack_push(&data->main_menu_window, true);

  app_event_loop();
}

const PebbleProcessMd* mfg_btle_app_get_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common.main_func = &prv_main,
    .name = "Test BTLE",
  };
  return (const PebbleProcessMd*) &s_app_info;
}
#endif // BT_CONTROLLER_DA14681
