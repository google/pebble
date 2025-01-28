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

#include <inttypes.h>
#include <stdbool.h>

#include "services/common/put_bytes/put_bytes.h"

struct PutBytesStorageImplementation;
typedef struct PutBytesStorageImplementation PutBytesStorageImplementation;

typedef struct {
  // A struct full of function pointers that implements a storage API
  const PutBytesStorageImplementation *impl;

  //! A void pointer that the PutBytesStorageImplementation is free to stash stuff into
  void *impl_data;

  //! The offset into the storage we've initialized. Updated by pb_storage_append. pb_storage_init
  //! may set this to a non-zero value.
  uint32_t current_offset;
} PutBytesStorage;

typedef struct {
  int index;
  char filename[];
} PutBytesStorageInfo;

//! Write data directly to the putbyte storage. Does not update storage->bytes_written
//! @param storage A pointer to the storage struct representing the underlying storage
//! @param offset the offset within the storage we'd like to write to
//! @param buffer the data to write
//! @param length the amount of data to write
void pb_storage_write(PutBytesStorage *storage, uint32_t offset,
                      const uint8_t *buffer, uint32_t length);

//! Append data to the end of a putbyte storage. Updates storage->bytes_written
//! @param storage A pointer to the storage struct representing the underlying storage
//! @param buffer the data to append
//! @param length the amount of data to append
void pb_storage_append(PutBytesStorage *storage, const uint8_t *buffer, uint32_t length);

typedef enum {
  PutBytesCrcType_Legacy = 0, // See 'legacy_defective_checksum' calculation
  PutBytesCrcType_CRC32,
} PutBytesCrcType;

//! Calculate the CRC of the data in storage
//! @param storage A pointer to the storage struct representing the underlying storage
//! @param crc_type The type of CRC to compute
//! @return the checksum computed using 'crc_type' specified
//! @see drivers/crc.h
uint32_t pb_storage_calculate_crc(PutBytesStorage *storage, PutBytesCrcType crc_type);

//! Initialize a storage struct for a new putbyte transaction
//! @param storage a pointer-to-pointer to where we want to keep a reference to the storage
//! @param object_type the type of putbyte object we're about to store
//! @param total_size the size of the incoming object, in bytes
//! @param append_offset if != 0, this means we are continuing a PB operation that previously failed
//!                      for some reason. The incoming writes will start at this offset
//! @param info additional information about the data (see PutBytesStorageInfo).
bool pb_storage_init(PutBytesStorage *storage, PutBytesObjectType object_type,
                     uint32_t total_size, PutBytesStorageInfo *info, uint32_t append_offset);

//! Deinitialize and free a storage struct after a transaction is over
//! @param storage a pointer-to-pointer to where the reference to the storage is currently held
//! @param is_success whether the putbyte transfer succeeded or not
//! @note if putbytes is unsuccessful, the data will be deleted
void pb_storage_deinit(PutBytesStorage *storage, bool is_success);

//! Some types of storage allow the state of a partial installation to be recovered (today, just
//! firmware & resources).
//! @param obj_type The type of resource to recover the install status of
//! @param status[out] How many bytes have been written and their crc
//! @return True iff the status struct was populated with valid data
bool pb_storage_get_status(PutBytesObjectType obj_type, PbInstallStatus *status);
