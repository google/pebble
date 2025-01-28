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

#include "ice40lp.h"

#include "board/board.h"
#include "drivers/gpio.h"
#include "drivers/spi.h"
#include "drivers/pmic.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/delay.h"

#include "stm32f4xx.h"
#include "misc.h"

#include <string.h>

// We want the SPI clock to run at 16 by default
const uint32_t SPI_DEFAULT_MHZ = 16;
static uint32_t s_spi_clock_hz;

bool display_busy(void) {
  bool busy = GPIO_ReadInputDataBit(DISP_GPIO, DISP_PIN_BUSY);
  return busy;
}

static void prv_configure_spi(uint32_t spi_clock_hz) {
  // Set up a SPI bus on SPI6
  SPI_InitTypeDef spi_cfg;
  SPI_I2S_DeInit(DISP_SPI);
  SPI_StructInit(&spi_cfg);
  spi_cfg.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
  spi_cfg.SPI_Mode = SPI_Mode_Master;
  spi_cfg.SPI_DataSize = SPI_DataSize_8b;
  spi_cfg.SPI_CPOL = SPI_CPOL_High;
  spi_cfg.SPI_CPHA = SPI_CPHA_2Edge;
  spi_cfg.SPI_NSS = SPI_NSS_Soft;
  spi_cfg.SPI_BaudRatePrescaler = spi_find_prescaler(spi_clock_hz, DISPLAY_SPI_CLOCK_PERIPH);
  spi_cfg.SPI_FirstBit = SPI_FirstBit_MSB;
  SPI_Init(DISP_SPI, &spi_cfg);

  SPI_Cmd(DISP_SPI, ENABLE);
}

void display_start(void) {
  // Enable the GPIOG clock; this is required before configuring the pins
  gpio_use(DISP_GPIO);

  GPIO_PinAFConfig(DISP_GPIO, GPIO_PINSOURCE_SCK, GPIO_AF_SPI6); // SCK
  GPIO_PinAFConfig(DISP_GPIO, GPIO_PINSOURCE_MOSI, GPIO_AF_SPI6); // MOSI
  GPIO_PinAFConfig(DISP_GPIO, GPIO_PINSOURCE_MISO, GPIO_AF_SPI6); // MOSI

  GPIO_InitTypeDef gpio_cfg;
  gpio_cfg.GPIO_OType = GPIO_OType_PP;
  gpio_cfg.GPIO_PuPd = GPIO_PuPd_NOPULL;
  gpio_cfg.GPIO_Mode = GPIO_Mode_AF;
  gpio_cfg.GPIO_Speed = GPIO_Speed_25MHz;
  gpio_cfg.GPIO_Pin = DISP_PIN_SCLK;
  GPIO_Init(DISP_GPIO, &gpio_cfg);

  gpio_cfg.GPIO_Pin = DISP_PIN_SI;
  GPIO_Init(DISP_GPIO, &gpio_cfg);

  gpio_cfg.GPIO_Pin = DISP_PIN_SO;
  GPIO_Init(DISP_GPIO, &gpio_cfg);

  gpio_cfg.GPIO_Mode = GPIO_Mode_IN;
  gpio_cfg.GPIO_PuPd = GPIO_PuPd_UP;
  gpio_cfg.GPIO_Pin = DISP_PIN_CDONE;
  GPIO_Init(DISP_GPIO, &gpio_cfg);

  gpio_cfg.GPIO_Mode = GPIO_Mode_IN;
  gpio_cfg.GPIO_PuPd = GPIO_PuPd_NOPULL;
  gpio_cfg.GPIO_Pin = DISP_PIN_BUSY;
  GPIO_Init(DISP_GPIO, &gpio_cfg);

  gpio_cfg.GPIO_Mode = GPIO_Mode_OUT;
  gpio_cfg.GPIO_PuPd = GPIO_PuPd_NOPULL;
  gpio_cfg.GPIO_Pin = DISP_PIN_SCS;
  GPIO_Init(DISP_GPIO, &gpio_cfg);

  gpio_cfg.GPIO_OType = GPIO_OType_OD;
  gpio_cfg.GPIO_PuPd = GPIO_PuPd_NOPULL;
  gpio_cfg.GPIO_Pin = DISP_PIN_CRESET;
  GPIO_Init(DISP_GPIO, &gpio_cfg);

  RCC_APB2PeriphClockCmd(DISPLAY_SPI_CLOCK, ENABLE);

  s_spi_clock_hz = MHZ_TO_HZ(SPI_DEFAULT_MHZ);
  prv_configure_spi(s_spi_clock_hz);
}

bool display_program(const uint8_t *fpga_bitstream, uint32_t bitstream_size) {
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_SET);

  // wait a bit.
  delay_ms(1);

  GPIO_WriteBit(DISP_GPIO, DISP_PIN_CRESET, Bit_RESET); // CRESET LOW
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_RESET); // SCS LOW

  delay_ms(1);

  GPIO_WriteBit(DISP_GPIO, DISP_PIN_CRESET, Bit_SET); // CRESET -> HIGH

  delay_ms(1);

  PBL_ASSERT(!GPIO_ReadInputDataBit(DISP_GPIO, DISP_PIN_CDONE), "CDONE not low during reset");
  PBL_ASSERT(GPIO_ReadInputDataBit(DISP_GPIO, DISP_PIN_CRESET), "CRESET not high during reset");

  // Program the FPGA
  for (unsigned int i = 0; i < bitstream_size; ++i) {
    display_write_byte(fpga_bitstream[i]);
  }

  // Set SCS high so that we don't process any of these clocks as commands.
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_SET); // SCS -> HIGH

  // Send dummy clocks
  for (unsigned int i = 0; i < 8; ++i) {
    display_write_byte(0x00);
  }

  if (!GPIO_ReadInputDataBit(DISP_GPIO, DISP_PIN_CDONE)) {
    PBL_LOG(LOG_LEVEL_WARNING, "CDONE not high after programming!");
    return false;
  }
  return true;
}

void display_power_enable(void) {
  // The display requires us to wait 1ms between each power rail coming up. The PMIC
  // initialization brings up the 3.2V rail (VLCD on the display, LD02 on the PMIC) for us, but
  // we still need to wait before turning on the subsequent rails.
  delay_ms(2);

  PBL_LOG(LOG_LEVEL_DEBUG, "Enabling 6v6 (Display VDDC)");
  set_6V6_power_state(true);

  delay_ms(2);

  PBL_LOG(LOG_LEVEL_DEBUG, "Enabling 4v5 (Display VDDP)");
  set_4V5_power_state(true);
}

void display_power_disable(void) {
  PBL_LOG(LOG_LEVEL_DEBUG, "Disabling 4v5 (Display VDDP)");
  set_4V5_power_state(false);

  delay_ms(2);

  PBL_LOG(LOG_LEVEL_DEBUG, "Disabling 6v6 (Display VDDC)");
  set_6V6_power_state(false);

  delay_ms(2);
}

//!
//! Write a single byte synchronously to the display. Use this
//! sparingly, as it will tie up the micro during the write.
//!
void display_write_byte(uint8_t d) {
  // Block until the tx buffer is empty
  while (!SPI_I2S_GetFlagStatus(DISP_SPI, SPI_I2S_FLAG_TXE)) continue;
  SPI_I2S_SendData(DISP_SPI, d);
}

uint8_t display_write_and_read_byte(uint8_t d) {
  SPI_I2S_ReceiveData(DISP_SPI);
  while (!SPI_I2S_GetFlagStatus(DISP_SPI, SPI_I2S_FLAG_TXE)) continue;
  SPI_I2S_SendData(DISP_SPI, d);
  while (!SPI_I2S_GetFlagStatus(DISP_SPI, SPI_I2S_FLAG_RXNE)) continue;
  return SPI_I2S_ReceiveData(DISP_SPI);
}
