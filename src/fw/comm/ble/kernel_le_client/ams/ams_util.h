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

//! Parses a string with a real number into int32_t, using given multiplication factor.
//! @param number_str The string containing a real, base-10 number. Some valid examples: "-1.234",
//! "42", "-.1", "1,0", "-0".
//! The string does not have to be zero-terminated, since the length is passed as an argument.
//! @param number_str_length The length of the number_str buffer.
//! @param multiplier The factor by which to multiply the parsed number.
//! @param[out] number_out If the parsing was successful, the result will be stored here.
//! @return True if the string was parsed successfully.
//! @note The first comma or period found is treated as decimal separator. Any subsequent comma or
//! period that is found will cause parsing to be aborted and return false.
//! @note An empty / zero-length string still will fail to parse and return false.
//! @note When the input number multiplied by the multiplier overflows the output storage (int32_t)
//! the function will return false.
bool ams_util_float_string_parse(const char *number_str, uint32_t number_str_length,
                                 int32_t multiplier, int32_t *number_out);

//! Value callback for use with ams_util_csv_parse
//! @param value The found value (not zero terminated!)
//! @param value_length The length of the found value in bytes
//! @param index The index of the value in the total CSV list
//! @param context User-specified callback, as passed into ams_util_csv_parse
//! @return True to continue parsing, false to stop parsing
typedef bool (*AMSUtilCSVCallback)(const char *value, uint32_t value_length,
                                   uint32_t index, void *context);

//! Parses a comma separated value string.
//! @param csv_value The buffer with the CSV string. The string does not necessarily need to be
//! NULL-terminated.
//! @param csv_length The length in bytes of csv_value
//! @param context User context that will be passed into the callback
//! @param callback The function to call for each found value.
//! @return The number of parsed values. In case the number of values in csv_value is different from
//! the number of callbacks passed, only up to the smallest number will be parsed.
uint8_t ams_util_csv_parse(const char *csv_value, uint32_t csv_length,
                           void *context, AMSUtilCSVCallback callback);
