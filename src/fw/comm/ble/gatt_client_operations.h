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

//! @file This file contains adapter code between Bluetopia's GATT APIs and
//! Pebble's GATT/API code. The functions in this file take the internal
//! reference types BLECharacteristic and BLEDescriptor to perform operations
//! upon those remote resources. The implementation uses the functions
//! gatt_client_characteristic_get_handle_and_connection_id and
//! gatt_client_descriptor_get_handle_and_connection_id from
//! gatt_client_accessors.c to look up the Bluetopia ConnectionID and the
//! ATT handles. These pieces of information is what Bluetopia cares about
//! when asked to perform a GATT operation.

#include <bluetooth/bluetooth_types.h>

#include "gap_le_task.h"

#define GATT_MTU_MINIMUM (23)

BTErrno gatt_client_op_read(BLECharacteristic characteristic,
                            GAPLEClient client);

void gatt_client_consume_read_response(uintptr_t object_ref,
                                       uint8_t value_out[],
                                       uint16_t value_length,
                                       GAPLEClient client);

BTErrno gatt_client_op_write(BLECharacteristic characteristic,
                             const uint8_t *value,
                             size_t value_length,
                             GAPLEClient client);

BTErrno gatt_client_op_write_without_response(BLECharacteristic characteristic,
                                              const uint8_t *value,
                                              size_t value_length,
                                              GAPLEClient client);

BTErrno gatt_client_op_write_descriptor(BLEDescriptor descriptor,
                                        const uint8_t *value,
                                        size_t value_length,
                                        GAPLEClient client);

BTErrno gatt_client_op_read_descriptor(BLEDescriptor descriptor,
                                       GAPLEClient client);

void gatt_client_op_cleanup(GAPLEClient client);
