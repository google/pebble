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

#include "dls_list.h"
#include "dls_storage.h"

#include "drivers/flash.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "process_management/process_manager.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/uuid.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static DataLoggingSession *s_logging_sessions;
static PebbleRecursiveMutex * s_list_mutex;


// ---------------------------------------------------------------------------------------
// Assert that the current task owns the list mutex
void dls_assert_own_list_mutex(void) {
  PBL_ASSERTN(mutex_is_owned_recursive(s_list_mutex));
}

// ---------------------------------------------------------------------------------------
// Lock a session (if active). If session was active, locks it and returns true.
// If session is not active, does no locking and returns false.
//
// Note regarding the list_mutex and the session->data-mutex:
//   * session->status can only be read/modified while holding the list mutex
//   * session->data->open_count can only be read/modified while holding the list mutex
//     and is only available if session->status == DataLoggingStatusActive
//   * In order to avoid deadlocks,
//      - s_list_mutex MUST be released before trying to grab session->data->mutex.
//      - session->data->open_count must incremented to be > 0 under s_list_mutex before you can
//        grab session->data-mutex
//      - if you already own session->data-mutex, it is OK to grab s_list_mutex
bool dls_lock_session(DataLoggingSession *session) {
  mutex_lock_recursive(s_list_mutex);
  if (session->status != DataLoggingStatusActive) {
    mutex_unlock_recursive(s_list_mutex);
    return false;
  }

  PBL_ASSERTN(session->data);

  // Incrementing open_count insures that no one else can do a dls_unlock_session(inactivate=true)
  // on it and cause it to be freed before we grab the session->data->mutex below.
  session->data->open_count++;
  mutex_unlock_recursive(s_list_mutex);

  mutex_lock(session->data->mutex);
  return true;
}


// ---------------------------------------------------------------------------------------
// Callback used to free a storage buffer from unprivileged mode.
static void prv_free_storage_buffer_cb(void *p) {
  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Freeing buffer storage ptr: %p", p);
  task_free(p);
}


// ---------------------------------------------------------------------------------------
static void prv_free_storage_buffer(DataLoggingSession *session) {
  if (!session->data->buffer_storage) {
    // Skip if no buffer to free.
    return;
  }

  if (session->data->buffer_in_kernel_heap) {
    kernel_free(session->data->buffer_storage);
  } else {
    // The subscriber's buffer was allocated on its unprivileged process heap (app or worker). It
    // is unsafe to free it from here in privileged mode because a corrupted heap could crash the
    // watch. We will post a callback event to the process's event handler which is executed
    // in unprivileged mode.
    PebbleEvent e = {
      .type = PEBBLE_CALLBACK_EVENT,
      .callback = {
        .callback = prv_free_storage_buffer_cb,
        .data = session->data->buffer_storage
      }
    };
    PebbleTask task = pebble_task_get_current();
    process_manager_send_event_to_process(task, &e);
  }
}


// ---------------------------------------------------------------------------------------
// Unlock a session previous locked by dls_lock_session(). If inactive is true, this also marks
// the session inactive and frees the memory used for maintaining the active state. See the
// comments above in dls_lock_session() for a description of the locking strategy.
void dls_unlock_session(DataLoggingSession *session, bool inactivate) {
  mutex_lock_recursive(s_list_mutex);

  PBL_ASSERTN(session->data->open_count > 0);
  if (inactivate) {
    session->data->inactivate_pending = true;
  }
  session->data->open_count--;
  if (session->data->inactivate_pending && session->data->open_count == 0) {
    session->status = DataLoggingStatusInactive;
    mutex_unlock_recursive(s_list_mutex);

    prv_free_storage_buffer(session);
    mutex_unlock(session->data->mutex);
    mutex_destroy(session->data->mutex);
    kernel_free(session->data);
    session->data = NULL;

  } else {
    mutex_unlock_recursive(s_list_mutex);
    mutex_unlock(session->data->mutex);
  }
}

// ---------------------------------------------------------------------------------------
// Return session status
DataLoggingStatus dls_get_session_status(DataLoggingSession *session) {
  mutex_lock_recursive(s_list_mutex);
  DataLoggingStatus status = session->status;
  mutex_unlock_recursive(s_list_mutex);
  return status;
}


DataLoggingSession *dls_list_find_by_session_id(uint8_t session_id) {
  mutex_lock_recursive(s_list_mutex);
  DataLoggingSession *iter = s_logging_sessions;
  while (iter != NULL) {
    if (iter->comm.session_id > session_id) {
      break;
    }
    if (iter->comm.session_id == session_id) {
      mutex_unlock_recursive(s_list_mutex);
      return (iter);
    }
    iter = iter->next;
  }

  mutex_unlock_recursive(s_list_mutex);
  return (NULL);
}

DataLoggingSession *dls_list_find_active_session(uint32_t tag, const Uuid *app_uuid) {
  mutex_lock_recursive(s_list_mutex);
  DataLoggingSession *iter = s_logging_sessions;
  while (iter != NULL) {
    if (iter->tag == tag && uuid_equal(&(iter->app_uuid), app_uuid)
        && iter->status == DataLoggingStatusActive) {
      mutex_unlock_recursive(s_list_mutex);
      return (iter);
    }
    iter = iter->next;
  }

  mutex_unlock_recursive(s_list_mutex);
  return (NULL);
}

void dls_list_remove_session(DataLoggingSession *logging_session) {
  if (uuid_is_system(&logging_session->app_uuid)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Deleting the system data logging session with tag %"PRIu32,
            logging_session->tag);
  }

  mutex_lock_recursive(s_list_mutex);
  DataLoggingSession **iter = &s_logging_sessions;

  while (*iter != NULL) {
    if (*iter == logging_session) {
      *iter = (*iter)->next;
      mutex_unlock_recursive(s_list_mutex);
      if (logging_session->data) {
        mutex_destroy(logging_session->data->mutex);
        kernel_free(logging_session->data);
      }
      kernel_free(logging_session);
      return;
    }
    iter = &((*iter)->next);
  }

  mutex_unlock_recursive(s_list_mutex);
}

void dls_list_remove_all(void) {
  mutex_lock_recursive(s_list_mutex);
  DataLoggingSession *cur = s_logging_sessions;
  DataLoggingSession *next;
  while (cur != NULL) {
    next = cur->next;
    if (cur->data) {
      mutex_destroy(cur->data->mutex);
      kernel_free(cur->data);
    }
    kernel_free(cur);
    cur = next;
  }

  s_logging_sessions = NULL;
  mutex_unlock_recursive(s_list_mutex);
}

//! Insert logging session with known id
void dls_list_insert_session(DataLoggingSession *logging_session) {
  mutex_lock_recursive(s_list_mutex);
  DataLoggingSession **iter = &s_logging_sessions;

  for (int i = 0; i < logging_session->comm.session_id; i++) {
    if (*iter == NULL) {
      break;
    }
    PBL_ASSERTN(logging_session->comm.session_id != (*iter)->comm.session_id);
    if ((*iter)->comm.session_id > logging_session->comm.session_id) {
      break;
    }
    iter = &((*iter)->next);
  }

  logging_session->next = *iter;
  *iter = logging_session;

  PBL_LOG_D(LOG_DOMAIN_DATA_LOGGING, LOG_LEVEL_DEBUG, "Created session: %p id %"PRIu8
      " tag %"PRIu32, logging_session, logging_session->comm.session_id, logging_session->tag);

  mutex_unlock_recursive(s_list_mutex);
}

// The newlib headers do not expose this because of the __STRICT_ANSI__ define
extern int rand_r(unsigned *seed);

uint8_t dls_list_add_new_session(DataLoggingSession *logging_session) {
  // generate random ID
  int loops = 0;
  uint8_t session_id;
  unsigned seed = rtc_get_time() ^ (uintptr_t)&session_id;
  do {
    // use a custom seed to avoid apps maliciously setting the task-global seed
    session_id = rand_r(&seed) % 255;
    // FIXME better way to avoid infinite loop? or tune this
    PBL_ASSERTN(++loops < 100);
  } while (dls_list_find_by_session_id(session_id));
  logging_session->comm.session_id = session_id;

  // insert in the spool list
  dls_list_insert_session(logging_session);

  return (session_id);
}

static bool count_session_cb(DataLoggingSession *session, void *data) {
  uint32_t *counter = (uint32_t *) data;
  ++(*counter);
  return true;
}

static uint32_t prv_get_num_sessions(void) {
  uint32_t counter = 0;
  dls_list_for_each_session(count_session_cb, &counter);
  return counter;
}

DataLoggingSession *dls_list_create_session(uint32_t tag, DataLoggingItemType type, uint16_t size,
    const Uuid *app_uuid, time_t timestamp, DataLoggingStatus status) {

  uint32_t num_sessions = prv_get_num_sessions();
  if (num_sessions >= DLS_MAX_NUM_SESSIONS) {
    PBL_LOG(LOG_LEVEL_WARNING, "Could not allocate additional DataLoggingSession objects");
    return NULL;
  }

  DataLoggingSession *logging_session = kernel_malloc_check(sizeof(DataLoggingSession));

  *logging_session = (DataLoggingSession){
    .status = status,
    .app_uuid = *app_uuid,
    .tag = tag,
    .task = pebble_task_get_current(),
    .item_type = type,
    .item_size = size,
    .session_created_timestamp = timestamp,
    .storage.fd = DLS_INVALID_FILE
  };

  if (status == DataLoggingStatusActive) {
    DataLoggingActiveState *active_state = kernel_malloc_check(sizeof(DataLoggingActiveState));
    *active_state = (DataLoggingActiveState){};
    active_state->mutex = mutex_create();
    logging_session->data = active_state;
  }

  return (logging_session);
}

DataLoggingSession *dls_list_get_next(DataLoggingSession *cur) {
  mutex_lock_recursive(s_list_mutex);
  if (cur == NULL) {
    // Return the head
    mutex_unlock_recursive(s_list_mutex);
    return s_logging_sessions;
  }

  DataLoggingSession *logging_session = ((DataLoggingSession *)cur)->next;
  mutex_unlock_recursive(s_list_mutex);
  return logging_session;
}

bool dls_list_for_each_session(bool (callback(DataLoggingSession*, void*)), void *data) {
  mutex_lock_recursive(s_list_mutex);
  DataLoggingSession *logging_session = s_logging_sessions;

  while (logging_session != NULL) {
    // Read the next pointer first, just in case the callback ends up removing the session.
    DataLoggingSession *next_logging_session = logging_session->next;

    if (!callback(logging_session, data)) {
      mutex_unlock_recursive(s_list_mutex);
      return false;
    }

    logging_session = next_logging_session;
  }
  mutex_unlock_recursive(s_list_mutex);
  return true;
}

void dls_list_init(void) {
  s_list_mutex = mutex_create_recursive();
  s_logging_sessions = NULL;
}

bool dls_list_is_session_valid(DataLoggingSession *logging_session) {
  mutex_lock_recursive(s_list_mutex);
  DataLoggingSession *iter = s_logging_sessions;

  while (iter != NULL) {
    if (iter == logging_session) {
      mutex_unlock_recursive(s_list_mutex);
      return true;
    }
    iter = iter->next;
  }
  mutex_unlock_recursive(s_list_mutex);

  return false;
}

