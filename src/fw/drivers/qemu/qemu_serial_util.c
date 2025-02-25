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

#include "kernel/pbl_malloc.h"

#include "drivers/rtc.h"
#include "drivers/qemu/qemu_serial.h"
#include "drivers/qemu/qemu_serial_private.h"

#include "system/passert.h"
#include "system/logging.h"

#include "util/math.h"
#include "util/net.h"
#include "system/hexdump.h"


// -----------------------------------------------------------------------------------------
void qemu_serial_private_init_state(QemuSerialGlobals *state)
{

  // Create our mutex
  state->qemu_comm_lock = mutex_create();
  state->initialized = true;

  // Allocate buffer for received characters from the ISR
  uint32_t buffer_size = QEMU_ISR_RECV_BUFFER_SIZE;
  uint8_t *buffer_data = kernel_malloc_check(buffer_size);
  shared_circular_buffer_init(&state->isr_buffer, buffer_data,  buffer_size);
  shared_circular_buffer_add_client(&state->isr_buffer, &state->isr_buffer_client);

  // Allocate buffer for the received message
  state->msg_buffer = kernel_malloc_check(QEMU_MAX_DATA_LEN);
  state->msg_buffer_bytes = 0;

}


// -----------------------------------------------------------------------------------------
// Helper function triggered by our ISR handler when we detect a high water mark on our receive
// buffer or a footer signature.
//
// Parses the ISR's circular buffer and collects assembled message into a message buffer. If
// a complete packets has been assembled, it returns a pointer to the message buffer,
// the size of the packet (in *msg_bytes), and the protocol (in *protocol). If a complete packet
// is not ready yet, it returns a NULL.
//
// @param[in] state pointer to our state variables
// @param[out] *msg_bytes number of bytes in assembled message
// @param[out] *protocol protocol for the message
// @return pointer to message, or NULL if no message available yet
uint8_t *qemu_serial_private_assemble_message(QemuSerialGlobals *state, uint32_t *msg_bytes,
            uint16_t *protocol) {
  uint16_t bytes_read;
  uint8_t byte;
  bool exit = false;
  bool got_msg = false;
  time_t  cur_time = rtc_get_time();

  // Reset our state if too much time has passed since we detected start of a packet
  if (state->recv_state != QemuRecvState_WaitingHdrSignatureMSB
      && cur_time > state->start_recv_packet_time + QEMU_RECV_PACKET_TIMEOUT_SEC) {
    state->recv_state = QemuRecvState_WaitingHdrSignatureMSB;
    PBL_LOG(LOG_LEVEL_WARNING, "Resetting receive state - max packet time expired");
  }

  state->callback_pending = false;

  uint16_t bytes_avail = shared_circular_buffer_get_read_space_remaining(&state->isr_buffer,
                &state->isr_buffer_client);
  QEMU_LOG_DEBUG("prv_assemble_packet, state:%d, bytes:%d", state->recv_state,
            bytes_avail);

  // Log message if we detected any receive errors
  if (state->recv_error_count) {
    PBL_LOG(LOG_LEVEL_ERROR, "%"PRIu32" receive errors detected", state->recv_error_count);
    state->recv_error_count = 0;
  }

  while (!exit && bytes_avail) {
    switch (state->recv_state) {
      case QemuRecvState_WaitingHdrSignatureMSB:  {
        state->msg_buffer_bytes = 0;
        shared_circular_buffer_read_consume(&state->isr_buffer, &state->isr_buffer_client, 1, &byte,
              &bytes_read);
        bytes_avail -= bytes_read;
        if (byte == QEMU_HEADER_MSB) {
          QEMU_LOG_DEBUG("got header signature MSB");
          state->recv_state = QemuRecvState_WaitingHdrSignatureLSB;
          state->start_recv_packet_time = cur_time;
        }
      }
      break;

      case QemuRecvState_WaitingHdrSignatureLSB:  {
        shared_circular_buffer_read_consume(&state->isr_buffer, &state->isr_buffer_client, 1, &byte,
              &bytes_read);
        bytes_avail -= bytes_read;
        if (byte == QEMU_HEADER_LSB) {
          state->recv_state = QemuRecvState_WaitingHdr;
          QEMU_LOG_DEBUG("got header signature LSB");
        } else {
          state->recv_state = QemuRecvState_WaitingHdr;
        }
      }
      break;

      case QemuRecvState_WaitingHdr:  {
        // We already read in the header signature
        uint16_t req_bytes = sizeof(state->hdr) - sizeof(state->hdr.signature);
        if (bytes_avail < req_bytes) {
          exit = true;
          break;
        }
        shared_circular_buffer_read_consume(&state->isr_buffer, &state->isr_buffer_client,
              req_bytes, (uint8_t *)&state->hdr.protocol, &bytes_read);
        bytes_avail -= bytes_read;

        // Do byte swapping
        state->hdr.signature = QEMU_HEADER_SIGNATURE;
        state->hdr.protocol = ntohs(state->hdr.protocol);
        state->hdr.len = ntohs(state->hdr.len);

        // Validity checking
        if (state->hdr.len > QEMU_MAX_DATA_LEN) {
          PBL_LOG(LOG_LEVEL_ERROR, "Invalid header data size %d", state->hdr.len);
          state->recv_state = QemuRecvState_WaitingHdrSignatureMSB;
        } else {
          QEMU_LOG_DEBUG("got header: protocol: %d, len: %d", state->hdr.protocol, state->hdr.len);
          state->recv_state = QemuRecvState_WaitingData;
        }
      }
      break;

      case QemuRecvState_WaitingData: {
        uint16_t bytes_needed = state->hdr.len - state->msg_buffer_bytes;
        shared_circular_buffer_read_consume(&state->isr_buffer, &state->isr_buffer_client,
              MIN(bytes_avail, bytes_needed), state->msg_buffer + state->msg_buffer_bytes,
              &bytes_read);
        state->msg_buffer_bytes += bytes_read;
        bytes_avail -= bytes_read;

        QEMU_LOG_DEBUG("received %d bytes of msg data, need %d more", bytes_read,
                        state->hdr.len - state->msg_buffer_bytes);

        // Got the complete message?
        if (state->msg_buffer_bytes >= state->hdr.len) {
          state->recv_state = QemuRecvState_WaitingFooter;
          got_msg = true;
          exit = true;
        }
      }
      break;

      case QemuRecvState_WaitingFooter: {
        QemuCommChannelFooter footer;
        if (bytes_avail < sizeof(QemuCommChannelFooter)) {
          exit = true;
        } else {
          shared_circular_buffer_read_consume(&state->isr_buffer, &state->isr_buffer_client,
                sizeof(footer), (uint8_t *)&footer, &bytes_read);
          bytes_avail -= bytes_read;
          footer.signature = ntohs(footer.signature);
          if (footer.signature != QEMU_FOOTER_SIGNATURE) {
            PBL_LOG(LOG_LEVEL_WARNING, "Invalid footer signature");
          }
          state->recv_state = QemuRecvState_WaitingHdrSignatureMSB;
        }
      }
      break;
    } // switch()
  } // while (!exit && bytes_avail)

  // Return pointer if we got a complete message
  if (got_msg) {
    *msg_bytes= state->msg_buffer_bytes;
    *protocol = state->hdr.protocol;
    return state->msg_buffer;
  } else {
    return NULL;
  }
}



// ------------------------------------------------------------------------------------------
// Unit test support

// @return true if successful (buffer not full)
bool qemu_test_add_byte_from_isr(QemuSerialGlobals *state, uint8_t byte) {
  return shared_circular_buffer_write(&state->isr_buffer, &byte, 1, false /*advance_slackers*/);
}
