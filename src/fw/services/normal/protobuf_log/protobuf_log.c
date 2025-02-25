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

#include "protobuf_log.h"
#include "protobuf_log_private.h"
#include "protobuf_log_util.h"

#include "applib/data_logging.h"
#include "drivers/rtc.h"
#include "kernel/pbl_malloc.h"
#include "mfg/mfg_serials.h"
#include "os/mutex.h"
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "services/normal/data_logging/data_logging_service.h"
#include "system/logging.h"
#include "system/passert.h"
#include "system/version.h"
#include "util/math.h"
#include "util/size.h"
#include "util/time/time.h"

#include "util/uuid.h"

#include <string.h>

// These headers auto-generated from the measurements.proto
#include "nanopb/common.pb.h"
#include "nanopb/event.pb.h"
#include "nanopb/measurements.pb.h"
#include "nanopb/payload.pb.h"

#define PROTOBUF_LOG_DEBUG(fmt, args...) \
            PBL_LOG_D(LOG_DOMAIN_PROTOBUF, LOG_LEVEL_DEBUG, fmt, ## args)

#define MLOG_MAX_VARINT_ENCODED_SIZE 5

// Our globals
typedef struct PLogState {
  PebbleMutex *mutex;
  DataLoggingSession *dls_session;
} PLogState;
static PLogState s_plog_state;


// ---------------------------------------------------------------------------------------
// Get the data logging session. Creating it if not already created
static DataLoggingSession *prv_get_dls_session(void) {
  if (s_plog_state.dls_session == NULL) {
    const bool buffered = true;
    const bool resume = false;
    Uuid system_uuid = UUID_SYSTEM;
    s_plog_state.dls_session = dls_create(DlsSystemTagProtobufLogSession,
                                          DATA_LOGGING_BYTE_ARRAY, PLOG_DLS_RECORD_SIZE, buffered,
                                          resume, &system_uuid);
    if (!s_plog_state.dls_session) {
      // This can happen when you are not connected to the phone and have rebooted a number of
      // times because each time you reboot, you get new sessions created and reach the limit
      // of the max # of sessions allowed.
      PBL_LOG(LOG_LEVEL_WARNING, "Error creating activity logging session");
      return NULL;
    }
  }
  return s_plog_state.dls_session;
}


// ---------------------------------------------------------------------------------------------
// Our default transport, which sends the data over data logging
static bool prv_dls_transport(uint8_t *buffer, size_t buf_size) {
  bool success = false;

  mutex_lock(s_plog_state.mutex);
  {
    DataLoggingSession *dls_session = prv_get_dls_session();
    if (!dls_session) {
      goto unlock;
    }

    // Log the data now, padding with 0's
    PBL_ASSERTN(buf_size <= PLOG_DLS_RECORD_SIZE);
    memset(buffer + buf_size, 0, PLOG_DLS_RECORD_SIZE - buf_size);
    DataLoggingResult result = dls_log(dls_session, buffer, 1);
    if (result == DATA_LOGGING_SUCCESS) {
      success = true;
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Error %d while logging data", (int)result);
    }
  }
unlock:
  mutex_unlock(s_plog_state.mutex);
  return success;
}


// -----------------------------------------------------------------------------------------
// Encode a struct `msg` with the field number and fields passed.
static bool prv_encode_struct(pb_ostream_t *stream, uint32_t field_number,
                              const pb_msgdesc_t * fields, const void *msg) {
  // Encode the field tag and data type
  if (!pb_encode_tag(stream, PB_WT_STRING, field_number)) {
    return false;
  }
  return pb_encode_submessage(stream, fields, msg);
}


// -----------------------------------------------------------------------------------------
// Encode a payload containing the data blob passed in
static bool prv_populate_payload(ProtobufLogConfig *config, size_t buffer_len, uint8_t *buffer,
                                 pb_ostream_t *stream) {
  PLogBufferEncoderArg ms_encoder_arg = {
    .len = buffer_len,
    .buffer = buffer,
  };

  // Version and Patch
  unsigned int v_major, v_minor;
  const char *version_patch_ptr;
  version_get_major_minor_patch(&v_major, &v_minor, &version_patch_ptr);

  // Sender ID
  const char *watch_serial = mfg_get_serial_number();

  pebble_pipeline_Payload payload = {
    .sender = {
      .type = {
        .funcs.encode = protobuf_log_util_encode_string,
        .arg = (void *)PLOG_PAYLOAD_SENDER_TYPE,
      },
      .id = {
        .funcs.encode = protobuf_log_util_encode_string,
        .arg = (void *)watch_serial,
      },
      .has_version = true,
      .version = {
        .major = v_major,
        .minor = v_minor,
        .patch = {
          .funcs.encode = protobuf_log_util_encode_string,
          .arg = (void *)version_patch_ptr,
        },
      },
    },
    .send_time_utc = rtc_get_time(),
  };

  // NOTE: A Payload is the master protobuf struct that we send to the phone.
  // Events have already been encoded for Payloads (in `protobuf_log_session_add_event`)
  // MeasurementSets have not. They are currently only written to the stream as a self standing
  // object, not for a payload.
  // This results in encoding them a bit differently at the end.
  // For Events, just write the exact buffer.
  // For MeasurementSets, encode them for the Payload.
  switch (config->type) {
    case ProtobufLogType_Events:
      pb_write(stream, ms_encoder_arg.buffer, ms_encoder_arg.len);
      break;
    case ProtobufLogType_Measurements:
      payload.measurement_sets = (pb_callback_t) {
        .funcs.encode = protobuf_log_util_encode_buffer,
        .arg = &ms_encoder_arg,
      };
      break;
  }

  bool success = pb_encode(stream, pebble_pipeline_Payload_fields, &payload);
  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error encoding payload");
  }

  // PBL-43622: Will revert later
  PBL_LOG(LOG_LEVEL_INFO, "Logged protobuf payload type: %d, utc:%"PRIu32, config->type,
          payload.send_time_utc);
  return success;
}


// -----------------------------------------------------------------------------------------
// Free all memory associated with a session
static void prv_session_free(PLogSession *session) {
  kernel_free(session->msg_buffer);
  kernel_free(session->data_buffer);
  kernel_free(session);
}


bool protobuf_log_init(void) {
  s_plog_state.mutex = mutex_create();
  return true;
}

// ---------------------------------------------------------------------------------------
// How much space is needed in the allocation of the PLogSession (useful for storing extra data).
static size_t prv_session_extra_space_needed(const ProtobufLogConfig *config) {
  switch (config->type) {
    case ProtobufLogType_Measurements:
      return (config->measurements.num_types * sizeof(ProtobufLogMeasurementType));
    case ProtobufLogType_Events:
      return 0;
  }
  WTF;
  return 0;
}


// ---------------------------------------------------------------------------------------
// Starts/restarts a measurement session.
static bool prv_session_measurement_encode_start(PLogSession *session) {
  const ProtobufLogConfig *config = &session->config;

  // Set the types pointer to the space allocated right after the PLogSession
  ProtobufLogMeasurementType *types_copy = (ProtobufLogMeasurementType *) (session + 1);
  const size_t extra_space = prv_session_extra_space_needed(config);
  // Copy the types array directly after the Session bytes.
  memcpy(types_copy, config->measurements.types, extra_space);

  // Generate a new UUID
  Uuid uuid;
  uuid_generate(&uuid);

  PLogTypesEncoderArg types_encoder_arg = {
    .num_types = session->config.measurements.num_types,
    .types = session->config.measurements.types,
  };

  pebble_pipeline_MeasurementSet msg = {
    .uuid = {
      .funcs.encode = protobuf_log_util_encode_uuid,
      .arg = &uuid,
    },
    .time_utc = session->start_utc,
    .utc_to_local = time_util_utc_to_local_offset(),
    .types = {
      .funcs.encode = protobuf_log_util_encode_measurement_types,
      .arg = &types_encoder_arg,
    },
  };

  return pb_encode(&session->data_stream, pebble_pipeline_MeasurementSet_fields, &msg);
}

// ---------------------------------------------------------------------------------------
// Starts/restarts a session. Allows each type to setup what they need to setup.
static bool prv_session_encode_start(PLogSession *session) {
  const ProtobufLogConfig *config = &session->config;

  // New session start time
  session->start_utc = rtc_get_time();

  // Create a new stream, reserving space for the header in front
  session->data_stream = pb_ostream_from_buffer(session->data_buffer, session->max_data_size);

  switch (config->type) {
    case ProtobufLogType_Measurements: {
      return prv_session_measurement_encode_start(session);
    }
    case ProtobufLogType_Events:
      return true;
  }
  WTF;
}

// ---------------------------------------------------------------------------------------
// Calculates how much space an empty Payload will consume in our buffer. Useful for seeing how
// much *other* data we can store in a fixed sized DataLogging packet
static uint32_t prv_get_hdr_reserved_size(ProtobufLogConfig *config) {
  // Figure out how much space we need to reserve for the payload structure in each record
  pb_ostream_t substream = PB_OSTREAM_SIZING;
  // Encode a payload with a 0 length data blob.
  bool success = prv_populate_payload(config, 0, NULL, &substream);
  PBL_ASSERT(success, "error encoding payload");

  // Save enough room for us to encode the length of the data buffer
  return substream.bytes_written + MLOG_MAX_VARINT_ENCODED_SIZE;
}


ProtobufLogRef protobuf_log_create(ProtobufLogConfig *config,
                                   ProtobufLogTransportCB transport,
                                   size_t max_msg_size) {
  // Error check the passed in max encoded message size
  PBL_ASSERTN(max_msg_size <= PLOG_DLS_RECORD_SIZE);
  if (max_msg_size == 0) {
    max_msg_size = PLOG_DLS_RECORD_SIZE;
  }

  // Default transport
  if (!transport) {
    transport = prv_dls_transport;
  }

  // Create a buffer for the final fully-formed record. Since we send it out through data logging,
  // make it the size of a data logging record
  uint8_t *msg_buffer = kernel_zalloc(PLOG_DLS_RECORD_SIZE);
  if (!msg_buffer) {
    return NULL;
  }

  // Number of bytes that are needed to encode the payload structure
  // (not including the data blob)
  const uint32_t payload_hdr_size = prv_get_hdr_reserved_size(config);
  PROTOBUF_LOG_DEBUG("Creating payload session with hdr size of %"PRIu32, payload_hdr_size);

  // Create a buffer for the encoded data blob. We form this first as the caller calls
  // protobuf_log_session_add_* repeatedly. Once it's filled up, we grab it as the
  // data blob portion of the payload that's formed in msg_buffer.
  uint32_t max_data_size = max_msg_size - payload_hdr_size - sizeof(PLogMessageHdr);
  PROTOBUF_LOG_DEBUG("Max data buffer size: %"PRIu32, max_data_size);
  uint8_t *data_buffer = kernel_zalloc(max_data_size);
  if (!data_buffer) {
    kernel_free(msg_buffer);
    return NULL;
  }

  // Extra space needed for each config to store some variables and information.
  // e.g. Measurement needs to store an array of types.
  const size_t extra_size = prv_session_extra_space_needed(config);
  PLogSession *session = kernel_zalloc(sizeof(PLogSession) + extra_size);
  if (!session) {
    kernel_free(msg_buffer);
    kernel_free(data_buffer);
    return NULL;
  }

  *session = (PLogSession) {
    .config = *config,
    .msg_buffer = msg_buffer,
    .data_buffer = data_buffer,
    .max_msg_size = max_msg_size,
    .max_data_size = max_data_size,
    .transport = transport,
  };

  // Start a new encoding
  const bool success = prv_session_encode_start(session);
  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error encoding msg");
    prv_session_free(session);
    session = NULL;
  }

  return session;
}


// ---------------------------------------------------------------------------------------
// Sets the stream to PB_OSTREAM_SIZING and calculates the size of the protobuf structs
static uint32_t prv_get_encoded_struct_size(uint32_t field_number, const pb_msgdesc_t * fields,
                                            const void *msg) {
  pb_ostream_t stream = PB_OSTREAM_SIZING;
  prv_encode_struct(&stream, field_number, fields, msg);
  return stream.bytes_written;
}


// ---------------------------------------------------------------------------------------
// Takes a generic protobuf struct, calculates the size, and writes it out to the internal buffer.
// If it is full, flush then log it.
static bool prv_log_struct(PLogSession *session, uint32_t field_number,
                           const pb_msgdesc_t * fields, const void *msg) {
  // Calculate the size of our struct encoded on wire
  const uint32_t calc_size = prv_get_encoded_struct_size(field_number, fields, msg);
  // Calculate our data blob buffer size if we add this struct to it
  const uint32_t size_if_added = session->data_stream.bytes_written + calc_size;

  // If it fits, add it. If it doesn't, flush first.
  if (size_if_added > session->max_data_size) {
    // We would be over capacity if we added this message. Let's flush first.
    PROTOBUF_LOG_DEBUG("Session: 0x%x - Would have been over limit at size %"PRIu32", flushing",
                       (int)session, size_if_added);
    protobuf_log_session_flush(session);
  }

  // Encode the struct into the message
  bool success = prv_encode_struct(&session->data_stream, field_number, fields, msg);
  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error adding sample, resetting session");
    return prv_session_encode_start(session);
  }

  return true;
}


bool protobuf_log_session_add_measurements(ProtobufLogRef session_ref, time_t sample_utc,
                                          uint32_t num_values, uint32_t *values) {
  PBL_ASSERTN(session_ref != NULL);
  PLogSession *session = (PLogSession *)session_ref;
  PBL_ASSERTN(session->config.type == ProtobufLogType_Measurements);
  int32_t offset_sec = sample_utc - session->start_utc;
  offset_sec = MAX(0, offset_sec);

  // error check
  PBL_ASSERT(num_values == session->config.measurements.num_types, "Wrong number of values passed");

  PROTOBUF_LOG_DEBUG("Session: 0x%x - Adding measurement sample with %"PRIu32" values",
                     (int)session_ref, num_values);

  // Encode the Measurement
  PLogPackedVarintsEncoderArg packed_varint_encoder_arg = {
    .num_values = num_values,
    .values = values,
  };
  pebble_pipeline_Measurement msg = {
    .offset_sec = offset_sec,
    .data = {
      .funcs.encode = protobuf_log_util_encode_packed_varints,
      .arg = &packed_varint_encoder_arg,
    },
  };

  bool success = prv_log_struct(session,
                                pebble_pipeline_MeasurementSet_measurements_tag,
                                pebble_pipeline_Measurement_fields,
                                &msg);
  return success;
}


bool protobuf_log_session_add_event(ProtobufLogRef session_ref, pebble_pipeline_Event *event) {
  PBL_ASSERTN(session_ref != NULL);
  PLogSession *session = (PLogSession *)session_ref;
  PBL_ASSERTN(session->config.type == ProtobufLogType_Events);

  // Generate a new UUID
  Uuid uuid;
  uuid_generate(&uuid);

  // Don't use {} notation because we won't want to overwrite the data that is already set in
  // the event.
  event->created_time_utc = rtc_get_time();
  event->has_created_time_utc = true;
  event->utc_to_local = time_util_utc_to_local_offset();
  event->uuid = (pb_callback_t) {
    .funcs.encode = protobuf_log_util_encode_uuid,
    .arg = &uuid,
  };

  PROTOBUF_LOG_DEBUG("Session: 0x%x - Adding event with type: %d", (int)session_ref, event->type);

  bool success = prv_log_struct(session,
                                pebble_pipeline_Payload_events_tag,
                                pebble_pipeline_Event_fields,
                                event);
  return success;
}


bool protobuf_log_session_flush(ProtobufLogRef session_ref) {
  PBL_ASSERTN(session_ref != NULL);
  PLogSession *session = (PLogSession *)session_ref;
  bool encode_success = false;

  // Encode the buffer into a Payload
  const size_t hdr_size = sizeof(PLogMessageHdr);
  pb_ostream_t stream = pb_ostream_from_buffer(session->msg_buffer + hdr_size,
                                               session->max_msg_size - hdr_size);

  bool success = prv_populate_payload(&session->config, session->data_stream.bytes_written,
                                      session->data_buffer, &stream);
  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error encoding payload");
    goto exit;
  }

  // Fill in the message header now
  PLogMessageHdr *hdr = (PLogMessageHdr *)session->msg_buffer;
  *hdr = (PLogMessageHdr) {
    .msg_size = stream.bytes_written,
  };

  // Send it out now
  PROTOBUF_LOG_DEBUG("Session: 0x%x - Flushing %d bytes", (int)session_ref, hdr->msg_size);
  success = (session->transport)(session->msg_buffer, hdr->msg_size + sizeof(PLogMessageHdr));
  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Failure when sending encoded message, resetting session");
  }

  // TODO: Call a success callback so the clients know exactly which data has been sent.
  // If we don't do this an we crash without pushing to datalogging, we'll lose data

exit:
  encode_success = prv_session_encode_start(session);
  return (success && encode_success);
}


bool protobuf_log_session_delete(ProtobufLogRef session_ref) {
  PROTOBUF_LOG_DEBUG("Session: 0x%x - Deleting", (int)session_ref);

  if (session_ref == NULL) {
    return true;
  }
  protobuf_log_session_flush(session_ref);
  prv_session_free(session_ref);
  return true;
}
