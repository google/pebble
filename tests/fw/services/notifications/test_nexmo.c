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

#include "clar.h"

#include "services/normal/notifications/ancs/nexmo.h"
#include "services/normal/notifications/ancs/ancs_notifications_util.h"
#include "services/normal/timeline/attributes_actions.h"

// Stubs
////////////////////////////////////////////////////////////////
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"

// Fakes
////////////////////////////////////////////////////////////////
extern const char *NEXMO_REAUTH_STRING;

static AttributeList *s_expected_attributes = NULL;
static TimelineItemActionGroup *s_expected_actions = NULL;
static bool s_performed_store = false;

status_t ios_notif_pref_db_store_prefs(const uint8_t *app_id, int length, AttributeList *attr_list,
                                       TimelineItemActionGroup *action_group) {
  const int buf_size = 256;
  uint8_t buf1[buf_size];
  uint8_t buf2[buf_size];
  attributes_actions_serialize_payload(attr_list, action_group, buf1, buf_size);
  attributes_actions_serialize_payload(s_expected_attributes, s_expected_actions, buf2, buf_size);
  cl_assert_equal_m(buf1, buf2, attributes_actions_get_serialized_payload_size(s_expected_attributes, s_expected_actions));

  s_performed_store = true;
  return S_SUCCESS;
}


static uint32_t s_expected_uid = 0;
static bool s_performed_dismiss = false;

void ancs_perform_action(uint32_t notification_uid, uint8_t action_id) {
  cl_assert_equal_i(notification_uid, s_expected_uid);
  cl_assert_equal_i(action_id, ActionIDNegative);
  s_performed_dismiss = true;
}

void test_nexmo__initialize(void) {
  s_expected_attributes = NULL;
  s_expected_actions = NULL;
  s_performed_store = false;
  s_expected_uid = INVALID_UID;
  s_performed_dismiss = false;
}

void test_nexmo__cleanup(void) {
}

void test_nexmo__is_reauth_sms(void) {
  uint8_t expected_app_id_buf[128];
  ANCSAttribute *expected_app_id = (ANCSAttribute *)&expected_app_id_buf;
  expected_app_id->length = strlen(IOS_SMS_APP_ID);
  memcpy(expected_app_id->value, IOS_SMS_APP_ID, strlen(IOS_SMS_APP_ID));

  char valid_message[128];
  strcpy(valid_message, "possible preamble ");
  strcat(valid_message, NEXMO_REAUTH_STRING);
  strcat(valid_message, " possible postamble");

  uint8_t expected_message_buf[128];
  ANCSAttribute *expected_message = (ANCSAttribute *)&expected_message_buf;
  expected_message->length = strlen(valid_message);
  memcpy(expected_message->value, valid_message, strlen(valid_message));


  uint8_t bad_app_id_buf[128];
  ANCSAttribute *bad_app_id = (ANCSAttribute *)&bad_app_id_buf;
  bad_app_id->length = strlen(IOS_MAIL_APP_ID);
  memcpy(bad_app_id->value, IOS_MAIL_APP_ID, strlen(IOS_MAIL_APP_ID));

  const char *bad_string = "Phil was here";
  uint8_t bad_message_buf[128];
  ANCSAttribute *bad_message = (ANCSAttribute *)&bad_message_buf;
  bad_message->length = strlen(bad_string);
  memcpy(bad_message->value, bad_string, strlen(bad_string));


  cl_assert(nexmo_is_reauth_sms(expected_app_id, expected_message));
  cl_assert(!nexmo_is_reauth_sms(bad_app_id, expected_message));
  cl_assert(!nexmo_is_reauth_sms(expected_app_id, bad_message));
  cl_assert(!nexmo_is_reauth_sms(bad_app_id, bad_message));
}

void test_nexmo__handle_reauth_sms(void) {
  // UID
  const uint32_t uid = 42;
  s_expected_uid = uid;

  // App ID
  uint8_t app_id_buf[128];
  ANCSAttribute *app_id = (ANCSAttribute *)&app_id_buf;
  app_id->length = strlen(IOS_SMS_APP_ID);
  memcpy(app_id->value, IOS_SMS_APP_ID, strlen(IOS_SMS_APP_ID));

  // Message
  char valid_message[128];
  strcpy(valid_message, "possible preamble ");
  strcat(valid_message, NEXMO_REAUTH_STRING);
  strcat(valid_message, " possible postamble");

  uint8_t message_buf[128];
  ANCSAttribute *message = (ANCSAttribute *)&message_buf;
  message->length = strlen(valid_message);
  memcpy(message->value, valid_message, strlen(valid_message));

  // Existing prefs
  iOSNotifPrefs existing_prefs = {
    .attr_list = {
      .num_attributes = 3,
      .attributes = (Attribute[]) {
        { .id = AttributeIdTitle, .cstring = "Title" },
        { .id = AttributeIdBody, .cstring = "Body" },
        { .id = AttributeIdAppName, .cstring = "Awesome" },
      },
    },
  };

  // Make sure that the prefs we store are the existing ones + the reauth msg
  AttributeList expected_attr_list = {
    .num_attributes = 4,
    .attributes = (Attribute[]) {
      { .id = AttributeIdTitle, .cstring = "Title" },
      { .id = AttributeIdBody, .cstring = "Body" },
      { .id = AttributeIdAppName, .cstring = "Awesome" },
      { .id = AttributeIdAuthCode, .cstring = valid_message },
    },
  };
  s_expected_attributes = &expected_attr_list;

  nexmo_handle_reauth_sms(uid, app_id, message, &existing_prefs);
  cl_assert(s_performed_store);
  cl_assert(s_performed_dismiss);
}
