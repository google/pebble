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

#define SECTOR_SIZE_BYTES 0x20000
#define SECTOR_ADDR_MASK (~(SECTOR_SIZE_BYTES - 1))

#define SUBSECTOR_SIZE_BYTES 0x20000
#define SUBSECTOR_ADDR_MASK (~(SUBSECTOR_SIZE_BYTES - 1))

// Filesystem layout
///////////////////////////////////////

// Space for our flash logs
// NOTE: This range of memory is actually in the special "bottom boot" area of our flash chip
// where the erase sectors are smaller (32k instead of 128k everywhere else).
#define FLASH_REGION_DEBUG_DB_BEGIN 0x0
#define FLASH_REGION_DEBUG_DB_END   0x20000 // 128k

// Regions after this point are in standard, 128kb sized sectors.

// 768kb gap here. We should save some space for non-filesystem things. It also aligns the
// subsequent sectors nicely.

// 1 128kb sector for storing mfg info, see fw/mfg/snowy/mfg_info.c
#define FLASH_REGION_MFG_INFO_BEGIN 0x0e0000
#define FLASH_REGION_MFG_INFO_END   0x100000

// Scratch space for firmware images (normal and recovery).
#define FLASH_REGION_FIRMWARE_SCRATCH_BEGIN 0x100000
#define FLASH_REGION_FIRMWARE_SCRATCH_END   0x200000 // 1024k

#define FLASH_REGION_SAFE_FIRMWARE_BEGIN 0x200000
#define FLASH_REGION_SAFE_FIRMWARE_END   0x300000 // 1024k

#define FLASH_REGION_NEXT_SYSTEM_RESOURCES_BEGIN 0x300000
#define FLASH_REGION_NEXT_SYSTEM_RESOURCES_END   0x380000 // 512k

#define FLASH_REGION_SYSTEM_RESOURCES_BEGIN 0x380000
#define FLASH_REGION_SYSTEM_RESOURCES_END   0x400000 // 512k

// FIXME: The addresses below here are hacky workarounds and hopefully not the final place
// for these things. Hopefully many of them can move to the filesystem. Everything above here
// should be pretty stable.

#define REGISTRY_FLASH_BEGIN 0x400000 //
#define REGISTRY_FLASH_END   0x420000 // 128k

#define FACTORY_REGISTRY_FLASH_BEGIN 0x420000 //
#define FACTORY_REGISTRY_FLASH_END   0x440000 // 128k

#define FLASH_REGION_UNUSED0_BEGIN 0x440000
#define FLASH_REGION_UNUSED0_END   0x480000 // 256k

#define FLASH_REGION_FILESYSTEM_BEGIN 0x0480000
#define FLASH_REGION_FILESYSTEM_END   0x1000000 // 8mb+, aka the rest!

// Constants used for testing flash interface
// NOTE: This purposely overlaps the file system region since the flash test requires a non-critical
// region to operate on. Data in this region will get corrupted and will not get restored after the
// test runs. Any data in this region will have to be manually restored or reinitialized.
#define FLASH_TEST_ADDR_START 0x0800000 // 8MB
#define FLASH_TEST_ADDR_END   0x1000000 // 16MB
#define FLASH_TEST_ADDR_MSK   0x1FFFFFF // test all bits in the 16MB range


#if ((FLASH_REGION_FILESYSTEM_BEGIN > FLASH_TEST_ADDR_START) || (FLASH_REGION_FILESYSTEM_END < FLASH_TEST_ADDR_END))
#error "ERROR: Flash Test space not withing expected range"
#endif

// 0x1000000 is the end of the SPI flash address space.

// FIXME: These regions are used by code paths that are never executed on this flash part. Give
// them bogus values so things still compile
#define FLASH_REGION_APP_BEGIN 0x1000000
#define FLASH_REGION_APP_END   0x1020000

#define FLASH_REGION_APP_RESOURCES_BEGIN 0x1020000
#define FLASH_REGION_APP_RESOURCES_END   0x1040000

#define FLASH_REGION_MIGRATION_BEGIN 0x1040000
#define FLASH_REGION_MIGRATION_END   0x1060000
