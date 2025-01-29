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

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "board/board.h"
#include "drivers/dma.h"
#include "drivers/flash.h"
#include "drivers/flash/micron_n25q/flash_private.h"
#include "kernel/util/stop.h"
#include "process_management/worker_manager.h"
#include "services/common/analytics/analytics.h"
#include "os/mutex.h"
#include "kernel/util/delay.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"

#include "FreeRTOS.h"
#include "semphr.h"

/*
 * Each peripheral has a dma channel / stream it works with
 * c.f. section 9.3.3 in stm32 reference manual
 */
/* RX DMA */
static DMA_Stream_TypeDef* FLASH_DMA_STREAM = DMA2_Stream0;
static const uint32_t FLASH_DMA_CHANNEL = DMA_Channel_3;
static const uint32_t FLASH_DMA_IRQn = DMA2_Stream0_IRQn;
static const uint32_t FLASH_DATA_REGISTER_ADDR = (uint32_t)&(SPI1->DR);
/* TX DMA */
static DMA_Stream_TypeDef* FLASH_TX_DMA_STREAM = DMA2_Stream3;
static const uint32_t FLASH_TX_DMA_CHANNEL = DMA_Channel_3;

static uint32_t analytics_read_count;
static uint32_t analytics_read_bytes_count;
static uint32_t analytics_write_bytes_count;

void analytics_external_collect_system_flash_statistics(void) {
  // TODO: Add support back to tintin
}

void analytics_external_collect_app_flash_read_stats(void) {
  analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_COUNT, analytics_read_count, AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_BYTES_COUNT, analytics_read_bytes_count, AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_FLASH_WRITE_BYTES_COUNT, analytics_write_bytes_count, AnalyticsClient_App);

  // The overhead cost of tracking whether each flash read was due to the foreground
  // or background app is large, so the best we can do is to attribute to both of them
  if (worker_manager_get_current_worker_md() != NULL) {
    analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_COUNT, analytics_read_count, AnalyticsClient_Worker);
    analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_BYTES_COUNT, analytics_read_bytes_count, AnalyticsClient_Worker);
    analytics_set(ANALYTICS_APP_METRIC_FLASH_WRITE_BYTES_COUNT, analytics_write_bytes_count, AnalyticsClient_Worker);
  }

  analytics_read_count = 0;
  analytics_read_bytes_count = 0;
  analytics_write_bytes_count = 0;
}

struct FlashState {
  bool enabled;
  bool sleep_when_idle;
  bool deep_sleep;
  PebbleMutex * mutex;
  SemaphoreHandle_t dma_semaphore;
} s_flash_state;

static void flash_deep_sleep_enter(void);
static void flash_deep_sleep_exit(void);


void assert_usable_state(void) {
  PBL_ASSERTN(s_flash_state.mutex != 0);
}

static void enable_flash_dma_clock(void) {
  // TINTINHACK: Rather than update this file to use the new DMA driver, just rely on the fact that
  // this is the only consumer of DMA2.
  periph_config_enable(DMA2, RCC_AHB1Periph_DMA2);
}

static void disable_flash_dma_clock(void) {
  // TINTINHACK: Rather than update this file to use the new DMA driver, just rely on the fact that
  // this is the only consumer of DMA2.
  periph_config_disable(DMA2, RCC_AHB1Periph_DMA2);
}

static void setup_dma_read(uint8_t *buffer, int size) {
  DMA_InitTypeDef dma_config;

  DMA_DeInit(FLASH_DMA_STREAM);
  DMA_DeInit(FLASH_TX_DMA_STREAM);

  /* RX DMA config */
  DMA_StructInit(&dma_config);
  dma_config.DMA_Channel = FLASH_DMA_CHANNEL;
  dma_config.DMA_DIR = DMA_DIR_PeripheralToMemory;
  dma_config.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
  dma_config.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
  dma_config.DMA_Mode = DMA_Mode_Normal;
  dma_config.DMA_PeripheralBaseAddr = FLASH_DATA_REGISTER_ADDR;
  dma_config.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
  dma_config.DMA_MemoryInc = DMA_MemoryInc_Enable;
  dma_config.DMA_Priority = DMA_Priority_High;
  dma_config.DMA_FIFOMode = DMA_FIFOMode_Disable;
  dma_config.DMA_MemoryBurst = DMA_MemoryBurst_Single;
  dma_config.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
  dma_config.DMA_Memory0BaseAddr = (uint32_t)buffer;
  dma_config.DMA_BufferSize = size;

  DMA_Init(FLASH_DMA_STREAM, &dma_config);

  /* TX DMA config */
  dma_config.DMA_Channel = FLASH_TX_DMA_CHANNEL;
  dma_config.DMA_DIR = DMA_DIR_MemoryToPeripheral;
  dma_config.DMA_PeripheralBaseAddr = FLASH_DATA_REGISTER_ADDR;
  dma_config.DMA_MemoryInc = DMA_MemoryInc_Disable;
  dma_config.DMA_Priority = DMA_Priority_High;
  dma_config.DMA_Memory0BaseAddr = (uint32_t)&FLASH_CMD_DUMMY;
  dma_config.DMA_BufferSize = size;

  DMA_Init(FLASH_TX_DMA_STREAM, &dma_config);

  /* Setup DMA interrupts */
  NVIC_InitTypeDef nvic_config;
  nvic_config.NVIC_IRQChannel = FLASH_DMA_IRQn;
  nvic_config.NVIC_IRQChannelPreemptionPriority = 0x0f;
  nvic_config.NVIC_IRQChannelSubPriority = 0x00;
  nvic_config.NVIC_IRQChannelCmd = ENABLE;
  NVIC_Init(&nvic_config);

  DMA_ITConfig(FLASH_DMA_STREAM, DMA_IT_TC, ENABLE);

  // enable the DMA stream to start the transfer
  SPI_I2S_DMACmd(FLASH_SPI, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, ENABLE);
}

static void do_dma_transfer(void) {
  xSemaphoreTake(s_flash_state.dma_semaphore, portMAX_DELAY);
  stop_mode_disable(InhibitorFlash);
  DMA_Cmd(FLASH_DMA_STREAM, ENABLE);
  DMA_Cmd(FLASH_TX_DMA_STREAM, ENABLE);
  xSemaphoreTake(s_flash_state.dma_semaphore, portMAX_DELAY);
  stop_mode_enable(InhibitorFlash);
  xSemaphoreGive(s_flash_state.dma_semaphore);
}

void DMA2_Stream0_IRQHandler(void) {
  if (DMA_GetITStatus(FLASH_DMA_STREAM, DMA_IT_TCIF3)) {
    DMA_ClearITPendingBit(FLASH_DMA_STREAM, DMA_IT_TCIF3);
    NVIC_DisableIRQ(FLASH_DMA_IRQn);
    signed portBASE_TYPE was_higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flash_state.dma_semaphore, &was_higher_priority_task_woken);
    portEND_SWITCHING_ISR(was_higher_priority_task_woken);
    return; //notreached
  }
}

static void flash_deep_sleep_enter(void) {
  assert_usable_state();

  if (!s_flash_state.deep_sleep) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_DEEP_SLEEP);
    flash_end_cmd();

    // guarantee we have actually transitioned to deep sleep
    delay_us(5);
    s_flash_state.deep_sleep = true;
  }
}

static void flash_deep_sleep_exit(void) {
  assert_usable_state();

  if (s_flash_state.deep_sleep) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_WAKE);
    flash_end_cmd();

    // wait a sufficient amount of time to enter standby mode
    // It appears violating these timing conditions can lead to
    // random bit corruptions on flash writes!
    delay_us(100);
    s_flash_state.deep_sleep = false;
  }
}

void handle_sleep_when_idle_begin(void) {
  if (s_flash_state.sleep_when_idle) {
    flash_deep_sleep_exit();
  }
}

void flash_power_down_for_stop_mode(void) {
  if (s_flash_state.sleep_when_idle) {
    if (s_flash_state.enabled) {
      enable_flash_spi_clock();
      flash_deep_sleep_enter();
      disable_flash_spi_clock();
    }
  }
}

void flash_power_up_after_stop_mode(void) {
  // no need here as this platform doesn't support memory-mappable flash
}

uint32_t flash_get_sector_base_address(uint32_t addr) {
  return addr & ~(SECTOR_SIZE_BYTES - 1);
}

// This simply issues a command to read a specific register
static uint8_t prv_flash_get_register(uint8_t command) {
  flash_start_cmd();
  flash_send_and_receive_byte(command);
  uint8_t reg = flash_read_next_byte();
  flash_end_cmd();
  return reg;
}

// This will read the flag status register and check it for the SectorLockStatus flag
void prv_check_protection_flag() {
  uint8_t flag_status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);
  // assert if we found the flag to be enabled
  PBL_ASSERTN(!(flag_status_register & N25QFlagStatusBit_SectorLockStatus));
}

// This will clear the protection flag error from a previous error.
// We call this because the error bits persist across reboots
static void prv_clear_flag_status_register(void) {
  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_CLEAR_FLAG_STATUS_REG);
  flash_end_cmd();
}

/**
 * Write up to 1 page (256B) of data to flash. start_addr DOES NOT
 * need to be paged aligned. When writing into the middle of a page
 * (addr & 0xFFF > 0), overrunning the length of the page will cause
 * the write to "wrap around" and will modify (i.e. corrupt) data
 * stored before the starting address within the page.
 *
 */
static void flash_write_page(const uint8_t* buffer, uint32_t start_addr, uint16_t buffer_size) {
  // Ensure that we're not trying to write more data than a single page (256 bytes)
  PBL_ASSERTN(buffer_size <= FLASH_PAGE_SIZE);
  PBL_ASSERTN(buffer_size);
  mutex_assert_held_by_curr_task(s_flash_state.mutex, true /* is_held */);

  // Writing a zero-length buffer is a no-op.
  if (buffer_size < 1) {
    return;
  }

  flash_write_enable();

  flash_start_cmd();

  flash_send_and_receive_byte(FLASH_CMD_PAGE_PROGRAM);
  flash_send_24b_address(start_addr);

  while (buffer_size--) {
    flash_send_and_receive_byte(*buffer);
    buffer++;
  }

  flash_end_cmd();
  flash_wait_for_write();

  prv_check_protection_flag();
}

// Public interface
// From here on down, make sure you're taking the s_flash_state.mutex before doing anything to the SPI peripheral.

void flash_enable_write_protection(void) {
  return;
}

void flash_lock(void) {
  mutex_lock(s_flash_state.mutex);
}

void flash_unlock(void) {
  mutex_unlock(s_flash_state.mutex);
}

bool flash_is_enabled(void) {
  return (s_flash_state.enabled);
}

void flash_init(void) {
  if (s_flash_state.mutex != 0) {
    return; // Already initialized.
  }

  s_flash_state.mutex = mutex_create();
  vSemaphoreCreateBinary(s_flash_state.dma_semaphore);
  flash_lock();

  enable_flash_spi_clock();

  flash_start();

  s_flash_state.enabled = true;
  s_flash_state.sleep_when_idle = false;

  // Assume that last time we shut down we were asleep. Come back out.
  s_flash_state.deep_sleep = true;
  flash_deep_sleep_exit();

  prv_clear_flag_status_register();

  disable_flash_spi_clock();
  flash_unlock();

  flash_whoami();

  PBL_LOG_VERBOSE("Detected SPI Flash Size: %u bytes", flash_get_size());
}

void flash_stop(void) {
  if (s_flash_state.mutex == NULL) {
    return;
  }

  flash_lock();
  s_flash_state.enabled = false;
  flash_unlock();
}

void flash_read_bytes(uint8_t* buffer, uint32_t start_addr, uint32_t buffer_size) {
  if (!buffer_size) {
    return;
  }

  assert_usable_state();

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  analytics_read_count++;
  analytics_read_bytes_count += buffer_size;
  power_tracking_start(PowerSystemFlashRead);

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  flash_wait_for_write();

  flash_start_cmd();

  flash_send_and_receive_byte(FLASH_CMD_READ);
  flash_send_24b_address(start_addr);

  // There is delay associated with setting up the stm32 dma, using FreeRTOS
  // semaphores, handling ISRs, etc. Thus for short reads, the cost of using
  // DMA is far more expensive than the read being performed. Reads greater
  // than 34 was empirically determined to be the point at which using the DMA
  // engine is advantageous
#if !defined(TARGET_QEMU)
  const uint32_t num_reads_dma_cutoff = 34;
#else
  // We are disabling DMA reads when running under QEMU for now because they are not reliable.
  const uint32_t num_reads_dma_cutoff = buffer_size + 1;
#endif
  if (buffer_size < num_reads_dma_cutoff) {
    while (buffer_size--) {
      *buffer = flash_read_next_byte();
      buffer++;
    }
  } else {
    enable_flash_dma_clock();
    setup_dma_read(buffer, buffer_size);
    do_dma_transfer();
    disable_flash_dma_clock();
  }

  flash_end_cmd();

  disable_flash_spi_clock();

  power_tracking_stop(PowerSystemFlashRead);
  flash_unlock();
}

void flash_write_bytes(const uint8_t* buffer, uint32_t start_addr, uint32_t buffer_size) {
  if (!buffer_size) {
    return;
  }

  PBL_ASSERTN((start_addr + buffer_size) <= BOARD_NOR_FLASH_SIZE);

  assert_usable_state();

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  analytics_write_bytes_count += buffer_size;
  power_tracking_start(PowerSystemFlashWrite);

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  uint32_t first_page_available_bytes = FLASH_PAGE_SIZE - (start_addr % FLASH_PAGE_SIZE);
  uint32_t bytes_to_write = MIN(buffer_size, first_page_available_bytes);

  if (first_page_available_bytes < FLASH_PAGE_SIZE) {
    PBL_LOG_VERBOSE("Address is not page-aligned; first write will be %"PRId32"B at address 0x%"PRIX32,
      first_page_available_bytes, start_addr);
  }

  while (bytes_to_write) {
    flash_write_page(buffer, start_addr, bytes_to_write);

    start_addr += bytes_to_write;
    buffer += bytes_to_write;
    buffer_size -= bytes_to_write;
    bytes_to_write = MIN(buffer_size, FLASH_PAGE_SIZE);
  }

  disable_flash_spi_clock();

  power_tracking_stop(PowerSystemFlashWrite);
  flash_unlock();
}

void flash_erase_subsector_blocking(uint32_t subsector_addr) {
  assert_usable_state();

  PBL_LOG(LOG_LEVEL_DEBUG, "Erasing subsector 0x%"PRIx32" (0x%"PRIx32" - 0x%"PRIx32")",
      subsector_addr,
      subsector_addr & SUBSECTOR_ADDR_MASK,
      (subsector_addr & SUBSECTOR_ADDR_MASK) + SUBSECTOR_SIZE_BYTES);

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  analytics_inc(ANALYTICS_APP_METRIC_FLASH_SUBSECTOR_ERASE_COUNT, AnalyticsClient_CurrentTask);
  power_tracking_start(PowerSystemFlashErase);

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  flash_write_enable();

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_SUBSECTOR);
  flash_send_24b_address(subsector_addr);
  flash_end_cmd();

  flash_wait_for_write();

  prv_check_protection_flag();

  disable_flash_spi_clock();

  power_tracking_stop(PowerSystemFlashErase);
  flash_unlock();
}

void flash_erase_sector_blocking(uint32_t sector_addr) {
  assert_usable_state();

  PBL_LOG(LOG_LEVEL_DEBUG, "Erasing sector 0x%"PRIx32" (0x%"PRIx32" - 0x%"PRIx32")",
          sector_addr,
          sector_addr & SECTOR_ADDR_MASK,
          (sector_addr & SECTOR_ADDR_MASK) + SECTOR_SIZE_BYTES);

  if (flash_sector_is_erased(sector_addr)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Sector %#"PRIx32" already erased", sector_addr);
    return;
  }

  flash_lock();

  if (!flash_is_enabled()) {
    flash_unlock();
    return;
  }

  power_tracking_start(PowerSystemFlashErase);

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  flash_write_enable();

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_SECTOR);
  flash_send_24b_address(sector_addr);
  flash_end_cmd();

  flash_wait_for_write();

  prv_check_protection_flag();

  disable_flash_spi_clock();

  power_tracking_stop(PowerSystemFlashErase);
  flash_unlock();
}

// It is dangerous to leave this built in by default.
#if 0
void flash_erase_bulk(void) { 
  assert_usable_state();

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  flash_prf_set_protection(false);

  power_tracking_start(PowerSystemFlashErase);

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  flash_write_enable();

  flash_start_cmd();
  flash_send_and_receive_byte(FLASH_CMD_ERASE_BULK);
  flash_end_cmd();

  flash_wait_for_write();

  flash_prf_set_protection(true);

  disable_flash_spi_clock();

  power_tracking_stop(PowerSystemFlashErase);
  flash_unlock();
}
#endif

void flash_sleep_when_idle(bool enable) {
  if (enable == s_flash_state.sleep_when_idle) {
    return;
  }

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  enable_flash_spi_clock();

  s_flash_state.sleep_when_idle = enable;

  if (enable) {
    if (!s_flash_state.deep_sleep) {
      flash_deep_sleep_enter();
    }
  } else {
    if (s_flash_state.deep_sleep) {
      flash_deep_sleep_exit();
    }
  }

  disable_flash_spi_clock();
  flash_unlock();
}

bool flash_get_sleep_when_idle(void) {
  bool result;
  flash_lock();
  result = s_flash_state.deep_sleep;
  flash_unlock();
  return result;
}

void debug_flash_dump_registers(void) {
#ifdef PBL_LOG_ENABLED
  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  uint8_t status_register = prv_flash_get_register(FLASH_CMD_READ_STATUS_REG);
  uint8_t lock_register = prv_flash_get_register(FLASH_CMD_READ_LOCK_REGISTER);
  uint8_t flag_status_register = prv_flash_get_register(FLASH_CMD_READ_FLAG_STATUS_REG);
  uint8_t nonvolatile_config_register =
      prv_flash_get_register(FLASH_CMD_READ_NONVOLATILE_CONFIG_REGISTER);
  uint8_t volatile_config_register =
      prv_flash_get_register(FLASH_CMD_READ_VOLATILE_CONFIG_REGISTER);

  disable_flash_spi_clock();
  flash_unlock();

  PBL_LOG(LOG_LEVEL_DEBUG, "Status Register: 0x%x", status_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Lock Register: 0x%x", lock_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Flag Status Register: 0x%x", flag_status_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Nonvolatile Configuration Register: 0x%x", nonvolatile_config_register);
  PBL_LOG(LOG_LEVEL_DEBUG, "Volatile Configuration Register: 0x%x", volatile_config_register);
#endif
}

bool flash_is_initialized(void) {
  return (s_flash_state.mutex != 0);
}

size_t flash_get_size(void) {
  uint32_t spi_flash_id = flash_whoami();
  if (!check_whoami(spi_flash_id)) {
    // Zero bytes is the best size to report if the flash is corrupted
    return 0;
  }

  // capacity_megabytes = 2^(capacity in whoami)
  uint32_t capacity = spi_flash_id & 0x000000FF;
  // get the capacity of the flash in bytes
  return 1 << capacity;
}

void flash_prf_set_protection(bool do_protect) {
  assert_usable_state();

  flash_lock();

  if (!s_flash_state.enabled) {
    flash_unlock();
    return;
  }

  enable_flash_spi_clock();
  handle_sleep_when_idle_begin();

  flash_write_enable();

  const uint32_t start_addr = FLASH_REGION_SAFE_FIRMWARE_BEGIN;
  const uint32_t end_addr = FLASH_REGION_SAFE_FIRMWARE_END;
  const uint8_t lock_bits = do_protect ? N25QLockBit_SectorWriteLock : 0;
  for (uint32_t addr = start_addr; addr < end_addr; addr += SECTOR_SIZE_BYTES) {
    flash_start_cmd();
    flash_send_and_receive_byte(FLASH_CMD_WRITE_LOCK_REGISTER);
    flash_send_24b_address(addr);
    flash_send_and_receive_byte(lock_bits);
    flash_end_cmd();
  }

  disable_flash_spi_clock();

  flash_unlock();
}

void flash_erase_sector(uint32_t sector_addr,
                        FlashOperationCompleteCb on_complete_cb,
                        void *context) {
  // TODO: implement nonblocking erase
  flash_erase_sector_blocking(sector_addr);
  on_complete_cb(context, S_SUCCESS);
}

void flash_erase_subsector(uint32_t sector_addr,
                           FlashOperationCompleteCb on_complete_cb,
                           void *context) {
  // TODO: implement nonblocking erase
  flash_erase_subsector_blocking(sector_addr);
  on_complete_cb(context, S_SUCCESS);
}
