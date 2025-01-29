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

#include "drivers/qspi.h"
#include "drivers/qspi_definitions.h"

#include "board/board.h"
#include "drivers/dma.h"
#include "drivers/flash/flash_impl.h"
#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "kernel/util/delay.h"
#include "kernel/util/stop.h"
#include "mcu/cache.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"

#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#include "FreeRTOS.h"
#include "semphr.h"

//! Address value which signifies no address being sent
#define QSPI_ADDR_NO_ADDR (UINT32_MAX)
//! Word size for DMA reads
#define QSPI_DMA_READ_WORD_SIZE (4)


void qspi_init(QSPIPort *dev, uint32_t flash_size) {
  // Init the DMA semaphore, used for read
  dev->state->dma_semaphore = xSemaphoreCreateBinary();
  dma_request_init(dev->dma);

  // init GPIOs
  gpio_af_init(&dev->cs_gpio, GPIO_OType_PP, GPIO_Speed_100MHz, GPIO_PuPd_NOPULL);
  gpio_af_init(&dev->clk_gpio, GPIO_OType_PP, GPIO_Speed_100MHz, GPIO_PuPd_NOPULL);
  for (int i = 0; i < QSPI_NUM_DATA_PINS; i++) {
    gpio_af_init(&dev->data_gpio[i], GPIO_OType_PP, GPIO_Speed_100MHz, GPIO_PuPd_NOPULL);
  }

  // calculate the prescaler
  RCC_ClocksTypeDef RCC_ClocksStatus;
  RCC_GetClocksFreq(&RCC_ClocksStatus);
  int prescaler = RCC_ClocksStatus.HCLK_Frequency / dev->clock_speed_hz;
  if ((RCC_ClocksStatus.HCLK_Frequency / prescaler) > dev->clock_speed_hz) {
    // The desired prescaler is not an integer, so we'll round up so that the clock is never
    // faster than the desired frequency.
    prescaler++;
  }

  // enable clock while we initialize QSPI
  qspi_use(dev);

  // round the flash size up to the nearest power of 2 and calculate QSPI_FSize
  uint32_t fsize_value = ceil_log_two(flash_size) - 1;
  PBL_ASSERTN(flash_size == (uint32_t)1 << ceil_log_two(flash_size));

  // Init QSPI peripheral
  QSPI_InitTypeDef qspi_config;
  QSPI_StructInit(&qspi_config);
  qspi_config.QSPI_SShift = QSPI_SShift_HalfCycleShift;
  // QSPI clock = AHB / (1 + QSPI_Prescaler)
  qspi_config.QSPI_Prescaler = prescaler - 1;
  qspi_config.QSPI_CKMode = QSPI_CKMode_Mode0;
  qspi_config.QSPI_CSHTime = QSPI_CSHTime_1Cycle;
  qspi_config.QSPI_FSize = fsize_value;
  qspi_config.QSPI_FSelect = QSPI_FSelect_1;
  qspi_config.QSPI_DFlash = QSPI_DFlash_Disable;
  QSPI_Init(&qspi_config);
  QSPI_Cmd(ENABLE);

  qspi_release(dev);
}

void qspi_use(QSPIPort *dev) {
  if (dev->state->use_count++ == 0) {
    periph_config_enable(QUADSPI, dev->clock_ctrl);
  }
}

void qspi_release(QSPIPort *dev) {
  PBL_ASSERTN(dev->state->use_count > 0);
  if (--dev->state->use_count == 0) {
    periph_config_disable(QUADSPI, dev->clock_ctrl);
  }
}

static void prv_set_num_data_bytes(uint32_t length) {
  // From the docs: QSPI_DataLength: Number of data to be retrieved, value+1.
  // so 0 is 1 byte, so we substract 1 from the length. -1 is read the entire flash length.
  PBL_ASSERTN(length > 0);

  QSPI_SetDataLength(length - 1);
}

#if DEBUG_QSPI_WAITS
#define QSPI_WAIT_TIME (100000)

// These are a bit dangerous on long erase commands, but they can also be very useful to find out
// why the QSPI driver is locking up when doing development

static void prv_wait_for_transfer_complete(void) {
  int i = 0;
  while (QSPI_GetFlagStatus(QSPI_FLAG_TC) == RESET) {
    if (++i > QSPI_WAIT_TIME) {
      break;
    }
  }
  PBL_ASSERT(i < QSPI_WAIT_TIME, "Waited too long for the QSPI transfer to complete");
}

static void prv_wait_for_not_busy(void) {
  int i = 0;
  while (QSPI_GetFlagStatus(QSPI_FLAG_BUSY) != RESET) {
    if (++i > QSPI_WAIT_TIME) {
      break;
    }
  }
  PBL_ASSERT(i < QSPI_WAIT_TIME, "Waited too long for the QSPI to become not busy");
}

#else

static void prv_wait_for_transfer_complete(void) {
  while (QSPI_GetFlagStatus(QSPI_FLAG_TC) == RESET) { }
}

static void prv_wait_for_not_busy(void) {
  while (QSPI_GetFlagStatus(QSPI_FLAG_BUSY) != RESET) { }
}

#endif

static void prv_read_bytes(uint8_t *buffer, size_t buffer_size) {
  for (size_t i = 0; i < buffer_size; ++i) {
    buffer[i] = QSPI_ReceiveData8();
  }
}

static void prv_set_ddr_enabled(bool enabled) {
  PBL_ASSERTN(!QSPI_GetFlagStatus(QSPI_FLAG_BUSY));
  if (enabled) {
    QUADSPI->CR &= ~QUADSPI_CR_SSHIFT;
  } else {
    QUADSPI->CR |= QUADSPI_CR_SSHIFT;
  }
}

// CCR register Bits from LSB to MSB
// INSTRUCTION[7:0]: Instruction
// IMODE[1:0]: Instruction Mode
// ADMODE[1:0]: Address Mode
// ADSIZE[1:0]: Address Size
// ABMODE[1:0]: Alternate Bytes Mode
// ABSIZE[1:0]: Instruction Mode
// DCYC[4:0]: Dummy Cycles
// RESERVED
// DMODE[1:0]: Data Mode
// FMODE[1:0]: Functional Mode
// SIOO: Send Instruction Only Once Mode
// RESERVED
// DHHC: Delay Half Hclk Cycle
// DDRM: Double Data Rate Mode

//! Mask to clear out the valid bits while leaving the reserved bits untouched
#define QSPI_CCR_CLEAR_MASK (~(QUADSPI_CCR_INSTRUCTION | \
                               QUADSPI_CCR_IMODE | \
                               QUADSPI_CCR_ADMODE | \
                               QUADSPI_CCR_ADSIZE | \
                               QUADSPI_CCR_ABMODE | \
                               QUADSPI_CCR_ABSIZE | \
                               QUADSPI_CCR_DCYC | \
                               QUADSPI_CCR_DMODE | \
                               QUADSPI_CCR_FMODE | \
                               QUADSPI_CCR_SIOO | \
                               QUADSPI_CCR_DHHC | \
                               QUADSPI_CCR_DDRM))

static void prv_set_comm_config(uint32_t modes_bitset, uint32_t dummy_cycles) {
  uint32_t ccr = QUADSPI->CCR;
  ccr &= QSPI_CCR_CLEAR_MASK;
  ccr |= modes_bitset;
  ccr |= (dummy_cycles << 18);
  QUADSPI->CCR = ccr;
}

static bool prv_dma_irq_handler(DMARequest *request, void *context) {
  QSPIPort *dev = context;
  QSPI_DMACmd(DISABLE);

  signed portBASE_TYPE was_higher_priority_task_woken = pdFALSE;
  xSemaphoreGiveFromISR(dev->state->dma_semaphore, &was_higher_priority_task_woken);
  return was_higher_priority_task_woken != pdFALSE;
}

static void prv_config_indirect_read(QSPIPort *dev, uint8_t instruction, uint32_t addr,
                                     uint8_t dummy_cycles, bool is_ddr) {
  prv_set_ddr_enabled(is_ddr);

  uint32_t modes_bitset = 0;
  modes_bitset |= is_ddr ? QSPI_ComConfig_DDRMode_Enable : QSPI_ComConfig_DDRMode_Disable;
  modes_bitset |= is_ddr ? QSPI_ComConfig_DHHC_Enable : QSPI_ComConfig_DHHC_Disable;
  modes_bitset |= QSPI_ComConfig_FMode_Indirect_Read;
  modes_bitset |= QSPI_ComConfig_DMode_4Line;
  modes_bitset |= QSPI_ComConfig_IMode_4Line;
  modes_bitset |= instruction;
  if (addr != QSPI_ADDR_NO_ADDR) {
    modes_bitset |= QSPI_ComConfig_ADMode_4Line;
    modes_bitset |= QSPI_ComConfig_ADSize_24bit;
  }
  prv_set_comm_config(modes_bitset, dummy_cycles);

  if (addr != QSPI_ADDR_NO_ADDR) {
    QSPI_SetAddress(addr);
  }
}

static void prv_indirect_read(QSPIPort *dev, uint8_t instruction, uint32_t addr,
                              uint8_t dummy_cycles, void *buffer, uint32_t length, bool is_ddr) {
  prv_set_num_data_bytes(length);

  prv_config_indirect_read(dev, instruction, addr, dummy_cycles, is_ddr);

  prv_read_bytes(buffer, length);

  QSPI_ClearFlag(QSPI_FLAG_TC);
  prv_wait_for_not_busy();
}

void qspi_indirect_read_no_addr(QSPIPort *dev, uint8_t instruction, uint8_t dummy_cycles,
                                void *buffer, uint32_t length, bool is_ddr) {
  prv_indirect_read(dev, instruction, QSPI_ADDR_NO_ADDR, dummy_cycles, buffer, length, is_ddr);
}
void qspi_indirect_read(QSPIPort *dev, uint8_t instruction, uint32_t addr, uint8_t dummy_cycles,
                        void *buffer, uint32_t length, bool is_ddr) {
  prv_indirect_read(dev, instruction, addr, dummy_cycles, buffer, length, is_ddr);
}

void qspi_indirect_read_dma(QSPIPort *dev, uint8_t instruction, uint32_t start_addr,
                            uint8_t dummy_cycles, void *buffer, uint32_t length, bool is_ddr) {
  // DMA reads are most efficient when doing 32bits at a time.  The QSPI bus runs at 100Mhz
  // and we need to be efficient in handling the data to use it to its full capability.
  //
  // So this function is broken into 3 parts:
  // 1. Do reads 1 byte at a time until buffer_ptr is word-aligned
  // 2. Do 32-bit DMA transfers for as much as possible
  // 3. Do reads 1 bytes at a time to deal with non-aligned accesses at the end

  const uint32_t word_mask = dcache_alignment_mask_minimum(QSPI_DMA_READ_WORD_SIZE);
  const uintptr_t buffer_address = (uintptr_t)buffer;

  const uintptr_t last_address = buffer_address + length;
  const uintptr_t last_address_aligned = last_address & (~word_mask);

  const uintptr_t start_address_aligned = ((buffer_address + word_mask) & (~word_mask));

  uint32_t leading_bytes = start_address_aligned - buffer_address;
  uint32_t trailing_bytes = last_address - last_address_aligned;

  uint32_t dma_size = last_address_aligned - start_address_aligned;

  if (last_address_aligned < start_address_aligned) {
    dma_size = 0;
    leading_bytes = length;
    trailing_bytes = 0;
  }

  prv_set_num_data_bytes(length);

  prv_config_indirect_read(dev, instruction, start_addr, dummy_cycles, is_ddr);

  if (leading_bytes > 0) {
    prv_read_bytes(buffer, leading_bytes);
  }

  if (dma_size > 0) {
    // Do 4 bytes at a time for DMA
    QSPI_SetFIFOThreshold(QSPI_DMA_READ_WORD_SIZE);

    QSPI_DMACmd(ENABLE);
    stop_mode_disable(InhibitorFlash);
    dma_request_start_direct(dev->dma, (void *)start_address_aligned, (void *)&QUADSPI->DR,
                             dma_size, prv_dma_irq_handler, (void *)dev);

    xSemaphoreTake(dev->state->dma_semaphore, portMAX_DELAY);
    stop_mode_enable(InhibitorFlash);
  }

  if (trailing_bytes > 0) {
    prv_read_bytes((void *)last_address_aligned, trailing_bytes);
  }
}

static void prv_indirect_write(QSPIPort *dev, uint8_t instruction, uint32_t addr,
                               const void *buffer, uint32_t length) {
  if (length) {
    PBL_ASSERTN(buffer);
    prv_set_num_data_bytes(length);
  } else {
    PBL_ASSERTN(!buffer);
  }

  prv_set_ddr_enabled(false);

  uint32_t modes_bitset = 0;
  modes_bitset |= QSPI_ComConfig_FMode_Indirect_Write;
  modes_bitset |= QSPI_ComConfig_IMode_4Line;
  modes_bitset |= instruction;
  if (addr != QSPI_ADDR_NO_ADDR) {
    modes_bitset |= QSPI_ComConfig_ADMode_4Line;
    modes_bitset |= QSPI_ComConfig_ADSize_24bit;
  }
  if (length) {
    modes_bitset |= QSPI_ComConfig_DMode_4Line;
  }
  prv_set_comm_config(modes_bitset, 0);

  if (addr != QSPI_ADDR_NO_ADDR) {
    QSPI_SetAddress(addr);
  }

  const uint8_t *read_ptr = buffer;
  for (uint32_t i = 0; i < length; ++i) {
    // Note: this will stall the CPU when the FIFO gets full while data is being sent.
    // For performance reasons, we should replace it with DMA in the future
    // PBL-28805
    QSPI_SendData8(read_ptr[i]);
  }

  prv_wait_for_transfer_complete();
  QSPI_ClearFlag(QSPI_FLAG_TC);
  prv_wait_for_not_busy();
}

void qspi_indirect_write_no_addr(QSPIPort *dev, uint8_t instruction, const void *buffer,
                                 uint32_t length) {
  prv_indirect_write(dev, instruction, QSPI_ADDR_NO_ADDR, buffer, length);
}

void qspi_indirect_write(QSPIPort *dev, uint8_t instruction, uint32_t addr, const void *buffer,
                         uint32_t length) {
  prv_indirect_write(dev, instruction, addr, buffer, length);
}

void qspi_indirect_write_no_addr_1line(QSPIPort *dev, uint8_t instruction) {
  prv_set_ddr_enabled(false);

  uint32_t modes_bitset = 0;
  modes_bitset |= QSPI_ComConfig_FMode_Indirect_Write;
  modes_bitset |= QSPI_ComConfig_IMode_1Line;
  modes_bitset |= instruction;
  prv_set_comm_config(modes_bitset, 0);

  prv_wait_for_transfer_complete();
  QSPI_ClearFlag(QSPI_FLAG_TC);
  prv_wait_for_not_busy();
}

bool qspi_poll_bit(QSPIPort *dev, uint8_t instruction, uint8_t bit_mask, bool should_be_set,
                   uint32_t timeout_us) {
  prv_set_num_data_bytes(1);

  // Set autopolling on the register
  QSPI_AutoPollingMode_SetInterval(dev->auto_polling_interval);
  QSPI_AutoPollingMode_Config(should_be_set ? bit_mask : 0, bit_mask, QSPI_PMM_AND);
  QSPI_AutoPollingModeStopCmd(ENABLE);

  prv_set_ddr_enabled(false);

  // Prepare transaction
  uint32_t modes_bitset = 0;
  modes_bitset |= QSPI_ComConfig_FMode_Auto_Polling;
  modes_bitset |= QSPI_ComConfig_DMode_4Line;
  modes_bitset |= QSPI_ComConfig_IMode_4Line;
  modes_bitset |= instruction;
  prv_set_comm_config(modes_bitset, 0);

  uint32_t loops = 0;
  while (QSPI_GetFlagStatus(QSPI_FLAG_SM) == RESET) {
    if ((timeout_us != QSPI_NO_TIMEOUT) && (++loops > timeout_us)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Timeout waiting for a bit!?!?");
      return false;
    }
    delay_us(1);
  }

  // stop polling mode
  QSPI_AbortRequest();
  prv_wait_for_not_busy();

  return true;
}

void qspi_mmap_start(QSPIPort *dev, uint8_t instruction, uint32_t addr, uint8_t dummy_cycles,
                     uint32_t length, bool is_ddr) {
  dcache_invalidate((void *)(uintptr_t)(QSPI_MMAP_BASE_ADDRESS + addr), length);

  prv_set_ddr_enabled(is_ddr);

  uint32_t modes_bitset = 0;
  modes_bitset |= is_ddr ? QSPI_ComConfig_DDRMode_Enable : QSPI_ComConfig_DDRMode_Disable;
  modes_bitset |= is_ddr ? QSPI_ComConfig_DHHC_Enable : QSPI_ComConfig_DHHC_Disable;
  modes_bitset |= QSPI_ComConfig_FMode_Memory_Mapped;
  modes_bitset |= QSPI_ComConfig_DMode_4Line;
  modes_bitset |= QSPI_ComConfig_IMode_4Line;
  modes_bitset |= QSPI_ComConfig_ADMode_4Line;
  modes_bitset |= QSPI_ComConfig_ADSize_24bit;
  modes_bitset |= instruction;

  prv_set_comm_config(modes_bitset, dummy_cycles);

  // The QSPI will prefetch bytes as long as nCS is low. This causes the flash part to draw a lot
  // more power (10mA vs 10uA in the case of Silk). Set the timeout such that the prefetch will
  // stop after 10 clock cycles of inactivity.
  QSPI_MemoryMappedMode_SetTimeout(10);

  // HACK ALERT: It seems like the MCU may send the wrong address for the first MMAP after certain
  // flash operations (we have seen it with an indirect read). To work around this, kick off one
  // read sufficiently far away from the area we want to read. This seems to reset the QSPI
  // controller back into a good state. This workaround is a little wasteful as it kicks off a 32
  // byte flash read but at 50MHz that should only take ~1.5us:
  // ((1byte +3byteaddr + 32bytes data) * 2 clocks/byte + 4 dummy_clocks) / 50Mhz = 1.52us

  volatile uint8_t *qspi_wa_addr = (uint8_t *)(QSPI_MMAP_BASE_ADDRESS + ((addr > 128) ? 0 : 256));
  dcache_invalidate((void *)qspi_wa_addr, 1);
  (void)*qspi_wa_addr;
}

void qspi_mmap_stop(QSPIPort *dev) {
  QSPI_AbortRequest();
  prv_wait_for_not_busy();
}
