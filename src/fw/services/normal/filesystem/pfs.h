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

#include <inttypes.h>
#include <stddef.h>

#include "kernel/pebble_tasks.h"
#include "system/status_codes.h"
#include "util/list.h"

//! Exported APIs for the Pebble File System (PFS)
//!
//! Things to note:
//!  - All APIs are threadsafe
//!  - PFS implements a basic wear-leveling strategy to extend the life of
//!    the flash part
//!  - PFS allows the allocation of blocks of space which appear to the consumer
//!    as a contiguous region. It is up to the consumer to manage how they
//!    want to manage the allocated space
//!  - Assumes underlying HW is a NOR flash chip. This means that when a
//!    0 bit value is written to a given location, the file needs to be erased
//!    or rewritten to change it back to a 1. (pfs_open (i.e OP_FLAG_OVERWRITE)
//!    provides a mechanism that consumers can leverage to accomplish this)
//!  - Erasing flash sectors is a costly operation (from both a time/power
//!    perspective). Care should be taken not to constantly be deleting/creating
//!    files

#define OP_FLAG_READ                  (1 << 0)
#define OP_FLAG_WRITE                 (1 << 1)
#define OP_FLAG_OVERWRITE             (1 << 2)
#define OP_FLAG_SKIP_HDR_CRC_CHECK    (1 << 3)
#define OP_FLAG_USE_PAGE_CACHE        (1 << 4)

#define FILE_TYPE_STATIC        (0xfe)
#define FILE_MAX_NAME_LEN       (255)

typedef enum {
  FSeekSet,
  FSeekCur
} FSeekType;

//! Used by pfs_watch_file to know which events to trigger callbacks on
#define FILE_CHANGED_EVENT_CLOSED   (1 << 0)
#define FILE_CHANGED_EVENT_REMOVED  (1 << 1)
#define FILE_CHANGED_EVENT_ALL      (FILE_CHANGED_EVENT_CLOSED | FILE_CHANGED_EVENT_REMOVED)

//! Types used by pfs_watch_file()
typedef void (*PFSFileChangedCallback)(void *data);
typedef void *PFSCallbackHandle;

//! Used by pfs_list_files() and pfs_remove_files()
typedef bool (*PFSFilenameTestCallback)(const char *name);

//! Format of each entry in the linked list returned by pfs_create_file_list
typedef struct {
  ListNode list_node;
  char name[];
} PFSFileListEntry;


//! @param name - The name of the file to be opened
//! @param op_flags - The operation to be performed on the file
//!
//!   OP_FLAG_READ - Open a file such that pfs_read operations will work. If the
//!    file does not exist, opening a file with just this mode set will fail
//!
//!   OP_FLAG_WRITE - Creates a file if it does not exist, else opens a file
//!    such that pfs_write operations will work. It is up to the user to seek to
//!    the desired offset within the file
//!
//!   OP_FLAG_OVERWRITE - Provides a safe mechanism to incrementally overwrite
//!    a file that already exists with new data. Open will fail if the file does
//!    not already exist on flash. The changes for the overwritten file are not
//!    committed until the pfs_close is called. Until this time, pfs_open of the
//!    'name' will return a hdl to the original file. This way there is always a
//!    valid version of the file which can be read & the caller can copy parts
//!    of the original file in hunks rather than allocating a lot of RAM.
//!
//!   OP_FLAG_SKIP_HDR_CRC_CHECK - For files which are not accessed frequently,
//!    it is a good idea to sanity check the on-flash header CRCs to make sure
//!    nothing has gone astray. This has performance ramifications if you are
//!    doing thousands of opens on the same file so this flags allows consumers
//!    to turn the check off
//!
//!   OP_FLAG_USE_PAGE_CACHE - Turns on caching for the translation from
//!    virtual filesystem pages to their physical address. For large files with
//!    a lot of random access this is advantageous because we need to read
//!    flash bytes to get to the correct page. Ideally this should only be
//!    used for read operations so that heap corruption does not lead to us
//!    corrupting a file
//!
//! The following two parameters are only parsed when a file is
//! overwritten/created:
//!
//! @param file_type  - The type of file being opened
//! @param start_size - The initial space to be allocated for a file if it is
//!                     being created
//! @return - status_t error code if the operation failed,
//!           else a fd handle >= 0 if operation was successful
extern int pfs_open(const char *name, uint8_t op_flags, uint8_t file_type,
    size_t start_size);

//! Writes data to the fd specified. After each write, the internal file offset
//! is moved forward
//! @param fd - The fd to write data to
//! @param buf - The buffer of data to write
//! @param size - The number of bytes from buf to write
//!               (must be <= the size of buf)
//! @return - the number of bytes written or a status_t code on error
extern int pfs_write(int fd, const void *buf, size_t size);

//! Reads data from the fd specified. After each read, the internal file offset
//! is moved forward
//! @param fd - the file to read data from
//! @param buf - the buffer to store read data in
//! @param size - the amount of data to read (must be <= the size of buf)
//! @return - the number of bytes read or a status_t code on error
extern int pfs_read(int fd, void *buf, size_t size);

//! Seeks to offset specified
//! Returns the offset forwarded to on success,
//! status_t code < 0 to indicate type of failure
extern int pfs_seek(int fd, int offset, FSeekType seek_type);

//! Frees up internal tracking data associated with a given file.
//! @param fd - the fd to close
//! @return - S_SUCCESS or appropriate error code on failure
extern status_t pfs_close(int fd);

//! calls pfs_close and pfs_remove on a file successively
//! @param fd - the fd describing the file to remove
extern status_t pfs_close_and_remove(int fd);

//! Unlinks a given file from the filesystem
//! @param name - the name of the file to remove
//! @return - S_SUCCESS or appropriate error code on failure
extern status_t pfs_remove(const char *name);

//! Returns the size of the file. (The amount of bytes that can be read out)
extern size_t pfs_get_file_size(int fd);

//! Should only be called before using FS
extern status_t pfs_init(bool run_filesystem_check);

//! Should only be called once after reboot and before any file operations
//! are performed
extern void pfs_reboot_cleanup(void);

//! erases everything on the filesystem & removes any open
//! file entries from the cache
//! Note: assumes that pfs_init was called before this
extern void pfs_format(bool write_erase_headers);

//! Returns the size of the pfs filesystem.
extern uint32_t pfs_get_size(void);

//! Updates the size of the pfs filesystem.
//! @param new_size new size
//! @param new_region_erased if the pages being added have been erased and should be marked as so.
extern void pfs_set_size(uint32_t new_size, bool new_region_erased);

//! Returns true is pfs is active on this device
extern bool pfs_active(void);

//! Returns true is pfs is active in the region
extern bool pfs_active_in_region(uint32_t start_address, uint32_t ending_address);

//! In the case of a file which can actually make use of additional space
//! beyond a certain minimum, this function will return the optimal size
//! that should be used for such a file, in order to use no more sectors
//! than the minimum size would.
extern int pfs_sector_optimal_size(int min_size, int namelen);

//! Returns the number of bytes available on the filesystem
extern uint32_t get_available_pfs_space(void);

//! Watch a file. The callback is called whenever the given file (by name) is closed with
//! modifications or deleted
//! @param filename - name of the file to watch
//! @param callback - function to invoke when file is changed
//! @param event_flags - which events the callback should be triggered on
//! (see FILE_CHANGED_EVENT_ flags defined above)
//! @param data - pointer passed to callback when invoked
//! @return - cb handle to pass into \ref pfs_unwatch_file
PFSCallbackHandle pfs_watch_file(const char* filename, PFSFileChangedCallback callback,
                                 uint8_t event_flags, void* data);

//! Stop watching a file.
void pfs_unwatch_file(PFSCallbackHandle cb_handle);

//! calculate the CRC32 for a given part of a file
extern uint32_t pfs_crc_calculate_file(int fd, uint32_t offset, uint32_t num_bytes);

//! Get a directory listing, calling the filter callback on each filename.
//! Returns linked list of filenames, filtered by the callback.
//! @param callback - name filter function to be called for each filename, or NULL to include
//!          all files.
//! @return - pointer to head node of linked list of names, or NULL if no names match
extern PFSFileListEntry *pfs_create_file_list(PFSFilenameTestCallback callback);

//! Delete a directory list returned by pfs_list_files
//! @param callback - callback to be called for on filename
//! @return - pointer to head node of linked list of names, or NULL if no names match
extern void pfs_delete_file_list(PFSFileListEntry *list);

//! Run each filename in the filesystem through the filter callback and delete all files that match
//! @param callback - callback to be called for on filename
extern void pfs_remove_files(PFSFilenameTestCallback callback);
