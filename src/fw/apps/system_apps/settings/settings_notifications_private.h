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

#include "settings_notifications.h"

#include "applib/preferred_content_size.h"

//! TODO PBL-41920: This mapping should be an opt-in set in a platform specific location
typedef enum SettingsContentSize {
  SettingsContentSize_Small,
  SettingsContentSize_Default,
  SettingsContentSize_Large,
  SettingsContentSizeCount,
} SettingsContentSize;

static inline SettingsContentSize settings_content_size_from_preferred_size(
    PreferredContentSize preferred_size) {
  return preferred_size + (SettingsContentSize_Default - PreferredContentSizeDefault);
}

static inline PreferredContentSize settings_content_size_to_preferred_size(
    SettingsContentSize settings_size) {
  return settings_size + (PreferredContentSizeDefault - SettingsContentSize_Default);
}
