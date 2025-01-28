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

#include "applib/app_smartstrap.h"
#include "drivers/accessory.h"
#include "kernel/events.h"
#include "kernel/pebble_tasks.h"
#include "process_management/worker_manager.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/system_task.h"
#include "services/normal/accessory/accessory_manager.h"
#include "services/normal/accessory/smartstrap_comms.h"
#include "services/normal/accessory/smartstrap_connection.h"
#include "services/normal/accessory/smartstrap_profiles.h"
#include "services/normal/accessory/smartstrap_state.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "os/mutex.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/crc8.h"
#include "util/hdlc.h"
#include "util/math.h"
#include "util/mbuf_iterator.h"

//! The timeout for receiving the context frame after the break characters in ms
static const uint32_t NOTIFY_TIMEOUT = 100;
static const uint16_t SMARTSTRAP_MAX_TIMEOUT = 1000;

//! The header contains the version (1 byte), flags (4 bytes), and profile (2 bytes) fields
//! The footer contains the checksum (1 byte) field
#define FRAME_FOOTER_LENGTH             1
#define FRAME_MIN_LENGTH                (sizeof(FrameHeader) + FRAME_FOOTER_LENGTH)

typedef struct PACKED {
  uint8_t version;
  union {
    struct PACKED {
      uint8_t is_read:1;
      uint8_t is_master:1;
      uint8_t is_notify:1;
      uint32_t reserved:29;
    };
    uint32_t raw;
  } flags;
  uint16_t profile;
} FrameHeader;

typedef struct {
  //! HDLC context
  HdlcStreamingContext hdlc_ctx;
  //! The total number of bytes we've read for this frame
  uint32_t length;
  //! A temporary buffer for storing the footer (checksum byte)
  uint8_t footer_byte;
  //! The checksum byte (comes after the payload in the frame)
  uint8_t checksum;
  //! Flag which is set if we find the frame is invalid
  bool should_drop;
} ReadInfo;

typedef struct {
  //! The profile used for the request
  SmartstrapProfile profile;
  //! The MBufIterator to read data into
  MBufIterator mbuf_iter;
} ReadConsumer;

typedef union {
  struct PACKED {
    bool success;
    bool is_notify;
  };
  void *context_ptr;
} ReadCompleteContext;
_Static_assert(sizeof(ReadCompleteContext) == sizeof(void *), "ReadCompleteContext too big");

typedef struct {
  MBufIterator mbuf_iter;
  bool is_read;
  bool sent_escape;
  uint8_t escaped_byte;
} SendInfo;

//! Info on the current frame being read
static ReadInfo s_read_info;
//! The consumer of the next frame which is read
static ReadConsumer s_read_consumer;
//! MBuf for storing the header when receiving
static MBuf s_header_mbuf;
static uint8_t s_header_data[sizeof(FrameHeader)];
//! Info on the current frame being sent
static SendInfo s_send_info;

//! Timer used to enforce read timeouts
static TimerID s_read_timer = TIMER_INVALID_ID;


// Init
////////////////////////////////////////////////////////////////////////////////

void smartstrap_comms_init(void) {
  s_read_timer = new_timer_create();
  s_header_mbuf = MBUF_EMPTY;
  mbuf_set_data(&s_header_mbuf, s_header_data, sizeof(s_header_data));
}


// Helper functions for static variables
////////////////////////////////////////////////////////////////////////////////

static void prv_reset_read_info(void) {
  s_read_info = (ReadInfo) { };
  hdlc_streaming_decode_reset(&s_read_info.hdlc_ctx);
}

static void prv_reset_read_consumer(void) {
  s_read_consumer = (ReadConsumer) { 0 };
  mbuf_iterator_init(&s_read_consumer.mbuf_iter, NULL);
}

void smartstrap_comms_set_enabled(bool enabled) {
  if (enabled) {
    prv_reset_read_info();
    prv_reset_read_consumer();
  } else {
    new_timer_stop(s_read_timer);
  }
}

// Receive functions
////////////////////////////////////////////////////////////////////////////////

static void prv_read_complete_system_task_cb(void *context_ptr) {
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);
  ReadCompleteContext context = { .context_ptr = context_ptr };

  smartstrap_state_lock();
  if (smartstrap_fsm_state_get() != SmartstrapStateReadComplete) {
    // We could not be in a ReadComplete state if we got disconnected or if we got a complete frame
    // while the timeout was scheduled.
    mbuf_clear_next(&s_header_mbuf);
    smartstrap_state_unlock();
    return;
  }
  // All other tasks and ISRs will be blocked while we are in the ReadComplete state and while we
  // hold the state lock, so we're free to access / modify static variables until we transition
  // the state back to ReadReady.

  SmartstrapProfile read_profile = s_read_consumer.profile;
  uint32_t read_length = 0;
  if (context.success) {
    if (context.is_notify) {
      // get the profile from the frame
      FrameHeader *header = mbuf_get_data(&s_header_mbuf);
      read_profile = header->profile;
    }
    PBL_ASSERTN(s_read_info.length >= FRAME_MIN_LENGTH);
    read_length = s_read_info.length - FRAME_MIN_LENGTH;
    // don't care if the timeout is already queued as the FSM state will make it a noop
    new_timer_stop(s_read_timer);
  }

  accessory_use_dma(false);
  mbuf_clear_next(&s_header_mbuf);
  prv_reset_read_info();
  prv_reset_read_consumer();
  smartstrap_fsm_state_set(SmartstrapStateReadReady);
  smartstrap_state_unlock();

  if (context.is_notify) {
    smartstrap_profiles_handle_notification(context.success, read_profile);
  } else {
    smartstrap_profiles_handle_read(context.success, read_profile, read_length);
  }
}

static void prv_read_timeout(void *context) {
  if (smartstrap_fsm_state_test_and_set(SmartstrapStateReadInProgress,
                                        SmartstrapStateReadComplete)) {
    // we need to handle the timeout from KernelBG
    ReadCompleteContext context = {
      .success = false,
      .is_notify = false
    };
    system_task_add_callback(prv_read_complete_system_task_cb, context.context_ptr);
  }
}

static void prv_store_byte(const uint8_t data) {
  // NOTE: THIS IS RUN WITHIN AN ISR
  // The checksum byte is the last byte in the frame. This byte could be the last byte we receive
  // (making it the checksum byte), so we always keep a 1 byte temporary buffer before storing the
  // byte in the MBuf. This avoids us potentially overrunning a conservatively sized payload buffer;
  if (s_read_info.length > 0) {
    // copy the previous byte from the footer_byte field into the payload
    if (!mbuf_iterator_write_byte(&s_read_consumer.mbuf_iter,
                                  s_read_info.footer_byte)) {
      // no room left to store this byte
      s_read_info.should_drop = true;
    }
  }
  // Store this byte in the footer_byte. Note that we will still calculate the checksum on this byte
  // and verify that the checksum is 0 at the end, so if this byte is the actual footer byte (aka.
  // the checksum), we will still include it in the checksum.
  s_read_info.footer_byte = data;

  // increment the length and run the CRC calculation
  s_read_info.length++;
  crc8_calculate_bytes_streaming((uint8_t *)&data, sizeof(data), (uint8_t *)&s_read_info.checksum,
                                 false /* !big_endian */);
}

static void prv_handle_complete_frame(bool *should_context_switch) {
  FrameHeader *header = mbuf_get_data(&s_header_mbuf);
  bool is_notify = header->flags.is_notify;
  if ((is_notify && (smartstrap_fsm_state_get() != SmartstrapStateNotifyInProgress)) ||
      (!is_notify && (s_read_consumer.profile != header->profile))) {
    // We weither got a notify frame in response to a normal read, or we got a response for a
    // different frame than we requested.
    s_read_info.should_drop = true;
  }
  if ((s_read_info.should_drop == false) &&
      (header->version > 0) &&
      (header->version <= SMARTSTRAP_PROTOCOL_VERSION) &&
      !header->flags.is_read &&
      !header->flags.is_master &&
      !header->flags.reserved &&
      (header->profile > SmartstrapProfileInvalid) &&
      (header->profile < NumSmartstrapProfiles) &&
      (s_read_info.length >= FRAME_MIN_LENGTH) &&
      !s_read_info.checksum) {
    // If this is a notification, we shouldn't have a read consumer set.
    PBL_ASSERTN(!is_notify || (s_read_consumer.profile == SmartstrapProfileInvalid));
    // this frame is valid - transition the FSM and queue up processing of it
    smartstrap_fsm_state_set(SmartstrapStateReadComplete);
    ReadCompleteContext context = {
      .success = true,
      .is_notify = is_notify
    };
    system_task_add_callback_from_isr(prv_read_complete_system_task_cb, context.context_ptr,
                                      should_context_switch);
  } else {
    // Reset our context so we can try again to receive a frame in case we do happen to get a valid
    // one before the timeout occurs.
    prv_reset_read_info();
    mbuf_iterator_init(&s_read_consumer.mbuf_iter, &s_header_mbuf);
  }
}

bool smartstrap_handle_data_from_isr(uint8_t data) {
  // NOTE: THIS IS RUN WITHIN AN ISR
  if ((smartstrap_fsm_state_get() != SmartstrapStateReadInProgress) &&
      (smartstrap_fsm_state_get() != SmartstrapStateNotifyInProgress)) {
    return false;
  }

  bool should_context_switch = false;
  bool hdlc_err;
  bool should_store;
  bool is_complete = hdlc_streaming_decode(&s_read_info.hdlc_ctx, &data, &should_store, &hdlc_err);
  if (hdlc_err) {
    // the rest of the frame is invalid
    s_read_info.should_drop = true;
  } else if (is_complete) {
    prv_handle_complete_frame(&should_context_switch);
  } else if (should_store && !s_read_info.should_drop) {
    prv_store_byte(data);
  }

  return should_context_switch;
}

void prv_notify_timeout(void *context) {
  if (smartstrap_fsm_state_test_and_set(SmartstrapStateNotifyInProgress,
                                        SmartstrapStateReadComplete)) {
    // we need to handle the timeout from KernelBG
    ReadCompleteContext context = {
      .success = false,
      .is_notify = true
    };
    system_task_add_callback(prv_read_complete_system_task_cb, context.context_ptr);
  }
}

void prv_schedule_notify_timeout(void *context) {
  // make sure there's still a notification pending
  if (smartstrap_fsm_state_get() == SmartstrapStateNotifyInProgress) {
    PBL_ASSERTN(new_timer_start(s_read_timer, NOTIFY_TIMEOUT, prv_notify_timeout, NULL, 0));
  }
}

bool smartstrap_handle_break_from_isr(void) {
  // NOTE: THIS IS RUN WITHIN AN ISR
  bool should_context_switch = false;
  // we should only accept notifications if we're in the ReadReady state
  if (smartstrap_fsm_state_test_and_set(SmartstrapStateReadReady,
                                        SmartstrapStateNotifyInProgress)) {
    // prepare to read notification context
    PBL_ASSERTN(!mbuf_get_next(&s_header_mbuf));
    mbuf_iterator_init(&s_read_consumer.mbuf_iter, &s_header_mbuf);
    system_task_add_callback_from_isr(prv_schedule_notify_timeout, NULL, &should_context_switch);
  }
  return should_context_switch;
}


// Sending functions
////////////////////////////////////////////////////////////////////////////////

static bool prv_send_byte_and_check(uint8_t data) {
  // NOTE: THIS IS RUN WITHIN AN ISR
  accessory_send_byte(data);
  const bool bus_contention = accessory_bus_contention_detected();
  if (bus_contention) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Bus contention was detected!");
  }
  return !bus_contention;
}

static bool prv_send_byte(uint8_t data) {
  // NOTE: THIS IS RUN WITHIN AN ISR
  if (hdlc_encode(&data)) {
    PBL_ASSERTN(!s_send_info.sent_escape);
    s_send_info.sent_escape = true;
    s_send_info.escaped_byte = data;
    data = HDLC_ESCAPE;
  }
  return prv_send_byte_and_check(data);
}

static bool prv_send_stream_callback(void *context) {
  // NOTE: THIS IS RUN WITHIN AN ISR
  if (smartstrap_fsm_state_get() != SmartstrapStateReadDisabled) {
    // we should no longer be sending
    return false;
  }

  // handle escaped bytes first
  if (s_send_info.sent_escape) {
    s_send_info.sent_escape = false;
    return prv_send_byte_and_check(s_send_info.escaped_byte);
  }

  // send the next byte
  bool result = true;
  MBufIterator *iter = &s_send_info.mbuf_iter;
  MBuf *mbuf = mbuf_iterator_get_current_mbuf(iter);
  uint8_t read_data;
  PBL_ASSERTN(mbuf_iterator_read_byte(iter, &read_data));
  if (mbuf_is_flag_set(mbuf, MBUF_FLAG_IS_FRAMING)) {
    result = prv_send_byte_and_check(read_data);
  } else {
    result = prv_send_byte(read_data);
  }

  if (mbuf_iterator_is_finished(iter)) {
    // we just sent the last byte
    if (s_send_info.is_read) {
      // We just successfully sent a read request, so should move to ReadInProgress to prepare to
      // read the response. We do this here to ensure we don't miss any bytes of the response due to
      // KernelBG not getting scheduled quickly enough.
      PBL_ASSERTN(!mbuf_get_next(&s_header_mbuf));
      mbuf_append(&s_header_mbuf, context);
      mbuf_iterator_init(&s_read_consumer.mbuf_iter, &s_header_mbuf);
      smartstrap_fsm_state_set(SmartstrapStateReadInProgress);
    }
    result = false;
  }

  if (!result) {
    accessory_enable_input();
  }

  return result;
}

SmartstrapResult smartstrap_send(SmartstrapProfile profile, MBuf *write_mbuf, MBuf *read_mbuf,
                                 uint16_t timeout_ms) {
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);
  smartstrap_state_assert_locked_by_current_task();
  // we expect the arguments to be valid
  const bool is_read = (read_mbuf != NULL);
  PBL_ASSERTN((profile > SmartstrapProfileInvalid) && (profile < NumSmartstrapProfiles));
  PBL_ASSERTN(!is_read || (mbuf_get_chain_length(read_mbuf) > 0));
  PBL_ASSERTN((!write_mbuf && !read_mbuf) || (write_mbuf != read_mbuf));
  timeout_ms = MIN(timeout_ms, SMARTSTRAP_MAX_TIMEOUT);

  // transition the FSM state
  if (!smartstrap_fsm_state_test_and_set(SmartstrapStateReadReady, SmartstrapStateReadDisabled)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Failed to change smartstrap FSM state (%d)",
            smartstrap_fsm_state_get());
    return SmartstrapResultBusy;
  }

  // We are now be in a state which allows us to freely modify static variables as we can be sure
  // that no ISR or other tasks will be allowed to access or modify them while we are in this
  // state.
  accessory_disable_input();
  // NOTE: Accessory input will be re-enabled by the stream callback after we finish sending
  prv_reset_read_info();
  prv_reset_read_consumer();
  s_send_info = (SendInfo) { .is_read = is_read };

  if (is_read) {
    // populate the read consumer info
    s_read_consumer.profile = profile;
  }

  // Go through and build the frame: Start_Flag | Header | Payload | Checksum | End_Flag

  // Start_Flag
  uint8_t flag_data = HDLC_FLAG;
  MBuf start_flag_mbuf = MBUF_EMPTY;
  mbuf_set_data(&start_flag_mbuf, &flag_data, sizeof(flag_data));
  mbuf_set_flag(&start_flag_mbuf, MBUF_FLAG_IS_FRAMING, true);

  // Header
  FrameHeader header = (FrameHeader) {
    .version = SMARTSTRAP_PROTOCOL_VERSION,
    .flags = {
      .is_read = is_read,
      .is_master = true,
    },
    .profile = profile
  };
  MBuf header_mbuf = MBUF_EMPTY;
  mbuf_set_data(&header_mbuf, &header, sizeof(header));
  mbuf_append(&start_flag_mbuf, &header_mbuf);

  // Payload
  mbuf_append(&start_flag_mbuf, write_mbuf);

  // Checksum
  uint8_t checksum = 0;
  for (MBuf *m = &header_mbuf; m; m = mbuf_get_next(m)) {
    if (!mbuf_is_flag_set(m, MBUF_FLAG_IS_FRAMING)) {
      crc8_calculate_bytes_streaming(m->data, m->length, &checksum, false /* !big_endian */);
    }
  }
  MBuf footer_mbuf = MBUF_EMPTY;
  mbuf_set_data(&footer_mbuf, &checksum, sizeof(checksum));
  mbuf_append(&start_flag_mbuf, &footer_mbuf);

  // End_Flag
  MBuf end_flag_mbuf = MBUF_EMPTY;
  mbuf_set_data(&end_flag_mbuf, &flag_data, sizeof(flag_data));
  mbuf_set_flag(&end_flag_mbuf, MBUF_FLAG_IS_FRAMING, true);
  mbuf_append(&start_flag_mbuf, &end_flag_mbuf);

  // send off the frame
  mbuf_iterator_init(&s_send_info.mbuf_iter, &start_flag_mbuf);
  accessory_use_dma(true);
  if (!accessory_send_stream(prv_send_stream_callback, (void *)read_mbuf)) {
    accessory_enable_input();
  }

  if (is_read) {
    // If we sent the request successfully, the send ISR will have transitioned us out of
    // ReadDisabled.
    const bool was_successful = (smartstrap_fsm_state_get() != SmartstrapStateReadDisabled);
    if (was_successful) {
      // start the timer for the read timeout
      PBL_ASSERTN(new_timer_start(s_read_timer, timeout_ms, prv_read_timeout, NULL, 0));
    } else {
      // clean up and return an error
      accessory_use_dma(false);
      prv_reset_read_consumer();
      smartstrap_fsm_state_set(SmartstrapStateReadReady);
      return SmartstrapResultBusy;
    }
  } else {
    accessory_use_dma(false);
    smartstrap_fsm_state_set(SmartstrapStateReadReady);
    if (!mbuf_iterator_is_finished(&s_send_info.mbuf_iter)) {
      // The write was not successful, so return an error
      return SmartstrapResultBusy;
    }
  }

  return SmartstrapResultOk;
}

void smartstrap_cancel_send(void) {
  // Enter a critical region to prevent anybody else changing the state.
  portENTER_CRITICAL();
  SmartstrapState state = smartstrap_fsm_state_get();
  if ((state != SmartstrapStateReadDisabled) && (state != SmartstrapStateReadInProgress) &&
      (state != SmartstrapStateReadComplete)) {
    // we aren't in a state where something is in progress, so there's nothing to do
    portEXIT_CRITICAL();
    return;
  }
  smartstrap_fsm_state_reset();
  new_timer_stop(s_read_timer);
  smartstrap_profiles_handle_read_aborted(s_read_consumer.profile);
  prv_reset_read_info();
  prv_reset_read_consumer();
  mbuf_clear_next(&s_header_mbuf);
  portEXIT_CRITICAL();
  PBL_LOG(LOG_LEVEL_WARNING, "Canceled an in-progress request. Was in state: %d", state);
}
