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

#include "drivers/flash.h"
#include "services/common/new_timer/new_timer.h"

#include "clar.h"

#include <stdbool.h>

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_logging.h"
#include "stubs_passert.h"

// Fakes
///////////////////////////////////////////////////////////

typedef enum EraseCommandType {
  SectorEraseCommand = 1,
  SubsectorEraseCommand
} EraseCommandType;

typedef struct EraseCommand {
  uint32_t addr;
  EraseCommandType type;
} EraseCommand;

static EraseCommand s_command_list[32];
static int s_command_list_index = 0;
static int s_callback_called_count = 0;
static status_t s_callback_status;
static int s_simulate_flash_driver_error_countdown;
static int s_simulate_work_queue_full_countdown;
static bool s_erase_mutex_locked;

void prv_init_erase_mutex(void) {
}

void prv_lock_erase_mutex(void) {
  cl_assert_equal_i(s_erase_mutex_locked, false);
  s_erase_mutex_locked = true;
}

void prv_unlock_erase_mutex(void) {
  s_erase_mutex_locked = false;
}

void flash_erase_subsector_blocking(uint32_t subsector_addr) {
  s_command_list[s_command_list_index++] = (EraseCommand) {
      .addr = subsector_addr,
      .type = SubsectorEraseCommand
  };
}

void flash_erase_sector_blocking(uint32_t sector_addr) {
  s_command_list[s_command_list_index++] = (EraseCommand) {
      .addr = sector_addr,
      .type = SectorEraseCommand
  };
}

void flash_erase_subsector(uint32_t subsector_addr, FlashOperationCompleteCb cb,
                           void *context) {
  s_command_list[s_command_list_index++] = (EraseCommand) {
      .addr = subsector_addr,
      .type = SubsectorEraseCommand
  };
  cl_assert_equal_i(s_erase_mutex_locked, true);
  cb(context, (--s_simulate_flash_driver_error_countdown? S_SUCCESS : E_BUSY));
}

void flash_erase_sector(uint32_t sector_addr, FlashOperationCompleteCb cb,
                        void *context) {
  s_command_list[s_command_list_index++] = (EraseCommand) {
      .addr = sector_addr,
      .type = SectorEraseCommand
  };
  cl_assert_equal_i(s_erase_mutex_locked, true);
  cb(context, (--s_simulate_flash_driver_error_countdown? S_SUCCESS : E_BUSY));
}

bool new_timer_add_work_callback(NewTimerWorkCallback cb, void *data) {
  if (--s_simulate_work_queue_full_countdown == 0) {
    return false;
  }
  cb(data);
  return true;
}

static void prv_assert_erase_commands(EraseCommand *commands) {
  int expected_command_count = 0;
  EraseCommand *cursor = commands;
  while ((cursor++)->type != 0) {
    ++expected_command_count;
  }
  cl_assert_equal_i(s_command_list_index, expected_command_count);
  for (int i = 0; i < s_command_list_index; ++i) {
    cl_assert_equal_i(s_command_list[i].addr, commands[i].addr);
    cl_assert_equal_i(s_command_list[i].type, commands[i].type);
  }
}

static int s_dummy_value = 42;
static int *s_callback_ctx = &s_dummy_value;

static void prv_callback(void *context, status_t status) {
  cl_assert_equal_p(context, s_callback_ctx);
  cl_assert_equal_i(s_erase_mutex_locked, false);
  s_callback_status = status;
  s_callback_called_count++;
}

static void prv_assert_callback_called(status_t expected_status) {
  cl_assert_equal_i(s_callback_called_count, 1);
  cl_assert_equal_i(s_callback_status, expected_status);
}

static void prv_test_erase_optimal_range(
    uint32_t min_start, uint32_t max_start, uint32_t min_end, uint32_t max_end,
    EraseCommand *expected_commands) {
  flash_erase_optimal_range(min_start, max_start, min_end, max_end,
                            prv_callback, s_callback_ctx);
  prv_assert_erase_commands(expected_commands);
  prv_assert_callback_called(
      s_command_list_index? S_SUCCESS : S_NO_ACTION_REQUIRED);
}

// Tests
///////////////////////////////////////////////////////////

void test_flash_erase__initialize(void) {
  s_command_list_index = 0;
  s_callback_called_count = 0;
  s_callback_status = 42;
  s_simulate_work_queue_full_countdown = -1;
  s_simulate_flash_driver_error_countdown = -1;
  s_erase_mutex_locked = false;
}

void test_flash_erase__cleanup(void) {
}

void test_flash_erase__empty(void) {
  prv_test_erase_optimal_range(0, 0, 0, 0, (EraseCommand[]){ { } });
}

void test_flash_erase__sectors_simple_1(void) {
  // Erase one sector 0x10000 - 0x20000
  prv_test_erase_optimal_range(
      64 * 1024, 64 * 1024, 2 * 64 * 1024, 2 * 64 * 1024,
      (EraseCommand[]) {
        { 64 * 1024, SectorEraseCommand },
        { },
      });
}

void test_flash_erase__sectors_simple_2(void) {
  // Erase one sectors 0x10000 - 0x20000 but allow us to erase more
  prv_test_erase_optimal_range(
      0, 64 * 1024, 2 * 64 * 1024, 3 * 64 * 1024,
      (EraseCommand[]) {
        { 64 * 1024, SectorEraseCommand },
        { },
      });
}

void test_flash_erase__two_sectors(void) {
  // Erase two sectors 0x10000 - 0x30000 but allow us to erase more
  prv_test_erase_optimal_range(
      0, 64 * 1024, 3 * 64 * 1024, 4 * 64 * 1024,
      (EraseCommand[]) {
          { 64 * 1024, SectorEraseCommand },
          { 2 * 64 * 1024, SectorEraseCommand },
          { },
      });
}

void test_flash_erase__subsectors_1(void) {
  // Offer a less than full sector range but erase the full range
  prv_test_erase_optimal_range(
      0, 4 * 1024, 64 * 1024, 64 * 1024,
      (EraseCommand[]) {
          { 0, SectorEraseCommand },
          { },
      });
}

void test_flash_erase__sector_and_subsector(void) {
  // Offer a more than a full sector range, needs a sector and a subsector
  prv_test_erase_optimal_range(
      60 * 1024, 60 * 1024, 2 * 64 * 1024, 2 * 64 * 1024,
      (EraseCommand[]) {
          { 60 * 1024, SubsectorEraseCommand },
          { 64 * 1024, SectorEraseCommand },
          { },
      });
}

void test_flash_erase__subsectors_on_both_sides(void) {
  // Offer a more than a full sector range, needs subsectors on both sides
  prv_test_erase_optimal_range(
      60 * 1024, 60 * 1024, ((2 * 64) + 4) * 1024, ((2 * 64) + 8) * 1024,
      (EraseCommand[]) {
          { 60 * 1024, SubsectorEraseCommand },
          { 64 * 1024, SectorEraseCommand },
          { 2 * 64 * 1024, SubsectorEraseCommand },
          { },
      });
}

// Various tests that look like erasing our 96k app resource banks

void test_flash_erase__96k_app_banks_1(void) {
  // App that's in an aligned bank but smaller than 64k
  prv_test_erase_optimal_range(
      0, 0, 32 * 1024, 96 * 1024,
      (EraseCommand[]) {
          { 0, SectorEraseCommand },
          { },
      });
}

void test_flash_erase__96k_app_banks_2(void) {
  // App that's in an aligned bank but larger than 64k
  prv_test_erase_optimal_range(
      0, 0, 69 * 1024, 96 * 1024,
      (EraseCommand[]) {
          { 0, SectorEraseCommand },
          { 64 * 1024, SubsectorEraseCommand },
          { 68 * 1024, SubsectorEraseCommand },
          { },
      });
}

void test_flash_erase__96k_app_banks_3(void) {
  // App that's in an unaligned bank but smaller than 64k
  prv_test_erase_optimal_range(
      32 * 1024, 32 * 1024, (32 + 18) * 1024, (32 + 96) * 1024,
      (EraseCommand[]) {
          { 32 * 1024, SubsectorEraseCommand },
          { 36 * 1024, SubsectorEraseCommand },
          { 40 * 1024, SubsectorEraseCommand },
          { 44 * 1024, SubsectorEraseCommand },
          { 48 * 1024, SubsectorEraseCommand },
          { },
      });
}

void test_flash_erase__96k_app_banks_4(void) {
  // App that's in an unaligned bank but larger than 64k
  prv_test_erase_optimal_range(
      32 * 1024, 32 * 1024, (32 + 71) * 1024, (32 + 96) * 1024,
      (EraseCommand[]) {
          { 32 * 1024, SubsectorEraseCommand },
          { 36 * 1024, SubsectorEraseCommand },
          { 40 * 1024, SubsectorEraseCommand },
          { 44 * 1024, SubsectorEraseCommand },
          { 48 * 1024, SubsectorEraseCommand },
          { 52 * 1024, SubsectorEraseCommand },
          { 56 * 1024, SubsectorEraseCommand },
          { 60 * 1024, SubsectorEraseCommand },
          { 64 * 1024, SectorEraseCommand },
          { },
      });
}

void test_flash_erase__watch_and_learn(void) {
  // Test cases stolen from Alvin's watch and learn app that originally hit this bug
  prv_test_erase_optimal_range(
      0x320000, 0x320000, 0x33177c, 0x338000,
      (EraseCommand[]) {
          { 0x320000, SectorEraseCommand },
          { 0x330000, SubsectorEraseCommand },
          { 0x331000, SubsectorEraseCommand },
          { },
      });
}

void test_flash_erase__handle_work_queue_full(void) {
  s_simulate_work_queue_full_countdown = 3;
  flash_erase_optimal_range(
      32 * 1024, 32 * 1024, (32 + 71) * 1024, (32 + 96) * 1024,
      prv_callback, s_callback_ctx);
  prv_assert_callback_called(E_INTERNAL);
  prv_assert_erase_commands((EraseCommand[]) {
          { 32 * 1024, SubsectorEraseCommand },
          { 36 * 1024, SubsectorEraseCommand },
          { 40 * 1024, SubsectorEraseCommand },
          { },
  });
}

void test_flash_erase__handle_flash_driver_error(void) {
  s_simulate_flash_driver_error_countdown = 3;
  flash_erase_optimal_range(
      32 * 1024, 32 * 1024, (32 + 71) * 1024, (32 + 96) * 1024,
      prv_callback, s_callback_ctx);
  prv_assert_callback_called(E_BUSY);
  prv_assert_erase_commands((EraseCommand[]) {
          { 32 * 1024, SubsectorEraseCommand },
          { 36 * 1024, SubsectorEraseCommand },
          { 40 * 1024, SubsectorEraseCommand },
          { },
  });
}
