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

#include "util/uuid.h"

//! Launcher App Message is deprecated and on Android >= 2.3 and other devices that pass the
//! support flags for the AppRunState endpoint will use that endpoint (0x34) instead.  That
//! endpoint should be used for sending messages on start/stop status of applications and
//! sending/receiving application states.  The LauncherAppMessage endpoint is kept for
//! backwards compatibility with older mobile applications.
void launcher_app_message_send_app_state_deprecated(const Uuid *uuid, bool running);
