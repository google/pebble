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

#include "api.h"
#include "timeline_item_storage.h"

#include "system/status_codes.h"
#include "services/normal/timeline/item.h"

// reminderdb specific

//! Get the \ref TimelineItem with a given ID
//! @param item_out pointer to a TimelineItem to store the item into
//! @param id pointer to the UUID that corresponds to the \ref TimelineItem to find
//! @return \ref S_SUCCESS if the function succeeds, error code otherwise
status_t reminder_db_read_item(TimelineItem *item_out, TimelineItemId *id);

//! Get the header of the earliest \ref TimelineItem in the reminderdb
//! @param next_item_out pointer to a \ref TimelineItem (header only, no attributes or actions)
//! which will be set to the earliest item in reminderdb
//! @return \ref S_NO_MORE_ITEMS if there are no items in reminderdb, S_SUCCESS on success,
//! error code otherwise
status_t reminder_db_next_item_header(TimelineItem *next_item_out);

//! Insert a timeline item into reminderdb
//! @param item pointer to item to insert into reminderdb
//! @return \ref S_SUCCESS if the function succeeds, error code otherwise
status_t reminder_db_insert_item(TimelineItem *item);

//! Delete an item from reminderdb by ID
//! @param id pointer to a \ref TimelineItemId to delete
//! @param send_event if true, send a PebbleReminderEvent after deletion
//! @return \ref S_SUCCESS if the function succeeds, error code otherwise
status_t reminder_db_delete_item(const TimelineItemId *id, bool send_event);

//! Delete every reminder in reminderdb that has a given parent
//! @param parent_id the uuid of the parent whose children we want to delete
//! @return \ref S_SUCCESS if all reminders were deleted, an error code otherwise
status_t reminder_db_delete_with_parent(const TimelineItemId *parent_id);

//! Check whether or not there are items in reminder db.
//! @return true if the DB is empty, false otherwise
bool reminder_db_is_empty(void);

status_t reminder_db_set_status_bits(const TimelineItemId *id, uint8_t status);

//! Finds a reminder that is identical to the specified one by first searching the timestamps,
//! then comparing the titles and lastly using the filter callback (if provided)
//! @param timestamp Time to match with
//! @param title Title to match with
//! @param filter Callback to custom filter function for additional checks
//! @param reminder_out Out param for matching reminder - cannot be null, only valid if the
//! function returns true
//! @return true if matching reminder found in storage, false otherwise
bool reminder_db_find_by_timestamp_title(time_t timestamp, const char *title,
                                         TimelineItemStorageFilterCallback filter,
                                         TimelineItem *reminder_out);

// blobdb functions
void reminder_db_init(void);

void reminder_db_deinit(void);

status_t reminder_db_insert(const uint8_t *key, int key_len, const uint8_t *val, int val_len);

int reminder_db_get_len(const uint8_t *key, int key_len);

status_t reminder_db_read(const uint8_t *key, int key_len, uint8_t *val_out, int val_out_len);

status_t reminder_db_delete(const uint8_t *key, int key_len);

status_t reminder_db_flush(void);

status_t reminder_db_is_dirty(bool *is_dirty_out);

BlobDBDirtyItem* reminder_db_get_dirty_list(void);

status_t reminder_db_mark_synced(const uint8_t *key, int key_len);
