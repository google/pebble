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

﻿#include "ancs_filtering.h"

#include "drivers/rtc.h"
#include "kernel/pbl_malloc.h"
#include "services/normal/notifications/alerts_preferences.h"
#include "services/normal/timeline/attributes_actions.h"
#include "system/logging.h"
#include "util/pstring.h"

void ancs_filtering_record_app(iOSNotifPrefs **notif_prefs,
                               const ANCSAttribute *app_id,
                               const ANCSAttribute *display_name,
                               const ANCSAttribute *title) {
  // When we receive a notification, information about the app that sent us the notification
  // is recorded in the notif_pref_db. We sync this DB with the phone which allows us to
  // do things like add non ANCS actions, or filter notifications by app

  // The "default" attributes are merged with any existing attributes. This makes it easy to add
  // new attributes in the future as well as support EMail / SMS apps which already have data
  // stored.

  iOSNotifPrefs *app_notif_prefs = *notif_prefs;
  const int num_existing_attributes = app_notif_prefs ? app_notif_prefs->attr_list.num_attributes :
                                                        0;

  AttributeList new_attr_list;
  attribute_list_init_list(num_existing_attributes, &new_attr_list);
  bool list_dirty = false;

  // Copy over all the existing attributes to our new list
  if (app_notif_prefs) {
    for (int i = 0; i < num_existing_attributes; i++) {
      new_attr_list.attributes[i] = app_notif_prefs->attr_list.attributes[i];
    }
  }

  // The app name should be the display name
  // If there is no display name (Apple Pay) then fallback to the title
  const ANCSAttribute *app_name_attr = NULL;
  if (display_name && display_name->length > 0) {
    app_name_attr = display_name;
  } else if (title && title->length > 0) {
    app_name_attr = title;
  }

  char *app_name_buff = NULL;
  if (app_name_attr) {
    const char *existing_name = "";
    if (app_notif_prefs) {
      existing_name = attribute_get_string(&app_notif_prefs->attr_list, AttributeIdAppName, "");
    }

    if (!pstring_equal_cstring(&app_name_attr->pstr, existing_name)) {
      // If the existing name doesn't match our new name, update the name
      app_name_buff = kernel_zalloc_check(app_name_attr->length + 1);
      pstring_pstring16_to_string(&app_name_attr->pstr, app_name_buff);
      attribute_list_add_cstring(&new_attr_list, AttributeIdAppName, app_name_buff);
      list_dirty = true;
      PBL_LOG(LOG_LEVEL_INFO, "Adding app name to app prefs: <%s>", app_name_buff);
    }
  }

  // Add the mute attribute if we don't have one already
  // Default the app to not muted
  const bool already_has_mute =
      app_notif_prefs && attribute_find(&app_notif_prefs->attr_list, AttributeIdMuteDayOfWeek);
  if (!already_has_mute) {
    attribute_list_add_uint8(&new_attr_list, AttributeIdMuteDayOfWeek, MuteBitfield_None);
    list_dirty = true;
  }

  // Add / update the "last seen" timestamp
  Attribute *last_updated = NULL;
  if (app_notif_prefs) {
    last_updated = attribute_find(&app_notif_prefs->attr_list, AttributeIdLastUpdated);
  }
  uint32_t now = rtc_get_time();
  // Only perform an update if there is no timestamp or the current timestamp is more than a day old
  if (!last_updated ||
      (last_updated && now > (last_updated->uint32 + SECONDS_PER_DAY))) {
    attribute_list_add_uint32(&new_attr_list, AttributeIdLastUpdated, now);
    list_dirty = true;
    PBL_LOG(LOG_LEVEL_INFO, "Updating / adding timestamp to app prefs");
  }

  if (list_dirty) {
    // We don't change or add actions at this time
    TimelineItemActionGroup *new_action_group = NULL;
    if (app_notif_prefs) {
      new_action_group = &app_notif_prefs->action_group;
    }

    ios_notif_pref_db_store_prefs(app_id->value, app_id->length,
                                  &new_attr_list, new_action_group);

    // Update our copy of the prefs with the new data
    const size_t buf_size = attributes_actions_get_buffer_size(&new_attr_list, new_action_group);
    *notif_prefs = kernel_zalloc_check(sizeof(iOSNotifPrefs) + buf_size);
    uint8_t *buffer = (uint8_t*)*notif_prefs + sizeof(iOSNotifPrefs);

    attributes_actions_deep_copy(&new_attr_list, &(*notif_prefs)->attr_list, new_action_group,
                                 &(*notif_prefs)->action_group, buffer, buffer + buf_size);
    ios_notif_pref_db_free_prefs(app_notif_prefs);
  }


  kernel_free(app_name_buff);
  attribute_list_destroy_list(&new_attr_list);
}

uint8_t ancs_filtering_get_mute_type(const iOSNotifPrefs *app_notif_prefs) {
  if (app_notif_prefs) {
    return attribute_get_uint8(&app_notif_prefs->attr_list,
                               AttributeIdMuteDayOfWeek,
                               MuteBitfield_None);
  }

  return MuteBitfield_None;
}

bool ancs_filtering_is_muted(const iOSNotifPrefs *app_notif_prefs) {
  uint8_t mute_type = ancs_filtering_get_mute_type(app_notif_prefs);

  struct tm now_tm;
  time_t now = rtc_get_time();
  localtime_r(&now, &now_tm);

  return mute_type & (1 << now_tm.tm_wday);
}
