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

#include "event_service_client.h"
#include "applib/applib_malloc.auto.h"
#include "plugin_service.h"
#include "plugin_service_private.h"
#include "syscall/syscall.h"

#include "process_state/app_state/app_state.h"
#include "process_state/worker_state/worker_state.h"
#include "system/logging.h"
#include "system/passert.h"

// ---------------------------------------------------------------------------------------------------------------
// Get our state variables
static PluginServiceState* prv_get_state(PebbleTask task) {
  if (task == PebbleTask_Unknown) {
    task = pebble_task_get_current();
  }
  if (task == PebbleTask_App) {
    return app_state_get_plugin_service();
  } else {
    PBL_ASSERTN(task == PebbleTask_Worker);
    return worker_state_get_plugin_service();
  }
}


// ---------------------------------------------------------------------------------------------------------------
// Lookup the plugin service index from the UUID. We store the index in the event structure instead of the UUID
//  so that we have payload room.
static uint16_t prv_get_service_index(Uuid *uuid) {
  return sys_event_service_get_plugin_service_index(uuid);
}


// ---------------------------------------------------------------------------------------------------------------
// Used by list_find to locate the handler for a specific service index.
static bool prv_service_filter(ListNode *node, void *tp) {
  PluginServiceEntry *info = (PluginServiceEntry *)node;
  uint16_t service_idx = (uint16_t)(uintptr_t)tp;
  return (info->service_index == service_idx);
}


// ---------------------------------------------------------------------------------------------------------------
// Callback provided to the app_event_service. All events of type PEBBLE_PLUGIN_SERVICE_EVENT that get sent to
// this task trigger this callback. From here, we look up which user-supplied callback corresponds to the
// service index stored in the event structure and then pass control to that user-supplied callback.
static void prv_handle_event_service_event(PebbleEvent *e, void *context) {
  PluginServiceState *state = prv_get_state(PebbleTask_Unknown);
  uint16_t service_index = e->plugin_service.service_index;

  ListNode *found;
  ListNode *list = &state->subscribed_services;
  found = list_find(list, prv_service_filter, (void*)(uintptr_t)service_index);
  if (!found) {
    return;
  }

  // Call the handler provided by the client
  PluginServiceEntry *entry = (PluginServiceEntry *)found;
  entry->handler(e->plugin_service.type, &e->plugin_service.data);
}


// ---------------------------------------------------------------------------------------------------------------
// Subscribe to a specific plug-in service by uuid.
bool plugin_service_subscribe(Uuid *uuid, PluginServiceHandler handler) {
  PluginServiceState *state = prv_get_state(PebbleTask_Unknown);
  uint16_t service_index = prv_get_service_index(uuid);

  ListNode *list = &state->subscribed_services;
  if (list_find(list, prv_service_filter, (void*)(uintptr_t)service_index)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Plug service handler already subscribed");
    return false;
  }

  // Add to handlers list
  PluginServiceEntry *entry = applib_type_zalloc(PluginServiceEntry);
  if (!entry) {
    PBL_LOG(LOG_LEVEL_ERROR, "OOM in plugin_service_subscribe");
    return false;
  }
  entry->service_index = service_index;
  entry->handler = handler;
  list_append(list, &entry->list_node);

  // Subscribe to the app event service if we haven't already
  if (!state->subscribed_to_app_event_service) {
    state->subscribed_to_app_event_service = true;
    event_service_client_subscribe(&state->event_service_info);
  }
  return true;
}


// ---------------------------------------------------------------------------------------------------------------
// Unsubscribe from a specific plug-in service by uuid.
bool plugin_service_unsubscribe(Uuid *uuid) {
  PluginServiceState *state = prv_get_state(PebbleTask_Unknown);
  uint16_t service_index = prv_get_service_index(uuid);

  ListNode *found;
  ListNode *list = &state->subscribed_services;
  found = list_find(list, prv_service_filter, (void*)(uintptr_t)service_index);
  if (!found) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Plug service handler already unsubscribed");
    return true;
  }

  list_remove(found, NULL, NULL);
  applib_free(found);
  return true;
}


// ---------------------------------------------------------------------------------------------------------------
// Send an event to all registered subscribers of the given plugin service identified by UUID.
void plugin_service_send_event(Uuid *uuid, uint8_t type, PluginEventData *data) {
  uint16_t service_index = prv_get_service_index(uuid);

  PebbleEvent event = {
    .type = PEBBLE_PLUGIN_SERVICE_EVENT,
    .plugin_service = {
        .service_index = service_index,
        .type = type,
        .data = *data
    },
  };
  sys_send_pebble_event_to_kernel(&event);
}


// ---------------------------------------------------------------------------------------------------------------
// Init our state variables.
void plugin_service_state_init(PluginServiceState *state) {
  *state = (PluginServiceState) {
    .event_service_info = {
        .type = PEBBLE_PLUGIN_SERVICE_EVENT,
        .handler = &prv_handle_event_service_event,
    },
  };
}

