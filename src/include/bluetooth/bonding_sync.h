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

#include "util/attributes.h"

#include <bluetooth/sm_types.h>

//! Packed, because this is serialized for the host-controller protocol.
typedef struct PACKED BleBonding {
  SMPairingInfo pairing_info;
  //! True if the remote device is capable of talking PPoGATT.
  bool is_gateway:1;

  //! True if the local device address should be pinned.
  bool should_pin_address:1;

  //! @note bt_persistent_storage_... uses only 5 bits to store this!
  //! @see BleBondingFlag
  uint8_t flags:5;

  uint8_t rsvd:1;

  //! Valid iff should_pin_address is true
  BTDeviceAddress pinned_address;
} BleBonding;

//! Called by the FW after starting the Bluetooth stack to register existing bondings.
//! @note When the Bluetooth is torn down, there won't be any "remove" calls. If needed, the BT
//! driver lib should clean up itself in bt_driver_stop().
void bt_driver_handle_host_added_bonding(const BleBonding *bonding);

//! Called by the FW when a bonding is removed (i.e. user "Forgot" a bonding from Settings).
void bt_driver_handle_host_removed_bonding(const BleBonding *bonding);

//! Called by the BT driver after successfully pairing a new device.
//! @param addr The address that is used to refer to the connection. This is used to associate
//! the bonding with the GAPLEConnection.
extern void bt_driver_cb_handle_create_bonding(const BleBonding *bonding,
                                               const BTDeviceAddress *addr);
