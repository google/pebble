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

#include "services/common/regular_timer.h"

#include "os/mutex.h"
#include "services/common/new_timer/new_timer.h"
#include "system/logging.h"
#include "system/passert.h"

#include "FreeRTOS.h"
#include "portmacro.h"

//! Don't let users modify the list while callbacks are occurring.
static PebbleMutex * s_callback_list_semaphore = 0;

//! The timer we use
static TimerID s_timer_id = TIMER_INVALID_ID;

static ListNode s_seconds_callbacks;
static ListNode s_minutes_callbacks;

// Set to 90 seconds because we do eventually drift. Make it in the middle of a minute so we can
// be sure that it isn't due to drifting.
#define MISSING_MINUTE_CB_LOG_THRESHOLD_S 90
static time_t s_last_minute_fire_ts; // uses



// -------------------------------------------------------------------------------------------
// Passed to list_find() to determine if a callback is already registered or not
static bool prv_callback_registered_filter(ListNode *found_node, void *data) {
  return (found_node == (ListNode *)data);
}

// -------------------------------------------------------------------------------------------
static void do_callbacks(ListNode* list) {
  mutex_lock(s_callback_list_semaphore);

  for (ListNode* iter = list_get_next(list); iter != 0; ) {
    RegularTimerInfo* reg_timer = (RegularTimerInfo*) iter;

    if (--reg_timer->private_count == 0) {
      reg_timer->private_count = reg_timer->private_reset_count;

      // Release the mutex while we execute the callback
      reg_timer->is_executing = true;
      mutex_unlock(s_callback_list_semaphore);
      reg_timer->cb(reg_timer->cb_data);
      mutex_lock(s_callback_list_semaphore);
      reg_timer->is_executing = false;

      // Get the next one to execute before we possibly remove this one
      iter = list_get_next(iter);

      // Did the caller want to remove this one?
      // NOTE: We do not support callers that free the memory for the regular timer structure
      // from their callback procedure!
      if (reg_timer->pending_delete) {
        list_remove(&reg_timer->list_node, NULL, NULL);
      }

    } else {
      iter = list_get_next(iter);
    }
  }

  mutex_unlock(s_callback_list_semaphore);
}

// -------------------------------------------------------------------------------------------
static void timer_callback(void* data) {
  (void) data;

  do_callbacks(&s_seconds_callbacks);

  time_t t = rtc_get_time();
  struct tm time;
  localtime_r(&t, &time);
  if (time.tm_sec == 0) {
    // If the phone sets the time, we may actually skip a regular_timer minute callback. Let's
    // get an idea of how often this happens by logging
    const time_t now_ts = rtc_get_ticks() / configTICK_RATE_HZ;
    if ((now_ts - s_last_minute_fire_ts) > MISSING_MINUTE_CB_LOG_THRESHOLD_S) {
      PBL_LOG(LOG_LEVEL_WARNING,
              "Skipped a regular_timer_minute callback. Previous ts: %lu, Now ts: %lu",
              s_last_minute_fire_ts, now_ts);
    }
    s_last_minute_fire_ts = now_ts;

    do_callbacks(&s_minutes_callbacks);
  }
}

// -------------------------------------------------------------------------------------------
//! Used only once when we first start up. This should be really close to the 0ms point.
static void timer_callback_initializing(void* data) {
  // FIXME: FreeRTOS timers are subject to skew if something else is running on the millisecond.
  // We'll need to continuously adjust our timer period in really annoying ways.
  new_timer_start(s_timer_id, 1000, timer_callback, NULL, TIMER_START_FLAG_REPEATING);

  timer_callback(data);
}

// --------------------------------------------------------------------------------------------
void regular_timer_init(void) {
  PBL_ASSERTN(s_callback_list_semaphore == 0);

  s_callback_list_semaphore = mutex_create();

  time_t seconds;
  uint16_t milliseconds;
  rtc_get_time_ms(&seconds, &milliseconds);
  s_timer_id = new_timer_create();
  bool success = new_timer_start(s_timer_id, 1000-milliseconds, timer_callback_initializing, NULL, 0 /*flags*/);
  PBL_ASSERTN(success);
}

// -------------------------------------------------------------------------------------------
void regular_timer_add_multisecond_callback(RegularTimerInfo* cb, uint16_t seconds) {
  PBL_ASSERTN(s_callback_list_semaphore);

  mutex_lock(s_callback_list_semaphore);

  cb->private_reset_count = seconds;
  cb->private_count = seconds;

  // Only add to the list if not already registered
  if (!list_find(&s_seconds_callbacks, prv_callback_registered_filter, &cb->list_node)) {
    // better not be registered as a minute callback already
    PBL_ASSERTN(!list_find(&s_minutes_callbacks, prv_callback_registered_filter, &cb->list_node));
    cb->is_executing = false;
    cb->pending_delete = false;
    list_append(&s_seconds_callbacks, &cb->list_node);
  } else {
    // If it is marked for deletion, remove the deletion flag
    cb->pending_delete = false;
  }

  mutex_unlock(s_callback_list_semaphore);
}

// --------------------------------------------------------------------------------------------
void regular_timer_add_seconds_callback(RegularTimerInfo* cb) {
  // special case for triggering each second
  regular_timer_add_multisecond_callback(cb, 1);
}

// --------------------------------------------------------------------------------------------
void regular_timer_add_multiminute_callback(RegularTimerInfo* cb, uint16_t minutes) {
  PBL_ASSERTN(s_callback_list_semaphore);

  mutex_lock(s_callback_list_semaphore);

  cb->private_reset_count = minutes;
  cb->private_count = minutes;

  if (!list_find(&s_minutes_callbacks, prv_callback_registered_filter, &cb->list_node)) {
    // better not be registered as a minute callback already
    PBL_ASSERTN(!list_find(&s_seconds_callbacks, prv_callback_registered_filter, &cb->list_node));
    cb->is_executing = false;
    cb->pending_delete = false;
    list_append(&s_minutes_callbacks, &cb->list_node);
  } else {
    // If it is marked for deletion, remove the deletion flag
    cb->pending_delete = false;
  }

  mutex_unlock(s_callback_list_semaphore);
}

// -----------------------------------------------------------------------------------------
void regular_timer_add_minutes_callback(RegularTimerInfo* cb) {
  // special case for triggering each minute
  regular_timer_add_multiminute_callback(cb, 1);
}

// ------------------------------------------------------------------------------------------
static bool prv_regular_timer_is_scheduled(RegularTimerInfo *cb) {
  // Assumes mutex lock is already taken
  return (list_find(&s_seconds_callbacks, prv_callback_registered_filter, &cb->list_node) ||
          list_find(&s_minutes_callbacks, prv_callback_registered_filter, &cb->list_node));
}

// ------------------------------------------------------------------------------------------
bool regular_timer_is_scheduled(RegularTimerInfo *cb) {
  PBL_ASSERTN(s_callback_list_semaphore);

  mutex_lock(s_callback_list_semaphore);
  bool rv = prv_regular_timer_is_scheduled(cb);
  mutex_unlock(s_callback_list_semaphore);

  return (rv);
}

bool regular_timer_pending_deletion(RegularTimerInfo *cb) {
  return cb->pending_delete;
}

// ------------------------------------------------------------------------------------------
bool regular_timer_remove_callback(RegularTimerInfo* cb) {
  PBL_ASSERTN(s_callback_list_semaphore);
  bool timer_removed = false;
  mutex_lock(s_callback_list_semaphore);

  if (!prv_regular_timer_is_scheduled(cb)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Timer not registered");
  } else {
    // If currently executing, mark for deletion. do_callbacks will delete it for us once
    // it completes.
    if (cb->is_executing) {
      cb->pending_delete = true;
    } else {
      list_remove(&cb->list_node, NULL, NULL);
      timer_removed = true;
    }
  }

  mutex_unlock(s_callback_list_semaphore);
  return timer_removed;
}


// ---------------------------------------------------------------------------------------
// For Testing:

void regular_timer_deinit(void) {
  mutex_destroy((PebbleMutex *) s_callback_list_semaphore);
  s_callback_list_semaphore = NULL;
  new_timer_delete(s_timer_id);
  s_timer_id = TIMER_INVALID_ID;
}

static void prv_fire_callbacks(ListNode *list, uint16_t mod) {
  mutex_lock(s_callback_list_semaphore);
  ListNode* iter = list_get_next(list);
  while (iter) {
    RegularTimerInfo* reg_timer = (RegularTimerInfo*) iter;
    if (reg_timer->private_reset_count % mod == 0) {
      // Last one. Will trigger callback when do_callbacks() is called:
      reg_timer->private_count = 1;
    }
    iter = list_get_next(iter);
  }
  mutex_unlock(s_callback_list_semaphore);

  do_callbacks(list);
}

void regular_timer_fire_seconds(uint8_t secs) {
  prv_fire_callbacks(&s_seconds_callbacks, secs);
}

void regular_timer_fire_minutes(uint8_t mins) {
  prv_fire_callbacks(&s_minutes_callbacks, mins);
}

static uint32_t prv_count(ListNode *list) {
  uint32_t count = 0;
  mutex_lock(s_callback_list_semaphore);
  // -1, because s_..._callbacks is a ListNode too
  count = list_count(list) - 1;
  mutex_unlock(s_callback_list_semaphore);
  return count;
}

uint32_t regular_timer_seconds_count(void) {
  return prv_count(&s_seconds_callbacks);
}

uint32_t regular_timer_minutes_count(void) {
  return prv_count(&s_minutes_callbacks);
}
