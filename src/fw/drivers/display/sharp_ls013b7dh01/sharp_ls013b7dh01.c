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

#include "sharp_ls013b7dh01.h"

#include "applib/graphics/gtypes.h"
#include "board/board.h"
#include "debug/power_tracking.h"
#include "drivers/dma.h"
#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "drivers/spi.h"
#include "kernel/util/sleep.h"
#include "kernel/util/stop.h"
#include "os/tick.h"
#include "services/common/analytics/analytics.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/bitset.h"
#include "util/net.h"
#include "util/reverse.h"
#include "util/units.h"


#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#include <mcu.h>

#include "misc.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GPIO constants
static const unsigned int DISP_MODE_STATIC = 0x00;
static const unsigned int DISP_MODE_WRITE = 0x80;
static const unsigned int DISP_MODE_CLEAR = 0x20;

// We want the SPI clock to run at 2MHz by default
static uint32_t s_spi_clock_hz;

// DMA constants
static DMA_Stream_TypeDef* DISPLAY_DMA_STREAM = DMA1_Stream4;
static const uint32_t DISPLAY_DMA_CLOCK = RCC_AHB1Periph_DMA1;

static bool s_initialized = false;

// DMA state
static DisplayContext s_display_context;
static uint32_t s_dma_line_buffer[DISP_DMA_BUFFER_SIZE_WORDS];

static SemaphoreHandle_t s_dma_update_in_progress_semaphore;

static void prv_display_write_byte(uint8_t d);
static void prv_display_context_init(DisplayContext* context);
static void prv_setup_dma_transfer(uint8_t* framebuffer_addr, int framebuffer_size);
static bool prv_do_dma_update(void);


static void prv_enable_display_spi_clock() {
  periph_config_enable(BOARD_CONFIG_DISPLAY.spi, BOARD_CONFIG_DISPLAY.spi_clk);
  power_tracking_start(PowerSystemMcuSpi2);
}

static void prv_disable_display_spi_clock() {
  periph_config_disable(BOARD_CONFIG_DISPLAY.spi, BOARD_CONFIG_DISPLAY.spi_clk);
  power_tracking_stop(PowerSystemMcuSpi2);
}

static void prv_enable_chip_select(void) {
  gpio_output_set(&BOARD_CONFIG_DISPLAY.cs, true);
  // setup time > 3us
  // this produces a setup time of ~7us
  for (volatile int i = 0; i < 32; i++);
}

static void prv_disable_chip_select(void) {
  // delay while last byte is emitted by the SPI peripheral (~7us)
  for (volatile int i = 0; i < 48; i++);
  gpio_output_set(&BOARD_CONFIG_DISPLAY.cs, false);
  // hold time > 1us
  // this produces a delay of ~3.5us
  for (volatile int i = 0; i < 16; i++);
}


static void prv_display_start(void) {
  periph_config_acquire_lock();

  gpio_af_init(&BOARD_CONFIG_DISPLAY.clk, GPIO_OType_PP, GPIO_Speed_50MHz, GPIO_PuPd_NOPULL);
  gpio_af_init(&BOARD_CONFIG_DISPLAY.mosi, GPIO_OType_PP, GPIO_Speed_50MHz, GPIO_PuPd_NOPULL);
  gpio_output_init(&BOARD_CONFIG_DISPLAY.cs, GPIO_OType_PP, GPIO_Speed_50MHz);
  gpio_output_init(&BOARD_CONFIG_DISPLAY.on_ctrl,
                   BOARD_CONFIG_DISPLAY.on_ctrl_otype,
                   GPIO_Speed_50MHz);

  if (BOARD_CONFIG.power_5v0_options != OptionNotPresent) {
    GPIOOType_TypeDef otype = (BOARD_CONFIG.power_5v0_options == OptionActiveLowOpenDrain)
        ? GPIO_OType_OD : GPIO_OType_PP;
    gpio_output_init(&BOARD_CONFIG.power_ctl_5v0, otype, GPIO_Speed_50MHz);
  }

  if (BOARD_CONFIG.lcd_com.gpio) {
    gpio_output_init(&BOARD_CONFIG.lcd_com, GPIO_OType_PP, GPIO_Speed_50MHz);
  }

  // Set up a SPI bus on SPI2
  SPI_InitTypeDef spi_cfg;
  SPI_I2S_DeInit(BOARD_CONFIG_DISPLAY.spi);
  SPI_StructInit(&spi_cfg);
  spi_cfg.SPI_Direction = SPI_Direction_1Line_Tx; // Write-only SPI
  spi_cfg.SPI_Mode = SPI_Mode_Master;
  spi_cfg.SPI_DataSize = SPI_DataSize_8b;
  spi_cfg.SPI_CPOL = SPI_CPOL_Low;
  spi_cfg.SPI_CPHA = SPI_CPHA_1Edge;
  spi_cfg.SPI_NSS = SPI_NSS_Soft;
  spi_cfg.SPI_BaudRatePrescaler =
      spi_find_prescaler(s_spi_clock_hz, BOARD_CONFIG_DISPLAY.spi_clk_periph);
  spi_cfg.SPI_FirstBit = SPI_FirstBit_MSB;
  SPI_Init(BOARD_CONFIG_DISPLAY.spi, &spi_cfg);

  gpio_use(BOARD_CONFIG_DISPLAY.spi_gpio);
  SPI_Cmd(BOARD_CONFIG_DISPLAY.spi, ENABLE);
  gpio_release(BOARD_CONFIG_DISPLAY.spi_gpio);

  if (BOARD_CONFIG.power_5v0_options != OptionNotPresent) {
    // +5V to 5V_EN pin
    gpio_output_set(&BOARD_CONFIG.power_ctl_5v0, true);
  }

  // +5V to LCD_DISP pin (Set this pin low to turn off the display)
  gpio_output_set(&BOARD_CONFIG_DISPLAY.on_ctrl, true);

  periph_config_release_lock();
}

uint32_t display_baud_rate_change(uint32_t new_frequency_hz) {
  // Take the semaphore so that we can be sure that we are not interrupting a transfer
  xSemaphoreTake(s_dma_update_in_progress_semaphore, portMAX_DELAY);

  uint32_t old_spi_clock_hz = s_spi_clock_hz;
  s_spi_clock_hz = new_frequency_hz;
  prv_enable_display_spi_clock();
  prv_display_start();
  prv_disable_display_spi_clock();

  xSemaphoreGive(s_dma_update_in_progress_semaphore);
  return old_spi_clock_hz;
}

void display_init(void) {
  if (s_initialized) {
    return;
  }

  s_spi_clock_hz = MHZ_TO_HZ(2);

  prv_display_context_init(&s_display_context);

  vSemaphoreCreateBinary(s_dma_update_in_progress_semaphore);

  dma_request_init(SHARP_SPI_TX_DMA);

  prv_enable_display_spi_clock();

  prv_display_start();

  prv_disable_display_spi_clock();
  s_initialized = true;
}

static void prv_display_context_init(DisplayContext* context) {
  context->state = DISPLAY_STATE_IDLE;
  context->get_next_row = NULL;
  context->complete = NULL;
}

// Clear-all mode is entered by sending 0x04 to the panel
void display_clear(void) {
  prv_enable_display_spi_clock();
  prv_enable_chip_select();

  prv_display_write_byte(DISP_MODE_CLEAR);
  prv_display_write_byte(0x00);

  prv_disable_chip_select();
  prv_disable_display_spi_clock();
}

bool display_update_in_progress(void) {
  if (xSemaphoreTake(s_dma_update_in_progress_semaphore, 0) == pdPASS) {
    xSemaphoreGive(s_dma_update_in_progress_semaphore);
    return false;
  }
  return true;
}

void display_update(NextRowCallback nrcb, UpdateCompleteCallback uccb) {
  PBL_ASSERTN(nrcb != NULL);
  PBL_ASSERTN(uccb != NULL);
  stop_mode_disable(InhibitorDisplay);
  xSemaphoreTake(s_dma_update_in_progress_semaphore, portMAX_DELAY);
  analytics_stopwatch_start(ANALYTICS_APP_METRIC_DISPLAY_WRITE_TIME, AnalyticsClient_App);
  analytics_inc(ANALYTICS_DEVICE_METRIC_DISPLAY_UPDATES_PER_HOUR, AnalyticsClient_System);

  prv_enable_display_spi_clock();
  power_tracking_start(PowerSystemMcuDma1);
  SPI_I2S_DMACmd(BOARD_CONFIG_DISPLAY.spi, SPI_I2S_DMAReq_Tx, ENABLE);

  prv_display_context_init(&s_display_context);
  s_display_context.get_next_row = nrcb;
  s_display_context.complete = uccb;

  prv_do_dma_update();

  // Block while we wait for the update to finish.
  TickType_t ticks = milliseconds_to_ticks(4000); // DMA should be fast
  if (xSemaphoreTake(s_dma_update_in_progress_semaphore, ticks) != pdTRUE) {
    // something went wrong, gather some debug info & reset
    int dma_status = DMA_GetITStatus(DISPLAY_DMA_STREAM, DMA_IT_TCIF4);
    uint32_t spi_clock_status = (RCC->APB1ENR & BOARD_CONFIG_DISPLAY.spi_clk);
    uint32_t dma_clock_status = (RCC->AHB1ENR & DISPLAY_DMA_CLOCK);
    uint32_t pri_mask = __get_PRIMASK();
    PBL_CROAK("display DMA failed: 0x%" PRIx32 " %d 0x%lx 0x%lx", pri_mask,
        dma_status, spi_clock_status, dma_clock_status);
  }

  power_tracking_stop(PowerSystemMcuDma1);
  prv_disable_display_spi_clock();

  xSemaphoreGive(s_dma_update_in_progress_semaphore);
  stop_mode_enable(InhibitorDisplay);
  analytics_stopwatch_stop(ANALYTICS_APP_METRIC_DISPLAY_WRITE_TIME);
}

// Static mode is entered by sending 0x00 to the panel
static void prv_display_enter_static(void) {
  prv_enable_chip_select();

  prv_display_write_byte(DISP_MODE_STATIC);
  prv_display_write_byte(0x00);
  prv_display_write_byte(0x00);

  prv_disable_chip_select();
}

void display_pulse_vcom(void) {
  PBL_ASSERTN(BOARD_CONFIG.lcd_com.gpio != 0);
  gpio_output_set(&BOARD_CONFIG.lcd_com, true);
  // the spec requires at least 1us; this provides ~2 so should be safe
  for (volatile int i = 0; i < 8; i++);
  gpio_output_set(&BOARD_CONFIG.lcd_com, false);
}


static bool prv_dma_handler(DMARequest *request, void *context) {
  return prv_do_dma_update();
}

#if DISPLAY_ORIENTATION_ROTATED_180
//!
//! memcpy the src buffer to dst and reverse the bits
//! to match the display order
//!
static void prv_memcpy_reverse_bytes(uint8_t* dst, uint8_t* src, int bytes) {
    // Skip the mode selection and column address bytes
    dst+=2;
    while (bytes--) {
        *dst++ = reverse_byte(*src++);
    }
}
#else
//!
//! memcpy the src buffer to dst backwards (i.e. the highest src byte
//! is the lowest byte in dst.
//!
static void prv_memcpy_backwards(uint32_t* dst, uint32_t* src, int length) {
  dst += length - 1;
  while (length--) {
    *dst-- = ntohl(*src++);
  }
}
#endif


//!
//! Write a single byte synchronously to the display. Use this
//! sparingly, as it will tie up the micro during the write.
//!
static void prv_display_write_byte(uint8_t d) {
  // Block until the tx buffer is empty
  SPI_I2S_SendData(BOARD_CONFIG_DISPLAY.spi, d);
  while (!SPI_I2S_GetFlagStatus(BOARD_CONFIG_DISPLAY.spi, SPI_I2S_FLAG_TXE)) {}
}

static bool prv_do_dma_update(void) {
  DisplayRow r;

  PBL_ASSERTN(s_display_context.get_next_row != NULL);
  bool is_end_of_buffer = !s_display_context.get_next_row(&r);

  switch (s_display_context.state) {
  case DISPLAY_STATE_IDLE:
  {
    if (is_end_of_buffer) {
      // If nothing has been modified, bail out early
      return false;
    }

    // Enable display slave select
    prv_enable_chip_select();

    s_display_context.state = DISPLAY_STATE_WRITING;

#if DISPLAY_ORIENTATION_ROTATED_180
      prv_memcpy_reverse_bytes((uint8_t*)s_dma_line_buffer, r.data, DISP_LINE_BYTES);
      s_dma_line_buffer[0] &= ~(0xffff);
      s_dma_line_buffer[0] |= (DISP_MODE_WRITE | reverse_byte(r.address + 1) << 8);
#else
      prv_memcpy_backwards(s_dma_line_buffer, (uint32_t*)r.data, DISP_LINE_WORDS);
      s_dma_line_buffer[0] &= ~(0xffff);
      s_dma_line_buffer[0] |= (DISP_MODE_WRITE | reverse_byte(167 - r.address + 1) << 8);
#endif
    prv_setup_dma_transfer(((uint8_t*) s_dma_line_buffer), DISP_DMA_BUFFER_SIZE_BYTES);

    break;
  }
  case DISPLAY_STATE_WRITING:
  {
    if (is_end_of_buffer) {
      prv_display_write_byte(0x00);

      // Disable display slave select
      prv_disable_chip_select();

      prv_display_enter_static();

      s_display_context.complete();

      signed portBASE_TYPE was_higher_priority_task_woken = pdFALSE;
      xSemaphoreGiveFromISR(s_dma_update_in_progress_semaphore, &was_higher_priority_task_woken);

      return was_higher_priority_task_woken != pdFALSE;
    }

#if DISPLAY_ORIENTATION_ROTATED_180
    prv_memcpy_reverse_bytes((uint8_t*)s_dma_line_buffer, r.data, DISP_LINE_BYTES);
    s_dma_line_buffer[0] &= ~(0xffff);
    s_dma_line_buffer[0] |= (DISP_MODE_WRITE | reverse_byte(r.address + 1) << 8);
#else
    prv_memcpy_backwards(s_dma_line_buffer, (uint32_t*)r.data, DISP_LINE_WORDS);
    s_dma_line_buffer[0] &= ~(0xffff);
    s_dma_line_buffer[0] |= reverse_byte(167 - r.address + 1) << 8;
#endif
    prv_setup_dma_transfer(((uint8_t*) s_dma_line_buffer) + 1, DISP_DMA_BUFFER_SIZE_BYTES - 1);
    break;
  }
  default:
    WTF;
  }
  return false;
}

static void prv_setup_dma_transfer(uint8_t *framebuffer_addr, int framebuffer_size) {
  void *dst = (void *)&(BOARD_CONFIG_DISPLAY.spi->DR);
  dma_request_start_direct(SHARP_SPI_TX_DMA, dst, framebuffer_addr, framebuffer_size,
                           prv_dma_handler, NULL);
}

void display_show_splash_screen(void) {
  // The bootloader has already drawn the splash screen for us; nothing to do!
}

// Stubs for display offset
void display_set_offset(GPoint offset) {}

GPoint display_get_offset(void) { return GPointZero; }
