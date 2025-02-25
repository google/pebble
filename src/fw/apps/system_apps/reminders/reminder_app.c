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

#include "reminder_app.h"
#include "reminder_app_prefs.h"

#include "applib/app.h"
#include "applib/ui/dialogs/simple_dialog.h"
#include "applib/ui/ui.h"
#include "applib/voice/transcription_dialog.h"
#include "applib/voice/voice_window.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "resource/timeline_resource_ids.auto.h"
#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/common/clock.h"
#include "services/common/comm_session/session_remote_version.h"
#include "services/common/i18n/i18n.h"
#include "services/normal/timeline/timeline.h"
#include "services/normal/blob_db/watch_app_prefs_db.h"
#include "util/time/time.h"

#include "system/logging.h"

typedef enum ReminderAppUIState {
  ReminderAppUIState_Start,
  ReminderAppUIState_WaitForDictationEvent,
  ReminderAppUIState_Exit,
} ReminderAppUIState;

typedef struct ReminderAppData {
  Window window;
  VoiceWindow *voice_window;
  EventServiceInfo event_service_info;
  TranscriptionDialog transcription_dialog;
  char *dialog_text;
  char *reminder_str;
  time_t timestamp;
  ReminderAppUIState ui_state;
} ReminderAppData;

static void prv_create_reminder(ReminderAppData *data) {
  AttributeList pin_attr_list = {0};
  attribute_list_add_uint32(&pin_attr_list, AttributeIdIconTiny,
                            TIMELINE_RESOURCE_NOTIFICATION_REMINDER);
  attribute_list_add_cstring(&pin_attr_list, AttributeIdTitle, data->reminder_str);
  attribute_list_add_uint8(&pin_attr_list, AttributeIdBgColor, GColorChromeYellowARGB8);

  AttributeList completed_attr_list = {0};
  attribute_list_add_cstring(&completed_attr_list, AttributeIdTitle,
                             i18n_get("Completed", &pin_attr_list));

  AttributeList postpone_attr_list = {0};
  attribute_list_add_cstring(&postpone_attr_list, AttributeIdTitle,
                             i18n_get("Postpone", &pin_attr_list));

  AttributeList remove_attr_list = {0};
  attribute_list_add_cstring(&remove_attr_list, AttributeIdTitle,
                             i18n_get("Remove", &pin_attr_list));

  const int num_actions = 3;
  TimelineItemActionGroup action_group = {
    .num_actions = num_actions,
    .actions = (TimelineItemAction[]) {
      {
        .id = 0,
        .type = TimelineItemActionTypeComplete,
        .attr_list = completed_attr_list,
      },
      {
        .id = 1,
        .type = TimelineItemActionTypePostpone,
        .attr_list = postpone_attr_list,
      },
      {
        .id = 2,
        .type = TimelineItemActionTypeRemoteRemove,
        .attr_list = remove_attr_list,
      }
    },
  };

  TimelineItem *item = timeline_item_create_with_attributes(data->timestamp,
                                                            0, // duration
                                                            TimelineItemTypePin,
                                                            LayoutIdGeneric,
                                                            &pin_attr_list,
                                                            &action_group);
  item->header.from_watch = true;
  item->header.parent_id = (Uuid)UUID_REMINDERS_DATA_SOURCE;
  timeline_add(item);

  // Tweak the item before adding the reminder
  item->header.parent_id = item->header.id;
  uuid_generate(&item->header.id);
  item->header.type = TimelineItemTypeReminder;
  item->header.layout = LayoutIdReminder;
  reminders_insert(item);

  i18n_free_all(&pin_attr_list);
  attribute_list_destroy_list(&pin_attr_list);
  attribute_list_destroy_list(&completed_attr_list);
  attribute_list_destroy_list(&postpone_attr_list);
  attribute_list_destroy_list(&remove_attr_list);
  timeline_item_destroy(item);
}

static void prv_push_success_dialog(void) {
  SimpleDialog *simple_dialog = simple_dialog_create("Reminder Added");
  Dialog *dialog = simple_dialog_get_dialog(simple_dialog);
  dialog_set_text(dialog, i18n_get("Added", dialog));
  dialog_set_icon(dialog, RESOURCE_ID_GENERIC_REMINDER_LARGE);
  dialog_set_background_color(dialog, GColorChromeYellow);
  dialog_set_timeout(dialog, DIALOG_TIMEOUT_DEFAULT);
  app_simple_dialog_push(simple_dialog);
  i18n_free_all(dialog);
}

static void prv_confirm_cb(void *context) {
  ReminderAppData *data = context;
  data->ui_state = ReminderAppUIState_Exit;
  prv_create_reminder(data);
  prv_push_success_dialog();
}

static void prv_push_transcription_dialog(ReminderAppData *data) {
  TranscriptionDialog *transcription_dialog = &data->transcription_dialog;
  transcription_dialog_init(transcription_dialog);
  transcription_dialog_update_text(transcription_dialog,
                                   data->dialog_text,
                                   strlen(data->dialog_text));
  transcription_dialog_set_callback(transcription_dialog, prv_confirm_cb, data);
  Dialog *dialog = expandable_dialog_get_dialog((ExpandableDialog *)transcription_dialog);
  dialog_set_destroy_on_pop(dialog, false /* free_on_pop */);
  app_transcription_dialog_push(transcription_dialog);
}

static void prv_build_transcription_dialog_text(ReminderAppData *data) {
  if (data->dialog_text) {
    app_free(data->dialog_text);
  }
  const size_t sentence_len = strlen(data->reminder_str);
  const size_t date_time_len = 32; // "September 19th 9:05pm", "Yesterday 12:33pm"
  const size_t required_buf_size = sentence_len + date_time_len + 2 /* \n\n */ + 1 /* \0 */;
  data->dialog_text = app_zalloc_check(required_buf_size);

  // The string that is being built below looks something like:
  // "Take out the trash"
  //
  // "Tomorrow 7:00AM"

  int buf_space_remaining = required_buf_size - 1 /*for the final \0 */;
  strncpy(data->dialog_text, data->reminder_str, sentence_len);
  // Having to call MAX every time is a bit silly, but the strn function expect a size_t (unsigned).
  // Calling MAX ensures that a negative value isn't passed in which gets cast to something positive
  buf_space_remaining = MAX(buf_space_remaining - sentence_len, 0);

  strncat(data->dialog_text, "\n\n", buf_space_remaining);
  buf_space_remaining = MAX(buf_space_remaining - 2, 0);


  char tmp[date_time_len];
  clock_get_friendly_date(tmp, date_time_len, data->timestamp);
  strncat(data->dialog_text, tmp, buf_space_remaining);
  buf_space_remaining = MAX(buf_space_remaining - strlen(tmp), 0);
  strncat(data->dialog_text, " ", buf_space_remaining);
  buf_space_remaining = MAX(buf_space_remaining - 1, 0);;

  clock_get_time_number(tmp, date_time_len, data->timestamp);
  strncat(data->dialog_text, tmp, buf_space_remaining);
  buf_space_remaining = MAX(buf_space_remaining - strlen(tmp), 0);

  clock_get_time_word(tmp, date_time_len, data->timestamp);
  strncat(data->dialog_text, tmp, buf_space_remaining);
}

static void prv_handle_dictation_event(PebbleEvent *e, void *context) {
  ReminderAppData *data = context;
  const DictationSessionStatus status = e->dictation.result;

  if (status == DictationSessionStatusSuccess) {
    if (data->reminder_str) {
      app_free(data->reminder_str);
    }
    const size_t reminder_str_len = strlen(e->dictation.text);
    data->reminder_str = app_zalloc_check(reminder_str_len + 1 /* \0 */);
    strcpy(data->reminder_str, e->dictation.text);
    data->reminder_str[reminder_str_len] = '\0';

    data->timestamp = e->dictation.timestamp;
    if (data->timestamp == 0) {
      // If the user didn't specify a time set it to be 1 hour from the current time,
      // rounded up to the nearest 15 min.
      // Ex: a reminder created at 10:08 AM with no specified time is due at 11:15 AM
      time_t utc_sec = rtc_get_time() + SECONDS_PER_HOUR + (15 * SECONDS_PER_MINUTE);
      struct tm local_tm;
      localtime_r(&utc_sec, &local_tm);
      local_tm.tm_min -= (local_tm.tm_min % 15);
      local_tm.tm_sec = 0;
      data->timestamp = mktime(&local_tm);
    }

    // If the user doesn't accept the transcription, try again.
    data->ui_state = ReminderAppUIState_Start;

    prv_build_transcription_dialog_text(data);

    prv_push_transcription_dialog(data);
  } else {
    // Exit immediately because this event may or may not be handled before the main window appears.
    data->ui_state = ReminderAppUIState_Exit;
    app_window_stack_pop_all(false);
  }
}

static void prv_appear(struct Window *window) {
  ReminderAppData *data = app_state_get_user_data();
  switch (data->ui_state) {
    case ReminderAppUIState_Start:
      // Start a transcription
      data->ui_state = ReminderAppUIState_WaitForDictationEvent;
      voice_window_reset(data->voice_window);
      voice_window_push(data->voice_window);
      break;
    case ReminderAppUIState_WaitForDictationEvent:
      break;
    case ReminderAppUIState_Exit:
      app_window_stack_pop_all(false);
      break;
    default:
      WTF;
  }
}

static NOINLINE void prv_init(void) {
  ReminderAppData *data = app_zalloc_check(sizeof(ReminderAppData));
  app_state_set_user_data(data);

  data->ui_state = ReminderAppUIState_Start;

  // This "background" window is needed because without voice confirmation enabled,
  // the voice window pops before we get the event and can push the transcription dialog.
  // This means we have no windows for a moment and thus the app deinits.
  // This window is now also used to catch a 'back' at the confirmation dialog.
  Window *window = &data->window;
  window_init(window, WINDOW_NAME("Reminders"));

  WindowHandlers handlers = { .appear = prv_appear, };
  window_set_window_handlers(window, &handlers);

  data->event_service_info = (EventServiceInfo) {
    .type = PEBBLE_DICTATION_EVENT,
    .handler = prv_handle_dictation_event,
    .context = data,
  };
  event_service_client_subscribe(&data->event_service_info);

  data->voice_window = voice_window_create(NULL, 0, VoiceEndpointSessionTypeNLP);
  voice_window_set_confirmation_enabled(data->voice_window, false);

  // Let the main window manage the voice window
  app_window_stack_push(window, false);
}

static void prv_deinit(void) {
  ReminderAppData *data = app_state_get_user_data();
  voice_window_destroy(data->voice_window);
  event_service_client_unsubscribe(&data->event_service_info);
  app_free(data->dialog_text);
  app_free(data);
}

static void prv_main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}

const PebbleProcessMd* reminder_app_get_info(void) {
  PebbleProtocolCapabilities capabilities;
  bt_persistent_storage_get_cached_system_capabilities(&capabilities);
  SerializedReminderAppPrefs *prefs = watch_app_prefs_get_reminder();

  const bool is_visible_in_launcher = capabilities.reminders_app_support &&
                          (prefs ? (prefs->appState == ReminderAppState_Enabled) : false);

  task_free(prefs);

  static const PebbleProcessMdSystem s_reminder_app_info = {
    .common = {
      .main_func = prv_main,
      .uuid = UUID_REMINDERS_DATA_SOURCE,
    },
    .name = i18n_noop("Reminder"),
#if CAPABILITY_HAS_APP_GLANCES
    .icon_resource_id = RESOURCE_ID_GENERIC_REMINDER_TINY,
#endif
  };

  return is_visible_in_launcher ? (const PebbleProcessMd *)&s_reminder_app_info : NULL;
}
