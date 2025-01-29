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

#include "services/common/legacy/registry_common.h"

#include <stdint.h>

//! Initialize the factory settings registry
//!
//! This must be called before using the factory registry.
void factory_registry_init();

//! Add a record to the factory settings registry
//!
//! @return 0 on success, -1 if factory settings registry is full or if key_length or value_length is
//! greater than MAX_KEY_SIZE_BYTES or MAX_VALUE_SIZE_BYTES, respectively
int factory_registry_add(const char* key, const uint8_t key_length, const uint8_t* uuid, const uint8_t description,
        const uint8_t* value, uint8_t value_length);

//! Get a record from the factory settings registry
//!
//! @return Record if found, NULL if not found
Record* factory_registry_get(const char* key, const uint8_t key_length, const uint8_t* uuid);

//! Remove a record from the factory settings registry
//!
//! @return 0 on success, -1 if record not found
int factory_registry_remove(const char* key, const uint8_t key_length, const uint8_t* uuid);

//!  Write the factory settings registry to flash
void factory_registry_write_to_flash(void);

//!  Remove all records from the factory settings registry that match `uuid`
void factory_registry_remove_all(const uint8_t* uuid);

