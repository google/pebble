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

#include "phone_call.h"

#include "applib/event_service_client.h"
#include "applib/ui/vibes.h"
#include "comm/ble/kernel_le_client/ancs/ancs.h"
#include "comm/ble/kernel_le_client/ancs/ancs_types.h"
#include "kernel/pbl_malloc.h"
#include "popups/phone_ui.h"
#include "services/common/analytics/analytics.h"
#include "services/common/comm_session/session.h"
#include "services/common/phone_pp.h"
#include "services/common/system_task.h"
#include "services/normal/notifications/alerts.h"
#include "services/normal/notifications/ancs/ancs_phone_call.h"
#include "system/logging.h"

//! This service is a little confusing, but generally here is how the phone calls work:
//! On Android:
//! - The watch gets PP messages (parsed in phone_pp.c), which come in as events happen.
//! - The watch can decline / hangup the call by sending PP messages to the phone.
//! On iOS:
//! - The watch gets incoming calls from ANCS (parsed in ancs_notifications.c).
//! - After that the watch must poll the phone for its status if not iOS 9+ (using PP messages).
//! - On iOS 9, ANCS tells us when the phone stops ringing
//! - The watch can pickup / decline a call using ANCS actions
//! - We don't show the ongoing call UI because we must continue to poll so that we know when the
//!   call ends, which consumes a lot of battery especially for longer calls. On iOS 9, we only
//!   know when the phone stops ringing, we don't know what happens after the user accepts/rejects


static bool s_call_in_progress = false;
static PhoneCallSource s_call_source;

// When using Android this is the cookie, when using ANCS this is the NotificationUUID
static uint32_t s_call_identifier;

// If the mobile app is closed we won't receive PP messages and thus might miss a call end event
// which puts us in a bad state until BT disconnects
static bool s_mobile_app_is_connected;

// We can't expect iOS to reliably send us phone call events, so we must poll for the current
// status of the phone call
static TimerID s_call_watchdog = TIMER_INVALID_ID;


static void prv_handle_call_end(bool disconnected);

static bool prv_call_is_ancs(void) {
  return (s_call_source == PhoneCallSource_ANCS_Legacy) || (s_call_source == PhoneCallSource_ANCS);
}

static void prv_poll_phone_for_status(void *context) {
  pp_get_phone_state();
}

static void prv_timer_callback(void *context) {
  // Make sure we aren't overflowing / backing up the queue too much
  if (system_task_get_available_space() > 10) {
    system_task_add_callback(prv_poll_phone_for_status, context);
  }
}

static void prv_schedule_call_watchdog(int poll_interval_ms) {
  // The Android app currently crashes if it recieves the get_state event. It currently doesn't
  // respond either so don't bother sending messages we don't need to. We also don't need to poll
  // iOS 9 since we can rely on ANCS to tell us when the phone stops ringing
  if (s_call_source == PhoneCallSource_ANCS_Legacy) {
    // Schedule/reschedule the watchdog
    if (!new_timer_start(s_call_watchdog, poll_interval_ms, prv_timer_callback,
                         NULL, TIMER_START_FLAG_REPEATING)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Could not start the phone call watchdog timer");
      prv_handle_call_end(true /* Treat this as a disconnection */);
    } else {
      PBL_LOG(LOG_LEVEL_INFO, "Phone call watchdog timer started");
      pp_get_phone_state_set_enabled(true);
    }
  } else {
    PBL_LOG(LOG_LEVEL_INFO, "Not starting phone call watchdog, this isn't iOS 8: %d",
            s_call_source);
  }
}

static void prv_cancel_call_watchdog(void) {
  new_timer_stop(s_call_watchdog);
  pp_get_phone_state_set_enabled(false);
}

static bool prv_can_answer(void) {
  // We can't answer calls with Android
  return prv_call_is_ancs();
}

static bool prv_should_show_ongoing_call_ui(void) {
  // We only want to show the ongoing call UI on Android
  return (s_call_source == PhoneCallSource_PP);
}

// hangup != decline. Decline == reject incoming call, Hangup == stop in progress call
static bool prv_can_hangup(void) {
  // We can't hangup with iOS
  return !prv_call_is_ancs();
}

// Handles the common things when we hide an incoming call
static void prv_call_end_common(void) {
  s_call_in_progress = false;
  prv_cancel_call_watchdog();
  analytics_stopwatch_stop(ANALYTICS_DEVICE_METRIC_PHONE_CALL_TIME);
}

static void prv_handle_incoming_call(const PebblePhoneEvent *event) {
  // Only 1 call at a time is supported
  if (s_call_in_progress) {
    PBL_LOG(LOG_LEVEL_INFO, "Ignoring incoming call. A call is already in progress");
    return;
  }

  // If we're not on iOS9+, we need to be connected to the mobile app since it tells us when
  // the phone has stopped ringing
  if ((event->source != PhoneCallSource_ANCS) && !s_mobile_app_is_connected) {
    PBL_LOG(LOG_LEVEL_INFO, "Ignoring incoming call. Mobile app is not connected. Call source: %d ",
            event->source);
    return;
  }

  s_call_in_progress = true;
  s_call_source = event->source;
  s_call_identifier = event->call_identifier;

  prv_schedule_call_watchdog(600);

  phone_ui_handle_incoming_call(event->caller, prv_can_answer(), prv_should_show_ongoing_call_ui(),
                                s_call_source);
  analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_INCOMING_COUNT, AnalyticsClient_System);
  analytics_stopwatch_start(ANALYTICS_DEVICE_METRIC_PHONE_CALL_TIME, AnalyticsClient_System);
}

static void prv_handle_outgoing_call(PebblePhoneEvent *event) {
  // Only 1 call at a time is supported
  if (!s_call_in_progress && s_mobile_app_is_connected) {
    phone_ui_handle_outgoing_call(event->caller);
    analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_OUTGOING_COUNT, AnalyticsClient_System);
  } else {
    // PBL_LOG(LOG_LEVEL_DEBUG, "Ignoring outgoing call. A call is already in progress: %d, "
    //     "the mobile app is connected: %d", s_call_in_progress, s_mobile_app_is_connected);
  }
}

static void prv_handle_missed_call(PebblePhoneEvent *event) {
  if (s_call_in_progress) {
    prv_call_end_common();
    phone_ui_handle_missed_call();
    analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_INCOMING_COUNT, AnalyticsClient_System);
  } else {
    // PBL_LOG(LOG_LEVEL_DEBUG, "Ignoring missed call. A call is not in progress");
  }
}

static void prv_handle_call_start(void) {
  if (s_call_in_progress) {
    if (prv_call_is_ancs()) {
      // We don't show an ongoing call UI on iOS, so from this service's point of view the call is
      // now complete.
      prv_call_end_common();
      phone_ui_handle_call_end(true /*call accepted*/, false /*disconnected*/);
    } else {
      phone_ui_handle_call_start(prv_can_hangup());
    }
    analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_START_COUNT, AnalyticsClient_System);
  } else {
    PBL_LOG(LOG_LEVEL_INFO, "Ignoring start call. A call is not in progress");
  }
}

static void prv_handle_call_hide(PebblePhoneEvent *event) {
  if (!s_call_in_progress) {
    return;
  }

  // Make sure this wasn't caused due to an unrelated ANCS removal
  if (prv_call_is_ancs() && (s_call_identifier != event->call_identifier)) {
    PBL_LOG(LOG_LEVEL_INFO, "Ignoring hide call. Call identifier %"PRIu32" doesn't match %"PRIu32,
            s_call_identifier, event->call_identifier);
    return;
  }

  prv_call_end_common();
  phone_ui_handle_call_hide();
  analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_END_COUNT, AnalyticsClient_System);
}

static void prv_handle_call_end(bool disconnected) {
  if (!disconnected) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_END_COUNT, AnalyticsClient_System);
  }

  if (s_call_in_progress) {
    prv_call_end_common();
    phone_ui_handle_call_end(false /*call accepted*/, disconnected);
  } else if (!disconnected) {
    PBL_LOG(LOG_LEVEL_INFO, "Ignoring end call. A call is not in progress");
  }
}

static void prv_handle_caller_id(PebblePhoneEvent *event) {
  if (s_call_in_progress) {
    phone_ui_handle_caller_id(event->caller);
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Ignoring caller id. A call is not in progress");
  }
}

T_STATIC void prv_handle_phone_event(PebbleEvent *e, void *context) {
  PebblePhoneEvent event = (PebblePhoneEvent) e->phone;

  if (!alerts_should_notify_for_type(AlertPhoneCall)) {
    prv_handle_call_end(true /* disconnected */);
    phone_call_util_destroy_caller(event.caller);
    return;
  }

  if (!(event.type == PhoneEventType_Incoming && new_timer_scheduled(s_call_watchdog, NULL))) {
    // Be careful not to spam the logs with the new iOS polling implementation
    PBL_LOG(LOG_LEVEL_INFO, "PebblePhoneEvent: %d, Call in progress: %s, Connected: %s",
      event.type, s_call_in_progress ? "T": "F", s_mobile_app_is_connected ? "T": "F");
  }

  switch (event.type) {
    case PhoneEventType_Incoming:
      prv_handle_incoming_call(&event);
      break;
    case PhoneEventType_Outgoing:
      prv_handle_outgoing_call(&event);
      break;
    case PhoneEventType_Missed:
      prv_handle_missed_call(&event);
      break;
    case PhoneEventType_Ring:
      // Just ignore these. We can ring on our own.
      break;
    case PhoneEventType_Start:
      prv_handle_call_start();
      break;
    case PhoneEventType_End:
      prv_handle_call_end(false /* disconnected */);
      break;
    case PhoneEventType_CallerID:
      prv_handle_caller_id(&event);
      break;
    case PhoneEventType_Disconnect:
      prv_handle_call_end(true /* disconnected */);
      break;
    case PhoneEventType_Hide:
      prv_handle_call_hide(&event);
      break;
    case PhoneEventType_Invalid:
      break;
  }

  phone_call_util_destroy_caller(event.caller);
}

T_STATIC void prv_handle_mobile_app_event(PebbleEvent *e, void *context) {
  if (!e->bluetooth.comm_session_event.is_system) {
    return;
  }

  s_mobile_app_is_connected = e->bluetooth.comm_session_event.is_open;
  if (!s_mobile_app_is_connected && (s_call_source != PhoneCallSource_ANCS)) {
    prv_handle_call_end(true /* disconnected */);
  }
}

T_STATIC void prv_handle_ancs_disconnected_event(PebbleEvent *e, void *context) {
  if (s_call_source == PhoneCallSource_ANCS) {
    prv_handle_call_end(true /* disconnected */);
  }
}

//!
//! Phone Call API
//!
void phone_call_service_init() {
  static EventServiceInfo phone_event_info;
  phone_event_info = (EventServiceInfo) {
    .type = PEBBLE_PHONE_EVENT,
    .handler = prv_handle_phone_event,
  };
  event_service_client_subscribe(&phone_event_info);

  static EventServiceInfo mobile_app_event_info;
  mobile_app_event_info = (EventServiceInfo) {
    .type = PEBBLE_COMM_SESSION_EVENT,
    .handler = prv_handle_mobile_app_event,
  };
  event_service_client_subscribe(&mobile_app_event_info);

  static EventServiceInfo ancs_disconnected_event_info;
  ancs_disconnected_event_info = (EventServiceInfo) {
    .type = PEBBLE_ANCS_DISCONNECTED_EVENT,
    .handler = prv_handle_ancs_disconnected_event,
  };
  event_service_client_subscribe(&ancs_disconnected_event_info);

  s_mobile_app_is_connected = (comm_session_get_system_session() != NULL);

  s_call_watchdog = new_timer_create();
}

void phone_call_answer(void) {
  analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_ANSWER_COUNT, AnalyticsClient_System);
  PBL_LOG(LOG_LEVEL_INFO, "Call accepted");

  if (prv_call_is_ancs()) {
    ancs_perform_action(s_call_identifier, ActionIDPositive);

    // We don't show an ongoing call UI on iOS, so from this service's point of view the call is
    // now complete.
    prv_call_end_common();
  } else {
    pp_answer_call(s_call_identifier);
  }
}

void phone_call_decline(void) {
  analytics_inc(ANALYTICS_DEVICE_METRIC_PHONE_CALL_DECLINE_COUNT, AnalyticsClient_System);
  PBL_LOG(LOG_LEVEL_INFO, "Call declined");

  if (prv_call_is_ancs()) {
    ancs_perform_action(s_call_identifier, ActionIDNegative);
    ancs_phone_call_temporarily_block_missed_calls();
    prv_cancel_call_watchdog();
  } else {
    pp_decline_call(s_call_identifier);
  }

  if (s_call_in_progress) {
    s_call_in_progress = false;
    analytics_stopwatch_stop(ANALYTICS_DEVICE_METRIC_PHONE_CALL_TIME);
  }
}
