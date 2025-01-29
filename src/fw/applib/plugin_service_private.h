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
#include "kernel/events.h"
#include "event_service_client.h"
#include "plugin_service.h"
#include "util/uuid.h"


// We dynamically allocate one of these for every service we subscribe to
typedef struct {
  ListNode  list_node;
  uint16_t  service_index;                    // index of the service
  PluginServiceHandler handler;               // handler for this service
} PluginServiceEntry;


typedef struct __attribute__((packed)) PluginServiceState {
  bool subscribed_to_app_event_service : 1;   // Set on first plugin_service_subscribe by this app
  EventServiceInfo event_service_info;
  ListNode subscribed_services;               // Linked list of PluginServiceEntry
} PluginServiceState;

void plugin_service_state_init(PluginServiceState *state);

