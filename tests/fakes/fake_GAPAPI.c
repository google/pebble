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

#include "fake_GAPAPI.h"

#include "bluetopia_interface.h"

#include <string.h>

#include "clar_asserts.h"

static bool s_is_le_advertising_enabled;

static GAP_LE_Event_Callback_t s_le_adv_connection_event_callback;
static unsigned long s_le_adv_connection_callback_param;

static uint16_t s_min_advertising_interval_slots;
static uint16_t s_max_advertising_interval_slots;

void gap_le_set_advertising_disabled(void) {
  s_is_le_advertising_enabled = false;
  s_min_advertising_interval_slots = 0;
  s_max_advertising_interval_slots = 0;
}

int GAP_LE_Advertising_Disable(unsigned int BluetoothStackID) {
  s_is_le_advertising_enabled = false;
  s_le_adv_connection_event_callback = NULL;
  s_le_adv_connection_callback_param = 0;
  s_min_advertising_interval_slots = 0;
  s_max_advertising_interval_slots = 0;
  return 0;
}

int GAP_LE_Advertising_Enable(unsigned int BluetoothStackID,
                              Boolean_t EnableScanResponse,
                              GAP_LE_Advertising_Parameters_t *GAP_LE_Advertising_Parameters,
                              GAP_LE_Connectability_Parameters_t *GAP_LE_Connectability_Parameters,
                              GAP_LE_Event_Callback_t GAP_LE_Event_Callback,
                              unsigned long CallbackParameter) {
  s_is_le_advertising_enabled = true;
  s_le_adv_connection_event_callback = GAP_LE_Event_Callback;
  s_le_adv_connection_callback_param = CallbackParameter;
  if (GAP_LE_Advertising_Parameters) {
    // Convert from ms to slots:
    s_min_advertising_interval_slots =
    (GAP_LE_Advertising_Parameters->Advertising_Interval_Min * 16) / 10;
    s_max_advertising_interval_slots =
    (GAP_LE_Advertising_Parameters->Advertising_Interval_Max * 16) / 10;
  } else {
    s_min_advertising_interval_slots = 0;
    s_max_advertising_interval_slots = 0;
  }
  return 0;
}

void gap_le_assert_advertising_interval(uint16_t expected_min_slots, uint16_t expected_max_slots) {
  cl_assert_equal_i(s_min_advertising_interval_slots, expected_min_slots);
  cl_assert_equal_i(s_max_advertising_interval_slots, expected_max_slots);
}

bool gap_le_is_advertising_enabled(void) {
  return s_is_le_advertising_enabled;
}

static Advertising_Data_t s_ad_data;
static unsigned int s_ad_data_length;

int GAP_LE_Set_Advertising_Data(unsigned int BluetoothStackID,
                                unsigned int Length,
                                Advertising_Data_t *Advertising_Data) {
  memcpy(&s_ad_data, Advertising_Data, Length);
  s_ad_data_length = Length;
  return 0;
}

unsigned int gap_le_get_advertising_data(Advertising_Data_t *ad_data_out) {
  *ad_data_out = s_ad_data;
  return s_ad_data_length;
}

static Scan_Response_Data_t s_scan_resp_data;
static unsigned int s_scan_resp_data_length;

int GAP_LE_Set_Scan_Response_Data(unsigned int BluetoothStackID,
                                  unsigned int Length,
                                  Scan_Response_Data_t *Scan_Response_Data) {
  memcpy(&s_scan_resp_data, Scan_Response_Data, Length);
  s_scan_resp_data_length = Length;
  return 0;
}

unsigned int gap_le_get_scan_response_data(Scan_Response_Data_t *scan_resp_data_out) {
  *scan_resp_data_out = s_scan_resp_data;
  return s_scan_resp_data_length;
}

static GAP_LE_Event_Callback_t s_le_create_connection_event_callback;
static unsigned long s_le_create_connection_callback_param;

int GAP_LE_Create_Connection(unsigned int BluetoothStackID,
                             unsigned int ScanInterval,
                             unsigned int ScanWindow,
                             GAP_LE_Filter_Policy_t InitiatorFilterPolicy,
                             GAP_LE_Address_Type_t RemoteAddressType,
                             BD_ADDR_t *RemoteDevice,
                             GAP_LE_Address_Type_t LocalAddressType,
                             GAP_LE_Connection_Parameters_t *ConnectionParameters,
                             GAP_LE_Event_Callback_t GAP_LE_Event_Callback,
                             unsigned long CallbackParameter) {

  s_le_create_connection_event_callback = GAP_LE_Event_Callback;
  s_le_create_connection_callback_param = CallbackParameter;
  return 0;
}

void prv_fake_gap_le_create_connection_event_put(GAP_LE_Event_Data_t *event) {
  cl_assert(s_le_create_connection_event_callback != NULL);
  s_le_create_connection_event_callback(1, event, s_le_create_connection_callback_param);
}

void prv_fake_gap_le_adv_connection_event_put(GAP_LE_Event_Data_t *event) {
  cl_assert(s_le_adv_connection_event_callback != NULL);
  s_le_adv_connection_event_callback(1, event, s_le_adv_connection_callback_param);
}

void fake_gap_put_connection_event(uint8_t status,
                                   bool is_master,
                                   const BTDeviceInternal *device) {
  GAP_LE_Connection_Complete_Event_Data_t event_data =
                                     (GAP_LE_Connection_Complete_Event_Data_t) {
    .Status = status,
    .Master = is_master,
    .Peer_Address_Type = device->is_random_address ? latRandom : latPublic,
    .Peer_Address = BTDeviceAddressToBDADDR(device->address),
  };
  GAP_LE_Event_Data_t event =  (GAP_LE_Event_Data_t) {
    .Event_Data_Type = etLE_Connection_Complete,
    .Event_Data_Size = sizeof(GAP_LE_Connection_Complete_Event_Data_t),
    .Event_Data.GAP_LE_Connection_Complete_Event_Data = &event_data,
  };
  if (is_master) {
    prv_fake_gap_le_create_connection_event_put(&event);
  } else {
    prv_fake_gap_le_adv_connection_event_put(&event);
  }
}


void fake_gap_put_disconnection_event(uint8_t status, uint8_t reason,
                                      bool is_master,
                                      const BTDeviceInternal *device) {
  GAP_LE_Disconnection_Complete_Event_Data_t event_data =
  (GAP_LE_Disconnection_Complete_Event_Data_t) {
    .Status = status,
    .Reason = reason,
    .Peer_Address_Type = device->is_random_address ? latRandom : latPublic,
    .Peer_Address = BTDeviceAddressToBDADDR(device->address),
  };
  GAP_LE_Event_Data_t event =  (GAP_LE_Event_Data_t) {
    .Event_Data_Type = etLE_Disconnection_Complete,
    .Event_Data_Size = sizeof(GAP_LE_Disconnection_Complete_Event_Data_t),
    .Event_Data.GAP_LE_Disconnection_Complete_Event_Data = &event_data,
  };
  if (is_master) {
    prv_fake_gap_le_create_connection_event_put(&event);
  } else {
    prv_fake_gap_le_adv_connection_event_put(&event);
  }
}


void fake_GAPAPI_put_encryption_change_event(bool encrypted, uint8_t status, bool is_master,
                                             const BTDeviceInternal *device) {
  GAP_LE_Encryption_Change_Event_Data_t event_data =
  (GAP_LE_Encryption_Change_Event_Data_t) {
    .BD_ADDR = BTDeviceAddressToBDADDR(device->address),
    .Encryption_Change_Status = status,
    .Encryption_Mode = encrypted ? emEnabled : emDisabled,
  };
  GAP_LE_Event_Data_t event =  (GAP_LE_Event_Data_t) {
    .Event_Data_Type = etLE_Encryption_Change,
    .Event_Data_Size = sizeof(GAP_LE_Encryption_Change_Event_Data_t),
    .Event_Data.GAP_LE_Encryption_Change_Event_Data = &event_data,
  };
  if (is_master) {
    prv_fake_gap_le_create_connection_event_put(&event);
  } else {
    prv_fake_gap_le_adv_connection_event_put(&event);
  }
}

int GAP_LE_Cancel_Create_Connection(unsigned int BluetoothStackID) {
  return 0;
}

// Puts the event that the BT Controller will emit after a succesfull
// GAP_LE_Cancel_Create_Connection call.
void fake_gap_le_put_cancel_create_event(const BTDeviceInternal *device, bool is_master) {
  fake_gap_put_connection_event(HCI_ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER,
                                is_master,
                                device);
}

int GAP_LE_Disconnect(unsigned int BluetoothStackID,
                      BD_ADDR_t BD_ADDR) {
  return 0;
}

int GAP_LE_Pair_Remote_Device(unsigned int BluetoothStackID,
                              BD_ADDR_t BD_ADDR,
                              GAP_LE_Pairing_Capabilities_t *Capabilities,
                              GAP_LE_Event_Callback_t GAP_LE_Event_Callback,
                              unsigned long CallbackParameter) {
  return 0;
}

// -------------------------------------------------------------------------------------------------
// Bluetopia's Security Manager API

int GAP_LE_Authentication_Response(unsigned int BluetoothStackID, BD_ADDR_t BD_ADDR,
                  GAP_LE_Authentication_Response_Information_t *GAP_LE_Authentication_Information) {
  return 0;
}

int GAP_LE_Diversify_Function(unsigned int BluetoothStackID, Encryption_Key_t *Key, Word_t DIn,
                              Word_t RIn, Encryption_Key_t *Result) {
  return 0;
}

int GAP_LE_Generate_Long_Term_Key(unsigned int BluetoothStackID, Encryption_Key_t *DHK,
                                  Encryption_Key_t *ER, Long_Term_Key_t *LTK_Result,
                                  Word_t *DIV_Result, Word_t *EDIV_Result,
                                  Random_Number_t *Rand_Result) {
  return 0;
}

static BD_ADDR_t s_encrypted_device;

int GAP_LE_Query_Encryption_Mode(unsigned int BluetoothStackID, BD_ADDR_t BD_ADDR,
                                 GAP_Encryption_Mode_t *GAP_Encryption_Mode) {
  *GAP_Encryption_Mode = (COMPARE_BD_ADDR(s_encrypted_device, BD_ADDR)) ? emEnabled : emDisabled;
  return 0;
}

void fake_GAPAPI_set_encrypted_for_device(const BTDeviceInternal *device) {
  s_encrypted_device = BTDeviceAddressToBDADDR(device->address);
}

int GAP_LE_Regenerate_Long_Term_Key(unsigned int BluetoothStackID, Encryption_Key_t *DHK,
                                    Encryption_Key_t *ER, Word_t EDIV, Random_Number_t *Rand,
                                    Long_Term_Key_t *LTK_Result) {
  return 0;
}

int GAP_LE_Register_Remote_Authentication(unsigned int BluetoothStackID, GAP_LE_Event_Callback_t GAP_LE_Event_Callback, unsigned long CallbackParameter) {
  return 0;
}

int GAP_LE_Un_Register_Remote_Authentication(unsigned int BluetoothStackID) {
  return 0;
}

int GAP_LE_Request_Security(unsigned int BluetoothStackID, BD_ADDR_t BD_ADDR,
                            GAP_LE_Bonding_Type_t Bonding_Type, Boolean_t MITM,
                            GAP_LE_Event_Callback_t GAP_LE_Event_Callback,
                            unsigned long CallbackParameter) {
  return 0;
}

int GAP_LE_Set_Pairability_Mode(unsigned int BluetoothStackID,
                                GAP_LE_Pairability_Mode_t PairableMode) {
  return 0;
}

int GAP_LE_Generate_Resolvable_Address(unsigned int BluetoothStackID, Encryption_Key_t *IRK,
                                       BD_ADDR_t *ResolvableAddress_Result) {
  return 0;
}

int GAP_LE_Set_Random_Address(unsigned int BluetoothStackID, BD_ADDR_t RandomAddress) {
  return 0;
}

int GAP_Query_Local_BD_ADDR(unsigned int BluetoothStackID, BD_ADDR_t *BD_ADDR) {
  return 0;
}

static const Encryption_Key_t s_fake_irk = {
  0xaa,
  0xaa,
};

static const BD_ADDR_t s_resolving_bd_addr = {
  0xaa, 0xff, 0xff, 0xff, 0xff,
  0x7f /* 6th byte: bit 6 set, bit 7 unset indicates "resolvable private address" */
};

static const BD_ADDR_t s_not_resolving_bd_addr = {
  0xff,
};

const Encryption_Key_t *fake_GAPAPI_get_fake_irk(void) {
  return &s_fake_irk;
}

const BD_ADDR_t *fake_GAPAPI_get_bd_addr_not_resolving_to_fake_irk(void) {
  return &s_not_resolving_bd_addr;
}

const BTDeviceInternal *fake_GAPAPI_get_device_not_resolving_to_fake_irk(void) {
  static BTDeviceInternal s_not_resolving_device;
  s_not_resolving_device = (const BTDeviceInternal) {
    .address = BDADDRToBTDeviceAddress(s_not_resolving_bd_addr),
    .is_random_address = true,
  };
  return &s_not_resolving_device;
}

const BD_ADDR_t *fake_GAPAPI_get_bd_addr_resolving_to_fake_irk(void) {
  return &s_resolving_bd_addr;
}

const BTDeviceInternal *fake_GAPAPI_get_device_resolving_to_fake_irk(void) {
  static BTDeviceInternal s_resolving_device;
  s_resolving_device = (const BTDeviceInternal) {
    .address = BDADDRToBTDeviceAddress(s_resolving_bd_addr),
    .is_random_address = true,
  };
  return &s_resolving_device;
}

Boolean_t GAP_LE_Resolve_Address(unsigned int BluetoothStackID, Encryption_Key_t *IRK,
                                 BD_ADDR_t ResolvableAddress) {
  return COMPARE_BD_ADDR(ResolvableAddress, s_resolving_bd_addr) &&
         COMPARE_ENCRYPTION_KEY(*IRK, s_fake_irk);
}

void fake_GAPAPI_init(void) {
  memset(&s_encrypted_device, 0, sizeof(s_encrypted_device));
  s_is_le_advertising_enabled = false;
  s_le_adv_connection_event_callback = NULL;
  s_le_adv_connection_callback_param = 0;
  memset(&s_ad_data, 0, sizeof(s_ad_data));
  s_ad_data_length = 0;
  s_le_create_connection_event_callback = NULL;
  s_le_create_connection_callback_param = 0;
  memset(&s_scan_resp_data, 0, sizeof(s_scan_resp_data));
  s_scan_resp_data_length = 0;
}
