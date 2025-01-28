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

#include "drivers/battery.h"
#include "drivers/qemu/qemu_serial.h"
#include "drivers/qemu/qemu_settings.h"

#include "system/passert.h"
#include "services/common/battery/battery_state.h"
#include "services/common/battery/battery_curve.h"
#include "system/logging.h"

#include "util/math.h"
#include "util/net.h"

static uint16_t s_battery_mv = 4000;
static bool s_usb_connected;
static uint8_t s_percent = 100;

void battery_init(void) {
  s_usb_connected = qemu_setting_get(QemuSetting_DefaultPluggedIn);
}

// TODO: update whoever uses this function
int battery_get_millivolts(void) {
  return s_battery_mv;
}

bool battery_charge_controller_thinks_we_are_charging_impl(void) {
  return s_usb_connected && (s_percent < 100);
}

bool battery_is_usb_connected_impl(void) {
  return s_usb_connected;
}

void battery_set_charge_enable(bool charging_enabled) {
  s_usb_connected = false;
}

void battery_set_fast_charge(bool fast_charge_enabled) {
}


void qemu_battery_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolBatteryHeader *hdr = (QemuProtocolBatteryHeader *)data;
  if (len != sizeof(*hdr)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet length");
    return;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Got battery msg: pct: %d, charger_connected:%d",
        hdr->battery_pct, hdr->charger_connected);

  s_percent = MIN(100, hdr->battery_pct);
  s_usb_connected = hdr->charger_connected;
  s_battery_mv = battery_curve_lookup_voltage_by_percent(s_percent, hdr->charger_connected);

  // Reset the time averaging so these new values take effect immediately
  battery_state_reset_filter();

  // Force a state machine update
  battery_state_handle_connection_event(s_usb_connected);
}


