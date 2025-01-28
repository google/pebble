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

#include "vibe_pattern.h"

#include "drivers/vibe.h"
#include "drivers/battery.h"

#include "util/list.h"
#include "util/math.h"

#include "os/mutex.h"

#include "services/common/accel_manager.h"
#include "services/common/new_timer/new_timer.h"
#include "kernel/events.h"

#include "kernel/pbl_malloc.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "system/passert.h"

#include <stddef.h>

typedef struct {
  ListNode list_node;
  uint64_t time_start;
  uint64_t time_end;
} VibeHistory;

// The maximum history we need to keep is based on the maximum time between accel samples (the
// lowest sampling rate) in milliseconds and the maximum number of accel samples per update.
#define MAX_HISTORY_MS (ACCEL_MAX_SAMPLES_PER_UPDATE * 1000 / ACCEL_MINIMUM_SAMPLING_RATE)
#define END_NOT_SET 0
#define HISTORY_CLEAR_ALL 0

static PebbleMutex *s_vibe_history_mutex = NULL;
static VibeHistory *s_vibe_history = NULL;
static bool s_vibe_history_enabled = false;
static bool s_vibe_service_enabled = true;

DEFINE_SYSCALL(bool, sys_vibe_history_was_vibrating, uint64_t time_search) {
  bool rc = false;

  PBL_ASSERTN(s_vibe_history_mutex);
  mutex_lock(s_vibe_history_mutex);
  VibeHistory *node = s_vibe_history;
  while (node) {
    if (node->time_end == END_NOT_SET && time_search >= node->time_start) {
      rc = true;
      break;
    }
    if (time_search >= node->time_start && time_search <= node->time_end) {
      rc = true;
      break;
    }
    node = (VibeHistory*)list_get_next((ListNode*)node);
  }
  mutex_unlock(s_vibe_history_mutex);
  return rc;
}

// @param cutoff The time to cut off the list at. 0 means to clear the list.
static void prv_vibe_history_clear(uint64_t cutoff) {
  PBL_ASSERTN(s_vibe_history_mutex);
  mutex_assert_held_by_curr_task(s_vibe_history_mutex, true);
  while (s_vibe_history && s_vibe_history->time_end != END_NOT_SET) {
    VibeHistory *vibe = s_vibe_history;
    if (cutoff != HISTORY_CLEAR_ALL && vibe->time_end >= cutoff) {
      break;
    }
    s_vibe_history = (VibeHistory*)list_get_next((ListNode*)vibe);
    kernel_free(vibe);
  }
}

DEFINE_SYSCALL(void, sys_vibe_history_start_collecting, void) {
  s_vibe_history_enabled = true;
}

DEFINE_SYSCALL(void, sys_vibe_history_stop_collecting, void) {
  s_vibe_history_enabled = false;
  mutex_lock(s_vibe_history_mutex);
  prv_vibe_history_clear(HISTORY_CLEAR_ALL);
  mutex_unlock(s_vibe_history_mutex);
}

static void prv_vibe_history_start_event(void) {
  if (!s_vibe_history_enabled) {
    return;
  }
  VibeHistory *vibe = kernel_malloc(sizeof(VibeHistory));
  if (vibe == NULL) {
    s_vibe_history_enabled = false;
    return;
  }
  list_init((ListNode*)vibe);
  time_t s;
  uint16_t ms;
  rtc_get_time_ms(&s, &ms);
  vibe->time_start = ((uint64_t)s) * 1000 + ms;
  vibe->time_end = END_NOT_SET;

  PBL_ASSERTN(s_vibe_history_mutex);
  mutex_lock(s_vibe_history_mutex);
  if (s_vibe_history == NULL) {
    s_vibe_history = vibe;
  } else {
    list_append((ListNode*)s_vibe_history, (ListNode*)vibe);
  }
  prv_vibe_history_clear(vibe->time_start - MAX_HISTORY_MS);
  mutex_unlock(s_vibe_history_mutex);
}

// Ends the last vibration event
static void prv_vibe_history_end_event(void) {
  if (!s_vibe_history_enabled) {
    return;
  }
  if (!s_vibe_history) {
    // Possible that it was enabled while the watch was vibrating
    return;
  }

  time_t s;
  uint16_t ms;
  rtc_get_time_ms(&s, &ms);

  mutex_lock(s_vibe_history_mutex);
  VibeHistory *vibe = (VibeHistory*)list_get_tail((ListNode*)s_vibe_history);
  if (vibe->time_end == END_NOT_SET) {
    vibe->time_end = ((uint64_t)s) * 1000 + ms;
  }
  mutex_unlock(s_vibe_history_mutex);
}

typedef struct {
  ListNode list_node;
  uint32_t duration_ms;
  int32_t  strength;
} VibePatternStep;

static const uint32_t MAX_VIBE_DURATION_MS = 10000;

static int s_pattern_timer = TIMER_INVALID_ID;
static bool s_pattern_in_progress = false;
// s_vibe_strength is the current vibration strength setting of the motor
static int32_t s_vibe_strength = VIBE_STRENGTH_OFF;
// s_vibe_strength_default is the vibrations strength of the motor used when one is not specified
// explicitly, and can be changed in the notification vibration strength setting.
static int32_t s_vibe_strength_default = VIBE_STRENGTH_MAX;

static PebbleMutex *s_vibe_pattern_mutex = NULL;
static VibePatternStep *s_vibe_queue_head = NULL;

void vibes_init() {
  s_vibe_history_mutex = mutex_create();
  s_vibe_pattern_mutex = mutex_create();
  s_pattern_in_progress = false;
  s_pattern_timer = new_timer_create();
}

//! Turn the vibe motor on or off.
//!
//! This function should be used instead of vibe_ctl so that the vibe
//! history is kept in sync with the vibe state.
//! The caller must be holding s_vibe_pattern_mutex
static void prv_vibes_set_vibe_strength(int32_t new_strength) {
  mutex_assert_held_by_curr_task(s_vibe_pattern_mutex, true);
  if (!s_vibe_service_enabled) {
    PBL_ASSERTN(s_vibe_strength == VIBE_STRENGTH_OFF);
    return;
  }
  if (new_strength != VIBE_STRENGTH_OFF) {
    vibe_set_strength(new_strength);
    vibe_ctl(true /* on */);
    if (s_vibe_strength == VIBE_STRENGTH_OFF) {
      prv_vibe_history_start_event();
    }
  } else {
    vibe_ctl(false /* on */);
    if (s_vibe_strength != VIBE_STRENGTH_OFF) {
      prv_vibe_history_end_event();
    }
  }
  s_vibe_strength = new_strength;
}

void vibe_service_set_enabled(bool enable) {
  mutex_lock(s_vibe_pattern_mutex);
  if (enable != s_vibe_service_enabled) {
    // ensure that the vibe is off before disabling it. No op if enabling it
    prv_vibes_set_vibe_strength(VIBE_STRENGTH_OFF);
    s_vibe_service_enabled = enable;
  }
  mutex_unlock(s_vibe_pattern_mutex);
}

static void prv_timer_callback(void* data) {
  if (s_vibe_queue_head == NULL) {
    PBL_LOG(LOG_LEVEL_ERROR, "Tried to handle a vibe event with a null vibe queue");
    return;
  }

  mutex_lock(s_vibe_pattern_mutex);

  // remove the event I've finished
  VibePatternStep *removed_node = s_vibe_queue_head;
  s_vibe_queue_head = (VibePatternStep*)list_pop_head((ListNode*)s_vibe_queue_head);
  kernel_free(removed_node);

  if (s_vibe_queue_head != NULL) {
    // move to the next step
    prv_vibes_set_vibe_strength(s_vibe_queue_head->strength);
    bool success = new_timer_start(s_pattern_timer, s_vibe_queue_head->duration_ms,
                                   prv_timer_callback, NULL, 0 /*flags*/);
    PBL_ASSERTN(success);
  } else {
    // I'm done with the active pattern
    // make sure it's off
    prv_vibes_set_vibe_strength(VIBE_STRENGTH_OFF);
    s_pattern_in_progress = false;
  }

  mutex_unlock(s_vibe_pattern_mutex);
}

int32_t vibes_get_vibe_strength(void) {
  return s_vibe_strength;
}

int32_t vibes_get_default_vibe_strength(void) {
  return s_vibe_strength_default;
}

void vibes_set_default_vibe_strength(int32_t vibe_strength_default) {
  s_vibe_strength_default = vibe_strength_default;
}

DEFINE_SYSCALL(int32_t, sys_vibe_get_vibe_strength, void) {
  return vibes_get_vibe_strength();
}

bool prv_vibe_pattern_enqueue_step_raw(uint32_t duration_ms, int32_t strength) {
  mutex_lock(s_vibe_pattern_mutex);

  if (s_pattern_in_progress) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Pattern is in progress");
    mutex_unlock(s_vibe_pattern_mutex);
    return false;
  }

  VibePatternStep *step = kernel_malloc(sizeof(VibePatternStep));
  if (step == NULL) {
    PBL_LOG(LOG_LEVEL_ERROR, "Couldn't malloc for a vibe step");
    mutex_unlock(s_vibe_pattern_mutex);
    return false;
  }

  list_init((ListNode*)step);
  step->duration_ms = MIN(duration_ms, MAX_VIBE_DURATION_MS);
  step->strength = strength;

  if (s_vibe_queue_head == NULL) {
    s_vibe_queue_head = step;
  } else {
    list_append((ListNode*)s_vibe_queue_head, (ListNode*)step);
  }

  mutex_unlock(s_vibe_pattern_mutex);

  return true;
}

DEFINE_SYSCALL(bool, sys_vibe_pattern_enqueue_step_raw, uint32_t duration_ms, int32_t strength) {
  return prv_vibe_pattern_enqueue_step_raw(duration_ms, strength);
}

DEFINE_SYSCALL(bool, sys_vibe_pattern_enqueue_step, uint32_t duration_ms, bool on) {
  return prv_vibe_pattern_enqueue_step_raw(duration_ms, on ? s_vibe_strength_default
                                                           : VIBE_STRENGTH_OFF);
}

DEFINE_SYSCALL(void, sys_vibe_pattern_trigger_start, void) {
  mutex_lock(s_vibe_pattern_mutex);
  if (s_vibe_queue_head == NULL || s_pattern_in_progress) {
    // either no vibes queued or I've already started
    mutex_unlock(s_vibe_pattern_mutex);
    return;
  }

  if (pebble_task_get_current() == PebbleTask_App) {
    analytics_inc(ANALYTICS_APP_METRIC_VIBRATOR_ON_COUNT, AnalyticsClient_App);
  }

  prv_vibes_set_vibe_strength(s_vibe_queue_head->strength);
  s_pattern_in_progress = true;
  bool success = new_timer_start(s_pattern_timer, s_vibe_queue_head->duration_ms,
                                 prv_timer_callback, NULL, 0 /*flags*/);
  PBL_ASSERTN(success);
  mutex_unlock(s_vibe_pattern_mutex);
}

DEFINE_SYSCALL(void, sys_vibe_pattern_clear, void) {
  mutex_lock(s_vibe_pattern_mutex);
  while (s_vibe_queue_head) {
    VibePatternStep *removed_node = s_vibe_queue_head;
    s_vibe_queue_head = (VibePatternStep*)list_pop_head((ListNode*)s_vibe_queue_head);
    kernel_free(removed_node);
  }
  prv_vibes_set_vibe_strength(VIBE_STRENGTH_OFF);
  s_pattern_in_progress = false;
  mutex_unlock(s_vibe_pattern_mutex);
}
