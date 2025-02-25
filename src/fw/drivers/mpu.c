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

#include "mpu.h"

#include "mcu/cache.h"
#include "system/passert.h"
#include "util/size.h"

#include <inttypes.h>

#include "FreeRTOS.h"
#include "task.h"
#include "portmacro.h"

#define CMSIS_COMPATIBLE
#include <mcu.h>

extern const uint32_t __SRAM_size__[];
#if !defined(SRAM_BASE)
// On the STM32F2, SRAM_BASE is not defined, but is equal to SRAM1_BASE
#define SRAM_BASE SRAM1_BASE
#endif
#define SRAM_END (SRAM_BASE + (uint32_t)__SRAM_size__)

typedef struct PermissionMapping {
  bool priv_read:1;
  bool priv_write:1;
  bool user_read:1;
  bool user_write:1;
  uint8_t value:3;
} PermissionMapping;

static const PermissionMapping s_permission_mappings[] = {
  { false, false, false, false, 0x0 },
  { true,  true,  false, false, 0x1 },
  { true,  true,  true,  false, 0x2 },
  { true,  true,  true,  true,  0x3 },
  { true,  false, false, false, 0x5 },
  { true,  false, true,  false, 0x6 },
  { true,  false, true,  false, 0x7 } // Both 0x6 and 0x7 map to the same permissions.
};

static const uint32_t s_cache_settings[MpuCachePolicyNum] = {
  [MpuCachePolicy_NotCacheable] = (0x1 << MPU_RASR_TEX_Pos) | (MPU_RASR_S_Msk),
  [MpuCachePolicy_WriteThrough] = (MPU_RASR_S_Msk | MPU_RASR_C_Msk),
  [MpuCachePolicy_WriteBackWriteAllocate] =
      (0x1 << MPU_RASR_TEX_Pos) | (MPU_RASR_S_Msk | MPU_RASR_C_Msk | MPU_RASR_B_Msk),
  [MpuCachePolicy_WriteBackNoWriteAllocate] =
      (MPU_RASR_S_Msk | MPU_RASR_C_Msk | MPU_RASR_B_Msk),
};

static uint8_t get_permission_value(const MpuRegion* region) {
  for (unsigned int i = 0; i < ARRAY_LENGTH(s_permission_mappings); ++i) {
    if (s_permission_mappings[i].priv_read == region->priv_read &&
        s_permission_mappings[i].priv_write == region->priv_write &&
        s_permission_mappings[i].user_read == region->user_read &&
        s_permission_mappings[i].user_write == region->user_write) {
      return s_permission_mappings[i].value;
    }
  }
  WTF;
  return 0;
}

static uint8_t get_size_field(const MpuRegion* region) {
  unsigned int size = 32;
  int result = 4;
  while (size != region->size) {
    PBL_ASSERT(size < region->size || size == 0x400000, "Invalid region size: %"PRIu32,
               region->size);

    size *= 2;
    ++result;
  }

  return result;
}

void mpu_enable(void) {
  MPU->CTRL |= (MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk);
}

void mpu_disable(void) {
  MPU->CTRL &= ~MPU_CTRL_ENABLE_Msk;
}

// Get the required region base address and region attribute register settings for the given region.
// These are the values which should written to the RBAR and RASR registers to configure that
// region.
void mpu_get_register_settings(const MpuRegion* region, uint32_t *base_address_reg,
                               uint32_t *attributes_reg) {
  PBL_ASSERTN(region);
  PBL_ASSERTN((region->base_address & 0x1f) == 0);
  PBL_ASSERTN((region->region_num & ~0xf) == 0);
  PBL_ASSERTN((region->cache_policy != MpuCachePolicy_Invalid) &&
              (region->cache_policy < MpuCachePolicyNum));

  // MPU Region Base Address Register
  // | Addr (27 bits) | Region Valid Bit | Region Num (4 bits) |
  // The address is unshifted, we take the top bits of the address and assume everything below
  // is zero, since the address must be power of 2 size aligned.
  *base_address_reg = region->base_address |
              0x1 << 4 |
              region->region_num;

  // MPU Region Attribute and Size Register
  // A lot of stuff here! Split into bytes...
  // | Reserved (3 bits) | XN Bit | Reserved Bit | Permission Field (3 bits) |
  // | Reserved (2 bits) | TEX (3 bits) | S | C | B |
  // | Subregion Disable Byte |
  // | Reserved (2 bits) | Size Field (5 bits) | Enable Bit |
  *attributes_reg = (get_permission_value(region) << 24) |
              s_cache_settings[region->cache_policy] |
              region->disabled_subregions << 8 |      // Disabled subregions
              (get_size_field(region) << 1) |
              region->enabled; // Enabled
}


void mpu_set_region(const MpuRegion* region) {
  uint32_t base_reg, attr_reg;

  mpu_get_register_settings(region, &base_reg, &attr_reg);
  MPU->RBAR = base_reg;
  MPU->RASR = attr_reg;
}


MpuRegion mpu_get_region(int region_num) {
  MpuRegion region = { .region_num = region_num };

  MPU->RNR = region_num;

  const uint32_t attributes = MPU->RASR;

  region.enabled = attributes & 0x1;

  if (region.enabled) {
    const uint8_t size_field = (attributes >> 1) & 0x1f;
    region.size = 32 << (size_field - 4);

    region.disabled_subregions = (attributes & 0x0000ff00) >> 8;

    const uint32_t raw_base_address = MPU->RBAR;
    region.base_address = raw_base_address & ~(region.size - 1);

    const uint8_t access_permissions = (attributes >> 24) & 0x7;

    for (unsigned int i = 0; i < ARRAY_LENGTH(s_permission_mappings); ++i) {
      if (s_permission_mappings[i].value == access_permissions) {
        region.priv_read = s_permission_mappings[i].priv_read;
        region.priv_write = s_permission_mappings[i].priv_write;
        region.user_read = s_permission_mappings[i].user_read;
        region.user_write = s_permission_mappings[i].user_write;
        break;
      }
    }
  }

  return region;
}


// Fill in the task parameters for a new task with the configurable memory regions we want.
void mpu_set_task_configurable_regions(MemoryRegion_t *memory_regions,
                                       const MpuRegion **region_ptrs) {
  unsigned int region_num, region_idx;
  uint32_t base_reg, attr_reg;

  // Setup the configurable MPU regions
  for (region_num=portFIRST_CONFIGURABLE_REGION, region_idx=0; region_num <= portLAST_CONFIGURABLE_REGION;
            region_num++, region_idx++) {
    const MpuRegion *mpu_region = region_ptrs[region_idx];
    MpuRegion unused_region = {};

    // If not region defined, use unused
    if (mpu_region == NULL) {
      mpu_region = &unused_region;
      attr_reg = 0; // Has a 0 in the enable bit, so this region won't be enabled.
    } else {
      // Make sure that the region numbers passed in jive with the configurable region numbers.
      PBL_ASSERTN(mpu_region->region_num == region_num);
      // Our FreeRTOS port makes the assumption that the ulParameters field contains exactly what
      // should be placed into the MPU_RASR register. It will figure out the MPU_RBAR from the
      // pvBaseAddress field.
      mpu_get_register_settings(mpu_region, &base_reg, &attr_reg);
    }

    memory_regions[region_idx] = (MemoryRegion_t) {
      .pvBaseAddress = (void *)mpu_region->base_address,
      .ulLengthInBytes = mpu_region->size,
      .ulParameters = attr_reg,
    };
  }

}

bool mpu_memory_is_cacheable(const void *addr) {
  if (!dcache_is_enabled()) {
    return false;
  }
  // TODO PBL-37601: We're assuming only SRAM is cacheable for now for simplicity sake. We should
  // account for MPU configuration and also the fact that memory-mapped QSPI access goes through the
  // cache.
  return ((uint32_t)addr >= SRAM_BASE) && ((uint32_t)addr < SRAM_END);
}

void mpu_init_region_from_region(MpuRegion *copy, const MpuRegion *from, bool allow_user_access) {
  *copy = *from;
  copy->user_read = allow_user_access;
  copy->user_write = allow_user_access;
}
