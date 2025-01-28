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

#include "data_logging_service.h"
#include "dls_storage.h"
#include "dls_list.h"

#include "drivers/flash.h"
#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "kernel/util/sleep.h"
#include "services/common/analytics/analytics.h"
#include "services/normal/filesystem/pfs.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/string.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>


typedef enum {
  DLS_VERSION_0 = 0x20,
} DLSFileHeaderVersion;

static const DLSFileHeaderVersion DLS_CURRENT_VERSION = DLS_VERSION_0;

// Set while executing dls_storage_rebuild() which is called from dls_init() during boot time
// When set, we allow storage accesses from KernelMain whereas normally, only KernelBG is allowed.
static bool s_initializing_storage = false;

// Each session stores data in a separate pfs file with this data in the front. The file name
// is constructed as ("%s%d", DLS_FILE_NAME_PREFIX, comm_session_id)
typedef struct PACKED {
  DLSFileHeaderVersion version:8;

  uint8_t comm_session_id;
  uint32_t timestamp;
  uint32_t tag;
  Uuid app_uuid;
  DataLoggingItemType item_type:8;
  uint16_t item_size;
} DLSFileHeader;


// We organize data in the file into chunks with this header at the front of each chunk.
// This allows us to mark chunks as already read by setting the valid bit to 0 after we
// successfully read it out. This is necessary to keep track of read chunks in the file system
// so that we can recover our read position after a reboot.
#define DLS_CHUNK_HDR_NUM_BYTES_UNINITIALIZED  0x7f
typedef struct PACKED {
  //! The number of data bytes after this header, not including this header. If this value
  //! is DLS_CHUNK_HDR_NUM_BYTES_UNINITIALIZED (all bits set), it means no data follows.
  uint8_t num_bytes:7;
  bool valid:1;             // Set to false after chunk is consumed.
} DLSChunkHeader;

// The most we try to fit into a data chunk. This value must be small enough to fit within the 7
// bytes reserved for it within the DLSChunkHeader.
#define DLS_MAX_CHUNK_SIZE_BYTES  100
_Static_assert(DLS_MAX_CHUNK_SIZE_BYTES < DLS_CHUNK_HDR_NUM_BYTES_UNINITIALIZED,
    "DLS_MAX_CHUNK_SIZE_BYTES must be less than DLS_CHUNK_HDR_NUM_BYTES_UNINITIALIZED");

// Forward declarations
static bool prv_realloc_storage(DataLoggingSession *session, uint32_t new_size);
static bool prv_get_session_file(DataLoggingSession *session, uint32_t space_needed);
static void prv_release_session_file(DataLoggingSession *session);


// ----------------------------------------------------------------------------------------
static void prv_assert_valid_task(void) {
  if (!s_initializing_storage) {
    // The s_initializing_storage is only set during boot, when dls_storage_rebuild() is called.
    // That is the only time we allow a task (KernelMain) other than KernelBG access to the storage
    // functions.
    PBL_ASSERT_TASK(PebbleTask_KernelBackground);
  }
}


// -----------------------------------------------------------------------------------------
static void prv_get_filename(char *name, DataLoggingSession *session) {
  concat_str_int(DLS_FILE_NAME_PREFIX, session->comm.session_id, name, DLS_FILE_NAME_MAX_LEN);
}


// ----------------------------------------------------------------------------------------
// Logs if an error occurs, returns true on success
static bool prv_pfs_read(int fd, void *buf, size_t size) {
  int bytes_read;
  bytes_read = pfs_read(fd, buf, size);
  if (bytes_read < S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Err %d while reading", (int)bytes_read);
    return false;
  } else if (bytes_read < (int)size) {
    PBL_LOG(LOG_LEVEL_ERROR, "Read only %d bytes, expected %d", (int)bytes_read, (int)size);
    return false;
  }
  return true;
}


// ----------------------------------------------------------------------------------------
// Logs if an error occurs, returns true on success
static bool prv_pfs_write(int fd, void *buf, size_t size) {
  int bytes_wrote = pfs_write(fd, buf, size);
  if (bytes_wrote < S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Err %d while writing", (int)bytes_wrote);
    return false;
  } else if (bytes_wrote < (int)size) {
    PBL_LOG(LOG_LEVEL_ERROR, "Wrote only %d bytes, expected %d", (int)bytes_wrote, (int)size);
    return false;
  }
  return true;
}


// ----------------------------------------------------------------------------------------
// Logs if an error occurs, returns true on success
static bool prv_pfs_seek(int fd, int offset, FSeekType seek_type) {
  int result;
  result = pfs_seek(fd, offset, seek_type);
  if (result < S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Err %d while seeking", result);
    return false;
  }
  return true;
}


// ----------------------------------------------------------------------------------------
// Logs if an error occurs, returns true on success
static size_t prv_pfs_get_file_size(int fd) {
  size_t result = pfs_get_file_size(fd);
  if (result == 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Err getting size");
  }
  return result;
}


// -----------------------------------------------------------------------------------------
// Callback passed to pfs_iterate_files. Used to find data logging files by name
static bool prv_filename_filter_cb(const char *name) {
  const int prefix_len = strlen(DLS_FILE_NAME_PREFIX);
  return (strncmp(name, DLS_FILE_NAME_PREFIX, prefix_len) == 0);
}


// -----------------------------------------------------------------------------------------
// Given a session pointer, return how much larger we want to grow the file for it if/when
// we decide to reallocate it
static uint32_t prv_get_desired_free_bytes(DataLoggingSession *session) {
  // By default, we will make the file 50% larger than the currently used number of bytes
  uint32_t free_bytes = session->storage.num_bytes / 2;

  // Set some lower and upper bounds
  free_bytes = CLIP(free_bytes, DLS_MIN_FILE_FREE_BYTES, DLS_MAX_FILE_FREE_BYTES);
  return free_bytes;
}


// ----------------------------------------------------------------------------------------
static bool prv_accumulate_size_cb(DataLoggingSession* session, void *data) {
  uint32_t *size_p = (uint32_t *)data;
  if (session->storage.write_offset != 0) {
    if (prv_get_session_file(session, 0)) {
      *size_p += prv_pfs_get_file_size(session->storage.fd);
      prv_release_session_file(session);
    }
  }
  return true;
}


// ----------------------------------------------------------------------------------------
// Get total amount of space we have allocated from the file system. This is the sum of the
// file sizes of all the DLS files.
static uint32_t prv_get_total_file_system_bytes(void) {
  uint32_t size = 0;
  dls_list_for_each_session(prv_accumulate_size_cb, &size);
  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Total used space: %d", (int)size);
  return size;
}


// ----------------------------------------------------------------------------------------
// Compact all storage files. Used to free up space for new data.
static bool prv_compact_session_cb(DataLoggingSession* session, void *data) {
  if (session->storage.write_offset == 0) {
    // The write offset is 0 if we've never created storage for this session.
    return true;
  }

  if (!prv_get_session_file(session, 0)) {
    // We couldn't open up this storage file. Since we are just compacting where we can,
    // just return true so that we go on to the next session in the list.
    return true;
  }

  size_t cur_size = prv_pfs_get_file_size(session->storage.fd);
  uint32_t target_free_bytes = prv_get_desired_free_bytes(session);
  if ((cur_size > 0) && (session->storage.num_bytes + target_free_bytes < cur_size)) {
    // We have more than the desired number of free bytes in this file, so it is a candidate
    // for compaction.
    uint32_t new_size = session->storage.num_bytes + target_free_bytes;
    new_size = MAX(new_size, DLS_FILE_INIT_SIZE_BYTES);
    prv_release_session_file(session);
    if (new_size != cur_size || session->storage.read_offset > sizeof(DLSFileHeader)) {
      prv_realloc_storage(session, new_size);
    }
  } else {
    prv_release_session_file(session);
  }
  return true;
}


// -----------------------------------------------------------------------------------------
// Make sure there is at least 'needed' bytes available in our file system space
// allowed for data logging
static bool prv_make_file_system_space(uint32_t needed) {
  // Make sure we have at least 'needed' bytes free in the file system
  uint32_t used_space = prv_get_total_file_system_bytes();
  if (used_space + needed >= DLS_MAX_DATA_BYTES) {
    dls_list_for_each_session(prv_compact_session_cb, NULL);

    used_space = prv_get_total_file_system_bytes();
    if (used_space + needed >= DLS_MAX_DATA_BYTES) {
      return false;
    }
  }
  return true;
}


// -----------------------------------------------------------------------------------------
// Open an existing or create a new storage file. If the write_offset in the storage structure
// is 0, then write a new file header based on the info from the given session.
static bool prv_open_file(DataLoggingSessionStorage *storage, uint8_t op_flags,
                          int32_t size, DataLoggingSession *session) {
  // Open/Create the file
  char name[DLS_FILE_NAME_MAX_LEN];
  prv_get_filename(name, session);

  int fd = pfs_open(name, op_flags, FILE_TYPE_STATIC, size);
  if (fd < S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Could not open/create DLS file %s", name);
    return false;
  }

  if (storage->write_offset != 0) {
    storage->fd = fd;
    return true;
  }

  DLSFileHeader hdr = (DLSFileHeader) {
    .version = DLS_CURRENT_VERSION,
    .comm_session_id = session->comm.session_id,
    .timestamp = session->session_created_timestamp,
    .tag = session->tag,
    .app_uuid = session->app_uuid,
    .item_type = session->item_type,
    .item_size = session->item_size
  };

  // Write the header
  if (!prv_pfs_write(fd, &hdr, sizeof(hdr))) {
    pfs_close_and_remove(fd);
    return false;
  }

  // Init the storage struct
  *storage = (DataLoggingSessionStorage) {
    .fd = fd,
    .write_offset = sizeof(hdr),
    .read_offset = sizeof(hdr)
  };

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Created session-storage: "
      "id %"PRIu8", filename: %s, fd: %d, size: %d", session->comm.session_id, name, fd,
      (int)size);
  return true;
}


// -----------------------------------------------------------------------------------------
// Close the session file
static void prv_release_session_file(DataLoggingSession *session) {
  PBL_ASSERTN(session->storage.fd != DLS_INVALID_FILE);

  status_t status = pfs_close(session->storage.fd);
  if (status != S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error %d closing file for session %d", (int)status,
            session->comm.session_id);
  }
  session->storage.fd = DLS_INVALID_FILE;
}


// -----------------------------------------------------------------------------------------
// Open the session file, creating it if necessary. If space_needed is > 0, then make sure there is
// enough space in the file to add 'space_needed' more bytes, compacting, growing, or lopping off
// older data in the file if necessary. Returns true if there is enough space and the file was
// successfully opened.
// If space_needed is 0 (generally used when reading only), we just attempt to open the file and
// don't check the available space for writing.
// If this method returns true, the caller must eventually close the file using
// prv_release_session_file().
static bool prv_get_session_file(DataLoggingSession *session, uint32_t space_needed) {
  bool success = false;
  PBL_ASSERTN(session->storage.fd == DLS_INVALID_FILE);

  // Open/create the file
  // We always reserve enough space to create a file of DLS_FILE_INIT_SIZE_BYTES, so no need to
  // check the quota (see calculation of DLS_MAX_DATA_BYTES).
  if (!prv_open_file(&session->storage, OP_FLAG_WRITE | OP_FLAG_READ, DLS_FILE_INIT_SIZE_BYTES,
                     session)) {
    return false;
  }

  if (space_needed == 0) {
    // If no extra space needed, we are done because we successfully opened the file.
    return true;
  }

  // Get the file size
  size_t file_size = prv_pfs_get_file_size(session->storage.fd);
  if (file_size == 0) {
    prv_release_session_file(session);
    return false;
  }

  // Add a minium buffer to needed. This gives us a little insurance and also allows for the
  // extra space needed for the chunk header byte that occurs at least once every
  // DLS_MAX_CHUNK_SIZE_BYTES bytes.
  space_needed += DLS_MIN_FREE_BYTES;
  uint32_t space_avail = file_size - session->storage.write_offset;
  if (space_needed <= space_avail) {
    // We have enough space
    return true;
  }
  uint32_t min_delta_size = space_needed - space_avail;

  // The remaining strategies rely on reallocating the file, so we need to close it first
  prv_release_session_file(session);

  // If we can free up space by reallocating this file, try that next. Since we are
  // reallocating anyways, take this chance to optimize the amount of free space in the file.
  uint32_t target_file_size = session->storage.num_bytes + space_needed
                            + prv_get_desired_free_bytes(session);
  target_file_size = MAX(target_file_size, DLS_FILE_INIT_SIZE_BYTES);
  uint32_t optimum_delta_size = target_file_size - file_size;

  bool have_space_to_grow = prv_make_file_system_space(optimum_delta_size);
  if (!have_space_to_grow) {
    // If we don't have enough space to grow to our optimum size, see if growing to fill whatever
    // free space is left in the file system is sufficient.
    uint32_t total_allocated_bytes = prv_get_total_file_system_bytes();
    if (total_allocated_bytes + min_delta_size <= DLS_MAX_DATA_BYTES) {
      target_file_size = DLS_MAX_DATA_BYTES - total_allocated_bytes + file_size;
      have_space_to_grow = true;
    }
  }
  if (have_space_to_grow) {
    if (!prv_realloc_storage(session, target_file_size)) {
      goto exit;
    }
    success = prv_open_file(&session->storage, OP_FLAG_WRITE | OP_FLAG_READ,
                            DLS_FILE_INIT_SIZE_BYTES, session);
    if (success) {
      goto exit;
    }
  }

  // Lop off old data at the beginning of the file if there is enough there.
  success = false;
  // If we are going to consume, we have to be prepared to consume at least 1 data chunk
  min_delta_size = MAX(min_delta_size, DLS_MAX_CHUNK_SIZE_BYTES);
  if (session->storage.num_bytes <= min_delta_size) {
    // Lopping off the used bytes won't satisfy space_needed
    goto exit;
  }
  uint32_t consume_bytes = MAX(session->storage.num_bytes/2, min_delta_size);
  if (dls_storage_consume(session, consume_bytes) < 0) {
    // We failed to lop off the used bytes
    goto exit;
  }
  // Reallocate which removes the consumed bytes from the beginning of the file.
  if (!prv_realloc_storage(session, file_size)) {
    goto exit;
  }

  // Re-open it now
  success = prv_open_file(&session->storage, OP_FLAG_WRITE | OP_FLAG_READ, DLS_FILE_INIT_SIZE_BYTES,
                          session);

exit:
  // If success, double-check that we have enough space.
  if (success && space_needed > 0) {
    file_size = prv_pfs_get_file_size(session->storage.fd);
    PBL_ASSERTN(session->storage.write_offset + space_needed <= file_size);
  }
  return success;
}


// -----------------------------------------------------------------------------------------
static bool prv_write_data(DataLoggingSessionStorage *storage, const void *data,
                           uint32_t remaining_bytes) {
  const uint8_t *data_ptr = data;

  // Write out in chunks
  while (remaining_bytes > 0) {
    uint8_t data_chunk_length = MIN(DLS_MAX_CHUNK_SIZE_BYTES, remaining_bytes);

    // Write the data first, so if an error occurs, the header is left in the uninitialized state
    if (!prv_pfs_seek(storage->fd, storage->write_offset + sizeof(DLSChunkHeader), FSeekSet)) {
      return false;
    }
    if (!prv_pfs_write(storage->fd, (void *)data_ptr, data_chunk_length)) {
      return false;
    }

    // Write data chunk header now
    if (!prv_pfs_seek(storage->fd, storage->write_offset, FSeekSet)) {
      return false;
    }
    DLSChunkHeader data_hdr = { .num_bytes = data_chunk_length, .valid = true };
    if (!prv_pfs_write(storage->fd, &data_hdr, sizeof(DLSChunkHeader))) {
      return false;
    }

    // Bump pointer and count
    storage->write_offset += data_chunk_length + sizeof(DLSChunkHeader);
    storage->num_bytes += data_chunk_length;

    remaining_bytes -= data_chunk_length;
    data_ptr += data_chunk_length;
  }
  return true;
}


// -----------------------------------------------------------------------------------------
// Migrate a session's data to a new file, removing already consumed bytes from the front
static bool prv_realloc_storage(DataLoggingSession *session, uint32_t new_size) {
  bool success;
  uint8_t *tmp_buf = NULL;

  // Record in metrics
  analytics_inc(ANALYTICS_DEVICE_METRIC_DATA_LOGGING_REALLOC_COUNT, AnalyticsClient_System);

  // Must be called with the file closed
  PBL_ASSERTN(session->storage.fd == DLS_INVALID_FILE);

  PBL_LOG(LOG_LEVEL_INFO, "Compacting storage for session %d", session->comm.session_id);
  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG,
            "Before compaction: num_bytes: %"PRIu32", write_offset:%"PRIu32,
            session->storage.num_bytes, session->storage.write_offset);

  // Init a storage struct and create a new file for the compacted data
  DataLoggingSessionStorage new_storage = {
    .fd = DLS_INVALID_FILE,
  };
  success = prv_open_file(&new_storage, OP_FLAG_OVERWRITE | OP_FLAG_READ, new_size,
                          session);
  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Could not create temporary file to migrate storage file");
    goto exit;
  }

  // Copy data in chunks from the old file to the new one. Things go faster with a bigger buffer.
  success = false;
  // We have to make sure we have at least 1 delineated item within each DLS_ENDPOINT_MAX_PAYLOAD
  // bytes and clipping max_chunk_size to DLS_ENDPOINT_MAX_PAYLOAD insures that. If we didn't clip
  // it and the item size was 645 for example, we might pack 2 items back to back in storage
  // using DLS_MAX_CHUNK_SIZE_BYTES (100) byte chunks and dls_private_send_session() wouldn't be
  // able to get a complete single item because we wrote 1290 bytes using DLS_MAX_CHUNK_SIZE_BYTES
  // byte chunks and there is no chunk boundary at the 645 byte offset.
  int32_t max_chunk_size = DLS_ENDPOINT_MAX_PAYLOAD;
  while (true) {
    tmp_buf = kernel_malloc(max_chunk_size);
    if (tmp_buf) {
      break;
    }
    if (max_chunk_size < 256) {
      PBL_LOG(LOG_LEVEL_ERROR, "Not enough memory for reallocation");
      goto exit;
    }
    max_chunk_size /= 2;
  }

  int32_t bytes_to_copy = session->storage.num_bytes;
  while (bytes_to_copy) {
    uint32_t new_read_offset;
    int32_t bytes_read = dls_storage_read(session, tmp_buf, MIN(max_chunk_size, bytes_to_copy),
                                          &new_read_offset);
    if (bytes_read <= 0) {
      goto exit;
    }

    // Write to new file
    if (!prv_write_data(&new_storage, tmp_buf, bytes_read)) {
      goto exit;
    }

    // Consume out of old one now.
    if (dls_storage_consume(session, bytes_read) < 0) {
      goto exit;
    }

    bytes_to_copy -= bytes_read;
  }

  // We successfully transferred the unread data to the new storage, place it into the session
  // info
  pfs_close(session->storage.fd);

  // Close the new file now. That will finish up the swap for us
  pfs_close(new_storage.fd);
  new_storage.fd = DLS_INVALID_FILE;

  // Plug in the new storage info into the session
  session->storage = new_storage;

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG,
            "After compaction: size: %d, num_bytes: %d, write_offset:%d",
            (int)new_size, (int)session->storage.num_bytes, (int)session->storage.write_offset);
  success = true;

exit:
  kernel_free(tmp_buf);
  if (new_storage.fd != DLS_INVALID_FILE) {
    pfs_close_and_remove(new_storage.fd);
  }

  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Migration failed of session file %d", session->comm.session_id);
    dls_storage_delete_logging_storage(session);
  }
  return success;
}


// -----------------------------------------------------------------------------------------
void dls_storage_invalidate_all(void) {
  // Iterate through all files in the file system, looking for all DLS storage files and
  // deleting them.
  pfs_remove_files(prv_filename_filter_cb);
}


// -----------------------------------------------------------------------------------------
void dls_storage_delete_logging_storage(DataLoggingSession *session) {
  prv_assert_valid_task();
  PBL_ASSERTN(session->storage.fd == DLS_INVALID_FILE);

  char name[DLS_FILE_NAME_MAX_LEN];
  prv_get_filename(name, session);
  status_t status = pfs_remove(name);
  if (status != S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error %d removing file", (int) status);
  }

  // Clear out storage info
  session->storage = (DataLoggingSessionStorage) {
    .fd = DLS_INVALID_FILE,
  };
}


// -----------------------------------------------------------------------------------------
// Write data directly to flash. Called from dls_log() when the session is
// unbuffered. Assumes the caller has already locked the session using dls_lock_session().
bool dls_storage_write_data(DataLoggingSession *session, const void *data, uint32_t num_bytes) {
  prv_assert_valid_task();

  bool success = false;
  bool got_session_file = prv_get_session_file(session, num_bytes);
  if (!got_session_file) {
    goto exit;
  }

  success = prv_write_data(&session->storage, data, num_bytes);

exit:
  if (got_session_file) {
    prv_release_session_file(session);
  }

  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Nuking storage for session %d", session->comm.session_id);
    dls_storage_delete_logging_storage(session);
  }
  return success;
}


// -----------------------------------------------------------------------------------------
// Copy data out of a session's circular buffer and write it to flash. Called from a KernelBG
// system task callback triggered by dls_log() after data is added to a buffered session.
bool dls_storage_write_session(DataLoggingSession *session) {
  prv_assert_valid_task();

  bool success = true;
  bool got_session_file = false;

  // Note that s_list_mutex is already owned because this is called from
  // dls_list_for_each_session(), so we CANNOT (and don't need to) call dls_lock_session() from
  // here because that could result in a deadlock (see comments in dls_lock_session).
  dls_assert_own_list_mutex();
  if (session->status != DataLoggingStatusActive) {
    // Not active
    return true;
  }

  // If this session is not buffered, there is no circular buffer to move data out of, it would
  // have been written directly to flash during dls_log. But, we can end up here because of a
  // call to prv_write_all_sessions_to_flash() which iterates through ALL active sessions.
  if (!session->data->buffer_storage) {
    goto exit;
  }

  session->data->write_request_pending = false;
  int bytes_remaining = shared_circular_buffer_get_read_space_remaining(
      &session->data->buffer, &session->data->buffer_client);
  if (bytes_remaining == 0) {
    goto exit;
  }
  got_session_file = prv_get_session_file(session, bytes_remaining);
  if (!got_session_file) {
    success = false;
    goto exit;
  }

  while (bytes_remaining > 0) {
    const uint8_t* read_ptr;
    uint16_t bytes_read;
    success = shared_circular_buffer_read(&session->data->buffer,
                                          &session->data->buffer_client,
                                          bytes_remaining, &read_ptr, &bytes_read);
    PBL_ASSERTN(success);
    success = prv_write_data(&session->storage, read_ptr, bytes_read);
    if (!success) {
      goto exit;
    }
    shared_circular_buffer_consume(&session->data->buffer, &session->data->buffer_client,
                                   bytes_read);
    bytes_remaining -= bytes_read;
  }

exit:
  if (got_session_file) {
    prv_release_session_file(session);
  }

  if (!success) {
    PBL_LOG(LOG_LEVEL_ERROR, "Nuking storage for session %d", session->comm.session_id);
    dls_storage_delete_logging_storage(session);
  }
  return success;
}


// -----------------------------------------------------------------------------------------
// Special case: if buffer is NULL, just doesn't perform any reads, it just returns the # of bytes
// of data available for reading. Returns -1 on error.
// On exit, *new_read_offset contains the new read_offset
int32_t dls_storage_read(DataLoggingSession *logging_session, uint8_t *buffer, int32_t num_bytes,
                         uint32_t *new_read_offset) {
  prv_assert_valid_task();

  int32_t read_bytes = 0;
  int32_t last_whole_items_read_bytes = 0;
  bool got_session_file = false;

  if (logging_session->storage.write_offset == 0) {
    // no data available for this session
    goto exit;
  }

  got_session_file = prv_get_session_file(logging_session, 0);
  if (!got_session_file) {
    last_whole_items_read_bytes = -1;    // error
    goto exit;
  }

  uint32_t read_offset = logging_session->storage.read_offset;

  while (!buffer || read_bytes < num_bytes) {
    DLSChunkHeader chunk_hdr;

    // Reached the end of the file?
    // NOTE: we don't do this check if we are scanning for the last written byte (buffer == NULL)
    if (buffer) {
      if (read_offset >= logging_session->storage.write_offset) {
        break;
      }
    }

    // Read the chunk header
    if (!prv_pfs_seek(logging_session->storage.fd, read_offset, FSeekSet)) {
      last_whole_items_read_bytes = -1;
      goto exit;
    }
    if (!prv_pfs_read(logging_session->storage.fd, &chunk_hdr, sizeof(chunk_hdr))) {
      last_whole_items_read_bytes = -1;
      goto exit;
    }

    // Reached the end of the valid data?
    if (chunk_hdr.valid && chunk_hdr.num_bytes == DLS_CHUNK_HDR_NUM_BYTES_UNINITIALIZED) {
      break;
    }

    // Valid data?
    if (chunk_hdr.valid) {
      if (buffer) {
        if (chunk_hdr.num_bytes + read_bytes > num_bytes) {
          // Not enough room in buffer to read next chunk.
          break;
        }

        if (!prv_pfs_read(logging_session->storage.fd, buffer, chunk_hdr.num_bytes)) {
          last_whole_items_read_bytes = -1;
          goto exit;
        }
        read_bytes += chunk_hdr.num_bytes;
        buffer += chunk_hdr.num_bytes;

      } else {
        // Just scanning for the last written byte
        read_bytes += chunk_hdr.num_bytes;
      }
    }
    read_offset += sizeof(chunk_hdr) + chunk_hdr.num_bytes;

    // Did we reach a whole item boundary? If so, update our "last_whole_item" bookkeeping now
    if ((read_bytes % logging_session->item_size) == 0) {
      last_whole_items_read_bytes = read_bytes;
      *new_read_offset = read_offset;
    }
  }

exit:
  if (got_session_file) {
    prv_release_session_file(logging_session);
  }

  if (last_whole_items_read_bytes < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Nuking storage for session %d", logging_session->comm.session_id);
    dls_storage_delete_logging_storage(logging_session);
  }

  return last_whole_items_read_bytes;
}


// -----------------------------------------------------------------------------------------
// Consume num_bytes of data. As a special case, if num_bytes is 0, this simply advances the
// internal storage.read_offset to match the # of bytes already consumed without consuming any more.
// This special mode is only used by dls_storage_rebuild() when we are resurrecting old
// sessions from the file system.
int32_t dls_storage_consume(DataLoggingSession *logging_session, int32_t num_bytes) {
  prv_assert_valid_task();

  int32_t consumed_bytes = 0;
  bool got_session_file = false;

  if (logging_session->storage.write_offset == 0) {
    // no data available for this session
    goto exit;
  }

  got_session_file = prv_get_session_file(logging_session, 0);
  if (!got_session_file) {
    consumed_bytes = -1;    // error
    goto exit;
  }

  bool reset_read_offset = (num_bytes == 0);
  while (reset_read_offset || consumed_bytes < num_bytes) {
    DLSChunkHeader chunk_hdr;

    // Reached the end of the file?
    if (logging_session->storage.read_offset >= logging_session->storage.write_offset) {
      break;
    }

    if (!prv_pfs_seek(logging_session->storage.fd, logging_session->storage.read_offset,
                      FSeekSet)) {
      consumed_bytes = -1;    // error
      goto exit;
    }
    if (!prv_pfs_read(logging_session->storage.fd, &chunk_hdr, sizeof(chunk_hdr))) {
      consumed_bytes = -1;    // error
      goto exit;
    }

    if (chunk_hdr.valid && chunk_hdr.num_bytes == DLS_CHUNK_HDR_NUM_BYTES_UNINITIALIZED) {
      // End of valid data
      break;
    }

    if (chunk_hdr.valid) {
      if (reset_read_offset) {
        // If we are only resetting the read offset, break out now.
        break;
      }
      if (chunk_hdr.num_bytes > num_bytes) {
        // Somehow the caller tried to consume less than they read?
        PBL_LOG(LOG_LEVEL_WARNING, "Read/consume out of sync");
        goto exit;
      }
      // Invalidate the chunk, now that we have consumed it
      chunk_hdr.valid = false;
      if (!prv_pfs_seek(logging_session->storage.fd, logging_session->storage.read_offset,
                        FSeekSet)) {
        consumed_bytes = -1;    // error
        goto exit;
      }
      if (!prv_pfs_write(logging_session->storage.fd, &chunk_hdr, sizeof(chunk_hdr))) {
        consumed_bytes = -1;    // error
        goto exit;
      }
      if (logging_session->storage.num_bytes < chunk_hdr.num_bytes) {
        PBL_LOG(LOG_LEVEL_ERROR, "Inconsistent tracking of num_bytes");
        consumed_bytes = -1;    // error
        goto exit;
      }
      logging_session->storage.num_bytes -= chunk_hdr.num_bytes;
    }

    logging_session->storage.read_offset += sizeof(DLSChunkHeader) + chunk_hdr.num_bytes;
    consumed_bytes += chunk_hdr.num_bytes;
  }

exit:
  if (consumed_bytes > 0) {
    PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG,
              "Consumed %d bytes from session %d", (int)consumed_bytes,
              logging_session->comm.session_id);
  }

  if (got_session_file) {
    prv_release_session_file(logging_session);
  }

  if (consumed_bytes < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Nuking storage for session %d", logging_session->comm.session_id);
    dls_storage_delete_logging_storage(logging_session);
  }
  return consumed_bytes;
}


// -----------------------------------------------------------------------------------------
// Called from dls_init() during boot time to scan for existing DLS storage files in the file
// system and recreate sessions from them.
void dls_storage_rebuild(void) {
  char name[DLS_FILE_NAME_MAX_LEN];

  // This disables the checks that verify that only KernelBG is accessing the storage files.
  // dls_storage_rebuild() is called from KernelMain during boot.
  s_initializing_storage = true;

  // Iterate through all files in the file system, looking for DLS storage files by name
  PFSFileListEntry *dir_list = pfs_create_file_list(prv_filename_filter_cb);

  // Create a session for each entry
  PFSFileListEntry *head = dir_list;
  int num_sessions_restored = 0;
  while (head) {
    DataLoggingSession *session = NULL;

    int fd = pfs_open(head->name, OP_FLAG_READ | OP_FLAG_WRITE, FILE_TYPE_STATIC,
                      DLS_FILE_INIT_SIZE_BYTES);
    if (fd < S_SUCCESS) {
      PBL_LOG(LOG_LEVEL_ERROR, "Error %d opening file %s", fd, head->name);
      pfs_remove(head->name);
      goto bad_session;
    }

    // Get the session info
    DLSFileHeader hdr;
    if (!prv_pfs_read(fd, &hdr, sizeof(hdr))) {
      pfs_close(fd);
      goto bad_session;
    }
    pfs_close(fd);

    // Create a new session based on the file info
    session = dls_list_create_session(hdr.tag, hdr.item_type, hdr.item_size, &hdr.app_uuid,
                                      hdr.timestamp, DataLoggingStatusInactive);
    if (!session) {
      goto bad_session;
    }
    session->comm.session_id = hdr.comm_session_id;
    session->storage = (DataLoggingSessionStorage) {
      .fd = DLS_INVALID_FILE,
      .write_offset = sizeof(hdr),
      .read_offset = sizeof(hdr)
    };

    // Make sure the filename is what we expect
    prv_get_filename(name, session);
    if (strncmp(name, head->name, DLS_FILE_NAME_MAX_LEN)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Expected name of %s, got %s", head->name, name);
      pfs_remove(head->name);
      goto bad_session;
    }

    // We need to figure out how many bytes of data are unread and the offset of the
    // last byte of data (which becomes the write offset). We pass NULL into the buffer argument
    // of dls_storage_read() to tell it to compute these for us.
    uint32_t write_offset;
    int32_t num_bytes = dls_storage_read(session, NULL, 0 /*numbytes*/, &write_offset);
    if (num_bytes < 0) {
      goto bad_session;
    }
    session->storage.num_bytes = num_bytes;
    session->storage.write_offset = write_offset;

    // To update the read offset, we pass 0 as num_bytes into dls_storage_consume()
    if (dls_storage_consume(session, 0) < 0) {
      goto bad_session;
    }

    PBL_LOG(LOG_LEVEL_INFO,
            "Restored session %"PRIu8
            " num_bytes:%"PRIu32", read_offset:%"PRIu32", write_offset:%"PRIu32,
            session->comm.session_id, session->storage.num_bytes,
            session->storage.read_offset, session->storage.write_offset);

    // Insert this session into our list
    dls_list_insert_session(session);
    head = (PFSFileListEntry *)head->list_node.next;

    if (head) {
      // This operation can take awhile and tends to starve out other threads while it's on going.
      // It typically takes 100-200ms to restore a session, so if you have a lot of sessions you
      // can take 2-4 seconds to do. The KernelMain task_watchdog isn't a problem at this time
      // because we haven't started monitoring it yet, but if we starve KernelBG we'll hit false
      // watchdog reboots. Sleep a bit here so the background task has a chance to run. See
      // PBL-24560 for a long term fix.
      psleep(10);
    }

    num_sessions_restored++;
    continue;

bad_session:
    pfs_remove(head->name);
    kernel_free(session);
    head = (PFSFileListEntry *)head->list_node.next;
  }

  PBL_LOG(LOG_LEVEL_INFO, "Restored %d sessions. Total %"PRIu32" bytes allocated",
          num_sessions_restored, prv_get_total_file_system_bytes());

  // Free the directory list
  pfs_delete_file_list(dir_list);

  // No longer in initialization. From now on, only KernelBG can use the session storage calls.
  s_initializing_storage = false;
}


// -------------------------------------------------------------------------------------------------
// Analytics
static bool prv_max_numbytes_cb(DataLoggingSession* session, void *data) {
  uint32_t *max_bytes = (uint32_t *)data;
  *max_bytes = MAX(*max_bytes, session->storage.num_bytes);
  return true;
}

void analytics_external_collect_dls_stats(void) {
  uint32_t max_bytes = 0;
  dls_list_for_each_session(prv_max_numbytes_cb, &max_bytes);
  analytics_set(ANALYTICS_DEVICE_METRIC_DATA_LOGGING_MAX_SPOOLED_BYTES, max_bytes,
                AnalyticsClient_System);
}
