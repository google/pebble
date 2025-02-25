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

#include <stdbool.h>

#include "services/common/firmware_update.h"

#include <util/attributes.h>

typedef enum SystemMessageType {
  SysMsgFirmwareAvailable_Deprecated = 0x00,
  SysMsgFirmwareStart = 0x01,
  SysMsgFirmwareComplete = 0x02,
  SysMsgFirmwareFail = 0x03,
  SysMsgFirmwareUpToDate = 0x04,
  // SysMsgFirmwareOutOfDate = 0x05, DEPRECATED
  SysMsgReconnectRequestStop = 0x06,
  SysMsgReconnectRequestStart = 0x07,
  SysMsgMAPRetry = 0x08,  // MAP is no longer used
  SysMsgMAPConnected = 0x09,  // MAP is no longer used
  SysMsgFirmwareStartResponse = 0x0a,
  SysMsgFirmwareStatus = 0x0b, // Phone -> Watch request for partial fw install info
  SysMsgFirmwareStatusResponse = 0x0c, // Watch -> Phone response of what fw is partially installed
} SystemMessageType;

typedef struct PACKED SysMsgSmoothFirmwareStartPayload {
  uint8_t deprecated; // not used anymore but all messages start with 0x0
  SystemMessageType type:8; // == SysMsgFirmwareStart
  // The number of bytes the phone has transferred in a previous operation
  uint32_t bytes_already_transferred;
  // The total number of bytes the phone needs to transfer to complete the firmware update
  // (i.e For a normal firmware, this would be the sum of outstanding bytes for the fw binary
  // and also the pbpack)
  uint32_t bytes_to_transfer;
} SysMsgSmoothFirmwareStartPayload;

void system_message_init(void);

void system_message_send(SystemMessageType type);

void system_message_send_firmware_start_response(FirmwareUpdateStatus status);

