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

#include "services/normal/accessory/smartstrap_comms.h"
#include "util/mbuf.h"

#include <string.h>

#include "stubs_freertos.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_serial.h"

#include "fake_accessory.h"
#include "fake_new_timer.h"
#include "fake_smartstrap_profiles.h"
#include "fake_smartstrap_state.h"
#include "fake_system_task.h"


// Fakes
static struct {
  bool enabled;
  int after_bytes;
} s_faked_bus_contention;

bool accessory_bus_contention_detected(void) {
  return s_faked_bus_contention.enabled && (s_faked_bus_contention.after_bytes-- <= 0);
}


// Setup

void test_smartstrap_comms__initialize(void) {
  smartstrap_comms_init();
  s_faked_bus_contention.enabled = false;
}

void test_smartstrap_comms__cleanup(void) {
}


// Helper functions

static void prv_do_send(MBuf *write_mbuf, MBuf *read_mbuf, uint8_t *expect_data,
                        int expect_length) {
  // send the data out the smartstrap port
  smartstrap_fsm_state_reset();
  smartstrap_state_lock();
  SmartstrapResult result = smartstrap_send(SmartstrapProfileRawData, write_mbuf, read_mbuf, 1000);
  smartstrap_state_unlock();
  cl_assert(result == SmartstrapResultOk);
  if (read_mbuf) {
    cl_assert(smartstrap_fsm_state_get() == SmartstrapStateReadInProgress);
  } else {
    cl_assert(smartstrap_fsm_state_get() == SmartstrapStateReadReady);
  }

  // verify the data that was sent out the accessory port
  uint8_t *buffer;
  int length;
  fake_accessory_get_buffer(&buffer, &length);
  cl_assert(length == expect_length);
  for (int i = 0; i < length; i++) {
    cl_assert(buffer[i] == expect_data[i]);
  }
}

static void prv_do_send_bus_contention(MBuf *write_mbuf, MBuf *read_mbuf, uint8_t *expect_data,
                                       int expect_length) {
  const int BUS_CONTENTION_AFTER = 5;
  // setup faked bus contention
  s_faked_bus_contention.enabled = true;
  s_faked_bus_contention.after_bytes = BUS_CONTENTION_AFTER;
  // send the data out the smartstrap port
  smartstrap_fsm_state_reset();
  smartstrap_state_lock();
  SmartstrapResult result = smartstrap_send(SmartstrapProfileRawData, write_mbuf, read_mbuf, 1000);
  smartstrap_state_unlock();
  cl_assert(result == SmartstrapResultBusy);
  cl_assert(smartstrap_fsm_state_get() == SmartstrapStateReadReady);

  // verify the data that was sent out the accessory port
  uint8_t *buffer;
  int length;
  fake_accessory_get_buffer(&buffer, &length);
  cl_assert(length == BUS_CONTENTION_AFTER + 1);
  for (int i = 0; i < length; i++) {
    cl_assert(buffer[i] == expect_data[i]);
  }
}

static void prv_do_read(uint8_t *data, int length, MBuf *read_mbuf, uint8_t *expect_data,
                        int expect_length) {
  for (int i = 0; i < length; i++) {
    smartstrap_handle_data_from_isr(data[i]);
  }
  fake_system_task_callbacks_invoke_pending();
  fake_smartstrap_profiles_check_read_params(true, SmartstrapProfileRawData, expect_length);
  uint8_t *read_data = mbuf_get_data(read_mbuf);
  int read_length = mbuf_get_length(read_mbuf);
  cl_assert(read_length == expect_length);
  for (int i = 0; i < read_length; i++) {
    cl_assert(read_data[i] == expect_data[i]);
  }
}

static void prv_do_read_notify(uint8_t *data, int length) {
  for (int i = 0; i < length; i++) {
    smartstrap_handle_data_from_isr(data[i]);
  }
  fake_system_task_callbacks_invoke_pending();
  cl_assert(smartstrap_fsm_state_get() == SmartstrapStateReadReady);
  fake_smartstrap_profiles_check_notify_params(true, SmartstrapProfileRawData);
}


// Tests

void test_smartstrap_comms__send_receive_data(void) {
  // write Mbuf
  MBuf write_mbuf = MBUF_EMPTY;
  uint8_t test_data[] = {0x00, 0x01};
  mbuf_set_data(&write_mbuf, test_data, sizeof(test_data));
  // read mbuf
  MBuf read_mbuf = MBUF_EMPTY;
  uint8_t read_data[sizeof(test_data)] = {0};
  mbuf_set_data(&read_mbuf, read_data, sizeof(read_data));
  // expected on-the-wire data for send
  uint8_t expected[] = {0x7E, 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0xEF, 0x7E};
  // faked on-the-wire data for response
  uint8_t response_raw[] = {0x7E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0x43, 0x7E};

  // send the request
  prv_do_send(&write_mbuf, &read_mbuf, expected, sizeof(expected));
  // process the fake response
  prv_do_read(response_raw, sizeof(response_raw), &read_mbuf, test_data, sizeof(test_data));
}

void test_smartstrap_comms__send_receive_escaped_data(void) {
  // write Mbuf
  MBuf write_mbuf = MBUF_EMPTY;
  uint8_t test_data[] = {0x7D, 0x7E, 0x00, 0x7E, 0x7D, 0x00};
  mbuf_set_data(&write_mbuf, test_data, sizeof(test_data));
  // read MBuf
  MBuf read_mbuf = MBUF_EMPTY;
  uint8_t read_data[sizeof(test_data)] = {0};
  mbuf_set_data(&read_mbuf, read_data, sizeof(read_data));
  // expected on-the-wire data from send
  uint8_t send_raw[] = {0x7E, 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7D, 0x5D, 0x7D, 0x5E,
                        0x00, 0x7D, 0x5E, 0x7D, 0x5D, 0x00, 0x59, 0x7E};
  // faked on-the-wire data for response
  uint8_t response_raw[] = {0x7E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7D, 0x5D, 0x7D, 0x5E,
                            0x00, 0x7D, 0x5E, 0x7D, 0x5D, 0x00, 0xC5, 0x7E};

  // send the request
  prv_do_send(&write_mbuf, &read_mbuf, send_raw, sizeof(send_raw));
  // process the fake response
  prv_do_read(response_raw, sizeof(response_raw), &read_mbuf, test_data, sizeof(test_data));
}

void test_smartstrap_comms__send_data(void) {
  // write Mbuf
  MBuf write_mbuf = MBUF_EMPTY;
  uint8_t test_data[] = {0x01, 0x11};
  mbuf_set_data(&write_mbuf, test_data, sizeof(test_data));
  // expected on-the-wire data from send
  uint8_t send_raw[] = {0x7E, 0x01, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x11, 0xCC, 0x7E};
  // send the write
  prv_do_send(&write_mbuf, NULL, send_raw, sizeof(send_raw));
}

void test_smartstrap_comms__send_data_bus_contention(void) {
  // write Mbuf
  MBuf write_mbuf = MBUF_EMPTY;
  uint8_t test_data[] = {0x01, 0x11};
  mbuf_set_data(&write_mbuf, test_data, sizeof(test_data));
  // expected on-the-wire data from send
  uint8_t send_raw[] = {0x7E, 0x01, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x11, 0xCC, 0x7E};
  // send the write
  prv_do_send_bus_contention(&write_mbuf, NULL, send_raw, sizeof(send_raw));
}

void test_smartstrap_comms__send_receive_data_bus_contention(void) {
  // write Mbuf
  MBuf write_mbuf = MBUF_EMPTY;
  uint8_t test_data[] = {0x01, 0x11};
  mbuf_set_data(&write_mbuf, test_data, sizeof(test_data));
  // read MBuf
  MBuf read_mbuf = MBUF_EMPTY;
  uint8_t read_data[sizeof(test_data)] = {0};
  mbuf_set_data(&read_mbuf, read_data, sizeof(read_data));
  // expected on-the-wire data from send
  uint8_t send_raw[] = {0x7E, 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x11, 0xA8, 0x7E};
  // send the write
  prv_do_send_bus_contention(&write_mbuf, &read_mbuf, send_raw, sizeof(send_raw));
}

void test_smartstrap_comms__notification(void) {
  // faked on-the-wire data for the context frame
  uint8_t notify_context_raw[] = {0x7E, 0x01, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x76, 0x7E};

  // send a break character
  smartstrap_fsm_state_reset();
  smartstrap_handle_break_from_isr();
  fake_system_task_callbacks_invoke_pending();
  cl_assert(smartstrap_fsm_state_get() == SmartstrapStateNotifyInProgress);

  // process the fake context frame
  prv_do_read_notify(notify_context_raw, sizeof(notify_context_raw));
}
