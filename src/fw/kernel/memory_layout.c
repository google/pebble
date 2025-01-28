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

#include "memory_layout.h"

#include "kernel/logging_private.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/size.h"
#include "util/string.h"

#include "kernel/mpu_regions.auto.h"

#include <inttypes.h>
#include <string.h>


static const char* const MEMORY_REGION_NAMES[8] = {
  "UNPRIV_FLASH",
  "UNPRIV_RO_BSS",
  "UNPRIV_RO_DATA",
  "ISR_STACK_GUARD",
  "Task Specific 1",
  "Task Specific 2",
  "Task Specific 3",
  "Task Specific 4"
};


void memory_layout_dump_mpu_regions_to_dbgserial(void) {
  static const int NUM_REGIONS = 8;

  char buffer[90];

  for (int i = 0; i < NUM_REGIONS; ++i) {
    MpuRegion region = mpu_get_region(i);

    if (!region.enabled) {
      PBL_LOG_FROM_FAULT_HANDLER_FMT(buffer, sizeof(buffer), "%u Not enabled", i);
      continue;
    }

    PBL_LOG_FROM_FAULT_HANDLER_FMT(
        buffer, sizeof(buffer),
        "%u < %-22s>: Addr %p Size 0x%08"PRIx32" Priv: %c%c User: %c%c",
        i, MEMORY_REGION_NAMES[i], (void*) region.base_address, region.size,
        region.priv_read ? 'R' : ' ', region.priv_write ? 'W' : ' ',
        region.user_read ? 'R' : ' ', region.user_write ? 'W' : ' ');

    if (region.disabled_subregions) {
      PBL_LOG_FROM_FAULT_HANDLER_FMT(
          buffer, sizeof(buffer),
          "  Disabled Subregions: %02x", region.disabled_subregions);
    }
  }
}

#ifdef UNITTEST
static const uint32_t __privileged_functions_start__ = 0;
static const uint32_t __privileged_functions_size__ = 0;
static const uint32_t __unpriv_ro_bss_start__ = 0;
static const uint32_t __unpriv_ro_bss_size__ = 0;
static const uint32_t __isr_stack_start__ = 0;
static const uint32_t __stack_guard_size__ = 0;

static const uint32_t __APP_RAM__ = 0;
static const uint32_t __WORKER_RAM__ = 0;

static const uint32_t __FLASH_start__ = 0;
static const uint32_t __FLASH_size__ = 0;

static const uint32_t __kernel_main_stack_start__ = 0;
static const uint32_t __kernel_bg_stack_start__ = 0;
#else
extern const uint32_t __privileged_functions_start__[];
extern const uint32_t __privileged_functions_size__[];
extern const uint32_t __unpriv_ro_bss_start__[];
extern const uint32_t __unpriv_ro_bss_size__[];
extern const uint32_t __isr_stack_start__[];
extern const uint32_t __stack_guard_size__[];

extern const uint32_t __APP_RAM__[];
extern const uint32_t __WORKER_RAM__[];

extern const uint32_t __FLASH_start__[];
extern const uint32_t __FLASH_size__[];

extern const uint32_t __kernel_main_stack_start__[];
extern const uint32_t __kernel_bg_stack_start__[];
#endif

// Kernel read only RAM. Parts of RAM that it's kosher for unprivileged apps to read
static const MpuRegion s_readonly_bss_region = {
  .region_num = MemoryRegion_ReadOnlyBss,
  .enabled = true,
  .base_address = (uint32_t) __unpriv_ro_bss_start__,
  .size = (uint32_t) __unpriv_ro_bss_size__,
  .cache_policy = MpuCachePolicy_WriteBackWriteAllocate,
  .priv_read = true,
  .priv_write = true,
  .user_read = true,
  .user_write = false
};

// ISR stack guard
static const MpuRegion s_isr_stack_guard_region = {
  .region_num = MemoryRegion_IsrStackGuard,
  .enabled = true,
  .base_address = (uint32_t) __isr_stack_start__,
  .size = (uint32_t) __stack_guard_size__,
  .cache_policy = MpuCachePolicy_NotCacheable,
  .priv_read = false,
  .priv_write = false,
  .user_read = false,
  .user_write = false
};

static const MpuRegion s_app_stack_guard_region = {
  .region_num = MemoryRegion_TaskStackGuard,
  .enabled = true,
  .base_address = (uint32_t) __APP_RAM__,
  .size = (uint32_t) __stack_guard_size__,
  .cache_policy = MpuCachePolicy_NotCacheable,
  .priv_read = false,
  .priv_write = false,
  .user_read = false,
  .user_write = false
};

static const MpuRegion s_worker_stack_guard_region = {
  .region_num = MemoryRegion_TaskStackGuard,
  .enabled = true,
  .base_address = (uint32_t) __WORKER_RAM__,
  .size = (uint32_t) __stack_guard_size__,
  .cache_policy = MpuCachePolicy_NotCacheable,
  .priv_read = false,
  .priv_write = false,
  .user_read = false,
  .user_write = false
};

static const MpuRegion s_app_region = {
  .region_num = MemoryRegion_AppRAM,
  .enabled = true,
  .base_address = MPU_REGION_APP_BASE_ADDRESS,
  .size = MPU_REGION_APP_SIZE,
  .disabled_subregions = MPU_REGION_APP_DISABLED_SUBREGIONS,
  .cache_policy = MpuCachePolicy_WriteBackWriteAllocate,
  .priv_read = true,
  .priv_write = true,
};

static const MpuRegion s_worker_region = {
  .region_num = MemoryRegion_WorkerRAM,
  .enabled = true,
  .base_address = MPU_REGION_WORKER_BASE_ADDRESS,
  .size = MPU_REGION_WORKER_SIZE,
  .disabled_subregions = MPU_REGION_WORKER_DISABLED_SUBREGIONS,
  .cache_policy = MpuCachePolicy_WriteBackWriteAllocate,
  .priv_read = true,
  .priv_write = true,
};

static const MpuRegion s_microflash_region = {
  .region_num = MemoryRegion_Flash,
  .enabled = true,
  .base_address = (uint32_t) __FLASH_start__,
  .size = (uint32_t) __FLASH_size__,
  .cache_policy = MpuCachePolicy_WriteThrough,
  .priv_read = true,
  .priv_write = false,
  .user_read = true,
  .user_write = false
};

static const MpuRegion s_kernel_main_stack_guard_region = {
  .region_num = MemoryRegion_TaskStackGuard,
  .enabled = true,
  .base_address = (uint32_t) __kernel_main_stack_start__,
  .size = (uint32_t) __stack_guard_size__,
  .cache_policy = MpuCachePolicy_NotCacheable,
  .priv_read = false,
  .priv_write = false,
  .user_read = false,
  .user_write = false
};

static const MpuRegion s_kernel_bg_stack_guard_region = {
  .region_num = MemoryRegion_TaskStackGuard,
  .enabled = true,
  .base_address = (uint32_t) __kernel_bg_stack_start__,
  .size = (uint32_t) __stack_guard_size__,
  .cache_policy = MpuCachePolicy_NotCacheable,
  .priv_read = false,
  .priv_write = false,
  .user_read = false,
  .user_write = false
};

void memory_layout_setup_mpu(void) {
  // Flash parts...
  // Read only for executing code and loading data out of.

  // Unprivileged flash, by default anyone can read any part of flash.
  mpu_set_region(&s_microflash_region);

  // RAM parts
  // The background memory map only allows privileged access. We need to add additional regions to
  // enable access to unprivileged code.

  mpu_set_region(&s_readonly_bss_region);
  mpu_set_region(&s_isr_stack_guard_region);

  mpu_enable();
}

const MpuRegion* memory_layout_get_app_region(void) {
  return &s_app_region;
}

const MpuRegion* memory_layout_get_readonly_bss_region(void) {
  return &s_readonly_bss_region;
}

const MpuRegion* memory_layout_get_app_stack_guard_region(void) {
  return &s_app_stack_guard_region;
}

const MpuRegion* memory_layout_get_worker_region(void) {
  return &s_worker_region;
}

const MpuRegion* memory_layout_get_worker_stack_guard_region(void) {
  return &s_worker_stack_guard_region;
}

const MpuRegion* memory_layout_get_microflash_region(void) {
  return &s_microflash_region;
}

const MpuRegion* memory_layout_get_kernel_main_stack_guard_region(void) {
  return &s_kernel_main_stack_guard_region;
}

const MpuRegion* memory_layout_get_kernel_bg_stack_guard_region(void) {
  return &s_kernel_bg_stack_guard_region;
}

bool memory_layout_is_pointer_in_region(const MpuRegion *region, const void *ptr) {
  uintptr_t p = (uintptr_t) ptr;
  return (p >= region->base_address && p < (region->base_address + region->size));
}

bool memory_layout_is_buffer_in_region(const MpuRegion *region, const void *buf, size_t length) {
  return memory_layout_is_pointer_in_region(region, buf) && memory_layout_is_pointer_in_region(region, (char *)buf + length - 1);
}

bool memory_layout_is_cstring_in_region(const MpuRegion *region, const char *str, size_t max_length) {
  uintptr_t region_end = region->base_address + region->size;

  if ((uintptr_t) str < region->base_address || (uintptr_t) str >= region_end) {
    return false;
  }

  const char *str_max_end = MIN((const char*) region_end, str + max_length);

  size_t str_len = strnlen(str, str_max_end - str);

  if (str[str_len] != 0) {
    // No null between here and the end of the memory region.
    return false;
  }

  return true;
}
