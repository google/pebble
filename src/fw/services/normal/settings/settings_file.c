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

#include "settings_file.h"
#include "settings_raw_iter.h"

#include "drivers/rtc.h"
#include "kernel/pbl_malloc.h"
#include "services/normal/filesystem/pfs.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/crc8.h"

#include <string.h>
#include <time.h>

static status_t bootup_check(SettingsFile *file);
static void compute_stats(SettingsFile *file);

static bool file_hdr_is_uninitialized(SettingsFileHeader *file_hdr) {
  return (file_hdr->magic == 0xffffffff) && (file_hdr->version == 0xffff)
      && (file_hdr->flags == 0xffff);
}

static status_t prv_open(SettingsFile *file, const char *name, uint8_t flags, int max_used_space) {
  // Making the max_space_total at least a little bit larger than the
  // max_used_space allows us to avoid thrashing. Without it, if
  // max_space_total == max_used_space, then if the file is full, changing a
  // single value would force the whole file to be rewritten- every single
  // time! It's probably worth it to "waste" a bit of flash space to avoid
  // this pathological case.
  int max_space_total = pfs_sector_optimal_size(max_used_space * 12 / 10, strlen(name));

  // TODO: Dynamically sized files?
  int fd = pfs_open(name, flags, FILE_TYPE_STATIC, max_space_total);
  if (fd < 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Could not open settings file '%s', %d", name, fd);
    if (fd == E_BUSY) {
      // This is very bad. Someone didn't use a mutex. There could already be
      // silent corruption, so it's better to crash now rather than let things
      // get even more scrambled.
      PBL_CROAK("Settings file is already open!");
    }
    return fd;
  }

  *file = (SettingsFile) {
    .name = kernel_strdup_check(name),
    .max_used_space = max_used_space,
    .max_space_total = max_space_total,
  };

  settings_raw_iter_init(&file->iter, fd, file->name);

  SettingsFileHeader file_hdr = file->iter.file_hdr;
  if (file_hdr_is_uninitialized(&file_hdr)) {
    // Newly created file, create & write out header.
    memcpy(&file_hdr.magic, SETTINGS_FILE_MAGIC, sizeof(file_hdr.magic));
    file_hdr.version = SETTINGS_FILE_VERSION;
    settings_raw_iter_write_file_header(&file->iter, &file_hdr);
  }

  if (memcmp(&file_hdr.magic, SETTINGS_FILE_MAGIC, sizeof(file_hdr.magic)) != 0) {
    PBL_LOG(LOG_LEVEL_ERROR, "Attempted to open %s, not a settings file.", name);
    pfs_close_and_remove(fd);
    return E_INVALID_OPERATION;
  }

  if (file_hdr.version > SETTINGS_FILE_VERSION) {
    PBL_LOG(LOG_LEVEL_WARNING,
            "Unrecognized version %d for file %s, removing...",
            file_hdr.version, name);
    pfs_close_and_remove(fd);
    return prv_open(file, name, flags, max_used_space);
  }

  status_t status = bootup_check(file);
  if (status < 0) {
    PBL_LOG(LOG_LEVEL_ERROR,
            "Bootup check failed (%"PRId32"), not good. "
            "Attempting to recover by deleting %s...", status, name);
    pfs_close_and_remove(fd);
    return prv_open(file, name, flags, max_used_space);
  }

  // There's a chance that the caller increased the desired size of the settings file since
  // the file was originally created (i.e. the file was created in an earlier version of the
  // firmware). If we detect that situation, let's re-write the file to the new larger requested
  // size.
  int actual_size = pfs_get_file_size(file->iter.fd);
  if (actual_size < max_space_total) {
    PBL_LOG(LOG_LEVEL_INFO, "Re-writing settings file %s to increase its size from %d to %d.",
            name, actual_size, max_space_total);
    // The settings_file_rewrite_filtered call creates a new file based on file->max_used_space
    // and copies the contents of the existing file into it.
    status = settings_file_rewrite_filtered(file, NULL, NULL);
    if (status < 0) {
      PBL_LOG(LOG_LEVEL_ERROR, "Could not resize file %s (error %"PRId32"). Creating new one",
              name, status);
      return prv_open(file, name, flags, max_used_space);
    }
  }

  compute_stats(file);

  return S_SUCCESS;
}

status_t settings_file_open(SettingsFile *file, const char *name,
                            int max_used_space) {
  return prv_open(file, name, OP_FLAG_READ | OP_FLAG_WRITE, max_used_space);
}

void settings_file_close(SettingsFile *file) {
  settings_raw_iter_deinit(&file->iter);
  kernel_free(file->name);
  file->name = NULL;
}

static int record_size(SettingsRecordHeader *hdr) {
  return sizeof(*hdr) + hdr->key_len + hdr->val_len;
}

// Flags are stored in flash the inverse of how you might normally expect- a
// zero denotes that the flag is set, a 1 means it is not. This is because our
// flash chip is NOR flash, and thus is all 1's by default.
// Once setting a flag, we cannot unset it.
static void set_flag(SettingsRecordHeader *hdr, uint8_t flags) {
  hdr->flags &= ~flags;
}
static void clear_flag(SettingsRecordHeader *hdr, uint8_t flags) {
  hdr->flags |= flags;
}
static bool flag_is_set(SettingsRecordHeader *hdr, uint8_t flags) {
  return (hdr->flags & flags) == 0;
}

// Records have 4 possible states:
// - EOF marker: Header is all 1s
// - partially_written: Some bits in the header have been changed to 0s, but
//     the entire record has not been completely written yet. Records in this
//     state are removed on bootup, since they are in an indeterminate state.
// - written: The typical state for a record. == !partially_written
// - partially_overwritten: This record has been superceeded by another, which
//     we are currently in the process of writing out to flash. Records in
//     this state are restored on bootup.
// - overwritten: This record has been superceeded by another, which has been
//     completely written out to flash. We skip over and ignore overwritten
//     records.
static bool partially_written(SettingsRecordHeader *hdr) {
  return !flag_is_set(hdr, SETTINGS_FLAG_WRITE_COMPLETE);
}
static bool partially_overwritten(SettingsRecordHeader *hdr) {
  return flag_is_set(hdr, SETTINGS_FLAG_OVERWRITE_STARTED)
      && !flag_is_set(hdr, SETTINGS_FLAG_OVERWRITE_COMPLETE);
}
static bool overwritten(SettingsRecordHeader *hdr) {
  return flag_is_set(hdr, SETTINGS_FLAG_OVERWRITE_STARTED)
      && flag_is_set(hdr, SETTINGS_FLAG_OVERWRITE_COMPLETE);
}

static uint32_t utc_time() {
  return rtc_get_time();
}

static bool deleted_and_expired(SettingsRecordHeader *hdr) {
  return (hdr->val_len == 0)
      && (hdr->last_modified <= (utc_time() - DELETED_LIFETIME));
}

static void compute_stats(SettingsFile *file) {
  file->dead_space = 0;
  file->used_space = 0;
  file->last_modified = 0;
  file->used_space += sizeof(SettingsFileHeader);
  file->used_space += sizeof(SettingsRecordHeader); // EOF Marker
  for (settings_raw_iter_begin(&file->iter); !settings_raw_iter_end(&file->iter);
       settings_raw_iter_next(&file->iter)) {
    if (overwritten(&file->iter.hdr) || deleted_and_expired(&file->iter.hdr)) {
      file->dead_space += record_size(&file->iter.hdr);
    } else {
      file->used_space += record_size(&file->iter.hdr);
    }
    if (file->iter.hdr.last_modified > file->last_modified) {
      file->last_modified = file->iter.hdr.last_modified;
    }
  }
}

status_t settings_file_rewrite_filtered(
    SettingsFile *file, SettingsFileRewriteFilterCallback filter_cb, void *context) {
  SettingsFile new_file;
  status_t status = prv_open(&new_file, file->name, OP_FLAG_OVERWRITE | OP_FLAG_READ,
                             file->max_used_space);
  if (status < 0) {
    PBL_LOG(LOG_LEVEL_ERROR,
            "Could not open temporary file to compact settings file. Error %"PRIi32".",
            status);
    return status;
  }

  settings_raw_iter_begin(&new_file.iter);

  for (settings_raw_iter_begin(&file->iter); !settings_raw_iter_end(&file->iter);
      settings_raw_iter_next(&file->iter)) {
    SettingsRecordHeader *hdr = &file->iter.hdr;
    if (partially_written(hdr)) {
      // This should only happen if we reboot in the middle of writing a new
      // record, and it should always be the most recently written record, so
      // we shouldn't lose any data here (except for the partially written
      // record, but we should ignore it's garbage data anyway).
      break;
    }
    if (overwritten(hdr) || deleted_and_expired(hdr)) {
      continue;
    }
    if (partially_overwritten(hdr)) {
      // The only case where we should hit this is if we are compacting a file
      // which has a record which was in the middle of being overwritten, but
      // the write of the new record didn't finish by the time we rebooted.
      // There should only ever be one such record, and we shouldn't ever hit
      // this if writing the new record actually completed, since we check
      // for this case in bootup_check().
      clear_flag(hdr, SETTINGS_FLAG_OVERWRITE_STARTED);
    }

    // Get the old key and value
    uint8_t *key = kernel_malloc(hdr->key_len);
    settings_raw_iter_read_key(&file->iter, key);
    uint8_t *val = kernel_malloc(hdr->val_len);
    settings_raw_iter_read_val(&file->iter, val, hdr->val_len);

    // Include in re-written file if it passes the filter
    if (!filter_cb || filter_cb(key, hdr->key_len, val, hdr->val_len, context)) {
      settings_raw_iter_write_header(&new_file.iter, hdr);
      settings_raw_iter_write_key(&new_file.iter, key);
      settings_raw_iter_write_val(&new_file.iter, val);
      settings_raw_iter_next(&new_file.iter);
    }
    kernel_free(key);
    kernel_free(val);
  }
  settings_file_close(file);
  // We have to close and reopen the new_file so that it's temp flag is cleared.
  // Before the close succeeds, if we reboot, we will just end up reading the
  // old file. After the close succeeds, we will end up reading the new
  // (compacted) file.
  char *name = kernel_strdup(new_file.name);
  settings_file_close(&new_file);
  status = prv_open(file, name, OP_FLAG_READ | OP_FLAG_WRITE, file->max_used_space);
  kernel_free(name);
  return status;
}

T_STATIC status_t settings_file_compact(SettingsFile *file) {
  return settings_file_rewrite_filtered(file, NULL, NULL);
}

static bool key_matches(SettingsRawIter *iter, const uint8_t *key, int key_len) {
  SettingsRecordHeader *hdr = &iter->hdr;
  if (key_len != hdr->key_len) {
    return false;
  }
  if (crc8_calculate_bytes(key, key_len, true /* big_endian */) != hdr->key_hash) {
    return false;
  }
  uint8_t hdr_key[hdr->key_len];
  settings_raw_iter_read_key(iter, hdr_key);
  if (memcmp(key, hdr_key, hdr->key_len) == 0) {
    return true;
  }
  return false;
}

static bool prv_is_desired_hdr(SettingsRawIter *iter, const uint8_t *key, int key_len) {
  if (overwritten(&iter->hdr) || partially_written(&iter->hdr)) {
    return false;
  }

  return key_matches(iter, key, key_len);
}

static bool search_forward(SettingsRawIter *iter, const uint8_t *key, int key_len) {
  const int resumed_pos = settings_raw_iter_get_resumed_record_pos(iter);

  // Resume searching at the current record
  for (; !settings_raw_iter_end(iter); settings_raw_iter_next(iter)) {
    if (prv_is_desired_hdr(iter, key, key_len)) {
      return true;
    }
  }

  // If we got here, we didn't find it between `resumed_pos` and the last record in the file.
  // Wrap around to the beginning and search until we get to the `resumed_pos`
  settings_raw_iter_begin(iter);
  for (; settings_raw_iter_get_current_record_pos(iter) < resumed_pos;
         settings_raw_iter_next(iter)) {
    if (prv_is_desired_hdr(iter, key, key_len)) {
      return true;
    }
  }

  // No record found
  return false;
}

static status_t cleanup_partial_transactions(SettingsFile *file) {
  for (settings_raw_iter_begin(&file->iter); !settings_raw_iter_end(&file->iter);
      settings_raw_iter_next(&file->iter)) {

    if (partially_written(&file->iter.hdr)) {
      // Compact will remove partially written records. We could be smarter,
      // but this is something of an edge case.
      return settings_file_compact(file);
    }

    if (!partially_overwritten(&file->iter.hdr)) {
      continue;
    }

    int partially_overwritten_record_pos =
        settings_raw_iter_get_current_record_pos(&file->iter);
    uint8_t key[file->iter.hdr.key_len];
    settings_raw_iter_read_key(&file->iter, key);
    settings_raw_iter_next(&file->iter); // Skip the current record
    bool found_another = search_forward(&file->iter, key, file->iter.hdr.key_len);

    if (!found_another) {
      // No other file->iter.hdr found, we must have rebooted in the middle of
      // writing the new record. Compacting the file will copy over the
      // previous record while clearing the overwrite bits for us, so that we
      // can still find the previous record. We could be smarter here, but
      // this should happen pretty rarely.
      return settings_file_compact(file);
    }

    // The overwrite completed, we just rebooted before getting a chance
    // to flip the completion bit on the previous record. Flip it now so
    // that we don't have to keep checking on every boot.
    settings_raw_iter_set_current_record_pos(&file->iter,
                                        partially_overwritten_record_pos);
    set_flag(&file->iter.hdr, SETTINGS_FLAG_OVERWRITE_COMPLETE);
    settings_raw_iter_write_header(&file->iter, &file->iter.hdr);
  }
  return S_SUCCESS;
}

static status_t bootup_check(SettingsFile* file) {
  return cleanup_partial_transactions(file);
}

int settings_file_get_len(SettingsFile *file, const void *key, size_t key_len) {
  settings_raw_iter_resume(&file->iter);
  if (search_forward(&file->iter, key, key_len)) {
    return file->iter.hdr.val_len;
  } else {
    return 0;
  }
}

bool settings_file_exists(SettingsFile *file, const void *key, size_t key_len) {
  return (settings_file_get_len(file, key, key_len) > 0);
}

status_t settings_file_get(SettingsFile *file, const void *key, size_t key_len,
                           void *val_out, size_t val_out_len) {
  settings_raw_iter_resume(&file->iter);
  if (!search_forward(&file->iter, key, key_len)) {
    memset(val_out, 0, val_out_len);
    return E_DOES_NOT_EXIST;
  }
  if (deleted_and_expired(&file->iter.hdr)) {
    memset(val_out, 0, val_out_len);
    return E_DOES_NOT_EXIST;
  }
  size_t val_len = file->iter.hdr.val_len;
  if (val_out_len > val_len) {
    memset(val_out, 0, val_out_len);
    return E_RANGE;
  }
  settings_raw_iter_read_val(&file->iter, val_out, val_out_len);
  return S_SUCCESS;
}

status_t settings_file_set_byte(SettingsFile *file, const void *key,
                                size_t key_len, size_t offset, uint8_t byte) {
  if (key_len > SETTINGS_KEY_MAX_LEN) {
    return E_RANGE;
  }

  // Find the record
  settings_raw_iter_resume(&file->iter);
  if (!search_forward(&file->iter, key, key_len) ||
      file->iter.hdr.val_len == 0) {
    return E_DOES_NOT_EXIST;
  }

  PBL_ASSERTN(offset < file->iter.hdr.val_len);
  settings_raw_iter_write_byte(&file->iter, offset, byte);

  return S_SUCCESS;
}

// Note that this operation is designed to be atomic from the perspective of
// an outside observer. That is, either the new value will be completely
// written and returned for all future queries, or, if we reboot/loose power/
// run into an error, then we will continue to return the previous value.
// We should never run into a case where neither value exists.
status_t settings_file_set(SettingsFile *file, const void *key, size_t key_len,
                           const void *val, size_t val_len) {
  // Cannot set keys while iterating (Try settings_file_rewrite)
  PBL_ASSERTN(file->cur_record_pos == 0);
  if (key_len > SETTINGS_KEY_MAX_LEN) {
    return E_RANGE;
  }
  if (val_len > SETTINGS_VAL_MAX_LEN) {
    return E_RANGE;
  }
  const bool is_delete = (val_len == 0);
  const int rec_size = sizeof(SettingsRecordHeader) + key_len + val_len;
  if (!is_delete && file->used_space + rec_size > file->max_used_space) {
    return E_OUT_OF_STORAGE;
  }
  if (file->used_space + file->dead_space + rec_size > file->max_space_total) {
    status_t status = settings_file_compact(file);
    if (status < 0) {
      return status;
    }
  }

  int overwritten_record = -1;
  // Find an existing record, if any, and mark it as overwrite-in-progress.
  settings_raw_iter_resume(&file->iter);
  if (search_forward(&file->iter, key, key_len)) {
    set_flag(&file->iter.hdr, SETTINGS_FLAG_OVERWRITE_STARTED);
    settings_raw_iter_write_header(&file->iter, &file->iter.hdr);
    overwritten_record = settings_raw_iter_get_current_record_pos(&file->iter);
  }

  while (!settings_raw_iter_end(&file->iter)) {
    settings_raw_iter_next(&file->iter);
  }

  // Create and write out a new record. Writing the header transitions us into
  // the write-in-progress state, since at least once of the bits must be
  // flipped from a 1 to a 0 in order for the header to be valid.
  SettingsRecordHeader new_hdr;
  memset(&new_hdr, 0xff, sizeof(new_hdr));
  new_hdr.last_modified = utc_time();
  new_hdr.key_hash = crc8_calculate_bytes(key, key_len, true /* big_endian */);
  new_hdr.key_len = key_len;
  new_hdr.val_len = val_len;

  settings_raw_iter_write_header(&file->iter, &new_hdr);
  settings_raw_iter_write_key(&file->iter, key);
  settings_raw_iter_write_val(&file->iter, val);

  // Mark the new record as write complete, now that we have completely written
  // out the header, key, and value.
  set_flag(&new_hdr, SETTINGS_FLAG_WRITE_COMPLETE);
  settings_raw_iter_write_header(&file->iter, &new_hdr);
  file->used_space += rec_size;

  // Finally, mark the existing record, if any, as overwritten.
  if (overwritten_record >= 0) {
    settings_raw_iter_set_current_record_pos(&file->iter, overwritten_record);
    set_flag(&file->iter.hdr, SETTINGS_FLAG_OVERWRITE_COMPLETE);
    settings_raw_iter_write_header(&file->iter, &file->iter.hdr);
    file->dead_space += record_size(&file->iter.hdr);
    file->used_space -= record_size(&file->iter.hdr);
  }

  return S_SUCCESS;
}

status_t settings_file_mark_synced(SettingsFile *file, const void *key, size_t key_len) {
  // Cannot set keys while iterating (Try settings_file_rewrite)
  PBL_ASSERTN(file->cur_record_pos == 0);
  if (key_len > SETTINGS_KEY_MAX_LEN) {
    return E_RANGE;
  }

  // Find an existing record, if any, and mark it as synced
  settings_raw_iter_resume(&file->iter);
  if (search_forward(&file->iter, key, key_len)) {
    set_flag(&file->iter.hdr, SETTINGS_FLAG_SYNCED);
    settings_raw_iter_write_header(&file->iter, &file->iter.hdr);
    return S_SUCCESS;
  }

  return E_DOES_NOT_EXIST;
}

status_t settings_file_delete(SettingsFile *file,
                              const void *key, size_t key_len) {
  return settings_file_set(file, key, key_len, NULL, 0);
}

static void prv_get_key(SettingsFile *file, void *key, size_t key_len) {
  PBL_ASSERTN(key_len <= file->iter.hdr.key_len);
  settings_raw_iter_set_current_record_pos(&file->iter, file->cur_record_pos);
  settings_raw_iter_read_key(&file->iter, key);
}
static void prv_get_val(SettingsFile *file, void *val, size_t val_len) {
  PBL_ASSERTN(val_len <= file->iter.hdr.val_len);
  settings_raw_iter_set_current_record_pos(&file->iter, file->cur_record_pos);
  settings_raw_iter_read_val(&file->iter, val, val_len);
}
status_t settings_file_each(SettingsFile *file, SettingsFileEachCallback cb,
                            void *context) {
  // Cannot set keys while iterating
  PBL_ASSERTN(file->cur_record_pos == 0);
  SettingsRecordInfo info;
  for (settings_raw_iter_begin(&file->iter); !settings_raw_iter_end(&file->iter);
      settings_raw_iter_next(&file->iter)) {
    if (overwritten(&file->iter.hdr) || deleted_and_expired(&file->iter.hdr)) {
      continue;
    }
    info = (SettingsRecordInfo) {
      .last_modified = file->iter.hdr.last_modified,
      .get_key = prv_get_key,
      .key_len = file->iter.hdr.key_len,
      .get_val = prv_get_val,
      .val_len = file->iter.hdr.val_len,
      .dirty = !flag_is_set(&file->iter.hdr, SETTINGS_FLAG_SYNCED),
    };
    file->cur_record_pos = settings_raw_iter_get_current_record_pos(&file->iter);
    // if the callback returns false, stop iterating.
    if (!cb(file, &info, context)) {
      break;
    }
    settings_raw_iter_set_current_record_pos(&file->iter, file->cur_record_pos);
  }

  file->cur_record_pos = 0;

  return S_SUCCESS;
}

typedef struct {
  SettingsFileRewriteCallback cb;
  SettingsFile *new_file;
  void *user_context;
} RewriteCbContext;
static bool prv_rewrite_cb(SettingsFile *file, SettingsRecordInfo *info,
                           void *context) {
  RewriteCbContext *cb_ctx = (RewriteCbContext*)context;
  cb_ctx->cb(file, cb_ctx->new_file, info, cb_ctx->user_context);
  return true; // continue iterating
}
status_t settings_file_rewrite(SettingsFile *file,
                               SettingsFileRewriteCallback cb, void *context) {
  SettingsFile new_file;
  status_t status = prv_open(&new_file, file->name,
                             OP_FLAG_OVERWRITE | OP_FLAG_READ,
                             file->max_used_space);
  if (status < 0) {
    return status;
  }
  RewriteCbContext cb_ctx = (RewriteCbContext) {
    .cb = cb,
    .new_file = &new_file,
    .user_context = context,
  };
  settings_file_each(file, prv_rewrite_cb, &cb_ctx);
  settings_file_close(file);
  // We have to close and reopen the new_file so that it's temp flag is cleared.
  // Before the close succeeds, if we reboot, we will just end up reading the
  // old file. After the close succeeds, we will end up reading the new
  // (compacted) file.
  char *name = kernel_strdup(new_file.name);
  settings_file_close(&new_file);
  status = prv_open(file, name, OP_FLAG_READ | OP_FLAG_WRITE, file->max_used_space);
  kernel_free(name);

  return status;
}
