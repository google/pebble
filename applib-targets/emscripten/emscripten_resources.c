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

#include "emscripten_resources.h"

#include "resource/resource_storage_impl.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  uint32_t offset;
  uint32_t length;
} Resource;

#define MANIFEST_SIZE (sizeof(ResourceManifest))
#define TABLE_ENTRY_SIZE (sizeof(ResTableEntry))
#define MAX_RESOURCES_FOR_SYSTEM_STORE 512
#define SYSTEM_STORE_METADATA_BYTES \
    (MANIFEST_SIZE + MAX_RESOURCES_FOR_SYSTEM_STORE * TABLE_ENTRY_SIZE)

////////////////////////////////////////////////
// Custom Resources
//
// Custom resources in Rocky.js are implemented with a set of callbacks
// on the javascript side which implement resource APIs
//
// We store those callbacks in an array and return a resource ID which
// can then be used as if it was a valid resource
//
// Under the hood, we just lookup & call the initially provided callbacks

typedef struct {
  uint32_t resource_id;
  ResourceReadCb read;
  ResourceGetSizeCb get_size;
} EmxCustomResource;

typedef struct {
  EmxCustomResource *custom_resources;
  uint32_t last_id;
  int array_size;
  int next_index;
} CustomResList;

static CustomResList s_custom_res_list = {0};

static int prv_custom_resource_get_index(uint32_t resource_id) {
  int index;
  for (index = 0; index < s_custom_res_list.next_index; ++index) {
    if (s_custom_res_list.custom_resources[index].resource_id == resource_id) {
      return index;
    }
  }

  return -1;
}

static EmxCustomResource *prv_custom_resource_get(uint32_t resource_id) {
  int index = prv_custom_resource_get_index(resource_id);
  if (index < 0) {
    return NULL;
  } else {
    return &s_custom_res_list.custom_resources[index];
  }
}


static void prv_custom_resource_add(EmxCustomResource *res) {
  if (s_custom_res_list.array_size <= s_custom_res_list.next_index) {
    // grow the list
    int new_size = (s_custom_res_list.array_size + 1) * 2;
    s_custom_res_list.custom_resources = realloc(s_custom_res_list.custom_resources, new_size);
    s_custom_res_list.array_size = new_size;
  }

  s_custom_res_list.custom_resources[s_custom_res_list.next_index] = *res;
  ++s_custom_res_list.next_index;
}

static void prv_custom_resource_remove(uint32_t resource_id) {
  int index = prv_custom_resource_get_index(resource_id);
  if (index < 0) {
    return;
  }

  --s_custom_res_list.next_index;
  if (s_custom_res_list.next_index == index) {
    // we had the last resource, we're done
    return;
  }

  memmove(&s_custom_res_list.custom_resources[index],
          &s_custom_res_list.custom_resources[index + 1],
          (s_custom_res_list.next_index - index) * sizeof(EmxCustomResource));
}

//////////////////////////////////////
// System Resources
//
// In this case, we shove the pbpack in an emscripten "file" (baked into the resulting JS
// and reimplement system resource APIs using standard C file I/O

static FILE *s_resource_file = NULL;

static uint32_t prv_read(uint32_t offset, void *data, size_t num_bytes) {
  if (!s_resource_file || fseek(s_resource_file, offset, SEEK_SET)) {
    printf("%s: Couldn't seek to %d\n", __FILE__, offset);
    return 0;
  }
  return fread(data, 1, num_bytes, s_resource_file);
}

static ResourceManifest prv_get_manifest(void) {
  ResourceManifest manifest = {};
  prv_read(0, &manifest, sizeof(ResourceManifest));
  return manifest;
}

static bool prv_get_table_entry(ResTableEntry *entry, uint32_t index) {
  uint32_t addr = sizeof(ResourceManifest) + index * sizeof(ResTableEntry);
  return prv_read(addr, entry, sizeof(ResTableEntry));
}

static bool prv_get_resource(uint32_t resource_id, Resource *res) {
  *res = (Resource){
    .length = 0,
    .offset = 0,
  };

  ResourceManifest manifest = prv_get_manifest();

  if (resource_id > manifest.num_resources) {
    printf("%s: resource id %d > %d is out of range\n", __FILE__,
                                                        resource_id,
                                                        manifest.num_resources);
    return false;
  }

  ResTableEntry entry;
  if (!prv_get_table_entry(&entry, resource_id - 1)) {
    printf("%s: Failed to read table entry for %d\n", __FILE__, resource_id);
    return false;
  }

  if ((entry.resource_id != resource_id) ||
      (entry.length == 0)) {
    // empty resource
    printf("%s: Invalid resource for %d\n", __FILE__, resource_id);
    return false;
  }

  res->offset = SYSTEM_STORE_METADATA_BYTES + entry.offset;
  res->length = entry.length;

  return true;
}

///////////////////////////////
// API

size_t emx_resources_read(ResAppNum app_num,
                          uint32_t resource_id,
                          uint32_t offset,
                          uint8_t *buf,
                          size_t num_bytes) {
  if (offset > INT_MAX || num_bytes > INT_MAX) {
    return 0;
  }

  EmxCustomResource *custom_res = NULL;
  if (app_num != SYSTEM_APP && (custom_res = prv_custom_resource_get(resource_id))) {
    return custom_res->read(offset, buf, num_bytes);
  }

  Resource resource = {};
  if (!prv_get_resource(resource_id, &resource)) {
    return 0;
  }

  if (offset + num_bytes > resource.length) {
    if (offset >= resource.length) {
      // Can't recover from trying to read from beyond the resource. Read nothing.
      printf("%s: Reading past the end of the resource!\n", __FILE__);
      return 0;
    }
    num_bytes = resource.length - offset;
  }

  return prv_read(offset + resource.offset, buf, num_bytes);
}

size_t emx_resources_get_size(ResAppNum app_num, uint32_t resource_id) {
  EmxCustomResource *custom_res = NULL;
  if (app_num != SYSTEM_APP && (custom_res = prv_custom_resource_get(resource_id))) {
    return custom_res->get_size();
  }

  Resource resource = {};
  if (!prv_get_resource(resource_id, &resource)) {
    return 0;
  }

  return resource.length;
}

bool emx_resources_init(void) {
  s_resource_file = fopen("system_resources.pbpack", "r");

  if (!s_resource_file) {
    printf("Error: Failed to open resources file\n");
    return false;
  }

  return true;
}

void emx_resources_deinit(void) {
  fclose(s_resource_file);
}

uint32_t emx_resources_register_custom(ResourceReadCb read_cb, ResourceGetSizeCb get_size_cb) {
  EmxCustomResource custom_res = {
    .resource_id = ++s_custom_res_list.last_id,
    .read = read_cb,
    .get_size = get_size_cb,
  };
  prv_custom_resource_add(&custom_res);

  return custom_res.resource_id;
}

void emx_resources_remove_custom(uint32_t resource_id) {
  prv_custom_resource_remove(resource_id);
}
