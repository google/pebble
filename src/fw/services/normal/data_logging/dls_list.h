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

#include "dls_private.h"
#include "applib/data_logging.h"

#include <stdint.h>
#include <time.h>

DataLoggingSession *dls_list_find_by_session_id(uint8_t session_id);

DataLoggingSession *dls_list_find_active_session(uint32_t tag, const Uuid *app_uuid);

void dls_list_remove_session(DataLoggingSession *logging_session);

//! Deletes all session state in memory without changing the flash state.
void dls_list_remove_all(void);

//! Add logging_session and assign ID
uint8_t dls_list_add_new_session(DataLoggingSession *logging_session);

//! Add logging session with an already assigned ID. Used at startup when restoring previous
//! sessions from flash.
void dls_list_insert_session(DataLoggingSession *logging_session);

//! Creates a new DataLoggingSession object that is only initialized with the parameters given. The
//! session will only be initialized with the given parameters. The .storage and .comm members must
//! be separately initialized. Also, the resulting object will need to be added to the list of
//! sessions using one of dls_list_add_new_session and dls_list_insert_session. May return NULL if
//! we've created too many sessions.
DataLoggingSession *dls_list_create_session(uint32_t tag, DataLoggingItemType type, uint16_t size,
                                            const Uuid *app_uuid, time_t timestamp,
                                            DataLoggingStatus status);

DataLoggingSession *dls_list_get_next(DataLoggingSession *cur);

void dls_list_rebuild_from_storage(void);

//! Call callback for each session we have. Pass the data param through to the callback each time.
//! If the callback returns false, stop iterating immediately and return false. Returns true
//! otherwise.
typedef bool (*DlsListCallback)(DataLoggingSession*, void*);
bool dls_list_for_each_session(DlsListCallback cb, void *data);

void dls_list_init(void);

//! Checks to see if this is an actual valid data session
//! Note that we pass in the logging_session parameter without making sure it's same. Make sure
//! this function handles passing in random pointers that don't actually point to valid sessions or
//! even valid memory.
bool dls_list_is_session_valid(DataLoggingSession *logging_session);

//! Lock a session (if active). If session was active, locks it and returns true.
//! If session is not active, returns false
bool dls_lock_session(DataLoggingSession *session);

//! Unlock a session previous locked by dls_lock_session()
void dls_unlock_session(DataLoggingSession *session, bool inactivate);

//! Return session status
DataLoggingStatus dls_get_session_status(DataLoggingSession *session);

//! Assert that the current task owns the list mutex
void dls_assert_own_list_mutex(void);
