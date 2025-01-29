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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// PascalStrings with string length of 0 are considered empty.
typedef struct {
  uint16_t str_length;
  char str_value[];
} PascalString16;

typedef struct {
  uint16_t data_size;
  uint8_t data[];
} SerializedArray;

// Used to encapsulate multiple PascalStrings.
// Empty PascalStrings only have their length serialized (no byte for value).
typedef struct {
  uint16_t count;
  SerializedArray *pstrings;
} PascalString16List;

// Create a PascalString16 with the passed max pstring size.
PascalString16 *pstring_create_pstring16(uint16_t size);

// Create a PascalString16 from the passed string.
PascalString16 *pstring_create_pstring16_from_string(char string[]);

void pstring_destroy_pstring16(PascalString16 *pstring);

//! @param string_out Array of chars to hold the converted pstring.
//!                   Must be at least size (pstring->length + 1).
void pstring_pstring16_to_string(const PascalString16 *pstring, char *string_out);

//! @param pstring_out Array of chars to hold the converted string.
//!                    Must be at least size (string + 1).
void pstring_string_to_pstring16(char string[], PascalString16 *pstring_out);

//! Checks if 2 pstrings are equal and returns true if so.
//! @note returns false if either / both pstrings are NULL
bool pstring_equal(const PascalString16 *ps1, const PascalString16 *ps2);

//! Compares a pstring to a cstring and returns true if they match
//! @note returns false if either / both params are NULL
bool pstring_equal_cstring(const PascalString16 *pstr, const char *cstr);

//------

SerializedArray *pstring_create_serialized_array(uint16_t data_size);

void pstring_destroy_serialized_array(SerializedArray* serialized_array);

// Projects a list on a serialized array so that pstring operations may be performed on it.
void pstring_project_list_on_serialized_array(PascalString16List *pstring16_list,
                                              SerializedArray *serialized_array);

// Adds a PascalString16 to the end of the list.
// Returns true if the PascalString16 was successfully added, false if there was no room.
bool pstring_add_pstring16_to_list(PascalString16List *pstring16_list, PascalString16* pstring);

// Retrieves the number of PascalString16s in the list.
uint16_t pstring_get_number_of_pstring16s_in_list(PascalString16List *pstring16_list);

// Returns a pointer to a PascalString16 of the passed index within the list.
// If the given index is not valid or the list is empty, returns NULL.
PascalString16* pstring_get_pstring16_from_list(PascalString16List *pstring16_list, uint16_t index);

void pstring_print_pstring(PascalString16 *pstring);

void pstring_print_pstring16list(PascalString16List *list);
