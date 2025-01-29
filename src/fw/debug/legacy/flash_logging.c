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

#include "debug_db.h"

#include "board/board.h"
#include "debug/flash_logging.h"
#include "drivers/flash.h"
#include "drivers/rtc.h"
#include "drivers/watchdog.h"
#include "flash_region/flash_region.h"
#include "kernel/pbl_malloc.h"
#include "system/logging.h"
#include "system/passert.h"
#include "kernel/util/sleep.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/net.h"

#include <inttypes.h>

//! @file flash_logging.c
//! Logs messages to SPI flash for later retrieval.
//!
//! The different chunks allow us to implement a rolling log, where if we fill up all the chunks, we can erase the oldest
//! chunk to find us some more space. Each chunk gets its own header at the top of the chunk to indicate the order in
//! which the chunks should be reassembled.

//! Make sure chunks are still an even number of flash subsectors. Our log space is 7 subsectors, so our NUM_CHUNKS
//! makes it so each chunk has its own subsector.
#define NUM_CHUNKS 7

#define CHUNK_SIZE_BYTES (SECTION_LOGS_SIZE_BYTES / NUM_CHUNKS)

#define CHUNK_ID_BIT_WIDTH 8

//! None of the values in this struct are allowed to be equal to 0xff. 0xff is used as an invalid
//! value (as the spi flash sets bytes to 0xff when they're erased)
typedef struct PACKED {
  bool invalid:1; //!< Whether or not this chunk is formatted
  bool valid:1; //!< Set to 0 when the chunk is stale.

  //! The ID of the current chunk. Each chunk in the gets an auto-incrementing ID. This
  //! allows the logging infrastructure to find the head and tail of the circular buffer after a
  //! reboot. The oldest chunk will also have the lowest ID.
  uint8_t chunk_id:CHUNK_ID_BIT_WIDTH; // :6
} LogChunkHeader;

typedef struct PACKED {
  //! The length of the log message after this header, not including this header. If this value
  //! is 0xff that means no log message follows. If this value is 0x0 this means there are no
  //! more logs remaining in this chunk.
  uint8_t log_length;
} LogHeader;

//! Which chunk we're writing to. [0 - NUM_CHUNKS)
static int s_current_chunk;
//! The id we're using for the current chunk.
static int s_current_chunk_id;

//! The current offset in the chunk in bytes. [0 - CHUNK_SIZE_BYTES)
static int s_current_offset;

static bool s_enabled;

static uint32_t get_current_address(int chunk, int offset) {
  return debug_db_get_logs_base_address(0) + (chunk * CHUNK_SIZE_BYTES) + offset;
}

static uint32_t get_generation_address(int generation, int chunk, int offset) {
  return debug_db_get_logs_base_address(generation) + (chunk * CHUNK_SIZE_BYTES) + offset;
}

//! Get next 8 bit value, avoiding 0xff (all bits set)
static uint8_t get_next_chunk_id(uint8_t chunk_id) {
  return (chunk_id + 1) % (1 << (CHUNK_ID_BIT_WIDTH));
}

static void format_current_chunk(void) {
  uint32_t addr = get_current_address(s_current_chunk, 0);
  PBL_ASSERT((addr & (SUBSECTOR_SIZE_BYTES - 1)) == 0,
      "Sections must be subsector aligned! addr is 0x%" PRIx32, addr);
  PBL_ASSERT((CHUNK_SIZE_BYTES & (SUBSECTOR_SIZE_BYTES - 1)) == 0,
      "Sections divide into subsectors evenly, size is 0x%" PRIx16, CHUNK_SIZE_BYTES);

  for (unsigned int i = 0; i < (CHUNK_SIZE_BYTES / SUBSECTOR_SIZE_BYTES); ++i) {
    flash_erase_subsector_blocking(addr + (i * SUBSECTOR_SIZE_BYTES));
  }

  LogChunkHeader chunk_header = {
    .invalid = false,
    .valid = true,
    .chunk_id = s_current_chunk_id
  };
  flash_write_bytes((const uint8_t*) &chunk_header, get_current_address(s_current_chunk, 0), sizeof(chunk_header));

  s_current_offset = sizeof(LogChunkHeader);
}

static void make_space_for_log(int length) {
  if (s_current_offset + sizeof(LogHeader) + length + sizeof(LogHeader) < CHUNK_SIZE_BYTES) {
    // We got space, nothing to do here
    return;
  }

  // Need to roll over to the next chunk

  // Seal off the current chunk with a 0 length log message.
  LogHeader log_header = { .log_length = 0 };
  flash_write_bytes((const uint8_t*) &log_header, get_current_address(s_current_chunk, s_current_offset), sizeof(log_header));

  // Set up the next chunk.
  s_current_chunk = (s_current_chunk + 1) % NUM_CHUNKS;
  s_current_chunk_id = get_next_chunk_id(s_current_chunk_id);
  format_current_chunk();
}

uint32_t flash_logging_log_start(uint8_t msg_length) {
  make_space_for_log(msg_length);

  LogHeader log_header = { .log_length = msg_length };
  flash_write_bytes((const uint8_t*) &log_header, get_current_address(s_current_chunk, s_current_offset), sizeof(log_header));
  s_current_offset += sizeof(log_header);

  uint32_t addr = get_current_address(s_current_chunk, s_current_offset);
  s_current_offset += msg_length;
  return addr;
}

bool flash_logging_write(const uint8_t *data_to_write, uint32_t flash_addr,
    uint32_t data_length) {
  flash_write_bytes(data_to_write, flash_addr, data_length);
  return (true);
}

void flash_logging_init(void) {
  debug_db_init();
  s_current_chunk = 0;
  s_current_chunk_id = 0;

  // Formatting the file we're going to use by erasing the first chunk and writing a new header.
  format_current_chunk();

  // Mark all the other chunks as stale. This will mark the "valid" member of LogChunkHeader to 0.
  for (int i = 1; i < NUM_CHUNKS; ++i) {
    uint8_t zero = 0;
    flash_write_bytes(&zero, get_current_address(i, 0), sizeof(zero));
  }

  s_enabled = true;
}

// Dumping commands
///////////////////////////////////////////////////////////////////////////////

static bool dump_chunk(int generation, int chunk_index, DumpLineCallback cb) {
  int offset = sizeof(LogChunkHeader);
  uint8_t* read_buffer = kernel_malloc(256);
  bool error = false;

  while (!error) {
    LogHeader log_header;
    flash_read_bytes((uint8_t*) &log_header, get_generation_address(generation, chunk_index, offset), sizeof(LogHeader));

    if (log_header.log_length == 0 || log_header.log_length == 0xff) {
      break;
    }

    offset += sizeof(LogHeader);

    flash_read_bytes(read_buffer, get_generation_address(generation, chunk_index, offset), log_header.log_length);
    offset += log_header.log_length;

    int retries = 3;
    while (true) {
      --retries;
      if (cb(read_buffer, log_header.log_length)) {
        break;
      } else if (retries == 0) {
        error = true;
        break;
      }
    }
  }

  kernel_free(read_buffer);
  return !error;
}

bool flash_dump_log_file(int generation, DumpLineCallback cb, DumpCompletedCallback completed_cb) {
  if (generation < 0 || generation >= DEBUG_DB_NUM_FILES) {
    completed_cb(false);
    return false;
  }

  if (!debug_db_is_generation_valid(generation)) {
    completed_cb(false);
    return (false);
  }

  int lowest_chunk_index = 0;
  uint8_t lowest_chunk_id = 0;
  int num_valid_chunks = 0;

  for (int i = 0; i < NUM_CHUNKS; ++i) {
    LogChunkHeader chunk_header;
    flash_read_bytes((uint8_t*) &chunk_header, get_generation_address(generation, i, 0), sizeof(chunk_header));

    if (chunk_header.invalid || !chunk_header.valid) {
      // No more valid chunks
      break;
    }

    if (i == 0 || serial_distance(lowest_chunk_id, chunk_header.chunk_id, CHUNK_ID_BIT_WIDTH) < 0) {
      lowest_chunk_index = i;
      lowest_chunk_id = chunk_header.chunk_id;
    }

    ++num_valid_chunks;
  }

  bool success = true;
  for (int i = 0; success && i < num_valid_chunks; ++i) {
    success = dump_chunk(generation, lowest_chunk_index, cb);
    lowest_chunk_index = (lowest_chunk_index + 1) % NUM_CHUNKS;
  }

  completed_cb(num_valid_chunks != 0);
  return true;
}

void flash_logging_set_enabled(bool enabled) {
  s_enabled = enabled;
}
