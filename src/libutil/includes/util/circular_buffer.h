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
#include <stddef.h>
#include <stdint.h>


//! The circular buffer is a contiguous buffer of a fixed length where data is written and consumed
//! in a circular fashion. When the write ends up writing past the end of the buffer, it wraps
//! around to use the first part of the buffer again, assuming that someone else has consumed some
//! data to free it up.
typedef struct CircularBuffer {
  uint8_t* buffer;
  bool write_in_progress;
  bool auto_reset;
  uint16_t buffer_size;
  uint16_t read_index; //! What offset in buffer that we should read from next.
  uint16_t data_length; //! How many bytes after read_index contain valid data.
} CircularBuffer;

void circular_buffer_init(CircularBuffer* buffer, uint8_t* storage, uint16_t storage_size);

//! Extended _init() -- provides access to auto_reset flag.
//! If auto_reset is true (default), on _consume(), read/write pointers will be reset in an attempt
//! to reduce buffer wraps. If false, the buffer will always wrap, leaving the previous data
//! in the buffer. This is handy for post mortem evaluation of debug logs, etc.
void circular_buffer_init_ex(CircularBuffer* buffer, uint8_t* storage, uint16_t storage_size,
                             bool auto_reset);

//! Copies data from a given buffer and length into the circular buffer.
//! @return false if there's insufficient space.
bool circular_buffer_write(CircularBuffer* buffer, const void* data, uint16_t length);

//! Gets a pointer into the circular buffer where the caller itself can write data to.
//! @note After the client is done writing data, it _must_ call circular_buffer_write_finish()
//! so that the CircularBuffer can update the length of the data it contains and update its internal
//! bookkeeping
//! @param[out] data_out Pointer to storage for the pointer that is set the the start of the
//! writable area when the function returns, or NULL if there is no space available.
//! @return The maximum number of bytes that can be written starting at the returned the pointer.
//! Zero is returned when there is no space available.
uint16_t circular_buffer_write_prepare(CircularBuffer *buffer, uint8_t **data_out);

//! To be used after circular_buffer_write_prepare(), to make the CircularBuffer update the length
//! of the data it contains.
//! @param written_length The length that has just been writted at the pointer provided by
//! circular_buffer_write_prepare().
void circular_buffer_write_finish(CircularBuffer *buffer, uint16_t written_length);

//! Read a contiguous chunk of memory from the circular buffer. The data remains on the buffer until
//! circular_buffer_consume is called.
//!
//! If the circular buffer wraps in the middle of the requested data, this function call will return true but will
//! provide fewer bytes that requested. When this happens, the length_out parameter will be set to a value smaller
//! than length. A second read call can be made with the remaining smaller length to retreive the rest.
//!
//! The reason this read doesn't consume is to avoid having to copy out the data. The data_out pointer should be
//! stable until you explicitly ask for it to be consumed with circular_buffer_consume.
//!
//! @param buffer The buffer to read from
//! @param length How many bytes to read
//! @param[out] data_out The bytes that were read.
//! @param[out] length_out How many bytes were read.
//! @return false if there's less than length bytes in the buffer.
bool circular_buffer_read(const CircularBuffer* buffer, uint16_t length, const uint8_t** data_out, uint16_t* length_out);

//! Same as circular_buffer_copy_offset(), but with a starting offset of zero.
uint16_t circular_buffer_copy(const CircularBuffer* buffer, void *data_out, uint16_t length);

//! Copy a number of bytes from the circular buffer into another (contiguous)
//! buffer. The function takes care of any wrapping of the circular buffer.
//! The data remains on the circular buffer until circular_buffer_consume is
//! called.
//! @param buffer The circular buffer to copy from.
//! @param start_offset Number of bytes of the source content to skip.
//! @param[out] data_out The buffer to copy the data to.
//! @param length The number of bytes to copy.
//! @return The number of bytes copied.
uint16_t circular_buffer_copy_offset(const CircularBuffer* buffer, uint16_t start_offset,
                                     uint8_t *data_out, uint16_t length);

//! Gets a pointer to a continuous byte array of the requested length from the circular buffer.
//! In case the requested length wraps around the edges of the circular buffer, a heap-allocated
//! copy is made.
//! @param buffer The buffer to read or copy from.
//! @param[out] data_ptr_out After returning, it will point to the byte array with the data. NOTE:
//! This can be NULL in the case the malloc_imp wasn't able to allocate the buffer.
//! @param[in] length The length of the data to read. If this is longer than what
//! circular_buffer_get_read_space_remaining() returns, the function will return false.
//! @param[in] malloc_imp The malloc() implementation to allocate a new buffer, in case if the
//! requested length is non-contiguously stored.
//! @param[out] caller_should_free Will be true after returning if the caller must call
//! free(*data_ptr_out) at some point, because the data had been copied into a temporary
//! heap-allocated buffer.
//! @return false if there's less than length bytes in the buffer or if the copy failed because
//! there was not enough memory.
bool circular_buffer_read_or_copy(const CircularBuffer* buffer, uint8_t **data_out, size_t length,
                                  void *(*malloc_imp)(size_t), bool *caller_should_free);

//! Removes length bytes of the oldest data from the buffer.
bool circular_buffer_consume(CircularBuffer* buffer, uint16_t length);

//! @return The number of bytes we can write before circular_buffer_write will return false.
uint16_t circular_buffer_get_write_space_remaining(const CircularBuffer* buffer);

//! @return The number of bytes we can read before circular_buffer_read will return false.
uint16_t circular_buffer_get_read_space_remaining(const CircularBuffer* buffer);

