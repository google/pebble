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

#include "flash_region/flash_region.h"

#include "clar.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_logging.h"
#include "stubs_pebble_tasks.h"
#include "stubs_passert.h"
#include "stubs_sleep.h"
#include "stubs_task_watchdog.h"

void flash_read_bytes(uint8_t* buffer, uint32_t start_addr, uint32_t buffer_size) {}
void flash_write_bytes(const uint8_t* buffer, uint32_t start_addr, uint32_t buffer_size) {}

// Fakes
///////////////////////////////////////////////////////////

typedef enum EraseCommandType {
  SectorEraseCommand,
  SubsectorEraseCommand
} EraseCommandType;

typedef struct EraseCommand {
  uint32_t addr;
  EraseCommandType type;
} EraseCommand;

static EraseCommand s_command_list[32];
static int s_command_list_index = 0;

void flash_erase_subsector_blocking(uint32_t subsector_addr) {
  s_command_list[s_command_list_index++] = (EraseCommand) { .addr = subsector_addr, .type = SubsectorEraseCommand };
}

void flash_erase_sector_blocking(uint32_t subsector_addr) {
  s_command_list[s_command_list_index++] = (EraseCommand) { .addr = subsector_addr, .type = SectorEraseCommand };
}

// Tests
///////////////////////////////////////////////////////////

void test_flash_region__initialize(void) {
  s_command_list_index = 0;
}

void test_flash_region__cleanup(void) {
}

void test_flash_region__erase_optimal_range_empty(void) {
  flash_region_erase_optimal_range(0, 0, 0, 0);

  cl_assert_equal_i(s_command_list_index, 0);
}

void test_flash_region__erase_optimal_range_sectors_simple(void) {
  // Erase one sector 0x10000 - 0x20000
  flash_region_erase_optimal_range(64 * 1024, 64 * 1024, 2 * 64 * 1024, 2 * 64 * 1024);

  cl_assert_equal_i(s_command_list_index, 1);
  cl_assert_equal_i(s_command_list[0].addr, 64 * 1024);
  cl_assert_equal_i(s_command_list[0].type, SectorEraseCommand);

  s_command_list_index = 0;

  // Erase one sectors 0x10000 - 0x20000 but allow us to erase more
  flash_region_erase_optimal_range(0, 64 * 1024, 2 * 64 * 1024, 3 * 64 * 1024);

  cl_assert_equal_i(s_command_list_index, 1);
  cl_assert_equal_i(s_command_list[0].addr, 64 * 1024);
  cl_assert_equal_i(s_command_list[0].type, SectorEraseCommand);

  s_command_list_index = 0;

  // Erase two sectors 0x10000 - 0x30000 but allow us to erase more
  flash_region_erase_optimal_range(0, 64 * 1024, 3 * 64 * 1024, 4 * 64 * 1024);

  cl_assert_equal_i(s_command_list_index, 2);

  cl_assert_equal_i(s_command_list[0].addr, 64 * 1024);
  cl_assert_equal_i(s_command_list[0].type, SectorEraseCommand);

  cl_assert_equal_i(s_command_list[1].addr, 2 * 64 * 1024);
  cl_assert_equal_i(s_command_list[1].type, SectorEraseCommand);
}

void test_flash_region__erase_optimal_range_subsectors(void) {
  // Offer a less than full sector range but erase the full range
  flash_region_erase_optimal_range(0, 4 * 1024, 64 * 1024, 64 * 1024);

  cl_assert_equal_i(s_command_list_index, 1);
  cl_assert_equal_i(s_command_list[0].addr, 0);
  cl_assert_equal_i(s_command_list[0].type, SectorEraseCommand);

  s_command_list_index = 0;

  // Offer a more than a full sector range, needs a sector and a subsector
  flash_region_erase_optimal_range(60 * 1024, 60 * 1024, 2 * 64 * 1024, 2 * 64 * 1024);

  cl_assert_equal_i(s_command_list_index, 2);
  cl_assert_equal_i(s_command_list[0].addr, 60 * 1024);
  cl_assert_equal_i(s_command_list[0].type, SubsectorEraseCommand);
  cl_assert_equal_i(s_command_list[1].addr, 64 * 1024);
  cl_assert_equal_i(s_command_list[1].type, SectorEraseCommand);

  s_command_list_index = 0;

  // Offer a more than a full sector range, needs subsectors on both sides
  flash_region_erase_optimal_range(60 * 1024, 60 * 1024, ((2 * 64) + 4) * 1024, ((2 * 64) + 8) * 1024);

  cl_assert_equal_i(s_command_list_index, 3);
  cl_assert_equal_i(s_command_list[0].addr, 60 * 1024);
  cl_assert_equal_i(s_command_list[0].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[1].addr, 64 * 1024);
  cl_assert_equal_i(s_command_list[1].type, SectorEraseCommand);

  cl_assert_equal_i(s_command_list[2].addr, 2 * 64 * 1024);
  cl_assert_equal_i(s_command_list[2].type, SubsectorEraseCommand);
}

void test_flash_region__erase_optimal_range_96k_app_banks(void) {
  // Various tests that look like erasing our 96k app resource banks

  // App that's in an aligned bank but smaller than 64k
  flash_region_erase_optimal_range(0, 0, 32 * 1024, 96 * 1024);

  cl_assert_equal_i(s_command_list_index, 1);
  cl_assert_equal_i(s_command_list[0].addr, 0);
  cl_assert_equal_i(s_command_list[0].type, SectorEraseCommand);

  s_command_list_index = 0;

  // App that's in an aligned bank but larger than 64k
  flash_region_erase_optimal_range(0, 0, 69 * 1024, 96 * 1024);

  cl_assert_equal_i(s_command_list_index, 3);
  cl_assert_equal_i(s_command_list[0].addr, 0);
  cl_assert_equal_i(s_command_list[0].type, SectorEraseCommand);

  cl_assert_equal_i(s_command_list[1].addr, 64 * 1024);
  cl_assert_equal_i(s_command_list[1].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[2].addr, 68 * 1024);
  cl_assert_equal_i(s_command_list[2].type, SubsectorEraseCommand);

  s_command_list_index = 0;

  // App that's in an unaligned bank but smaller than 64k
  flash_region_erase_optimal_range(32 * 1024, 32 * 1024, (32 + 18) * 1024, (32 + 96) * 1024);

  cl_assert_equal_i(s_command_list_index, 5);

  cl_assert_equal_i(s_command_list[0].addr, 32 * 1024);
  cl_assert_equal_i(s_command_list[0].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[1].addr, 36 * 1024);
  cl_assert_equal_i(s_command_list[1].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[2].addr, 40 * 1024);
  cl_assert_equal_i(s_command_list[2].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[3].addr, 44 * 1024);
  cl_assert_equal_i(s_command_list[3].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[4].addr, 48 * 1024);
  cl_assert_equal_i(s_command_list[4].type, SubsectorEraseCommand);

  s_command_list_index = 0;

  // App that's in an unaligned bank but larger than 64k
  flash_region_erase_optimal_range(32 * 1024, 32 * 1024, (32 + 71) * 1024, (32 + 96) * 1024);

  cl_assert_equal_i(s_command_list_index, 9);

  cl_assert_equal_i(s_command_list[0].addr, 32 * 1024);
  cl_assert_equal_i(s_command_list[0].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[1].addr, 36 * 1024);
  cl_assert_equal_i(s_command_list[1].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[2].addr, 40 * 1024);
  cl_assert_equal_i(s_command_list[2].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[3].addr, 44 * 1024);
  cl_assert_equal_i(s_command_list[3].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[4].addr, 48 * 1024);
  cl_assert_equal_i(s_command_list[4].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[5].addr, 52 * 1024);
  cl_assert_equal_i(s_command_list[5].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[6].addr, 56 * 1024);
  cl_assert_equal_i(s_command_list[6].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[7].addr, 60 * 1024);
  cl_assert_equal_i(s_command_list[7].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[8].addr, 64 * 1024);
  cl_assert_equal_i(s_command_list[8].type, SectorEraseCommand);

  s_command_list_index = 0;

}

void test_flash_region__erase_optimal_range_watch_and_learn(void) {
  // Test cases stolen from Alvin's watch and learn app that originally hit this bug

  flash_region_erase_optimal_range(0x320000, 0x320000, 0x33177c, 0x338000);

  cl_assert_equal_i(s_command_list_index, 3);

  cl_assert_equal_i(s_command_list[0].addr, 0x320000);
  cl_assert_equal_i(s_command_list[0].type, SectorEraseCommand);

  cl_assert_equal_i(s_command_list[1].addr, 0x330000);
  cl_assert_equal_i(s_command_list[1].type, SubsectorEraseCommand);

  cl_assert_equal_i(s_command_list[2].addr, 0x331000);
  cl_assert_equal_i(s_command_list[2].type, SubsectorEraseCommand);
}

