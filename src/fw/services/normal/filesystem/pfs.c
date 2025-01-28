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

#include "pfs.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console/prompt.h"
#include "drivers/flash.h"
#include "drivers/task_watchdog.h"
#include "flash_region/filesystem_regions.h"
#include "flash_region/flash_region.h"
#include "kernel/pbl_malloc.h"
#include "kernel/pebble_tasks.h"
#include "kernel/util/sleep.h"
#include "os/mutex.h"
#include "services/common/analytics/analytics.h"
#include "services/normal/filesystem/flash_translation.h"
#include "system/hexdump.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/crc8.h"
#include "util/legacy_checksum.h"
#include "util/math.h"

static PebbleRecursiveMutex *s_pfs_mutex = NULL;

#define IS_FILE_TYPE(file_type, type)   ((file_type) == (type))

#define PFS_PAGE_SIZE              FLASH_FILESYSTEM_BLOCK_SIZE
#define PFS_PAGES_PER_ERASE_SECTOR (SECTOR_SIZE_BYTES / PFS_PAGE_SIZE)
#define GC_REGION_SIZE             SECTOR_SIZE_BYTES

// The filesystem is broken into discrete blocks called 'pages'. Each page has
// a header that describes the contents contained within it. Static fields are
// CRC protected and are verified each time a file is opened. Convenience
// defines and the struct are defined below.

#define PAGE_FLAG_ERASED_PAGE            (1 << 0) // page erase completed
#define PAGE_FLAG_DELETED_PAGE           (1 << 1) // page was deleted
#define PAGE_FLAG_START_PAGE             (1 << 2) // first page of file
#define PAGE_FLAG_CONT_PAGE              (1 << 3) // continuation page of file

#define PAGE_FLAGS_BIT_SET(page_flags, type)  ((~(page_flags) & (type)) != 0)

#define DELETED_START_PAGE_MASK \
  (PAGE_FLAG_ERASED_PAGE | PAGE_FLAG_DELETED_PAGE | PAGE_FLAG_START_PAGE)
#define DELETED_CONT_PAGE_MASK \
  (PAGE_FLAG_ERASED_PAGE | PAGE_FLAG_DELETED_PAGE | PAGE_FLAG_CONT_PAGE)

// Header Layout Overview
//  First Page of a file:
//     | PageHeader | FileHeader | FileMetaData | name | File Data
//  Continuation Pages:
//     | PageHeader | File Data

#define IS_PAGE_TYPE(page_flags, type)                         \
  (PAGE_FLAGS_BIT_SET(page_flags, type)  &&                     \
   !PAGE_FLAGS_BIT_SET(page_flags, PAGE_FLAG_DELETED_PAGE))

#define SET_PAGE_FLAGS(page_flags, type) ((page_flags) &= ~(type))

#define PFS_MAGIC       0x50
#define PFS_VERS        0x01
#define PFS_CUR_VERSION ((PFS_MAGIC << 8) | PFS_VERS)

#define LAST_WRITTEN_TAG        0xfe
#define LAST_WRITTEN_UNMARK     0xfc

typedef struct PACKED PageHeader {
  uint16_t      version;
  uint8_t       last_written; //!< used by wear leveling algo
  uint8_t       page_flags;
  uint8_t       rsvd0[4];
  uint32_t      erase_count;
  uint8_t       rsvd1[9]; //!< for future extensions
  uint8_t       next_page_crc;
  uint16_t      next_page;
  uint32_t      hdr_crc; //!< a crc for all data that comes before it
} PageHeader;

typedef struct PACKED FileHeader {
  uint32_t      file_size;
  uint8_t       file_type;
  uint8_t       file_namelen;
  uint8_t       rsvd[6];
  uint32_t      hdr_crc;
} FileHeader;

// File metadata stored immediately after the header in the first page
typedef struct PACKED FileMetaData {
  uint16_t      tmp_state;
  uint16_t      create_state;
  uint16_t      delete_state;
  uint8_t       rsvd[10];
  uint8_t       uuid[16]; //!< rsvd for UUIDs in the future
  char          name[0];
} FileMetaData;

#define TMP_STATE_DONE          0x0
#define CREATE_STATE_DONE       0x0
#define DELETE_STATE_DONE       0x0

#define TMP_STATE_OFFSET        (offsetof(FileMetaData, tmp_state))
#define CREATE_STATE_OFFSET     (offsetof(FileMetaData, create_state))
#define DELETE_STATE_OFFSET     (offsetof(FileMetaData, delete_state))

#define AVAIL_BYTES_OFFSET      (sizeof(PageHeader))
#define FILEHEADER_OFFSET       (sizeof(PageHeader))
#define METADATA_OFFSET         (FILEHEADER_OFFSET + sizeof(FileHeader))
#define FILEDATA_LEN            (sizeof(FileHeader) + sizeof(FileMetaData))
#define FILE_NAME_OFFSET        (FILEHEADER_OFFSET + FILEDATA_LEN)

// defines & struct for data that needs to be tracked once a file is opened
#define INVALID_PAGE         ((uint16_t)~0)

#define GC_FILE_NAME "GC"
#define GC_DATA_VALID (0x1)

typedef struct {
  uint8_t version;
  uint8_t flags;
  uint16_t gc_start_page;
  uint32_t page_mask;
  uint8_t num_entries;
} GCData;

#define GCDATA_VALID(flags) ((~(flags) & GC_DATA_VALID) != 0)

typedef struct {
  uint16_t virtual_pg;
  uint16_t physical_pg;
  uint16_t contiguous_pgs;
} FilePageCache;

typedef struct File {
  // file specifics loaded from header
  char          *name;
  uint8_t       namelen;
  uint32_t      file_size;
  uint16_t      start_page; //!< the physical page at which the file begins
  uint16_t      start_offset; //!< offset at which file data begins
  uint8_t       file_type;

  // items dynamically changing
  uint8_t       op_flags;
  bool          is_tmp;
  uint32_t      offset; // the current offset within the file
  uint16_t      curr_page; // the current page the offset is on
  FilePageCache *pg_cache;
  uint8_t       pg_cache_len;
} File;

// The backing information tracked using the handle returned to callers
#define FD_STATUS_IN_USE        0x0 // A caller is using this fd
#define FD_STATUS_UNREFERENCED  0x1 // Valid data, no one using
#define FD_STATUS_FREE          0x2 // No data in the fd

// max number of files (cache size) that can be opened at any given time
#define PFS_FD_SET_SIZE         8
// 1 fd dedicated for GC (we always want an FD available for this operation!)
#define GC_FD_HANDLE_ID         (FD_INDEX_OFFSET + PFS_FD_SET_SIZE)
#define GC_FD_SET_SIZE          1
#define MAX_FD_HANDLES          (PFS_FD_SET_SIZE + GC_FD_SET_SIZE)
// Offset for FD numbers so that zero can't be a valid FD. This makes it much
// less likely for a file descriptor in an uninitialized object to reference a
// valid open file.
#define FD_INDEX_OFFSET         1001

static uint16_t time_closed_counter = 0;

typedef struct FileDesc {
  File          file;
  uint16_t      time_closed; //!< used for fd caching scheme
  uint8_t       fd_status;
} FileDesc;

static FileDesc s_pfs_avail_fd[MAX_FD_HANDLES];
// All accesses to s_pfs_avail_fd should be handled through the PFS_FD macro.
#define PFS_FD(fd) s_pfs_avail_fd[(fd)-FD_INDEX_OFFSET]

typedef struct GCBlock {
  bool     block_valid;
  uint8_t  block_writes;
  uint16_t gc_start_page;
} GCBlock;

static GCBlock s_gc_block;

// This is used by unit tests to clear out static state and simulate a reboot.
void pfs_reset_all_state(void) {
  s_gc_block = (GCBlock){};
  memset(s_pfs_avail_fd, 0, sizeof(s_pfs_avail_fd));
  time_closed_counter = 0;
}

#define FD_VALID(fd) ((((fd) >= FD_INDEX_OFFSET) && \
      ((fd) < (FD_INDEX_OFFSET+MAX_FD_HANDLES))) && \
      (PFS_FD(fd).fd_status == FD_STATUS_IN_USE))

#define VALID_TYPE(type) ((type) == FILE_TYPE_STATIC)

typedef struct {
  ListNode list_node;
  //! Name of the file to watch
  const char* name;
  //! Which events will invoke callbacks (see FILE_CHANGED_EVENT_ flags in pfs.h)
  uint8_t event_flags;
  //! Caller provided data pointer
  void *data;
  //! Callback pointer
  PFSFileChangedCallback callback;
} PFSFileChangedCallbackNode;

static uint8_t *s_pfs_page_flags_cache = NULL;
static uint16_t s_pfs_page_count = 0;
static uint32_t s_pfs_size = 0;
static ListNode *s_head_callback_node_list = NULL;

// In the interest of being able to leverage sector erases / minimize seek time
// for large files, deploying a variable length page size may be beneficial.
// Therefore, isolating the page offset related calculations to one location.
static uint32_t prv_page_to_flash_offset(uint16_t page) {
  return ((uint32_t)page * PFS_PAGE_SIZE);
}

static void prv_flash_read(void *buffer, uint32_t size, uint32_t offset) {
  if ((offset + size) <= s_pfs_size) {
    ftl_read(buffer, size, offset);
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "FS read out of bounds 0x%x", (int)offset);
  }
}

// Invalidates s_pfs_page_flags_cache for a given range of bytes. This should be called after the
// contents of the backing-flash are changed so that we re-read the page flags into our cache.
static void prv_invalidate_page_flags_cache(uint32_t offset, uint32_t size) {
  if (!s_pfs_page_flags_cache) {
    return;
  }

  // Prefetch any page flags which fall within the range of bytes which have been updated.
  const uint16_t start_page = offset / PFS_PAGE_SIZE;
  const uint16_t end_page = (offset + size - 1) / PFS_PAGE_SIZE;
  PBL_ASSERTN(end_page < s_pfs_page_count);
  const int page_flags_offset = offsetof(PageHeader, page_flags);
  for (uint16_t pg = start_page; pg <= end_page; pg++) {
    prv_flash_read(&s_pfs_page_flags_cache[pg], sizeof(s_pfs_page_flags_cache[pg]),
                   prv_page_to_flash_offset(pg) + page_flags_offset);
  }
}

static void prv_invalidate_page_flags_cache_all(void) {
  prv_invalidate_page_flags_cache(0, s_pfs_page_count * PFS_PAGE_SIZE);
}

static void prv_flash_write(const void *buffer, uint32_t size, uint32_t offset) {
  if ((offset + size) <= s_pfs_size) {
    ftl_write(buffer, size, offset);
    prv_invalidate_page_flags_cache(offset, size);
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "FS write out of bounds 0x%x", (int)offset);
  }
}

// Erases all pages for the sector which begins at 'start_page'
static void prv_flash_erase_sector(uint16_t start_page) {
  uint32_t offset = PFS_PAGE_SIZE * start_page;
  if (offset < s_pfs_size) {
    ftl_erase_sector(PFS_PAGE_SIZE * PFS_PAGES_PER_ERASE_SECTOR, offset);
    prv_invalidate_page_flags_cache(offset, PFS_PAGE_SIZE * PFS_PAGES_PER_ERASE_SECTOR);
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "Erase out of bounds, 0x%x", (int)start_page);
  }
}

static uint32_t free_bytes_in_page(uint16_t page) {
  return (PFS_PAGE_SIZE - AVAIL_BYTES_OFFSET);
}

static bool page_type_bits_set(uint8_t page_flags, uint8_t type_mask) {
  type_mask = ~type_mask;
  return (page_flags == type_mask);
}

static bool page_is_deleted(uint8_t page_flags) {
  return (page_type_bits_set(page_flags, DELETED_START_PAGE_MASK) ||
      page_type_bits_set(page_flags, DELETED_CONT_PAGE_MASK));
}

static bool page_is_erased(uint8_t page_flags) {
  return (page_type_bits_set(page_flags, PAGE_FLAG_ERASED_PAGE));
}

static bool page_is_unallocated(uint8_t page_flags) {
  return (page_is_deleted(page_flags) || page_is_erased(page_flags) ||
          (page_flags == 0xff));
}

static uint8_t prv_get_page_flags(uint16_t pg) {
#if UNITTEST
  // no caching for unit tests
  uint8_t flash_value = 0;
  prv_flash_read(&flash_value, sizeof(flash_value),
                 prv_page_to_flash_offset(pg) + offsetof(PageHeader, page_flags));
  return flash_value;
#else
  PBL_ASSERTN(s_pfs_page_flags_cache && (pg < s_pfs_page_count));
  return s_pfs_page_flags_cache[pg];
#endif
}

static void prv_build_page_flags_cache(void) {
#if UNITTEST
  // no caching for unit tests
  return;
#endif

  // if it already exists, free it first
  if (s_pfs_page_flags_cache) {
    kernel_free(s_pfs_page_flags_cache);
    s_pfs_page_flags_cache = NULL;
  }

  // if there are no pages in PFS, we don't need a cache
  if (s_pfs_page_count == 0) {
    return;
  }

  // allocate the new cache
  s_pfs_page_flags_cache = kernel_malloc_check(s_pfs_page_count * sizeof(*s_pfs_page_flags_cache));

  // read and set each of the page flags into the cache
  prv_invalidate_page_flags_cache_all();
}

static void update_curr_state(uint16_t start_page, uint32_t offset,
    uint16_t state) {
  offset += prv_page_to_flash_offset(start_page) + METADATA_OFFSET;
  prv_flash_write((uint8_t *)&state, sizeof(state), offset);
}

static bool get_curr_state(uint16_t start_page, uint32_t offset,
    uint16_t state) {
  uint16_t curr_state;
  offset += prv_page_to_flash_offset(start_page) + METADATA_OFFSET;
  prv_flash_read((uint8_t *)&curr_state, sizeof(state), offset);
  return (curr_state == state);
}

static bool is_create_complete(uint16_t start_page) {
  return (get_curr_state(start_page, CREATE_STATE_OFFSET, CREATE_STATE_DONE));
}

static bool is_delete_complete(uint16_t start_page) {
  return (get_curr_state(start_page, DELETE_STATE_OFFSET, DELETE_STATE_DONE));
}

static bool is_tmp_file(uint16_t start_page) {
  return (!get_curr_state(start_page, TMP_STATE_OFFSET, TMP_STATE_DONE));
}

static uint32_t compute_pg_header_crc(PageHeader *hdr) {
  PageHeader crc_hdr = *hdr;
  // don't factor fields which can change after file write into crc calc
  crc_hdr.last_written = 0xff;

  return legacy_defective_checksum_memory(&crc_hdr,
                                          offsetof(PageHeader, hdr_crc));
}

static uint32_t compute_file_header_crc(FileHeader *hdr) {
  return legacy_defective_checksum_memory(hdr, offsetof(FileHeader, hdr_crc));
}

// the start page is written to, the end page is not written to.
static void prv_write_erased_header_on_page_range(uint16_t start, uint16_t end,
    int erase_count) {
  // create a header representing an erase header
  PageHeader pg_hdr;
  memset(&pg_hdr, 0xff, sizeof(pg_hdr));
  pg_hdr.version = PFS_CUR_VERSION;
  pg_hdr.erase_count = erase_count;
  SET_PAGE_FLAGS(pg_hdr.page_flags, PAGE_FLAG_ERASED_PAGE);

  // write that header to each region in pfs
  uint8_t erased_header_size = offsetof(PageHeader, erase_count) + sizeof(pg_hdr.erase_count);
  for (uint16_t i = start; i < end; i++) {
    prv_flash_write((uint8_t*)&pg_hdr, erased_header_size, prv_page_to_flash_offset(i));
  }
}

typedef enum {
  PageHdrValid = 0,
  PageAndFileHdrValid = 1,
  HdrCrcCorrupt = -1,
  HdrVersionCheckFail = -2
} ReadHeaderStatus;

static ReadHeaderStatus read_header(uint16_t page, PageHeader *pg_hdr,
    FileHeader *file_hdr) {
  prv_flash_read((uint8_t *)pg_hdr, sizeof(*pg_hdr), prv_page_to_flash_offset(page));

  if (compute_pg_header_crc(pg_hdr) != pg_hdr->hdr_crc) {
    return (HdrCrcCorrupt);
  }

  if (pg_hdr->version > PFS_CUR_VERSION) {
    PBL_LOG(LOG_LEVEL_ERROR, "Unexpected Version Header, 0x%x",
        (int)pg_hdr->version);
    return (HdrVersionCheckFail); // let caller handle
  }

  if (!IS_PAGE_TYPE(pg_hdr->page_flags, PAGE_FLAG_START_PAGE)) {
    return (PageHdrValid);
  }

  prv_flash_read((uint8_t *)file_hdr, sizeof(*file_hdr), FILEHEADER_OFFSET +
      prv_page_to_flash_offset(page));

  if (compute_file_header_crc(file_hdr) != file_hdr->hdr_crc) {
    return (HdrCrcCorrupt);
  }

  return (PageAndFileHdrValid);
}

static status_t write_file_header(FileHeader *hdr, uint16_t pg) {
  hdr->hdr_crc = compute_file_header_crc(hdr);
  prv_flash_write((uint8_t *)hdr, sizeof(*hdr), prv_page_to_flash_offset(pg) +
      FILEHEADER_OFFSET);
  return (S_SUCCESS);
}

static status_t write_pg_header(PageHeader *hdr, uint16_t pg) {
  // recover current erase count which is updated in erase routine
  prv_flash_read((uint8_t *)&hdr->erase_count, sizeof(hdr->erase_count),
      prv_page_to_flash_offset(pg) + offsetof(PageHeader, erase_count));
  prv_flash_read((uint8_t *)&hdr->last_written, sizeof(hdr->last_written),
     prv_page_to_flash_offset(pg) + offsetof(PageHeader, last_written));

  hdr->hdr_crc = compute_pg_header_crc(hdr);
  prv_flash_write((uint8_t *)hdr, sizeof(*hdr), prv_page_to_flash_offset(pg));

  return (S_SUCCESS);
}

// note: the goal here is to do as few flash reads as possible
// while scanning the flash to find a given file.
static status_t locate_flash_file(const char *name, uint16_t *page) {
  const int file_namelen_offset = FILEHEADER_OFFSET +
      offsetof(FileHeader, file_namelen);
  uint8_t namelen = strlen(name);

  for (uint16_t pg = 0; pg < s_pfs_page_count; pg++) {
    PageHeader pg_hdr;
    FileHeader file_hdr;
    pg_hdr.page_flags = prv_get_page_flags(pg);

    if (!IS_PAGE_TYPE(pg_hdr.page_flags, PAGE_FLAG_START_PAGE)) {
      continue; // only start pages contain file name info
    }

    prv_flash_read((uint8_t *)&file_hdr.file_namelen, sizeof(file_hdr.file_namelen),
        prv_page_to_flash_offset(pg) + file_namelen_offset);

    if (file_hdr.file_namelen == namelen) {
      char file_name[namelen];

      prv_flash_read((uint8_t *)file_name, namelen, prv_page_to_flash_offset(pg) +
          FILE_NAME_OFFSET);

      if ((memcmp(name, file_name, namelen) == 0) && (!is_tmp_file(pg))) {

        if (read_header(pg, &pg_hdr, &file_hdr) == HdrCrcCorrupt) {
          PBL_LOG(LOG_LEVEL_WARNING, "%d: CRC corrupt", pg);
          continue;
        }

        *page = pg;
        return (S_SUCCESS);
      }
    }
  }

  return (E_DOES_NOT_EXIST);
}

// Populates 'hdr' with what the new erase header for the 'page' specified
// should look like
static int get_updated_erase_hdr(PageHeader *hdr, uint16_t page) {
  memset(hdr, 0xff, sizeof(*hdr));

  // before wiping a page, get its erase_count. This is not currently used but
  // enables future wear leveling improvements / analysis
  prv_flash_read((uint8_t *)&hdr->erase_count, sizeof(hdr->erase_count),
      prv_page_to_flash_offset(page) + offsetof(PageHeader, erase_count));
  prv_flash_read((uint8_t *)&hdr->last_written, sizeof(hdr->last_written),
      prv_page_to_flash_offset(page) + offsetof(PageHeader, last_written));

  // feed watchdog since erases can take a while & give lower priority tasks
  // a little time in case we are calling this from a high priority task and
  // stalling them
  task_watchdog_bit_set(pebble_task_get_current());
  psleep(1);

  // mark the page as erased. This way we know that the erase completed
  // next time we scan the sector
  SET_PAGE_FLAGS(hdr->page_flags, PAGE_FLAG_ERASED_PAGE);
  if (hdr->erase_count == 0xffffffff) {
    // should only happen after a filesystem format so assume 0 but could
    // also occur if we reboot during an erase cycle
    hdr->erase_count = 0;
  }
  hdr->erase_count++;
  hdr->version = PFS_CUR_VERSION;

  if (hdr->last_written != LAST_WRITTEN_TAG) {
    hdr->last_written = 0xff; // reset last written tag
  }

  return (S_SUCCESS);
}

static int s_last_page_written = 0;
#if UNITTEST
static int s_test_last_page_written_override = -1;
#endif

static void update_last_written_page(void) {
  for (uint16_t pg = 0; pg < s_pfs_page_count; pg++) {
    PageHeader hdr;
    prv_flash_read((uint8_t *)&hdr.last_written, sizeof(hdr.last_written),
        prv_page_to_flash_offset(pg) + offsetof(PageHeader, last_written));
    if (hdr.last_written == LAST_WRITTEN_TAG) {
      s_last_page_written = pg;
      PBL_LOG(LOG_LEVEL_INFO, "Last written page %d", (int)s_last_page_written);
      return;
    }
  }

  // should only happen after a filesystem format
  PBL_LOG(LOG_LEVEL_WARNING, "Couldn't resolve last written pg");
  s_last_page_written = s_pfs_page_count - 1;
#if UNITTEST
  if (s_test_last_page_written_override != -1) {
    s_last_page_written = s_test_last_page_written_override;
  }
#endif
}

//! @param region - The erase sector we want to scan through
//! @param first_free_page - Populated with the first erased page found
//!     in the region. If none are found, populated with INVALID_PAGE
//! @return A bitmask indicating which pages in the sector are occupied.
//!     For example, 0b1001 would indicate page 0 and page 3 within the sector
//!     are in use
static uint32_t prv_get_sector_page_status(uint16_t region,
     uint16_t *first_free_page) {

  // our bitmask needs to be large enough to describe all the pages in a sector
  _Static_assert((sizeof(uint32_t) * 8) >= PFS_PAGES_PER_ERASE_SECTOR,
      "Number of PFS pages is larger than bitmask");

  *first_free_page = INVALID_PAGE;

  uint16_t start_pg = region * PFS_PAGES_PER_ERASE_SECTOR;
  uint16_t end_pg = start_pg + PFS_PAGES_PER_ERASE_SECTOR;
  uint32_t sectors_active = 0;
  for (uint16_t pg = start_pg; pg < end_pg; pg++) {
    uint8_t page_flags = prv_get_page_flags(pg);

    if (page_is_erased(page_flags)) {
      if (*first_free_page == INVALID_PAGE) {
        *first_free_page = pg;
      }
    } else if (!page_is_unallocated(page_flags)) {
      sectors_active |= (0x1 << (pg % PFS_PAGES_PER_ERASE_SECTOR));
    }
  }

  return (sectors_active);
}

//! Scans through the filesystem and finds a sector with no pages that
//! are active
//!
//! @param skip_gc_region - will skip checking the region that is
//!     used for garbage collection
//! @return the beginning page in the region which is free or -1 on failure
static int prv_find_free_erase_region(bool skip_gc_region) {
  int num_erase_regions = s_pfs_page_count / PFS_PAGES_PER_ERASE_SECTOR;
  int start_region = s_last_page_written / PFS_PAGES_PER_ERASE_SECTOR;
  int end_region = start_region + num_erase_regions;

  uint16_t gc_erase_block = s_gc_block.gc_start_page / PFS_PAGES_PER_ERASE_SECTOR;

  for (int region = start_region; region < end_region; region++) {
    int erase_region = region % num_erase_regions;

    if (skip_gc_region && (erase_region == gc_erase_block)) {
      continue;
    }

    uint16_t free_pg;
    uint32_t sectors_active = prv_get_sector_page_status(erase_region, &free_pg);
    if ((__builtin_popcount(sectors_active) == 0)) {
      return (erase_region * PFS_PAGES_PER_ERASE_SECTOR);
    }
  }

  return (-1);
}

static status_t garbage_collect_sector(uint16_t *free_page,
    uint16_t sector_start_page, uint32_t sectors_active);

//! Updates the last written page to point to next_page
static NOINLINE void prv_update_last_written_page(uint16_t next_page) {
  PageHeader hdr = { 0 };

  uint16_t prev_written_page = s_last_page_written;
  // unmark the previous page as last written (should only have one pg
  // marked as written at any given time).
  prv_flash_read((uint8_t *)&hdr.last_written, sizeof(hdr.last_written),
      prv_page_to_flash_offset(prev_written_page) +
      offsetof(PageHeader, last_written));

  if (hdr.last_written == LAST_WRITTEN_TAG) {
    hdr.last_written = LAST_WRITTEN_UNMARK;
    prv_flash_write((uint8_t *)&hdr.last_written, sizeof(hdr.last_written),
        prv_page_to_flash_offset(prev_written_page) +
        offsetof(PageHeader, last_written));
  }

  hdr.last_written = LAST_WRITTEN_TAG;

  prv_flash_write((uint8_t *)&hdr.last_written, sizeof(hdr.last_written),
      prv_page_to_flash_offset(next_page) + offsetof(PageHeader, last_written));
}

//! The wear leveling strategy deployed is as follows:
//!    Always track the last page which was written. Every time a new page needs
//!    to be allocated, search for the next page that comes after the
//!    'last written' page.
//!
//! Note:
//!  - This is the only routine that should ever tag a page as last written.
//!  - This routine can be called at any time to force garbage collection at
//!    opportune times (i.e in an idle task). For this scenario, use_page
//!    described below should be 'false'.
//! @param free_page - Populated with a free page that is erased and available
//!     to be written on. Value should initially be set to INVALID_PAGE if it's
//!     the first page being allocated for a file. Afterward the value should be
//!     the previously allocated page
//! @param use_gc_allocator - should be true iff the page should be allocated
//!      from the region dedicated for garbage collection handling
//! @param use_page - should be true iff the page is about to be used in a file.
static status_t find_free_page(uint16_t *free_page, bool use_gc_allocator,
    bool use_page) {

  // if we are allocating a file from the garbage collection region,
  // we don't need to search for free pages since we know what ones to use
  if (use_gc_allocator) {
    uint16_t next_page = (*free_page == INVALID_PAGE) ? s_gc_block.gc_start_page :
        (*free_page + 1);
    PBL_ASSERTN(s_gc_block.block_valid && (next_page >= s_gc_block.gc_start_page) &&
        (next_page < (s_gc_block.gc_start_page + PFS_PAGES_PER_ERASE_SECTOR)));
    *free_page = next_page;
    return (S_SUCCESS);
  }

  uint16_t next_page = INVALID_PAGE;
  uint16_t start_pg = (s_last_page_written + 1) % s_pfs_page_count;
  uint16_t remaining_pgs_in_block = PFS_PAGES_PER_ERASE_SECTOR -
      (start_pg % PFS_PAGES_PER_ERASE_SECTOR);

  uint16_t gc_erase_region = s_gc_block.gc_start_page / PFS_PAGES_PER_ERASE_SECTOR;
  bool in_gc_region = (gc_erase_region == (start_pg / PFS_PAGES_PER_ERASE_SECTOR));

  // are we looking for free pages in the sector we last wrote to?
  if (remaining_pgs_in_block < PFS_PAGES_PER_ERASE_SECTOR) {
    // are any of the pages already erased?
    for (uint16_t pg = 0; pg < remaining_pgs_in_block && !in_gc_region; pg++) {
      uint16_t curr_page = start_pg + pg;
      uint8_t page_flags = prv_get_page_flags(curr_page);

      if (page_is_erased(page_flags)) {
        next_page = curr_page;
        break;
      }
    }

    start_pg += remaining_pgs_in_block;
  }

  // we should now be processing on a sector aligned boundary
  PBL_ASSERTN((start_pg % PFS_PAGES_PER_ERASE_SECTOR) == 0);

  // if we could not find a free page in the sector we were previosuly using
  // we need to scan through the erase regions and either perform some garbage
  // collection or find an erased page in another erase region
  if (next_page == INVALID_PAGE) {
    int num_erase_regions = s_pfs_page_count / PFS_PAGES_PER_ERASE_SECTOR;
    uint16_t start_region = start_pg / PFS_PAGES_PER_ERASE_SECTOR;

    for (uint16_t region = 0; region < num_erase_regions; region++) {
      uint16_t curr_region = (region + start_region) % num_erase_regions;

      if (s_gc_block.block_valid && (gc_erase_region == curr_region)) {
        // don't use pre-allocated garbage collection regions
        continue;
      }

      uint32_t sectors_active = prv_get_sector_page_status(curr_region, &next_page);
      if (next_page != INVALID_PAGE) {
        // we have found a page which is already erased
        break;
      } else if (__builtin_popcount(sectors_active) < PFS_PAGES_PER_ERASE_SECTOR) {
        // we can erase this region and have at least 1 free page after
        uint16_t sector_start_pg = curr_region * PFS_PAGES_PER_ERASE_SECTOR;
        garbage_collect_sector(&next_page, sector_start_pg, sectors_active);
        break;
      }
    }
  }

  if (next_page != INVALID_PAGE) { // a free page was found
    if (use_page) {
      prv_update_last_written_page(next_page);
    }

    *free_page = s_last_page_written = next_page;
    return (S_SUCCESS);
  }

  return (E_OUT_OF_STORAGE);
}

//! Note: expects that the caller does _not_ hold the pfs mutex
//! Note: If pages are already pre-erased on the FS, this routine will return
//!  very quickly. If we need to do erases, it will take longer because this
//!  operation can take seconds to complete on certain flash parts
//!
//! @param file_size - The amount of file space to erase
//! @param max_elapsed_ticks - The max amount of time to spend attempting to
//!    find / create the free space. If 0, then there is no timeout
static void pfs_prepare_for_file_creation(uint32_t file_size,
                                          uint32_t max_elapsed_ticks) {
  uint16_t pages_to_find = (file_size + PFS_PAGE_SIZE) / PFS_PAGE_SIZE;
  uint16_t free_page = 0;

  uint32_t start_ticks = rtc_get_ticks();

  uint16_t last_written_page = s_last_page_written;
  while ((pages_to_find > 0) && (free_page != INVALID_PAGE)) {
    mutex_lock_recursive(s_pfs_mutex);
    find_free_page(&free_page, false, false);
    mutex_unlock_recursive(s_pfs_mutex);
    // TODO: might be nice to only sleep here if we had to perform GC as part
    // of finding a free page
    if ((pages_to_find % 4) == 0) {
      psleep(2);
    }
    pages_to_find--;

    uint32_t elapsed_ticks = rtc_get_ticks() - start_ticks;
    if (max_elapsed_ticks != 0 && (elapsed_ticks > max_elapsed_ticks)) {
      break;
    }
  }

  mutex_lock_recursive(s_pfs_mutex);
  s_last_page_written = last_written_page; // reset our tracker
  mutex_unlock_recursive(s_pfs_mutex);
}

// In the future, the next_page field may be updated dynamically (i.e to resize
// a file). Use a CRC to catch corruption issues in this field.
static uint8_t crc8_next_page(uint16_t next_page) {
  return crc8_calculate_bytes((uint8_t*)&next_page, sizeof(next_page), true /* big_endian */);
}

static status_t get_next_page(uint16_t curr_page, uint16_t *next_page) {
  PageHeader hdr;
  prv_flash_read((uint8_t *)&hdr.next_page_crc, sizeof(hdr.next_page_crc) +
      sizeof(hdr.next_page), prv_page_to_flash_offset(curr_page) +
      offsetof(PageHeader, next_page_crc));
  *next_page = hdr.next_page;

  if (*next_page == INVALID_PAGE) {
    return (S_NO_MORE_ITEMS);
  }

  if (crc8_next_page(*next_page) == hdr.next_page_crc) {
    if (*next_page < s_pfs_page_count) {
      return (S_SUCCESS);
    }
  }

  return (E_INTERNAL); // the next page pointer is corrupt
}

static status_t unlink_flash_file(uint16_t page) {
  uint16_t first_page = page;
  if (page > s_pfs_page_count) { // should never happen
    return (E_INTERNAL);
  }

  // Mark the files to indicate that they are ready to be erased
  PageHeader hdr;
  hdr.page_flags = 0xff;
  SET_PAGE_FLAGS(hdr.page_flags, PAGE_FLAG_DELETED_PAGE);
  int rv = S_SUCCESS;
  int unlink_count = 0;
  do {
    if ((page > s_pfs_page_count) || (unlink_count > s_pfs_page_count)) {
      rv = E_INTERNAL; // should never happen
      break;
    }
    prv_flash_write((uint8_t *)&hdr.page_flags, sizeof(hdr.page_flags),
        prv_page_to_flash_offset(page) + offsetof(PageHeader, page_flags));

    unlink_count++;
  } while (get_next_page(page, &page) == S_SUCCESS);

  // Add a tag to indicate that all pages within a file have been marked for
  // deletion we check for this during reboot to clean up a partial delete
  update_curr_state(first_page, DELETE_STATE_OFFSET, DELETE_STATE_DONE);

  return (rv);
}

static status_t create_flash_file(File *f) {
  status_t rv;
  uint16_t start_page = INVALID_PAGE;

  PageHeader pg_hdr;
  memset(&pg_hdr, 0xff, sizeof(pg_hdr));

  bool use_gc_allocator = (strcmp(f->name, GC_FILE_NAME) == 0);

  if ((rv = find_free_page(&start_page, use_gc_allocator, true)) != S_SUCCESS) {
    return (rv);
  }

  pg_hdr.version = PFS_CUR_VERSION;
  SET_PAGE_FLAGS(pg_hdr.page_flags,
      PAGE_FLAG_START_PAGE | PAGE_FLAG_ERASED_PAGE);

  // Note: We have already allocated 1 pg so just subtract 1 to roundup
  // We assume all pages are the same size
  int pgs_needed = (f->file_size + FILEDATA_LEN + strlen(f->name) - 1) /
      free_bytes_in_page(start_page);
  uint16_t curr_page = start_page;
  uint16_t next_page = start_page;

  for (; pgs_needed >= 0; pgs_needed--) {
    // flag the page as in use
    prv_flash_write((uint8_t *)&pg_hdr.page_flags, sizeof(pg_hdr.page_flags),
        prv_page_to_flash_offset(curr_page) + offsetof(PageHeader, page_flags));

    if (pgs_needed > 0) { // do we need to find a free page
      if ((rv = find_free_page(&next_page, use_gc_allocator, true)) != S_SUCCESS) {
        unlink_flash_file(start_page); // on failure, unallocate
        return (rv);
      }
      pg_hdr.next_page_crc = crc8_next_page(next_page);
      pg_hdr.next_page = next_page;
      write_pg_header(&pg_hdr, curr_page);
      curr_page = next_page;

      // continuation page header settings
      memset(&pg_hdr, 0xff, sizeof(PageHeader));
      pg_hdr.version = PFS_CUR_VERSION;
      SET_PAGE_FLAGS(pg_hdr.page_flags,
          PAGE_FLAG_CONT_PAGE | PAGE_FLAG_ERASED_PAGE);
    } else {
      write_pg_header(&pg_hdr, curr_page);
      break; // we are done
    }
  }

  // we have succesfully allocated space for the file, so add file specific info
  f->start_page = f->curr_page = start_page;

  FileHeader file_hdr;
  memset(&file_hdr, 0xff, sizeof(file_hdr));
  file_hdr.file_namelen = strlen(f->name);
  file_hdr.file_size = f->file_size;
  file_hdr.file_type = f->file_type;
  write_file_header(&file_hdr, start_page);

  prv_flash_write((uint8_t *)f->name, strlen(f->name),
      prv_page_to_flash_offset(start_page) + FILE_NAME_OFFSET);

  if (!f->is_tmp) {
    update_curr_state(f->start_page, TMP_STATE_OFFSET, TMP_STATE_DONE);
  }

  // finally, mark the creation as complete
  update_curr_state(start_page, CREATE_STATE_OFFSET, CREATE_STATE_DONE);

  return (S_SUCCESS);
}

static status_t scan_to_offset(File *f, uint32_t *pg_offset) {
  uint32_t data_offset = f->offset + f->start_offset;

  // a read or write could have ended at a page boundary so check for that
  if ((f->curr_page == INVALID_PAGE) ||
      ((data_offset % free_bytes_in_page(f->curr_page)) == 0)) {
    uint16_t next_page = f->start_page;
    int pages_to_seek = (data_offset / free_bytes_in_page(f->start_page));

    int closest_match = -1;
    if (((f->op_flags & OP_FLAG_USE_PAGE_CACHE) != 0) && (f->pg_cache != NULL)) {

      // Flash pages are singly linked together with the next pointer located
      // on the current flash page. This means the optimal page to find in the
      // cache is the one closest to what we are looking for without going past
      // it
      for (int i = 0; i < f->pg_cache_len; i++) {
        FilePageCache *pgc = &f->pg_cache[i];
        if (pgc->virtual_pg > pages_to_seek) {
          continue;
        } else if ((closest_match == -1) ||
            (f->pg_cache[closest_match].virtual_pg < pgc->virtual_pg)) {
          closest_match = i;
        }
      }
    }

    if (closest_match != -1) {
      FilePageCache *close_pg = &f->pg_cache[closest_match];

      pages_to_seek -= f->pg_cache[closest_match].virtual_pg;
      next_page = f->pg_cache[closest_match].physical_pg;

      // if we still are not on the page we are looking for, see how
      // many contiguous pages we can skip ahead.
      if (pages_to_seek > 0) {
        uint16_t contig_pgs = MIN(close_pg->contiguous_pgs, pages_to_seek);
        pages_to_seek -= contig_pgs;
        next_page += contig_pgs;
      }
    }

    for (uint16_t i = 0; i < pages_to_seek; i++) {
      if (get_next_page(next_page, &next_page) != S_SUCCESS) {
        return (E_RANGE);
      }
    }
    f->curr_page = next_page;
  }

  *pg_offset = data_offset % free_bytes_in_page(f->curr_page);
  return (S_SUCCESS);
}

static int mark_fd_free(int fd) {
  if (PFS_FD(fd).file.name != NULL) {
    kernel_free(PFS_FD(fd).file.name);
    PFS_FD(fd).file.name = NULL;
  }
  if (PFS_FD(fd).file.pg_cache != NULL) {
    kernel_free(PFS_FD(fd).file.pg_cache);
    PFS_FD(fd).file.pg_cache = NULL;
    PFS_FD(fd).file.pg_cache_len = 0;
  }

  PFS_FD(fd).fd_status = FD_STATUS_FREE;

  return (S_SUCCESS);
}

typedef enum {
  FDBusy = 2,
  FDAlreadyLoaded = 1,
  FDAvail = 0,
  NoFDAvail = -1
} AvailFdStatus;

//! @param is_tmp specified to indicate whether or not you are looking for
//!        a tmp file
static AvailFdStatus get_avail_fd(const char *name, int *fdp, bool is_tmp) {
  // First search to see if the fd has already been located
  for (int fd = FD_INDEX_OFFSET; fd < FD_INDEX_OFFSET+MAX_FD_HANDLES; fd++) {
    File *f = &PFS_FD(fd).file;
    if ((f->is_tmp == is_tmp) && (f->name != NULL)) {
      if (strcmp(f->name, name) == 0) {
        PBL_ASSERTN(PFS_FD(fd).fd_status != FD_STATUS_FREE);
        *fdp = fd;
        return ((PFS_FD(fd).fd_status == FD_STATUS_IN_USE) ? FDBusy : FDAlreadyLoaded);
      }
    }
  }

  // a simple least-recently-accessed cache scheme
  int unref = -1;
  uint16_t curr_time_closed = 0;

  for (int fd = FD_INDEX_OFFSET; fd < FD_INDEX_OFFSET+PFS_FD_SET_SIZE; fd++) {
    if (PFS_FD(fd).fd_status == FD_STATUS_FREE) {
      *fdp = fd;
      return (FDAvail);
    }
    if (PFS_FD(fd).fd_status == FD_STATUS_UNREFERENCED) {
      if ((unref == -1) || (PFS_FD(fd).time_closed < curr_time_closed)) {
        unref = fd;
        curr_time_closed = PFS_FD(fd).time_closed;
      }
    }
  }

  *fdp = unref;
  if (unref != -1) {
    mark_fd_free(unref); // clean up previous file state
  }

  return ((*fdp != -1) ? FDAvail : NoFDAvail);
}

/*
 * Exported PFS APIs
 */

size_t pfs_get_file_size(int fd) {
  mutex_lock_recursive(s_pfs_mutex);

  size_t res = 0;
  if (FD_VALID(fd)) {
    res = PFS_FD(fd).file.file_size;
  }

  mutex_unlock_recursive(s_pfs_mutex);
  return (res);
}

int pfs_read(int fd, void *buf_ptr, size_t size) {
  uint8_t *buf = buf_ptr;
  mutex_lock_recursive(s_pfs_mutex);

  int res = E_UNKNOWN;
  if (!FD_VALID(fd) || (buf == NULL) || (size == 0)) {
    res = E_INVALID_ARGUMENT;
    goto cleanup;
  }

  File *file = &PFS_FD(fd).file;

  if ((file->op_flags & OP_FLAG_READ) == 0) {
    res = E_INVALID_ARGUMENT;
    goto cleanup;
  }

  if ((file->offset + size) > file->file_size) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Out of bound read at %d",
        (int)(file->offset + size));
    res = E_RANGE;
    goto cleanup;
  }

  uint32_t pg_offset;
  if (scan_to_offset(file, &pg_offset) != S_SUCCESS) {
    res = E_INTERNAL;
    goto cleanup;
  }

  // we have found the page from which to start reading data from
  size_t bytes_read = 0;
  while (bytes_read < size) {
    size_t bytes_to_read = MIN(free_bytes_in_page(file->curr_page) - pg_offset,
        size - bytes_read);

    prv_flash_read(buf + bytes_read, bytes_to_read,
        prv_page_to_flash_offset(file->curr_page) + AVAIL_BYTES_OFFSET + pg_offset);

    bytes_read += bytes_to_read;
    file->offset += bytes_to_read;

    if (bytes_read == size) {
      break; // we are done
    }

    pg_offset = 0; // first usable byte next page
    if (get_next_page(file->curr_page, &file->curr_page) != S_SUCCESS) {
      PBL_LOG(LOG_LEVEL_WARNING, "R:Couldn't find next page for %d",
          file->curr_page);
      res = E_INTERNAL;
      goto cleanup;
    }
  }

  res = bytes_read;
cleanup:
  mutex_unlock_recursive(s_pfs_mutex);
  return (res);
}

int pfs_seek(int fd, int offset, FSeekType seek_type) {
  mutex_lock_recursive(s_pfs_mutex);
  int res = E_UNKNOWN;
  if (!FD_VALID(fd)) {
    res = E_INVALID_ARGUMENT;
    goto cleanup;
  }

  int new_offset = PFS_FD(fd).file.offset;
  if (seek_type == FSeekSet) {
    new_offset = offset;
  } else if (seek_type == FSeekCur) {
    new_offset += offset;
  }

  // allow one to seek to very EOF
  if ((new_offset >= 0) &&
      (new_offset <= (int)PFS_FD(fd).file.file_size)) {

    if (PFS_FD(fd).file.offset != (uint32_t)new_offset) {
      PFS_FD(fd).file.offset = (uint32_t)new_offset;
      PFS_FD(fd).file.curr_page = INVALID_PAGE;
    }
    res = new_offset;
  } else {
    res = E_RANGE;
  }

cleanup:
  mutex_unlock_recursive(s_pfs_mutex);
  return (res);
}

int pfs_write(int fd, const void *buf_ptr, size_t size) {
  const uint8_t *buf = buf_ptr;
  mutex_lock_recursive(s_pfs_mutex);
  int res = E_UNKNOWN;
  if (!FD_VALID(fd) || (buf == NULL) || (size == 0)) {
    res = E_INVALID_ARGUMENT;
    goto cleanup;
  }

  File *file = &PFS_FD(fd).file;

  if ((file->op_flags & (OP_FLAG_WRITE | OP_FLAG_OVERWRITE)) == 0) {
    res = E_INVALID_ARGUMENT;
    goto cleanup;
  }

  if ((file->offset + size) > file->file_size) {
    res = E_RANGE;
    goto cleanup;
  }

  uint32_t pg_offset;
  if (scan_to_offset(file, &pg_offset) != S_SUCCESS) {
    res = E_INTERNAL;
    goto cleanup;
  }

  size_t bytes_written = 0;
  while (bytes_written < size) {
    size_t bytes_to_write = MIN(free_bytes_in_page(file->curr_page) - pg_offset,
        size - bytes_written);

    prv_flash_write(buf + bytes_written, bytes_to_write,
        prv_page_to_flash_offset(file->curr_page) + AVAIL_BYTES_OFFSET + pg_offset);

    bytes_written += bytes_to_write;
    file->offset += bytes_to_write;

    if (bytes_written == size) {
      break;
    }

    pg_offset = 0; // first usable byte next page
    if (get_next_page(file->curr_page, &file->curr_page) != S_SUCCESS) {
      PBL_LOG(LOG_LEVEL_WARNING, "W:Couldn't find next page for %d",
          file->curr_page);
      res = E_INTERNAL;
      goto cleanup;
    }
  }

  res = bytes_written;
cleanup:
  mutex_unlock_recursive(s_pfs_mutex);
  return (res);
}

uint32_t pfs_get_size(void) {
  // one sector is needed for internal book keeping
  return s_pfs_size - GC_REGION_SIZE;
}

void pfs_set_size(uint32_t new_size, bool new_region_erased) {
  uint32_t prev_size = s_pfs_size;
  s_pfs_size = new_size;
  s_pfs_page_count = new_size / PFS_PAGE_SIZE;

  // re-build the flags cache
  prv_build_page_flags_cache();

  if (new_region_erased) {
    prv_write_erased_header_on_page_range((prev_size/PFS_PAGE_SIZE),
        (new_size/PFS_PAGE_SIZE), 1);
  }

  update_last_written_page();
}

bool pfs_active_in_region(uint32_t start_address, uint32_t ending_address) {
  uint16_t starting_page = start_address / PFS_PAGE_SIZE;
  uint16_t ending_page = (ending_address) / PFS_PAGE_SIZE;

  for (uint16_t pg = starting_page; pg < ending_page; pg++) {

    PageHeader hdr;

    // read version first, check magic, then check version and make sure it makes sense
    prv_flash_read((uint8_t *)&hdr.version, sizeof(hdr.version),
        prv_page_to_flash_offset(pg) + offsetof(PageHeader, version));

    if ((hdr.version >> 8) != PFS_MAGIC) {
      continue;
    }

    if (hdr.version > PFS_CUR_VERSION) {
      PBL_LOG(LOG_LEVEL_WARNING, "Incompatible version of PFS active, 0x%x",
          hdr.version);

      // pfs filesystem is a newer version than we support
      return (false);
    }

    hdr.page_flags = prv_get_page_flags(pg);
    // read the header flags to see if the page is a file start page or an erased page
    if (IS_PAGE_TYPE(hdr.page_flags, PAGE_FLAG_ERASED_PAGE)
       || IS_PAGE_TYPE(hdr.page_flags, PAGE_FLAG_START_PAGE)
       || IS_PAGE_TYPE(hdr.page_flags, PAGE_FLAG_CONT_PAGE)
       || page_is_deleted(hdr.page_flags)) {

      return (true);
    }
  }

  // pfs filesystem is not active
  return (false);
}

// migration utility
// Returns true if valid PFS file found, false otherwise
bool pfs_active(void) {
  return pfs_active_in_region(0, s_pfs_size);
}

// Scans through the filesystem to see if we rebooted while a file was in the
// middle of being created and cleans up these partial files.
void pfs_reboot_cleanup(void) {
  static uint16_t curr_pg = 0;

  for (; curr_pg < s_pfs_page_count; curr_pg++) {
    uint8_t page_flags = prv_get_page_flags(curr_pg);

    if (IS_PAGE_TYPE(page_flags, PAGE_FLAG_START_PAGE)) {
      if (!is_create_complete(curr_pg)) { // make sure file creation completed
        PBL_LOG(LOG_LEVEL_WARNING, "File at %d creation did not complete ",
            curr_pg);
        unlink_flash_file(curr_pg);
      } else if (is_tmp_file(curr_pg)) { // make sure this isn't a temp file
        PBL_LOG(LOG_LEVEL_WARNING, "Removing temp file at %d", curr_pg);
        unlink_flash_file(curr_pg);
      }
    } else if (page_type_bits_set(page_flags, DELETED_START_PAGE_MASK) &&
        !is_delete_complete(curr_pg)) {
      PBL_LOG(LOG_LEVEL_WARNING, "Delete of %d did not complete", curr_pg);
      unlink_flash_file(curr_pg);
    }
  }

  update_last_written_page();
}

static void prv_handle_sector_erase(uint16_t start_page, bool update_erase_count) {
  if (!update_erase_count) {
    prv_flash_erase_sector(start_page);
    return;
  }

  uint16_t max_erase = 0;
  uint16_t last_written_pg = INVALID_PAGE;
  PageHeader hdr;
  for (int i = 0; i < PFS_PAGES_PER_ERASE_SECTOR; i++) {
    get_updated_erase_hdr(&hdr, i + start_page);
    if (hdr.erase_count > max_erase) {
      max_erase = hdr.erase_count;
    }

    if (hdr.last_written == LAST_WRITTEN_TAG) {
      last_written_pg = i + start_page;
    }
  }

  prv_flash_erase_sector(start_page);
  prv_write_erased_header_on_page_range(start_page,
      start_page + PFS_PAGES_PER_ERASE_SECTOR, max_erase);

  if (last_written_pg != INVALID_PAGE) {
    hdr.last_written = LAST_WRITTEN_TAG;
    prv_flash_write((uint8_t *)&hdr.last_written, sizeof(hdr.last_written),
        prv_page_to_flash_offset(last_written_pg) +
        offsetof(PageHeader, last_written));
  }
}

static bool prv_update_gc_reserved_region(void) {
  if (!s_gc_block.block_valid || (s_gc_block.block_writes > 5)) {
    int free_region_start = prv_find_free_erase_region(s_gc_block.block_valid);

    if (free_region_start >= 0) {
      s_gc_block = (GCBlock) {
        .block_valid = true,
        .block_writes = 0,
        .gc_start_page = free_region_start
      };
      PBL_LOG(LOG_LEVEL_DEBUG, "New Erase Region: %d", s_gc_block.gc_start_page);
      return (true);
    }

    return (false);
  }

  return (true); // gc block must be valid to get here
}

static bool watch_list_find_str(ListNode *node, void *data) {
  PFSFileChangedCallbackNode *filechg_node = (PFSFileChangedCallbackNode *)node;
  return (strcmp(filechg_node->name, (char *)data) == 0);
}

PFSCallbackHandle pfs_watch_file(const char* filename, PFSFileChangedCallback callback,
                                 uint8_t event_flags, void* data) {
  mutex_lock_recursive(s_pfs_mutex);

  PFSFileChangedCallbackNode *node = kernel_malloc_check(sizeof(PFSFileChangedCallbackNode));
  *node = (PFSFileChangedCallbackNode) {
    .callback = callback,
    .event_flags = event_flags,
    .data = data
  };

  // find out if we already have a string for this particular filename
  ListNode *find_str = list_find(s_head_callback_node_list, watch_list_find_str, (char *)filename);
  if (find_str == NULL) {
    node->name = kernel_strdup_check(filename);
  } else {
    node->name = ((PFSFileChangedCallbackNode *)find_str)->name;
  }

  s_head_callback_node_list = list_prepend(s_head_callback_node_list, &node->list_node);
  mutex_unlock_recursive(s_pfs_mutex);

  return node;
}

void pfs_unwatch_file(PFSCallbackHandle cb_handle) {
  mutex_lock_recursive(s_pfs_mutex);

  PFSFileChangedCallbackNode *callback_node = (PFSFileChangedCallbackNode *)cb_handle;

  PBL_ASSERTN(callback_node->list_node.next != NULL || callback_node->list_node.prev != NULL
              || s_head_callback_node_list == &callback_node->list_node);
  PBL_ASSERTN(list_contains(s_head_callback_node_list, &callback_node->list_node));
  list_remove(&callback_node->list_node, &(s_head_callback_node_list), NULL);

  // if no one is watching the file anymore, free the string
  ListNode *find_str = list_find(s_head_callback_node_list, watch_list_find_str,
      (char *)callback_node->name);
  if (find_str == NULL) {
    kernel_free((void *)(callback_node->name));
  }

  kernel_free(callback_node);

  mutex_unlock_recursive(s_pfs_mutex);
}

// IMPORTANT: This call assumes that the caller has already grabbed s_pfs_mutex
static void prv_invoke_watch_file_callbacks(const char* file_name, uint8_t event) {
  PFSFileChangedCallbackNode *callback_node =
                              (PFSFileChangedCallbackNode *)s_head_callback_node_list;
  while (callback_node) {
    if (!strcmp(callback_node->name, file_name) && (callback_node->event_flags & event)) {
      callback_node->callback(callback_node->data);
    }
    callback_node = (PFSFileChangedCallbackNode *)list_get_next(&callback_node->list_node);
  }
}

status_t pfs_close(int fd) {
  mutex_lock_recursive(s_pfs_mutex);

  int res = E_UNKNOWN;
  if (!FD_VALID(fd)) {
    res = E_INVALID_ARGUMENT;
    goto cleanup;
  }

  File *f = &PFS_FD(fd).file;
  if (f->is_tmp) {
    // TODO: For safety, could disallow this op if user has orig file hdl open
    pfs_remove(f->name);
    // Note: if we reboot before updating the tmp state flag to done, the tmp &
    // original file will be deleted. This is an extremely small window, but
    // could be resolved by checking on reboot to see if both versions exist.
    // If both exist, the orig is valid. Iff tmp exists, the tmp file is valid
    update_curr_state(f->start_page, TMP_STATE_OFFSET, TMP_STATE_DONE);
    f->is_tmp = false;
  }

  // Note: We don't free f->name here because we keep the file metadata
  // (including the name, so we can detect hits) in the cache until we
  // have to evict it to make room for a new file.

  PFS_FD(fd).fd_status = FD_STATUS_UNREFERENCED;
  PFS_FD(fd).time_closed = time_closed_counter++;

  // If this file was modified, invoke the callbacks
  if (f->op_flags & (OP_FLAG_WRITE | OP_FLAG_OVERWRITE)) {
    // IMPORTANT: prv_invoke_watch_file_callbacks assumes that we already have s_pfs_mutex
    prv_invoke_watch_file_callbacks(f->name, FILE_CHANGED_EVENT_CLOSED);
  }

  res = S_SUCCESS;
cleanup:
  mutex_unlock_recursive(s_pfs_mutex);
  return (res);
}

status_t pfs_close_and_remove(int fd) {
  mutex_lock_recursive(s_pfs_mutex);

  status_t res = E_UNKNOWN;
  if (!FD_VALID(fd)) {
    res = E_INVALID_ARGUMENT;
  } else {
    File *f = &PFS_FD(fd).file;
    char file_name[f->namelen + 1];
    file_name[f->namelen] = '\0';
    memcpy(file_name, f->name, f->namelen);

    if ((res = pfs_close(fd)) >= 0) {
      res = pfs_remove(file_name);
    }
  }

  mutex_unlock_recursive(s_pfs_mutex);
  return (res);
}

status_t pfs_remove(const char *name) {
  if (name == NULL) {
    return E_INVALID_ARGUMENT;
  }
  size_t namelen = strlen(name);
  if ((namelen < 1) || (namelen > FILE_MAX_NAME_LEN)) {
    return (E_INVALID_ARGUMENT);
  }

  mutex_lock_recursive(s_pfs_mutex);
  uint16_t page = 0;
  int fd;
  status_t rv = get_avail_fd(name, &fd, false);
  if (rv >= FDAlreadyLoaded) { // the file is in the cache
    if (rv == FDBusy) {
      PBL_CROAK("Cannot delete %s, it is currently in use",
                PFS_FD(fd).file.name);
    }
    page = PFS_FD(fd).file.start_page;
    mark_fd_free(fd);
  } else if ((rv = locate_flash_file(name, &page)) != S_SUCCESS) {
    goto cleanup; // could not find the file on flash
  }

  rv = unlink_flash_file(page);
  // IMPORTANT: prv_invoke_watch_file_callbacks assumes that we already have s_pfs_mutex
  prv_invoke_watch_file_callbacks(name, FILE_CHANGED_EVENT_REMOVED);
cleanup:
  mutex_unlock_recursive(s_pfs_mutex);
  return (rv);
}

PFSFileListEntry *pfs_create_file_list(PFSFilenameTestCallback callback) {
  ListNode *head = NULL;

  mutex_lock_recursive(s_pfs_mutex);

  const int file_namelen_offset = FILEHEADER_OFFSET + offsetof(FileHeader, file_namelen);

  for (uint16_t pg = 0; pg < s_pfs_page_count; pg++) {
    PageHeader pg_hdr;
    pg_hdr.page_flags = prv_get_page_flags(pg);

    if (!IS_PAGE_TYPE(pg_hdr.page_flags, PAGE_FLAG_START_PAGE)) {
      continue; // only start pages contain file name info
    }

    FileHeader file_hdr;
    prv_flash_read((uint8_t *)&file_hdr.file_namelen, sizeof(file_hdr.file_namelen),
                   prv_page_to_flash_offset(pg) + file_namelen_offset);

    char file_name[file_hdr.file_namelen + 1];
    prv_flash_read((uint8_t *)file_name, file_hdr.file_namelen,
                   prv_page_to_flash_offset(pg) + FILE_NAME_OFFSET);
    file_name[file_hdr.file_namelen] = 0;

    if (callback && !callback(file_name)) {
      // Don't include
      continue;
    }

    // Make sure the rest of the page header contents are valid. We are doing this after the
    // filename filter call because it requires more flash reads and is likely slower than the
    // filter call.
    if (read_header(pg, &pg_hdr, &file_hdr) != PageAndFileHdrValid) {
      PBL_LOG(LOG_LEVEL_WARNING, "%d: Invalid page/file header", pg);
      continue;
    }

    // Add a new entry
    PFSFileListEntry *entry = kernel_malloc_check(sizeof(PFSFileListEntry)
                                                  + file_hdr.file_namelen + 1);
    *entry = (PFSFileListEntry) {};
    strcpy(entry->name, file_name);
    head = list_insert_before(head, &entry->list_node);
  }
  mutex_unlock_recursive(s_pfs_mutex);
  return (PFSFileListEntry *)head;
}

void pfs_delete_file_list(PFSFileListEntry *head) {
  ListNode *node = (ListNode *)head;
  ListNode *next;
  while (node) {
    next = node->next;
    kernel_free(node);
    node = next;
  }
}

// PBL-19098 Refactor this to share code with pfs_create_file_list
void pfs_remove_files(PFSFilenameTestCallback callback) {
  mutex_lock_recursive(s_pfs_mutex);

  const int file_namelen_offset = FILEHEADER_OFFSET + offsetof(FileHeader, file_namelen);

  for (uint16_t pg = 0; pg < s_pfs_page_count; pg++) {
    PageHeader pg_hdr;
    pg_hdr.page_flags = prv_get_page_flags(pg);

    if (!IS_PAGE_TYPE(pg_hdr.page_flags, PAGE_FLAG_START_PAGE)) {
      continue; // only start pages contain file name info
    }

    FileHeader file_hdr;
    prv_flash_read((uint8_t *)&file_hdr.file_namelen, sizeof(file_hdr.file_namelen),
                   prv_page_to_flash_offset(pg) + file_namelen_offset);

    char file_name[file_hdr.file_namelen + 1];
    prv_flash_read((uint8_t *)file_name, file_hdr.file_namelen,
                   prv_page_to_flash_offset(pg) + FILE_NAME_OFFSET);
    file_name[file_hdr.file_namelen] = 0;

    if (callback && !callback(file_name)) {
      // Don't include
      continue;
    }

    // Make sure the rest of the page header contents are valid. We are doing this after the
    // filename filter call because it requires more flash reads and is likely slower than the
    // filter call.
    if (read_header(pg, &pg_hdr, &file_hdr) != PageAndFileHdrValid) {
      PBL_LOG(LOG_LEVEL_WARNING, "%d: Invalid page/file header", pg);
      continue;
    }

    int fd;
    status_t rv = get_avail_fd(file_name, &fd, false);
    if (rv >= FDAlreadyLoaded) { // the file is in the cache
      if (rv == FDBusy) {
        PBL_CROAK("Cannot delete %s, it is currently in use",
                  s_pfs_avail_fd[fd].file.name);
      }
      mark_fd_free(fd);
    }

    unlink_flash_file(pg);
    // IMPORTANT: prv_invoke_watch_file_callbacks assumes that we already have s_pfs_mutex
    prv_invoke_watch_file_callbacks(file_name, FILE_CHANGED_EVENT_REMOVED);
  }
  mutex_unlock_recursive(s_pfs_mutex);
}

#define MAX_PAGE_CACHE_ENTRIES    10 // 6 bytes per entry
static void update_page_cache(FilePageCache *fpc, int *cur_idx,
    FilePageCache *toadd) {

  int optimal_idx = *cur_idx;
  if ((*cur_idx) == MAX_PAGE_CACHE_ENTRIES) {
    // default index to overwrite if nothing better is found
    optimal_idx = MAX_PAGE_CACHE_ENTRIES - 1;

    uint8_t contiguous_pgs = fpc[0].contiguous_pgs;

    // find the entry with the smallest number of sequential pages as this
    // will be the best page to remove from the cache
    for (int i = 0; i < MAX_PAGE_CACHE_ENTRIES; i++) {
      if (fpc[i].contiguous_pgs < contiguous_pgs) {
        optimal_idx = i;
        contiguous_pgs = fpc[i].contiguous_pgs;
      }
    }

    // only kick the current cache entry if it's worse than the one
    // we are adding
    if (fpc[optimal_idx].contiguous_pgs > toadd->contiguous_pgs) {
      return;
    }
  } else {
    (*cur_idx)++; // we are adding a new entry
  }

  fpc[optimal_idx] = *toadd;
}

static NOINLINE void allocate_page_cache(int fd) {
  File *f = &PFS_FD(fd).file;

  if (f->pg_cache != NULL) {
    return;  // already cached
  }

  if ((f->file_size / free_bytes_in_page(f->start_page)) < 1) {
    return; // only one page in use so we don't need to cache anything
  }

  // Note: If there was more space for statics or stack space we could
  // put this temporary buffer there
  FilePageCache *fpc =
      kernel_malloc_check(sizeof(FilePageCache) * MAX_PAGE_CACHE_ENTRIES);
  memset(fpc, 0x00, sizeof(FilePageCache) * MAX_PAGE_CACHE_ENTRIES);

  uint16_t virtual_pg = 0;
  uint16_t curr_page = f->start_page;
  uint16_t next_page;
  int cur_idx = 0;

  FilePageCache curr = {
    .virtual_pg = 0,
    .physical_pg = f->start_page,
    .contiguous_pgs = 0
  };

  while (get_next_page(curr_page, &next_page) == S_SUCCESS) {
    if (next_page == (curr_page + 1)) {
      curr.contiguous_pgs++;
    } else {
      update_page_cache(&fpc[0], &cur_idx, &curr);

      // reset logic for next entry
      curr.virtual_pg = virtual_pg + 1;
      curr.physical_pg = next_page;
      curr.contiguous_pgs = 0;
    }

    curr_page = next_page;
    virtual_pg++;
  }

  // see if the last set should be added to the cache
  update_page_cache(&fpc[0], &cur_idx, &curr);

  // The cache is likely to be around for a while and there is no reason to
  // burn up more memory than necessary for a long duration
  f->pg_cache = kernel_malloc(sizeof(FilePageCache) * cur_idx);
  if (f->pg_cache != NULL) { // if we are not OOM
    memcpy(f->pg_cache, fpc, sizeof(FilePageCache) * cur_idx);
    f->pg_cache_len = cur_idx;
  }

  kernel_free(fpc);
}

///
/// Helper routines for pfs_open()
///

//! Returns true iff the file is found in the cache and the fd is ready to use
//! fd_used >= 0 if we were able to allocate a fd for the file (regardless of
//! whether or not its in the cache), else it reflects the error code
static NOINLINE bool file_found_in_cache(const char *name, uint8_t op_flags, int *fd_used) {
  int fd, res;
  bool is_tmp = ((op_flags & OP_FLAG_OVERWRITE) != 0);
  bool file_found = false;

  if ((res = get_avail_fd(name, &fd, is_tmp)) == NoFDAvail) {
    res = E_OUT_OF_RESOURCES;
    goto cleanup;
  } else if (res == FDBusy) {
    res = E_BUSY; // the file is already open
    goto cleanup;
  }

  File *file = &PFS_FD(fd).file;

  // settings for cached & new fds
  file->op_flags = op_flags;
  file->offset = 0; // (re)set seek position
  file->is_tmp = is_tmp;

  if (res == FDAlreadyLoaded) { // we found the FD in cache!
    file->curr_page = file->start_page;

    bool perform_crc_check = (op_flags & OP_FLAG_SKIP_HDR_CRC_CHECK) == 0;
    if (perform_crc_check) {
      // make sure the header is not corrupted
      PageHeader pg_hdr;
      FileHeader file_hdr;
      if ((res = read_header(file->start_page, &pg_hdr, &file_hdr)) !=
          PageAndFileHdrValid) {
        mark_fd_free(fd); // file has been corrupted so clear fd
        goto cleanup;
      }
    }

    PFS_FD(fd).fd_status = FD_STATUS_IN_USE;
    file_found = true;
  }

cleanup:
  *fd_used = (res >= 0) ? fd : res;
  return (file_found);
}

// handles the creation of a file which was not previously on the FS
static NOINLINE status_t pfs_open_handle_create_request(int fd, uint8_t file_type,
    size_t start_size) {

  if (!VALID_TYPE(file_type) || (start_size == 0)) {
    return (E_INVALID_ARGUMENT);
  }

  File *file = &PFS_FD(fd).file;
  file->file_size = start_size;
  file->file_type = file_type;

  // temporarily mark the file as in use so no one tries to use the fd once we
  // release the lock
  if (fd != GC_FD_HANDLE_ID) {
    FileDesc *file_desc = &PFS_FD(fd);
    uint8_t curr_status = file_desc->fd_status;
    file_desc->fd_status = FD_STATUS_IN_USE;
    mutex_unlock_recursive(s_pfs_mutex);
    pfs_prepare_for_file_creation(start_size, 0 /* no timeout */);
    mutex_lock_recursive(s_pfs_mutex);
    file_desc->fd_status = curr_status;
  }

  int res = create_flash_file(file);
  return (res);
}

// given the fd and start page of a file, loads file description with relevent
// info about file so it can be read from
static NOINLINE status_t pfs_open_handle_read_request(int fd, uint16_t page) {
  PageHeader pg_hdr;
  FileHeader file_hdr;
  int hdr_rv;

  if ((hdr_rv = read_header(page, &pg_hdr, &file_hdr)) == PageAndFileHdrValid) {
    File *file = &PFS_FD(fd).file;
    file->file_size = file_hdr.file_size;
    file->file_type = file_hdr.file_type;
    file->start_page = file->curr_page = page;
    return (S_SUCCESS);
  }

  PBL_LOG(LOG_LEVEL_WARNING, "Could not read header %d", hdr_rv);
  return (E_INTERNAL);
}

static int file_found_or_added_to_pfs(int fd, const char *name,
    uint8_t op_flags, uint8_t file_type, size_t start_size) {

  uint16_t page = 0;
  int res = locate_flash_file(name, &page);

  if ((res != S_SUCCESS) && (res != E_DOES_NOT_EXIST)) { // unexpected error
    goto cleanup;
  }

  // check to see if we are trying to read the file and it doesn't exist
  bool is_read_only = (op_flags & (OP_FLAG_READ | OP_FLAG_WRITE |
      OP_FLAG_OVERWRITE)) == OP_FLAG_READ;
  bool is_tmp = ((op_flags & OP_FLAG_OVERWRITE) != 0);
  if ((is_read_only || is_tmp) && (res == E_DOES_NOT_EXIST)) {
    goto cleanup;
  }

  // Prepare the new FD
  FileDesc *file_desc = &PFS_FD(fd);
  File *file = &PFS_FD(fd).file;

  file_desc->fd_status = FD_STATUS_UNREFERENCED; // set to IN_USE on success
  if ((file->name = kernel_strdup(name)) == NULL) {
    res = E_OUT_OF_MEMORY;
    goto cleanup;
  }
  file->namelen = strlen(name);
  file->start_offset = FILEDATA_LEN + file->namelen;

  if (is_tmp || ((res == E_DOES_NOT_EXIST) && ((op_flags & OP_FLAG_WRITE) != 0))) {
    res = pfs_open_handle_create_request(fd, file_type, start_size);
  } else if ((op_flags & OP_FLAG_READ) != 0) {
    res = pfs_open_handle_read_request(fd, page);
  } else { // unexpected situation
    res = E_INTERNAL;
  }

cleanup:
  if (res < S_SUCCESS) {
    mark_fd_free(fd);
  } else {
    PFS_FD(fd).fd_status = FD_STATUS_IN_USE;
  }
  return (res);
}

int pfs_open(const char *name, uint8_t op_flags, uint8_t file_type,
    size_t start_size) {

  size_t namelen = (name == NULL) ? 0 : strlen(name);
  if ((namelen < 1) || (namelen > FILE_MAX_NAME_LEN)) {
    return (E_INVALID_ARGUMENT);
  }

  mutex_lock_recursive(s_pfs_mutex);
  int res;
  // if the file is in the cache or we encountered a failure we are done
  if (file_found_in_cache(name, op_flags, &res) || (res < S_SUCCESS)) {
    goto cleanup;
  }

  // The file is not in the cache, let's see if it's on the filesystem
  int fd = res;
  if ((res = file_found_or_added_to_pfs(fd, name, op_flags, file_type,
      start_size)) >= S_SUCCESS) {
    res = fd; // success so return the fd
  }

cleanup:
  if (res >= S_SUCCESS) {
    // we are returning a valid file handle so if the user has asked for the
    // page translations to be cached let's do that now
    if ((op_flags & OP_FLAG_USE_PAGE_CACHE) != 0) {
        allocate_page_cache(res);
    }
    // check to see if we should update the gc block
    prv_update_gc_reserved_region();
  }

  mutex_unlock_recursive(s_pfs_mutex);
  return (res);
}

static int pfs_open_gc_file(uint32_t space_needed, bool create) {
  int fd = GC_FD_HANDLE_ID; // the gc fd follows the avail fd
  File *file = &PFS_FD(fd).file;

  // settings for cached & new fds
  file->op_flags = OP_FLAG_READ;
  if (create) {
    file->op_flags |= OP_FLAG_WRITE;
  }
  file->offset = 0; // (re)set seek position
  file->is_tmp = false;

  if (s_gc_block.block_valid && create) {
    prv_flash_erase_sector(s_gc_block.gc_start_page);
  }

  int res = file_found_or_added_to_pfs(fd, GC_FILE_NAME, file->op_flags,
      FILE_TYPE_STATIC, space_needed);

  PBL_ASSERTN(!create || res >= 0); // we are toast if we cannot create the file
  return (res >= 0) ? fd : res;
}

static status_t copy_or_recover_gc_data(int fd, GCData *gcdata, bool do_copy) {
  //////
  //
  // GC File Format
  //
  // GCData
  // Page 0 Header | Data Len | Data
  // ...
  // Page N Header | Data Len | Data
  //
  //////
  uint32_t sector_start_page = gcdata->gc_start_page;
  uint32_t sectors_active = gcdata->page_mask;

  // copy the entire block to file (could use bss?)
  const size_t copy_buf_size = 256;
  uint8_t *buf = kernel_malloc_check(copy_buf_size);

  for (uint16_t pg = 0; pg < PFS_PAGES_PER_ERASE_SECTOR; pg++) {
    uint32_t base_addr = prv_page_to_flash_offset(sector_start_page + pg);

    uint32_t data_len;
    PageHeader hdr;
    if (do_copy) {
      // if the sector is not active we only need to copy the page header info
      data_len = (((sectors_active >> pg) & 0x1) == 0) ?
          0 : PFS_PAGE_SIZE - sizeof(PageHeader);
      if (data_len == 0) {
        get_updated_erase_hdr(&hdr, sector_start_page + pg);
      } else {
        prv_flash_read((uint8_t *)&hdr, sizeof(hdr), base_addr);
      }
    }

    // Write Page Header + DataLen
    if (do_copy) {
      pfs_write(fd, &hdr, sizeof(hdr));
      pfs_write(fd, &data_len, sizeof(data_len));
    } else { // recover
      pfs_read(fd, &hdr, sizeof(hdr));
      pfs_read(fd, &data_len, sizeof(data_len));
      prv_flash_write(&hdr, sizeof(hdr), base_addr);
    }

    base_addr += sizeof(PageHeader);
    for (int i = 0; i < (int)data_len; i += copy_buf_size) {
      size_t to_copy = MIN(data_len - i, copy_buf_size);
      if (do_copy) {
        prv_flash_read(buf, to_copy, base_addr + i);
        pfs_write(fd, buf, to_copy);
      } else {
        pfs_read(fd, buf, to_copy);
        prv_flash_write(buf, to_copy, base_addr + i);
      }
    }
  }

  kernel_free(buf);
  return (S_SUCCESS);
}

static void recover_region_from_file(int fd) {
  GCData gcdata;

  pfs_seek(fd, 0, FSeekSet);
  pfs_read(fd, &gcdata, sizeof(gcdata));

  if (!GCDATA_VALID(gcdata.flags)) {
    // we never completed setting up the migration
    goto done;
  }

  // at this point we can erase the block
  prv_handle_sector_erase(gcdata.gc_start_page, false);

  copy_or_recover_gc_data(fd, &gcdata, false);

done:
  pfs_close_and_remove(fd);
}

static int prv_copy_sector_to_gc_file(uint16_t *free_page,
    uint16_t sector_start_page, uint32_t sectors_active) {
  size_t num_entries = __builtin_popcount(sectors_active);

  // We need space to store all the data for active pages, the
  // page header for all pages, and the GCData struct
  size_t space_needed = 0;
  space_needed += (num_entries * (PFS_PAGE_SIZE - AVAIL_BYTES_OFFSET));
  space_needed += (PFS_PAGES_PER_ERASE_SECTOR * (sizeof(PageHeader) + 4));
  space_needed += sizeof(GCData);

  // we rely on having 1 page to store some metadata so make sure
  // we always have enough space based on our block & erase size
  _Static_assert((PFS_PAGES_PER_ERASE_SECTOR * (sizeof(PageHeader) + 4)) <
      (PFS_PAGE_SIZE - AVAIL_BYTES_OFFSET), "Too many pages per Erase sector");
  PBL_ASSERTN(num_entries < PFS_PAGES_PER_ERASE_SECTOR);

  int fd = pfs_open_gc_file(space_needed, true);
  GCData gcdata = {
    .version = 0, // Version 0 for now, bump if we change
    .flags = 0xff,
    .gc_start_page = sector_start_page,
    .num_entries = num_entries,
    .page_mask = sectors_active
  };

  // write out the GCData to the file
  pfs_write(fd, &gcdata, sizeof(gcdata));

  // copy all the data we need to the file
  copy_or_recover_gc_data(fd, &gcdata, true);

  // mark our data as valid
  gcdata.flags &= ~GC_DATA_VALID;
  pfs_seek(fd, offsetof(GCData, flags), FSeekSet);
  pfs_write(fd, &gcdata.flags, sizeof(gcdata.flags));

  return (fd);
}

static NOINLINE status_t garbage_collect_sector(uint16_t *free_page,
    uint16_t sector_start_page, uint32_t sectors_active) {

  // if no sectors are active in the region, just erase it!
  if (sectors_active == 0) {
    prv_handle_sector_erase(sector_start_page, true);
    goto done;
  }

  int fd = prv_copy_sector_to_gc_file(free_page, sector_start_page,
      sectors_active);

  recover_region_from_file(fd);

  // we used the gc block
  s_gc_block.block_writes++;

done:
  for (uint16_t pg = 0; pg < PFS_PAGES_PER_ERASE_SECTOR; pg++) {
    if (((sectors_active >> pg) & 0x1) == 0) {
      *free_page = pg + sector_start_page;
      return (S_SUCCESS);
    }
  }

  return (E_INTERNAL);
}

status_t pfs_init(bool run_filesystem_check) {
  if (s_pfs_mutex == NULL) {
    s_pfs_mutex = mutex_create_recursive();
  }

  for (int fd = FD_INDEX_OFFSET; fd < FD_INDEX_OFFSET+MAX_FD_HANDLES; fd++) {
    PFS_FD(fd) = (FileDesc) { .fd_status = FD_STATUS_FREE };
  }

  ftl_populate_region_list();

  if (run_filesystem_check) {
    if (!pfs_active()) {
      // either we have downgraded or there is no data on the flash
      PBL_LOG(LOG_LEVEL_INFO, "PFS not active ... formatting");
      pfs_format(true /* write erase headers */);
    }
  }

  // we need to run this before reserving a new GC region so that we don't
  // think a region is free when in reality we just rebooted in the middle of it
  // being re-written
  int fd;
  if ((fd = pfs_open_gc_file(0, false)) >= S_SUCCESS) {
    // we rebooted while we were in the middle of a garbage collection
    PBL_LOG(LOG_LEVEL_INFO, "Recovering flash region from GC file");
    recover_region_from_file(fd);
  }

  // find a free region
  if (!prv_update_gc_reserved_region()) {
    PBL_LOG(LOG_LEVEL_ERROR, "No free flash erase units!");
    // Note: It should not be possible for this to happen since start of day no
    // files will be written on then flash. We could also try to force apps to
    // be flushed out of the FS in an attempt to free up space since they are
    // only being cached on the FS
    pfs_format(true);
  }

  // get us off to a good start by ensuring there is some pre-erased space on
  // the filesystem. We do a lot of initialization from different threads early
  // during boot flow. This prevents those threads from blocking each other
  uint32_t bytes_to_free = ((s_pfs_page_count * PFS_PAGE_SIZE) * 4) / 100;
  PBL_LOG(LOG_LEVEL_DEBUG, "Preparing %"PRIu32" bytes of flash for filesystem use",
          bytes_to_free);

  pfs_prepare_for_file_creation(bytes_to_free, 15 * RTC_TICKS_HZ);

  return (S_SUCCESS);
}

void pfs_format(bool write_erase_headers) {
  PBL_LOG(LOG_LEVEL_INFO, "FS-Format Start");
  mutex_lock_recursive(s_pfs_mutex);

  for (int i = FD_INDEX_OFFSET; i < FD_INDEX_OFFSET+PFS_FD_SET_SIZE; i++) {
    mark_fd_free(i);
  }

  // clear out all pages
  filesystem_regions_erase_all();
  prv_invalidate_page_flags_cache_all();

  if (write_erase_headers) {
    prv_write_erased_header_on_page_range(0, s_pfs_page_count, 1);
  }

  mutex_unlock_recursive(s_pfs_mutex);
  PBL_LOG(LOG_LEVEL_INFO, "FS-Format Done");
}


int pfs_sector_optimal_size(int min_size, int namelen) {
  min_size += sizeof(FileHeader);
  min_size += sizeof(FileMetaData);
  min_size += namelen;

  int bytes_per_sector = PFS_PAGE_SIZE - sizeof(PageHeader);
  int num_pages = min_size / bytes_per_sector;
  if ((min_size % bytes_per_sector) > 0) {
    num_pages++;
  }
  int optimal_size = num_pages * bytes_per_sector;

  optimal_size -= sizeof(FileHeader);
  optimal_size -= sizeof(FileMetaData);
  optimal_size -= namelen;
  return optimal_size;
}

uint32_t get_available_pfs_space(void) {
  uint32_t allocated_space = 0;

  for (uint16_t pg = 0; pg < s_pfs_page_count; pg++) {
    uint8_t page_flags = prv_get_page_flags(pg);

    if ((IS_PAGE_TYPE(page_flags, PAGE_FLAG_START_PAGE)) ||
        (IS_PAGE_TYPE(page_flags, PAGE_FLAG_CONT_PAGE))) {
      allocated_space += free_bytes_in_page(pg);
    }
  }

  // A full filesystem is bad for wear leveling since the same sectors will
  // wind up getting written repeatedly. We should really be enforcing this
  // within pfs_open but for now we will just let external callers use this
  // routine before allocating large files
  uint32_t tot_capacity = (pfs_get_size() * 8) / 10;

  return ((allocated_space >= tot_capacity) ?
          0 : (tot_capacity - allocated_space));
}

uint32_t pfs_crc_calculate_file(int fd, uint32_t offset, uint32_t num_bytes) {
  LegacyChecksum checksum;
  legacy_defective_checksum_init(&checksum);

  // grab the pfs lock to prevent lock inversion with crc lock
  mutex_lock_recursive(s_pfs_mutex);

  // go to offset
  pfs_seek(fd, offset, FSeekSet);
  const unsigned int chunk_size = 128;
  uint8_t buffer[chunk_size];

  while (num_bytes > chunk_size) {
    pfs_read(fd, buffer, chunk_size);
    legacy_defective_checksum_update(&checksum, buffer, chunk_size);
    num_bytes -= chunk_size;
  }

  pfs_read(fd, buffer, num_bytes);
  legacy_defective_checksum_update(&checksum, buffer, num_bytes);
  uint32_t crc = legacy_defective_checksum_finish(&checksum);

  mutex_unlock_recursive(s_pfs_mutex);

  return (crc);
}

void analytics_external_collect_pfs_stats(void) {
  uint16_t avail_kilobytes = (uint16_t)(get_available_pfs_space() / 1024);
  analytics_set(ANALYTICS_DEVICE_METRIC_PFS_SPACE_FREE_KB,
                avail_kilobytes, AnalyticsClient_System);
}

/*
 * Debug Utilities
 */

// TODO: Remove once we figure out PBL-20973
void pfs_collect_diagnostic_data(int fd, void *diagnostic_buf, size_t diagnostic_buf_len)  {
  mutex_lock_recursive(s_pfs_mutex);
  memcpy(diagnostic_buf, &PFS_FD(fd), MIN(diagnostic_buf_len, sizeof(FileDesc)));
  mutex_unlock_recursive(s_pfs_mutex);
}

// pass in either 0 or 1 to as argument
void pfs_command_fs_format(const char *erase_headers) {
  int write_erase_headers = atoi(erase_headers);
  if (write_erase_headers == 1) {
    pfs_format(true /* write erase headers */);
  } else {
    pfs_format(false /* write erase headers */);
  }
}

void pfs_command_dump_hdr(const char *page) {
  uint16_t pg = (uint16_t) atoi(page);
  if (pg > s_pfs_page_count) {
    prompt_send_response("ERROR");
    return;
  }

  uint8_t hdr[FILE_NAME_OFFSET + 10];
  prv_flash_read((uint8_t *)&hdr, sizeof(hdr), prv_page_to_flash_offset(pg));

  PBL_HEXDUMP_D_SERIAL(LOG_LEVEL_DEBUG, hdr, sizeof(hdr));
}

void pfs_command_fs_ls(void) {
  char display_buf[80];
  int pages_in_use = 0;

  prompt_send_response("Page:\tFilename\tFile Size\tFile Info\tErase Count\n");

  for (uint16_t pg = 0; pg < s_pfs_page_count; pg++) {
    PageHeader pg_hdr;
    FileHeader file_hdr;
    pg_hdr.page_flags = prv_get_page_flags(pg);

    if (!IS_PAGE_TYPE(pg_hdr.page_flags, PAGE_FLAG_START_PAGE)) {
      pages_in_use += IS_PAGE_TYPE(pg_hdr.page_flags,
          PAGE_FLAG_CONT_PAGE) ? 1 : 0;
      continue; // only start pages contain file name info
    }
    pages_in_use++;

    if (read_header(pg, &pg_hdr, &file_hdr) != PageAndFileHdrValid) {
      snprintf(display_buf, sizeof(display_buf), "%3d: Corrupt Sector", pg);
      prompt_send_response(display_buf);
    }

    char file_name[file_hdr.file_namelen + 1];
    file_name[file_hdr.file_namelen] = '\0';

    prv_flash_read((uint8_t *)file_name, file_hdr.file_namelen,
        prv_page_to_flash_offset(pg) + FILE_NAME_OFFSET);

    snprintf(display_buf, sizeof(display_buf), "%3d:\t%8s%s\t%5d\t\t0x%x\t%15d",
        pg, file_name, is_tmp_file(pg) ? "(tmp)" : "", (int)file_hdr.file_size,
        file_hdr.file_type, (int)pg_hdr.erase_count);
    prompt_send_response(display_buf);
  }

  snprintf(display_buf, sizeof(display_buf), "\n---\n%d / %d pages in use "
      "(%"PRIu32" kB available)", pages_in_use, s_pfs_page_count,
       get_available_pfs_space() / 1024);
  prompt_send_response(display_buf);
}

// Dump the first n bytes of a file (from current seek position)
void pfs_debug_dump(int fd, int num_bytes) {
  char buf[16];
  uint8_t *bytes = kernel_malloc(num_bytes);

  if (bytes == NULL) {
    prompt_send_response("malloc error");
    goto cleanup;
  }

  memset(bytes, 0x00, num_bytes);
  if ((num_bytes = pfs_read(fd, bytes, num_bytes)) < 0) {
    prompt_send_response_fmt(buf, sizeof(buf), "rd err: %d", num_bytes);
    goto cleanup;
  }

  PBL_HEXDUMP_D_SERIAL(LOG_LEVEL_DEBUG, bytes, num_bytes);

  prompt_send_response("DONE");
cleanup:
  kernel_free(bytes);
}

void pfs_command_cat(const char *filename, const char *num_chars) {
  int fd = pfs_open(filename, OP_FLAG_READ, 0, 0);
  char buf[16];
  if (fd < 0) {
    prompt_send_response_fmt(buf, sizeof(buf), "fd open err: %d", fd);
    return;
  }
  int num_bytes = atoi(num_chars);
  pfs_debug_dump(fd, num_bytes);
  pfs_close(fd);
}

void pfs_command_crc(const char *filename) {
  int fd = pfs_open(filename, OP_FLAG_READ, 0, 0);
  char buffer[32];
  if (fd < 0) {
    prompt_send_response_fmt(buffer, sizeof(buffer), "fd open err: %d", fd);
    return;
  }
  size_t num_bytes = pfs_get_file_size(fd);
  uint32_t crc = pfs_crc_calculate_file(fd, 0, num_bytes);
  pfs_close(fd);
  prompt_send_response_fmt(buffer, sizeof(buffer), "CRC: %"PRIx32, crc);
}

/*
 * Routines to facilitate unit testing
 */
#if UNITTEST
uint16_t test_get_file_start_page(int fd) {
  return (PFS_FD(fd).file.start_page);
}

void test_force_garbage_collection(uint16_t start_page) {
  start_page  = (start_page / PFS_PAGES_PER_ERASE_SECTOR) * PFS_PAGES_PER_ERASE_SECTOR;

  uint16_t free_page;
  uint32_t active_sectors =
      prv_get_sector_page_status(start_page / PFS_PAGES_PER_ERASE_SECTOR , &free_page);

  garbage_collect_sector(&free_page, start_page, active_sectors);
}

status_t test_scan_for_last_written(void) {
  for (uint16_t pg = 0; pg < s_pfs_page_count; pg++) {
    PageHeader hdr;
    prv_flash_read((uint8_t *)&hdr.last_written, sizeof(hdr.last_written),
        prv_page_to_flash_offset(pg) + offsetof(PageHeader, last_written));
    if (hdr.last_written == LAST_WRITTEN_TAG) {
      return (pg);
    }
  }

  return (-1);
}

void test_force_recalc_of_gc_region(void) {
  s_gc_block.block_valid = false;
  prv_update_gc_reserved_region();
}

void test_force_reboot_during_garbage_collection(uint16_t start_page) {
  start_page =
      (start_page / PFS_PAGES_PER_ERASE_SECTOR) * PFS_PAGES_PER_ERASE_SECTOR;

  uint16_t free_page;
  uint32_t active_sectors = prv_get_sector_page_status(start_page, &free_page);

  prv_copy_sector_to_gc_file(&free_page, start_page, active_sectors);

  // blow away the sector
  prv_handle_sector_erase(s_gc_block.gc_start_page, false);
}

void test_override_last_written_page(uint16_t start_page) {
  s_test_last_page_written_override = s_last_page_written;
}
#endif
