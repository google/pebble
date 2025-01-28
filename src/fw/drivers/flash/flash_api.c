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

#include "drivers/flash.h"
#include "drivers/flash/flash_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "drivers/flash/flash_impl.h"
#include "drivers/task_watchdog.h"
#include "drivers/watchdog.h"
#include "flash_region/flash_region.h"
#include "kernel/util/stop.h"
#include "os/mutex.h"
#include "os/tick.h"
#include "process_management/worker_manager.h"
#include "services/common/analytics/analytics.h"
#include "services/common/new_timer/new_timer.h"
#include "system/logging.h"
#include "system/passert.h"
#include "kernel/util/sleep.h"

#include "FreeRTOS.h"
#include "semphr.h"

#define MAX_ERASE_RETRIES (3)

static PebbleMutex *s_flash_lock;
static SemaphoreHandle_t s_erase_semphr;

static struct FlashEraseContext {
  bool in_progress;
  bool suspended;
  bool is_subsector;
  uint8_t retries;
  PebbleTask task;
  uint32_t address;
  FlashOperationCompleteCb on_complete_cb;
  void *cb_context;
  uint32_t expected_duration;
} s_erase = { 0 };

static TimerID s_erase_poll_timer;
static TimerID s_erase_suspend_timer;

static uint32_t s_analytics_read_count;
static uint32_t s_analytics_read_bytes_count;
static uint32_t s_analytics_write_bytes_count;

static uint32_t s_system_analytics_read_bytes_count = 0;
static uint32_t s_system_analytics_write_bytes_count = 0;
static uint8_t s_system_analytics_erase_count = 0;

void analytics_external_collect_system_flash_statistics(void) {
  analytics_set(ANALYTICS_DEVICE_METRIC_FLASH_READ_BYTES_COUNT,
                s_system_analytics_read_bytes_count, AnalyticsClient_System);
  analytics_set(ANALYTICS_DEVICE_METRIC_FLASH_WRITE_BYTES_COUNT,
                s_system_analytics_write_bytes_count, AnalyticsClient_System);
  analytics_set(ANALYTICS_DEVICE_METRIC_FLASH_ERASE_COUNT,
                s_system_analytics_erase_count, AnalyticsClient_System);

  s_system_analytics_read_bytes_count = 0;
  s_system_analytics_write_bytes_count = 0;
  s_system_analytics_erase_count = 0;
}

void analytics_external_collect_app_flash_read_stats(void) {
  analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_COUNT,
                s_analytics_read_count, AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_BYTES_COUNT,
                s_analytics_read_bytes_count, AnalyticsClient_App);
  analytics_set(ANALYTICS_APP_METRIC_FLASH_WRITE_BYTES_COUNT,
                s_analytics_write_bytes_count, AnalyticsClient_App);

  // The overhead cost of tracking whether each flash read was due to the foreground
  // or background app is large, so the best we can do is to attribute to both of them
  if (worker_manager_get_current_worker_md() != NULL) {
    analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_COUNT,
                  s_analytics_read_count, AnalyticsClient_Worker);
    analytics_set(ANALYTICS_APP_METRIC_FLASH_READ_BYTES_COUNT,
                  s_analytics_read_bytes_count, AnalyticsClient_Worker);
    analytics_set(ANALYTICS_APP_METRIC_FLASH_WRITE_BYTES_COUNT,
                  s_analytics_write_bytes_count, AnalyticsClient_Worker);
  }

  s_analytics_read_count = 0;
  s_analytics_read_bytes_count = 0;
  s_analytics_write_bytes_count = 0;
}

//! Assumes that s_flash_lock is held.
static status_t prv_try_restart_interrupted_erase(bool is_subsector,
                                                  uint32_t addr) {
  status_t status = is_subsector? flash_impl_erase_subsector_begin(addr)
                                : flash_impl_erase_sector_begin(addr);
  if (FAILED(status)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Got error trying to reissue interrupted erase: "
            "%"PRIi32, status);
    return status;
  }

  // Hopefully the task watchdog isn't enabled; this could take a while.
  while (1) {
    psleep(10);
    status = flash_impl_get_erase_status();
    if (status != E_BUSY && status != E_AGAIN) {
      // Success or failure
      return status;
    }
  }
}

void flash_init(void) {
  if (s_flash_lock) {
    return; // Already initialized
  }
  s_flash_lock = mutex_create();
  s_erase_semphr = xSemaphoreCreateBinary();
  xSemaphoreGive(s_erase_semphr);
  s_erase_poll_timer = new_timer_create();
  s_erase_suspend_timer = new_timer_create();
  flash_erase_init();

  mutex_lock(s_flash_lock);
  flash_impl_init(false /* coredump_mode */);

  uint32_t erase_in_progress_address = 0;
  bool is_subsector = false;
  if (flash_impl_get_nvram_erase_status(
        &is_subsector, &erase_in_progress_address) == S_TRUE) {
    // An erase was interrupted by e.g. a crash. Retry the erase so the
    // incompletely-erased sector doesn't cause us trouble later on.
    PBL_LOG(LOG_LEVEL_WARNING, "Flash erase to 0x%"PRIx32" was interrupted "
            "last boot. Restarting the erase...", erase_in_progress_address);

    // When an erase is interrupted, subsequent erases of the same sector tend to
    // take an exceptionally long time and may even outright fail. Try the erase a
    // few times before giving up.
    int attempt = 1;
    while (true) {
      status_t status = prv_try_restart_interrupted_erase(
          is_subsector, erase_in_progress_address);
      if (PASSED(status)) {
        break;
      }
      PBL_LOG(LOG_LEVEL_ERROR, "Flash erase failed, status %"PRIi32, status);
      if (attempt++ >= MAX_ERASE_RETRIES) {
        // We've tried all we can. No point in croaking; it's not like a reboot
        // is going to fix anything. Best we can do is pretend like nothing is
        // wrong and hope for the best.
        PBL_LOG(LOG_LEVEL_ERROR, "Giving up on restarting the flash erase.");
        break;
      }
    }
  }

  flash_impl_clear_nvram_erase_status();
  mutex_unlock(s_flash_lock);
}

#if UNITTEST
void flash_api_reset_for_test(void) {
  s_erase = (struct FlashEraseContext) {0};
  s_flash_lock = NULL;
}

TimerID flash_api_get_erase_poll_timer_for_test(void) {
  return s_erase_poll_timer;
}
#endif

//! Assumes that s_flash_lock is held.
static void prv_erase_pause(void) {
  if (s_erase.in_progress && !s_erase.suspended) {
    // If an erase is in progress, make sure it gets at least a minimum time slice to progress.
    // If not, the successive kicking of the suspend timer could starve it out completely
    psleep(100);
    task_watchdog_bit_set(s_erase.task);
    status_t status = flash_impl_erase_suspend(s_erase.address);
    PBL_ASSERT(PASSED(status), "Erase suspend failure: %" PRId32, status);
    if (status == S_NO_ACTION_REQUIRED) {
      // The erase has already completed. No need to resume.
      s_erase.in_progress = false;
    } else {
      s_erase.suspended = true;
    }
  }
}

//! Assumes that s_flash_lock is held.
static void prv_erase_resume(void) {
  if (s_erase.suspended) {
    status_t status = flash_impl_erase_resume(s_erase.address);
    PBL_ASSERT(PASSED(status), "Erase resume failure: %" PRId32, status);
    s_erase.suspended = false;
  }
}

static void prv_erase_suspend_timer_cb(void *unused) {
  mutex_lock(s_flash_lock);
  prv_erase_resume();
  mutex_unlock(s_flash_lock);
}

void flash_read_bytes(uint8_t* buffer, uint32_t start_addr,
                      uint32_t buffer_size) {
  mutex_lock(s_flash_lock);
  s_analytics_read_count++;
  s_analytics_read_bytes_count += buffer_size;
  s_system_analytics_read_bytes_count += buffer_size;
  // TODO: use DMA when possible
  // TODO: be smarter about pausing erases. Some flash chips allow concurrent
  // reads while an erase is in progress, as long as the read is to another bank
  // than the one being erased.
  prv_erase_pause();
  new_timer_start(s_erase_suspend_timer, 5, prv_erase_suspend_timer_cb, NULL, 0);
  flash_impl_read_sync(buffer, start_addr, buffer_size);
  mutex_unlock(s_flash_lock);
}

#ifdef TEST_FLASH_LOCK_PROTECTION
static bool s_assert_write_error = false;
void flash_expect_program_failure(bool expect_failure) {
  s_assert_write_error = expect_failure;
}
#endif

void flash_write_bytes(const uint8_t *buffer, uint32_t start_addr,
                       uint32_t buffer_size) {
  mutex_lock(s_flash_lock);
  stop_mode_disable(InhibitorFlash);  // FIXME: PBL-18028
  s_analytics_write_bytes_count += buffer_size;
  s_system_analytics_write_bytes_count += buffer_size;
  prv_erase_pause();
  new_timer_start(s_erase_suspend_timer, 50, prv_erase_suspend_timer_cb, NULL, 0);
  while (buffer_size) {
    int written = flash_impl_write_page_begin(buffer, start_addr, buffer_size);
    PBL_ASSERT(
#ifdef TEST_FLASH_LOCK_PROTECTION
        s_assert_write_error ||
#endif
        PASSED(written),
        "flash_impl_write_page_begin failed: %d", written);
    status_t status;
    while ((status = flash_impl_get_write_status()) == E_BUSY) {
      psleep(0);
    }
#ifdef TEST_FLASH_LOCK_PROTECTION
    if (s_assert_write_error) {
      PBL_ASSERT(FAILED(status), "flash write unexpectedly succeeded: %" PRId32,
                 status);
    } else {
#endif
    PBL_ASSERT(PASSED(status), "flash_impl_get_write_status returned %" PRId32,
               status);
#ifdef TEST_FLASH_LOCK_PROTECTION
    }
#endif
    buffer += written;
    start_addr += written;
    buffer_size -= written;
    // Give higher-priority tasks a chance to access the flash in between
    // each page write.
    // TODO: uncomment the lines below to resolve PBL-17503
    // if (buffer_size) {
    //   mutex_unlock(s_flash_lock);
    //   mutex_lock(s_flash_lock);
    // }
  }
  stop_mode_enable(InhibitorFlash);
  mutex_unlock(s_flash_lock);
}

// Returns 0 if the erase has completed, or a non-zero expected duration (in
// ms) if not. If the erase has not finished (non-zero has been returned), the
// caller is responsible for calling the prv_flash_erase_poll() method until
// the erase completes.
static uint32_t prv_flash_erase_start(uint32_t addr,
                                      FlashOperationCompleteCb on_complete_cb,
                                      void *context,
                                      bool is_subsector,
                                      uint8_t retries) {
  xSemaphoreTake(s_erase_semphr, portMAX_DELAY);
  mutex_lock(s_flash_lock);
  PBL_ASSERTN(s_erase.in_progress == false);
  s_erase = (struct FlashEraseContext) {
    .in_progress = true,
    .task = pebble_task_get_current(),
    .retries = retries,
  // FIXME: We should just assert that the address is already aligned. If
  // someone is depending on this behaviour without already knowing the range
  // that's being erased they're going to have a bad time. This will probably
  // cause some client fallout though, so tackle this later.
    .is_subsector = is_subsector,
    .address = is_subsector? flash_impl_get_subsector_base_address(addr)
                           : flash_impl_get_sector_base_address(addr),
    .on_complete_cb = on_complete_cb,
    .cb_context = context,
    .expected_duration = is_subsector?
        flash_impl_get_typical_subsector_erase_duration_ms() :
        flash_impl_get_typical_sector_erase_duration_ms(),
  };
  stop_mode_disable(InhibitorFlash);  // FIXME: PBL-18028
  status_t status = is_subsector? flash_impl_blank_check_subsector(addr)
                                : flash_impl_blank_check_sector(addr);
  PBL_ASSERT(PASSED(status), "Blank check error: %" PRId32, status);
  if (status != S_FALSE) {
    stop_mode_enable(InhibitorFlash);
    s_erase.in_progress = false;
    mutex_unlock(s_flash_lock);
    xSemaphoreGive(s_erase_semphr);
    // Only run the callback with no locks held so that the callback won't
    // deadlock if it kicks off another sector erase.
    on_complete_cb(context, S_NO_ACTION_REQUIRED);
    return 0;
  }

  analytics_inc(ANALYTICS_APP_METRIC_FLASH_SUBSECTOR_ERASE_COUNT,
                AnalyticsClient_CurrentTask);
  s_system_analytics_erase_count++;
  status = is_subsector? flash_impl_erase_subsector_begin(addr)
                       : flash_impl_erase_sector_begin(addr);

  if (PASSED(status)) {
    flash_impl_set_nvram_erase_status(is_subsector, addr);
    mutex_unlock(s_flash_lock);
    return (s_erase.expected_duration * 7 / 8);
  } else {
    stop_mode_enable(InhibitorFlash);
    s_erase.in_progress = false;
    mutex_unlock(s_flash_lock);
    xSemaphoreGive(s_erase_semphr);
    // Only run the callback with no locks held so that the callback won't
    // deadlock if it kicks off another sector erase.
    on_complete_cb(context, status);
    return 0;
  }
}

// Returns non-zero expected remaining time if the erase has not finished. If the erase
// has finished it will re-enable stop-mode, clear the in_progress flag and call the
// completed callback before returning 0.
static uint32_t prv_flash_erase_poll(void) {
  mutex_lock(s_flash_lock);
  status_t status = flash_impl_get_erase_status();
  bool erase_finished;
  struct FlashEraseContext saved_ctx = s_erase;
  switch (status) {
    case E_BUSY:
    case E_AGAIN:
      erase_finished = false;
      break;
    case S_SUCCESS:
    default:
      // Success or failure; the erase has finished either way.
      erase_finished = true;
      break;
  }

  if (erase_finished) {
    stop_mode_enable(InhibitorFlash);
    s_erase.in_progress = false;
    flash_impl_clear_nvram_erase_status();
  }
  mutex_unlock(s_flash_lock);

  if (!erase_finished) {
    return s_erase.expected_duration / 8;
  }

  xSemaphoreGive(s_erase_semphr);
  if (status == E_ERROR && saved_ctx.retries < MAX_ERASE_RETRIES) {
    // Try issuing the erase again. It might succeed this time around.
    PBL_LOG(LOG_LEVEL_DEBUG, "Erase of 0x%"PRIx32" failed (attempt %d)."
            " Trying again...", saved_ctx.address, saved_ctx.retries);
    return prv_flash_erase_start(
        saved_ctx.address, saved_ctx.on_complete_cb, saved_ctx.cb_context,
        saved_ctx.is_subsector, saved_ctx.retries + 1);
  } else {
    // Only run the callback with no locks held so that the callback won't
    // deadlock if it kicks off another sector erase.
    saved_ctx.on_complete_cb(saved_ctx.cb_context, status);
    return 0;
  }
}

// Timer callback that checks to see if the erase has finished. Used by the non-blocking
// routines.
static void prv_flash_erase_timer_cb(void *context) {
  uint32_t remaining_ms = prv_flash_erase_poll();
  if (remaining_ms) {
    // Erase is in progress or suspended; poll again later.
    new_timer_start(s_erase_poll_timer, remaining_ms, prv_flash_erase_timer_cb, NULL, 0);
  }
}

static void prv_flash_erase_async(
    uint32_t sector_addr, bool is_subsector, FlashOperationCompleteCb on_complete_cb,
    void *context) {
  uint32_t remaining_ms = prv_flash_erase_start(sector_addr, on_complete_cb,
                                                context, is_subsector, 0);
  if (remaining_ms) {
    // Start timer that will periodically check for the erase to complete
    new_timer_start(s_erase_poll_timer, remaining_ms, prv_flash_erase_timer_cb, NULL, 0);
  }
}

static void prv_blocking_erase_complete(void *context, status_t status) {
  PBL_ASSERT(PASSED(status), "Flash erase failure: %" PRId32, status);
}

static void prv_flash_erase_blocking(uint32_t sector_addr, bool is_subsector) {
  uint32_t total_time_spent_waiting_ms = 0;

  uint32_t remaining_ms = prv_flash_erase_start(
      sector_addr, prv_blocking_erase_complete, NULL, is_subsector, 0);
  while (remaining_ms) {
    psleep(remaining_ms);
    total_time_spent_waiting_ms += remaining_ms;

    remaining_ms = prv_flash_erase_poll();

    // check to see if the cb responsible for resuming erases should have run
    // but is blocked. See PBL-25741 for details
    uint32_t erase_suspend_time_remaining;
    if (new_timer_scheduled(s_erase_suspend_timer, &erase_suspend_time_remaining) &&
        (erase_suspend_time_remaining == 0)) {
      prv_erase_suspend_timer_cb(NULL);
    }


    // An erase can take a long time, especially if the erase needs to be
    // retried. Appease the watchdog so that it doesn't get angry when an
    // erase takes >6 seconds.
    //
    // After a certain amount of time passes, stop kicking the watchdog. This is to handle a case
    // where the erase never finishes or takes an unheard of amount of time to complete. Just let
    // the watchdog kill us in this case.
    static const uint32_t FLASH_ERASE_BLOCKING_TIMEOUT_MS = 5000;
    if (total_time_spent_waiting_ms < FLASH_ERASE_BLOCKING_TIMEOUT_MS) {
#if IS_BIGBOARD
      // Our bigboards have had a hard life and they have some fairly abused flash chips, and we
      // run into 5+ second erases pretty regularly. We're not holding the flash lock while we're
      // doing this, so other threads are allowed to use flash, but it's pretty common to hold
      // other locks while waiting for a flash operation to complete, leading to other tasks
      // triggering their task watchdogs before this erase completes. Let's kick all watchdogs
      // instead. The downside to this is that it may take us longer to detect another thread is
      // stuck, but we should still detect it eventually as long as we're not constantly erasing.
      task_watchdog_bit_set_all();
#else
      // Just kick the watchdog for the current task. This should give us more accurate watchdog
      // behaviour and sealed watches haven't been abused as much and shouldn't have extremely
      // long erase problems.
      task_watchdog_bit_set(pebble_task_get_current());
#endif
    }
  }
}

void flash_erase_sector(uint32_t sector_addr,
                        FlashOperationCompleteCb on_complete_cb,
                        void *context) {
  prv_flash_erase_async(sector_addr, false /* is_subsector */, on_complete_cb, context);
}

void flash_erase_subsector(uint32_t sector_addr,
                           FlashOperationCompleteCb on_complete_cb,
                           void *context) {
  prv_flash_erase_async(sector_addr, true /* is_subsector */, on_complete_cb, context);
}

void flash_erase_sector_blocking(uint32_t sector_addr) {
  prv_flash_erase_blocking(sector_addr, false /* is_subsector */);
}

void flash_erase_subsector_blocking(uint32_t subsector_addr) {
  prv_flash_erase_blocking(subsector_addr, true /* is_subsector */);
}

void flash_enable_write_protection(void) {
  flash_impl_enable_write_protection();
}

void flash_prf_set_protection(bool do_protect) {
  status_t status;
  mutex_lock(s_flash_lock);
  if (do_protect) {
    status = flash_impl_write_protect(
        FLASH_REGION_SAFE_FIRMWARE_BEGIN,
        (FLASH_REGION_SAFE_FIRMWARE_END - SECTOR_SIZE_BYTES));
  } else {
    status = flash_impl_unprotect();
  }
  PBL_ASSERT(PASSED(status), "flash_prf_set_protection failed: %" PRId32, status);
  mutex_unlock(s_flash_lock);
}

#if 0
void flash_erase_bulk(void) {
  mutex_lock(s_flash_lock);
  flash_impl_erase_bulk_begin();
  while (flash_impl_erase_is_in_progress()) {
    psleep(10);
  }
  mutex_unlock(s_flash_lock);
}
#endif

void flash_sleep_when_idle(bool enable) {
  // the S29VS flash automatically enters and exits standby
}

bool flash_get_sleep_when_idle(void) {
  return false;
}

bool flash_is_initialized(void) {
  return (s_flash_lock != NULL);
}

void flash_stop(void) {
  if (!flash_is_initialized()) {
    // Not yet initialized, nothing to do.
    return;
  }

  mutex_lock(s_flash_lock);
  if (s_erase.in_progress) {
    new_timer_stop(s_erase_suspend_timer);
    prv_erase_resume();
    mutex_unlock(s_flash_lock);
    while (__atomic_load_n(&s_erase.in_progress, __ATOMIC_SEQ_CST)) {
      psleep(10);
    }
  }
}

void flash_switch_mode(FlashModeType mode) {
  mutex_lock(s_flash_lock);
  flash_impl_set_burst_mode(mode == FLASH_MODE_SYNC_BURST);
  mutex_unlock(s_flash_lock);
}

uint32_t flash_get_sector_base_address(uint32_t flash_addr) {
  return flash_impl_get_sector_base_address(flash_addr);
}

uint32_t flash_get_subsector_base_address(uint32_t flash_addr) {
  return flash_impl_get_subsector_base_address(flash_addr);
}

void flash_power_down_for_stop_mode(void) {
  flash_impl_enter_low_power_mode();
}

void flash_power_up_after_stop_mode(void) {
  flash_impl_exit_low_power_mode();
}

bool flash_sector_is_erased(uint32_t sector_addr) {
  return flash_impl_blank_check_sector(flash_impl_get_sector_base_address(sector_addr));
}

bool flash_subsector_is_erased(uint32_t sector_addr) {
  return flash_impl_blank_check_subsector(flash_impl_get_subsector_base_address(sector_addr));
}

void flash_use(void) {
  mutex_lock(s_flash_lock);
  flash_impl_use();
  mutex_unlock(s_flash_lock);
}

void flash_release_many(uint32_t num_locks) {
  mutex_lock(s_flash_lock);
  flash_impl_release_many(num_locks);
  mutex_unlock(s_flash_lock);
}

#include "console/prompt.h"
void command_flash_unprotect(void) {
  flash_impl_unprotect();
  prompt_send_response("OK");
}
