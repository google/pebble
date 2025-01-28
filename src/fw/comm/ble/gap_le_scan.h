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

#include <stdbool.h>

//! @internal
//! The number of reports that the circular reports buffer can contain.
//! Accomodate for 4 reports with advertisement and scan response data:
#define GAP_LE_SCAN_REPORTS_BUFFER_SIZE (4 * (sizeof(GAPLERawAdReport) + \
(2 * GAP_LE_AD_REPORT_DATA_MAX_LENGTH)))

//! @internal
//! This is a semi-processed advertisement report. It is "raw" in the sense that
//! the payload is not parsed. We use the unparsed payload to make it easier to
//! stuff into a circular buffer.
typedef struct {
  //! Is the advertiser's address a public address or random address?
  bool is_random_address:1;
  uint8_t rsvd:7; // free for use

  //! The address of the advertiser
  BTDeviceInternal address;

  //! Received signal strength indication
  int8_t rssi;

  //! The raw advertisement data, concatenated with the raw scan response data.
  //! This will be parsed later down the road.
  BLEAdData payload;
} GAPLERawAdReport;

//! @internal
//! Starts scanning for advertising reports and performs scan requests when
//! possible. Duplicates are filtered to avoid flooding the system. Advertising
//! reports and scan responses will be buffered. A PEBBLE_BLE_SCAN_EVENT will
//! be generated when there is data to be collected.
//! @see gap_le_consume_scan_results
//! @return 0 if scanning started succesfully or an error code otherwise.
bool gap_le_start_scan(void);

//! @internal
//! Stops scanning.
//! @return 0 if scanning stopped succesfully or an error code otherwise.
bool gap_le_stop_scan(void);

//! @internal
//! @return true if the controller is currently scanning, or false otherwise.
bool gap_le_is_scanning(void);

//! @internal
//! Copies the number of reports that have been collected.
//! @param[out] buffer into which the reports should be copied.
//! @param[in,out] size_in_out In: The number of bytes the buffer can hold.
//! It must be a valid address and not be NULL. Out: Number of copied bytes.
//! @return true if there were more reports to be copied, false if all buffered
//! reports have been copied.
bool gap_le_consume_scan_results(uint8_t *buffer, uint16_t *size_in_out);

//! @internal
//! Initializes the static state for this module and creates anything it needs
//! to function.
void gap_le_scan_init(void);

//! @internal
//! Stops any ongoing scanning and related activities and cleans up anything that
//! had been created by gap_le_scan_init()
void gap_le_scan_deinit(void);
