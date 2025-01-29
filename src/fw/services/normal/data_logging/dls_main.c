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

#include "applib/data_logging.h"
#include "util/uuid.h"

#include "data_logging_service.h"
#include "dls_endpoint.h"
#include "dls_list.h"
#include "dls_storage.h"

#include "comm/bt_lock.h"
#include "drivers/flash.h"
#include "drivers/rtc.h"
#include "drivers/watchdog.h"
#include "flash_region/flash_region.h"
#include "kernel/pbl_malloc.h"
#include "process_management/pebble_process_md.h"
#include "process_management/process_manager.h"
#include "services/common/analytics/analytics.h"
#include "services/common/comm_session/session.h"
#include "services/common/regular_timer.h"
#include "services/common/system_task.h"
#include "syscall/syscall.h"
#include "system/logging.h"
#include "os/mutex.h"
#include "system/passert.h"
#include "kernel/util/sleep.h"
#include "util/string.h"

#include <string.h>
#include <stdlib.h>

static bool s_initialized = false;
static bool s_sends_enabled_pp = true;
static bool s_sends_enabled_run_level = true;

#define DATALOGGING_DO_FLUSH_CHECK_INTERVAL_MINUTES 5


static bool prv_sends_enabled(void) {
  return (s_sends_enabled_run_level && s_sends_enabled_pp);
}

// ----------------------------------------------------------------------------------------
//! Wrapper for data_logging_send_session that makes it usable in dls_list_for_each_session
//! @param empty_all_data A bool that indicates if the session should be force emptied
static bool prv_send_session(DataLoggingSession *logging_session, void *empty_all_data) {
  dls_private_send_session(logging_session, (bool) empty_all_data);
  return true;
}

//! @param empty_all_data A bool that indicates if the session should be force emptied
static void prv_send_all_sessions_system_task_cb(void *empty_all_data) {
  dls_list_for_each_session(prv_send_session, (void*) empty_all_data);
}


// ----------------------------------------------------------------------------------------
//! @param data unused
static void prv_check_all_sessions_timer_cb(void *data) {
  // If sends are not enabled, do nothing
  if (!prv_sends_enabled()) {
    PBL_LOG(LOG_LEVEL_INFO, "Not sending sessions because sending is disabled");
    return;
  }

  // We regularly check all our sessions to see if we have any data to send. Normally we want to
  // avoid sending the data unless there's a lot of data spooled up. This allows us to reduce the
  // number of times we have to send data for each session by batching it up into larger, fewer
  // messages. However, occasionally we do want to flush everything out.
  static int check_counter = 0;

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "send all sessions: empty %s connected %s counter %u",
      bool_to_str(check_counter == 0),
      bool_to_str(comm_session_get_system_session() != NULL),
      check_counter);

  system_task_add_callback(prv_send_all_sessions_system_task_cb,
                           (void*)(uintptr_t)(check_counter == 0));

  // force a flush every 15 minutes
  static const int EMPTY_ALL_SESSIONS_INTERVAL_MINUTES =
      15 / DATALOGGING_DO_FLUSH_CHECK_INTERVAL_MINUTES;
  check_counter = (check_counter + 1) % EMPTY_ALL_SESSIONS_INTERVAL_MINUTES;
}


// ----------------------------------------------------------------------------------------
static RegularTimerInfo prv_check_all_sessions_timer_info = {
  .cb = prv_check_all_sessions_timer_cb
};


// ----------------------------------------------------------------------------------------
static void prv_remove_logging_session(DataLoggingSession *data) {
  DataLoggingSession *logging_session = (DataLoggingSession *)data;

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Removing session %d.",
            logging_session->comm.session_id);

  dls_endpoint_close_session(logging_session->comm.session_id);
  dls_storage_delete_logging_storage(logging_session);
  dls_list_remove_session(logging_session);
}

// ----------------------------------------------------------------------------------------
// Grab the next chunk of bytes out of the session's storage and send it to the mobile
// Returns false on unexpected errors, else true
bool dls_private_send_session(DataLoggingSession *logging_session, bool empty) {
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);

  // If sends are not enabled, ignore
  if (!prv_sends_enabled()) {
    PBL_LOG(LOG_LEVEL_INFO, "Not sending session because sending is disabled");
    return true;
  }

  // Only attempt to send data out if we can communicate with the phone
  if (comm_session_get_system_session() == NULL) {
    return true;
  }

  int32_t total_bytes = logging_session->storage.num_bytes;
  bool inactive = (dls_get_session_status(logging_session) == DataLoggingStatusInactive);

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG,
            "de-logging session %"PRIu8", tag %"PRIu32
            " (inactive %s tot_bytes %"PRIu32" empty %s)",
            logging_session->comm.session_id, logging_session->tag,
            bool_to_str(inactive), total_bytes, bool_to_str(empty));

  if (inactive && (total_bytes == 0)) {
    prv_remove_logging_session(logging_session);
    return true;
  } else if (!empty && !inactive && (total_bytes < 8000)) {
    return true; // nothing to flush yet
  }

  bool success = false;
  uint8_t *buffer = kernel_malloc_check(DLS_ENDPOINT_MAX_PAYLOAD);
  unsigned int num_bytes = DLS_ENDPOINT_MAX_PAYLOAD;
  PBL_ASSERTN(logging_session->item_size <= num_bytes);
  num_bytes -= (num_bytes % logging_session->item_size);

  uint32_t new_read_offset;
  int32_t read_bytes = dls_storage_read(logging_session, buffer, num_bytes, &new_read_offset);
  if (read_bytes < 0) {
    goto exit;
  }
  PBL_ASSERTN((uint32_t)read_bytes <= DLS_ENDPOINT_MAX_PAYLOAD);

  unsigned int leftover_bytes = read_bytes % logging_session->item_size;
  if (leftover_bytes) {
    PBL_LOG(LOG_LEVEL_ERROR, "leftover bytes in the session. Flushing...");
    // remove the number of leftover bytes so we fall back on our feet
    read_bytes -= leftover_bytes;
    dls_storage_consume(logging_session, leftover_bytes);
    analytics_inc(ANALYTICS_DEVICE_METRIC_DATA_LOGGING_FLUSH_COUNT, AnalyticsClient_System);
  }

  success = dls_endpoint_send_data(logging_session, buffer, read_bytes);

exit:
  kernel_free(buffer);
  return (success);
}


// ----------------------------------------------------------------------------------------
void dls_pause(void) {
  regular_timer_remove_callback(&prv_check_all_sessions_timer_info);
}


// ----------------------------------------------------------------------------------------
void dls_resume(void) {
  regular_timer_add_multiminute_callback(&prv_check_all_sessions_timer_info,
                                         DATALOGGING_DO_FLUSH_CHECK_INTERVAL_MINUTES);
}


// ----------------------------------------------------------------------------------------
void dls_init(void) {
  dls_endpoint_init();
  dls_list_init();

  // rebuild data logging_sessions
  dls_storage_rebuild();

  // add callbacks to empty and check logging_sessions
  dls_resume();
  s_initialized = true;
}


// ----------------------------------------------------------------------------------------
bool dls_initialized(void) {
  return s_initialized;
}


// ----------------------------------------------------------------------------------------
void dls_clear(void) {
  dls_list_remove_all();
  dls_storage_invalidate_all();
}


// ----------------------------------------------------------------------------------------
// Get the send_enable setting
bool dls_get_send_enable(void) {
  return prv_sends_enabled();
}


// ----------------------------------------------------------------------------------------
// Set the send_enable setting
void dls_set_send_enable_pp(bool setting) {
  s_sends_enabled_pp = setting;
}

// ----------------------------------------------------------------------------------------
// Set the send_enable setting
void dls_set_send_enable_run_level(bool setting) {
  s_sends_enabled_run_level = setting;
}


// ----------------------------------------------------------------------------------------
// Callback used by dls_inactivate_sessions.
static bool prv_inactivate_sessions_each_cb(DataLoggingSession *session, void *data) {
  // Note that s_list_mutex is already owned because this is called from
  // dls_list_for_each_session(), so we CANNOT (and don't need to) call dls_lock_session() from
  // here because that could result in a deadlock (see comments in dls_lock_session).
  dls_assert_own_list_mutex();
  if (session->status != DataLoggingStatusActive) {
    // Already inactive
    return true;
  }

  PebbleTask task = (PebbleTask)(uintptr_t)data;

  // System data logging sessions are responsible for killing themselves
  Uuid system_uuid = UUID_SYSTEM;
  if (!uuid_equal(&session->app_uuid, &system_uuid)) {
    if (task == session->task) {
      PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Inactivating session: %"PRIu8,
                session->comm.session_id);

      // Free the buffer if it's in kernel heap. If not in kernel heap we are intentionally not
      // freeing the data->buffer_storage because it was allocated on the client's heap, and the
      // client is being destroyed.
      if (session->data->buffer_in_kernel_heap) {
        kernel_free(session->data->buffer_storage);
      }

      // All the lock/unlock session calls are made from privileged mode, so it is impossible
      // for the task to exit with the session locked (open_count > 0)
      PBL_ASSERTN(session->data->open_count == 0);

      session->status = DataLoggingStatusInactive;
      // Free up the data and mutex for this session
      mutex_destroy(session->data->mutex);
      kernel_free(session->data);
      session->data = NULL;
    }
  }

  return true;
}


// ----------------------------------------------------------------------------------------
void dls_send_all_sessions(void) {
  // If sends are not enabled, do nothing
  if (!prv_sends_enabled()) {
    PBL_LOG(LOG_LEVEL_INFO, "Not sending sessions because sending is disabled");
    return;
  }
  system_task_add_callback(prv_send_all_sessions_system_task_cb, (void*) true);
}


// ----------------------------------------------------------------------------------------
// Mark all sessions belonging to 'task' as inactive so that no more data can be added to them.
// They will only be deleted after the endpoint finishes sending the data to the mobile.
void dls_inactivate_sessions(PebbleTask task) {
  dls_list_for_each_session(prv_inactivate_sessions_each_cb, (void *)(uintptr_t)task);
}


// ----------------------------------------------------------------------------------------
static DataLoggingSession *prv_dls_create(uint32_t tag, DataLoggingItemType item_type,
                                          uint16_t item_size, bool buffered, void *buffer,
                                          bool resume, const Uuid* uuid) {
  // validate size parameter
  if (item_size == 0 || (buffered && item_size > DLS_SESSION_MAX_BUFFERED_ITEM_SIZE)
     || (!buffered && item_size > DLS_ENDPOINT_MAX_PAYLOAD)) {
    PBL_LOG(LOG_LEVEL_ERROR, "invalid logging_session item size, %d", item_size);
    return (NULL);
  } else if (item_type == DATA_LOGGING_UINT || item_type == DATA_LOGGING_INT) {
    if (item_size > 4 || item_size == 3) {
      PBL_LOG(LOG_LEVEL_ERROR, "Invalid data width: integer types can be 1, 2, or 4 bytes");
      return (NULL);
    }
  }

  DataLoggingSession *logging_session = dls_list_find_active_session(tag, uuid);

  if (!resume && logging_session != NULL) {
    dls_finish(logging_session);
    logging_session = NULL;
  }

  if (logging_session == NULL) {
    logging_session = dls_list_create_session(tag, item_type, item_size, uuid, rtc_get_time(),
                                              DataLoggingStatusActive);
    if (logging_session == NULL) {
      // No need to log again here, dls_list_create_session will log on our behalf
      return NULL;
    }

    // Add to the linked list of logging_sessions. This assigns a new unique session_id to this
    // session.
    dls_list_add_new_session(logging_session);

    if (buffered) {
      uint32_t buf_size = DLS_SESSION_MIN_BUFFER_SIZE;

      // Allocate the buffer if the caller didn't
      if (!buffer) {
        // Workers are allowed to allocate the buffer storage in the system heap because
        // they have such limited memory
        PebbleTask task = pebble_task_get_current();
        PBL_ASSERTN(task == PebbleTask_Worker || task == PebbleTask_KernelMain ||
                    task == PebbleTask_KernelBackground);
        buffer = kernel_malloc_check(buf_size);
        logging_session->data->buffer_in_kernel_heap = true;
      }
      logging_session->data->buffer_storage = buffer;
      shared_circular_buffer_init(&logging_session->data->buffer,
                                  logging_session->data->buffer_storage, buf_size);
      shared_circular_buffer_add_client(&logging_session->data->buffer,
                                        &logging_session->data->buffer_client);
    } else {
      // non buffered sessions can only be created/used from KernelBG
      PBL_ASSERT_TASK(PebbleTask_KernelBackground);
    }
  }

  // send an open message
  dls_endpoint_open_session(logging_session);

  return (logging_session);
}


// ----------------------------------------------------------------------------------------
DataLoggingSession* dls_create(uint32_t tag, DataLoggingItemType item_type, uint16_t item_size,
                               bool buffered, bool resume, const Uuid* uuid) {
  return prv_dls_create(tag, item_type, item_size, buffered, NULL /*buffer*/, resume, uuid);
}


// ----------------------------------------------------------------------------------------
DataLoggingSession* dls_create_current_process(uint32_t tag, DataLoggingItemType item_type,
                                               uint16_t item_size, void* buffer, bool resume) {
  const PebbleProcessMd *md = sys_process_manager_get_current_process_md();
  return prv_dls_create(tag, item_type, item_size, true, buffer, resume, &md->uuid);
}


// ----------------------------------------------------------------------------------------
void dls_finish(DataLoggingSession *logging_session) {
  PBL_ASSERTN(logging_session != NULL);
  if (uuid_is_system(&logging_session->app_uuid)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Finishing the system data logging session at %p", logging_session);
  }

  bool is_active = dls_lock_session(logging_session);
  if (!is_active) {
    PBL_LOG(LOG_LEVEL_WARNING, "Tried to close a non-active data logging session");
    return;
  }

  // Wait for write buffer to empty
  int timeout = 1000; // 1 second
  while (logging_session->data->buffer_storage != NULL && timeout) {
    int bytes_pending = shared_circular_buffer_get_read_space_remaining(
                          &logging_session->data->buffer, &logging_session->data->buffer_client);
    if (bytes_pending == 0) {
      break;
    }

    // There's still bytes in the circular buffer that haven't been persisted to flash yet. Just
    // unlock and wait a little bit, since the system task should be busy writing these to flash.
    dls_unlock_session(logging_session, false /*inactivate*/);
    timeout -= 10;
    psleep(10);
    if (!dls_lock_session(logging_session)) {
      // Someone snuck in and marked it inactive on us
      goto exit;
    }
  }

  if (timeout <= 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Timed out waiting for logging_session to write");
  }
  dls_unlock_session(logging_session, true /*inactivate*/);
exit:
  dls_send_all_sessions();
}


// ----------------------------------------------------------------------------------------
static bool prv_write_session_to_flash(DataLoggingSession* session, void *data) {
  dls_storage_write_session(session);
  return true;
}

static void prv_write_all_sessions_to_flash(void *data) {
  dls_list_for_each_session(prv_write_session_to_flash, NULL);
}


// ----------------------------------------------------------------------------------------
DataLoggingResult dls_log(DataLoggingSession *session, const void* data, uint32_t num_items) {
#if !RELEASE
  // TODO: We should be able to remove this requirement once PBL-23925 is fixed
  //
  // Some datalogging code holds the dls_list.c:s_list_mutex while taking the
  // bt_lock. Since we are locking the list and then trying to get the bt_lock,
  // any other thread which holds the bt_lock and then tries to call a log could
  // result in a deadlock (since dls_lock_session() uses the list mutex). For non-release
  // builds assert when this happens so we can catch the cases and fix them.
  bt_lock_assert_held(false);
#endif

  PBL_ASSERTN(session != NULL && data != NULL);
  DataLoggingResult result = DATA_LOGGING_SUCCESS;

  if (num_items == 0 || data == NULL) {
    return (DATA_LOGGING_INVALID_PARAMS);
  }

  uint32_t num_bytes = num_items * session->item_size;
  if (session->data->buffer_storage && num_bytes > DLS_SESSION_MAX_BUFFERED_ITEM_SIZE) {
    return (DATA_LOGGING_INVALID_PARAMS);
  }

  bool active = dls_lock_session(session);
  if (!active) {
    return (DATA_LOGGING_CLOSED);
  }

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "logging %d items of size %d to session %d",
            (int)num_items, (int)session->item_size, session->comm.session_id);

  if (!session->data->buffer_storage) {
    // Unbuffered, we can write to storage immediately
    if (!dls_storage_write_data(session, data, num_bytes)) {
      // We always overwrite old data, so the only possibility for failure here is an internal
      // PFS error.
      result = DATA_LOGGING_INTERNAL_ERR;
    }
    goto unlock_and_exit;
  }

  if (shared_circular_buffer_get_write_space_remaining(&session->data->buffer) < num_bytes) {
    result = DATA_LOGGING_BUSY;
    goto unlock_and_exit;
  }

  shared_circular_buffer_write(&session->data->buffer, data, num_bytes,
                               false /*advance_slackers*/);

  // Only enqueue work on the system_task if we're not already waiting on the system task to handle
  // previously enqueued work for this session.
  if (!session->data->write_request_pending) {
    session->data->write_request_pending = true;
    system_task_add_callback(prv_write_all_sessions_to_flash, NULL);
  }

unlock_and_exit:
  dls_unlock_session(session, false /*inactive*/);
#ifdef DLS_DEBUG_SEND_IMMEDIATELY
  dls_send_all_sessions();
#endif
  return (result);
}


// ----------------------------------------------------------------------------------------
bool dls_is_session_valid(DataLoggingSession *logging_session) {
  return dls_list_is_session_valid(logging_session);
}


// ----------------------------------------------------------------------------------------
// These methods provided for unit tests
int dls_test_read(DataLoggingSession *logging_session, uint8_t *buffer, int num_bytes) {
  uint32_t new_read_offset;
  return (dls_storage_read(logging_session, buffer, num_bytes, &new_read_offset));
}

int dls_test_consume(DataLoggingSession *logging_session, int num_bytes) {
  dls_storage_consume(logging_session, num_bytes);

  return (num_bytes);
}

int dls_test_get_num_bytes(DataLoggingSession *logging_session) {
  return (logging_session->storage.num_bytes);
}

int dls_test_get_tag(DataLoggingSession *logging_session) {
  return (logging_session->tag);
}

uint8_t dls_test_get_session_id(DataLoggingSession *logging_session) {
  return (logging_session->comm.session_id);
}
