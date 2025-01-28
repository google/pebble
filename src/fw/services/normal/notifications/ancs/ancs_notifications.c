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

#include "ancs_notifications.h"

#include "ancs_filtering.h"
#include "ancs_item.h"
#include "ancs_phone_call.h"
#include "ancs_notifications_util.h"
#include "nexmo.h"

#include "comm/ble/kernel_le_client/ancs/ancs_types.h"
#include "drivers/rtc.h"
#include "kernel/pbl_malloc.h"
#include "services/common/analytics/analytics.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/blob_db/ios_notif_pref_db.h"
#include "services/normal/blob_db/pin_db.h"
#include "services/normal/blob_db/reminder_db.h"
#include "services/normal/notifications/notification_storage.h"
#include "services/normal/notifications/notifications.h"
#include "services/normal/timeline/timeline.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/pstring.h"

#include <stdio.h>

static const Uuid uuid_reminders_data_source = UUID_REMINDERS_DATA_SOURCE;

static void prv_dismiss_notification(const TimelineItem *notification) {
  const TimelineItemAction *action = timeline_item_find_dismiss_action(notification);

  if (action) {
    timeline_invoke_action(notification, action, NULL);
  } else {
    char uuid_buffer[UUID_STRING_BUFFER_LENGTH];
    uuid_to_string(&notification->header.id, uuid_buffer);
    PBL_LOG(LOG_LEVEL_ERROR, "Failed to load action for dismissal from %s", uuid_buffer);
  }
}

static void prv_handle_new_ancs_notif(TimelineItem *notification) {
  uuid_generate(&notification->header.id);
  notifications_add_notification(notification);
  timeline_item_destroy(notification);
}

static void prv_handle_ancs_update(TimelineItem *notification,
                                   CommonTimelineItemHeader *existing_header) {
  if (existing_header->dismissed) {
    // this should be dismissed from iOS. Dismiss it again
    PBL_LOG(LOG_LEVEL_DEBUG,
            "ANCS notification already dismissed, dismissing again: %"PRIu32,
            notification->header.ancs_uid);
    prv_dismiss_notification(notification);
  }

  notification->header.status = existing_header->status;
  notification->header.id = existing_header->id;

  // Replace existing version of the notification
  notification_storage_remove(&notification->header.id);
  notification_storage_store(notification);

  // we won't use this anywhere, free up the memory
  timeline_item_destroy(notification);
}

static time_t prv_get_timestamp_from_ancs_date(const ANCSAttribute *date,
                                               const ANCSAttribute *app_id) {
  time_t timestamp = ancs_notifications_util_parse_timestamp(date);
  if (timestamp == 0) {
    // Another ANCS / iOS quirk, some apps (i.e. the Phone app)
    // send an invalid-length string as date...
    // Apple rdar://19639333
    timestamp = rtc_get_time();

    // copy out app ID to a char buffer
    char app_id_buffer[app_id->length + 1];
    pstring_pstring16_to_string(&app_id->pstr, app_id_buffer);
    PBL_LOG(LOG_LEVEL_WARNING, "No valid date. Offending iOS app: %s", app_id_buffer);
  }

  return timestamp;
}

static bool prv_should_ignore_because_calendar_reminder(const ANCSAttribute *app_id) {
  // do not show calendar notifications if we have reminders set (PBL-13271)
  return pstring_equal_cstring(&app_id->pstr, IOS_CALENDAR_APP_ID) && !reminder_db_is_empty();
}

static bool prv_reminder_filter(SerializedTimelineItemHeader *hdr, void *context) {
  // Check that the data source is the reminders app
  TimelineItem pin;
  pin_db_read_item_header(&pin, &hdr->common.parent_id);
  if (uuid_equal(&pin.header.parent_id, &uuid_reminders_data_source)) {
    return true;
  }
  return false;
}

static bool prv_should_ignore_because_time_reminder(const ANCSAttribute *app_id,
                                                    time_t timestamp,
                                                    const ANCSAttribute *title,
                                                    uint32_t uid,
                                                    const ANCSAttribute *attr_action_neg) {
  if (!pstring_equal_cstring(&app_id->pstr, IOS_REMINDERS_APP_ID)) {
    return false;
  }

  // copy out reminder title to a char buffer
  char reminder_title_buffer[title->length + 1];
  pstring_pstring16_to_string(&title->pstr, reminder_title_buffer);

  TimelineItem reminder;

  // If we found an existing reminder, replace its dismiss action with ancs negative action
  if (reminder_db_find_by_timestamp_title(timestamp, reminder_title_buffer, prv_reminder_filter,
                                          &reminder)) {
    ancs_item_update_dismiss_action(&reminder, uid, attr_action_neg);

    // Overwrite the existing item and notify system that reminder was updated
    reminder_db_insert_item(&reminder);
    timeline_item_free_allocated_buffer(&reminder);
    return true;
  }
  return false;
}

static bool prv_find_existing_notification(TimelineItem *notification,
                                           CommonTimelineItemHeader *existing_header_out) {
  // PBL-9509: iOS' Calendar app uses the timestamp of the ANCS notification for the time of the
  // event, not the time the notification was sent. If a calendar event has multiple notifications,
  // for example, 15 mins before and 5 minutes before, ANCS will send 2x ANCS notifications.
  // Because our dupe detection is based on the ANCS timestamp field, it filters out any calendar
  // event reminder following the initial one. Therefore, skip dupe filtering for ANCS
  // notifications from the calendar app.
  // Also note that the 1st will be "removed" by ANCS as the 2nd gets sent out. Therefore, we do not
  // need to do anything special to "clean up" any previous reminders for the same event.
  if (notification->header.layout == LayoutIdCalendar) {
    return false;
  }

  // Check if the notification is a duplicate/update
  return notification_storage_find_ancs_notification_by_timestamp(notification,
                                                                  existing_header_out);
}

static bool prv_should_ignore_because_duplicate(TimelineItem *notification,
                                                CommonTimelineItemHeader *existing_header) {
  return (notification->header.ancs_uid == existing_header->ancs_uid);
}

static bool prv_should_ignore_because_apple_mail_dot_app_bug(const ANCSAttribute *app_id,
                                                             const ANCSAttribute *message) {
  // Apple's Mail.app sometimes sends a notification with "Loading..." or
  // "This message has no content." when Mail.app is still fetching the body of the email.
  // PBL-8407 / PBL-1090 / Apple bug report number: rdr://17851582
  // Obviously this only works around the issue when the language is set to English.
  if (!pstring_equal_cstring(&app_id->pstr, IOS_MAIL_APP_ID)) {
    return false;
  }
  static const char loading_str[] = "Loading\xe2\x80\xa6";
  if (pstring_equal_cstring(&message->pstr, loading_str)) {
    return true;
  }
  static const char no_content_str[] = "This message has no content.";
  if (pstring_equal_cstring(&message->pstr, no_content_str)) {
    return true;
  }
  return false;
}

static bool prv_should_ignore_because_stale(time_t timestamp) {
  static const time_t MAXIMUM_NOTIFY_TIME = 2 * 60 * 60; // 2 hours
  static const time_t INVALID_TIME = ~0;

  const time_t now = rtc_get_time();
  // workaround for PBL-8400 (ignore notifications older than 2 hours)
  // PBL-9066: Increased to 20 minutes due to Mail.app only fetching emails every 15 minutes
  // PBL-9251: Increased to 2 hours. People have Fetch set to hourly.
  // PBL-12726: Added a check to see if the timestamp is coming from a location based reminder
  // This work-around is causing more trouble than the problem it was solving...
  if (timestamp < (now - MAXIMUM_NOTIFY_TIME) && timestamp != INVALID_TIME) {
    PBL_LOG(LOG_LEVEL_INFO, "Not presenting stale notif (ts=%ld)", timestamp);
    return true;
  }

  return false;
}

static bool prv_should_ignore_because_muted(const iOSNotifPrefs *app_notif_prefs) {
  return ancs_filtering_is_muted(app_notif_prefs);
}

static bool prv_should_ignore_notification(uint32_t uid,
                                           time_t timestamp,
                                           ANCSAttribute **notif_attributes,
                                           iOSNotifPrefs *app_notif_prefs) {
  const ANCSAttribute *app_id = notif_attributes[FetchedNotifAttributeIndexAppID];
  const ANCSAttribute *message = notif_attributes[FetchedNotifAttributeIndexMessage];
  const ANCSAttribute *title = notif_attributes[FetchedNotifAttributeIndexTitle];
  const ANCSAttribute *negative_action =
      notif_attributes[FetchedNotifAttributeIndexNegativeActionLabel];

  if (prv_should_ignore_because_muted(app_notif_prefs)) {
    char app_id_buffer[app_id->length + 1];
    pstring_pstring16_to_string(&app_id->pstr, app_id_buffer);

    PBL_LOG(LOG_LEVEL_INFO, "Ignoring notification from <%s>: Muted", app_id_buffer);
    analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_FILTERED_BECAUSE_MUTED_COUNT,
                  AnalyticsClient_System);
    return true;
  }

  // filter out mail buggy mail messages
  if (prv_should_ignore_because_apple_mail_dot_app_bug(app_id, message)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Ignoring ANCS notification because Mail.app bug");
    return true;
  }

  // Calendar and time-based Reminders app reminders are handled through the mobile app and are
  // added to reminder_db, so we filter them out to avoid doubling up. Notifications from other
  // apps and location-based reminder are handled as regular notifications.

  // filter out extraneous calendar messages
  if (prv_should_ignore_because_calendar_reminder(app_id)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Ignoring ANCS calendar notification because reminders are set");
    return true;
  }

  // filter out time based reminder notifications
  if (prv_should_ignore_because_time_reminder(app_id, timestamp, title, uid,
                                              negative_action)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Ignoring ANCS reminders notification because existing "
            "time-based reminder was found in db");
    return true;
  }

  // filter out super-old notifications
  if (prv_should_ignore_because_stale(timestamp)) {
    return true;
  }

  return false;
}

void ancs_notifications_handle_message(uint32_t uid,
                                       ANCSProperty properties,
                                       ANCSAttribute **notif_attributes,
                                       ANCSAttribute **app_attributes) {
  PBL_ASSERTN(notif_attributes && app_attributes);

  const ANCSAttribute *app_id = notif_attributes[FetchedNotifAttributeIndexAppID];
  if (!app_id || app_id->length == 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Can't handle notifications without an app id");
    return;
  }

  const ANCSAttribute *title = notif_attributes[FetchedNotifAttributeIndexTitle];
  const ANCSAttribute *subtitle = notif_attributes[FetchedNotifAttributeIndexSubtitle];
  const ANCSAttribute *display_name = app_attributes[FetchedAppAttributeIndexDisplayName];
  const ANCSAttribute *date = notif_attributes[FetchedNotifAttributeIndexDate];
  const ANCSAttribute *message = notif_attributes[FetchedNotifAttributeIndexMessage];

  iOSNotifPrefs *app_notif_prefs = ios_notif_pref_db_get_prefs(app_id->value, app_id->length);
  ancs_filtering_record_app(&app_notif_prefs, app_id, display_name, title);

  if (nexmo_is_reauth_sms(app_id, message)) {
    nexmo_handle_reauth_sms(uid, app_id, message, app_notif_prefs);
    goto cleanup;
  }

  const time_t timestamp = prv_get_timestamp_from_ancs_date(date, app_id);

  if (prv_should_ignore_notification(uid, timestamp, notif_attributes, app_notif_prefs)) {
    goto cleanup;
  }

  // If this is an incoming call, let the phone service handle it
  // It would be nice to handle facetime calls with the phone service but it doesn't look like we
  // can: https://pebbletechnology.atlassian.net/browse/PBL-16955
  const bool is_notification_from_phone_app = ancs_notifications_util_is_phone(app_id);
  const bool has_incoming_call_property = (properties & ANCSProperty_IncomingCall);
  const bool has_missed_call_property = (properties & ANCSProperty_MissedCall);
  if (is_notification_from_phone_app) {
    if (has_incoming_call_property) {
      ancs_phone_call_handle_incoming(uid, properties, notif_attributes);
      goto cleanup;
    }

    // When declining a phone call from the Phone UI we still get a missed call notification
    // with a different UID. We don't want to show a missed call notification / pin in this case.
    if (has_missed_call_property && ancs_phone_call_should_ignore_missed_calls()) {
      PBL_LOG(LOG_LEVEL_INFO, "Ignoring missed call");
      goto cleanup;
    }
  }

  const bool is_sms = ancs_notifications_util_is_sms(app_id);
  if (is_sms) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_SMS_COUNT,
                  AnalyticsClient_System);
  }
  if (ancs_notifications_util_is_group_sms(app_id, subtitle)) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_GROUP_SMS_COUNT,
                  AnalyticsClient_System);
  }

  // add a notification
  const ANCSAppMetadata* app_metadata = ancs_notifications_util_get_app_metadata(app_id);
  TimelineItem *notification = ancs_item_create_and_populate(notif_attributes, app_attributes,
                                                             app_metadata, app_notif_prefs,
                                                             timestamp, properties);
  if (!notification) { goto cleanup; }
  notification->header.ancs_uid = uid;
  notification->header.type = TimelineItemTypeNotification;
  notification->header.layout = LayoutIdNotification;
  notification->header.ancs_notif = true;

  notification_storage_lock();
  // filter out duplicate notifications
  CommonTimelineItemHeader existing_header;
  if (prv_find_existing_notification(notification, &existing_header)) {
    if (prv_should_ignore_because_duplicate(notification, &existing_header)) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Duplicate ANCS notification: %"PRIu32, uid);
      timeline_item_destroy(notification);
      notification_storage_unlock();
      goto cleanup;
    }
    prv_handle_ancs_update(notification, &existing_header);
  } else {
    prv_handle_new_ancs_notif(notification);
  }

  notification_storage_unlock();

  // if missed call, also add a pin
  if (is_notification_from_phone_app && has_missed_call_property) {
    TimelineItem *missed_call_pin =
        ancs_item_create_and_populate(notif_attributes, app_attributes, app_metadata,
                                      app_notif_prefs, timestamp, properties);
    if (missed_call_pin == NULL) { goto cleanup; }
    timeline_add_missed_call_pin(missed_call_pin, uid);
    timeline_item_destroy(missed_call_pin);
  }

cleanup:
  ios_notif_pref_db_free_prefs(app_notif_prefs);
}

void ancs_notifications_handle_notification_removed(uint32_t ancs_uid, ANCSProperty properties) {
  // Dismissal from phone is only properly supported on iOS 9 and up
  // The presence of the DIS service tells us we have at least iOS 9
  const bool ios_9 = (properties & ANCSProperty_iOS9);
  if (!ios_9) {
    return;
  }

  Uuid *notification_id = kernel_malloc_check(sizeof(Uuid));

  if (notification_storage_find_ancs_notification_id(ancs_uid, notification_id)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Notification removed from notification centre: (UID: %"PRIu32")",
            ancs_uid);
    notification_storage_set_status(notification_id, TimelineItemStatusDismissed);

    notifications_handle_notification_acted_upon(notification_id);
  } else {
    // notification_id is passed into an event if a matching notification was found, so it will
    // be freed by the system later
    kernel_free(notification_id);
  }

  if (properties & ANCSProperty_IncomingCall) {
    ancs_phone_call_handle_removed(ancs_uid, ios_9);
  }
}
