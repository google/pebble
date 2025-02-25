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

#include "rocky_api_datetime.h"
#include "rocky_api_errors.h"
#include "rocky_api_util.h"
#include "services/common/clock.h"
#include "system/passert.h"
#include "util/size.h"

#include <string.h>

#define ROCKY_DATE_TOLOCALETIMESTRING "toLocaleTimeString"
#define ROCKY_DATE_TOLOCALEDATESTRING "toLocaleDateString"
#define ROCKY_DATE_TOLOCALESTRING "toLocaleString"
#define ROCKY_DATE_FORMAT_NUMERIC "numeric"
#define ROCKY_DATE_FORMAT_2DIGIT "2-digit"
#define ROCKY_DATE_FORMAT_SHORT "short"
#define ROCKY_DATE_FORMAT_LONG "long"

#define BUFFER_LEN_DATE 40
#define BUFFER_LEN_TIME 20
// 2 = strlen(", ")
#define BUFFER_LEN_DATETIME (BUFFER_LEN_DATE + BUFFER_LEN_TIME + 2)


static void prv_tm_from_js_date(jerry_value_t date, struct tm *tm) {
  JS_VAR js_seconds = jerry_get_object_getter_result(date, "getSeconds");
  JS_VAR js_minutes = jerry_get_object_getter_result(date, "getMinutes");
  JS_VAR js_hours = jerry_get_object_getter_result(date, "getHours");
  JS_VAR js_mdays = jerry_get_object_getter_result(date, "getDate");
  JS_VAR js_month = jerry_get_object_getter_result(date, "getMonth");
  JS_VAR js_year = jerry_get_object_getter_result(date, "getFullYear");
  JS_VAR js_wday = jerry_get_object_getter_result(date, "getDay");

  *tm = (struct tm) {
    .tm_sec = (int)jerry_get_number_value(js_seconds),
    .tm_min = (int)jerry_get_number_value(js_minutes),
    .tm_hour = (int)jerry_get_number_value(js_hours),
    .tm_mday = (int)jerry_get_number_value(js_mdays),
    .tm_mon = (int)jerry_get_number_value(js_month),
    .tm_year = (int)jerry_get_number_value(js_year) - 1900,
    .tm_wday = (int)jerry_get_number_value(js_wday),
// seems as we can live without those for now
//    .tm_yday = ,
//    .tm_isdst = ,
//    .tm_gmtoff = ,
//    .tm_zone = ,
  };
}

static bool prv_matches_system_locale(jerry_value_t locale) {
  if (jerry_value_is_undefined(locale)) {
    return true;
  }

  // in the future, we could run a case-insensitive compare against app_get_system_locale()
  // but as we want apps to encourage to be i18n, there's no real point to
  // receive strings such as 'en-us'. We will ask them to always pass undefined instead
  return false;
}

typedef enum {
  ToStringFormatUnsupported = 1 << 0,
  ToStringFormatLocaleTime = 1 << 1,
  ToStringFormatSecondNumeric = 1 << 2,
  ToStringFormatSecond2Digit = 1 << 3,
  ToStringFormatMinuteNumeric = 1 << 4,
  ToStringFormatMinute2Digit = 1 << 5,
  ToStringFormatHourNumeric = 1 << 6,
  ToStringFormatHour2Digit = 1 << 7,
  ToStringFormatLocaleDate = 1 << 8,
  ToStringFormatDayNumeric = 1 << 9,
  ToStringFormatDay2Digit = 1 << 10,
  ToStringFormatDayShort = 1 << 11,
  ToStringFormatDayLong = 1 << 12,
  ToStringFormatMonthNumeric = 1 << 13,
  ToStringFormatMonth2Digit = 1 << 14,
  ToStringFormatMonthShort = 1 << 15,
  ToStringFormatMonthLong = 1 << 16,
  ToStringFormatYearNumeric = 1 << 17,
  ToStringFormatYear2Digit = 1 << 18,
  ToStringFormatEmpty = 1 << 19,
} ToStringFormat;

static const ToStringFormat ToStringFormatTimeMask = (
  ToStringFormatLocaleTime |
  ToStringFormatSecondNumeric |
  ToStringFormatSecond2Digit |
  ToStringFormatMinuteNumeric |
  ToStringFormatMinute2Digit |
  ToStringFormatHourNumeric |
  ToStringFormatHour2Digit
);

static const ToStringFormat ToStringFormatDateMask = (
  ToStringFormatLocaleDate |
  ToStringFormatDayNumeric |
  ToStringFormatDay2Digit |
  ToStringFormatDayShort |
  ToStringFormatDayLong |
  ToStringFormatMonthNumeric |
  ToStringFormatMonth2Digit |
  ToStringFormatMonthShort |
  ToStringFormatMonthLong |
  ToStringFormatYearNumeric |
  ToStringFormatYear2Digit
);

static ToStringFormat prv_parse_to_string_format(const jerry_value_t options,
                                                 ToStringFormat default_format,
                                                 ToStringFormat mask,
                                                 bool *is_24h_style) {
  JS_VAR second = jerry_get_object_field(options, "second");
  JS_VAR minute = jerry_get_object_field(options, "minute");
  JS_VAR hour = jerry_get_object_field(options, "hour");
  JS_VAR day = jerry_get_object_field(options, "day");
  JS_VAR month = jerry_get_object_field(options, "month");
  JS_VAR year = jerry_get_object_field(options, "year");
  JS_VAR hour12 = jerry_get_object_field(options, "hour12");

  if (!jerry_value_is_undefined(hour12)) {
    *is_24h_style = !jerry_get_boolean_value(hour12);
  }

  struct {
    const jerry_value_t field;
    const char *value;
    ToStringFormat format;
  } option_values[] = {
    {.field = second, .value = ROCKY_DATE_FORMAT_NUMERIC, .format = ToStringFormatSecondNumeric},
    {.field = second, .value = ROCKY_DATE_FORMAT_2DIGIT, .format = ToStringFormatSecond2Digit},
    {.field = minute, .value = ROCKY_DATE_FORMAT_NUMERIC, .format = ToStringFormatMinuteNumeric},
    {.field = minute, .value = ROCKY_DATE_FORMAT_2DIGIT, .format = ToStringFormatMinute2Digit},
    {.field = hour, .value = ROCKY_DATE_FORMAT_NUMERIC, .format = ToStringFormatHourNumeric},
    {.field = hour, .value = ROCKY_DATE_FORMAT_2DIGIT, .format = ToStringFormatHour2Digit},
    {.field = day, .value = ROCKY_DATE_FORMAT_NUMERIC, .format = ToStringFormatDayNumeric},
    {.field = day, .value = ROCKY_DATE_FORMAT_2DIGIT, .format = ToStringFormatDay2Digit},
    {.field = day, .value = ROCKY_DATE_FORMAT_SHORT, .format = ToStringFormatDayShort},
    {.field = day, .value = ROCKY_DATE_FORMAT_LONG, .format = ToStringFormatDayLong},
    {.field = month, .value = ROCKY_DATE_FORMAT_NUMERIC, .format = ToStringFormatMonthNumeric},
    {.field = month, .value = ROCKY_DATE_FORMAT_2DIGIT, .format = ToStringFormatMonth2Digit},
    {.field = month, .value = ROCKY_DATE_FORMAT_SHORT, .format = ToStringFormatMonthShort},
    {.field = month, .value = ROCKY_DATE_FORMAT_LONG, .format = ToStringFormatMonthLong},
    {.field = year, .value = ROCKY_DATE_FORMAT_NUMERIC, .format = ToStringFormatYearNumeric},
    {.field = year, .value = ROCKY_DATE_FORMAT_2DIGIT, .format = ToStringFormatYear2Digit},
  };

  int found_options = 0;
  ToStringFormat result = default_format;
  for (size_t i = 0; i < ARRAY_LENGTH(option_values); i++) {
    if ((option_values[i].format & mask) == 0) {
      // skip option values that are irrelevant
      continue;
    }
    if (rocky_str_equal(option_values[i].field, option_values[i].value)) {
      result = option_values[i].format;
      found_options++;
    }
    if (found_options > 1) {
      // today, we don't support combinations of several options, it's either none or one
      return ToStringFormatUnsupported;
    }
  }

  return result;
}

static const char *prv_strftime_format(ToStringFormat format, bool is_24h_style) {
  switch (format) {
    case ToStringFormatUnsupported:
      WTF;
    case ToStringFormatLocaleTime:
      return is_24h_style ? "%H:%M:%S" : "%I:%M:%S %p";
    case ToStringFormatSecondNumeric:
    case ToStringFormatSecond2Digit:
      return "%S";
    case ToStringFormatMinuteNumeric:
    case ToStringFormatMinute2Digit:
      return "%M";
    case ToStringFormatHourNumeric:
    case ToStringFormatHour2Digit:
      return is_24h_style ? "%H" : "%I %p";
    case ToStringFormatLocaleDate:
      return "%x";
    case ToStringFormatDayNumeric:
    case ToStringFormatDay2Digit:
      return "%d";
    case ToStringFormatDayShort:
      return "%a";
    case ToStringFormatDayLong:
      return "%A";
    case ToStringFormatMonthNumeric:
    case ToStringFormatMonth2Digit:
      return "%m";
    case ToStringFormatMonthShort:
      return "%b";
    case ToStringFormatMonthLong:
      return "%B";
    case ToStringFormatYearNumeric:
      return "%Y";
    case ToStringFormatYear2Digit:
      return "%y";
    case ToStringFormatEmpty:
      return "";
  }
  return "";
}

static bool prv_strip_leading_zero(ToStringFormat format, bool is_24h_style) {
  switch (format) {
    case ToStringFormatLocaleTime:
      // %I adds leading zeros, for single digit hours. We don't want that for 12h
      return !is_24h_style;
    case ToStringFormatSecondNumeric:
    case ToStringFormatMinuteNumeric:
    case ToStringFormatHourNumeric:
    case ToStringFormatDayNumeric:
    case ToStringFormatMonthNumeric:
      return true;

    case ToStringFormatUnsupported:
    case ToStringFormatEmpty:
    case ToStringFormatSecond2Digit:
    case ToStringFormatMinute2Digit:
    case ToStringFormatHour2Digit:
    case ToStringFormatLocaleDate:
    case ToStringFormatDay2Digit:
    case ToStringFormatDayShort:
    case ToStringFormatDayLong:
    case ToStringFormatMonthShort:
    case ToStringFormatMonthLong:
    case ToStringFormatMonth2Digit:
      return false;
    case ToStringFormatYearNumeric:
    case ToStringFormatYear2Digit:
      // yes, we want to keep leading zeros in both cases for year as we control the format
      // exclusively via the strftime format
      return false;
  }
  return false;
}

static size_t prv_to_locale_buffer(jerry_value_t this_val,
                                   jerry_length_t argc, const jerry_value_t *argv,
                                   ToStringFormat default_format,
                                   ToStringFormat mask,
                                   char *buffer, size_t buffer_len,
                                   jerry_value_t *error) {
  JS_VAR locale = argc >= 1 ? jerry_acquire_value(argv[0]) : jerry_create_undefined();
  JS_VAR options = argc >= 2 ? jerry_acquire_value(argv[1]) : jerry_create_object();

  if (!prv_matches_system_locale(locale)) {
    if (error) {
      *error = rocky_error_argument_invalid("Unsupported locale");
    }
    return 0;
  }
  bool is_24h_style = clock_is_24h_style();
  const ToStringFormat format = prv_parse_to_string_format(options, default_format, mask,
                                                           &is_24h_style);
  if (format == ToStringFormatUnsupported) {
    if (error) {
      *error = rocky_error_argument_invalid("Unsupported options");
    }
    return 0;
  }

  struct tm tm;
  prv_tm_from_js_date(this_val, &tm);

  const char *strftime_format = prv_strftime_format(format, is_24h_style);
  const size_t str_len = strftime(buffer, buffer_len, strftime_format, &tm);

  const bool strip_leading_char = buffer[0] == '0' && prv_strip_leading_zero(format, is_24h_style);
  if (strip_leading_char) {
    memmove(buffer, buffer + 1, str_len);
  }
  return str_len;
}

static jerry_value_t prv_to_locale_time_or_date_string(jerry_value_t this_val,
                                                       jerry_length_t argc,
                                                       const jerry_value_t argv[],
                                                       ToStringFormat date_default_format,
                                                       ToStringFormat time_default_format) {
  // both, .toLocaleTimeString() and .toLocaleDateString() fall back to "<date>, <time>"
  // if clients specify options that are not part of time / date
  // similarly, .toLocaleString() falls back to "<date>, <time>" if no known option was specified
  // yes, in some code paths, this isn't the most-efficient but it's robust on the other hand

  // format date
  char date_buffer[BUFFER_LEN_DATE] = {0};
  jerry_value_t error = jerry_create_undefined();
  const size_t date_len = prv_to_locale_buffer(this_val, argc, argv,
                                               date_default_format, ToStringFormatDateMask,
                                               date_buffer, sizeof(date_buffer), &error);
  if (jerry_value_has_error_flag(error)) {
    return error;
  }

  // format time
  char time_buffer[BUFFER_LEN_TIME] = {0};
  const size_t time_len = prv_to_locale_buffer(this_val, argc, argv,
                                               time_default_format, ToStringFormatTimeMask,
                                               time_buffer, sizeof(time_buffer), &error);
  if (jerry_value_has_error_flag(error)) {
    return error;
  }

  // concatenate result
  char result_buffer[BUFFER_LEN_DATETIME] = {0};
  size_t remaining_buffer_size = sizeof(result_buffer);

  strncpy(result_buffer, date_buffer, sizeof(result_buffer));
  remaining_buffer_size -= date_len;

  if (date_len > 0 && time_len > 0) {
    strncat(result_buffer, ", ", remaining_buffer_size);
    remaining_buffer_size -= 2; // 2 = strlen(", ")
  }

  strncat(result_buffer, time_buffer, remaining_buffer_size);

  return jerry_create_string_utf8((jerry_char_t *)result_buffer);
}

JERRY_FUNCTION(prv_to_locale_time_string) {
  return prv_to_locale_time_or_date_string(this_val, argc, argv,
                                           ToStringFormatEmpty, ToStringFormatLocaleTime);
}

JERRY_FUNCTION(prv_to_locale_date_string) {
  return prv_to_locale_time_or_date_string(this_val, argc, argv,
                                           ToStringFormatLocaleDate, ToStringFormatEmpty);
}

JERRY_FUNCTION(prv_to_locale_string) {
  jerry_value_t error = jerry_create_undefined();
  char buffer[BUFFER_LEN_DATETIME] = {0};

  // we allow users to pick from any option to here
  const size_t total_len = prv_to_locale_buffer(this_val, argc, argv,
                                                ToStringFormatEmpty,
                                                ToStringFormatDateMask | ToStringFormatTimeMask,
                                                buffer, sizeof(buffer), &error);
  if (jerry_value_has_error_flag(error)) {
    return error;
  }

  if (total_len != 0) {
    // user picked options, so we formatted something into buffer
    return jerry_create_string_utf8((jerry_char_t *) buffer);
  }

  // if total_len == 0, nothing was formatted and we default to "<date>, <time>"
  return prv_to_locale_time_or_date_string(this_val, 0, NULL,
                                           ToStringFormatLocaleDate, ToStringFormatLocaleTime);
}

//
static void prv_rocky_add_date_functions(jerry_value_t global) {
  JS_VAR date_constructor = jerry_get_object_field(global, "Date");
  JS_VAR date_prototype = jerry_get_object_field(date_constructor, "prototype");
  JS_VAR locale_time_string = jerry_create_external_function(prv_to_locale_time_string);
  jerry_set_object_field(date_prototype, ROCKY_DATE_TOLOCALETIMESTRING, locale_time_string);
  JS_VAR locale_date_string = jerry_create_external_function(prv_to_locale_date_string);
  jerry_set_object_field(date_prototype, ROCKY_DATE_TOLOCALEDATESTRING, locale_date_string);
  JS_VAR locale_string = jerry_create_external_function(prv_to_locale_string);
  jerry_set_object_field(date_prototype, ROCKY_DATE_TOLOCALESTRING, locale_string);
}

static void prv_init(void) {
  JS_VAR global = jerry_get_global_object();
  prv_rocky_add_date_functions(global);
}

const RockyGlobalAPI DATETIME_APIS = {
  .init = prv_init,
};
