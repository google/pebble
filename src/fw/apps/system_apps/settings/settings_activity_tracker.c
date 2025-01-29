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

#include "settings_activity_tracker.h"
#include "settings_menu.h"
#include "settings_window.h"

#include "applib/app.h"
#include "applib/app_timer.h"
#include "applib/ui/kino/kino_reel.h"
#include "applib/ui/option_menu_window.h"
#include "applib/ui/ui.h"
#include "applib/ui/window.h"
#include "applib/ui/window_stack.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "popups/switch_worker_ui.h"
#include "process_management/app_menu_data_source.h"
#include "process_management/worker_manager.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "services/common/i18n/i18n.h"
#include "shell/normal/watchface.h"
#include "system/passert.h"

#include <string.h>

typedef struct SettingsActivityTrackerData {
  OptionMenu option_menu;
  MenuLayer menu_layer;
  AppMenuDataSource *data_source;
  TextLayer *text_layer;
  EventServiceInfo worker_launch_info;
} SettingsActivityTrackerData;

////////////////////
// AppMenuDataSource callbacks

static bool prv_app_filter_callback(struct AppMenuDataSource *const source,
                                    AppInstallEntry *entry) {
  if (!app_install_entry_is_hidden(entry) &&
      app_install_entry_has_worker(entry)) {
    return true;
  }
  return false;
}

static int16_t prv_get_chosen_row_index_for_id(SettingsActivityTrackerData *data,
                                               AppInstallId worker_id) {
  if (worker_id == INSTALL_ID_INVALID) {
    return 0;
  }

  const uint16_t current_worker_app_index =
      app_menu_data_source_get_index_of_app_with_install_id(data->data_source, worker_id);

  if (current_worker_app_index == MENU_INDEX_NOT_FOUND) {
    return 0;
  } else {
    return current_worker_app_index + 1;
  }
}

// Gets the current chosen row index; i.e., the row which was most recently chosen by the user.
static int16_t prv_get_chosen_row_index(SettingsActivityTrackerData *data) {
  const AppInstallId worker_id = worker_manager_get_current_worker_id();
  return prv_get_chosen_row_index_for_id(data, worker_id);
}

static int prv_num_rows(SettingsActivityTrackerData *data) {
  if (data->data_source) {
    return app_menu_data_source_get_count(data->data_source);
  } else {
    return 0;
  }
}

static void prv_reload_menu_data(void *context) {
  SettingsActivityTrackerData *data = context;
  const uint16_t count = prv_num_rows(data);
  const bool use_icons = (count != 0);
  option_menu_set_icons_enabled(&data->option_menu, use_icons /* icons_enabled */);

  option_menu_set_choice(&data->option_menu, prv_get_chosen_row_index(data));
  option_menu_reload_data(&data->option_menu);
}

// Settings Menu callbacks
///////////////////////////

static void prv_select_cb(OptionMenu *option_menu, int row, void *context) {
  SettingsActivityTrackerData *data = context;
  if (app_menu_data_source_get_count(data->data_source) == 0) {
    return;
  }
  if (row == 0) {
    // Killing current worker
    process_manager_put_kill_process_event(PebbleTask_Worker, true /* graceful */);
    worker_manager_set_default_install_id(INSTALL_ID_INVALID);
  } else {
    const uint16_t app_index = row - 1; // offset because of the "None" selection
    const AppMenuNode *app_node =
        app_menu_data_source_get_node_at_index(data->data_source, app_index);
    if (worker_manager_get_task_context()->install_id == INSTALL_ID_INVALID) {
      // No worker currently running, launch this one and make it the default
      worker_manager_put_launch_worker_event(app_node->install_id);
      worker_manager_set_default_install_id(app_node->install_id);
    } else if (worker_manager_get_task_context()->install_id != app_node->install_id) {
      // Undo the choice change that the OptionMenu does before we call select. We may decline
      // the change and therefore we don't want it to visually update yet. prv_worker_launch_handler
      // will update the choice if it fires.
      option_menu_set_choice(&data->option_menu, prv_get_chosen_row_index(data));

      // Switching to a different worker, display confirmation dialog
      switch_worker_confirm(app_node->install_id, true /* set as default */,
                            app_state_get_window_stack());
    } else {
      // User selected the option they already had, do nothing
    }
  }
}

static void prv_draw_no_activities_cell_rect(GContext *ctx, const Layer *cell_layer,
                                             const char *no_activities_string) {
  const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GRect box = cell_layer->bounds;

  const GTextOverflowMode overflow = GTextOverflowModeTrailingEllipsis;
  const GTextAlignment alignment = GTextAlignmentCenter;

  const GSize text_size = graphics_text_layout_get_max_used_size(ctx, no_activities_string, font,
                                                                 box, overflow, alignment, NULL);

  // We want to position the text in the center of the cell vertically,
  // we divide the height of the cell by two and subtract half of the text size.
  // However, that just puts the TOP of a line vertically aligned.
  // So we also have to subtract half of a single line's width.
  box.origin.y = (box.size.h - text_size.h - fonts_get_font_height(font)/2) / 2;

  graphics_draw_text(ctx, no_activities_string, font, box, overflow, alignment, NULL);
}

static void prv_draw_no_activities_cell_round(GContext *ctx, const Layer *cell_layer,
                                              const char *no_activities_string) {
  menu_cell_basic_draw(ctx, cell_layer, no_activities_string, NULL, NULL);
}

static uint16_t prv_get_num_rows_cb(OptionMenu *option_menu, void *context) {
  SettingsActivityTrackerData *data = context;
  const uint16_t count = prv_num_rows(data);
  return count + 1;
}

static void prv_draw_row_cb(OptionMenu *option_menu, GContext *ctx, const Layer *cell_layer,
                            const GRect *text_frame, uint32_t row, bool selected, void *context) {
  SettingsActivityTrackerData *data = context;

  if (prv_num_rows(data) == 0) {
    // Draw "No background apps" box and exit
    const char *no_background_apps_string = i18n_get("No background apps", data);
    PBL_IF_RECT_ELSE(prv_draw_no_activities_cell_rect,
                     prv_draw_no_activities_cell_round)
                     (ctx, cell_layer, no_background_apps_string);
    return;
  }

  const char *title = NULL;
  if (row == 0) {
    title = i18n_get("None", data);
  } else {
    AppMenuNode *node = app_menu_data_source_get_node_at_index(data->data_source, row - 1);
    title = node->name;
  }

  option_menu_system_draw_row(option_menu, ctx, cell_layer, text_frame, title, false, NULL);
}

static uint16_t prv_row_height_cb(OptionMenu *option_menu, uint16_t row, bool is_selected,
                                  void *context) {
  const int16_t cell_height =
      option_menu_default_cell_height(option_menu->content_type, is_selected);
#if PBL_RECT
  if (prv_num_rows(context) == 0) {
    // When we have no background apps, we want a double height row to display the
    // 'No background apps' line, so that translations can fit and we stop wasting so much screen
    // space.
    return 2 * cell_height;
  }
#endif
  return cell_height;
}

static void prv_worker_launch_handler(PebbleEvent *event, void *context) {
  // Our worker changed while we were visible, update the selected choice
  SettingsActivityTrackerData *data = context;

  const AppInstallId worker_id = event->launch_app.id;
  const int16_t chosen_row = prv_get_chosen_row_index_for_id(data, worker_id);

  option_menu_set_choice(&data->option_menu, chosen_row);
}

static void prv_unload_cb(OptionMenu *option_menu, void *context) {
  SettingsActivityTrackerData *data = context;

  event_service_client_unsubscribe(&data->worker_launch_info);

  app_menu_data_source_deinit(data->data_source);

  app_free(data->data_source);
  data->data_source = NULL;

  option_menu_deinit(&data->option_menu);

  i18n_free_all(data);
  app_free(data);
}

static Window *prv_init(void) {
  SettingsActivityTrackerData *data = app_zalloc_check(sizeof(SettingsActivityTrackerData));

  const OptionMenuCallbacks option_menu_callbacks = {
      .unload = prv_unload_cb,
      .draw_row = prv_draw_row_cb,
      .select = prv_select_cb,
      .get_num_rows = prv_get_num_rows_cb,
      .get_cell_height = prv_row_height_cb,
  };

  data->data_source = app_zalloc_check(sizeof(AppMenuDataSource));
  app_menu_data_source_init(data->data_source, &(AppMenuDataSourceCallbacks) {
    .changed = prv_reload_menu_data,
    .filter = prv_app_filter_callback,
  }, data);

  option_menu_init(&data->option_menu);
  // Not using option_menu_configure because prv_reload_menu_data already sets
  // icons_enabled and chosen row index
  option_menu_set_status_colors(&data->option_menu, GColorWhite, GColorBlack);
  option_menu_set_highlight_colors(&data->option_menu, SETTINGS_MENU_HIGHLIGHT_COLOR, GColorWhite);
  option_menu_set_title(&data->option_menu, i18n_get("Background App", data));
  option_menu_set_content_type(&data->option_menu, OptionMenuContentType_SingleLine);
  option_menu_set_callbacks(&data->option_menu, &option_menu_callbacks, data);
  prv_reload_menu_data(data);

  data->worker_launch_info = (EventServiceInfo) {
    .type = PEBBLE_WORKER_LAUNCH_EVENT,
    .handler = prv_worker_launch_handler,
    .context = data
  };
  event_service_client_subscribe(&data->worker_launch_info);

  return &data->option_menu.window;
}

const SettingsModuleMetadata *settings_activity_tracker_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Background App"),
    .init = prv_init,
  };

  return &s_module_info;
}
