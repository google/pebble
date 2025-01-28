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

#include "util/uuid.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

//! Bluetooth error codes.
typedef enum {
  //! The operation was successful.
  BTErrnoOK = 0,

  //! Connection established successfully.
  BTErrnoConnected = BTErrnoOK,

  //! One or more parameters were invalid.
  BTErrnoInvalidParameter = 1,

  //! The connection was terminated because it timed out. Examples of cause for
  //! a connection timeout are: devices going out of range of each other or
  //! lost packets due to RF interference.
  BTErrnoConnectionTimeout = 2,

  //! The connection was terminated by the remote device.
  BTErrnoRemotelyTerminated = 3,

  //! The connection was terminated by the system.
  BTErrnoLocallyTerminatedBySystem = 4,

  //! The connection was terminated by the application.
  BTErrnoLocallyTerminatedByApp = 5,

  //! The system did not have enough resources for the operation.
  BTErrnoNotEnoughResources = 6,

  //! The remote device does not support pairing.
  BTErrnoPairingNotSupported = 7,

  //! The pairing failed because the user did not confirm.
  BTErrnoPairingConfirmationFailed = 8,

  //! The pairing failed because it timed out.
  BTErrnoPairingTimeOut = 9,

  //! The pairing failed because Out-of-Band data was not available.
  BTErrnoPairingOOBNotAvailable = 10,

  //! The requested operation cannot be performed in the current state.
  BTErrnoInvalidState = 11,

  //! GATT Service Discovery timed out
  BTErrnoServiceDiscoveryTimeout = 12,

  //! GATT Service Discovery failed due to disconnection
  BTErrnoServiceDiscoveryDisconnected = 13,

  //! GATT Service Discovery was restarted because the remote device indicated that it changed
  //! its GATT database. Prior BLEService, BLECharacteristic and BLEDescriptor handles must be
  //! invalidated when receiving this status code. The system will automatically start the
  //! service discovery process again, therefore apps do not need to call
  //! ble_client_discover_services_and_characteristics() again.
  BTErrnoServiceDiscoveryDatabaseChanged = 14,

  //! Errors after this value are internal Bluetooth stack errors that could not
  //! be mapped onto more meaningful errors by the system.
  BTErrnoInternalErrorBegin = 9000,

  //! Errors after this fvalue are HCI errors that could not be mapped into more
  //! meaningful errors by the system.
  BTErrnoHCIErrorBegin = 10000,

  //! Other, uncategorized error.
  //! @internal This is also the highest allowed value (14 bits all set).
  //! See PebbleBLEGATTClientEvent for why.
  BTErrnoOther = 0x3fff,
} BTErrno;

//! Error values that can be returned by the server in response to read, write
//! and subscribe operations. These error values correspond to the (G)ATT error
//! codes as specified in the Bluetooth 4.0 Specification, Volume 3, Part F,
//! 3.4.1.1, Table 3.3.
typedef enum {
  BLEGATTErrorSuccess = 0x00,
  BLEGATTErrorInvalidHandle = 0x01,
  BLEGATTErrorReadNotPermitted = 0x02,
  BLEGATTErrorWriteNotPermitted = 0x03,
  BLEGATTErrorInvalidPDU = 0x04,
  BLEGATTErrorInsufficientAuthentication = 0x05,
  BLEGATTErrorRequestNotSupported = 0x06,
  BLEGATTErrorInvalidOffset = 0x07,
  BLEGATTErrorInsufficientAuthorization = 0x08,
  BLEGATTErrorPrepareQueueFull = 0x09,
  BLEGATTErrorAttributeNotFound = 0x0A,
  BLEGATTErrorAttributeNotLong = 0x0B,
  BLEGATTErrorInsufficientEncryptionKeySize = 0x0C,
  BLEGATTErrorInvalidAttributeValueLength = 0x0D,
  BLEGATTErrorUnlikelyError = 0x0E,
  BLEGATTErrorInsufficientEncryption = 0x0F,
  BLEGATTErrorUnsupportedGroupType = 0x10,
  BLEGATTErrorInsufficientResources = 0x11,

  BLEGATTErrorApplicationSpecificErrorStart = 0x80,
  BLEGATTErrorApplicationSpecificErrorEnd = 0xFC,

  BLEGATTErrorCCCDImproperlyConfigured = 0xFD,
  BLEGATTErrorProcedureAlreadyInProgress = 0xFE,
  BLEGATTErrorOutOfRange = 0xFF,

  BLEGATTErrorRequestTimeOut = 0x100,
  BLEGATTErrorRequestPrepareWriteDataMismatch = 0x101,
  BLEGATTErrorLocalInsufficientResources = 0x102,
} BLEGATTError;

//! @internal Macro to map Bluetopia errors to BTErrno
#define BTErrnoWithBluetopiaError(e) ((int) BTErrnoInternalErrorBegin - e)

//! @internal Macro to map HCI errors to BTErrno
#define BTErrnoWithHCIError(e) ((int) BTErrnoHCIErrorBegin + e)

//! Property bits of a characteristic
//! See the Bluetooth 4.0 Specification, Volume 3, Part G,
//! 3.3.1.1 "Characteristic Properties" for more details.
//! @see ble_characteristic_get_properties
typedef enum {
  BLEAttributePropertyNone = 0,
  BLEAttributePropertyBroadcast = (1 << 0),
  BLEAttributePropertyRead = (1 << 1),
  BLEAttributePropertyWriteWithoutResponse = (1 << 2),
  BLEAttributePropertyWrite = (1 << 3),
  BLEAttributePropertyNotify = (1 << 4),
  BLEAttributePropertyIndicate = (1 << 5),
  BLEAttributePropertyAuthenticatedSignedWrites = (1 << 6),
  BLEAttributePropertyExtendedProperties = (1 << 7),

  // Properties for Characteristics & Descriptors that
  // are hosted by the local server:
  BLEAttributePropertyReadingRequiresEncryption = (1 << 8),
  BLEAttributePropertyWritingRequiresEncryption = (1 << 9),
} BLEAttributeProperty;

//! Opaque reference to a service object.
typedef uintptr_t BLEService;

//! Opaque reference to a characteristic object.
typedef uintptr_t BLECharacteristic;

//! Opaque reference to a descriptor object.
typedef uintptr_t BLEDescriptor;

_Static_assert(sizeof(BLEDescriptor) == sizeof(uintptr_t), "BLEDescriptor is invalid size");
_Static_assert(sizeof(BLECharacteristic) == sizeof(uintptr_t), "BLECharacteristic is invalid size");

#define BLE_SERVICE_INVALID ((BLEService) 0)
#define BLE_CHARACTERISTIC_INVALID ((BLECharacteristic) 0)
#define BLE_DESCRIPTOR_INVALID ((BLEDescriptor) 0)

//! Identifier for a device bonding.
//! They stay the same across reboots, so they can be persisted by apps.
typedef uint8_t BTBondingID;

#define BT_BONDING_ID_INVALID ((BTBondingID) ~0)

typedef struct __attribute__((__packed__)) BTDeviceAddress {
  uint8_t octets[6];
} BTDeviceAddress;

//! Size of a BTDeviceAddress struct
#define BT_DEVICE_ADDRESS_SIZE (sizeof(BTDeviceAddress))

//! Print format for printing BTDeviceAddress structs
//! @see BT_DEVICE_ADDRESS_XPLODE
#define BT_DEVICE_ADDRESS_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define BT_DEVICE_ADDRESS_FMT_BUFFER_SIZE   (18)

#define BD_ADDR_FMT "0x%02X%02X%02X%02X%02X%02X"
#define BT_ADDR_FMT_BUFFER_SIZE_BYTES   (15)

#define BT_DEVICE_NAME_BUFFER_SIZE      (20)

//! Macro decompose a BTDeviceAddress struct into its parts, so it can be used
//! with the BT_DEVICE_ADDRESS_FMT format macro
#define BT_DEVICE_ADDRESS_XPLODE(a) \
    (a).octets[5], (a).octets[4], (a).octets[3], \
    (a).octets[2], (a).octets[1], (a).octets[0]

#define BT_DEVICE_ADDRESS_XPLODE_PTR(a) \
    (a)->octets[5], (a)->octets[4], (a)->octets[3], \
    (a)->octets[2], (a)->octets[1], (a)->octets[0]

//! Data structure that represents a remote Bluetooth device.
//! The fields of the structure are opaque. Its contents should not be changed
//! or relied upon by the application.
typedef struct BTDevice {
  union {
    uint32_t opaque[2];
    uint64_t opaque_64;
  };
} BTDevice;

//! @internal The internal layout of the opaque BTDevice. This should not be
//! exported. It can also never be changed in size. It has to be exactly as
//! large as the BTDevice struct.
typedef struct __attribute__((__packed__)) BTDeviceInternal {
  union {
    struct __attribute__((__packed__)) {
      BTDeviceAddress address;
      bool is_classic:1;
      bool is_random_address:1;
      //! !!! WARNING: If you're adding more flags here, you need to update
      //! the bt_device_bits field in PebbleBLEGATTClientEvent and PebbleBLEConnectionEvent !!!
      uint16_t zero:14;
    };
    BTDevice opaque;
  };
} BTDeviceInternal;

#define BT_DEVICE_INVALID ((const BTDevice) {})
#define BT_DEVICE_INTERNAL_INVALID ((const BTDeviceInternal) {})

_Static_assert(sizeof(BTDeviceInternal) == sizeof(BTDevice),
               "BTDeviceInternal should be equal in size to BTDevice");

//! Opaque data structure representing an advertisement report and optional
//! scan response. Use the ble_ad... functions to query its contents.
struct BLEAdData;

//! @internal
//! The maximum size in bytes of an advertising report.
#define GAP_LE_AD_REPORT_DATA_MAX_LENGTH (31)

//! Flags used in an LE Advertising packet. Listed in
//! Supplement to Bluetooth Core Specification | CSSv6, Part A, 1.3.1
#define GAP_LE_AD_FLAGS_LIM_DISCOVERABLE_MASK            (1 << 0)
#define GAP_LE_AD_FLAGS_GEN_DISCOVERABLE_MASK            (1 << 1)
#define GAP_LE_AD_FLAGS_BR_EDR_NOT_SUPPORTED_MASK        (1 << 2)
#define GAP_LE_AD_FLAGS_LE_BR_EDR_SIMULT_CONTROLLER_MASK (1 << 3)
#define GAP_LE_AD_FLAGS_LE_BR_EDR_SIMULT_HOST_MASK       (1 << 4)

#define LL_CONN_INTV_MIN_SLOTS (6)    // 1.25ms / slot
#define LL_CONN_INTV_MAX_SLOTS (3200) // 1.25ms / slot
#define LL_SUPERVISION_TIMEOUT_MIN_MS (100)

//! Advertisement and scan response data
//! @internal Exported as forward struct
typedef struct BLEAdData {
  //! Lengths of the raw advertisement data
  uint8_t ad_data_length;

  //! Lengths of the raw scan response data
  uint8_t scan_resp_data_length;

  //! The raw advertisement data, concatenated with the raw scan response data.
  uint8_t data[0];
} BLEAdData;

//! Macro that does the same as bt_uuid_expand_32bit / bt_uuid_expand_16bit, but at compile-time
#define BT_UUID_EXPAND(u) \
  (0xff & ((uint32_t) u) >> 24), \
  (0xff & ((uint32_t) u) >> 16), \
  (0xff & ((uint32_t) u) >> 8), \
  (0xff & ((uint32_t) u) >> 0), \
  0x00, 0x00, 0x10, 0x00, \
  0x80, 0x00, 0x00, 0x80, \
  0x5F, 0x9B, 0x34, 0xFB
