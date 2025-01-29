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

#include "debug/flash_logging.h"

#include "drivers/flash.h"
#include "flash_region/flash_region.h"
#include "kernel/pbl_malloc.h"
#include "services/common/system_task.h"
#include "syscall/syscall.h"
#include "system/logging.h"
#include "system/passert.h"
#include "system/version.h"
#include "util/attributes.h"
#include "util/build_id.h"
#include "util/size.h"
#include "util/string.h"

#include <inttypes.h>
#include <stdio.h>

// Notes:
//
// This implements a simple circular logging scheme format.
//
// The only assumption it makes is that you have at least two erasable flash
// units. However, the more units (i.e sectors) that you have, the smaller % of
// logs that will be erased when the log buffer fills.
//
// On each boot, we create a file to hold all the messages for that boot. This
// file is called a log generation or log.
//
// Within each erasable unit multiple 'pages' exist. A log generation can span
// one or more pages. Multiple log generations can be stored at any given
// time. The oldest pages will be removed as the log buffer wraps around.
//
// Since our logging routines call into this module, we should NOT have any
// PBL_LOGs in this file, else you could generate infinite loops!

// Configuration Defines
#define LOG_REGION_SIZE (FLASH_REGION_DEBUG_DB_END - FLASH_REGION_DEBUG_DB_BEGIN)
#define ERASE_UNIT_SIZE (FLASH_DEBUG_DB_BLOCK_SIZE)

#define DEFAULT_LOG_PAGE_SIZE (0x2000)
#if ERASE_UNIT_SIZE < DEFAULT_LOG_PAGE_SIZE
#define LOG_PAGE_SIZE ERASE_UNIT_SIZE
#else
#define LOG_PAGE_SIZE DEFAULT_LOG_PAGE_SIZE
#endif

#define MAX_POSSIBLE_LOG_GENS (LOG_REGION_SIZE / LOG_PAGE_SIZE)

static bool s_flash_logging_enabled = false;

typedef struct PACKED {
  uint32_t  magic;
  uint8_t   version;
  uint8_t   build_id[BUILD_ID_EXPECTED_LEN];
  uint8_t   log_file_id;
  uint8_t   log_chunk_id; // For a given log file, the id of the page
  uint8_t   log_flags; // this should be the last header field written
} FlashLoggingHeader;

// indicates the region is erased and no logs are stored in it
#define LOG_MAGIC_PAGE_FREE 0xffffffff
#define LOG_MAGIC   0x21474F4C /* LOG! */
#define LOG_VERSION 0x1

typedef struct PACKED {
  uint8_t flags;
  uint8_t length;
} LogRecordHeader;

#define LOG_FLAGS_VALID (0x1 << 0)

typedef struct {
  uint32_t page_start_addr; // absolute start addr of the page we are logging to
  uint32_t offset_in_log_page; // the offset we writing to in a given page
  uint32_t log_start_addr; // the starting address of the curr log being written
  uint8_t  bytes_remaining; // the bytes left to write for the current log
  uint8_t  log_chunk_id;  // the id of the current page being logged to
  uint8_t  log_file_id; // the id of the current log generation
} CurrentLoggingState;

static CurrentLoggingState s_curr_state;

#define CHUNK_ID_BITWIDTH (sizeof(((FlashLoggingHeader *)0)->log_chunk_id) * 8)
#define LOG_ID_BITWIDTH   (sizeof(((FlashLoggingHeader *)0)->log_file_id) * 8)
#define MAX_LOG_FILE_ID   (0x1UL << LOG_ID_BITWIDTH)
#define MAX_PAGE_CHUNK_ID (0x1UL << CHUNK_ID_BITWIDTH)

// we use 0xff... to indicate an unpopulated msg so define max msg len to be 1
// less than that
#define MAX_MSG_LEN  ((0x1UL << sizeof(((LogRecordHeader *)0)->length) * 8) - 2)

// This is the state used while performing flash_log_file(). Each log message gets handled
// by a separate system task callback
typedef struct {
  uint8_t   page_index;               // which page we are currently dumping
  uint8_t   num_pages;                // number of pages to dump
  uint8_t   retry_count;              // How many retries we have performed at this offset
  bool      sent_build_id;            // True after we've sent the build ID
  uint16_t  page_offset;              // current offset within the page
  uint32_t  log_start_addr;           // start address of the log file we are dumping
  DumpLineCallback  line_cb;          // Called to send each line
  DumpCompletedCallback completed_cb; // Called when completed
  uint8_t   msg_buf[MAX_MSG_LEN];     // Message buffer
} DumpLogState;

#define DUMP_LOG_MAX_RETRIES          3

typedef enum {
  DumpStatus_DoneFailure,
  DumpStatus_InProgress,
  DumpStatus_DoneSuccess,
} DumpStatus;


// Static asserts to make sure user has configured flash logging correctly for
// the platform of interest

_Static_assert((MAX_POSSIBLE_LOG_GENS >= 4) &&
    (MAX_POSSIBLE_LOG_GENS < MAX_LOG_FILE_ID),
    "Invalid number of log generation numbers");
_Static_assert(MAX_POSSIBLE_LOG_GENS < MAX_PAGE_CHUNK_ID,
    "Invalid number of chunk ids for serial distance to work");
_Static_assert((LOG_REGION_SIZE / ERASE_UNIT_SIZE) >= 2,
    "Need to have at least 2 erasable units for flash logging to work");
_Static_assert((LOG_REGION_SIZE % LOG_PAGE_SIZE) == 0,
    "The log page size must be divisible by the log region size");
_Static_assert(((FLASH_REGION_DEBUG_DB_END % ERASE_UNIT_SIZE) == 0) &&
    ((FLASH_REGION_DEBUG_DB_END % ERASE_UNIT_SIZE) == 0),
    "Space for flash logging must be aligned on an erase region boundary");
_Static_assert(LOG_PAGE_SIZE <= ERASE_UNIT_SIZE,
     "Log pages must fit within an erase unit");
_Static_assert((ERASE_UNIT_SIZE % LOG_PAGE_SIZE) == 0,
     "The log page size must be divisible by the erase unit size");

//! Given the current address and amount to increment it by, handles wrapping
//! and computes the valid flash address
static uint32_t prv_get_page_addr(uint32_t curr_page_addr, uint32_t incr_by) {
  uint32_t new_offset =
      ((curr_page_addr - FLASH_REGION_DEBUG_DB_BEGIN) + incr_by) %
      LOG_REGION_SIZE;
  return (new_offset + FLASH_REGION_DEBUG_DB_BEGIN);
}

//! Given the header magic and version, returns true if the log is valid
static bool prv_flash_log_valid(const FlashLoggingHeader *hdr) {
  return (hdr->magic == LOG_MAGIC && hdr->version == LOG_VERSION);
}

static uint8_t prv_get_next_log_file_id(uint8_t file_id) {
  return (file_id + 1) % MAX_LOG_FILE_ID;
}

static uint32_t prv_get_unit_base_address(uint32_t addr) {
#if PLATFORM_SNOWY || PLATFORM_SPALDING
  return flash_get_sector_base_address(addr);
#elif PLATFORM_SILK || PLATFORM_CALCULUS || PLATFORM_ROBERT
  return flash_get_subsector_base_address(addr);
#else
#error "Invalid platform!"
#endif
}

static void prv_erase_unit(uint32_t addr) {
#if PLATFORM_SNOWY || PLATFORM_SPALDING
  flash_erase_sector_blocking(addr);
#elif PLATFORM_SILK || PLATFORM_CALCULUS || PLATFORM_ROBERT
  flash_erase_subsector_blocking(addr);
#else
#error "Invalid platform!"
#endif
}

static void prv_format_flash_logging_region(void) {
  uint32_t sector_addr;
  for (sector_addr = FLASH_REGION_DEBUG_DB_BEGIN;
       sector_addr < FLASH_REGION_DEBUG_DB_END; sector_addr += ERASE_UNIT_SIZE) {
    prv_erase_unit(sector_addr);
  }
}

static uint8_t generation_to_log_file_id(int generation) {
  int log_id = (int)s_curr_state.log_file_id - generation;

  if (log_id < 0) {
    log_id += MAX_LOG_FILE_ID;
  }

  return (log_id);
}

//! Scans the flash log region and checks the FlashLoggingHeader magic and
//! version of each log page for validity. If any header looks completely bogus,
//! we format the log region to put us back into a known state
//!
//! @return addr of the first active section
static uint32_t prv_validate_flash_log_region(uint8_t *first_log_file_id) {
  uint32_t first_used_region = UINT32_MAX;

  for (uint32_t offset = 0; offset < LOG_REGION_SIZE; offset += LOG_PAGE_SIZE) {
    uint32_t flash_addr = FLASH_REGION_DEBUG_DB_BEGIN + offset;

    FlashLoggingHeader hdr;
    flash_read_bytes((uint8_t *)&hdr, flash_addr, sizeof(hdr));

    if (!prv_flash_log_valid(&hdr)) { // is the region erased ?
      FlashLoggingHeader erased_hdr;
      memset(&erased_hdr, 0xff, sizeof(erased_hdr));
      bool region_erased = (memcmp(&erased_hdr, &hdr, sizeof(hdr)) == 0);
      if (!region_erased) { // unrecognized format, erase everything
        prv_format_flash_logging_region();
        return (UINT32_MAX); // no region in use after formatting
      }
    } else if (first_used_region == UINT32_MAX) {
      first_used_region = offset;
      *first_log_file_id = hdr.log_file_id;
    }
  }
  return (first_used_region);
}

//! @param log_file_id - the id of the log file to find
//! @param[out] start_page_addr - the address of the page the log starts on
//! @return the number of pages in the log file requested or 0 if no
//!   file is found
static int prv_get_start_of_log_file(uint8_t log_file_id,
    uint32_t *start_page_addr) {
  uint8_t num_log_pages = 0;
  uint8_t prev_chunk_id = 0;
  uint32_t log_start_addr = FLASH_LOG_INVALID_ADDR;

  for (uint32_t offset = 0; offset < LOG_REGION_SIZE; offset += LOG_PAGE_SIZE) {
    uint32_t flash_addr = FLASH_REGION_DEBUG_DB_BEGIN + offset;

    FlashLoggingHeader hdr;
    flash_read_bytes((uint8_t *)&hdr, flash_addr, sizeof(hdr));

    bool in_use_and_valid = prv_flash_log_valid(&hdr);

    // if the page is not in use or the log id is not for the generation we
    // are searching for, continue looking
    if (!in_use_and_valid || (hdr.log_file_id != log_file_id)) {
      continue;
    }

    num_log_pages++;

    int32_t dist = serial_distance(prev_chunk_id, hdr.log_chunk_id,
        LOG_ID_BITWIDTH);

    if (log_start_addr == FLASH_LOG_INVALID_ADDR) {
      // this is the first page we've found
      log_start_addr = flash_addr;
      dist = 0; // nothing else to compare against yet
    }

    if ((dist == 0) || (dist == 1)) {
      prev_chunk_id = hdr.log_chunk_id;
      continue; // keep looking
    }

    // we have found a gap in the number sequence which means we have found
    // the beginning and end of the log generation. We must continue looping
    // to make sure we count the total number of pages
    prev_chunk_id = hdr.log_chunk_id;
    log_start_addr = flash_addr;
  }

  *start_page_addr = log_start_addr;
  return (num_log_pages);
}

//! Update page in flash to reflect settings of s_curr_state
static void prv_allocate_page_for_use(void) {
  FlashLoggingHeader hdr;

  hdr.magic = LOG_MAGIC;
  hdr.version = LOG_VERSION;
  hdr.log_file_id = s_curr_state.log_file_id;
  hdr.log_chunk_id = s_curr_state.log_chunk_id;
  hdr.log_flags = ~(LOG_FLAGS_VALID);
  s_curr_state.log_chunk_id++;

  size_t len;
  const uint8_t *build_id = version_get_build_id(&len);
  memcpy(hdr.build_id, build_id, sizeof(hdr.build_id));

  flash_write_bytes((uint8_t *)&hdr, s_curr_state.page_start_addr, sizeof(hdr));
  s_curr_state.offset_in_log_page = sizeof(hdr);
}

void flash_logging_set_enabled(bool enabled) {
  s_flash_logging_enabled = enabled;
}

void flash_logging_init(void) {
  s_curr_state = (CurrentLoggingState){};

  uint8_t prev_log_id = 0;
  uint32_t first_used_region = prv_validate_flash_log_region(&prev_log_id);

  if (first_used_region == UINT32_MAX) { // no logs exist so start at region 0
    s_curr_state.page_start_addr = FLASH_REGION_DEBUG_DB_BEGIN;
    goto done;
  }

  bool new_log_region_found = false;
  bool multiple_gens_found = false;

  for (uint32_t offset = 0; offset < LOG_REGION_SIZE; offset += LOG_PAGE_SIZE) {
    uint32_t flash_addr = prv_get_page_addr(first_used_region, offset);

    FlashLoggingHeader hdr;
    flash_read_bytes((uint8_t *)&hdr, flash_addr, sizeof(hdr));

    if (prv_flash_log_valid(&hdr)) {
      // we use serial distance to find the gap in the numbering
      int32_t dist = serial_distance(prev_log_id, hdr.log_file_id,
        LOG_ID_BITWIDTH);

      if ((dist == 0) || (dist == 1)) {
        prev_log_id = hdr.log_file_id;
        multiple_gens_found |= (dist != 0 && offset != 0);
        continue; // keep looking
      }

      // we have found a page to use, but we need to erase the contents first
      prv_erase_unit(prv_get_unit_base_address(flash_addr));
    }

    s_curr_state.log_file_id = prv_get_next_log_file_id(prev_log_id);
    s_curr_state.page_start_addr = flash_addr;
    new_log_region_found = true;
    break;
  }

  // everything was in increasing order or there was only one log generation
  // if there was only one log generation, we must find the oldest part of it
  if (!new_log_region_found) {
    if (multiple_gens_found || (prv_get_start_of_log_file(prev_log_id,
        &s_curr_state.page_start_addr) == 0)) {
      s_curr_state.page_start_addr = FLASH_REGION_DEBUG_DB_BEGIN;
    }

    s_curr_state.page_start_addr = prv_get_unit_base_address(s_curr_state.page_start_addr);
    prv_erase_unit(s_curr_state.page_start_addr);

    s_curr_state.log_file_id = prv_get_next_log_file_id(prev_log_id);
  }

done: // we have allocated a region to be used
  prv_allocate_page_for_use();
  flash_logging_set_enabled(true);
}

//! Writes the log record header to flash and advances the
//! s_curr_state.offset_in_log_page field
//!
//! @param msg_length - the length of the message to be written
static void prv_write_flash_log_record_header(uint8_t msg_length) {
  LogRecordHeader record_hdr;
  memset((uint8_t *)&record_hdr, 0xff, sizeof(record_hdr));

  record_hdr.length = msg_length;

  uint32_t addr = s_curr_state.page_start_addr + s_curr_state.offset_in_log_page;
  flash_write_bytes((uint8_t *)&record_hdr, addr, sizeof(record_hdr));
  s_curr_state.offset_in_log_page += sizeof(record_hdr);
}

uint32_t flash_logging_log_start(uint8_t msg_length) {
  if ((msg_length == 0) || (msg_length > MAX_MSG_LEN) ||
      !s_flash_logging_enabled) {
    return FLASH_LOG_INVALID_ADDR;
  }

  // bytes_remaining should always be 0, but if for some reason this gets called
  // again, just skip onto the next record spot
  s_curr_state.offset_in_log_page += s_curr_state.bytes_remaining;

  uint32_t payload_size = sizeof(LogRecordHeader) + msg_length;
  if ((s_curr_state.offset_in_log_page + payload_size) <= LOG_PAGE_SIZE) {
    goto done; // there is enough space in the current page
  }

  // out of space, mark end of page
  uint32_t new_flash_addr = prv_get_page_addr(s_curr_state.page_start_addr,
      LOG_PAGE_SIZE);

  uint32_t curr_sector = s_curr_state.page_start_addr / ERASE_UNIT_SIZE;
  uint32_t new_sector = new_flash_addr / ERASE_UNIT_SIZE;
  if (curr_sector != new_sector) { // have we crossed into a new erase region ?
    prv_erase_unit(prv_get_unit_base_address(new_flash_addr));
  }

  s_curr_state.page_start_addr = new_flash_addr;
  prv_allocate_page_for_use();

done:
  s_curr_state.log_start_addr = s_curr_state.offset_in_log_page +
      s_curr_state.page_start_addr;
  s_curr_state.bytes_remaining = msg_length;

  prv_write_flash_log_record_header(msg_length);
  return (s_curr_state.log_start_addr);
}

bool flash_logging_write(const uint8_t *data_to_write, uint32_t flash_addr,
     uint32_t read_length) {

  if ((s_curr_state.bytes_remaining < read_length) || !s_flash_logging_enabled) {
    return (false);
  }

  uint32_t addr = s_curr_state.page_start_addr + s_curr_state.offset_in_log_page;
  flash_write_bytes(data_to_write, addr, read_length);

  s_curr_state.offset_in_log_page += read_length;
  s_curr_state.bytes_remaining -= read_length;

  if (s_curr_state.bytes_remaining == 0) {
    // we are done with the current log record, mark it valid
    uint8_t flags = ~(LOG_FLAGS_VALID);
    flash_write_bytes((uint8_t *)&flags, s_curr_state.log_start_addr,
        sizeof(flags));
  }

  return (true);
}

// Extract the next log message out of flash and send it using the DumpLineCallback.
// This system task callback is used by flash_dump_log_file()
static void prv_dump_log_system_cb(void *context) {
  DumpStatus status = DumpStatus_DoneFailure;
  DumpLogState *state = (DumpLogState *)context;

  // Get the start address of the current page
  uint32_t flash_addr = prv_get_page_addr(state->log_start_addr, state->page_index * LOG_PAGE_SIZE);

  if (!state->sent_build_id) {
    // dump header data first

    // use the read buffer to also hold build id str. Each byte of the build ID requires 2
    // characters.
    const int off = MEMBER_SIZE(DumpLogState, msg_buf)
                    - 2 * MEMBER_SIZE(FlashLoggingHeader, build_id) - 1;
    uint8_t build_id[MEMBER_SIZE(FlashLoggingHeader, build_id)];
    uint32_t build_id_addr = flash_addr + offsetof(FlashLoggingHeader, build_id);

    flash_read_bytes((uint8_t *)build_id, build_id_addr, sizeof(build_id));
    byte_stream_to_hex_string((char *)&state->msg_buf[off], MAX_MSG_LEN - off, (uint8_t *)build_id,
                              sizeof(build_id), false);
    int len = pbl_log_get_bin_format((char *)state->msg_buf, MAX_MSG_LEN, LOG_LEVEL_INFO, "", 0,
                                     "Build ID: %s", &state->msg_buf[off]);

    if (!state->line_cb(state->msg_buf, len)) {
      // Failed to send, if we expired our retry count, fail
      if (++state->retry_count >= DUMP_LOG_MAX_RETRIES) {
        goto exit;
      }
    } else {
      // Go into reading the log messages now.
      state->sent_build_id = true;
      state->retry_count = 0;
      state->page_offset = sizeof(FlashLoggingHeader);
    }
    status = DumpStatus_InProgress;
    goto exit;
  }

  // Read next log message and send it out
  LogRecordHeader rec;
  flash_read_bytes((uint8_t *)&rec, flash_addr + state->page_offset, sizeof(rec));
  bool page_done = false;
  if ((rec.length > MAX_MSG_LEN) || (rec.length == 0)) {
    // The record contents indicate the end of a page
    page_done = true;

  } else {
    // This record has data, read it out
    if ((~rec.flags & LOG_FLAGS_VALID) != 0) {
      // read data and execute callback to dump data
      flash_read_bytes(state->msg_buf, flash_addr + state->page_offset + sizeof(rec), rec.length);
      if (!state->line_cb(state->msg_buf, rec.length)) {
        if (++state->retry_count >= DUMP_LOG_MAX_RETRIES) {
          goto exit;
        }
      }
    }

    // Onto the next record
    status = DumpStatus_InProgress;
    state->retry_count = 0;
    state->page_offset += rec.length + sizeof(rec);
  }


  // If we're done with this page, onto the next
  if (page_done || state->page_offset + sizeof(LogRecordHeader) >= LOG_PAGE_SIZE) {
    state->page_index++;
    state->page_offset = sizeof(FlashLoggingHeader);
    if (state->page_index >= state->num_pages) {
      status = DumpStatus_DoneSuccess;
    } else {
      status = DumpStatus_InProgress;
      PBL_LOG(LOG_LEVEL_DEBUG, "Dumping page %d of %d", state->page_index, state->num_pages-1);
    }
  }

exit:
  if (status == DumpStatus_DoneFailure || status == DumpStatus_DoneSuccess) {
    state->completed_cb(status == DumpStatus_DoneSuccess);
    kernel_free(state);
  } else {
    // Keep going
    system_task_add_callback(prv_dump_log_system_cb, state);
  }
}


bool flash_dump_log_file(int generation, DumpLineCallback line_cb,
                         DumpCompletedCallback completed_cb) {
  uint8_t log_file_id = generation_to_log_file_id(generation);

  uint32_t log_start_addr;
  int num_log_pages = prv_get_start_of_log_file(log_file_id, &log_start_addr);
  PBL_LOG(LOG_LEVEL_DEBUG, "Dumping generation %d, %d pages", generation, num_log_pages);

  if (num_log_pages == 0) {
    completed_cb(false);
    return (false); // no match found
  }

  DumpLogState *state = kernel_malloc_check(sizeof(DumpLogState));

  // Init our state
  *state = (DumpLogState) {
    .log_start_addr = log_start_addr,
    .page_index = 0,
    .num_pages = num_log_pages,
    .page_offset = sizeof(FlashLoggingHeader),
    .line_cb = line_cb,
    .completed_cb = completed_cb,
  };

  // Kick it off
  system_task_add_callback(prv_dump_log_system_cb, state);
  return (true);
}

//! For unit tests
void test_flash_logging_get_info(uint32_t *tot_size, uint32_t *erase_unit_size,
    uint32_t *chunk_size, uint32_t *page_hdr_size) {
  *tot_size = LOG_REGION_SIZE;
  *erase_unit_size = ERASE_UNIT_SIZE;
  *chunk_size = LOG_PAGE_SIZE;
  *page_hdr_size = sizeof(FlashLoggingHeader);
}
