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

#include "ios_notif_pref_db.h"

#include "sync.h"
#include "sync_util.h"

#include "console/prompt.h"
#include "kernel/pbl_malloc.h"
#include "os/mutex.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/settings/settings_file.h"
#include "services/normal/timeline/attributes_actions.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/units.h"

#include <stdio.h>

T_STATIC const char *iOS_NOTIF_PREF_DB_FILE_NAME = "iosnotifprefdb";
T_STATIC const int iOS_NOTIF_PREF_MAX_SIZE = KiBYTES(10);


typedef struct PACKED {
  uint32_t flags;
  uint8_t num_attributes;
  uint8_t num_actions;
  uint8_t data[]; // Serialized attributes followed by serialized actions
} SerializedNotifPrefs;

static PebbleMutex *s_mutex;

static status_t prv_file_open_and_lock(SettingsFile *file) {
  mutex_lock(s_mutex);

  status_t rv = settings_file_open(file, iOS_NOTIF_PREF_DB_FILE_NAME, iOS_NOTIF_PREF_MAX_SIZE);
  if (rv != S_SUCCESS) {
    mutex_unlock(s_mutex);
  }

  return rv;
}

static void prv_file_close_and_unlock(SettingsFile *file) {
  settings_file_close(file);
  mutex_unlock(s_mutex);
}

//! Assumes the file is opened and locked
static status_t prv_save_serialized_prefs(SettingsFile *file, const void *key, size_t key_len,
                                          const void *val, size_t val_len) {
  // Invert flags before writing to flash
  ((SerializedNotifPrefs *)val)->flags = ~(((SerializedNotifPrefs *)val)->flags);

  return settings_file_set(file, key, key_len, val, val_len);
}

//! Assumes the file is opened and locked
static status_t prv_read_serialized_prefs(SettingsFile *file, const void *key, size_t key_len,
                                          void *val_out, size_t val_out_len) {

  status_t rv = settings_file_get(file, key, key_len, val_out, val_out_len);

  // The flags for inverted before writing, revert them back
  ((SerializedNotifPrefs *)val_out)->flags = ~(((SerializedNotifPrefs *)val_out)->flags);

  return rv;
}

//! Returns the length of the data
//! When done with the prefs, call prv_free_serialized_prefs()
static int prv_get_serialized_prefs(SettingsFile *file, const uint8_t *app_id, int key_len,
                                    SerializedNotifPrefs **prefs_out) {
  const unsigned prefs_len = settings_file_get_len(file, app_id, key_len);
  if (prefs_len < sizeof(SerializedNotifPrefs)) {
    return 0;
  }

  *prefs_out = kernel_zalloc(prefs_len);
  if (!*prefs_out) {
    return 0;
  }

  status_t rv = prv_read_serialized_prefs(file, app_id, key_len, (void *) *prefs_out, prefs_len);
  if (rv != S_SUCCESS) {
    kernel_free(*prefs_out);
    return 0;
  }

  return (prefs_len - sizeof(SerializedNotifPrefs));
}

static void prv_free_serialized_prefs(SerializedNotifPrefs *prefs) {
  kernel_free(prefs);
}

iOSNotifPrefs* ios_notif_pref_db_get_prefs(const uint8_t *app_id, int key_len) {
  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return 0;
  }

  if (!settings_file_exists(&file, app_id, key_len)) {
    char buffer[key_len + 1];
    strncpy(buffer, (const char *)app_id, key_len);
    buffer[key_len] = '\0';
    PBL_LOG(LOG_LEVEL_DEBUG, "No prefs found for <%s>", buffer);
    prv_file_close_and_unlock(&file);
    return NULL;
  }

  SerializedNotifPrefs *serialized_prefs = NULL;
  const int serialized_prefs_data_len = prv_get_serialized_prefs(&file, app_id, key_len,
                                                                 &serialized_prefs);
  prv_file_close_and_unlock(&file);

  size_t string_alloc_size;
  uint8_t attributes_per_action[serialized_prefs->num_actions];
  bool r = attributes_actions_parse_serial_data(serialized_prefs->num_attributes,
                                                serialized_prefs->num_actions,
                                                serialized_prefs->data,
                                                serialized_prefs_data_len,
                                                &string_alloc_size,
                                                attributes_per_action);
  if (!r) {
    char buffer[key_len + 1];
    strncpy(buffer, (const char *)app_id, key_len);
    buffer[key_len] = '\0';
    PBL_LOG(LOG_LEVEL_ERROR, "Could not parse serial data for <%s>", buffer);
    prv_free_serialized_prefs(serialized_prefs);
    return NULL;
  }

  const size_t alloc_size =
      attributes_actions_get_required_buffer_size(serialized_prefs->num_attributes,
                                                  serialized_prefs->num_actions,
                                                  attributes_per_action,
                                                  string_alloc_size);

  iOSNotifPrefs *notif_prefs = kernel_zalloc_check(sizeof(iOSNotifPrefs) + alloc_size);

  uint8_t *buffer = (uint8_t *)notif_prefs + sizeof(iOSNotifPrefs);
  uint8_t *const buf_end = buffer + alloc_size;

  attributes_actions_init(&notif_prefs->attr_list, &notif_prefs->action_group,
                          &buffer, serialized_prefs->num_attributes, serialized_prefs->num_actions,
                          attributes_per_action);

  if (!attributes_actions_deserialize(&notif_prefs->attr_list,
                                      &notif_prefs->action_group,
                                      buffer,
                                      buf_end,
                                      serialized_prefs->data,
                                      serialized_prefs_data_len)) {
    char buffer[key_len + 1];
    strncpy(buffer, (const char *)app_id, key_len);
    buffer[key_len] = '\0';
    PBL_LOG(LOG_LEVEL_ERROR, "Could not deserialize data for <%s>", buffer);
    prv_free_serialized_prefs(serialized_prefs);
    kernel_free(notif_prefs);
    return NULL;
  }

  prv_free_serialized_prefs(serialized_prefs);
  return notif_prefs;
}

void ios_notif_pref_db_free_prefs(iOSNotifPrefs *prefs) {
  kernel_free(prefs);
}

status_t ios_notif_pref_db_store_prefs(const uint8_t *app_id, int length, AttributeList *attr_list,
                                       TimelineItemActionGroup *action_group) {
  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  size_t payload_size = attributes_actions_get_serialized_payload_size(attr_list, action_group);
  size_t serialized_prefs_size = sizeof(SerializedNotifPrefs) + payload_size;
  SerializedNotifPrefs *new_prefs = kernel_zalloc_check(serialized_prefs_size);
  *new_prefs = (SerializedNotifPrefs) {
    .num_attributes = attr_list ? attr_list->num_attributes : 0,
    .num_actions = action_group ? action_group->num_actions : 0,
  };
  attributes_actions_serialize_payload(attr_list, action_group, new_prefs->data, payload_size);

  // Add the new entry to the DB
  rv = prv_save_serialized_prefs(&file, app_id, length, new_prefs, serialized_prefs_size);
  prv_file_close_and_unlock(&file);

  if (rv == S_SUCCESS) {
    char buffer[length + 1];
    strncpy(buffer, (const char *)app_id, length);
    buffer[length] = '\0';
    PBL_LOG(LOG_LEVEL_INFO, "Added <%s> to the notif pref db", buffer);

    blob_db_sync_record(BlobDBIdiOSNotifPref, app_id, length, rtc_get_time());
  }

  return rv;
}

void ios_notif_pref_db_init(void) {
  s_mutex = mutex_create();
  PBL_ASSERTN(s_mutex != NULL);
}

status_t ios_notif_pref_db_insert(const uint8_t *key, int key_len,
                                  const uint8_t *val, int val_len) {
  if (key_len == 0 || val_len == 0 || val_len < (int) sizeof(SerializedNotifPrefs)) {
    return E_INVALID_ARGUMENT;
  }

  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  rv = prv_save_serialized_prefs(&file, key, key_len, val, val_len);
  if (rv == S_SUCCESS) {
    char buffer[key_len + 1];
    strncpy(buffer, (const char *)key, key_len);
    buffer[key_len] = '\0';
    PBL_LOG(LOG_LEVEL_INFO, "iOS notif pref insert <%s>", buffer);

    // All records inserted from the phone are not dirty (the phone is the source of truth)
    rv = settings_file_mark_synced(&file, key, key_len);
  }

  prv_file_close_and_unlock(&file);

  return rv;
}

int ios_notif_pref_db_get_len(const uint8_t *key, int key_len) {
  if (key_len == 0) {
    return 0;
  }

  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return 0;
  }

  int length = settings_file_get_len(&file, key, key_len);

  prv_file_close_and_unlock(&file);

  return length;
}

status_t ios_notif_pref_db_read(const uint8_t *key, int key_len,
                                uint8_t *val_out, int val_out_len) {
  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  rv = prv_read_serialized_prefs(&file, key, key_len, val_out, val_out_len);

  prv_file_close_and_unlock(&file);

  return rv;
}

status_t ios_notif_pref_db_delete(const uint8_t *key, int key_len) {
  if (key_len == 0) {
    return E_INVALID_ARGUMENT;
  }

  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  rv = settings_file_delete(&file, key, key_len);

  prv_file_close_and_unlock(&file);

  return rv;
}

status_t ios_notif_pref_db_flush(void) {
  mutex_lock(s_mutex);
  status_t rv = pfs_remove(iOS_NOTIF_PREF_DB_FILE_NAME);
  mutex_unlock(s_mutex);
  return rv;
}

status_t ios_notif_pref_db_is_dirty(bool *is_dirty_out) {
  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  *is_dirty_out = false;
  rv = settings_file_each(&file, sync_util_is_dirty_cb, is_dirty_out);

  prv_file_close_and_unlock(&file);

  return rv;
}

BlobDBDirtyItem* ios_notif_pref_db_get_dirty_list(void) {
  SettingsFile file;
  if (S_SUCCESS != prv_file_open_and_lock(&file)) {
    return NULL;
  }

  BlobDBDirtyItem *dirty_list = NULL;
  settings_file_each(&file, sync_util_build_dirty_list_cb, &dirty_list);

  prv_file_close_and_unlock(&file);

  return dirty_list;
}

status_t ios_notif_pref_db_mark_synced(const uint8_t *key, int key_len) {
  if (key_len == 0) {
    return E_INVALID_ARGUMENT;
  }

  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return rv;
  }

  rv = settings_file_mark_synced(&file, key, key_len);

  prv_file_close_and_unlock(&file);

  return rv;
}

// ----------------------------------------------------------------------------------------------
#if UNITTEST
uint32_t ios_notif_pref_db_get_flags(const uint8_t *app_id, int key_len) {
  SettingsFile file;
  status_t rv = prv_file_open_and_lock(&file);
  if (rv != S_SUCCESS) {
    return 0;
  }

  SerializedNotifPrefs *prefs = NULL;
  prv_get_serialized_prefs(&file, app_id, key_len, &prefs);
  uint32_t flags = prefs->flags;
  prv_free_serialized_prefs(prefs);
  prv_file_close_and_unlock(&file);
  return flags;
}
#endif

// ----------------------------------------------------------------------------------------------
static bool prv_print_notif_pref_db(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  char app_id[64];
  info->get_key(file, app_id, info->key_len);
  app_id[info->key_len] = '\0';
  prompt_send_response(app_id);

  char buffer[64];
  prompt_send_response_fmt(buffer, sizeof(buffer), "Dirty: %s", info->dirty ? "Yes" : "No");
  prompt_send_response_fmt(buffer, sizeof(buffer), "Last modified: %"PRIu32"", info->last_modified);

  SerializedNotifPrefs *serialized_prefs = NULL;
  prv_get_serialized_prefs(file, (uint8_t *)app_id, info->key_len, &serialized_prefs);
  prompt_send_response_fmt(buffer, sizeof(buffer), "Attributes: %d,  Actions: %d",
      serialized_prefs->num_attributes, serialized_prefs->num_actions);

  // TODO: Print the attributes and actions

  prv_free_serialized_prefs(serialized_prefs);
  prompt_send_response("");
  return true;
}

void command_dump_notif_pref_db(void) {
  SettingsFile file;
  if (S_SUCCESS != prv_file_open_and_lock(&file)) {
    return;
  }

  settings_file_each(&file, prv_print_notif_pref_db, NULL);

  prv_file_close_and_unlock(&file);
}
