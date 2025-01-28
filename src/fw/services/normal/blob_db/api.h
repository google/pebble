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

#include "api_types.h"

#include <stdint.h>
#include <stdbool.h>

#include "system/status_codes.h"
#include "util/attributes.h"
#include "util/list.h"
#include "util/time/time.h"

//! The BlobDB API is a single consistent API to a number of key/value stores on the watch.
//! It is used in conjunction with the BlobDB endpoint.
//! Key/Value stores that are meant to be used with the endpoint need to implement this API
//! by implementing each of the Impl functions (see below).
//! A BlobDB is not guaranteed to persist across reboots, but it is guaranteed to
//! have executed a command when it returns a success code.
//! If you want to route commands to your BlobDB implementation API, you need
//! to add it to the \ref BlobDBId enum and to the BlobDBs list (\ref s_blob_dbs) in api.c

typedef enum PACKED {
  BlobDBIdTest = 0x00,
  BlobDBIdPins = 0x01,
  BlobDBIdApps = 0x02,
  BlobDBIdReminders = 0x03,
  BlobDBIdNotifs = 0x04,
  BlobDBIdWeather = 0x05,
  BlobDBIdiOSNotifPref = 0x06,
  BlobDBIdPrefs = 0x07,
  BlobDBIdContacts = 0x08,
  BlobDBIdWatchAppPrefs = 0x09,
  BlobDBIdHealth = 0x0A,
  BlobDBIdAppGlance = 0x0B,
  NumBlobDBs,
} BlobDBId;
_Static_assert(sizeof(BlobDBId) == 1, "BlobDBId is larger than 1 byte");

//! A linked list of blob DB items that need to be synced
typedef struct {
  ListNode node;
  time_t last_updated;
  int key_len; //!< length of the key, in bytes
  uint8_t key[]; //!< key_len-size byte array of key data
} BlobDBDirtyItem;

//! A Blob DB's initialization routine.
//! This function will be called at boot when all blob dbs are init-ed
typedef void (*BlobDBInitImpl)(void);

//! Implements the insert API. Note that this function should be blocking.
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
//! \param val a pointer to the value data
//! \param val_len the length of the value, in bytes
//! \returns S_SUCCESS if the key/val pair was successfully inserted
//! and an error code otherwise (See \ref StatusCode)
typedef status_t (*BlobDBInsertImpl)
    (const uint8_t *key, int key_len, const uint8_t *val, int val_len);

//! Implements the get length API.
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
//! \returns the length in bytes of the value for key on success
//! and an error code otherwise (See \ref StatusCode)
typedef int (*BlobDBGetLenImpl)
    (const uint8_t *key, int key_len);

//! Implements the read API. Note that this function should be blocking.
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
//! \param[out] val_out a pointer to a buffer of size val_len
//! \param val_len the length of the value to be copied, in bytes
//! \returns S_SUCCESS if the value for key was successfully read,
//! and an error code otherwise (See \ref StatusCode)
typedef status_t (*BlobDBReadImpl)
    (const uint8_t *key, int key_len, uint8_t *val_out, int val_len);

//! Implements the delete API. Note that this function should be blocking.
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
//! \returns S_SUCCESS if the key/val pair was successfully deleted
//! and an error code otherwise (See \ref StatusCode)
typedef status_t (*BlobDBDeleteImpl)
    (const uint8_t *key, int key_len);

//! Implements the flush API. Note that this function should be blocking.
//! \returns S_SUCCESS if all key/val pairs were successfully deleted
//! and an error code otherwise (See \ref StatusCode)
typedef status_t (*BlobDBFlushImpl)(void);

//! Implements the IsDirty API.
//! \param[out] is_dirty_out reference to a boolean that will be set depending on the DB state
//! \note if the function does not return S_SUCCESS, the state of the boolean is undefined.
//! \returns S_SUCCESS if the query succeeded, an error code otherwise
typedef status_t (*BlobDBIsDirtyImpl)(bool *is_dirty_out);

//! Implements the GetDirtyList API.
//! \return a linked list of \ref BlobDBDirtyItem with a node per out-of-sync item.
//! \note There is no limit how large this list can get. Handle OOM scenarios gracefully!
typedef BlobDBDirtyItem *(*BlobDBGetDirtyListImpl)(void);

//! Implements the MarkSynced API.
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
//! \returns S_SUCCESS if the item was marked synced, an error code otherwise
typedef status_t (*BlobDBMarkSyncedImpl)(const uint8_t *key, int key_len);

//! Emits a Blob DB event.
//! \param type The type of event to emit
//! \param db_id the ID of the blob DB
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
void blob_db_event_put(BlobDBEventType type, BlobDBId db_id, const uint8_t *key, int key_len);

//! Call the BlobDBInitImpl for all the databases
void blob_db_init_dbs(void);

//! Call the BlobDBIsDirtyImpl for each database, and fill the 'ids' list
//! with all the dirty DB ids
//! \param[out] ids an array of BlobDbIds of size NumBlobDBs or more.
//! \param[out] num_ids an array of BlobDbIds of size NumBlobDBs or more.
//! \note The unused entries will be set to 0.
void blob_db_get_dirty_dbs(uint8_t *ids, uint8_t *num_ids);

//! Insert a key/val pair in a blob DB.
//! See \ref BlobDBReadImpl
//! \param db_id the ID of the blob DB
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
status_t blob_db_insert(BlobDBId db_id,
    const uint8_t *key, int key_len, const uint8_t *val, int val_len);

//! Get the length of the value in a blob DB for a given key.
//! See \ref BlobDBGetLenImpl
//! \param db_id the ID of the blob DB
//! \param key a pointer to the key data
//! \param key_len the length of the key, in bytes
int blob_db_get_len(BlobDBId db_id,
    const uint8_t *key, int key_len);

//! Get the value of length val_len for a given key
//! \param db_id the ID of the blob DB
//! See \ref BlobDBReadImpl
status_t blob_db_read(BlobDBId db_id,
    const uint8_t *key, int key_len, uint8_t *val_out, int val_len);

//! Delete the key/val pair in a blob DB for a given key
//! \param db_id the ID of the blob DB
//! See \ref BlobDBDeleteImpl
status_t blob_db_delete(BlobDBId db_id,
    const uint8_t *key, int key_len);

//! Delete all key/val pairs in a blob DB.
//! \param db_id the ID of the blob DB
//! See \ref BlobDBFlushImpl
status_t blob_db_flush(BlobDBId db_id);

//! Get the list of items in a given blob DB that have yet to be synced.
//! Items originating from the phone are always marked as synced.
//! \note Use the APIs in sync.h to initiate a sync.
//! \param db_id the ID of the blob DB
//! \see BlobDBGetDirtyListImpl
BlobDBDirtyItem *blob_db_get_dirty_list(BlobDBId db_id);

//! Mark an item in a blob DB as having been synced
//! \note This API is used upon receiving an ACK from the phone during sync
//! \param db_id the ID of the blob DB
//! \see BlobDBMarkSyncedImpl
status_t blob_db_mark_synced(BlobDBId db_id, uint8_t *key, int key_len);
