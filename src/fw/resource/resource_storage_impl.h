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

#include "util/attributes.h"

#include "resource.h"
#include "resource_storage.h"

//! @file resource_storage_impl.h
//!
//! Shared functionality that all the different ResourceStoreImplementation's need.

// TODO PBL-21382: Abstract these details out of the resource storage implementation.

//  Apart from builtins which do not have a header at all, the resource stores
//  are structured as follow:
//
//  +----------------------------------------------------------------+
//  | ResourceManifest | ResTableEntry (n-times) | Raw resource data |
//  +----------------------------------------------------------------+
//
//  Each ResTableEntry contains metadata about resources, and an offset in the
//  raw resource data blob.
//
//  More info at:
//  https://pebbletechnology.atlassian.net/wiki/display/DEV/Pebble+Resource+Pack+Format

//! Actually baked into the flash storage format.
//! Do not change this without changing the associated tooling!
typedef struct PACKED {
  uint32_t num_resources;
  ResourceVersion version;
} ResourceManifest;

//! Actually baked into the flash storage format.
//! Do not change this without changing the associated tooling!
typedef struct PACKED {
  uint32_t resource_id;
  uint32_t offset;
  uint32_t length;
  uint32_t crc;
} ResTableEntry;

#define MAX_RESOURCES_PER_STORE 256
#define MAX_RESOURCES_FOR_SYSTEM_STORE 512
#define MANIFEST_SIZE (sizeof(ResourceManifest))
#define TABLE_ENTRY_SIZE (sizeof(ResTableEntry))
#define RESOURCE_STORE_METADATA_BYTES \
    (MANIFEST_SIZE + MAX_RESOURCES_PER_STORE * TABLE_ENTRY_SIZE)
#define SYSTEM_STORE_METADATA_BYTES \
    (MANIFEST_SIZE + MAX_RESOURCES_FOR_SYSTEM_STORE * TABLE_ENTRY_SIZE)

void resource_storage_generic_init(void);
void resource_storage_generic_clear(ResourceStoreEntry *entry);
bool resource_storage_generic_check(ResAppNum app_num, uint32_t resource_id,
                                    ResourceStoreEntry *entry,
                                    const ResourceVersion *expected_version);
uint32_t resource_storage_generic_metadata_size(ResourceStoreEntry *entry);
bool resource_storage_generic_get_resource(ResourceStoreEntry *entry);
uint32_t resource_storage_generic_get_length(ResourceStoreEntry *entry);
uint32_t resource_storage_generic_get_crc(ResourceStoreEntry *entry, uint32_t num_bytes,
                                          uint32_t entry_offset);
uint32_t resource_storage_generic_write(ResourceStoreEntry *entry, uint32_t offset, void *data,
                                        size_t num_bytes);
ResourceCallbackHandle resource_storage_generic_watch(ResourceStoreEntry *entry,
                                                      ResourceChangedCallback callback,
                                                      void* data);
bool resource_storage_generic_unwatch(ResourceCallbackHandle cb_handle);

#define RESOURCE_IMPL(impl) extern const ResourceStoreImplementation impl;
#include "resource_impl.def"
#undef RESOURCE_IMPL
