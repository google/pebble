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

#include "shared_circular_buffer.h"

#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"

#include <string.h>


// -------------------------------------------------------------------------------------------------
// Returns the amount of data available for the given client
static uint32_t prv_get_data_length(const SharedCircularBuffer* buffer, SharedCircularBufferClient *client) {

  uint32_t  len;

  // The end_index is the index of the next byte to go into the buffer
  // The read_index is the index of the first byte
  // An empty buffer is when end_index == read_index
  if (buffer->write_index >= client->read_index) {
    len = buffer->write_index - client->read_index;
  } else {
    len = buffer->buffer_size - client->read_index;
    len += buffer->write_index;
  }

  return len;
}


// -------------------------------------------------------------------------------------------------
// Returns max amount of data available among all clients
// On exit, *max_client will contain the client with the most amount of data available
static uint32_t prv_get_max_data_length(const SharedCircularBuffer* buffer, SharedCircularBufferClient **max_client) {
  ListNode *iter = buffer->clients;
  if (!iter) {
    *max_client = NULL;
    return 0;
  }

  uint32_t max_data = 0;
  uint32_t len;
  while (iter) {
    len = prv_get_data_length(buffer, (SharedCircularBufferClient *)iter);
    if (len >= max_data) {
      *max_client = (SharedCircularBufferClient *)iter;
      max_data = len;
    }

    iter = iter->next;
  }
  return max_data;
}


// -------------------------------------------------------------------------------------------------
void shared_circular_buffer_init(SharedCircularBuffer* buffer, uint8_t* storage, uint16_t storage_size) {
  buffer->buffer = storage;
  buffer->buffer_size = storage_size;
  buffer->clients = NULL;
  buffer->write_index = 0;
}


// -------------------------------------------------------------------------------------------------
bool shared_circular_buffer_add_client(SharedCircularBuffer* buffer, SharedCircularBufferClient *client) {
  PBL_ASSERTN(!list_contains(buffer->clients, &client->list_node));
  buffer->clients = list_prepend(buffer->clients, &client->list_node);
  client->read_index = buffer->write_index;
  return true;
}


// -------------------------------------------------------------------------------------------------
void shared_circular_buffer_remove_client(SharedCircularBuffer* buffer, SharedCircularBufferClient *client) {
  PBL_ASSERTN(list_contains(buffer->clients, &client->list_node));
  list_remove(&client->list_node, &buffer->clients, NULL);
}


// -------------------------------------------------------------------------------------------------
bool shared_circular_buffer_write(SharedCircularBuffer* buffer, const uint8_t* data, uint16_t length,
      bool advance_slackers) {

  // If no clients, no need to write
  if (!buffer->clients) {
    PBL_LOG(LOG_LEVEL_WARNING, "no readers");
    return false;
  }

  // If this is bigger than the queue would allow, error
  if (length >= buffer->buffer_size) {
    return false;
  }

  // Make sure there's room, deleting bytes from slackers if requested
  SharedCircularBufferClient *slacker;
  uint32_t max_data = prv_get_max_data_length(buffer, &slacker);
  uint32_t avail_space = buffer->buffer_size - 1 - max_data;
  while (length > avail_space) {
    if (!advance_slackers) {
      return false;
    }

    // Delete data from the biggest slacker
    slacker->read_index = buffer->write_index;

    max_data = prv_get_max_data_length(buffer, &slacker);
    avail_space = buffer->buffer_size - max_data;
  }

  const uint16_t remaining_length = buffer->buffer_size - buffer->write_index;
  if (remaining_length < length) {
    // Need to write the message in two chunks around the end of the buffer. Write the first chunk
    memcpy(&buffer->buffer[buffer->write_index], data, remaining_length);

    buffer->write_index = 0;
    data += remaining_length;
    length -= remaining_length;
  }

  // Write the last chunk
  memcpy(&buffer->buffer[buffer->write_index], data, length);
  buffer->write_index = (buffer->write_index + length) % buffer->buffer_size;
  return true;
}


// ---------------------------------------------------------------------------------------------
bool shared_circular_buffer_read(const SharedCircularBuffer* buffer,
                                 SharedCircularBufferClient *client, uint16_t length,
                                 const uint8_t** data_out, uint16_t* length_out) {
  PBL_ASSERTN(list_contains(buffer->clients, &client->list_node));

  uint32_t data_length = prv_get_data_length(buffer, client);
  if (data_length < length) {
    return false;
  }

  *data_out = &buffer->buffer[client->read_index];

  const uint16_t bytes_read = buffer->buffer_size - client->read_index;
  *length_out = MIN(bytes_read, length);

  return true;
}


// -------------------------------------------------------------------------------------------------
bool shared_circular_buffer_consume(SharedCircularBuffer* buffer, SharedCircularBufferClient *client, uint16_t length) {
  PBL_ASSERTN(list_contains(buffer->clients, &client->list_node));
  uint32_t data_length = prv_get_data_length(buffer, client);
  if (data_length < length) {
    return false;
  }

  client->read_index = (client->read_index + length) % buffer->buffer_size;
  return true;
}


// -------------------------------------------------------------------------------------------------
uint16_t shared_circular_buffer_get_write_space_remaining(const SharedCircularBuffer* buffer) {
  SharedCircularBufferClient *slacker;
  uint32_t max_data = prv_get_max_data_length(buffer, &slacker);
  return  buffer->buffer_size - 1 - max_data;
}


// -------------------------------------------------------------------------------------------------
uint16_t shared_circular_buffer_get_read_space_remaining(const SharedCircularBuffer* buffer,
      SharedCircularBufferClient *client) {
  PBL_ASSERTN(list_contains(buffer->clients, &client->list_node));
  return prv_get_data_length(buffer, client);
}


// -------------------------------------------------------------------------------------------------
bool shared_circular_buffer_read_consume(SharedCircularBuffer *buffer, SharedCircularBufferClient *client,
                          uint16_t length, uint8_t *data, uint16_t *length_out) {

  uint16_t data_length = prv_get_data_length(buffer, client);
  uint16_t bytes_left = MIN(length, data_length);

  *length_out = bytes_left;
  while (bytes_left) {
    uint16_t chunk;
    const uint8_t *read_ptr;

    shared_circular_buffer_read(buffer, client, bytes_left, &read_ptr, &chunk);
    memcpy(data, read_ptr, chunk);
    shared_circular_buffer_consume(buffer, client, chunk);

    data += chunk;
    bytes_left -= chunk;
  }

  return (*length_out == length);
}


// -------------------------------------------------------------------------------------------------
void shared_circular_buffer_add_subsampled_client(
    SharedCircularBuffer *buffer, SubsampledSharedCircularBufferClient *client,
    uint16_t subsample_numerator, uint16_t subsample_denominator) {
  PBL_ASSERTN(shared_circular_buffer_add_client(buffer,
                                                &client->buffer_client));
  subsampled_shared_circular_buffer_client_set_ratio(
      client, subsample_numerator, subsample_denominator);
}

// -------------------------------------------------------------------------------------------------
void shared_circular_buffer_remove_subsampled_client(
    SharedCircularBuffer *buffer,
    SubsampledSharedCircularBufferClient *client) {
  shared_circular_buffer_remove_client(buffer, &client->buffer_client);
}

// -------------------------------------------------------------------------------------------------
void subsampled_shared_circular_buffer_client_set_ratio(
    SubsampledSharedCircularBufferClient *client,
    uint16_t numerator, uint16_t denominator) {
  PBL_ASSERTN(numerator > 0 && denominator >= numerator);
  if (client->numerator != numerator || client->denominator != denominator) {
    // The subsampling algorithm does not need the subsampling ratio to
    // be normalied to reduced form.
    client->numerator = numerator;
    client->denominator = denominator;
    // Initialize the state so that the next item in the buffer is copied,
    // not discarded.
    client->subsample_state = denominator - numerator;
  }
}

// -------------------------------------------------------------------------------------------------
size_t shared_circular_buffer_read_subsampled(
    SharedCircularBuffer* buffer,
    SubsampledSharedCircularBufferClient *client,
    size_t item_size, void *data, uint16_t num_items) {
  uint16_t bytes_available = prv_get_data_length(
      buffer, &client->buffer_client);

  // Optimized case when no subsampling
  if (client->numerator == client->denominator) {
    num_items = MIN(num_items, bytes_available / item_size);
    uint16_t bytes_out;
    shared_circular_buffer_read_consume(
        buffer, &client->buffer_client, num_items * item_size,
        (uint8_t *)data, &bytes_out);
    PBL_ASSERTN(bytes_out == num_items * item_size);
    return num_items;
  }

  // An interesting property of the subsampling algorithm used is that
  // the subsampling ratio does not need to be in reduced form. It will
  // give the exact same results if the numerator and denominator have a
  // common divisor.
  char *out_buf = data;
  size_t items_read = 0;
  while (items_read < num_items && bytes_available >= item_size) {
    bytes_available -= item_size;
    client->subsample_state += client->numerator;
    if (client->subsample_state >= client->denominator) {
      client->subsample_state %= client->denominator;
      uint16_t bytes_out;
      shared_circular_buffer_read_consume(buffer, &client->buffer_client,
                                          item_size, (uint8_t *)out_buf,
                                          &bytes_out);
      PBL_ASSERTN(bytes_out == item_size);
      out_buf += item_size;
      items_read++;
    } else {
      PBL_ASSERTN(shared_circular_buffer_consume(buffer, &client->buffer_client,
                                                 item_size));
    }
  }
  return items_read;
}
