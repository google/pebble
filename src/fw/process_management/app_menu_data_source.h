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

#pragma once

//!@file app_menu_data_source.h
//!
//! This file provides a utility for populating a MenuLayer with the apps that are currently installed. This should
//! only be used by system apps such as the Launcher or the Watchface Selector apps, as it integrates tightly with
//! app_install_manager.

#include "applib/ui/kino/kino_reel.h"
#include "applib/ui/menu_layer.h"
#include "os/mutex.h"
#include "process_management/app_install_manager.h"
#include "process_management/pebble_process_info.h"
#include "process_management/pebble_process_md.h"
#include "services/normal/process_management/app_order_storage.h"
#include "util/attributes.h"
#include "util/list.h"

struct AppMenuDataSource;

//! This enum provides special cases of app storage order and helps calculate the fixed offset at
//! which the general storage order should begin
typedef enum AppMenuStorageOrder {
  AppMenuStorageOrder_NoOrder = 0,
  AppMenuStorageOrder_SettingsDefaultOrder,

  AppMenuStorageOrderGeneralOrderOffset
} AppMenuStorageOrder;

typedef struct PACKED AppMenuNode {
  ListNode node;
  AppInstallId install_id;
  ResAppNum app_num;
  uint32_t icon_resource_id;
  GBitmap *icon;
  Uuid uuid;
  GColor color;
  char *name;
  ProcessVisibility visibility;
  Version sdk_version;
  unsigned int storage_order; //!< See \ref AppMenuStorageOrder for special values of this field
  unsigned int record_order; //!< 0 means not in the app registry
} AppMenuNode;

// Clang makes the size of enums like AppMenuStorageOrder larger than the minimum size needed to
// represent all of the enum's values, so this assert doesn't make since when compiling using Clang
#if !__clang__
_Static_assert(
    sizeof(((AppMenuNode *)0)->storage_order) >=
        sizeof(((AppMenuOrderStorage *)0)->list_length) + sizeof(AppMenuStorageOrder),
    "The size of AppMenuNode.storage_order must be at least as large as the combined size of "
    "AppMenuOrderStorage.list_length and the AppMenuStorageOrder enum since the enum provides "
    "additional values for AppMenuNode.storage_order.");
#endif

typedef bool (*AppMenuFilterCallback)(struct AppMenuDataSource *source, AppInstallEntry *entry);

typedef void (*AppMenuDataSourceFunc)(void *context);

typedef uint16_t (*AppMenuDataSourceIndexTransform)(struct AppMenuDataSource *source,
                                                    uint16_t index, void *context);

typedef struct AppMenuDataSourceCallbacks {
  AppMenuFilterCallback filter;
  AppMenuDataSourceFunc changed;
  AppMenuDataSourceIndexTransform transform_index;
} AppMenuDataSourceCallbacks;

typedef struct AppMenuDataSource {
  AppMenuNode *list;
  AppMenuOrderStorage *order_storage;
  AppInstallCallbackNode app_install_callback_node;
  AppMenuDataSourceCallbacks callbacks;
  void *callback_context;
  GBitmap *default_icon;
  bool show_icons;
  bool is_list_loaded;
} AppMenuDataSource;

//! Initialize the AppMenuDataSource
void app_menu_data_source_init(AppMenuDataSource *source,
                               const AppMenuDataSourceCallbacks *handlers,
                               void *callback_context);

//! Deinitialize the AppMenuDataSource
void app_menu_data_source_deinit(AppMenuDataSource *source);

//! Will load the icons for each `AppMenuNode`. Will automatically be unloaded when
//! `app_menu_data_source_deinit` is called.
//! @param fallback_icon_id The fallback resource id that should be used if the entry does not have
//! an icon.
void app_menu_data_source_enable_icons(AppMenuDataSource *source, uint32_t fallback_icon_id);

//! Returns the AppMenuNode at the given index.
AppMenuNode* app_menu_data_source_get_node_at_index(AppMenuDataSource *source, uint16_t row_index);

//! Returns the AppMenuNode with the given AppInstallId.
uint16_t app_menu_data_source_get_index_of_app_with_install_id(AppMenuDataSource *source,
                                                               AppInstallId install_id);

//! Returns the count of AppMenuNode's in the source.
uint16_t app_menu_data_source_get_count(AppMenuDataSource *source);

//! Calls `menu_cell_basic_draw` for the AppMenuNode at the given MenuIndex
//! @note Will only draw an icon if `show_icons` is set to True.
void app_menu_data_source_draw_row(AppMenuDataSource *source, GContext *ctx, Layer *cell_layer,
                                   MenuIndex *cell_index);

//! Returns the allocated `GBitmap *` for the given node. Will return NULL if no icon is loaded.
//! @note Will only return an icon if `show_icons` is set to True.
GBitmap *app_menu_data_source_get_node_icon(AppMenuDataSource *source, AppMenuNode *node);
