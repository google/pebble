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

#include "drivers/flash.h"
#include "flash_region/flash_region.h"
#include "services/common/legacy/registry_private.h"
#include "system/logging.h"
#include "system/passert.h"

#include <string.h>
#include <stdbool.h>

// Header patterns must precede any registry written to flash.
// They are used to indicate if a registry is active. Only one registry should be
// active at a time
static const uint8_t s_active_header[] = {0xff, 0x00, 0xff};
static const uint8_t s_inactive_header[] = {0x00, 0x00, 0x00};

////////////////////////////////////////////////////////////
// Functions for manipulating the cursor in flash
////////////////////////////////////////////////////////////
static int round_up_to_nearest_subsector(uint32_t addr) {
  // - 1 because if we're currently on a subsector border we don't want to round up to the
  // next one.
  return (addr + SUBSECTOR_SIZE_BYTES - 1) & SUBSECTOR_ADDR_MASK;
}

static bool is_cursor_at_active_registry(uint32_t cursor) {
  uint8_t header[REGISTRY_HEADER_SIZE_BYTES];
  flash_read_bytes(header, cursor, REGISTRY_HEADER_SIZE_BYTES);
  return memcmp(header, s_active_header, REGISTRY_HEADER_SIZE_BYTES) == 0;
}

static uint32_t get_next_cursor_position(uint32_t old_address, Registry* registry) {
  const uint32_t new_address =
      round_up_to_nearest_subsector(old_address + registry->total_buffer_size_bytes);

  const bool is_space_for_registry =
      new_address + registry->total_buffer_size_bytes < registry->cursor->end;

  if (is_space_for_registry) {
    return new_address;
  }

  return registry->cursor->begin;
}

static void move_cursor_to_active_registry(Registry* registry) {
  // Search for an active registry, starting at the start of the registry sector
  // in flash (`REGISTRY_FLASH_BEGIN`).
  // If cursor loops back to the start of the registry sector, then the
  // entire sector has been scanned and no registry was found. In that case, leave
  // the cursor in an invalid position (address 0xffff...)
  uint32_t temp_address = registry->cursor->begin;

  registry->cursor->address = FLASH_CURSOR_UNINITIALIZED;

  do {
    if (is_cursor_at_active_registry(temp_address)) {
      registry->cursor->address = temp_address;
      break;
    }
    temp_address = get_next_cursor_position(temp_address, registry);
  } while (temp_address != registry->cursor->begin);
}

////////////////////////////////////////////////////////////
// Registry data structure
////////////////////////////////////////////////////////////
static int registry_get_next_available_index(Registry* registry) {
  for (int i = 0; i < registry->num_records; i++) {
    Record* r = registry->records + i;
    if (r->active == false) {
      return i;
    }
  }
  return -1;
}

int registry_private_add(const char* key, const uint8_t key_length, const uint8_t* uuid,
    uint8_t description, const uint8_t* value, uint8_t value_length, Registry* registry) {
  if (value_length >= MAX_VALUE_SIZE_BYTES) {
    PBL_LOG(LOG_LEVEL_WARNING, "Length of record value exceeds maximum length.");
    return -1;
  }

  if (key_length > MAX_KEY_SIZE_BYTES) {
    PBL_LOG(LOG_LEVEL_WARNING, "Length of record key exceeds maximum length.");
    return -1;
  }

  Record* r = registry_private_get(key, key_length, uuid, registry);

  if (r) {
    if (r->value_length == value_length &&
        r->description == description &&
        memcmp(r->value, value, value_length) == 0) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Key & value already exist.");
      return 0;
    }
    PBL_LOG(LOG_LEVEL_DEBUG, "Key already exists. Updating record.");
    r->description = description;
    memcpy(r->value, value, value_length);
    r->value_length = value_length;
    registry->is_different_from_flash = true;
    return 0;
  }

  int idx = registry_get_next_available_index(registry);

  if (idx < 0) {
    PBL_LOG(LOG_LEVEL_WARNING, "Registry full.");
    return -1;
  }

  PBL_ASSERTN(idx >= 0 && idx < registry->num_records);

  r = registry->records + idx;

  r->active = true;
  strncpy(r->key, key, key_length);
  r->key[MAX_KEY_SIZE_BYTES - 1] = '\0';
  r->key_length = key_length;
  memcpy(r->uuid, uuid, UUID_SIZE_BYTES);
  r->description = description;
  memcpy(r->value, value, value_length);
  r->value_length = value_length;
  registry->is_different_from_flash = true;

  PBL_LOG(LOG_LEVEL_DEBUG, "Writing new key: %s", r->key);

  return 0;
}

void registry_record_print(Record* r) {
  PBL_LOG_VERBOSE("Active:\n\t");
  if (r->active) {
    PBL_LOG_VERBOSE("True");
  } else {
    PBL_LOG_VERBOSE("False");
  }
  PBL_LOG_VERBOSE("\n");

  PBL_LOG_VERBOSE("Key is:\n\t");
  for (int i = 0; i < r->key_length; i++) {
    PBL_LOG_VERBOSE("'%c'", r->key[i]);
  }
  PBL_LOG_VERBOSE("\n");

  PBL_LOG_VERBOSE("UUID is:\n\t");
  for (int i = 0; i < UUID_SIZE_BYTES; i++) {
    PBL_LOG_VERBOSE("%#.2hx, ",r->uuid[i]);
  }
  PBL_LOG_VERBOSE("\n");

  PBL_LOG_VERBOSE("Description is:\n\t%#.2hx\n", r->description);

  PBL_LOG_VERBOSE("Value is:\n\t");
  for (int i = 0; i < r->value_length; i++) {
    PBL_LOG_VERBOSE("%#.2hx, ",r->value[i]);
  }
  PBL_LOG_VERBOSE("\n");
}

static bool record_compare(Record* r, const char* key, const uint8_t key_length, const uint8_t* uuid) {
  if ((r->key_length == key_length) &&
      (memcmp(r->uuid, uuid, UUID_SIZE_BYTES) == 0) &&
      (memcmp(r->key, key, r->key_length) == 0)) {
    return true;
  }
  return false;
}

static int record_get_index(const char* key, const uint8_t key_length, const uint8_t* uuid, Registry* registry) {
  for (int i = 0; i < registry->num_records; i++) {
    Record* r = registry->records + i;

    if (r->active) {
      if (record_compare(r, key, key_length, uuid)) {
        return i;
      }
    }
  }
  return -1;
}

Record* registry_private_get(const char* key, const uint8_t key_length,
                             const uint8_t* uuid, Registry* registry) {
  const int idx = record_get_index(key, key_length, uuid, registry);

  PBL_ASSERTN(idx < registry->num_records);

  if (idx >= 0) {
    return registry->records + idx;
  }

  return NULL;
}

void registry_private_remove_all(const uint8_t* uuid, Registry* registry) {
  for (int i = 0; i < registry->num_records; i++) {
    Record* r = registry->records + i;

    if (r->active) {
      bool uuid_match = memcmp(r->uuid, uuid, UUID_SIZE_BYTES) == 0;
      if (uuid_match) {
        r->active = false;
        registry->is_different_from_flash = true;
      }
    }
  }
}

int registry_private_remove(const char* key, const uint8_t key_length, const uint8_t* uuid,
                            Registry* registry) {
  const int idx = record_get_index(key, key_length, uuid, registry);

  PBL_ASSERTN(idx < registry->num_records);

  if (idx >= 0) {
    Record* r = registry->records + idx;
    r->active = false;
    registry->is_different_from_flash = true;
    return 0;
  }

  return -1;
}

////////////////////////////////////////////////////////////
// Read and write from flash
////////////////////////////////////////////////////////////
static void registry_set_header(uint32_t cursor, bool active) {
  const uint8_t* header = s_inactive_header;
  if (active) {
    header = s_active_header;
  }
  flash_write_bytes(header, cursor, REGISTRY_HEADER_SIZE_BYTES);
}

//! Verifies the flash cursor is at the active registry (as initialized by `registry_init()`.
//! Cursor must be subsector aligned and within the bounds of REGISTRY_FLASH_BEGIN and
//! REGISTRY_FLASH_END.
static void prv_assert_valid_cursor(const RegistryCursor *cursor) {
  const bool is_addr_subsector_aligned = (cursor->address % (SUBSECTOR_SIZE_BYTES)) == 0;

  PBL_ASSERTN(is_addr_subsector_aligned &&
              cursor->address >= cursor->begin &&
              cursor->address < cursor->end);
}

void registry_private_read_from_flash(Registry* registry) {
  prv_assert_valid_cursor(registry->cursor);

  uint32_t registry_addr = registry->cursor->address + REGISTRY_HEADER_SIZE_BYTES;
  flash_read_bytes((uint8_t*)registry->records, registry_addr, registry->registry_size_bytes);
  registry->is_different_from_flash = false;
}

static void prv_write_next_registry(Registry* registry) {
  // Compute the addresses of the subsectors where the next registry will
  // be stored in flash; erase those subsectors
  const uint32_t next_start_address = get_next_cursor_position(registry->cursor->address, registry);
  const uint32_t next_end_address =
      round_up_to_nearest_subsector(next_start_address + registry->total_buffer_size_bytes);

  flash_region_erase_optimal_range(next_start_address, next_start_address,
                                   next_end_address, next_end_address);

  // Write the next header + content
  registry_set_header(next_start_address, true);

  const uint32_t data_start_address = next_start_address + REGISTRY_HEADER_SIZE_BYTES;
  flash_write_bytes((uint8_t*)registry->records, data_start_address,
                    registry->registry_size_bytes);

  registry->cursor->address = next_start_address;
}

void registry_private_write_to_flash(Registry* registry) {
  // If the flash cursor is 0 (if no registry currently exists in flash), then set
  // the flash cursor to REGISTRY_FLASH_BEGIN.
  //
  // Write a registry to flash by erasing the following subsectors, writing the
  // registry to those subsectors. Then, set the current registry to be
  // inactive and advance the flash cursor.

  // Cursor not initialized, no registry exists
  if (registry->cursor->address == (unsigned)FLASH_CURSOR_UNINITIALIZED) {
    registry->cursor->address = registry->cursor->begin;
  }
  PBL_LOG(LOG_LEVEL_DEBUG, "Writing registry to flash...");

  prv_assert_valid_cursor(registry->cursor);

  // Mark the previous registry as invalid
  registry_set_header(registry->cursor->address, false);

  // Erase the spot for the next registry and write to it
  prv_write_next_registry(registry);

  registry->is_different_from_flash = false;
}

void registry_private_init(Registry* registry) {
  // This fills the statically allocated registry with zeroes, tests that
  // the registry is large enough to fit in memory using PBL_ASSERT and initializes
  // the `flash cursor`---the flash address of the active registry's header.
  //
  // Registries are stored in flash with a preceeding three-byte header.  This
  // header is used to identify if the registry is active. The pattern 0xff,
  // 0x00, 0xff indicates a registry is active, 0x00, 0x00, 0x00 indicates a
  // registry is inactive. One registry should be active at any time. If no
  // active registry can be found when `registry_init()` is called, an empty registry
  // is written.
  //
  // The flash cursor starts at the beginning of the registry's SPIFlash
  // address (`REGISTRY_FLASH_BEGIN`), and is incremented to the next completely
  // empty subsector every time the registry is written to flash.
  memset(registry->records, 0, registry->registry_size_bytes);

  // Test that there is enough space in SPIFlash to write the registry
  int flash_space_available_bytes = registry->cursor->end - registry->cursor->begin;

  // Registry is too large to store in SPIFlash: allocate more space
  PBL_ASSERTN((signed)registry->registry_size_bytes < flash_space_available_bytes);

  move_cursor_to_active_registry(registry);

  // Write empty registry if one does not exist
  if (registry->cursor->address == (unsigned)FLASH_CURSOR_UNINITIALIZED) {
    registry_private_write_to_flash(registry);
  } else {
    registry_private_read_from_flash(registry);
  }

  prv_assert_valid_cursor(registry->cursor);
}
