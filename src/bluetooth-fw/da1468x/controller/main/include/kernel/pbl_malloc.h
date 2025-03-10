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

#include <inttypes.h>
#include <stddef.h>

// Defined with prefix of `kernel` to allow for code to be shared
// between normal firmware the bluetooth firmware.
void kernel_heap_init(void);
void *kernel_malloc(size_t bytes);
void *kernel_zalloc(size_t bytes);
void *kernel_malloc_check(size_t bytes);
void *kernel_zalloc_check(size_t bytes);
void kernel_free(void *ptr);
