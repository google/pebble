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

#define FILE_LOG_COLOR LOG_COLOR_BLUE

#include "gap_le_connect_params.h"
#include "gap_le_connection.h"

#include "bluetooth/gap_le_connect.h"
#include "bluetooth/responsiveness.h"
#include "comm/bluetooth_analytics.h"
#include "comm/bt_conn_mgr.h"
#include "comm/bt_lock.h"
#include "drivers/rtc.h"
#include "kernel/pbl_malloc.h"
#include "services/common/new_timer/new_timer.h"
#include "system/logging.h"
#include "util/time/time.h"

#include <bluetooth/bluetooth_types.h>
#include <stdint.h>

// [MT] See page 129 of BLE Developer's Handbook (R. Heydon) and also
// http://www.ti.com/lit/ug/swru271f/swru271f.pdf
//
// Connection Event – In a BLE connection between two devices, a
// frequency-hopping scheme is used, in that the two devices each send and
// receive data from one another on a specific channel, then “meet” at a new
// channel (the link layer of the BLE stack handles the channel switching) at a
// specific amount of time later. This “meeting” where the two devices send and
// receive data is known as a “connection event”. Even if there is no
// application data to be sent or received, the two devices will still exchange
// link layer data to maintain the connection.
//
// Connection Interval - The connection interval is the amount of time between
// two connection events, in units of 1.25ms. The connection interval can range
// from a minimum value of 6 (7.5ms) to a maximum of 3200 (4.0s).
//
// Slave Latency (SL): the number of connection events that the slave can
// ignore. This allows the slave save power. When needed, the slave can respond
// to a connection event. Therefore the slave gets (SL+1) opportunities to
// send data back to the master. In other words, this enables lower latency
// responses from the slave, at the cost of the master's energy budget.
// Valid values: 0 - 499, however the maximum value must not make the effective
// connection interval (see below) greater than 16.0s
//
// Supervision timeout: This is the maximum amount of time between two
// successful connection events. If this amount of time passes without a
// successful connection event, the device is to consider the connection lost,
// and return to an unconnected (standby) state.
// Valid values: 100ms to 32,000ms. In addition, the timeout must be larger
// than the effective connection interval (explained below).
// Rule of thumb: the slave should be given at least 6 opportunities
// to resynchronize.
//
// Effective connection interval: is equal to the amount of time between two
// connection events, assuming that the slave skips the maximum number of
// possible events if slave latency is allowed (the effective connection
// interval is equal to the actual connection interval if slave latency is set
// to zero). It can be calculated using the formula:
// Effective Connection Interval = (Connection Interval) * (1+(Slave Latency))

//! This module contains a workaround for parameter update requests not being applied by
//! iOS / Apple's BT controller, even though they get "accepted" by the host.
//! @see gap_le_connect_params_handle_connection_parameter_update_response below for more
//! commentary about the erroneous behavior.
//! Apple bugs / shortcomings: http://www.openradar.me/21400278 and http://www.openradar.me/21400457
//! It seems that if we start hammering the iOS device with more change requests, things don't get
//! better. This timeout value is empirically established using the "ble mode_monkey" prompt
//! command. After running the "ble mode_monkey" for a couple hours, no re-requests had happened.
#define REQUEST_TIMEOUT_MS (40 * 1000)

//! See v4.2 "9.3.12 Connection Interval Timing Parameters":
//! "The Peripheral device should not perform a Connection Parameter Update procedure
//! within TGAP(conn_pause_peripheral = 5 seconds) after establishing a connection."
#define REQUIRED_INIT_PAUSE_S (5)
#define REQUIRED_INIT_PAUSE_TICKS (REQUIRED_INIT_PAUSE_S * RTC_TICKS_HZ)

//! Try 3 times before giving up.
#define MAX_UPDATE_REQUEST_ATTEMPTS (3)

static const GAPLEConnectRequestParams s_default_connection_params_table[NumResponseTimeState] = {
  [ResponseTimeMax] = {
#if BT_CONTROLLER_DA14681
    .slave_latency_events = 0,  // See PBL-38653
#else
    .slave_latency_events = 4,  // Max. allowed by iOS
#endif
    .connection_interval_min_1_25ms = 135,
    .connection_interval_max_1_25ms = 161,
    .supervision_timeout_10ms = 600,
  },
  [ResponseTimeMiddle] = {
#if BT_CONTROLLER_DA14681
    .slave_latency_events = 0,  // See PBL-38653
#else
    .slave_latency_events = 2,
#endif
    .connection_interval_min_1_25ms = 135,
    .connection_interval_max_1_25ms = 161,
    .supervision_timeout_10ms = 600,
  },
  [ResponseTimeMin] = {
    .slave_latency_events = 0, // Not using Slave Latency
    .connection_interval_min_1_25ms = 9,  // Min. allowed by iOS
    .connection_interval_max_1_25ms = 17,
    .supervision_timeout_10ms = 600,
  },
};

extern void conn_mgr_handle_desired_state_granted(GAPLEConnection *hdl,
                                                  ResponseTimeState granted_state);

static void prv_watchdog_timer_callback(void *ctx);

static const GAPLEConnectRequestParams *prv_params_for_state(const GAPLEConnection *connection,
                                                             ResponseTimeState state) {
  if (connection->connection_parameter_sets) {
    return &connection->connection_parameter_sets[state];
  }
  return &s_default_connection_params_table[state];
}

static bool prv_do_actual_params_match_desired_state(const GAPLEConnection *connection,
                                                     ResponseTimeState state,
                                                     uint16_t *actual_conn_interval_ms_out) {
  const BleConnectionParams *actual_params = &connection->conn_params;

  if (actual_conn_interval_ms_out) {
    *actual_conn_interval_ms_out = actual_params->conn_interval_1_25ms;
  }
  const GAPLEConnectRequestParams *desired_params = prv_params_for_state(connection, state);

  // When the fastest state is desired, ignore the minimum bound:
  bool is_interval_min_acceptable;
  if (state == ResponseTimeMin) {
    is_interval_min_acceptable = true;
  } else {
    is_interval_min_acceptable =
        (actual_params->conn_interval_1_25ms >= desired_params->connection_interval_min_1_25ms);
  }

  return (is_interval_min_acceptable &&
          actual_params->conn_interval_1_25ms <= desired_params->connection_interval_max_1_25ms &&
          actual_params->slave_latency_events == desired_params->slave_latency_events);
}

static void prv_request_params_update(GAPLEConnection *connection,
                                      ResponseTimeState state) {
  if (connection->is_remote_device_managing_connection_parameters ||
      connection->param_update_info.is_request_pending) {
    return;
  }

  // We need to wait at least REQUIRED_INIT_PAUSE_TICKS after a connection before
  // requesting new parameters.
  uint32_t retry_ms = REQUEST_TIMEOUT_MS;
  if ((rtc_get_ticks() - connection->ticks_since_connection) < REQUIRED_INIT_PAUSE_TICKS) {
    retry_ms = (REQUIRED_INIT_PAUSE_S * MS_PER_SECOND);
    goto retry;
  }

  // Fall-back:
  uint16_t actual_connection_interval_ms =
      prv_params_for_state(connection, ResponseTimeMax)->connection_interval_max_1_25ms;
  if (prv_do_actual_params_match_desired_state(connection, state, &actual_connection_interval_ms)) {
    return;
  }
  if (connection->param_update_info.attempts++ >= MAX_UPDATE_REQUEST_ATTEMPTS) {
    // [MT]: I've hit this once now. When this happened the TI CC2564B became unresponsive.
    // From the iOS side, it appeared as a connection timeout. A little while after this happened,
    // the BT chip auto-reset work-around kicked in.
    PBL_LOG(LOG_LEVEL_ERROR, "Max attempts reached, giving up. desired_state=%u", state);
    bluetooth_analytics_handle_param_update_failed();
    return;
  }

  // Note: the spec recommends waiting for a 30 second Tgap timeout before issuing a new update
  // request. Bluetopia does not enforce this. However, Sriram Hariharan of Apple confirmed we
  // do not need to do this with Apple devices: "As long as your stack ensures that connection
  // update requests are sent only after the previous request is completed, you can ignore the
  // 30 second Tgap timeout."

  const GAPLEConnectRequestParams *desired_params = prv_params_for_state(connection, state);
  BleConnectionParamsUpdateReq req = {
    .interval_min_1_25ms = desired_params->connection_interval_min_1_25ms,
    .interval_max_1_25ms = desired_params->connection_interval_max_1_25ms,
    .slave_latency_events = desired_params->slave_latency_events,
    .supervision_timeout_10ms = desired_params->supervision_timeout_10ms,
  };

  const bool success = bt_driver_le_connection_parameter_update(&connection->device, &req);
  if (success) {
    connection->param_update_info.is_request_pending = true;
  }

retry:
  // Restart watchdog timer:
  new_timer_start(connection->param_update_info.watchdog_timer, retry_ms,
                  prv_watchdog_timer_callback, connection, 0);
}

static void prv_watchdog_timer_callback(void *ctx) {
  // This should all take very little time, so just execute on NewTimer task:
  bt_lock();
  GAPLEConnection *connection = (GAPLEConnection *)ctx;
  if (gap_le_connection_is_valid(connection)) {
    // Override the flag:
    connection->param_update_info.is_request_pending = false;
    // Retry with most recently requested latency:
    const ResponseTimeState state = conn_mgr_get_latency_for_le_connection(connection, NULL);
    if (connection->param_update_info.attempts > 0) {
      PBL_LOG(LOG_LEVEL_INFO, "Conn param request timed out: re-requesting %u", state);
    }
    prv_request_params_update(connection, state);
  }
  bt_unlock();
}

void gap_le_connect_params_request(GAPLEConnection *connection,
                                   ResponseTimeState desired_state) {
  // A new desired state is requested by the FW, start afresh:
  connection->param_update_info.attempts = 0;

  prv_request_params_update(connection, desired_state);
}

void gap_le_connect_params_setup_connection(GAPLEConnection *connection) {
  connection->param_update_info.watchdog_timer = new_timer_create();
}

void gap_le_connect_params_cleanup_by_connection(GAPLEConnection *connection) {
  new_timer_delete(connection->param_update_info.watchdog_timer);
}

// -------------------------------------------------------------------------------------------------
//! Extern'd for and used by bt_conn_mgr.c
ResponseTimeState gap_le_connect_params_get_actual_state(GAPLEConnection *connection) {
  for (ResponseTimeState state = 0; state < NumResponseTimeState; ++state) {
    if (prv_do_actual_params_match_desired_state(connection, state, NULL)) {
      return state;
    }
  }
  return ResponseTimeInvalid;
}

static void prv_evaluate(GAPLEConnection *connection, ResponseTimeState desired_state) {
  if (prv_do_actual_params_match_desired_state(connection, desired_state, NULL)) {
    conn_mgr_handle_desired_state_granted(connection, desired_state);

    // If the timer callback is executing (waiting on bt_lock) at this point, it's not a problem
    // because the actual vs desired state gets checked in the timer callback path as well.
    new_timer_stop(connection->param_update_info.watchdog_timer);
    return;
  }

  // Connection parameters are updated, but they don't match the desired parameters.
  // (Re)request a parameter update:
  prv_request_params_update(connection, desired_state);
}

// -------------------------------------------------------------------------------------------------
//! Extern'd for and used by services/common/bluetooth/pebble_pairing_service.c
//! bt_lock is assumed to be taken before calling this function.
//! Forces the module to re-evaluate whether the current parameters match the desired ones.
//! This is used when the set of desired request params are changed through Pebble Pairing Service.
void gap_le_connect_params_re_evaluate(GAPLEConnection *connection) {
  const ResponseTimeState desired_state = conn_mgr_get_latency_for_le_connection(connection, NULL);
  prv_evaluate(connection, desired_state);
}

// -------------------------------------------------------------------------------------------------
//! Extern'd for and used by gap_le_connect.c
//! Handles Bluetopia's Connection Parameter Updated event.
//! This event is sent by our BT controller when the updated parameters have actually been applied
//! and taken effect.
//! bt_lock is assumed to be taken before calling this function.
void bt_driver_handle_le_conn_params_update_event(const BleConnectionUpdateCompleteEvent *event) {
  bt_lock();
  const BleConnectionParams *params = &event->conn_params;
  if (event->status != HciStatusCode_Success) {
    goto unlock;
  }

  GAPLEConnection *connection = gap_le_connection_by_addr(&event->dev_address);
  if (!connection) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Receiving conn param update but connection is no longer open");
    goto unlock;
  }

  const ResponseTimeState desired_state = conn_mgr_get_latency_for_le_connection(connection, NULL);
  const bool did_match_desired_state_before =
      prv_do_actual_params_match_desired_state(connection, desired_state, NULL);

  PBL_LOG(LOG_LEVEL_INFO,
          "LE Conn params updated: status: %u, %u, slave lat: %u, supervision timeout: %u "
          "did_match_before: %u",
          event->status, params->conn_interval_1_25ms, params->slave_latency_events,
          params->supervision_timeout_10ms, did_match_desired_state_before);

  // Cache the BLE connection parameters
  connection->conn_params = *params;
  connection->param_update_info.is_request_pending = false;

  const bool local_is_master = connection->local_is_master;
  if (!local_is_master) {
     bluetooth_analytics_handle_connection_params_update(params);
  }

  prv_evaluate(connection, desired_state);
unlock:
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------
//! Extern'd for and used by gap_le_connect.c
//! Handles Bluetopia's Connection Parameter Update Response.
//! This event is sent by the remote's host over the LE Signaling L2CAP channel, to either "accept"
//! or "reject" the parameter set as requested with GAP_LE_Connection_Parameter_Update_Request.
//! When the parameters are "accepted" the other side ought to apply them and a
//! LL_CONNECTION_UPDATE_REQ message (link layer) ought to be the result. However, this does not
//! always seem to be the case on iOS (8.3 and 9.0 beta 1).
#if 0 // TODO: Move to cc2564x driver and keep logging around
void gap_le_connect_params_handle_connection_parameter_update_response(
                       const GAP_LE_Connection_Parameter_Update_Response_Event_Data_t *event_data) {
  if (event_data->Accepted) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Connection Parameter Update Response: accepted=%u",
            event_data->Accepted);
  } else {
    PBL_LOG(LOG_LEVEL_ERROR, "Connection Parameter Update Response: accepted=%u",
            event_data->Accepted);
  }
}
#endif
