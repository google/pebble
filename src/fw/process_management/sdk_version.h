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
#include "pebble_process_info.h"
#include <stdbool.h>

//! Inspects the app metadata whether the app supports app messaging.
//! Only if this returns true, the .messaging_info field of PebbleAppHandlers can be used.
//! @return true if the app is built with an SDK that supports app messaging or not
bool sdk_version_is_app_messaging_supported(const Version * const sdk_version);
