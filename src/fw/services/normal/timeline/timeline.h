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
#include "services/normal/timeline/item.h"
#include "system/status_codes.h"
#include "util/iterator.h"

struct TimelineNode;
typedef struct TimelineNode TimelineNode;

typedef enum {
  TimelineIterDirectionPast,
  TimelineIterDirectionFuture,
} TimelineIterDirection;

typedef struct {
  TimelineNode *node;
  int index;
  time_t start_time;
  TimelineIterDirection direction;
  TimelineItem pin;
  bool show_all_day_events;
  time_t midnight; // midnight at iter_init
  time_t current_day; // midnight of the current pin
} TimelineIterState;

//! initialize the timeline (builds the list of TimelineNodes)
status_t timeline_init(TimelineNode **timeline);

//! Add a timeline pin we've created to the timeline.
//! Call \ref timeline_destroy_item after this in order to free up the memory used by the item.
//! @return true on success, false otherwise
bool timeline_add(TimelineItem *item);

bool timeline_add_missed_call_pin(TimelineItem *pin, uint32_t uid);

//! Remove a timeline pin we've added to the timeline
//! @return true on success, false otherwise
bool timeline_remove(const Uuid *id);

//! Check whether a timeline pin exists
//! @return true if it does, false otherwise
bool timeline_exists(Uuid *id);

//! Enables bulk action mode for ancs actions to avoid filling the event queue
void timeline_enable_ancs_bulk_action_mode(bool enable);

//! Returns whether or not bulk action mode is enabled for ancs actions
bool timeline_is_bulk_ancs_action_mode_enabled(void);

//! invokes a timelineitem's action. This can end up triggering a bluetooth message.
void timeline_invoke_action(const TimelineItem *item, const TimelineItemAction *action,
                            const AttributeList *attributes);

TimelineIterDirection timeline_direction_for_item(TimelineItem *item,
    TimelineNode *timeline, time_t now);

bool timeline_nodes_equal(TimelineNode *a, TimelineNode *b);

//! Get the UUID of the originator of a timeline item. For pins and notifications, this
//! returns the first parent_id of the item, which is the app's UUID (for pins) or source ID
//! (for notifications). For reminders, it will return the parent_id of the parent, which is the
//! app UUID of the pin that created the reminder.
//! @param [out] id pointer to storage for returned uuid. Set to UUID_INVALID when false
//!   is returned
//! @return true if success, false on failure
bool timeline_get_originator_id(const TimelineItem *item, Uuid *id);

//! Timeline item time comparator which sorts items as they would appear in Timeline with the
//! exception of all day events.
//! @param new_common The common that the resulting value is in reference to.
//! @param old_common The other common that is being compared against.
//! @param direction The Timeline direction.
//! @return < 0 if new_common should be before old_common, > 0 if new_common should be after
//! old_common, and 0 if equal in priority.
int timeline_item_time_comparator(CommonTimelineItemHeader *new_common,
                                  CommonTimelineItemHeader *old_common,
                                  TimelineIterDirection direction);

//! Whether a Timeline item should show up in the Timeline direction with the exception of all
//! day events.
//! @param common The common header of the item to consider.
//! @param direction The Timeline direction.
//! @return true if the item would show up, false otherwise.
bool timeline_item_should_show(CommonTimelineItemHeader *header, TimelineIterDirection direction);

///////////////////////////////////
//! Timeline Iterator functions
///////////////////////////////////

status_t timeline_iter_init(Iterator *iter, TimelineIterState *iter_state, TimelineNode **timeline,
    TimelineIterDirection direction, time_t timestamp);

// Copy an iterator's contents into another one
void timeline_iter_copy_state(TimelineIterState *dst_state, TimelineIterState *src_state,
    Iterator *dst_iter, Iterator *src_iter);

void timeline_iter_deinit(Iterator *iter, TimelineIterState *iter_state, TimelineNode **head);

//! refresh the pin at the current timeline iterator. Does a fairly naive refresh, i.e. does not
//! correctly place the pin in the timeline if the timestamp changes
void timeline_iter_refresh_pin(TimelineIterState *iter_state);

//! Remove a timeline item from the iterator list
void timeline_iter_remove_node(TimelineNode **timeline, TimelineNode *node);

//! Remove a timeline item from the iterator list
//! @return true if a node exists and was removed, false otherwise
bool timeline_iter_remove_node_with_id(TimelineNode **timeline, Uuid *key);

///////////////////////////////////
//! Timeline datasource functions
///////////////////////////////////

// ed429c16-f674-4220-95da-454f303f15e2
#define UUID_NOTIFICATIONS_DATA_SOURCE {0xed, 0x42, 0x9c, 0x16, 0xf6, 0x74, 0x42, 0x20, 0x95, \
                                        0xda, 0x45, 0x4f, 0x30, 0x3f, 0x15, 0xe2}

// 6c6c6fc2-1912-4d25-8396-3547d1dfac5b
#define UUID_CALENDAR_DATA_SOURCE {0x6c, 0x6c, 0x6f, 0xc2, 0x19, 0x12, 0x4d, 0x25, 0x83, \
                                   0x96, 0x35, 0x47, 0xd1, 0xdf, 0xac, 0x5b}

// 61b22bc8-1e29-460d-a236-3fe409a439ff
#define UUID_WEATHER_DATA_SOURCE {0x61, 0xb2, 0x2b, 0xc8, 0x1e, 0x29, 0x46, 0xd, 0xa2, \
                                  0x36, 0x3f, 0xe4, 0x9, 0xa4, 0x39, 0xff}

// 42a07217-5491-4267-904a-d02a156752b6
#define UUID_REMINDERS_DATA_SOURCE {0x42, 0xa0, 0x72, 0x17, 0x54, 0x91, 0x42, 0x67, \
                                    0x90, 0x4a, 0xd0, 0x2a, 0x15, 0x67, 0x52, 0xb6}

// UUID: 67a32d95-ef69-46d4-a0b9-854cc62f97f9
#define UUID_ALARMS_DATA_SOURCE {0x67, 0xa3, 0x2d, 0x95, 0xef, 0x69, 0x46, 0xd4, \
                                 0xa0, 0xb9, 0x85, 0x4c, 0xc6, 0x2f, 0x97, 0xf9}

// UUID: 36d8c6ed-4c83-4fa1-a9e2-8f12dc941f8c
#define UUID_HEALTH_DATA_SOURCE {0x36, 0xd8, 0xc6, 0xed, 0x4c, 0x83, 0x4f, 0xa1, \
                                 0xa9, 0xe2, 0x8f, 0x12, 0xdc, 0x94, 0x1f, 0x8c}

// UUID: fef82c82-7176-4e22-88de-35a3fc18d43f
#define UUID_WORKOUT_DATA_SOURCE {0xfe, 0xf8, 0x2c, 0x82, 0x71, 0x76, 0x4e, 0x22, \
                                  0x88, 0xde, 0x35, 0xa3, 0xfc, 0x18, 0xd4, 0x3f}

// UUID: 0863fc6a-66c5-4f62-ab8a-82ed00a98b5d
#define UUID_SEND_TEXT_DATA_SOURCE {0x08, 0x63, 0xfc, 0x6a, 0x66, 0xc5, 0x4f, 0x62, \
                                    0xab, 0x8a, 0x82, 0xed, 0x00, 0xa9, 0x8b, 0x5d}

// UUID: 0f71aaba-5814-4b5c-96e2-c9828c9734cb
// Special UUID that allows the watch to send SMS messages to a specific phone number
#define UUID_SEND_SMS {0x0f, 0x71, 0xaa, 0xba, 0x58, 0x14, 0x4b, 0x5c, \
                       0x96, 0xe2, 0xc9, 0x82, 0x8c, 0x97, 0x34, 0xcb}

// UUID: 68010669-4b38-4751-ad04-067f1d8d2ab5
#define UUID_INTERCOM_DATA_SOURCE {0x68, 0x01, 0x06, 0x69, 0x4b, 0x38, 0x47, 0x51, \
                                   0xad, 0x04, 0x06, 0x7f, 0x1d, 0x8d, 0x2a, 0xb5}

//! Get the name of a non-app, i.e. "private" datasource like Weather or Calendar
//! return NULL if parent_id is not a private data source, otherwise the name of the source
const char *timeline_get_private_data_source(Uuid *parent_id);
