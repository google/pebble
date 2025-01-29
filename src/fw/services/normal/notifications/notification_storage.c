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

#include "notification_storage.h"
#include "notification_storage_private.h"

#include "util/uuid.h"
#include "kernel/pbl_malloc.h"
#include "services/normal/filesystem/pfs.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "system/logging.h"
#include "os/mutex.h"
#include "system/passert.h"
#include "util/iterator.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

typedef struct NotificationIterState {
  SerializedTimelineItemHeader header;
  int fd;
  TimelineItem notification;
} NotificationIterState;

static const char *FILENAME = "notifstr";     //The filename should not be changed

static PebbleRecursiveMutex *s_notif_storage_mutex = NULL;

static uint32_t s_write_offset;

static bool prv_iter_next(NotificationIterState *iter_state);
static bool prv_get_notification(TimelineItem *notification,
    SerializedTimelineItemHeader *header, int fd);
static void prv_set_header_status(SerializedTimelineItemHeader *header, uint8_t status, int fd);

void notification_storage_init(void) {
  PBL_ASSERTN(s_notif_storage_mutex == NULL);

  //Clear notifications storage on reset
  pfs_remove(FILENAME);
  // Create a new file and close it (removes delay when receiving first notification after boot)
  int fd = pfs_open(FILENAME, OP_FLAG_WRITE, FILE_TYPE_STATIC, NOTIFICATION_STORAGE_FILE_SIZE);
  if (fd < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error opening file %d", fd);
  } else {
    pfs_close(fd);
  }
  s_write_offset = 0;
  s_notif_storage_mutex = mutex_create_recursive();
}

void notification_storage_lock(void) {
  mutex_lock_recursive(s_notif_storage_mutex);
}

void notification_storage_unlock(void) {
  mutex_unlock_recursive(s_notif_storage_mutex);
}

static int prv_file_open(uint8_t op_flags) {
  notification_storage_lock();
  int fd = pfs_open(FILENAME, op_flags, FILE_TYPE_STATIC, NOTIFICATION_STORAGE_FILE_SIZE);
  if (fd < 0) {
    // If a write operation or a read operation fails with anything other than "does not exist",
    // log error
    bool read_only =
        ((op_flags & (OP_FLAG_WRITE | OP_FLAG_OVERWRITE | OP_FLAG_READ)) == OP_FLAG_READ);
    if ((!read_only) || (fd != E_DOES_NOT_EXIST)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Error opening file %d", fd);
      // Remove file so next open will create a new one (notification storage trashed)
      pfs_remove(FILENAME);
    }
    notification_storage_unlock();
  }
  return fd;
}

static void prv_file_close(int fd) {
  pfs_close(fd);
  notification_storage_unlock();
}

static int prv_write_notification(TimelineItem *notification,
    SerializedTimelineItemHeader *header, int fd) {
  int bytes_written = 0;

  // Invert flags & status to store on flash
  header->common.flags = ~header->common.flags;
  header->common.status = ~header->common.status;

  int result = pfs_write(fd, (uint8_t *) header, sizeof(*header));

  // Restore flags & status
  header->common.flags = ~header->common.flags;
  header->common.status = ~header->common.status;

  if (result < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error writing notification header %d", result);
    return result;
  }
  bytes_written += result;

  if (!header->payload_length) {
    return result;
  }

  uint8_t *write_buffer = kernel_malloc_check(header->payload_length);

  timeline_item_serialize_payload(notification, write_buffer, header->payload_length);
  result = pfs_write(fd, write_buffer, header->payload_length);
  if (result < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error writing notification payload %d", result);
    kernel_free(write_buffer);
    return result;
  }
  bytes_written += result;

  kernel_free(write_buffer);

  return bytes_written;
}

//! Iterate over notifications space and mark the oldest notifications as deleted until we have
//! enough space available
static void prv_reclaim_space(size_t size_needed, int fd) {
  size_needed = ((size_needed / NOTIFICATION_STORAGE_MINIMUM_INCREMENT_SIZE) + 1) *
      NOTIFICATION_STORAGE_MINIMUM_INCREMENT_SIZE; // Free up space size in blocks
  size_t size_available = 0;
  NotificationIterState iter_state = {
      .fd = fd,
  };
  Iterator iter;
  iter_init(&iter, (IteratorCallback)&prv_iter_next, NULL, &iter_state);
  while (iter_next(&iter)) {
    uint8_t status = iter_state.header.common.status;
    if (!(status & TimelineItemStatusDeleted)) {
      // Mark for deletion
      prv_set_header_status(&iter_state.header, TimelineItemStatusDeleted, fd);
      size_available += sizeof(SerializedTimelineItemHeader) + iter_state.header.payload_length;
      if (size_needed <= size_available) {
        return;
      }
    }
    if (pfs_seek(fd, iter_state.header.payload_length, FSeekCur) < 0) {
      break;
    }
  }
}

//! Check whether there exists @ref size_needed available space in storage after compression
static bool prv_is_storage_full(size_t size_needed, size_t *size_available, int fd) {
  *size_available = 0;
  NotificationIterState iter_state = {
      .fd = fd,
  };
  Iterator iter;
  iter_init(&iter, (IteratorCallback)&prv_iter_next, NULL, &iter_state);
  while (iter_next(&iter)) {
    // Check header status to detect deleted notifications. Add size of deleted notifications
    uint8_t status = iter_state.header.common.status;
    if (status & TimelineItemStatusDeleted) {
      *size_available += sizeof(SerializedTimelineItemHeader) + iter_state.header.payload_length;
      if (size_needed <= *size_available) {
        return false;
      }
    }
    if (pfs_seek(fd, iter_state.header.payload_length, FSeekCur) < 0) {
      break;
    }
  }
  return true;
}

//! Compress storage by reading all valid notifications out of old file and storing to new file
//! via overwrite
static bool prv_compress(size_t size_needed, int *fd) {
  pfs_seek(*fd, 0, FSeekSet);

  //Open file for overwrite
  int new_fd = pfs_open(FILENAME, OP_FLAG_OVERWRITE, FILE_TYPE_STATIC,
      NOTIFICATION_STORAGE_FILE_SIZE);
  if (new_fd < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error opening new file for compression %d", new_fd);
    return false;
  }

  // Delete old notifications if there is no space left in storage
  size_t size_available;
  if (prv_is_storage_full(size_needed, &size_available, *fd)) {
    pfs_seek(*fd, 0, FSeekSet);
    prv_reclaim_space(size_needed - size_available, *fd);
  }
  pfs_seek(*fd, 0, FSeekSet);

  int write_offset = 0;

  // Iterate over notifications stored and write to new file
  NotificationIterState iter_state = {
      .fd = *fd,
  };
  Iterator iter;
  iter_init(&iter, (IteratorCallback)&prv_iter_next, NULL, &iter_state);
  while (iter_next(&iter)) {
    // Check header flags to detect deleted notifications
    uint8_t status = iter_state.header.common.status;
    if (status & TimelineItemStatusDeleted) {
      // Skip over deleted notification
      pfs_seek(*fd, iter_state.header.payload_length, FSeekCur);
      continue;
    }

    TimelineItem notification;
    if (!prv_get_notification(&notification, &iter_state.header, *fd)) {
      // Error occurred
      goto cleanup;
    }
    int result = prv_write_notification(&notification, &iter_state.header, new_fd);
    if (result < 0) {
      // Error occurred
      kernel_free(notification.allocated_buffer);
      goto cleanup;
    }
    write_offset += result;
    kernel_free(notification.allocated_buffer);
  }

  s_write_offset = write_offset;

  pfs_close(*fd);
  pfs_close(new_fd);

  *fd = pfs_open(FILENAME, OP_FLAG_READ | OP_FLAG_WRITE, FILE_TYPE_STATIC,
      NOTIFICATION_STORAGE_FILE_SIZE);
  if (*fd < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error re-opening after compression %d", new_fd);
    return false;
  }

  return true;

cleanup:
  pfs_close(*fd);
  pfs_close(new_fd);
  return false;
}

void notification_storage_store(TimelineItem* notification) {
  PBL_ASSERTN(s_notif_storage_mutex != NULL);
  PBL_ASSERTN(notification != NULL);

  SerializedTimelineItemHeader header = { .common.id = UUID_INVALID };
  timeline_item_serialize_header(notification, &header);

  int fd = prv_file_open(OP_FLAG_WRITE | OP_FLAG_READ);
  if (fd < 0) {
    return;
  }
  size_t size_needed = header.payload_length + sizeof(SerializedTimelineItemHeader);
  if (size_needed > (NOTIFICATION_STORAGE_FILE_SIZE - s_write_offset)) {
    if (!prv_compress(size_needed, &fd)) {
      // Notification storage compression failed. Clear notifications storage
      goto reset_storage;
    }
  }

  pfs_seek(fd, s_write_offset, FSeekSet);

  int result = prv_write_notification(notification, &header, fd);
  if (result < 0) {
    // [AS] TODO: Write failure: reset storage, compression or reset watch?
    goto reset_storage;
  }

  s_write_offset += result;

  prv_file_close(fd);
  return;

reset_storage:
  mutex_unlock_recursive(s_notif_storage_mutex);
  notification_storage_reset_and_init();
}

// Finds the next match in the notification storage file from the current position
// Position in file will be at the start of notification payload if return value is true
static bool prv_find_next_notification(SerializedTimelineItemHeader* header,
    bool (*compare_func)(SerializedTimelineItemHeader* header, void* data), void* data, int fd) {
  for (;;) {
    int result = pfs_read(fd, (uint8_t *)header, sizeof(*header));

    // Restore flags & status
    header->common.flags = ~header->common.flags;
    header->common.status = ~header->common.status;

    if ((result < 0) || (uuid_is_invalid(&header->common.id))) {
      break;
    }

    uint8_t status = header->common.status;
    if ((status & TimelineItemStatusUnused) ||
        (header->common.type >= TimelineItemTypeOutOfRange) ||
        (header->common.layout >= NumLayoutIds)) {
      pfs_close(fd);
      notification_storage_reset_and_init();
      PBL_LOG(LOG_LEVEL_ERROR, "Notification storage corrupt. Resetting...");
      break;
    }

    if (!(status & TimelineItemStatusDeleted)) {
      // Only check compare notifications if it is not deleted, otherwise skip it and look for
      // the next match
      if (compare_func(header, data)) {
        return true;
      }
    }

    if (pfs_seek(fd, header->payload_length, FSeekCur) < 0) {
      break;
    }
  }

  return false;
}

static bool prv_uuid_equal_func(SerializedTimelineItemHeader *header, void *data) {
  Uuid *uuid = (Uuid *)data;
  return uuid_equal(&header->common.id, uuid);
}

static bool prv_ancs_id_compare_func(SerializedTimelineItemHeader* header, void* data) {
  uint32_t ancs_uid = (uint32_t) data;
  return header->common.ancs_uid == ancs_uid;
}

bool notification_storage_notification_exists(const Uuid *id) {
  int fd = prv_file_open(OP_FLAG_READ);
  if (fd < 0) {
    return false;
  }

  SerializedTimelineItemHeader header = { .common.id = UUID_INVALID };
  bool found = prv_find_next_notification(&header, prv_uuid_equal_func, (void *)id, fd);

  prv_file_close(fd);

  return found;
}

static bool prv_get_notification(TimelineItem *notification,
    SerializedTimelineItemHeader* header, int fd) {

  notification->allocated_buffer = NULL; // Must be initialized in case this goes to cleanup
  // Read notification to temporary buffer
  uint8_t *read_buffer = task_zalloc_check(header->payload_length);

  int result = pfs_read(fd, read_buffer, header->payload_length);
  if (result < 0) {
    goto cleanup;
  }

  if (!timeline_item_deserialize_item(notification, header, read_buffer)) {
    goto cleanup;
  }

  task_free(read_buffer);
  return true;

cleanup:
  task_free(read_buffer);
  return false;
}

size_t notification_storage_get_len(const Uuid *uuid) {
  int fd = prv_file_open(OP_FLAG_READ);
  if (fd < 0) {
    return 0;
  }

  size_t size = 0;
  SerializedTimelineItemHeader header = { .common.id = UUID_INVALID };
  if (prv_find_next_notification(&header, prv_uuid_equal_func, (void *) uuid, fd)) {
    size = header.payload_length + sizeof(SerializedTimelineItemHeader);
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "notification not found");
  }

  prv_file_close(fd);
  return (size);
}

bool notification_storage_get(const Uuid *id, TimelineItem *item_out) {
  PBL_ASSERTN(item_out && (s_notif_storage_mutex != NULL));

  int fd = prv_file_open(OP_FLAG_READ);
  if (fd < 0) {
    return false;
  }

  bool rv = true;

  SerializedTimelineItemHeader header = { .common.id = UUID_INVALID };
  char uuid_string[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(id, uuid_string);
  if (!prv_find_next_notification(&header, prv_uuid_equal_func, (void *) id, fd)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "notification not found, %s", uuid_string);
    rv = false;
  } else {

    if (!prv_get_notification(item_out, &header, fd)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Could not retrieve notification with id %s and size %u",
          uuid_string, header.payload_length);
      rv = false;
    }
  }

  prv_file_close(fd);

  return rv;
}

//! @return is_advanced
static bool prv_iter_next(NotificationIterState *iter_state) {

  int result = pfs_read(iter_state->fd, (uint8_t *)&iter_state->header, sizeof(iter_state->header));

  // Restore flags & status
  iter_state->header.common.flags = ~iter_state->header.common.flags;
  iter_state->header.common.status = ~iter_state->header.common.status;

  if ((result == E_RANGE) || (uuid_is_invalid(&iter_state->header.common.id))) {
    //End iteration if we have reached the end of the file or the header ID is invalid (erased flash)
    return false;
  } else if (result < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error reading notification header while iterating %d", result);
    return false;
  }

  return true;
}

static bool prv_rewrite_iter_next(NotificationIterState *iter_state) {
  int result = 0;

  result = pfs_read(iter_state->fd, (uint8_t*)&iter_state->header, sizeof(iter_state->header));

  // Restore flags & status
  iter_state->header.common.flags = ~iter_state->header.common.flags;
  iter_state->header.common.status = ~iter_state->header.common.status;

  if ((result == E_RANGE) || (uuid_is_invalid(&iter_state->header.common.id))) {
    return false;
  } else if (result < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error reading notification header while iterating %d", result);
    return false;
  }

  if (iter_state->header.common.status & TimelineItemStatusDeleted) {
    return true;
  }
  return prv_get_notification(&iter_state->notification, &iter_state->header, iter_state->fd);
}

static void prv_set_header_status(SerializedTimelineItemHeader *header, uint8_t status, int fd) {
  // Seek to the status field
  pfs_seek(fd, (-(int)sizeof(*header) +
      (int)offsetof(CommonTimelineItemHeader, status)),
      FSeekCur);

  // Invert flags & status to store on flash
  status = ~status;

  int result = pfs_write(fd, (uint8_t *)&status, sizeof(header->common.status));
  if (result < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Error writing status to notification header %d", result);
  }

  // Seek to the end of the header
  pfs_seek(fd, ((int)sizeof(*header) - (int)offsetof(CommonTimelineItemHeader, status) -
      sizeof(header->common.status)), FSeekCur);
}

bool notification_storage_get_status(const Uuid *id, uint8_t *status) {
  int fd = prv_file_open(OP_FLAG_READ);
  bool rv = false;
  if (fd < 0) {
    return rv;
  }

  SerializedTimelineItemHeader header = { .common.id = UUID_INVALID };
  if (prv_find_next_notification(&header, prv_uuid_equal_func, (void *) id, fd)) {
    *status = header.common.status;
    rv = true;
  }

  prv_file_close(fd);
  return rv;
}

void notification_storage_set_status(const Uuid *id, uint8_t status) {
  PBL_ASSERTN(s_notif_storage_mutex != NULL);

  SerializedTimelineItemHeader header = { .common.id = UUID_INVALID };

  int fd = prv_file_open(OP_FLAG_READ | OP_FLAG_WRITE);
  if (fd < 0) {
    return;
  }

  if (prv_find_next_notification(&header, prv_uuid_equal_func, (void *) id, fd)) {
    prv_set_header_status(&header, status, fd);
  }

  prv_file_close(fd);
}

void notification_storage_remove(const Uuid *id) {
  notification_storage_set_status(id, TimelineItemStatusDeleted);
}

bool notification_storage_find_ancs_notification_id(uint32_t ancs_uid, Uuid *uuid_out) {
  PBL_ASSERTN(s_notif_storage_mutex != NULL);

  int fd = prv_file_open(OP_FLAG_READ);
  if (fd < 0) {
    return false;
  }

  SerializedTimelineItemHeader header = { .common.id = UUID_INVALID };

  // Find the most recent notification which matches this ANCS UID - this will be the last entry in
  // the db. iOS can reset ANCS UIDs on reconnect, so we want to avoid finding an old notification
  bool found = false;
  while (prv_find_next_notification(&header, prv_ancs_id_compare_func,
                                    (void *)(uintptr_t) ancs_uid, fd)) {
    found = true;
    *uuid_out = header.common.id;

    // Seek to the end of this item's payload (start of the next item)
    if (pfs_seek(fd, header.payload_length, FSeekCur) < 0) {
      break;
    }
  }

  prv_file_close(fd);

  return found;
}

static bool prv_compare_ancs_notifications(TimelineItem *notification, const uint8_t *payload,
    size_t payload_size, SerializedTimelineItemHeader *header, int fd) {

  if ((notification->header.timestamp != header->common.timestamp) ||
      (notification->header.layout != header->common.layout) ||
      (header->payload_length != payload_size)) {
    return false;
  }

  bool found = false;
  if (header->payload_length == payload_size) {
    uint8_t *read_buffer = kernel_malloc_check(payload_size);

    int result = pfs_read(fd, read_buffer, payload_size);
    if (result < 0) {
      kernel_free(read_buffer);
      return false;
    }

    //Seek back to the end of the header so that the next iterator seek finds the next record
    pfs_seek(fd, -payload_size, FSeekCur);

    found = (memcmp(payload, read_buffer, payload_size) == 0);
    kernel_free(read_buffer);
  }
  return found;
}

bool notification_storage_find_ancs_notification_by_timestamp(
    TimelineItem *notification, CommonTimelineItemHeader *header_out) {

  PBL_ASSERTN(s_notif_storage_mutex && notification && header_out);

  int fd = prv_file_open(OP_FLAG_READ);
  if (fd < 0) {
    return false;
  }

  //Serialize notification attributes and actions for easy comparison
  size_t payload_size = timeline_item_get_serialized_payload_size(notification);
  uint8_t *payload = kernel_malloc_check(payload_size);
  timeline_item_serialize_payload(notification, payload, payload_size);

  //Iterate over all records until a match is found
  bool rv = false;
  NotificationIterState iter_state = {
      .fd = fd,
  };
  Iterator iter;
  iter_init(&iter, (IteratorCallback)&prv_iter_next, NULL, &iter_state);
  while (iter_next(&iter)) {
    // Check header flags to detect deleted notifications
    uint8_t status = iter_state.header.common.status;
    if (!(status & TimelineItemStatusDeleted)) {
      if (prv_compare_ancs_notifications(notification, payload, payload_size, &iter_state.header,
          fd)) {
        *header_out = iter_state.header.common;
        rv = true;
        break;
      }
    }
    int result = pfs_seek(iter_state.fd, iter_state.header.payload_length, FSeekCur);
    if (result < 0) {
      break;
    }
  }

  kernel_free(payload);

  prv_file_close(fd);

  return rv;
}

void notification_storage_rewrite(void (*iter_callback)(TimelineItem *notification,
    SerializedTimelineItemHeader *header, void *data), void *data) {

  PBL_ASSERTN(s_notif_storage_mutex != NULL);

  if (iter_callback == NULL) {
    return;
  }

  int fd = prv_file_open(OP_FLAG_READ | OP_FLAG_WRITE);
  if (fd < 0) {
    return;
  }

  int new_fd = pfs_open(FILENAME, OP_FLAG_OVERWRITE | OP_FLAG_READ, FILE_TYPE_STATIC,
      NOTIFICATION_STORAGE_FILE_SIZE);
  if (new_fd < 0) {
    prv_file_close(fd);
    return;
  }

  Iterator iter;
  NotificationIterState iter_state = {
    .fd = fd
  };
  iter_init(&iter, (IteratorCallback)prv_rewrite_iter_next, NULL, &iter_state);

  while (iter_next(&iter)) {
    uint8_t status = iter_state.header.common.status;
    if (!(status & TimelineItemStatusDeleted)) {
      iter_callback(&iter_state.notification, &iter_state.header, data);
    }
    prv_write_notification(&iter_state.notification, &iter_state.header, new_fd);
  }

  // Close the old file
  prv_file_close(fd);

  // We have to close and reopened the new file pointed to by the file descriptor, so
  // that it's temp flag is cleared.
  pfs_close(new_fd);
  new_fd = prv_file_open(OP_FLAG_READ | OP_FLAG_WRITE);
  // Finally, close that new file as this fd is only known here
  prv_file_close(new_fd);
}

void notification_storage_iterate(bool (*iter_callback)(void *data,
    SerializedTimelineItemHeader *header), void *data) {

  PBL_ASSERTN(s_notif_storage_mutex != NULL);

  if (iter_callback == NULL) {
    return;
  }

  int fd = prv_file_open(OP_FLAG_READ);
  if (fd < 0) {
    return;
  }

  Iterator iter;
  NotificationIterState iter_state = {
    .fd = fd
  };

  iter_init(&iter, (IteratorCallback)prv_iter_next, NULL, &iter_state);

  while (iter_next(&iter)) {
    uint8_t status = iter_state.header.common.status;
    if (!(status & TimelineItemStatusDeleted)) {
      if (!iter_callback(data, &iter_state.header)) {
        break;
      }
    }
    int result = pfs_seek(iter_state.fd, iter_state.header.payload_length, FSeekCur);
    if (result < 0) {
      break;
    }
  }

  prv_file_close(fd);
}

void notification_storage_reset_and_init(void) {
  notification_storage_lock();
  pfs_remove(FILENAME);
  s_write_offset = 0;
  notification_storage_unlock();
}

// Added for use by unit tests. Do not call from firmware
#if UNITTEST
void notification_storage_reset(void) {
  if (s_notif_storage_mutex != NULL) {
    mutex_destroy((PebbleMutex *) s_notif_storage_mutex);
    s_notif_storage_mutex = NULL;
  }
  notification_storage_init();
}
#endif
