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

#include <stdint.h>
#include <stdbool.h>

#include "freertos_types.h"

typedef enum MpuCachePolicy {
  MpuCachePolicy_Invalid,

  MpuCachePolicy_NotCacheable,
  MpuCachePolicy_WriteThrough,
  MpuCachePolicy_WriteBackWriteAllocate,
  MpuCachePolicy_WriteBackNoWriteAllocate,

  MpuCachePolicyNum
} MpuCachePolicy;

typedef struct MpuRegion {
  uint8_t region_num:4;
  bool enabled:1;
  uintptr_t base_address;
  uint32_t size;
  uint8_t disabled_subregions; // 8 bits, each disables 1/8 of the region.
  MpuCachePolicy cache_policy;
  bool priv_read:1;
  bool priv_write:1;
  bool user_read:1;
  bool user_write:1;
} MpuRegion;

void mpu_enable(void);

void mpu_disable(void);

void mpu_set_region(const MpuRegion* region);

MpuRegion mpu_get_region(int region_num);

void mpu_get_register_settings(const MpuRegion* region, uint32_t *base_address_reg,
                               uint32_t *attributes_reg);

void mpu_set_task_configurable_regions(MemoryRegion_t *memory_regions,
                                       const MpuRegion **region_ptrs);

bool mpu_memory_is_cacheable(const void *addr);

void mpu_init_region_from_region(MpuRegion *copy, const MpuRegion *from, bool allow_user_access);
