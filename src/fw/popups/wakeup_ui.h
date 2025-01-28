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

#include <stdint.h>

#include "process_management/app_install_types.h"

//! This function creates a popup window displaying a missed wakeup events notification
//! along with the application names that were missed
//! Note: missed_apps_ids is free'd by this function
//! @param missed_apps_count number of app names to display
//! @param missed_app_ids an array of AppInstallIds of the app names to display
void wakeup_popup_window(uint8_t missed_apps_count, AppInstallId *missed_app_ids);

