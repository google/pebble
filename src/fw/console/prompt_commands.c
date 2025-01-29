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

#include "prompt_commands.h"

#include "applib/graphics/8_bit/framebuffer.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "comm/ble/gap_le_connection.h"
#include "comm/bt_lock.h"
#include "console_internal.h"
#include "dbgserial.h"
#include "debug/flash_logging.h"
#include "drivers/flash.h"
#include "drivers/task_watchdog.h"
#include "flash_region/flash_region.h"
#include "kernel/event_loop.h"
#include "kernel/logging_private.h"
#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "kernel/util/delay.h"
#include "kernel/util/factory_reset.h"
#include "kernel/util/sleep.h"
#include "kernel/util/stop.h"
#include "mfg/mfg_apps/mfg_flash_test.h"
#include "process_management/app_manager.h"
#include "process_management/worker_manager.h"
#include "prompt.h"
#include "resource/resource_storage_flash.h"
#include "services/common/compositor/compositor.h"
#include "services/common/system_task.h"
#include "services/normal/filesystem/pfs.h"
#include "syscall/syscall.h"
#include "system/bootbits.h"
#include "system/hexdump.h"
#include "system/logging.h"
#include "system/passert.h"
#include "system/reboot_reason.h"
#include "system/reset.h"
#include "util/math.h"
#include "util/net.h"
#include "util/string.h"

#define CMSIS_COMPATIBLE
#include <mcu.h>

#include <bluetooth/bt_test.h>
#include <bluetooth/responsiveness.h>
#include <bluetooth/gatt_discovery.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static TimerID s_console_button_timer = TIMER_INVALID_ID;

static void prv_pfs_stress_callback(void *data) {
  pfs_remove_files(NULL);

  system_task_add_callback(prv_pfs_stress_callback, NULL);
}

// Issue regular pfs accesses from KernelBG
void pfs_command_stress(void) {
  prompt_send_response("PFS stress from kernel BG");
  system_task_add_callback(prv_pfs_stress_callback, NULL);
}

extern void command_read_word(const char* address_str) {
  int32_t address = str_to_address(address_str);
  if (address == -1) {
    prompt_send_response("Invalid address");
    return;
  }

  uint32_t word = *(uint32_t*) address;

  char buffer[32];
  prompt_send_response_fmt(buffer, sizeof(buffer), "0x%"PRIx32" = 0x%"PRIx32, address, word);
}

void command_format_flash(void) {
  flash_erase_bulk();
}

void command_erase_flash(const char *address_str, const char *length_str) {
  int32_t address = str_to_address(address_str);
  if (address < 0) {
    prompt_send_response("Invalid address");
    return;
  }

  int length = atoi(length_str);
  if (length <= 0) {
    prompt_send_response("Invalid length");
    return;
  }

  char buffer[128];
  prompt_send_response_fmt(buffer, 128, "Erasing sectors from 0x%"PRIx32" for %ub",
                           address, length);

  const uint32_t end_address = address + length;
  const uint32_t aligned_end_address =
      (end_address + (SUBSECTOR_SIZE_BYTES - 1)) & SUBSECTOR_ADDR_MASK;

  flash_region_erase_optimal_range_no_watchdog(address, address, end_address, aligned_end_address);

  prompt_send_response("OK");
}

void command_dump_flash(const char* address_str, const char* length_str) {
  int32_t address = str_to_address(address_str);
  if (address == -1) {
    prompt_send_response("Invalid address");
    return;
  }

  int length = atoi(length_str);
  if (length == 0) {
    prompt_send_response("Invalid length");
    return;
  }

  // Temporarily turn on logging so the hexdump comes out.
  serial_console_set_state(SERIAL_CONSOLE_STATE_LOGGING);

  uint8_t buffer[128];

  while (length) {
    uint32_t chunk_size = MIN(length, 128);
    flash_read_bytes(buffer, address, chunk_size);

    PBL_LOG(LOG_LEVEL_ALWAYS, "Data at address 0x%"PRIx32, address);
    hexdump_log(LOG_LEVEL_ALWAYS, buffer, chunk_size);

    address += chunk_size;
    length -= chunk_size;
  }

  // Go back to the prompt.
  serial_console_set_state(SERIAL_CONSOLE_STATE_PROMPT);
}

void command_crc_flash(const char* address_str, const char* length_str) {
  int32_t address = str_to_address(address_str);
  if (address == -1) {
    prompt_send_response("Invalid address");
    return;
  }

  int length = atoi(length_str);
  if (length == 0) {
    prompt_send_response("Invalid length");
    return;
  }

  uint32_t crc = flash_calculate_legacy_defective_checksum(address, length);
  char buffer[32];
  prompt_send_response_fmt(buffer, sizeof(buffer), "CRC: %"PRIx32, crc);
}

#define MAX_READ_FLASH_SIZE 1024 // 1KB
void command_flash_read(const char* address_str, const char* length_str) {
  // Read data from flash and output the data directly to serial port in segmented chunks

  int32_t address = str_to_address(address_str);
  if (address == -1) {
    prompt_send_response("Invalid address");
    return;
  }

  int length = atoi(length_str);
  if (length == 0) {
    prompt_send_response("Invalid length");
    return;
  }

  // Allocate a 1KB buffer to read data in segments
  uint8_t *buffer = (uint8_t *) kernel_malloc(MIN(MAX_READ_FLASH_SIZE,length));
  if (buffer == 0) {
    prompt_send_response("Unable to allocate read buffer");
    return;
  }

  while (length > 0) {
    uint32_t read_length = MAX_READ_FLASH_SIZE;
    if (length < MAX_READ_FLASH_SIZE){
      read_length = length;
    }

    flash_read_bytes(buffer, address, read_length);

    // Output to serial
    for (uint32_t i = 0; i < read_length; i++) {
      dbgserial_putchar(buffer[i]);
    }

    address += read_length;
    length -= read_length;
  }

  kernel_free(buffer);
}

void command_flash_switch_mode (const char* mode_str) {
  int mode = atoi(mode_str);
  flash_switch_mode(mode);
}

#define WRITE_PAGE_SIZE_BYTES 64
void command_flash_fill (const char* address_str, const char* length_str, const char* value_str) {
  int32_t address = str_to_address(address_str);
  if (address == -1) {
    prompt_send_response("Invalid address");
    return;
  }

  int length = atoi(length_str);
  if (length <= 0) {
    prompt_send_response("Invalid length");
    return;
  }

  int value = atoi(value_str);
  if ((value < 0) || (value > 0xFF)) {
    prompt_send_response("Invalid value");
    return;
  }

  // Fill flash with a character value
  uint8_t page[WRITE_PAGE_SIZE_BYTES];
  for (uint32_t i = 0; i < WRITE_PAGE_SIZE_BYTES; i++) {
    page[i] = (uint8_t)(value++ & 0xFF);
  }

  uint32_t bytes_remaining = length;
  while (bytes_remaining > 0)
  {
    uint32_t bytes_to_write = WRITE_PAGE_SIZE_BYTES;
    if (bytes_remaining < WRITE_PAGE_SIZE_BYTES) {
      bytes_to_write = bytes_remaining;
    }

    flash_write_bytes(page, address, bytes_to_write);
    bytes_remaining -= bytes_to_write;
    address += bytes_to_write;
  }
}

// Pass in test case number and number of iterations to run
//   Currently iterations only applies to FLASH_TEST_CASE_RUN_STRESS_ADDR_TEST
//   All other tests run once
void command_flash_test(const char* test_case_num_str, const char* iterations_str) {
  int32_t test_case_num = atoi(test_case_num_str);
  int32_t iterations = atoi(iterations_str);

  int32_t status = FLASH_TEST_ERR_OTHER;
  if (!((test_case_num == FLASH_TEST_CASE_RUN_STRESS_ADDR_TEST) && (iterations <= 0))) {
    // Check to make sure stress test has at least 1 iteration or its another test case
    status = run_flash_test_case(test_case_num, iterations);
  }

  char buffer[80];
  if (status == 0) {
    prompt_send_response_fmt(buffer, sizeof(buffer), "PASS: TEST CASE %"PRId32, test_case_num);
  } else {
    prompt_send_response_fmt(buffer, sizeof(buffer),
                             ">FAIL: TEST CASE %"PRId32", Status: %"PRId32,
                             test_case_num, status);
  }
}

void command_flash_validate(void) {
  // just test one sector, which is probably less than the size of the region
  const uint32_t TEST_ADDR = FLASH_REGION_FIRMWARE_SCRATCH_BEGIN;
  const uint32_t TEST_LENGTH = SECTOR_SIZE_BYTES;
  PBL_ASSERTN((TEST_ADDR & SECTOR_ADDR_MASK) == TEST_ADDR);
  PBL_ASSERTN((TEST_ADDR + TEST_LENGTH) <= FLASH_REGION_FIRMWARE_SCRATCH_END);

  // erase a sector
  flash_erase_sector_blocking(TEST_ADDR);
  if (!flash_sector_is_erased(TEST_ADDR)) {
    prompt_send_response("FAIL: sector not erased");
    return;
  }

  // write data into the sector
  const uint32_t BUFFER_SIZE = 256;
  uint8_t buffer[BUFFER_SIZE];
  for (uint32_t i = 0; i < BUFFER_SIZE; i++) {
    buffer[i] = i;
  }
  for (uint32_t offset = 0; offset < TEST_LENGTH; offset += BUFFER_SIZE) {
    const uint32_t addr = TEST_ADDR + offset;
    flash_write_bytes(buffer, addr, BUFFER_SIZE);
  }

  // read it back
  for (uint32_t offset = 0; offset < TEST_LENGTH; offset += BUFFER_SIZE) {
    memset(buffer, 0, BUFFER_SIZE);
    const uint32_t addr = TEST_ADDR + offset;
    flash_read_bytes(buffer, addr, BUFFER_SIZE);
    for (uint32_t i = 0; i < BUFFER_SIZE; i++) {
      if (buffer[i] != i) {
        char err_buf[80];
        prompt_send_response_fmt(err_buf, sizeof(err_buf), "FAIL: Incorrect value at 0x%"PRIx32,
                                 addr + i);
        return;
      }
    }
  }

  // read it back, albeit awkwardly. We have seen issues that arise when stitching different
  // types of flash ops together (i.e single byte reads followed by memmaps)
  const uint32_t SHORT_TEST_LENGTH = 1000; // single byte reads are slow so do a shorter test length
  for (uint32_t offset = 0; offset < SHORT_TEST_LENGTH ; offset++) {
    uint8_t memmap_buffer[130]; // > 128 bytes, triggers a memmap read for QSPI
    memset(memmap_buffer, 0x00, sizeof(memmap_buffer));

    const uint32_t pre_addr = TEST_ADDR + offset - MIN(offset, 1);
    uint8_t pre_byte;
    flash_read_bytes(&pre_byte, pre_addr, sizeof(pre_byte));

    const uint32_t addr = TEST_ADDR + offset;
    size_t read_size = MIN(sizeof(memmap_buffer), SHORT_TEST_LENGTH - offset);
    flash_read_bytes(&memmap_buffer[0], addr, read_size);
    for (size_t i = 0; i < read_size; i++) {
      uint8_t want = (offset + i) & 0xff;
      if (memmap_buffer[i] != want) {
        char err_buf[80];
        prompt_send_response_fmt(err_buf, sizeof(err_buf), "FAIL at ADDR %d Got: %d Wanted %d",
                                 (int)offset, (int)memmap_buffer[i], (int)want);
        break;
      }
    }
  }

  // clean up
  flash_erase_sector_blocking(TEST_ADDR);
  if (!flash_sector_is_erased(TEST_ADDR)) {
    prompt_send_response("FAIL: sector not erased");
    return;
  }

  prompt_send_response("OK");
}


//! Some flash chips have an accelerated method of checking for erased sectors. This is a sanity
//! check against that method. It reads the bytes in raw form and makes sure it is really erased.
static bool prv_is_really_erased(uint32_t addr, bool is_subsector) {
  bool erased = (is_subsector) ? flash_subsector_is_erased(addr) : flash_sector_is_erased(addr);
  if (erased) {
    char buffer[64];
    uint32_t end_addr = addr + (is_subsector ? SUBSECTOR_SIZE_BYTES : SECTOR_SIZE_BYTES);
    for (uint32_t i_addr = addr; i_addr < end_addr; i_addr += sizeof(buffer)) {
      flash_read_bytes((uint8_t *)buffer, i_addr, sizeof(buffer));
      for (uint32_t j = 0; j < sizeof(buffer); j++) {
        if (buffer[j] != 0xFF) {
          erased = false;
          prompt_send_response_fmt(buffer, sizeof(buffer),
              "(Sub)Sector at addr: 0x%"PRIX32" not really erased. is_subsector: %d",
              addr, is_subsector);
          goto done;
        }
      }
    }
  }

done:
  return erased;
}

// ARG:
// 0 - Only show sectors
// 1 - Show subsectors too if sector is not erased
void command_flash_show_erased_sectors(const char *arg) {
  const bool show_subsectors = (atoi(arg) == 1);

  char buffer[64];
  uint32_t addr = 0;
  while (addr < BOARD_NOR_FLASH_SIZE) {
    bool erased = prv_is_really_erased(addr, false);
    prompt_send_response_fmt(buffer, sizeof(buffer), "SECTOR - 0x%-6"PRIX32" :: %s",
                             addr, erased ? "true" : "false");
    if (show_subsectors && !erased) {
      for (uint32_t i = 0; i < (SECTOR_SIZE_BYTES / SUBSECTOR_SIZE_BYTES); i++) {
        const uint32_t sub_addr = (addr + (i * SUBSECTOR_SIZE_BYTES));
        bool sub_erased = prv_is_really_erased(sub_addr, true);
        prompt_send_response_fmt(buffer, sizeof(buffer), "  SUBSECTOR - 0X%-6"PRIx32" :: %s",
                                 sub_addr, sub_erased ? "true" : "false");
      }
    }
    addr += SECTOR_SIZE_BYTES;
    task_watchdog_bit_set(pebble_task_get_current());
  }
}

/*
 * Commented out by default because this command can harm your flash chip!!
static void prv_flash_stress_callback(void *data) {
  uint32_t flash_addr = FLASH_REGION_DEBUG_DB_BEGIN;
  uint32_t sector_address = flash_get_sector_base_address(flash_addr);
  PBL_LOG(LOG_LEVEL_DEBUG, "erasing flash address %x", sector_address);
  flash_erase_sector_blocking(sector_address);

  const char* data_to_write = "hello world";
  PBL_LOG(LOG_LEVEL_DEBUG, "writing to flash address %x", flash_addr);
  flash_write_bytes((const uint8_t *)data_to_write, flash_addr, strlen(data_to_write));
  system_task_add_callback(prv_flash_stress_callback, NULL);
}


void command_flash_stress(void) {
  // WARNING!! Running this test can shorten the life of your flash chip because it violates the
  // "wait 90 seconds between erases of the same sector" spec.
  prompt_send_response("Stress flash");
  system_task_add_callback(prv_flash_stress_callback, NULL);
}
*/


void command_reset() {
  prompt_command_finish();

  RebootReason reason = { RebootReasonCode_Serial, 0 };
  reboot_reason_set(&reason);
  system_reset();
}

void command_crash() {
  prompt_command_finish();

  RebootReason reason = { RebootReasonCode_LauncherPanic, 0 };
  reboot_reason_set(&reason);
  system_reset();
}

void command_hard_crash() {
  prompt_command_finish();

  RebootReason reason = { RebootReasonCode_HardFault, 0 };
  reboot_reason_set(&reason);
  boot_bit_set(BOOT_BIT_FW_START_FAIL_STRIKE_TWO);
  boot_bit_set(BOOT_BIT_SOFTWARE_FAILURE_OCCURRED);
  boot_bit_clear(BOOT_BIT_FW_STABLE);
  system_hard_reset();
}

void command_boot_prf(void) {
  prompt_command_finish();

  RebootReason reason = { RebootReasonCode_Serial, 0 };
  reboot_reason_set(&reason);
  boot_bit_set(BOOT_BIT_FORCE_PRF);
  system_reset();
}

void command_infinite_loop(void) {
  while(1);
}

void stuck_timer_cb(void* data) {
  while(1);
}

#include "services/common/new_timer/new_timer.h"
void command_stuck_timer(void) {
  TimerID timer = new_timer_create();
  new_timer_start(timer, 10, stuck_timer_cb, NULL, 0 /*flags*/);
}

#include "drivers/rtc.h"

void command_assert_fail(void) {
  prompt_command_finish();

  RtcTicks ticks = rtc_get_ticks();
  PBL_ASSERT(false, "The world doesn't make sense anymore! Tick count: 0x%08" PRIx32 "%08" PRIx32,
             SPLIT_64_BIT_ARG(ticks));
}

void command_croak(void) {
  prompt_command_finish();

  PBL_CROAK("You asked for this!");
}

typedef void (*KaboomCallback)(void);

void command_hardfault(void) {
  prompt_command_finish();

  KaboomCallback kaboom = 0;
  kaboom();
}

void command_boot_bit_set(const char* bit, const char* value) {
  int len = strlen(bit);
  int bit_number = 0;

  for (int i = 0; i < len; ++i) {
    bit_number *= 10;
    int next_digit = bit[i] - '0';
    if (next_digit < 0 || next_digit > 9) {
      prompt_send_response("invalid bit number");
      return;
    }
    bit_number += next_digit;
  }

  int bit_mask = 1 << bit_number;

  if (value[0] == '0') {
    boot_bit_clear(bit_mask);
  } else if (value[0] == '1') {
    boot_bit_set(bit_mask);
  } else {
    prompt_send_response("invalid bit value, pick 1 or 0");
    return;
  }
  prompt_send_response("OK bit assigned");
}

typedef struct {
  ButtonId button_id;
  bool button_is_held_down;
  uint32_t num_presses_remaining;
  uint32_t hold_down_time_ms;
  uint32_t delay_between_presses_ms;
} ButtonPressNewTimerContext;

// This is a callback to only be used in conjunction with command_button_press() and
// command_button_press_multiple()
static void command_button_press_callback(void *cb_data) {
  ButtonPressNewTimerContext *context = cb_data;

  const bool button_is_held_down = context->button_is_held_down;
  // Choose the next event type to emit and the next timeout based on the current button state
  PebbleEventType next_event_type = button_is_held_down ? PEBBLE_BUTTON_UP_EVENT :
                                                          PEBBLE_BUTTON_DOWN_EVENT;
  const uint32_t next_timeout_ms = button_is_held_down ? context->delay_between_presses_ms :
                                                         context->hold_down_time_ms;

  // Add the next button event to the queue
  PebbleEvent next_button_event = {
    .type = next_event_type,
    .button.button_id = context->button_id,
  };
  event_put(&next_button_event);

  // Decrement the number of presses remaining if the button is currently held down (because we
  // just pushed it up by adding that event)
  if (button_is_held_down) {
    context->num_presses_remaining--;
  }

  if (context->num_presses_remaining > 0) {
    // Toggle the state of the button
    context->button_is_held_down = !button_is_held_down;
    // Restart the timer
    new_timer_start(s_console_button_timer, next_timeout_ms, command_button_press_callback,
                    context, 0 /* flags */);
  } else {
    kernel_free(context);
  }
}

static bool prv_convert_and_validate_timeout_value(const char *timeout_string,
                                                   uint32_t default_value,
                                                   uint32_t *result) {
  if (!result) {
    return false;
  }

  if (!timeout_string) {
    *result = default_value;
    return true;
  }

  char *end;
  *result = MAX(0, strtol(timeout_string, &end, 10));
  return (*end == '\0');
}

//! Press a button multiple times.
//! @param button_index The index of the button in \ref ButtonId to press.
//! @param presses The number of times to press the button. If NULL, defaults to 1. If <= 0, errors.
//! @param hold_down_time_ms The time (in ms) to hold down the button for each press. If NULL,
//!        defaults to 20 ms. If < 0, errors.
//! @param delay_between_presses_ms The time (in ms) to delay between successive button presses.
//!        If NULL, defaults to 0. If < 0, errors.
static void prv_button_press_multiple(const char *button_index, const char *presses,
                                      const char *hold_down_time_ms,
                                      const char *delay_between_presses_ms) {
  const uint32_t default_delay = 20;

  // Convert and validate the button value
  int button = atoi(button_index);
  if (!WITHIN(button, 0, NUM_BUTTONS - 1)) {
    goto error;
  }

  const ButtonId button_id = (ButtonId)button;

  uint32_t num_presses = 1;
  // If presses is NULL, default to 1; otherwise convert the char string to an integer
  if (presses) {
    char *end;
    num_presses = MAX(0, strtol(presses, &end, 10));
    // Validate the num_presses value
    if (*end != '\0') {
      goto error;
    }
  }

  // If hold_down_time_ms is NULL it's a short press so use default value
  // Otherwise convert the char string to an integer; error if the converted value is negative
  uint32_t hold_down_timeout_ms;
  if (!prv_convert_and_validate_timeout_value(hold_down_time_ms, default_delay,
                                              &hold_down_timeout_ms)) {
    goto error;
  }

  // If delay_between_presses_ms is NULL then default to 0 milliseconds
  // Otherwise convert the char string to an integer; error if the converted value is negative
  uint32_t delay_between_presses_timeout_ms;
  if (!prv_convert_and_validate_timeout_value(delay_between_presses_ms, 0,
                                              &delay_between_presses_timeout_ms)) {
    goto error;
  }

  // Initialize timer on first use
  if (s_console_button_timer == TIMER_INVALID_ID) {
    s_console_button_timer = new_timer_create();
  }

  // If the callback is already scheduled, notify busy and exit
  if (new_timer_scheduled(s_console_button_timer, NULL)) {
    prompt_send_response("BUSY");
    return;
  }

  // Construct our new_timer context, will be freed in command_button_press_callback()
  ButtonPressNewTimerContext *new_timer_context = kernel_malloc(sizeof(ButtonPressNewTimerContext));
  if (!new_timer_context) {
    goto error;
  }
  *new_timer_context = (ButtonPressNewTimerContext) {
    .button_id = button_id,
    .button_is_held_down = false,
    .num_presses_remaining = num_presses,
    .hold_down_time_ms = hold_down_timeout_ms,
    .delay_between_presses_ms = delay_between_presses_timeout_ms,
  };

  // In order to avoid race conditions between button events and timers being registered, drive the
  // entire multi click sequence in new_timers. The callback will re-register this timer as needed
  // for each subsequent click event in the sequence.
  const bool timer_started = new_timer_start(s_console_button_timer, 0,
                                             command_button_press_callback, new_timer_context,
                                             0 /* flags */);

  if (!timer_started) {
    kernel_free(new_timer_context);
    goto error;
  }

  prompt_send_response("OK");
  return;

  error:
    prompt_send_response("ERROR");
}

// Perform a button press from the serial console.  Three responses are provided
// to users/tools using the interface to indicate status.  They are: OK, BUSY, and ERROR.
void command_button_press(const char *button_index, const char *hold_down_time_ms) {
  prv_button_press_multiple(button_index, NULL, hold_down_time_ms, NULL);
}

// Perform multiple presses of the same button from the serial console. Three responses are
// provided to users/tools using the interface to indicate status.  They are: OK, BUSY, and ERROR.
void command_button_press_multiple(const char *button_index, const char *num_presses,
                                   const char *hold_down_time_ms,
                                   const char *delay_between_presses_ms) {
  prv_button_press_multiple(button_index, num_presses, hold_down_time_ms, delay_between_presses_ms);
}

static void prv_button_press_short_launcher_task_cb(void *data) {
  uintptr_t button = (uintptr_t)data;
  PebbleEvent e = {
    .type = PEBBLE_BUTTON_DOWN_EVENT,
    .button.button_id = button
  };
  event_put(&e);
  e = (PebbleEvent) {
    .type = PEBBLE_BUTTON_UP_EVENT,
    .button.button_id = button
  };
  event_put(&e);
}

void command_button_press_short(const char* button_index) {
  uintptr_t button = (uintptr_t)atoi(button_index);
  launcher_task_add_callback(prv_button_press_short_launcher_task_cb, (void *)button);
  prompt_send_response("OK");
}

void command_factory_reset(void) {
  prompt_command_finish();

  factory_reset(false /* should_shutdown */);
}

void command_factory_reset_fast(void) {
  prompt_command_finish();

  worker_manager_disable();

  while (worker_manager_get_current_worker_md()) {
    // Wait for the worker to die
    psleep(3);
  }

  launcher_task_add_callback(factory_reset_fast, NULL);
}

static bool prv_serial_dump_chunk_callback(uint8_t* msg, uint32_t total_length) {
  LogBinaryMessage* message = (LogBinaryMessage *)msg;

  char buffer[256];
  char time_buffer[TIME_STRING_BUFFER_SIZE];
  message->message[message->message_length] = 0;
  prompt_send_response_fmt(buffer, sizeof(buffer), "%c %s %s:%d> %s",
                           pbl_log_get_level_char(message->log_level),
                           time_t_to_string(time_buffer, htonl(message->timestamp)),
                           message->filename,
                           (int)htons(message->line_number),
                           message->message);
  return true;
}

static void prv_serial_dump_completed_callback(bool success) {
  prompt_command_finish();
}

void command_log_dump_current(void) {
  flash_dump_log_file(0, prv_serial_dump_chunk_callback, prv_serial_dump_completed_callback);
  prompt_command_continues_after_returning();
}

void command_log_dump_last(void) {
  flash_dump_log_file(1, prv_serial_dump_chunk_callback, prv_serial_dump_completed_callback);
  prompt_command_continues_after_returning();
}

void command_log_dump_generation(const char* generation_str) {
  int generation = atoi(generation_str);
  flash_dump_log_file(generation, prv_serial_dump_chunk_callback,
                      prv_serial_dump_completed_callback);
  prompt_command_continues_after_returning();
}

static void spam_callback(void *data) {
  uint32_t iteration = (uintptr_t) data;
  uint8_t buffer[128];
  time_t base = sys_get_time();
  for (int i = 0; i < 16; ++i) {
    LogBinaryMessage* msg = (LogBinaryMessage*) buffer;
    msg->timestamp = htonl(base + iteration * 16 + i);
    msg->log_level = LOG_LEVEL_ERROR;
    msg->message_length = sizeof(buffer) - sizeof(LogBinaryMessage);
    msg->line_number = 0;
    strncpy(msg->filename, "spam.exe", sizeof(msg->filename));
    char letter = 'A' + i;
    memset(msg->message, letter, msg->message_length - 1);
    msg->message[msg->message_length - 1] = 0;

    uint32_t flash_addr = flash_logging_log_start(sizeof(buffer));
    flash_logging_write(buffer, flash_addr, sizeof(buffer));
  }
  (void)data;
}

void command_log_dump_spam(void) {
  prompt_send_response("Spam logs!");
  for (int i = 0; i < 16; ++i) {
    system_task_add_callback(spam_callback, (void *)(uintptr_t) i);
  }
}

#ifdef TEST_FLASH_LOCK_PROTECTION
#include "flash_region/flash_region.h"
#include "drivers/task_watchdog.h"
#include "drivers/watchdog.h"

// This test attempts to write over every region of the flash.
// If we can still boot PRF after running this, it means we have successfully
// protected those regions
void flash_expect_program_failure(bool expect_failure);
void command_flash_test_locked_sectors(void) {
  // write 0's to the entire flash
  static uint8_t buf[2048] = { 0 };
  char status[80];

  __disable_irq();

  for (int i = 0; i < 2; i++) {
    for (uint32_t addr = 0; addr < BOARD_NOR_FLASH_SIZE; addr += sizeof(buf)) {
      if (addr >=  FLASH_REGION_SAFE_FIRMWARE_BEGIN &&
          addr < FLASH_REGION_SAFE_FIRMWARE_END) {
        flash_expect_program_failure(true);
      }

      if ((addr % SECTOR_SIZE_BYTES) == 0) {
        prompt_send_response_fmt(status, sizeof(status), "Validated: 0x%lx", addr);
        flash_erase_sector_blocking(addr);
        flash_erase_sector_blocking(addr); // exercise already erased check
      }

      flash_write_bytes(&buf[0], addr, sizeof(buf));

      flash_expect_program_failure(false);
      watchdog_feed();
    }
  }

  task_watchdog_bit_set(pebble_task_get_current());
  __enable_irq();
}
#endif

static TimerID s_abusive_timer = TIMER_INVALID_ID;

struct WasteTimerData {
  uint16_t count;
  uint16_t delay;
};
_Static_assert(sizeof(struct WasteTimerData) <= sizeof(uintptr_t),
               "struct WasteTimerData too big");

static void prv_waste_time_cb(void *context) {
  struct WasteTimerData data;
  memcpy(&data, &context, sizeof data);

  for (int i = 0; i < data.delay; ++i) delay_us(1000);
  if (--data.count > 0) {
    memcpy(&context, &data, sizeof context);
    new_timer_start(s_abusive_timer, 1, prv_waste_time_cb, context, 0);
  }
}

void command_waste_time(const char *count_arg, const char *delay_arg) {
  int count = atoi(count_arg);
  int delay = atoi(delay_arg);

  if (count <= 0 || count > 0xFFFF || delay <= 0 || delay > 0xFFFF) {
    prompt_send_response("Nope.");
    return;
  }

  struct WasteTimerData data = { count, delay };
  uintptr_t data_pack;
  memcpy(&data_pack, &data, sizeof data_pack);

  if (s_abusive_timer == TIMER_INVALID_ID) {
    s_abusive_timer = new_timer_create();
  }
  if (new_timer_start(s_abusive_timer, 100, prv_waste_time_cb,
                      (void *)data_pack, 0)) {
    prompt_send_response("OK");
  }  else {
    prompt_send_response("ERROR");
  }
}

#ifndef RELEASE
#include "system/profiler.h"
void command_audit_delay_us(void) {
  profiler_init();

  // don't let context switches skew our results
  __disable_irq();

  char buf[80];
  // test short delays because we should really be using psleep() for longer stalls!
  for (uint32_t i = 1; i <= 1000; i += 2) {
    profiler_start();
    delay_us(i);
    profiler_stop();
    uint32_t duration_us = profiler_get_total_duration(true);

    // make sure we have idled for at least the time specified and have not exceeded
    // the requested time by more than 5%
    bool passed = ((duration_us >= i) && (duration_us <= ((i * 105) / 100)));
    if (!passed) {
      prompt_send_response_fmt(buf, sizeof(buf), "Audit Failed: Expected %"PRIu32", Got %"PRIu32,
                               i, duration_us);
    }
  }
  prompt_send_response("delay_us audit complete");
  __enable_irq();
}

// Simply parks the chip permanently in stop mode in whatever state it's currently in. This can be
// pretty handy when trying to profile power of the chip under certain states
// NOTE: If you did not configure with `--nowatchdog`, the HW watchdog will reboot you in ~8s
void command_enter_stop(void) {
  dbgserial_putstr("Entering stop mode indefinitely ... reboot your board to get out!!");
  __disable_irq();
  RTC_ITConfig(RTC_IT_WUT, DISABLE);
  RTC_WakeUpCmd(DISABLE);

  // disable all IRQn_Type >= 0 interrupts
  for (size_t i = 0; i < ARRAY_LENGTH(NVIC->ISER); i++) {
    NVIC->ICER[i] = NVIC->ISER[i];
  }

  // disable SysTick
  SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;

  enter_stop_mode();

  dbgserial_putstr("woah, failed to enter stop mode");
  while (1) { }
}

#ifndef RECOVERY_FW
// Create a bunch of fragmentation in the filesystem by creating a large number
// of small files and only deleting a small number of them
void command_litter_filesystem(void) {
  char name[10];
  for (int i = 0; i < 100; i++) {
    snprintf(name, sizeof(name), "litter%d", i);
    int fd = pfs_open(name, OP_FLAG_WRITE, FILE_TYPE_STATIC, 300);
    if (i % 5 == 0) {
      pfs_close_and_remove(fd);
    } else {
      pfs_close(fd);
    }
  }
}
#endif
#endif

static GAPLEConnection *prv_get_le_connection_and_print_info(void) {
  GAPLEConnection *conn = gap_le_connection_any();
  if (!conn) {
    prompt_send_response("No device connected");
  } else {
    char buf[80];
    prompt_send_response_fmt(buf, sizeof(buf), "Connected to " BT_DEVICE_ADDRESS_FMT,
                             BT_DEVICE_ADDRESS_XPLODE(conn->device.address));
  }


  return conn;
}

void command_bt_conn_param_set(
    char *interval_min_1_25ms, char *interval_max_1_25ms, char *slave_latency_events,
    char *timeout_10ms) {

    BleConnectionParamsUpdateReq req = {
      .interval_min_1_25ms = atoi(interval_min_1_25ms),
      .interval_max_1_25ms = atoi(interval_max_1_25ms),
      .slave_latency_events = atoi(slave_latency_events),
      .supervision_timeout_10ms = atoi(timeout_10ms),
    };

    GAPLEConnection *conn = prv_get_le_connection_and_print_info();
    BTDeviceInternal addr = {};
    if (conn) {
      addr.address = conn->device.address;
    }

    bt_driver_le_connection_parameter_update(&addr, &req);
}
// Not in a header because it's really only used from within the gatt_service_changed module
extern void gatt_client_discovery_discover_range(
    GAPLEConnection *connection, ATTHandleRange *hdl_range);
void command_bt_disc_start(char *start_handle, char *end_handle) {
  bt_lock();
  {
    ATTHandleRange range = {
      .start = atoi(start_handle),
      .end = atoi(end_handle)
    };

    GAPLEConnection *conn = prv_get_le_connection_and_print_info();
    if (conn) {
      gatt_client_discovery_discover_range(conn, &range);
    }
  }
  bt_unlock();
}

void command_bt_disc_stop(void) {
  bt_lock();
  {
    GAPLEConnection *conn = prv_get_le_connection_and_print_info();
    if (conn) {
      bt_driver_gatt_stop_discovery(conn);
    }
  }
  bt_unlock();
}

#if BT_CONTROLLER_DA14681
static TimerID s_sleep_test_timer = TIMER_INVALID_ID;
static uint32_t s_num_sleep_iters = 0;
static void prv_sleep_timer_test_cb(void *data) {
  uint32_t count = (uint32_t)data;

  for (int j = 0; j < 10; j++) {
    bt_driver_send_sleep_test_cmd(true);
    psleep(9);
  }

  if (count >= s_num_sleep_iters) {
    bt_driver_send_sleep_test_cmd(false);
    prompt_send_response("100 % - PASS");
    new_timer_delete(s_sleep_test_timer);
    prompt_command_finish();
  } else {
    char buf[80];
    prompt_send_response_fmt(buf, sizeof(buf), "%d %%", (int)((count * 100) / s_num_sleep_iters));
    count++;
    new_timer_start(s_sleep_test_timer, 31, prv_sleep_timer_test_cb, (void *)(uintptr_t)count, 0);
  }
}

// Attempts to check that the Dialog chip sleep mode is working correctly by forcing wakeups at
// different times. (See PBL-39777). The timeouts delays may seem a little random but I found with
// this combo of values I was able to hit the issue on both failing units I have pretty quickly.
extern bool bt_test_chip_in_test_mode(void);
void command_bt_sleep_check(const char *iters) {
  s_num_sleep_iters = atoi(iters);

  // It seems like advertising while doing this makes the issue more likely to happen
  // (I suspect because there are even more enters/exits from sleep happening)
  if (bt_test_chip_in_test_mode()) {
    prompt_send_response("You must run \"bt test stop\" to conduct sleep test!");
    return;
  }

  prompt_send_response("Starting BT sleep check test");
  s_sleep_test_timer = new_timer_create();
  new_timer_start(s_sleep_test_timer , 10, prv_sleep_timer_test_cb, (void *)(uintptr_t)0, 0);

  prompt_command_continues_after_returning();
}

void command_btle_unmod_tx_start(char *tx_channel) {
  bt_driver_start_unmodulated_tx(atoi(tx_channel));
}

void command_btle_unmod_tx_stop(void) {
  bt_driver_stop_unmodulated_tx();
}

void command_btle_test_le_tx_start(
    char *tx_channel, char *tx_packet_length, char *packet_payload_type) {
  bt_driver_le_transmitter_test(
      atoi(tx_channel), atoi(tx_packet_length), atoi(packet_payload_type));
}

void command_btle_test_rx_start(char *rx_channel) {
  bt_driver_le_receiver_test(atoi(rx_channel));
}

void command_btle_test_end(void) {
  bt_driver_le_test_end();
}

//# if PLATFORM_ROBERT
static const char *s_btle_pa_config_strings[3] = { "disable", "enable", "bypass" };

void command_btle_pa_set(char *command) {
  char buffer[64];
  int index;

  for (index = 0; index < BtlePaConfigCount; ++index) {
    if (strcmp(command, s_btle_pa_config_strings[index]) == 0) {
      break;
    }
  }

  if (index >= BtlePaConfigCount) {
    prompt_send_response_fmt(buffer, sizeof(buffer), "BTLE PA options are: %s, %s, %s",
                             s_btle_pa_config_strings[BtlePaConfig_Disable],
                             s_btle_pa_config_strings[BtlePaConfig_Enable],
                             s_btle_pa_config_strings[BtlePaConfig_Bypass]);
  } else {
    bt_driver_le_test_pa(index);
    prompt_send_response_fmt(buffer, sizeof(buffer), "BTLE PA set to: %s",
                             s_btle_pa_config_strings[index]);
  }
}
//# endif
#endif

extern void hc_endpoint_logging_set_level(uint8_t level);
void command_ble_logging_set_level(const char *level) {
  char buffer[32];
  int log_level = atoi(level);
  if (log_level < 0) {
    log_level = 0;
  } else if (log_level > 255) {
    log_level = 255;
  }
  hc_endpoint_logging_set_level(log_level);
  prompt_send_response_fmt(buffer, 32, "Ble Log level set to: %i", log_level);
}

extern bool hc_endpoint_logging_get_level(uint8_t *level);
void command_ble_logging_get_level(void) {
  uint8_t log_level;
  char buffer[32];
  if (!hc_endpoint_logging_get_level(&log_level)) {
    prompt_send_response("Unable to get Ble Log level");
  } else {
    prompt_send_response_fmt(buffer, 32, "Ble Log level: %d", log_level);
  }
}
// ARG:
// 0 - Request BLE firmware to coredump
// 1 - Force BLE firmware to hard fault
// 2 - Force the BLE chip to watchdog (by wedging a task)
void command_ble_core_dump(const char *command) {
  int option = atoi(command);
  if ((option < 0) || (option >= BtleCoreDumpCount)) {
    prompt_send_response("Invalid BLE core command");
    return;
  }

  bt_driver_core_dump(option);
}

#if PERFORMANCE_TESTS
// for task_watchdog_bit_set_all
#include "drivers/task_watchdog.h"
// For taskYIELD()
#include "FreeRTOS.h"
#include "task.h"

// Average this many iterations of the text test for getting useful perf numbers.
#define PERFTEST_TEXT_ITERATIONS 5

static GContext s_perftest_ctx = {};

static GContext *prv_perftest_get_context(void) {
  GContext *ctx = &s_perftest_ctx;
  FrameBuffer *fb = compositor_get_framebuffer();
  memset(fb->buffer, 0xff, FRAMEBUFFER_SIZE_BYTES);
  graphics_context_init(ctx, fb, GContextInitializationMode_App);
  return ctx;
}

void watchdog_feed(void);
void command_perftest_line(const char *do_aa, const char *width) {
  watchdog_feed();

  GContext *ctx = prv_perftest_get_context();

  GColor color = { .argb = (uint8_t)0x33 };
  graphics_context_set_stroke_color(ctx, color);
  bool aa_enable = false;
  if (strcmp(do_aa, "aa") == 0) {
    aa_enable = true;
  } else if (strcmp(do_aa, "noaa") != 0) {
    prompt_send_response("Incorrect aa argument, must be 'aa' or 'noaa'.");
    return;
  }
  graphics_context_set_antialiased(ctx, aa_enable);
  uint8_t stroke_width = atoi(width);
  graphics_context_set_stroke_width(ctx, stroke_width);

  profiler_start();
  // 45 degrees
  graphics_draw_line(ctx, GPoint(0, 0), GPoint(DISP_COLS, DISP_ROWS));
  // ~63 degrees
  graphics_draw_line(ctx, GPoint(DISP_COLS/2, 0), GPoint(DISP_COLS, DISP_ROWS));
  // ~33 degrees
  graphics_draw_line(ctx, GPoint(0, DISP_ROWS/3), GPoint(DISP_COLS, DISP_ROWS));
  // ~53 degrees
  graphics_draw_line(ctx, GPoint(DISP_COLS/4, 0), GPoint(DISP_COLS, DISP_ROWS));
  // ~39 degrees
  graphics_draw_line(ctx, GPoint(0, DISP_ROWS/5), GPoint(DISP_COLS, DISP_ROWS));
  profiler_stop();

  uint32_t total_time = profiler_get_total_duration(false);
  uint32_t us = profiler_get_total_duration(true);
  char buf[80];
  prompt_send_response_fmt(buf, sizeof(buf),
                           "%s, %s, %"PRIu32", %"PRIu32,
                           do_aa, width, us, total_time);
}

void command_perftest_line_all(void) {
  prompt_send_response("Antialiasing?, Width, Total time (us), Total cycles");
  command_perftest_line("noaa", "8");
  command_perftest_line("noaa", "6");
  command_perftest_line("noaa", "5");
  command_perftest_line("noaa", "4");
  command_perftest_line("noaa", "3");
  command_perftest_line("noaa", "2");
  command_perftest_line("noaa", "1");
  command_perftest_line("aa", "8");
  command_perftest_line("aa", "6");
  command_perftest_line("aa", "5");
  command_perftest_line("aa", "4");
  command_perftest_line("aa", "3");
  command_perftest_line("aa", "2");
  command_perftest_line("aa", "1");
}

typedef struct PerftestTextArguments {
  const char *string_type;
  const char *font_key;
  const char *y_offset;
} PerftestTextArguments;

static volatile PerftestTextArguments s_perftest_text_arguments;

enum {
  TestString_Best, // The best case
  TestString_Worst, // Entirely unique characters, in order to miss the font cache every time
  TestString_Typical, // A very typical notification
  TestStringCount,
};

enum {
  TestStringFont_Gothic18,
  TestStringFont_Gothic24B,
  TestStringFont_Other,
  TestStringFontCount,
};

// A very big number
#define STRING_LENGTH_MAX 99999

typedef struct PerftestTextString {
  const char *string;
  size_t lengths[TestStringFontCount];
} PerftestTextString;

static const PerftestTextString s_perftest_text_strings[TestStringCount] = {
  [TestString_Best] = {
    .string = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
              "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
              "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
              "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
              "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
              "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"
              "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM",
    .lengths = {
#if PLATFORM_ROBERT
      [TestStringFont_Gothic18] = 204,
      [TestStringFont_Gothic24B] = 144,
      [TestStringFont_Other] = STRING_LENGTH_MAX,
#elif PLATFORM_SNOWY
      [TestStringFont_Gothic18] = 109,
      [TestStringFont_Gothic24B] = 78,
      [TestStringFont_Other] = STRING_LENGTH_MAX,
#endif
    },
  },
  [TestString_Worst] = {
    .string = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./~!@#$%%^&*()_+QWERTYUIOP{}|A"
              "SDFGHJKL:\"ZXCVBNM<>?èéêëēėęÿûüùúūîïíī"
              "įìôöòóœøōõàáâäæãåāßśšłžźżçćčñń∑´®†¥¨ˆπ"
              "∂ƒ©˙∆˚¬…Ω≈√∫˜µ≤≥÷¡™£¢∞§¶•ªº–≠`“‘"
              "«ÈÉÊËĒĖĘŸÛÜÙÚŪÎÏÍĪĮÌÔÖÒÓŒØŌÕÀÁÂÄÆÃÅĀŚ"
              "ŠŁŽŹŻÇĆČÑŃ∑ˇ∏”’»˝¸˛◊ı˜¯˘¿"
              "あいうえおかきくけこさしすせそたちつてとなに"
              "ぬねのはひふへほまみむめもやゆよらりるれろわ"
              "をんアイウエオサシスセソタチツテトナニヌネノ"
              "ハヒフヘホマミムメモヤユヨラリルレロワヲン",
    .lengths = {
#if PLATFORM_ROBERT
      [TestStringFont_Gothic18] = 579,
      [TestStringFont_Gothic24B] = 291,
      [TestStringFont_Other] = STRING_LENGTH_MAX,
#elif PLATFORM_SNOWY
      [TestStringFont_Gothic18] = 256,
      [TestStringFont_Gothic24B] = 113,
      [TestStringFont_Other] = STRING_LENGTH_MAX,
#endif
    },
  },
  [TestString_Typical] = {
    .string = "Brian Gomberg\n"
              "Re: Robert stand-up 06/06 • "
              "y: - DDAD (enabling system apps to take advantage of memory mapped "
              "FLASH access on Robe"
              "\xe2\x80\xa6",
    .lengths = {
#if PLATFORM_ROBERT
      [TestStringFont_Gothic18] = 134,
      [TestStringFont_Gothic24B] = 134,
      [TestStringFont_Other] = STRING_LENGTH_MAX,
#elif PLATFORM_SNOWY
      [TestStringFont_Gothic18] = 134,
      [TestStringFont_Gothic24B] = 112,
      [TestStringFont_Other] = STRING_LENGTH_MAX,
#endif
    },
  },
};

#define TEXT_ALIGNMENT (GTextAlignmentCenter)
#define TEXT_OVERFLOW  (GTextOverflowModeWordWrap)

// Enable this to show the actual contents, to check lengths and such.
#define TEXT_PERFTEST_MODAL 0

#if TEXT_PERFTEST_MODAL
#include "applib/ui/dialogs/simple_dialog.h"
#include "applib/ui/dialogs/dialog_private.h"
#include "kernel/ui/modals/modal_manager.h"

static void prv_dialog_appear(Window *window) {
  Dialog *dialog = window_get_user_data(window);
  dialog_appear(dialog);
}

static void prv_dialog_unload(Window *window) {
  Dialog *dialog = window_get_user_data(window);
  dialog_unload(dialog);
}

static void prv_dialog_load(Window *window) {
  Dialog *dialog = window_get_user_data(window);

  GFont font = fonts_get_system_font(s_perftest_text_arguments.font_key);

  TextLayer *text_layer = &dialog->text_layer;
  text_layer_init_with_parameters(text_layer, &GRect(0, 0, DISP_COLS, DISP_ROWS), dialog->buffer,
                                  font, GColorBlack, GColorClear, TEXT_ALIGNMENT,
                                  TEXT_OVERFLOW);
  layer_add_child(&window->layer, &text_layer->layer);

#if PBL_ROUND
  text_layer_enable_screen_text_flow_and_paging(text_layer, TEXT_FLOW_INSET_PX);
#endif

  dialog_load(dialog);
}

static Dialog s_test_dialog;
static void prv_display_modal(WindowStack *stack, const char *string) {
  Dialog *new_dialog = &s_test_dialog;
  dialog_init(new_dialog, "");
  dialog_set_text(new_dialog, string);

  Window *window = &new_dialog->window;
  window_set_window_handlers(window, &(WindowHandlers) {
    .load = prv_dialog_load,
    .unload = prv_dialog_unload,
    .appear = prv_dialog_appear,
  });
  window_set_user_data(window, new_dialog);
  dialog_push(new_dialog, stack);
}
#endif

static char s_text_test_str[1024];

static void prv_perftest_test_main(void *data) {
  profiler_init();
  GFont font = fonts_get_system_font(s_perftest_text_arguments.font_key);

  int text_index;
  int font_index;
  if (strcmp(s_perftest_text_arguments.string_type, "best") == 0) {
    text_index = TestString_Best;
  } else if (strcmp(s_perftest_text_arguments.string_type, "worst") == 0) {
    text_index = TestString_Worst;
  } else if (strcmp(s_perftest_text_arguments.string_type, "typical") == 0) {
    text_index = TestString_Typical;
  } else {
    prompt_send_response("Incorrect type argument, must be 'best', 'typical', or 'worst'.");
    return;
  }
  if (strcmp(s_perftest_text_arguments.font_key, "RESOURCE_ID_GOTHIC_18") == 0) {
    font_index = TestStringFont_Gothic18;
  } else if (strcmp(s_perftest_text_arguments.font_key, "RESOURCE_ID_GOTHIC_24_BOLD") == 0) {
    font_index = TestStringFont_Gothic24B;
  } else {
    font_index = TestStringFont_Other;
  }

#if TEXT_PERFTEST_MODAL
  // Length replaces the yoffset argument
  size_t length = atoi(s_perftest_text_arguments.y_offset);
  if (length == 0) {
    length = STRING_LENGTH_MAX;
  }
#else
  size_t length = s_perftest_text_strings[text_index].lengths[font_index];
#endif

  length = MIN(length, sizeof(s_text_test_str));
  strncpy(s_text_test_str, s_perftest_text_strings[text_index].string,
          length);
  s_text_test_str[length] = '\0';

#if TEXT_PERFTEST_MODAL
  prv_display_modal(modal_manager_get_window_stack(ModalPriorityAlert), s_text_test_str);
  s_perftest_text_arguments.string_type = NULL;
  return;
#endif

  GRect bounds = GRect(0, 0, DISP_COLS, DISP_ROWS);

  int y_offset = atoi(s_perftest_text_arguments.y_offset);
  bounds.origin.y -= y_offset;
  if (y_offset > 0) {
    bounds.size.h = DISP_ROWS + y_offset;
  }

  uint32_t avg = 0;

  for (int i = 0; i < PERFTEST_TEXT_ITERATIONS; i++) {
    // Sometimes this loop takes long enough that we end up watchdogging
    watchdog_feed();
    task_watchdog_bit_set_all();

    GContext *ctx = prv_perftest_get_context();
    graphics_context_set_text_color(ctx, GColorBlack);

    profiler_start();
    graphics_draw_text(ctx, s_text_test_str, font, bounds,
                       TEXT_OVERFLOW, TEXT_ALIGNMENT, NULL);
    profiler_stop();
    avg += profiler_get_total_duration(true);
  }

  avg /= PERFTEST_TEXT_ITERATIONS;
  char buf[80];
  uint32_t flash_us_avg = PROFILER_NODE_GET_TOTAL_US(text_render_flash) / PERFTEST_TEXT_ITERATIONS;
  prompt_send_response_fmt(buf, sizeof(buf), "%s, %s, %s, %"PRIu32", %"PRIu32,
                           s_perftest_text_arguments.font_key,
                           s_perftest_text_arguments.string_type,
                           s_perftest_text_arguments.y_offset,
                           avg, flash_us_avg);

  s_perftest_text_arguments.string_type = NULL;
}

void command_perftest_text(const char *string_type, const char *fontkey, const char *yoffset) {
  s_perftest_text_arguments.string_type = string_type;
  s_perftest_text_arguments.font_key = fontkey;
  s_perftest_text_arguments.y_offset = yoffset;
  launcher_task_add_callback(prv_perftest_test_main, NULL);
  while (s_perftest_text_arguments.string_type != NULL) {
    taskYIELD();
    watchdog_feed();
    task_watchdog_bit_set_all();
  }
}

void command_perftest_text_all(void) {
  static const char *fonts[] = {
    "RESOURCE_ID_GOTHIC_28",
    "RESOURCE_ID_GOTHIC_24",
    "RESOURCE_ID_GOTHIC_18",
    "RESOURCE_ID_GOTHIC_28_BOLD",
    "RESOURCE_ID_GOTHIC_24_BOLD",
    "RESOURCE_ID_GOTHIC_18_BOLD",
  };
  static const char *types[] = {
    "best", "worst", "typical",
  };
  static const char *offsets[] = {
    "0", "2000",
  };
  prompt_send_response("Font, Type, Offset, Total avg us, Flash avg us");
  for (unsigned int type = 0; type < ARRAY_LENGTH(types); type++) {
    for (unsigned int font = 0; font < ARRAY_LENGTH(fonts); font++) {
      for (unsigned int offset = 0; offset < ARRAY_LENGTH(offsets); offset++) {
        command_perftest_text(types[type], fonts[font], offsets[offset]);
      }
    }
  }
}
#endif
