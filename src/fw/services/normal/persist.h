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

//! Persist service
//!
//! The persist service manages persistent app key-value stores. A persistent
//! store is simply a SettingsFile identified by the app's UUID. The service
//! manages the creation, opening and deletion of persist stores so that an app
//! and its worker can both access the same file through a single file handle
//! and SettingsFile state object.
//!
//! The persist service makes no attempt to make SettingsFile reentrant; it is
//! the caller's responsibility to enforce mutual exclusion and prevent
//! concurrent access to the SettingsFile.

#include <stdint.h>
#include <stddef.h>

#include "util/uuid.h"
#include "process_management/app_install_types.h"
#include "system/status_codes.h"

typedef struct SettingsFile SettingsFile;


//! Initialize the persist service.
void persist_service_init(void);

//! Lock and get the persist store for the given app.
SettingsFile *persist_service_lock_and_get_store(const Uuid *uuid);

//! Unlock the given persist store.
void persist_service_unlock_store(SettingsFile *store);

//! Call during each process's startup.
void persist_service_client_open(const Uuid *uuid);

//! Call once after process exits to clean it up.
void persist_service_client_close(const Uuid *uuid);

//! Deletes the app's persist file.
status_t persist_service_delete_file(const Uuid *uuid);
