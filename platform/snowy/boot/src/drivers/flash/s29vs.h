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

#include "drivers/flash.h"

//! An address in the flash address spac
typedef uint32_t FlashAddress;

//! This is the memory mapped region that's mapped to the parallel flash.
static const uintptr_t FMC_BANK_1_BASE_ADDRESS = 0x60000000;

//! This is the unit that we use for erasing
static const uint32_t SECTOR_SIZE_BYTES = 0x20000; // 128kb
//! This is the unit that we use for writing
static const uint32_t PAGE_SIZE_BYTES = 64;

//! Different commands we can send to the flash
typedef enum S29VSCommand {
  S29VSCommand_WriteBufferLoad = 0x25,
  S29VSCommand_BufferToFlash = 0x29,
  S29VSCommand_ReadStatusRegister = 0x70,
  S29VSCommand_ClearStatusRegister = 0x71,
  S29VSCommand_EraseSetup = 0x80,
  S29VSCommand_DeviceIDEntry = 0x90,
  S29VSCommand_CFIEntry = 0x98,
  S29VSCommand_ConfigureRegisterEntry = 0xD0,
  S29VSCommand_SoftwareReset = 0xF0
} S29VSCommand;

//! Arguments to the S29VSCommand_EraseSetup command
typedef enum S29VSCommandEraseArguments {
  S29VSCommandEraseArguments_ChipErase = 0x10,
  S29VSCommandEraseArguments_SectorErase = 0x30
} S29VSCommandEraseArguments;

//! The bitset stored in the status register, see flash_s29vs_read_status_register
typedef enum S29VSStatusBit {
  S29VSStatusBit_BankStatus = 0x00,
  S29VSStatusBit_SectorLockStatus = 0x01,
  S29VSStatusBit_ProgramSuspended = 0x02,
  // 0x04 is unused
  S29VSStatusBit_ProgramStatus = 0x10,
  S29VSStatusBit_EraseStatus = 0x20,
  S29VSStatusBit_EraseSuspended = 0x40,
  S29VSStatusBit_DeviceReady = 0x80,
} S29VSStatusBit;
