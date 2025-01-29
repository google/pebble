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

#include "util/circular_buffer.h"

#include "util/assert.h"
#include "util/math.h"

#include <string.h>

static uint16_t get_write_length_available(CircularBuffer* buffer) {
  return buffer->buffer_size - buffer->data_length;
}

static uint16_t get_write_index(CircularBuffer* buffer) {
  return (buffer->read_index + buffer->data_length) % buffer->buffer_size;
}

void circular_buffer_init(CircularBuffer* buffer, uint8_t* storage, uint16_t storage_size) {
  buffer->buffer = storage;
  buffer->buffer_size = storage_size;
  buffer->read_index = 0;
  buffer->data_length = 0;
  buffer->write_in_progress = false;
  buffer->auto_reset = true;
}

void circular_buffer_init_ex(CircularBuffer* buffer, uint8_t* storage, uint16_t storage_size,
                             bool auto_reset) {
  circular_buffer_init(buffer, storage, storage_size);
  buffer->auto_reset = auto_reset;
}

bool circular_buffer_write(CircularBuffer* buffer, const void* data, uint16_t length) {
  if (get_write_length_available(buffer) < length) {
    return false;
  }

  // Now we know the message will fit, so no more checking against buffer_read_index required.

  uint16_t write_index = get_write_index(buffer);

  // Update the data_length member now before we muck with the length parameter
  buffer->data_length += length;

  const uint16_t remaining_length = buffer->buffer_size - write_index;
  if (remaining_length < length) {
    // Need to write the message in two chunks around the end of the buffer. Write the first chunk
    memcpy(&buffer->buffer[write_index], data, remaining_length);

    write_index = 0;
    data = ((uint8_t*) data) + remaining_length;
    length -= remaining_length;
  }

  // Write the last chunk
  memcpy(&buffer->buffer[write_index], data, length);
  return true;
}

uint16_t circular_buffer_write_prepare(CircularBuffer *buffer, uint8_t **data_out) {
  if (!get_write_length_available(buffer) || buffer->write_in_progress) {
    *data_out = NULL;
    return 0;
  }
  buffer->write_in_progress = true;

  const uint16_t write_index = get_write_index(buffer);
  *data_out = buffer->buffer + write_index;
  if (buffer->read_index > write_index) {
    return buffer->read_index - write_index;
  }
  return buffer->buffer_size - write_index;
}

void circular_buffer_write_finish(CircularBuffer *buffer, uint16_t written_length) {
  UTIL_ASSERT(buffer->data_length + written_length <= buffer->buffer_size);
  buffer->data_length += written_length;
  buffer->write_in_progress = false;
}

bool circular_buffer_read(const CircularBuffer* buffer, uint16_t length, const uint8_t** data_out, uint16_t* length_out) {
  if (buffer->data_length < length) {
    return false;
  }

  *data_out = &buffer->buffer[buffer->read_index];

  const uint16_t remaining_length = buffer->buffer_size - buffer->read_index;
  *length_out = MIN(remaining_length, length);

  return true;
}

uint16_t circular_buffer_copy(const CircularBuffer* buffer,
                                      void *data_out,
                                      uint16_t length_to_copy) {
  return circular_buffer_copy_offset(buffer, 0, data_out, length_to_copy);
}

uint16_t circular_buffer_copy_offset(const CircularBuffer* buffer, uint16_t start_offset,
                                     uint8_t *data_out, uint16_t length_to_copy) {
  if (buffer->data_length <= start_offset) {
    return 0;
  }
  const uint16_t read_index = (buffer->read_index + start_offset) % buffer->buffer_size;
  const uint16_t data_length = buffer->data_length - start_offset;

  length_to_copy = MIN(length_to_copy, data_length);

  // Number of total bytes after the read index to end of buffer:
  const uint16_t total_length_to_end = buffer->buffer_size - read_index;
  // Number of bytes of after the read index that need to be copied:
  const uint16_t end_copy_length = MIN(total_length_to_end, length_to_copy);
  memcpy(data_out, &buffer->buffer[read_index], end_copy_length);

  // If length_to_copy > total_length_to_end, there is more data at the
  // beginning of the circular buffer (wrapped around):
  const uint16_t wrapped_length = length_to_copy - end_copy_length;
  if (wrapped_length) {
    memcpy(data_out + total_length_to_end, buffer->buffer, wrapped_length);
  }

  return length_to_copy;
}

bool circular_buffer_read_or_copy(const CircularBuffer* buffer, uint8_t **data_out, size_t length,
                                  void *(*malloc_imp)(size_t), bool *caller_should_free) {
  UTIL_ASSERT(buffer && malloc_imp && data_out && caller_should_free);
  if (buffer->data_length < length) {
    return false;
  }
  const uint16_t contiguous_length = (buffer->buffer_size - buffer->read_index);
  const bool should_malloc_and_copy = (length > contiguous_length);
  *caller_should_free = should_malloc_and_copy;
  if (should_malloc_and_copy) {
    *data_out = (uint8_t *) malloc_imp(length);
    if (*data_out) {
      circular_buffer_copy(buffer, *data_out, length);
    } else {
      *caller_should_free = false;
      return false;
    }
  } else {
    uint16_t length_out;
    circular_buffer_read(buffer, length, (const uint8_t **)data_out, &length_out);
  }
  return true;
}

bool circular_buffer_consume(CircularBuffer* buffer, uint16_t length) {
  if (buffer->data_length < length) {
    return false;
  }

  buffer->read_index = (buffer->read_index + length) % buffer->buffer_size;
  buffer->data_length -= length;

  // Reset read_index if there's no more data, so any newly written data won't wrap
  if (buffer->auto_reset && buffer->data_length == 0 && !buffer->write_in_progress) {
    buffer->read_index = 0;
  }

  return true;
}

uint16_t circular_buffer_get_write_space_remaining(const CircularBuffer* buffer) {
  return buffer->buffer_size - buffer->data_length;
}

uint16_t circular_buffer_get_read_space_remaining(const CircularBuffer* buffer) {
  return buffer->data_length;
}
