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

#include "services/common/comm_session/session.h"

typedef struct SendBuffer SendBuffer;

//! @return The maximum number of bytes that a client can copy into a CommSessionSendBuffer, or
//! zero if the session is invalid (e.g. disconnected in the mean time).
size_t comm_session_send_buffer_get_max_payload_length(const CommSession *session);

//! Creates a kernel-heap allocated buffer for outbound messages.
//! This will block if the required space is not yet available.
//! If you want to avoid the allocation on the kernel-heap, use comm_session_send_queue_add_job()
//! directly.
//! @see comm_session_send_data for a simpler, one-liner interface to send data.
//! @note Remember to call comm_session_send_buffer_end_write when you're done.
//! @note bt_lock() MUST NOT be held when making the call. For ..._write and ..._end_write it
//! is fine if bt_lock() is held.
//! @param session The session to which the message should be sent.
//! @param endpoint_id The Pebble Protocol endpoint ID to send the message to.
//! @param required_free_length The number of bytes of free space the caller needs at minimum. Once
//! the function returns with `true`, the amount of space (or more) is guaranteed to be available.
//! @param timeout_ms The maximum duration to wait for the send buffer to become available with the
//! required number of bytes of free space.
//! @return True if the "writer access" was successfully acquired, false otherwise.
SendBuffer * comm_session_send_buffer_begin_write(CommSession *session, uint16_t endpoint_id,
                                                  size_t required_free_length,
                                                  uint32_t timeout_ms);

//! Copies data into the send buffer of the session.
//! @note The caller must have called comm_session_send_buffer_begin_write() first.
//! @note bt_lock() may be held when making the call.
//! @param session The session for which to enqueue data
//! @param data Pointer to the data to enqueue
//! @param length Length of the data to enqueue
//! @return true if the data was successfully queued up for sending, or false if there was not
//! enough space left in the send buffer to enqueue the data. Note that the `required_free_length`
//! as passed into comm_session_send_buffer_begin_write() is guaranteed. Nonetheless, callers can
//! try to stash in more data than `required_free_length` but will need to handle the return value
//! of this function when it does attempt to write more than `required_free_length`.
bool comm_session_send_buffer_write(SendBuffer *send_buffer, const uint8_t *data, size_t length);

//! Finish writing to the send buffer. Any enqueued data will be transmitted after this call,
//! to the session that was passed in the ..._begin_write() call.
//! @note The caller must have called comm_session_send_buffer_begin_write() first.
//! @note bt_lock() may be held when making the call.
//! @param session The session for which to release the send buffer.
void comm_session_send_buffer_end_write(SendBuffer *send_buffer);
