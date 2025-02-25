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

#include "applib/event_service_client.h"
#include "services/normal/accessory/smartstrap_attribute.h"
#include "kernel/pbl_malloc.h"

#include "fake_smartstrap_profiles.h"
#include "fake_smartstrap_state.h"
#include "fake_system_task.h"

#include "stubs_app_state.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_serial.h"

#define NON_NULL_MBUF ((MBuf *)1)
#define assert_result_ok(result) cl_assert(result == SmartstrapResultOk)
#define assert_result_invalid(result) cl_assert(result == SmartstrapResultInvalidArgs)
#define assert_result_busy(result) cl_assert(result == SmartstrapResultBusy)

typedef void (*EventServiceEventHandler)(PebbleEvent *e, void *context);
typedef struct {
  bool active;
  SmartstrapAttribute *attribute;
  size_t length;
} PendingInfo;

static EventServiceEventHandler s_event_handler;
static PendingInfo s_pending_did_read;
static PendingInfo s_pending_did_write;
static PendingInfo s_pending_notified;


// Stubs / fakes

void event_service_client_subscribe(EventServiceInfo *info) {
  cl_assert(info->type == PEBBLE_SMARTSTRAP_EVENT);
  s_event_handler = info->handler;
}

void event_service_client_unsubscribe(EventServiceInfo *info) {
  cl_assert(info->type == PEBBLE_SMARTSTRAP_EVENT);
  s_event_handler = NULL;
}

bool process_manager_send_event_to_process(PebbleTask task, PebbleEvent *e) {
  cl_assert(task == PebbleTask_App);
  cl_assert(e->type == PEBBLE_SMARTSTRAP_EVENT);
  cl_assert(s_event_handler);
  s_event_handler(e, NULL);
  return true;
}

void smartstrap_cancel_send(void) {
}


// Helper functions
static void prv_prepare_for_did_read(SmartstrapAttribute *attribute, uint16_t read_length) {
  cl_assert(!s_pending_did_read.active);
  s_pending_did_read = (PendingInfo) {
    .active = true,
    .attribute = attribute,
    .length = read_length
  };
}

static void prv_did_read_handler(SmartstrapAttribute *attribute, SmartstrapResult result,
                                 const uint8_t *data, size_t length) {
  cl_assert(data == (uint8_t *)attribute);
  cl_assert(result == SmartstrapResultOk);
  cl_assert(s_pending_did_read.active);
  cl_assert(s_pending_did_read.attribute == attribute);
  cl_assert(s_pending_did_read.length == length);
  s_pending_did_read.active = false;
}

static void prv_prepare_for_did_write(SmartstrapAttribute *attribute) {
  cl_assert(!s_pending_did_write.active);
  s_pending_did_write = (PendingInfo) {
    .active = true,
    .attribute = attribute
  };
}

static void prv_did_write_handler(SmartstrapAttribute *attribute, SmartstrapResult result) {
  cl_assert(result == SmartstrapResultOk);
  cl_assert(s_pending_did_write.active);
  cl_assert(s_pending_did_write.attribute == attribute);
  s_pending_did_write.active = false;
}

static void prv_prepare_for_notified(SmartstrapAttribute *attribute) {
  cl_assert(!s_pending_notified.active);
  s_pending_notified = (PendingInfo) {
    .active = true,
    .attribute = attribute
  };
}

static void prv_notified_handler(SmartstrapAttribute *attribute) {
  cl_assert(s_pending_notified.active);
  cl_assert(s_pending_notified.attribute == attribute);
  s_pending_notified.active = false;
}


// Setup

void test_app_smartstrap__initialize(void) {
  smartstrap_attribute_init();
  app_smartstrap_subscribe((SmartstrapHandlers) {
    .did_read = prv_did_read_handler,
    .did_write = prv_did_write_handler,
    .notified = prv_notified_handler
  });
}

void test_app_smartstrap__cleanup(void) {
}


// Tests

void test_app_smartstrap__invalid_args(void) {
  // create test attribute
  SmartstrapAttribute *attr = app_smartstrap_attribute_create(0x1111, 0x2222, 100);

  // smartstrap_attribute_create()
  cl_assert(app_smartstrap_attribute_create(0x1111, 0x2222, 0) == NULL);

  // smartstrap_attribute_destroy()
  app_smartstrap_attribute_destroy(NULL);

  // smartstrap_attribute_get_*_id()
  cl_assert(app_smartstrap_attribute_get_service_id(NULL) == 0);
  cl_assert(app_smartstrap_attribute_get_attribute_id(NULL) == 0);

  // smartstrap_attribute_begin_write()
  uint8_t *buffer;
  size_t buffer_len;
  assert_result_invalid(app_smartstrap_attribute_begin_write(NULL, NULL, NULL));
  assert_result_invalid(app_smartstrap_attribute_begin_write(NULL, &buffer, NULL));
  assert_result_invalid(app_smartstrap_attribute_begin_write(NULL, NULL, &buffer_len));
  assert_result_invalid(app_smartstrap_attribute_begin_write(NULL, &buffer, &buffer_len));
  assert_result_invalid(app_smartstrap_attribute_begin_write(attr, NULL, NULL));
  assert_result_invalid(app_smartstrap_attribute_begin_write(attr, &buffer, NULL));
  assert_result_invalid(app_smartstrap_attribute_begin_write(attr, NULL, &buffer_len));

  // smartstrap_attribute_end_write()
  assert_result_invalid(app_smartstrap_attribute_end_write(NULL, 0, false));
  assert_result_invalid(app_smartstrap_attribute_end_write(NULL, 0, true));
  assert_result_invalid(app_smartstrap_attribute_end_write(NULL, 100, false));
  assert_result_invalid(app_smartstrap_attribute_end_write(NULL, 100, true));
  assert_result_invalid(app_smartstrap_attribute_end_write(attr, 0, false));
  assert_result_invalid(app_smartstrap_attribute_end_write(attr, 0, true));
  assert_result_invalid(app_smartstrap_attribute_end_write(attr, 100, false));
  assert_result_invalid(app_smartstrap_attribute_end_write(attr, 100, true));

  // smartstrap_attribute_read()
  assert_result_invalid(app_smartstrap_attribute_read(NULL));

  // destroy test attribute
  app_smartstrap_attribute_destroy(attr);
}

void test_app_smartstrap__check_ids(void) {
  // create an attribute
  SmartstrapAttribute *attr = app_smartstrap_attribute_create(0x1111, 0x2222, 100);
  cl_assert(attr != NULL);

  // verify the ids
  cl_assert(app_smartstrap_attribute_get_service_id(attr) == 0x1111);
  cl_assert(app_smartstrap_attribute_get_attribute_id(attr) == 0x2222);

  // destroy the attribute
  app_smartstrap_attribute_destroy(attr);

  // verify that we can no longer get the ids
  cl_assert(app_smartstrap_attribute_get_service_id(attr) == 0);
  cl_assert(app_smartstrap_attribute_get_attribute_id(attr) == 0);
}

void test_app_smartstrap__create_duplicate(void) {
  // create the attribute once
  SmartstrapAttribute *attr = app_smartstrap_attribute_create(0x1111, 0x2222, 100);
  cl_assert(attr != NULL);

  // try to create it again
  SmartstrapAttribute *attr_dup = app_smartstrap_attribute_create(0x1111, 0x2222, 100);
  cl_assert(attr_dup == NULL);

  // destroy the attribute
  app_smartstrap_attribute_destroy(attr);

  // destroy again (shouldn't do anything bad)
  app_smartstrap_attribute_destroy(attr);
}

void test_app_smartstrap__read(void) {
  // create the attribute
  SmartstrapAttribute *attr = app_smartstrap_attribute_create(0x1111, 0x2222, 100);

  // start a read request
  stub_pebble_tasks_set_current(PebbleTask_App);
  assert_result_ok(app_smartstrap_attribute_read(attr));

  // attempt to issue another read request
  assert_result_busy(app_smartstrap_attribute_read(attr));

  // trigger the read request to be sent and expect a did_write handler call
  stub_pebble_tasks_set_current(PebbleTask_KernelBackground);
  prv_prepare_for_did_write(attr);
  cl_assert(smartstrap_attribute_send_pending());
  cl_assert(!s_pending_did_write.active);

  // attempt to issue another read request (should report busy)
  assert_result_busy(app_smartstrap_attribute_read(attr));

  // check that it was sent successfully
  SmartstrapRequest request = {
    .service_id = 0x1111,
    .attribute_id = 0x2222,
    .write_mbuf = NULL,
    .read_mbuf = NON_NULL_MBUF,
    .timeout_ms = SMARTSTRAP_TIMEOUT_DEFAULT
  };
  fake_smartstrap_profiles_check_request_params(&request);

  // fake the response and expect a did_read handler call
  prv_prepare_for_did_read(attr, 10);
  smartstrap_attribute_send_event(SmartstrapDataReceivedEvent, SmartstrapProfileGenericService,
                                  SmartstrapResultOk, 0x1111, 0x2222, 10);
  cl_assert(!s_pending_did_read.active);

  // destroy the attribute
  app_smartstrap_attribute_destroy(attr);
}

void test_app_smartstrap__write(void) {
  // create the attribute
  SmartstrapAttribute *attr = app_smartstrap_attribute_create(0x1111, 0x2222, 100);

  // start a write request
  stub_pebble_tasks_set_current(PebbleTask_App);
  uint8_t *write_buffer = NULL;
  size_t write_length = 0;
  assert_result_ok(app_smartstrap_attribute_begin_write(attr, &write_buffer, &write_length));
  cl_assert(write_buffer == (uint8_t *)attr);
  cl_assert(write_length == 100);

  // attempt to start another write request
  assert_result_busy(app_smartstrap_attribute_read(attr));
  uint8_t *write_buffer2 = NULL;
  size_t write_length2 = 0;
  assert_result_busy(app_smartstrap_attribute_begin_write(attr, &write_buffer2, &write_length2));
  cl_assert(write_buffer2 == NULL);
  cl_assert(write_length2 == 0);

  // end the write request without sending anything
  assert_result_invalid(app_smartstrap_attribute_end_write(attr, 0, false));

  // start the write request again
  write_buffer = NULL;
  write_length = 0;
  assert_result_ok(app_smartstrap_attribute_begin_write(attr, &write_buffer, &write_length));
  cl_assert(write_buffer == (uint8_t *)attr);
  cl_assert(write_length == 100);

  // end the write request
  assert_result_ok(app_smartstrap_attribute_end_write(attr, 100, false));

  // trigger the write request to be sent
  stub_pebble_tasks_set_current(PebbleTask_KernelBackground);
  cl_assert(smartstrap_attribute_send_pending());

  // check that it was sent successfully
  SmartstrapRequest request = {
    .service_id = 0x1111,
    .attribute_id = 0x2222,
    .write_mbuf = NON_NULL_MBUF,
    .read_mbuf = NULL,
    .timeout_ms = SMARTSTRAP_TIMEOUT_DEFAULT
  };

  // fake the ACK and expect a did_write handler call
  fake_smartstrap_profiles_check_request_params(&request);
  prv_prepare_for_did_write(attr);
  smartstrap_attribute_send_event(SmartstrapDataReceivedEvent, SmartstrapProfileGenericService,
                                  SmartstrapResultOk, 0x1111, 0x2222, 100);
  cl_assert(!s_pending_did_write.active);

  // destroy the attribute
  app_smartstrap_attribute_destroy(attr);
}

void test_app_smartstrap__write_read(void) {
  // create the attribute
  SmartstrapAttribute *attr = app_smartstrap_attribute_create(0x1111, 0x2222, 100);

  // start a write request
  stub_pebble_tasks_set_current(PebbleTask_App);
  uint8_t *write_buffer = NULL;
  size_t write_length = 0;
  assert_result_ok(app_smartstrap_attribute_begin_write(attr, &write_buffer, &write_length));
  cl_assert(write_buffer == (uint8_t *)attr);
  cl_assert(write_length == 100);

  // end the write request without sending anything
  assert_result_invalid(app_smartstrap_attribute_end_write(attr, 0, true));

  // start the write request again
  write_buffer = NULL;
  write_length = 0;
  assert_result_ok(app_smartstrap_attribute_begin_write(attr, &write_buffer, &write_length));
  cl_assert(write_buffer == (uint8_t *)attr);
  cl_assert(write_length == 100);

  // end the write request with request_read=true
  assert_result_ok(app_smartstrap_attribute_end_write(attr, 100, true));

  // trigger the write request to be sent and expect a did_write handler call
  stub_pebble_tasks_set_current(PebbleTask_KernelBackground);
  prv_prepare_for_did_write(attr);
  cl_assert(smartstrap_attribute_send_pending());
  cl_assert(!s_pending_did_write.active);

  // check that it was sent successfully
  SmartstrapRequest request = {
    .service_id = 0x1111,
    .attribute_id = 0x2222,
    .write_mbuf = NON_NULL_MBUF,
    .read_mbuf = NON_NULL_MBUF,
    .timeout_ms = SMARTSTRAP_TIMEOUT_DEFAULT
  };

  // fake the response and expect a did_write and a did_read handler call
  fake_smartstrap_profiles_check_request_params(&request);
  prv_prepare_for_did_read(attr, 100);
  smartstrap_attribute_send_event(SmartstrapDataReceivedEvent, SmartstrapProfileGenericService,
                                  SmartstrapResultOk, 0x1111, 0x2222, 100);
  cl_assert(!s_pending_did_read.active);

  // destroy the attribute
  app_smartstrap_attribute_destroy(attr);
}

void test_app_smartstrap__notify(void) {
  // create the attribute
  SmartstrapAttribute *attr = app_smartstrap_attribute_create(0x1111, 0x2222, 100);

  // send a notification and expect a notified handler call
  prv_prepare_for_notified(attr);
  smartstrap_attribute_send_event(SmartstrapNotifyEvent, SmartstrapProfileGenericService,
                                  SmartstrapResultOk, 0x1111, 0x2222, 0);
  cl_assert(!s_pending_notified.active);

  // send a notification for a non-created attribute which shouldn't cause a notified handler call
  smartstrap_attribute_send_event(SmartstrapNotifyEvent, SmartstrapProfileGenericService,
                                  SmartstrapResultOk, 0x1111, 0x3333, 0);

  // destroy the attribute
  app_smartstrap_attribute_destroy(attr);

  // send a notification for the destroyed attribute which shouldn't cause a notified handler call
  smartstrap_attribute_send_event(SmartstrapNotifyEvent, SmartstrapProfileGenericService,
                                  SmartstrapResultOk, 0x1111, 0x2222, 0);
}
