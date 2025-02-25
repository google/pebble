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

#include "item.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

//! Takes serialized data and fills two arrays: string_alloc_size_out and attributes_per_action_out
//! The information in these arrays is used in the following steps
//! @param num_attributes   number of non-action attributes
//! @param num_actions      number of actions
//! @param data               serialized data buffer
//! @param data_size          size of the serial data buffer
//! @param string_alloc_size_out        size of string buffer that is required
//! @param attributes_per_action_out    an array of counts for the number of attributes per action
//!                                     in order corresponding to action order
//! @return True if the data was parsed successfully, False if not
bool attributes_actions_parse_serial_data(uint8_t num_attributes,
                                          uint8_t num_actions,
                                          const uint8_t *data,
                                          size_t data_size,
                                          size_t *string_alloc_size_out,
                                          uint8_t *attributes_per_action_out);

//! Return the size of the buffer needed to store the attributes, actions and their strings
//! @param num_attributes   number of non-action attributes
//! @param num_actions      number of actions
//! @param attributes_per_action    an array of counts for the number of attributes per action
//!                                 in order corresponding to action order
//! @param required_size_for_strings    total size of all attribute strings
//! @return The size of the buffer required to store the attributes, actions and strings
size_t attributes_actions_get_required_buffer_size(uint8_t num_attributes,
                                                   uint8_t num_actions,
                                                   uint8_t *attributes_per_action,
                                                   size_t required_size_for_strings);


//! @return The size of the buffer needed to hold the attribute list and action group
size_t attributes_actions_get_buffer_size(AttributeList *attr_list,
                                          TimelineItemActionGroup *action_group);

//! Initializes an AttributeList and ActionGroup
//! @param attr_list          The AttributeList to initialize
//! @param action_group       The ActionGroup to initialize
//! @param buffer             The buffer to hold the list of attributes and actions
//! @param num_attributes     number of attributes
//! @param num_actions        number of actions
//! @param attributes_per_action    an array of counts for the number of attributes per action
//!                                 in order corresponding to action order
void attributes_actions_init(AttributeList *attr_list,
                             TimelineItemActionGroup *action_group,
                             uint8_t **buffer,
                             uint8_t num_attributes,
                             uint8_t num_actions,
                             const uint8_t *attributes_per_action);

//! Fills an AttributeList and ActionGroup from serialized data
//! @param attr_list          The AttributeList to fill
//! @param action_group       The ActionGroup to fill
//! @param buffer             The buffer which holds the list of attributes and actions
//! @param buf_end            A pointer to the end of the buffer
//! @param payload            Serialized payload buffer
//! @param payload_size       Size of the payload buffer in bytes
bool attributes_actions_deserialize(AttributeList *attr_list,
                                    TimelineItemActionGroup *action_group,
                                    uint8_t *buffer,
                                    uint8_t *buf_end,
                                    const uint8_t *payload,
                                    size_t payload_size);

//! Calculate the required size for a buffer to store actions & attributes
size_t attributes_actions_get_serialized_payload_size(AttributeList *list,
                                                      TimelineItemActionGroup *action_group);

//! Serializes an attribute list and action group into a buffer
//! @param attr_list          The AttributeList to serialize
//! @param action_group       The ActionGroup to serialize
//! @param buffer a pointer to the buffer to write to
//! @param buffer_size the size of the buffer in bytes
//! @returns the number of bytes written to buffer
size_t attributes_actions_serialize_payload(AttributeList *attr_list,
                                            TimelineItemActionGroup *action_group,
                                            uint8_t *buffer,
                                            size_t buffer_size);

//! @return true if successful, false if the buffer isn't large enough
bool attributes_actions_deep_copy(AttributeList *src_attr_list,
                                  AttributeList *dest_attr_list,
                                  TimelineItemActionGroup *src_action_group,
                                  TimelineItemActionGroup *dest_action_group,
                                  uint8_t *buffer, uint8_t *buf_end);
