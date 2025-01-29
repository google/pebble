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

#include "services/normal/bluetooth/ble_hrm.h"

#include "comm/ble/gap_le_connection.h"
#include "services/common/hrm/hrm_manager_private.h"

#include <bluetooth/hrm_service.h>
#include <btutil/bt_device.h>
#include <util/size.h>

#include <clar.h>


////////////////////////////////////////////////////////////////////////////////////////////////////
// Stubs & Fakes

#include "fake_event_service.h"
#include "fake_pebble_tasks.h"
#include "fake_pbl_malloc.h"
#include "fake_regular_timer.h"

#include "stubs_analytics.h"
#include "stubs_bt_lock.h"
#include "stubs_logging.h"
#include "stubs_passert.h"

void gap_le_slave_reconnect_hrm_restart(void) {
}

void gap_le_slave_reconnect_hrm_stop(void) {
}

static bool s_activity_prefs_heart_rate_is_enabled;
bool activity_prefs_heart_rate_is_enabled(void) {
  return s_activity_prefs_heart_rate_is_enabled;
}

static bool s_bt_driver_hrm_service_is_enabled;
static int s_bt_driver_hrm_service_enable_call_count;
void bt_driver_hrm_service_enable(bool enable) {
  s_bt_driver_hrm_service_enable_call_count++;
  s_bt_driver_hrm_service_is_enabled = enable;
}

static BleHrmServiceMeasurement s_last_ble_hrm_measurement;
static int s_bt_driver_hrm_service_handle_measurement_call_count;
static BTDeviceInternal s_last_permitted_devices[10];
static size_t s_last_num_permitted_devices;
void bt_driver_hrm_service_handle_measurement(const BleHrmServiceMeasurement *measurement,
                                              const BTDeviceInternal *permitted_devices,
                                              size_t num_permitted_devices) {
  ++s_bt_driver_hrm_service_handle_measurement_call_count;
  s_last_ble_hrm_measurement = *measurement;
  s_last_num_permitted_devices = num_permitted_devices;
  memcpy(s_last_permitted_devices, permitted_devices,
         sizeof(*permitted_devices) * num_permitted_devices);
}

static BLEHRMSharingRequest *s_last_sharing_request;
static int s_ble_hrm_push_sharing_request_window_call_count;
void ble_hrm_push_sharing_request_window(BLEHRMSharingRequest *sharing_request) {
  ++s_ble_hrm_push_sharing_request_window_call_count;
  cl_assert_equal_p(s_last_sharing_request, NULL);
  s_last_sharing_request = sharing_request;
}

bool bt_driver_is_hrm_service_supported(void) {
  return true;
}

static BTDeviceInternal s_last_disconnected;
int bt_driver_gap_le_disconnect(const BTDeviceInternal *peer_address) {
  s_last_disconnected = *peer_address;
  return 0;
}

static void prv_assert_last_disconnected(const BTDeviceInternal *peer_address) {
  cl_assert_equal_b(bt_device_internal_equal(peer_address, &s_last_disconnected), true);
}

static int s_ble_hrm_push_reminder_popup_call_count;
void ble_hrm_push_reminder_popup(void) {
  s_ble_hrm_push_reminder_popup_call_count++;
}

static int s_hrm_manager_subscribe_with_callback_call_count;
static HRMSessionRef s_last_session_ref;
static HRMSessionRef s_next_session_ref;
HRMSessionRef hrm_manager_subscribe_with_callback(AppInstallId app_id, uint32_t update_interval_s,
                                                  uint16_t expire_s, HRMFeature features,
                                                  HRMSubscriberCallback callback, void *context) {
  cl_assert_equal_p(NULL, callback); // we're using the event service
  cl_assert_equal_i(features, HRMFeature_BPM);
  ++s_hrm_manager_subscribe_with_callback_call_count;
  s_last_session_ref = ++s_next_session_ref;
  return s_last_session_ref;
}

static GAPLEConnection *s_connections[2];

GAPLEConnection *gap_le_connection_by_device(const BTDeviceInternal *device) {
  for (int i = 0; i < ARRAY_LENGTH(s_connections); ++i) {
    if (bt_device_internal_equal(device, &s_connections[i]->device)) {
      return s_connections[i];
    }
  }
  return NULL;
}
BTDeviceInternal *device_from_le_connection(GAPLEConnection *conn) {
  return &conn->device;
}

bool gap_le_connection_is_valid(const GAPLEConnection *conn) {
  for (int i = 0; i < ARRAY_LENGTH(s_connections); ++i) {
    if (s_connections[i] == conn) {
      return true;
    }
  }
  return false;
}

void gap_le_connection_for_each(GAPLEConnectionForEachCallback cb, void *data) {
  for (int i = 0; i < ARRAY_LENGTH(s_connections); ++i) {
    cb(s_connections[i], data);
  }
}

void launcher_task_add_callback(CallbackEventCallback callback, void *data) {
  callback(data);
}

bool sys_hrm_manager_is_hrm_present(void) {
  return true;
}

static int s_sys_hrm_manager_unsubscribe_call_count;
bool sys_hrm_manager_unsubscribe(HRMSessionRef session) {
  ++s_sys_hrm_manager_unsubscribe_call_count;
  cl_assert_equal_i(session, s_last_session_ref);
  return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Tests

static void prv_assert_event_service_subscribed(bool is_subscribed) {
  const EventServiceInfo *const info = fake_event_service_get_info(PEBBLE_HRM_EVENT);
  if (is_subscribed) {
    cl_assert(info->handler);
  } else {
    cl_assert_equal_p(NULL, info->handler);
  }
}

void test_ble_hrm__cleanup(void) {
  ble_hrm_deinit();

  prv_assert_event_service_subscribed(false);
  // hrm manager sub vs unsub calls should be the same, there should be no subscription any more
  // after de-initing:
  cl_assert_equal_i(s_sys_hrm_manager_unsubscribe_call_count,
                    s_hrm_manager_subscribe_with_callback_call_count);

  fake_pbl_malloc_check_net_allocs();

  // Assert all regular timers are deregistered:
  cl_assert_equal_p(s_seconds_callbacks.next, NULL);
  cl_assert_equal_p(s_minutes_callbacks.next, NULL);
}

#define TEST_DEVICE_NAME "iPhone Martijn"

static GAPLEConnection s_conn_a;
static GAPLEConnection s_conn_b;
static const BTDeviceInternal *s_device_a;
static const BTDeviceInternal *s_device_b;

void test_ble_hrm__initialize(void) {
  fake_pbl_malloc_clear_tracking();
  for (int i = 0; i < ARRAY_LENGTH(s_connections); ++i) {
    s_connections[i] = NULL;
  }
  s_activity_prefs_heart_rate_is_enabled = true;
  s_bt_driver_hrm_service_is_enabled = true;
  s_last_num_permitted_devices = 0;
  memset(s_last_permitted_devices, 0, sizeof(s_last_permitted_devices));
  s_bt_driver_hrm_service_enable_call_count = 0;
  s_hrm_manager_subscribe_with_callback_call_count = 0;
  s_sys_hrm_manager_unsubscribe_call_count = 0;
  s_bt_driver_hrm_service_handle_measurement_call_count = 0;
  s_ble_hrm_push_sharing_request_window_call_count = 0;
  s_ble_hrm_push_reminder_popup_call_count = 0;
  s_last_session_ref = ~0;
  s_next_session_ref = 1234;
  s_last_disconnected = (BTDeviceInternal) {};
  s_last_sharing_request = NULL;
  s_last_ble_hrm_measurement = (BleHrmServiceMeasurement) {};
  fake_event_service_init();

  // Set up fake devices/connections:
  s_conn_a = (GAPLEConnection) {
    .device_name = TEST_DEVICE_NAME,
    .device = {
      .address = {
        .octets = {1, 2, 3, 4, 5, 6},
      },
    },
  };
  s_conn_b = (GAPLEConnection) {
    .device_name = TEST_DEVICE_NAME,
    .device = {
      .address = {
        .octets = {6, 5, 4, 3, 2, 1},
      },
    },
  };
  s_connections[0] = &s_conn_a;
  s_connections[1] = &s_conn_b;
  s_device_a = device_from_le_connection(&s_conn_a);
  s_device_b = device_from_le_connection(&s_conn_b);

  ble_hrm_init();
}

void test_ble_hrm__init_deinit_no_subscriptions(void) {
  // let cleanup & initialize do the work :)
}

static void prv_assert_permissions_ui_and_respond(bool is_granted) {
  cl_assert(s_last_sharing_request);
  ble_hrm_handle_sharing_request_response(is_granted, s_last_sharing_request);
  s_last_sharing_request = NULL;
}

void test_ble_hrm__sub_unsub(void) {
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 0);
  cl_assert_equal_i(s_sys_hrm_manager_unsubscribe_call_count, 0);
  prv_assert_event_service_subscribed(false);

  // Device A subscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // Expect HRM manager NOT to be subscribed to yet, need to grant permission first:
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 0);
  prv_assert_event_service_subscribed(false);
  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_a));

  // Expect permissions UI to be presented:
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  // Expect HRM manager to be subscribed to:
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 1);
  prv_assert_event_service_subscribed(true);
  cl_assert_equal_b(true, ble_hrm_is_sharing_to_connection(&s_conn_a));

  // Device A subscribes again, should be a no-op, no new permissions prompt:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 1);
  cl_assert_equal_i(s_sys_hrm_manager_unsubscribe_call_count, 0);

  // Device B subscribes, shouldn't resubscribe to HRM manager, but should present a new
  // permission prompt, because it's a different device:
  bt_driver_cb_hrm_service_update_subscription(s_device_b, true);
  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_b));
  prv_assert_permissions_ui_and_respond(true /* is_granted */);
  cl_assert_equal_b(true, ble_hrm_is_sharing_to_connection(&s_conn_b));
  prv_assert_event_service_subscribed(true);
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 1);
  cl_assert_equal_i(s_sys_hrm_manager_unsubscribe_call_count, 0);

  // Device A disconnects, shouldn't unsubscribe from HRM manager because A is still subscribed:
  ble_hrm_handle_disconnection(&s_conn_a);
  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_a));
  prv_assert_event_service_subscribed(true);
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 1);
  cl_assert_equal_i(s_sys_hrm_manager_unsubscribe_call_count, 0);

  // Device B unsubscribes, expect to be unsubscribed from HRM manager, because there are no more
  // devices subscribed to the BLE HRM service:
  bt_driver_cb_hrm_service_update_subscription(s_device_b, false);
  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_a));
  prv_assert_event_service_subscribed(false);
  cl_assert_equal_i(s_sys_hrm_manager_unsubscribe_call_count, 1);

  // Device B unsubscribes again, should be no-op
  bt_driver_cb_hrm_service_update_subscription(s_device_b, false);
  prv_assert_event_service_subscribed(false);
  cl_assert_equal_i(s_sys_hrm_manager_unsubscribe_call_count, 1);
}

void test_ble_hrm__sub_unsub_resub(void) {
  // Device A subscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // Expect permissions UI to be presented:
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  // Device A unsubscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, false);

  prv_assert_event_service_subscribed(false);

  // Device A re-subscribes, permission should still be valid:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  prv_assert_event_service_subscribed(true);
}

void test_ble_hrm__revoke(void) {
  // Device A subscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // Expect permissions UI to be presented:
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  cl_assert_equal_b(true, ble_hrm_is_sharing_to_connection(&s_conn_a));
  cl_assert_equal_b(true, ble_hrm_is_sharing());
  prv_assert_event_service_subscribed(true);

  // Revoke:
  ble_hrm_revoke_sharing_permission_for_connection(&s_conn_a);

  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_a));
  cl_assert_equal_b(false, ble_hrm_is_sharing());
  prv_assert_event_service_subscribed(false);
  prv_assert_last_disconnected(s_device_a);
}

void test_ble_hrm__revoke_all(void) {
  // Device A subscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // Expect permissions UI to be presented:
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  // Device B subscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_b, true);

  // Expect permissions UI to be presented:
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  ble_hrm_revoke_all();

  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_a));
  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_b));
  cl_assert_equal_b(false, ble_hrm_is_sharing());
  prv_assert_event_service_subscribed(false);
}

void test_ble_hrm__revoke_after_disconnection(void) {
  ble_hrm_revoke_sharing_permission_for_connection(NULL);

  s_connections[0] = NULL;
  ble_hrm_revoke_sharing_permission_for_connection(&s_conn_a);

  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(NULL));

  // Shouldn't crash or anything
}

void test_ble_hrm__grant_after_disconnection(void) {
  // Device A subscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // Fake disconnection:
  s_connections[0] = NULL;

  // Grab permission after disconnection.
  // Request object should be freed and thing shouldn't crash.
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_a));
}

void test_ble_hrm__decline_permission_dont_ask_again_even_after_reconnecting(void) {
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // Decline:
  prv_assert_permissions_ui_and_respond(false /* is_granted */);

  // Unsub, resub:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, false);
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // No sharing request UI:
  cl_assert_equal_p(NULL, s_last_sharing_request);

  // Fake disconnection:
  ble_hrm_handle_disconnection(&s_conn_a);

  // Fake reconn & subscribe:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);

  // No sharing request UI:
  cl_assert_equal_p(NULL, s_last_sharing_request);

  // Still declined:
  cl_assert_equal_b(false, ble_hrm_is_sharing_to_connection(&s_conn_a));
}

void test_ble_hrm__unsub_upon_deinit(void) {
  // Device A subscribes:
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  // __cleanup() will do the deinit and also assert that  there's no subscription to the HRM mgr.
}

// Test that we handle a races where a subscription/disconnection callback happens in after
// deiniting the stack:
void test_ble_hrm__sub_after_deinit(void) {
  ble_hrm_deinit();

  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);
  prv_assert_event_service_subscribed(false);
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 0);

  ble_hrm_handle_disconnection(&s_conn_a);
  prv_assert_event_service_subscribed(false);
  cl_assert_equal_i(s_hrm_manager_subscribe_with_callback_call_count, 0);

  ble_hrm_init(); // reinit, __cleanup() will deinit again
}

static void prv_put_and_assert_hrm_event(HRMEventType subtype, uint8_t bpm, HRMQuality quality,
                                         bool expect_bt_driver_cb, bool expected_is_on_wrist) {
  int call_count_before = s_bt_driver_hrm_service_handle_measurement_call_count;

  PebbleEvent hrm_event = {
    .type = PEBBLE_HRM_EVENT,
    .hrm = {
      .event_type = subtype,
      .bpm = {
        .bpm = bpm,
        .quality = quality,
      },
    },
  };
  event_put(&hrm_event);
  fake_event_service_handle_last();

  if (expect_bt_driver_cb) {
    cl_assert_equal_i(call_count_before + 1, s_bt_driver_hrm_service_handle_measurement_call_count);
    cl_assert_equal_i(bpm, s_last_ble_hrm_measurement.bpm);
    cl_assert_equal_b(expected_is_on_wrist, s_last_ble_hrm_measurement.is_on_wrist);
  } else {
    cl_assert_equal_i(call_count_before, s_bt_driver_hrm_service_handle_measurement_call_count);
  }
}

void test_ble_hrm__handle_hrm_event(void) {
  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);
  cl_assert_equal_i(0, s_bt_driver_hrm_service_handle_measurement_call_count);
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  // Don't grant permission to device B:
  bt_driver_cb_hrm_service_update_subscription(s_device_b, true);
  prv_assert_permissions_ui_and_respond(false /* is_granted */);

  prv_put_and_assert_hrm_event(HRMEvent_BPM, 80, HRMQuality_Excellent,
                               true /* expect bt driver cb */, true /* expected_is_on_wrist */);

  // Assert only device A is listed as "permitted device" and B is not:
  cl_assert_equal_i(1, s_last_num_permitted_devices);
  cl_assert_equal_m(&s_last_permitted_devices[0], s_device_a, sizeof(*s_device_a));

  prv_put_and_assert_hrm_event(HRMEvent_BPM, 80, HRMQuality_NoSignal,
                               true /* expect bt driver cb */, false /* expected_is_on_wrist */);

  prv_put_and_assert_hrm_event(HRMEvent_BPM, 80, HRMQuality_NoAccel,
                               true /* expect bt driver cb */, false /* expected_is_on_wrist */);

  prv_put_and_assert_hrm_event(HRMEvent_BPM, 80, HRMQuality_OffWrist,
                               true /* expect bt driver cb */, false /* expected_is_on_wrist */);

  // Ignore non-BPM event:
  prv_put_and_assert_hrm_event(HRMEvent_HRV, 80, HRMQuality_OffWrist,
                               false /* expect bt driver cb */, false /* expected_is_on_wrist */);
}

void test_ble_hrm__handle_activity_pref_hrm_changes(void) {
  cl_assert_equal_b(true, s_bt_driver_hrm_service_is_enabled);
  cl_assert_equal_i(0, s_bt_driver_hrm_service_enable_call_count);
  ble_hrm_handle_activity_prefs_heart_rate_is_enabled(false);
  cl_assert_equal_i(1, s_bt_driver_hrm_service_enable_call_count);
  cl_assert_equal_b(false, s_bt_driver_hrm_service_is_enabled);

  // Disabled, again -- would lead to another call to bt_driver_hrm_service_enable(),
  // the BT driver lib keeps track of whether it's enabled and is expected to ignore the call.
  ble_hrm_handle_activity_prefs_heart_rate_is_enabled(false);
  cl_assert_equal_i(2, s_bt_driver_hrm_service_enable_call_count);
  cl_assert_equal_b(false, s_bt_driver_hrm_service_is_enabled);

  // Enable
  ble_hrm_handle_activity_prefs_heart_rate_is_enabled(true);
  cl_assert_equal_i(3, s_bt_driver_hrm_service_enable_call_count);
  cl_assert_equal_b(true, s_bt_driver_hrm_service_is_enabled);
}

void test_ble_hrm__popup_after_long_continuous_use(void) {
  extern RegularTimerInfo *ble_hrm_timer(void);
  RegularTimerInfo *timer = ble_hrm_timer();

  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);
  prv_assert_permissions_ui_and_respond(true /* is_granted */);

  cl_assert_equal_b(true, regular_timer_is_scheduled(timer));

  bt_driver_cb_hrm_service_update_subscription(s_device_a, false);
  cl_assert_equal_b(false, regular_timer_is_scheduled(timer));

  bt_driver_cb_hrm_service_update_subscription(s_device_a, true);
  cl_assert_equal_b(true, regular_timer_is_scheduled(timer));

  cl_assert_equal_i(0, s_ble_hrm_push_reminder_popup_call_count);
  fake_regular_timer_trigger(timer);
  cl_assert_equal_i(1, s_ble_hrm_push_reminder_popup_call_count);

  // Except timer to be rescheduled again:
  cl_assert_equal_b(true, regular_timer_is_scheduled(timer));

  bt_driver_cb_hrm_service_update_subscription(s_device_a, false);
  cl_assert_equal_b(false, regular_timer_is_scheduled(timer));
}
