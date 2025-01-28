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

#include "kernel/pulse_logging.h"

// How many bytes are in a log message before the actual message content in pulse log messages
const int LOG_METADATA_LENGTH = 29;

// Stubs
///////////////////////////////////////////////////////////

#include "kernel/events.h"
#include "kernel/pebble_tasks.h"

#include "FreeRTOS.h"
#include "task.h"

static int s_num_event_puts;
static PebbleEvent s_last_event;
bool event_put_isr(PebbleEvent *e) {
  cl_assert_equal_i(e->type, PEBBLE_CALLBACK_EVENT);

  ++s_num_event_puts;

  s_last_event = *e;

  return true;
}

char pbl_log_get_level_char(const uint8_t log_level) {
  return 'L';
}

char pebble_task_get_char(PebbleTask task) {
  return 'T';
}

PebbleTask pebble_task_get_current(void) {
  return PebbleTask_Unknown;
}

void *pulse_best_effort_send_begin(uint16_t protocol) {
  static char buffer[1024];
  return buffer;
  // todo
}

static int s_num_packets_sent = 0;
static int s_num_bytes_sent = 0;
static char s_log_message_buffer[256];
void pulse_best_effort_send(void *buf, size_t length) {
  ++s_num_packets_sent;
  s_num_bytes_sent += length;

  const size_t message_length = length - LOG_METADATA_LENGTH;
  memcpy(s_log_message_buffer, ((char*) buf) + LOG_METADATA_LENGTH, message_length);
  s_log_message_buffer[message_length] = '\0';
}

bool pulse_is_started(void) {
  return true;
}

void rtc_get_time_ms(time_t* out_seconds, uint16_t* out_ms) {
  *out_seconds = 0;
  *out_ms = 0;
}


void vPortEnterCritical(void) {
}

void vPortExitCritical(void) {
}

bool s_in_critical_section;
bool vPortInCritical(void) {
  return s_in_critical_section;
}

BaseType_t xTaskGetSchedulerState(void) {
  return taskSCHEDULER_RUNNING;
}

// Tests
///////////////////////////////////////////////////////////

void test_pulse_logging__initialize(void) {
  s_num_event_puts = 0;
  s_last_event = (PebbleEvent) { 0 };

  s_num_packets_sent = 0;
  s_num_bytes_sent = 0;
  s_log_message_buffer[0] = '\0';

  s_in_critical_section = false;

  pulse_logging_init();
}

void test_pulse_logging__simple(void) {
  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "Test");

  cl_assert_equal_i(s_num_event_puts, 0);
  cl_assert_equal_i(s_num_packets_sent, 1);
  cl_assert_equal_i(s_num_bytes_sent, LOG_METADATA_LENGTH + 4);
  cl_assert_equal_s(s_log_message_buffer, "Test");

  s_num_bytes_sent = 0;

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTest");

  cl_assert_equal_i(s_num_event_puts, 0);
  cl_assert_equal_i(s_num_packets_sent, 2);
  cl_assert_equal_i(s_num_bytes_sent, LOG_METADATA_LENGTH + 20);
  cl_assert_equal_s(s_log_message_buffer, "TestTestTestTestTest");
}

void test_pulse_logging__simple_truncate(void) {
  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTest"
                                            "TestTestTestTestTestTestTestTestTestTest"
                                            "TestTestTestTestTestTestTestTestTestTest"
                                            "TestTestTestTestTestTestTestTestTestTest");

  cl_assert_equal_i(s_num_event_puts, 0);
  cl_assert_equal_i(s_num_packets_sent, 1);
  cl_assert_equal_i(s_num_bytes_sent, LOG_METADATA_LENGTH + 128);
  cl_assert_equal_s(s_log_message_buffer, "TestTestTestTestTestTestTestTestTestTest"
                                          "TestTestTestTestTestTestTestTestTestTest"
                                          "TestTestTestTestTestTestTestTestTestTest"
                                          "TestTest");
}

void test_pulse_logging__isr_simple(void) {
  s_in_critical_section = true;

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "Test");

  cl_assert_equal_i(s_num_event_puts, 1);

  s_last_event.callback.callback(NULL);
  cl_assert_equal_i(s_num_packets_sent, 1);
  cl_assert_equal_i(s_num_bytes_sent, LOG_METADATA_LENGTH + 4);
  cl_assert_equal_s(s_log_message_buffer, "Test");
  s_num_bytes_sent = 0;

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTest");

  cl_assert_equal_i(s_num_event_puts, 2);

  s_last_event.callback.callback(NULL);
  cl_assert_equal_i(s_num_packets_sent, 2);
  cl_assert_equal_i(s_num_bytes_sent, LOG_METADATA_LENGTH + 20);
  cl_assert_equal_s(s_log_message_buffer, "TestTestTestTestTest");
}

void test_pulse_logging__isr_truncate(void) {
  s_in_critical_section = true;

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTest"
                                            "TestTestTestTestTestTestTestTestTestTest"
                                            "TestTestTestTestTestTestTestTestTestTest"
                                            "TestTestTestTestTestTestTestTestTestTest");

  cl_assert_equal_i(s_num_event_puts, 1);

  s_last_event.callback.callback(NULL);
  cl_assert_equal_i(s_num_packets_sent, 1);
  cl_assert_equal_i(s_num_bytes_sent, LOG_METADATA_LENGTH + 128);
  cl_assert_equal_s(s_log_message_buffer, "TestTestTestTestTestTestTestTestTestTest"
                                          "TestTestTestTestTestTestTestTestTestTest"
                                          "TestTestTestTestTestTestTestTestTestTest"
                                          "TestTest");
}

void test_pulse_logging__isr_buffer_full(void) {
  s_in_critical_section = true;

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTestA");
  cl_assert_equal_i(s_num_event_puts, 1);
  cl_assert_equal_i(s_num_packets_sent, 0);

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTestB");
  cl_assert_equal_i(s_num_event_puts, 1);
  cl_assert_equal_i(s_num_packets_sent, 0);

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTestC");
  cl_assert_equal_i(s_num_event_puts, 1);
  cl_assert_equal_i(s_num_packets_sent, 0);

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTestD");
  cl_assert_equal_i(s_num_event_puts, 1);
  cl_assert_equal_i(s_num_packets_sent, 0);

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTestE");
  cl_assert_equal_i(s_num_event_puts, 1);
  cl_assert_equal_i(s_num_packets_sent, 0);

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTestF");
  cl_assert_equal_i(s_num_event_puts, 1);
  cl_assert_equal_i(s_num_packets_sent, 0);

  pulse_logging_log(LOG_LEVEL_DEBUG, "", 0, "TestTestTestTestTestTestTestTestTestTestG");

  s_last_event.callback.callback(NULL);
  cl_assert_equal_i(s_num_packets_sent, 7);
  cl_assert_equal_s(s_log_message_buffer, "ISR Message Dropped!");
}
