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

#include "drivers/flash/flash_impl.h"

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "flash_region/flash_region.h"
#include "system/passert.h"
#include "system/logging.h"
#include "kernel/util/delay.h"
#include "util/math.h"
#include "util/size.h"
#include "util/units.h"

#define STM32F4_COMPATIBLE
#include <mcu.h>

//! This is the memory mapped region that's mapped to the parallel flash.
static const uintptr_t FMC_BANK_1_BASE_ADDRESS = 0x60000000;

//! This is the unit that we use for writing
static const uint32_t PAGE_SIZE_BYTES = 64;


//! Different commands we can send to the flash
typedef enum S29VSCommand {
  S29VSCommand_WriteBufferLoad = 0x25,
  S29VSCommand_BufferToFlash = 0x29,
  S29VSCommand_EraseResume = 0x30,
  S29VSCommand_SectorBlank = 0x33,
  S29VSCommand_SectorLock = 0x60,
  S29VSCommand_SectorLockRangeArg = 0x61,
  S29VSCommand_ReadStatusRegister = 0x70,
  S29VSCommand_ClearStatusRegister = 0x71,
  S29VSCommand_EraseSetup = 0x80,
  S29VSCommand_DeviceIDEntry = 0x90,
  S29VSCommand_EraseSuspend = 0xB0,
  S29VSCommand_ConfigureRegisterEntry = 0xD0,
  S29VSCommand_SoftwareReset = 0xF0
} S29VSCommand;

//! Arguments to the S29VSCommand_EraseSetup command
typedef enum S29VSCommandEraseArguments {
  S29VSCommandEraseArguments_ChipErase = 0x10,
  S29VSCommandEraseArguments_SectorErase = 0x30
} S29VSCommandEraseArguments;

//! The bitset stored in the status register, see prv_read_status_register
typedef enum S29VSStatusBit {
  S29VSStatusBit_BankStatus = (1 << 0),
  S29VSStatusBit_SectorLockStatus = (1 << 1),
  S29VSStatusBit_ProgramSuspended = (1 << 2),
  // Bit 3 is reserved
  S29VSStatusBit_ProgramStatus = (1 << 4),
  S29VSStatusBit_EraseStatus = (1 << 5),
  S29VSStatusBit_EraseSuspended = (1 << 6),
  S29VSStatusBit_DeviceReady = (1 << 7),
} S29VSStatusBit;
static const uint16_t SPANSION_MANUFACTURER_ID = 0x01;
static const uint16_t MACRONIX_MANUFACTURER_ID = 0xc2;
static const GPIO_InitTypeDef s_default_at_flash_cfg = {
  .GPIO_Mode = GPIO_Mode_AF,
  .GPIO_Speed = GPIO_Speed_100MHz,
  .GPIO_OType = GPIO_OType_PP,
  .GPIO_PuPd  = GPIO_PuPd_NOPULL
};

static void prv_issue_command_argument(FlashAddress sector_address, uint16_t cmd_arg);
static void prv_issue_command(FlashAddress sector_address, S29VSCommand cmd);

// puts gpios into or out of analog to save power when idle/in use respectively
static void prv_flash_idle_gpios(bool enable_gpios) {
  static bool gpios_idled = false;
  if (gpios_idled == enable_gpios) {
    return;
  }
  gpios_idled = enable_gpios;

  gpio_use(GPIOB);
  gpio_use(GPIOD);
  gpio_use(GPIOE);

  GPIO_InitTypeDef gpio_init;
  if (enable_gpios) {
    gpio_init = s_default_at_flash_cfg;
  } else {
    gpio_init = (GPIO_InitTypeDef) {
      .GPIO_Mode = GPIO_Mode_AN,
      .GPIO_Speed = GPIO_Speed_2MHz,
      .GPIO_PuPd  = GPIO_PuPd_NOPULL
    };
  }

  // leave RESET_N and CE: they need to retain their state
  // Configure the rest as analog inputs to save as much power as possible
  // D2 - Reset - GPIO Reset line
  // D7 - FMC CE - FMC Chip Enable
  gpio_init.GPIO_Pin = GPIO_Pin_7;
  GPIO_Init(GPIOB, &gpio_init);

  gpio_init.GPIO_Pin = GPIO_Pin_All & (~GPIO_Pin_2) & (~GPIO_Pin_7);
  GPIO_Init(GPIOD, &gpio_init);

  gpio_init.GPIO_Pin = GPIO_Pin_All & (~GPIO_Pin_0) & (~GPIO_Pin_1);
  GPIO_Init(GPIOE, &gpio_init);

  gpio_release(GPIOE);
  gpio_release(GPIOD);
  gpio_release(GPIOB);
}

static uint32_t s_num_flash_uses = 0;

void flash_impl_use(void) {
  if (s_num_flash_uses == 0) {
    periph_config_enable(FMC_Bank1, RCC_AHB3Periph_FMC); // FIXME
    prv_flash_idle_gpios(true);
  }
  s_num_flash_uses++;
}

void flash_impl_release_many(uint32_t num_locks) {
  PBL_ASSERTN(s_num_flash_uses >= num_locks);
  s_num_flash_uses -= num_locks;
  if (s_num_flash_uses == 0) {
    periph_config_disable(FMC_Bank1, RCC_AHB3Periph_FMC); // FIXME
  }
}

void flash_impl_release(void) {
  flash_impl_release_many(1);
}

static uint16_t flash_s29vs_read_short(FlashAddress addr) {
  return *((__IO uint16_t*)(FMC_BANK_1_BASE_ADDRESS + addr));
}

FlashAddress flash_impl_get_sector_base_address(FlashAddress addr) {
  if (addr < BOTTOM_BOOT_REGION_END) {
    return addr & ~(BOTTOM_BOOT_SECTOR_SIZE - 1);
  }

  return addr & ~(SECTOR_SIZE_BYTES - 1);
}

FlashAddress flash_impl_get_subsector_base_address(FlashAddress addr) {
  return flash_impl_get_sector_base_address(addr);
}

static uint8_t prv_read_status_register(FlashAddress sector_base_addr) {
  prv_issue_command(sector_base_addr, S29VSCommand_ReadStatusRegister);
  return flash_s29vs_read_short(sector_base_addr);
}

static uint8_t prv_poll_for_ready(FlashAddress sector_base_addr) {
  // TODO: We should probably just assert if this takes too long
  uint8_t status;
  while (((status = prv_read_status_register(sector_base_addr)) &
         S29VSStatusBit_DeviceReady) == 0) {
    delay_us(10);
  }

  return (status);
}

//! Issue the second part of a two-cycle command. This is not merged with the
//! prv_issue_command as not all commands have an argument.
//!
//! @param sector_address The address of the start of the sector to write the command to.
//! @param cmd_arg The command argument to write.
static void prv_issue_command_argument(FlashAddress sector_address, uint16_t cmd_arg) {
  // The offset in the sector we write the second part of commands to. Note that this is a 16-bit
  // word aligned address as opposed to a byte address.
  static const uint32_t COMMAND_ARGUMENT_ADDRESS = 0x2AA;

  ((__IO uint16_t*) (FMC_BANK_1_BASE_ADDRESS + sector_address))[COMMAND_ARGUMENT_ADDRESS] = cmd_arg;
}

//! @param sector_address The address of the start of the sector to write the command to.
//! @param cmd The command to write.
static void prv_issue_command(FlashAddress sector_address, S29VSCommand cmd) {
  // The offset in the sector we write the first part of commands to. Note that this is a 16-bit
  // word aligned address as opposed to a byte address.
  static const uint32_t COMMAND_ADDRESS = 0x555;

  ((__IO uint16_t*) (FMC_BANK_1_BASE_ADDRESS + sector_address))[COMMAND_ADDRESS] = cmd;
}

static void prv_software_reset(void) {
  prv_issue_command(0, S29VSCommand_SoftwareReset);
}

// Note: If this command has been executed at least once, all sectors are
// locked. They then must be unlocked before and relocked after each program
// operation (i.e write or erase). The chip only allows for one sector to be
// unlocked at any given time. For sector ranges which have been protected using
// the "Sector Lock Range Command", this function will have no effect.
static void prv_allow_write_if_sector_is_not_protected(bool lock, uint32_t sector_addr) {
    prv_issue_command(0, S29VSCommand_SectorLock);
    prv_issue_command_argument(0, S29VSCommand_SectorLock);

    int lock_flag = (lock ? 0 : 1) << 7; // set A6 to 0 to lock and 1 to unlock
    ((__IO uint16_t*) (FMC_BANK_1_BASE_ADDRESS + sector_addr + lock_flag))[0] =
        S29VSCommand_SectorLock;
}

static uint16_t prv_read_manufacturer_id(void) {
  // Issue the DeviceIDEntry command to change to the ID-CFI Address Map. This means that reading from the bank will
  // give us ID-CFI information instead of the normal flash contents. See Table 11.2 (ID/CFI Data) for all the
  // content you can read here. Reset the state afterwards to return to the default address map.

  flash_impl_use();
  prv_issue_command(0, S29VSCommand_DeviceIDEntry);
  uint16_t result = flash_s29vs_read_short(0x0);
  prv_software_reset();
  flash_impl_release();
  return result;
}

static uint16_t prv_read_configuration_register(void) {
  prv_issue_command(0, S29VSCommand_ConfigureRegisterEntry);
  uint16_t result = flash_s29vs_read_short(0x0);
  prv_software_reset();
  return result;
}

static void prv_write_configuration_register(uint16_t data) {
  // See section 5.8.1 of data sheet for command sequence
  prv_issue_command(0, S29VSCommand_ConfigureRegisterEntry);

  // Cycle 1: SA+Address 555h & Data 25h
  // Cycle 2: SA+Address 2AAh & Data 00h
  // Cycle 3: SA+Address X00h & PD
  // Cycle 4: SA+ Address 555h & Data 29h
  prv_issue_command(0, S29VSCommand_WriteBufferLoad);
  prv_issue_command_argument(0, 0);
  ((__IO uint16_t*) (FMC_BANK_1_BASE_ADDRESS))[0] = data;
  prv_issue_command(0, S29VSCommand_BufferToFlash);

  prv_software_reset();
}

// Use the "Sector Lock Range Command" (section 8.2 of data sheet) to block
// writes or erases to the PRF image residing on the flash. The only way to undo
// this is to issue a HW reset or pull power
static void prv_flash_protect_range(uint32_t start_sector, uint32_t end_sector) {
  PBL_ASSERTN(start_sector <= end_sector);

  flash_impl_use();

  prv_issue_command(0, S29VSCommand_SectorLock);
  prv_issue_command_argument(0, S29VSCommand_SectorLock);

  start_sector = flash_impl_get_sector_base_address(start_sector);
  end_sector = flash_impl_get_sector_base_address(end_sector);

  ((__IO uint16_t*) (FMC_BANK_1_BASE_ADDRESS + start_sector))[0] =
      S29VSCommand_SectorLockRangeArg;
  ((__IO uint16_t*) (FMC_BANK_1_BASE_ADDRESS + end_sector))[0] =
      S29VSCommand_SectorLockRangeArg;

  flash_impl_release();
}

void flash_s29vs_hw_init(void) {
  // Configure the reset pin (D2)
  GPIO_InitTypeDef gpio_init = {
  .GPIO_Pin = GPIO_Pin_2,
  .GPIO_Mode = GPIO_Mode_OUT,
  .GPIO_Speed = GPIO_Speed_100MHz,
  .GPIO_OType = GPIO_OType_PP,
  .GPIO_PuPd = GPIO_PuPd_NOPULL
  };
  GPIO_Init(GPIOD, &gpio_init);

  GPIO_WriteBit(GPIOD, GPIO_Pin_2, Bit_SET);

  // Configure pins relating to the FMC peripheral (30 pins!)

  // B7 - FMC AVD - FMC Address Valid aka Latch
  // D0-D1, D8-D15, E2-15 - FMC A, AD - FMC Address and Address/Data lines
  // D2 - Reset - GPIO Reset line
  // D3 - FMC CLK
  // D4 - FMC OE - FMC Output Enable
  // D5 - FMC WE - FMC Write Enable
  // D6 - FMC RDY - FMC Ready line
  // D7 - FMC CE - FMC Chip Enable

  GPIO_PinAFConfig(GPIOB, GPIO_PinSource7, GPIO_AF_FMC);
  gpio_init = s_default_at_flash_cfg;
  gpio_init.GPIO_Pin = GPIO_Pin_7;
  GPIO_Init(GPIOB, &gpio_init);

  for (uint8_t pin_source = 0; pin_source < 16; ++pin_source) {
    if (pin_source == 2) {
      continue;
    }
    GPIO_PinAFConfig(GPIOD, pin_source, GPIO_AF_FMC);
  }
  gpio_init.GPIO_Pin = GPIO_Pin_All & (~GPIO_Pin_2);
  GPIO_Init(GPIOD, &gpio_init);

  for (uint8_t pin_source = 2; pin_source < 16; ++pin_source) {
    GPIO_PinAFConfig(GPIOE, pin_source, GPIO_AF_FMC);
  }
  gpio_init.GPIO_Pin = GPIO_Pin_All & (~GPIO_Pin_0) & (~GPIO_Pin_1);
  GPIO_Init(GPIOE, &gpio_init);

  // We have configured the pins, lets perform a full HW reset to put the chip
  // in a good state
  GPIO_WriteBit(GPIOD, GPIO_Pin_2, Bit_RESET);
  delay_us(10); // only needs to be 50ns according to data sheet
  GPIO_WriteBit(GPIOD, GPIO_Pin_2, Bit_SET);
  delay_us(30); // need 200ns + 10us before CE can be pulled low

  flash_impl_set_burst_mode(false);
}

static void prv_flash_reset(void) {
  s_num_flash_uses = 0;
  gpio_use(GPIOB);
  gpio_use(GPIOD);
  gpio_use(GPIOE);
  flash_impl_use();

  flash_s29vs_hw_init();

  flash_impl_release();
  gpio_release(GPIOE);
  gpio_release(GPIOD);
  gpio_release(GPIOB);
}

void flash_impl_enable_write_protection(void) {
}

// Protects start_sector - end_sector, inclusive, from any kind of program
// operation
status_t flash_impl_write_protect(FlashAddress start_sector,
                                  FlashAddress end_sector) {
  prv_flash_reset();
  prv_flash_protect_range(start_sector, end_sector);
  return S_SUCCESS;
}

status_t flash_impl_unprotect(void) {
  // The only way to undo sector protection is to pull power from the chip or
  // issue a hardware reset
  prv_flash_reset();
  return S_SUCCESS;
}

status_t flash_impl_init(bool coredump_mode) {
  // Don't need to do anything to enable coredump mode.

  prv_flash_reset();
  return S_SUCCESS;
}

status_t flash_impl_get_erase_status(void) {
  flash_impl_use();
  uint8_t status = prv_read_status_register(0);
  flash_impl_release();

  if ((status & S29VSStatusBit_DeviceReady) == 0) return E_BUSY;
  if ((status & S29VSStatusBit_EraseSuspended) != 0) return E_AGAIN;
  if ((status & S29VSStatusBit_EraseStatus) != 0) return E_ERROR;
  return S_SUCCESS;
}

status_t flash_impl_erase_subsector_begin(FlashAddress subsector_addr) {
  return flash_impl_erase_sector_begin(subsector_addr);
}

status_t flash_impl_erase_sector_begin(FlashAddress sector_addr) {
  status_t result = E_UNKNOWN;

  // FIXME: We should just assert that the address is already aligned. If
  // someone is depending on this behaviour without already knowing the range
  // that's being erased they're going to have a bad time. This will probably
  // cause some client fallout though, so tackle this later.
  sector_addr = flash_impl_get_sector_base_address(sector_addr);

  flash_impl_use();
  prv_issue_command(sector_addr, S29VSCommand_ClearStatusRegister);

  // Some sanity checks
  {
    status_t error = S_SUCCESS;
    const uint8_t sr = prv_read_status_register(sector_addr);
    if ((sr & S29VSStatusBit_DeviceReady) == 0) {
      // Another operation is already in progress.
      error = E_BUSY;
    } else if (sr & S29VSStatusBit_EraseSuspended) {
      // Cannot program while another program operation is suspended.
      error = E_INVALID_OPERATION;
    }
    if (FAILED(error)) {
      result = error;
      goto done;
    }
  }

  prv_allow_write_if_sector_is_not_protected(false, sector_addr);

  prv_issue_command(sector_addr, S29VSCommand_EraseSetup);
  prv_issue_command_argument(sector_addr,
                             S29VSCommandEraseArguments_SectorErase);
  prv_allow_write_if_sector_is_not_protected(true, sector_addr);

  // Check the status register to make sure that the erase has started.
  const uint8_t sr = prv_read_status_register(sector_addr);
  if ((sr & S29VSStatusBit_DeviceReady) == 0) {
    // Program or erase operation in progress. Is it in the current bank?
    result = ((sr & S29VSStatusBit_BankStatus) == 0)? S_SUCCESS : E_BUSY;
  } else {
    // Operation hasn't started. Something is wrong.
    if (sr & S29VSStatusBit_SectorLockStatus) {
      // Sector is write-protected.
      result = E_INVALID_OPERATION;
    } else if (sr & S29VSStatusBit_EraseStatus) {
      // Erase failed for some reason.
      result = E_ERROR;
    } else {
      // The erase has either completed in the time between starting the erase
      // and polling the status register, or the erase was never started. The
      // former case could be due to a context switch at the worst time and
      // subsequent task starvation, or being run in QEMU. The latter could be
      // due to a software bug or hardware failure. It would be possible to tell
      // the two situations apart by performing a blank check, but that takes
      // more time than a nonblocking erase should require. Let the upper layers
      // verify that the erase succeeded if they care about it.
      result = S_SUCCESS;
    }
  }

done:
flash_impl_release();
  return result;
}

status_t flash_impl_erase_suspend(FlashAddress sector_addr) {
  status_t status = E_INTERNAL;
  sector_addr = flash_impl_get_sector_base_address(sector_addr);
  flash_impl_use();
  const uint8_t sr = prv_read_status_register(sector_addr);
  // Is an operation in progress?
  if ((sr & S29VSStatusBit_DeviceReady) != 0) {
    // No erase in progress to suspend. Maybe the erase completed before this
    // call.
    status = S_NO_ACTION_REQUIRED;
  } else if ((sr & S29VSStatusBit_BankStatus) != 0) {
    // Operation is in a different bank than the given address.
    status = E_INVALID_ARGUMENT;
  } else {
    // All clear.
    prv_issue_command(sector_addr, S29VSCommand_EraseSuspend);
    if (prv_poll_for_ready(sector_addr) & S29VSStatusBit_EraseSuspended) {
      status = S_SUCCESS;
    } else {
      // The erase must have completed between the status register read and
      // the EraseSuspend command.
      status = S_NO_ACTION_REQUIRED;
    }
  }
  flash_impl_release();
  return status;
}

status_t flash_impl_erase_resume(FlashAddress sector_addr) {
  status_t status = E_INTERNAL;
  sector_addr = flash_impl_get_sector_base_address(sector_addr);
  flash_impl_use();
  uint8_t sr = prv_read_status_register(sector_addr);
  if ((sr & S29VSStatusBit_DeviceReady) != 0 &&
      (sr & S29VSStatusBit_EraseSuspended) != 0) {
    prv_issue_command(sector_addr, S29VSCommand_EraseResume);
    status = S_SUCCESS;
  } else {
    // Device busy or no suspended erase to resume.
    status = E_INVALID_OPERATION;
  }
  flash_impl_release();
  return status;
}


// It is dangerous to leave this built in by default.
#if 0
status_t flash_impl_erase_bulk_begin(void) {
  flash_s29vs_use();

  prv_issue_command(0, S29VSCommand_EraseSetup);
  prv_issue_command_argument(0, S29VSCommandEraseArguments_ChipErase);

  flash_s29vs_release();
}
#endif

static void prv_read_words_pio(uint16_t* buffer, uint16_t* flash_data_region,
                               uint32_t num_words) {
  for (uint32_t words_read = 0; words_read < num_words; words_read++) {
    buffer[words_read] = flash_data_region[words_read];
  }
}

// Currently this implementation reads halfwords at a time (16-bits). Burst
// length is currently 1 for synchronous reads. This can be optimized in future
// to do larger burst sizes and/or unrolling larger transfer sizes into 32-bit
// reads.
status_t flash_impl_read_sync(void *buffer_ptr, FlashAddress start_addr,
                              size_t buffer_size) {
  uint8_t *buffer = buffer_ptr;
  flash_impl_use();

  uint32_t flash_data_addr = (FMC_BANK_1_BASE_ADDRESS + start_addr);
  bool odd_start_addr = ((start_addr % 2) == 1);
  uint32_t bytes_read = 0;

  uint16_t* buff_ptr = (uint16_t *)&buffer[bytes_read];
  if (odd_start_addr) {
    // read first byte into a temporary buffer but read from source on aligned word boundary
    uint16_t temp_buffer = *(__IO uint16_t *)(flash_data_addr-1);
    buffer[bytes_read++] = (uint8_t)((temp_buffer >> 8) & 0xFF);
  }

  // At this point, flash_data_addr is now halfword aligned
  buff_ptr = (uint16_t *)&buffer[bytes_read];
  bool odd_buff_addr = ((((uint32_t)buff_ptr) % 2) == 1);
  if (buffer_size - bytes_read >= 2) {
    // if at least one halfword to read
    if (!odd_buff_addr) {
      // Both flash_data_addr and buffer are aligned
      uint32_t num_words = (buffer_size - bytes_read) / 2;
      prv_read_words_pio(buff_ptr, (uint16_t*)(flash_data_addr + bytes_read), num_words);
      bytes_read += num_words*2;
    } else {
      // Not aligned - read into temporary buffer and copy over
      __IO uint16_t *flash_data_region = (__IO uint16_t*)(flash_data_addr + bytes_read);
      uint32_t num_words = (buffer_size - bytes_read) / 2;
      for (uint32_t words_read = 0; words_read < num_words; words_read++) {
        uint16_t temp_buffer = flash_data_region[words_read];
        buffer[bytes_read++] = (uint8_t)(temp_buffer & 0xFF);
        buffer[bytes_read++] = (uint8_t)((temp_buffer >> 8) & 0xFF);
      }
    }
  }

  buff_ptr = (uint16_t *)&buffer[bytes_read];
  // See if there are any remaining bytes left - at this point - flash_data_addr is still halfword aligned
  if (buffer_size - bytes_read == 1) {
    uint16_t temp_buffer = *(__IO uint16_t *)(flash_data_addr + bytes_read);
    buffer[bytes_read++] = (uint8_t)(temp_buffer & 0xFF);
  } else if (buffer_size - bytes_read != 0) {
    // Should not reach here
    PBL_LOG(LOG_LEVEL_DEBUG, "Invalid data length read");
  }

  flash_impl_release();

  return S_SUCCESS;
}

int flash_impl_write_page_begin(const void *vp_buffer,
                                const FlashAddress start_addr, size_t len) {
  if (!len) {
    return E_INVALID_ARGUMENT;
  }
  const uint8_t *buffer = vp_buffer;
  // Flash write transactions can only write one page at a time, where each
  // page is 64 bytes in size. Split up our transactions into pages and then
  // write one page.
  const uint32_t offset_in_page = start_addr % PAGE_SIZE_BYTES;
  const uint32_t bytes_in_page = MIN(PAGE_SIZE_BYTES - offset_in_page, len);

  // We're only allowed to write whole 16-bit words during a write operation.
  // Therefore we'll need to pad out our write if it's not perfectly aligned at
  // the start or the end.
  int num_shorts = bytes_in_page / 2;

  // 4 cases
  // Perfectly aligned - No additional writes
  // Unaligned start, even length - Need to pad both ends
  // Unaligned start, odd length - Pad the start
  // Aligned start, odd length - Pad the end
  if (start_addr & 0x1 || bytes_in_page & 0x1) {
    ++num_shorts;
  }

  const FlashAddress sector_addr =
    flash_impl_get_sector_base_address(start_addr);

  flash_impl_use();
  prv_issue_command(sector_addr, S29VSCommand_ClearStatusRegister);

  // Some sanity checks
  {
    status_t error = S_SUCCESS;
    const uint8_t sr = prv_read_status_register(sector_addr);
    if ((sr & S29VSStatusBit_DeviceReady) == 0) {
      // Another operation is already in progress.
      error = E_BUSY;
    } else if (sr & S29VSStatusBit_ProgramSuspended) {
      // Cannot program while another program operation is suspended.
      error = E_INVALID_OPERATION;
    }
    if (FAILED(error)) {
      flash_impl_release();
      return error;
    }
  }

  prv_allow_write_if_sector_is_not_protected(false, sector_addr);
  prv_issue_command(sector_addr, S29VSCommand_WriteBufferLoad);
  prv_issue_command_argument(sector_addr, num_shorts - 1);

  // We're now ready to write the words. Subsequent writes to the sector will
  // actually write the data through to the write buffer.

  __IO uint16_t *flash_write_dest = (__IO uint16_t*)
      (FMC_BANK_1_BASE_ADDRESS + (start_addr & ~0x1));
  uint32_t bytes_remaining = bytes_in_page;

  // Handle leading byte
  if (start_addr & 0x1) {
    // Handle a buffer with an unaligned start. Write 0xff for the first byte
    // since flash can only flip ones to zeros, and no data will be lost.
    const uint16_t first_short_value = 0xFF | ((*buffer) << 8);
    *flash_write_dest = first_short_value;

    // Now for the rest of the function let's pretend this never happened.
    ++flash_write_dest;
    ++buffer;
    --bytes_remaining;
  }

  // Handle body words
  for (; bytes_remaining >= 2; bytes_remaining -= 2, buffer += 2) {
    uint16_t buffer_word;
    memcpy(&buffer_word, buffer, sizeof buffer_word);
    *flash_write_dest++ = buffer_word;
  }

  // Handle trailing byte if present. This will be present if we started out
  // aligned and we wrote an odd number of bytes or if we started out unaligned
  // and wrote an even number of bytes.
  if (bytes_remaining) {
    // We need to write only a single byte, but we're only allowed to write
    // words. If we write a single byte followed by 0xFFFF, we won't modify the
    // second byte as bits are only allowed to be written from 1 -> 0. 1s will
    // stay 1s, and 0s will stay 0s.
    const uint16_t trailing_short_value = *buffer | 0xFF00;
    *flash_write_dest = trailing_short_value;
  }

  // Buffer writing is complete, issue the buffer to flash command to actually
  // commit the changes to memory.
  prv_issue_command(sector_addr, S29VSCommand_BufferToFlash);

  // Check the status register to make sure that the write has started.
  status_t result = E_UNKNOWN;
  const uint8_t sr = prv_read_status_register(sector_addr);
  if ((sr & S29VSStatusBit_DeviceReady) == 0) {
    // Program or erase operation in progress. Is it in the current bank?
    result = ((sr & S29VSStatusBit_BankStatus) == 0)? S_SUCCESS : E_BUSY;
  } else {
    // Operation hasn't started. Something is wrong.
    if (sr & S29VSStatusBit_SectorLockStatus) {
      // Sector is write-protected.
      result = E_INVALID_OPERATION;
    } else if (sr & S29VSStatusBit_ProgramStatus) {
      // Programming failed for some reason.
      result = E_ERROR;
    } else {
      // The flash never appeared to go busy and there is no error. Either the
      // flash write completed between the write command and the status register
      // read (inopportune context switch or running in QEMU), or the write
      // never started. It's possible to tell them apart by validating that the
      // data was actually written to flash, but that adds even more complexity
      // to this function. Let the upper layers verify that the write succeeded
      // if they are concerned about reliability.
      result = S_SUCCESS;
    }
  }

  prv_allow_write_if_sector_is_not_protected(true, sector_addr);
  flash_impl_release();
  return FAILED(result) ? result : (int)bytes_in_page;
}

status_t flash_impl_get_write_status(void) {
  flash_impl_use();
  const uint8_t status = prv_read_status_register(0);
  flash_impl_release();

  if ((status & S29VSStatusBit_DeviceReady) == 0) return E_BUSY;
  if ((status & S29VSStatusBit_ProgramSuspended) != 0) return E_AGAIN;
  if ((status & S29VSStatusBit_ProgramStatus) != 0) return E_ERROR;
  return S_SUCCESS;
}

uint8_t pbl_28517_flash_impl_get_status_register(uint32_t sector_addr) {
  flash_impl_use();

  const FlashAddress base_addr = flash_impl_get_sector_base_address(sector_addr);
  const uint8_t status = prv_read_status_register(base_addr);

  flash_impl_release();

  return status;
}

status_t flash_impl_enter_low_power_mode(void) {
  prv_flash_idle_gpios(false);
  return S_SUCCESS;
}

status_t flash_impl_exit_low_power_mode(void) {
  // it's ok to access s_num_flash_uses here directly, as only caller enter_stop_mode() is called
  // only while interrupts are disabled
  prv_flash_idle_gpios(s_num_flash_uses > 0);
  return S_SUCCESS;
}

static void prv_switch_flash_mode(FMC_NORSRAMInitTypeDef *nor_init) {
  FMC_NORSRAMCmd(FMC_Bank1_NORSRAM1, DISABLE);
  FMC_NORSRAMInit(nor_init);
  FMC_NORSRAMCmd(FMC_Bank1_NORSRAM1, ENABLE);
}

static uint16_t prv_get_num_wait_cycles(uint32_t flash_clock_freq) {
  // wait_cycle table based on frequency (table 7.1)
  // NOTE: 27MHZ frequency skipped due to data latency being 4 smaller than the wait_cycle
  uint32_t wait_cycle[] = {
    40000000  ,
    54000000  ,
    66000000  ,
    80000000  ,
    95000000  ,
    104000000 ,
    120000000
  };
  // find number wait states based on table
  uint32_t wait_state;
  for (wait_state = 4; wait_state < (ARRAY_LENGTH(wait_cycle) + 4); wait_state++) {
    if (flash_clock_freq < wait_cycle[wait_state-4]) {
      break;
    }
  }
  return wait_state;
}

status_t flash_impl_set_burst_mode(bool burst_mode) {
  const uint32_t MAX_FREQ = MHZ_TO_HZ(108);  // max frequency of the flash 108MHZ
  const uint32_t TAVDP_MIN = 60;             // min addr setup time in tenths of ns
  const uint32_t TADVO_MIN = 40;             // min addr hold time in tenths

  const uint32_t SETUP_STEP = MHZ_TO_HZ(16); // for data setup equation

  const uint16_t WAIT_STATE_MASK = 0x7800;   // mask for wait state binary for sync burst

  flash_impl_use();

  // get system clock tick speed
  RCC_ClocksTypeDef clocks;
  RCC_GetClocksFreq(&clocks);
  uint32_t h_clock = clocks.HCLK_Frequency;          // frequency in hertz
  uint32_t time_per_cycle = ((uint64_t)(10000000000)) / h_clock; // period in 1/10th ns

  FMC_NORSRAMTimingInitTypeDef nor_timing_init = {
    // time between address write and address latch (AVD high)
    // tAAVDS on datasheet, min 4 ns
    //
    // AVD low time
    // tAVDP on datasheet, min 6 ns
    .FMC_AddressSetupTime = (TAVDP_MIN / time_per_cycle) + 1, // give setup of min 6ns

    // time between AVD high (address is available) and OE low (memory can write)
    // tAVDO on the datasheet, min 4 ns
    .FMC_AddressHoldTime = (TADVO_MIN / time_per_cycle) + 1,  // gives hold of min 4ns

    // time between OE low (memory can write) and valid data being available
    // FIXME: optimize this equation
    // current linear equation has slope of 1 cycle/SETUP_STEP, with initial value 1
    // setupTime based on h_clock frequency
    // equation derived from existing working values; 5 at 64Mhz, 8 at 128 Mhz
    // the data was then interpolated into a line, with a padded value of 1
    .FMC_DataSetupTime = (h_clock / SETUP_STEP) + 1,

    // Time between chip selects
    // not on the datasheet, picked a random safe number
    // FIXME: at high bus frequencies, more than one cycle may be needed
    .FMC_BusTurnAroundDuration = 1, // TODO: actually ok? See back-to-back Read/Write Cycle

    .FMC_CLKDivision = 15, // Not used for async NOR
    .FMC_DataLatency = 15, // Not used for async NOR
    .FMC_AccessMode = FMC_AccessMode_A // Only used for ExtendedMode == FMC_ExtendedMode_Enable, which we don't use
  };

  FMC_NORSRAMInitTypeDef nor_init = {
    .FMC_Bank = FMC_Bank1_NORSRAM1,
    .FMC_DataAddressMux = FMC_DataAddressMux_Enable,
    .FMC_MemoryType = FMC_MemoryType_NOR,
    .FMC_MemoryDataWidth = FMC_NORSRAM_MemoryDataWidth_16b,
    .FMC_BurstAccessMode = FMC_BurstAccessMode_Disable,
    .FMC_AsynchronousWait = FMC_AsynchronousWait_Disable,
    .FMC_WaitSignalPolarity = FMC_WaitSignalPolarity_Low,
    .FMC_WrapMode = FMC_WrapMode_Disable,
    .FMC_WaitSignalActive = FMC_WaitSignalActive_BeforeWaitState,
    .FMC_WriteOperation = FMC_WriteOperation_Enable,
    .FMC_WaitSignal = FMC_WaitSignal_Enable,
    .FMC_ExtendedMode = FMC_ExtendedMode_Disable,
    .FMC_WriteBurst = FMC_WriteBurst_Disable,
    .FMC_ContinousClock = FMC_CClock_SyncOnly,
    .FMC_ReadWriteTimingStruct = &nor_timing_init
  };

  // configure the peripheral before we try to read from it
  prv_switch_flash_mode(&nor_init);

  uint16_t configuration_register = prv_read_configuration_register();
  // clear bits that are about to be set
  configuration_register &= 0x0278; // clear bits [15:10], [8:7], [2:0]

  // add one. This way, if (h_clock < MAX_FREQ), only divide by one (use h_clock as is)
  // else divide by whatever is needed to be under MAX_FREQ
  uint32_t clk_division = (h_clock / (MAX_FREQ + 1)) + 1;

  // Update necessary parameters for synchronous modes
  if (burst_mode) {
    nor_init.FMC_BurstAccessMode = FMC_BurstAccessMode_Enable;
    nor_init.FMC_WaitSignalActive = FMC_WaitSignalActive_DuringWaitState;

    nor_timing_init.FMC_BusTurnAroundDuration = 1;

    // nor_timing_init.FMC_DataSetupTime = 1;           // FIXME: originally set to 1 for 64Mhz
    // but sync burst was not working at this value;
    // commented out so the DataSetupTime for ASYNC (up above) is used instead
    // this is to ensure sync_burst works with dynamic changes to h_clk frequency

    nor_timing_init.FMC_CLKDivision = clk_division;    // divide h_clock if h_clock > 108MHZ

    uint16_t wait_state = prv_get_num_wait_cycles(h_clock / clk_division);
    // testing shows that a difference of 4 needs to be maintained between wait_state and latency
    nor_timing_init.FMC_DataLatency = wait_state - 4;

    // Set bits according to value needed - see Table 7.11 in data sheet
    // [15]    Device Read Mode                0b0     Synchronous Read Mode
    // [14:11] Programmable Read Wait States   0bXXXX  N wait cycles, wait states set to (N - 2)
    // [10]    RDY Polarity                    0b1     RDY signal is active high (default)
    // [8]     RDY Timing                      0b0     RDY active once cycle before data (default)
    // [7]     Output Drive Strength           0b0     Full Drive=Current Driver Strength (default)
    // [2:0]   Burst Length                    0b000   Continuous (default)
    configuration_register |= 0x400 | (((wait_state - 2) << 11) & (WAIT_STATE_MASK));
  } else {
    // Set bits according to value needed - see Table 7.11 in data sheet
    // [15]    Device Read Mode                0b1     Asynchronous Read Mode
    // [14:11] Programmable Read Wait States   0b1011  13 wait cycles (default)
    // [10]    RDY Polarity                    0b1     RDY signal is active high (default)
    // [8]     RDY Timing                      0b1     RDY active with data (default)
    // [7]     Output Drive Strength           0b0     Full Drive=Current Driver Strength (default)
    // [2:0]   Burst Length                    0b000   Continuous (default)
    configuration_register |= 0xDD00;
  }

  prv_write_configuration_register(configuration_register);

  prv_switch_flash_mode(&nor_init);

  prv_poll_for_ready(0);
  flash_impl_release();

  return S_SUCCESS;
}

status_t flash_impl_blank_check_sector(FlashAddress addr) {
  // FIXME: Blank check operation is only allowed in asynchronous mode. Fall
  // back to a software blank check in synchronous mode.
  const FlashAddress base_addr = flash_impl_get_sector_base_address(addr);
  status_t ret = E_INTERNAL;

  flash_impl_use();
  uint8_t status = prv_read_status_register(base_addr);
  if ((status & S29VSStatusBit_DeviceReady) == 0 ||
      (status & (S29VSStatusBit_EraseSuspended |
                 S29VSStatusBit_ProgramSuspended)) != 0) {
    ret = E_BUSY;
    goto done;
  }

  prv_issue_command(base_addr, S29VSCommand_SectorBlank);
  status = prv_poll_for_ready(base_addr);
  ret = ((status & S29VSStatusBit_EraseStatus) == 0)? S_TRUE : S_FALSE;

done:
flash_impl_release();
  return ret;
}

status_t flash_impl_blank_check_subsector(FlashAddress addr) {
  return flash_impl_blank_check_sector(addr);
}


bool flash_check_whoami(void) {
  uint16_t manufacturer_id = prv_read_manufacturer_id();
  PBL_LOG(LOG_LEVEL_DEBUG, "Flash Manufacturer ID: 0x%"PRIx16, manufacturer_id);

  return manufacturer_id == SPANSION_MANUFACTURER_ID ||
         manufacturer_id == MACRONIX_MANUFACTURER_ID;
}

uint32_t flash_impl_get_typical_sector_erase_duration_ms(void) {
  return 800;
}

uint32_t flash_impl_get_typical_subsector_erase_duration_ms(void) {
  return 800;
}
