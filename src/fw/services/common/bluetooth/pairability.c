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
#include "system/logging.h"
#include "system/passert.h"

#include "comm/ble/gap_le_slave_discovery.h"
#include "kernel/pebble_tasks.h"
#include "services/common/bluetooth/bluetooth_ctl.h"
#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/common/bluetooth/local_addr.h"
#include "services/common/bluetooth/pairability.h"
#include "services/common/regular_timer.h"
#include "services/common/system_task.h"

#include <bluetooth/connectability.h>
#include <bluetooth/features.h>
#include <bluetooth/pairability.h>

static void prv_pairability_timer_cb(void *unused);

static int s_allow_bt_pairing_refcount = 0;
static int s_allow_ble_pairing_refcount = 0;

static RegularTimerInfo s_pairability_timer_info = {
  .cb = prv_pairability_timer_cb,
};

static void evaluate_pairing_refcount(void *data) {
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);

  if (!bt_ctl_is_bluetooth_running()) {
    return;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Pairability state: LE=%u, Classic=%u",
          s_allow_ble_pairing_refcount, s_allow_bt_pairing_refcount);

  bool is_ble_pairable_and_discoverable = (s_allow_ble_pairing_refcount > 0);

  bt_driver_le_pairability_set_enabled(is_ble_pairable_and_discoverable);
  if (gap_le_slave_is_discoverable() != is_ble_pairable_and_discoverable) {
    if (is_ble_pairable_and_discoverable) {
      bt_local_addr_pause_cycling();
    } else {
      bt_local_addr_resume_cycling();
    }
    gap_le_slave_set_discoverable(is_ble_pairable_and_discoverable);
  }

  if (bt_driver_supports_bt_classic()) {
    bt_driver_classic_pairability_set_enabled((s_allow_bt_pairing_refcount > 0));
    bt_driver_classic_update_connectability();
  }
}

static void prv_schedule_evaluation(void) {
  // We used to sparingly schedule the evaluation and had a bug because of this:
  // https://pebbletechnology.atlassian.net/browse/PBL-22884
  // Because this pretty much only happens in response to user input, don't bother limiting this,
  // and always evaluate, even though the state might not have changed:
  system_task_add_callback(evaluate_pairing_refcount, NULL);
}

void bt_pairability_use(void) {
  ++s_allow_bt_pairing_refcount;
  ++s_allow_ble_pairing_refcount;
  prv_schedule_evaluation();
}

void bt_pairability_use_bt(void) {
  ++s_allow_bt_pairing_refcount;
  prv_schedule_evaluation();
}

void bt_pairability_use_ble(void) {
  ++s_allow_ble_pairing_refcount;
  prv_schedule_evaluation();
}

static void prv_pairability_timer_cb(void *unused) {
  regular_timer_remove_callback(&s_pairability_timer_info);
  bt_pairability_release_ble();
}

void bt_pairability_use_ble_for_period(uint16_t duration_secs) {
  if (!regular_timer_is_scheduled(&s_pairability_timer_info)) {
    // If this function is called multiple times before the timer is unscheduled, limit to calling
    // "use" only once:
    bt_pairability_use_ble();
  }
  // Always reschedule, even if the duration is shorter than the one that might already be
  // scheduled:
  regular_timer_add_multisecond_callback(&s_pairability_timer_info, duration_secs);
}

void bt_pairability_release(void) {
  PBL_ASSERT(s_allow_bt_pairing_refcount != 0 && s_allow_ble_pairing_refcount != 0, "");
  --s_allow_bt_pairing_refcount;
  --s_allow_ble_pairing_refcount;
  prv_schedule_evaluation();
}

void bt_pairability_release_bt(void) {
  PBL_ASSERT(s_allow_bt_pairing_refcount != 0, "");
  --s_allow_bt_pairing_refcount;
  prv_schedule_evaluation();
}

void bt_pairability_release_ble(void) {
  PBL_ASSERT(s_allow_ble_pairing_refcount != 0, "");
  --s_allow_ble_pairing_refcount;
  prv_schedule_evaluation();
}

//! Call this whenever we modify the number of saved bondings we have.
void bt_pairability_update_due_to_bonding_change(void) {
  static bool s_pairable_due_to_no_gateway_bondings = false;
  const bool has_classic_bonding =
      (bt_driver_supports_bt_classic() &&
       bt_persistent_storage_has_active_bt_classic_gateway_bonding());

  if (!has_classic_bonding &&
      !bt_persistent_storage_has_active_ble_gateway_bonding() &&
      !bt_persistent_storage_has_ble_ancs_bonding()) {
    if (!s_pairable_due_to_no_gateway_bondings) {
      bt_pairability_use();
      s_pairable_due_to_no_gateway_bondings = true;
    }
  } else {
    if (s_pairable_due_to_no_gateway_bondings) {
      bt_pairability_release();
      s_pairable_due_to_no_gateway_bondings = false;
    }
  }
}

void bt_pairability_init(void) {
  bt_pairability_update_due_to_bonding_change();
  prv_schedule_evaluation();
}
