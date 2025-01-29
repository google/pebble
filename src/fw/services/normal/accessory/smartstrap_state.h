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

/*
 * FSM state transitions:
 * +------------------+------------------+--------------+---------------------------+
 * | From State       | To State         | Task         | Event                     |
 * +------------------+------------------+--------------+---------------------------+
 * | Unsubscribed     | ReadReady        | KernelBG     | First subscriber          |
 * | ReadReady        | NotifyInProgress | ISR          | Break character received  |
 * | ReadReady        | ReadDisabled     | KernelBG     | Send started              |
 * | NotifyInProgress | ReadComplete     | ISR          | Complete frame received   |
 * | NotifyInProgress | ReadComplete     | NewTimer     | Read timeout              |
 * | ReadDisabled     | ReadInProgress   | KernelBG,ISR | Send completed (is_read)  |
 * | ReadDisabled     | ReadReady        | KernelBG     | Send completed (!is_read) |
 * | ReadDisabled     | ReadReady        | KernelBG     | Send failed               |
 * | ReadInProgress   | ReadComplete     | ISR          | Complete frame received   |
 * | ReadInProgress   | ReadComplete     | NewTimer     | Read timeout              |
 * | ReadComplete     | ReadReady        | KernelBG     | Frame processed           |
 * | *ANY STATE*      | Unsubscribed     | KernelBG     | No more subscribers       |
 * +------------------+------------------+--------------+---------------------------+
 * Notes:
 * - Only KernelBG can send frames when s_is_connected == false
 * - Transitions which can take place from "Any" task are not allowed from ISRs
 * - Received data is ignored in any state except ReadInProgress or NotifyInProgress
 * - Break characters are ignored in any state except ReadReady
 * - We can only start sending data after a successful transition from ReadReady to ReadDisabled
 */
typedef enum {
  SmartstrapStateUnsubscribed,
  SmartstrapStateReadReady,
  SmartstrapStateNotifyInProgress,
  SmartstrapStateReadDisabled,
  SmartstrapStateReadInProgress,
  SmartstrapStateReadComplete
} SmartstrapState;

//! Initialize the smartstrap state
void smartstrap_state_init(void);

//! Attempt to transition from expected_state to next_state and returns whether or not we
//! transitioned successfully. This transition is done atomically.
bool smartstrap_fsm_state_test_and_set(SmartstrapState expected_state, SmartstrapState next_state);

//! Sets the FSM state, regardless of what the current state is.
//! @note The caller must ensure that there can be no other task or an ISR trying to access or
//! change the state at the same time. If there is a possibility for contention, the caller should use
//! prv_fsm_state_test_and_set instead or enter a critical region.
void smartstrap_fsm_state_set(SmartstrapState next_state);

//! Change the FSM state to ReadReady without doing any assertions. Should only be used with great
//! care and from a critical region (such as within smartstrap_send_cancel).
void smartstrap_fsm_state_reset(void);

//! Returns the current FSM state
SmartstrapState smartstrap_fsm_state_get(void);

//! Returns whether or not we're connected to a smartstrap
bool smartstrap_is_connected(void);

//! Acquires the smartstrap lock
void smartstrap_state_lock(void);

//! Releases the smartstrap lock
void smartstrap_state_unlock(void);

//! Asserts that the current task has acquired the state lock
void smartstrap_state_assert_locked_by_current_task(void);

//! Set whether or not the specified service is currently connected
void smartstrap_connection_state_set_by_service(uint16_t service_id, bool connected);

//! Set whether or not we are connected to a smartstrap
void smartstrap_connection_state_set(bool connected);


// syscalls

//! Returns whether or not the specified service is available on a connected smartstrap
bool sys_smartstrap_is_service_connected(uint16_t service_id);
