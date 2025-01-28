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

#if PLATFORM_TINTIN
  // v2_0 and v1_5 have 8MB flash chips instead of 4MB. In the following definition,
  // BOARD_NOR_FLASH_SIZE is set to allow 6MB of the flash chip to be used. The extra 2MB tacked
  // onto the end will be used for the filesystem and is being added to help with storing large
  // language packs (ex. Chinese). If the entire 8MB needs to be used, this variable will have to
  // be changed. Migrations are likely as well.
  //
  // On watches with only 4MB of flash, the region will have a size of zero and be ignored by the
  // filesystem.
  #if defined(BOARD_V2_0) || defined(BOARD_V1_5) || defined(LARGE_SPI_FLASH)
    #define BOARD_NOR_FLASH_SIZE 0x600000
  #else
    #define BOARD_NOR_FLASH_SIZE 0x400000
  #endif

  #include "flash_region_n25q.h"
#elif PLATFORM_SILK
  #include "flash_region_mx25u.h"
#elif PLATFORM_CALCULUS || PLATFORM_ROBERT
  #include "flash_region_mt25q.h"
#elif PLATFORM_SNOWY || PLATFORM_SPALDING
  #include "flash_region_s29vs.h"
#endif

// NOTE: The following functions are deprecated! New code should use the
// asynchronous version, flash_erase_optimal_range, in flash.h.

//! Erase at least (max_start, min_end) but no more than (min_start, max_end) using as few erase operations
//! as possible.
//! (min_start, max_end) must be both 4kb aligned, as that's the smallest unit that we can erase.
void flash_region_erase_optimal_range(uint32_t min_start, uint32_t max_start,
                                      uint32_t min_end, uint32_t max_end);

//! The same as flash_region_erase_optimal_range but first disables the task watchdog for the
//! current task.
void flash_region_erase_optimal_range_no_watchdog(uint32_t min_start, uint32_t max_start,
                                                  uint32_t min_end, uint32_t max_end);
