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

/*
 * Generally, an mbuf is a header for a buffer which adds some useful functionality with regards to
 * grouping multiple distinct buffers together into a single packet. They are primarily used for
 * networking. As you go down a traditional network stack, headers need to be added to the data.
 * Rather than having to allocate and copy every time a new header needs to be added, or forcing the
 * upper layer to leave room for the header, mbufs allows for buffers to be chained together into an
 * mbuf chain. With mbufs, as you go down the stack, you simply add the headers as new mbufs at the
 * start of the chain. Then, the lowest layer can simply walk through the chain to get the content
 * of the entire packet, and no copying is necessary at any point in the process. Going up the stack
 * works the same way, except that MBufs are removed as you go up the stack instead of added.
 *
 * This is a very basic implementation of the mbuf type found in FreeBSD. If you're interested in
 * learning more about real mbufs, the FreeBSD man page is a good read:
 * https://www.freebsd.org/cgi/man.cgi?query=mbuf&sektion=9
 *
 * For the purposes of this implementation, MBuf headers are of a fixed size with a single pointer
 * to the data which the header is responsible for. Linking multiple MBuf chains together is not
 * supported.
 */

//! Helper macro for clearing an MBuf
#define MBUF_EMPTY ((MBuf){ 0 })

//! Flags used by consumers of MBufs (bits 0-23 are allocated for this purpose)
#define MBUF_FLAG_IS_FRAMING ((uint32_t)(1 << 0))


//! Consumers of MBufs which use mbuf_get() should add an enum value and add the maximum number of
//! MBufs which may be allocated for that pool to the MBUF_POOL_MAX_ALLOCATED array within mbuf.c.
typedef enum {
  MBufPoolSmartstrap,
#if UNITTEST
  MBufPoolUnitTest,
#endif
  NumMBufPools
} MBufPool;

typedef struct MBuf {
  //! The next MBuf in the chain
  struct MBuf *next;
  //! A pointer to the data itself
  void *data;
  //! The length of the data
  uint32_t length;
  //! Flags which are used by the consumers of MBufs
  uint32_t flags;
} MBuf;

//! Initializes the MBuf code (called from main.c)
void mbuf_init(void);

//! Returns a new heap-allocated MBuf (either from an internal pool or by allocating a new one)
MBuf *mbuf_get(void *data, uint32_t length, MBufPool pool);

//! Frees an MBuf which was created via mbuf_get()
void mbuf_free(MBuf *m);

//! Sets the data and length fields of an MBuf
void mbuf_set_data(MBuf *m, void *data, uint32_t length);

//! Returns the data for the MBuf
void *mbuf_get_data(MBuf *m);

//! Returns whether or not the specified flag is set
bool mbuf_is_flag_set(MBuf *m, uint32_t flag);

//! Sets the specified flag to the specified value
void mbuf_set_flag(MBuf *m, uint32_t flag, bool is_set);

//! Gets the next MBuf in the chain
MBuf *mbuf_get_next(MBuf *m);

//! Gets the length of the specified MBuf (NOT the entire chain)
uint32_t mbuf_get_length(MBuf *m);

//! Gets the total number of bytes in the MBuf chain
uint32_t mbuf_get_chain_length(MBuf *m);

//! Appends a new MBuf chain to the end of the chain
void mbuf_append(MBuf *m, MBuf *new_mbuf);

//! Removes any MBufs in the chain after the specified one
void mbuf_clear_next(MBuf *m);

//! Dump an MBuf chain to dbgserial
void mbuf_debug_dump(MBuf *m);
