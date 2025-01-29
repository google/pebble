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

#include "drivers/accessory.h"
#include "drivers/rtc.h"
#include "kernel/pebble_tasks.h"
#include "services/normal/accessory/smartstrap_comms.h"
#include "services/normal/accessory/smartstrap_link_control.h"
#include "services/normal/accessory/smartstrap_profiles.h"
#include "services/normal/accessory/smartstrap_state.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"

#include <inttypes.h>
#include <time.h>

#define LINK_CONTROL_VERSION              1
#define TIMEOUT_MS                        100
#define MAX_DATA_LENGTH                   4
#define MAX_FRAME_LENGTH                  (sizeof(FrameHeader) + MAX_DATA_LENGTH)
//! The number of consecutive invalid link control responses before we try to reconnect
#define MAX_STRIKES                       3
//! How often we'll go without some valid data from the smartstrap before sending a status message
//! and disconnecting if the smartstrap doesn't reply. This is in seconds.
#define STATUS_CHECK_INTERVAL             5
//! The minimum number of seconds between connection requests to avoid spamming the smartstrap.
#define MIN_CONNECTION_REQUEST_INTERVAL   1
//! The minimum number of seconds between status requests to avoid spamming the smartstrap
#define MIN_STATUS_REQUEST_INTERVAL       5

typedef enum {
  LinkControlTypeInvalid = 0,
  LinkControlTypeStatus = 1,
  LinkControlTypeProfiles = 2,
  LinkControlTypeBaud = 3,
  NumLinkControlTypes
} LinkControlType;

typedef enum {
  LinkControlStatusOk = 0,
  LinkControlStatusBaudRate = 1,
  LinkControlStatusDisconnect = 2
} LinkControlStatus;

static const AccessoryBaud BAUDS[] = {
  AccessoryBaud9600,
  AccessoryBaud14400,
  AccessoryBaud19200,
  AccessoryBaud28800,
  AccessoryBaud38400,
  AccessoryBaud57600,
  AccessoryBaud62500,
  AccessoryBaud115200,
  AccessoryBaud125000,
  AccessoryBaud230400,
  AccessoryBaud250000,
  AccessoryBaud460800
};
_Static_assert(ARRAY_LENGTH(BAUDS) - 1 == AccessoryBaud460800, "BAUDS doesn't match AccessoryBaud");

typedef struct {
  uint8_t version;
  LinkControlType type:8;
  uint8_t data[];
} FrameHeader;

//! store supported profiles as a series of bits
static uint32_t s_profiles;
_Static_assert(sizeof(s_profiles) * 8 >= NumSmartstrapProfiles, "s_profiles is too small");
//! MBuf used for receiving
static MBuf *s_read_mbuf;
static uint8_t s_read_data[MAX_FRAME_LENGTH];
//! The type of the most recent link control message which was sent
static LinkControlType s_type;
//! Number of consecutive bad status message responses received from the smartstrap
static int s_strikes;


static void prv_do_send(LinkControlType type) {
  FrameHeader header = (FrameHeader) {
    .version = LINK_CONTROL_VERSION,
    .type = type
  };
  s_type = type;
  MBuf send_mbuf = MBUF_EMPTY;
  mbuf_set_data(&send_mbuf, &header, sizeof(header));
  PBL_ASSERTN(!s_read_mbuf);
  s_read_mbuf = mbuf_get(s_read_data, MAX_FRAME_LENGTH, MBufPoolSmartstrap);
  SmartstrapResult result = smartstrap_send(SmartstrapProfileLinkControl, &send_mbuf, s_read_mbuf,
                                            TIMEOUT_MS);
  if (result != SmartstrapResultOk) {
    mbuf_free(s_read_mbuf);
    s_read_mbuf = NULL;
    PBL_LOG(LOG_LEVEL_WARNING, "Sending of link control message failed: result=%d, type=%d", result,
            type);
    smartstrap_link_control_disconnect();
  }
}

static void prv_fatal_error_strike(void) {
  s_strikes++;
  PBL_LOG(LOG_LEVEL_WARNING, "Fatal error strike %d", s_strikes);
  if (s_strikes >= MAX_STRIKES) {
    // out of strikes
    smartstrap_link_control_disconnect();
  }
}

static bool prv_read_complete(bool success, uint32_t length) {
  const FrameHeader *header = mbuf_get_data(s_read_mbuf);
  mbuf_free(s_read_mbuf);
  s_read_mbuf = NULL;
  const uint32_t data_length = length - sizeof(FrameHeader);
  if (!success ||
      (length < sizeof(FrameHeader)) ||
      (data_length > MAX_DATA_LENGTH) ||
      (header->type != s_type) ||
      (header->version != LINK_CONTROL_VERSION)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Invalid link control response (type=%d).", s_type);
    if (s_type == LinkControlTypeStatus) {
      prv_fatal_error_strike();
    } else if (!s_profiles) {
      smartstrap_link_control_disconnect();
    }
    return false;
  }
  s_strikes = 0;

  if (header->type == LinkControlTypeStatus) {
    // status message
    const LinkControlStatus status = header->data[0];
    if (status == LinkControlStatusOk) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Got link control status: Ok");
      smartstrap_connection_state_set(true);
    } else if (status == LinkControlStatusBaudRate) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Got link control status: Baud rate");
      prv_do_send(LinkControlTypeBaud);
    } else if (status == LinkControlStatusDisconnect) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Got link control status: Disconnect");
      smartstrap_link_control_disconnect();
    } else {
      PBL_LOG(LOG_LEVEL_WARNING, "Got link control status: INVALID (%d)", status);
      smartstrap_link_control_disconnect();
      success = false;
    }
  } else if (header->type == LinkControlTypeProfiles) {
    // profiles message
    if ((data_length % sizeof(uint16_t)) == 0) {
      s_profiles = 0;
      uint16_t *profiles = (uint16_t *)header->data;
      const uint32_t num_profiles = data_length / sizeof(uint16_t);
      for (uint32_t i = 0; i < num_profiles; i++) {
        if ((profiles[i] > SmartstrapProfileInvalid) &&
            (profiles[i] < NumSmartstrapProfiles) &&
            (profiles[i] != SmartstrapProfileLinkControl)) {
          // handle the valid profile
          s_profiles |= (1 << profiles[i]);
        }
      }
      if (s_profiles == 0) {
        PBL_LOG(LOG_LEVEL_WARNING, "No profiles specified");
        smartstrap_link_control_disconnect();
        success = false;
      } else {
        prv_do_send(LinkControlTypeStatus);
      }
    } else {
      // length is invalid (should be an even multiple of the size of the profile value)
      PBL_LOG(LOG_LEVEL_WARNING, "Got invalid profiles length (%"PRIu32")", data_length);
      smartstrap_link_control_disconnect();
      success = false;
    }
  } else if (header->type == LinkControlTypeBaud) {
    // new baud rate
    const uint8_t requested_baud = header->data[0];
    if (requested_baud >= ARRAY_LENGTH(BAUDS)) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Invalid baud rate (%"PRIu8")", requested_baud);
      smartstrap_link_control_disconnect();
      success = false;
    } else {
      accessory_set_baudrate(BAUDS[requested_baud]);
      prv_do_send(LinkControlTypeStatus);
    }
  } else {
    PBL_LOG(LOG_LEVEL_DEBUG, "Invalid response type (%d)", header->type);
    smartstrap_link_control_disconnect();
    success = false;
  }
  return success;
}

void smartstrap_link_control_connect(void) {
  prv_do_send(LinkControlTypeProfiles);
  accessory_set_baudrate(AccessoryBaud9600);
}

void smartstrap_link_control_disconnect(void) {
  s_strikes = 0;
  s_profiles = 0;
  accessory_set_baudrate(AccessoryBaud9600);
  smartstrap_connection_state_set(false);
}

bool smartstrap_link_control_is_profile_supported(SmartstrapProfile profile) {
  PBL_ASSERTN((profile > SmartstrapProfileInvalid) && (profile < NumSmartstrapProfiles));
  return !!(s_profiles & (1 << profile));
}

static bool prv_send_control(void) {
  static time_t s_last_connection_request_time = 0;
  static time_t s_last_status_check_time = 0;
  const time_t current_time = rtc_get_time();
  if (!smartstrap_is_connected() && accessory_is_present() &&
      (smartstrap_fsm_state_get() == SmartstrapStateReadReady)) {
    if (current_time > s_last_connection_request_time + MIN_CONNECTION_REQUEST_INTERVAL) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Smartstrap detected - attempting to connect.");
      s_last_connection_request_time = current_time;
      smartstrap_link_control_connect();
    }
    return true;
  } else if (smartstrap_connection_get_time_since_valid_data() > STATUS_CHECK_INTERVAL) {
    if (current_time > s_last_status_check_time + MIN_STATUS_REQUEST_INTERVAL) {
      // send a status message
      prv_do_send(LinkControlTypeStatus);
      if (accessory_bus_contention_detected()) {
        PBL_LOG(LOG_LEVEL_WARNING, "Bus contention while sending status message");
        // Count bus contention as a strike as it could be that the accessory is disconnected
        // or misbehaving.
        prv_fatal_error_strike();
      }
      s_last_status_check_time = current_time;
    }
    return true;
  }
  return false;
}

static void prv_read_aborted(void) {
  mbuf_free(s_read_mbuf);
  s_read_mbuf = NULL;
}

const SmartstrapProfileInfo *smartstrap_link_control_get_info(void) {
  static const SmartstrapProfileInfo s_link_control_info = {
    .profile = SmartstrapProfileLinkControl,
    .read_complete = prv_read_complete,
    .control = prv_send_control,
    .read_aborted = prv_read_aborted,
  };
  return &s_link_control_info;
}
