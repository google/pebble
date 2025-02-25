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

#include <bluetooth/responsiveness.h>

#include <inttypes.h>

struct Remote;

typedef struct GAPLEConnection GAPLEConnection;

#define BT_CONN_MGR_INACTIVITY_TIMEOUT_SECS (2)

#define MAX_PERIOD_RUN_FOREVER ((uint16_t)(~0))

//! Informs the BT manger module that we want to run the provided LE connection
//! at the requested rate. Care should be taken to minimize the amount of time
//! we need to be in low latency states as they consume more power.
//!
//! Note: Users should really be calling this twice. Once to enter a fast
//!       connection state and then to exit back to the lowest power state. The
//!       max_period_secs variable will protect against being stuck
//!       indefinitely in a high power state.
//!
//! Note: The second call for a particular consumer will override the settings
//!       specified for that consumer during the first call
//!
//! Note: Depending on the mode the controller is currently in there can be a
//!       several second delay before entering the requested state
//!
//! @param[in] hdl             The LE connection to update
//! @param[in] consumer        The consumer requesting the rate change
//! @param[in] state           Choose between the latency levels in ResponseTimeState.
//!                            The lower the latency, the more power being consumed
//! @param[in] max_period_secs The maximum amount of time to keep the connection in an
//!                            elevated response state before returning to ResponseTimeMax
//!                            If MAX_PERIOD_RUN_FOREVER, the requested state will never timeout
//! @param[in] granted_handler The function to call back to when a state has been entered that is
//!                            *as least* as responsive as the requested state.
//!                            It will be executed on KernelMain.
//!                            It is guaranteed to be called exactly once per call to this function.
void conn_mgr_set_ble_conn_response_time_ext(
    GAPLEConnection *hdl, BtConsumer consumer, ResponseTimeState state,
    uint16_t max_period_secs, ResponsivenessGrantedHandler granted_handler);

//! Same as conn_mgr_set_ble_conn_response_time_ext, but without granted_handler.
void conn_mgr_set_ble_conn_response_time(
    GAPLEConnection *hdl, BtConsumer consumer, ResponseTimeState state,
    uint16_t max_period_secs);

//! Informs the BT manager module that we want to run the provided classic
//! connection at the requested rate.
//!
//! Note: This currently supports two modes. ResponseTimeMax maps to BT classic sniff mode
//!       and anything faster maps to BT classic active mode
//!
//! @param[in] remote          The BT Classic connection requesting the rate change
//! @param[in] consumer        The consumer requesting the rate change
//! @param[in] state           Choose between the latency levels in ResponseTimeState.
//!                            The lower the latency, the more power being consumed
//! @param[in] max_period_secs The maximum amount of time to expect being out of sniff mode
//! @param[in] granted_handler The function to call back to when a state has been entered that is
//!                            *as least* as responsive as the requested state.
//!                            It will be executed on KernelMain.
//!                            It is guaranteed to be called exactly once per call to this function.
void conn_mgr_set_bt_classic_conn_response_time_ext(
    struct Remote *remote, BtConsumer consumer, ResponseTimeState state,
    uint16_t max_period_secs, ResponsivenessGrantedHandler granted_handler);

//! Same as conn_mgr_set_bt_classic_conn_response_time_ext, but without granted_handler.s
void conn_mgr_set_bt_classic_conn_response_time(
    struct Remote *remote, BtConsumer consumer, ResponseTimeState state,
    uint16_t max_period_secs);

//! @param[in] connection The connection for which to get the lowest requested latency.
//! @param[out] secs_to_wait The longest amount of time that interval has been requested.
//! If the caller is not interested in this information, NULL can be passed in.
//! @return the lowest latency requested for the connection.
//! @note bt_lock MUST be held by the caller.
ResponseTimeState conn_mgr_get_latency_for_le_connection(GAPLEConnection *connection,
                                                         uint16_t *secs_to_wait);
