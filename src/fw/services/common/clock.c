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

#include "services/common/clock.h"

#include "console/prompt.h"
#include "drivers/rtc.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "services/common/comm_session/session.h"
#include "services/common/i18n/i18n.h"
#include "services/common/regular_timer.h"
#include "services/normal/alarms/alarm.h"
#include "services/normal/timezone_database.h"
#include "services/normal/wakeup.h"
#include "shell/prefs.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/net.h"
#include "util/size.h"
#include "util/string.h"

#include <stdio.h>

// NOTE: There are RECOVERY_FW ifdefs in this file because PRF does not have
// timezone support

#define UNKNOWN_TIMEZONE_ID (-1)

static const uint16_t protocol_time_endpoint_id = 11;

static RegularTimerInfo s_dst_checker;

//! Migrations for services that use timezone info
static void prv_migrate_timezone_info(int utc_diff) {
#ifndef RECOVERY_FW
  // Since all migrations are to UTC time, we are passed the relative offset from UTC
  notifications_migrate_timezone(utc_diff);
  wakeup_migrate_timezone(utc_diff);
#endif
}

static time_t prv_migrate_local_time_to_UTC(time_t local_time) {
  return time_local_to_utc(local_time);
}

// Should only called by prv_update_time_info_and_generate_event()!
static void prv_handle_timezone_set(TimezoneInfo *tz_info) {
  // Check if the timezone is set before setting it.  This ensures that this
  // will only be false once as needed for us to migrate.
  bool timezone_migration_needed = !clock_is_timezone_set();

  time_util_update_timezone(tz_info);

  // Update the RTC registers with the latest timezone info
  rtc_set_timezone(tz_info);

  // We are pivoting to UTC from localtime for the first time
  if (timezone_migration_needed) {
    time_t t = rtc_get_time();
    t = prv_migrate_local_time_to_UTC(t);
    rtc_sanitize_time_t(&t);
    rtc_set_time(t);  // Pivot RTC from localtime to UTC
    prv_migrate_timezone_info(tz_info->tm_gmtoff);
  }
}

typedef struct PACKED {
// This struct is packed because it mirrors the endpoint definition:
// https://pebbletechnology.atlassian.net/wiki/pages/viewpage.action?pageId=491698#PebbleProtocol(BluetoothSerial)-0xb(11)-Time/Clock(bigendian)
  time_t utc_time;                           // UTC timestamp
  int16_t utc_offset_min;                    // local timestamp - UTC timestamp in mins
  int8_t region_name_len;                    // timezone name length
  char region_name[TIMEZONE_NAME_LENGTH];  // timezone name string
} TimezoneCBData;

#ifndef UNITTEST
_Static_assert(sizeof(time_t) == 4, "Sizeof time_t does not match endpoint definition");
#endif

#if !defined(RECOVERY_FW)
static time_t prv_clock_dstrule_to_timestamp(
    bool is_end, const TimezoneInfo *tz_info, const TimezoneDSTRule *rule, int year) {

  struct tm time_tm = {
    .tm_min = rule->minute,
    .tm_hour = rule->hour,
    .tm_mday = rule->mday,
    .tm_mon = rule->month,
    .tm_year = year,
    .tm_gmtoff = 0,
    .tm_isdst = 0,
  };
  // A few countries actually have their DST rule on the midnight AFTER a day
  // This is subtly different from the midnight OF a day.
  if (rule->hour >= HOURS_PER_DAY) {
    time_tm.tm_hour %= HOURS_PER_DAY;
  }
  // Brazil delays DST end by one week every 3 years for elections
  if (tz_info->dst_id == DSTID_BRAZIL && (((TM_YEAR_ORIGIN + year) % 3) == 2) && is_end) {
    time_tm.tm_mday += DAYS_PER_WEEK;
  }
  time_t uxtime = mktime(&time_tm);
  gmtime_r(&uxtime, &time_tm);

  for (int i = 0; i < DAYS_PER_WEEK; i++) { // max is DAYS_PER_WEEK to find a day_of_week
    // we also have to check month here, as leap-year case puts us 1 day past feb
#define DSTRULE_WDAY_ANY (255)
    if ((time_tm.tm_wday == rule->wday || rule->wday == DSTRULE_WDAY_ANY) &&
        time_tm.tm_mon == rule->month) {
      break;
    }
    time_tm.tm_mday += (rule->flag & TIMEZONE_FLAG_DAY_DECREMENT) ? -1 : 1;
    uxtime = mktime(&time_tm);
    gmtime_r(&uxtime, &time_tm);
  }

  if (rule->hour >= HOURS_PER_DAY) {
    time_tm.tm_mday += rule->hour / HOURS_PER_DAY;
    uxtime = mktime(&time_tm);
    gmtime_r(&uxtime, &time_tm);
  }

  if (rule->flag & TIMEZONE_FLAG_STANDARD_TIME) { // Standard time (not wall time)
    time_tm.tm_gmtoff = tz_info->tm_gmtoff;
    time_tm.tm_isdst = 0;
  } else if (rule->flag & TIMEZONE_FLAG_UTC_TIME) { // UTC
    time_tm.tm_gmtoff = 0;
    time_tm.tm_isdst = 0;
  } else { // Wall time
    time_tm.tm_gmtoff = tz_info->tm_gmtoff;
    time_tm.tm_isdst = is_end;
  }
  // Lord Howe Island has a half-hour DST
  if (tz_info->dst_id == DSTID_LORDHOWE) {
    uxtime -= time_tm.tm_isdst ? SECONDS_PER_HOUR / 2 : 0;
  } else {
    uxtime -= time_tm.tm_isdst ? SECONDS_PER_HOUR : 0;
  }
  uxtime -= time_tm.tm_gmtoff;
  return uxtime;
}
#endif // RECOVERY_FW

T_STATIC void prv_update_dstrule_timestamps_by_dstzone_id(TimezoneInfo *tz_info, time_t utc_time) {
  if (tz_info->dst_id == 0) {
    tz_info->dst_start = 0;
    tz_info->dst_end = 0;
    return;
  }

#if defined(RECOVERY_FW)
  return;
#else

  // Load the pair of TimezoneDSTRule objects from the timezone database

  TimezoneDSTRule dst_rule_begin;
  TimezoneDSTRule dst_rule_end;

  if (!timezone_database_load_dst_rule(tz_info->dst_id, &dst_rule_begin, &dst_rule_end)) {
    // No DST rule or invalid DST ID. Either way just clear the DST information.
    tz_info->dst_start = 0;
    tz_info->dst_end = 0;
    return;
  }

  struct tm current_tm;
  gmtime_r(&utc_time, &current_tm);

  // Calculate the timestamps of the start and ends of DST for the previous year, the current
  // year, and the next year.

#define DST_YEARS_RANGE 3
#define DST_YEARS_OFFSET 1
  time_t dst_start_stamps[DST_YEARS_RANGE];
  time_t dst_end_stamps[DST_YEARS_RANGE];
  for (int i = 0; i < DST_YEARS_RANGE; i++) {
    const int year = current_tm.tm_year + (i - DST_YEARS_OFFSET);

    dst_start_stamps[i] =
        prv_clock_dstrule_to_timestamp(false, tz_info, &dst_rule_begin, year);
    dst_end_stamps[i] =
        prv_clock_dstrule_to_timestamp(true, tz_info, &dst_rule_end, year);
  }

  //  Figure out which timestamps are relevant to us

  int start_idx = DST_YEARS_OFFSET;
  int end_idx = DST_YEARS_OFFSET;

  if (dst_start_stamps[start_idx] > dst_end_stamps[end_idx]) {
    start_idx--;
  }

  if (dst_start_stamps[start_idx] < utc_time && dst_end_stamps[end_idx] < utc_time) {
    start_idx++;
    end_idx++;
  }

  tz_info->dst_start = dst_start_stamps[start_idx];
  tz_info->dst_end = dst_end_stamps[end_idx];

#endif // RECOVERY_FW
}

static void prv_clock_get_timezone_info_from_region_id(
    int16_t region_id, time_t utc_time, TimezoneInfo *tz_info) {

#ifdef RECOVERY_FW
  *tz_info = (TimezoneInfo) { .dst_id = 0 };
#else
  timezone_database_load_region_info(region_id, tz_info);
  prv_update_dstrule_timestamps_by_dstzone_id(tz_info, utc_time);
#endif
}

static TimezoneInfo prv_get_timezone_info_from_data(TimezoneCBData *tz_data) {
  int region_id = -1;

  if (tz_data->region_name_len) {
    region_id = timezone_database_find_region_by_name(tz_data->region_name,
                                                      tz_data->region_name_len);
  }

  if (region_id != -1) {
    // We have a valid region!
    TimezoneInfo tz_info;
    prv_clock_get_timezone_info_from_region_id(region_id, tz_data->utc_time, &tz_info);
    return tz_info;
  }

  // Else, we couldn't find the specified timezone.
#ifndef RECOVERY_FW
  TimezoneInfo tz_info = {
    .dst_id = 0,
    .timezone_id = UNKNOWN_TIMEZONE_ID,
    .tm_gmtoff = tz_data->utc_offset_min * SECONDS_PER_MINUTE,
    .dst_start = 0,
    .dst_end = 0,
  };

  // I was hoping to fill the name with something like UTC-10 or UTC+4.25 but we only get 5 chars
  strncpy(tz_info.tm_zone, "N/A", TZ_LEN - 1);
  return tz_info;
#else
  return (TimezoneInfo) {};
#endif
}

// This routine is solely responsible for setting the time and/or timezone for
// the system RTC. After the time is changed, it generates an event for
// consumers interested in time changes
T_STATIC void prv_update_time_info_and_generate_event(time_t *t, TimezoneInfo *tz_info) {
  int orig_gmt_offset = time_get_gmtoffset();
  time_t orig_utc_time = rtc_get_time();
  TimezoneInfo tz_adjust_info = {{0}};

  if (clock_is_timezone_set()) { // We'll need to update timezone stamps.
    time_t tz_adjust_time;
    // Get the time that we need to adjust for.
    if (t) {
      tz_adjust_time = *t;
    } else {
      tz_adjust_time = orig_utc_time;
    }
    if (tz_info) { // Adjust the DST rule timestamps of the provided tz_info
      prv_update_dstrule_timestamps_by_dstzone_id(tz_info, tz_adjust_time);
    } else if (clock_get_timezone_region_id() != UNKNOWN_TIMEZONE_ID) {
      // If we have a timezone _actually_ set, update our own.
      int region_id = clock_get_timezone_region_id();
      prv_clock_get_timezone_info_from_region_id(region_id, tz_adjust_time, &tz_adjust_info);
      tz_info = &tz_adjust_info; // We need to set timezone info so point to the new info.
    }
  }

  // Note: update the timezone before setting the utc time. (If we set the utc
  // time first we could wind up accidentally applying the timezone correction
  // to that value in the case where no timezone data previously existed
  // ... such as a migration from legacy firmware or after the RTC backup
  // domain had completely powered down)
  if (tz_info) {
    prv_handle_timezone_set(tz_info);
  }

  if (t) {
    rtc_set_time(*t);
  }

  int new_gmt_offset = time_get_gmtoffset();
  int new_utc_time = rtc_get_time();

  PebbleEvent e = {
    .type = PEBBLE_SET_TIME_EVENT,
    .set_time_info = {
      .utc_time_delta = new_utc_time - orig_utc_time,
      .gmt_offset_delta = new_gmt_offset - orig_gmt_offset,
      .dst_changed = false,
    }
  };
  event_put(&e);
}

static void prv_handle_set_utc_and_timezone_msg(TimezoneCBData *tz_data) {
  tz_data->utc_time = ntohl(tz_data->utc_time);
  tz_data->utc_offset_min = ntohs(tz_data->utc_offset_min);

  const char *region_name = tz_data->region_name;
  if (tz_data->region_name_len == 0) {
    region_name = "[N/A]";
  }
  PBL_LOG(LOG_LEVEL_DEBUG, "set_timezone utc_time: %u offset: %d region_name: %s",
          (int) tz_data->utc_time, (int) tz_data->utc_offset_min, region_name);

  TimezoneInfo tz_info = prv_get_timezone_info_from_data(tz_data);
  shell_prefs_set_automatic_timezone_id(tz_info.timezone_id);
  if (clock_timezone_source_is_manual()) {
    prv_update_time_info_and_generate_event(&tz_data->utc_time, NULL);
  } else {
    prv_update_time_info_and_generate_event(&tz_data->utc_time, &tz_info);
  }
}

static void prv_handle_set_time_msg(time_t new_time) {
  PBL_LOG(LOG_LEVEL_WARNING, "Mobile app calling deprecated API, time = %d",
          (int)new_time);
  if (clock_is_timezone_set()) {
    new_time = prv_migrate_local_time_to_UTC(new_time);
  }

  prv_update_time_info_and_generate_event(&new_time, NULL);
}

void clock_protocol_msg_callback(CommSession *session, const uint8_t* data, unsigned int length) {
  char sub_command = *data++;

  switch (sub_command) {
    // Get time request:
    case 0x00: {
      time_t t = rtc_get_time();

      // Send Get time response (0x01):
      const unsigned int response_buffer_length = 1 + 4;
      uint8_t response_buffer[response_buffer_length];

      response_buffer[0] = 0x01;

      *(uint32_t*)(response_buffer + 1) = htonl(t);

      comm_session_send_data(session, protocol_time_endpoint_id, response_buffer,
                             response_buffer_length, COMM_SESSION_DEFAULT_TIMEOUT);
      PBL_LOG_VERBOSE("protocol_time_callback called, responding with current time: %"PRIu32,
                      (uint32_t)t);
      break;
    }
    // Set time:
    case 0x02: {
      time_t new_time = ntohl(*(uint32_t*)data);
      prv_handle_set_time_msg(new_time);
      break;
    }
    // Set timezone:
    case 0x03: {
      // Verify that the message length is correct
      const size_t header_size = offsetof(TimezoneCBData, region_name);
      const uint8_t *timezone_length_ptr = data + offsetof(TimezoneCBData, region_name_len);
      if (length != (sizeof(uint8_t) + header_size + *timezone_length_ptr)) {
        PBL_LOG(LOG_LEVEL_WARNING, "Set timezone message invalid length");
        return;
      }

      TimezoneCBData *timezone_data = (TimezoneCBData *)data;
      prv_handle_set_utc_and_timezone_msg(timezone_data);
      break;
    }
    default:
      PBL_LOG(LOG_LEVEL_WARNING, "Invalid message received. First byte is %u", data[0]);
      break;
  }
}

// TODO: Using a regular timer is pretty gross...
static void prv_watch_dst(void* user) {
  const bool was_dst = (bool)user;
  const bool is_dst = time_get_isdst(rtc_get_time());
  if (is_dst != was_dst) {
    PebbleEvent e = {
      .type = PEBBLE_SET_TIME_EVENT,
      .set_time_info = {
        .utc_time_delta = 0,
        .gmt_offset_delta = 0,
        .dst_changed = true,
      }
    };
    event_put(&e);
    s_dst_checker.cb_data = (void*)is_dst;
  }
}

void clock_init(void) {
  if (clock_is_timezone_set()) {
    TimezoneInfo tz_info;
    rtc_get_timezone(&tz_info);
    time_util_update_timezone(&tz_info);
  }
  // TODO: Using a regular timer is pretty gross...
  s_dst_checker = (RegularTimerInfo) {
    .cb = prv_watch_dst,
    .cb_data = (void*)time_get_isdst(rtc_get_time()),
  };
  regular_timer_add_seconds_callback(&s_dst_checker);
}

void clock_get_time_tm(struct tm* time_tm) {
  rtc_get_time_tm(time_tm);
}

size_t clock_format_time(char *buffer, uint8_t size, int16_t hours, int16_t minutes,
                         bool add_space) {
  if (size == 0 || buffer == NULL) {
    return 0;
  }

  bool is24h = clock_is_24h_style();
  const char *format;

  // [INTL] you want to have layout resources that specify time formatting,
  // and be able to set a default one for each locale.
  if (is24h) {
    format = "%u:%02u";
  } else {
    if (hours < 12) {
      format = add_space ? "%u:%02u AM" : "%u:%02uAM";
    } else {
      format = add_space ? "%u:%02u PM" : "%u:%02uPM";
    }
  }
  return sniprintf(buffer, size, format, time_util_get_num_hours(hours, is24h), minutes);
}

size_t clock_copy_time_string_timestamp(char *buffer, uint8_t size, time_t timestamp) {
  struct tm time;
  sys_localtime_r(&timestamp, &time);
  return clock_format_time(buffer, size, time.tm_hour, time.tm_min, true);
}

void clock_copy_time_string(char *buffer, uint8_t size) {
  time_t t = sys_get_time();
  clock_copy_time_string_timestamp(buffer, size, t);
}

static size_t prv_format_time(char *buffer, int buf_size, const char *format, time_t timestamp) {
  struct tm time_tm;
  localtime_r(&timestamp, &time_tm);
  const size_t ret_val = strftime(buffer, buf_size, i18n_get(format, buffer), &time_tm);
  i18n_free(format, buffer);
  return ret_val;
}

size_t clock_get_time_number(char *number_buffer, size_t number_buffer_size, time_t timestamp) {
  const size_t written =
      prv_format_time(number_buffer, number_buffer_size,
                      (clock_is_24h_style() ? i18n_noop("%R") : i18n_noop("%l:%M")), timestamp);
  const char *number_buffer_ptr = string_strip_leading_whitespace(number_buffer);
  memmove(number_buffer,
          number_buffer_ptr,
          number_buffer_size - (number_buffer_ptr - number_buffer));
  return written - (number_buffer_ptr - number_buffer);
}

size_t clock_get_time_word(char *buffer, size_t buffer_size, time_t timestamp) {
  if (clock_is_24h_style()) {
    buffer[0] = '\0';
    return 0;
  } else {
    return prv_format_time(buffer, buffer_size, i18n_noop("%p"), timestamp);
  }
}

static void prv_copy_time_string_timestamp(char *number_buffer, uint8_t number_buffer_size,
    char *word_buffer, uint8_t word_buffer_size, time_t timestamp) {
  clock_get_time_number(number_buffer, number_buffer_size, timestamp);
  clock_get_time_word(word_buffer, word_buffer_size, timestamp);
}

static void prv_get_relative_all_day_string(char *buffer, int buffer_size, time_t timestamp) {
  time_t today = time_util_get_midnight_of(rtc_get_time());
  if (time_util_get_midnight_of(timestamp) == today) {
    i18n_get_with_buffer("Today", buffer, buffer_size);
  } else {
    i18n_get_with_buffer("All day", buffer, buffer_size);
  }
}

static void prv_copy_relative_time_string(char *number_buffer, uint8_t number_buffer_size,
    char *word_buffer, uint8_t word_buffer_size, time_t timestamp, time_t end_time) {
  time_t now = rtc_get_time();
  // average without overflows since time_t might be signed and now ~1.4 billion, so 2*now > INT_MAX
  time_t midtime = timestamp / 2 + end_time / 2;
  if (midtime > now) { // future
    time_t difference = timestamp - now;
    if (timestamp < now || difference < SECONDS_PER_MINUTE) {
      i18n_get_with_buffer("Now", word_buffer, word_buffer_size);
      strncpy(number_buffer, "", number_buffer_size);
    } else if (difference <= SECONDS_PER_HOUR) {
      snprintf(number_buffer, number_buffer_size, "%ld", difference / SECONDS_PER_MINUTE);
      i18n_get_with_buffer(" MIN. TO", word_buffer, word_buffer_size);
    }  else {
      prv_copy_time_string_timestamp(number_buffer, number_buffer_size, word_buffer,
        word_buffer_size, timestamp);
    }
  } else { // past
    time_t difference = now - timestamp;
    if (now < timestamp || difference < SECONDS_PER_MINUTE) {
      i18n_get_with_buffer("Now", word_buffer, word_buffer_size);
      strncpy(number_buffer, "", number_buffer_size);
    } else {
      prv_copy_time_string_timestamp(number_buffer, number_buffer_size, word_buffer,
        word_buffer_size, timestamp);
    }
  }
}

// number: 10
// word: min to
void clock_get_event_relative_time_string(char *number_buffer, int number_buffer_size,
    char *word_buffer, int word_buffer_size, time_t timestamp, uint16_t duration,
    time_t current_day, bool all_day) {
  time_t end_time = timestamp + duration * SECONDS_PER_MINUTE;
  if (all_day) {
    // all day event, multiday or single day
    prv_get_relative_all_day_string(word_buffer, word_buffer_size, current_day);
    strncpy(number_buffer, "", number_buffer_size);
  } else if (time_util_get_midnight_of(timestamp) == current_day) {
    // first day of multiday event or only day
    prv_copy_relative_time_string(number_buffer, number_buffer_size, word_buffer,
        word_buffer_size, timestamp, end_time);
  } else if (time_util_get_midnight_of(end_time) == current_day) {
    // last day of multiday event
    prv_copy_relative_time_string(number_buffer, number_buffer_size, word_buffer,
        word_buffer_size, end_time, end_time);
  } else {
    // middle day of non-all day multiday event
    prv_get_relative_all_day_string(word_buffer, word_buffer_size, current_day);
    strncpy(number_buffer, "", number_buffer_size);
  }
}

DEFINE_SYSCALL(bool, clock_is_24h_style, void) {
  return shell_prefs_get_clock_24h_style();
}

void clock_set_24h_style(bool is_24h_style) {
  shell_prefs_set_clock_24h_style(is_24h_style);
}

DEFINE_SYSCALL(bool, clock_is_timezone_set, void) {
  return rtc_is_timezone_set();  // If timezone abbr isn't set
}

bool clock_timezone_source_is_manual(void) {
  return shell_prefs_is_timezone_source_manual();
}

void clock_set_manual_timezone_source(bool manual) {
  shell_prefs_set_timezone_source_manual(manual);
}

time_t clock_to_timestamp(WeekDay day, int hour, int minute) {
  time_t t = sys_get_time();
  struct tm cal;
  sys_localtime_r(&t, &cal);

  if (day != TODAY) {
    // If difference between WeekDay and current day, always in the future
    day -= 1;  // cal_wday is 0-6
    int day_offset = (day > cal.tm_wday) ? (day - cal.tm_wday) : (day - cal.tm_wday + 7);
    cal.tm_mday += day_offset;  // normalized by mktime
  } else if ((hour < cal.tm_hour) || (hour == cal.tm_hour && minute <= cal.tm_min)){
    // Always return a future timestamp, so if day was today, and
    // minutes and hours already occurred, just make it tomorrow
    cal.tm_mday++; // normalized by mktime
  }

  cal.tm_hour = hour;
  cal.tm_min = minute;

  return mktime(&cal);
}

void command_timezone_clear(void) {
  rtc_timezone_clear();
}

void command_get_time(void) {
  char buffer[80];
  char time_buffer[26];
  prompt_send_response_fmt(buffer, 80, "Time is now <%s>", rtc_get_time_string(time_buffer));
}

void command_set_time(const char *arg) {
  time_t t = atoi(arg);
  if (t == 0) {
    prompt_send_response("Invalid length");
    return;
  }

  prv_update_time_info_and_generate_event(&t, NULL);

  char buffer[80];
  char time_buffer[26];
  prompt_send_response_fmt(buffer, 80, "Time is now <%s>", rtc_get_time_string(time_buffer));
}

void clock_get_timezone_region(char* region_name, const size_t buffer_size) {
  if (!region_name) {
    return;
  }

  if (clock_is_timezone_set()) {
    int region_id = clock_get_timezone_region_id();
    if (region_id != UNKNOWN_TIMEZONE_ID) {
      timezone_database_load_region_name(region_id, region_name);
    } else {
      // Show something like UTC-4 or UTC-10.25
      // This will typically happen in the emulator when we know the UTC offset, but not
      // the timezone (fallback case).
      int gmt_offset_m = time_get_gmtoffset() / SECONDS_PER_MINUTE;
      int hour_offset = gmt_offset_m / MINUTES_PER_HOUR;

      char min_buf[4] = {0};
      int min_offset_percent = ((ABS(gmt_offset_m) % MINUTES_PER_HOUR) * 100) / MINUTES_PER_HOUR;
      if (min_offset_percent) {
        snprintf(min_buf, sizeof(min_buf), ".%d", min_offset_percent);
      }
      snprintf(region_name, buffer_size, "UTC%+d%s", hour_offset, min_buf);
    }
  } else {
    strncpy(region_name, "---", buffer_size);
  }
}

int16_t clock_get_timezone_region_id(void) {
  return rtc_get_timezone_id();
}

void clock_set_timezone_by_region_id(uint16_t region_id) {
  TimezoneInfo tz_info;
  prv_clock_get_timezone_info_from_region_id(region_id, rtc_get_time(), &tz_info);
  prv_update_time_info_and_generate_event(NULL, &tz_info);
}

void clock_get_friendly_date(char *buffer, int buf_size, time_t timestamp) {
  const time_t now = rtc_get_time();

  const time_t midnight = time_util_get_midnight_of(timestamp);
  const time_t today_midnight = time_util_get_midnight_of(now);

  if (midnight == today_midnight) {
    i18n_get_with_buffer("Today", buffer, buf_size);
  } else if (midnight == (today_midnight - SECONDS_PER_DAY)) {
    i18n_get_with_buffer("Yesterday", buffer, buf_size);
  } else if (midnight == (today_midnight + SECONDS_PER_DAY)) {
    i18n_get_with_buffer("Tomorrow", buffer, buf_size);
  } else if (midnight <= (today_midnight + (5 * SECONDS_PER_DAY))) {
    // Use weekday name up to 5 days in the future, aka "Sunday"
    prv_format_time(buffer, buf_size, i18n_noop("%A"), timestamp);
  } else {
    // Otherwise use "Month Day", aka "June 21"
    prv_format_time(buffer, buf_size, i18n_noop("%B %d"), timestamp);
  }
}

enum {
  RoundTypeHalfUp,
  RoundTypeHalfDown,
  RoundTypeAlwaysUp,
  RoundTypeAlwaysDown,
};

static time_t prv_round(time_t round_me, time_t multiple, int round_type) {
  switch (round_type) {
    case RoundTypeHalfDown:
      return ((round_me + multiple / 2 - 1) / multiple) * multiple;
    case RoundTypeAlwaysUp:
      return ((round_me + multiple - 1) / multiple) * multiple;
    case RoundTypeAlwaysDown:
      return (round_me / multiple) * multiple;
    case RoundTypeHalfUp:
    default:
      return ((round_me + multiple / 2) / multiple) * multiple;
  }
}

enum {
  FullStyleLower12h,
  FullStyleCapital12h,
  FullStyleLower24h,
  FullStyleCapital24h,
};

static void prv_clock_get_full_relative_time(char *buffer, int buf_size, time_t timestamp,
                                             bool capitalized, bool with_fulltime) {
  time_t today_midnight = time_util_get_midnight_of(rtc_get_time());
  time_t timestamp_midnight = time_util_get_midnight_of(timestamp);
  time_t yesterday_midnight = time_util_get_midnight_of(rtc_get_time() - SECONDS_PER_DAY);
  time_t last_week_midnight = time_util_get_midnight_of(rtc_get_time() - SECONDS_PER_WEEK);
  time_t next_week_midnight = time_util_get_midnight_of(rtc_get_time() + SECONDS_PER_WEEK);

  const char *time_fmt = NULL;
  int style;
  if (clock_is_24h_style()) {
    if (capitalized) {
      style = FullStyleCapital24h;
    } else {
      style = FullStyleLower24h;
    }
  } else {
    if (capitalized) {
      style = FullStyleCapital12h;
    } else {
      style = FullStyleLower12h;
    }
  }

  if (timestamp_midnight == today_midnight) {
    switch (style) {
      case FullStyleLower12h:
      case FullStyleCapital12h:
        time_fmt = i18n_noop("%l:%M %p");
        break;
      case FullStyleLower24h:
      case FullStyleCapital24h:
        time_fmt = i18n_noop("%R");
        break;
    }
  } else if (timestamp_midnight == yesterday_midnight) {
    switch (style) {
      case FullStyleLower12h:
      case FullStyleCapital12h:
        if (with_fulltime) {
          time_fmt = i18n_noop("Yesterday, %l:%M %p");
        } else {
          time_fmt = i18n_noop("Yesterday");
        }
        break;
      case FullStyleLower24h:
      case FullStyleCapital24h:
        if (with_fulltime) {
          time_fmt = i18n_noop("Yesterday, %R");
        } else {
          time_fmt = i18n_noop("Yesterday");
        }
        break;
    }
  } else if (timestamp_midnight <= last_week_midnight || timestamp_midnight >= next_week_midnight) {
    switch (style) {
      case FullStyleLower12h:
      case FullStyleCapital12h:
        if (with_fulltime) {
          time_fmt = i18n_noop("%b %e, %l:%M %p");
        } else {
          time_fmt = i18n_noop("%B %e");
        }
        break;
      case FullStyleLower24h:
      case FullStyleCapital24h:
        if (with_fulltime) {
          time_fmt = i18n_noop("%b %e, %R");
        } else {
          time_fmt = i18n_noop("%B %e");
        }
        break;
    }
  } else {
    switch (style) {
      case FullStyleLower12h:
      case FullStyleCapital12h:
        if (with_fulltime) {
          time_fmt = i18n_noop("%a, %l:%M %p");
        } else {
          time_fmt = i18n_noop("%A");
        }
        break;
      case FullStyleLower24h:
      case FullStyleCapital24h:
        if (with_fulltime) {
          time_fmt = i18n_noop("%a, %R");
        } else {
          time_fmt = i18n_noop("%A");
        }
        break;
    }
  }
  prv_format_time(buffer, buf_size, time_fmt, timestamp);
}

static void prv_clock_get_relative_time_string(char *buffer, int buf_size, time_t timestamp,
                                               bool capitalized, int max_relative_hrs,
                                               bool with_fulltime) {
  time_t difference = rtc_get_time() - timestamp;

  time_t today_midnight = time_util_get_midnight_of(rtc_get_time());
  time_t timestamp_midnight = time_util_get_midnight_of(timestamp);

  if (today_midnight != timestamp_midnight) {
    prv_clock_get_full_relative_time(buffer, buf_size, timestamp, capitalized, with_fulltime);

  } else if (difference >= (SECONDS_PER_HOUR * max_relative_hrs)) {
    prv_clock_get_full_relative_time(buffer, buf_size, timestamp, capitalized, with_fulltime);

  } else if (difference >= SECONDS_PER_HOUR) {
    const int num_hrs =
        prv_round(difference, SECONDS_PER_HOUR, RoundTypeHalfUp) / SECONDS_PER_HOUR;

    const char *str_fmt;
    if (capitalized) {
      str_fmt = i18n_noop("%lu H AGO");
    } else if (num_hrs == 1) {
      str_fmt = i18n_noop("An hour ago");
    } else {
      str_fmt = i18n_noop("%lu hours ago");
    }
    snprintf(buffer, buf_size, i18n_get(str_fmt, buffer), num_hrs);

  } else if (difference >= SECONDS_PER_MINUTE) {
    const int num_minutes =
        prv_round(difference, SECONDS_PER_MINUTE, RoundTypeAlwaysDown) / SECONDS_PER_MINUTE;

    const char *str_fmt;
    if (capitalized) {
      str_fmt = i18n_noop("%lu MIN AGO");
    } else if (num_minutes == 1) {
      str_fmt = i18n_noop("%lu minute ago");
    } else {
      str_fmt = i18n_noop("%lu minutes ago");
    }
    snprintf(buffer, buf_size, i18n_get(str_fmt, buffer), num_minutes);

  } else if (difference >= 0) {
    strncpy(buffer, capitalized ? i18n_get("NOW", buffer) : i18n_get("Now", buffer), buf_size);

  } else if (difference >= -(SECONDS_PER_HOUR - SECONDS_PER_MINUTE)) {
    const int num_minutes =
        prv_round(-difference, SECONDS_PER_MINUTE, RoundTypeAlwaysUp) / SECONDS_PER_MINUTE;

    const char *str_fmt;
    if (capitalized) {
      str_fmt = i18n_noop("IN %lu MIN");
    } else if (num_minutes == 1) {
      str_fmt = i18n_noop("In %lu minute");
    } else {
      str_fmt = i18n_noop("In %lu minutes");
    }
    snprintf(buffer, buf_size, i18n_get(str_fmt, buffer), num_minutes);

  } else if (difference >= -(SECONDS_PER_HOUR * max_relative_hrs)) {
    const int num_hrs =
        prv_round(-difference, SECONDS_PER_HOUR, RoundTypeHalfDown) / SECONDS_PER_HOUR;

    const char *str_fmt;
    if (capitalized) {
      str_fmt = i18n_noop("IN %lu H");
    } else if (num_hrs == 1) {
      str_fmt = i18n_noop("In %lu hour");
    } else {
      str_fmt = i18n_noop("In %lu hours");
    }
    snprintf(buffer, buf_size, i18n_get(str_fmt, buffer), num_hrs);

  } else {
    prv_clock_get_full_relative_time(buffer, buf_size, timestamp, capitalized, with_fulltime);
  }
  i18n_free_all(buffer);
}

size_t clock_get_date(char *buffer, int buf_size, time_t timestamp) {
  return prv_format_time(buffer, buf_size, i18n_noop("%m/%d"), timestamp);
}

size_t clock_get_day_date(char *buffer, int buf_size, time_t timestamp) {
  return prv_format_time(buffer, buf_size, i18n_noop("%d"), timestamp);
}

static size_t prv_clock_get_month_named_date(char *buffer, size_t buffer_size, time_t timestamp,
                                             bool abbrev) {
  const char *format = abbrev ? i18n_noop("%b ") : i18n_noop("%B ");
  const size_t month_size = prv_format_time(buffer, buffer_size, format, timestamp);
  char *day_buffer = buffer + month_size;
  const size_t day_buffer_size = buffer_size - month_size;
  size_t day_size = prv_format_time(day_buffer, day_buffer_size, i18n_noop("%e"), timestamp);
  const char *day_buffer_ptr = string_strip_leading_whitespace(day_buffer);
  memmove(day_buffer, day_buffer_ptr, day_buffer_size - (day_buffer_ptr - day_buffer));
  return month_size + day_size;
}

size_t clock_get_month_named_date(char *buffer, size_t buffer_size, time_t timestamp) {
  const bool abbrev = false;
  return prv_clock_get_month_named_date(buffer, buffer_size, timestamp, abbrev);
}

size_t clock_get_month_named_abbrev_date(char *buffer, size_t buffer_size, time_t timestamp) {
  const bool abbrev = true;
  return prv_clock_get_month_named_date(buffer, buffer_size, timestamp, abbrev);
}

void clock_get_since_time(char *buffer, int buf_size, time_t timestamp) {
  const time_t now = rtc_get_time();
  const time_t clamped_timestamp = MIN(now, timestamp);
  prv_clock_get_relative_time_string(buffer, buf_size, clamped_timestamp, false, HOURS_PER_DAY,
                                     true);
}

void clock_get_until_time(char *buffer, int buf_size, time_t timestamp,
                          int max_relative_hrs) {
  prv_clock_get_relative_time_string(buffer, buf_size, timestamp, false, max_relative_hrs, true);
}

void clock_get_until_time_capitalized(char *buffer, int buf_size, time_t timestamp,
                                      int max_relative_hrs) {
  prv_clock_get_relative_time_string(buffer, buf_size, timestamp, true, max_relative_hrs, true);
}

void clock_get_until_time_without_fulltime(char *buffer, int buf_size, time_t timestamp,
                                           int max_relative_hrs) {
  prv_clock_get_relative_time_string(buffer, buf_size, timestamp, true, max_relative_hrs, false);
}

DEFINE_SYSCALL(void, sys_clock_get_timezone, char *timezone, const size_t buffer_size) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(timezone, TIMEZONE_NAME_LENGTH);
  }
  clock_get_timezone_region(timezone, buffer_size);
}

typedef struct daypart_message {
  const uint32_t hour_offset;  // hours from 12am of current day
  const char* const message;  // text containing daypart
} daypart_message;

static const daypart_message daypart_messages[] = {
  {0,  i18n_noop("this morning")},  // anything before 12pm of the current day
  {12, i18n_noop("this afternoon")},  // 12pm today
  {18, i18n_noop("this evening")},  // 6pm today
  {21, i18n_noop("tonight")}, // 9pm today
  {33, i18n_noop("tomorrow morning")},  // 9am tomorrow
  {36, i18n_noop("tomorrow afternoon")},  // 12pm tomorrow
  {42, i18n_noop("tomorrow evening")},  // 6pm tomorrow
  {45, i18n_noop("tomorrow night")},  // 9pm tomorrow
  {57, i18n_noop("the day after tomorrow")}, // starting 9am 2 days from now
  {72, i18n_noop("the day after tomorrow")}, // ends midnight 2 days from now
  {73, i18n_noop("the foreseeable future")},  // Catchall for beyond 3 days
};

//! Daypart string is used internally for battery popups
//! and is a minimum threshold, ie. "Powered 'til at least"...
const char *clock_get_relative_daypart_string(time_t current_timestamp,
                                              uint32_t hours_in_the_future) {
  struct tm current_tm;
  const char *message = NULL;
  localtime_r(&current_timestamp, &current_tm);

  // Look for the furthest time in the future that we are "above"
  for (int i = ARRAY_LENGTH(daypart_messages) - 1; i >= 0; i--) {
    if ((current_tm.tm_hour + hours_in_the_future) >= daypart_messages[i].hour_offset) {
      message = daypart_messages[i].message;
      break;
    }
  }
  return message;
}

void clock_hour_and_minute_add(int *hour, int *minute, int delta_minutes) {
  const int new_minutes = positive_modulo(*hour * MINUTES_PER_HOUR + *minute + delta_minutes,
                                          MINUTES_PER_DAY);
  *hour = new_minutes / MINUTES_PER_HOUR;
  *minute = new_minutes % MINUTES_PER_HOUR;
}
