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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "resource/resource.h"

// transformed to int to avoid surprises between C->JS
typedef int (*ResourceReadCb)(int offset, uint8_t *buf, int num_bytes);
typedef int (*ResourceGetSizeCb)(void);

bool emx_resources_init(void);
void emx_resources_deinit(void);
size_t emx_resources_get_size(ResAppNum app_num, uint32_t resource_id);
size_t emx_resources_read(ResAppNum app_num,
                          uint32_t resource_id,
                          uint32_t offset,
                          uint8_t *buf,
                          size_t num_bytes);
uint32_t emx_resources_register_custom(ResourceReadCb read_cb, ResourceGetSizeCb get_size_cb);
void emx_resources_remove_custom(uint32_t resource_id);
