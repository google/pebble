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
#include "board/board.h"
#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "drivers/spi.h"
#include "flash_region/flash_region.h"
#include "system/passert.h"
#include "system/logging.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#include <mcu.h>

#include "debug/power_tracking.h"

/* GPIO */
static GPIO_TypeDef* const FLASH_GPIO = GPIOA;
/* SPI */
static SPI_TypeDef* const FLASH_SPI = SPI1;
static const uint32_t FLASH_SPI_CLOCK = RCC_APB2Periph_SPI1;
static const SpiPeriphClock FLASH_SPI_CLOCK_PERIPH = SpiPeriphClockAPB2;


/* Pin Definitions */
static const uint32_t FLASH_PIN_SCS = GPIO_Pin_4;
static const uint32_t FLASH_PIN_SCLK = GPIO_Pin_5;
static const uint32_t FLASH_PIN_MISO = GPIO_Pin_6;
static const uint32_t FLASH_PIN_MOSI = GPIO_Pin_7;

/* Flash SPI commands */
static const uint8_t FLASH_CMD_WRITE_ENABLE = 0x06;
static const uint8_t FLASH_CMD_WRITE_DISABLE = 0x04;
static const uint8_t FLASH_CMD_READ_STATUS_REG = 0x05;
static const uint8_t FLASH_CMD_READ_FLAG_STATUS_REG = 0x70;
static const uint8_t FLASH_CMD_CLEAR_FLAG_STATUS_REG = 0x50;
static const uint8_t FLASH_CMD_READ = 0x03;
static const uint8_t FLASH_CMD_READ_ID = 0x9F;
static const uint8_t FLASH_CMD_PAGE_PROGRAM = 0x02;
static const uint8_t FLASH_CMD_ERASE_SUBSECTOR = 0x20;
static const uint8_t FLASH_CMD_ERASE_SECTOR = 0xD8;
static const uint8_t FLASH_CMD_ERASE_BULK = 0xC7;
static const uint8_t FLASH_CMD_DEEP_SLEEP = 0xB9;
static const uint8_t FLASH_CMD_WAKE = 0xAB;
static const uint8_t FLASH_CMD_DUMMY = 0xA9;
static const uint8_t FLASH_CMD_WRITE_LOCK_REGISTER = 0xE5;
static const uint8_t FLASH_CMD_READ_LOCK_REGISTER = 0xE8;
static const uint8_t FLASH_CMD_READ_NONVOLATILE_CONFIG_REGISTER = 0xB5;
static const uint8_t FLASH_CMD_READ_VOLATILE_CONFIG_REGISTER = 0x85;

static const uint16_t FLASH_PAGE_SIZE = 0x100;

typedef enum N25QFlagStatusBit {
  // Bit 0 is reserved
  N25QFlagStatusBit_SectorLockStatus = (1 << 1),
  N25QFlagStatusBit_ProgramSuspended = (1 << 2),
  N25QFlagStatusBit_VppStatus        = (1 << 3),
  N25QFlagStatusBit_ProgramStatus    = (1 << 4),
  N25QFlagStatusBit_EraseStatus      = (1 << 5),
  N25QFlagStatusBit_EraseSuspended   = (1 << 6),
  N25QFlagStatusBit_DeviceReady      = (1 << 7),
} N25QFlagStatusBit;

typedef enum N25QStatusBit {
  N25QStatusBit_WriteInProgress     = (1 << 0),
  N25QStatusBit_WriteEnableLatch    = (1 << 1),
  N25QStatusBit_BlockProtect0       = (1 << 2),
  N25QStatusBit_BlockProtect1       = (1 << 3),
  N25QStatusBit_BlockProtect2       = (1 << 4),
  N25QStatusBit_ProtectTopBottom    = (1 << 5),
  // Bit 6 is reserved
  N25QStatusBit_StatusRegisterWrite = (1 << 7),
} N25QStatusBit;

typedef enum N25QLockBit {
  N25QLockBit_SectorWriteLock = (1 << 0),
  N25QLockBit_SectorLockDown  = (1 << 1),
  // Bits 2-7 are reserved
} N25QLockBit;

// Method shared with flash.c and the core dump logic in core_dump.c
void flash_hw_init(void);
void assert_usable_state(void);
void flash_lock(void);
void flash_unlock(void);
bool flash_is_enabled(void);
void handle_sleep_when_idle_begin(void);
void enable_flash_spi_clock(void);
void disable_flash_spi_clock(void);
void flash_start(void);
void flash_start_cmd(void);
void flash_end_cmd(void);
uint8_t flash_send_and_receive_byte(uint8_t byte);
void flash_write_enable(void);
void flash_send_24b_address(uint32_t start_addr);
uint8_t flash_read_next_byte(void);
void flash_wait_for_write_bounded(volatile int cycles_to_wait);
void flash_wait_for_write(void);
bool check_whoami(uint32_t spi_flash_id);
bool flash_is_whoami_correct(void);
