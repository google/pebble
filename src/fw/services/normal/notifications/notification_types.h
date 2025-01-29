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

//! This list is shared by notifications and reminders.
typedef enum {
  NotificationInvalid   = 0,
  NotificationMobile    = (1 << 0),
  NotificationPhoneCall = (1 << 1),
  NotificationOther     = (1 << 2),
  NotificationReminder  = (1 << 3)
} NotificationType;

//! Type and ID for the notification or reminder.
typedef struct {
  NotificationType type;
  Uuid id;
} NotificationInfo;
