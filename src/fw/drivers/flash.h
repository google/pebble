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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "system/status_codes.h"

static const uint32_t EXPECTED_SPI_FLASH_ID_32MBIT = 0x20bb16;
static const uint32_t EXPECTED_SPI_FLASH_ID_64MBIT = 0x20bb17;

/**
 * Configure the micro's peripherals to communicate with the flash
 * chip.
 */
void flash_init(void);

//! Stop all flash transactions.
void flash_stop(void);

/**
 * Retrieve the first 3 bytes of the flash's device id. This ID
 * should remain fixed across all chips.
 */
uint32_t flash_whoami(void);

/**
 * Read 1 or more bytes starting at the specified 24bit address into
 * the provided buffer. This function does no range checking, so it is
 * currently possible to run off the end of the flash.
 *
 * @param buffer A byte-buffer that will be used to store the data
 * read from flash.
 * @param start_addr The address of the first byte to be read from flash.
 * @param buffer_size The total number of bytes to be read from flash.
 */
void flash_read_bytes(uint8_t* buffer, uint32_t start_addr, uint32_t buffer_size);

/**
 * Write 1 or more bytes from the buffer to flash starting at the
 * specified 24bit address. This function will handle both writing a
 * buffer that is larger than the flash's page size and writing to a
 * non-page aligned address.
 *
 * @param buffer A byte-buffer containing the data to be written to flash.
 * @param start_addr The address of the first byte to be written to flash.
 * @param buffer_size The total number of bytes to be written.
 */
void flash_write_bytes(const uint8_t* buffer, uint32_t start_addr, uint32_t buffer_size);

typedef void (*FlashOperationCompleteCb)(void *context, status_t result);

/**
 * Erase a subsector asynchronously.
 *
 * The callback function will be called when the erase completes, whether the
 * erase succeeded or failed. The callback will be executed on an arbitrary
 * (possibly high-priority) task, so the callback function must return quickly.
 * The callback may also be called directly from within flash_erase_subsector.
 */
void flash_erase_subsector(uint32_t subsector_addr,
                           FlashOperationCompleteCb on_complete,
                           void *context);

/**
 * Erase a sector asynchronously.
 *
 * The callback function will be called when the erase completes, whether the
 * erase succeeded or failed. The callback will be executed on an arbitrary
 * (possibly high-priority) task, so the callback function must return quickly.
 * The callback may also be called directly from within flash_erase_sector.
 */
void flash_erase_sector(uint32_t sector_addr,
                        FlashOperationCompleteCb on_complete,
                        void *context);

/**
 * Erase the subsector containing the specified address.
 */
void flash_erase_subsector_blocking(uint32_t subsector_addr);

/**
 * Erase the sector containing the specified address.
 *
 * Beware: this function takes 100ms+ to execute, so be careful when you call it.
 */
void flash_erase_sector_blocking(uint32_t sector_addr);

/**
 * Check whether the sector containing the specified address is already erased.
 */
bool flash_sector_is_erased(uint32_t sector_addr);

/**
 * Check whether the subsector containing the specified address is already erased.
 */
bool flash_subsector_is_erased(uint32_t sector_addr);

/**
 * Erase the entire contents of flash.
 *
 * Note: This is a very slow (up to a minute) blocking operation. Don't let the watchdog kill
 * you when calling this.
 */
void flash_erase_bulk(void);

/**
 * Erase a region of flash asynchronously using as few erase operations as
 * possible.
 *
 * At least (max_start, min_end) but no more than (min_start, max_end) will be
 * erased. Both min_start and max_end must be aligned to a subsector address as
 * that is the smallest unit that can be erased.
 */
void flash_erase_optimal_range(
    uint32_t min_start, uint32_t max_start, uint32_t min_end, uint32_t max_end,
    FlashOperationCompleteCb on_complete, void *context);

/**
 * Configure the flash driver to enter a deep sleep mode between commands.
 */
void flash_sleep_when_idle(bool enable);

//! @return True if sleeping when idle is currently enabled.
bool flash_get_sleep_when_idle(void);

void debug_flash_dump_registers(void);

//! @return true if the flash peripheral has been initialized.
bool flash_is_initialized(void);

//! Helper function to check that the Flash ID (whoami) is correct
//! @return true if the flash ID matches what we expect based on the board config
bool flash_is_whoami_correct(void);

//! Helper function to extract the Flash Size from the ID (whoami)
//! @return the size of the flash in bytes
size_t flash_get_size(void);

// This is only intended to be called when entering stop mode. It does not use
// any locks because IRQs have already been disabled. The idea is to only incur
// the wait penalty for entering/exiting deep sleep mode for the flash
// before/after stop mode. The flash part consumes ~100uA in standby mode and
// ~10uA when its in deep sleep mode. If the MCU is not in stop mode, this
// difference is negligible
void flash_power_down_for_stop_mode(void);
void flash_power_up_after_stop_mode(void);

typedef enum {
  FLASH_MODE_ASYNC = 0,
  FLASH_MODE_SYNC_BURST,
  
  // Add new modes above this
  FLASH_MODE_NUM_MODES
}FlashModeType;

/**
 * Manually switches modes between asynchronous/synchronous
 * 
 */
void flash_switch_mode(FlashModeType mode);

// Returns the sector address that the given flash address lies in
uint32_t flash_get_sector_base_address(uint32_t flash_addr);

// Returns the subsector address that the given flash address lies in
uint32_t flash_get_subsector_base_address(uint32_t flash_addr);

// Enable write protection on flash
void flash_enable_write_protection(void);

// Write-protects the prf region of flash
void flash_prf_set_protection(bool do_protect);

//! Compute a CRC32 checksum of a region of flash.
uint32_t flash_crc32(uint32_t flash_addr, uint32_t length);

//! Apply the legacy defective checksum to a region of flash.
uint32_t flash_calculate_legacy_defective_checksum(uint32_t flash_addr,
                                                   uint32_t length);

//! Call this before any external flash access (including memory-mapped)
//! to power on the flash peripheral if it wasn't already, and
//! to increase the internal reference counter that prevents flash peripheral from powering down.
void flash_use(void);

//! Convenience for \ref flash_release_many(1)
void flash_release(void);

//! Call this after you finished accessing external flash
//! to decrease the internal reference counter by num_locks, and
//! to turn off the flash peripheral if the reference counter reaches 0
//! param num_locks usually 1, the amount by which the reference counter should be decremented
void flash_release_many(uint32_t num_locks);
