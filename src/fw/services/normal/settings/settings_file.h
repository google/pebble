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

#include "settings_raw_iter.h"

// Deleted records have their key stick around for at least DELETED_LIFETIME
// before they can be garbage collected from the file in which they are
// contained, that way they have time to propegate to all devices we end up
// syncronizing with. For more information, refer to the sync protocol proposal:
// https://pebbletechnology.atlassian.net/wiki/pages/viewpage.action?pageId=26837564
//
// FIXME: See PBL-18945
#define DELETED_LIFETIME (0 * SECONDS_PER_DAY)

//! A SettingsFile is just a simple binary key-value store. Keys can be strings,
//! uint32_ts, or arbitrary bytes. Values are similarilly flexible. All
//! operations are atomic, so a reboot in the middle of changing the value for a
//! key will always either complete, returning the new value upon reboot, or
//! will just return the old value.
//! It also supports bidirectional syncronization between the phone & watch,
//! using timestamps to resolve conflicts.
//! Note that although all operations are atomic, they are not thread-safe. If
//! you will be accessing a SettingsFile from multiple threads, make sure you
//! use locks!

// NOTE: These fields are internal, modify them at your own risk!
typedef struct SettingsFile {
  SettingsRawIter iter;
  char *name;

  //! Maximum total space which can be used by this settings_file before a
  //! compaction will be forced. (Must be >= max_used_space)
  int max_space_total;

  //! Maximum space that can be used by valid records within this settings_file.
  //! Once this has been exceeded, attempting to add more keys or values will
  //! fail.
  int max_used_space;

  //! Amount of space in the settings_file that is currently dead, i.e.
  //! has been written to with some data, but that data is no longer valid.
  //! (overwritten records get added to this)
  int dead_space;

  //! Amount of space in the settings_file that is currently used by valid
  //! records.
  int used_space;

  //! When this file as a whole was last_modified.
  //! Defined as records.max(&:last_modified)
  uint32_t last_modified;

  //! The position of the current record in the iteration (if any). Necessary
  //! so that clients can read other records in the middle of iteration (i.e.
  //! settings_file_each()/settings_file_rewrite()),  without messing up the
  //! state of the iteration. Set to 0 if not in use.
  int cur_record_pos;
} SettingsFile;


//! max_used_space should be >= 5317 for persist files to make sure we can
//! always fit all of the records in the worst case (if the programmer stored
//! nothing but booleans).
//! Note: If the settings file already exists, the max_used_space parameter is
//! ignored. We could change this if the need arises.
status_t settings_file_open(SettingsFile *file, const char *name,
                            int max_used_space);
void settings_file_close(SettingsFile *file);

bool settings_file_exists(SettingsFile *file, const void *key, size_t key_len);
status_t settings_file_delete(SettingsFile *file,
                              const void *key, size_t key_len);

int settings_file_get_len(SettingsFile *file, const void *key, size_t key_len);
//! val_out_len must exactly match the length of the record on disk.
status_t settings_file_get(SettingsFile *file, const void *key, size_t key_len,
                           void *val_out, size_t val_out_len);
status_t settings_file_set(SettingsFile *file, const void *key, size_t key_len,
                           const void *val, size_t val_len);

//! Mark a record as synced. The flag will remain until the record is overwritten
//! @param file the settings_file that contains the record
//! @param key the key to the settings file. Note: keys can be up to 127 bytes
//! @param key_len the length of the key
status_t settings_file_mark_synced(SettingsFile *file, const void *key, size_t key_len);

//! set a byte in a setting. This can only be used a byte at a time to guarantee
//! atomicity. Do not use to modify several bytes in a row!
//! Note that only the reset bits will be applied (it writes flash directly)
status_t settings_file_set_byte(
    SettingsFile *file, const void *key, size_t key_len,
    size_t offset, uint8_t byte);



  //////////////////
 // Each/rewrite //
//////////////////
typedef void (*SettingsFileGetter)(SettingsFile *file,
                                   void *buf, size_t buf_len);

typedef struct {
  uint32_t last_modified;
  SettingsFileGetter get_key;
  int key_len;
  SettingsFileGetter get_val;
  int val_len;
  bool dirty; // has the dirty flag set
} SettingsRecordInfo;

//! Callback used for using settings_file_each.
//! The bool returned is used to control the iteration.
//! - If a callback returns true, the iteration continues
//! - If a callback returns false, the iteration stops.
typedef bool (*SettingsFileEachCallback)(SettingsFile *file,
                                         SettingsRecordInfo *info,
                                         void *context);
//! Calls cb for each and every entry within the given file.
//! Note that you cannot modify the settings file while iterating. If you want
//! to do this, try settings_file_rewrite instead. (you can read other entries
//! without fault).
status_t settings_file_each(SettingsFile *file, SettingsFileEachCallback cb,
                            void *context);


typedef void (*SettingsFileRewriteCallback)(SettingsFile *old_file,
                                            SettingsFile *new_file,
                                            SettingsRecordInfo *info,
                                            void *context);
//! Opens a new SettingsFile with the same name as the original SettingsFile,
//! in overwrite mode. This new file is passed into the given
//! SettingsFileRewriteCallback, which is called for each entry within the
//! original file. If you desire to preserve a key/value pair, you must write
//! it to the new file.
status_t settings_file_rewrite(SettingsFile *file,
                               SettingsFileRewriteCallback cb,
                               void *context);


//! Callback used for using settings_file_rewrite_filtered.
//! The bool returned is used to control whether or not the record is included in the file
//! after compaction. This callback is not allowed to use any other settings_file calls.
//! - If callback returns true, the record is included
//! - If callback returns false, the record is not included
typedef bool (*SettingsFileRewriteFilterCallback)(void *key, size_t key_len, void *value,
                                                  size_t value_len, void *context);

//! Opens a new SettingsFile with the same name as the original SettingsFile,
//! in overwrite mode. Any records from the old file which pass through the filter_cb with
//! a true result are included into the new file. This call is much faster than using
//! settings_file_rewrite if all you are doing is excluding specific records from the old file.
status_t settings_file_rewrite_filtered(SettingsFile *file,
                                        SettingsFileRewriteFilterCallback filter_cb, void *context);
