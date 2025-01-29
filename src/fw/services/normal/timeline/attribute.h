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

#include "util/stringlist.h"
#include <stdint.h>
#include <stdbool.h>

#define ATTRIBUTE_ICON_LARGE_SIZE_PX 80
#define ATTRIBUTE_ICON_SMALL_SIZE_PX 50
#define ATTRIBUTE_ICON_TINY_SIZE_PX 25

#define ATTRIBUTE_TITLE_MAX_LEN 64
#define ATTRIBUTE_SUBTITLE_MAX_LEN 64
#define ATTRIBUTE_APP_GLANCE_SUBTITLE_MAX_LEN (150)

#define Uint32ListSize(num_values) (sizeof(Uint32List) + (num_values) * sizeof(uint32_t))

typedef enum {
  AttributeIdUnused = 0,
  //! The title that will display in a detailed view.
  AttributeIdTitle = 1,
  //! Auxiliary text to display under title.
  AttributeIdSubtitle = 2,
  //! The body text that will display in the body of the view.
  AttributeIdBody = 3,
  //! Tiny Icon is the icon displayed in the status bar (if applicable).
  AttributeIdIconTiny = 4,
  //! Small icon is the icon displayed in the rendered view.
  AttributeIdIconSmall = 5,
  //! Large icon is the icon displayed when actions are triggered.
  AttributeIdIconLarge = 6,
  //! Internal ID to persist ANCS action for ANCS notifications.
  AttributeIdAncsAction = 7,
  //! Identifier for list of strings for responses
  AttributeIdCannedResponses = 8,
  //! The title that will display in a list view.
  AttributeIdShortTitle = 9,
  //! Pin Icon is the icon displayed in the timeline.
  AttributeIdIconPin = 10,
  //! Location Name is the name of the location (in a broad sense) if applicable of the item
  AttributeIdLocationName = 11,
  //! Sender is the name of the sender of the message, or organizer of the event, etc
  AttributeIdSender = 12,
  //! Launch code is an action attribute
  AttributeIdLaunchCode = 13,
  //! Last Updated is the unix timestamp of when the item was last updated, e.g. for weather
  AttributeIdLastUpdated = 14,
  //! Rank is the rank of the sports team in the league
  AttributeIdRankAway = 15,
  AttributeIdRankHome = 16,
  //! Name is the abbreviated name of the team (max 4 chars)
  AttributeIdNameAway = 17,
  AttributeIdNameHome = 18,
  //! Record is the record (wins / losses) of the team
  AttributeIdRecordAway = 19,
  AttributeIdRecordHome = 20,
  //! Score is the current score of the team for scored games (in-game only)
  AttributeIdScoreAway = 21,
  AttributeIdScoreHome = 22,
  //! If sports layout, if the layout should show "pregame" or "ingame" view
  //! See GameState enum in sports_layout.h
  AttributeIdSportsGameState = 23,
  //! Broadcaster is the TV channel, radio station, website, etc of a sports game or similar event
  AttributeIdBroadcaster = 24,
  //! Headings and Paragraphs are for Generic pins with additional detail sections
  AttributeIdHeadings = 25,
  AttributeIdParagraphs = 26,
  //! Colors related to the source
  AttributeIdPrimaryColor = 27,
  AttributeIdBgColor = 28,
  AttributeIdSecondaryColor = 29,
  //! Display name of the application the notification originates from
  AttributeIdAppName = 30,
  //! Recurring information to display typically for calendar events
  //! See CalendarRecurring enum in calendar_layout.h
  AttributeIdDisplayRecurring = 31,
  //! iOS App identifier, e.g. com.apple.MobileSMS
  AttributeIdiOSAppIdentifier = 32,
  //! True if emoji is supported, false if not
  AttributeIdEmojiSupported = 33,
  //! ANCS ID associated with the item/action (when parent is used to link to another item)
  AttributeIdAncsId = 34,
  //! (uint8_t) Health insight that the item is from
  AttributeIdHealthInsightType = 35,
  //! The subtitle that will display in a list view.
  AttributeIdShortSubtitle = 36,
  //! (uint32_t) ANCS Timestamp of the notification since the epoch
  AttributeIdTimestamp = 37,
  //! (uint8_t) Indicator for which timestamp to display (or none) typically for weather events
  AttributeIdDisplayTime = 38,
  //! String used for phone number, email address, whatsapp address, twitter, etc.
  AttributeIdAddress = 39,
  //! (uint8_t) Bitmask for which days of week notifications should be muted.
  //! Bit 1 = Sunday, Bit 7 = Saturday, Bit 8 unused.
  AttributeIdMuteDayOfWeek = 40,
  //! (StringList) Metric names for pins to display numeric data
  AttributeIdMetricNames = 41,
  //! (StringList) Metric values for Generic pins to display numeric data
  AttributeIdMetricValues = 42,
  //! (Uint32List) Metric icons, casted to TimelineResourceId (uin16_t) on use
  AttributeIdMetricIcons = 43,
  //! (uint8_t) Health activity that the item is from
  AttributeIdHealthActivityType = 44,
  //! (uint8_t) Kind of alarm, see AlarmKind enum in alarm.h
  AttributeIdAlarmKind = 45,
  //! String containing an authentication code (i.e. nexmo).
  AttributeIdAuthCode = 46,
  //! Auxiliary template string to display under a title.
  AttributeIdSubtitleTemplateString = 47,
  //! Generic icon.
  AttributeIdIcon = 48,
  NumAttributeIds,
} AttributeId;

typedef struct {
  uint16_t num_values;
  uint32_t values[];
} Uint32List;

typedef struct {
  AttributeId id;
  //! Based on the attribute id, we read the particular value corresponding to the
  //! type of attribute we're attempting to read to compensate for the lack of
  //! generics in C.
  union {
    char *cstring;
    uint8_t uint8;
    uint16_t uint16;
    uint32_t uint32;
    int8_t int8;
    int16_t int16;
    int32_t int32;
    StringList *string_list;
    Uint32List *uint32_list;
  };
} Attribute;

typedef struct {
  uint8_t num_attributes;
  Attribute *attributes;
} AttributeList;

//! Initialize a string type attribute
void attribute_init_string(Attribute *attribute, char *buffer, AttributeId attribute_id);

//! Copy an attribute into another attribute, placing any strings in a contiguous region
//! of memory given by buffer
//! @param dest a pointer to the destination attribute
//! @param src a pointer to the source attribute
//! @param buffer a pointer to a region of memory at least as long as the src string (if any). The
//! buffer pointer will be incremented if used, and will point the start of free memory
//! @param buffer_end a pointer to the end of the buffer
//! @return true if successful, false if buffer was not large enough
bool attribute_copy(Attribute *dest, const Attribute *src, uint8_t **buffer,
                    uint8_t *const buffer_end);

//! Copy an attribute list into another attribute list, placing all attributes
//! in the "out" list in a contiguous region of memory given by buffer
//! @param out a pointer to the destination attribute list
//! @param in a pointer to the source attribute list
//! @param buffer a pointer to a region of memory at least \ref attribute_list_get_buffer_size(in)
//! bytes
//! @param buffer_end a pointer to the end of the buffer
//! @return true if successful, false if buffer was not large enough
bool attribute_list_copy(AttributeList *out, const AttributeList *in, uint8_t *buffer,
                         uint8_t *const buffer_end);

//! Get the size required for a buffer to contain the attributes in an AttributeList
//! @param list pointer to an attribute list
//! @return the size in bytes of the buffer required
size_t attribute_list_get_buffer_size(const AttributeList *list);

//! Get the size required for a buffer to contain the strings in an AttributeList
//! @param list pointer to an attribute list
//! @return the size in bytes of the buffer required
size_t attribute_list_get_string_buffer_size(const AttributeList *list);

//! Append an attribute or replace an existing one in an attribute list.
//! Note: for cstring attributes, i.e. Title, Subtitle, Body
//! @param list pointer to the attribute list
//! @param id AttributeID of the attribute to add
//! @param cstring string to store as the content of the attribute
//! Note that this function does not make a deep copy of the string, so ensure
//! that cstring is not freed until the attribute list is copied or added to
//! a timeline item
void attribute_list_add_cstring(AttributeList *list, AttributeId id, const char *cstring);

//! Append an attribute or replace an existing one in an attribute list.
//! @param list pointer to the attribute list
//! @param id AttributeID of the attribute to add
//! @param uint32 value to store as the content of the attribute
void attribute_list_add_uint32(AttributeList *list, AttributeId id, uint32_t uint32);

//! Append an attribute or replace an existing one in an attribute list.
//! @param list pointer to the attribute list
//! @param id AttributeID of the attribute to add
//! @param TimelineResourceId value to store as the content of the attribute
void attribute_list_add_resource_id(AttributeList *list, AttributeId id,
                                    uint32_t resource_id);

//! Append an attribute or replace an existing one in an attribute list.
//! @param list pointer to the attribute list
//! @param id AttributeID of the attribute to add
//! @param uint8 value to store as the content of the attribute
void attribute_list_add_uint8(AttributeList *list, AttributeId id, uint8_t uint8);

//! Append an attribute or replace an existing one in an attribute list.
//! For StringList attributes, i.e. Headings, Paragraphs. This will not make
//! a deep copy, so ensure the StringList is not freed until the attribute list
//! is copied or added to a timeline item
//! @param list pointer to the attribute list
//! @param id AttributeId of the attribute to add
//! @param string_list StringList to store as the content of the attribute
void attribute_list_add_string_list(AttributeList *list, AttributeId id, StringList *string_list);

//! Append an attribute or replace an existing one in an attribute list.
//! For Uint32List attributes, i.e. MetricIcons, MetricValues. This will not make
//! a deep copy, so ensure the Uint32List is not freed until the attribute list
//! is copied or added to a timeline item
//! @param list pointer to the attribute list
//! @param id AttributeId of the attribute to add
//! @param uint32_list Uint32List to store as the content of the attribute
void attribute_list_add_uint32_list(AttributeList *list, AttributeId id, Uint32List *uint32_list);

//! Append an attribute or replace an existing one in an attribute list.
//! No deep copy is performed
//! @param list pointer to the attribute list
//! @param new_attribute The attribute to add
void attribute_list_add_attribute(AttributeList *list, const Attribute *new_attribute);

//! Initializes an attribute list.
//! @param num_attributes Number of attributes to initialize for this list
//! @param list the attribute list to initialize
void attribute_list_init_list(uint8_t num_attributes, AttributeList *list_out);

//! Destroy an attribute list.
//! Only use this when the attribute list was stack-allocated and used attribute_list_add_*,
//! not if the attribute list was a copy from attribute_list_copy()
//! @param list the attribute list to free
void attribute_list_destroy_list(AttributeList *list);

//! Find an attribute in a list by attribute ID
//! @param attr_list a pointer to an attribute list
//! @param id the attribute id of the desired attribute
//! @return a pointer to the attribute, NULL if not found
Attribute *attribute_find(const AttributeList *attr_list, AttributeId id);

//! Find a string attribute in a list by attribute ID
//! @param attr_list a pointer to an attribute list
//! @param id the attribute id of the desired attribute
//! @param default_value the value to return if not found
//! @return a pointer to the string, default_value if not found
const char *attribute_get_string(const AttributeList *attr_list,
    AttributeId id, char *default_value);

//! Find a string list attribute in an attribute list by attribute ID
//! @param attr_list a pointer to an attribute list
//! @param id the attribute id of the desired attribute
//! @return a pointer to the string list, NULL if not found
StringList *attribute_get_string_list(const AttributeList *attr_list, AttributeId id);

//! Find a uint8 attribute in a list by attribute ID
//! @param attr_list a pointer to an attribute list
//! @param id the attribute id of the desired attribute
//! @param default_value the value to return if not found
//! @return the uint8 attribute value, default_value if not found
uint8_t attribute_get_uint8(const AttributeList *attr_list,
    AttributeId id, uint8_t default_value);

//! Find a uint32 attribute in a list by attribute ID
//! @param attr_list a pointer to an attribute list
//! @param id the attribute id of the desired attribute
//! @param default_value the value to return if not found
//! @return the uint32 attribute value, default_value if not found
uint32_t attribute_get_uint32(const AttributeList *attr_list,
    AttributeId id, uint32_t default_value);

//! Find a Uint32List attribute in a list by attribute id
//! @param attr_list a pointer to an attribute list
//! @param id the attribute id of the desired attribute
//! @return a Uint32List pointer if found, otherwise NULL
Uint32List *attribute_get_uint32_list(const AttributeList *attr_list, AttributeId id);

//! Serialize a list of attributes into a buffer.
//! @param attr_list a pointer to the list of attributes to serialize
//! @param buffer a pointer to the buffer to write to
//! @param buf_end the end of buffer
//! @returns the number of serialized bytes
size_t attribute_list_serialize(const AttributeList *attr_list, uint8_t *buffer, uint8_t *buf_end);

//! Calculate the required size for a buffer to store a list of attributes
size_t attribute_list_get_serialized_size(const AttributeList *attr_list);

//! Check whether a serialized list is well-formed and output which attributes it contains
bool attribute_check_serialized_list(const uint8_t *cursor, const uint8_t *val_end,
    uint8_t num_attributes, bool has_attribute[]);

//! number of required bytes for in-memory representation of a list of a serialized attributes
int32_t attribute_get_buffer_size_for_serialized_attributes(uint8_t num_attributes,
    const uint8_t **cursor, const uint8_t *end);

//! true, if successfully transforms a serialized attribute into in-memory representation
bool attribute_deserialize_list(char **buffer, char *const buf_end,
    const uint8_t **cursor, const uint8_t *payload_end, AttributeList attr_list);
