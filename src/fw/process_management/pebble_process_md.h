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

#include "applib/platform.h"
#include "kernel/pebble_tasks.h"
#include "resource/resource.h"
#include "util/build_id.h"

#include "pebble_process_info.h"

#include "util/uuid.h"

#include <stdbool.h>

typedef void (*PebbleMain)(void);

typedef enum {
  ProcessVisibilityShown = 0,
  ProcessVisibilityHidden = 1,
  ProcessVisibilityShownOnCommunication = 2,
  ProcessVisibilityQuickLaunch = 3,
} ProcessVisibility;

typedef enum {
  ProcessTypeApp = 0,
  ProcessTypeWatchface = 1,
  ProcessTypeWorker = 2,
} ProcessType;

typedef enum ProcessAppRunLevel {
  ProcessAppRunLevelNormal = 0,
  ProcessAppRunLevelSystem = 1,
  ProcessAppRunLevelCritical = 2
} ProcessAppRunLevel;

typedef enum {
  ProcessStorageBuiltin = 0,
  ProcessStorageFlash = 1,
  ProcessStorageResource = 2
} ProcessStorage;

typedef enum {
  ProcessAppSDKType_System,
  ProcessAppSDKType_Legacy2x,
  ProcessAppSDKType_Legacy3x,
  ProcessAppSDKType_4x
} ProcessAppSDKType;

//! This structure is used internally to describe the process. This struct here is actually a polymorphic base
//! class, and can be casted to either \ref PebbleProcessMdSystem or \ref PebbleProcessMdFlash depending on the value
//! of \ref is_flash_based. Clients shouldn't do this casting themselves though, and instead should use the
//! process_metadata_get_* functions to safely retrieve values from this struct.
typedef struct PebbleProcessMd {
  Uuid uuid;

  //! The address of the main function of the process. This will be inside the firmware for firmware processes and
  //! will be inside the process's RAM region for 3rd party processes.
  PebbleMain main_func;

  //! The type of process
  ProcessType process_type;

  // Flags
  ProcessVisibility visibility;

  //! Where is the process stored?
  ProcessStorage process_storage;

  //! Can this process call kernel functionality directly or does it need to go through syscalls?
  bool is_unprivileged;

  //! Allow JavaScript applications to access this process
  bool allow_js;

  //! This process has a sister worker process in flash.
  bool has_worker;

  //! Process is allowed to call RockyJS APIs
  bool is_rocky_app;

  //! Bits of the sdk_platform as they were stored in the binary, or 0 if undefined
  uint16_t stored_sdk_platform;
} PebbleProcessMd;

//! App metadata for apps that are built into the firmware.
typedef struct PebbleProcessMdSystem {
  PebbleProcessMd common;

  const char* name;

  uint32_t icon_resource_id;

  //! The level at which the process runs. Any processes that try to start but they have a lower level than what's
  //! set using the \ref app_manager_set_minimum_run_level() function will not be launched.
  ProcessAppRunLevel run_level;
} PebbleProcessMdSystem;

//! Metadata for processes that are dynamically loaded from flash.
typedef struct PebbleProcessMdFlash {
  PebbleProcessMd common;

  char name[PROCESS_NAME_BYTES];

  //! Size in bytes of the app region that is occupied when this app is loaded
  //! Used when sizing the app heap. For first-party apps, this value will
  //! always be zero.
  uint16_t size_bytes;

  //! The version specified by the author for this process
  Version process_version;
  //! The version of the SDK this process was created with.
  Version sdk_version;

  //! The bank this process will get it's code and data from. This field is only valid if the
  //! \ref process_storage is ProcessStorageFlash
  uint32_t code_bank_num;

  //! The bank this app will get its resources from
  ResAppNum res_bank_num;

  //! A version we can use to verify the resources in the resource bank on the filesystem are valid
  ResourceVersion res_version;

  //! Build id of the application
  uint8_t build_id[BUILD_ID_EXPECTED_LEN];
} PebbleProcessMdFlash;


//! Metadata for processes that are dynamically loaded from a system resource.
typedef struct {
  PebbleProcessMd common;

  char name[PROCESS_NAME_BYTES];

  //! Size in bytes of the app region that is occupied when this app is loaded
  //! Used when sizing the app heap. 
  uint16_t size_bytes;

  //! The resource number of the app binary
  uint32_t bin_resource_id;

} PebbleProcessMdResource;


const char* process_metadata_get_name(const PebbleProcessMd *md);
uint32_t process_metadata_get_size_bytes(const PebbleProcessMd *md);
Version process_metadata_get_process_version(const PebbleProcessMd *md);
Version process_metadata_get_sdk_version(const PebbleProcessMd *md);
ProcessAppRunLevel process_metadata_get_run_level(const PebbleProcessMd *md);
int process_metadata_get_code_bank_num(const PebbleProcessMd *md);
int process_metadata_get_res_bank_num(const PebbleProcessMd *md);
ResourceVersion process_metadata_get_res_version(const PebbleProcessMd *md);
const uint8_t *process_metadata_get_build_id(const PebbleProcessMd *md);

//! @param[out] md
void process_metadata_init_with_flash_header(PebbleProcessMdFlash *md,
    const PebbleProcessInfo *flash_header, int process_bank_num, PebbleTask task,
    uint8_t *build_id_buffer);

//! @param[out] md
void process_metadata_init_with_resource_header(PebbleProcessMdResource *md,
    const PebbleProcessInfo *info, int bin_resource_id, PebbleTask task);

ProcessVisibility process_metadata_flags_visibility(PebbleProcessInfoFlags flags);

ProcessType process_metadata_flags_process_type(PebbleProcessInfoFlags flags, PebbleTask task);

bool process_metadata_flags_allow_js(PebbleProcessInfoFlags flags);

bool process_metadata_flags_has_worker(PebbleProcessInfoFlags flags);

bool process_metadata_flags_rocky_app(PebbleProcessInfoFlags flags);

uint16_t process_metadata_flags_stored_sdk_platform(PebbleProcessInfoFlags flags);

ProcessAppSDKType process_metadata_get_app_sdk_type(const PebbleProcessMd *md);

PlatformType process_metadata_get_app_sdk_platform(const PebbleProcessMd *md);
