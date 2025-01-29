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

#include "applib/graphics/gtypes.h"
#include "applib/graphics/gdraw_command_image.h"
#include "util/uuid.h"

#if !RECOVERY_FW
#include "resource/timeline_resource_ids.auto.h"
#else
typedef uint32_t TimelineResourceId;
#endif

#define SYSTEM_RESOURCE_FLAG 0x80000000

//! Earliest SDK version which supports timeline icons in PBWs (see pebble_process_info.h)
#define TIMELINE_RESOURCE_PBW_SUPPORT_FIRST_SDK_VERSION_MAJOR (0x5)
#define TIMELINE_RESOURCE_PBW_SUPPORT_FIRST_SDK_VERSION_MINOR (0x3d)

typedef struct {
  TimelineResourceId res_id;
  const Uuid *app_id;
  TimelineResourceId fallback_id;
} TimelineResourceInfo;

typedef struct {
  uint32_t res_id;
  ResAppNum res_app_num;
} AppResourceInfo;

typedef enum {
  TimelineResourceSizeTiny, // 25x25
  TimelineResourceSizeSmall, // 50x50
  TimelineResourceSizeLarge, // 80x80
  TimelineResourceSizeCount
} TimelineResourceSize;

typedef struct {
  ResAppNum res_app_num;
  uint32_t resource_id;
} AppResourceId;

typedef struct {
  uint32_t tiny;
  uint32_t small;
  uint32_t large;
} TimelineLutEntry;

#define TLUT_SIGNATURE MAKE_WORD('T', 'L', 'U', 'T')
#define TLUT_DATA_OFFSET sizeof(TLUT_SIGNATURE)
#define TLUT_RESOURCE_ID 1

#define TIMELINE_TINY_RESOURCE_SIZE (GSize(25, 25))
#define TIMELINE_SMALL_RESOURCE_SIZE (GSize(50, 50))
#define TIMELINE_LARGE_RESOURCE_SIZE (GSize(80, 80))

static inline GSize timeline_resources_get_gsize(TimelineResourceSize size) {
  switch (size) {
    case TimelineResourceSizeTiny: return TIMELINE_TINY_RESOURCE_SIZE;
    case TimelineResourceSizeSmall: return TIMELINE_SMALL_RESOURCE_SIZE;
    case TimelineResourceSizeLarge: return TIMELINE_LARGE_RESOURCE_SIZE;
    default: return GSizeZero;
  }
}

//! Tests if a given timeline resource ID is a system resource
//! @param timeline_id ID of the timeline resource to test
//! @return `true` if system resource, `false` if not
bool timeline_resources_is_system(TimelineResourceId timeline_id);

//! @internal
//! Get the resource info for a given timeline resource and size.
//! @note: This function does NOT check if the app represented by res_app_num was compiled with
//! an SDK version that supports published/timeline resources so if you can't assert that yourself
//! then you must use `timeline_resources_get_id` instead.
//! @param timeline_id Published/timeline resource ID to lookup
//! @param size The requested size of the published/timeline icon resource
//! @param res_app_num App bank to use if the provided timeline_id doesn't belong to the system
//! @param res_info_out Optional \ref AppResourceInfo pointer that will be set to the resulting
//! resource info if the function returns true
//! @return True if the timeline_id is valid for the provided size and res_app_num, false otherwise
bool timeline_resources_get_id_system(TimelineResourceId timeline_id, TimelineResourceSize size,
                                      ResAppNum res_app_num, AppResourceInfo *res_info_out);

//! Get the resource_id for a given timeline resource and size
//! @param timeline_res pointer to TimelineResourceInfo which contains the timeline resource ID and
//! corresponding app UUID
//! @param size the TimelineResourceSize requested
//! @param resource_id outparam pointer to AppResourceIdInfo containing the ID and
//! ResAppNum for the requested timeline resource (both 0 if the resource could not be located)
void timeline_resources_get_id(const TimelineResourceInfo *timeline_res, TimelineResourceSize size,
                               AppResourceInfo *res_info_out);
void sys_timeline_resources_get_id(const TimelineResourceInfo *timeline_res,
                                   TimelineResourceSize size, AppResourceInfo *res_info_out);
