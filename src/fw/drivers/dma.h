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


typedef const struct DMARequest DMARequest;

//! The type of function that's called from an ISR to notify the consumer that a direct DMA transfer
//! has completed.
typedef bool (*DMADirectRequestHandler)(DMARequest *this, void *context);
//! The type of function that's called from an ISR to notify the consumer that a circular DMA
//! transfer is either complete or half transferred (specified via the `is_complete` parameter).
typedef bool (*DMACircularRequestHandler)(DMARequest *this, void *context, bool is_complete);


//! Initializes a DMA transfer and its underlying stream / controller as necessary. This is called
//! from the consumer's init function (i.e. uart_init() or compositor_dma_init()).
void dma_request_init(DMARequest *this);

//! Starts a direct DMA transfer which automatically stops and calls a callback (from an ISR) once
//! it's complete. The length should be specified in bytes.
void dma_request_start_direct(DMARequest *this, void *dst, const void *src, uint32_t length,
                              DMADirectRequestHandler handler, void *context);

//! Starts a circular DMA transfer which calls the callback for when the transfer is both complete
//! and half complete. The length should be specified in bytes.
//! @note The destination address must not be in a cacheable region of memory (i.e. SRAM on the F7).
//! See the comment within dma.c for more info.
void dma_request_start_circular(DMARequest *this, void *dst, const void *src, uint32_t length,
                                DMACircularRequestHandler handler, void *context);

//! Stops an in-progress DMA transfer (typically only used for circular transfers, otherwise the
//! transfer will be stopped when it completes)
void dma_request_stop(DMARequest *this);

//! Returns whether or not the transfer is currently in progress
bool dma_request_in_progress(DMARequest *this);

//! Gets the current value of the underlying DMA stream's data counter
uint32_t dma_request_get_current_data_counter(DMARequest *this);

//! Get the current value of the transfer error flag and clears it
bool dma_request_get_and_clear_transfer_error(DMARequest *this);

//! Allows for disabling of auto-incrementing of memory buffer addresses. This is currently only
//! used by SPI in order to allow receiving of data by sending the same dummy value over and over.
void dma_request_set_memory_increment_disabled(DMARequest *this, bool disabled);
