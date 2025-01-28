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
#include "services/normal/notifications/notification_types.h"

#include <inttypes.h>

//! @file notifications_presented_list.h
//!
//! \brief File that manages a list of presented notifications.

typedef struct  {
  ListNode list_node;
  NotificationInfo notif;
} NotifList;

//! Get the first notification ID in the presented list
Uuid *notifications_presented_list_first(void);

//! Get the last notification ID in the presented list
Uuid *notifications_presented_list_last(void);

//! Get the notification ID that has the given relative offset from the given id
Uuid *notifications_presented_list_relative(Uuid *id, int offset);

//! Get the currently presented notification in the list
Uuid *notifications_presented_list_current(void);

//! Get the next notification in the list
Uuid *notifications_presented_list_next(void);

//! Set the current notification in the presented list (user scrolled, new notif, etc)
bool notifications_presented_list_set_current(Uuid *id);

//! Remove the given notification from the presented list
void notifications_presented_list_remove(Uuid *id);

//! Add the given notification to the presented list
void notifications_presented_list_add(Uuid *id, NotificationType type);

//! Add the given notification to the presented list
//! The comparator will have to compare two NotifList*
void notifications_presented_list_add_sorted(Uuid *id, NotificationType type,
                                             Comparator comparator, bool ascending);

//! Get the type of the given notification
NotificationType notifications_presented_list_get_type(Uuid *id);

//! Get the count of notifications in the presented list
int notifications_presented_list_count(void);

//! Get the current index (integer based) of the current notification in the presented list
//! This is used for the status bar (ex. "2/5")
int notifications_presented_list_current_idx(void);

//! Inits the notification presented list
void notifications_presented_list_init(void);

typedef void (*NotificationListEachCallback)(Uuid *id, NotificationType type, void *cb_data);

//! Executes the specified callback for each notification in the presented list
//! @param callback If null this function is a no-op
void notifications_presented_list_each(NotificationListEachCallback callback, void *cb_data);

//! Deinits the notification presented list
//! @param callback - If non-null, notifies the caller what item is being removed.
//!            The callback routine should not try to modify the notification list
void notifications_presented_list_deinit(NotificationListEachCallback callback, void *cb_data);
