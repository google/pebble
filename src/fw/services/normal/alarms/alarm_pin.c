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

#include "alarm_pin.h"

#include "apps/system_app_ids.h"
#include "kernel/pbl_malloc.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/blob_db/pin_db.h"
#include "services/normal/timeline/attribute.h"
#include "services/normal/timeline/timeline.h"
#include "services/normal/timeline/timeline_resources.h"


// ----------------------------------------------------------------------------------------------
//! Sets attributes for an alarm pin
static void prv_set_pin_attributes(AttributeList *list, AlarmType type, AlarmKind kind) {
  const bool is_smart = (type == AlarmType_Smart);
  attribute_list_add_cstring(list, AttributeIdTitle,
                              is_smart ? i18n_get("Smart Alarm", list) : i18n_get("Alarm", list));
  attribute_list_add_resource_id(list, AttributeIdIconPin,
                                 is_smart ? TIMELINE_RESOURCE_SMART_ALARM :
                                            TIMELINE_RESOURCE_ALARM_CLOCK);
  attribute_list_add_resource_id(list, AttributeIdIconTiny, TIMELINE_RESOURCE_ALARM_CLOCK);
  const bool all_caps = false;
  const char *alarm_string = i18n_get(alarm_get_string_for_kind(kind, all_caps), list);
  attribute_list_add_cstring(list, AttributeIdSubtitle, alarm_string);
  attribute_list_add_uint8(list, AttributeIdAlarmKind, kind);
}

// ----------------------------------------------------------------------------------------------
static void prv_set_edit_action_attributes(AttributeList *list) {
  attribute_list_add_cstring(list, AttributeIdTitle, i18n_get("Edit", list));
}

// ----------------------------------------------------------------------------------------------
void alarm_pin_add(time_t alarm_time, AlarmId id, AlarmType type, AlarmKind kind, Uuid *uuid_out) {
  const unsigned num_actions = 1; // We are just supporting "edit" for now
  TimelineItemActionGroup action_group = {
    .num_actions = num_actions,
    .actions = task_zalloc_check(sizeof(TimelineItemAction) * num_actions),
  };
  AttributeList edit_attr_list = {0};
  prv_set_edit_action_attributes(&edit_attr_list);
  action_group.actions[0] = (TimelineItemAction) {
    .id = (uint8_t) id, // id is guaranteed to be valid here, and we only support 10 alarms
    .type = TimelineItemActionTypeOpenWatchApp,
    .attr_list = edit_attr_list,
  };

  AttributeList pin_attr_list = {0};
  prv_set_pin_attributes(&pin_attr_list, type, kind);
  TimelineItem *item = timeline_item_create_with_attributes(alarm_time, 0, TimelineItemTypePin,
      LayoutIdAlarm, &pin_attr_list, &action_group);
  item->header.from_watch = true;
  item->header.parent_id = (Uuid)UUID_ALARMS_DATA_SOURCE;

  pin_db_insert_item_without_event(item);

  i18n_free_all(&pin_attr_list);
  i18n_free_all(&edit_attr_list);
  attribute_list_destroy_list(&pin_attr_list);
  attribute_list_destroy_list(&edit_attr_list);
  task_free(action_group.actions);

  if (uuid_out) {
    *uuid_out = item->header.id;
  }

  timeline_item_destroy(item);
}

// ----------------------------------------------------------------------------------------------
void alarm_pin_remove(Uuid *alarm_id) {
  pin_db_delete((uint8_t *)alarm_id, sizeof(Uuid));
}
