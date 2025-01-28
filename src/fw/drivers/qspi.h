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

//! Memory mapped region for the QSPI controller
#define QSPI_MMAP_BASE_ADDRESS ((uintptr_t) 0x90000000)

//! Timeouts for qspi_poll_bit
#define QSPI_NO_TIMEOUT (0)

typedef const struct QSPIPort QSPIPort;

//! Enable the peripheral clock
void qspi_use(QSPIPort *dev);

//! Disable the peripheral clock
void qspi_release(QSPIPort *dev);

//! Perform an indirect read operation
//! @param instruction The instruction to issue
//! @param dummy_cycles How many cycles to wait before reading data
//! @param buffer The buffer to read into
//! @param length The number of bytes to read
void qspi_indirect_read_no_addr(QSPIPort *dev, uint8_t instruction, uint8_t dummy_cycles,
                                void *buffer, uint32_t length, bool is_ddr);

//! Perform an indirect read operation with an address
//! @param instruction The instruction to issue
//! @param addr The address to read from
//! @param dummy_cycles How many cycles to wait before reading data
//! @param buffer The buffer to read into
//! @param length The number of bytes to read
void qspi_indirect_read(QSPIPort *dev, uint8_t instruction, uint32_t addr, uint8_t dummy_cycles,
                        void *buffer, uint32_t length, bool is_ddr);

//! Performs an indirect read operation with DMA
//! @param instruction The instruction to issue
//! @param start_addr The address to read from
//! @param dummy_cycles How many cycles to wait before reading data
//! @param buffer The buffer to read into
//! @param length The number of bytes to read
void qspi_indirect_read_dma(QSPIPort *dev, uint8_t instruction, uint32_t start_addr,
                            uint8_t dummy_cycles, void *buffer, uint32_t length, bool is_ddr);

//! Perform an indirect write operation
//! @param instruction The instruction to issue
//! @param buffer The buffer to write from or NULL if no data should be written
//! @param length The number of bytes to write or 0 if no data should be written
void qspi_indirect_write_no_addr(QSPIPort *dev, uint8_t instruction, const void *buffer,
                                 uint32_t length);

//! Perform an indirect write operation with an address
//! @param instruction The instruction to issue
//! @param addr The address to write to
//! @param buffer The buffer to write from or NULL if no data should be written
//! @param length The number of bytes to write or 0 if no data should be written
void qspi_indirect_write(QSPIPort *dev, uint8_t instruction, uint32_t addr, const void *buffer,
                         uint32_t length);

//! Perform an indirect write operation in single SPI mode (not quad SPI)
//! @param instruction The instruction to issue
void qspi_indirect_write_no_addr_1line(QSPIPort *dev, uint8_t instruction);

//! Perform an automatic poll operation which will wait for the specified bits to be set/cleared
//! @param instruction The instruction to issue
//! @param bit_mask The bit(s) to poll on (wait for)
//! @param should_be_set Whether the bits should be set or cleared (true / false respectively)
//! @param timeout_us The maximum amount of time to wait in us
bool qspi_poll_bit(QSPIPort *dev, uint8_t instruction, uint8_t bit_mask, bool should_be_set,
                   uint32_t timeout_us);

//! Puts the QSPI in memory-mapped mode
//! @param instruction The instruction to issue
//! @param addr address of data that will be accessed via memory mapping
//! @param dummy_cycles How many cycles to wait before we can start reading the mapped memory
//! @param length length of data that will be accessed via memory mapping
void qspi_mmap_start(QSPIPort *dev, uint8_t instruction, uint32_t addr, uint8_t dummy_cycles,
                     uint32_t length, bool is_ddr);

//! Aborts the memory-mapped mode
void qspi_mmap_stop(QSPIPort *dev);
