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
#include <stdint.h>

#include "applib/accel_service.h"
#include "applib/compass_service.h"
#include "applib/preferred_content_size.h"
#include "drivers/button_id.h"
#include "util/attributes.h"

// The QEMU protocols implemented
typedef enum {
  QemuProtocol_SPP = 1,
  QemuProtocol_Tap = 2,
  QemuProtocol_BluetoothConnection = 3,
  QemuProtocol_Compass = 4,
  QemuProtocol_Battery = 5,
  QemuProtocol_Accel = 6,
  QemuProtocol_Vibration = 7,
  QemuProtocol_Button = 8,
  QemuProtocol_TimeFormat = 9,
  QemuProtocol_TimelinePeek = 10,
  QemuProtocol_ContentSize = 11,
} QemuProtocol;


// ---------------------------------------------------------------------------------------
// Structure of the data for various protocols

// For QemuProtocol_SPP, the data is raw Pebble Protocol

// QemuProtocol_Tap
typedef struct PACKED {
  uint8_t axis;              // 0: x-axis, 1: y-axis, 2: z-axis
  int8_t direction;         // either +1 or -1
} QemuProtocolTapHeader;


// QemuProtocol_BluetoothConnection
typedef struct PACKED {
  uint8_t connected;         // true if connected
} QemuProtocolBluetoothConnectionHeader;


// QemuProtocol_Compass
typedef struct PACKED {
  uint32_t magnetic_heading;      // 0x10000 represents 360 degrees
  CompassStatus calib_status:8;   // CompassStatus enum
} QemuProtocolCompassHeader;


// QemuProtocol_Battery
typedef struct PACKED {
  uint8_t battery_pct;            // from 0 to 100
  uint8_t charger_connected;
} QemuProtocolBatteryHeader;


// QemuProtocol_Accel request (to Pebble)
typedef struct PACKED {
  uint8_t     num_samples;
  AccelRawData samples[0];
} QemuProtocolAccelHeader;

// QemuProtocol_Accel response (back to host)
typedef struct PACKED {
  uint16_t     avail_space;   // Number of samples we can accept
} QemuProtocolAccelResponseHeader;


// QemuProtocol_Vibration notification (sent from Pebble to host)
typedef struct PACKED {
  uint8_t     on;             // non-zero if vibe is on, 0 if off
} QemuProtocolVibrationNotificationHeader;


// QemuProtocol_Button
typedef struct PACKED {
  // New button state. Bit x specifies the state of button x, where x is one of the
  // ButtonId enum values.
  uint8_t     button_state;
} QemuProtocolButtonHeader;


// QemuProtocol_TimeFormat
typedef struct PACKED {
  uint8_t is_24_hour; // non-zero if 24h format, 0 if 12h format
} QemuProtocolTimeFormatHeader;


// QemuProtocol_TimelinePeek
typedef struct PACKED {
  //! Decides whether the Timeline Peek will show. Timeline Peek will animate only when this state
  //! toggles, and subsequent interactions that manipulate Timeline Peek outside of this
  //! QemuProtocol packet apply without an animation. The state received by this packet is also
  //! persisted, for example if enabled is true, exiting the watchface will instantly hide the
  //! peek, but returning to the watchface will instantly show the peek since this state persists.
  bool enabled;
} QemuProtocolTimelinePeekHeader;


// QemuProtocol_ContentSize
typedef struct PACKED {
  //! New system content size.
  uint8_t size;
} QemuProtocolContentSizeHeader;
#if !UNITTEST
_Static_assert(sizeof(PreferredContentSize) == sizeof(((QemuProtocolContentSizeHeader *)0)->size),
               "sizeof(PreferredContentSize) grew, need to update QemuContentSize in libpebble2 !");
#endif

// ---------------------------------------------------------------------------------------
// API
void qemu_serial_init(void);

void qemu_serial_send(QemuProtocol protocol, const uint8_t *data, uint32_t len);
