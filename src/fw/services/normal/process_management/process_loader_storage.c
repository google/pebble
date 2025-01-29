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

#include "process_management/process_loader.h"

#include "drivers/flash.h"
#include "kernel/util/segment.h"
#include "process_management/pebble_process_md.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/process_management/app_storage.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/legacy_checksum.h"

#include <string.h>

//! This comes from the generated pebble.auto.c with all the exported functions in it.
extern const void* const g_pbl_system_tbl[];

// ----------------------------------------------------------------------------------------------
static bool prv_verify_checksum(const PebbleProcessInfo* app_info, const uint8_t* data) {
  const uint8_t header_size = sizeof(PebbleProcessInfo);

  const uint8_t *crc_data = data + header_size;
  const uint32_t app_size = app_info->load_size - header_size;
  uint32_t calculated_crc = legacy_defective_checksum_memory(crc_data,
                                                             app_size);

  if (app_info->crc != calculated_crc) {
    PBL_LOG(LOG_LEVEL_WARNING, "Calculated App CRC is 0x%"PRIx32", expected 0x%"PRIx32"!",
            calculated_crc, app_info->crc);
    return false;
  } else {
    return true;
  }
}

static void * prv_offset_to_address(MemorySegment *segment, size_t offset) {
  return (char *)segment->start + offset;
}

// ---------------------------------------------------------------------------------------------
static bool prv_initialize_sdk_process(PebbleTask task, const PebbleProcessInfo *info,
                                      MemorySegment *destination) {
  if (!prv_verify_checksum(info, destination->start)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Calculated CRC does not match, aborting...");
    return false;
  }

  // Poke in the address of the OS's API jump table to an address known by the shims
  uint32_t *pbl_jump_table_addr = prv_offset_to_address(destination, info->sym_table_addr);
  *pbl_jump_table_addr = (uint32_t)&g_pbl_system_tbl;

  //
  // offset any relative addresses, as indicated by the reloc table
  // TODO PBL-1627: insert link to the wiki page I'm about to write about PIC and relocatable
  //                values
  //

  // an array of app-relative pointers to addresses needing an offset
  uint32_t *reloc_array = prv_offset_to_address(destination, info->load_size);

  for (uint32_t i = 0; i < info->num_reloc_entries; ++i) {
    // an absolute pointer to an app-relative pointer which needs to be offset
    uintptr_t *addr_to_change = prv_offset_to_address(destination, reloc_array[i]);
    *addr_to_change = (uintptr_t) prv_offset_to_address(destination, *addr_to_change);
  }

  // Now fix up the part of RAM where the relocation table overwrote .bss. We don't need the table
  // anymore so restore the zero values.
  memset(reloc_array, 0, info->num_reloc_entries * 4);
  return true;
}

// ----------------------------------------------------------------------------------------------
static bool prv_load_from_flash(const PebbleProcessMd *app_md, PebbleTask task,
                                MemorySegment *destination) {
  PebbleProcessInfo info;
  AppStorageGetAppInfoResult result;
  AppInstallId app_id = process_metadata_get_code_bank_num(app_md);

  result = app_storage_get_process_info(&info, NULL, app_id, task);

  if (result != GET_APP_INFO_SUCCESS) {
    // Failed to load the app out of flash, this function will have already printed an error.
    return false;
  }

  // We load the full binary (.text + .data) into ram as well as the relocation entries. These
  // relocation entries will overlap with the .bss section of the loaded app, but we'll fix that
  // up later.
  const size_t load_size = app_storage_get_process_load_size(&info);

  if (load_size > memory_segment_get_size(destination)) {
    PBL_LOG(LOG_LEVEL_ERROR,
            "App/Worker exceeds available program space: %"PRIu16" + (%"PRIu32" * 4) = %zu",
            info.load_size, info.num_reloc_entries, load_size);
    return false;
  }

  // load the process from the pfs file appX or workerX
  char process_name[APP_FILENAME_MAX_LENGTH];
  int fd;
  app_storage_get_file_name(process_name, sizeof(process_name), app_id, task);

  if ((fd = pfs_open(process_name, OP_FLAG_READ, 0, 0)) < S_SUCCESS) {
    PBL_LOG(LOG_LEVEL_ERROR, "Process open failed for process %s, fd = %d", process_name, fd);
    return (false);
  }

  if (pfs_read(fd, destination->start, load_size) != (int)load_size) {
    PBL_LOG(LOG_LEVEL_ERROR, "Process read failed for process %s, fd = %d", process_name, fd);
    pfs_close(fd);
    return (false);
  }
  pfs_close(fd);

  return prv_initialize_sdk_process(task, &info, destination);
}

// ----------------------------------------------------------------------------------------------
static bool prv_load_from_resource(const PebbleProcessMdResource *app_md,
                                   PebbleTask task,
                                   MemorySegment *destination) {
  PebbleProcessInfo info;
  PBL_ASSERTN(resource_load_byte_range_system(SYSTEM_APP, app_md->bin_resource_id, 0,
        (uint8_t *)&info, sizeof(info)) == sizeof(info));

  // We load the full binary (.text + .data) into ram as well as the relocation entries. These
  // relocation entries will overlap with the .bss section of the loaded app, but we'll fix that
  // up later.
  const size_t load_size = app_storage_get_process_load_size(&info);

  if (load_size > memory_segment_get_size(destination)) {
    PBL_LOG(LOG_LEVEL_ERROR,
        "App/Worker exceeds available program space: %"PRIu16" + (%"PRIu32" * 4) = %zu",
        info.load_size, info.num_reloc_entries, load_size);
    return false;
  }

  // load the process from the resource
  PBL_ASSERTN(resource_load_byte_range_system(SYSTEM_APP, app_md->bin_resource_id, 0,
        destination->start, load_size) == load_size);

  // Process the relocation entries
  return prv_initialize_sdk_process(task, &info, destination);
}

void * process_loader_load(const PebbleProcessMd *app_md, PebbleTask task,
                           MemorySegment *destination) {

  if (app_md->process_storage == ProcessStorageFlash) {
    if (!prv_load_from_flash(app_md, task, destination)) {
      return NULL;
    }
  } else if (app_md->process_storage == ProcessStorageResource) {
    PebbleProcessMdResource *res_app_md = (PebbleProcessMdResource *)app_md;
    if (!prv_load_from_resource(res_app_md, task, destination)) {
      return NULL;
    }
  }

  // The final process image size may be smaller than the amount of
  // memory required to load it, (the relocation table needs to be
  // loaded into memory during load but is not needed after) so the
  // memory segment is split only after loading completes.
  size_t loaded_size = process_metadata_get_size_bytes(app_md);
  if (loaded_size) {
    void *main_func = prv_offset_to_address(
        destination, (uintptr_t)app_md->main_func);
    if (!memory_segment_split(destination, NULL, loaded_size)) {
      return NULL;
    }
    // Set the THUMB bit on the function pointer.
    return (void *)((uintptr_t)main_func | 1);
  } else {
    // No loaded size; must be builtin. The entry point address is
    // already a physical address.
    return app_md->main_func;
  }
}
