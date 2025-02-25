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

#include "services/normal/alarms/alarm.h"
#include "services/normal/alarms/alarm_pin.h"

#include "drivers/rtc.h"
#include "resource/timeline_resource_ids.auto.h"
#include "services/common/cron.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/system_task.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/settings/settings_file.h"
#include "services/normal/timeline/item.h"
#include "util/attributes.h"

#include <stdint.h>
#include <string.h>

#include "clar.h"


// Stubs
#include "stubs_analytics.h"
#include "stubs_app_cache.h"
#include "stubs_app_install_manager.h"
#include "stubs_blob_db.h"
#include "stubs_calendar.h"
#include "stubs_hexdump.h"
#include "stubs_i18n.h"
#include "stubs_layout_layer.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_tasks.h"
#include "stubs_prompt.h"
#include "stubs_queue.h"
#include "stubs_rand_ptr.h"
#include "stubs_regular_timer.h"
#include "stubs_sleep.h"
#include "stubs_task_watchdog.h"
#include "stubs_timeline_event.h"

// Fakes
#include "fake_spi_flash.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Stubs

status_t reminder_db_delete_with_parent(const TimelineItemId *id) {
  return S_SUCCESS;
}

const PebbleProcessMd* alarms_app_get_info() {
  static const PebbleProcessMdSystem s_alarms_app_info = {
    .common = {
      .uuid = {0x67, 0xa3, 0x2d, 0x95, 0xef, 0x69, 0x46, 0xd4,
               0xa0, 0xb9, 0x85, 0x4c, 0xc6, 0x2f, 0x97, 0xf9},
    },
  };
  return (const PebbleProcessMd*) &s_alarms_app_info;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Structs from alarm.c (used to assert correctness)

#define MAX_CONFIGURED_ALARMS (4)

typedef enum AlarmDataType {
  ALARM_DATA_CONFIG = 0,
  ALARM_DATA_PINS = 1,
} AlarmDataType;

typedef struct PACKED AlarmStorageKey {
  AlarmId id;
  AlarmDataType type:8;
} AlarmStorageKey;

typedef struct PACKED {
  AlarmKind kind:8;
  bool is_disabled;
  uint8_t hour;
  uint8_t minute;
  // 1 entry per week day. True if the alarm should go off on that week day. Sunday = 0.
  bool scheduled_days[7];
} AlarmConfig;


///////////////////////////////////////////////////////////////////////////////////////////////////
//! State variables
static const bool s_every_day_schedule[7] = {true, true, true, true, true, true, true};
static const bool s_weekend_schedule[7] = {true, false, false, false, false, false, true};
static const bool s_weekday_schedule[7] = {false, true, true, true, true, true, false};
static int s_current_hour = 0;
static int s_current_minute = 0;
static int s_current_day = 0;

// Thursday, March 12, 2015, 00:00 UTC
static const int s_thursday = 1426118400;
// Friday March 13, 2015, 00:00 UTC
static const int s_friday = 1426204800;
// Saturday March 14, 2015, 00:00 UTC
static const int s_saturday = 1426291200;
// Sunday March 15, 2015, 00:00 UTC
static const int s_sunday = 1426377600;
// Monday March 16, 2015, 00:00 UTC
static const int s_monday = 1426464000;
// Tuesday, March 17, 2015, 00:00 UTC
static const int s_tuesday = 1426550400;
// Wednesday, March 18, 2015, 00:00 UTC
static const int s_wednesday = 1426636800;

static TimelineItem *s_last_timeline_item_added = NULL;
static Uuid s_last_timeline_item_removed_uuid = {};


///////////////////////////////////////////////////////////////////////////////////////////////////
//! Counter variables
static int s_num_timeline_adds = 0;
static int s_num_timeline_removes = 0;
static int s_num_alarm_events_put = 0;
static int s_num_alarms_fired = 0;


///////////////////////////////////////////////////////////////////////////////////////////////////
//! Fakes

#include "system/logging.h"

void prv_timer_kernel_bg_callback(void *data);

bool system_task_add_callback(SystemTaskEventCallback cb, void *data) {
  cb(data);
  if (cb == prv_timer_kernel_bg_callback) {
    s_num_alarms_fired++;
  }
  return true;
}

int prv_hours_and_minutes_to_seconds(int hour, int minute) {
  return (hour * SECONDS_PER_HOUR) + (minute * SECONDS_PER_MINUTE);
}

const char *timeline_get_private_data_source(Uuid *parent_id) {
  return NULL;
}

status_t pin_db_insert_item_without_event(TimelineItem *item) {
  s_num_timeline_adds++;
  timeline_item_destroy(s_last_timeline_item_added);
  s_last_timeline_item_added = timeline_item_copy(item);
  return true;
}

status_t pin_db_delete(const uint8_t *key, int key_len) {
  s_num_timeline_removes++;
  s_last_timeline_item_removed_uuid = *(Uuid *)key;
  return true;
}

void event_put(PebbleEvent* event) {
  s_num_alarm_events_put++;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Helper Functions

void prv_assert_settings_key_absent(const void *key, size_t key_len) {
  SettingsFile file;
  cl_must_pass(settings_file_open(&file, "alarms", 1024));
  cl_assert(settings_file_get_len(&file, key, key_len) == 0);
  settings_file_close(&file);
}

void prv_assert_settings_value(const void *key, size_t key_len,
                               const void *expected_value, size_t value_len) {
  SettingsFile file;
  char buffer[value_len];
  cl_must_pass(settings_file_open(&file, "alarms", 1024));
  cl_must_pass(settings_file_get(&file, key, key_len, buffer, value_len));
  settings_file_close(&file);
  cl_assert_equal_m(expected_value, buffer, value_len);
}

void prv_assert_alarm_config(AlarmId id, uint8_t hour, uint8_t minute,
                             bool disabled, AlarmKind kind, const bool scheduled_days[7]) {
  AlarmStorageKey key = { .id = id, .type = ALARM_DATA_CONFIG };
  AlarmConfig config = {
    .kind = kind,
    .is_disabled = disabled,
    .hour = hour,
    .minute = minute,
  };
  memcpy(&config.scheduled_days, scheduled_days, 7);
  prv_assert_settings_value(&key, sizeof(key), &config, sizeof(config));

  // Test the getters
  AlarmKind stored_kind;
  alarm_get_kind(id, &stored_kind);
  cl_assert_equal_i(kind, stored_kind);

  int stored_hours, stored_minutes;
  alarm_get_hours_minutes(id, &stored_hours, &stored_minutes);
  cl_assert_equal_i(hour, stored_hours);
  cl_assert_equal_i(minute, stored_minutes);

  cl_assert_equal_i(alarm_get_enabled(id), !disabled);
}

void prv_assert_alarm_config_absent(AlarmId id) {
  AlarmStorageKey key = { .id = id, .type = ALARM_DATA_CONFIG };
  prv_assert_settings_key_absent(&key, sizeof(key));
}

void assert_alarm_pins_absent(AlarmId id) {
  AlarmStorageKey key = { .id = id, .type = ALARM_DATA_PINS };
  prv_assert_settings_key_absent(&key, sizeof(key));
}
