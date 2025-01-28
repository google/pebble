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
#include <bluetooth/pebble_bt.h>
#include <bluetooth/responsiveness.h>

#define PEBBLE_BT_PAIRING_SERVICE_CONNECTION_STATUS_UUID PEBBLE_BT_UUID_EXPAND(1)
#define PEBBLE_BT_PAIRING_SERVICE_TRIGGER_PAIRING_UUID PEBBLE_BT_UUID_EXPAND(2)
#define PEBBLE_BT_PAIRING_SERVICE_GATT_MTU_UUID PEBBLE_BT_UUID_EXPAND(3)
// Note: UUID 4 was used by the 3.14-rc Android App for V0 of the Connection Param characteristic
// but never shipped externally
#define PEBBLE_BT_PAIRING_SERVICE_CONNECTION_PARAMETERS_UUID PEBBLE_BT_UUID_EXPAND(5)

typedef enum {
  PebblePairingServiceGATTError_UnknownCommandID =
      BLEGATTErrorApplicationSpecificErrorStart,
  PebblePairingServiceGATTError_ConnParamsInvalidRemoteDesiredState,
  PebblePairingServiceGATTError_ConnParamsMinSlotsTooSmall,
  PebblePairingServiceGATTError_ConnParamsMinSlotsTooLarge,
  PebblePairingServiceGATTError_ConnParamsMaxSlotsTooLarge,
  PebblePairingServiceGATTError_ConnParamsSupervisionTimeoutTooSmall,
  PebblePairingServiceGATTError_DeviceDoesNotSupportPLE,
} PebblePairingServiceGATTError;

//! The connectivity status, with respect to the device reading it.
typedef struct PACKED {
  union {
    struct {
      //! true if the device that is reading the status is connected (always true)
      bool ble_is_connected:1;
      //! true if the device that is reading the status is bonded, false if not
      bool ble_is_bonded:1;
      //! true if the current LE link is encrypted, false if not
      bool ble_is_encrypted:1;
      //! true if the watch has a bonding to a gateway (LE-based).
      bool has_bonded_gateway:1;
      //! true if the watch supports writing the "Don't send slave security request" bit.
      //! See https://pebbletechnology.atlassian.net/wiki/display/DEV/Pebble+GATT+Services
      bool supports_pinning_without_security_request:1;
      //! true if the reversed ppogatt was enabled at the time of bonding
      bool is_reversed_ppogatt_enabled:1;

      //! Reserved, leave zero for future use.
      uint32_t rsvd:18;

      //! The error of the last pairing process or all zeroes, if no pairing process has completed
      //! or when there were no errors. Also see BT Spec 4.2, Vol 3, Part H, 3.5.5 Pairing Failed.
      uint8_t last_pairing_result;
    };
    uint8_t bytes[4];
  };
} PebblePairingServiceConnectivityStatus;

_Static_assert(sizeof(PebblePairingServiceConnectivityStatus) == 4, "");

typedef struct PACKED {
  bool should_pin_address:1;

  //! @note Not available in Bluetopia/cc2564x implementation
  //! This flag and should_force_slave_security_request are mutually exclusive!
  bool no_slave_security_request:1;

  //! @note Not available in Bluetopia/cc2564x implementation
  //! This flag and no_slave_security_request are mutually exclusive!
  bool should_force_slave_security_request:1;

  //! @note Not available in Bluetopia/cc2564x implementation
  //! Flag to indicate that when re-pairing this device, the re-pairing should be accepted
  //! automatically for this remote device (matching IRK or matching identity address).
  //! @note This is a work-around for an Android 4.4.x bug. This opens up a security hole :( where
  //! a phone could pretend to be the "trusted" phone and pair w/o the user even knowing about it.
  //! @see https://pebbletechnology.atlassian.net/browse/PBL-39369
  bool should_auto_accept_re_pairing:1;

  //! @note Not available in Bluetopia/cc2564x implementation
  //! Flag to indicate that the PPoGATT server/client roles should be reversed to support the
  //! connected phone. Some older Android phones' GATT service API is completely busted. For those
  //! poor phones, this bit is set before pairing. The Pebble includes a "reversed" PPoGATT service
  //! that the phone app can connect to as GATT client, but this service only works if this bit
  //! gets set *before pairing*. This is a security measure: 1. to prevent non-paired devices from
  //! talking to the "reversed" PPoGATT service. 2. to prevent non-Pebble apps on paired phone that
  //! does support normal PPoGATT from connecting to the "reversed" PPoGATT service.
  //! @see ppogatt_emulated_server_wa.c
  //! @see https://pebbletechnology.atlassian.net/browse/PBL-39634
  bool is_reversed_ppogatt_enabled:1;
} PairingTriggerRequestData;

typedef struct PACKED {
  //! interval_min_ms / 1.25 msec – valid range: 7.5 msec to 4 seconds
  uint16_t interval_min_1_25ms;

  //! (interval_max_ms - interval_min_ms) / 1.25 msec
  //! @note To fit the parent struct in the minimum GATT MTU, this field is a delta and only one
  //! byte instead of the uint16_t that the BT spec uses.
  uint8_t interval_max_delta_1_25ms;

  //! Slave latency (in number of connection events)
  //! @note To fit the parent struct in the minimum GATT MTU, this field is only one byte instead
  //! of the uint16_t that the BT spec uses.
  uint8_t slave_latency_events;

  //! Supervision Timeout / 30 msec – valid range: 100 msec to 32 seconds. To fit this into one
  //! byte and to fit the parent struct in the minimum GATT MTU, the increments is not the standard
  //! 10msec!
  uint8_t supervision_timeout_30ms;
} PebblePairingServiceConnParamSet;

//! The connection parameters settings, with respect to connection to the device reading them.
typedef struct PACKED PebblePairingServiceConnParamsReadNotif {
  //! Capability bits. Reserved for future use.
  uint8_t packet_length_extension_supported:1;
  uint8_t rsvd:7;

  //! Current interval / 1.25 msec – valid range: 7.5 msec to 4 seconds
  uint16_t current_interval_1_25ms;

  //! Current Slave latency (in number of connection events) – actual max is 0x01F3, but in
  //! practice values are much lower.
  uint16_t current_slave_latency_events;

  //! Current Supervision Timeout / 10 msec – valid range: 100 msec to 32 seconds.
  uint16_t current_supervision_timeout_10ms;
} PebblePairingServiceConnParamsReadNotif;

typedef enum PebblePairingServiceConnParamsWriteCmd {
  //! Allows phone to change connection parameter set and take over control of parameter management
  PebblePairingServiceConnParamsWriteCmd_SetRemoteParamMgmtSettings = 0x00,
  //! Issues a connection parameter change request if the watch is not in the desired state
  PebblePairingServiceConnParamsWriteCmd_SetRemoteDesiredState = 0x01,
  //! Controls settings for BLE 4.2 Packet Length Extension feature
  PebblePairingServiceConnParamsWriteCmd_EnablePacketLengthExtension = 0x02,
  //! If written to disables Dialog BLE sleep mode (safeguard against PBL-39777 in case it affects
  //! more watches in the future)
  PebblePairingServiceConnParamsWriteCmd_InhibitBLESleep = 0x03,
  PebblePairingServiceConnParamsWriteCmdCount,
} PebblePairingServiceConnParamsWriteCmd;

typedef struct PACKED PebblePairingServiceRemoteParamMgmtSettings {
  //! If false/zero, Pebble should manage the connection parameters. If true/one, Pebble should
  //! NOT manage the connection parameters. In this mode, Pebble will never request a
  //! connection parameter change.
  bool is_remote_device_managing_connection_parameters:1;
  uint8_t rsvd:7;
  //! Optional. Current parameters sets used by Pebble's Connection Parameter manager.
  PebblePairingServiceConnParamSet connection_parameter_sets[];
} PebblePairingServiceRemoteParamMgmtSettings;

typedef struct PACKED PebblePairingServiceRemoteDesiredState {
  //! The desired ResponseTime as desired by the remote device.  The remote end can set this
  //! value to a faster mode when it's about to transfer/receive a lot of data. For example,
  //! when a lot of BlobDB operations are queued up, the watch doesn't know how much data is
  //! queued up on the remote end. In this case, the remote could write "ResponseTimeMin" so
  //! increase the speed temporarily. It's the remote end's responsibility to reset this to
  //! ResponseTimeMax when the bulk transfer is done.  As a safety measure, the watch is will
  //! reset it back to ResponseTimeMax after 5 minutes.  In case the phone app still wants to
  //! keep a particular desired ResponseTime, the phone app is responsible for making sure to
  //! write the value again before the 5 minute timer expires.
  uint8_t state:2;

  uint8_t rsvd:6;
} PebblePairingServiceRemoteDesiredState;

typedef struct PACKED PebblePairingServicePacketLengthExtension {
  uint8_t trigger_ll_length_req:1;
  uint8_t rsvd:7;
} PebblePairingServicePacketLengthExtension;

typedef struct PACKED PebblePairingServiceInhibitBLESleep {
  uint8_t rsvd; // for future use
} PebblePairingServiceInhibitBLESleep;

//! The connection parameters settings, with respect to connection to the device writing them.
typedef struct PACKED PebblePairingServiceConnParamsWrite {
  PebblePairingServiceConnParamsWriteCmd cmd:8;
  union PACKED {
    //! Valid iff cmd == PebblePairingServiceConnParamsWriteCmd_SetRemoteParamMgmtSettings
    PebblePairingServiceRemoteParamMgmtSettings remote_param_mgmt_settings;

    //! Valid iff cmd == PebblePairingServiceConnParamsWriteCmd_SetRemoteDesiredState
    PebblePairingServiceRemoteDesiredState remote_desired_state;

    //! Valid iff cmd == PebblePairingServiceConnParamsWriteCmd_EnablePacketLengthExtension
    PebblePairingServicePacketLengthExtension ple_req;

    //! Valid iff cmd == PebblePairingServiceConnParamsWriteCmd_InhibitBLESleep
    PebblePairingServiceInhibitBLESleep ble_sleep;
  };
} PebblePairingServiceConnParamsWrite;

#define PEBBLE_PAIRING_SERVICE_REMOTE_PARAM_MGTM_SETTINGS_SIZE_WITH_PARAM_SETS \
  (sizeof(PebblePairingServiceRemoteParamMgmtSettings) + \
   (sizeof(PebblePairingServiceConnParamSet) * NumResponseTimeState))

#define PEBBLE_PAIRING_SERVICE_CONN_PARAMS_WRITE_SIZE_WITH_PARAM_SETS \
  (offsetof(PebblePairingServiceConnParamsWrite, remote_param_mgmt_settings) + \
   PEBBLE_PAIRING_SERVICE_REMOTE_PARAM_MGTM_SETTINGS_SIZE_WITH_PARAM_SETS)

_Static_assert(NumResponseTimeState == 3, "");
_Static_assert(sizeof(PebblePairingServiceConnParamsReadNotif) <= 20, "Larger than minimum MTU!");
_Static_assert(PEBBLE_PAIRING_SERVICE_CONN_PARAMS_WRITE_SIZE_WITH_PARAM_SETS <= 20,
               "Larger than minimum MTU!");
_Static_assert(sizeof(PebblePairingServiceConnParamsWrite) <= 20, "Larger than minimum MTU!");
_Static_assert(sizeof(PebblePairingServiceConnectivityStatus) <= 20, "Larger than minimum MTU!");

typedef struct GAPLEConnection GAPLEConnection;

//! Signals to the Pebble GATT service that status change has occurred (pairing, encryption, ...),
//! allowing it to notify any BLE devices that are subscribed to connectivity status updates of the
//! change.
//! @param connection The connection for which the status was changed.
void bt_driver_pebble_pairing_service_handle_status_change(const GAPLEConnection *connection);

//! Signals to the Pebble GATT service the GATT MTU has changed, allowing it to notify any BLE
//! devices that are subscribed to GATT MTU characteristic.
//! @param connection The connection for which the GATT MTU has changed.
//! @note the caller is responsible for taking bt_lock!
void bt_driver_pebble_pairing_service_handle_gatt_mtu_change(const GAPLEConnection *connection);

//! Indicate to the FW that Connectivity Status characteristic has been unsubscribed from.
//! This is used to detect that the Pebble iOS app has been terminated.
extern void bt_driver_cb_pebble_pairing_service_handle_ios_app_termination_detected(void);

//! Indicate to the FW that the Connection Parameters characteristic has been written to with a new
//! values.
//! @param conn_params The value as written to the Connection Parameters characteristic. The BT
//! driver lib is expected to validate any written values and only call this function with valid
//! values.
//! @param conn_params_length The length of conn_params in bytes.
extern void bt_driver_cb_pebble_pairing_service_handle_connection_parameter_write(
    const BTDeviceInternal *device,
    const PebblePairingServiceConnParamsWrite *conn_params,
    size_t conn_params_length);
