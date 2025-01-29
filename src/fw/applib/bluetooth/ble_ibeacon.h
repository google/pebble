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

#include "applib/bluetooth/ble_ad_parse.h"

//! Size in bytes of the iBeacon advertisement data, including the length and
//! AD Type bytes.
#define IBEACON_ADVERTISEMENT_DATA_SIZE (27)

//! Data structure representing an iBeacon advertisement.
typedef struct {
  //! The application UUID that the iBeacon advertised. In iOS' CoreBluetooth,
  //! this corresponds to the "proximityUUID" property of instances of CLBeacon.
  Uuid uuid;

  //! Custom value, most significant part.
  uint16_t major;

  //! Custom value, least significant part.
  uint16_t minor;

  //! Estimated distance to the iBeacon in centimeters. In iOS' CoreBluetooth,
  //! this corresponds to the "accuracy" property of instances of CLBeacon.
  uint16_t distance_cm;

  //! The received signal strength from the iBeacon, in decibels.
  int8_t rssi;

  //! The calibrated power of the iBeacon. This is the RSSI measured at 1 meter
  //! distance from the iBeacon. The iBeacon transmits this information in its
  //! advertisement. Using this and the actual RSSI, the distance is estimated.
  int8_t calibrated_tx_power;
} BLEiBeacon;

//! Gets the UUID of the iBeacon.
//! @param The iBeacon
//! @return The UUID that the iBeacon advertised. In iOS' CoreBluetooth,
//! this corresponds to the "proximityUUID" property of instances of CLBeacon.
Uuid ble_ibeacon_get_uuid(const BLEiBeacon *ibeacon);

//! Gets the major value of the iBeacon.
//! @param The iBeacon
//! @return The major, custom value.
uint16_t ble_ibeacon_get_major(const BLEiBeacon *ibeacon);

//! Gets the minor value of the iBeacon.
//! @param The iBeacon
//! @return The minor, custom value.
uint16_t ble_ibeacon_get_minor(const BLEiBeacon *ibeacon);

//! Gets the estimated distance to the iBeacon, in centimeters.
//! @param The iBeacon
//! @return The estimated distance in centimeters.
uint16_t ble_ibeacon_get_distance_cm(const BLEiBeacon *ibeacon);

//! Create BLEiBeacon from advertisement data.
//! @param ad Advertisement data, as acquired from the BLEScanHandler callback.
//! @param rssi The RSSI of the advertisement, as acquired from the
//! BLEScanHandler callback.
//! @return BLEiBeacon object if iBeacon data is found, or NULL if the
//! advertisement data did not contain valid iBeacon data.
BLEiBeacon *ble_ibeacon_create_from_ad_data(const BLEAdData *ad,
                                            int8_t rssi);

//! Destroys an BLEiBeacon object and frees its resources that were allocated
//! earlier by ble_ibeacon_create_from_ad_data().
//! @param ibeacon Reference to the BLEiBeacon to destroy.
void ble_ibeacon_destroy(BLEiBeacon *ibeacon);

// -----------------------------------------------------------------------------
//! Internal iBeacon Advertisement Data parser
//! @param ad The raw advertisement data
//! @param rssi The RSSI of the advertisement
//! @param[out] ibeacon_out Will contain the parsed iBeacon data if the call
//! returns true.
//! @return true if the data element was successfully parsed as iBeacon,
//! false if the data element could not be parsed as iBeacon.
bool ble_ibeacon_parse(const BLEAdData *ad, int8_t rssi,
                       BLEiBeacon *ibeacon_out);

// -----------------------------------------------------------------------------
//! Internal iBeacon Advertisement Data serializer
//! @param ibeacon_in The iBeacon structure to serialize. The rssi and
//! distance_cm fields are ignored because they are only valid for received
//! iBeacon packets.
//! @param[out] ad_out The advertisement payload to write the data into.
//! @return true if the iBeacon data was written successfully.
bool ble_ibeacon_compose(const BLEiBeacon *ibeacon_in, BLEAdData *ad_out);
