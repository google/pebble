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

#include "util/list.h"
#include "util/attributes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


//! This is a CircularBuffer that supports one writer but multiple read clients. Data added to the buffer is kept
//! available for reading until every client has read it. Each client has their own read index that gets updated as
//! they consume data. If desired, the read index for clients that "fall behind" can be force advanced to make room
//! for new write data.

typedef struct PACKED SharedCircularBufferClient {
  ListNode list_node;
  uint16_t read_index;    // Index of next available byte in the buffer for this client
} SharedCircularBufferClient;

typedef struct SharedCircularBuffer {
  uint8_t *buffer;
  uint16_t buffer_size;
  uint16_t write_index;    //! where next byte will be written, (read_index == write_index) is an empty queue
  ListNode *clients;        //! linked list of clients
} SharedCircularBuffer;

//! Init the buffer
//! @param buffer The buffer to initialize
//! @param storage storage for the data
//! @param storage_size Size of the storage buffer
void shared_circular_buffer_init(SharedCircularBuffer* buffer, uint8_t* storage, uint16_t storage_size);

//! Add data to the buffer
//! @param buffer The buffer to write to
//! @param data The data to write
//! @param length Number of bytes to write
//! @param advance_slackers If true, automatically advance the read index, starting from the client farthest behind,
//!  until there is room for the new data. If the length is bigger than the entire buffer, then false is returned.
//! @return false if there's insufficient space.
bool shared_circular_buffer_write(SharedCircularBuffer* buffer, const uint8_t* data, uint16_t length,
        bool advance_slackers);

//! Add a read client
//! @param buffer The buffer to add the client to
//! @param client Pointer to a client structure. This structure must be allocated by the caller and cannot
//!   be freed until the client is removed
//! @return true if successfully added
bool shared_circular_buffer_add_client(SharedCircularBuffer* buffer, SharedCircularBufferClient *client);

//! Remove a read client
//! @param buffer The buffer to remove the client from
//! @param client Pointer to a client structure. This must be the same pointer passed to circular_buffer_add_client
void shared_circular_buffer_remove_client(SharedCircularBuffer* buffer, SharedCircularBufferClient *client);

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
//! @param client pointer to the client struct originally passed to circular_buffer_add_client
//! @param length How many bytes to read
//! @param[out] data_out The bytes that were read.
//! @param[out] length_out How many bytes were read.
//! @return false if there's less than length bytes in the buffer.
bool shared_circular_buffer_read(const SharedCircularBuffer* buffer, SharedCircularBufferClient *client,
        uint16_t length, const uint8_t** data_out, uint16_t* length_out);

//! Removes length bytes of the oldest data from the buffer.
//! @param buffer The buffer to operate on
//! @param client Pointer to a client structure originally passed to circular_buffer_add_client
//! @param length The number of bytes to consume
//! @return True if success
bool shared_circular_buffer_consume(SharedCircularBuffer* buffer, SharedCircularBufferClient *client, uint16_t length);

//! @param buffer The buffer to operate on
//! @return The number of bytes we can write before circular_buffer_write will return false.
uint16_t shared_circular_buffer_get_write_space_remaining(const SharedCircularBuffer* buffer);

//! @param buffer The buffer to operate on
//! @param client Pointer to a client structure originally passed to circular_buffer_add_client
//! @return The number of bytes we can read before circular_buffer_read will return false.
uint16_t shared_circular_buffer_get_read_space_remaining(const SharedCircularBuffer* buffer,
         SharedCircularBufferClient *client);


//! Read and consume bytes.
//!
//! @param buffer The buffer to read from
//! @param client pointer to the client struct originally passed to circular_buffer_add_client
//! @param length How many bytes to read
//! @param data Buffer to copy the data into
//! @param[out] length_out How many bytes were read into the caller's buffer
//! @return true if we returned length bytes, false if we returned less.
bool shared_circular_buffer_read_consume(SharedCircularBuffer *buffer, SharedCircularBufferClient *client,
                          uint16_t length, uint8_t *data, uint16_t *length_out);


typedef struct SubsampledSharedCircularBufferClient {
  SharedCircularBufferClient buffer_client;
  uint16_t numerator;
  uint16_t denominator;
  //! Used to track whether to copy or discard each successive data item
  uint16_t subsample_state;
} SubsampledSharedCircularBufferClient;

//! Add a read client which subsamples the data.
//!
//! @param buffer The buffer to add the client to
//! @param client Pointer to a client structure. This structure must be
//!     allocated by the caller and cannot be freed until the client is
//!     removed.
//! @param subsample_numerator The numerator of the client's initial subsampling
//!     ratio.
//! @param subsample_denominator The denominator of the client's initial
//!     subsampling ratio. This value must be equal to or greater than the
//!     subsampling numerator.
//! @sa subsampled_shared_circular_buffer_client_set_ratio
void shared_circular_buffer_add_subsampled_client(
    SharedCircularBuffer *buffer, SubsampledSharedCircularBufferClient *client,
    uint16_t subsample_numerator, uint16_t subsample_denominator);

//! Remove a subsampling read client
//! @param buffer The buffer to remove the client from
//! @param client Pointer to a client structure. This must be the same pointer
//!     passed to shared_circular_buffer_add_subsampled_client
void shared_circular_buffer_remove_subsampled_client(
    SharedCircularBuffer *buffer, SubsampledSharedCircularBufferClient *client);

//! Change the subsampling ratio of a subsampling shared circular buffer client.
//!
//! This resets the subsampling state, which may introduce jitter on the next
//! read operation.
//!
//! @param client The client to apply the new ratio to
//! @param numerator The numerator of the subsampling ratio.
//! @param denominator The denominator of the subsampling ratio. This value must
//!     be equal to or greater than the numerator.
//!
//! @note Specifying a subsampling ratio with a numerator greater than 1 will
//!     introduce jitter to the subsampled data stream.
void subsampled_shared_circular_buffer_client_set_ratio(
    SubsampledSharedCircularBufferClient *client,
    uint16_t numerator, uint16_t denominator);

//! Read and consume items with subsampling.
//!
//! @param buffer The buffer to read from
//! @param client pointer to the client struct originally passed to
//!     shared_circular_buffer_add_subsampled_client
//! @param item_size Size of each item, in bytes
//! @param data Buffer to copy the data into. Must be at least
//!     item_size * num_items bytes in size.
//! @param num_items How many items to read. This is the number of items AFTER
//!     subsampling.
//! @return The number of items actually read into the data buffer after
//!     subsampling. This may be less than num_items.
size_t shared_circular_buffer_read_subsampled(
    SharedCircularBuffer* buffer,
    SubsampledSharedCircularBufferClient *client,
    size_t item_size, void *data, uint16_t num_items);
