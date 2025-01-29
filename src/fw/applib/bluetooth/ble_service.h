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

#include <bluetooth/bluetooth_types.h>

//! Gets the characteristics associated with a service.
//! @param service The service for which to get the characteristics
//! @param[out] characteristics_out An array of pointers to characteristics,
//! into which the associated characteristics will be copied.
//! @param num_characteristics The size of the characteristics_out array.
//! @return The total number of characteristics for the service. This might be a
//! larger number than num_in_out will contain, if the passed array was not
//! large enough to hold all the pointers.
//! @note For convenience, the services are owned by the system and references
//! to services, characteristics and descriptors are guaranteed to remain valid
//! *until the BLEClientServiceChangeHandler is called again* or until
//! application is terminated.
uint8_t ble_service_get_characteristics(BLEService service,
                                       BLECharacteristic characteristics_out[],
                                       uint8_t num_characteristics);

//! Gets the Service UUID of a service.
//! @param service The service for which to get the Service UUID.
//! @return The 128-bit Service UUID, or UUID_INVALID if the service reference
//! was invalid.
//! @note The returned UUID is always a 128-bit UUID, even if the device
//! its internal GATT service database uses 16-bit or 32-bit Service UUIDs.
//! @see bt_uuid_expand_16bit for a macro that converts 16-bit UUIDs to 128-bit
//! equivalents.
//! @see bt_uuid_expand_32bit for a macro that converts 32-bit UUIDs to 128-bit
//! equivalents.
Uuid ble_service_get_uuid(BLEService service);

//! Gets the device that hosts the service.
//! @param service The service for which to find the device it belongs to.
//! @return The device hosting the service, or an invalid device if the service
//! reference was invalid. Use bt_device_is_invalid() to test whether the
//! returned device is invalid.
BTDevice ble_service_get_device(BLEService service);

//! Gets the services that are references by a service as "Included Service".
//! @param service The service for which to get the included services
//! @param[out] included_services_out An array of pointers to services,
//! into which the included services will be copied.
//! @param num_services the size of the included_services_out array.
//! @return The total number of included services for the service. This might be
//! a larger number than included_services_out can contain, if the passed array
//! was not large enough to hold all the pointers.
//! @note For convenience, the services are owned by the system and references
//! to services, characteristics and descriptors are guaranteed to remain valid
//! *until the BLEClientServiceChangeHandler is called again* or until
//! application is terminated.
uint8_t ble_service_get_included_services(BLEService service,
                                          BLEService included_services_out[],
                                          uint8_t num_services);


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// (FUTURE / LATER / NOT SCOPED)
// Just to see how symmetric the Server APIs could be:

// creates + adds to GATT DB (?)
// Services aren't supposed to change. Pass everything into the 'create' call:
BLEService ble_service_create(const Uuid *service_uuid,
                              BLECharacteristic characteristics[],
                              uint8_t num_characteristics);

void ble_service_set_included_services(BLEService service,
                                       BLEService included_services[],
                                       uint8_t num_included_services);

// removes from GATT DB (?) + destroys
void ble_service_destroy(BLEService service);
