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

#include "gap_le_slave_reconnect.h"

#include "applib/bluetooth/ble_ad_parse.h"

#include "gap_le.h"
#include "gap_le_advert.h"
#include "gap_le_connect.h"

#include "comm/ble/ble_log.h"
#include "comm/bt_lock.h"

#include "kernel/event_loop.h"
#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/common/regular_timer.h"
#include "util/size.h"

#include <bluetooth/adv_reconnect.h>
#include <btutil/bt_uuid.h>

//! Reference to the reconnection advertising job.
//! bt_lock() needs to be taken before accessing this variable.
static GAPLEAdvertisingJobRef s_reconnect_advert_job;
static bool s_is_basic_reconnection_enabled;
static bool s_is_hrm_reconnection_enabled;

typedef enum {
  ReconnectType_None,  // Not advertising for reconnection
  ReconnectType_Plain, // Advertising for reconnection with empty payload
  ReconnectType_BleHrm // Advertising for reconnection with HRM payload
} ReconnectType;

// -----------------------------------------------------------------------------
//! Static, internal helper functions
static void prv_advert_job_unscheduled_callback(GAPLEAdvertisingJobRef job,
                                                bool completed,
                                                void *data) {
  // bt_lock() is still held for us by gap_le_advert
  s_reconnect_advert_job = NULL;
}

static bool prv_is_advertising_for_reconnection(void) {
  return (s_reconnect_advert_job != NULL);
}

static ReconnectType prv_current_reconnect_type(void) {
  if (s_is_hrm_reconnection_enabled) {
    return ReconnectType_BleHrm;
  }
  if (s_is_basic_reconnection_enabled) {
    return ReconnectType_Plain;
  }
  return ReconnectType_None;
}

static void prv_unschedule_adv_if_needed(void) {
  if (prv_is_advertising_for_reconnection()) {
    gap_le_advert_unschedule(s_reconnect_advert_job);
  }
}

static void prv_evaluate(ReconnectType prev_type) {
  ReconnectType cur_type = prv_current_reconnect_type();
  if (cur_type == prev_type) {
    return;
  }

  if (cur_type != ReconnectType_None) {
    prv_unschedule_adv_if_needed();

#if CAPABILITY_HAS_BUILTIN_HRM
    const bool use_hrm_payload = (cur_type == ReconnectType_BleHrm);
#else
    const bool use_hrm_payload = false;
#endif

    BLEAdData *ad;
    if (use_hrm_payload) {
      // Create adv payload with only flags + HR service UUID. This is enough for various mobile
      // fitness apps to be able to reconnect to Pebble as BLE HRM.
      ad = ble_ad_create();
      ble_ad_set_flags(ad, GAP_LE_AD_FLAGS_GEN_DISCOVERABLE_MASK);
      Uuid service_uuid = bt_uuid_expand_16bit(0x180D);
      ble_ad_set_service_uuids(ad, &service_uuid, 1);
    } else {
      // [MT] Advertise with an empty payload to save battery life with these
      // reconnection ad packets. This should be enough for the other
      // device to be able to reconnect. With iOS it works, need to test Android.

      // [MT] Note we leave out the Flags AD. According to the spec you have to
      // include flags if any are non-zero. To abide, Pebble ought to always
      // include the SIMULTANEOUS_LE_BR_EDR_TO_SAME_DEVICE_CONTROLLER and
      // SIMULTANEOUS_LE_BR_EDR_TO_SAME_DEVICE_HOST flags. However, we have never
      // done this (ignorance) and gotten by, by using a "random" address (the
      // public address, but then inverted) as a work-around for the problems
      // leaving out these flags caused with Android.
      // I intend to use the "Peripheral privacy feature" some time in the
      // near future. With this, these flags and the issues on Android become
      // a non-issue (because addresses will be private). Therefore I decided to
      // still leave out the flags.

      static BLEAdData payload = {
        .ad_data_length = 0,
        .scan_resp_data_length = 0,
      };
      ad = &payload;
    }

    size_t num_terms = 0;
    const GAPLEAdvertisingJobTerm *advert_terms = bt_driver_adv_reconnect_get_job_terms(&num_terms);

    s_reconnect_advert_job = gap_le_advert_schedule(ad,
                                                    advert_terms, num_terms,
                                                    prv_advert_job_unscheduled_callback,
                                                    NULL,
                                                    GAPLEAdvertisingJobTagReconnection);
    if (use_hrm_payload) {
      ble_ad_destroy(ad);
    }
  } else {
    prv_unschedule_adv_if_needed();
  }
}

static void prv_set_and_evaluate(bool *val, bool new_value) {
  const ReconnectType prev_type = prv_current_reconnect_type();
  *val = new_value;
  prv_evaluate(prev_type);
}

// -----------------------------------------------------------------------------
void gap_le_slave_reconnect_stop(void) {
  bt_lock();
  {
    prv_set_and_evaluate(&s_is_basic_reconnection_enabled, false);
  }
  bt_unlock();
}

// -----------------------------------------------------------------------------
void gap_le_slave_reconnect_start(void) {
#if RECOVERY_FW
  return; // Only use discoverable packet for PRF
#endif
  bt_lock();
  {
    if (prv_is_advertising_for_reconnection()) {
      // Already advertising for reconnection...
      goto unlock;
    }

    if (gap_le_connect_is_connected_as_slave()) {
      // Already connected as slave...
      goto unlock;
    }

    if (!bt_persistent_storage_has_active_ble_gateway_bonding() &&
        !bt_persistent_storage_has_ble_ancs_bonding()) {
      // No bonded master device that would want to reconnect, do nothing.
      goto unlock;
    }

    prv_set_and_evaluate(&s_is_basic_reconnection_enabled, true);
  }
unlock:
  bt_unlock();
}

#if CAPABILITY_HAS_BUILTIN_HRM

#define RECONNECT_HRM_TIMEOUT_SECS (60)

static RegularTimerInfo s_hrm_reconnect_timer;

static void prv_hrm_reconnect_timeout_kernel_main_callback(void *data) {
  gap_le_slave_reconnect_hrm_stop();
}

static void prv_hrm_reconnect_timeout_timer_callback(void *data) {
  launcher_task_add_callback(prv_hrm_reconnect_timeout_kernel_main_callback, NULL);
}

// -----------------------------------------------------------------------------
void gap_le_slave_reconnect_hrm_restart(void) {
  bt_lock();
  {
    prv_set_and_evaluate(&s_is_hrm_reconnection_enabled, true);

    // Always restart the timer:
    if (!regular_timer_is_scheduled(&s_hrm_reconnect_timer)) {
      s_hrm_reconnect_timer = (RegularTimerInfo) {
        .cb = prv_hrm_reconnect_timeout_timer_callback,
      };
      regular_timer_add_multisecond_callback(&s_hrm_reconnect_timer, RECONNECT_HRM_TIMEOUT_SECS);
    }
  }
  bt_unlock();
}

// -----------------------------------------------------------------------------
void gap_le_slave_reconnect_hrm_stop(void) {
  bt_lock();
  {
    prv_set_and_evaluate(&s_is_hrm_reconnection_enabled, false);

    if (regular_timer_is_scheduled(&s_hrm_reconnect_timer)) {
      regular_timer_remove_callback(&s_hrm_reconnect_timer);
    }
  }
  bt_unlock();
}

#endif
