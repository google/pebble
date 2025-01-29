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

#include "drivers/hrm.h"
#include "os/tick.h"
#include "services/common/hrm/hrm_manager.h"
#include "services/common/hrm/hrm_manager_private.h"
#include "util/size.h"

#include "fake_app_manager.h"
#include "fake_events.h"
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_system_task.h"
#include "fake_queue.h"
#include "fake_rtc.h"

#include "stubs_accel_manager.h"
#include "stubs_analytics.h"
#include "stubs_event_service_client.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_prompt.h"
#include "stubs_worker_manager.h"



#include <stdio.h>
#include <services/common/hrm/hrm_manager.h>


// -----------------------------------------------------------------------------
// T_STATIC functions
// -----------------------------------------------------------------------------
extern HRMSubscriberState * prv_get_subscriber_state_from_ref(HRMSessionRef session);
extern HRMSubscriberState * prv_get_subscriber_state_from_app_id(PebbleTask task,
                                                                 AppInstallId app_id);
extern void prv_read_event_from_buffer_and_consume(CircularBuffer *buffer, PebbleHRMEvent *event);
extern uint32_t prv_num_system_task_events_queued(void);
extern TimerID prv_get_timer_id(void);
extern bool prv_can_turn_sensor_on(void);
extern void prv_charger_event_cb(PebbleEvent *e);


// -----------------------------------------------------------------------------
// HRM Driver fakes
// -----------------------------------------------------------------------------

static struct {
  bool enabled;
} s_hrm_state;

void hrm_enable(HRMDevice *dev) { s_hrm_state.enabled = true; }
void hrm_disable(HRMDevice *dev) { s_hrm_state.enabled = false; }
bool hrm_is_enabled(HRMDevice *dev) { return s_hrm_state.enabled; }

// -----------------------------------------------------------------------------
// Queue Fakes
// -----------------------------------------------------------------------------

static const QueueHandle_t FAKE_APP_QUEUE = (QueueHandle_t) 1337;
static uint32_t s_event_count;
static PebbleEvent s_events_received[16];
signed portBASE_TYPE xQueueGenericSend(QueueHandle_t xQueue, const void * const pvItemToQueue,
                                       TickType_t xTicksToWait, portBASE_TYPE xCopyPosition) {
  cl_assert_equal_i((intptr_t) xQueue, (intptr_t) FAKE_APP_QUEUE);
  if (s_event_count < ARRAY_LENGTH(s_events_received)) {
    s_events_received[s_event_count] = *((PebbleEvent *)pvItemToQueue);
  }
  ++s_event_count;
  return pdTRUE;
}

QueueHandle_t pebble_task_get_to_queue(PebbleTask task) {
  switch (task) {
    case PebbleTask_App:
      return FAKE_APP_QUEUE;
    case PebbleTask_KernelBackground:
      return NULL;
    default:
      WTF;
  }
}

// -----------------------------------------------------------------------------
// Fakes
// -----------------------------------------------------------------------------
bool mfg_info_is_hrm_present(void) {
  return true;
}

static bool s_activity_prefs_heart_rate_is_enabled = true;
bool activity_prefs_heart_rate_is_enabled(void) {
  return s_activity_prefs_heart_rate_is_enabled;
}

bool battery_is_usb_connected(void) {
  return false;
}

// -----------------------------------------------------------------------------
// Test Helpers
// -----------------------------------------------------------------------------
#define TO_SESSION_REF(x) ((HRMSessionRef)(long)(x))

static const HRMData s_hrm_event_data = {
  .led_current_ua = 243,

  .hrm_bpm = 82,
  .hrm_quality = HRMQuality_Excellent,
};

static void prv_fake_send_new_data(void) {
  hrm_manager_new_data_cb(&s_hrm_event_data);
}

static PebbleHRMEvent s_cb_events_1[16];
static int s_num_cb_events_1 = 0;
static void prv_fake_hrm_1_cb(PebbleHRMEvent *event, void *context) {
  if (s_num_cb_events_1 >= ARRAY_LENGTH(s_cb_events_1)) {
    return;
  }
  s_cb_events_1[s_num_cb_events_1++] = *event;
}

static PebbleHRMEvent s_cb_events_2[16];
static int s_num_cb_events_2 = 0;
static void prv_fake_hrm_2_cb(PebbleHRMEvent *event, void *context) {
  if (s_num_cb_events_2 >= ARRAY_LENGTH(s_cb_events_2)) {
    return;
  }
  s_cb_events_2[s_num_cb_events_2++] = *event;
}

static void prv_put_battery_state_change_event(bool is_plugged_in) {
  PebbleEvent e = {
    .type = PEBBLE_BATTERY_STATE_CHANGE_EVENT,
    .battery_state.new_state.is_plugged = is_plugged_in,
  };
  event_put(&e);
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

void test_hrm_manager__initialize(void) {
  // Init time
  fake_rtc_init(100 /*initial_ticks*/, 1465243370);

  stub_pebble_tasks_set_current(PebbleTask_App);
  app_manager_get_task_context()->to_process_event_queue = (void *)0x1;
  fake_system_task_callbacks_cleanup();

  s_activity_prefs_heart_rate_is_enabled = true;
  s_event_count = 0;
  s_num_cb_events_1 = 0;
  s_num_cb_events_2 = 0;
  memset(&s_hrm_state, 0, sizeof(s_hrm_state));
  hrm_manager_init();
  hrm_manager_enable(true);

  fake_event_init();
}

void test_hrm_manager__subscription(void) {
  AppInstallId app_id = 1;
  const uint32_t update_interval_s = 1;
  const uint16_t expire_s = SECONDS_PER_MINUTE;
  HRMFeature features = HRMFeature_BPM;
  HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(app_id, update_interval_s, expire_s,
                                                            features);
  fake_system_task_callbacks_invoke_pending();

  HRMSubscriberState *subscriber = prv_get_subscriber_state_from_ref(session_ref);
  cl_assert(subscriber);
  cl_assert(subscriber->session_ref == session_ref);
  cl_assert(subscriber->expire_utc == rtc_get_time() + expire_s);
  cl_assert(subscriber->update_interval_s == update_interval_s);
  cl_assert(subscriber->features == HRMFeature_BPM);
  cl_assert_equal_b(hrm_is_enabled(HRM), true);

  // We should be able to find it by app ad as well
  cl_assert(sys_hrm_manager_get_app_subscription(app_id) == session_ref);

  // We should be able to get info on it
  AppInstallId  ret_app_id;
  uint32_t ret_update_interval_s;
  uint16_t ret_expire_s;
  HRMFeature ret_features;
  cl_assert(sys_hrm_manager_get_subscription_info(session_ref, &ret_app_id, &ret_update_interval_s,
            &ret_expire_s, &ret_features));
  cl_assert(ret_app_id == app_id);
  cl_assert(ret_update_interval_s == update_interval_s);
  cl_assert(ret_expire_s == expire_s);
  cl_assert(ret_features == features);

  sys_hrm_manager_unsubscribe(session_ref);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(prv_get_subscriber_state_from_ref(session_ref) == NULL);
  cl_assert_equal_b(hrm_is_enabled(HRM), false);
}

// When we cleanup after an app process, its subscription, if any, should get an expiration time
// placed on it
void test_hrm_manager__app_cleanup(void) {
  stub_pebble_tasks_set_current(PebbleTask_App);

  AppInstallId app_id = 1;
  const uint32_t update_interval_s = 1;
  uint16_t expire_s = 0;
  HRMFeature features = HRMFeature_BPM;

  // If we subscribe with no expiration, we should get 0 back
  HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(app_id, update_interval_s, expire_s,
                                                            features);
  cl_assert(sys_hrm_manager_get_app_subscription(app_id) == session_ref);
  uint16_t ret_expire_s;
  sys_hrm_manager_get_subscription_info(session_ref, NULL, NULL, &ret_expire_s, NULL);
  cl_assert_equal_i(ret_expire_s, 0);

  // Now, call the process cleanup. This should place an expiration time on the subscription
  hrm_manager_process_cleanup(PebbleTask_App, app_id);
  cl_assert(sys_hrm_manager_get_app_subscription(app_id) == session_ref);

  sys_hrm_manager_get_subscription_info(session_ref, NULL, NULL, &ret_expire_s, NULL);
  cl_assert_equal_i(ret_expire_s, HRM_MANAGER_APP_EXIT_EXPIRATION_SEC);

  sys_hrm_manager_unsubscribe(session_ref);
}


// Test that app subscriptions expire correctly
void test_hrm_manager__app_expiration(void) {
  AppInstallId  app_id = 1;
  const uint16_t expire_s = SECONDS_PER_MINUTE;
  HRMSessionRef  session_ref = sys_hrm_manager_app_subscribe(app_id, 1, expire_s, HRMFeature_BPM);
  cl_assert(sys_hrm_manager_get_app_subscription(app_id) == session_ref);

  prv_fake_send_new_data();

  // We should get the BPM event
  cl_assert_equal_i(s_event_count, 1);
  cl_assert_equal_i(s_events_received[0].type, PEBBLE_HRM_EVENT);
  cl_assert_equal_i(s_events_received[0].hrm.event_type, HRMEvent_BPM);

  // Subscribe again before we expire, should get the same session ref back
  HRMSessionRef new_session_ref = sys_hrm_manager_app_subscribe(app_id, 1, expire_s,
                                                                HRMFeature_BPM);
  cl_assert(new_session_ref == session_ref);

  // Now advance time past the expiration time
  rtc_set_time(rtc_get_time() + expire_s + 1);

  // Send more data, the subscription should get expired now
  prv_fake_send_new_data();
  cl_assert_equal_i(s_event_count, 3);
  cl_assert_equal_i(s_events_received[1].type, PEBBLE_HRM_EVENT);
  cl_assert_equal_i(s_events_received[1].hrm.event_type, HRMEvent_BPM);
  cl_assert_equal_i(s_events_received[2].type, PEBBLE_HRM_EVENT);
  cl_assert_equal_i(s_events_received[2].hrm.event_type, HRMEvent_SubscriptionExpiring);

  // Subscription should be gone
  cl_assert(prv_get_subscriber_state_from_ref(session_ref) == NULL);
  cl_assert(sys_hrm_manager_get_app_subscription(app_id) == HRM_INVALID_SESSION_REF);
}


// Test that system subscriptions expire correctly
void test_hrm_manager__kernel_expiration(void) {
  stub_pebble_tasks_set_current(PebbleTask_KernelBackground);
  const uint16_t expire_s = SECONDS_PER_MINUTE;
  HRMSessionRef session_ref = hrm_manager_subscribe_with_callback(INSTALL_ID_INVALID, 1,
                                                                  expire_s, HRMFeature_BPM,
                                                                  prv_fake_hrm_1_cb, NULL);
  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();

  // Make sure we got the expected data
  cl_assert_equal_i(s_num_cb_events_1, 1);
  cl_assert_equal_i(s_cb_events_1[0].event_type, HRMEvent_BPM);
  cl_assert_equal_i(s_cb_events_1[0].bpm.bpm, s_hrm_event_data.hrm_bpm);
  cl_assert_equal_i(s_cb_events_1[0].bpm.quality, s_hrm_event_data.hrm_quality);


  // Now advance time to just before the expiration time
  rtc_set_time(rtc_get_time() + expire_s - 1);

  // Send more data, the callback should get the expiring event
  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();

  cl_assert_equal_i(s_num_cb_events_1, 3);
  cl_assert_equal_i(s_cb_events_1[1].event_type, HRMEvent_SubscriptionExpiring);
  cl_assert_equal_i(s_cb_events_1[2].event_type, HRMEvent_BPM);


  // Now advance time to past expiration time
  rtc_set_time(rtc_get_time() + expire_s + 1);

  // Send more data, the subscription should go away now
  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();
  cl_assert(prv_get_subscriber_state_from_ref(session_ref) == NULL);
}


void test_hrm_manager__subscribe_multiple(void) {
  const int num_refs = 3;
  HRMSessionRef session_refs[num_refs];
  AppInstallId app_ids[num_refs];

  stub_pebble_tasks_set_current(PebbleTask_App);
  AppInstallId  app_id = 1;
  for (int i = 0; i < num_refs; ++i, app_id++) {
    const uint16_t expire_s = SECONDS_PER_MINUTE;
    session_refs[i] = sys_hrm_manager_app_subscribe(app_id, 1, expire_s, HRMFeature_BPM);
    app_ids[i] = app_id;
  }

  // Ensure sure all can be found
  for (int i = 0; i < num_refs; ++i) {
    cl_assert(prv_get_subscriber_state_from_ref(session_refs[i]));
    cl_assert(prv_get_subscriber_state_from_app_id(PebbleTask_App, app_ids[i]));
  }

  cl_assert(prv_get_subscriber_state_from_ref(HRM_INVALID_SESSION_REF) == NULL);
  cl_assert(prv_get_subscriber_state_from_app_id(PebbleTask_App, INSTALL_ID_INVALID) == NULL);

  // Unsubscribe, HRM should be disabled after
  for (int i = 0; i < num_refs; ++i) {
    sys_hrm_manager_unsubscribe(session_refs[i]);
    cl_assert(prv_get_subscriber_state_from_ref(session_refs[i]) == NULL);
    cl_assert(prv_get_subscriber_state_from_app_id(PebbleTask_App, app_ids[i]) == NULL);
  }
  cl_assert_equal_b(hrm_is_enabled(HRM), false);
}

void test_hrm_manager__feature_callbacks(void) {
  const int num_refs = 2;
  HRMSessionRef session_refs[num_refs];

  AppInstallId  app_id = 1;
  for (int i = 0; i < num_refs; ++i, app_id++) {
    const uint16_t expire_s = SECONDS_PER_MINUTE;
    session_refs[i] = sys_hrm_manager_app_subscribe(app_id, 1, expire_s, HRMFeature_BPM);
  }

  prv_fake_send_new_data();

  // One event for each app subscriber
  cl_assert_equal_i(s_event_count, num_refs);

  for (int i = 0; i < num_refs; ++i) {
    sys_hrm_manager_unsubscribe(session_refs[i]);
  }
}

void test_hrm_manager__no_feature_callbacks(void) {
  // Subscribe and fake data being sent
  AppInstallId  app_id = 1;
  const uint16_t expire_s = SECONDS_PER_MINUTE;
  HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(app_id, 1, expire_s,
                                                            0 /* No feature */);

  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();

  // HRM should be enabled, subscriber should exist, no callbacks triggered.
  cl_assert_equal_b(hrm_is_enabled(HRM), true);
  cl_assert(prv_get_subscriber_state_from_ref(session_ref));

  cl_assert_equal_i(s_event_count, 0);

  sys_hrm_manager_unsubscribe(session_ref);
}

void test_hrm_manager__different_feature_callbacks(void) {
  AppInstallId  app_id = 1;
  const uint16_t expire_s = SECONDS_PER_MINUTE;
  HRMSessionRef bpm_session = sys_hrm_manager_app_subscribe(app_id, 1, expire_s, HRMFeature_BPM);
  HRMSessionRef led_session = sys_hrm_manager_app_subscribe(app_id + 1, 1, expire_s,
                                                            HRMFeature_LEDCurrent);
  HRMSessionRef all_session = sys_hrm_manager_app_subscribe(app_id + 2, 1, expire_s,
                                                            HRMFeature_BPM|HRMFeature_LEDCurrent);
  HRMSessionRef no_session = sys_hrm_manager_app_subscribe(app_id + 3, 1, expire_s,
                                                            0 /* no features */);

  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();

  // Expect 4 events: 1 for BPM, 1 for LED, 2 for subscribing to all, none for no feature.
  cl_assert_equal_i(s_event_count, 4);

  sys_hrm_manager_unsubscribe(bpm_session);
  sys_hrm_manager_unsubscribe(led_session);
  sys_hrm_manager_unsubscribe(all_session);
  sys_hrm_manager_unsubscribe(no_session);
}

void test_hrm_manager__multiple_feature_callbacks(void) {
  const int num_refs = 2;
  HRMSessionRef session_refs[num_refs];

  AppInstallId  app_id = 1;
  for (int i = 0; i < num_refs; ++i, app_id++) {
    session_refs[i] = TO_SESSION_REF(i+1);
    const uint16_t expire_s = SECONDS_PER_MINUTE;
    sys_hrm_manager_app_subscribe(app_id, 1, expire_s, HRMFeature_BPM|HRMFeature_LEDCurrent);
  }

  prv_fake_send_new_data();

  // Two events for each app subscriber
  cl_assert_equal_i(s_event_count, num_refs * 2);

  for (int i = 0; i < num_refs; ++i) {
    sys_hrm_manager_unsubscribe(session_refs[i]);
  }
}

void test_hrm_manager__system_task_data_callback(void) {
  stub_pebble_tasks_set_current(PebbleTask_KernelBackground);
  s_num_cb_events_1 = 0;

  const uint16_t expire_s = SECONDS_PER_MINUTE;
  HRMSessionRef session_ref = hrm_manager_subscribe_with_callback(INSTALL_ID_INVALID, 1,
                                                                  expire_s, HRMFeature_BPM,
                                                                  prv_fake_hrm_1_cb, NULL);

  fake_system_task_callbacks_invoke_pending();
  prv_fake_send_new_data();

  // Make sure event is queued up
  cl_assert_equal_i(prv_num_system_task_events_queued(), 1);

  // Make sure we successfully consume the event
  fake_system_task_callbacks_invoke_pending();

  // Make sure we got the expected data
  cl_assert_equal_i(s_num_cb_events_1, 1);
  cl_assert_equal_i(s_cb_events_1[0].event_type, HRMEvent_BPM);
  cl_assert_equal_i(s_cb_events_1[0].bpm.bpm, s_hrm_event_data.hrm_bpm);
  cl_assert_equal_i(s_cb_events_1[0].bpm.quality, s_hrm_event_data.hrm_quality);

  sys_hrm_manager_unsubscribe(session_ref);
}

// Test having 2 different KernelBG subscribers. The data should only get pushed into the
// circular buffer once, but both clients should get it
void test_hrm_manager__multiple_system_task_data_callbacks(void) {
  stub_pebble_tasks_set_current(PebbleTask_KernelBackground);
  s_num_cb_events_1 = 0;
  s_num_cb_events_2 = 0;

  const uint16_t expire_s = SECONDS_PER_MINUTE;
  HRMSessionRef session_ref_1 = hrm_manager_subscribe_with_callback(INSTALL_ID_INVALID, 1,
                                                                  expire_s, HRMFeature_BPM,
                                                                  prv_fake_hrm_1_cb, NULL);
  fake_system_task_callbacks_invoke_pending();
  HRMSessionRef session_ref_2 = hrm_manager_subscribe_with_callback(INSTALL_ID_INVALID, 1,
                                                                  expire_s, HRMFeature_BPM,
                                                                  prv_fake_hrm_2_cb, NULL);
  fake_system_task_callbacks_invoke_pending();
  prv_fake_send_new_data();

  // Make sure only 1 callback (and hence one circular buffer entry) got queued up
  cl_assert_equal_i(prv_num_system_task_events_queued(), 1);

  // Make sure we successfully get the event sent to both subscribers
  fake_system_task_callbacks_invoke_pending();

  // Make sure we got the expected data to both clients
  cl_assert_equal_i(s_num_cb_events_1, 1);
  cl_assert_equal_i(s_cb_events_1[0].event_type, HRMEvent_BPM);
  cl_assert_equal_i(s_cb_events_1[0].bpm.bpm, s_hrm_event_data.hrm_bpm);
  cl_assert_equal_i(s_cb_events_1[0].bpm.quality, s_hrm_event_data.hrm_quality);

  cl_assert_equal_i(s_num_cb_events_1, 1);
  cl_assert_equal_i(s_cb_events_1[0].event_type, HRMEvent_BPM);
  cl_assert_equal_i(s_cb_events_1[0].bpm.bpm, s_hrm_event_data.hrm_bpm);
  cl_assert_equal_i(s_cb_events_1[0].bpm.quality, s_hrm_event_data.hrm_quality);

  sys_hrm_manager_unsubscribe(session_ref_1);
  sys_hrm_manager_unsubscribe(session_ref_2);
}

void test_hrm_manager__set_features(void) {
  AppInstallId  app_id = 1;
  const uint16_t expire_s = SECONDS_PER_MINUTE;
  const HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(app_id, 1, expire_s,
                                                                  HRMFeature_BPM);
  HRMSubscriberState *state = prv_get_subscriber_state_from_ref(session_ref);

  // Starts off with BPM enabled
  cl_assert_equal_i(state->features, HRMFeature_BPM);

  // Change to only LED Current
  sys_hrm_manager_set_features(session_ref, HRMFeature_LEDCurrent);
  cl_assert_equal_i(state->features, HRMFeature_LEDCurrent);

  // Change to LEDCurrent + BPM
  sys_hrm_manager_set_features(session_ref, HRMFeature_LEDCurrent | HRMFeature_BPM);
  cl_assert_equal_i(state->features, HRMFeature_LEDCurrent | HRMFeature_BPM);
}

void test_hrm_manager__set_update_internal(void) {
  AppInstallId  app_id = 1;
  const uint16_t expire_a_s = SECONDS_PER_MINUTE;
  uint32_t update_interval_a_s = 1;
  HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(app_id, update_interval_a_s,
                                                            expire_a_s, HRMFeature_BPM);
  HRMSubscriberState *state = prv_get_subscriber_state_from_ref(session_ref);
  cl_assert(state->update_interval_s == update_interval_a_s);
  cl_assert(state->expire_utc == rtc_get_time() + expire_a_s);

  // Change update interval and expiration
  const uint16_t expire_b_s = 2 * SECONDS_PER_MINUTE;
  // TODO: PBL-37298 Support subscribing to different data rates
  uint32_t update_interval_b_s = 1;
  sys_hrm_manager_set_update_interval(session_ref, update_interval_b_s, expire_b_s);
  cl_assert(prv_get_subscriber_state_from_ref(session_ref) == state);
  cl_assert(state->update_interval_s == update_interval_b_s);
  cl_assert(state->expire_utc == rtc_get_time() + expire_b_s);
}

#define NUM_TEST_EVENTS 2
void test_hrm_manager__circular_buffer_event_copy(void) {
  // Make sure there will be unaligned data
  const uint16_t buf_size = sizeof(PebbleHRMEvent) * 2 + sizeof(PebbleHRMEvent) / 2;
  uint8_t buffer[buf_size];

  CircularBuffer cb;
  circular_buffer_init(&cb, buffer, buf_size);

  PebbleHRMEvent event[NUM_TEST_EVENTS] = {
    { .event_type = HRMEvent_BPM, .bpm = { .bpm = 65, .quality = 5 } },
    { .event_type = HRMEvent_LEDCurrent, .led = { .current_ua = 243 } },
  };
  { // These events will insert properly aligned in the buffer
    for (int i = 0; i < NUM_TEST_EVENTS; ++i) {
      circular_buffer_write(&cb, (const uint8_t *)&event[i], sizeof(PebbleHRMEvent));
    }
    PebbleHRMEvent out_event[NUM_TEST_EVENTS];
    for (int i = 0; i < NUM_TEST_EVENTS; ++i) {
      prv_read_event_from_buffer_and_consume(&cb, &out_event[i]);
      cl_assert_equal_b(memcmp(&event[i], &out_event[i], sizeof(PebbleHRMEvent)), false);
    }
  }
  { // Test reading back unaligned
    for (int i = 0; i < NUM_TEST_EVENTS; ++i) {
      circular_buffer_write(&cb, (const uint8_t *)&event[i], sizeof(PebbleHRMEvent));
    }
    PebbleHRMEvent out_event[NUM_TEST_EVENTS];
    for (int i = 0; i < NUM_TEST_EVENTS; ++i) {
      prv_read_event_from_buffer_and_consume(&cb, &out_event[i]);
      cl_assert_equal_b(memcmp(&event[i], &out_event[i], sizeof(PebbleHRMEvent)), false);
    }
  }
}
#undef NUM_TEST_EVENTS

// Test the enable and disable functionality across subscriptions
void test_hrm_manager__enable_disable(void) {
  // 1. Disabling and then enabling with no subscriber should leave the HRM off
  hrm_manager_enable(false);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), false);
  hrm_manager_enable(true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), false);

  // 2. Subscribing while disabled should not enable the hrm
  hrm_manager_enable(false);
  fake_system_task_callbacks_invoke_pending();
  HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(1, 1, SECONDS_PER_MINUTE,
                                                            HRMFeature_BPM);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), false);

  // 3. Enabling with a subscriber should turn HRM on
  hrm_manager_enable(true);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), true);

  // 4. Disabling with a Subscriber should disable the HRM
  hrm_manager_enable(false);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), false);

  sys_hrm_manager_unsubscribe(session_ref);
}


// Advance time the given number of milliseconds
static void prv_advance_time_ms(uint32_t ms) {
  RtcTicks delta_ticks = milliseconds_to_ticks(ms);
  fake_rtc_set_ticks(rtc_get_ticks() + delta_ticks);
  rtc_set_time(rtc_get_time() + ms / MS_PER_SECOND);
}

// Test that we handle different update intervals correctly
void test_hrm_manager__update_interval(void) {
  AppInstallId app_id = 1;
  uint32_t update_interval_s = 600;
  const uint16_t expire_s = 30 * SECONDS_PER_MINUTE;
  HRMFeature features = HRMFeature_BPM;
  HRMSessionRef session_ref = sys_hrm_manager_app_subscribe(app_id, update_interval_s, expire_s,
                                                            features);
  fake_system_task_callbacks_invoke_pending();

  // Should start out enabled before we get the first good reading
  cl_assert_equal_b(hrm_is_enabled(HRM), true);

  // Send some data while enabled
  int num_updates;
  for (num_updates = 0; num_updates < 1000 && hrm_is_enabled(HRM); num_updates++) {
    prv_fake_send_new_data();
    fake_system_task_callbacks_invoke_pending();
  }

  // Should be disabled relatively quickly since we don't need another reading for another 600
  // seconds
  cl_assert(num_updates <= HRM_CHECK_SENSOR_DISABLE_COUNT);

  // The timer should be set to fire just before we need another update
  uint32_t timeout_ms = stub_new_timer_timeout(prv_get_timer_id());
  cl_assert_equal_i(timeout_ms, (update_interval_s - HRM_SENSOR_SPIN_UP_SEC) * MS_PER_SECOND);


  // Fire the timer after the elapsed time, make sure we are re-enabled after that
  prv_advance_time_ms(timeout_ms);
  stub_new_timer_fire(prv_get_timer_id());
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), true);


  // Send the next data, should be disabled again after that
  for (num_updates = 0; num_updates < 1000 && hrm_is_enabled(HRM); num_updates++) {
    prv_fake_send_new_data();
    fake_system_task_callbacks_invoke_pending();
  }
  cl_assert(num_updates <= HRM_CHECK_SENSOR_DISABLE_COUNT);
  prv_advance_time_ms(1000);


  // Now, change the update interval to 10 seconds. That should re-enable the sensor immediately
  update_interval_s = 10;
  cl_assert(sys_hrm_manager_set_update_interval(session_ref, update_interval_s, expire_s));
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), true);
  prv_advance_time_ms(1000);

  // Send the next data, should still be enabled since the interval is less that the spin-up
  // time
  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), true);
  prv_advance_time_ms(1000);


  // Now add a 10 minute subscription back in.
  AppInstallId app_id_2 = 2;
  sys_hrm_manager_app_subscribe(app_id_2, 600, expire_s, features);
  fake_system_task_callbacks_invoke_pending();

  // We should stay enabled after each update because we still have the 10 second subscription
  // too
  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), true);
  prv_advance_time_ms(1000);

  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), true);
  prv_advance_time_ms(1000);


  // Remove the 10 second subscription - We should get disabled after the next update now
  sys_hrm_manager_unsubscribe(session_ref);
  prv_fake_send_new_data();
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_b(hrm_is_enabled(HRM), false);
}

void test_hrm_manager__can_turn_sensor_on(void) {
  fake_event_set_callback(prv_charger_event_cb);

  cl_assert(prv_can_turn_sensor_on());

  // Add a subscription so we have a reason to turn the sensor on (if the conditions are right)
  sys_hrm_manager_app_subscribe(1, 1, 60, HRMFeature_BPM);
  fake_system_task_callbacks_invoke_pending();

  // Test run level changes
  hrm_manager_enable(false);
  cl_assert(!prv_can_turn_sensor_on());
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!hrm_is_enabled(HRM));

  hrm_manager_enable(true);
  cl_assert(prv_can_turn_sensor_on());
  fake_system_task_callbacks_invoke_pending();
  cl_assert(hrm_is_enabled(HRM));

  // Test the pref changes
  s_activity_prefs_heart_rate_is_enabled = false;
  hrm_manager_handle_prefs_changed();
  cl_assert(!prv_can_turn_sensor_on());
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!hrm_is_enabled(HRM));

  s_activity_prefs_heart_rate_is_enabled = true;
  hrm_manager_handle_prefs_changed();
  cl_assert(prv_can_turn_sensor_on());
  fake_system_task_callbacks_invoke_pending();
  cl_assert(hrm_is_enabled(HRM));

  // Test charging state changes
  prv_put_battery_state_change_event(true /* is plugged in */);
  cl_assert(!prv_can_turn_sensor_on());
  fake_system_task_callbacks_invoke_pending();
  cl_assert(!hrm_is_enabled(HRM));

  prv_put_battery_state_change_event(false /* is plugged in */);
  cl_assert(prv_can_turn_sensor_on());
  fake_system_task_callbacks_invoke_pending();
  cl_assert(hrm_is_enabled(HRM));
}

