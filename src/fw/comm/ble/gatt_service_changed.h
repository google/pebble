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

#include <stdint.h>
#include <stdbool.h>

//! @file This module contains the "Generic Attribute Profile Service" code, both the server and
//! client parts. Both ends can optionally implement this service (and client). iOS does for example
//! and so does Pebble. The one characteristic this service has is called "Service Changed". Its
//! purpose is to indicate to the other side whenever there are changes to the local GATT database
//! (and what ATT handle range the change is affecting), for example when an app adds or removes
//! a GATT service or characteristics.
//!
//! The server is mostly implemented in Bluetopia's GATT.c, but relies on our FW for some mundane
//! things like handling subscription events and actually firing off "Service Changed" indications.
//!
//! The client part is hooking into the guts of Pebble's gatt.c and gatt_client_discovery.c,
//! to catch GATT Indications before they reach higher layers and to trigger transparent rediscovery
//! of remote services.
//!
//! See BT Spec 4.0, Volume 3, Part G, 7.1 "Service Changed" for more information about the service.

////////////////////////////////////////////////////////////////////////////////////////////////////
// Client -- Pebble consuming the remote's "Service Changed" characteristic

struct GAPLEConnection;

//! Optionally handles GATT Value Indications, in case the ATT handle matches the GATT Service
//! Changed characteristic value for the connection. When it matches, it will autonomously initiate
//! GATT Service Discovery to refresh the local GATT cache.
//! @note bt_lock is assumed to be taken by the caller
bool gatt_service_changed_client_handle_indication(struct GAPLEConnection *connection,
                                                   uint16_t att_handle, const uint8_t *value,
                                                   uint16_t value_length);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Server -- Pebble serving up the "Service Changed" characteristic to the remote

void gatt_service_changed_server_handle_fw_update(void);

////////////////////////////
// For Testing:

// BLEChrIdx gatt_service_changed_get_characteristic_idx(void);
