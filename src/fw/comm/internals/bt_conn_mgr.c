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

#include <bluetooth/responsiveness.h>

#include "comm/ble/gap_le_connect_params.h"
#include "comm/ble/gap_le_connection.h"
#include "comm/bt_conn_mgr.h"
#include "comm/bt_lock.h"
#include "drivers/rtc.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/regular_timer.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/list.h"
#include "util/math.h"
#include "util/rand.h"

#include <stdlib.h>

//! The Bluetooth Connection Manager is responsible for managing the power
//! state of the active bluetooth connections. Sub-modules using bluetooth are
//! expected to notify this module when they are active or expect inbound data
//! and want to minimize latency. Using this info, the module decides whether
//! the LE or classic connection needs to be bumped out of its lower power
//! state in order to respond more quickly.
//!
//! Note: This module currently only manages the LE connections. In the
//! future, we will add support for handling classic connections as well

typedef struct {
  ListNode          list_node;
  uint32_t          timeout; // time to stop this request (in rtc ticks)
  ResponseTimeState req_state;
  BtConsumer        consumer;
  ResponsivenessGrantedHandler granted_handler;
} ConnectionStateRequest;

typedef struct ConnectionMgrInfo {
  // callback which returns us to a low power state if user of API does not exit
  // a high power state
  RegularTimerInfo        watchdog_cb_info;
  // current running state of the connection
  ResponseTimeState       curr_requested_state;
  // A list of consumers who have requested changes to latency state != ResponseTimeMax
  ConnectionStateRequest *requests;
} ConnectionMgrInfo;

ResponseTimeState gap_le_connect_params_get_actual_state(GAPLEConnection *connection);

//! Walks through and finds the lowest latency requested for the given type of
//! connection. Also detects the longest amount of time that interval has been
//! requested. Also gets the consumer that is responsible for the lowest latency + longest timeout
//! combo. These pieces of information are then returned to the caller.
static ResponseTimeState prv_determine_latency_for_connection(
    ConnectionStateRequest *requests, uint16_t *secs_to_wait, BtConsumer *consumer_out) {
  ResponseTimeState state = ResponseTimeMax;
  uint32_t timeout = 0;
  BtConsumer responsible_consumer = BtConsumerNone;

  ConnectionStateRequest *curr_request = requests;
  while (curr_request != NULL) {
    if (curr_request->req_state > state) {
      // reset our tracker, we have found a higher power mode requested
      timeout = curr_request->timeout;
      state = curr_request->req_state;
      responsible_consumer = curr_request->consumer;
    } else if (curr_request->req_state == state) {
      if (curr_request->timeout > timeout) {
        timeout = curr_request->timeout;
        responsible_consumer = curr_request->consumer;
      }
    }

    curr_request = (ConnectionStateRequest *)list_get_next(&curr_request->list_node);
  }

  if (consumer_out) {
    *consumer_out = responsible_consumer;
  }

  if (secs_to_wait) {
    uint32_t curr_ticks = rtc_get_ticks();
    if (curr_ticks < timeout) {
      uint16_t wait_time = (timeout - curr_ticks) / RTC_TICKS_HZ;
      *secs_to_wait = MAX(1, wait_time);
    } else {
      *secs_to_wait = 0;
    }
  }

  return state;
}

/*
 * LE connection manager handling for a gateway connection
 */

static void prv_bt_le_gateway_response_latency_watchdog_cb(void *data);

static void prv_granted_kernel_main_cb(void *ctx) {
  ResponsivenessGrantedHandler granted_handler = ctx;
  granted_handler();
}

static void prv_schedule_granted_handler(ResponsivenessGrantedHandler granted_handler) {
  PBL_ASSERTN(granted_handler);
  launcher_task_add_callback(prv_granted_kernel_main_cb, granted_handler);
}

//! extern'd for gap_le_connect_params.c
void conn_mgr_handle_desired_state_granted(GAPLEConnection *hdl,
                                           ResponseTimeState granted_state) {
  bt_lock_assert_held(true);

  ConnectionStateRequest *curr_request = hdl->conn_mgr_info->requests;
  while (curr_request != NULL) {
    if (curr_request->granted_handler &&
        curr_request->req_state <= granted_state) {
      prv_schedule_granted_handler(curr_request->granted_handler);
      curr_request->granted_handler = NULL;
    }
    curr_request = (ConnectionStateRequest *)list_get_next(&curr_request->list_node);
  }
}

static void prv_handle_response_latency_for_le_conn(GAPLEConnection *hdl) {
  uint16_t secs_til_max_latency;
  ResponseTimeState state;
  BtConsumer responsible_consumer;
#ifdef RECOVERY_FW
  // We don't care if we burn up some power from PRF and we want FW to update quickly
  secs_til_max_latency = MAX_PERIOD_RUN_FOREVER;
  state = ResponseTimeMin;
  responsible_consumer = 0;
#else
  state = prv_determine_latency_for_connection(hdl->conn_mgr_info->requests,
      &secs_til_max_latency, &responsible_consumer);
#endif

  // actually request the mode if it has changed:
  if (hdl->conn_mgr_info->curr_requested_state != state) {
    PBL_LOG(LOG_LEVEL_INFO, "LE: Requesting state %d for %d secs, due to %u",
            state, secs_til_max_latency, responsible_consumer);
    gap_le_connect_params_request(hdl, state);
  }

  // remove a watchdog timer if it was already scheduled and schedule a new one
  RegularTimerInfo *watchdog_cb_info = &hdl->conn_mgr_info->watchdog_cb_info;
  if (regular_timer_is_scheduled(watchdog_cb_info)) {
    regular_timer_remove_callback(watchdog_cb_info);
  }

  // don't start the watchdog timer if we have entered the lowest power mode or
  // if we want to run at the specified rate indefinitely
  if ((state != ResponseTimeMax) && (secs_til_max_latency != MAX_PERIOD_RUN_FOREVER)) {
    watchdog_cb_info->cb = prv_bt_le_gateway_response_latency_watchdog_cb;
    watchdog_cb_info->cb_data = hdl;
    // wait an extra second since the multisecond callback will fire somewhere
    // between 0 and 1 seconds from now and we want to make sure the interval
    // we are currently running at actually expires
    regular_timer_add_multisecond_callback(
            watchdog_cb_info, secs_til_max_latency + 1);
  }

  hdl->conn_mgr_info->curr_requested_state = state;
}

static void prv_bt_le_gateway_response_latency_watchdog_handler(void *data) {
  bt_lock();
  GAPLEConnection *hdl = (GAPLEConnection *)data;

  // Let's make sure our connection handle is still valid in case we
  // disconnected before this CB had a chance to execute
  if (!gap_le_connection_is_valid(hdl)) {
    goto unlock;
  }

  ConnectionMgrInfo *conn_mgr_info = hdl->conn_mgr_info;

  // if we are executing this cb, we have timed out running at the currently
  // selected state so check and see what consumer timeouts have expired

  ConnectionStateRequest *curr_request = conn_mgr_info->requests;
  uint32_t curr_ticks = rtc_get_ticks();
  while (curr_request != NULL) {
    ConnectionStateRequest *next =
        (ConnectionStateRequest *)list_get_next(&curr_request->list_node);
    if (conn_mgr_info->curr_requested_state == curr_request->req_state) {
      if (curr_ticks >= curr_request->timeout) {
        list_remove(&curr_request->list_node, (ListNode **)&conn_mgr_info->requests, NULL);
        kernel_free(curr_request);
      }
    }
    curr_request = next;
  }

  // Note: As an optimization, we could track how long we have been in a lower
  // latency state and subtract that from higher latency requests, but most of
  // the time we should be in the maximum latency (low power) state anyway

  // get & set the new state
  prv_handle_response_latency_for_le_conn(hdl);

unlock:
  bt_unlock();
}

static void prv_bt_le_gateway_response_latency_watchdog_cb(void *data) {
  // offload handling onto KernelBG so we don't stall the timer thread
  // trying to get the bt lock
  system_task_add_callback(prv_bt_le_gateway_response_latency_watchdog_handler, data);
}

static bool prv_find_source(ListNode *found_node, void *data) {
  return (((ConnectionStateRequest *)found_node)->consumer == (BtConsumer)data);
}

/*
 * Exported APIs
 */

void conn_mgr_set_ble_conn_response_time(
    GAPLEConnection *hdl, BtConsumer consumer, ResponseTimeState state,
    uint16_t max_period_secs) {
  conn_mgr_set_ble_conn_response_time_ext(hdl, consumer, state, max_period_secs, NULL);
}

void conn_mgr_set_ble_conn_response_time_ext(
    GAPLEConnection *hdl, BtConsumer consumer, ResponseTimeState state,
    uint16_t max_period_secs, ResponsivenessGrantedHandler granted_handler) {
  ConnectionMgrInfo *conn_mgr_info;
  if (!hdl || !((conn_mgr_info = hdl->conn_mgr_info))) {
    PBL_LOG(LOG_LEVEL_ERROR, "GAP Handle not properly initialized");
    return;
  }

  bt_lock();
  // remove the watchdog timer if it was already scheduled since we are
  // going to recompute
  RegularTimerInfo *watchdog_cb_info = &hdl->conn_mgr_info->watchdog_cb_info;
  if (regular_timer_is_scheduled(watchdog_cb_info)) {
    regular_timer_remove_callback(watchdog_cb_info);
  }

  ConnectionStateRequest *consumer_request =
      (ConnectionStateRequest *)list_find((ListNode *)conn_mgr_info->requests,
                                           prv_find_source, (void *)consumer);

  bool is_already_granted = (gap_le_connect_params_get_actual_state(hdl) >= state);

  if (consumer_request == NULL) {
    if (state == ResponseTimeMax) {
      // No changes: there was no previous node and the new state is the default "low power" one.
      goto handle_current_state;
    }

    // create node
    consumer_request = kernel_malloc_check(sizeof(ConnectionStateRequest));
    list_init(&consumer_request->list_node);
    conn_mgr_info->requests = (ConnectionStateRequest *)list_prepend(
        &conn_mgr_info->requests->list_node, &consumer_request->list_node);
  }

  // If the consumer requests to go back to low power (ResponseTimeMax), wait a little longer
  // before actually going back. This prevents rapid back-n-forths between low power and fast modes,
  // that can happen especially in a chain of operations, for example, the resource & bin put-bytes
  // sessions to install an app.
  if (state == ResponseTimeMax) {
    // Keep the existing node in the list for the duration of our "activity timeout". It will be
    // cleaned up automatically by the watchdog timer.
    max_period_secs = BT_CONN_MGR_INACTIVITY_TIMEOUT_SECS;
    state = consumer_request->req_state;
  }

  // populate node with new info. If it was previously set we override it
  consumer_request->timeout =  rtc_get_ticks() + max_period_secs * RTC_TICKS_HZ;
  consumer_request->req_state = state;
  consumer_request->consumer = consumer;
  consumer_request->granted_handler = is_already_granted ? NULL : granted_handler;

handle_current_state:

  if (is_already_granted && granted_handler) {
    prv_schedule_granted_handler(granted_handler);
  }
  prv_handle_response_latency_for_le_conn(hdl);

  bt_unlock();
}

//! expects that the bt lock is held
ConnectionMgrInfo * bt_conn_mgr_info_init(void) {
  ConnectionMgrInfo *newinfo = kernel_malloc_check(sizeof(ConnectionMgrInfo));
  *newinfo =  (ConnectionMgrInfo) {
    .curr_requested_state = ResponseTimeMax,
  };

  return newinfo;
}

//! expects that the bt_lock is held
void bt_conn_mgr_info_deinit(ConnectionMgrInfo **info) {
  // If we have any callbacks scheduled for this device, take them out
  RegularTimerInfo *watchdog_cb_info = &(*info)->watchdog_cb_info;
  if (regular_timer_is_scheduled(watchdog_cb_info)) {
    regular_timer_remove_callback(watchdog_cb_info);
  }

  ListNode *curr_request = (ListNode *)(*info)->requests;
  while (curr_request != NULL) {
    ListNode *temp = list_get_next(curr_request);
    list_remove(curr_request, NULL, NULL);
    kernel_free(curr_request);
    curr_request = temp;
  }

  kernel_free(*info);
  *info = NULL;
}

void command_change_le_mode(char *mode) {
  // assume we only have one connection for debug
  GAPLEConnection *conn_hdl = gap_le_connection_any();
  ResponseTimeState state = atoi(mode);

  conn_mgr_set_ble_conn_response_time(
      conn_hdl, BtConsumerPrompt, state, MAX_PERIOD_RUN_FOREVER);
}

static TimerID s_chaos_monkey_timer;
static ResponseTimeState s_chaos_monkey_last_state;

static void prv_mode_chaos_monkey_stop(void) {
  new_timer_delete(s_chaos_monkey_timer);
  s_chaos_monkey_timer = TIMER_INVALID_ID;
}

static void prv_mode_chaos_monkey_callback(void *data) {
  bt_lock();

  GAPLEConnection *hdl = (GAPLEConnection *) data;
  if (s_chaos_monkey_timer == TIMER_INVALID_ID) {
    goto unlock;
  }
  if (!gap_le_connection_is_valid(hdl)) {
    prv_mode_chaos_monkey_stop();
    goto unlock;
  }

  ResponseTimeState requested_state;
  do {
    requested_state = bounded_rand_int(ResponseTimeMax, ResponseTimeMin);
  } while (requested_state == s_chaos_monkey_last_state);
  s_chaos_monkey_last_state = requested_state;

  conn_mgr_set_ble_conn_response_time(hdl, BtConsumerPrompt,
                                      requested_state, MAX_PERIOD_RUN_FOREVER);

  const uint32_t delay_ms = bounded_rand_int(1, 3000);
  PBL_LOG(LOG_LEVEL_DEBUG, "Mode chaos monkey: next change=%"PRIu32"ms", delay_ms);

  new_timer_start(s_chaos_monkey_timer, delay_ms, prv_mode_chaos_monkey_callback, data, 0);

unlock:
  bt_unlock();
}

void command_le_mode_chaos_monkey(char *enabled_str) {
  bool new_enabled = atoi(enabled_str);

  bool is_enabled = (s_chaos_monkey_timer != TIMER_INVALID_ID);
  if (new_enabled == is_enabled) {
    return;
  }

  bt_lock();
  if (new_enabled) {
    GAPLEConnection *conn_hdl = gap_le_connection_any();
    if (conn_hdl) {
      s_chaos_monkey_timer = new_timer_create();
      prv_mode_chaos_monkey_callback(conn_hdl);
    }
  } else {
    prv_mode_chaos_monkey_stop();
  }
  bt_unlock();
}

ResponseTimeState conn_mgr_get_latency_for_le_connection(
    GAPLEConnection *hdl, uint16_t *secs_to_wait) {
  bt_lock_assert_held(true);
  return prv_determine_latency_for_connection(
      hdl->conn_mgr_info->requests, secs_to_wait, NULL);
}
