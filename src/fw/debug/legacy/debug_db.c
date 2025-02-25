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

#include "drivers/flash.h"
#include "flash_region/flash_region.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/math.h"
#include "system/version.h"

#include <inttypes.h>

//! @file debug_db.c
//!
//! The flash space is divided into multiple files, and those files are further divided into multiple chunks. Every time
//! the system boots up a different file is used. This leaves the file from the previous boot intact in case we previously
//! crashed.
//!
//! Files are referred to in multiple ways. The "file generation" is how recent the file is. 0 is the generation of the current
//! boot, 1 is the generation of the previous boot, and so on. The "file index" is which physical slot the file is in. File index
//! 0 has the lowest address in flash, where DEBUG_DB_NUM_FILES-1 has the highest. The "file id" is an id that is used to identify
//! which generation the file is in. See debug_db_determine_current_index for the logic that is used to convert file ids into
//! generations.
//!
//! The layout for each file looks like the following.
//!
//!  Header
//!     +  Metrics
//!     v     v      Logs
//!    +--+--------+-------------------------------------+
//!    |  |        |                                     |
//!    |  |        |                                     |
//!    +--+--------+-------------------------------------+
//!

#define FILE_SIZE_BYTES ((FLASH_REGION_DEBUG_DB_END - FLASH_REGION_DEBUG_DB_BEGIN) / DEBUG_DB_NUM_FILES)

#define FILE_ID_BIT_WIDTH 4
#define VERSION_ID_BIT_WIDTH 2

#define CURRENT_VERSION_ID 1


typedef struct PACKED {
  uint8_t magic:2; //<! Set to 0x2 if valid.

  //! The file id of this file. We always replace the oldest of the two files if they're both
  //! valid. Serial distance is used to determine if the id overflowed or not.
  uint8_t file_id:FILE_ID_BIT_WIDTH;

  //! Just in case we have to change this struct in the future.
  uint8_t version_id:VERSION_ID_BIT_WIDTH;
} FileHeaderBasic;

typedef struct PACKED {
  char version_tag[FW_METADATA_VERSION_TAG_BYTES];
  uint8_t is_recovery;
} FileHeaderDetails;

typedef struct PACKED {
  FileHeaderBasic basic;
  FileHeaderDetails details;
} FileHeader;

//! This value is chosen because older style (pre In-N-Out) filesystems set the first bit to zero to indicate that it's a
//! valid chunk. We should consider those invalid (different format) so we want to see a 1 there if it's actually a post-In-N-Out
//! file. Then, we set the second bit to 0 to differentiate it from unformatted SPI flash, as newly erased SPI flash will have the
//! value 0x03 (both bits set).
static const uint8_t VALID_FILE_HEADER_MAGIC = 0x02;

//! Which file we're writing to this boot. [0 - DEBUG_DB_NUM_FILES)
static int s_current_file_index;
//! The id we're using for the current file.
static uint8_t s_current_file_id;

static int generation_to_index(int file_generation) {
  int index = s_current_file_index - file_generation;
  if (index < 0) {
    index += DEBUG_DB_NUM_FILES;
  }
  return index;
}

static uint32_t get_file_address(int file_index) {
  return FLASH_REGION_DEBUG_DB_BEGIN + (file_index * FILE_SIZE_BYTES);
}

static uint32_t get_current_file_address(void) {
  return get_file_address(generation_to_index(0));
}

//! Get next FILE_ID_BIT_WIDTH bit value
static uint8_t get_next_file_id(uint8_t file_id) {
  return (file_id + 1) % (1 << (FILE_ID_BIT_WIDTH));
}

// Make sure this is out of range for uint8_t:FILE_ID_BIT_WIDTH.
static const uint8_t INVALID_FILE_ID = 0xff;

void debug_db_determine_current_index(uint8_t* file_id, int* current_file_index, uint8_t* current_file_id) {
  for (int i = 0; i < DEBUG_DB_NUM_FILES; ++i) {
    // If we find an unused slot, use that one. We fill in slots from left to right,
    // so the first one we find when searching left to right is the one we should use.
    if (file_id[i] == INVALID_FILE_ID) {
      *current_file_index = i;
      if (i == 0) {
        *current_file_id = 0;
      } else {
        *current_file_id = get_next_file_id(file_id[i - 1]);
      }
      return;
    }

    if (i != 0) {
      // If we find a reduction in an id, this is the end of the sequence and we've found
      // the oldest file. For example, if the IDs are (5, 6, 3, 4), when we find three we'll
      // see that the ids have stopped increasing. We should be using index 2 with an id of 7.
      int32_t distance = serial_distance(file_id[i - 1], file_id[i], FILE_ID_BIT_WIDTH);
      if (distance < 0 || distance > 2) {
        *current_file_id = get_next_file_id(file_id[i - 1]);
        *current_file_index = i;
        return;
      }
    }
  }

  // Everything was increasing which means everything was in order from oldest to newest
  // and we need to wrap around.
  *current_file_index = 0;
  *current_file_id = get_next_file_id(file_id[DEBUG_DB_NUM_FILES - 1]);
}

void debug_db_init(void) {
  // Scan the flash to find out what the two file ids are
  uint8_t file_id[DEBUG_DB_NUM_FILES] = { INVALID_FILE_ID, INVALID_FILE_ID };

  for (int i = 0; i < DEBUG_DB_NUM_FILES; ++i) {
    FileHeaderBasic file_header;
    flash_read_bytes((uint8_t*) &file_header, get_file_address(i), sizeof(file_header));
    if (file_header.magic == VALID_FILE_HEADER_MAGIC && file_header.version_id == CURRENT_VERSION_ID) {
      file_id[i] = file_header.file_id;
    }
  }

  debug_db_determine_current_index(file_id, &s_current_file_index, &s_current_file_id);

  PBL_LOG(LOG_LEVEL_DEBUG, "Found files {%u, %u, %u, %u}, using file %u with new id %u",
        file_id[0], file_id[1], file_id[2], file_id[3], s_current_file_index, s_current_file_id);

  debug_db_reformat_header_section();
}

bool debug_db_is_generation_valid(int file_generation) {
  PBL_ASSERTN(file_generation >= 0 && file_generation < DEBUG_DB_NUM_FILES);

  FileHeaderBasic file_header;
  flash_read_bytes((uint8_t*) &file_header, get_file_address(generation_to_index(file_generation)), sizeof(file_header));

  if (file_header.magic != VALID_FILE_HEADER_MAGIC) {
    return false;
  }

  if (file_header.version_id != CURRENT_VERSION_ID) {
    return false;
  }

  if (file_header.file_id != (s_current_file_id - file_generation)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "ID: %"PRIu8" Expected: %u", file_header.file_id, (s_current_file_id - file_generation));
    return false;
  }

  return true;
}

uint32_t debug_db_get_stats_base_address(int file_generation) {
  PBL_ASSERTN(file_generation >= 0 && file_generation < DEBUG_DB_NUM_FILES);

  return get_file_address(generation_to_index(file_generation)) + sizeof(FileHeader);
}

uint32_t debug_db_get_logs_base_address(int file_generation) {
  PBL_ASSERTN(file_generation >= 0 && file_generation < DEBUG_DB_NUM_FILES);

  return get_file_address(generation_to_index(file_generation)) + SECTION_HEADER_SIZE_BYTES;
}

void debug_db_reformat_header_section(void) {
  flash_erase_subsector_blocking(get_current_file_address());

  FirmwareMetadata md;
  bool result = version_copy_running_fw_metadata(&md);
  PBL_ASSERTN(result);

  FileHeader file_header = {
    .basic = {
      .magic = VALID_FILE_HEADER_MAGIC,
      .file_id = s_current_file_id,
      .version_id = CURRENT_VERSION_ID
    },
    .details = {
      .version_tag = "",
      .is_recovery = md.is_recovery_firmware ? 1 : 0
    }
  };
  strncpy(file_header.details.version_tag, md.version_tag, sizeof(md.version_tag));

  flash_write_bytes((const uint8_t*) &file_header, get_current_file_address(), sizeof(file_header));
}

uint32_t debug_db_get_stat_section_size(void) {
  return SECTION_HEADER_SIZE_BYTES - sizeof(FileHeader);
}

