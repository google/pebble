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

#include "rocky_api_errors.h"
#include "rocky_api_tickservice.h"
#include "rocky_api_util.h"

#include <string.h>

#include "applib/tick_timer_service.h"
#include "rocky_api_global.h"
#include "rocky_api_util.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/size.h"

#define ROCKY_EVENT_SECONDCHANGE "secondchange"
#define ROCKY_EVENT_MINUTECHANGE "minutechange"
#define ROCKY_EVENT_HOURCHANGE   "hourchange"
#define ROCKY_EVENT_DAYCHANGE    "daychange"
#define ROCKY_FIELD_EVENT_DATE "date"

// TODO: PBL-35780 use app_state_get_rocky_runtime_context().context_binding instead
SECTION(".rocky_bss") static TimeUnits s_units;

static void prv_init(void) {
  s_units = (TimeUnits)0;
}

static jerry_value_t prv_create_event(const char *event_name, struct tm *tick_time) {
  JS_VAR event = rocky_global_create_event(event_name);

  JS_VAR date_obj = rocky_util_create_date(tick_time);
  jerry_set_object_field(event, ROCKY_FIELD_EVENT_DATE, date_obj);

  return jerry_acquire_value(event);
}

static const struct {
  const char *event_name;
  TimeUnits time_units;
} s_events[] = {
  {ROCKY_EVENT_SECONDCHANGE,
    // In some scenarios, our C-API doesn't trigger callbacks with just SECOND_UNIT or MINUTE_UNIT
    // if the hour changes. To make the JS-API more conveniently to use without changing the
    // existing C behavior, we subscribe to all "higher" units as well.
    SECOND_UNIT | MINUTE_UNIT | HOUR_UNIT | DAY_UNIT | MONTH_UNIT | YEAR_UNIT},
  {ROCKY_EVENT_MINUTECHANGE,
                  MINUTE_UNIT | HOUR_UNIT | DAY_UNIT | MONTH_UNIT | YEAR_UNIT},
  {ROCKY_EVENT_HOURCHANGE,
                                HOUR_UNIT | DAY_UNIT | MONTH_UNIT | YEAR_UNIT},
  {ROCKY_EVENT_DAYCHANGE,
                                            DAY_UNIT | MONTH_UNIT | YEAR_UNIT},
};

T_STATIC void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  for (size_t i = 0; i < ARRAY_LENGTH(s_events); i++) {
    if (units_changed & s_events[i].time_units) {
      JS_VAR event = prv_create_event(s_events[i].event_name, tick_time);
      rocky_global_call_event_handlers(event);
    }
  }
}

static bool prv_add_handler(const char *event_name, jerry_value_t handler) {
  TimeUnits added_units = (TimeUnits)0;

  for (size_t i = 0; i < ARRAY_LENGTH(s_events); i++) {
    if (strcmp(s_events[i].event_name, event_name) == 0) {
      added_units |= s_events[i].time_units;
      break;
    }
  }
  if (added_units == 0) {
    return false;
  }

  s_units |= added_units;
  tick_timer_service_subscribe(s_units, prv_tick_handler);

  // contract is: we call handler immediately after subscribe once
  JS_VAR event = prv_create_event(event_name, NULL);
  rocky_util_call_user_function_and_log_uncaught_error(handler, jerry_create_undefined(),
                                                       &event, 1);
  return true;
}

const RockyGlobalAPI TICKSERVICE_APIS = {
  .init = prv_init,
  .add_handler = prv_add_handler,
  // TODO: PBL-43380 apparently, we never unsubscribed from tick events…
};
