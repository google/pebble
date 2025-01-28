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

#include "rocky_api_memory.h"

#include "rocky_api_global.h"
#include "rocky_api_util.h"

#include "kernel/pbl_malloc.h"
#include "process_state/app_state/app_state.h"
#include "syscall/syscall.h"
#include "system/passert.h"

#include <ecma/base/ecma-gc.h>
#include <jmem/jmem-allocator.h>
#include <jmem/jmem-heap.h>
#include <jmem/jmem-poolman.h>

#include <util/math.h>

#include <stdbool.h>
#include <string.h>

#define ROCKY_EVENT_MEMORYPRESSURE "memorypressure"
#define ROCKY_EVENT_MEMORYPRESSURE_LEVEL "level"
#define ROCKY_EVENT_MEMORYPRESSURE_LEVEL_HIGH "high"
// #define ROCKY_EVENT_MEMORYPRESSURE_LEVEL_NORMAL "normal"  // NYI: PBL-42081
// #define ROCKY_EVENT_MEMORYPRESSURE_LEVEL_LOW "low"  // NYI: PBL-42081

#define HEADROOM_MIN_SIZE_BYTES (128)

//! This struct should only be accessed from the app task, so no locking is required.
typedef struct RockyMemoryAPIContext {
  //! Reserved headroom that will be made available just before calling into the
  //! 'memorypressure' event handler.
  void *headroom;
  size_t headroom_size;

  //! True if we're currently calling the 'memorypressure' event handler.
  bool is_calling_memory_callback;
} RockyMemoryAPIContext;

static bool prv_is_headroom_allocated(const RockyMemoryAPIContext *ctx) {
  return (ctx->headroom != NULL);
}

static void prv_allocate_headroom_or_die(RockyMemoryAPIContext *ctx) {
  // It's highly likely that while executing the handler for the 'memorypressure' event,
  // new objects have been created on the heap. Therefore, it's unlikely we'll be able to reclaim
  // the desired headroom immediately after returning from the handler. Try to grab as much as we
  // can and resize it later on, see prv_resize_headroom_if_needed().
  jmem_heap_stats_t stats = {};
  jmem_heap_get_stats(&stats);
  if (stats.largest_free_block_bytes < HEADROOM_MIN_SIZE_BYTES) {
    jerry_port_fatal(ERR_OUT_OF_MEMORY, __builtin_return_address(0));
    return;
  }
  const size_t headroom_size = MIN(stats.largest_free_block_bytes,
                                   ROCKY_API_MEMORY_HEADROOM_DESIRED_SIZE_BYTES);
  // This will jerry_port_fatal() if the size isn't available:
  ctx->headroom = jmem_heap_alloc_block(headroom_size);
  ctx->headroom_size = headroom_size;
}

static void prv_deallocate_headroom(RockyMemoryAPIContext *ctx) {
  jmem_heap_free_block(ctx->headroom, ctx->headroom_size);
  ctx->headroom = NULL;
  ctx->headroom_size = 0;
}

static void prv_resize_headroom_if_needed(RockyMemoryAPIContext *ctx) {
  // If needed, try to get our headroom back at the level where we want it to be.
  if (ctx->headroom &&
      ctx->headroom_size < ROCKY_API_MEMORY_HEADROOM_DESIRED_SIZE_BYTES) {
    prv_deallocate_headroom(ctx);
    prv_allocate_headroom_or_die(ctx);
  }
}

static void prv_collect_all_garbage(void) {
  ecma_free_unused_memory(JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH, 0, true);
  jmem_pools_collect_empty();
}

static void prv_memorypressure_app_log(const char *level, const jmem_heap_stats_t *stats) {
  APP_LOG(LOG_LEVEL_WARNING, "Memory pressure level: %s", level);
  APP_LOG(LOG_LEVEL_WARNING,
          "heap size: %zu, alloc'd: %zu, waste: %zu, largest free block: %zu,",
          stats->size, stats->allocated_bytes, stats->waste_bytes, stats->largest_free_block_bytes);
  APP_LOG(LOG_LEVEL_WARNING, "used blocks: %zu, free blocks: %zu",
          stats->alloc_count, stats->free_count);
}

static void prv_call_memorypressure_handler(RockyMemoryAPIContext *ctx,
                                            const char *level, jmem_heap_stats_t *stats,
                                            bool fatal_if_not_freed) {
  if (ctx->is_calling_memory_callback && fatal_if_not_freed) {
    // If this happens, the event handler wasn't able to run because there wasn't enough memory
    // and triggered the OOM callback again -- basically this means our headroom was too small to
    // execute the handler...
    sys_analytics_inc(ANALYTICS_APP_METRIC_MEM_ROCKY_RECURSIVE_MEMORYPRESSURE_EVENT_COUNT,
                      AnalyticsClient_CurrentTask);
    return;
  }
  ctx->is_calling_memory_callback = true;

  // TODO: PBL-41990 -- Release caches internal to Rocky's API implementation

  prv_memorypressure_app_log(level, stats);

  prv_deallocate_headroom(ctx);
  prv_collect_all_garbage();

  { // New scope to cleanup the event immediately after the event handler call.
    JS_VAR memory_pressure_event = rocky_global_create_event(ROCKY_EVENT_MEMORYPRESSURE);
    JS_VAR level_val = jerry_create_string_utf8((const jerry_char_t *)level);
    jerry_set_object_field(memory_pressure_event, ROCKY_EVENT_MEMORYPRESSURE_LEVEL, level_val);
    rocky_global_call_event_handlers(memory_pressure_event);
  }

  prv_collect_all_garbage();

  prv_allocate_headroom_or_die(ctx);

  ctx->is_calling_memory_callback = false;
}

static void prv_memory_callback(jmem_free_unused_memory_severity_t severity,
                                size_t requested_size_bytes,
                                bool fatal_if_not_freed) {
  RockyMemoryAPIContext *ctx = app_state_get_rocky_memory_api_context();
  if (!fatal_if_not_freed || severity < JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH) {
    ecma_free_unused_memory(severity, requested_size_bytes, fatal_if_not_freed);

    if (!ctx->is_calling_memory_callback) {
      // It's likely memory has just been free'd, try resizing now.
      // See comment at the top of prv_allocate_headroom_or_die() why this is needed.
      prv_resize_headroom_if_needed(ctx);
    }
    return;
  }

  // Trigger aggressive garbage collection, force property hashmaps to be dropped:
  prv_collect_all_garbage();
  jmem_heap_stats_t stats = {};
  jmem_heap_get_stats(&stats);
  if (stats.largest_free_block_bytes >= requested_size_bytes + sizeof(jmem_heap_free_t)) {
    return;
  }

  prv_call_memorypressure_handler(ctx, ROCKY_EVENT_MEMORYPRESSURE_LEVEL_HIGH, &stats,
                                  fatal_if_not_freed);
}

static void prv_init(void) {
  RockyMemoryAPIContext *ctx = task_zalloc_check(sizeof(RockyMemoryAPIContext));
  app_state_set_rocky_memory_api_context(ctx);

  jmem_unregister_free_unused_memory_callback(ecma_free_unused_memory);
  jmem_register_free_unused_memory_callback(prv_memory_callback);
}

static void prv_deinit(void) {
  RockyMemoryAPIContext *ctx = app_state_get_rocky_memory_api_context();
  if (prv_is_headroom_allocated(ctx)) {
    prv_deallocate_headroom(ctx);
  }
  jmem_unregister_free_unused_memory_callback(prv_memory_callback);
  jmem_register_free_unused_memory_callback(ecma_free_unused_memory);

  task_free(ctx);
  app_state_set_rocky_memory_api_context(NULL);
}

static bool prv_add_handler(const char *event_name, jerry_value_t handler) {
  if (strcmp(event_name, ROCKY_EVENT_MEMORYPRESSURE) == 0) {
    RockyMemoryAPIContext *ctx = app_state_get_rocky_memory_api_context();
    if (!prv_is_headroom_allocated(ctx)) {
      prv_allocate_headroom_or_die(ctx);
    }
    return true;
  }
  return false;
}

const RockyGlobalAPI MEMORY_APIS = {
  .init = prv_init,
  .deinit = prv_deinit,
  .add_handler = prv_add_handler,
};
