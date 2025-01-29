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

#include "applib_resource.h"
#include "resource/resource.h"

//! Checks if a passed pointer refers to builtin or memory-mapped data and manages reference
//! counters as needed.
//! @note You might want to use \ref applib_resource_mmap_or_load() instead
//! @return true, if the passed pointer expresses memory-mapped data ans was successfully tracked
bool applib_resource_track_mmapped(const void *bytes);

//! True, if the passed pointer refers to builtin or memory-mapped data
bool applib_resource_is_mmapped(const void *bytes);

//! Checks if a passed pointer refers to builtin or memory-mapped data and manages reference
//! counters as needed.
//! @note You might want to use \ref applib_resource_munmap_or_free() instead
//! @return true, if the passed pointer expresses memory-mapped data ans was successfully
//!     untracked
bool applib_resource_munmap(const void *bytes);

//! Manages the reference counters for memory-mapped resources.
//! Should not be called by anyone but the process manager.
//! @return true, if any remaining resources were untracked
bool applib_resource_munmap_all();

//! Tries to load a resource as memory-mapped data. If this isn't supported on the system
//! or for a given resource if will try to allocate data and load it into RAM instead.
//! Have a look at \ref resource_load_byte_range_system for the discussion of arguments
//! @param used_aligned True, if you want this function to allocate 7 extra bytes if it cannot mmap
//! @return NULL, if the resource couldn't be memory-mapped or allocated
void *applib_resource_mmap_or_load(ResAppNum app_num, uint32_t resource_id,
                                   size_t offset, size_t length, bool used_aligned);

//! Updates reference counters if bytes is memory-mapped,
//! alternatively it deallocates the data bytes points to.
void applib_resource_munmap_or_free(void *bytes);
