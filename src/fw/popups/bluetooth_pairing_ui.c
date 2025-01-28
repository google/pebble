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

#define FILE_LOG_COLOR LOG_COLOR_BLUE

#include "bluetooth_pairing_ui.h"

#include "applib/fonts/fonts.h"
#include "applib/ui/kino/kino_layer.h"
#include "applib/ui/kino/kino_reel.h"
#include "applib/ui/ui.h"
#include "applib/ui/window_private.h"
#include "applib/ui/window_stack.h"
#include "comm/bt_lock.h"
#include "kernel/events.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/modals/modal_manager.h"
#include "kernel/ui/system_icons.h"
#include "resource/resource_ids.auto.h"
#include "services/common/i18n/i18n.h"
#include "services/common/light.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "system/passert.h"

#include "util/size.h"

#include <bluetooth/id.h>
#include <bluetooth/pairing_confirm.h>

#include <stdio.h>
#include <string.h>

#define CODE_BUF_SIZE 16
#define MAX_PAIR_STR_LEN 16

typedef enum {
  BTPairingUIStateAwaitingUserConfirmation, // It's possible to go from here straight to Failed
  BTPairingUIStateAwaitingResult,
  BTPairingUIStateSuccess,
  BTPairingUIStateFailed,
} BTPairingUIState;

typedef struct BTPairingUIData {
  Window window;
  KinoLayer kino_layer;
  KinoReel *reel;
  GBitmap *approve_bitmap;
  GBitmap *decline_bitmap;
  ActionBarLayer action_bar_layer;
  Layer info_text_mask_layer;
  PropertyAnimation *info_text_out_animation;
  PropertyAnimation *info_text_in_animation;
  // The info text layers store show the text that prompts the user to pair
  TextLayer info_text_layer;
  char info_text_layer_buffer[MAX_PAIR_STR_LEN];
#ifdef RECOVERY_FW
  GRect pair_text_area;
  GRect above_pair_text_area;
  TextLayer info_text_layer2;
  char info_text_layer2_buffer[MAX_PAIR_STR_LEN];
  int num_strings_shown;
  int translated_str_idx;
#endif
  TextLayer device_name_text_layer;
  char device_name_layer_buffer[BT_DEVICE_NAME_BUFFER_SIZE];
  TextLayer code_text_layer;
  char code_text_layer_buffer[CODE_BUF_SIZE];
  TimerID timer;
  BTPairingUIState ui_state;
  const PairingUserConfirmationCtx *ctx;

} BTPairingUIData;

//! This pointer and the data it points to should only be accessed from KernelMain
static BTPairingUIData *s_data_ptr = NULL;

static void prv_handle_pairing_complete(bool success);

#ifdef RECOVERY_FW  // PRF -- animate through a few hard-coded text strings for "Pair?"

static void prv_animate_info_text(BTPairingUIData *data);

static void prv_info_text_animation_stopped(Animation *anim, bool finished, void *context) {
  if (!s_data_ptr) {
    return;
  }

  // Reset the text box positions
  layer_set_frame(&s_data_ptr->info_text_layer.layer, &s_data_ptr->pair_text_area);
  layer_set_frame(&s_data_ptr->info_text_layer2.layer, &s_data_ptr->above_pair_text_area);

  if (s_data_ptr->ui_state == BTPairingUIStateAwaitingUserConfirmation) {
    // Reschedule animations
    prv_animate_info_text(s_data_ptr);
  }
}

static void prv_cleanup_prf_animations(BTPairingUIData *data) {
  animation_unschedule(property_animation_get_animation(data->info_text_in_animation));
  animation_unschedule(property_animation_get_animation(data->info_text_out_animation));
  layer_set_hidden(&data->info_text_layer2.layer, true);
}


typedef struct {
  const char *string;
  const char *font_key;
} Translation;

static void prv_update_text_layer_with_translation(TextLayer *text_layer,
                                                   const Translation *translation) {
  text_layer_set_text(text_layer, translation->string);
  text_layer_set_font(text_layer, fonts_get_system_font(translation->font_key));
}

static void prv_update_prf_info_text_layers_text(BTPairingUIData *data) {
#if PLATFORM_ROBERT || PLATFORM_CALCULUS
  const char *font_key_default = FONT_KEY_GOTHIC_28_BOLD;
  const char *font_key_japanese = FONT_KEY_MINCHO_24_PAIR;
#else
  const char *font_key_default = FONT_KEY_GOTHIC_24_BOLD;
  const char *font_key_japanese = FONT_KEY_MINCHO_20_PAIR;
#endif
  const Translation english_translation = { "Pair?", font_key_default };
  const Translation translations[] = {
    { "Koppeln?",    font_key_default }, // German
    { "Jumeler?",    font_key_default }, // French
    { "¿Enlazar?",   font_key_default }, // Spanish
    { "Associare?",  font_key_default }, // Italian
    { "Emparelhar?", font_key_default }, // Portuguese
    { "ペアリング",  font_key_japanese }, // Japanese
    { "配对",        font_key_default }, // Chinese (traditional?)
    { "配對",        font_key_default }  // Chinese (simplified?)
  };

  // The strings should be displayed in the following pattern:
  // english, translated, translated, english, translated, translated, ...
  if (data->num_strings_shown % 3 == 0) {
    prv_update_text_layer_with_translation(&data->info_text_layer, &english_translation);
  } else {
    prv_update_text_layer_with_translation(&data->info_text_layer,
                                           &translations[data->translated_str_idx]);
    data->translated_str_idx = (data->translated_str_idx + 1) % ARRAY_LENGTH(translations);
  }

  if ((data->num_strings_shown + 1) % 3 == 0) {
    prv_update_text_layer_with_translation(&data->info_text_layer2, &english_translation);
  } else {
    prv_update_text_layer_with_translation(&data->info_text_layer2,
                                           &translations[data->translated_str_idx]);
  }

  data->num_strings_shown++;
}

static void prv_animate_info_text(BTPairingUIData *data) {
  prv_update_prf_info_text_layers_text(data);

  animation_schedule(property_animation_get_animation(data->info_text_in_animation));
  animation_schedule(property_animation_get_animation(data->info_text_out_animation));
}

static void prv_add_prf_layers(GRect pair_text_area, BTPairingUIData *data) {
  GRect below_pair_text = GRect(0, 38, pair_text_area.size.w, 30);
  GRect above_pair_text = GRect(0, -34, pair_text_area.size.w, 30);

  data->pair_text_area = pair_text_area;
  data->above_pair_text_area = above_pair_text;

  TextLayer *info_text_layer2 = &data->info_text_layer2;
  text_layer_init_with_parameters(info_text_layer2,
                                  &above_pair_text,
                                  data->info_text_layer2_buffer,
                                  fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                                  GColorBlack, GColorClear, GTextAlignmentCenter,
                                  GTextOverflowModeTrailingEllipsis);
  layer_add_child(&data->info_text_mask_layer, &info_text_layer2->layer);

  // The order in which the languages are shown (see prv_update_prf_info_text_layers_text()) means
  // that in order to see each translated language twice we must show 15 total strings.
  // The Bluetooth SPP popup will timeout in 30 seconds so each animation + delay = 30/15 = 2s
  const int animation_duration_ms = 300;
  const int animation_delay_ms = 1700;

  // This is the text that is currently not visible and animates into view
  data->info_text_in_animation =
        property_animation_create_layer_frame(&data->info_text_layer2.layer,
                                              &above_pair_text, &pair_text_area);
  PBL_ASSERTN(data->info_text_in_animation);
  Animation *animation = property_animation_get_animation(data->info_text_in_animation);
  animation_set_auto_destroy(animation, false);
  animation_set_duration(animation, animation_duration_ms);
  animation_set_delay(animation, animation_delay_ms);

  // This is the text that is currently visible and animates out of view
  data->info_text_out_animation =
        property_animation_create_layer_frame(&data->info_text_layer.layer,
                                              &pair_text_area, &below_pair_text);
  PBL_ASSERTN(data->info_text_out_animation);
  animation = property_animation_get_animation(data->info_text_out_animation);
  animation_set_auto_destroy(animation, false);
  animation_set_duration(animation, animation_duration_ms);
  animation_set_delay(animation, animation_delay_ms);

  // We only need a stop handler for one of the animations as they should finish at the same time
  AnimationHandlers handlers = {
    .stopped = prv_info_text_animation_stopped,
  };
  animation_set_handlers(animation, handlers, NULL);
}

static void prv_initialize_info_text(BTPairingUIData *data) {
  prv_animate_info_text(data);
}

static void prv_deinitialize_info_text(BTPairingUIData *data) {
}

#else  // Normal FW -- use i18n text for "Pair?"

static void prv_cleanup_prf_animations(BTPairingUIData *data) {
}

static void prv_add_prf_layers(GRect pair_text_area, BTPairingUIData *data) {
}

static void prv_initialize_info_text(BTPairingUIData *data) {
  strncpy(data->info_text_layer_buffer, (char *)i18n_get("Pair?", data), MAX_PAIR_STR_LEN);
}
static void prv_deinitialize_info_text(BTPairingUIData *data) {
  i18n_free_all(data);
}

#endif


static uint32_t prv_resource_id_for_state(BTPairingUIState state) {
  switch (state) {
    case BTPairingUIStateAwaitingUserConfirmation:
      return RESOURCE_ID_BT_PAIR_CONFIRMATION;
    case BTPairingUIStateAwaitingResult:
      return RESOURCE_ID_BT_PAIR_APPROVE_ON_PHONE;
    case BTPairingUIStateSuccess:
      return RESOURCE_ID_BT_PAIR_SUCCESS;
    case BTPairingUIStateFailed:
      return RESOURCE_ID_BT_PAIR_FAILURE;
    default:
      WTF;
  }
}

static void prv_adjust_background_frame_for_state(BTPairingUIData *data) {
  GAlign alignment;
  const int16_t width_of_sidebar = data->action_bar_layer.layer.frame.size.w;
  const int16_t window_width = data->window.layer.bounds.size.w;
  const int16_t config_width = window_width - width_of_sidebar + 10;
  int16_t x_offset, y_offset, width;

  switch (data->ui_state) {
    case BTPairingUIStateAwaitingUserConfirmation:
      alignment = GAlignTopLeft;
#if PLATFORM_ROBERT || PLATFORM_CALCULUS
      x_offset = 39;
      y_offset = 85;
#else
      x_offset = PBL_IF_RECT_ELSE(10, 31);
      y_offset = PBL_IF_RECT_ELSE(44, 46);
#endif
      width = config_width;
      break;
    case BTPairingUIStateAwaitingResult:
      alignment = GAlignLeft;
#if PLATFORM_ROBERT || PLATFORM_CALCULUS
      x_offset = 76;
      y_offset = 30;
#else
      x_offset = PBL_IF_RECT_ELSE(49, 67);
      y_offset = PBL_IF_RECT_ELSE(22, 25);
#endif
      width = window_width;
      break;
    case BTPairingUIStateFailed:
    case BTPairingUIStateSuccess:
      alignment = GAlignTop;
#if PLATFORM_ROBERT || PLATFORM_CALCULUS
      x_offset = 0;
      y_offset = 59;
#else
      x_offset = 2;
      y_offset = PBL_IF_RECT_ELSE(30, 36);
#endif
      width = window_width;
      break;
    default:
      WTF;
  }

  GRect kino_area;
  kino_area = GRect(x_offset, y_offset, width, data->window.layer.bounds.size.h);
  kino_layer_set_alignment(&data->kino_layer, alignment);
  layer_set_frame(&data->kino_layer.layer, &kino_area);

  kino_layer_set_reel_with_resource(&data->kino_layer, prv_resource_id_for_state(data->ui_state));
}

static void prv_send_response(bool is_confirmed) {
  if (!s_data_ptr) {
    return;
  }

  bt_lock();
  bt_driver_pairing_confirm(s_data_ptr->ctx, is_confirmed);
  bt_unlock();
}

static bool prv_has_device_name(BTPairingUIData *data) {
  return (strlen(data->device_name_layer_buffer) != 0);
}

static bool prv_has_confirmation_token(BTPairingUIData *data) {
  return (strlen(data->code_text_layer_buffer) != 0);
}

static void prv_exit_awaiting_user_confirmation(BTPairingUIData *data) {
  // Remove UI components that are not needed any more after the user input confirmation screen:
  prv_cleanup_prf_animations(data);
  layer_set_hidden(&data->info_text_layer.layer, true);

  if (prv_has_device_name(data)) {
    layer_remove_from_parent(&data->device_name_text_layer.layer);
  }
  if (prv_has_confirmation_token(data)) {
    layer_remove_from_parent(&data->code_text_layer.layer);
  }

  // Disable all buttons in this screen:
  action_bar_layer_remove_from_window(&data->action_bar_layer);
  action_bar_layer_set_click_config_provider(&data->action_bar_layer, NULL);
}

static void prv_confirm_click_handler(ClickRecognizerRef recognizer, void *ctx) {
  Window *window = (Window *) ctx;
  BTPairingUIData *data = window_get_user_data(window);
  PBL_ASSERTN(data->ui_state == BTPairingUIStateAwaitingUserConfirmation);
  prv_exit_awaiting_user_confirmation(data);
  data->ui_state = BTPairingUIStateAwaitingResult;
  prv_send_response(true /* is_confirmed */);
  prv_adjust_background_frame_for_state(data);
}

static void prv_decline_click_handler(ClickRecognizerRef recognizer, void *ctx) {
  Window *window = (Window *) ctx;
  BTPairingUIData *data = window_get_user_data(window);
  PBL_ASSERTN(data->ui_state == BTPairingUIStateAwaitingUserConfirmation);
  prv_send_response(false /* is_confirmed */);
  // Not updating ui_state, the handler is capable of dealing with transitioning from
  // BTPairingUIStateAwaitingUserConfirmation directly to BTPairingUIStateFailed
  prv_handle_pairing_complete(false /* success */);
}

static void prv_user_confirmation_click_config_provider(void *unused) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_confirm_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_decline_click_handler);
}

static void prv_window_load(Window *window) {
  BTPairingUIData *data = window_get_user_data(window);
  window_set_background_color(&data->window, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));

  const int32_t width_of_action_bar_with_padding = ACTION_BAR_WIDTH + PBL_IF_RECT_ELSE(2, -4);
  const int32_t width = window->layer.bounds.size.w - width_of_action_bar_with_padding;
  const int32_t x_offset = PBL_IF_RECT_ELSE(0, 22);
#if PLATFORM_ROBERT || PLATFORM_CALCULUS
  const int32_t info_text_y_offset = 36;
#else
  const int32_t info_text_y_offset = PBL_IF_RECT_ELSE(10, 12);
#endif

  KinoLayer *kino_layer = &data->kino_layer;
  kino_layer_init(kino_layer, &window->layer.bounds);
  layer_add_child(&window->layer, &kino_layer->layer);

#if PLATFORM_ROBERT || PLATFORM_CALCULUS
  GRect pair_text_area = GRect(0, -2, width, 44);
#else
  GRect pair_text_area = GRect(0, -2, width, 30);
#endif

#if PLATFORM_ROBERT || PLATFORM_CALCULUS
  layer_set_frame(&data->info_text_mask_layer, &GRect(x_offset, info_text_y_offset, width, 30));
#else
  layer_set_frame(&data->info_text_mask_layer, &GRect(x_offset, info_text_y_offset, width, 26));
#endif
  layer_set_clips(&data->info_text_mask_layer, true);
  layer_add_child(&window->layer, &data->info_text_mask_layer);

  TextLayer *info_text_layer = &data->info_text_layer;
  text_layer_init_with_parameters(info_text_layer,
                                  &pair_text_area,
                                  data->info_text_layer_buffer,
#if PLATFORM_ROBERT || PLATFORM_CALCULUS
                                  fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
#else
                                  fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
#endif
                                  GColorBlack, GColorClear, GTextAlignmentCenter,
                                  GTextOverflowModeTrailingEllipsis);
  layer_add_child(&data->info_text_mask_layer, &info_text_layer->layer);

  ActionBarLayer *action_bar_layer = &data->action_bar_layer;
  action_bar_layer_init(action_bar_layer);
  action_bar_layer_add_to_window(action_bar_layer, window);
  data->approve_bitmap = gbitmap_create_with_resource(RESOURCE_ID_ACTION_BAR_ICON_CHECK);
  data->decline_bitmap = gbitmap_create_with_resource(RESOURCE_ID_ACTION_BAR_ICON_X);
  action_bar_layer_set_click_config_provider(action_bar_layer,
                                             prv_user_confirmation_click_config_provider);
  action_bar_layer_set_icon(action_bar_layer, BUTTON_ID_UP, data->approve_bitmap);
  action_bar_layer_set_icon(action_bar_layer, BUTTON_ID_DOWN, data->decline_bitmap);
  action_bar_layer_set_context(action_bar_layer, data);

  prv_add_prf_layers(pair_text_area, data);

  const int16_t y_offset = PBL_IF_RECT_ELSE(0, 2);
  // Device name:
  if (prv_has_device_name(data)) {
    TextLayer *device_name_layer = &data->device_name_text_layer;
    text_layer_init_with_parameters(device_name_layer,
                                    &GRect(x_offset, 122 + y_offset, width - x_offset, 30),
                                    data->device_name_layer_buffer,
                                    fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                                    GColorBlack, GColorClear, GTextAlignmentCenter,
                                    GTextOverflowModeTrailingEllipsis);
    layer_add_child(&window->layer, &device_name_layer->layer);
  }
  // Confirmation token:
  if (prv_has_confirmation_token(data)) {
    TextLayer *code_text_layer = &data->code_text_layer;
    text_layer_init_with_parameters(code_text_layer,
                                    &GRect(x_offset, 148 + y_offset, width, 30),
                                    data->code_text_layer_buffer,
                                    fonts_get_system_font(FONT_KEY_GOTHIC_14),
                                    GColorBlack, GColorClear, GTextAlignmentCenter,
                                    GTextOverflowModeTrailingEllipsis);
    layer_add_child(&window->layer, &code_text_layer->layer);
  }

  prv_adjust_background_frame_for_state(data);

  prv_initialize_info_text(data);
}

static void prv_window_unload(Window *window) {
  BTPairingUIData *data = window_get_user_data(window);
  if (data) {
    kino_layer_deinit(&data->kino_layer);
    text_layer_deinit(&data->info_text_layer);
    text_layer_deinit(&data->device_name_text_layer);
    text_layer_deinit(&data->code_text_layer);
    gbitmap_destroy(data->approve_bitmap);
    gbitmap_destroy(data->decline_bitmap);
    action_bar_layer_deinit(&data->action_bar_layer);
    new_timer_delete(data->timer);
    if (data->ui_state == BTPairingUIStateAwaitingUserConfirmation) {
      prv_send_response(false /* is_confirmed */);
    }
    prv_deinitialize_info_text(data);
    property_animation_destroy(data->info_text_in_animation);
    property_animation_destroy(data->info_text_out_animation);
    kernel_free(data);
  }

  s_data_ptr = NULL;
}

static void prv_show_failure_kernel_main_cb(void *unused) {
  prv_handle_pairing_complete(false /* success */);
}

static void prv_pairing_timeout_timer_callback(void *unused) {
  PBL_LOG(LOG_LEVEL_WARNING, "SSP timeout fired!");
  launcher_task_add_callback(prv_show_failure_kernel_main_cb, NULL);
}

static void prv_pop_window(void) {
  if (s_data_ptr) {
    window_stack_remove(&s_data_ptr->window, true /* animated */);
  }
}

static void prv_pop_window_kernel_main_cb(void* unused) {
  prv_pop_window();
}

static void prv_pop_window_timer_callback(void *unused) {
  launcher_task_add_callback(prv_pop_window_kernel_main_cb, NULL);
}

static void prv_push_pairing_window(void) {
  BTPairingUIData *data = s_data_ptr;
  Window *window = &data->window;
  window_init(window, WINDOW_NAME("Bluetooth SSP"));
  window_set_window_handlers(window, &(WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_set_user_data(window, data);
  window_set_overrides_back_button(window, true);

  modal_window_push(window, ModalPriorityCritical, true /* animated */);

  vibes_double_pulse();
  light_enable_interaction();

  // This timeout is 0.5s longer than the BT Spec's timeout, to decrease the chances of getting
  // a success confirmation right at the max allowed time of 30 secs:
  const uint32_t timeout_ms = (30 * 1000) + 500;
  data->timer = new_timer_create();
  bool success = new_timer_start(data->timer, timeout_ms, prv_pairing_timeout_timer_callback, data,
                                 0 /* flags */);
  PBL_ASSERTN(success);
}

static void prv_pop_click_handler(ClickRecognizerRef recognizer, void *ctx) {
  prv_pop_window();
}

static void prv_success_or_failure_click_config_provider(void *unused) {
  window_single_click_subscribe(BUTTON_ID_BACK, prv_pop_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_pop_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_pop_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_pop_click_handler);
}

static void prv_create_new_pairing_data(void) {
  // If we already have a window up, remove that before we push another
  if (s_data_ptr) {
    window_stack_remove(&s_data_ptr->window, true /* animated */);
  }

  BTPairingUIData *data = kernel_malloc_check(sizeof(BTPairingUIData));
  *data = (BTPairingUIData){};
  data->ui_state = BTPairingUIStateAwaitingUserConfirmation;
  s_data_ptr = data;
}

static void prv_handle_confirmation_request(const PairingUserConfirmationCtx *ctx,
                                            PebbleBluetoothPairingConfirmationInfo *info) {
  prv_create_new_pairing_data();

  s_data_ptr->ctx = ctx;

  strncpy(s_data_ptr->device_name_layer_buffer, info->device_name ?: "",
          sizeof(s_data_ptr->device_name_layer_buffer));
  strncpy(s_data_ptr->code_text_layer_buffer, info->confirmation_token ?: "",
          sizeof(s_data_ptr->code_text_layer_buffer));

  prv_push_pairing_window();
}

static void prv_handle_pairing_complete(bool success) {
  if (!s_data_ptr) {
    PBL_LOG(LOG_LEVEL_WARNING, "Dialog was not present, but got complete (%u) event", success);
    return;
  }

  BTPairingUIData *data = s_data_ptr;
  if (data->ui_state == BTPairingUIStateAwaitingUserConfirmation) {
    prv_exit_awaiting_user_confirmation(data);
  } else if (data->ui_state != BTPairingUIStateAwaitingResult) {
    PBL_LOG(LOG_LEVEL_WARNING,
            "Got completion (%u) but not right state", success);
    return;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Got Completion! %u", success);
  data->ui_state = success ? BTPairingUIStateSuccess : BTPairingUIStateFailed;
  prv_adjust_background_frame_for_state(data);

  if (!new_timer_stop(data->timer)) {
    // Timer was already executing...
    if (success) {
      PBL_LOG(LOG_LEVEL_WARNING, "Timeout cb executing while received successful completion event");
    }
  }

  // On failure, leave the message on screen for 60 seconds, on success, only for 5 seconds:
  const uint32_t timeout_ms = (success ? 5 : 60) * 1000;
  new_timer_start(data->timer, timeout_ms, prv_pop_window_timer_callback, NULL, 0 /* flags */);

  window_set_click_config_provider(&data->window, prv_success_or_failure_click_config_provider);

  vibes_short_pulse();
  light_enable_interaction();
}

void bluetooth_pairing_ui_handle_event(PebbleBluetoothPairEvent *event) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  switch (event->type) {
    case PebbleBluetoothPairEventTypePairingUserConfirmation:
      prv_handle_confirmation_request(event->ctx, event->confirmation_info);
      break;

    case PebbleBluetoothPairEventTypePairingComplete:
      if (s_data_ptr && s_data_ptr->ctx == event->ctx) {
        prv_handle_pairing_complete(event->success);
      } else {
        PBL_LOG(LOG_LEVEL_ERROR, "Got complete event for unknown process %p vs %p",
                event->ctx, s_data_ptr ? s_data_ptr->ctx : NULL);
      }
      break;

    default:
      WTF;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// BT Driver callback implementations:

static void prv_put_pairing_event(const PebbleBluetoothPairEvent *pair_event) {
  PebbleEvent event = {
    .type = PEBBLE_BT_PAIRING_EVENT,
    .bluetooth.pair = *pair_event,
  };
  event_put(&event);
}

static void prv_copy_string_and_move_cursor(const char *in_str, char **out_str, uint8_t **cursor) {
  if (!in_str) {
    return;
  }
  size_t str_size_bytes = strlen(in_str) + 1;
  strncpy((char *)*cursor, in_str, str_size_bytes);
  *out_str = (char *) *cursor;
  *cursor += str_size_bytes;
}

void bt_driver_cb_pairing_confirm_handle_request(const PairingUserConfirmationCtx *ctx,
                                                 const char *device_name,
                                                 const char *confirmation_token) {
  // events.c clean-up (see event_deinit) can only clean up one associated heap allocation,
  // so put everything in a single buffer:
  size_t device_name_len = device_name ? (strlen(device_name) + 1) : 0;
  size_t token_len = confirmation_token ? (strlen(confirmation_token) + 1) : 0;
  size_t info_len = (sizeof(PebbleBluetoothPairingConfirmationInfo) + device_name_len + token_len);
  uint8_t *cursor = (uint8_t *)kernel_zalloc_check(info_len);

  PebbleBluetoothPairingConfirmationInfo *confirmation_info =
      (PebbleBluetoothPairingConfirmationInfo *)cursor;
  cursor += sizeof(PebbleBluetoothPairingConfirmationInfo);

  char *device_name_copy = NULL;
  prv_copy_string_and_move_cursor(device_name, &device_name_copy, &cursor);

  char *confirmation_token_copy = NULL;
  prv_copy_string_and_move_cursor(confirmation_token, &confirmation_token_copy, &cursor);

  *confirmation_info = (PebbleBluetoothPairingConfirmationInfo) {
    .device_name = device_name_copy,
    .confirmation_token = confirmation_token_copy,
  };
  PebbleBluetoothPairEvent pair_event = {
    .type = PebbleBluetoothPairEventTypePairingUserConfirmation,
    .ctx = ctx,
    .confirmation_info = confirmation_info,
  };
  prv_put_pairing_event(&pair_event);
}

void bt_driver_cb_pairing_confirm_handle_completed(const PairingUserConfirmationCtx *ctx,
                                                   bool success) {

  PebbleBluetoothPairEvent pair_event = {
    .type = PebbleBluetoothPairEventTypePairingComplete,
    .ctx = ctx,
    .success = success,
  };
  prv_put_pairing_event(&pair_event);
}
