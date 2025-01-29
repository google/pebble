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

//! This file displays the main Quick Launch menu that is found in our settings menu
//! It allows the feature to be enabled or for an app to be set
//! The list of apps that the user can choose from is found in settings_quick_launch_app_menu.c
//! This file is also responsible for saving / storing the uuid of each quicklaunch app as well as
//! whether or not the quicklaunch app is enabled.

#include "settings_menu.h"
#include "settings_quick_launch.h"
#include "settings_quick_launch_app_menu.h"
#include "settings_quick_launch_setup_menu.h"
#include "settings_window.h"

#include "applib/app.h"
#include "applib/app_launch_button.h"
#include "applib/app_launch_reason.h"
#include "applib/ui/window_stack.h"
#include "kernel/pbl_malloc.h"
#include "process_management/app_menu_data_source.h"
#include "resource/resource_ids.auto.h"
#include "services/common/i18n/i18n.h"
#include "shell/normal/quick_launch.h"
#include "system/passert.h"
#include "system/status_codes.h"

typedef struct QuickLaunchData {
  SettingsCallbacks callbacks;
  char app_names[NUM_BUTTONS][APP_NAME_SIZE_BYTES];
} QuickLaunchData;

static const char *s_button_titles[NUM_BUTTONS] = {
  /// Shown in Quick Launch Settings as the title of the up button quick launch option.
  [BUTTON_ID_UP]     = i18n_noop("Up Button"),
  /// Shown in Quick Launch Settings as the title of the center button quick launch option.
  [BUTTON_ID_SELECT] = i18n_noop("Center Button"),
  /// Shown in Quick Launch Settings as the title of the down button quick launch option.
  [BUTTON_ID_DOWN]   = i18n_noop("Down Button"),
  /// Shown in Quick Launch Settings as the title of the back button quick launch option.
  [BUTTON_ID_BACK]   = i18n_noop("Back Button"),
};

static ButtonId s_button_order[NUM_BUTTONS] = {
  BUTTON_ID_UP,
  BUTTON_ID_SELECT,
  BUTTON_ID_DOWN,
  BUTTON_ID_BACK,
};

static ButtonId s_button_order_map[NUM_BUTTONS] = {
  [BUTTON_ID_UP]     = 0,
  [BUTTON_ID_SELECT] = 1,
  [BUTTON_ID_DOWN]   = 2,
  [BUTTON_ID_BACK]   = 3,
};

static void prv_get_subtitle_string(AppInstallId app_id, QuickLaunchData *data,
                                    char *buffer, uint8_t buf_len) {
  if (app_id == INSTALL_ID_INVALID) {
    /// Shown in Quick Launch Settings when the button is disabled.
    i18n_get_with_buffer("Disabled", buffer, buf_len);
    return;
  } else {
    AppInstallEntry entry;
    if (app_install_get_entry_for_install_id(app_id, &entry)) {
      strncpy(buffer, entry.name, buf_len);
      buffer[buf_len - 1] = '\0';
      return;
    }
  }
  // if failed both, set as empty string
  buffer[0] = '\0';
}

// Filter List Callbacks
////////////////////////
static void prv_deinit_cb(SettingsCallbacks *context) {
  QuickLaunchData *data = (QuickLaunchData *) context;
  i18n_free_all(data);
  app_free(data);
}

static void prv_update_app_names(QuickLaunchData *data) {
  for (ButtonId button = 0; button < NUM_BUTTONS; button++) {
    char *subtitle_buf = data->app_names[button];
    prv_get_subtitle_string(quick_launch_get_app(button), data, subtitle_buf, APP_NAME_SIZE_BYTES);
  }
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx,
                            const Layer *cell_layer, uint16_t row, bool selected) {
  QuickLaunchData *data = (QuickLaunchData *)context;
  PBL_ASSERTN(row < NUM_BUTTONS);
  const ButtonId button = s_button_order[row];
  const char *title = i18n_get(s_button_titles[button], data);
  char *subtitle_buf = data->app_names[button];
  menu_cell_basic_draw(ctx, cell_layer, title, subtitle_buf, NULL);
}

static uint16_t prv_get_initial_selection_cb(SettingsCallbacks *context) {
  // If launched by quick launch, select the row of the button pressed, otherwise default to 0
  return (app_launch_reason() == APP_LAUNCH_QUICK_LAUNCH) ?
      s_button_order_map[app_launch_button()] : 0;
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  PBL_ASSERTN(row < NUM_BUTTONS);
  quick_launch_app_menu_window_push(s_button_order[row]);
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  return NUM_BUTTONS;
}

static void prv_appear(SettingsCallbacks *context) {
  QuickLaunchData *data = (QuickLaunchData *)context;
  prv_update_app_names(data);
}

static Window *prv_init(void) {
  QuickLaunchData *data = app_malloc_check(sizeof(*data));
  *data = (QuickLaunchData){};

  data->callbacks = (SettingsCallbacks) {
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .get_initial_selection = prv_get_initial_selection_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .appear = prv_appear,
  };

  return settings_window_create(SettingsMenuItemQuickLaunch, &data->callbacks);
}

const SettingsModuleMetadata *settings_quick_launch_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    /// Title of the Quick Launch Settings submenu in Settings
    .name = i18n_noop("Quick Launch"),
    .init = prv_init,
  };

  return &s_module_info;
}
