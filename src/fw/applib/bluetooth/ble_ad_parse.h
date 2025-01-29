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

//! @file ble_ad_parse.h
//! API to serialize and deserialize advertisement and scan response payloads.
//!
//! Inbound payloads, as received using the ble_scan.h public API, can be
//! consumed/deserialized using the functions below.
//!
//! Outbound payloads can be created/serialized and then advertised using the
//! gap_le_advert.h functions. At the moment, there is no public API.

// -----------------------------------------------------------------------------
// Consuming BLEAdData:
// -----------------------------------------------------------------------------

//! Searching the advertisement data to check whether a given service UUID is
//! included.
//! @param ad The advertisement data
//! @param service_uuid The UUID of the service to look for
//! @return true if the service UUID was found, false if not
bool ble_ad_includes_service(const BLEAdData *ad, const Uuid *service_uuid);

//! If present, copies the Service UUIDs from the advertisement data.
//! @param ad The advertisement data
//! @param[out] uuids_out An array of Uuid`s into which the found Service UUIDs
//! will be copied.
//! @param num_uuids The size of the uuids_out array.
//! @return The total number of found Service UUIDs. This might be a larger
//! number than num_uuids, if the passed array was not large enough to hold all
//! the UUIDs.
//! @note All UUIDs from advertisement data will be converted to their 128-bit
//! equivalents using the Bluetooth Base UUID using bt_uuid_expand_16bit or
//! bt_uuid_expand_32bit.
//! @see ble_ad_get_number_of_service_uuids
uint8_t ble_ad_copy_service_uuids(const BLEAdData *ad,
                                  Uuid *uuids_out,
                                  uint8_t num_uuids);

//! If present, returns the number of Service UUIDs the advertisement data
//! contains.
//! @param ad The advertisement data
//! @return If Service UUIDs data is present, the number of UUIDs is contains,
//! or zero otherwise.
uint8_t ble_ad_get_number_of_service_uuids(const BLEAdData *ad);

//! If present, gets the TX Power Level from the advertisement data.
//! @param ad The advertisement data
//! @param[out] tx_power_level_out Will contain the TX Power Level if the return
//! value is true.
//! @return true if the TX Power Level was found and assigned.
bool ble_ad_get_tx_power_level(const BLEAdData *ad, int8_t *tx_power_level_out);

//! If present, copies the Local Name from the advertisement data.
//! If the Local Name is bigger than the size of the buffer, only the part that
//! fits will be copied. For convenience, the copied c-string will always be
//! zero terminated for you.
//! @param ad The advertisement data
//! @param buffer The buffer into which to copy the Local Name, if found.
//! @param size The size of the buffer
//! @return The size of the Local Name in bytes, *including* zero terminator.
//! Note that this might be more than the size of the provided buffer.
//! If the Local Name was not found, the return value will be zero.
size_t ble_ad_copy_local_name(const BLEAdData *ad,
                              char *buffer, size_t size);

//! If the Local Name is present in the advertisement data, returns the number
//! of bytes a C-string needs to be to hold the full name.
//! @param ad The advertisement data
//! @return The size of the Local Name in bytes, *including* zero terminator.
//! If the Local Name is not present, zero is returned.
size_t ble_ad_get_local_name_buffer_size(const BLEAdData *ad);

//! If present, copies the Manufacturer Specific data from the advertisement
//! data. If the provided buffer is smaller than the size of the data, only
//! the data up that fits the buffer will be copied.
//! @param ad The advertisement data
//! @param[out] company_id_out Out: The Company Identifier Code, see
//! https://www.bluetooth.org/en-us/specification/assigned-numbers/company-identifiers
//! This will only get written, if there was Manufacturer Specific data in the
//! advertisement. In case you are not interested in getting the company ID,
//! NULL can be passed in.
//! @param buffer The buffer into which to copy the Manufacturer Specific Data,
//! if found.
//! @param size The size of the buffer
//! @return The size of the Manufacturer Specific data in bytes. Note that this
//! can be larger than the size of the provided buffer. If the Manufacturer
//! Specific data was not found, the return value will be zero.
//! @see ble_ad_get_manufacturer_specific_data_size
size_t ble_ad_copy_manufacturer_specific_data(const BLEAdData *ad,
                                              uint16_t *company_id_out,
                                              uint8_t *buffer, size_t size);

//! Gets the size in bytes of Manufacturer Specific data in the advertisement.
//! @param ad The advertisement data
//! @return The size of the data, in bytes. If the Manufacturer Specific data is
//! not present, zero is returned.
size_t ble_ad_get_manufacturer_specific_data_size(const BLEAdData *ad);

//! Gets the size in bytes of the raw advertisement and scan response data.
//! @param ad The advertisement data
//! @return The size of the raw advertisement and scan response data in bytes.
size_t ble_ad_get_raw_data_size(const BLEAdData *ad);

//! Copies the raw bytes of advertising and scan response data into a buffer.
//! If there was scan response data, it will be concatenated directly after the
//! advertising data.
//! @param ad The advertisement data
//! @param buffer The buffer into which to copy the raw bytes
//! @param size The size of the buffer
//! @return The number of bytes copied.
size_t ble_ad_copy_raw_data(const BLEAdData *ad, uint8_t *buffer, size_t size);


// -----------------------------------------------------------------------------
// Creating BLEAdData:
// -----------------------------------------------------------------------------

//! Creates a blank, mutable advertisement and scan response payload.
//! It can contain up to 31 bytes of advertisement data and up to 31 bytes of
//! scan response data. The underlying storage for this is automatically
//! allocated.
//! @note When adding elements to the BLEAdData, using the ble_ad_set_...
//! functions, elements will be added to the advertisement data part first.
//! If the element to add does not fit, the scan response is automatically
//! used. Added elements cannot span across the two parts.
//! @return The blank payload object.
//! @note BLEAdData created with this function must be destroyed at some point
//! in time using ble_ad_destroy()
//! @note Use the ble_ad_set... functions to write data into the object. The
//! data written to it will occupy the advertisement payload until there is not
//! enough space left, in which case all following data is written into the scan
//! response. @see ble_ad_start_scan_response()
BLEAdData* ble_ad_create(void);

//! Destroys an advertisement payload that was created earlier with
//! ble_ad_create().
void ble_ad_destroy(BLEAdData *ad);

//! Marks the start of the scan response and finalizes the advertisement
//! payload. This forces successive writes to be written to the scan response,
//! even though it would have fit into the advertisement payload.
void ble_ad_start_scan_response(BLEAdData *ad_data);

//! Writes the Service UUID list to the advertisement or scan response payload.
//! The list is assumed to be the complete list of Service UUIDs.
//! @param ad The advertisement payload as created earlier by ble_ad_create()
//! @param uuids Array of UUIDs to add to the list. If the UUIDs are all derived
//! from the Bluetooth SIG base UUID, this function will automatically use
//! a smaller Service UUID size if possible.
//! @see bt_uuid_expand_16bit
//! @see bt_uuid_expand_32bit
//! @param num_uuids Number of UUIDs in the uuids array.
//! @return true if the data was successfully written or false if not.
bool ble_ad_set_service_uuids(BLEAdData *ad,
                              const Uuid uuids[], uint8_t num_uuids);

//! Writes the Local Name to the advertisement or scan response payload.
//! @param ad The advertisement payload as created earlier by ble_ad_create()
//! @param local_name Zero terminated, UTF-8 string with the Local Name. The
//! name is assumed to be complete and not abbreviated.
//! @return true if the data was successfully written or false if not.
bool ble_ad_set_local_name(BLEAdData *ad,
                           const char *local_name);

//! Writes the TX Power Level to advertisement or scan response payload.
//! The actual transmission power level value is set automatically, based on the
//! value as used by the Bluetooth hardware.
//! @param ad The advertisement payload as created earlier by ble_ad_create()
//! @return true if the data was successfully written or false if not.
bool ble_ad_set_tx_power_level(BLEAdData *ad);

//! Writes Manufacturer Specific Data to advertisement or scan response payload.
//! @param ad The advertisement payload as created earlier by ble_ad_create()
//! @param company_id The Company Identifier Code, see
//! https://www.bluetooth.org/en-us/specification/assigned-numbers/company-identifiers
//! @param data The data
//! @param size The size of data in bytes
//! @return true if the data was successfully written or false if not.
bool ble_ad_set_manufacturer_specific_data(BLEAdData *ad,
                                           uint16_t company_id,
                                           const uint8_t *data, size_t size);

//! @internal -- Do not export
//! Writes the Flags AD Type to the advertisement or scan response payload.
//! @param ad The advertisement payload as created earlier by ble_ad_create()
//! @param flags The flags to write. See Core_v4.0.pdf Vol 3, Appendix C, 18.1.
//! @return true if the data was successfully written or false if not.
bool ble_ad_set_flags(BLEAdData *ad, uint8_t flags);
