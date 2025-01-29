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

#include "system/status_codes.h"
#include "services/normal/timeline/item.h"
#include "services/common/new_timer/new_timer.h"

typedef TimelineItem Reminder;
typedef TimelineItemId ReminderId;

//! Set the reminder timer to the next stored reminder chronologically
//! @return S_SUCCESS or appropriate error
status_t reminders_update_timer(void);

//! Insert a reminder to be popped up at a certain time
//! @param reminder pointer to the reminder to be inserted
//! @return S_SUCCESS or appropriate error
status_t reminders_insert(Reminder *reminder);

//! Initialize the reminders so they can be activated on the watch
//! @return S_SUCCESS or appropriate error
status_t reminders_init(void);

//! Delete a reminder
//! @param reminder_id pointer to an ID of the reminder to be deleted
//! @return S_SUCCESS or appropriate error
status_t reminders_delete(ReminderId *reminder_id);

//! @return True if the reminder can snooze for a non-zero amount of time, false otherwise.
bool reminders_can_snooze(Reminder *reminder);

//! Snooze a reminder
//! @param reminder Pointer to the reminder to snooze
//! @return S_SUCCESS, E_INVALID_OPERATION if cannot snooze, or some other error otherwise
status_t reminders_snooze(Reminder *reminder);

//! Creates an event to alert the system that a reminder has been removed
//! @param reminder_id Pointer to the uuid of the removed reminder
void reminders_handle_reminder_removed(const Uuid *reminder_id);

//! Creates an event to alert the system that a triggered reminder has changed
//! @param reminder_id Pointer to the uuid of the updated reminder
void reminders_handle_reminder_updated(const Uuid *reminder_id);
