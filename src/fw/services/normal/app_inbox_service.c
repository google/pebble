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

#include "app_inbox_service.h"

#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "process_management/process_manager.h"
#include "os/mutex.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/buffer.h"
#include "util/list.h"

typedef struct AppInboxNode {
  ListNode node;
  AppInboxServiceTag tag;
  AppInboxMessageHandler message_handler;
  AppInboxDroppedHandler dropped_handler;
  PebbleTask event_handler_task;

  //! Indicates whether there is a writer.
  //! The writer can set it to anything they want, mostly for debugging purposes.
  void *writer;
  bool write_failed;
  bool has_pending_event;

  uint32_t num_failed;
  uint32_t num_success;

  struct {
    //! The size of `storage`.
    size_t size;

    //! The positive offset relative relative to write_index, up until which the current
    //! (incomplete) message has been written.
    size_t current_offset;

    //! Index after which the current message should get written.
    //! If this index is non-zero, there are completed message(s) in the buffer.
    size_t write_index;

    ///! Pointer to the beginning of the storage.
    uint8_t *storage;
  } buffer;
} AppInboxNode;

typedef struct AppInboxConsumerInfo {
  AppInboxServiceTag tag;
  AppInboxMessageHandler message_handler;
  AppInboxDroppedHandler dropped_handler;
  uint32_t num_failed;
  uint32_t num_success;
  uint8_t *it;
  uint8_t *end;
} AppInboxConsumerInfo;


_Static_assert(sizeof(AppInboxServiceTag) <= sizeof(void *),
               "AppInboxServiceTag should fit inside a void *");

static AppInboxNode *s_app_inbox_head;

static PebbleRecursiveMutex *s_app_inbox_mutex;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Declarations of permitted handlers:

extern void app_message_receiver_message_handler(const uint8_t *data, size_t length,
                                                 AppInboxConsumerInfo *consumer_info);
extern void app_message_receiver_dropped_handler(uint32_t num_dropped_messages);

#ifdef UNITTEST
extern void test_message_handler(const uint8_t *data, size_t length,
                                 AppInboxConsumerInfo *consumer_info);
extern void test_dropped_handler(uint32_t num_dropped_messages);
extern void test_alt_message_handler(const uint8_t *data, size_t length,
                                     AppInboxConsumerInfo *consumer_info);
extern void test_alt_dropped_handler(uint32_t num_dropped_messages);
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// Syscalls

static AppInboxServiceTag prv_tag_for_event_handlers(const AppInboxMessageHandler message_handler,
                                                     const AppInboxDroppedHandler dropped_handler) {
  static const struct {
    AppInboxMessageHandler message_handler;
    AppInboxDroppedHandler dropped_handler;
  } s_event_handler_map[] = {
    [AppInboxServiceTagAppMessageReceiver] = {
      .message_handler = app_message_receiver_message_handler,
      .dropped_handler = app_message_receiver_dropped_handler,
    },
#ifdef UNITTEST
    [AppInboxServiceTagUnitTest] = {
      .message_handler = test_message_handler,
      .dropped_handler = test_dropped_handler,
    },
    [AppInboxServiceTagUnitTestAlt] = {
      .message_handler = test_alt_message_handler,
      .dropped_handler = test_alt_dropped_handler,
    }
#endif
  };
  for (AppInboxServiceTag tag = 0; tag < NumAppInboxServiceTag; ++tag) {
    if (s_event_handler_map[tag].message_handler == message_handler &&
        s_event_handler_map[tag].dropped_handler == dropped_handler) {
      return tag;
    }
  }
  return AppInboxServiceTagInvalid;
}

DEFINE_SYSCALL(bool, sys_app_inbox_service_register, uint8_t *storage, size_t storage_size,
               AppInboxMessageHandler message_handler, AppInboxDroppedHandler dropped_handler) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(storage, storage_size);
  }
  const AppInboxServiceTag service_tag = prv_tag_for_event_handlers(message_handler,
                                                                    dropped_handler);
  if (AppInboxServiceTagInvalid == service_tag) {
    PBL_LOG(LOG_LEVEL_ERROR, "AppInbox event handlers not allowed <0x%"PRIx32", 0x%"PRIx32">",
            // Ugh.. no more format signature slots free for %p %p...
            (uint32_t)(uintptr_t)message_handler, (uint32_t)(uintptr_t)dropped_handler);
    syscall_failed();
  }

  return app_inbox_service_register(storage, storage_size,
                                    message_handler, dropped_handler, service_tag);
}

DEFINE_SYSCALL(uint32_t, sys_app_inbox_service_unregister, uint8_t *storage) {
  // No check is needed on the value of `storage `, we're not going to dereference it.
  return app_inbox_service_unregister_by_storage(storage);
}

static bool prv_get_consumer_info(AppInboxServiceTag tag, AppInboxConsumerInfo *info_in_out);

DEFINE_SYSCALL(bool, sys_app_inbox_service_get_consumer_info,
               AppInboxServiceTag tag, AppInboxConsumerInfo *info_out) {
  if (PRIVILEGE_WAS_ELEVATED) {
    if (info_out) {
      syscall_assert_userspace_buffer(info_out, sizeof(*info_out));
    }
  }
  return prv_get_consumer_info(tag, info_out);
}

static void prv_consume(AppInboxConsumerInfo *consumer_info);

DEFINE_SYSCALL(void, sys_app_inbox_service_consume, AppInboxConsumerInfo *consumer_info) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(consumer_info, sizeof(*consumer_info));
  }
  prv_consume(consumer_info);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

static void prv_lock(void) {
  // Using one "global" lock for all app inboxes.
  // If needed, we could easily give each app inbox its own mutex, but it seems overkill right now.
  mutex_lock_recursive(s_app_inbox_mutex);
}

static void prv_unlock(void) {
  mutex_unlock_recursive(s_app_inbox_mutex);
}

static bool prv_list_filter_by_storage(ListNode *found_node, void *data) {
  return ((AppInboxNode *)found_node)->buffer.storage == (uint8_t *)data;
}

static AppInboxNode *prv_find_inbox_by_storage(uint8_t *storage) {
  return (AppInboxNode *) list_find((ListNode *)s_app_inbox_head,
                                    prv_list_filter_by_storage, storage);
}

static bool prv_list_filter_by_tag(ListNode *found_node, void *data) {
  return ((AppInboxNode *)found_node)->tag == (AppInboxServiceTag)(uintptr_t)data;
}

static AppInboxNode *prv_find_inbox_by_tag(AppInboxServiceTag tag) {
  return (AppInboxNode *) list_find((ListNode *)s_app_inbox_head,
                                    prv_list_filter_by_tag, (void *)(uintptr_t)tag);
}

static AppInboxNode *prv_find_inbox_by_tag_and_log_if_not_found(AppInboxServiceTag tag) {
  AppInboxNode *inbox = prv_find_inbox_by_tag(tag);
  if (!inbox) {
    PBL_LOG(LOG_LEVEL_ERROR, "No AppInbox for tag <%d>", tag);
  }
  return inbox;
}

//! We don't report "number of messages consumed", because that would force the system to parse
//! the contents of the (app space) buffer, which might have been corrupted by the app.
//! Note that it's in theory possible for a misbehaving app to pass in a consumed_up_to_ptr that is
//! mid-way in a message. If it does so, it won't crash the kernel, but it will result in delivery
//! of broken messages to the app, but it won't be our fault...
static void prv_consume(AppInboxConsumerInfo *consumer_info) {
  prv_lock();
  {
    AppInboxNode *inbox = prv_find_inbox_by_tag_and_log_if_not_found(consumer_info->tag);
    if (!inbox) {
      goto unlock;
    }
    uint8_t *const consumed_up_to_ptr = consumer_info->it;
    uint8_t * const completed_messages_end = (inbox->buffer.storage + inbox->buffer.write_index);
    if (consumed_up_to_ptr < inbox->buffer.storage ||
        consumed_up_to_ptr > completed_messages_end) {
      PBL_LOG(LOG_LEVEL_ERROR, "Out of bounds");
      goto unlock;
    }
    const size_t bytes_consumed = (consumed_up_to_ptr - inbox->buffer.storage);
    if (0 == bytes_consumed) {
      goto unlock;
    }
    uint8_t * const partial_message_end = completed_messages_end + inbox->buffer.current_offset;
    const size_t remaining_size = partial_message_end - consumed_up_to_ptr;
    consumer_info->it = inbox->buffer.storage;
    consumer_info->end = inbox->buffer.storage + remaining_size;
    if (remaining_size) {
      // New data has been written in the mean-time, move it all to the front of the buffer:
      memmove(inbox->buffer.storage, consumed_up_to_ptr, remaining_size);
    }
    inbox->buffer.write_index -= bytes_consumed;
  }
unlock:
  prv_unlock();
}

static bool prv_get_consumer_info(AppInboxServiceTag tag, AppInboxConsumerInfo *info_out) {
  if (!info_out) {
    return false;
  }
  bool success = false;
  prv_lock();
  {
    AppInboxNode *inbox = prv_find_inbox_by_tag_and_log_if_not_found(tag);
    if (!inbox) {
      goto unlock;
    }

    *info_out = (const AppInboxConsumerInfo) {
      .tag = tag,
      .message_handler = inbox->message_handler,
      .dropped_handler = inbox->dropped_handler,
      .num_failed = inbox->num_failed,
      .num_success = inbox->num_success,
      .it = inbox->buffer.storage,
      .end = inbox->buffer.storage + inbox->buffer.write_index,
    };

    // Also mark that there is no event pending any more:
    inbox->has_pending_event = false;

    // Reset counters because the info is communicated to app and it's about to consume the data.
    inbox->num_failed = 0;
    inbox->num_success = 0;

    success = true;
  }
unlock:
  prv_unlock();
  return success;
}

//! @note Executes on app task, therefore we need to go through syscalls to access AppInbox!
static void prv_callback_event_handler(void *ctx) {
  AppInboxServiceTag tag = (AppInboxServiceTag)(uintptr_t)ctx;
  AppInboxConsumerInfo info = {};
  size_t num_message_consumed = 0;
  if (!sys_app_inbox_service_get_consumer_info(tag, &info)) {
    // Inbox wasn't there any more
    return;
  }
  if (!info.message_handler) {
    // Shouldn't ever happen, but better not PBL_ASSERTN on app task
    PBL_LOG(LOG_LEVEL_ERROR, "No AppInbox message handler!");
    return;
  }
  if (!info.num_success && !info.num_failed) {
    // Shouldn't ever happen, but better not PBL_ASSERTN on app task
    PBL_LOG(LOG_LEVEL_ERROR, "Got callback, but zero messages!?");
    // fall-through
  }

  // These conditions are redundant, just for safety:
  while ((num_message_consumed < info.num_success) && (info.it < info.end)) {
    AppInboxMessageHeader *msg = (AppInboxMessageHeader *)info.it;

    // Increment now so that if the message_handler calls into sys_app_inbox_service_consume(),
    // it will be pointing *after* the message that is just handled:
    info.it += (sizeof(AppInboxMessageHeader) + msg->length);

    // Check for safety, just in case the app has corrupted the buffer in the mean time:
    if (msg->data + msg->length <= info.end) {
      info.message_handler(msg->data, msg->length, &info);
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Corrupted AppInbox message!");
    }
    ++num_message_consumed;
  }

  if (info.num_failed) {
    if (info.dropped_handler) {
      info.dropped_handler(info.num_failed);
    } else {
      PBL_LOG(LOG_LEVEL_ERROR, "Dropped %"PRIu32" messages but no dropped_handler",
              info.num_failed);
    }
  }

  // Report back up to which byte we've consumed the data.
  sys_app_inbox_service_consume(&info);
}

bool app_inbox_service_register(uint8_t *storage, size_t storage_size,
                                AppInboxMessageHandler message_handler,
                                AppInboxDroppedHandler dropped_handler, AppInboxServiceTag tag) {
  AppInboxNode *new_node = (AppInboxNode *)kernel_zalloc(sizeof(AppInboxNode));
  if (!new_node) {
    PBL_LOG(LOG_LEVEL_ERROR, "Not enough memory to allocate AppInboxNode");
    return false;
  }

  prv_lock();
  {
    bool has_error = false;

    if (prv_find_inbox_by_storage(storage)) {
      PBL_LOG(LOG_LEVEL_ERROR, "AppInbox already registered for storage <%p>", storage);
      has_error = true;
    }

    // This check effectively caps the kernel RAM impact of this service,
    // so it's not possible to abuse the syscall and cause kernel OOM.
    if (prv_find_inbox_by_tag(tag)) {
      PBL_LOG(LOG_LEVEL_ERROR, "AppInbox already registered for tag <%d>", tag);
      has_error = true;
    }

    if (has_error) {
      kernel_free(new_node);
      new_node = NULL;
    } else {
      new_node->tag = tag;
      new_node->message_handler = message_handler;
      new_node->dropped_handler = dropped_handler;
      new_node->event_handler_task = pebble_task_get_current();
      new_node->buffer.storage = storage;
      new_node->buffer.size = storage_size;
      s_app_inbox_head = (AppInboxNode *)list_prepend((ListNode *)s_app_inbox_head,
                                                      (ListNode *)new_node);
    }
  }
  prv_unlock();

  return (new_node != NULL);
}

uint32_t app_inbox_service_unregister_by_storage(uint8_t *storage) {
  uint32_t num_messages_lost = 0;
  prv_lock();
  {
    AppInboxNode *node = prv_find_inbox_by_storage(storage);
    if (node) {
      list_remove((ListNode *)node, (ListNode **)&s_app_inbox_head, NULL);
      num_messages_lost = node->num_failed + node->num_success + (node->writer ? 1 : 0);
      kernel_free(node);
    }
  }
  prv_unlock();
  return num_messages_lost;
}

void app_inbox_service_unregister_all(void) {
  prv_lock();
  {
    AppInboxNode *node = s_app_inbox_head;
    while (node) {
      AppInboxNode *next = (AppInboxNode *) node->node.next;
      kernel_free(node);
      node = next;
    }
    s_app_inbox_head = NULL;
  }
  prv_unlock();
}

static bool prv_is_inbox_being_written(AppInboxNode *inbox) {
  return (inbox->writer != NULL);
}

static size_t prv_get_space_remaining(AppInboxNode *inbox) {
  return (inbox->buffer.size - inbox->buffer.write_index - inbox->buffer.current_offset);
}

bool prv_check_space_remaining(AppInboxNode *inbox, size_t required_free_length) {
  const size_t space_remaining = prv_get_space_remaining(inbox);
  if (required_free_length > space_remaining) {
    PBL_LOG(LOG_LEVEL_ERROR, "Dropping data, not enough space %"PRIu32" vs %"PRIu32,
            (uint32_t)required_free_length, (uint32_t)space_remaining);
    return false;
  }
  return true;
}

static void prv_send_event_if_needed(AppInboxNode *inbox) {
  if (!inbox || inbox->has_pending_event) {
    return;
  }
  PebbleEvent event = {
    .type = PEBBLE_CALLBACK_EVENT,
    .callback = {
      .callback = prv_callback_event_handler,
      .data = (void *)(uintptr_t) inbox->tag,
    },
  };
  const bool is_event_enqueued = process_manager_send_event_to_process(inbox->event_handler_task,
                                                                       &event);
  if (!is_event_enqueued) {
    PBL_LOG(LOG_LEVEL_ERROR, "Event queue full");
  }
  inbox->has_pending_event = is_event_enqueued;
}

static void prv_mark_failed_if_no_writer(AppInboxNode *inbox) {
  if (!inbox->writer) {
    // See PBL-41464
    // App message has been reset (closed and opened again) while a message was being received.
    // Fail it because our state got lost.
    inbox->write_failed = true;
  }
}

bool app_inbox_service_begin(AppInboxServiceTag tag, size_t required_free_length, void *writer) {
  if (!writer) {
    return false;
  }
  bool success = false;
  prv_lock();
  {
    AppInboxNode *inbox = prv_find_inbox_by_tag_and_log_if_not_found(tag);
    if (!inbox) {
      goto unlock;
    }
    if (prv_is_inbox_being_written(inbox)) {
      ++inbox->num_failed;
      PBL_LOG(LOG_LEVEL_ERROR, "Dropping data, already written by <%p>", inbox->writer);
      // Don't send event here, when the current write finishes, the drop(s) will be reported too.
      goto unlock;
    }
    if (!prv_check_space_remaining(inbox, required_free_length + sizeof(AppInboxMessageHeader))) {
      ++inbox->num_failed;
      // If it doesn't fit, send event immediately, we don't know when the next write will happen.
      prv_send_event_if_needed(inbox);
      goto unlock;
    }

    inbox->writer = writer;
    inbox->write_failed = false;
    // Leave space at the beginning for the header, which we'll write in the end
    inbox->buffer.current_offset = sizeof(AppInboxMessageHeader);
    success = true;
  }
unlock:
  prv_unlock();
  return success;
}

bool app_inbox_service_write(AppInboxServiceTag tag, const uint8_t *data, size_t length) {
  bool success = false;
  prv_lock();
  {
    AppInboxNode *inbox = prv_find_inbox_by_tag_and_log_if_not_found(tag);
    if (!inbox) {
      goto unlock;
    }
    prv_mark_failed_if_no_writer(inbox);
    if (inbox->write_failed) {
      goto unlock;
    }
    if (!prv_check_space_remaining(inbox, length)) {
      inbox->write_failed = true;
      goto unlock;
    }
    memcpy(inbox->buffer.storage + inbox->buffer.write_index + inbox->buffer.current_offset,
           data, length);
    inbox->buffer.current_offset += length;
    success = true;
  }
unlock:
  prv_unlock();
  return success;
}

static void prv_finish(AppInboxNode *inbox) {
  inbox->writer = NULL;
  inbox->buffer.current_offset = 0;
}

void app_inbox_service_init(void) {
  s_app_inbox_mutex = mutex_create_recursive();
}

bool app_inbox_service_end(AppInboxServiceTag tag) {
  bool success = false;
  prv_lock();
  {
    AppInboxNode *inbox = prv_find_inbox_by_tag_and_log_if_not_found(tag);
    if (!inbox) {
      goto unlock;
    }
    prv_mark_failed_if_no_writer(inbox);
    if (inbox->write_failed) {
      ++inbox->num_failed;
    } else {
      const AppInboxMessageHeader header = (const AppInboxMessageHeader) {
        .length = inbox->buffer.current_offset - sizeof(AppInboxMessageHeader),
        // Fill with something that might aid debugging one day:
        .padding = { 0xaa, 0xaa, 0xaa, 0xaa },
      };
      memcpy(inbox->buffer.storage + inbox->buffer.write_index, &header, sizeof(header));
      inbox->buffer.write_index += inbox->buffer.current_offset;
      ++inbox->num_success;
      success = true;
    }
    prv_finish(inbox);

    prv_send_event_if_needed(inbox);
  }
unlock:
  prv_unlock();
  return success;
}

void app_inbox_service_cancel(AppInboxServiceTag tag) {
  prv_lock();
  {
    AppInboxNode *inbox = prv_find_inbox_by_tag_and_log_if_not_found(tag);
    if (!inbox) {
      goto unlock;
    }
    prv_finish(inbox);
  }
unlock:
  prv_unlock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Unit Test Interfaces

bool app_inbox_service_has_inbox_for_tag(AppInboxServiceTag tag) {
  bool has_inbox;
  prv_lock();
  has_inbox = (prv_find_inbox_by_tag(tag) != NULL);
  prv_unlock();
  return has_inbox;
}

bool app_inbox_service_has_inbox_for_storage(uint8_t *storage) {
  bool has_inbox;
  prv_lock();
  has_inbox = (prv_find_inbox_by_storage(storage) != NULL);
  prv_unlock();
  return has_inbox;
}

bool app_inbox_service_is_being_written_for_tag(AppInboxServiceTag tag) {
  bool is_written = false;
  prv_lock();
  AppInboxNode *inbox = prv_find_inbox_by_tag(tag);
  if (inbox) {
    is_written = (inbox->writer != NULL);
  }
  prv_unlock();
  return is_written;
}

uint32_t app_inbox_service_num_failed_for_tag(AppInboxServiceTag tag) {
  uint32_t num_failed = 0;
  prv_lock();
  AppInboxNode *inbox = prv_find_inbox_by_tag(tag);
  if (inbox) {
    num_failed = inbox->num_failed;
  }
  prv_unlock();
  return num_failed;
}

uint32_t app_inbox_service_num_success_for_tag(AppInboxServiceTag tag) {
  uint32_t num_success = 0;
  prv_lock();
  AppInboxNode *inbox = prv_find_inbox_by_tag(tag);
  if (inbox) {
    num_success = inbox->num_success;
  }
  prv_unlock();
  return num_success;
}
