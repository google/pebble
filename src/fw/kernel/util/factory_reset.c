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

#include "kernel/util/factory_reset.h"

#include "drivers/rtc.h"
#include "drivers/task_watchdog.h"
#include "flash_region/filesystem_regions.h"
#include "kernel/event_loop.h"
#include "kernel/util/standby.h"
#include "process_management/worker_manager.h"
#include "services/common/event_service.h"
#include "services/common/shared_prf_storage/shared_prf_storage.h"
#include "services/common/system_task.h"
#include "services/runlevel.h"
#include "shell/normal/app_idle_timeout.h"
#include "system/bootbits.h"
#include "system/logging.h"
#include "system/reboot_reason.h"
#include "system/reset.h"
#include "kernel/util/sleep.h"

#if !RECOVERY_FW
#include "services/normal/blob_db/pin_db.h"
#include "services/normal/blob_db/reminder_db.h"
#include "services/normal/filesystem/pfs.h"
#include "services/normal/timeline/event.h"
#endif

static bool s_in_factory_reset = false;

static void prv_factory_reset_non_pfs_data() {
  PBL_LOG_SYNC(LOG_LEVEL_INFO, "Factory resetting...");

  // This function can block the system task for a long time.
  // Prevent callbacks being added to the system task so it doesn't overflow.
  system_task_block_callbacks(true /* block callbacks */);
  launcher_block_popups(true);

  worker_manager_disable();
  event_service_clear_process_subscriptions(PebbleTask_App);

  shared_prf_storage_wipe_all();

  services_set_runlevel(RunLevel_BareMinimum);
  app_idle_timeout_stop();

  while (worker_manager_get_current_worker_md()) {
    // busy loop until the worker is killed
    psleep(3);
  }

  rtc_timezone_clear();
}

void factory_reset_set_reason_and_reset(void) {
  RebootReason reason = { RebootReasonCode_FactoryResetReset, 0 };
  reboot_reason_set(&reason);
  system_reset();
}

static void prv_factory_reset_post(bool should_shutdown) {
  if (should_shutdown) {
    enter_standby(RebootReasonCode_FactoryResetShutdown);
  } else {
    factory_reset_set_reason_and_reset();
  }
}

void factory_reset(bool should_shutdown) {
  s_in_factory_reset = true;

  prv_factory_reset_non_pfs_data();

  // TODO: wipe the registry on tintin?
  filesystem_regions_erase_all();

#if !defined(RECOVERY_FW)
  // "First use" is part of the PRF image for Snowy
  boot_bit_set(BOOT_BIT_FORCE_PRF);
#endif

  prv_factory_reset_post(should_shutdown);
}

#if !RECOVERY_FW
void close_db_files() {
  // Deinit the databases and any clients
  timeline_event_deinit();
  reminder_db_deinit();
  pin_db_deinit();
}

void factory_reset_fast(void *unused) {
  s_in_factory_reset = true;

  // disable the watchdog... we've got lots to do before we reset
  task_watchdog_mask_clear(pebble_task_get_current());

  close_db_files();

  prv_factory_reset_non_pfs_data();

  pfs_remove_files(NULL);

  prv_factory_reset_post(false /* should_shutdown */);
}
#endif // !RECOVERY_FW

//! Used by the mfg flow to kick us out the MFG firmware and into the consumer PRF that's stored
//! on the external flash.
void command_enter_consumer_mode(void) {
  boot_bit_set(BOOT_BIT_FORCE_PRF);
  factory_reset(true /* should_shutdown */);
}

bool factory_reset_ongoing(void) {
  return s_in_factory_reset;
}
