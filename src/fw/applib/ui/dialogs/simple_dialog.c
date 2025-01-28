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

#include "simple_dialog.h"

#include "applib/applib_malloc.auto.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/bitmap_layer.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/dialog_private.h"
#include "applib/ui/layer.h"
#include "applib/ui/text_layer.h"
#include "applib/ui/window.h"
#include "kernel/ui/kernel_ui.h"
#include "system/passert.h"

#include <limits.h>
#include <string.h>

#if (RECOVERY_FW || UNITTEST)
#define SIMPLE_DIALOG_ANIMATED false
#else
#define SIMPLE_DIALOG_ANIMATED true
#endif

// Layout Defines
#define TEXT_ALIGNMENT (GTextAlignmentCenter)
#define TEXT_OVERFLOW  (GTextOverflowModeWordWrap)
#define TEXT_FONT      (fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD))

#define TEXT_LEFT_MARGIN_PX  (PBL_IF_RECT_ELSE(6, 0))
#define TEXT_RIGHT_MARGIN_PX (PBL_IF_RECT_ELSE(6, 0))
#define TEXT_FLOW_INSET_PX   (PBL_IF_RECT_ELSE(0, 8))
#define TEXT_LINE_HEIGHT_PX  (fonts_get_font_height(TEXT_FONT))
#define TEXT_MAX_HEIGHT_PX   ((2 * TEXT_LINE_HEIGHT_PX) + 8) // 2 line + some space for descenders


static int prv_get_rendered_text_height(const char *text, const GRect *text_box) {
  GContext *ctx = graphics_context_get_current_context();
  TextLayoutExtended layout = { 0 };
  graphics_text_attributes_enable_screen_text_flow((GTextLayoutCacheRef) &layout,
                                                   TEXT_FLOW_INSET_PX);
  return graphics_text_layout_get_max_used_size(ctx,
                                                text,
                                                TEXT_FONT,
                                                *text_box,
                                                TEXT_OVERFLOW,
                                                TEXT_ALIGNMENT,
                                                (GTextLayoutCacheRef) &layout).h;
}

static int prv_get_icon_top_margin(bool has_status_bar, int icon_height, int window_height) {
  const uint16_t status_layer_offset = has_status_bar ? 6 : 0;
#if PLATFORM_ROBERT || PLATFORM_CALCULUS
  const uint16_t icon_top_default_margin_px = 42 + status_layer_offset;
#else
  const uint16_t icon_top_default_margin_px = 18 + status_layer_offset;
#endif
  const uint16_t frame_height_claimed = icon_height + TEXT_MAX_HEIGHT_PX + status_layer_offset;
  const uint16_t icon_top_adjusted_margin_px = MAX(window_height - frame_height_claimed, 0);
  // Try and use the default value if possible.
  return (icon_top_adjusted_margin_px < icon_top_default_margin_px) ? icon_top_adjusted_margin_px :
                                                                      icon_top_default_margin_px;
}

static void prv_get_text_box(GSize frame_size, GSize icon_size,
                              int icon_top_margin_px, GRect *text_box_out) {
  const uint16_t icon_text_spacing_px = PBL_IF_ROUND_ELSE(2, 4);

  const uint16_t text_x = TEXT_LEFT_MARGIN_PX;
  const uint16_t text_y = icon_top_margin_px + MAX(icon_size.h, 6) + icon_text_spacing_px;
  const uint16_t text_w = frame_size.w - TEXT_LEFT_MARGIN_PX - TEXT_RIGHT_MARGIN_PX;
  // Limit to 2 lines if there is an icon
  const uint16_t text_h = icon_size.h ? TEXT_MAX_HEIGHT_PX : frame_size.h - text_y;

  *text_box_out = GRect(text_x, text_y, text_w, text_h);
}

static void prv_simple_dialog_load(Window *window) {
  SimpleDialog *simple_dialog = window_get_user_data(window);
  Dialog *dialog = &simple_dialog->dialog;

  // Ownership of icon is taken over by KinoLayer in dialog_init_icon_layer() call below
  KinoReel *icon = dialog_create_icon(dialog);
  const GSize icon_size = icon ? kino_reel_get_size(icon) : GSizeZero;

  GRect frame = window->layer.bounds;

  // Status Layer
  if (dialog->show_status_layer) {
    dialog_add_status_bar_layer(dialog, &GRect(0, 0, frame.size.w, STATUS_BAR_LAYER_HEIGHT));
  }

  uint16_t icon_top_margin_px = prv_get_icon_top_margin(dialog->show_status_layer,
                                                        icon_size.h, frame.size.h);

  // Text
  GRect text_box;
  prv_get_text_box(frame.size, icon_size, icon_top_margin_px, &text_box);

  const uint16_t text_height = prv_get_rendered_text_height(dialog->buffer, &text_box);

  if (text_height <= TEXT_LINE_HEIGHT_PX) {
    const int additional_icon_top_offset_for_single_line_text_px = 13;
    // Move the icon down by increasing the margin to vertically center things
    icon_top_margin_px += additional_icon_top_offset_for_single_line_text_px;
    // Move the text down as well to preserve spacing
    // The -1 is there to preserve prior functionality ¯\_(ツ)_/¯
    text_box.origin.y += additional_icon_top_offset_for_single_line_text_px - 1;
  }

  TextLayer *text_layer = &dialog->text_layer;
  text_layer_init_with_parameters(text_layer, &text_box, dialog->buffer, TEXT_FONT,
                                  dialog->text_color, GColorClear, TEXT_ALIGNMENT, TEXT_OVERFLOW);
  layer_add_child(&window->layer, &text_layer->layer);

#if PBL_ROUND
  text_layer_enable_screen_text_flow_and_paging(text_layer, TEXT_FLOW_INSET_PX);
#endif

  // Icon
  const GPoint icon_origin = GPoint((grect_get_max_x(&frame) - icon_size.w) / 2,
                                    icon_top_margin_px);

  if (dialog_init_icon_layer(dialog, icon, icon_origin, !simple_dialog->icon_static)) {
    layer_add_child(&dialog->window.layer, &dialog->icon_layer.layer);
  }

  dialog_load(dialog);
}

static void prv_simple_dialog_appear(Window *window) {
  SimpleDialog *simple_dialog = window_get_user_data(window);
  Dialog *dialog = simple_dialog_get_dialog(simple_dialog);
  dialog_appear(dialog);
}

static void prv_simple_dialog_unload(Window *window) {
  SimpleDialog *simple_dialog = window_get_user_data(window);
  dialog_unload(&simple_dialog->dialog);
  if (simple_dialog->dialog.destroy_on_pop) {
    applib_free(simple_dialog);
  }
}

static void prv_click_handler(ClickRecognizerRef recognizer, void *context) {
  SimpleDialog *simple_dialog = context;
  if (!simple_dialog->buttons_disabled) {
    dialog_pop(&simple_dialog->dialog);
  }
}

static void prv_config_provider(void *context) {
  // Simple dialogs are dismissed when any button is pushed.
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_click_handler);
}

Dialog *simple_dialog_get_dialog(SimpleDialog *simple_dialog) {
  return &simple_dialog->dialog;
}

void simple_dialog_push(SimpleDialog *simple_dialog, WindowStack *window_stack) {
  dialog_push(&simple_dialog->dialog, window_stack);
}

void app_simple_dialog_push(SimpleDialog *simple_dialog) {
  app_dialog_push(&simple_dialog->dialog);
}

void simple_dialog_init(SimpleDialog *simple_dialog, const char *dialog_name) {
  PBL_ASSERTN(simple_dialog);
  *simple_dialog = (SimpleDialog) {
    .icon_static = !SIMPLE_DIALOG_ANIMATED,
  };

  dialog_init(&simple_dialog->dialog, dialog_name);
  Window *window = &simple_dialog->dialog.window;
  window_set_window_handlers(window, &(WindowHandlers) {
    .load = prv_simple_dialog_load,
    .unload = prv_simple_dialog_unload,
    .appear = prv_simple_dialog_appear,
  });
  window_set_click_config_provider_with_context(window, prv_config_provider, simple_dialog);
  window_set_user_data(window, simple_dialog);
}

SimpleDialog *simple_dialog_create(const char *dialog_name) {
  SimpleDialog *simple_dialog = applib_malloc(sizeof(SimpleDialog));
  if (simple_dialog) {
    simple_dialog_init(simple_dialog, dialog_name);
  }
  return simple_dialog;
}

void simple_dialog_set_buttons_enabled(SimpleDialog *simple_dialog, bool enabled) {
  simple_dialog->buttons_disabled = !enabled;
}

void simple_dialog_set_icon_animated(SimpleDialog *simple_dialog, bool animated) {
  // This cannot be set after the window has been loaded
  PBL_ASSERTN(!window_is_loaded(&simple_dialog->dialog.window));
  simple_dialog->icon_static = !animated;
}

bool simple_dialog_does_text_fit(const char *text, GSize window_size,
                                 GSize icon_size, bool has_status_bar) {
  const uint16_t icon_top_margin_px = prv_get_icon_top_margin(has_status_bar, icon_size.h,
                                                              window_size.h);
  GRect text_box;
  prv_get_text_box(window_size, icon_size, icon_top_margin_px, &text_box);
  return prv_get_rendered_text_height(text, &text_box) <= TEXT_MAX_HEIGHT_PX;
}
