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

#include "settings_factory_reset.h"
#include "settings_menu.h"
#include "settings_system.h"
#include "settings_window.h"

#include "applib/fonts/fonts.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/dialogs/actionable_dialog.h"
#include "applib/ui/dialogs/confirmation_dialog.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/expandable_dialog.h"
#include "applib/ui/option_menu_window.h"
#include "applib/ui/ui.h"
#include "kernel/core_dump.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/modals/modal_manager.h"
#include "mfg/mfg_info.h"
#include "mfg/mfg_serials.h"
#include "resource/resource_ids.auto.h"
#include "services/common/bluetooth/local_id.h"
#include "services/common/i18n/i18n.h"
#include "services/common/status_led.h"
#include "services/common/system_task.h"
#include "services/normal/stationary.h"
#include "shell/normal/battery_ui.h"
#include "shell/prefs.h"
#include "system/bootbits.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/size.h"
#include "util/time/time.h"
#include "system/version.h"

#include "services/normal/activity/activity.h"

#include <stdio.h>
#include <string.h>

#include "settings_certifications.h"

enum {
  SystemInformationItemBtAddress = 0,
  SystemInformationItemFirmware,
  SystemInformationItemLanguage,
  SystemInformationItemRecovery,
  SystemInformationItemBootloader,
  SystemInformationItemHardware,
  SystemInformationItemSerial,
  SystemInformationItemUptime,
  SystemInformationItemLegal,
  SystemInformationItem_Count,
};


typedef struct SystemCertificationData SystemCertificationData;

typedef struct SystemCertificationMenuItem {
    void (*draw_cell_fn)(
        GContext *ctx, const Layer *cell_layer, SystemCertificationData *cd,
        bool is_selected, const void *arg1, const void *arg2);
    const void *arg1;
    const void *arg2;
    void (*select_cb)(SystemCertificationData *cd);
} SystemCertificationMenuItem;

typedef struct SystemCertificationData {
  GBitmap fcc_mark;
  GBitmap kcc_mark;
  GBitmap ce_mark;
  GBitmap weee_mark;
  GBitmap r_mark;
  GBitmap t_mark;
  GBitmap aus_rcm_mark;
  GBitmap nom_nyce_mark;

  GBitmap **regulatory_marks;
  uint8_t regulatory_marks_count;
  // For building up regulatory marks cells when constructing the menu
  uint8_t current_regulatory_marks_cell_start_idx;
  uint8_t num_regulatory_marks_in_current_cell;
  uint16_t current_regulatory_marks_cell_width;

  SystemCertificationMenuItem *menu_items;
  uint16_t menu_count;

  Window kcc_window;
  BitmapLayer bmp_layer;
  TextLayer title_text;
  TextLayer info_text;
  StatusBarLayer status_layer;
} SystemCertificationData;

typedef struct SystemInformationData {
  FirmwareMetadata recovery_fw_metadata;
  char bt_mac_addr[BT_DEVICE_ADDRESS_FMT_BUFFER_SIZE];
  char boot_version_string[(sizeof(uint32_t) * 2) + 3];
  char recovery_version_string[sizeof(TINTIN_METADATA.version_tag)];
  // Ensure that OTP values are null-terminated
  char serial_string[MFG_SERIAL_NUMBER_SIZE + 1];
  char hw_version_string[MFG_HW_VERSION_SIZE + 1];
  char uptime_string[16]; // "xxd xxh xxm xxs"
  char const * subtitle_text[SystemInformationItem_Count];
  char language_string[16];
} SystemInformationData;

typedef struct SettingsSystemData {
  SettingsCallbacks callbacks;

  SystemInformationData information_data;
  SystemCertificationData certification_data;

  // The following components are shared by information, and certification.
  Window window;
  MenuLayer menu_layer;
  StatusBarLayer status_layer;
} SettingsSystemData;

typedef enum {
  SystemMenuItemInformation,
  SystemMenuItemCertification,
  SystemMenuItemStationaryToggle,
  SystemMenuItemShutDown,
  SystemMenuItemFactoryReset,
  SystemMenuItem_Count,
} SystemMenuItem;

static const char *s_item_titles[SystemMenuItem_Count] = {
  [SystemMenuItemInformation]  = i18n_noop("Information"),
  [SystemMenuItemCertification] = i18n_noop("Certification"),
  [SystemMenuItemStationaryToggle] = i18n_noop("Stand-By Mode"),
  [SystemMenuItemShutDown]      = i18n_noop("Shut Down"),
  [SystemMenuItemFactoryReset]  = i18n_noop("Factory Reset"),
};

// Common status bar component is used across all windows that need them.
// This will init it and set the correct style to be used within the settings
// app.
static void prv_init_status_bar(StatusBarLayer *status_layer, Window *window, const char *text) {
  status_bar_layer_init(status_layer);
  status_bar_layer_set_title(status_layer, text, false, false);
  status_bar_layer_set_separator_mode(status_layer, OPTION_MENU_STATUS_SEPARATOR_MODE);
  status_bar_layer_set_colors(status_layer, GColorWhite, GColorBlack);
  layer_add_child(&window->layer, status_bar_layer_get_layer(status_layer));
}

// Deinit the common status bar component.
static void prv_deinit_status_bar(StatusBarLayer *status_layer) {
  layer_remove_from_parent(status_bar_layer_get_layer(status_layer));
  status_bar_layer_deinit(status_layer);
}

// Dialog callbacks for confirmation.
////////////////////////////////////////////////////
static ConfirmationDialog *prv_settings_confirm(const char *title, const char *text,
                                                uint32_t resource_id) {
  ConfirmationDialog *confirmation_dialog = confirmation_dialog_create(title);
  Dialog *dialog = confirmation_dialog_get_dialog(confirmation_dialog);

  dialog_set_text(dialog, i18n_get(text, confirmation_dialog));
  dialog_set_background_color(dialog, GColorRed);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_icon(dialog, resource_id);

  i18n_free_all(confirmation_dialog);

  return confirmation_dialog;
}

// Information Window
//////////////////////

static const char* s_information_titles[SystemInformationItem_Count] = {
  [SystemInformationItemBtAddress] = i18n_noop("BT Address"),
  [SystemInformationItemFirmware] = i18n_noop("Firmware"),
  [SystemInformationItemLanguage] = i18n_noop("Language"),
  [SystemInformationItemRecovery] = i18n_noop("Recovery"),
  [SystemInformationItemBootloader] = i18n_noop("Bootloader"),
  [SystemInformationItemHardware] = i18n_noop("Hardware"),
  [SystemInformationItemSerial] = i18n_noop("Serial"),
  [SystemInformationItemUptime] = i18n_noop("Uptime"),
  [SystemInformationItemLegal] = i18n_noop("Legal")
};

static void prv_populate_uptime_string(SystemInformationData* data) {
  uint32_t seconds_since_reboot = time_get_uptime_seconds();

  uint32_t days, hours, minutes, seconds;
  time_util_split_seconds_into_parts(seconds_since_reboot, &days, &hours, &minutes, &seconds);

  sniprintf(data->uptime_string, sizeof(data->uptime_string),
            "%"PRIu32"d %"PRIu32"h %"PRIu32"m %"PRIu32"s", days, hours, minutes, seconds);
}

static void prv_information_draw_row_callback(GContext* ctx, const Layer *cell_layer,
                                              MenuIndex *cell_index, void *context) {
  PBL_ASSERTN(cell_index->section == 0);
  PBL_ASSERTN(cell_index->row < SystemInformationItem_Count);

  SettingsSystemData *data = (SettingsSystemData *) context;
  SystemInformationData *info = &data->information_data;

  const char *title = i18n_get(s_information_titles[cell_index->row], data);
  menu_cell_basic_draw(ctx, cell_layer, title, info->subtitle_text[cell_index->row], NULL);
}

int16_t prv_information_get_cell_height_callback(MenuLayer *menu_layer,
                                                 MenuIndex *cell_index, void *context) {
  return PBL_IF_RECT_ELSE(menu_cell_basic_cell_height(),
                          (menu_layer_is_index_selected(menu_layer, cell_index) ?
                           MENU_CELL_ROUND_FOCUSED_SHORT_CELL_HEIGHT :
                           MENU_CELL_ROUND_UNFOCUSED_TALL_CELL_HEIGHT));
}

static uint16_t prv_information_get_num_rows_callback(MenuLayer *menu_layer,
                                                      uint16_t section_index, void *context) {
  return SystemInformationItem_Count;
}

static void prv_coredump_confirm_cb(ClickRecognizerRef recognizer, void *context) {
  core_dump_reset(true /* force_overwrite */);
}

static void prv_confirm_pop(ClickRecognizerRef recognizer, void *context) {
  confirmation_dialog_pop((ConfirmationDialog *)context);
}

static void prv_coredump_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_coredump_confirm_cb);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_confirm_pop);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_confirm_pop);
}

static void prv_coredump_reset_confirm_cb(ClickRecognizerRef recognizer, void *context) {
  ExpandableDialog *s_dialog = expandable_dialog_create("Core Dump Reset");
  Dialog *dialog = expandable_dialog_get_dialog(s_dialog);

  dialog_set_text(dialog, i18n_get("The core dump was created successfully. "
      "Please create a support request as usual.", s_dialog));
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_background_color(dialog, GColorRed);

  i18n_free_all(s_dialog);

  app_expandable_dialog_push(s_dialog);
  confirmation_dialog_pop((ConfirmationDialog *)context);
}

static void prv_coredump_reset_decline_cb(ClickRecognizerRef recognizer, void *context) {
  ConfirmationDialog *confirmation_dialog = prv_settings_confirm("Core Dump",
      i18n_noop("Core dump and reboot?"), RESOURCE_ID_GENERIC_QUESTION_LARGE);
  confirmation_dialog_set_click_config_provider(confirmation_dialog,
      prv_coredump_click_config);
  app_confirmation_dialog_push(confirmation_dialog);
  confirmation_dialog_pop((ConfirmationDialog *)context);
}

static void prv_information_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_coredump_reset_confirm_cb);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_coredump_reset_decline_cb);
}

static void prv_information_long_click_callback(MenuLayer *menu_layer,
                                                MenuIndex *cell_index,
                                                void *callback_context) {
  if (cell_index->row == SystemInformationItemFirmware) {
    ConfirmationDialog *confirmation_dialog = prv_settings_confirm("Information",
        i18n_noop("Did your Pebble reset?"), RESOURCE_ID_RESULT_FAILED_LARGE);

    confirmation_dialog_set_click_config_provider(confirmation_dialog,
        prv_information_click_config);

    app_confirmation_dialog_push(confirmation_dialog);
  }
}

#include "drivers/led_controller.h"
#include "system/rtc_registers.h"
static void prv_color_led_easter_egg(void) {
#if CAPABILITY_HAS_LED
  static int i = 0;

  // Simple test code to exercise each of the LEDs in the RGB cluster.
  // Start after 3 clicks
  switch (i) {
    case 3:
      led_controller_rgb_set_color(LED_RED);
      break;
    case 4: led_controller_rgb_set_color(LED_GREEN); break;
    case 5: led_controller_rgb_set_color(LED_BLUE); break;
    case 6: led_controller_rgb_set_color(LED_BLACK); i = 2; break;
    default: break;
  }

  i = (i + 1)%7;
#endif
}

static void prv_information_select_callback(MenuLayer *menu_layer,
                                            MenuIndex *cell_index,
                                            void *context) {
  prv_color_led_easter_egg();
}

static void prv_information_window_load(Window *window) {
  SettingsSystemData *data = (SettingsSystemData*) window_get_user_data(window);

  prv_init_status_bar(&data->status_layer, &data->window, i18n_get("Information", data));

  // Create the menu
  MenuLayer *menu_layer = &data->menu_layer;
  GRect bounds = data->window.layer.bounds;
  const GEdgeInsets menu_layer_insets = (GEdgeInsets) {
    .top = STATUS_BAR_LAYER_HEIGHT,
    .bottom = PBL_IF_RECT_ELSE(0, STATUS_BAR_LAYER_HEIGHT)
  };
  bounds = grect_inset(bounds, menu_layer_insets);
  menu_layer_init(menu_layer, &bounds);
  menu_layer_set_callbacks(menu_layer, data, &(MenuLayerCallbacks) {
    .get_num_rows = prv_information_get_num_rows_callback,
    .get_cell_height = prv_information_get_cell_height_callback,
    .draw_row = prv_information_draw_row_callback,
    .select_click = prv_information_select_callback,
    .select_long_click = prv_information_long_click_callback,
  });
  menu_layer_set_highlight_colors(menu_layer, SETTINGS_MENU_HIGHLIGHT_COLOR, GColorWhite);
  menu_layer_set_click_config_onto_window(menu_layer, &data->window);

  layer_add_child(&data->window.layer, menu_layer_get_layer(menu_layer));
}

static void prv_information_window_unload(Window *window) {
  SettingsSystemData *data = (SettingsSystemData*) window_get_user_data(window);
  menu_layer_deinit(&data->menu_layer);
  prv_deinit_status_bar(&data->status_layer);
}

static void prv_information_window_push(SettingsSystemData *data) {
  SystemInformationData *info = &data->information_data;
  *info = (SystemInformationData){};

  bool success = version_copy_recovery_fw_version(info->recovery_version_string,
                                                  ARRAY_LENGTH(info->recovery_version_string));
  if (success == false) {
    info->recovery_version_string[0] = '\0';
  }

  sniprintf(info->boot_version_string, sizeof(info->boot_version_string),
            "0x%" PRIx32, boot_version_read());
  bt_local_id_copy_address_mac_string(info->bt_mac_addr);

  // Ensure OTP strings are null-terminated
  mfg_info_get_serialnumber(info->serial_string, MFG_SERIAL_NUMBER_SIZE + 1);
  mfg_info_get_hw_version(info->hw_version_string, MFG_HW_VERSION_SIZE + 1);
  prv_populate_uptime_string(info);

  sniprintf(info->language_string, sizeof(info->language_string),
            "%s, v%u", i18n_get_locale(), i18n_get_version());

  info->subtitle_text[SystemInformationItemBtAddress]  = info->bt_mac_addr;
  info->subtitle_text[SystemInformationItemFirmware]   = (char*)TINTIN_METADATA.version_tag;
  info->subtitle_text[SystemInformationItemLanguage]   = info->language_string;
  info->subtitle_text[SystemInformationItemRecovery]   = info->recovery_version_string;
  info->subtitle_text[SystemInformationItemBootloader] = info->boot_version_string;
  info->subtitle_text[SystemInformationItemHardware]   = info->hw_version_string;
  info->subtitle_text[SystemInformationItemSerial]     = info->serial_string;
  info->subtitle_text[SystemInformationItemUptime]     = info->uptime_string;
  info->subtitle_text[SystemInformationItemLegal]      = "pebble.com/legal";

  window_init(&data->window, WINDOW_NAME("System Information"));
  window_set_user_data(&data->window, data);
  window_set_window_handlers(&data->window, &(WindowHandlers) {
    .load = prv_information_window_load,
    .unload = prv_information_window_unload,
  });

  app_window_stack_push(&data->window, true);
}


// Certification Window
///////////////////////

int16_t prv_certification_get_cell_height_callback(MenuLayer *menu_layer,
                                                   MenuIndex *cell_index,
                                                   void *context) {
  return PBL_IF_RECT_ELSE(menu_cell_basic_cell_height(),
                          (menu_layer_is_index_selected(menu_layer, cell_index) ?
                           MENU_CELL_ROUND_FOCUSED_SHORT_CELL_HEIGHT :
                           MENU_CELL_ROUND_UNFOCUSED_TALL_CELL_HEIGHT));
}

static void prv_draw_mark_with_inversion(GContext *ctx, GBitmap *mark, const GRect *box,
                                         bool inverted) {
  graphics_context_set_compositing_mode(ctx, GCompOpTint);
  graphics_draw_bitmap_in_rect(ctx, mark, box);
}

static int16_t prv_draw_generic_mark(GContext *ctx, GBitmap *mark, GPoint origin, bool highlight) {
  GRect box = (GRect) {
    .origin = origin,
    .size = mark->bounds.size
  };
  prv_draw_mark_with_inversion(ctx, mark, &box, highlight);
  return origin.x + box.size.w;
}

#define MARK_PADDING 10

static void prv_draw_rt_cell_rect(GContext *ctx, const Layer *cell_layer, GBitmap *mark,
                                  const char *text, UNUSED bool is_selected) {
  int16_t x = (MARK_PADDING / 2);
  GRect box = cell_layer->bounds;
  const bool highlight = menu_cell_layer_is_highlighted(cell_layer);
  const int16_t vertical_padding = 6;
  const GPoint mark_origin = GPoint(x, vertical_padding);
  x = prv_draw_generic_mark(ctx, mark, mark_origin, highlight) + (MARK_PADDING / 2);
  box.origin.x = x;
  box.origin.y += 8;
  box.size.w -= x;
  box.size.h -= 8;
  const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void prv_draw_rt_cell_round(GContext *ctx, const Layer *cell_layer, GBitmap *mark,
                                   const char *text, bool is_selected) {
  GRect rt_rect = cell_layer->bounds;
  const int16_t horizontal_padding = 10;
  const int16_t vertical_padding = is_selected ? 6 : 0;
  rt_rect = grect_inset_internal(rt_rect, horizontal_padding, vertical_padding);

  // Calculate where the mark should be drawn
  const GSize mark_size = mark->bounds.size;
  GRect mark_rect = (GRect) { .size = mark_size };
  // If the cell is selected, align the mark at the top center so we can draw the text below it
  const GAlign alignment = is_selected ? GAlignTop : GAlignCenter;
  grect_align(&mark_rect, &rt_rect, alignment, true /* clip */);

  // Draw the mark
  const bool highlight = menu_cell_layer_is_highlighted(cell_layer);
  prv_draw_generic_mark(ctx, mark, mark_rect.origin, highlight);

  // Only draw the text if the cell is selected
  if (is_selected) {
    const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    GRect text_rect = rt_rect;
    text_rect.size.h = fonts_get_font_height(font);
    grect_align(&text_rect, &rt_rect, GAlignBottom, true /* clip */);
    graphics_draw_text(ctx, text, font, text_rect, GTextOverflowModeFill, GTextAlignmentCenter,
                       NULL);
  }
}

static void prv_draw_rt_cell(
    GContext *ctx, const Layer *cell_layer, SystemCertificationData *cd,
    bool is_selected, const void *arg1, const void *arg2) {
  GBitmap *mark = (GBitmap *)arg1;
  const char *text = arg2;
  PBL_IF_RECT_ELSE(prv_draw_rt_cell_rect,
                   prv_draw_rt_cell_round)(ctx, cell_layer, mark, text, is_selected);
}

static void prv_draw_fcc_cell_round(
    GContext *ctx, const GRect *cell_layer_bounds, const char *fcc_title,
    const char *fcc_number_subtitle, GBitmap *fcc_mark_icon,
    bool cell_is_selected, bool cell_is_highlighted) {
  const GFont fcc_title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  const int16_t fcc_title_font_cap_padding = 10;
  const GFont fcc_number_subtitle_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  const uint8_t fcc_title_height = fonts_get_font_height(fcc_title_font);
  const uint8_t fcc_number_subtitle_height = fonts_get_font_height(fcc_number_subtitle_font);
  const GTextOverflowMode text_overflow_mode = GTextOverflowModeFill;

  graphics_context_set_text_color(ctx, cell_is_highlighted ? GColorWhite : GColorBlack);

  // Calculate the container of the FCC cell content and center it within the cell
  const int16_t title_and_icon_width = 50;
  GRect container_rect = (GRect) {
    .size = GSize(title_and_icon_width, fcc_title_height - fcc_title_font_cap_padding)
  };
  if (cell_is_selected) {
    // Note that we don't subtract the subtitle font's cap padding from the container height
    // because it exactly matches the vertical spacing we want between the title and subtitle
    container_rect.size.h += fcc_number_subtitle_height;
  }
  grect_align(&container_rect, cell_layer_bounds, GAlignCenter, true /* clip */);

  // Draw the FCC title in the top left of the container
  // We'll reuse this box for the title, subtitle, and icon frames
  GRect box = (GRect) { .size = GSize(container_rect.size.w, fcc_title_height) };
  grect_align(&box, &container_rect, GAlignTopLeft, true /* clip */);
  box.origin.y -= fcc_title_font_cap_padding;
  graphics_draw_text(ctx, fcc_title, fcc_title_font, box, text_overflow_mode, GTextAlignmentLeft,
                     NULL);

  // If the cell is selected, draw the FCC # subtitle centered at the bottom of the container
  if (cell_is_selected) {
    const int16_t fcc_number_subtitle_width = 60;
    box.size = GSize(fcc_number_subtitle_width, fcc_number_subtitle_height);
    // Note that we don't clip when we align the subtitle frame because it is wider than the
    // combined width of the title and icon
    grect_align(&box, &container_rect, GAlignBottom, false /* clip */);
    graphics_draw_text(ctx, fcc_number_subtitle, fcc_number_subtitle_font, box, text_overflow_mode,
                       GTextAlignmentCenter, NULL);
  }

  // Align the FCC mark icon to be drawn in the top right of the container
  box.size = fcc_mark_icon->bounds.size;
  grect_align(&box, &container_rect, GAlignTopRight, true /* clip */);
  prv_draw_mark_with_inversion(ctx, fcc_mark_icon, &box, cell_is_highlighted);
}

static void prv_draw_fcc_cell(
    GContext *ctx, const Layer *cell_layer, SystemCertificationData *cd,
    bool is_selected, const void *arg1, const void *arg2) {
  const char *title = arg1;
  const char *subtitle = arg2;
  const bool highlight = menu_cell_layer_is_highlighted(cell_layer);
  GBitmap *mark = &cd->fcc_mark;
#if PBL_RECT
  menu_cell_basic_draw(ctx, cell_layer, title, subtitle, NULL);
  // FCC has a mark in the top right of its cell
  const GPoint mark_origin = GPoint(119, 7);
  const GRect box = (GRect){.origin = mark_origin, .size = mark->bounds.size};
  prv_draw_mark_with_inversion(ctx, mark, &box, highlight);
#else
  prv_draw_fcc_cell_round(ctx, &cell_layer->bounds, title, subtitle, mark,
                          is_selected, highlight);
#endif
}

static void prv_draw_regulatory_marks_cell(
    GContext *ctx, const Layer *cell_layer, SystemCertificationData *cd,
    bool is_selected, const void *arg1, const void *arg2) {
  const GRect *cell_layer_bounds = &cell_layer->bounds;
  uint32_t start_idx = (uintptr_t)arg1;
  uint32_t num_marks = (uintptr_t)arg2;
  // Calculate rect containing generic marks
  GSize overall_size = GSize(MARK_PADDING * (num_marks + 1), 0);
  for (uint32_t i = 0; i < num_marks; i++) {
    const GSize mark_size = cd->regulatory_marks[start_idx + i]->bounds.size;
    overall_size.h = MAX(overall_size.h, mark_size.h);
    overall_size.w += mark_size.w;
  }
  GRect regulatory_marks_rect = (GRect) { .size = overall_size };
  // Align the rect based on the display shape
  const GAlign alignment = PBL_IF_RECT_ELSE(GAlignLeft, GAlignCenter);
  grect_align(&regulatory_marks_rect, cell_layer_bounds, alignment,
              /* clip */ true);
  // Draw the regulatory marks
  GPoint mark_origin = regulatory_marks_rect.origin;
  mark_origin.x += MARK_PADDING;
  const bool highlight = menu_cell_layer_is_highlighted(cell_layer);
  for (uint32_t i = 0; i < num_marks; i++) {
    GBitmap *mark = cd->regulatory_marks[start_idx + i];
    // Vertically center the icon in the cell
    mark_origin.y = (cell_layer_bounds->size.h - mark->bounds.size.h) / 2;
    // Draw the icon and advance the x coordinate for drawing the next icon
    mark_origin.x = prv_draw_generic_mark(ctx, mark, mark_origin,
                                          highlight) + MARK_PADDING;
  }
}

static void prv_append_certification_menu(SystemCertificationData *cd,
                                          SystemCertificationMenuItem *item) {
  PBL_ASSERTN(item->draw_cell_fn);
  cd->menu_items = app_realloc(cd->menu_items,
                               sizeof(*cd->menu_items) * ++cd->menu_count);
  PBL_ASSERTN(cd->menu_items);
  cd->menu_items[cd->menu_count - 1] = *item;
}

static void prv_append_regulatory_compliance_mark(SystemCertificationData *cd,
                                                  GBitmap *mark) {
  // Determine whether adding this mark overflows the cell, necessitating
  // another cell for this mark.
  uint16_t mark_width = mark->bounds.size.w;
  if (cd->current_regulatory_marks_cell_width + mark_width >= DISP_COLS) {
    // Flush the current marks to a cell and start a new one.
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_regulatory_marks_cell,
        .arg1 = (void *)(uintptr_t)cd->current_regulatory_marks_cell_start_idx,
        .arg2 = (void *)(uintptr_t)cd->num_regulatory_marks_in_current_cell,
    });
    cd->current_regulatory_marks_cell_start_idx +=
        cd->num_regulatory_marks_in_current_cell;
    cd->num_regulatory_marks_in_current_cell = 0;
    cd->current_regulatory_marks_cell_width = 0;
  }

  cd->regulatory_marks = app_realloc(
      cd->regulatory_marks, sizeof(GBitmap *) * ++cd->regulatory_marks_count);
  PBL_ASSERTN(cd->regulatory_marks);
  cd->regulatory_marks[cd->regulatory_marks_count - 1] = mark;
  cd->num_regulatory_marks_in_current_cell++;
  cd->current_regulatory_marks_cell_width += mark_width + MARK_PADDING;
}

static void prv_finished_appending_regulatory_compliance_marks(
    SystemCertificationData *cd) {
  if (cd->num_regulatory_marks_in_current_cell) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_regulatory_marks_cell,
        .arg1 = (void *)(uintptr_t)cd->current_regulatory_marks_cell_start_idx,
        .arg2 = (void *)(uintptr_t)cd->num_regulatory_marks_in_current_cell,
    });
  }
}

static void prv_draw_regulatory_id_cell(
    GContext *ctx, const Layer *cell_layer, SystemCertificationData *cd,
    bool is_selected, const void *arg1, const void *arg2) {
  const char *title = arg1;
  const char *subtitle = arg2;
  menu_cell_basic_draw(ctx, cell_layer, title, subtitle, NULL);
}

static void prv_draw_korea_regulatory_cell(
    GContext *ctx, const Layer *cell_layer, SystemCertificationData *cd,
    bool is_selected, const void *arg1, const void *arg2) {
  const char *title = arg1;
  const char *subtitle = i18n_get("See details...", title);
  menu_cell_basic_draw(ctx, cell_layer, title, subtitle, NULL);
  i18n_free(subtitle, title);
}

static void prv_certification_draw_row_callback(GContext* ctx, const Layer *cell_layer,
                                              MenuIndex *cell_index, void *context) {
  SettingsSystemData *data = (SettingsSystemData *) context;
  PBL_ASSERTN(cell_index->section == 0);

  SystemCertificationData *cd = &data->certification_data;
  const bool is_selected = menu_layer_is_index_selected(&data->menu_layer, cell_index);
  SystemCertificationMenuItem * const item = &cd->menu_items[cell_index->row];
  item->draw_cell_fn(ctx, cell_layer, cd, is_selected, item->arg1, item->arg2);
}

static uint16_t prv_certification_get_num_rows_callback(MenuLayer *menu_layer,
                                                        uint16_t section_index,
                                                        void *context) {
  SettingsSystemData *data = (SettingsSystemData *) context;
  return data->certification_data.menu_count;
}

static void prv_push_kcc_window(SystemCertificationData *data);

static void prv_certification_select_callback(MenuLayer *menu_layer,
                                              MenuIndex *cell_index,
                                              void *context) {
  SettingsSystemData *data = (SettingsSystemData *) context;
  SystemCertificationData *cd = &data->certification_data;
  if (cell_index->row < cd->menu_count &&
      cd->menu_items[cell_index->row].select_cb) {
    cd->menu_items[cell_index->row].select_cb(cd);
  }
}

static void prv_certification_window_load(Window *window) {
  SettingsSystemData *data = (SettingsSystemData*) window_get_user_data(window);

  prv_init_status_bar(&data->status_layer, &data->window, i18n_get("Certification", data));

  SystemCertificationData *cd = &data->certification_data;
  *cd = (SystemCertificationData) {};

  // Load up the assets
  gbitmap_init_with_resource(&cd->fcc_mark, RESOURCE_ID_SYSTEM_FCC_MARK);
  gbitmap_init_with_resource(&cd->kcc_mark, RESOURCE_ID_SYSTEM_KCC_MARK);
  gbitmap_init_with_resource(&cd->ce_mark, RESOURCE_ID_SYSTEM_CE_MARK);
  gbitmap_init_with_resource(&cd->weee_mark, RESOURCE_ID_SYSTEM_WEEE_MARK);
  gbitmap_init_with_resource(&cd->r_mark, RESOURCE_ID_SYSTEM_R_MARK);
  gbitmap_init_with_resource(&cd->t_mark, RESOURCE_ID_SYSTEM_T_MARK);
  gbitmap_init_with_resource(
      &cd->aus_rcm_mark, RESOURCE_ID_SYSTEM_AUS_RCM_MARK);
  gbitmap_init_with_resource(
      &cd->nom_nyce_mark, RESOURCE_ID_SYSTEM_NOM_NYCE_MARK);

  // Construct the certification menu
  const RegulatoryFlags *flags = prv_get_regulatory_flags();
  if (flags->has_usa_fcc) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_fcc_cell,
        .arg1 = "FCC",
        .arg2 = prv_get_usa_fcc_id(),
    });
  }
  if (flags->has_canada_ic) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_regulatory_id_cell,
        .arg1 = "Canada IC",
        .arg2 = prv_get_canada_ic_id(),
    });
  }
  if (flags->has_china_cmiit) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_regulatory_id_cell,
        .arg1 = "CMIIT ID",
        .arg2 = prv_get_china_cmiit_id(),
    });
  }
  if (flags->has_korea_kcc) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_korea_regulatory_cell,
        .arg1 = "South Korea KCC",
        .select_cb = prv_push_kcc_window,
    });
  }
  if (flags->has_mexico_nom_nyce) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_regulatory_id_cell,
        .arg1 = "IFETEL",
        .arg2 = prv_get_mexico_ifetel_id(),
    });
  }

  if (flags->has_korea_kcc) {
    prv_append_regulatory_compliance_mark(cd, &cd->kcc_mark);
  }
  if (flags->has_eu_ce) {
    prv_append_regulatory_compliance_mark(cd, &cd->ce_mark);
  }
  if (flags->has_eu_weee) {
    prv_append_regulatory_compliance_mark(cd, &cd->weee_mark);
  }
  if (flags->has_australia_rcm) {
    prv_append_regulatory_compliance_mark(cd, &cd->aus_rcm_mark);
  }
  if (flags->has_mexico_nom_nyce) {
    prv_append_regulatory_compliance_mark(cd, &cd->nom_nyce_mark);
  }
  prv_finished_appending_regulatory_compliance_marks(cd);

  if (flags->has_japan_telec_r) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_rt_cell,
        .arg1 = &cd->r_mark,
        .arg2 = prv_get_japan_telec_r_id()
    });
  }
  if (flags->has_japan_telec_t) {
    prv_append_certification_menu(cd, &(SystemCertificationMenuItem) {
        .draw_cell_fn = prv_draw_rt_cell,
        .arg1 = &cd->t_mark,
        .arg2 = prv_get_japan_telec_t_id()
    });
  }

  // Create the menu
  MenuLayer *menu_layer = &data->menu_layer;
  GRect bounds = data->window.layer.bounds;
  const GEdgeInsets menu_layer_insets = (GEdgeInsets) {
    .top = STATUS_BAR_LAYER_HEIGHT,
    .bottom = PBL_IF_RECT_ELSE(0, STATUS_BAR_LAYER_HEIGHT)
  };
  bounds = grect_inset(bounds, menu_layer_insets);
  menu_layer_init(menu_layer, &bounds);
  menu_layer_set_callbacks(menu_layer, data, &(MenuLayerCallbacks) {
    .get_num_rows = prv_certification_get_num_rows_callback,
    .get_cell_height = prv_certification_get_cell_height_callback,
    .draw_row = prv_certification_draw_row_callback,
    .select_click = prv_certification_select_callback,
  });
  menu_layer_set_highlight_colors(menu_layer, SETTINGS_MENU_HIGHLIGHT_COLOR, GColorWhite);
  menu_layer_set_click_config_onto_window(menu_layer, &data->window);

  layer_add_child(&data->window.layer, menu_layer_get_layer(menu_layer));
}

static void prv_certification_window_unload(Window *window) {
  SettingsSystemData *data = (SettingsSystemData*) window_get_user_data(window);

  menu_layer_deinit(&data->menu_layer);

  gbitmap_deinit(&data->certification_data.fcc_mark);
  gbitmap_deinit(&data->certification_data.kcc_mark);
  gbitmap_deinit(&data->certification_data.ce_mark);
  gbitmap_deinit(&data->certification_data.weee_mark);
  gbitmap_deinit(&data->certification_data.r_mark);
  gbitmap_deinit(&data->certification_data.t_mark);
  gbitmap_deinit(&data->certification_data.aus_rcm_mark);
  gbitmap_deinit(&data->certification_data.nom_nyce_mark);

  app_free(data->certification_data.regulatory_marks);
  app_free(data->certification_data.menu_items);

  prv_deinit_status_bar(&data->status_layer);
}

static void prv_certification_window_push(SettingsSystemData *data) {
  window_init(&data->window, WINDOW_NAME("System Certification"));
  window_set_user_data(&data->window, data);
  window_set_window_handlers(&data->window, &(WindowHandlers) {
    .load = prv_certification_window_load,
    .unload = prv_certification_window_unload,
  });
  app_window_stack_push(&data->window, true);
}

static void prv_kcc_window_load(Window *window) {
  SystemCertificationData *data = (SystemCertificationData *) window_get_user_data(window);
  Layer *window_layer = window_get_root_layer(window);

  const char *title = "South Korea KCC";
  prv_init_status_bar(&data->status_layer, &data->kcc_window, title);

  GRect window_bounds = window_layer->bounds;

  // Calculate the bounding rect for the certification content and center it in the window
  GBitmap *bmp = &data->kcc_mark;
  const GSize bmp_size = bmp->bounds.size;
  const GFont title_text_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  const GSize title_text_size = GSize(window_bounds.size.w, fonts_get_font_height(title_text_font));
  const GFont info_text_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  const GSize info_text_size = GSize(window_bounds.size.w, fonts_get_font_height(info_text_font));
  const int16_t vertical_spacing = 3;
  GRect certification_rect = (GRect) {
    .size = GSize(window_bounds.size.w,
                  bmp_size.h + title_text_size.h + info_text_size.h + vertical_spacing)
  };
  grect_align(&certification_rect, &window_bounds, GAlignCenter, true /* clip */);

  GRect bmp_frame = (GRect) { .size = bmp_size };
  grect_align(&bmp_frame, &certification_rect, GAlignTop, true /* clip */);
  bitmap_layer_init(&data->bmp_layer, &bmp_frame);
  bitmap_layer_set_bitmap(&data->bmp_layer, bmp);
  bitmap_layer_set_compositing_mode(&data->bmp_layer, GCompOpAssign);
  layer_add_child(window_layer, bitmap_layer_get_layer(&data->bmp_layer));

  GRect title_text_frame = (GRect) { .size = title_text_size };
  const int16_t title_text_internal_padding = 5;
  title_text_frame.origin.y = bmp_frame.origin.y + bmp_size.h + vertical_spacing
                                - title_text_internal_padding;
  text_layer_init_with_parameters(&data->title_text, &title_text_frame,
                                  title, title_text_font,
                                  GColorBlack, GColorClear, GTextAlignmentCenter,
                                  GTextOverflowModeTrailingEllipsis);
  layer_add_child(window_layer, text_layer_get_layer(&data->title_text));

  GRect info_text_frame = (GRect) { .size = info_text_size };
  info_text_frame.origin.y = title_text_frame.origin.y + title_text_size.h + vertical_spacing;
  text_layer_init_with_parameters(&data->info_text, &info_text_frame,
                                  prv_get_korea_kcc_id(), info_text_font,
                                  GColorBlack, GColorClear, GTextAlignmentCenter,
                                  GTextOverflowModeTrailingEllipsis);
  layer_add_child(window_layer, text_layer_get_layer(&data->info_text));
}

static void prv_kcc_window_unload(Window *window) {
  SystemCertificationData *data = (SystemCertificationData *) window_get_user_data(window);
  prv_deinit_status_bar(&data->status_layer);
  bitmap_layer_deinit(&data->bmp_layer);
  text_layer_deinit(&data->title_text);
  text_layer_deinit(&data->info_text);
  i18n_free_all(data);
}

static void prv_push_kcc_window(SystemCertificationData *data) {
  window_init(&data->kcc_window, WINDOW_NAME("System KCC"));
  window_set_user_data(&data->kcc_window, data);
  window_set_window_handlers(&data->kcc_window, &(WindowHandlers) {
    .load = prv_kcc_window_load,
    .unload = prv_kcc_window_unload,
  });
  app_window_stack_push(&data->kcc_window, true);
}

// Callbacks for the main settings filter list menu.
////////////////////////////////////////////////////

#define SHUTDOWN_MIN_BOOT_VERSION 1354647953

static bool prv_shutdown_enabled(void) {
  return boot_version_read() >= SHUTDOWN_MIN_BOOT_VERSION;
}

static void prv_shutdown_confirm_cb(ClickRecognizerRef recognizer, void *context) {
  actionable_dialog_pop((ActionableDialog *) context);
  battery_ui_handle_shut_down();
}

static void prv_shutdown_back_cb(ClickRecognizerRef recognizer, void *context) {
  actionable_dialog_pop((ActionableDialog *) context);
}

static void prv_shutdown_click_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_shutdown_confirm_cb);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_shutdown_back_cb);
}

static void prv_shutdown_cb(void* data) {
  ActionableDialog *a_dialog = actionable_dialog_create("Shutdown");
  Dialog *dialog = actionable_dialog_get_dialog(a_dialog);

  actionable_dialog_set_action_bar_type(a_dialog, DialogActionBarConfirm, NULL);
  actionable_dialog_set_click_config_provider(a_dialog, prv_shutdown_click_provider);

  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_background_color(dialog, GColorCobaltBlue);
  dialog_set_text(dialog, i18n_get("Do you want to shut down?", a_dialog));
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_QUESTION_LARGE);

  i18n_free_all(a_dialog);

  actionable_dialog_push(a_dialog, modal_manager_get_window_stack(ModalPriorityGeneric));
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsSystemData *data = (SettingsSystemData *) context;
  i18n_free_all(data);
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx,
                            const Layer *cell_layer, uint16_t row, bool selected) {
  SettingsSystemData *data = (SettingsSystemData *) context;
  const char *subtitle = NULL;
  PBL_ASSERTN(row < SystemMenuItem_Count);
  switch (row) {
    case SystemMenuItemStationaryToggle:
      subtitle = stationary_get_enabled() ? i18n_get("On", data) : i18n_get("Off", data);
      break;
    case SystemMenuItemShutDown:
      if (!prv_shutdown_enabled()) {
        // XXX: For now, gray out the Shut Down item if unusable.
        graphics_context_set_text_color(ctx, GColorDarkGray);
      }
      break;
    case SystemMenuItemInformation:
    case SystemMenuItemCertification:
    case SystemMenuItemFactoryReset:
    case SystemMenuItem_Count:
      break;
    default:
      WTF;
  }
  menu_cell_basic_draw(ctx, cell_layer, i18n_get(s_item_titles[row], data), subtitle, NULL);
}

void factory_reset_select_callback(int index, void *context) {
  settings_factory_reset_window_push();
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsSystemData *data = (SettingsSystemData *) context;

  switch (row) {
    case SystemMenuItemInformation:
      prv_information_window_push(data);
      break;
    case SystemMenuItemCertification:
      prv_certification_window_push(data);
      break;
    case SystemMenuItemStationaryToggle:
      stationary_set_enabled(!stationary_get_enabled());
      break;
    case SystemMenuItemShutDown:
      if (prv_shutdown_enabled()) {
        launcher_task_add_callback(prv_shutdown_cb, 0);
      }
      break;
    case SystemMenuItemFactoryReset:
      settings_factory_reset_window_push();
      break;
    default:
      WTF;
  }
  settings_menu_reload_data(SettingsMenuItemSystem);
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  return SystemMenuItem_Count;
}

static Window *prv_init(void) {
  SettingsSystemData *data = app_malloc_check(sizeof(SettingsSystemData));
  *data = (SettingsSystemData){};

  data->callbacks = (SettingsCallbacks) {
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
  };

  return settings_window_create(SettingsMenuItemSystem, &data->callbacks);
}

const SettingsModuleMetadata *settings_system_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("System"),
    .init = prv_init,
  };

  return &s_module_info;
}
