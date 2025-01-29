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

#include "drivers/crc.h"
#include "drivers/dbgserial.h"
#include "drivers/display.h"
#include "drivers/flash.h"
#include "drivers/system_flash.h"
#include "firmware.h"
#include "flash_region.h"
#include "system/bootbits.h"
#include "system/firmware_storage.h"
#include "system/reset.h"
#include "util/misc.h"
#include "util/delay.h"

#include <inttypes.h>
#include <stdint.h>

#define MAX_CHUNK_SIZE 65536

static bool prv_check_valid_firmware_crc(uint32_t flash_address, FirmwareDescription* desc) {
  dbgserial_putstr("Checksumming firmware update");
  uint32_t crc = crc_calculate_flash(flash_address, desc->firmware_length);
  return crc == desc->checksum;
}

// Fills in the first 50% of the progress bar
static void prv_display_erase_progress(
    uint32_t progress, uint32_t total, void *ctx) {
  display_firmware_update_progress(progress, total * 2);
}

//! Returns true if we're going to install a new world firmware
static bool prv_check_firmware_world(uint32_t flash_new_fw_addr) {
  // Read the beginning of the firmware off flash to see if it's new world or old world
  const unsigned world_length = FW_IDENTIFIER_OFFSET + sizeof(uint32_t); // NeWo read as uint32_t

  uint8_t buffer[world_length];
  flash_read_bytes(buffer, flash_new_fw_addr, world_length);

  return firmware_is_new_world(buffer);
}

static bool prv_erase_old_firmware(bool new_world, uint32_t firmware_length) {
  dbgserial_putstr("erase_old_firmware");
  uint32_t system_flash_base = FIRMWARE_NEWWORLD_BASE;
  uint32_t erase_length = firmware_length;

  if (!new_world) {
    // If the incoming firmware is an old world firmware, we need to not only erase
    // enough room for the old world firmware but we also need to erase the area
    // between the new world and old world base addresses. If we don't erase this
    // area and just load in the old world firmware at FIRMWARE_OLDWORLD_BASE, the
    // bootloader will still see the start of the old new world firmware in this
    // section and will attempt to boot that.
    dbgserial_putstr("Old World firmware base");
    erase_length += FW_WORLD_DIFFERENCE;
  }

  return system_flash_erase(system_flash_base, erase_length, prv_display_erase_progress, 0);
}

// Fills in the last 50% of the progress bar
static void prv_display_write_progress(
    uint32_t progress, uint32_t total, void *ctx) {
  display_firmware_update_progress(progress/2 + total/2, total);
}

static bool prv_write_new_firmware(bool new_world, uint32_t flash_new_fw_start,
                                   uint32_t firmware_length) {
  dbgserial_putstr("write_new_firmware");
  uint32_t system_flash_base = new_world ? FIRMWARE_NEWWORLD_BASE : FIRMWARE_OLDWORLD_BASE;
  // We can't just read the flash like memory, so we gotta lift everything ourselves.
  // buffer is static so it goes in BSS, since stack is only 8192 bytes
  static uint8_t buffer[MAX_CHUNK_SIZE];
  uint32_t chunk_size;
  for (uint32_t i = 0; i < firmware_length; i += chunk_size) {
    chunk_size = MIN(MAX_CHUNK_SIZE, firmware_length - i);
    flash_read_bytes(buffer, flash_new_fw_start + i, chunk_size);
    if (!system_flash_write(system_flash_base + i, buffer, chunk_size, NULL, NULL)) {
      dbgserial_putstr("We're dead");
      return false;
    }
    prv_display_write_progress(i, firmware_length, NULL);
  }
  return true;
}

static bool prv_check_firmware_crc(FirmwareDescription* firmware_description) {
  dbgserial_print("Checksumming ");
  dbgserial_print_hex(firmware_description->firmware_length);
  dbgserial_putstr(" bytes");

  void *system_flash_base = (void*)FIRMWARE_OLDWORLD_BASE;

  if (firmware_is_new_world(NULL)) {
    dbgserial_putstr("New World firmware system_flash_base");
    system_flash_base = (void*)FIRMWARE_NEWWORLD_BASE;
  } else {
    dbgserial_putstr("Old World firmware system_flash_base");
  }
  uint32_t calculated_crc = crc_calculate_bytes((const uint8_t*)system_flash_base,
                                                firmware_description->firmware_length);

  dbgserial_print("Checksum - wanted ");
  dbgserial_print_hex(firmware_description->checksum);
  dbgserial_print(" got ");
  dbgserial_print_hex(calculated_crc);
  dbgserial_newline();

  return calculated_crc == firmware_description->checksum;
}

typedef enum UpdateFirmwareResult {
  UPDATE_FW_SUCCESS = 0,
  UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED = 1,
  UPDATE_FW_ERROR_MICRO_FLASH_MANGLED = 2
} UpdateFirmwareResult;

static UpdateFirmwareResult prv_update_fw(uint32_t flash_new_fw_addr) {
  display_firmware_update_progress(0, 1);
  boot_bit_set(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);

  FirmwareDescription firmware_description =
      firmware_storage_read_firmware_description(flash_new_fw_addr);

  if (!firmware_storage_check_valid_firmware_description(&firmware_description)) {
    dbgserial_print("Desclen ");
    dbgserial_print_hex(firmware_description.description_length);
    dbgserial_print("\nFirmlen ");
    dbgserial_print_hex(firmware_description.firmware_length);
    dbgserial_print("\nXsum ");
    dbgserial_print_hex(firmware_description.checksum);
    dbgserial_putstr("\nInvalid firmware description!");
    return UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED;
  }

  if (!prv_check_valid_firmware_crc(
        flash_new_fw_addr + sizeof(FirmwareDescription), &firmware_description)) {
    dbgserial_putstr("Invalid firmware CRC in SPI flash!");
    return UPDATE_FW_ERROR_MICRO_FLASH_UNTOUCHED;
  }

  bool new_world = prv_check_firmware_world(flash_new_fw_addr + sizeof(FirmwareDescription));

  prv_erase_old_firmware(new_world, firmware_description.firmware_length);

  prv_write_new_firmware(new_world, flash_new_fw_addr + sizeof(FirmwareDescription),
                         firmware_description.firmware_length);

  if (!prv_check_firmware_crc(&firmware_description)) {
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

  UpdateFirmwareResult result = prv_update_fw(FLASH_REGION_FIRMWARE_SCRATCH_BEGIN);
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
  }

  // Done, we're ready to boot.
  boot_bit_clear(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);
  boot_bit_clear(BOOT_BIT_NEW_FW_AVAILABLE);
  boot_bit_set(BOOT_BIT_NEW_FW_INSTALLED);
}

bool switch_to_recovery_fw() {
  dbgserial_putstr("Loading recovery firmware");

  UpdateFirmwareResult result = prv_update_fw(FLASH_REGION_SAFE_FIRMWARE_BEGIN);
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
