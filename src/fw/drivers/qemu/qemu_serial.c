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

#include "drivers/qemu/qemu_accel.h"
#include "drivers/qemu/qemu_battery.h"
#include "drivers/qemu/qemu_serial.h"
#include "drivers/qemu/qemu_serial_private.h"
#include "drivers/qemu/qemu_settings.h"
#include "drivers/uart.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "popups/timeline/peek.h"
#include "process_management/app_manager.h"
#include "shell/system_theme.h"
#include "services/common/clock.h"
#include "services/common/system_task.h"
#include "system/hexdump.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/likely.h"
#include "util/net.h"
#include "util/size.h"

#include "FreeRTOS.h"

#include <bluetooth/qemu_transport.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>


static bool prv_uart_irq_handler(UARTDevice *dev, uint8_t byte, const UARTRXErrorFlags *err_flags);

// Our globals
static QemuSerialGlobals s_qemu_state;


// -----------------------------------------------------------------------------------------
// Handle incoming Tap packet data (QemuProtocol_Tap)
static void prv_tap_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolTapHeader *hdr = (QemuProtocolTapHeader *)data;
  if (len != sizeof(*hdr)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet length");
    return;
  }

  QEMU_LOG_DEBUG("Got tap msg: axis: %d, direction: %d", hdr->axis, hdr->direction);
  PebbleEvent e = {
    .type = PEBBLE_ACCEL_SHAKE_EVENT,
    .accel_tap = {
      .axis = hdr->axis,
      .direction = hdr->direction,
    },
  };

  event_put(&e);
}


// -----------------------------------------------------------------------------------------
// Handle incoming Bluetooth connection packet data (QemuProtocol_BluetoothConnection)
static void prv_bluetooth_connection_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolBluetoothConnectionHeader *hdr = (QemuProtocolBluetoothConnectionHeader *)data;
  if (len != sizeof(*hdr)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet length");
    return;
  }

  QEMU_LOG_DEBUG("Got bluetooth connection msg: connected:%d", hdr->connected);
  bool current_status = qemu_transport_is_connected();
  bool new_status = (hdr->connected != 0);


  if (new_status != current_status && !bt_ctl_is_airplane_mode_on()) {
    // Change to new status if we're not in airplane mode
    qemu_transport_set_connected(new_status);
  }

}


// -----------------------------------------------------------------------------------------
// Handle incoming compass packet data (QemuProtocol_Compass)
static void prv_compass_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolCompassHeader *hdr = (QemuProtocolCompassHeader *)data;
  if (len != sizeof(*hdr)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet length");
    return;
  }

  QEMU_LOG_DEBUG("Got compass msg: magnetic_heading: %"PRId32", calib_status:%u",
        ntohl(hdr->magnetic_heading), hdr->calib_status);
  PebbleEvent e = {
    .type = PEBBLE_COMPASS_DATA_EVENT,
    .compass_data = {
      .magnetic_heading = ntohl(hdr->magnetic_heading),
      .calib_status = hdr->calib_status
    }
  };

  event_put(&e);
}


// -----------------------------------------------------------------------------------------
// Handle incoming time format data (QemuProtocol_TimeFormat)
static void prv_time_format_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolTimeFormatHeader *hdr = (QemuProtocolTimeFormatHeader *)data;
  if (len != sizeof(*hdr)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet length");
    return;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Got time format msg: is 24 hour: %d", hdr->is_24_hour);
  clock_set_24h_style(hdr->is_24_hour);
}


// -----------------------------------------------------------------------------------------
// Handle incoming timeline peek format data (QemuProtocol_TimelinePeek)
static void prv_timeline_peek_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolTimelinePeekHeader *hdr = (QemuProtocolTimelinePeekHeader *)data;
  if (len != sizeof(*hdr)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet length");
    return;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Got timeline peek msg: enabled: %d", hdr->enabled);
#if !RECOVERY_FW && CAPABILITY_HAS_TIMELINE_PEEK
  timeline_peek_set_enabled(hdr->enabled);
#endif
}


static void prv_content_size_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolContentSizeHeader *hdr = (QemuProtocolContentSizeHeader *)data;
  if (len != sizeof(*hdr)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet length");
    return;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Got content size msg: size: %d", hdr->size);
#if !RECOVERY_FW
  system_theme_set_content_size(hdr->size);

  // Exit out of any currently running app so we force the UI to update to the new content size
  // (must be called from the KernelMain task)
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  app_manager_close_current_app(true /* gracefully */);
#endif
}


// -----------------------------------------------------------------------------------------
// List of incoming message handlers
static const QemuMessageHandler s_qemu_endpoints[] = {
  // IMPORTANT: These must be in sorted order!!
  { QemuProtocol_SPP, qemu_transport_handle_received_data },
  { QemuProtocol_Tap, prv_tap_msg_callback },
  { QemuProtocol_BluetoothConnection, prv_bluetooth_connection_msg_callback },
  { QemuProtocol_Compass, prv_compass_msg_callback },
  { QemuProtocol_Battery, qemu_battery_msg_callback },
  { QemuProtocol_Accel, qemu_accel_msg_callback },
  { QemuProtocol_TimeFormat, prv_time_format_msg_callback },
  { QemuProtocol_TimelinePeek, prv_timeline_peek_msg_callback },
  { QemuProtocol_ContentSize, prv_content_size_msg_callback },
  // Button messages are handled by QEMU directly
};


// -----------------------------------------------------------------------------------------
// Find handler from s_qemu_endpoints for a given protocol
static const QemuMessageHandler* prv_find_handler(uint16_t protocol_id) {
  for (size_t i = 0; i < ARRAY_LENGTH(s_qemu_endpoints); ++i) {
    const QemuMessageHandler* handler = &s_qemu_endpoints[i];
    if (!handler || handler->protocol_id > protocol_id) {
      break;
    }

    if (handler->protocol_id == protocol_id) {
      return handler;
    }
  }

  return NULL;
}


// -----------------------------------------------------------------------------------------
void qemu_serial_init(void) {
  // Init our state variables
  qemu_serial_private_init_state(&s_qemu_state);

  // Init the UART
  uart_init(QEMU_UART);
  uart_set_baud_rate(QEMU_UART, UART_SERIAL_BAUD_RATE);
  uart_set_rx_interrupt_handler(QEMU_UART, prv_uart_irq_handler);

  // enable the UART RX interrupt
  uart_set_rx_interrupt_enabled(QEMU_UART, true);
}


// -----------------------------------------------------------------------------------------
// KernelMain callback triggered by our ISR handler when we detect a high water mark on our
//  receive buffer or a footer signature
static void prv_process_receive_buffer(void *context) {
  uint32_t msg_bytes;
  uint16_t protocol;

  // Process ISR receive buffer, see if we have a complete message
  // Prevent our ISR from putting more characters in while we muck with the receive buffer by
  //  disabling UART interrupts while we process it.
  while (true) {
    uart_set_rx_interrupt_enabled(QEMU_UART, false);
    uint8_t *msg_ptr = qemu_serial_private_assemble_message(&s_qemu_state, &msg_bytes, &protocol);
    uart_set_rx_interrupt_enabled(QEMU_UART, true);
    if (!msg_ptr) {
      break;
    }

    // Dispatch the received message
    PBL_LOG(LOG_LEVEL_DEBUG, "Dispatching msg of len %"PRIu32" for protocol %d", msg_bytes,
              protocol);
    const QemuMessageHandler* handler = prv_find_handler(protocol);
    if (!handler) {
      PBL_LOG(LOG_LEVEL_WARNING, "No handler for protocol: %d", protocol);
    } else {
      handler->callback(msg_ptr, msg_bytes);
    }
  }
}


// -----------------------------------------------------------------------------------------
static bool prv_uart_irq_handler(UARTDevice *dev, uint8_t byte, const UARTRXErrorFlags *err_flags) {
  // The interrupt triggers when a byte has been read from the UART. QEMU's
  // emulated UARTs don't emulate receive overruns by default so we don't have
  // to worry about that case. QEMU will buffer the data stream until we're
  // ready to consume more data by reading from the UART again.
  PBL_ASSERTN(!err_flags->overrun_error);

  // Add to circular buffer. It's safe to assume that the buffer has space
  // remaining as the RX interrupt will be disabled from the time the buffer
  // fills up until when the buffer is drained.
  bool success = shared_circular_buffer_write(&s_qemu_state.isr_buffer, &byte, 1,
                                              false/*advance_slackers*/);
  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "ISR buf too small 0x%x", byte);
    s_qemu_state.recv_error_count++;
  }

  bool buffer_full = false;
  if (!shared_circular_buffer_get_write_space_remaining(&s_qemu_state.isr_buffer)) {
    // There's no more room in the buffer, so disable the RX interrupt. No more
    // data will be read from the UART until prv_process_receive_buffer() is
    // run, draining the buffer and re-enabling the RX interrupt. QEMU will
    // buffer the remaining data until the interrupt is re-enabled.
    uart_set_rx_interrupt_enabled(dev, false);
    buffer_full = true;
  }

  // Is it time to wake up the main thread?
  bool should_context_switch = false;
  if (s_qemu_state.recv_error_count || buffer_full ||
      (byte == QEMU_FOOTER_LSB && s_qemu_state.prev_byte == QEMU_FOOTER_MSB)) {
    if (!s_qemu_state.callback_pending) {
      s_qemu_state.callback_pending = true;
      PebbleEvent e = {
        .type = PEBBLE_CALLBACK_EVENT,
        .callback = {
          .callback = prv_process_receive_buffer,
          .data = NULL
        }
      };
      should_context_switch = event_put_isr(&e);
    }
  }

  s_qemu_state.prev_byte = byte;

  return should_context_switch;
}


// -----------------------------------------------------------------------------------------
static void prv_send(const uint8_t *data, uint32_t len) {
  QEMU_LOG_DEBUG("Sending data:");
  QEMU_HEXDUMP(data, len);

  while (len--) {
    uart_write_byte(QEMU_UART, *data++);
  }
  uart_wait_for_tx_complete(QEMU_UART);
}


// -----------------------------------------------------------------------------------------
void qemu_serial_send(QemuProtocol protocol, const uint8_t *data, uint32_t len) {
  if (!s_qemu_state.initialized) {
    return;
  }

  mutex_lock(s_qemu_state.qemu_comm_lock);

  // Send the header
  QemuCommChannelHdr hdr = (QemuCommChannelHdr) {
    .signature = htons(QEMU_HEADER_SIGNATURE),
    .protocol = htons(protocol),
    .len = htons(len)
  };
  prv_send((uint8_t *)&hdr, sizeof(hdr));

  // Send the data
  prv_send(data, len);

  // Send the footer
  QemuCommChannelFooter footer = (QemuCommChannelFooter) {
    .signature = htons(QEMU_FOOTER_SIGNATURE)
  };
  prv_send((uint8_t *)&footer, sizeof(footer));

  mutex_unlock(s_qemu_state.qemu_comm_lock);
}
