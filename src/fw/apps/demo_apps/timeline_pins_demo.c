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

#include "timeline_pins_demo.h"

#include "applib/app.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/option_menu_window.h"
#include "apps/system_app_ids.h"
#include "apps/system_apps/timeline/timeline.h"
#include "process_management/app_manager.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/activity/activity_insights.h"
#include "services/normal/blob_db/pin_db.h"
#include "services/normal/notifications/notification_storage.h"
#include "services/normal/timeline/calendar_layout.h"
#include "services/normal/timeline/event.h"
#include "services/normal/timeline/health_layout.h"
#include "services/normal/timeline/weather_layout.h"
#include "util/size.h"

#include <sys/cdefs.h>

#define StringListLiteral(str) { \
  .serialized_byte_length = ARRAY_LENGTH(str) - 1, \
  .data = str, \
} \

const char *timeline_demo_strings[TimelinePinsDemoCount] = {
    [TimelinePinsDemo_Default] = "Default Pins",
    [TimelinePinsDemo_OneDayAway] = "Pins One Day Away",
    [TimelinePinsDemo_OngoingEvent] = "Ongoing Event",
    [TimelinePinsDemo_Notifications] = "Notifications",
    [TimelinePinsDemo_TodayAndTomorrow] = "Today & Tomorrow",
};

static void prv_set_timeline_icon(AttributeList *list, TimelineResourceId timeline_res,
                                  TimelineResourceId card_res) {
  attribute_list_add_uint32(list, AttributeIdIconTiny, timeline_res);
  attribute_list_add_uint32(list, AttributeIdIconSmall, card_res ?: timeline_res);
  attribute_list_add_uint32(list, AttributeIdIconLarge, card_res ?: timeline_res);
}

#define ARRAY_RAND(arr) arr[rand() % ARRAY_LENGTH(arr)]

static void prv_add_notification(int32_t delta_time_s) {
  const time_t now = rtc_get_time();

  AttributeList list = {};

  TimelineResourceId icon_resources[] = {
      TIMELINE_RESOURCE_NOTIFICATION_FACEBOOK_MESSENGER,
      TIMELINE_RESOURCE_NOTIFICATION_FACEBOOK,
      TIMELINE_RESOURCE_NOTIFICATION_MAILBOX,
      TIMELINE_RESOURCE_NOTIFICATION_GENERIC,
  };
  char *titles[] = {
      "Angela Tam", "Liron Damir", "Heiko Behrens", "Kevin Conley", "Matt Hungerford",
  };
  char *bodies[] = {
      "Late again? Can you be on time ever? Seriously? Dude!!!",
      "Late again. Sorry, I'll be there a few minutes. Meanwhile, I am just texting long messages.",
      "What's up for lunch?",
      "\xF0\x9F\x98\x83 \xF0\x9F\x92\xA9",
  };

  prv_set_timeline_icon(&list, ARRAY_RAND(icon_resources), 0);
  attribute_list_add_cstring(&list, AttributeIdTitle, ARRAY_RAND(titles));
  attribute_list_add_cstring(&list, AttributeIdBody, ARRAY_RAND(bodies));
  attribute_list_add_uint32(&list, AttributeIdLastUpdated, now);
  TimelineItem *item = timeline_item_create_with_attributes(now + delta_time_s, 0,
                                                            TimelineItemTypeNotification,
                                                            LayoutIdNotification,
                                                            &list, NULL);
  notification_storage_store(item);
  timeline_item_destroy(item);
  attribute_list_destroy_list(&list);
}

static void prv_add_weather_pin_with_params(int32_t delta_time_s, bool has_timestamp,
                                            bool has_short_title, bool has_short_subtitle) {
  const time_t now = rtc_get_time();

  AttributeList list = {};
  prv_set_timeline_icon(&list, TIMELINE_RESOURCE_TIMELINE_WEATHER, 0);
  attribute_list_add_cstring(&list, AttributeIdTitle, "SUNRISE");
  attribute_list_add_cstring(&list, AttributeIdSubtitle, "11°/6°");
  attribute_list_add_cstring(&list, AttributeIdLocationName,
                             "SAN LOUIS OBISPO\n"
                             "CALIFORNIA, USA");
  attribute_list_add_cstring(&list, AttributeIdBody,
                             "Cloudy with rain and snow. High 1C. Winds light and variable. "
                             "Chance of precip 100%. 3-7cm of snow expected.");
  attribute_list_add_uint32(&list, AttributeIdLastUpdated, now);
  if (!has_timestamp) {
    attribute_list_add_uint8(&list, AttributeIdDisplayTime, WeatherTimeType_None);
  }
  if (has_short_title) {
    attribute_list_add_cstring(&list, AttributeIdShortTitle, "Sunrise");
  }
  if (has_short_subtitle) {
    attribute_list_add_cstring(&list, AttributeIdShortSubtitle, "Cloudy with rain and snow");
  }
  TimelineItem *item = timeline_item_create_with_attributes(now + delta_time_s, 0,
                                                            TimelineItemTypePin, LayoutIdWeather,
                                                            &list, NULL);

  pin_db_insert_item_without_event(item);
  timeline_item_destroy(item);
  attribute_list_destroy_list(&list);
}

static void prv_add_weather_pin(int32_t delta_time_s) {
  prv_add_weather_pin_with_params(delta_time_s, true, false, false);
}

static void prv_add_sports_pin(int32_t delta_time_s, GColor secondary_color, bool is_ingame,
                               bool has_broadcaster) {
  const time_t now = rtc_get_time();

  AttributeList list = {};
  prv_set_timeline_icon(&list, TIMELINE_RESOURCE_TIMELINE_SPORTS, 0);
  attribute_list_add_uint8(&list, AttributeIdSecondaryColor, secondary_color.argb);
  attribute_list_add_cstring(&list, AttributeIdTitle, "Avalanche at Sharks");
  attribute_list_add_cstring(&list, AttributeIdSubtitle, "Q4 • 1:25");
  attribute_list_add_uint32(&list, AttributeIdSportsGameState, is_ingame ? 1 : 0);
  attribute_list_add_cstring(&list, AttributeIdNameAway, "GSW");
  attribute_list_add_cstring(&list, AttributeIdRecordAway, "114-152");
  attribute_list_add_cstring(&list, AttributeIdScoreAway, "86");
  attribute_list_add_cstring(&list, AttributeIdNameHome, "CHI");
  attribute_list_add_cstring(&list, AttributeIdRecordHome, "110-15");
  attribute_list_add_cstring(&list, AttributeIdScoreHome, "103");
  if (has_broadcaster) {
    attribute_list_add_cstring(&list, AttributeIdBroadcaster, "ABC");
  }
  attribute_list_add_cstring(&list, AttributeIdBody,
                             "01:45\nJames 3pt Shot: Missed\n"
                             "03:15 | 22-29\nLeonard Free Throw 2 of 2 (8PTS)");
  attribute_list_add_uint32(&list, AttributeIdLastUpdated, now);
  TimelineItem *item = timeline_item_create_with_attributes(now + delta_time_s, 0,
                                                            TimelineItemTypePin, LayoutIdSports,
                                                            &list, NULL);

  pin_db_insert_item_without_event(item);
  timeline_item_destroy(item);
  attribute_list_destroy_list(&list);
}

static void prv_add_calendar_pin(int32_t delta_time_s, int32_t duration_m, bool is_all_day,
                                 bool recurring, TimelineResourceId icon,
                                 TimelineResourceId card_icon) {
  const time_t now = rtc_get_time();
  time_t target = now + delta_time_s;
  if (is_all_day) {
    target = time_util_get_midnight_of(target);
    duration_m = ((duration_m - 1) / MINUTES_PER_DAY + 1) * MINUTES_PER_DAY;
  }

  AttributeList list = {};
  prv_set_timeline_icon(&list, icon ?: TIMELINE_RESOURCE_TIMELINE_CALENDAR, card_icon);
  if (recurring) {
    attribute_list_add_uint8(&list, AttributeIdDisplayRecurring, CalendarRecurringTypeRecurring);
  }
  attribute_list_add_cstring(&list, AttributeIdTitle, "Weekly All Hands design stuff");
  attribute_list_add_cstring(&list, AttributeIdLocationName, "ConfRM-HIGH_Video Room");
  static StringList headings = StringListLiteral("Description\0Attendees\0Organizer");
  static StringList paragraphs = StringListLiteral(
      "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
      "ut labore et dolore magna aliqua.\0"
      "Ryan Case\nBrad Murray\0Sarah Otten");
  attribute_list_add_string_list(&list, AttributeIdHeadings, &headings);
  attribute_list_add_string_list(&list, AttributeIdParagraphs, &paragraphs);
  attribute_list_add_cstring(&list, AttributeIdBody,
                             "Topics for the week can be found here: "
                             "http://docs.google.com/u/1/#inbox/14b9fa5f872ebbc6\n\n"
                             "Will email before if we need to cancel");
  attribute_list_add_uint32(&list, AttributeIdLastUpdated, now);
  TimelineItem *item = timeline_item_create_with_attributes(target, duration_m,
                                                            TimelineItemTypePin, LayoutIdCalendar,
                                                            &list, NULL);

  pin_db_insert_item_without_event(item);
  timeline_item_destroy(item);
  attribute_list_destroy_list(&list);
}

static void prv_add_generic_pin(int32_t delta_time_s, bool has_subtitle) {
  const time_t now = rtc_get_time();

  AttributeList list = {};
  prv_set_timeline_icon(&list, TIMELINE_RESOURCE_NOTIFICATION_FLAG, 0);
  attribute_list_add_cstring(&list, AttributeIdTitle, "Delfina Pizza");
  if (has_subtitle) {
    attribute_list_add_cstring(&list, AttributeIdSubtitle, "Open Table Reservation");
  }
  attribute_list_add_cstring(&list, AttributeIdLocationName,
                             "145 Williams John\n"
                             "Palo Alto");
  static StringList headings = StringListLiteral("Attendees\0Organizer");
  static StringList paragraphs = StringListLiteral("Ryan Case\nBrad Murray\0Sarah Otten");
  attribute_list_add_string_list(&list, AttributeIdHeadings, &headings);
  attribute_list_add_string_list(&list, AttributeIdParagraphs, &paragraphs);
  attribute_list_add_cstring(&list, AttributeIdBody, "Body message");
  attribute_list_add_uint32(&list, AttributeIdLastUpdated, now);
  TimelineItem *item = timeline_item_create_with_attributes(now + delta_time_s, 0,
                                                            TimelineItemTypePin, LayoutIdGeneric,
                                                            &list, NULL);

  pin_db_insert_item_without_event(item);
  timeline_item_destroy(item);
  attribute_list_destroy_list(&list);
}

static void prv_add_activity_session_pin(int32_t delta_time_s, int32_t duration_m) {
  const time_t now = rtc_get_time();

  AttributeList list = {};
  attribute_list_add_uint32(&list, AttributeIdIconPin, TIMELINE_RESOURCE_RUN);
  attribute_list_add_uint32(&list, AttributeIdIconTiny, TIMELINE_RESOURCE_PACE);
  attribute_list_add_uint8(&list, AttributeIdHealthInsightType,
                           ActivityInsightType_ActivitySessionRun);
  attribute_list_add_cstring(&list, AttributeIdTitle, "3.3 Mile run");
  attribute_list_add_cstring(&list, AttributeIdSubtitle, "30M of activity");
  attribute_list_add_uint32(&list, AttributeIdLastUpdated, now);

  uint8_t buffer[Uint32ListSize(ActivitySessionMetricCount)];
  Uint32List *icons = (Uint32List *)buffer;
  icons->num_values = ActivitySessionMetricCount,
  icons->values[0] = TIMELINE_RESOURCE_PACE;
  icons->values[1] = TIMELINE_RESOURCE_DURATION;
  icons->values[2] = TIMELINE_RESOURCE_CALORIES;
  icons->values[3] = TIMELINE_RESOURCE_DISTANCE;
  static StringList names = StringListLiteral("Pace\0Run duration\0Calories burned\0Distance");
  static StringList values = StringListLiteral("7:45\0" "30M\0" "8384\0" "3.3 miles");
  attribute_list_add_string_list(&list, AttributeIdMetricNames, &names);
  attribute_list_add_string_list(&list, AttributeIdMetricValues, &values);
  attribute_list_add_uint32_list(&list, AttributeIdMetricIcons, icons);

  TimelineItem *item = timeline_item_create_with_attributes(now + delta_time_s, duration_m,
                                                            TimelineItemTypePin, LayoutIdHealth,
                                                            &list, NULL);

  pin_db_insert_item_without_event(item);
  timeline_item_destroy(item);
  attribute_list_destroy_list(&list);
}

static void prv_launch_timeline(void) {
  app_manager_put_launch_app_event(&(AppLaunchEventConfig) { .id = APP_ID_TIMELINE });
}

static void prv_launch_notifications(void) {
  app_manager_put_launch_app_event(&(AppLaunchEventConfig) { .id = APP_ID_NOTIFICATIONS });
}

void timeline_pins_demo_add_pins(TimelinePinsDemoSet pin_set) {
  const bool has_broadcaster = true;
  const bool has_short_subtitle = true;
  const bool has_short_title = true;
  const bool has_subtitle = true;
  const bool has_timestamp = true;
  const bool is_all_day = true;
  const bool is_ingame = true;
  const bool recurring = true;
  switch (pin_set) {
    case TimelinePinsDemo_Default: {
      // Past pins (deprecated)
      prv_add_calendar_pin(-4 * 60 * 60, 60, !is_all_day, !recurring, 0, 0);
      prv_add_generic_pin(-6 * 60 * 60, !has_subtitle);
      prv_add_generic_pin(-5 * 60 * 60, has_subtitle);
      prv_add_sports_pin(-4 * 60 * 60, GColorBlack, is_ingame, !has_broadcaster);
      prv_add_activity_session_pin(-3 * 60 * 60, 30);
      prv_add_weather_pin_with_params(-2 * 60 * 60, !has_timestamp, !has_short_title,
                                      !has_short_subtitle);
      prv_add_weather_pin_with_params(-60 * 60, has_timestamp, has_short_title, has_short_subtitle);

      // Peek pins
      prv_add_calendar_pin(5 * 60, 60, !is_all_day, recurring, 0, 0);
      prv_add_weather_pin(10 * 60 + 15);
      prv_add_sports_pin(10 * 60 + 17, GColorWhite, !is_ingame, has_broadcaster);

      // Future pins
      prv_add_calendar_pin(30 * 60, 3 * 24 * 60, is_all_day, !recurring,
                           TIMELINE_RESOURCE_SCHEDULED_EVENT, 0);
      prv_add_calendar_pin(60 * 60, 3 * 24 * 60, is_all_day, recurring,
                           TIMELINE_RESOURCE_RADIO_SHOW, TIMELINE_RESOURCE_STOCKS_EVENT);
      prv_add_calendar_pin(90 * 60, 60, !is_all_day, !recurring, 0, 0);
      prv_add_weather_pin(50 * 60);
      prv_add_sports_pin(2 * 60 * 60, GColorWhite, !is_ingame, has_broadcaster);
      prv_add_sports_pin(3 * 60 * 60, GColorWhite, is_ingame, has_broadcaster);
      prv_add_calendar_pin(4 * 60 * 60, 60, !is_all_day, recurring, 0, 0);
      prv_add_calendar_pin(6 * 60 * 60, 60, !is_all_day, recurring, 0, 0);
      prv_add_generic_pin(7 * 60 * 60, has_subtitle);
      prv_add_generic_pin(8 * 60 * 60, !has_subtitle);
      prv_add_weather_pin(1 * 24 * 60 * 60);
      prv_add_weather_pin(2 * 24 * 60 * 60);
      prv_add_weather_pin(3 * 24 * 60 * 60);
      // Fallthrough
    }
    case TimelinePinsDemo_OneDayAway:
      prv_add_weather_pin(-2 * 24 * 60 * 60);
      prv_add_weather_pin(2 * 24 * 60 * 60);
      goto timeline;
    case TimelinePinsDemo_OngoingEvent: {
      prv_add_calendar_pin(-(3 * SECONDS_PER_DAY) / 2, 3 * MINUTES_PER_DAY, is_all_day,
                           !recurring, 0, 0);
      goto timeline;
    }
    case TimelinePinsDemo_TodayAndTomorrow:
      prv_add_generic_pin(-24 * 60 * 60, has_subtitle);
      prv_add_weather_pin(-60 * 60);
      prv_add_weather_pin(60 * 60);
      prv_add_generic_pin(24 * 60 * 60, has_subtitle);
      goto timeline;
    case TimelinePinsDemo_Notifications:
      prv_add_notification(-60 * 60 * 24);
      prv_add_notification(-60 * 60);
      prv_add_notification(-60 * 30);
      prv_add_notification(-60 * 5);
      prv_add_notification(-60);
      prv_add_notification(-1);
      goto notifications;
    default:
      // do nothing
      break;
  }
  return;

timeline:
  timeline_event_refresh();
  prv_launch_timeline();
  return;

notifications:
  prv_launch_notifications();
  return;
}

static void prv_menu_select(OptionMenu *option_menu, int selection, void *context) {
  timeline_pins_demo_add_pins(selection);
  app_window_stack_pop(true);
}

static uint16_t prv_menu_get_num_rows(OptionMenu *option_menu, void *context) {
  return ARRAY_LENGTH(timeline_demo_strings);
}

static void prv_menu_draw_row(OptionMenu *option_menu, GContext *ctx, const Layer *cell_layer,
                              const GRect *text_frame, uint32_t row, bool selected, void *context) {
  option_menu_system_draw_row(option_menu, ctx, cell_layer, text_frame,
                              timeline_demo_strings[row], selected, context);
}

static void prv_menu_unload(OptionMenu *option_menu, void *context) {
  option_menu_destroy(option_menu);
}

static void prv_handle_init(void) {
  // add CFLAGS="-DTIMELINE_PIN_SET=OneDayAway" before ./waf configure to skip menu
#ifdef TIMELINE_PIN_SET
  #define PREFIX_PIN_SET(set) (__CONCAT(TimelinePinsDemo, set))
  timeline_pins_demo_add_pins(PREFIX_PIN_SET(TIMELINE_PIN_SET));
#else
  OptionMenu *option_menu = option_menu_create();

  const OptionMenuConfig config = {
    .title = "Select Type of Pins to Add",
    .choice = OPTION_MENU_CHOICE_NONE,
    .status_colors = { GColorDarkGray, GColorWhite },
    .highlight_colors = { GColorLightGray, GColorBlack },
  };
  option_menu_configure(option_menu, &config);
  option_menu_set_callbacks(option_menu, &(OptionMenuCallbacks) {
    .select = prv_menu_select,
    .get_num_rows = prv_menu_get_num_rows,
    .draw_row = prv_menu_draw_row,
    .unload = prv_menu_unload,
  }, option_menu);

  const bool animated = true;
  app_window_stack_push(&option_menu->window, animated);
#endif
}

static void prv_main(void) {
  prv_handle_init();
  app_event_loop();
}

const PebbleProcessMd* timeline_pins_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
      .common = {
          .main_func = prv_main,
          // UUID: c53a79d7-3472-4062-a7d0-39ada9bfa415
          .uuid = {0xc5, 0x3a, 0x79, 0xd7, 0x34, 0x72, 0x40, 0x62,
                   0xa7, 0xd0, 0x39, 0xad, 0xa9, 0xbf, 0xa4, 0x15},
      },
      .name = "Timeline Pins Demo",
  };
  return (const PebbleProcessMd*) &s_app_info;
}
