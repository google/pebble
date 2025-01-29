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

#include "fw_copy.h"

#include "drivers/dbgserial.h"
#include "drivers/display.h"
#include "drivers/flash.h"
#include "drivers/system_flash.h"
#include "firmware.h"
#include "flash_region.h"
#include "system/bootbits.h"
#include "system/firmware_storage.h"
#include "system/reset.h"
#include "util/crc32.h"
#include "util/misc.h"
#include "util/delay.h"

#include <inttypes.h>
#include <stdint.h>

#define MAX_CHUNK_SIZE 65536

static bool check_valid_firmware_crc(uint32_t flash_address, FirmwareDescription* desc) {
  dbgserial_putstr("Checksumming firmware update");
  const uint32_t crc = flash_calculate_checksum(flash_address, desc->firmware_length);
  dbgserial_print("Calculated checksum: ");
  dbgserial_print_hex(crc);
  dbgserial_newline();
  return crc == desc->checksum;
}

static void prv_display_erase_progress(
    uint32_t progress, uint32_t total, void *ctx) {
  display_firmware_update_progress(progress, total * 2);
}

static bool erase_old_firmware(uint32_t firmware_length) {
  dbgserial_putstr("erase_old_firmware");
  return system_flash_erase(
        FIRMWARE_BASE, firmware_length, prv_display_erase_progress, 0);
}

static void prv_display_write_progress(
    uint32_t progress, uint32_t total, void *ctx) {
  display_firmware_update_progress(progress/2 + total/2, total);
}

static bool write_new_firmware(uint32_t flash_new_fw_start,
                               uint32_t firmware_length) {
  dbgserial_putstr("write_new_firmware");
  // We can't just read the flash like memory, so we gotta lift everything ourselves.
  // buffer is static so it goes in BSS, since stack is only 8192 bytes
  static uint8_t buffer[MAX_CHUNK_SIZE];
  uint32_t chunk_size;
  for (uint32_t i = 0; i < firmware_length; i += chunk_size) {
    chunk_size = MIN(MAX_CHUNK_SIZE, firmware_length - i);
    flash_read_bytes(buffer, flash_new_fw_start + i, chunk_size);
    if (!system_flash_write(FIRMWARE_BASE + i, buffer, chunk_size, NULL, NULL)) {
      dbgserial_putstr("We're dead");
      return false;
    }
    prv_display_write_progress(i + chunk_size, firmware_length, NULL);
  }
  return true;
}

static bool check_firmware_crc(FirmwareDescription* firmware_description) {
  dbgserial_print("Checksumming ");
  dbgserial_print_hex(firmware_description->firmware_length);
  dbgserial_print(" bytes\r\n");

  const uint32_t crc = crc32(CRC32_INIT, (const uint8_t *)FIRMWARE_BASE,
                             firmware_description->firmware_length);

  dbgserial_print("Checksum - wanted ");
  dbgserial_print_hex(firmware_description->checksum);
  dbgserial_print(" got ");
  dbgserial_print_hex(crc);
  dbgserial_newline();

  return crc == firmware_description->checksum;
}

typedef enum UpdateFirmwareResult {
  UPDATE_FW_SUCCESS = 0,
  UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED = 1,
  UPDATE_FW_ERROR_MICRO_FLASH_MANGLED = 2
} UpdateFirmwareResult;

static UpdateFirmwareResult update_fw(uint32_t flash_address) {
  display_firmware_update_progress(0, 1);
  boot_bit_set(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);

  FirmwareDescription firmware_description =
      firmware_storage_read_firmware_description(flash_address);

  if (!firmware_storage_check_valid_firmware_description(&firmware_description)) {
    dbgserial_putstr("Invalid firmware description!");
    return UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED;
  }

  if (!check_valid_firmware_crc(
        flash_address + sizeof(FirmwareDescription), &firmware_description)) {
    dbgserial_putstr("Invalid firmware CRC in SPI flash!");
    return UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED;
  }

  erase_old_firmware(firmware_description.firmware_length);

  write_new_firmware(
      flash_address + sizeof(FirmwareDescription),
      firmware_description.firmware_length);

  if (!check_firmware_crc(&firmware_description)) {
    dbgserial_putstr(
        "Our internal flash contents are bad (checksum failed)! "
        "This is really bad!");
    return UPDATE_FW_ERROR_MICRO_FLASH_MANGLED;
  }

  return UPDATE_FW_SUCCESS;
}

void check_update_fw(void) {
  if (!boot_bit_test(BOOT_BIT_NEW_FW_AVAILABLE)) {
    return;
  }

  if (boot_bit_test(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS)) {
    dbgserial_putstr("Our previous firmware update failed, aborting update.");

    // Pretend like the new firmware bit wasn't set after all. We'll just run the
    // previous code, whether that was normal firmware or the recovery firmware.
    boot_bit_clear(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);
    boot_bit_clear(BOOT_BIT_NEW_FW_AVAILABLE);
    boot_bit_clear(BOOT_BIT_NEW_FW_INSTALLED);
    return;
  }


  dbgserial_putstr("New firmware is available!");

  boot_bit_clear(BOOT_BIT_FW_START_FAIL_STRIKE_ONE);
  boot_bit_clear(BOOT_BIT_FW_START_FAIL_STRIKE_TWO);
  boot_bit_clear(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE);
  boot_bit_clear(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO);

  UpdateFirmwareResult result = update_fw(FLASH_REGION_FIRMWARE_SCRATCH_BEGIN);
  switch (result) {
  case UPDATE_FW_SUCCESS:
    break;
  case UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED:
    // Our firmware update failed in a way that didn't break our previous
    // firmware. Just run the previous code, whether that was normal firmware
    // or the recovery firmware.
    break;
  case UPDATE_FW_ERROR_MICRO_FLASH_MANGLED:
    // We've broken our internal flash when trying to update our normal
    // firmware. Fall back immediately to the recovery firmware.
    boot_bit_set(BOOT_BIT_FW_START_FAIL_STRIKE_ONE);
    boot_bit_set(BOOT_BIT_FW_START_FAIL_STRIKE_TWO);
    system_reset();
    return;
  }

  // Done, we're ready to boot.
  boot_bit_clear(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);
  boot_bit_clear(BOOT_BIT_NEW_FW_AVAILABLE);
  boot_bit_set(BOOT_BIT_NEW_FW_INSTALLED);
}

bool switch_to_recovery_fw() {
  dbgserial_putstr("Loading recovery firmware");

  UpdateFirmwareResult result = update_fw(FLASH_REGION_SAFE_FIRMWARE_BEGIN);
  bool recovery_fw_ok = true;
  switch (result) {
  case UPDATE_FW_SUCCESS:
    boot_bit_clear(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE);
    boot_bit_clear(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO);
    boot_bit_set(BOOT_BIT_RECOVERY_START_IN_PROGRESS);
    break;
  case UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED:
  case UPDATE_FW_ERROR_MICRO_FLASH_MANGLED:
    // Keep us booting into recovery firmware.
    boot_bit_set(BOOT_BIT_FW_START_FAIL_STRIKE_ONE);
    boot_bit_set(BOOT_BIT_FW_START_FAIL_STRIKE_TWO);

    if (!boot_bit_test(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE)) {
      dbgserial_putstr("Failed to load recovery firmware, strike one. Try again.");
      boot_bit_set(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE);
      boot_bit_set(BOOT_BIT_SOFTWARE_FAILURE_OCCURRED);
      system_reset();
    } else if (!boot_bit_test(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO)) {
      dbgserial_putstr("Failed to load recovery firmware, strike two. Try again.");
      boot_bit_set(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO);
      boot_bit_set(BOOT_BIT_SOFTWARE_FAILURE_OCCURRED);
      system_reset();
    } else {
      dbgserial_putstr("Failed to load recovery firmware, strike three. SAD WATCH");
      boot_bit_clear(BOOT_BIT_FW_START_FAIL_STRIKE_ONE);
      boot_bit_clear(BOOT_BIT_FW_START_FAIL_STRIKE_TWO);
      boot_bit_clear(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE);
      boot_bit_clear(BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO);
      recovery_fw_ok = false;
    }
    break;
  }

  boot_bit_clear(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);
  return recovery_fw_ok;
}
