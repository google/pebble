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

#include "kernel/pebble_tasks.h"
#include "util/mbuf.h"

#include <stdint.h>

//! The currently-supported Smartstrap profiles
typedef enum {
  SmartstrapProfileInvalid = 0,
  SmartstrapProfileLinkControl = 1,
  SmartstrapProfileRawData = 2,
  SmartstrapProfileGenericService = 3,
  NumSmartstrapProfiles,
} SmartstrapProfile;

typedef struct {
  uint16_t service_id;
  uint16_t attribute_id;
  MBuf *write_mbuf;
  MBuf *read_mbuf;
  uint16_t timeout_ms;
} SmartstrapRequest;

typedef void (*SmartstrapProfileInitHandler)(void);
typedef void (*SmartstrapProfileConnectedHandler)(bool connected);
typedef SmartstrapResult (*SmartstrapProfileSendHandler)(const SmartstrapRequest *request);
typedef bool (*SmartstrapProfileReadCompleteHandler)(bool success, uint32_t length);
typedef void (*SmartstrapProfileReadAbortedHandler)(void);
typedef void (*SmartstrapProfileNotifyHandler)(void);
typedef bool (*SmartstrapProfileSendControlHandler)(void);

typedef struct {
  //! The profile this info applies to
  SmartstrapProfile profile;
  //! The maximum number of services which a smartstrap may support for this profile
  uint8_t max_services;
  //! The loweest service id which this profile supports
  uint16_t min_service_id;
  //! Optional handler for initialization
  SmartstrapProfileInitHandler init;
  //! Optional handler for connection changes
  SmartstrapProfileConnectedHandler connected;
  //! Required handler for sending requests
  SmartstrapProfileSendHandler send;
  //! Required handler for completed read requests
  SmartstrapProfileReadCompleteHandler read_complete;
  //! Optional handler for aborted requests (NOTE: called from a critical region)
  SmartstrapProfileReadAbortedHandler read_aborted;
  //! Optional handler for notifications
  SmartstrapProfileNotifyHandler notify;
  //! Optional handler to send any pending control messages
  SmartstrapProfileSendControlHandler control;
} SmartstrapProfileInfo;

typedef const SmartstrapProfileInfo *(*SmartstrapProfileGetInfoFunc)(void);

// generate function prototypes for profile info functions
#define REGISTER_SMARTSTRAP_PROFILE(f) const SmartstrapProfileInfo *f(void);
#include "services/normal/accessory/smartstrap_profile_registry.def"
#undef REGISTER_SMARTSTRAP_PROFILE

void smartstrap_profiles_init(void);

//! Make a smartstrap request
SmartstrapResult smartstrap_profiles_handle_request(const SmartstrapRequest *request);

//! Handle a smartstrap read (either complete frame or timeout)
void smartstrap_profiles_handle_read(bool success, SmartstrapProfile profile, uint32_t length);

//! Handle an aborted (canceled) read request
void smartstrap_profiles_handle_read_aborted(SmartstrapProfile profile);

//! Handle a smartstrap notification (either valid context frame or timeout)
void smartstrap_profiles_handle_notification(bool success, SmartstrapProfile profile);

//! Handle a connection event
void smartstrap_profiles_handle_connection_event(bool connected);

//! Goes through the profiles in order and allows them to send control messages. Returns true once
//! one of them sends something (or false if none of them send anything).
bool smartstrap_profiles_send_control(void);

//! Returns the maximum number of services supported across all the profiles.
unsigned int smartstrap_profiles_get_max_services(void);
