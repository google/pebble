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
#include <stddef.h>
#include <stdint.h>

struct _tagHeapInfo_t;
typedef struct _tagHeapInfo_t HeapInfo_t;

typedef void (*LockFunction)(void*);
typedef void (*UnlockFunction)(void*);

typedef struct HeapLockImpl {
  LockFunction lock_function;
  UnlockFunction unlock_function;
  void* lock_context;
} HeapLockImpl;

typedef void (*DoubleFreeHandler)(void*);
typedef void (*CorruptionHandler)(void*);

typedef struct Heap {
  // These HeapInfo_t structure pointers are initialized to the start and the end of the heap area.
  // The begin will point to the first block that's in the heap area, where the end is actually a
  // pointer to the first block *after* the heap area. This means the last block in the heap isn't
  // actually at end, but at end - begin->PrevSize.
  HeapInfo_t *begin;
  HeapInfo_t *end;

  //! Number of allocated bytes, including headers
  unsigned int current_size;
  //! Peak number of allocated bytes, including headers
  unsigned int high_water_mark;

  HeapLockImpl lock_impl;

  DoubleFreeHandler double_free_handler;
  bool fuzz_on_free;

  void *corrupt_block;
  CorruptionHandler corruption_handler;
} Heap;

//! Initialize the heap inside the specified boundaries, zero-ing out the free
//! list data structure.
//!     @note Assumes 0 is not a valid address for allocation.
//!     @param start The start of the heap will be the first int-aligned address >= start
//!     @param end The end of the heap will be the last sizeof(HeaderBlock) aligned
//!         address that is < end
//!     @param fuzz_on_free if true, memsets memory contents to junk values upon free in order to
//!                         catch bad accesses more quickly
void heap_init(Heap* const heap, void* start, void* end, bool fuzz_on_free);

//! Configure this heap for thread safety using the given locking implementation
void heap_set_lock_impl(Heap *heap, HeapLockImpl lock_impl);

//! Configure the heap with a pointer that gets called when a double free is detected.
//! If this isn't configured on a heap, the default behaviour is to trigger a PBL_CROAK.
void heap_set_double_free_handler(Heap *heap, DoubleFreeHandler double_free_handler);

//! Configure the heap with a pointer that gets called when (and if) corruption is detected.
//! If this isn't configured on a heap, the default behaviour is to trigger a PBL_CROAK.
void heap_set_corruption_handler(Heap *heap, CorruptionHandler corruption_handler);

//! Allocate a fragment of memory on the given heap. Tries to avoid
//! fragmentation by obtaining memory requests larger than LARGE_SIZE from the
//! endo of the buffer, while small fragments are taken from the start of the
//! buffer.
//! @note heap_init() must be called prior to using heap_malloc().
//! @param nbytes Number of bytes to be allocated. Must be > 0.
//! @param client_pc The PC register of the client who caused this malloc. Only used when
//!                  MALLOC_INSTRUMENTATION is defined.
//! @return A pointer to the start of the allocated memory
void* heap_malloc(Heap* const heap, unsigned long nbytes, uintptr_t client_pc);

//! Return memory to free list. Where possible, make contiguous blocks of free
//! memory. The function tries to verify that the structure is a valid fragment
//! structure before the memory is freed. When a fragment is freed, adjacent
//! free fragments may be combined.
//!     @note heap_init() must be called prior to using heap_free().
//!         otherwise, the free list will be NULL.)
//!     @note Assumes that 0 is not a valid address for allocation.
void heap_free(Heap* const heap, void* ptr, uintptr_t client_pc);

//! Allocate a new block of the given size, and copy over the data at ptr into
//! the new block. If the new size is smaller than the old size, will only copy
//! over as much data as possible. Frees ptr.
//! @param ptr Points to the memory region to re-allocate.
//! @param nbytes The total number of bytes to allocate.
//! @param client_pc The PC register of the client who caused this malloc. Only used when
//!                  MALLOC_INSTRUMENTATION is defined.
void* heap_realloc(Heap* const heap, void *ptr, unsigned long nbytes, uintptr_t client_pc);

//! Allocate a buffer to hold anything. The initial contents of the buffer
//! are zero'd.
void* heap_zalloc(Heap* const heap, size_t size, uintptr_t client_pc);

//! Allocate a buffer to hold an array of count elements, each of size (in bytes)
//! and initializes all bits to zero.
void* heap_calloc(Heap* const heap, size_t count, size_t size, uintptr_t client_pc);

//! @return True if ptr is on the given heap, false otherwise.
bool heap_contains_address(Heap* const heap, void* ptr);

//! @return True if ptr is allocated on the given heap, false otherwise.
bool heap_is_allocated(Heap* const heap, void* ptr);

//! @return The size of the heap in bytes
size_t heap_size(const Heap *heap);

//! @return the fewest amount of bytes that the given heap had free
uint32_t heap_get_minimum_headroom(Heap *heap);

//! Used for debugging.
//! Calculates and outputs the current memory usage on the given heap.
//!     @param used Output, will contain the number of bytes currently
//!         allocated and in use.
//!     @param free Output, will contain the number of unallocated bytes.
//!     @param max_free Output, will contain size of the largest unallocated
//!         fragment.
void heap_calc_totals(Heap* const heap, unsigned int *used, unsigned int *free, unsigned int *max_free);

void heap_dump_malloc_instrumentation_to_dbgserial(Heap *heap);
