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

#include <string.h>
#include <inttypes.h>

#include "apps/system_apps/launcher/launcher_app.h"
#include "kernel/pbl_malloc.h"
#include "os/tick.h"
#include "process_management/worker_manager.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/list.h"
#include "util/time/time.h"

#include "services/common/analytics/analytics.h"
#include "services/common/analytics/analytics_storage.h"
#include "services/common/analytics/analytics_metric.h"
#include "services/common/analytics/analytics_heartbeat.h"
#include "services/common/analytics/analytics_logging.h"
#include "services/common/analytics/analytics_external.h"

// Stopwatches
typedef struct {
  ListNode node;
  AnalyticsMetric metric;
  RtcTicks starting_ticks;
  uint32_t count_per_sec;
  AnalyticsClient client;
} AnalyticsStopwatchNode;

static ListNode *s_stopwatch_list = NULL;
static bool prv_is_stopwatch_for_metric(ListNode *found_node, void *data);

void analytics_init(void) {
  analytics_metric_init();
  analytics_storage_init();
  analytics_logging_init();
}

void analytics_set(AnalyticsMetric metric, int64_t value, AnalyticsClient client) {
  analytics_set_for_uuid(metric, value, analytics_uuid_for_client(client));
}

void analytics_max(AnalyticsMetric metric, int64_t val, AnalyticsClient client) {
  const Uuid *uuid = analytics_uuid_for_client(client);

  analytics_storage_take_lock();

  AnalyticsHeartbeat *heartbeat = analytics_storage_find(metric, uuid, AnalyticsClient_Ignore);
  if (heartbeat) {
    const int64_t prev_value = analytics_heartbeat_get(heartbeat, metric);
    if (prev_value < val) {
      analytics_heartbeat_set(heartbeat, metric, val);
    }
  }

  analytics_storage_give_lock();
}

void analytics_set_for_uuid(AnalyticsMetric metric, int64_t value, const Uuid *uuid) {
  analytics_storage_take_lock();

  AnalyticsHeartbeat *heartbeat = analytics_storage_find(metric, uuid, AnalyticsClient_Ignore);
  if (heartbeat) {
    // We allow only a limited number of app heartbeats to accumulate. A NULL means we reached the
    // limit
    analytics_heartbeat_set(heartbeat, metric, value);
  }

  analytics_storage_give_lock();
}

void analytics_set_entire_array(AnalyticsMetric metric, const void *value, AnalyticsClient client) {
  analytics_storage_take_lock();

  AnalyticsHeartbeat *heartbeat = analytics_storage_find(metric, NULL, client);
  if (heartbeat) {
    // We allow only a limited number of app heartbeats to accumulate. A NULL means we reached the
    // limit
    analytics_heartbeat_set_entire_array(heartbeat, metric, value);
  }

  analytics_storage_give_lock();
}


void analytics_inc(AnalyticsMetric metric, AnalyticsClient client) {
  analytics_add(metric, 1, client);
}

void analytics_inc_for_uuid(AnalyticsMetric metric, const Uuid *uuid) {
  analytics_add_for_uuid(metric, 1, uuid);
}

void analytics_add_for_uuid(AnalyticsMetric metric, int64_t amount, const Uuid *uuid) {
  analytics_storage_take_lock();

  // We don't currently allow incrementing signed integers, because the
  // only intended use of this API call is to increment counters, and counters
  // should always be unsigned. This restriction could be changed in the future,
  // however.
  PBL_ASSERTN(analytics_metric_is_unsigned(metric));

  AnalyticsHeartbeat *heartbeat = analytics_storage_find(metric, uuid, AnalyticsClient_Ignore);
  if (heartbeat) {
    // We allow only a limited number of app heartbeats to accumulate. A NULL means we reached the
    // limit
    uint64_t val = analytics_heartbeat_get(heartbeat, metric);
    analytics_heartbeat_set(heartbeat, metric, val + amount);
  }

  analytics_storage_give_lock();
}

void analytics_add(AnalyticsMetric metric, int64_t amount, AnalyticsClient client) {
  analytics_add_for_uuid(metric, amount, analytics_uuid_for_client(client));
}

///////////////////
// Stopwatches
static bool prv_is_stopwatch_for_metric(ListNode *found_node, void *data) {
  AnalyticsStopwatchNode* stopwatch_node = (AnalyticsStopwatchNode*)found_node;
  return stopwatch_node->metric == (AnalyticsMetric)data;
}
AnalyticsStopwatchNode *prv_find_stopwatch(AnalyticsMetric metric) {
  ListNode *node = list_find(s_stopwatch_list, prv_is_stopwatch_for_metric, (void*)metric);
  return (AnalyticsStopwatchNode*)node;
}

static uint32_t prv_stopwatch_elapsed_ms(AnalyticsStopwatchNode *stopwatch, uint64_t current_ticks) {
  const uint64_t dt_ms = ticks_to_milliseconds(current_ticks - stopwatch->starting_ticks);
  return (((uint64_t) stopwatch->count_per_sec) * dt_ms) / MS_PER_SECOND;
}

void analytics_stopwatch_start(AnalyticsMetric metric, AnalyticsClient client) {
  analytics_stopwatch_start_at_rate(metric, MS_PER_SECOND, client);
}

void analytics_stopwatch_start_at_rate(AnalyticsMetric metric, uint32_t count_per_sec, AnalyticsClient client) {
  analytics_storage_take_lock();

  // Stopwatch metrics must be UINT32!
  PBL_ASSERTN(analytics_metric_element_type(metric) == ANALYTICS_METRIC_ELEMENT_TYPE_UINT32);

  if (prv_find_stopwatch(metric)) {
    // TODO: Increment this back up to LOG_LEVEL_WARNING when it doesn't happen
    // on every bootup (PBL-5393)
    PBL_LOG(LOG_LEVEL_DEBUG, "Analytics stopwatch for metric %d already started!", metric);
    goto unlock;
  }

  AnalyticsStopwatchNode *stopwatch = kernel_malloc_check(sizeof(*stopwatch));
  *stopwatch = (AnalyticsStopwatchNode) {
    .metric = metric,
    .starting_ticks = rtc_get_ticks(),
    .count_per_sec = count_per_sec,
    .client = client,
  };

  list_init(&stopwatch->node);
  s_stopwatch_list = list_prepend(s_stopwatch_list, &stopwatch->node);

unlock:
  analytics_storage_give_lock();
}

void analytics_stopwatch_stop(AnalyticsMetric metric) {
  analytics_storage_take_lock();

  AnalyticsStopwatchNode *stopwatch = prv_find_stopwatch(metric);
  if (!stopwatch) {
    // TODO: Increment this back up to LOG_LEVEL_WARNING when it doesn't happen
    // on every bootup (PBL-5393)
    PBL_LOG(LOG_LEVEL_DEBUG, "Analytics stopwatch for metric %d already stopped!", metric);
    goto unlock;
  }

  analytics_add(metric, prv_stopwatch_elapsed_ms(stopwatch, rtc_get_ticks()), stopwatch->client);

  list_remove(&stopwatch->node, &s_stopwatch_list, NULL);
  kernel_free(stopwatch);

unlock:
  analytics_storage_give_lock();
}

void analytics_stopwatches_update(uint64_t current_ticks) {
  PBL_ASSERTN(analytics_storage_has_lock());

  ListNode *cur = s_stopwatch_list;
  while (cur != NULL) {
    AnalyticsStopwatchNode *node = (AnalyticsStopwatchNode*)cur;
    analytics_add(node->metric, prv_stopwatch_elapsed_ms(node, current_ticks), node->client);
    node->starting_ticks = current_ticks;
    cur = cur->next;
  }
}
