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

#if PULSE_EVERYWHERE

#include "pulse_protocol_impl.h"
#include "pulse2_reliable_retransmit_timer.h"

#include "console/control_protocol.h"
#include "console/control_protocol_impl.h"
#include "console/pulse.h"
#include "console/pulse2_transport_impl.h"
#include "console/pulse_control_message_protocol.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "services/common/system_task.h"
#include "system/passert.h"
#include <util/attributes.h>
#include <util/net.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>


//! Modulus for sequence numbers
#define MODULUS (128u)
#define MAX_RETRANSMITS (10)
#define RETRANSMIT_TIMEOUT_MS (200)

// Reliable Transport protocol
// ===========================

//! A buffer for holding a reliable info packet in memory while it is un-ACKed.
typedef struct ReliableInfoBuffer {
  uint16_t app_protocol;
  uint16_t length;
  char information[];
} ReliableInfoBuffer;

enum SupervisoryKind {
  SupervisoryKind_ReceiveReady = 0b00,
  SupervisoryKind_ReceiveNotReady = 0b01,
  SupervisoryKind_Reject = 0b10,
};

typedef union ReliablePacket {
  bool is_supervisory:1;
  struct PACKED ReliableInfoPacket {
    bool is_supervisory:1;
    uint8_t sequence_number:7;
    bool poll:1;
    uint8_t ack_number:7;
    net16 protocol;
    net16 length;
    char information[];
  } i;
  struct PACKED ReliableSupervisoryPacket {
    bool is_supervisory:1;
    bool is_unnumbered:1;  // is_unnumbered=true is unsupported
    enum SupervisoryKind kind:2;
    char _reserved:4;
    bool poll_or_final:1;
    uint8_t ack_number:7;
  } s;
} ReliablePacket;

static PulseControlMessageProtocol s_reliable_pcmp = {
  .send_begin_fn = pulse_reliable_send_begin,
  .send_fn = pulse_reliable_send,
};

_Static_assert(sizeof((ReliablePacket){0}.i) == 6,
               "sizeof ReliablePacket.i is wrong");
_Static_assert(sizeof((ReliablePacket){0}.s) == 2,
               "sizeof ReliablePacket.s is wrong");
_Static_assert(sizeof((ReliablePacket){0}.i) == sizeof(ReliablePacket),
               "Something is really wrong here");

static bool s_layer_up = false;
static ReliableInfoBuffer *s_tx_buffer;
static SemaphoreHandle_t s_tx_lock;

//! The sequence number of the next in-sequence I-packet to be transmitted.
//! V(S) in the LAPB spec.
static uint8_t s_send_variable;
static uint8_t s_retransmit_count;
static uint8_t s_last_ack_number;  //!< N(R) of most recently received packet.
static uint8_t s_receive_variable;  //!< V(R) in the LAPB spec.

static void prv_bounce_ncp_state(void);

size_t pulse_reliable_max_send_size(void) {
  return pulse_link_max_send_size() - sizeof(ReliablePacket);
}

static void prv_send_supervisory_response(enum SupervisoryKind kind,
                                          bool final) {
  ReliablePacket *packet = pulse_link_send_begin(
      PULSE2_RELIABLE_TRANSPORT_RESPONSE);
  packet->s = (struct ReliableSupervisoryPacket) {
    .is_supervisory = true,
    .kind = kind,
    .poll_or_final = final,
    .ack_number = s_receive_variable,
  };
  pulse_link_send(packet, sizeof(packet->s));
}

static void prv_send_info_packet(uint8_t sequence_number,
                                 uint16_t app_protocol,
                                 const char *information,
                                 uint16_t info_length) {
  PBL_ASSERT(info_length <= pulse_reliable_max_send_size(),
             "Packet too big to send");

  ReliablePacket *packet = pulse_link_send_begin(
      PULSE2_RELIABLE_TRANSPORT_COMMAND);
  size_t packet_size = sizeof(ReliablePacket) + info_length;
  packet->i = (struct ReliableInfoPacket) {
    .sequence_number = sequence_number,
    .poll = true,
    .ack_number = s_receive_variable,
    .protocol = hton16(app_protocol),
    .length = hton16(packet_size),
  };
  memcpy(&packet->i.information[0], information, info_length);
  pulse_link_send(packet, packet_size);
}

static void prv_process_ack(uint8_t ack_number) {
  if ((ack_number - 1) % MODULUS == s_send_variable) {
    pulse2_reliable_retransmit_timer_cancel();
    s_retransmit_count = 0;
    s_send_variable = (s_send_variable + 1) % MODULUS;
    xSemaphoreGive(s_tx_lock);
  }
}

static void prv_send_port_closed_message(void *context) {
  net16 bad_port;
  memcpy(&bad_port, &context, sizeof(bad_port));
  pulse_control_message_protocol_send_port_closed_message(
      &s_reliable_pcmp, bad_port);
}

void pulse2_reliable_transport_on_command_packet(
    void *raw_packet, size_t length) {
  if (!s_layer_up) {
    return;
  }

  if (length < sizeof((ReliablePacket){0}.s)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Received malformed command packet");
    prv_bounce_ncp_state();
    return;
  }
  ReliablePacket *packet = raw_packet;

  if (packet->is_supervisory) {
    if (packet->s.kind != SupervisoryKind_ReceiveReady &&
        packet->s.kind != SupervisoryKind_Reject) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Received a command packet of type %" PRIu8
              " which is not supported by this implementation.",
              (uint8_t)packet->s.kind);
      // Pretend it is an RR packet
    }
    prv_process_ack(packet->s.ack_number);
    if (packet->s.poll_or_final) {
      prv_send_supervisory_response(SupervisoryKind_ReceiveReady,
                                    /* final */ true);
    }
  } else {  // Information transfer packet
    if (length < sizeof(ReliablePacket)) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Received malformed Information packet");
      prv_bounce_ncp_state();
      return;
    }
    if (packet->i.sequence_number == s_receive_variable) {
      s_receive_variable = (s_receive_variable + 1) % MODULUS;
      if (ntoh16(packet->i.length) <= length) {
        size_t info_length = ntoh16(packet->i.length) - sizeof(ReliablePacket);
        // This variable is read in the macro-expansion below, but linters
        // have a hard time figuring that out.
        (void)info_length;

        switch (ntoh16(packet->i.protocol)) {
          case PULSE_CONTROL_MESSAGE_PROTOCOL:
            // TODO PBL-37695: PCMP sends packets synchronously, which will
            //      trip the not-KernelMain check on pulse_reliable_send_begin.
            // pulse_control_message_protocol_on_packet(
            //    &s_reliable_pcmp, packet->i.information, info_length);
            break;
#define ON_PACKET(N, HANDLER) \
          case N: \
            HANDLER(packet->i.information, info_length); \
            break;
#define ON_TRANSPORT_STATE_CHANGE(...)
#include "console/pulse2_reliable_protocol_registry.def"
#undef ON_PACKET
#undef ON_TRANSPORT_STATE_CHANGE
          default: {
            // Work around PBL-37695 by sending the Port-Closed message
            // from KernelBG.
            uintptr_t bad_port;
            memcpy(&bad_port, &packet->i.protocol, sizeof(packet->i.protocol));
            system_task_add_callback(prv_send_port_closed_message,
                                     (void *)bad_port);
            break;
          }
        }
      } else {
        PBL_LOG(LOG_LEVEL_DEBUG, "Received truncated or corrupt info packet "
                "field (expected %" PRIu16 ", got %" PRIu16 " data bytes). "
                "Discarding.", ntoh16(packet->i.length), (uint16_t)length);
        return;
      }
    }
    prv_send_supervisory_response(SupervisoryKind_ReceiveReady, packet->i.poll);
  }
}

void pulse2_reliable_transport_on_response_packet(
    void *raw_packet, size_t length) {
  if (!s_layer_up) {
    return;
  }

  if (length < sizeof((ReliablePacket){0}.s)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Received malformed response packet");
    prv_bounce_ncp_state();
    return;
  }
  ReliablePacket *packet = raw_packet;

  if (!packet->is_supervisory) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Received Information packet response; this is "
            "not permitted by the protocol (Information packets can only be "
            "commands). Discarding.");
    return;
  }

  prv_process_ack(packet->s.ack_number);

  if (packet->s.kind != SupervisoryKind_ReceiveReady &&
      packet->s.kind != SupervisoryKind_Reject) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Received a command packet of type %" PRIu8
            " which is not supported by this implementation.",
            (uint8_t)packet->s.kind);
  }
}

static void prv_start_retransmit_timer(uint8_t sequence_number);

void pulse2_reliable_retransmit_timer_expired_handler(
    uint8_t retransmit_sequence_number) {
  if (s_send_variable != retransmit_sequence_number) {
    // ACK was received and processed between the time that the
    // retransmit timer expired and this callback ran.
    return;
  }
  if (++s_retransmit_count < MAX_RETRANSMITS) {
        prv_send_info_packet(retransmit_sequence_number,
                             s_tx_buffer->app_protocol,
                             &s_tx_buffer->information[0],
                             s_tx_buffer->length);
    prv_start_retransmit_timer(retransmit_sequence_number);
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Reached maximum number of retransmit attempts.");
    prv_bounce_ncp_state();
  }
}

static void prv_start_retransmit_timer(uint8_t sequence_number) {
  pulse2_reliable_retransmit_timer_start(
      RETRANSMIT_TIMEOUT_MS, sequence_number);
}

static void prv_assert_reliable_buffer(void *buf) {
  PBL_ASSERT(buf == &s_tx_buffer->information[0],
             "The passed-in buffer pointer is not a buffer given by "
             "pulse_reliable_send_begin");
}

void *pulse_reliable_send_begin(const uint16_t app_protocol) {
  // The PULSE task processes ACKs and retransmits timed-out packets.
  // We will deadlock if we ever have to wait on s_tx_lock from the PULSE task.
  PBL_ASSERT_NOT_TASK(PebbleTask_PULSE);
  if (!s_layer_up) {
    return NULL;
  }
  xSemaphoreTake(s_tx_lock, portMAX_DELAY);
  if (!s_layer_up) {
    // Transport went down while waiting for the lock
    PBL_LOG(LOG_LEVEL_DEBUG, "Transport went down while waiting for lock");
    xSemaphoreGive(s_tx_lock);
    return NULL;
  }
  s_tx_buffer->app_protocol = app_protocol;
  return &s_tx_buffer->information[0];
}

void pulse_reliable_send_cancel(void *buf) {
  prv_assert_reliable_buffer(buf);
  xSemaphoreGive(s_tx_lock);
}

void pulse_reliable_send(void *buf, const size_t length) {
  if (!s_layer_up) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Transport went down before send");
    return;
  }
  prv_assert_reliable_buffer(buf);

  s_tx_buffer->length = length;
  uint8_t sequence_number = s_send_variable;

  prv_start_retransmit_timer(sequence_number);

  prv_send_info_packet(sequence_number,
                       s_tx_buffer->app_protocol,
                       &s_tx_buffer->information[0],
                       s_tx_buffer->length);

  // As soon as we send the packet we could get ACK'd, preempting this thread and releasing the
  // s_tx_lock. Don't do anything here that assumes the s_tx_lock is held.
}

// Reliable Transport Control Protocol
// ===================================

static void prv_on_this_layer_up(PPPControlProtocol *this) {
  s_layer_up = true;
  s_send_variable = 0;
  s_receive_variable = 0;
  s_retransmit_count = 0;
  s_last_ack_number = 0;
  xSemaphoreGive(s_tx_lock);

#define ON_PACKET(...)
#define ON_TRANSPORT_STATE_CHANGE(UP_HANDLER, DOWN_HANDLER) \
  UP_HANDLER();
#include "console/pulse2_reliable_protocol_registry.def"
#undef ON_PACKET
#undef ON_TRANSPORT_STATE_CHANGE
}

static void prv_on_this_layer_down(PPPControlProtocol *this) {
  pulse2_reliable_retransmit_timer_cancel();
  s_layer_up = false;
  xSemaphoreGive(s_tx_lock);

#define ON_PACKET(...)
#define ON_TRANSPORT_STATE_CHANGE(UP_HANDLER, DOWN_HANDLER) \
  DOWN_HANDLER();
#include "console/pulse2_reliable_protocol_registry.def"
#undef ON_PACKET
#undef ON_TRANSPORT_STATE_CHANGE
}

static void prv_on_receive_code_reject(PPPControlProtocol *this,
                                       LCPPacket *packet) {
  // TODO
}

static PPPControlProtocolState s_traincp_state = {};

static PPPControlProtocol s_traincp_protocol = {
  .protocol_number = PULSE2_RELIABLE_CONTROL_PROTOCOL,
  .state = &s_traincp_state,
  .on_this_layer_up = prv_on_this_layer_up,
  .on_this_layer_down = prv_on_this_layer_down,
  .on_receive_code_reject = prv_on_receive_code_reject,
};

PPPControlProtocol * const PULSE2_TRAINCP = &s_traincp_protocol;

void pulse2_reliable_control_on_packet(void *packet, size_t length) {
  ppp_control_protocol_handle_incoming_packet(PULSE2_TRAINCP, packet, length);
}

// Shared events
// =============
void pulse2_reliable_on_link_up(void) {
  ppp_control_protocol_lower_layer_is_up(PULSE2_TRAINCP);
}

void pulse2_reliable_on_link_down(void) {
  ppp_control_protocol_lower_layer_is_down(PULSE2_TRAINCP);
}

void pulse2_reliable_init(void) {
  ppp_control_protocol_init(PULSE2_TRAINCP);
  ppp_control_protocol_open(PULSE2_TRAINCP);
  s_tx_buffer = kernel_zalloc_check(sizeof(ReliableInfoBuffer) +
                                    pulse_reliable_max_send_size());
  s_tx_lock = xSemaphoreCreateBinary();
  xSemaphoreGive(s_tx_lock);
}

static void prv_bounce_ncp_state(void) {
  ppp_control_protocol_lower_layer_is_down(PULSE2_TRAINCP);
  ppp_control_protocol_lower_layer_is_up(PULSE2_TRAINCP);
}

#endif
