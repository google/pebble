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

#include "protobuf_log_test_helpers.h"

#include "services/normal/protobuf_log/protobuf_log.h"
#include "services/normal/protobuf_log/protobuf_log_private.h"
#include "services/normal/protobuf_log/protobuf_log_test.h"
#include "services/normal/protobuf_log/protobuf_log_hr.h"
#include "services/normal/protobuf_log/protobuf_log_activity_sessions.h"
#include "services/normal/activity/activity.h"

#include "applib/data_logging.h"
#include "drivers/rtc.h"
#include "services/normal/data_logging/data_logging_service.h"
#include "system/logging.h"
#include "util/attributes.h"
#include "util/size.h"

#include "stubs_passert.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_pbl_malloc.h"
#include "stubs_prompt.h"
#include "stubs_serial.h"

#include "fake_rtc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define WRITE_TO_FILE 0

#define LOG(fmt, args...) \
        PBL_LOG(LOG_LEVEL_DEBUG, fmt, ## args)

extern uint32_t prv_hr_quality_int(HRMQuality quality);

// ---------------------------------------------------------------------------------------------
// We start time out at 5pm on Jan 1, 2015 for all of these tests
static const  struct tm s_init_time_tm = {
  // Thursday, Jan 1, 2015, 5:pm
  .tm_hour = 17,
  .tm_mday = 1,
  .tm_mon = 0,
  .tm_year = 115
};

// Logged items
#define TEST_PL_DLS_SESSION_ID 1
typedef struct {
  uint8_t data[PLOG_DLS_RECORD_SIZE];
} TestPLDLSRecord;
static bool s_dls_session_created;
static int s_num_dls_records;
static TestPLDLSRecord s_dls_records[10];

static void prv_reset_captured_dls_data(void) {
  s_num_dls_records = 0;
}

//
// Data Logging Fakes
//

DataLoggingResult dls_log(DataLoggingSession *logging_session, const void *data,
                          uint32_t num_items) {
  cl_assert(s_dls_session_created);

  TestPLDLSRecord *records = (TestPLDLSRecord *)data;
  for (int i = 0; i < num_items; i++) {
    cl_assert(s_num_dls_records < ARRAY_LENGTH(s_dls_records));
    s_dls_records[s_num_dls_records++] = records[i];
  }

  return DATA_LOGGING_SUCCESS;
}

DataLoggingSession *dls_create(uint32_t tag, DataLoggingItemType item_type, uint16_t item_size,
                               bool buffered, bool resume, const Uuid *uuid) {
  if (tag == DlsSystemTagProtobufLogSession) {
    s_dls_session_created = true;
    cl_assert_equal_i(item_size, sizeof(TestPLDLSRecord));
    return (DataLoggingSession *)TEST_PL_DLS_SESSION_ID;
  } else {
    return NULL;
  }
}

void dls_finish(DataLoggingSession *logging_session) {
  if (logging_session == (DataLoggingSession *)TEST_PL_DLS_SESSION_ID) {
    s_dls_session_created = false;
  } else {
    cl_assert(false);
  }
}

//
// MFG/Version Fakes
//

#define TEST_PL_SERIAL_NUM "ABC01234567"
const char* mfg_get_serial_number(void) {
  return TEST_PL_SERIAL_NUM;
}

#define GIT_TAG_V_MAJOR 4
#define GIT_TAG_V_MINOR 17
#define GIT_TAG_V_PATCH "ROBERT-mfg4-6-gb91951a"
void version_get_major_minor_patch(unsigned int *major, unsigned int *minor,
                                   const char **patch_ptr) {
  *major = GIT_TAG_V_MAJOR;
  *minor = GIT_TAG_V_MINOR;
  *patch_ptr = GIT_TAG_V_PATCH;
}

// ---------------------------------------------------------------------------------------------
// The transport callback we pass to protobuf_log_create that captures the encoded message
static uint8_t *s_saved_encoded_msg;
static bool prv_protobuf_log_transport(uint8_t *buffer, size_t buf_size) {
  if (s_saved_encoded_msg != NULL) {
    free(s_saved_encoded_msg);
    s_saved_encoded_msg = NULL;
  }
  s_saved_encoded_msg = malloc(buf_size);
  cl_assert(s_saved_encoded_msg != NULL);
  memcpy(s_saved_encoded_msg, buffer, buf_size);
  return true;
}


// This structure used to capture the contents of a decoded message into a global
typedef struct {
  ProtobufLogType type;

  char payload_sender_type[PLOG_MAX_SENDER_TYPE_LEN];
  char payload_sender_id[PLOG_MAX_SENDER_ID_LEN];
  char payload_sender_version_patch[PLOG_MAX_SENDER_VERSION_PATCH_LEN];
  uint32_t payload_send_time;
  uint32_t payload_sender_v_major;
  uint32_t payload_sender_v_minor;
  union {
    struct {
      Uuid uuid;
      uint32_t num_types;
      ProtobufLogMeasurementType *types;
      uint32_t num_samples;
      uint32_t num_values;
      uint32_t *values;
      uint32_t *offset_sec;
      uint32_t time_utc;
      uint32_t time_end_utc;
      int32_t utc_to_local;
    } msrmt;
  };
  struct {
    uint32_t num_events;
    pebble_pipeline_Event *events;
    Uuid *uuids;
    uint32_t num_sessions;
    ActivitySession *sessions;
  } events;
} TestPLParsedMsg;


// ---------------------------------------------------------------------------------------------
// Parse and encoded message and return a TestPLParsedMsg structure pointer pointing to it's
// parsed contents. The contents are valid until prv_parse_encoded_msg is called again.
static TestPLParsedMsg *prv_parse_encoded_mset_payload(void *buffer) {
  static TestPLParsedMsg s_parsed_msg;
  static ProtobufLogMeasurementType s_types[10];
  static uint32_t s_offsets[1000];
  static uint32_t s_values[1000];

  memset(&s_parsed_msg, 0, sizeof(s_parsed_msg));
  memset(&s_types, 0, sizeof(s_types));
  memset(&s_offsets, 0, sizeof(s_offsets));
  memset(&s_values, 0, sizeof(s_values));

  s_parsed_msg = (TestPLParsedMsg) {
    .msrmt = {
      .num_types = ARRAY_LENGTH(s_types),
      .types = s_types,
      .num_samples = ARRAY_LENGTH(s_offsets),
      .offset_sec = s_offsets,
      .num_values = ARRAY_LENGTH(s_values),
      .values = s_values,
    },
  };

  // Get the message size and pointer to encoded data
  PLogMessageHdr *hdr = (PLogMessageHdr *)buffer;
  bool success = protobuf_log_private_mset_decode(
      &s_parsed_msg.type,
      (uint8_t *)buffer + sizeof(*hdr),
      hdr->msg_size,
      s_parsed_msg.payload_sender_type,
      s_parsed_msg.payload_sender_id,
      s_parsed_msg.payload_sender_version_patch,
      &s_parsed_msg.payload_send_time,
      &s_parsed_msg.payload_sender_v_major,
      &s_parsed_msg.payload_sender_v_minor,
      &s_parsed_msg.msrmt.uuid,
      &s_parsed_msg.msrmt.time_utc,
      &s_parsed_msg.msrmt.time_end_utc,
      &s_parsed_msg.msrmt.utc_to_local,
      &s_parsed_msg.msrmt.num_types,
      s_parsed_msg.msrmt.types,
      &s_parsed_msg.msrmt.num_samples,
      s_parsed_msg.msrmt.offset_sec,
      &s_parsed_msg.msrmt.num_values,
      s_parsed_msg.msrmt.values);
  if (!success) {
    LOG("No encoded msg available");
  } else {
    LOG("ProtobufLogType: %d", s_parsed_msg.type);
    LOG("payload_sender_type: %s", s_parsed_msg.payload_sender_type);
    LOG("payload_sender_id: %s", s_parsed_msg.payload_sender_id);
    LOG("payload_sender_version: major: %"PRIu32", minor: %"PRIu32", patch: %s",
        s_parsed_msg.payload_sender_v_major, s_parsed_msg.payload_sender_v_minor, s_parsed_msg.payload_sender_version_patch);
    LOG("payload_send_time: %"PRIu32"", s_parsed_msg.payload_send_time);
    LOG("MeasurementSet:");
    char uuid_str[UUID_STRING_BUFFER_LENGTH];
    uuid_to_string(&s_parsed_msg.msrmt.uuid, uuid_str);
    LOG("  Uuid: %s", uuid_str);
    LOG("  time_utc: %"PRIu32", time_end_utc: %"PRIu32", utc_to_local: %"PRIi32"",
        s_parsed_msg.msrmt.time_utc, s_parsed_msg.msrmt.time_end_utc,
        s_parsed_msg.msrmt.utc_to_local);
    LOG("  %"PRIu32" types: ", s_parsed_msg.msrmt.num_types);
    for (unsigned i = 0; i < s_parsed_msg.msrmt.num_types; i++) {
      LOG("    %d", (int)s_parsed_msg.msrmt.types[i]);
    }
    LOG("  %"PRIu32" measurements: ", s_parsed_msg.msrmt.num_samples);
    for (unsigned i = 0; i < s_parsed_msg.msrmt.num_samples; i++) {
      LOG("    offset_sec: %"PRIu32"", s_parsed_msg.msrmt.offset_sec[i]);
      for (unsigned j = 0; j < s_parsed_msg.msrmt.num_types; j++) {
        LOG("      0x%"PRIx32"",
                s_parsed_msg.msrmt.values[i * s_parsed_msg.msrmt.num_types + j]);
      }
    }
  }

  return &s_parsed_msg;
}


// ---------------------------------------------------------------------------------------------
// Parse and encoded message and return a TestPLParsedMsg structure pointer pointing to it's
// parsed contents. The contents are valid until prv_parse_encoded_msg is called again.
static TestPLParsedMsg *prv_parse_encoded_event_payload(void *buffer) {
  static TestPLParsedMsg s_parsed_msg;

  static pebble_pipeline_Event events[10];
  static Uuid event_uuids[10];
  static ActivitySession event_sessions[10];

  s_parsed_msg = (TestPLParsedMsg) {
    .events = {
      .num_events = ARRAY_LENGTH(events),
      .events = events,
      .uuids = event_uuids,
      .num_sessions = ARRAY_LENGTH(event_sessions),
      .sessions = event_sessions,
    },
  };

  // Get the message size and pointer to encoded data
  PLogMessageHdr *hdr = (PLogMessageHdr *)buffer;
  bool success = protobuf_log_private_events_decode(
    &s_parsed_msg.type,
    (uint8_t *)buffer + sizeof(*hdr),
    hdr->msg_size,
    s_parsed_msg.payload_sender_type,
    s_parsed_msg.payload_sender_id,
    s_parsed_msg.payload_sender_version_patch,
    &s_parsed_msg.payload_send_time,
    &s_parsed_msg.payload_sender_v_major,
    &s_parsed_msg.payload_sender_v_minor,
    &s_parsed_msg.events.num_events,
    s_parsed_msg.events.events,
    s_parsed_msg.events.uuids,
    &s_parsed_msg.events.num_sessions,
    s_parsed_msg.events.sessions);

  if (!success) {
    LOG("No encoded msg available");
  } else {
    LOG("ProtobufLogType: %d", s_parsed_msg.type);
    LOG("payload_sender_type: %s", s_parsed_msg.payload_sender_type);
    LOG("payload_sender_id: %s", s_parsed_msg.payload_sender_id);
    LOG("payload_sender_version: major: %"PRIu32", minor: %"PRIu32", patch: %s",
        s_parsed_msg.payload_sender_v_major,
        s_parsed_msg.payload_sender_v_minor,
        s_parsed_msg.payload_sender_version_patch);
    LOG("payload_send_time: %"PRIu32"", s_parsed_msg.payload_send_time);

    const int num_events = s_parsed_msg.events.num_events;
    LOG("Events: Number: %d", num_events);
    for (int i = 0; i < num_events; i++) {
      pebble_pipeline_Event *event = &s_parsed_msg.events.events[i];
      LOG("  Event -- Type: %d", event->type);
      char uuid_str[UUID_STRING_BUFFER_LENGTH];
      uuid_to_string(&s_parsed_msg.events.uuids[i], uuid_str);
      LOG("  Uuid: %s", uuid_str);
      LOG("  time_utc: %"PRIu32", created_time_utc: %"PRIu32", utc_to_local: %"PRIi32"",
          event->time_utc, event->created_time_utc,
          event->utc_to_local);
      LOG("  duration: %"PRIu32, event->duration);
      // Activity Event
      if (event->type == pebble_pipeline_Event_Type_ActivitySessionEvent) {
        const pebble_pipeline_ActivitySession *session = &event->activity_session;
        LOG("  Activity Type: %d, Start Reason: %d", session->type.type.internal_type,
            session->start_reason);
        // TODO: Add more detailed logging
      }
    }
  }

  return &s_parsed_msg;
}

// ---------------------------------------------------------------------------------------------
static void prv_assert_msg_equal(TestPLParsedMsg *a, TestPLParsedMsg *b) {
  // Payload Specific
  cl_assert_equal_s(a->payload_sender_type, b->payload_sender_type);
  cl_assert_equal_s(a->payload_sender_id, b->payload_sender_id);
  cl_assert_equal_i(a->payload_send_time, b->payload_send_time);
  cl_assert_equal_i(a->payload_sender_v_major, b->payload_sender_v_major);
  cl_assert_equal_i(a->payload_sender_v_minor, b->payload_sender_v_minor);
  cl_assert_equal_s(a->payload_sender_version_patch, b->payload_sender_version_patch);

  // Ensure they are the same type
  cl_assert_equal_i(a->type, b->type);

  // MeasurementSet Specific
  if (a->type == ProtobufLogType_Measurements) {
    cl_assert_equal_i(a->msrmt.time_utc, b->msrmt.time_utc);
    cl_assert_equal_i(a->msrmt.utc_to_local, b->msrmt.utc_to_local);
    cl_assert_equal_i(a->msrmt.num_types, b->msrmt.num_types);
    cl_assert_equal_m(a->msrmt.types, b->msrmt.types,
                      b->msrmt.num_types * sizeof(ProtobufLogMeasurementType));
    cl_assert_equal_i(a->msrmt.num_samples, b->msrmt.num_samples);
    cl_assert_equal_m(a->msrmt.offset_sec, b->msrmt.offset_sec,
                      b->msrmt.num_samples * sizeof(uint32_t));
    cl_assert_equal_i(a->msrmt.num_values, b->msrmt.num_values);
    cl_assert_equal_m(a->msrmt.values, b->msrmt.values, b->msrmt.num_values * sizeof(uint32_t));
  } else if (a->type == ProtobufLogType_Events) {
    cl_assert_equal_i(a->events.num_events, b->events.num_events);
    cl_assert_equal_i(a->events.num_sessions, b->events.num_sessions);
    for (int i = 0; i < a->events.num_events; i++) {
      const pebble_pipeline_Event *a_event = &a->events.events[i];
      const pebble_pipeline_Event *b_event = &b->events.events[i];

      cl_assert_equal_i(a_event->type, b_event->type);
      cl_assert_equal_i(a_event->created_time_utc, b_event->created_time_utc);
      cl_assert_equal_i(a_event->duration, b_event->duration);
      cl_assert_equal_i(a_event->time_utc, b_event->time_utc);
      cl_assert_equal_i(a_event->utc_to_local, b_event->utc_to_local);

      if (a_event->type == pebble_pipeline_Event_Type_ActivitySessionEvent) {
        cl_assert_equal_i(a_event->activity_session.type.type.internal_type,
                          b_event->activity_session.type.type.internal_type);
        cl_assert_equal_i(a_event->activity_session.start_reason,
                          b_event->activity_session.start_reason);
        // TODO: Add more detailed comparisons
      }
    }
  }
}

static void prv_fill_version(TestPLParsedMsg *msg) {
  msg->payload_sender_v_major = GIT_TAG_V_MAJOR;
  msg->payload_sender_v_minor = GIT_TAG_V_MINOR;
  strcpy(msg->payload_sender_version_patch, GIT_TAG_V_PATCH);
}

static void prv_common_payload_initialize(TestPLParsedMsg *input) {
  // We always have the same sender type and id
  strncpy(input->payload_sender_type, PLOG_PAYLOAD_SENDER_TYPE, PLOG_MAX_SENDER_ID_LEN);
  strncpy(input->payload_sender_id, TEST_PL_SERIAL_NUM, PLOG_MAX_SENDER_ID_LEN);

  prv_fill_version(input);

  // Reset data logging storage
  prv_reset_captured_dls_data();
}

static ProtobufLogRef prv_log_create_measurement(TestPLParsedMsg *input, bool use_data_logging) {
  // Create a session
  ProtobufLogTransportCB transport_cb = prv_protobuf_log_transport;
  if (use_data_logging) {
    transport_cb = NULL;
  }

  ProtobufLogConfig log_config = {
    .type = ProtobufLogType_Measurements,
    .measurements = {
      .num_types = input->msrmt.num_types,
      .types = input->msrmt.types,
    }
  };

  ProtobufLogRef session_ref = protobuf_log_create(&log_config, transport_cb, 0);
  cl_assert(session_ref != NULL);
  return session_ref;
}

static TestPLParsedMsg *prv_flush_get_record(TestPLParsedMsg *input, bool use_data_logging,
                                             ProtobufLogRef session_ref) {
  // Flush it out now, this should end up in our transport method being called
  input->payload_send_time = rtc_get_time();
  bool success = protobuf_log_session_flush(session_ref);
  cl_assert(success);


  TestPLParsedMsg *(*parser)(void *);
  switch (input->type) {
    case ProtobufLogType_Events:
      parser = prv_parse_encoded_event_payload;
      break;
    case ProtobufLogType_Measurements:
      parser = prv_parse_encoded_mset_payload;
      break;
  }

  // Verify the contents
  TestPLParsedMsg *msg;
  if (use_data_logging) {
    cl_assert(s_num_dls_records == 1);
    msg = parser(&s_dls_records[0]);
  } else {
#if WRITE_TO_FILE
    protobuf_log_test_parse_protoc(s_saved_encoded_msg);
#endif
    msg = parser(s_saved_encoded_msg);
  }
  return msg;
}

// ---------------------------------------------------------------------------------------------
static ProtobufLogRef *prv_test_encode_measurements(TestPLParsedMsg *input, bool use_data_logging) {
  prv_common_payload_initialize(input);
  ProtobufLogRef session_ref = prv_log_create_measurement(input, use_data_logging);

  // Encode the measurements
  uint32_t values_per_samples = input->msrmt.num_types;
  bool success;
  for (unsigned i = 0; i < input->msrmt.num_samples; i++) {
    rtc_set_time(input->msrmt.time_utc + input->msrmt.offset_sec[i]);
    success = protobuf_log_session_add_measurements(session_ref, rtc_get_time(),
                                                    input->msrmt.num_types,
                                                    &input->msrmt.values[i * values_per_samples]);
    cl_assert(success);
  }
  return session_ref;
}

static void prv_test_decode_payload(TestPLParsedMsg *input, bool use_data_logging,
                                         ProtobufLogRef session_ref) {
  TestPLParsedMsg *record = prv_flush_get_record(input, use_data_logging, session_ref);

  prv_assert_msg_equal(input, record);

  // Delete the session
  protobuf_log_session_delete(session_ref);
}

// ---------------------------------------------------------------------------------------------
void test_protobuf_log__initialize(void) {
  struct tm time_tm = s_init_time_tm;
  time_t utc_sec = mktime(&time_tm);
  fake_rtc_init(100 /*initial_ticks*/, utc_sec);

  TimezoneInfo tz_info = {
    .tm_zone = "???",
    .tm_gmtoff = SECONDS_PER_HOUR,
  };
  time_util_update_timezone(&tz_info);

  s_dls_session_created = false;
  s_saved_encoded_msg = NULL;

  protobuf_log_init();
}


// ---------------------------------------------------------------------------------------------
void test_protobuf_log__cleanup(void) {
}


// ---------------------------------------------------------------------------------------------
// Test some simple message variants
void test_protobuf_log__measurements_simple(void) {
  // A simple message with 2 types, 2 samples
  {
    ProtobufLogMeasurementType types[] = {ProtobufLogMeasurementType_Steps,
                                          ProtobufLogMeasurementType_BPM};
    uint32_t offset_sec[] = {1, 2};
    uint32_t values[] = {0x11, 0x22, 0x33, 0x44};

    TestPLParsedMsg input = {
      .type = ProtobufLogType_Measurements,
      .msrmt = {
        .time_utc = rtc_get_time(),
        .utc_to_local = time_util_utc_to_local_offset(),
        .num_types = ARRAY_LENGTH(types),
        .types = types,
        .num_samples = ARRAY_LENGTH(offset_sec),
        .offset_sec = offset_sec,
        .num_values = ARRAY_LENGTH(values),
        .values = values,
      },
    };
    ProtobufLogRef ref = prv_test_encode_measurements(&input, false /*use_data_logging*/);
    prv_test_decode_payload(&input, false /*use_data_logging*/, ref);
  }

  // A simple message with 1 type, 1 sample. Large sample values
  {
    ProtobufLogMeasurementType types[] = {ProtobufLogMeasurementType_BPM};
    uint32_t offset_sec[] = {2, 4};
    uint32_t values[] = {0x11223344, 0x22334455};

    TestPLParsedMsg input = {
      .type = ProtobufLogType_Measurements,
      .msrmt = {
        .time_utc = rtc_get_time(),
        .utc_to_local = time_util_utc_to_local_offset(),
        .num_types = ARRAY_LENGTH(types),
        .types = types,
        .num_samples = ARRAY_LENGTH(offset_sec),
        .offset_sec = offset_sec,
        .num_values = ARRAY_LENGTH(values),
        .values = values,
      },
    };
    ProtobufLogRef ref = prv_test_encode_measurements(&input, false /*use_data_logging*/);
    prv_test_decode_payload(&input, false /*use_data_logging*/, ref);
  }

  // A message with 4 types, 3 samples
  {
    ProtobufLogMeasurementType types[] = {ProtobufLogMeasurementType_Steps,
                                          ProtobufLogMeasurementType_BPM,
                                          ProtobufLogMeasurementType_VMC,
                                          ProtobufLogMeasurementType_DistanceCM};
    uint32_t offset_sec[] = {1, 2, 3};
    uint32_t values[] = {0x11, 0x22, 0x33, 0x44,
                         0x1111, 0x2222, 0x3333, 0x4444,
                         0x111111, 0x222222, 0x333333, 0x444444};

    TestPLParsedMsg input = {
      .type = ProtobufLogType_Measurements,
      .msrmt = {
        .time_utc = rtc_get_time(),
        .utc_to_local = time_util_utc_to_local_offset(),
        .num_types = ARRAY_LENGTH(types),
        .types = types,
        .num_samples = ARRAY_LENGTH(offset_sec),
        .offset_sec = offset_sec,
        .num_values = ARRAY_LENGTH(values),
        .values = values,
      }
    };
    ProtobufLogRef ref = prv_test_encode_measurements(&input, false /*use_data_logging*/);
    prv_test_decode_payload(&input, false /*use_data_logging*/, ref);
  }
}


// ---------------------------------------------------------------------------------------------
// Try doing multiple flushes from the same session
void test_protobuf_log__measurements_multiple(void) {
  ProtobufLogMeasurementType types[] = {ProtobufLogMeasurementType_Steps,
                                        ProtobufLogMeasurementType_BPM};
  uint32_t offset_sec[] = {1, 2};
  uint32_t values[] = {0x11, 0x22, 0x33, 0x44};

  TestPLParsedMsg input = {
    .type = ProtobufLogType_Measurements,
    .msrmt = {
      .time_utc = rtc_get_time(),
      .utc_to_local = time_util_utc_to_local_offset(),
      .num_types = ARRAY_LENGTH(types),
      .types = types,
      .num_samples = ARRAY_LENGTH(offset_sec),
      .offset_sec = offset_sec,
      .num_values = ARRAY_LENGTH(values),
      .values = values,
    },
  };
  prv_common_payload_initialize(&input);

  // Create a session
  ProtobufLogConfig log_config = {
    .type = ProtobufLogType_Measurements,
    .measurements = {
      .num_types = input.msrmt.num_types,
      .types = input.msrmt.types,
    }
  };

  ProtobufLogRef session_ref = protobuf_log_create(&log_config, prv_protobuf_log_transport, 0);
  cl_assert(session_ref != NULL);

  // ------------------
  // Encode the first set of measurements
  uint32_t values_per_samples = input.msrmt.num_types;
  bool success;
  for (unsigned i = 0; i < input.msrmt.num_samples; i++) {
    rtc_set_time(input.msrmt.time_utc + input.msrmt.offset_sec[i]);
    success = protobuf_log_session_add_measurements(session_ref, rtc_get_time(),
                                                    input.msrmt.num_types,
                                                    &input.msrmt.values[i * values_per_samples]);
    cl_assert(success);
  }

  // Flush it out now, this should end up in our transport method being called
  TestPLParsedMsg *msg = prv_flush_get_record(&input, false /* use_data_logging */, session_ref);
  prv_assert_msg_equal(&input, msg);


  // ------------------
  // Send another set of measurements
  uint32_t offset_sec_b[] = {2, 4, 6};
  uint32_t values_b[] = {0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666};

  input = (TestPLParsedMsg) {
    .type = ProtobufLogType_Measurements,
    .msrmt = {
      .time_utc = rtc_get_time(),
      .utc_to_local = time_util_utc_to_local_offset(),
      .num_types = ARRAY_LENGTH(types),
      .types = types,
      .num_samples = ARRAY_LENGTH(offset_sec_b),
      .offset_sec = offset_sec_b,
      .num_values = ARRAY_LENGTH(values_b),
      .values = values_b,
    }
  };
  prv_common_payload_initialize(&input);

  for (unsigned i = 0; i < input.msrmt.num_samples; i++) {
    rtc_set_time(input.msrmt.time_utc + input.msrmt.offset_sec[i]);
    success = protobuf_log_session_add_measurements(session_ref, rtc_get_time(),
                                                  input.msrmt.num_types,
                                                  &input.msrmt.values[i * values_per_samples]);
    cl_assert(success);
  }

  // Flush it out now, this should end up in our transport method being called
  msg = prv_flush_get_record(&input, false /* use_data_logging */, session_ref);
  prv_assert_msg_equal(&input, msg);

  // Delete the session now
  protobuf_log_session_delete(session_ref);
}

// ---------------------------------------------------------------------------------------------
// Test the automatic flush functionality
void test_protobuf_log__measurements_auto_flush(void) {
  ProtobufLogMeasurementType types[] = {ProtobufLogMeasurementType_Steps,
                                        ProtobufLogMeasurementType_BPM};
  const int num_samples = 50;
  const int num_values_per_sample = ARRAY_LENGTH(types);
  uint32_t offset_sec[num_samples];
  uint32_t values[num_samples * num_values_per_sample];

  for (unsigned i = 0; i < num_samples; i++) {
    offset_sec[i] = i * 2;
  }
  for (unsigned i = 0; i < num_samples * num_values_per_sample; i++) {
    values[i] = i * 3;
  }

  // Create a session with an artificially small buffer size which will cause it to flush
  // automatically
  time_t start_time = rtc_get_time();
  ProtobufLogConfig log_config = {
    .type = ProtobufLogType_Measurements,
    .measurements = {
      .num_types = num_values_per_sample,
      .types = types,
    }
  };

  ProtobufLogRef session_ref = protobuf_log_create(&log_config, prv_protobuf_log_transport, 110);
  cl_assert(session_ref != NULL);

  // ------------------
  // Keep adding samples, relying on auto-flush
  bool success;
  TestPLParsedMsg *msg;
  uint32_t num_samples_encoded = 0;
  for (unsigned i = 0; i < num_samples; i++) {
    rtc_set_time(start_time + offset_sec[i]);
    success = protobuf_log_session_add_measurements(session_ref, rtc_get_time(),
                                                    num_values_per_sample,
                                                    &values[i * num_values_per_sample]);
    cl_assert(success);
    if (s_saved_encoded_msg == NULL) {
      LOG("No message available yet...");
    } else {
      msg = prv_parse_encoded_mset_payload(s_saved_encoded_msg);
      cl_assert_equal_m(msg->msrmt.values, &values[num_samples_encoded * num_values_per_sample],
                        msg->msrmt.num_values * sizeof(uint32_t));
      num_samples_encoded += msg->msrmt.num_samples;
      free(s_saved_encoded_msg);
      s_saved_encoded_msg = NULL;
    }
  }

  // Flush it out now, this should send any remaining data out
  success = protobuf_log_session_flush(session_ref);
  cl_assert(success);

  if (num_samples_encoded < num_samples) {
    if (s_saved_encoded_msg == NULL) {
      LOG("No message available yet...");
    } else {
      msg = prv_parse_encoded_mset_payload(s_saved_encoded_msg);
      cl_assert_equal_m(msg->msrmt.values, &values[num_samples_encoded * num_values_per_sample],
                        msg->msrmt.num_values * sizeof(uint32_t));
    }
  }

  // Delete the session now
  protobuf_log_session_delete(session_ref);
}

// ---------------------------------------------------------------------------------------------
// Test using the data logging transport
void test_protobuf_log__measurements_with_data_logging(void) {
  ProtobufLogMeasurementType types[] = {ProtobufLogMeasurementType_Steps,
                                        ProtobufLogMeasurementType_BPM};
  uint32_t offset_sec[] = {1, 2};
  uint32_t values[] = {0x11, 0x22, 0x33, 0x44};

  TestPLParsedMsg input = {
    .type = ProtobufLogType_Measurements,
    .msrmt = {
      .time_utc = rtc_get_time(),
      .utc_to_local = time_util_utc_to_local_offset(),
      .num_types = ARRAY_LENGTH(types),
      .types = types,
      .num_samples = ARRAY_LENGTH(offset_sec),
      .offset_sec = offset_sec,
      .num_values = ARRAY_LENGTH(values),
      .values = values,
    }
  };
  ProtobufLogRef ref = prv_test_encode_measurements(&input, false /*use_data_logging*/);
  prv_test_decode_payload(&input, false /*use_data_logging*/, ref);
}


// ---------------------------------------------------------------------------------------------
// Test using the data logging transport
void test_protobuf_log__hr_samples(void) {
  ProtobufLogMeasurementType types[] = {ProtobufLogMeasurementType_BPM,
                                        ProtobufLogMeasurementType_HRQuality};

  uint32_t offset_sec[] = {1, 2};
  uint32_t values[] = {0x11, HRMQuality_Acceptable, 0x33, HRMQuality_Excellent};

  TestPLParsedMsg input = {
    .type = ProtobufLogType_Measurements,
    .msrmt = {
      .time_utc = rtc_get_time(),
      .utc_to_local = time_util_utc_to_local_offset(),
      .num_types = 2,
      .types = types,
      .num_samples = ARRAY_LENGTH(offset_sec),
      .offset_sec = offset_sec,
      .num_values = ARRAY_LENGTH(values),
      .values = values,
    }
  };

  prv_common_payload_initialize(&input);

  // Create a session
  ProtobufLogTransportCB transport_cb = prv_protobuf_log_transport;
  ProtobufLogRef session_ref = protobuf_log_hr_create(transport_cb);
  cl_assert(session_ref != NULL);

  const uint32_t values_per_samples = input.msrmt.num_types;
  bool success;
  for (unsigned i = 0; i < input.msrmt.num_samples; i++) {
    rtc_set_time(input.msrmt.time_utc + input.msrmt.offset_sec[i]);
    uint32_t *vals = &input.msrmt.values[i * values_per_samples];
    success = protobuf_log_hr_add_sample(session_ref, rtc_get_time(),
                                         vals[0], // BPM
                                         vals[1]); // Quality
    cl_assert(success);
  }

  // Convert the values from HRMQuality to pebble_pipeline values
  for (unsigned i = 1; i < input.msrmt.num_values; i += 2) {
    values[i] = prv_hr_quality_int(values[i]);
  }

  prv_test_decode_payload(&input, false /*use_data_logging*/, session_ref);
}

// ---------------------------------------------------------------------------------------------
// Test using the data logging transport
void test_protobuf_log__events_basic(void) {
  pebble_pipeline_Event events[] = {
    {
      .type = pebble_pipeline_Event_Type_UnknownEvent,
      .duration = 17,
      .has_duration = true,
      .time_utc = rtc_get_time() - 3000,
    },
    {
      .type = pebble_pipeline_Event_Type_UnknownEvent,
      .duration = 34,
      .has_duration = true,
      .time_utc = rtc_get_time() - 2000,
    }
  };

  TestPLParsedMsg input = {
    .type = ProtobufLogType_Events,
    .events = {
      .num_events = ARRAY_LENGTH(events),
      .events = events,
    }
  };

  prv_common_payload_initialize(&input);

  // Create a session
  ProtobufLogConfig log_config = {
    .type = ProtobufLogType_Events,
  };

  ProtobufLogTransportCB transport_cb = prv_protobuf_log_transport;
  ProtobufLogRef session_ref = protobuf_log_create(&log_config, transport_cb, 0);
  cl_assert(session_ref != NULL);

  bool success;
  for (int i = 0; i < ARRAY_LENGTH(events); i++) {
    success = protobuf_log_session_add_event(session_ref, &events[i]);
    cl_assert(success);
  }

  prv_test_decode_payload(&input, false /*use_data_logging*/, session_ref);
}
