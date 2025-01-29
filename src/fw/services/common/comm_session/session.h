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

#include "comm/bt_conn_mgr.h"

//! CommSession represents a Pebble Protocol communication session. It attempts to abstract away the
//! differences in the underlying data Transport types (iAP over SPP for iOS, plain SPP for Android,
//! PPoGATT for BLE, QEMU ...)
//! There are two types of sessions: the system session and the app session.
//! The system session must be used when communicating to the Pebble app. There can only be one
//! system session at a time. On Android, the system session uses a "hybrid" transport, which means
//! that it also connects to PebbleKit apps (via the Pebble Android app).
//! With iAP/PPoGATT, an app session is a dedicated Pebble Protocol session, connecting directly to
//!  a 3rd party phone app. With PPoGATT, there can be multiple transports and thus multiple app
//! sessions at a time. With iAP transport, it's different. There can only be one iAP-based app
//! session.

typedef struct CommSession CommSession;

typedef enum {
  CommSessionTypeInvalid = -1,
  CommSessionTypeSystem = 0,
  CommSessionTypeApp = 1,
  NumCommSessions,
} CommSessionType;

// Note: The FW packs the capabilities it supports in the PebbleProtocolCapabilities struct
typedef enum {
  CommSessionRunState = 1 << 0,
  CommSessionInfiniteLogDumping = 1 << 1,
  CommSessionExtendedMusicService = 1 << 2,
  CommSessionExtendedNotificationService = 1 << 3,
  CommSessionLanguagePackSupport = 1 << 4,
  CommSessionAppMessage8kSupport = 1 << 5,
  CommSessionActivityInsightsSupport = 1 << 6,
  CommSessionVoiceApiSupport = 1 << 7,
  CommSessionSendTextSupport = 1 << 8,
  CommSessionNotificationFilteringSupport = 1 << 9,
  CommSessionUnreadCoredumpSupport = 1 << 10,
  CommSessionWeatherAppSupport = 1 << 11,
  CommSessionRemindersAppSupport = 1 << 12,
  CommSessionWorkoutAppSupport = 1 << 13,
  CommSessionSmoothFwInstallProgressSupport = 1 << 14,
  CommSessionOutOfRange
} CommSessionCapability;

#define COMM_SESSION_DEFAULT_TIMEOUT  (4000)

//! @return whether the specified capability is supported by the session provided
bool comm_session_has_capability(CommSession *session, CommSessionCapability capability);

//! @return Capabilities bitset by the provided session.
CommSessionCapability comm_session_get_capabilities(CommSession *session);

//! @return a reference to the system (Pebble app) communication session, or NULL if the session
//! does not exist (is not connected).
//! @note It is possible that the session becomes disconnected at any point in time.
CommSession *comm_session_get_system_session(void);

//! @return a reference to the third party app communication session for the *currently running*
//! watch app, or NULL if the session does not exist (is not connected).
//! @note It is possible that the session becomes disconnected at any point in time.
CommSession *comm_session_get_current_app_session(void);

//! @param session_in_out[in, out] Pass in a pointer to session pointer to sanitize it. The current
//! *session value can be NULL, to "auto-select" the session for the currently running app.
//! After returning, if *session_in_out was non-NULL when passed in, it will be unchanged if the app
//! is permitted to use it. If not, it *session_in_out will be set to NULL.
//! If *session_in_out was NULL when passed, but there is no session available, it will stay NULL.
void comm_session_sanitize_app_session(CommSession **session_in_out);

//! @return the type of the given session
CommSessionType comm_session_get_type(const CommSession *session);

//! @return the session of the requested type, or NULL if the session does not exist.
//! @note If CommSessionTypeApp is passed in, the *currently running app* session will be returned,
//! if it exists.
CommSession *comm_session_get_by_type(CommSessionType type);

//! @returns a pointer to the UUID of the session, or NULL if the UUID is not known.
//! @note The caller is expected to hold bt_lock! After bt_unlock(), the pointer should no longer
//! be used!
const Uuid *comm_session_get_uuid(const CommSession *session);

//! @return True if the session is the system session
bool comm_session_is_system(CommSession *session);

//! Resets the session (close and attempt re-opening the session)
//! @note If the underlying transport is iAP, this will end up closing all the sessions on top of
//! the transport, since we don't really have the ability to close a single iAP session.
void comm_session_reset(CommSession *session);

//! Convenience function to send data to session for given endpoint id.
//! Note, this is implemented by calling comm_session_send_buffer_begin_write(),
//! comm_session_send_buffer_write() and comm_session_send_buffer_end_write().
//! This will allocate a buffer on the kernel-heap to store the message. If you want to avoid
//! this, use comm_session_send_queue_add_job() directly.
//! If you want to write parts of a Pebble Protocol message piece-meal, it's probably better to use
//! these functions directly instead of this.
//! @param session The session to use
//! @param endpoint_id Which endpoint to send the pebble protocol message to.
//! @param data Pointer to the buffer with data to send
//! @param length The length of the data
//! @param timeout The duration for how long the call is allowed to block. If the send buffer does
//! not have enough space available to enqueue the data, this function will block up to timeout_ms.
//! @return true if the data was successfully queued up for sending.
bool comm_session_send_data(CommSession *session, uint16_t endpoint_id,
                            const uint8_t *data, size_t length, uint32_t timeout_ms);

//! See bt_conn_mgr.h for more details on the parameters
void comm_session_set_responsiveness(
    CommSession *session, BtConsumer consumer, ResponseTimeState state, uint16_t max_period_secs);

//! See bt_conn_mgr.h for more details on the parameters
void comm_session_set_responsiveness_ext(CommSession *session, BtConsumer consumer,
                                         ResponseTimeState state, uint16_t max_period_secs,
                                         ResponsivenessGrantedHandler granted_handler);

void comm_session_init(void);
