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

#include "qspi_flash.h"
#include "qspi_flash_definitions.h"

#include "board/board.h"
#include "drivers/flash/flash_impl.h"
#include "drivers/gpio.h"
#include "drivers/qspi.h"
#include "flash_region/flash_region.h"
#include "kernel/util/delay.h"
#include "kernel/util/sleep.h"
#include "mcu/cache.h"
#include "system/logging.h"
#include "system/passert.h"
#include "system/status_codes.h"
#include "util/math.h"

#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>


#define FLASH_RESET_WORD_VALUE (0xffffffff)


static void prv_read_register(QSPIFlash *dev, uint8_t instruction, uint8_t *data, uint32_t length) {
  qspi_indirect_read_no_addr(dev->qspi, instruction, 0, data, length, false /* !is_ddr */);
}

static void prv_write_cmd_no_addr(QSPIFlash *dev, uint8_t cmd) {
  qspi_indirect_write_no_addr(dev->qspi, cmd, NULL, 0);
}

static void prv_write_enable(QSPIFlash *dev) {
  prv_write_cmd_no_addr(dev, dev->state->part->instructions.write_enable);
  // wait for writing to be enabled
  qspi_poll_bit(dev->qspi, dev->state->part->instructions.read_status,
                dev->state->part->status_bit_masks.write_enable, true /* set */, QSPI_NO_TIMEOUT);
}

static bool prv_check_whoami(QSPIFlash *dev) {
  // The WHOAMI is 3 bytes
  const uint32_t whoami_length = 3;
  uint32_t read_whoami = 0;
  prv_read_register(dev, dev->state->part->instructions.qspi_id, (uint8_t *)&read_whoami,
                    whoami_length);

  if (read_whoami == dev->state->part->qspi_id_value) {
    PBL_LOG(LOG_LEVEL_INFO, "Flash is %s", dev->state->part->name);
    return true;
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "Flash isn't expected %s (whoami: 0x%"PRIx32")",
            dev->state->part->name, read_whoami);
    return false;
  }
  qspi_release(dev->qspi);
}

bool qspi_flash_check_whoami(QSPIFlash *dev) {
  qspi_use(dev->qspi);
  bool result = prv_check_whoami(dev);
  qspi_release(dev->qspi);
  return result;
}

static void prv_set_fast_read_ddr_enabled(QSPIFlash *dev, bool enabled) {
  // If we're supposed to use DDR for fast read, make sure the part can support it
  PBL_ASSERTN(!enabled || dev->state->part->supports_fast_read_ddr);
  dev->state->fast_read_ddr_enabled = enabled;
}

bool qspi_flash_is_in_coredump_mode(QSPIFlash *dev) {
  return dev->state->coredump_mode;
}

void qspi_flash_init(QSPIFlash *dev, QSPIFlashPart *part, bool coredump_mode) {
  dev->state->part = part;
  dev->state->coredump_mode = coredump_mode;
  prv_set_fast_read_ddr_enabled(dev, dev->default_fast_read_ddr_enabled);
  qspi_use(dev->qspi);

  if (dev->reset_gpio.gpio) {
    gpio_output_init(&dev->reset_gpio, GPIO_OType_PP, GPIO_Speed_2MHz);
    gpio_output_set(&dev->reset_gpio, false);
  }

  // Must call quad_enable first, all commands are QSPI
  qspi_indirect_write_no_addr_1line(dev->qspi, dev->state->part->instructions.enter_quad_mode);

  // Reset the flash to stop any program's or erase in progress from before reboot
  prv_write_cmd_no_addr(dev, dev->state->part->instructions.reset_enable);
  prv_write_cmd_no_addr(dev, dev->state->part->instructions.reset);

  if (coredump_mode) {
    delay_us(dev->state->part->reset_latency_ms * 1000);
  } else {
    psleep(dev->state->part->reset_latency_ms);
  }

  // Return the flash to Quad SPI mode, all our commands are quad-spi and it'll just cause
  // problems/bugs for someone if it comes back in single spi mode
  qspi_indirect_write_no_addr_1line(dev->qspi, dev->state->part->instructions.enter_quad_mode);

  if (!coredump_mode) {
    prv_check_whoami(dev);
  }

  qspi_release(dev->qspi);
}

status_t qspi_flash_is_erase_complete(QSPIFlash *dev) {
  qspi_use(dev->qspi);

  uint8_t status_reg;
  uint8_t flag_status_reg;
  prv_read_register(dev, dev->state->part->instructions.read_status, &status_reg, 1);
  prv_read_register(dev, dev->state->part->instructions.read_flag_status, &flag_status_reg, 1);

  qspi_release(dev->qspi);

  if (status_reg & dev->state->part->status_bit_masks.busy) {
    return E_BUSY;
  } else if (flag_status_reg & dev->state->part->flag_status_bit_masks.erase_suspend) {
    return E_AGAIN;
  } else {
    return S_SUCCESS;
  }
}

status_t qspi_flash_erase_begin(QSPIFlash *dev, uint32_t addr, bool is_subsector) {
  uint8_t instruction;
  if (is_subsector) {
    instruction = dev->state->part->instructions.erase_sector_4k;
  } else {
    instruction = dev->state->part->instructions.erase_block_64k;
  }

  qspi_use(dev->qspi);
  prv_write_enable(dev);
  qspi_indirect_write(dev->qspi, instruction, addr, NULL, 0);
  // wait for busy to be set indicating the erase has started
  const uint32_t busy_timeout_us = 500;
  const bool result = qspi_poll_bit(dev->qspi, dev->state->part->instructions.read_status,
                                    dev->state->part->status_bit_masks.busy, true /* set */,
                                    busy_timeout_us);
  qspi_release(QSPI);

  return result ? S_SUCCESS : E_ERROR;
}

status_t qspi_flash_erase_suspend(QSPIFlash *dev, uint32_t addr) {
  qspi_use(dev->qspi);

  uint8_t status_reg;
  prv_read_register(dev, dev->state->part->instructions.read_status, &status_reg, 1);
  if (!(status_reg & dev->state->part->status_bit_masks.busy)) {
    // no erase in progress
    qspi_release(QSPI);
    return S_NO_ACTION_REQUIRED;
  }

  prv_write_cmd_no_addr(dev, dev->state->part->instructions.erase_suspend);

  qspi_release(QSPI);

  if (dev->state->part->suspend_to_read_latency_us) {
    delay_us(dev->state->part->suspend_to_read_latency_us);
  }

  return S_SUCCESS;
}

void qspi_flash_erase_resume(QSPIFlash *dev, uint32_t addr) {
  qspi_use(dev->qspi);
  prv_write_cmd_no_addr(dev, dev->state->part->instructions.erase_resume);
  // wait for the erase_suspend bit to be cleared
  qspi_poll_bit(dev->qspi, dev->state->part->instructions.read_flag_status,
                dev->state->part->flag_status_bit_masks.erase_suspend, false /* !set */,
                QSPI_NO_TIMEOUT);
  qspi_release(dev->qspi);
}

static void prv_get_fast_read_params(QSPIFlash *dev, uint8_t *instruction, uint8_t *dummy_cycles,
                                     bool *is_ddr) {
  if (dev->state->fast_read_ddr_enabled) {
    *instruction = dev->state->part->instructions.fast_read_ddr;
    *dummy_cycles = dev->state->part->dummy_cycles.fast_read_ddr;
    *is_ddr = true;
  } else {
    *instruction = dev->state->part->instructions.fast_read;
    *dummy_cycles = dev->state->part->dummy_cycles.fast_read;
    *is_ddr = false;
  }
}

static void prv_read_mmap_with_params(QSPIFlash *dev, uint32_t addr, void *buffer, uint32_t length,
                                      uint8_t instruction, uint8_t dummy_cycles, bool is_ddr) {
  qspi_mmap_start(dev->qspi, instruction, addr, dummy_cycles, length, is_ddr);

  // Point the buffer at the QSPI region
  memcpy(buffer, (uint32_t *)(QSPI_MMAP_BASE_ADDRESS + addr), length);

  // stop memory mapped mode
  qspi_mmap_stop(dev->qspi);
}

static void prv_read_mmap(QSPIFlash *dev, uint32_t addr, void *buffer, uint32_t length) {
  uint8_t instruction;
  uint8_t dummy_cycles;
  bool is_ddr;
  prv_get_fast_read_params(dev, &instruction, &dummy_cycles, &is_ddr);

  prv_read_mmap_with_params(dev, addr, buffer, length, instruction,
                            dummy_cycles, is_ddr);
}

void qspi_flash_read_blocking(QSPIFlash *dev, uint32_t addr, void *buffer, uint32_t length) {
  // TODO: Figure out what thresholds we should use when switching between memory mapping, DMA, &
  // polling PBL-37438
  bool should_use_dma = length > 128 && !dev->state->coredump_mode;
  bool should_use_memmap = length > 128;

#if QSPI_DMA_DISABLE
  // Known issues with some platforms, see PBL-37278 as an example
  should_use_dma = false;
#endif

#if TARGET_QEMU
  // QEMU doesn't yet support DMA or memory-mapping
  should_use_dma = should_use_memmap = false;
#endif

  qspi_use(dev->qspi);
  uint8_t instruction;
  uint8_t dummy_cycles;
  bool is_ddr;
  prv_get_fast_read_params(dev, &instruction, &dummy_cycles, &is_ddr);
  if (should_use_dma) {
    qspi_indirect_read_dma(dev->qspi, instruction, addr, dummy_cycles, buffer, length, is_ddr);
  } else if (should_use_memmap) {
    prv_read_mmap_with_params(dev, addr, buffer, length, instruction, dummy_cycles, is_ddr);
  } else {
    qspi_indirect_read(dev->qspi, instruction, addr, dummy_cycles, buffer, length, is_ddr);
  }
  qspi_release(dev->qspi);
}

int qspi_flash_write_page_begin(QSPIFlash *dev, const void *buffer, uint32_t addr,
                                uint32_t length) {
  const uint32_t offset_in_page = addr % PAGE_SIZE_BYTES;
  const uint32_t bytes_in_page = MIN(PAGE_SIZE_BYTES - offset_in_page, length);

  qspi_use(dev->qspi);
  prv_write_enable(dev);
  qspi_indirect_write(dev->qspi, dev->state->part->instructions.page_program, addr, buffer,
                      bytes_in_page);
  qspi_poll_bit(dev->qspi, dev->state->part->instructions.read_status,
                dev->state->part->status_bit_masks.busy, false /* !set */, QSPI_NO_TIMEOUT);
  qspi_release(dev->qspi);

  return bytes_in_page;
}

status_t qspi_flash_get_write_status(QSPIFlash *dev) {
  qspi_use(dev->qspi);
  uint8_t status_reg;
  prv_read_register(dev, dev->state->part->instructions.read_status, &status_reg, 1);
  qspi_release(dev->qspi);
  return (status_reg & dev->state->part->status_bit_masks.busy) ? E_BUSY : S_SUCCESS;
}

void qspi_flash_set_lower_power_mode(QSPIFlash *dev, bool active) {
  qspi_use(dev->qspi);
  uint8_t instruction;
  uint32_t delay;
  if (active) {
    instruction = dev->state->part->instructions.enter_low_power;
    delay = dev->state->part->standby_to_low_power_latency_us;
  } else {
    instruction = dev->state->part->instructions.exit_low_power;
    delay = dev->state->part->low_power_to_standby_latency_us;
  }
  prv_write_cmd_no_addr(dev, instruction);
  qspi_release(dev->qspi);
  if (delay) {
    delay_us(delay);
  }
}

#if TARGET_QEMU
// While this works with normal hardware, it has a large stack requirment and I can't
// see a compelling reason to use it over the mmap blank check variant
static bool prv_blank_check_poll(QSPIFlash *dev, uint32_t addr, bool is_subsector) {
  const uint32_t size_bytes = is_subsector ? SUBSECTOR_SIZE_BYTES : SECTOR_SIZE_BYTES;
  const uint32_t BUF_SIZE_BYTES = 128;
  const uint32_t BUF_SIZE_WORDS = BUF_SIZE_BYTES / sizeof(uint32_t);
  uint32_t buffer[BUF_SIZE_WORDS];
  for (uint32_t offset = 0; offset < size_bytes; offset += BUF_SIZE_BYTES) {
    flash_impl_read_sync(buffer, addr + offset, BUF_SIZE_BYTES);
    for (uint32_t i = 0; i < BUF_SIZE_WORDS; ++i) {
      if (buffer[i] != FLASH_RESET_WORD_VALUE) {
        return false;
      }
    }
  }
  return true;
}
#endif

static bool prv_blank_check_mmap(QSPIFlash *dev, uint32_t addr, bool is_subsector) {
  const uint32_t size_bytes = is_subsector ? SUBSECTOR_SIZE_BYTES : SECTOR_SIZE_BYTES;
  bool result = true;
  uint8_t instruction;
  uint8_t dummy_cycles;
  bool is_ddr;
  prv_get_fast_read_params(dev, &instruction, &dummy_cycles, &is_ddr);
  qspi_mmap_start(dev->qspi, instruction, addr, dummy_cycles, size_bytes, is_ddr);

  // Point the buffer at the QSPI region
  uint32_t const volatile * const buffer = (uint32_t *)(QSPI_MMAP_BASE_ADDRESS + addr);
  uint32_t size_words = size_bytes / sizeof(uint32_t);
  for (uint32_t i = 0; i < size_words; ++i) {
    if (buffer[i] != FLASH_RESET_WORD_VALUE) {
      result = false;
      break;
    }
  }

  // stop memory mapped mode
  qspi_mmap_stop(QSPI);
  return result;
}
status_t qspi_flash_blank_check(QSPIFlash *dev, uint32_t addr, bool is_subsector) {
  qspi_use(dev->qspi);
#if TARGET_QEMU
  // QEMU doesn't support memory-mapping the FLASH
  const bool result = prv_blank_check_poll(dev, addr, is_subsector);
#else
  const bool result = prv_blank_check_mmap(dev, addr, is_subsector);
#endif
  qspi_release(dev->qspi);
  return result ? S_TRUE : S_FALSE;
}

void qspi_flash_ll_set_register_bits(QSPIFlash *dev, uint8_t read_instruction,
                                     uint8_t write_instruction, uint8_t value, uint8_t mask) {
  // make sure we're not trying to set any bits not within the mask
  PBL_ASSERTN((value & mask) == value);

  qspi_use(dev->qspi);

  // first read the register
  uint8_t reg_value;
  prv_read_register(dev, read_instruction, &reg_value, 1);

  // set the desired bits
  reg_value = (reg_value & ~mask) | value;

  // enable writing and write the register value
  prv_write_cmd_no_addr(dev, dev->state->part->instructions.write_enable);
  qspi_indirect_write_no_addr(dev->qspi, write_instruction, &reg_value, 1);

  qspi_release(dev->qspi);
}

static bool prv_protection_is_enabled(QSPIFlash *dev) {
  uint8_t status;
  prv_read_register(dev, dev->state->part->instructions.read_protection_status, &status, 1);
  return (status & dev->state->part->block_lock.protection_enabled_mask);
}

status_t qspi_flash_write_protection_enable(QSPIFlash *dev) {
#if TARGET_QEMU
  return S_NO_ACTION_REQUIRED;
#endif
  qspi_use(dev->qspi);
  prv_write_enable(dev);
  const bool already_enabled = prv_protection_is_enabled(dev);
  if (already_enabled == false) {
    PBL_LOG(LOG_LEVEL_INFO, "Enabling flash protection");
    // Enable write protection
    prv_write_cmd_no_addr(dev, dev->state->part->instructions.write_protection_enable);

    // Poll busy status until done
    qspi_poll_bit(dev->qspi, dev->state->part->instructions.read_status,
                  dev->state->part->status_bit_masks.busy, false /* !set */, QSPI_NO_TIMEOUT);

  }
  qspi_release(dev->qspi);

  return (already_enabled) ? S_NO_ACTION_REQUIRED : S_SUCCESS;
}

status_t qspi_flash_lock_sector(QSPIFlash *dev, uint32_t addr) {
#if TARGET_QEMU
  return S_SUCCESS;
#endif
  qspi_use(dev->qspi);

  prv_write_enable(dev);

  // Lock or unlock the sector
  const uint8_t instruction = dev->state->part->instructions.block_lock;
  if (dev->state->part->block_lock.has_lock_data) {
    qspi_indirect_write(dev->qspi, instruction, addr, &dev->state->part->block_lock.lock_data, 1);
  } else {
    qspi_indirect_write(dev->qspi, instruction, addr, NULL, 0);
  }

  // Poll busy status until done
  qspi_poll_bit(dev->qspi, dev->state->part->instructions.read_status,
                dev->state->part->status_bit_masks.busy, false /* !set */, QSPI_NO_TIMEOUT);

  // Read lock status
  uint8_t status;
  qspi_indirect_read(dev->qspi, dev->state->part->instructions.block_lock_status,
                     addr, 0, &status, sizeof(status), false);

  qspi_release(dev->qspi);

  return (status == dev->state->part->block_lock.locked_check) ? S_SUCCESS : E_ERROR;
}

status_t qspi_flash_unlock_all(QSPIFlash *dev) {
#if TARGET_QEMU
  return S_SUCCESS;
#endif
  qspi_use(dev->qspi);
  prv_write_enable(dev);
  prv_write_cmd_no_addr(dev, dev->state->part->instructions.block_unlock_all);
  qspi_release(dev->qspi);
  return S_SUCCESS;
}

#if !RELEASE
#include "console/prompt.h"
#include "drivers/flash.h"
#include "kernel/pbl_malloc.h"
#include "system/profiler.h"
#include "util/size.h"

static bool prv_flash_read_verify(QSPIFlash *dev, int size, int offset) {
  bool success = true;
  char *buffer_dma_ptr = kernel_malloc_check(size + offset + 3);
  char *buffer_pol = kernel_malloc_check(size + 3);
  char *buffer_mmap = kernel_malloc_check(size + 3);

  char *buffer_dma = buffer_dma_ptr + offset;

  // The buffers need to be different, so when compared against each other we can make
  // sure the write functions wrote the same thing.
  memset(buffer_dma, 0xA5, size);
  memset(buffer_pol, 0xCC, size);
  memset(buffer_mmap, 0x33, size);

  profiler_start();
  prv_read_mmap(dev, 0, buffer_mmap, size);
  profiler_stop();
  uint32_t mmap_time = profiler_get_total_duration(true);

  profiler_start();
  uint8_t instruction;
  uint8_t dummy_cycles;
  bool is_ddr;
  prv_get_fast_read_params(dev, &instruction, &dummy_cycles, &is_ddr);
  qspi_indirect_read_dma(dev->qspi, instruction, 0, dummy_cycles, buffer_dma, size, is_ddr);
  profiler_stop();
  uint32_t dma_time = profiler_get_total_duration(true);

  profiler_start();
  qspi_indirect_read(dev->qspi, instruction, 0, dummy_cycles, buffer_pol, size, is_ddr);
  profiler_stop();
  uint32_t pol_time = profiler_get_total_duration(true);

  if (memcmp(buffer_dma, buffer_pol, size) != 0) {
    prompt_send_response("FAILURE: buffer_dma != buffer_pol");
    success = false;
  }
  if (memcmp(buffer_dma, buffer_mmap, size) != 0) {
    prompt_send_response("FAILURE: buffer_dma != buffer_mmap");
    success = false;
  }

  const int buf_size = 64;
  char buf[buf_size];
  prompt_send_response_fmt(buf, buf_size, "Size: %d DMA: %"PRIu32 " POL: %"PRIu32 " MMP: %"PRIu32,
                           size, dma_time, pol_time, mmap_time);


  kernel_free(buffer_dma_ptr);
  kernel_free(buffer_pol);
  kernel_free(buffer_mmap);

  return success;
}

struct FlashReadTestValues {
  int size;
  int offset;
};

const struct FlashReadTestValues FLASH_READ_TEST_TABLE[] = {
  { .size = 1024, .offset = 0 },
  { .size = 1025, .offset = 0 },
  { .size = 1026, .offset = 0 },
  { .size = 1027, .offset = 0 },
  { .size = 1024, .offset = 1 },
  { .size = 1025, .offset = 2 },
  { .size = 1026, .offset = 3 },
  { .size = 4,    .offset = 0 },
  { .size = 20,   .offset = 0 },
  { .size = 60,   .offset = 0 },
  { .size = 127,  .offset = 0 },
  { .size = 128,  .offset = 0 },
};


void command_flash_apicheck(const char *len_str) {
  QSPIFlash *dev = QSPI_FLASH;
  const int buf_size = 64;
  char buf[buf_size];
  int failures = 0;
  int passes = 0;

  profiler_init();

  prompt_send_response("Check whoami");
  if (!qspi_flash_check_whoami(dev)) {
    ++failures;
    prompt_send_response("ERROR: Who am I failed");
  } else {
    ++passes;
  }

  prompt_send_response("Enter low power mode");
  flash_impl_enter_low_power_mode();

  // WHOAMI should fail in low-power mode
  prompt_send_response("Check whoami, should fail in low power mode");
  if (qspi_flash_check_whoami(dev)) {
    ++failures;
    prompt_send_response("ERROR: Who am I failed");
  } else {
    ++passes;
  }

  prompt_send_response("Exit low power mode");
  flash_impl_exit_low_power_mode();

  prompt_send_response("Start flash_read_verify test");
  qspi_use(QSPI);

  const int final_size = atoi(len_str);

  // If size is 0 run through a pre-defined table
  if (final_size == 0) {
    for (unsigned int i = 0; i < ARRAY_LENGTH(FLASH_READ_TEST_TABLE); ++i) {
      bool result = prv_flash_read_verify(dev, FLASH_READ_TEST_TABLE[i].size,
                                          FLASH_READ_TEST_TABLE[i].offset);
      if (!result) {
        ++failures;
      } else {
        ++passes;
      }
    }

  } else {
    if (prv_flash_read_verify(dev, final_size, 3)) {
      ++passes;
    } else {
      ++failures;
      prompt_send_response("ERROR: flash_read_verify failed");
    }
  }

  qspi_release(QSPI);

  bool was_busy = false;

  // write a few bytes to the sector we're going to erase so it's not empty
  uint8_t dummy_data = 0x55;
  flash_write_bytes(&dummy_data, FLASH_REGION_FIRMWARE_SCRATCH_BEGIN, sizeof(dummy_data));
  profiler_start();
  status_t result = flash_impl_erase_sector_begin(FLASH_REGION_FIRMWARE_SCRATCH_BEGIN);
  flash_impl_get_erase_status();
  if (result == S_SUCCESS) {
    while (flash_impl_get_erase_status() == E_BUSY) {
      was_busy = true;
    }
  }
  profiler_stop();
  uint32_t duration = profiler_get_total_duration(true);
  prompt_send_response_fmt(buf, buf_size, "Erase took: %"PRIu32, duration);

  // Fash erases take at least ~100ms, if we're too short we probably didn't erase
  const uint32_t min_erase_time = 10000;
  if (result != S_SUCCESS) {
    ++failures;
     prompt_send_response_fmt(buf, buf_size,
                              "FAILURE: erase did not report success %"PRIi32, result);
  } else if (was_busy == false) {
    ++failures;
    prompt_send_response("FAILURE: Flash never became busy, but we should be busy for 300ms.");
    prompt_send_response("FAILURE: Flash probably never did an erase.");
  } else if (duration < min_erase_time) {
    ++failures;
    prompt_send_response("FAILURE: Flash erase completed way to quickly to have succeeded.");
  } else {
    ++passes;
  }

  // must call blank_check_poll by hand, otherwise we'll get the dma version
  profiler_start();
  qspi_use(QSPI);
  bool is_blank = qspi_flash_blank_check(QSPI_FLASH, FLASH_REGION_FIRMWARE_SCRATCH_BEGIN,
                                       SUBSECTOR_SIZE_BYTES);
  qspi_release(QSPI);
  profiler_stop();

  uint32_t blank = profiler_get_total_duration(true);
  prompt_send_response_fmt(buf, buf_size, "Sector blank check via read took: %"PRIu32, blank);
  if (is_blank != S_TRUE) {
    ++failures;
    prompt_send_response("FAILURE: sector not blank!?!");
  } else {
    ++passes;
  }

  profiler_start();
  is_blank = flash_impl_blank_check_subsector(FLASH_REGION_FIRMWARE_SCRATCH_BEGIN);
  profiler_stop();

  blank = profiler_get_total_duration(true);
  prompt_send_response_fmt(buf, buf_size, "Subsector blank check via read took: %"PRIu32,
                           blank);
  if (is_blank != S_TRUE) {
    ++failures;
    prompt_send_response("FAILURE: sector not blank!?!");
  } else {
    ++passes;
  }

  if (failures == 0) {
    prompt_send_response_fmt(buf, buf_size, "SUCCESS: run %d tests and all passed", passes);
  }
  else {
    prompt_send_response_fmt(buf, buf_size, "FAILED: run %d tests and %d failed",
                             passes + failures,
                             failures);
  }
}

#endif

#if RECOVERY_FW
#include "console/prompt.h"
#include "drivers/flash.h"

#define SIGNAL_TEST_MAGIC_PATTERN (0xA5)
#define TEST_BUFFER_SIZE (1024)
static uint8_t s_test_buffer[TEST_BUFFER_SIZE];
static const uint32_t s_test_addr = FLASH_REGION_FIRMWARE_SCRATCH_END - SECTOR_SIZE_BYTES;
static bool s_signal_test_initialized;

void command_flash_signal_test_init(void) {
  // just test one sector, which is probably less than the size of the region

  // erase the sector
  flash_erase_sector_blocking(s_test_addr);

  // set the contents of the sector such that we will end up reading alternating 1s and 0s
  memset(s_test_buffer, SIGNAL_TEST_MAGIC_PATTERN, sizeof(s_test_buffer));
  flash_write_bytes(s_test_buffer, s_test_addr, sizeof(s_test_buffer));

  QSPIFlash *dev = QSPI_FLASH;
  // Ensure DDR is disabled for write check
  prv_set_fast_read_ddr_enabled(dev, false);
  uint8_t instruction;
  uint8_t dummy_cycles;
  bool is_ddr;
  prv_get_fast_read_params(dev, &instruction, &dummy_cycles, &is_ddr);
  PBL_ASSERTN(!is_ddr);

  qspi_use(QSPI);
  qspi_indirect_read(dev->qspi, instruction, s_test_addr, dummy_cycles, s_test_buffer,
                     sizeof(s_test_buffer), is_ddr);

  prv_set_fast_read_ddr_enabled(dev, dev->default_fast_read_ddr_enabled);
  qspi_release(QSPI);

  bool success = true;
  for (uint32_t i = 0; i < sizeof(s_test_buffer); ++i) {
    if (s_test_buffer[i] != SIGNAL_TEST_MAGIC_PATTERN) {
      success = false;
      break;
    }
  }

  if (success) {
    prompt_send_response("Done!");
    s_signal_test_initialized = true;
  } else {
    prompt_send_response("ERROR: Data read (SDR mode) did not match data written!");
  }
}

void command_flash_signal_test_run(void) {
  if (!s_signal_test_initialized) {
    prompt_send_response("ERROR: 'flash signal test init' must be run first!");
    return;
  }

  QSPIFlash *dev = QSPI_FLASH;
  qspi_use(QSPI);

  // set to DDR
  prv_set_fast_read_ddr_enabled(dev, true);

  // issue the read
  uint8_t instruction;
  uint8_t dummy_cycles;
  bool is_ddr;
  prv_get_fast_read_params(dev, &instruction, &dummy_cycles, &is_ddr);
  PBL_ASSERTN(is_ddr);
  qspi_indirect_read(dev->qspi, instruction, s_test_addr, dummy_cycles, s_test_buffer,
                     sizeof(s_test_buffer), is_ddr);

  bool success = true;
  for (uint32_t i = 0; i < sizeof(s_test_buffer); ++i) {
    if (s_test_buffer[i] != SIGNAL_TEST_MAGIC_PATTERN) {
      success = false;
      break;
    }
  }

  // set back to default mode
  prv_set_fast_read_ddr_enabled(dev, dev->default_fast_read_ddr_enabled);
  qspi_release(QSPI);

  if (success) {
    prompt_send_response("Ok");
  } else {
    prompt_send_response("ERROR: Read value didn't match!");
  }
}
#endif
