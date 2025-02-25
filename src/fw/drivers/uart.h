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

#pragma once

#include "board/board.h"

#include <stdbool.h>
#include <stdint.h>

typedef const struct UARTDevice UARTDevice;

typedef struct UARTRXErrorFlags {
  union {
    struct {
      uint8_t reserved:4;
      bool parity_error:1;
      bool overrun_error:1;
      bool framing_error:1;
      bool noise_detected:1;
    };
    uint8_t error_mask;
  };
} UARTRXErrorFlags;

//! The type of function which can be called from within the UART ISR (@see \Ref
//! uart_set_*_interrupt_handler)
//! @return Whether or not the ISR should context switch at the end instead of resuming the previous
//! task (@see \Ref portEND_SWITCHING_ISR)
typedef bool (*UARTRXInterruptHandler)(UARTDevice *dev, uint8_t data,
                                       const UARTRXErrorFlags *err_flags);
typedef bool (*UARTTXInterruptHandler)(UARTDevice *dev);

//! Initializes the device
void uart_init(UARTDevice *dev);

//! Initializes the device with open-drain pins instead of push-pull
void uart_init_open_drain(UARTDevice *dev);

//! Same as uart_init() but only enables the TX UART
void uart_init_tx_only(UARTDevice *dev);

//! Same as uart_init() but only enables the RX UART
void uart_init_rx_only(UARTDevice *dev);

//! Deinitializes the device
void uart_deinit(UARTDevice *dev);

//! Sets the baud rate of the device
void uart_set_baud_rate(UARTDevice *dev, uint32_t baud_rate);

//! Sets a receive IRQ handler for the device which is called whenever we receive a byte (within an
//! ISR)
//! @note This cannot be set at the same time as a raw interrupt handler
void uart_set_rx_interrupt_handler(UARTDevice *dev, UARTRXInterruptHandler irq_handler);

//! Sets a transmit IRQ handler for the device which is called whenever we send a byte (within an
//! ISR)
//! @note This cannot be set at the same time as a raw interrupt handler
void uart_set_tx_interrupt_handler(UARTDevice *dev, UARTTXInterruptHandler irq_handler);

//! Sets whether or not receive/transmit interrupts are enabled
void uart_set_rx_interrupt_enabled(UARTDevice *dev, bool enabled);
void uart_set_tx_interrupt_enabled(UARTDevice *dev, bool enabled);

//! Writes a byte to the UART device
//! @note This will block until the transmit buffer is clear if necessary
void uart_write_byte(UARTDevice *dev, uint8_t data);

//! Reads a byte from the UART device
//! @note This will cause error flags (framing / overrun) to be cleared
//! @param[in] dev The UART device to read from
//! @return The read byte
uint8_t uart_read_byte(UARTDevice *dev);

//! Starts the use of DMA for receiving (the DMARequest must be configured)
//! @param[in] dev The UART device
void uart_start_rx_dma(UARTDevice *dev, void *buffer, uint32_t length);

//! Stops the use of DMA for receiving (the DMARequest must be configured)
//! @param[in] dev The UART device
void uart_stop_rx_dma(UARTDevice *dev);

//! Discards any pending data in the RX DMA buffer
void uart_clear_rx_dma_buffer(UARTDevice *dev);

//! Returns whether or not the peripheral has a byte ready to be read
bool uart_is_rx_ready(UARTDevice *dev);

//! Returns whether or not the peripheral has a byte ready to be read
//! @note This should be called before reading from the RX buffer as doing so will clear this flag
bool uart_has_rx_overrun(UARTDevice *dev);

//! Returns whether or not the peripheral has a byte ready to be read
//! @note This should be called before reading from the RX buffer as doing so will clear this flag
bool uart_has_rx_framing_error(UARTDevice *dev);

//! Returns whether or not the peripheral is ready to send a byte
bool uart_is_tx_ready(UARTDevice *dev);

//! Returns whether or not the peripheral has finished sending the last byte
bool uart_is_tx_complete(UARTDevice *dev);

//! Waits for the current transmit to complete
void uart_wait_for_tx_complete(UARTDevice *dev);

//! Checks to see if any errors are pended on the UART. Returns a non-zero
//! if error_mask if an error has occurred.
UARTRXErrorFlags uart_has_errored_out(UARTDevice *dev);

//! Clears all interrupt flags
//! @param[in] dev The UART device
void uart_clear_all_interrupt_flags(UARTDevice *dev);
