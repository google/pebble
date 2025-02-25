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

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "services/common/system_task.h"

//! Different speeds we support running the accessory connector at.
//!
//! @internal
//! Please keep this enum in order from lowest speed to highest.
typedef enum {
  AccessoryBaud9600,
  AccessoryBaud14400,
  AccessoryBaud19200,
  AccessoryBaud28800,
  AccessoryBaud38400,
  AccessoryBaud57600,
  AccessoryBaud62500,
  AccessoryBaud115200,
  AccessoryBaud125000,
  AccessoryBaud230400,
  AccessoryBaud250000,
  AccessoryBaud460800,
  AccessoryBaud921600,

  AccessoryBaudInvalid
} AccessoryBaud;

//! The type of function used for ISR-based sending via accessory_send_stream(). This function MUST
//! send a single byte by calling accessory_send_byte() and/or return false to indicate that there
//! is no more data to be sent.
typedef bool (*AccessoryDataStreamCallback)(void *context);

//! Initialize the accessory driver
void accessory_init(void);

//! Blocks the accessory port from being used
void accessory_block(void);

//! Unblocks the accessory port and allows it to be used
void accessory_unblock(void);

//! Enable power output on the accessory connector.
void accessory_set_power(bool on);

//! Send a single byte synchronously out the accessory connector. Input must be disabled before
//! calling this function.
void accessory_send_byte(uint8_t data);

//! Send data synchronously out the accessory connector. Will return once all data has been sent.
void accessory_send_data(const uint8_t *data, size_t length);

//! Sends data using ISRs by calling the provided function to send the next byte until the stream
//! callback returns false to indicate sending is complete or bus contention is detected
bool accessory_send_stream(AccessoryDataStreamCallback stream_callback, void *context);

//! Stops any ISR-based sending which is in progress
void accessory_send_stream_stop(void);

//! Stop the driver from reading any input on the accessory port. When input is disabled we can
//! write out the accessory port at higher rates as we don't have to worry about suppressing
//! reading back our own output.
void accessory_disable_input(void);

//! Allow the driver to start receiving input again. Only valid after calling
//! accessory_disable_input.
void accessory_enable_input(void);

//! Set the baudrate
void accessory_set_baudrate(AccessoryBaud baud_select);

//! Called from the accessory UART interrupt. The manager is responsible for implementing this
//! function.
//! @return whether we need to trigger a context switch based on handling this character
bool accessory_manager_handle_character_from_isr(char c);

//! Called from the accessory UART interrupt. The manager is responsible for implementing this
//! function.
//! @return whether we need to trigger a context switch based on handling this character
bool accessory_manager_handle_break_from_isr(void);

//! Returns whether or not there has been bus contention detected since accessory_disable_input()
//! was last called.
bool accessory_bus_contention_detected(void);

//! Checks if the pull-up resistor which is required for smartstraps is present
bool accessory_is_present(void);

//! Uses DMA for receiving from the peripheral
void accessory_use_dma(bool use_dma);
