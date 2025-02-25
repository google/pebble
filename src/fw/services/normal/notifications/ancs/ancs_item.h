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

#include "ancs_notifications_util.h"

#include "comm/ble/kernel_le_client/ancs/ancs_types.h"
#include "services/normal/blob_db/ios_notif_pref_db.h"
#include "services/normal/timeline/timeline.h"

//! Creates a new timeline item from ANCS data
//! @param notif_attributes ANCS Notification attributes
//! @param app_attributes ANCS App attributes (namely, the display name)
//! @param app_metadata The icon and color associated with the app
//! @param notif_prefs iOS notification prefs for this notification
//! @param timestamp Time the notification occurred
//! @param properties Additional ANCS properties (category, flags, etc)
//! @return The newly created timeline item
TimelineItem *ancs_item_create_and_populate(ANCSAttribute *notif_attributes[],
                                            ANCSAttribute *app_attributes[],
                                            const ANCSAppMetadata *app_metadata,
                                            iOSNotifPrefs *notif_prefs,
                                            time_t timestamp,
                                            ANCSProperty properties);

//! Replaces the dismiss action of an existing timeline item with the ancs negative action
//! @param item The timeline item to update
//! @param uid The uid of the ANCS notification we're using the dismiss action from
//! @param attr_action_neg The negative action from the ANCS notification to use as the new
//! dismiss action
void ancs_item_update_dismiss_action(TimelineItem *item, uint32_t uid,
                                     const ANCSAttribute *attr_action_neg);
