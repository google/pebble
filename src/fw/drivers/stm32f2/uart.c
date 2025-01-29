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

#include "uart_definitions.h"
#include "drivers/uart.h"

#include "drivers/dma.h"
#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "system/passert.h"

#include "FreeRTOS.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#include <mcu.h>
#include <mcu/interrupts.h>

// The STM32F2 standard peripheral library uses a precision of 100 which is plenty, so we'll do the
// same.
#define DIV_PRECISION (100)


// Initialization / Configuration APIs
////////////////////////////////////////////////////////////////////////////////

typedef enum UARTCR1Flags {
  UARTCR1Flags_Duplex = USART_CR1_TE | USART_CR1_RE,
  UARTCR1Flags_TE = USART_CR1_TE,
  UARTCR1Flags_RE = USART_CR1_RE,
} UARTCR1Flags;

static void prv_init(UARTDevice *dev, bool is_open_drain, UARTCR1Flags cr1_extra_flags) {
  // Enable peripheral clock
  periph_config_enable(dev->periph, dev->rcc_apb_periph);

  // configure GPIO
  const GPIOOType_TypeDef otype = is_open_drain ? GPIO_OType_OD : GPIO_OType_PP;
  if (dev->tx_gpio.gpio) {
    gpio_af_init(&dev->tx_gpio, otype, GPIO_Speed_50MHz, GPIO_PuPd_NOPULL);
  }
  if (dev->rx_gpio.gpio) {
    // half-duplex should only define a TX pin
    PBL_ASSERTN(!dev->half_duplex);
    gpio_af_init(&dev->rx_gpio, otype, GPIO_Speed_50MHz, GPIO_PuPd_NOPULL);
  }

  // configure the UART peripheral control registers
  // - 8-bit word length
  // - no parity
  // - RX / TX enabled
  // - 1 stop bit
  // - no flow control
  dev->periph->CR1 = cr1_extra_flags;
  dev->periph->CR2 = 0;
  dev->periph->CR3 = (dev->half_duplex ? USART_CR3_HDSEL : 0);

  // QEMU doesn't want you to read the DR while the UART is not enabled, but it
  // should be fine to clear errors this way
#if !TARGET_QEMU
  // Clear any stale errors that may be in the registers. This can be accomplished
  // by reading the status register followed by the data register
  (void)dev->periph->SR;
  (void)dev->periph->DR;
#endif

  dev->periph->CR1 |= USART_CR1_UE;

  dev->state->initialized = true;

  // initialize the DMA request
  if (dev->rx_dma) {
    dma_request_init(dev->rx_dma);
  }
}

void uart_init(UARTDevice *dev) {
  prv_init(dev, false /* !is_open_drain */, UARTCR1Flags_Duplex);
}

void uart_init_open_drain(UARTDevice *dev) {
  prv_init(dev, true /* is_open_drain */, UARTCR1Flags_Duplex);
}

void uart_init_tx_only(UARTDevice *dev) {
  prv_init(dev, false /* !is_open_drain */, UARTCR1Flags_TE);
}

void uart_init_rx_only(UARTDevice *dev) {
  prv_init(dev, false /* !is_open_drain */, UARTCR1Flags_RE);
}

void uart_deinit(UARTDevice *dev) {
  dev->periph->CR1 &= ~USART_CR1_UE;
  periph_config_disable(dev->periph, dev->rcc_apb_periph);
  // Change the pins to be digital inputs rather than AF pins. We can't change to analog inputs
  // because those aren't 5V tolerant which these pins may need to be.
  if (dev->tx_gpio.gpio) {
    const InputConfig input_config = {
      .gpio = dev->tx_gpio.gpio,
      .gpio_pin = dev->tx_gpio.gpio_pin
    };
    gpio_input_init(&input_config);
  }
  if (dev->rx_gpio.gpio) {
    const InputConfig input_config = {
      .gpio = dev->rx_gpio.gpio,
      .gpio_pin = dev->rx_gpio.gpio_pin
    };
    gpio_input_init(&input_config);
  }
}

void uart_set_baud_rate(UARTDevice *dev, uint32_t baud_rate) {
  PBL_ASSERTN(dev->state->initialized);

  RCC_ClocksTypeDef RCC_ClocksStatus;
  RCC_GetClocksFreq(&RCC_ClocksStatus);
  uint64_t scaled_apbclock = DIV_PRECISION;
  if ((dev->periph == USART1) || (dev->periph == USART6)) {
    scaled_apbclock *= RCC_ClocksStatus.PCLK2_Frequency;
  } else {
    scaled_apbclock *= RCC_ClocksStatus.PCLK1_Frequency;
  }

  // We need to calculate the divider to get from the clock frequency down to the sampling
  // frequency (samples * baud_rate) and store it in USART_BBR as a fixed-point number with a
  // fractional component equal to the number of samples per symbol. In other words, if OVER8=0,
  // the fractional component will be 4 bits, and if OVER8=1, it will be 3 bits.
  // The formula works out to: DIV = f_clk / (samples * BAUD)

  const bool over8 = dev->periph->CR1 & USART_CR1_OVER8;
  const uint32_t samples = over8 ? 8 : 16;

  // calculate the divider multiplied by DIV_PRECISION
  const uint32_t div_temp = scaled_apbclock / (samples * baud_rate);

  // calculate the mantissa component of BRR
  const uint32_t mantissa = div_temp / DIV_PRECISION;
  // isolate the fraction component by subtracting the mantissa component
  uint32_t fraction = div_temp - mantissa * DIV_PRECISION;
  // convert the fractional component to be in terms of the number of samples (with rounding)
  fraction = (fraction * samples + (DIV_PRECISION / 2)) / DIV_PRECISION;

  if (over8) {
    // 3 bits of fraction
    dev->periph->BRR = (mantissa << 3) | (fraction & 0x7);
  } else {
    // 4 bits of fraction
    dev->periph->BRR = (mantissa << 4) | (fraction & 0xF);
  }
}


// Read / Write APIs
////////////////////////////////////////////////////////////////////////////////

void uart_write_byte(UARTDevice *dev, uint8_t data) {
  PBL_ASSERTN(dev->state->initialized);

  // wait for us to be ready to send
  while (!uart_is_tx_ready(dev)) continue;

  dev->periph->DR = data;
}

uint8_t uart_read_byte(UARTDevice *dev) {
  // read the data regardless since it will clear interrupt flags
  return dev->periph->DR;
}

UARTRXErrorFlags uart_has_errored_out(UARTDevice *dev) {
  uint16_t errors = dev->periph->SR;
  UARTRXErrorFlags flags = {
    .parity_error = (errors & USART_FLAG_PE) != 0,
    .overrun_error = (errors & USART_FLAG_ORE) != 0,
    .framing_error = (errors & USART_FLAG_FE) != 0,
    .noise_detected = (errors & USART_FLAG_NE) != 0,
  };

  return flags;
}

bool uart_is_rx_ready(UARTDevice *dev) {
  return dev->periph->SR & USART_SR_RXNE;
}

bool uart_has_rx_overrun(UARTDevice *dev) {
  return dev->periph->SR & USART_SR_ORE;
}

bool uart_has_rx_framing_error(UARTDevice *dev) {
  return dev->periph->SR & USART_SR_FE;
}

bool uart_is_tx_ready(UARTDevice *dev) {
  return dev->periph->SR & USART_SR_TXE;
}

bool uart_is_tx_complete(UARTDevice *dev) {
  return dev->periph->SR & USART_SR_TC;
}

void uart_wait_for_tx_complete(UARTDevice *dev) {
  while (!uart_is_tx_complete(dev)) continue;
}


// Interrupts
////////////////////////////////////////////////////////////////////////////////

static void prv_set_interrupt_enabled(UARTDevice *dev, bool enabled) {
  if (enabled) {
    PBL_ASSERTN(dev->state->tx_irq_handler || dev->state->rx_irq_handler);
    // enable the interrupt
    NVIC_SetPriority(dev->irq_channel, dev->irq_priority);
    NVIC_EnableIRQ(dev->irq_channel);
  } else {
    // disable the interrupt
    NVIC_DisableIRQ(dev->irq_channel);
  }
}

void uart_set_rx_interrupt_handler(UARTDevice *dev, UARTRXInterruptHandler irq_handler) {
  PBL_ASSERTN(dev->state->initialized);
  dev->state->rx_irq_handler = irq_handler;
}

void uart_set_tx_interrupt_handler(UARTDevice *dev, UARTTXInterruptHandler irq_handler) {
  PBL_ASSERTN(dev->state->initialized);
  dev->state->tx_irq_handler = irq_handler;
}

void uart_set_rx_interrupt_enabled(UARTDevice *dev, bool enabled) {
  PBL_ASSERTN(dev->state->initialized);
  if (enabled) {
    dev->state->rx_int_enabled = true;
    dev->periph->CR1 |= USART_CR1_RXNEIE;
    prv_set_interrupt_enabled(dev, true);
  } else {
    // disable interrupt if TX is also disabled
    prv_set_interrupt_enabled(dev, dev->state->tx_int_enabled);
    dev->periph->CR1 &= ~USART_CR1_RXNEIE;
    dev->state->rx_int_enabled = false;
  }
}

void uart_set_tx_interrupt_enabled(UARTDevice *dev, bool enabled) {
  PBL_ASSERTN(dev->state->initialized);
  if (enabled) {
    dev->state->tx_int_enabled = true;
    dev->periph->CR1 |= USART_CR1_TXEIE;
    prv_set_interrupt_enabled(dev, true);
  } else {
    // disable interrupt if RX is also disabled
    prv_set_interrupt_enabled(dev, dev->state->rx_int_enabled);
    dev->periph->CR1 &= ~USART_CR1_TXEIE;
    dev->state->tx_int_enabled = false;
  }
}

void uart_irq_handler(UARTDevice *dev) {
  PBL_ASSERTN(dev->state->initialized);
  bool should_context_switch = false;
  if (dev->state->rx_irq_handler && dev->state->rx_int_enabled) {
    const UARTRXErrorFlags err_flags = {
      .overrun_error = uart_has_rx_overrun(dev),
      .framing_error = uart_has_rx_framing_error(dev),
    };
    if (dev->state->rx_dma_buffer) {
      // process bytes from the DMA buffer
      const uint32_t dma_length = dev->state->rx_dma_length;
      const uint32_t next_idx = dma_length - dma_request_get_current_data_counter(dev->rx_dma);
      // make sure we didn't underflow the index
      PBL_ASSERTN(next_idx < dma_length);
      while (dev->state->rx_dma_index != next_idx) {
        const uint8_t data = dev->state->rx_dma_buffer[dev->state->rx_dma_index];
        if (dev->state->rx_irq_handler(dev, data, &err_flags)) {
          should_context_switch = true;
        }
        if (++dev->state->rx_dma_index == dma_length) {
          dev->state->rx_dma_index = 0;
        }
      }
      // explicitly clear error flags since we're not reading from the data register
      uart_clear_all_interrupt_flags(dev);
    } else {
      const bool has_byte = uart_is_rx_ready(dev);
      // read the data register regardless to clear the error flags
      const uint8_t data = uart_read_byte(dev);
      if (has_byte) {
        if (dev->state->rx_irq_handler(dev, data, &err_flags)) {
          should_context_switch = true;
        }
      }
    }
  }
  if (dev->state->tx_irq_handler && dev->state->tx_int_enabled && uart_is_tx_ready(dev)) {
    if (dev->state->tx_irq_handler(dev)) {
      should_context_switch = true;
    }
  }
  portEND_SWITCHING_ISR(should_context_switch);
}

void uart_clear_all_interrupt_flags(UARTDevice *dev) {
  dev->periph->SR &= ~(USART_SR_TXE | USART_SR_RXNE | USART_SR_ORE);
}


// DMA
////////////////////////////////////////////////////////////////////////////////

void uart_start_rx_dma(UARTDevice *dev, void *buffer, uint32_t length) {
  dev->periph->CR3 |= USART_CR3_DMAR;
  dma_request_start_circular(dev->rx_dma, buffer, (void *)&dev->periph->DR, length, NULL, NULL);
  dev->state->rx_dma_index = 0;
  dev->state->rx_dma_length = length;
  dev->state->rx_dma_buffer = buffer;
}

void uart_stop_rx_dma(UARTDevice *dev) {
  dev->state->rx_dma_buffer = NULL;
  dev->state->rx_dma_length = 0;
  dma_request_stop(dev->rx_dma);
  dev->periph->CR3 &= ~USART_CR3_DMAR;
}

void uart_clear_rx_dma_buffer(UARTDevice *dev) {
  dev->state->rx_dma_index = dev->state->rx_dma_length -
                             dma_request_get_current_data_counter(dev->rx_dma);
}
