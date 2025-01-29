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

#include "api.h"

#include <stddef.h>
#include <stdbool.h>

#include "app_db.h"
#include "app_glance_db.h"
#include "contacts_db.h"
#include "health_db.h"
#include "ios_notif_pref_db.h"
#include "notif_db.h"
#include "pin_db.h"
#include "prefs_db.h"
#include "reminder_db.h"
#include "watch_app_prefs_db.h"
#include "weather_db.h"

#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "system/logging.h"

typedef struct {
  BlobDBInitImpl init;
  BlobDBInsertImpl insert;
  BlobDBGetLenImpl get_len;
  BlobDBReadImpl read;
  BlobDBDeleteImpl del;
  BlobDBFlushImpl flush;
  BlobDBIsDirtyImpl is_dirty;
  BlobDBGetDirtyListImpl get_dirty_list;
  BlobDBMarkSyncedImpl mark_synced;
  bool disabled;
} BlobDB;

static const BlobDB s_blob_dbs[NumBlobDBs] = {
  [BlobDBIdPins] = {
    .init = pin_db_init,
    .insert = pin_db_insert,
    .get_len = pin_db_get_len,
    .read = pin_db_read,
    .del = pin_db_delete,
    .flush = pin_db_flush,
    .is_dirty = pin_db_is_dirty,
    .get_dirty_list = pin_db_get_dirty_list,
    .mark_synced = pin_db_mark_synced,
  },
  [BlobDBIdApps] = {
    .init = app_db_init,
    .insert = app_db_insert,
    .get_len = app_db_get_len,
    .read = app_db_read,
    .del = app_db_delete,
    .flush = app_db_flush,
  },
  [BlobDBIdReminders] = {
    .init = reminder_db_init,
    .insert = reminder_db_insert,
    .get_len = reminder_db_get_len,
    .read = reminder_db_read,
    .del = reminder_db_delete,
    .flush = reminder_db_flush,
    .is_dirty = reminder_db_is_dirty,
    .get_dirty_list = reminder_db_get_dirty_list,
    .mark_synced = reminder_db_mark_synced,
  },
  [BlobDBIdNotifs] = {
    .init = notif_db_init,
    .insert = notif_db_insert,
    .get_len = notif_db_get_len,
    .read = notif_db_read,
    .del = notif_db_delete,
    .flush = notif_db_flush,
  },
  [BlobDBIdWeather] = {
#if CAPABILITY_HAS_WEATHER
    .init = weather_db_init,
    .insert = weather_db_insert,
    .get_len = weather_db_get_len,
    .read = weather_db_read,
    .del = weather_db_delete,
    .flush = weather_db_flush,
#else
    .disabled = true,
#endif
  },
  [BlobDBIdiOSNotifPref] = {
    .init = ios_notif_pref_db_init,
    .insert = ios_notif_pref_db_insert,
    .get_len = ios_notif_pref_db_get_len,
    .read = ios_notif_pref_db_read,
    .del = ios_notif_pref_db_delete,
    .flush = ios_notif_pref_db_flush,
    .is_dirty = ios_notif_pref_db_is_dirty,
    .get_dirty_list = ios_notif_pref_db_get_dirty_list,
    .mark_synced = ios_notif_pref_db_mark_synced,
  },
  [BlobDBIdPrefs] = {
    .init = prefs_db_init,
    .insert = prefs_db_insert,
    .get_len = prefs_db_get_len,
    .read = prefs_db_read,
    .del = prefs_db_delete,
    .flush = prefs_db_flush,
  },
  [BlobDBIdContacts] = {
#if !PLATFORM_TINTIN
    .init = contacts_db_init,
    .insert = contacts_db_insert,
    .get_len = contacts_db_get_len,
    .read = contacts_db_read,
    .del = contacts_db_delete,
    .flush = contacts_db_flush,
#else
    // Disabled on tintin for code savings
    .disabled = true,
#endif
  },
  [BlobDBIdWatchAppPrefs] = {
#if !PLATFORM_TINTIN
    .init = watch_app_prefs_db_init,
    .insert = watch_app_prefs_db_insert,
    .get_len = watch_app_prefs_db_get_len,
    .read = watch_app_prefs_db_read,
    .del = watch_app_prefs_db_delete,
    .flush = watch_app_prefs_db_flush,
#else
    // Disabled on tintin for code savings
    .disabled = true,
#endif
  },
  [BlobDBIdHealth] = {
#if CAPABILITY_HAS_HEALTH_TRACKING
    .init = health_db_init,
    .insert = health_db_insert,
    .get_len = health_db_get_len,
    .read = health_db_read,
    .del = health_db_delete,
    .flush = health_db_flush,
#else
    .disabled = true,
#endif
  },
  [BlobDBIdAppGlance] = {
#if CAPABILITY_HAS_APP_GLANCES
    .init = app_glance_db_init,
    .insert = app_glance_db_insert,
    .get_len = app_glance_db_get_len,
    .read = app_glance_db_read,
    .del = app_glance_db_delete,
    .flush = app_glance_db_flush,
#else
    .disabled = true,
#endif
  },
};

static bool prv_db_valid(BlobDBId db_id) {
  return (db_id < NumBlobDBs) && (!s_blob_dbs[db_id].disabled);

}

void blob_db_event_put(BlobDBEventType type, BlobDBId db_id, const uint8_t *key, int key_len) {
  // copy key for event
  uint8_t *key_bytes = NULL;
  if (key_len > 0) {
    key_bytes = kernel_malloc(key_len);
    memcpy(key_bytes, key, key_len);
  }

  PebbleEvent e = {
    .type = PEBBLE_BLOBDB_EVENT,
    .blob_db = {
      .db_id = db_id,
      .type = type,
      .key = key_bytes,
      .key_len = (uint8_t)key_len,
    }
  };
  event_put(&e);
}

void blob_db_init_dbs(void) {
  const BlobDB *db = s_blob_dbs;
  for (int i = 0; i < NumBlobDBs; ++i, ++db) {
    if (db->init) {
      db->init();
    }
  }
}

void blob_db_get_dirty_dbs(uint8_t *ids, uint8_t *num_ids) {
  const BlobDB *db = s_blob_dbs;
  *num_ids = 0;
  for (uint8_t i = 0; i < NumBlobDBs; ++i, ++db) {
    bool is_dirty = false;
    if (db->is_dirty && (db->is_dirty(&is_dirty) == S_SUCCESS) && is_dirty) {
      ids[*num_ids] = i;
      *num_ids += 1;
    }
  }
}

status_t blob_db_insert(BlobDBId db_id,
    const uint8_t *key, int key_len, const uint8_t *val, int val_len) {
  if (!prv_db_valid(db_id)) {
    return E_RANGE;
  }

  const BlobDB *db = &s_blob_dbs[db_id];
  if (db->insert) {
    status_t rv = db->insert(key, key_len, val, val_len);
    if (rv == S_SUCCESS) {
      blob_db_event_put(BlobDBEventTypeInsert, db_id, key, key_len);
    }

    return rv;
  }

  return E_INVALID_OPERATION;
}

int blob_db_get_len(BlobDBId db_id,
    const uint8_t *key, int key_len) {
  if (!prv_db_valid(db_id)) {
    return E_RANGE;
  }

  const BlobDB *db = &s_blob_dbs[db_id];
  if (db->get_len) {
    return db->get_len(key, key_len);
  }

  return E_INVALID_OPERATION;
}

status_t blob_db_read(BlobDBId db_id,
    const uint8_t *key, int key_len, uint8_t *val_out, int val_len) {
  if (!prv_db_valid(db_id)) {
    return E_RANGE;
  }

  const BlobDB *db = &s_blob_dbs[db_id];
  if (db->read) {
    return db->read(key, key_len, val_out, val_len);
  }

  return E_INVALID_OPERATION;
}

status_t blob_db_delete(BlobDBId db_id,
    const uint8_t *key, int key_len) {
  if (!prv_db_valid(db_id)) {
    return E_RANGE;
  }

  const BlobDB *db = &s_blob_dbs[db_id];
  if (db->del) {
    status_t rv = db->del(key, key_len);
    if (rv == S_SUCCESS) {
      blob_db_event_put(BlobDBEventTypeDelete, db_id, key, key_len);
    }
    return rv;
  }

  return E_INVALID_OPERATION;
}

status_t blob_db_flush(BlobDBId db_id) {
  if (!prv_db_valid(db_id)) {
    return E_RANGE;
  }

  const BlobDB *db = &s_blob_dbs[db_id];
  if (db->flush) {
    status_t rv = db->flush();
    if (rv == S_SUCCESS) {
      PBL_LOG(LOG_LEVEL_INFO, "Flushing BlobDB with ID %d", db_id);
      blob_db_event_put(BlobDBEventTypeFlush, db_id, NULL, 0);
    }
    return rv;
  }

  return E_INVALID_OPERATION;
}

BlobDBDirtyItem *blob_db_get_dirty_list(BlobDBId db_id) {
  if (!prv_db_valid(db_id)) {
    return NULL;
  }

  const BlobDB *db = &s_blob_dbs[db_id];
  if (db->get_dirty_list) {
    return db->get_dirty_list();
  }

  return NULL;
}

status_t blob_db_mark_synced(BlobDBId db_id, uint8_t *key, int key_len) {
  if (!prv_db_valid(db_id)) {
    return E_RANGE;
  }

  const BlobDB *db = &s_blob_dbs[db_id];
  if (db->mark_synced) {
    status_t rv = db->mark_synced(key, key_len);
    // TODO event?
    return rv;
  }

  return E_INVALID_OPERATION;
}
