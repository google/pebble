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

#include "kernel/events.h"
#include "services/normal/blob_db/pin_db.h"
#include "services/normal/timeline/event.h"
#include "services/normal/timeline/peek.h"
#include "services/normal/timeline/timeline.h"
#include "system/logging.h"

#include "clar.h"
#include "pebble_asserts.h"

// Stubs
////////////////////////////////////////////////////////////////
#include "stubs_analytics.h"
#include "stubs_ancs.h"
#include "stubs_ancs_notifications.h"
#include "stubs_app_cache.h"
#include "stubs_app_install_manager.h"
#include "stubs_app_manager.h"
#include "stubs_blob_db.h"
#include "stubs_blob_db_sync.h"
#include "stubs_blob_db_sync_util.h"
#include "stubs_event_loop.h"
#include "stubs_event_service_client.h"
#include "stubs_hexdump.h"
#include "stubs_i18n.h"
#include "stubs_layout_layer.h"
#include "stubs_logging.h"
#include "stubs_modal_manager.h"
#include "stubs_mutex.h"
#include "stubs_notification_storage.h"
#include "stubs_notifications.h"
#include "stubs_passert.h"
#include "stubs_phone_call_util.h"
#include "stubs_prompt.h"
#include "stubs_rand_ptr.h"
#include "stubs_regular_timer.h"
#include "stubs_reminder_db.h"
#include "stubs_session.h"
#include "stubs_sleep.h"
#include "stubs_system_task.h"
#include "stubs_task_watchdog.h"
#include "stubs_text_layer_flow.h"
#include "stubs_timeline.h"
#include "stubs_timeline_peek.h"
#include "stubs_timeline_pin_window.h"
#include "stubs_window_stack.h"

// Fakes
////////////////////////////////////////////////////////////////
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_pebble_tasks.h"
#include "fake_rtc.h"
#include "fake_spi_flash.h"
#include "fake_settings_file.h"
#include "fake_events.h"

bool calendar_layout_verify(bool existing_attributes[]) {
  return true;
}

bool weather_layout_verify(bool existing_attributes[]) {
  return true;
}

const TimelineEventImpl *calendar_get_event_service(void) {
  return NULL;
}

// Helpers
////////////////////////////////////////////////////////////////
typedef struct PeekTestData {
  PebbleTimelinePeekEvent last_peek_event;
  unsigned int num_peek_events;
} PeekTestData;

static PeekTestData s_data;

static PebbleTimelinePeekEvent prv_get_peek_event(void) {
  return s_data.last_peek_event;
}

static void prv_event_handler(PebbleEvent *event) {
  if (event->type == PEBBLE_TIMELINE_PEEK_EVENT) {
    s_data.last_peek_event = event->timeline_peek;
    s_data.num_peek_events++;
  }
}

// Fake pins
////////////////////////////////////////////////////////////////

static Attribute title_attr = {
  .id = AttributeIdTitle,
  .cstring = "title",
};

static TimelineItem s_item1 = {
  .header = {
    .id = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 1 * SECONDS_PER_MINUTE,
    .duration = 15,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

static TimelineItem s_item2 = {
  .header = {
    .id = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 5 * SECONDS_PER_MINUTE,
    .duration = 20,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

static TimelineItem s_item3 = {
  .header = {
    .id = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 9 * SECONDS_PER_MINUTE,
    .duration = 5,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

static TimelineItem s_future_item = {
  .header = {
    .id = { 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 100 * SECONDS_PER_MINUTE,
    .duration = 10,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

static TimelineItem s_short_future_item = {
  .header = {
    .id = { 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 100 * SECONDS_PER_MINUTE,
    .duration = 5,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// not a calendar pin
static TimelineItem s_weather_item = {
  .header = {
    .id = { 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 10 * SECONDS_PER_MINUTE,
    .duration = 10,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdWeather,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// add day pin
static TimelineItem s_all_day_item = {
  .header = {
    .id = { 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 100 * SECONDS_PER_MINUTE,
    .duration = 10,
    .type = TimelineItemTypePin,
    .all_day = true,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// 0-duration event
static TimelineItem s_point_item = {
  .header = {
    .id = { 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 20 * SECONDS_PER_MINUTE,
    .duration = 0,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdWeather,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// recurring calendar event 1
static TimelineItem s_recurring_calendar_item1 = {
  .header = {
    .id = { 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 50 * SECONDS_PER_MINUTE - SECONDS_PER_DAY,
    .duration = 30,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// recurring calendar event 2
static TimelineItem s_recurring_calendar_item2 = {
  .header = {
    .id = { 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 50 * SECONDS_PER_MINUTE,
    .duration = 30,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// recurring calendar event 3
static TimelineItem s_recurring_calendar_item3 = {
  .header = {
    .id = { 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 50 * SECONDS_PER_MINUTE + SECONDS_PER_DAY,
    .duration = 30,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// back-to-back calendar event 1
static TimelineItem s_back_to_back_calendar_item1 = {
  .header = {
    .id = { 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 60 * SECONDS_PER_MINUTE,
    .duration = 30,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// back-to-back calendar event 2
static TimelineItem s_back_to_back_calendar_item2 = {
  .header = {
    .id = { 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    .timestamp = 90 * SECONDS_PER_MINUTE,
    .duration = 30,
    .type = TimelineItemTypePin,
    .all_day = false,
    .layout = LayoutIdCalendar,
  },
  .attr_list = {
    .num_attributes = 1,
    .attributes = &title_attr,
  },
};

// Setup
////////////////////////////////////////////////////////////////

void test_timeline_peek_event__initialize(void) {
  s_data = (PeekTestData){};
  rtc_set_time(0);
  fake_event_init();
  fake_event_set_callback(prv_event_handler);
  pin_db_init();
  timeline_event_init();
}

void test_timeline_peek_event__cleanup(void) {
  timeline_peek_set_show_before_time(TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S);
  timeline_event_deinit();
  stub_new_timer_cleanup();
  fake_settings_file_reset();
}

// Tests
////////////////////////////////////////////////////////////////

typedef struct AddEventParams {
  TimelineItem *item;
} AddEventParams;

#define ADD_EVENT(...) ({ \
  AddEventParams params = { __VA_ARGS__ }; \
  cl_assert(timeline_add(params.item)); \
  timeline_event_handle_blobdb_event(); \
  params.item; \
})

typedef struct CreateEventParams {
  uint8_t id;
  LayoutId layout;
  time_t timestamp;
  uint16_t duration;
  bool all_day;
  bool persistent;
} CreateEventParams;

#define DEFINE_EVENT(...) ({ \
  CreateEventParams params = { __VA_ARGS__ }; \
  TimelineItem item = { \
    .header = { \
      .type = TimelineItemTypePin, \
      .id = { params.id, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
              0x00     , 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, \
      .layout = params.layout ?: LayoutIdCalendar, \
      .persistent = params.persistent ? 1 : 0, \
      .timestamp = params.timestamp, \
      .all_day = params.all_day, \
      .duration = params.duration, \
    }, \
    .attr_list = { \
      .num_attributes = 1, \
      .attributes = &title_attr, \
    }, \
  }; \
  ADD_EVENT( .item = &item ); \
  item; \
})

typedef struct CheckNoEventsParams {
  unsigned int count;
  bool is_future_empty;
} CheckNoEventsParams;

#define CHECK_NO_EVENTS(...) ({ \
  CheckNoEventsParams params = { __VA_ARGS__ }; \
  PebbleTimelinePeekEvent peek = prv_get_peek_event(); \
  cl_assert_equal_i(s_data.num_peek_events, params.count); \
  cl_assert_equal_uuid(peek.item_id ? *peek.item_id : UUID_INVALID, UUID_INVALID); \
  cl_assert_equal_i(peek.time_type, TimelinePeekTimeType_None); \
  cl_assert_equal_i(peek.num_concurrent, 0); \
  cl_assert_equal_b(peek.is_future_empty, params.is_future_empty); \
  cl_assert_equal_i(stub_new_timer_get_next(), TIMER_INVALID_ID); \
  peek; \
})

typedef struct CheckEventParams {
  unsigned int count;
  Uuid item_id;
  unsigned int num_concurrent;
  unsigned int timeout_ms;
  TimelinePeekTimeType time_type;
  bool is_first_event;
} CheckEventParams;

#define CHECK_EVENT(...) ({ \
  CheckEventParams params = { __VA_ARGS__ }; \
  PebbleTimelinePeekEvent peek = prv_get_peek_event(); \
  cl_assert_equal_i(s_data.num_peek_events, params.count); \
  cl_assert_equal_uuid(peek.item_id ? *peek.item_id : UUID_INVALID, params.item_id); \
  cl_assert_equal_i(peek.time_type, params.time_type); \
  cl_assert_equal_i(peek.num_concurrent, params.num_concurrent); \
  cl_assert_equal_b(peek.is_first_event, params.is_first_event); \
  cl_assert_equal_b(peek.is_future_empty, false); \
  const TimerID timer_id = stub_new_timer_get_next(); \
  cl_assert(timer_id != TIMER_INVALID_ID); \
  cl_assert_equal_i(stub_new_timer_timeout(timer_id), params.timeout_ms); \
  peek; \
})

static void prv_invoke_timer(unsigned int timeout_s) {
  fake_rtc_increment_time(timeout_s);
  stub_new_timer_invoke(1 /* num_invoke */);
}

void test_timeline_peek_event__no_events(void) {
  CHECK_NO_EVENTS( .count = 1, .is_future_empty = true );
}

void test_timeline_peek_event__calendar_event(void) {
  ADD_EVENT( .item = &s_item1 );
  CHECK_EVENT( .count = 2, .item_id = s_item1.header.id, .num_concurrent = 0,
               .timeout_ms = SECONDS_PER_MINUTE * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
}

void test_timeline_peek_event__calendar_event_all_day(void) {
  ADD_EVENT( .item = &s_all_day_item );
  CHECK_NO_EVENTS( .count = 2, .is_future_empty = true );
}

void test_timeline_peek_event__weather_event(void) {
  ADD_EVENT( .item = &s_weather_item );
  CHECK_EVENT( .count = 2, .item_id = s_weather_item.header.id, .num_concurrent = 0,
               .timeout_ms = s_weather_item.header.timestamp * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
}

void test_timeline_peek_event__concurrent_count_and_priority(void) {
  // Test that num_concurrent increases accordingly
  // Also test that upcoming items take priority
  ADD_EVENT( .item = &s_item1 );
  CHECK_EVENT( .count = 2, .item_id = s_item1.header.id, .num_concurrent = 0,
               .timeout_ms = SECONDS_PER_MINUTE * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  ADD_EVENT( .item = &s_item2 );
  CHECK_EVENT( .count = 3, .item_id = s_item2.header.id, .num_concurrent = 1,
               .timeout_ms = SECONDS_PER_MINUTE * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart );
  ADD_EVENT( .item = &s_item3 );
  CHECK_EVENT( .count = 4, .item_id = s_item3.header.id, .num_concurrent = 2,
               .timeout_ms = SECONDS_PER_MINUTE * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart );
  // The future item is too far to increase the concurrent count
  ADD_EVENT( .item = &s_future_item );
  CHECK_EVENT( .count = 5, .item_id = s_item3.header.id, .num_concurrent = 2,
               .timeout_ms = SECONDS_PER_MINUTE * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart );
}

void test_timeline_peek_event__before_upcoming_event(void) {
  // Check that the event is about an upcoming item
  ADD_EVENT( .item = &s_future_item );
  CHECK_EVENT( .count = 2, .item_id = s_future_item.header.id, .num_concurrent = 0,
               .timeout_ms =
                   ((s_future_item.header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S) *
                    MS_PER_SECOND),
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
}

void test_timeline_peek_event__before_upcoming_event_custom_5min(void) {
  // Check that the event is about an upcoming item at a custom 5min timeout
  const unsigned int show_before_time_s = 5 * SECONDS_PER_MINUTE;
  timeline_peek_set_show_before_time(show_before_time_s);
  ADD_EVENT( .item = &s_future_item );
  CHECK_EVENT( .count = 3, .item_id = s_future_item.header.id, .num_concurrent = 0,
               .timeout_ms = ((s_future_item.header.timestamp - show_before_time_s) *
                              MS_PER_SECOND),
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
}

void test_timeline_peek_event__before_event_starts(void) {
  // Check that the event is about an item that is about to start
  rtc_set_time(s_future_item.header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S / 2);
  ADD_EVENT( .item = &s_future_item );
  CHECK_EVENT( .count = 2, .item_id = s_future_item.header.id, .num_concurrent = 0,
               .timeout_ms = (TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S * MS_PER_SECOND) / 2,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
}

void test_timeline_peek_event__after_event_starts(void) {
  // Check that the event is about an item about to pass the hide time
  rtc_set_time(5 * SECONDS_PER_MINUTE);
  ADD_EVENT( .item = &s_item1 );
  CHECK_EVENT( .count = 2, .item_id = s_item1.header.id, .num_concurrent = 0,
               .timeout_ms = ((TIMELINE_PEEK_HIDE_AFTER_TIME_S -
                               (rtc_get_time() - s_item1.header.timestamp)) * MS_PER_SECOND),
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
}

void test_timeline_peek_event__after_event_starts_short_event(void) {
  // Check that for a short event, the timeout is the end of the item instead
  rtc_set_time(10 * SECONDS_PER_MINUTE);
  ADD_EVENT( .item = &s_item3 );
  CHECK_EVENT( .count = 2, .item_id = s_item3.header.id, .num_concurrent = 0,
               .timeout_ms = 4 * SECONDS_PER_MINUTE * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
}

void test_timeline_peek_event__after_event_passed_hide_time(void) {
  // Check that there is no event if the last item passed the hide time
  rtc_set_time(15 * SECONDS_PER_MINUTE);
  ADD_EVENT( .item = &s_item2 );
  CHECK_NO_EVENTS( .count = 2 );
}

void test_timeline_peek_event__after_event_passed_completely(void) {
  rtc_set_time(30 * SECONDS_PER_MINUTE);
  ADD_EVENT( .item = &s_item2 );
  CHECK_NO_EVENTS( .count = 2, .is_future_empty = true );
}

void test_timeline_peek_event__dismiss_event(void) {
  // Check that dismissing the last event causes no events to peek
  TimelineItem *item = &s_future_item;
  rtc_set_time(item->header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S / 2);
  ADD_EVENT( .item = item );
  CHECK_EVENT( .count = 2, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = (TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S * MS_PER_SECOND) / 2,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );

  // Simulate a timeline peek dismiss
  cl_must_pass(pin_db_set_status_bits(&item->header.id, TimelineItemStatusDismissed));
  timeline_event_refresh();

  CHECK_NO_EVENTS( .count = 3 );
}

void test_timeline_peek_event__first_event_with_past_event(void) {
  TimelineItem item =
      DEFINE_EVENT( .id = 0x01, .timestamp  = 20 * SECONDS_PER_MINUTE, .duration = 70 );
  TimelineItem UNUSED item2 =
      DEFINE_EVENT( .id = 0x02, .timestamp  = -50 * SECONDS_PER_MINUTE, .duration = 30 );
  unsigned int timeout_s = item.header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
}

void test_timeline_peek_event__first_event_with_all_day_event_before(void) {
  // All day events show up if no timed event has yet passed
  TimelineItem item =
      DEFINE_EVENT( .id = 0x01, .timestamp  = 20 * SECONDS_PER_MINUTE, .duration = 70 );
  TimelineItem UNUSED item2 =
      DEFINE_EVENT( .id = 0x02, .timestamp  = 0, .duration = MINUTES_PER_DAY, .all_day = true );
  unsigned int timeout_s = item.header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext );
}

void test_timeline_peek_event__first_event_with_all_day_event_after(void) {
  // After a timed event has passed, all day events no longer show up for the day
  rtc_set_time(SECONDS_PER_HOUR);
  TimelineItem item =
      DEFINE_EVENT( .id = 0x01, .timestamp  = SECONDS_PER_HOUR + 20 * SECONDS_PER_MINUTE,
                    .duration = 70 );
  TimelineItem UNUSED item2 =
      DEFINE_EVENT( .id = 0x02, .timestamp  = 0, .duration = MINUTES_PER_DAY, .all_day = true );
  TimelineItem UNUSED item3 =
      DEFINE_EVENT( .id = 0x03, .timestamp  = 0, .duration = 10 );
  unsigned int timeout_s = 600;
  CHECK_EVENT( .count = 4, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
}

void test_timeline_peek_event__one_event_lifecycle(void) {
  // Check that one event progresses through SomeTimeNext, WillStart, ShowStarted, None
  TimelineItem *item = &s_future_item;
  ADD_EVENT( .item = item );
  unsigned int timeout_s = item->header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 2, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_HIDE_AFTER_TIME_S;
  CHECK_EVENT( .count = 4, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  CHECK_NO_EVENTS( .count = 5 );
}

void test_timeline_peek_event__one_short_event_lifecycle(void) {
  // Check that one event progresses through SomeTimeNext, WillStart, ShowStarted, None
  TimelineItem *item = &s_short_future_item;
  ADD_EVENT( .item = item );
  unsigned int timeout_s = item->header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 2, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = item->header.duration * SECONDS_PER_MINUTE;
  CHECK_EVENT( .count = 4, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  CHECK_NO_EVENTS( .count = 5 );
}

void test_timeline_peek_event__0_duration_event_lifecycle(void) {
  // Check that one event progresses through SomeTimeNext, WillStart, ShowStarted, None
  TimelineItem *item = &s_point_item;
  ADD_EVENT( .item = item );
  unsigned int timeout_s = item->header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 2, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  CHECK_NO_EVENTS( .count = 4 );
}

void test_timeline_peek_event__one_recurring_event_lifecycle(void) {
  // Check that one event progresses through SomeTimeNext, WillStart, ShowStarted
  TimelineItem *item = &s_recurring_calendar_item2;
  ADD_EVENT( .item = &s_recurring_calendar_item1 );
  ADD_EVENT( .item = item );
  ADD_EVENT( .item = &s_recurring_calendar_item3 );
  unsigned int timeout_s = item->header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 4, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 5, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_HIDE_AFTER_TIME_S;
  CHECK_EVENT( .count = 6, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = (SECONDS_PER_DAY - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S -
               TIMELINE_PEEK_HIDE_AFTER_TIME_S);
  CHECK_EVENT( .count = 7, .item_id = s_recurring_calendar_item3.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext );
}

void test_timeline_peek_event__two_back_to_back_events(void) {
  // Check that one event progresses through SomeTimeNext, WillStart, ShowStarted
  TimelineItem *item = &s_back_to_back_calendar_item1;
  ADD_EVENT( .item = item );
  ADD_EVENT( .item = &s_back_to_back_calendar_item2 );
  unsigned int timeout_s = item->header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 4, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_HIDE_AFTER_TIME_S;
  CHECK_EVENT( .count = 5, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  item = &s_back_to_back_calendar_item2;
  CHECK_EVENT( .count = 6, .item_id = item->header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext );
}

void test_timeline_peek_event__one_persistent_event_lifecycle(void) {
  TimelineItem item =
      DEFINE_EVENT( .id = 0x01, .timestamp  = 20 * SECONDS_PER_MINUTE, .duration = 30,
                    .persistent = true );
  unsigned int timeout_s = item.header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 2, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_HIDE_AFTER_TIME_S;
  CHECK_EVENT( .count = 4, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = item.header.duration * SECONDS_PER_MINUTE - TIMELINE_PEEK_HIDE_AFTER_TIME_S;
  CHECK_EVENT( .count = 5, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  CHECK_NO_EVENTS( .count = 6, .is_future_empty = true );
}

void test_timeline_peek_event__upcoming_prioritized_over_persistent_event_lifecycle(void) {
  TimelineItem item =
      DEFINE_EVENT( .id = 0x01, .timestamp  = 20 * SECONDS_PER_MINUTE, .duration = 70,
                    .persistent = true );
  TimelineItem item2 =
      DEFINE_EVENT( .id = 0x02, .timestamp  = 50 * SECONDS_PER_MINUTE, .duration = 30 );
  unsigned int timeout_s = item.header.timestamp - TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 3, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_SomeTimeNext, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 4, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_HIDE_AFTER_TIME_S;
  CHECK_EVENT( .count = 5, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = 10 * SECONDS_PER_MINUTE; // time until the next event
  CHECK_EVENT( .count = 6, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_DEFAULT_SHOW_BEFORE_TIME_S;
  CHECK_EVENT( .count = 7, .item_id = item2.header.id, .num_concurrent = 1,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowWillStart, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = TIMELINE_PEEK_HIDE_AFTER_TIME_S;
  CHECK_EVENT( .count = 8, .item_id = item2.header.id, .num_concurrent = 1,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted, .is_first_event = true );
  prv_invoke_timer(timeout_s);
  timeout_s = 30 * SECONDS_PER_MINUTE; // time until persistent event ends
  CHECK_EVENT( .count = 9, .item_id = item.header.id, .num_concurrent = 0,
               .timeout_ms = timeout_s * MS_PER_SECOND,
               .time_type = TimelinePeekTimeType_ShowStarted );
  prv_invoke_timer(timeout_s);
  CHECK_NO_EVENTS( .count = 10, .is_future_empty = true );
}
