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

#include "spi_definitions.h"
#include "drivers/spi.h"
#include "drivers/spi_dma.h"

#include "drivers/dma.h"
#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/units.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

//! Deduced by looking at the prescalers in stm32f2xx_spi.h
#define SPI_FREQ_LOG_TO_PRESCALER(LG) (((LG) - 1) * 0x8)
//! Bits in CR1 we intend to keep when updating it
#define CR1_CLEAR_MASK ((uint16_t)0x3040)

//! SPI / I2S DMA definitions
typedef enum SpiI2sDma {
  SpiI2sDma_ReqTx = 0x0002,
  SpiI2sDma_ReqRx = 0x0001
} SpiI2sDma;

//! SPI Master/Slave
typedef enum SpiMode {
  SpiMode_Master = 0x0104,
  SpiMode_Slave = 0x0000
} SpiMode;

//! SPI Data Size
typedef enum SpiDataSize {
  SpiDataSize_16b = 0x0800,
  SpiDataSize_8b = 0x0000
} SpiDataSize;

//! SPI Slave Select
typedef enum SpiSlaveSelect {
  SpiSlaveSelect_Soft = 0x0200,
  SpiSlaveSelect_Hard = 0x0000
} SpiSlaveSelect;

typedef enum {
  SpiDisable = 0,
  SpiEnable
} SpiFunctionalState;

//
// Private SPI bus functions.  No higher level code should
// get access to SPIBus functions or data directly
//

static bool prv_spi_get_flag_status(const SPIBus *bus, SpiI2sFlag flag) {
  /* Check the status of the specified SPI flag */
  return (bus->spi->SR & (uint16_t)flag) != 0;
}

static bool prv_spi_transmit_is_idle(const SPIBus *bus) {
  return prv_spi_get_flag_status(bus, SpiI2sFlag_TXE);
}

static bool prv_spi_receive_is_ready(const SPIBus *bus) {
  return prv_spi_get_flag_status(bus, SpiI2sFlag_RXNE);
}

void prv_spi_send_data(const SPIBus *bus, uint16_t Data) {
#if MICRO_FAMILY_STM32F7
  // STM32F7 needs to access as 8 bits in order to actually do 8 bits.
  // This _does_ work on F4, but QEMU doesn't agree, so let's just do it safely.
  *(volatile uint8_t*)&bus->spi->DR = Data;
#else
  bus->spi->DR = Data;
#endif
}

uint16_t prv_spi_receive_data(const SPIBus *bus) {
#if MICRO_FAMILY_STM32F7
  // STM32F7 needs to access as 8 bits in order to actually do 8 bits.
  // This _does_ work on F4, but QEMU doesn't agree, so let's just do it safely.
  return *(volatile uint8_t*)&bus->spi->DR;
#else
  return bus->spi->DR;
#endif
}

void prv_spi_enable_peripheral_clock(const SPIBus *bus) {
  periph_config_enable(bus->spi, bus->state->spi_clock_periph);
}

void prv_spi_disable_peripheral_clock(const SPIBus *bus) {
  periph_config_disable(bus->spi, bus->state->spi_clock_periph);
}

static void prv_spi_clear_flags(const SPIBus *bus) {
  prv_spi_receive_data(bus);
  prv_spi_get_flag_status(bus, (SpiI2sFlag)0);
}

static void prv_spi_dma_cmd(const SPIBus *bus, SpiI2sDma dma_bits, bool enable) {
  if (enable) {
    bus->spi->CR2 |= (uint16_t)dma_bits;
  } else {
    bus->spi->CR2 &= (uint16_t)~dma_bits;
  }
}

static void prv_spi_cmd(const SPIBus *bus, SpiFunctionalState state) {
  if (state != SpiDisable) {
    /* Enable the selected SPI peripheral */
    bus->spi->CR1 |= SPI_CR1_SPE;
  } else {
    /* Disable the selected SPI peripheral */
    bus->spi->CR1 &= (uint16_t)~((uint16_t)SPI_CR1_SPE);
  }
}

void prv_spi_pick_peripheral(const SPIBus *bus) {
  RCC_ClocksTypeDef clocks;
  RCC_GetClocksFreq(&clocks);
  if (bus->spi == SPI1) {
    bus->state->spi_clock_periph = RCC_APB2Periph_SPI1;
    bus->state->spi_clock_periph_speed = clocks.PCLK2_Frequency;
    bus->state->spi_apb = SpiAPB_2;
  } else if (bus->spi == SPI2) {
    bus->state->spi_clock_periph = RCC_APB1Periph_SPI2;
    bus->state->spi_clock_periph_speed = clocks.PCLK1_Frequency;
    bus->state->spi_apb = SpiAPB_1;
  } else if (bus->spi == SPI3) {
    bus->state->spi_clock_periph = RCC_APB1Periph_SPI3;
    bus->state->spi_clock_periph_speed = clocks.PCLK1_Frequency;
    bus->state->spi_apb = SpiAPB_1;
#ifdef SPI4
  } else if (bus->spi == SPI4) {
    bus->state->spi_clock_periph = RCC_APB2Periph_SPI4;
    bus->state->spi_clock_periph_speed = clocks.PCLK2_Frequency;
    bus->state->spi_apb = SpiAPB_2;
#endif
#ifdef SPI5
  } else if (bus->spi == SPI5) {
    bus->state->spi_clock_periph = RCC_APB2Periph_SPI5;
    bus->state->spi_clock_periph_speed = clocks.PCLK2_Frequency;
    bus->state->spi_apb = SpiAPB_2;
#endif
#ifdef SPI6
  } else if (bus->spi == SPI6) {
    bus->state->spi_clock_periph = RCC_APB2Periph_SPI6;
    bus->state->spi_clock_periph_speed = clocks.PCLK2_Frequency;
    bus->state->spi_apb = SpiAPB_2;
#endif
  }
}

static uint16_t prv_spi_find_prescaler(const SPIBus *bus) {
  int lg;
  if (bus->state->spi_clock_speed_hz > (bus->state->spi_clock_periph_speed / 2)) {
    lg = 1; // Underclock to the highest possible frequency
  } else {
    uint32_t divisor = bus->state->spi_clock_periph_speed / bus->state->spi_clock_speed_hz;
    lg = ceil_log_two(divisor);
  }

  // Prescalers only exists for values in [2 - 256] range
  PBL_ASSERTN(lg > 0);
  PBL_ASSERTN(lg < 9);

  // return prescaler
  return (SPI_FREQ_LOG_TO_PRESCALER(lg));
}

void prv_spi_transmit_flush_blocking(const SPIBus *bus) {
  while (!prv_spi_transmit_is_idle(bus)) continue;
}

void prv_spi_receive_wait_ready_blocking(const SPIBus *bus) {
  while (!prv_spi_receive_is_ready(bus)) continue;
}

static void prv_configure_spi_sclk(const AfConfig *clk_pin, uint16_t spi_sclk_speed) {
  gpio_af_init(clk_pin, GPIO_OType_PP, spi_sclk_speed, GPIO_PuPd_NOPULL);
}

static void prv_spi_bus_deinit(const SPIBus *bus, bool is_bidirectional) {
  // The pins are no longer in use so reconfigure as analog inputs to save some power

  // SCLK
  InputConfig sclk = { .gpio = bus->spi_sclk.gpio, .gpio_pin = bus->spi_sclk.gpio_pin };
  gpio_analog_init(&sclk);

  // MOSI
  InputConfig mosi = { .gpio = bus->spi_mosi.gpio, .gpio_pin = bus->spi_mosi.gpio_pin };
  gpio_analog_init(&mosi);

  // MISO
  if (is_bidirectional) {
    InputConfig miso = { .gpio = bus->spi_miso.gpio, .gpio_pin = bus->spi_miso.gpio_pin };
    gpio_analog_init(&miso);
  }

  bus->state->initialized = false;
}

void prv_spi_bus_init(const SPIBus *bus, bool is_bidirectional) {
  if (bus->state->initialized) {
    return;
  }
  // copy the speed over to the transient state since the slave port can change it
  bus->state->spi_clock_speed_hz = bus->spi_clock_speed_hz;
  prv_spi_pick_peripheral(bus);
  bus->state->initialized = true;
  // SCLK
  prv_configure_spi_sclk(&bus->spi_sclk, bus->spi_sclk_speed);
  // MOSI
  gpio_af_init(&bus->spi_mosi, GPIO_OType_PP, bus->spi_sclk_speed, GPIO_PuPd_NOPULL);
  // MISO
  if (is_bidirectional) {
    gpio_af_init(&bus->spi_miso, GPIO_OType_PP, bus->spi_sclk_speed, GPIO_PuPd_NOPULL);
  }
}

static void prv_spi_slave_init(const SPISlavePort *slave) {
  prv_spi_enable_peripheral_clock(slave->spi_bus);
  SPIBus *bus = slave->spi_bus;
  // Grab existing configuration
  uint16_t tmpreg = bus->spi->CR1;
  // Clear BIDIMode, BIDIOE, RxONLY, SSM, SSI, LSBFirst, BR, MSTR, CPOL and CPHA bits
  tmpreg &= CR1_CLEAR_MASK;
  // get the baudrate prescaler
  uint32_t prescaler = prv_spi_find_prescaler(bus);
  // Master mode, 8 bit Data Size and Soft Slave select are hardcoded
  // Direction, CPOL, CPHA, baudrate prescaler and first-bit come from the device config
  tmpreg |= (uint16_t)((uint32_t)slave->spi_direction | SpiMode_Master |
    SpiDataSize_8b | slave->spi_cpol |
    slave->spi_cpha | SpiSlaveSelect_Soft |
    prescaler | slave->spi_first_bit);
  // Write result back to CR1
  bus->spi->CR1 = tmpreg;

#if MICRO_FAMILY_STM32F7
  // On STM32F7 we need to set FRXTH in order to do 8-bit transfers.
  // If we don't, the MCU always tries to read 16-bits even though we
  // specified that the data is 8-bits.
  // Why clear isn't 8-bit is beyond me, but ok.
  bus->spi->CR2 |= SPI_CR2_FRXTH;
#endif

  // Activate the SPI mode (Reset I2SMOD bit in I2SCFGR register)
  bus->spi->I2SCFGR &= (uint16_t)~((uint16_t)SPI_I2SCFGR_I2SMOD);

  prv_spi_disable_peripheral_clock(slave->spi_bus);
}

static void prv_spi_slave_deinit(const SPISlavePort *slave) {
  spi_ll_slave_acquire(slave);
  SPIBus *bus = slave->spi_bus;
  if (bus->state->spi_apb == SpiAPB_1) {
    // Enable SPIx reset state
    RCC_APB1PeriphResetCmd(bus->state->spi_clock_periph, ENABLE);
    // Release SPIx from reset state
    RCC_APB1PeriphResetCmd(bus->state->spi_clock_periph, DISABLE);
  } else if (bus->state->spi_apb == SpiAPB_2) {
    // Enable SPIx reset state
    RCC_APB2PeriphResetCmd(bus->state->spi_clock_periph, ENABLE);
    // Release SPIx from reset state
    RCC_APB2PeriphResetCmd(bus->state->spi_clock_periph, DISABLE);
  }
  spi_ll_slave_release(slave);
}

//
//! High level slave port interface
//! This part of the API can be used for fairly straightforward SPI interactions
//! The assertion and deassertion of the SCS line is automatic
//

static bool prv_is_bidirectional(const SPISlavePort *slave) {
    bool is_bidirectional = (slave->spi_direction == SpiDirection_2LinesFullDuplex) ||
        (slave->spi_direction == SpiDirection_2LinesRxOnly);

    return (is_bidirectional);
}

void spi_slave_port_deinit(const SPISlavePort *slave) {
  // don't deinitialize twice
  if (!slave->slave_state->initialized) {
    return;
  }
  prv_spi_slave_deinit(slave);
  prv_spi_bus_deinit(slave->spi_bus, prv_is_bidirectional(slave));
  slave->slave_state->initialized = false;
}

void spi_slave_port_init(const SPISlavePort *slave) {
  // don't initialize twice
  if (slave->slave_state->initialized) {
    return;
  }
  slave->slave_state->initialized = true;
  slave->slave_state->acquired = false;
  slave->slave_state->scs_selected = false;
  prv_spi_bus_init(slave->spi_bus, prv_is_bidirectional(slave));

  // SCS
  gpio_output_init(&slave->spi_scs, GPIO_OType_PP, slave->spi_bus->spi_sclk_speed);
  gpio_output_set(&slave->spi_scs, false);  // SCS not asserted (high)

  // Set up an SPI
  prv_spi_slave_deinit(slave);

  prv_spi_slave_init(slave);

  // Set up DMA
  if (slave->rx_dma) {
    dma_request_init(slave->rx_dma);
  }
  if (slave->tx_dma) {
    dma_request_init(slave->tx_dma);
  }
}

static void prv_spi_acquire_helper(const SPISlavePort *slave) {
  spi_ll_slave_acquire(slave);
  spi_ll_slave_scs_assert(slave);
}

static void prv_spi_release_helper(const SPISlavePort *slave) {
  spi_ll_slave_scs_deassert(slave);
  spi_ll_slave_release(slave);
}

uint8_t spi_slave_read_write(const SPISlavePort *slave, uint8_t out) {
  prv_spi_acquire_helper(slave);
  uint8_t ret = spi_ll_slave_read_write(slave, out);
  prv_spi_release_helper(slave);
  return ret;
}

void spi_slave_write(const SPISlavePort *slave, uint8_t out) {
  prv_spi_acquire_helper(slave);
  spi_ll_slave_write(slave, out);
  prv_spi_release_helper(slave);
}

void spi_slave_burst_read(const SPISlavePort *slave, void *in, size_t len) {
  prv_spi_acquire_helper(slave);
  spi_ll_slave_burst_read(slave, in, len);
  prv_spi_release_helper(slave);
}

void spi_slave_burst_write(const SPISlavePort *slave, const void *out, size_t len) {
  prv_spi_acquire_helper(slave);
  spi_ll_slave_burst_write(slave, out, len);
  prv_spi_release_helper(slave);
}

void spi_slave_burst_read_write(const SPISlavePort *slave, const void *out, void *in, size_t len) {
  prv_spi_acquire_helper(slave);
  spi_ll_slave_burst_read_write(slave, out, in, len);
  prv_spi_release_helper(slave);
}

void spi_slave_burst_read_write_scatter(const SPISlavePort *slave,
                             const SPIScatterGather *sc_info,
                             size_t num_sg) {
  prv_spi_acquire_helper(slave);
  spi_ll_slave_burst_read_write_scatter(slave, sc_info, num_sg);
  prv_spi_release_helper(slave);
}

void spi_slave_set_frequency(const SPISlavePort *slave, uint32_t frequency_hz) {
  slave->spi_bus->state->spi_clock_speed_hz = frequency_hz;
  prv_spi_slave_init(slave);
}

void spi_slave_wait_until_idle_blocking(const SPISlavePort *slave) {
  while (prv_spi_get_flag_status(slave->spi_bus, SpiI2sFlag_BSY)) continue;
}

uint32_t spi_get_dma_base_address(const SPISlavePort *slave) {
  return (uint32_t)&(slave->spi_bus->spi->DR);
}
//
//! Low level slave port interface
//! This part of the API can be used for slightly more complex SPI operations
//! (such as piecemeal reads or writes).  Assertion and deassertion of SCS
//! is up to the caller.  Asserts in the code will help to ensure that the
//! API is used correctly.
//

void spi_ll_slave_acquire(const SPISlavePort *slave) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired == false);
  prv_spi_enable_peripheral_clock(slave->spi_bus);
  prv_spi_clear_flags(slave->spi_bus);
  slave->slave_state->acquired = true;
  spi_ll_slave_spi_enable(slave);
}

void spi_ll_slave_release(const SPISlavePort *slave) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  spi_slave_wait_until_idle_blocking(slave);
  prv_spi_clear_flags(slave->spi_bus);
  spi_ll_slave_spi_disable(slave);
  slave->slave_state->acquired = false;
  prv_spi_disable_peripheral_clock(slave->spi_bus);
}

void spi_ll_slave_spi_enable(const SPISlavePort *slave) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  prv_spi_cmd(slave->spi_bus, SpiEnable);
}

void spi_ll_slave_spi_disable(const SPISlavePort *slave) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  prv_spi_cmd(slave->spi_bus, SpiDisable);
}

void spi_ll_slave_scs_assert(const SPISlavePort *slave) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->scs_selected == false);
  slave->slave_state->scs_selected = true;
  gpio_output_set(&slave->spi_scs, true);  // SCS asserted (low)
}

void spi_ll_slave_scs_deassert(const SPISlavePort *slave) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->scs_selected);
  slave->slave_state->scs_selected = false;
  gpio_output_set(&slave->spi_scs, false);  // SCS not asserted (high)
}

uint8_t spi_ll_slave_read_write(const SPISlavePort *slave, uint8_t out) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->scs_selected);
  prv_spi_transmit_flush_blocking(slave->spi_bus);
  prv_spi_send_data(slave->spi_bus, out);
  prv_spi_receive_wait_ready_blocking(slave->spi_bus);
  return prv_spi_receive_data(slave->spi_bus);
}

void spi_ll_slave_write(const SPISlavePort *slave, uint8_t out) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->scs_selected);
  prv_spi_transmit_flush_blocking(slave->spi_bus);
  prv_spi_send_data(slave->spi_bus, out);
}

void spi_ll_slave_burst_read(const SPISlavePort *slave, void *in, size_t len) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->scs_selected);
  uint8_t *cptr = in;
  while (len--) {
    *(cptr++) = spi_ll_slave_read_write(slave, 0); // useless write-data
  }
}

void spi_ll_slave_burst_write(const SPISlavePort *slave, const void *out, size_t len) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  const uint8_t *cptr = out;
  while (len--) {
    prv_spi_send_data(slave->spi_bus, *(cptr++));
    prv_spi_transmit_flush_blocking(slave->spi_bus);
  }
}

void spi_ll_slave_burst_read_write(const SPISlavePort *slave,
                                   const void *out,
                                   void *in,
                                   size_t len) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  const uint8_t *outp = out;
  uint8_t *inp = in;
  for (size_t n = 0; n < len; ++n) {
    uint8_t byte_out = outp ? *(outp++) : 0;
    uint8_t byte_in = spi_ll_slave_read_write(slave, byte_out);
    if (inp) {
      *(inp++) = byte_in;
    }
  }
}

void spi_ll_slave_burst_read_write_scatter(const SPISlavePort *slave,
                                           const SPIScatterGather *sc_info,
                                           size_t num_sg) {
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  for (size_t elem = 0; elem < num_sg; ++elem) {
    const SPIScatterGather *sg = &sc_info[elem];
    spi_ll_slave_burst_read_write(slave, sg->sg_out, sg->sg_in, sg->sg_len);
  }
}

static bool prv_dma_irq_handler(DMARequest *request, void *context) {
  const SPISlavePort *slave = context;
  PBL_ASSERTN(slave);
  bool is_done = false;
  switch (slave->slave_state->dma_state) {
    case SPISlavePortDMAState_Read:
    case SPISlavePortDMAState_Write:
    case SPISlavePortDMAState_ReadWriteOneInterrupt:
      slave->slave_state->dma_state = SPISlavePortDMAState_Idle;
      is_done = true;
      break;
    case SPISlavePortDMAState_ReadWrite:
      slave->slave_state->dma_state = SPISlavePortDMAState_ReadWriteOneInterrupt;
      break;
    case SPISlavePortDMAState_Idle:
    default:
      WTF;
  }
  SPIDMACompleteHandler handler = slave->slave_state->dma_complete_handler;
  if (is_done && handler) {
    return handler(slave, slave->slave_state->dma_complete_context);
  }
  return false;
}

void spi_ll_slave_read_dma_start(const SPISlavePort *slave, void *in, size_t len,
                                 SPIDMACompleteHandler handler, void *context) {
  PBL_ASSERTN(slave->rx_dma);
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->dma_state == SPISlavePortDMAState_Idle);
  slave->slave_state->dma_state = SPISlavePortDMAState_Read;
  slave->slave_state->dma_complete_handler = handler;
  slave->slave_state->dma_complete_context = context;
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqRx, true);
  dma_request_start_direct(slave->rx_dma, in, (void *)&slave->spi_bus->spi->DR, len,
                           prv_dma_irq_handler, (void *)slave);
}

void spi_ll_slave_read_dma_stop(const SPISlavePort *slave) {
  if (slave->slave_state->dma_state != SPISlavePortDMAState_Read) {
    return;
  }
  dma_request_stop(slave->rx_dma);
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqRx, false);
  slave->slave_state->dma_complete_handler = NULL;
  slave->slave_state->dma_complete_context = NULL;
}

void spi_ll_slave_write_dma_start(const SPISlavePort *slave, const void *out, size_t len,
                                  SPIDMACompleteHandler handler, void *context) {
  PBL_ASSERTN(slave->tx_dma);
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->dma_state == SPISlavePortDMAState_Idle);
  slave->slave_state->dma_state = SPISlavePortDMAState_Write;
  slave->slave_state->dma_complete_handler = handler;
  slave->slave_state->dma_complete_context = context;
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqTx, true);
  dma_request_start_direct(slave->tx_dma, (void *)&slave->spi_bus->spi->DR, out, len,
                           prv_dma_irq_handler, (void *)slave);
}

void spi_ll_slave_write_dma_stop(const SPISlavePort *slave) {
  if (slave->slave_state->dma_state != SPISlavePortDMAState_Write) {
    return;
  }
  dma_request_stop(slave->tx_dma);
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqTx, false);
  slave->slave_state->dma_complete_handler = NULL;
  slave->slave_state->dma_complete_context = NULL;
}

void spi_ll_slave_read_write_dma_start(const SPISlavePort *slave, const void *out, void *in,
                                       size_t len, SPIDMACompleteHandler handler, void *context) {
  PBL_ASSERTN(slave->rx_dma && slave->tx_dma);
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  PBL_ASSERTN(slave->slave_state->dma_state == SPISlavePortDMAState_Idle);
  slave->slave_state->dma_complete_handler = handler;
  slave->slave_state->dma_complete_context = context;

  if (out) {
    dma_request_set_memory_increment_disabled(slave->tx_dma, false);
  } else {
    dma_request_set_memory_increment_disabled(slave->tx_dma, true);
    static const uint8_t s_zero = 0;
    out = &s_zero;
  }

  if (in) {
    slave->slave_state->dma_state = SPISlavePortDMAState_ReadWrite;
    // start the read DMA
    prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqRx, true);
    dma_request_start_direct(slave->rx_dma, in, (void *)&slave->spi_bus->spi->DR, len,
                             prv_dma_irq_handler, (void *)slave);
  } else {
    slave->slave_state->dma_state = SPISlavePortDMAState_ReadWriteOneInterrupt;
  }
  // start the write DMA
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqTx, true);
  dma_request_start_direct(slave->tx_dma, (void *)&slave->spi_bus->spi->DR, out, len,
                           prv_dma_irq_handler, (void *)slave);
}

void spi_ll_slave_read_write_dma_stop(const SPISlavePort *slave) {
  if ((slave->slave_state->dma_state != SPISlavePortDMAState_ReadWrite) &&
      (slave->slave_state->dma_state != SPISlavePortDMAState_ReadWriteOneInterrupt)) {
    return;
  }
  PBL_ASSERTN(slave->tx_dma && slave->rx_dma);
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  dma_request_stop(slave->rx_dma);
  dma_request_stop(slave->tx_dma);
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqRx, false);
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqTx, false);
  slave->slave_state->dma_complete_handler = NULL;
  slave->slave_state->dma_complete_context = NULL;
}

bool spi_ll_slave_dma_in_progress(const SPISlavePort *slave) {
  PBL_ASSERTN(slave->tx_dma || slave->rx_dma);
  PBL_ASSERTN(slave->slave_state->initialized);
  PBL_ASSERTN(slave->slave_state->acquired);
  return (slave->rx_dma && dma_request_in_progress(slave->rx_dma)) ||
         (slave->tx_dma && dma_request_in_progress(slave->tx_dma));
}

void spi_ll_slave_set_tx_dma(const SPISlavePort *slave, bool enable) {
  PBL_ASSERTN(slave->slave_state->initialized);
  spi_ll_slave_acquire(slave);
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqTx, enable);
  spi_ll_slave_release(slave);
}

void spi_ll_slave_set_rx_dma(const SPISlavePort *slave, bool enable) {
  PBL_ASSERTN(slave->slave_state->initialized);
  spi_ll_slave_acquire(slave);
  prv_spi_dma_cmd(slave->spi_bus, SpiI2sDma_ReqRx, enable);
  spi_ll_slave_release(slave);
}

void spi_ll_slave_drive_clock(const SPISlavePort *slave, bool enable) {
  const AfConfig *spi_sclk = &slave->spi_bus->spi_sclk;

  if (enable) {
    OutputConfig clk_as_gpio = {
      .gpio = spi_sclk->gpio,
      .gpio_pin = spi_sclk->gpio_pin,
      .active_high = true,
    };
    gpio_output_init(&clk_as_gpio, GPIO_OType_PP, GPIO_Speed_50MHz);
    gpio_output_set(&clk_as_gpio, false);
  } else {
    prv_configure_spi_sclk(spi_sclk, slave->spi_bus->spi_sclk_speed);
  }
}

void spi_ll_slave_clear_errors(const SPISlavePort *slave) {
  // First, empty the RX FIFO by reading the data. If in TX-only mode, it's possible that
  // received data (0x00s) will be left in the RX FIFO.

  // NOTE: Obviously, do not call this function with transfer in progress.
  while (slave->spi_bus->spi->SR & SPI_SR_RXNE) {
    (void)slave->spi_bus->spi->DR;
  }

  // If the FIFO overflowed, the OVR error will be flagged. Clear the error.
  (void)slave->spi_bus->spi->DR;
  (void)slave->spi_bus->spi->SR;
}
