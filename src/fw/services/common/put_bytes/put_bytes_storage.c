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

#include "put_bytes_storage_internal.h"

#include "kernel/pbl_malloc.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"


#ifdef UNITTEST
extern const PutBytesStorageImplementation s_raw_implementation;
extern const PutBytesStorageImplementation s_file_implementation;
#else  // #ifdef UNITTEST
#include "put_bytes_storage_raw.h"

static const PutBytesStorageImplementation s_raw_implementation = {
  .init = pb_storage_raw_init,
  .get_max_size = pb_storage_raw_get_max_size,
  .write = pb_storage_raw_write,
  .calculate_crc = pb_storage_raw_calculate_crc,
  .deinit = pb_storage_raw_deinit
};

#ifndef RECOVERY_FW
#include "put_bytes_storage_file.h"

static const PutBytesStorageImplementation s_file_implementation = {
  .init = pb_storage_file_init,
  .get_max_size = pb_storage_file_get_max_size,
  .write = pb_storage_file_write,
  .calculate_crc = pb_storage_file_calculate_crc,
  .deinit = pb_storage_file_deinit
};
#endif  // #ifndef RECOVERY_FW
#endif  // #ifdef UNITTEST


void pb_storage_write(PutBytesStorage *storage, uint32_t offset, const uint8_t *buffer,
                      uint32_t length) {
  storage->impl->write(storage, offset, buffer, length);
}

void pb_storage_append(PutBytesStorage *storage, const uint8_t *buffer, uint32_t length) {
  pb_storage_write(storage, storage->current_offset, buffer, length);
  storage->current_offset += length;
}

uint32_t pb_storage_calculate_crc(PutBytesStorage *storage, PutBytesCrcType crc_type) {
  return storage->impl->calculate_crc(storage, crc_type);
}

bool pb_storage_init(PutBytesStorage *storage, PutBytesObjectType object_type,
                     uint32_t total_size, PutBytesStorageInfo *info, uint32_t append_offset) {
  static const PutBytesStorageImplementation* IMPL_FOR_OBJECT_TYPE[] = {
    [ObjectFirmware] = &s_raw_implementation,
    [ObjectRecovery] = &s_raw_implementation,
    [ObjectSysResources] = &s_raw_implementation,
#ifndef RECOVERY_FW
    [ObjectAppResources] = &s_file_implementation,
    [ObjectWatchApp] = &s_file_implementation,
    [ObjectFile] = &s_file_implementation,
    [ObjectWatchWorker] = &s_file_implementation
#endif
  };

  // Make sure we haven't initialized this storage yet.
  PBL_ASSERTN(info && storage && storage->impl == NULL);

  if (object_type >= ARRAY_LENGTH(IMPL_FOR_OBJECT_TYPE) ||
      IMPL_FOR_OBJECT_TYPE[object_type] == NULL) {
    PBL_LOG(LOG_LEVEL_WARNING, "Unsupported PutBytesObjectType %u", object_type);
    return false;
  }

  const PutBytesStorageImplementation *impl = IMPL_FOR_OBJECT_TYPE[object_type];

  uint32_t max_size = impl->get_max_size(object_type);
  if (total_size > max_size) {
    PBL_LOG(LOG_LEVEL_WARNING, "Invalid size for type %u, size: %"PRIu32", max_size: %"PRIu32,
            object_type, total_size, max_size);
    return false;
  }

  storage->impl = impl;
  return storage->impl->init(storage, object_type, total_size, info, append_offset);
}

void pb_storage_deinit(PutBytesStorage *storage, bool is_success) {
  if (storage->impl) {
    storage->impl->deinit(storage, is_success);
    *storage = (PutBytesStorage){};
  }
}

extern bool pb_storage_raw_get_status(PutBytesObjectType obj_type,  PbInstallStatus *status);
bool pb_storage_get_status(PutBytesObjectType obj_type, PbInstallStatus *status) {
  switch (obj_type) {
    case ObjectFirmware:
    case ObjectRecovery:
    case ObjectSysResources:
      return pb_storage_raw_get_status(obj_type, status);
    default: // Partial installs not supported for other object types today
      return false;
  }
}
