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

#include "comm/ble/kernel_le_client/ancs/ancs.h"
#include "comm/ble/kernel_le_client/ancs/ancs_util.h"
#include "comm/ble/kernel_le_client/ancs/ancs_definition.h"

#include "comm/ble/gap_le_connection.h"
#include "comm/ble/gap_le_task.h"

#include "services/common/evented_timer.h"
#include "services/common/regular_timer.h"
#include "services/normal/notifications/ancs/ancs_notifications.h"
#include "util/size.h"

#include "clar.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_analytics.h"
#include "stubs_ios_notif_pref_db.h"
#include "stubs_bt_stack.h"
#include "stubs_ble.h"
#include "stubs_pin_db.h"
#include "stubs_i18n.h"
#include "stubs_layout_layer.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pebble_tasks.h"
#include "stubs_pebble_pairing_service.h"
#include "stubs_prompt.h"
#include "stubs_timeline.h"
#include "stubs_queue.h"
#include "stubs_rand_ptr.h"
#include "stubs_reconnect.h"
#include "stubs_reminder_db.h"
#include "stubs_reminders.h"
#include "stubs_serial.h"
#include "stubs_sleep.h"
#include "stubs_system_reset.h"
#include "stubs_task_watchdog.h"
#include "stubs_nexmo.h"
#include "stubs_codepoint.h"
#include "stubs_utf8.h"

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

PebblePhoneCaller* phone_call_util_create_caller(const char *number, const char *name) {
  return NULL;
}

// Fakes
///////////////////////////////////////////////////////////

#include "fake_events.h"
#include "fake_kernel_services_notifications.h"
#include "fake_new_timer.h"
#include "fake_notification_storage.h"
#include "fake_pbl_malloc.h"
#include "fake_spi_flash.h"

bool shell_prefs_get_language_english(void) {
  return false;
}

static bool s_block_event_callback = false;
EventedTimerID evented_timer_register(uint32_t timeout_ms,
                                      bool repeating,
                                      EventedTimerCallback callback,
                                      void* callback_data) {
  if (!s_block_event_callback) {
    callback(callback_data);
  }
  return 0;
}

// Test data
///////////////////////////////////////////////////////////

#include "ancs_test_data.h"

const uint32_t s_invalid_param_uid = 0x12;
const uint32_t s_get_wrong_data_uid = 0xee;

static BLECharacteristic s_characteristics[NumANCSCharacteristic] = { 1, 2, 3 };

// Helper Functions
///////////////////////////////////////////////////////////

static int s_num_requested_app_attributes;
static int s_num_requested_notif_attributes;
static int s_num_ds_notifications_received;
static bool s_gatt_client_op_write_should_fail_unlimited = false;
static bool s_gatt_client_op_write_should_fail_once = false;

static void prv_fake_receiving_ds_notification(size_t value_length, uint8_t *value) {
  BLECharacteristic characteristic = s_characteristics[ANCSCharacteristicData];
  ancs_handle_read_or_notification(characteristic, (const uint8_t *) value, value_length, 0);
}

static void prv_fake_receiving_ns_notification(size_t value_length, uint8_t *value) {
  BLECharacteristic characteristic = s_characteristics[ANCSCharacteristicNotification];
  ancs_handle_read_or_notification(characteristic, (const uint8_t *) value, value_length, 0);
}

static void prv_send_notification_with_event_flags(const uint8_t *ancs_notification_dict,
                                                   int event_flags) {
  NSNotification ns_notification = {
    .event_id = EventIDNotificationAdded,
    .event_flags = event_flags,
    .category_id = CategoryIDSocial,
    .category_count = 1,
    .uid = 1,
  };

  const uint32_t ancs_notification_dict_uid =
      ((GetNotificationAttributesMsg *)ancs_notification_dict)->notification_uid;
  ns_notification.uid = ancs_notification_dict_uid;
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
}

static void prv_send_notification(const uint8_t *ancs_notification_dict) {
  prv_send_notification_with_event_flags(ancs_notification_dict, 0);
}

static uint8_t *prv_serialize_timeline_item(TimelineItem *item, size_t *size_out) {
  size_t payload_size = timeline_item_get_serialized_payload_size(item);
  *size_out = sizeof(SerializedTimelineItemHeader) + payload_size;
  uint8_t *buffer = malloc(*size_out);

  timeline_item_serialize_header(item, (SerializedTimelineItemHeader *)buffer);
  timeline_item_serialize_payload(item, buffer + sizeof(SerializedTimelineItemHeader),
                                  payload_size);

  return buffer;
}

void prv_cmp_last_received_notification(TimelineItem *item) {
  TimelineItem *notification = fake_notification_storage_get_last_notification();
  size_t size1, size2;

  // Clear out the id since it is auto-generated
  memset(&notification->header.id, 0, sizeof(notification->header.id));

  uint8_t *buf1 = prv_serialize_timeline_item(notification, &size1);
  uint8_t *buf2 = prv_serialize_timeline_item(item, &size2);

  cl_assert_equal_i(size1, size2);
  cl_assert_equal_m(buf1, buf2, size1);

  free(buf1);
  free(buf2);
}

// Called from inside prv_write_control_point_request.
// If this function is called we have requested a ds_notification
BTErrno gatt_client_op_write(BLECharacteristic characteristic,
                             const uint8_t *buffer,
                             size_t length,
                             GAPLEClient client) {

  cl_assert_equal_i(characteristic, s_characteristics[ANCSCharacteristicControl]);

  if (s_gatt_client_op_write_should_fail_once) {
    s_gatt_client_op_write_should_fail_once = false;
    return BTErrnoInvalidParameter;
  }

  if (s_gatt_client_op_write_should_fail_unlimited) {
    return BTErrnoInvalidParameter;
  }

  const uint32_t complete_dict_uid = ((GetNotificationAttributesMsg*)s_complete_dict)->notification_uid;
  const uint32_t chunked_dict_uid = ((GetNotificationAttributesMsg*)s_chunked_dict_part_one)->notification_uid;
  const uint32_t message_size_attr_dict_uid = ((GetNotificationAttributesMsg*)s_message_size_attr_dict)->notification_uid;
  const uint32_t invalid_dict_uid = ((GetNotificationAttributesMsg*)s_invalid_attribute_length)->notification_uid;
  const uint32_t attribute_at_end_uid = ((GetNotificationAttributesMsg*)memory_with_attribute_id_at_end.attribute_data)->notification_uid;
  const uint32_t loading_uid = ((GetNotificationAttributesMsg*)s_loading_response)->notification_uid;
  const uint32_t no_content_uid = ((GetNotificationAttributesMsg*)s_this_message_has_no_content_response)->notification_uid;
  const uint32_t multiple_complete_dict_uid = ((GetNotificationAttributesMsg*)s_multiple_complete_dicts)->notification_uid;
  const uint32_t split_timestamp_uid = ((GetNotificationAttributesMsg*)s_split_timestamp_dict_part_one)->notification_uid;
  const uint32_t message_dict_uid = ((GetNotificationAttributesMsg*)s_message_dict)->notification_uid;
  const uint32_t app_name_title_dict_uid = ((GetNotificationAttributesMsg*)s_app_name_title_dict)->notification_uid;
  const uint32_t unknown_app_message_dict_uid = ((GetNotificationAttributesMsg*)s_unknown_app_dict)->notification_uid;
  const uint32_t unknown_app_unique_title_dict_uid = ((GetNotificationAttributesMsg*)s_unknown_app_unique_title_dict)->notification_uid;
  const uint32_t mms_no_caption_dict_uid = ((GetNotificationAttributesMsg*)s_mms_no_caption_dict)->notification_uid;
  const uint32_t mms_with_caption_dict_uid = ((GetNotificationAttributesMsg*)s_mms_with_caption_dict)->notification_uid;

  const CPDSMessage *cmd_header = (const CPDSMessage *)buffer;
  if (cmd_header->command_id == CommandIDGetAppAttributes) {
    s_num_requested_app_attributes++;

    if (strcmp((const char *)cmd_header->data, "com.tests.NotAnApp") == 0) {
      prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_fake_app_info_dict),
                                         (uint8_t *)s_fake_app_info_dict);
    } else {
      prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_message_app_info_dict),
                                         (uint8_t *)s_message_app_info_dict);
    }
    return BTErrnoOK;
  }

  // else: notif request
  uint32_t uid = ((GetNotificationAttributesMsg *)buffer)->notification_uid;
  s_num_requested_notif_attributes++;

  if (uid == complete_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_complete_dict), (uint8_t*) s_complete_dict);
    s_num_ds_notifications_received++;
  } else if (uid == chunked_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_chunked_dict_part_one), (uint8_t*) s_chunked_dict_part_one);
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_chunked_dict_part_two), (uint8_t*) s_chunked_dict_part_two);
    s_num_ds_notifications_received += 2;
  } else if (uid == message_size_attr_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_message_size_attr_dict), (uint8_t*) s_message_size_attr_dict);
    s_num_ds_notifications_received++;
  } else if (uid == attribute_at_end_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(memory_with_attribute_id_at_end.attribute_data), (uint8_t*) memory_with_attribute_id_at_end.attribute_data);
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(memory_with_attribute_id_at_end_p2), (uint8_t*) memory_with_attribute_id_at_end_p2);
    s_num_ds_notifications_received += 2;
  } else if (uid == invalid_dict_uid) {
      prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_invalid_attribute_length), (uint8_t*) s_invalid_attribute_length);
      s_num_ds_notifications_received++;
  } else if (uid == loading_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_loading_response),
                            (uint8_t*) s_loading_response);
    s_num_ds_notifications_received++;
  } else if (uid == no_content_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_this_message_has_no_content_response),
                            (uint8_t*) s_this_message_has_no_content_response);
    s_num_ds_notifications_received++;
  } else if (uid == s_invalid_param_uid) {
    ancs_handle_write_response(0, 0xA2);
    s_num_ds_notifications_received++;
  } else if (uid ==  multiple_complete_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_multiple_complete_dicts), (uint8_t*) s_multiple_complete_dicts);
    s_num_ds_notifications_received += 3;
  } else if (uid == split_timestamp_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_split_timestamp_dict_part_one), (uint8_t*) s_split_timestamp_dict_part_one);
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_split_timestamp_dict_part_two), (uint8_t*) s_split_timestamp_dict_part_two);
    s_num_ds_notifications_received += 2;
  } else if (uid == message_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_message_dict), (uint8_t*) s_message_dict);
    s_num_ds_notifications_received++;
  } else if (uid == app_name_title_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_app_name_title_dict), (uint8_t *)s_app_name_title_dict);
    s_num_ds_notifications_received++;
  } else if (uid == unknown_app_message_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_unknown_app_dict), (uint8_t *)s_unknown_app_dict);
    s_num_ds_notifications_received++;
  } else if (uid == unknown_app_unique_title_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_unknown_app_unique_title_dict), (uint8_t *)s_unknown_app_unique_title_dict);
    s_num_ds_notifications_received++;
  } else if (uid == mms_no_caption_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_mms_no_caption_dict), (uint8_t *)s_mms_no_caption_dict);
    s_num_ds_notifications_received++;
  } else if (uid == mms_with_caption_dict_uid) {
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_mms_with_caption_dict), (uint8_t *)s_mms_with_caption_dict);
    s_num_ds_notifications_received++;
  } else if (uid == s_get_wrong_data_uid) {
    // We wanted a notification attributes message, but got a app attributes message...
    prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_message_app_info_dict),
                                         (uint8_t *)s_message_app_info_dict);
    s_num_ds_notifications_received++;
  }

  return BTErrnoOK;
}


// Tests
///////////////////////////////////////////////////////////

#define TEST_START FILESYSTEM_FILE_TEST_SPACE_BEGIN
#define TEST_SIZE (FILESYSTEM_FILE_TEST_SPACE_END - \
  FILESYSTEM_FILE_TEST_SPACE_BEGIN)

void test_ancs__initialize(void) {
  s_block_event_callback = false;
  regular_timer_init();
  s_num_requested_notif_attributes = 0;
  s_num_requested_app_attributes = 0;
  s_num_ds_notifications_received = 0;
  s_gatt_client_op_write_should_fail_once = false;
  s_gatt_client_op_write_should_fail_unlimited = false;
  fake_kernel_services_notifications_reset();
  fake_notification_storage_reset();
  fake_event_init();

  ancs_create();
  ancs_handle_service_discovered(s_characteristics);
}

void test_ancs__cleanup(void) {
  ancs_destroy();
  cl_assert_equal_i(regular_timer_seconds_count(), 0);
  cl_assert_equal_i(regular_timer_minutes_count(), 0);
  regular_timer_deinit();
}

// Janky black box smoke-test to exercise the ANCS message re-assembly state
// machine
void test_ancs__should_handle_small_and_large_messages(void) {
  // Get 4 complete notifications
  prv_send_notification((uint8_t *)&s_complete_dict);
  prv_send_notification((uint8_t *)&s_complete_dict);
  prv_send_notification((uint8_t *)&s_complete_dict);
  prv_send_notification((uint8_t *)&s_complete_dict);
  cl_assert_equal_i(s_num_requested_notif_attributes, 4);
  cl_assert_equal_i(s_num_ds_notifications_received, 4);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 4);

  // Get 4 2-part notifications
  prv_send_notification((uint8_t *)&s_chunked_dict_part_one);
  prv_send_notification((uint8_t *)&s_chunked_dict_part_one);
  prv_send_notification((uint8_t *)&s_chunked_dict_part_one);
  prv_send_notification((uint8_t *)&s_chunked_dict_part_one);
  cl_assert_equal_i(s_num_requested_notif_attributes, 4 + 4);
  cl_assert_equal_i(s_num_ds_notifications_received, 4 + 2*4);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 4 + 4);

  // Some alternating complete / 2-part notifications
  prv_send_notification((uint8_t *)&s_complete_dict);
  prv_send_notification((uint8_t *)&s_chunked_dict_part_one);
  prv_send_notification((uint8_t *)&s_complete_dict);
  prv_send_notification((uint8_t *)&s_chunked_dict_part_one);
  cl_assert_equal_i(s_num_requested_notif_attributes, 8 + 4);
  cl_assert_equal_i(s_num_ds_notifications_received, 12 + 1 + 2 + 1 + 2);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 4 + 4 + 4);

  // Send a "corrupted" notification.
  prv_send_notification((uint8_t *)&s_invalid_attribute_length);
  cl_assert_equal_i(s_num_requested_notif_attributes, 12 + 1);
  cl_assert_equal_i(s_num_ds_notifications_received, 18 + 1);
  // No increment:
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 4 + 4 + 4);
}

void test_ancs__should_handle_message_size_attribute(void) {
  prv_send_notification((uint8_t *)&s_message_size_attr_dict);
  cl_assert_equal_i(s_num_requested_notif_attributes, 1);
  cl_assert_equal_i(s_num_ds_notifications_received, 1);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);
}

void test_ancs__should_filter_out_loading_messages_from_mail_app(void) {
  // Get notification for which we'll get a "Loading..." response:
  prv_send_notification((uint8_t *)&s_loading_response);
  cl_assert_equal_i(s_num_requested_notif_attributes, 1);
  cl_assert_equal_i(s_num_ds_notifications_received, 1);
  // Assert it got filtered out:
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 0);

  // Get notification for which we'll get a "This message has no content." response:
  prv_send_notification((uint8_t *)&s_this_message_has_no_content_response);
  cl_assert_equal_i(s_num_requested_notif_attributes, 2);
  cl_assert_equal_i(s_num_ds_notifications_received, 2);
  // Assert it got filtered out:
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 0);
}

void test_ancs__should_filter_out_duplicate_messages(void) {
  // With an empty db, new notifications should be added as usual
  prv_send_notification((uint8_t *)&s_complete_dict);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);
  cl_assert_equal_i(fake_notification_storage_get_store_count(), 1);
  cl_assert_equal_i(fake_notification_storage_get_remove_count(), 0);

  // We should reject any notification that matches and has the exact same uid
  uint32_t uid = ((GetNotificationAttributesMsg *)&s_complete_dict)->notification_uid;
  fake_notification_storage_set_existing_ancs_notification(&(Uuid)UUID_SYSTEM, uid);
  prv_send_notification((uint8_t *)&s_complete_dict);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);
  cl_assert_equal_i(fake_notification_storage_get_store_count(), 1);
  cl_assert_equal_i(fake_notification_storage_get_remove_count(), 0);

  // If there's a notification that matches with a different uid, we update the notification by
  // removing and then storing again (we don't send a NotificationAdded event)
  fake_notification_storage_set_existing_ancs_notification(&(Uuid)UUID_SYSTEM, UINT32_MAX);
  prv_send_notification((uint8_t *)&s_complete_dict);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);
  cl_assert_equal_i(fake_notification_storage_get_store_count(), 2);
  cl_assert_equal_i(fake_notification_storage_get_remove_count(), 1);
}

void test_ancs__should_handle_split_timestamp_messages(void) {
  prv_send_notification((uint8_t *)&s_split_timestamp_dict_part_one);

  cl_assert_equal_i(s_num_requested_notif_attributes, 1);
  cl_assert_equal_i(s_num_ds_notifications_received, 2);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);
}


void test_ancs__attribute_at_end(void) {
  prv_send_notification((uint8_t *)&memory_with_attribute_id_at_end.attribute_data);
  cl_assert_equal_i(s_num_requested_notif_attributes, 1 );
  cl_assert_equal_i(s_num_ds_notifications_received, 2);
}

void test_ancs__app_name_cache(void) {
  prv_send_notification((uint8_t *)&s_message_dict);
  prv_send_notification((uint8_t *)&s_message_dict);

  cl_assert_equal_i(s_num_requested_notif_attributes, 2);
  // should have gotten cached the second time around
  cl_assert_equal_i(s_num_requested_app_attributes, 1);
  cl_assert_equal_i(s_num_ds_notifications_received, 2);
}



void test_ancs__ancs_invalid_param(void) {

  NSNotification ns_notification = {
    .event_id = EventIDNotificationAdded,
    .event_flags = 0,
    .category_id = CategoryIDSocial,
    .category_count = 1,
    .uid = 0,
  };

  const uint32_t complete_dict_uid = ((GetNotificationAttributesMsg*)s_complete_dict)->notification_uid;

  ns_notification.uid = s_invalid_param_uid;
  // This will return with an error ANCS_INVALID_PARAM
  // Should not get re-requested
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(s_num_requested_notif_attributes, 1 );
  cl_assert_equal_i(s_num_ds_notifications_received, 1);

  ns_notification.uid = complete_dict_uid;
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(s_num_requested_notif_attributes, 2);
  cl_assert_equal_i(s_num_ds_notifications_received, 2);

  ns_notification.uid = s_invalid_param_uid;
  // This will return with an error ANCS_INVALID_PARAM
  // Should not get re-requested
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(s_num_requested_notif_attributes, 3);
  cl_assert_equal_i(s_num_ds_notifications_received, 3);

  ns_notification.uid = s_invalid_param_uid;
  // This will return with an error ANCS_INVALID_PARAM
  // Should not get re-requested
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(s_num_requested_notif_attributes, 4);
  cl_assert_equal_i(s_num_ds_notifications_received, 4);

  ns_notification.uid = complete_dict_uid;
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(s_num_requested_notif_attributes, 5);
  cl_assert_equal_i(s_num_ds_notifications_received, 5);

}

extern ANCSClientState prv_get_state(void);
extern void prv_check_ancs_alive(void);

void test_ancs__alive_check_disconnection(void) {
  prv_check_ancs_alive();
  // check we're in the alive check state and we sent a single request
  cl_assert_equal_i(prv_get_state(), ANCSClientStateAliveCheck);
  cl_assert_equal_i(s_num_requested_notif_attributes, 1);
  // simulate a disconnection/reconnection
  ancs_handle_service_removed(s_characteristics, NumANCSCharacteristic);
  ancs_handle_service_discovered(s_characteristics);
  // we should be back in the Idle state
  cl_assert_equal_i(prv_get_state(), ANCSClientStateIdle);
  // Make sure we can still receive notifications
  prv_send_notification((uint8_t *)&s_complete_dict);
  cl_assert_equal_i(s_num_requested_notif_attributes, 2);
  cl_assert_equal_i(s_num_ds_notifications_received, 1);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);
}

void test_ancs__notification_dismissal(void) {
  NSNotification ns_notification = {
    .event_id = EventIDNotificationRemoved,
    .event_flags = 0,
    .category_id = CategoryIDSocial,
    .category_count = 1,
  };

  // Notification removal without DIS service - notification shouldn't be acted upon
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(fake_kernel_services_notifications_acted_upon_count(), 0);

  // DIS service / iOS 9+ detected - enabling notification dismissal
  ancs_handle_ios9_or_newer_detected();
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(fake_kernel_services_notifications_acted_upon_count(), 1);
}

void test_ancs__notification_parsing(void) {
  // Test a recognized app with a duplicated title
  // Run multiple times to make sure we're not corrupting the app name cache
  for (int i = 0; i < 4; ++i) {
    prv_send_notification((uint8_t *)&s_app_name_title_dict);
    prv_cmp_last_received_notification(&s_app_name_title_parsed_item);
  }

  // Test an unrecognized app with a duplicated title
  prv_send_notification((uint8_t *)&s_unknown_app_dict);
  prv_cmp_last_received_notification(&s_unknown_app_parsed_item);

  // Make sure both apps attributes were requested (Messages and FakeApp)
  cl_assert_equal_i(s_num_requested_app_attributes, 2);

  // Test a recognized app with a unique title
  prv_send_notification((uint8_t *)&s_message_dict);
  prv_cmp_last_received_notification(&s_message_parsed_item);

  // Test an unrecognized app with a unique title
  prv_send_notification((uint8_t *)&s_unknown_app_unique_title_dict);
  prv_cmp_last_received_notification(&s_unknown_app_unique_title_parsed_item);

  // Test an MMS without a caption
  prv_send_notification_with_event_flags((uint8_t *)&s_mms_no_caption_dict, EventFlagMultiMedia);
  prv_cmp_last_received_notification(&s_mms_no_caption_parsed_item);

  // Test an MMS with a caption
  prv_send_notification_with_event_flags((uint8_t *)&s_mms_with_caption_dict, EventFlagMultiMedia);
  prv_cmp_last_received_notification(&s_mms_with_caption_parsed_item);

  // Test a third party notification with the MultiMedia EventFlag
  prv_send_notification_with_event_flags((uint8_t *)&s_unknown_app_unique_title_dict, EventFlagMultiMedia);
  prv_cmp_last_received_notification(&s_unknown_app_unique_title_parsed_item);
}

// Make sure we send an ANCS_DISCONNECTED event whenever our session goes away
void test_ancs__disconnection(void) {
  // Simulate a disconnection/reconnection
  ancs_handle_service_removed(s_characteristics, NumANCSCharacteristic);
  ancs_handle_service_discovered(s_characteristics);
  cl_assert_equal_i(fake_event_get_last().type, PEBBLE_ANCS_DISCONNECTED_EVENT);
  fake_event_clear_last();

  // If we unexpectedly register another session, make sure we send the event
  ancs_handle_service_discovered(s_characteristics);
  cl_assert_equal_i(fake_event_get_last().type, PEBBLE_ANCS_DISCONNECTED_EVENT);
  fake_event_clear_last();

  ancs_invalidate_all_references();
  cl_assert_equal_i(fake_event_get_last().type, PEBBLE_ANCS_DISCONNECTED_EVENT);

  // Make sure that losing BT altogether sends the event
  ancs_destroy();
  cl_assert_equal_i(fake_event_get_last().type, PEBBLE_ANCS_DISCONNECTED_EVENT);
  fake_event_clear_last();
}

void test_ancs__unrequested_notifications(void) {
  prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_complete_dict), (uint8_t*) s_complete_dict);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 0);

  prv_fake_receiving_ds_notification(ARRAY_LENGTH(s_message_app_info_dict),
                                     (uint8_t *)s_message_app_info_dict);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 0);
}

void test_ancs__handle_unexpected_notifications(void) {
  NSNotification ns_notification = {
    .event_id = EventIDNotificationAdded,
    .event_flags = 0,
    .category_id = CategoryIDSocial,
    .category_count = 1,
    .uid = s_get_wrong_data_uid,
  };
  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 0);

  // And make sure we get to a state where we can handle more messages
  prv_send_notification((uint8_t *)&s_complete_dict);
  cl_assert_equal_i(s_num_requested_notif_attributes, 2);
  cl_assert_equal_i(s_num_ds_notifications_received, 2);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);
}

void test_ancs__get_notif_attributes_retry(void) {
  s_gatt_client_op_write_should_fail_once = true;

  prv_send_notification((uint8_t *)&s_complete_dict);
  // We will be successful on the retry (second attempt)
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);

  s_gatt_client_op_write_should_fail_unlimited = true;
  prv_send_notification((uint8_t *)&s_complete_dict);
  // The retry fails and we give up on this one
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 1);

  // And make sure we get to a state where we can handle more messages
  s_gatt_client_op_write_should_fail_unlimited = false;
  prv_send_notification((uint8_t *)&s_complete_dict);
  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 2);
}

void test_ancs__reset_after_retry(void) {
  s_block_event_callback = true;
  s_gatt_client_op_write_should_fail_once = true;
  prv_send_notification((uint8_t *)&s_complete_dict);
  ancs_invalidate_all_references();
  cl_assert_equal_i(prv_get_state(), ANCSClientStateIdle);
}

// No Longer Supported
//void test_ancs__should_handle_response_with_multiple_notifications(void) {
//
//  NSNotification ns_notification = {
//    .event_id = EventIDNotificationAdded,
//    .event_flags = 0,
//    .category_id = CategoryIDSocial,
//    .category_count = 1,
//    .uid = 0,
//  };
//
//  const uint32_t multiple_complete_dict_uid = ((GetNotificationAttributesMsg*)s_multiple_complete_dicts)->notification_uid;
//  ns_notification.uid = multiple_complete_dict_uid;
//  prv_fake_receiving_ns_notification(sizeof(ns_notification), (uint8_t*) &ns_notification);
//  cl_assert_equal_i(s_num_requested_notif_attributes, 1);
//  cl_assert_equal_i(s_num_ds_notifications_received, 3);
//
//  // The last one was a phone notification but I changed it so it no longer is
//  cl_assert_equal_i(fake_kernel_services_notifications_ancs_notifications_count(), 3);
//}

