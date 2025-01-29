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

#include "drivers/dma.h"

#include "drivers/mpu.h"
#include "drivers/periph_config.h"
#include "drivers/stm32f2/dma_definitions.h"
#include "mcu/cache.h"
#include "system/passert.h"

#define CMSIS_COMPATIBLE
#include <mcu.h>

#include "FreeRTOS.h"

#include <stdatomic.h>


#define CHSEL_OFFSET __builtin_ctz(DMA_SxCR_CHSEL)

// all interrupt flags for streams 0-3
#define ALL_INTERRUPT_FLAGS_L(s) (DMA_LIFCR_CTCIF##s | DMA_LIFCR_CHTIF##s | DMA_LIFCR_CTEIF##s | \
                                    DMA_LIFCR_CDMEIF##s | DMA_LIFCR_CFEIF##s)
// all interrupt flags for streams 4-7
#define ALL_INTERRUPT_FLAGS_H(s) (DMA_HIFCR_CTCIF##s | DMA_HIFCR_CHTIF##s | DMA_HIFCR_CTEIF##s | \
                                  DMA_HIFCR_CDMEIF##s | DMA_HIFCR_CFEIF##s)


// Stream Interrupt flag helper functions
////////////////////////////////////////////////////////////////////////////////

static void prv_clear_all_interrupt_flags(DMARequest *this) {
  switch ((uintptr_t)this->stream->periph) {
    case (uintptr_t)DMA1_Stream0:
    case (uintptr_t)DMA2_Stream0:
      this->stream->controller->periph->LIFCR = ALL_INTERRUPT_FLAGS_L(0);
      break;
    case (uintptr_t)DMA1_Stream1:
    case (uintptr_t)DMA2_Stream1:
      this->stream->controller->periph->LIFCR = ALL_INTERRUPT_FLAGS_L(1);
      break;
    case (uintptr_t)DMA1_Stream2:
    case (uintptr_t)DMA2_Stream2:
      this->stream->controller->periph->LIFCR = ALL_INTERRUPT_FLAGS_L(2);
      break;
    case (uintptr_t)DMA1_Stream3:
    case (uintptr_t)DMA2_Stream3:
      this->stream->controller->periph->LIFCR = ALL_INTERRUPT_FLAGS_L(3);
      break;
    case (uintptr_t)DMA1_Stream4:
    case (uintptr_t)DMA2_Stream4:
      this->stream->controller->periph->HIFCR = ALL_INTERRUPT_FLAGS_H(4);
      break;
    case (uintptr_t)DMA1_Stream5:
    case (uintptr_t)DMA2_Stream5:
      this->stream->controller->periph->HIFCR = ALL_INTERRUPT_FLAGS_H(5);
      break;
    case (uintptr_t)DMA1_Stream6:
    case (uintptr_t)DMA2_Stream6:
      this->stream->controller->periph->HIFCR = ALL_INTERRUPT_FLAGS_H(6);
      break;
    case (uintptr_t)DMA1_Stream7:
    case (uintptr_t)DMA2_Stream7:
      this->stream->controller->periph->HIFCR = ALL_INTERRUPT_FLAGS_H(7);
      break;
    default:
      WTF;
  }
}

static void prv_get_and_clear_interrupt_flags(DMARequest *this, bool *has_tc, bool *has_ht) {
  switch ((uintptr_t)this->stream->periph) {
    case (uintptr_t)DMA1_Stream0:
    case (uintptr_t)DMA2_Stream0:
      *has_tc = this->stream->controller->periph->LISR & DMA_LISR_TCIF0;
      *has_ht = this->stream->controller->periph->LISR & DMA_LISR_HTIF0;
      break;
    case (uintptr_t)DMA1_Stream1:
    case (uintptr_t)DMA2_Stream1:
      *has_tc = this->stream->controller->periph->LISR & DMA_LISR_TCIF1;
      *has_ht = this->stream->controller->periph->LISR & DMA_LISR_HTIF1;
      break;
    case (uintptr_t)DMA1_Stream2:
    case (uintptr_t)DMA2_Stream2:
      *has_tc = this->stream->controller->periph->LISR & DMA_LISR_TCIF2;
      *has_ht = this->stream->controller->periph->LISR & DMA_LISR_HTIF2;
      break;
    case (uintptr_t)DMA1_Stream3:
    case (uintptr_t)DMA2_Stream3:
      *has_tc = this->stream->controller->periph->LISR & DMA_LISR_TCIF3;
      *has_ht = this->stream->controller->periph->LISR & DMA_LISR_HTIF3;
      break;
    case (uintptr_t)DMA1_Stream4:
    case (uintptr_t)DMA2_Stream4:
      *has_tc = this->stream->controller->periph->HISR & DMA_HISR_TCIF4;
      *has_ht = this->stream->controller->periph->HISR & DMA_HISR_HTIF4;
      break;
    case (uintptr_t)DMA1_Stream5:
    case (uintptr_t)DMA2_Stream5:
      *has_tc = this->stream->controller->periph->HISR & DMA_HISR_TCIF5;
      *has_ht = this->stream->controller->periph->HISR & DMA_HISR_HTIF5;
      break;
    case (uintptr_t)DMA1_Stream6:
    case (uintptr_t)DMA2_Stream6:
      *has_tc = this->stream->controller->periph->HISR & DMA_HISR_TCIF6;
      *has_ht = this->stream->controller->periph->HISR & DMA_HISR_HTIF6;
      break;
    case (uintptr_t)DMA1_Stream7:
    case (uintptr_t)DMA2_Stream7:
      *has_tc = this->stream->controller->periph->HISR & DMA_HISR_TCIF7;
      *has_ht = this->stream->controller->periph->HISR & DMA_HISR_HTIF7;
      break;
    default:
      WTF;
  }
  prv_clear_all_interrupt_flags(this);
}


// Controller clock control
////////////////////////////////////////////////////////////////////////////////

static void prv_use_controller(DMAController *this) {
  const int old_refcount = atomic_fetch_add(&this->state->refcount, 1);
  if (old_refcount == 0) {
    periph_config_enable(this->periph, this->rcc_bit);
  }
}

static void prv_release_controller(DMAController *this) {
  const int refcount = atomic_fetch_sub(&this->state->refcount, 1) - 1;
  PBL_ASSERT(refcount >= 0, "Attempted to release a DMA controller that is not in use!");
  if (refcount == 0) {
    periph_config_disable(this->periph, this->rcc_bit);
  }
}


// Initialization
////////////////////////////////////////////////////////////////////////////////

static uint32_t prv_get_data_size_bytes(DMARequest *this) {
  switch (this->data_size) {
    case DMARequestDataSize_Byte:
      return 1;
    case DMARequestDataSize_HalfWord:
      return 2;
    case DMARequestDataSize_Word:
      return 4;
    default:
      WTF;
      return 0;
  }
}

static void prv_set_constant_config(DMARequest *this) {
  uint32_t cr_value = 0;
  uint32_t fcr_value = 0;

  // set the channel
  PBL_ASSERTN((this->channel & (DMA_SxCR_CHSEL >> CHSEL_OFFSET)) == this->channel);
  cr_value |= this->channel << CHSEL_OFFSET;

  // set the priority
  PBL_ASSERTN((this->priority & DMA_SxCR_PL) == this->priority);
  cr_value |= this->priority;

  // set the data size
  PBL_ASSERTN((this->data_size & (DMA_SxCR_MSIZE | DMA_SxCR_PSIZE)) == this->data_size);
  cr_value |= this->data_size;

  // set the direction
  PBL_ASSERTN((this->type & (DMA_SxCR_DIR_0 | DMA_SxCR_DIR_1)) == this->type);
  cr_value |= this->type;

  // set the incrementing modes, FIFO, and burst sizes
  switch (this->type) {
    case DMARequestType_MemoryToMemory:
      // memory and peripheral burst of 8 (found to be fastest based on testing on Snowy / Robert)
      cr_value |= DMA_SxCR_MBURST_1 | DMA_SxCR_PBURST_1;
      // memory and peripheral incrementing enabled
      cr_value |= DMA_SxCR_MINC | DMA_SxCR_PINC;
      // enable the FIFO with a threshold of 1/2 full
      fcr_value |= DMA_SxFCR_DMDIS;
      fcr_value |= DMA_SxFCR_FTH_0;
      break;
    case DMARequestType_MemoryToPeripheral:
    case DMARequestType_PeripheralToMemory:
      // just enable incrementing of memory address (no FIFO, single burst)
      cr_value |= DMA_SxCR_MINC;
      break;
    default:
      WTF;
  }

  prv_use_controller(this->stream->controller);
  // make sure the stream is disabled before trying to configure
  PBL_ASSERTN((this->stream->periph->CR & DMA_SxCR_EN) == 0);
  this->stream->periph->CR = cr_value;
  this->stream->periph->FCR = fcr_value;
  prv_release_controller(this->stream->controller);

  // Configure and enable the IRQ if necessary (DMA interrupts enabled later)
  if (this->irq_priority != IRQ_PRIORITY_INVALID) {
    NVIC_SetPriority(this->stream->irq_channel, this->irq_priority);
    NVIC_EnableIRQ(this->stream->irq_channel);
  }
}

void dma_request_init(DMARequest *this) {
  if (this->state->initialized) {
    return;
  }

  // we only support 1 transfer per stream so assert that the stream isn't already initialized
  PBL_ASSERTN(!this->stream->state->initialized);
  // sanity check that the stream and controller are valid
  switch ((uintptr_t)this->stream->periph) {
    case (uintptr_t)DMA1_Stream0:
    case (uintptr_t)DMA1_Stream1:
    case (uintptr_t)DMA1_Stream2:
    case (uintptr_t)DMA1_Stream3:
    case (uintptr_t)DMA1_Stream4:
    case (uintptr_t)DMA1_Stream5:
    case (uintptr_t)DMA1_Stream6:
    case (uintptr_t)DMA1_Stream7:
      PBL_ASSERTN(this->stream->controller->periph == DMA1);
      break;
    case (uintptr_t)DMA2_Stream0:
    case (uintptr_t)DMA2_Stream1:
    case (uintptr_t)DMA2_Stream2:
    case (uintptr_t)DMA2_Stream3:
    case (uintptr_t)DMA2_Stream4:
    case (uintptr_t)DMA2_Stream5:
    case (uintptr_t)DMA2_Stream6:
    case (uintptr_t)DMA2_Stream7:
      PBL_ASSERTN(this->stream->controller->periph == DMA2);
      break;
    default:
      WTF;
  }
  this->stream->state->initialized = true;

  prv_set_constant_config(this);
  this->state->initialized = true;
}


// Transfer APIs
////////////////////////////////////////////////////////////////////////////////

static void prv_validate_memory(DMARequest *this, void *dst, const void *src, uint32_t length) {
  if (mpu_memory_is_cacheable(src)) {
    // Flush the source buffer from cache so that SRAM has the correct data.
    uintptr_t aligned_src = (uintptr_t)src;
    size_t aligned_length = length;
    dcache_align(&aligned_src, &aligned_length);
    dcache_flush((const void *)aligned_src, aligned_length);
  }

  const uint32_t alignment_mask = prv_get_data_size_bytes(this) - 1;
  if (mpu_memory_is_cacheable(dst)) {
    // If a cache line within the dst gets evicted while we do the transfer, it'll corrupt SRAM, so
    // just invalidate it now.
    dcache_invalidate(dst, length);
    // since the dst address is cacheable, it needs to be aligned to a cache line and
    // the length must be an even multiple of cache lines
    const uint32_t dst_alignment_mask = dcache_alignment_mask_minimum(alignment_mask);
    PBL_ASSERTN(((length & dst_alignment_mask) == 0) &&
                 (((uintptr_t)dst & dst_alignment_mask) == 0) &&
                 (((uintptr_t)src & alignment_mask) == 0));
  } else {
    PBL_ASSERTN(((length & alignment_mask) == 0) &&
                 (((uintptr_t)dst & alignment_mask) == 0) &&
                 (((uintptr_t)src & alignment_mask) == 0));
  }

#if PLATFORM_ROBERT
  // There is an erratum in the STM32F7xx MCUs in which causes DMA transfers
  // that read from the DTCM to read corrupted data if the MCU enters sleep mode
  // during the transfer. Note that writes to DTCM will not be corrupted.
  extern const char __DTCM_RAM_size__[];
  PBL_ASSERT((uintptr_t)src >= (RAMDTCM_BASE + (uintptr_t)__DTCM_RAM_size__) ||
             ((uintptr_t)src + length) <= RAMDTCM_BASE,
             "DMA transfer will be corrupted if MCU enters sleep mode");
#endif
}

static void prv_request_start(DMARequest *this, void *dst, const void *src, uint32_t length,
                              DMARequestTransferType transfer_type) {
  this->state->transfer_dst = dst;
  this->state->transfer_length = length;
  prv_validate_memory(this, dst, src, length);
  prv_use_controller(this->stream->controller);

  // set the data length in terms of units (we assert that it's an even number in
  // prv_validate_memory above)
  this->stream->periph->NDTR = length / prv_get_data_size_bytes(this);

  // set the addresses
  switch (this->type) {
    case DMARequestType_MemoryToMemory:
    case DMARequestType_PeripheralToMemory:
      this->stream->periph->PAR = (uint32_t)src;
      this->stream->periph->M0AR = (uint32_t)dst;
      break;
    case DMARequestType_MemoryToPeripheral:
      this->stream->periph->PAR = (uint32_t)dst;
      this->stream->periph->M0AR = (uint32_t)src;
      break;
    default:
      WTF;
  }

  // set the mode and enable the appropriate interrupts
  switch (transfer_type) {
    case DMARequestTransferType_Direct:
      this->stream->periph->CR &= ~DMA_SxCR_CIRC;
      this->stream->periph->CR |=  DMA_SxCR_TCIE;
      break;
    case DMARequestTransferType_Circular:
      this->stream->periph->CR |= DMA_SxCR_CIRC | DMA_SxCR_HTIE | DMA_SxCR_TCIE;
      break;
    case DMARequestTransferType_None:
    default:
      WTF;
  }

  // "As a general recommendation, it is advised to clear all flags in the DMA_LIFCR and
  // DMA_HIFCR registers before starting a new transfer." -- STM32 AN4031 (DM00046011.pdf)
  // "Before setting EN bit to '1' to start a new transfer, the event flags corresponding to the
  // stream in DMA_LISR or DMA_HISR register must be" -- Page 213, STM RM0402
  prv_clear_all_interrupt_flags(this);

  // Start the DMA transfer
  this->stream->periph->CR |= DMA_SxCR_EN;
}

void dma_request_start_direct(DMARequest *this, void *dst, const void *src, uint32_t length,
                              DMADirectRequestHandler handler, void *context) {
  PBL_ASSERTN(this->state->initialized);

  PBL_ASSERTN(this->state->transfer_type == DMARequestTransferType_None);
  this->state->transfer_type = DMARequestTransferType_Direct;

  this->state->direct_transfer_handler = handler;
  this->state->context = context;
  PBL_ASSERTN(!this->stream->state->current_request);
  this->stream->state->current_request = this;

  prv_request_start(this, dst, src, length, DMARequestTransferType_Direct);
}

void dma_request_start_circular(DMARequest *this, void *dst, const void *src, uint32_t length,
                                DMACircularRequestHandler handler, void *context) {
  PBL_ASSERTN(this->state->initialized);

  PBL_ASSERTN(this->state->transfer_type == DMARequestTransferType_None);
  this->state->transfer_type = DMARequestTransferType_Circular;

  this->state->circular_transfer_handler = handler;
  this->state->context = context;
  PBL_ASSERTN(!this->stream->state->current_request);
  this->stream->state->current_request = this;

  // TODO: We don't currently support DMA'ing into a cacheable region of memory (i.e. SRAM) for
  // circular transfers. The reason is that it gets complicated because the consumer might be
  // reading from the buffer at any time (as UART does), as opposed to direct transfers where the
  // consumer is always reading only after the transfer has completed.
  PBL_ASSERTN(!mpu_memory_is_cacheable(dst));
  prv_request_start(this, dst, src, length, DMARequestTransferType_Circular);
}

void dma_request_stop(DMARequest *this) {
  if (this->state->transfer_type == DMARequestTransferType_None) {
    return;
  }

  this->stream->periph->CR &= ~(DMA_SxCR_HTIE | DMA_SxCR_TCIE);

  // disable the stream
  this->stream->periph->CR &= ~DMA_SxCR_EN;
  while ((this->stream->periph->CR & DMA_SxCR_EN) != 0) {}
  prv_release_controller(this->stream->controller);

  // clean up our state
  this->state->transfer_dst = NULL;
  this->state->transfer_length = 0;
  this->state->direct_transfer_handler = NULL;
  this->state->circular_transfer_handler = NULL;
  this->state->context = NULL;
  this->state->transfer_type = DMARequestTransferType_None;
  this->stream->state->current_request = NULL;
}


uint32_t dma_request_get_current_data_counter(DMARequest *this) {
  return this->stream->periph->NDTR;
}

bool dma_request_in_progress(DMARequest *this) {
  return this->state->transfer_type != DMARequestTransferType_None;
}

void dma_request_set_memory_increment_disabled(DMARequest *this, bool disabled) {
  prv_use_controller(this->stream->controller);
  if (disabled) {
    this->stream->periph->CR &= ~DMA_SxCR_MINC;
  } else {
    this->stream->periph->CR |= DMA_SxCR_MINC;
  }
  prv_release_controller(this->stream->controller);
}


// ISR
////////////////////////////////////////////////////////////////////////////////

void dma_stream_irq_handler(DMAStream *stream) {
  bool should_context_switch = false;

  DMARequest *this = stream->state->current_request;
  PBL_ASSERTN(this);
  PBL_ASSERTN(this->stream == stream);

  bool has_tc;
  bool has_ht;
  prv_get_and_clear_interrupt_flags(this, &has_tc, &has_ht);
  if (!has_tc && !has_ht) {
    // we shouldn't be here
    portEND_SWITCHING_ISR(should_context_switch);
    return;
  }

  switch (this->state->transfer_type) {
    case DMARequestTransferType_Direct:
      if (has_tc) {
        if (mpu_memory_is_cacheable(this->state->transfer_dst)) {
          dcache_invalidate(this->state->transfer_dst, this->state->transfer_length);
        }

        // Automatically stop the transfer before calling the handler so that the handler can start
        // another transfer immediately. We need to preserve the handler and context first.
        DMADirectRequestHandler handler = this->state->direct_transfer_handler;
        void *context = this->state->context;
        dma_request_stop(this);

        if (handler && handler(this, context)) {
          should_context_switch = true;
        }
      }
      break;
    case DMARequestTransferType_Circular:
      if (this->state->circular_transfer_handler) {
        if (this->state->circular_transfer_handler(this, this->state->context, has_tc)) {
          should_context_switch = true;
        }
      }
      break;
    case DMARequestTransferType_None:
    default:
      WTF;
  }
  portEND_SWITCHING_ISR(should_context_switch);
}
