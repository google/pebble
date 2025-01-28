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

#include "util/heap.h"

#include "util/assert.h"
#include "util/math.h"
#include "util/logging.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* The following defines a type that is the size in bytes of the     */
/* desired alignment of each datr fragment.                          */
typedef unsigned long Alignment_t;

/* The following defines the byte boundary size that has been        */
/* specified size if the alignment data.                             */
#define ALIGNMENT_SIZE          sizeof(Alignment_t)

/* The following structure is used to align data fragments on a     */
/* specified memory boundary.                                        */
typedef union _tagAlignmentStruct_t
{
  Alignment_t   AlignmentValue;
  unsigned char ByteValue;
} AlignmentStruct_t;

/* The following defines the size in bytes of a data fragment that is*/
/* considered a large value.  Allocations that are equal to and      */
/* larger than this value will be allocated from the end of the      */
/* buffer.                                                           */
#define LARGE_SIZE              (256/ALIGNMENT_SIZE)

//! The maximum size of the heap as a number of ALIGNMENT_SIZE
#define SEGMENT_SIZE_MAX      (0x7FFF)

/* The following defines the minimum size (in alignment units) of a  */
/* fragment that is considered useful.  The value is used when trying*/
/* to determine if a fragment that is larger than the requested size */
/* can be broken into 2 fragments leaving a fragment that is of the   */
/* requested size and one that is at least as larger as the          */
/* MINIMUM_MEMORY_SIZE.                                              */
#define MINIMUM_MEMORY_SIZE     1

/* The following defines the structure that describes a memory       */
/* fragment.                                                         */
typedef struct _tagHeapInfo_t
{
  //! Size of the preceding segment, measured in units of ALIGNMENT_SIZE, including the size of this header
  uint16_t PrevSize;

  //! Whether or not this block is currently allocated (vs being free).
  bool is_allocated:1;

  //! Size of this segment, measured in units of ALIGNMENT_SIZE, including this size of this header
  uint16_t Size:15;

#ifdef MALLOC_INSTRUMENTATION
  uintptr_t pc; //<! The address that called malloc.
#endif

  //! This is the actual buffer that's returned to the caller. We use this struct to make
  //! sure the buffer we return is appropriately aligned.
  AlignmentStruct_t Data;
} HeapInfo_t;


//! The size of a block in units of Alignment_t, including the header and including _x words of data.
#define HEAP_INFO_BLOCK_SIZE(_x) ((offsetof(HeapInfo_t, Data) / ALIGNMENT_SIZE) + (_x))

//! Convert a pointer to the Data member to a pointer to the HeapInfo_t header
#define HEAP_INFO_FOR_PTR(ptr) (HeapInfo_t *)(((Alignment_t *)ptr) - HEAP_INFO_BLOCK_SIZE(0))

_Static_assert((offsetof(HeapInfo_t, Data) % ALIGNMENT_SIZE) == 0, "Heap not properly aligned.");

//! Heap is assumed corrupt if expr does not evaluate true
#define HEAP_ASSERT_SANE(heap, expr, log_addr) \
            if (!(expr)) { prv_handle_corruption(heap, log_addr); }

//! Lock the heap, using whatever behaviour the heap has configured using heap_set_lock_impl
static void heap_lock(Heap* heap) {
  if (heap->lock_impl.lock_function) {
    heap->lock_impl.lock_function(heap->lock_impl.lock_context);
  }
}

//! Unlock the heap, using whatever behaviour the heap has configured using heap_set_lock_impl
static void heap_unlock(Heap* heap) {
  if (heap->lock_impl.unlock_function) {
    heap->lock_impl.unlock_function(heap->lock_impl.lock_context);
  }

  // Handle any heap corruption that may have been detected while the heap was locked.
  if (heap->corrupt_block != NULL) {
    heap->corruption_handler(heap->corrupt_block);
    heap->corrupt_block = NULL;
  }
}

static void prv_handle_corruption(Heap * const heap, void *ptr) {
  if (heap->corruption_handler) {
    heap->corrupt_block = ptr;
    return;
  }
  UTIL_ASSERT(0); // Error: Heap corrupt around <ptr>
}

static HeapInfo_t *find_segment(Heap* const heap, unsigned long n_units);
static HeapInfo_t *allocate_block(Heap* const heap, unsigned long n_units, HeapInfo_t* heap_info_ptr);

//! Advance the block pointer to the next block.
static HeapInfo_t* get_next_block(Heap * const heap, HeapInfo_t* block) {
  HEAP_ASSERT_SANE(heap, block->Size != 0, block);
  return (HeapInfo_t *)(((Alignment_t *)block) + block->Size);
}

//! Move the block pointer back to the previous block.
static HeapInfo_t* get_previous_block(Heap * const heap, HeapInfo_t* block) {
  HEAP_ASSERT_SANE(heap, block->PrevSize != 0, block);
  return (HeapInfo_t *)(((Alignment_t *)block) - block->PrevSize);
}

static void prv_calc_totals(Heap* const heap, unsigned int *used, unsigned int *free, unsigned int *max_free) {
  HeapInfo_t    *heap_info_ptr;
  uint16_t      free_segments;
  uint16_t      alloc_segments;

  /* Initialize the return values.                                  */
  *used         = 0;
  *free         = 0;
  *max_free      = 0;
  free_segments  = 0;
  alloc_segments = 0;
  heap_info_ptr   = heap->begin;

  do {
    /* Check to see if the current fragment is marked as free.     */
    if(heap_info_ptr->is_allocated) {
      alloc_segments++;

      *used += heap_info_ptr->Size * ALIGNMENT_SIZE;
    } else {
      free_segments++;

      /* Accumulate the total number of free Bytes.               */
      *free += heap_info_ptr->Size * ALIGNMENT_SIZE;

      /* Check to see if the current fragment is larger that any  */
      /* we have seen and update the Max Value if it is larger.   */
      if(heap_info_ptr->Size > *max_free) {
        *max_free = heap_info_ptr->Size * ALIGNMENT_SIZE;
      }
    }

    /* Adjust the pointer to the next entry.                       */
    heap_info_ptr = get_next_block(heap, heap_info_ptr);

  } while (heap_info_ptr < heap->end);

  UTIL_ASSERT(heap_info_ptr == heap->end);

  char format_str[80];
  snprintf(format_str, sizeof(format_str), "alloc: %u (%u bytes), free: %u (%u bytes)",
           alloc_segments, *used, free_segments, *free);
  util_dbgserial_str(format_str);
}

void heap_calc_totals(Heap* const heap, unsigned int *used, unsigned int *free, unsigned int *max_free) {
  UTIL_ASSERT(heap);

  /* Verify that the parameters that were passed in appear valid.      */
  if((used) && (free) && (max_free) && (heap->begin)) {
    heap_lock(heap);
    prv_calc_totals(heap, used, free, max_free);
    heap_unlock(heap);
  }
}

void heap_init(Heap* const heap, void* start, void* end, bool fuzz_on_free) {
  UTIL_ASSERT(start && end);

  // Align the pointer by advancing it to the next boundary.
  start = (void*)((((uintptr_t) start) + (sizeof(Alignment_t) - 1)) & ~(sizeof(Alignment_t) - 1));
  end = (void*) (((uintptr_t) end) & ~(sizeof(Alignment_t) - 1));

  // Calculate the size of the heap in alignment units.
  uint32_t heap_size = ((uintptr_t)end - (uintptr_t)start) / ALIGNMENT_SIZE;
  // If we have more space than we can use, just limit it to the usable space. This limit is caused
  // by the width of .Size and .PrevSize in HeapInfo_t
  heap_size = MIN(SEGMENT_SIZE_MAX, heap_size);
  end = ((char*) start) + (heap_size * ALIGNMENT_SIZE);

  memset(start, 0, heap_size * ALIGNMENT_SIZE);

  *heap = (Heap) {
    .begin = start,
    .end = end,
    .fuzz_on_free = fuzz_on_free
  };

  *(heap->begin) = (HeapInfo_t) {
    .PrevSize = heap_size,
    .is_allocated = false,
    .Size = heap_size
  };
}

void heap_set_lock_impl(Heap *heap, HeapLockImpl lock_impl) {
  heap->lock_impl = lock_impl;
}

void heap_set_double_free_handler(Heap *heap, DoubleFreeHandler double_free_handler) {
  heap->double_free_handler = double_free_handler;
}

void heap_set_corruption_handler(Heap *heap, CorruptionHandler corruption_handler) {
  heap->corruption_handler = corruption_handler;
}

void *heap_malloc(Heap* const heap, unsigned long nbytes, uintptr_t client_pc) {
  // Check to make sure the heap we have is initialized.
  UTIL_ASSERT(heap->begin);

  /* Convert the requested memory allocation in bytes to alignment     */
  /* size.                                                             */
  unsigned long allocation_size = (nbytes + (ALIGNMENT_SIZE - 1)) / ALIGNMENT_SIZE;

  /* Add the header size to the requested size.                        */
  allocation_size += HEAP_INFO_BLOCK_SIZE(0);

  /* Verify that the requested size is valid                           */
  if ((allocation_size < HEAP_INFO_BLOCK_SIZE(1)) || (allocation_size >= SEGMENT_SIZE_MAX)) {
    return NULL;
  }

  HeapInfo_t* allocated_block;

  heap_lock(heap);
  {
    HeapInfo_t* free_block = find_segment(heap, allocation_size);
    allocated_block = allocate_block(heap, allocation_size, free_block);

    if (allocated_block != NULL) {
      // We've allocated a new block, update our metrics

#ifdef MALLOC_INSTRUMENTATION
      allocated_block->pc = client_pc;
#endif

      heap->current_size += allocated_block->Size * ALIGNMENT_SIZE;
      if (heap->current_size > heap->high_water_mark) {
        heap->high_water_mark = heap->current_size;
      }
    }
  }
  heap_unlock(heap);

  if (allocated_block) {
    return &allocated_block->Data;
  }
  return NULL;
}

void heap_free(Heap* const heap, void *ptr, uintptr_t client_pc) {
  UTIL_ASSERT(heap->begin);

  if (!ptr) {
    // free(0) is a no-op, just bail
    return;
  }

  UTIL_ASSERT(heap_contains_address(heap, ptr)); // <ptr> not in range (heap->begin, heap->end)

  heap_lock(heap);
  {
    /* Get a pointer to the Heap Info.                             */
    HeapInfo_t *heap_info_ptr = HEAP_INFO_FOR_PTR(ptr);

    /* Verify that this segment is allocated.                      */
    if (!heap_info_ptr->is_allocated) {
      // If not, try to the call the handler if present. If there's no handler, just explode.
      // If there is a handler, let them know this happened and then just no-op and fast return.
      if (heap->double_free_handler) {
        heap_unlock(heap);

        heap->double_free_handler(ptr);

        return;
      }

      UTIL_ASSERT(0); // heap_free on invalid pointer <ptr>
    }

    /* Mask out the allocation bit of the segment to be freed.  */
    /* This will make calculations in this block easier.        */
    heap_info_ptr->is_allocated = false;

#ifndef RELEASE
    if (heap->fuzz_on_free) {
      memset(ptr, 0xBD,
             (heap_info_ptr->Size - HEAP_INFO_BLOCK_SIZE(0)) * ALIGNMENT_SIZE);
    }
#endif

    // Update metrics
#ifdef MALLOC_INSTRUMENTATION
    heap_info_ptr->pc = client_pc;
#endif
    heap->current_size -= heap_info_ptr->Size * ALIGNMENT_SIZE;

    /* If the segment to be freed is at the begin of the heap,   */
    /* then we do not have to merge or update any sizes of the  */
    /* previous segment.  This will also handle the case where  */
    /* the entire heap has been allocated to one segment.       */
    if(heap_info_ptr != heap->begin) {
      /* Calculate the pointer to the previous segment.        */
      HeapInfo_t *previous_block = get_previous_block(heap, heap_info_ptr);

      HEAP_ASSERT_SANE(heap, previous_block->Size == heap_info_ptr->PrevSize, heap_info_ptr);

      /* Check to see if the previous segment can be combined. */
      if(!previous_block->is_allocated) {
        /* Add the segment to be freed to the new header.     */
        previous_block->Size += heap_info_ptr->Size;

        /* Set the pointer to the beginning of the previous   */
        /* free segment.                                      */
        heap_info_ptr = previous_block;
      }
    }

    /* Calculate the pointer to the next segment.               */
    HeapInfo_t *next_block = get_next_block(heap, heap_info_ptr);

    /* If we are pointing at the end of the heap, then use the  */
    /* begin as the next segment.                                */
    if(next_block == heap->end) {
      /* We can't combine the begin with the end, so just      */
      /* update the PrevSize field.                            */
      heap->begin->PrevSize = heap_info_ptr->Size;
    }
    else {
      HEAP_ASSERT_SANE(heap, next_block->PrevSize == (HEAP_INFO_FOR_PTR(ptr))->Size, next_block);

      /* We are not pointing to the end of the heap, so if the */
      /* next segment is allocated then update the PrevSize    */
      /* field.                                                */
      if (next_block->is_allocated) {
        next_block->PrevSize = heap_info_ptr->Size;
      } else {
        /* The next segment is free, so merge it with the     */
        /* current segment.                                   */
        heap_info_ptr->Size += next_block->Size;

        /* Since we merged the next segment, we have to update*/
        /* the next segment's PrevSize field.                 */
        HeapInfo_t *next_next_block = get_next_block(heap, heap_info_ptr);

        /* If we are pointing at the end of the heap, then use*/
        /* the begin as the next segment.                     */
        if(next_next_block == heap->end) {
          heap->begin->PrevSize = heap_info_ptr->Size;
        }
        else {
          next_next_block->PrevSize = heap_info_ptr->Size;
        }
      }
    }
  }
  heap_unlock(heap);
}

bool heap_is_allocated(Heap* const heap, void* ptr) {
  bool rc = false;
  if (!heap_contains_address(heap, ptr)) {
    return rc;
  }

  HeapInfo_t *heap_info_ptr = heap->begin;

  // Iterate through the heap to see if this pointer is still allocated.
  heap_lock(heap);
  while (heap_info_ptr < heap->end) {
    if (heap_info_ptr == HEAP_INFO_FOR_PTR(ptr)) {
      rc = heap_info_ptr->is_allocated;
      break;
    }
    heap_info_ptr = get_next_block(heap, heap_info_ptr);
  }
  heap_unlock(heap);

  return rc;
}

bool heap_contains_address(Heap* const heap, void* ptr) {
  return (ptr >= (void*) heap->begin && ptr < (void*) heap->end);
}

size_t heap_size(const Heap *heap) {
  return ((char*) heap->end) - ((char*) heap->begin);
}

static void prv_sanity_check_block(Heap * const heap, HeapInfo_t *block) {
  HeapInfo_t* prev_block = get_previous_block(heap, block);
  HEAP_ASSERT_SANE(heap,
      prev_block <= heap->begin || prev_block->Size == block->PrevSize, block);
  HeapInfo_t* next_block = get_next_block(heap, block);
  HEAP_ASSERT_SANE(heap, next_block >= heap->end || next_block->PrevSize == block->Size, block);
}

//! Finds a segment where data of the size n_units  will fit.
//!     @param n_units number of ALIGNMENT_SIZE units this segment requires.
static HeapInfo_t *find_segment(Heap* const heap, unsigned long n_units) {
  HeapInfo_t *heap_info_ptr = NULL;
  /* If we are allocating a large segment, then start at the  */
  /* end of the heap.  Otherwise, start at the beginning of   */
  /* the heap.  If there is only one segment in the heap, then*/
  /* heap->begin will be used either way.                      */
  if (n_units >= LARGE_SIZE) {
    heap_info_ptr = (HeapInfo_t *)(((Alignment_t *)heap->end) - heap->begin->PrevSize);
  } else {
    heap_info_ptr = heap->begin;
  }

  /* Loop until we have walked the entire list.               */
  while (((n_units < LARGE_SIZE) || (heap_info_ptr > heap->begin)) && (heap_info_ptr < heap->end)) {

    prv_sanity_check_block(heap, heap_info_ptr);

    /* Check to see if the current entry is free and is large*/
    /* enough to hold the data being requested.              */
    if (heap_info_ptr->is_allocated || (heap_info_ptr->Size < n_units)) {
      /* If the requested size is larger than the limit then*/
      /* search backwards for an available buffer, else go  */
      /* forward.  This will hopefully help to reduce       */
      /* fragmentation problems.                           */
      if (n_units >= LARGE_SIZE) {
        heap_info_ptr = get_previous_block(heap, heap_info_ptr);
      } else {
        heap_info_ptr = get_next_block(heap, heap_info_ptr);
      }
    } else {
      break;
    }
  }

  // make sure the space we found is within the bounds of the heap
  UTIL_ASSERT((heap_info_ptr >= heap->begin) &&
      (heap_info_ptr <= heap->end));

  return heap_info_ptr;
}

//! Split a block into two smaller blocks, returning a pointer to the new second block.
//! The first block will be available at the same location as before, but with a smaller size.
//! Assumes the block is big enough to be split and is unallocated.
//! @param first_part_size the size of the new block, in ALIGNMENT_SIZE units, including the header
static HeapInfo_t* split_block(Heap *heap, HeapInfo_t* block, size_t first_part_size) {
  HeapInfo_t* second_block = (HeapInfo_t*) (((Alignment_t*) block) + first_part_size);

  second_block->PrevSize = first_part_size;
  second_block->is_allocated = false;
  second_block->Size = block->Size - first_part_size;

  block->Size = first_part_size;

  /* Calculate the pointer to the next segment.         */
  HeapInfo_t* next_next_block = get_next_block(heap, second_block);

  /* Check for a wrap condition and update the next     */
  /* segment's PrevSize field.                          */
  if (next_next_block == heap->end) {
    heap->begin->PrevSize = second_block->Size;
  } else {
    next_next_block->PrevSize = second_block->Size;
  }

  return second_block;
}

//! Allocated the block in the given HeapInfo_t segment.
//!     @param n_units number of ALIGNMENT_SIZE units this segment requires (including space for the header).
//!     @param heap_info_ptr the segment where the block should be allocated.
static HeapInfo_t *allocate_block(Heap* const heap, unsigned long n_units, HeapInfo_t* heap_info_ptr) {
  // Make sure we can use all or part of this block for this allocation.
  if (heap_info_ptr == heap->end || heap_info_ptr->is_allocated || heap_info_ptr->Size < n_units) {
    return NULL;
  }

  /* Check to see if we need to split this into two        */
  /* entries.                                              */
  /* * NOTE * If there is not enough room to make another  */
  /*          entry then we will not adjust the size of    */
  /*          this entry to match the amount requested.    */
  if (heap_info_ptr->Size < (n_units + HEAP_INFO_BLOCK_SIZE(MINIMUM_MEMORY_SIZE))) {
    // Nope! Just use the whole block.
    heap_info_ptr->is_allocated = true;
    return heap_info_ptr;
  }

  /* If this is a large segment allocation, then split  */
  /* the segment so that the free segment is at the     */
  /* beginning.                                         */
  if (n_units >= LARGE_SIZE) {
    HeapInfo_t *second_block = split_block(heap, heap_info_ptr, heap_info_ptr->Size - n_units);
    second_block->is_allocated = true;
    return second_block;
  }

  split_block(heap, heap_info_ptr, n_units);
  heap_info_ptr->is_allocated = true;
  return heap_info_ptr;
}

// A very naive realloc implementation
void* heap_realloc(Heap* const heap, void *ptr, unsigned long nbytes, uintptr_t client_pc) {
#if !defined(MALLOC_INSTRUMENTATION)
  client_pc = 0;
#endif
  // Get a pointer to the Heap Info.
  void *new_ptr = heap_malloc(heap, nbytes, client_pc);
  if (new_ptr && ptr) {
    // Copy over old data.
    const HeapInfo_t *heap_info_ptr = HEAP_INFO_FOR_PTR(ptr);
    const uint16_t original_size = heap_info_ptr->Size * ALIGNMENT_SIZE;
    memcpy(new_ptr, ptr, MIN(nbytes, original_size));
    heap_free(heap, ptr, client_pc);
  }
  return new_ptr;
}

void* heap_zalloc(Heap* const heap, size_t size, uintptr_t client_pc) {
  void *ptr = heap_malloc(heap, size, client_pc);
  if (ptr) {
    memset(ptr, 0, size);
  }
  return ptr;
}

void* heap_calloc(Heap* const heap, size_t count, size_t size, uintptr_t client_pc) {
  return heap_zalloc(heap, count * size, client_pc);
}

uint32_t heap_get_minimum_headroom(Heap *heap) {
  return (heap_size(heap) - heap->high_water_mark);
}

// Serial Commands
///////////////////////////////////////////////////////////
#ifdef MALLOC_INSTRUMENTATION
void heap_dump_malloc_instrumentation_to_dbgserial(Heap *heap) {
  char buffer[80];
  unsigned long num_free_blocks = 0;
  unsigned long num_free_bytes = 0;
  unsigned long num_alloc_blocks = 0;
  unsigned long num_alloc_bytes = 0;
  unsigned long largest_free = 0;

  // The output in this function is parsed by tools/parse_dump_malloc.py, so don't change it
  // without updating that file as well.

  HeapInfo_t* heap_iter = heap->begin;

  heap_lock(heap);
  void* pc;
  char* type;
  while (heap_iter < heap->end) {
    unsigned long block_size = (long)(heap_iter->Size) * ALIGNMENT_SIZE;

    if (heap_iter->is_allocated) {
      pc = (void *)heap_iter->pc;
      type = "";
      num_alloc_blocks++;
      num_alloc_bytes += block_size;
    } else {
      pc = NULL;
      type = "FREE";
      num_free_blocks++;
      num_free_bytes += block_size;
      largest_free = MAX(largest_free, block_size);
    }

    snprintf(buffer, sizeof(buffer), "PC:0x%08lX Addr:0x%08lX Bytes:%-8lu %s",
                         (long)pc, (long)&heap_iter->Data, block_size, type);
    util_dbgserial_str(buffer);
    heap_iter = get_next_block(heap, heap_iter);
  }

  snprintf(buffer, sizeof(buffer), "Heap start %p", heap->begin);
  util_dbgserial_str(buffer);

  snprintf(buffer, sizeof(buffer), "Heap end %p", heap->end);
  util_dbgserial_str(buffer);

  snprintf(buffer, sizeof(buffer), "Heap total size %zu", heap_size(heap));
  util_dbgserial_str(buffer);

  snprintf(buffer, sizeof(buffer), "Heap allocated %u", heap->current_size);
  util_dbgserial_str(buffer);

  snprintf(buffer, sizeof(buffer), "Heap high water mark %u", heap->high_water_mark);
  util_dbgserial_str(buffer);

  snprintf(buffer, sizeof(buffer), "Heap free blocks: %lu bytes, %lu blocks", num_free_bytes,
           num_free_blocks);
  util_dbgserial_str(buffer);

  snprintf(buffer, sizeof(buffer), "Heap alloc blocks: %lu bytes, %lu blocks", num_alloc_bytes,
           num_alloc_blocks);
  util_dbgserial_str(buffer);

  snprintf(buffer, sizeof(buffer), "Heap largest free block: %lu", largest_free);
  util_dbgserial_str(buffer);

  heap_unlock(heap);

}
#endif
