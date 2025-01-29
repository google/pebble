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

#undef PBL_LOGS_HASHED

#include "applib/app.h"
#include "applib/ui/ui.h"
#include "kernel/pbl_malloc.h"
#include "kernel/util/sleep.h"
#include "process_management/pebble_process_md.h"
#include "process_management/pebble_process_md.h"
#include "process_state/app_state/app_state.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/regular_timer.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"

#define NUM_MAX_TIMERS 10


// =================================================================================
// Application Data
typedef struct {
  Window *window;
  SimpleMenuLayer *menu_layer;

  TimerID timer[NUM_MAX_TIMERS];
  RtcTicks fired_time[NUM_MAX_TIMERS];

  RegularTimerInfo reg_timers[NUM_MAX_TIMERS];
  
  AppTimer *app_timer;
  
} TestTimersAppData;

static TestTimersAppData *s_app_data = 0;


// =================================================================================
static void timer_callback(void* data)
{
  int idx = (int)data;
  PBL_ASSERTN(idx >= 0 && idx < NUM_MAX_TIMERS);
  PBL_LOG(LOG_LEVEL_DEBUG, "STT normal callback %d executed", idx);
  s_app_data->fired_time[idx] = rtc_get_ticks();
  return;
}


// =================================================================================
static void stuck_timer_callback(void* data)
{
  PBL_LOG(LOG_LEVEL_DEBUG, "STT entering infinite loop in callback");
  while (true) {
    psleep(100);
  }
}


// =================================================================================
static void long_timer_callback(void* data)
{
  int idx = (int)data;
  PBL_ASSERTN(idx >= 0 && idx < NUM_MAX_TIMERS);
  PBL_LOG(LOG_LEVEL_DEBUG, "STT long running callback %d executed", idx);
  s_app_data->fired_time[idx] = rtc_get_ticks();

  psleep(250);
  return;
}


// =================================================================================
// Try and reschedule a regular timer from it's callback
static void reg_timer_1_callback(void* data) {
  PBL_LOG(LOG_LEVEL_DEBUG, "STT running reg_timer_1_callback");
  if (s_app_data->reg_timers[0].cb != 0) {
    PBL_LOG(LOG_LEVEL_DEBUG, "STT reg_timer_1_callback rescheduling from callback for every 2 secs. ");
    regular_timer_add_multisecond_callback(&s_app_data->reg_timers[0], 2);
  }
}


// =================================================================================
// Try and delete a regular timer from it's callback
static void reg_timer_2_callback(void* data) {
  PBL_LOG(LOG_LEVEL_DEBUG, "STT running reg_timer_2_callback");
  if (s_app_data->reg_timers[0].cb != 0) {
    PBL_LOG(LOG_LEVEL_DEBUG, "STT reg_timer_2_callback deleting from callback");
    regular_timer_remove_callback(&s_app_data->reg_timers[0]);
  }
}


// =================================================================================
// Try and delete, then re-add a regular timer from its callback
static int s_reg_timer_3_callback_count = 0;
static void reg_timer_3_callback(void* data) {
  s_reg_timer_3_callback_count++;
  PBL_LOG(LOG_LEVEL_DEBUG, "STT running reg_timer_3_callback");
  if (s_app_data->reg_timers[0].cb != 0) {
    PBL_LOG(LOG_LEVEL_DEBUG, "STT reg_timer_3_callback deleting then adding from callback");
    regular_timer_remove_callback(&s_app_data->reg_timers[0]);
    regular_timer_add_seconds_callback(&s_app_data->reg_timers[0]);
  }
}


// =================================================================================
static void menu_callback_prefix(int index, void *ctx) {
  PBL_LOG(LOG_LEVEL_DEBUG, "Hit menu item %d", index);

  // Mark the layer to be updated
  layer_mark_dirty(simple_menu_layer_get_layer(s_app_data->menu_layer));

  // Cancel and delete old timers if present
  for (int i=0; i<NUM_MAX_TIMERS; i++) {
    s_app_data->fired_time[i] = 0;
    if (s_app_data->timer[i] != TIMER_INVALID_ID) {
      PBL_LOG(LOG_LEVEL_DEBUG, "STT stopping and deleting previous timer %d", i);
      new_timer_stop(s_app_data->timer[i]);
      new_timer_delete(s_app_data->timer[i]);
      s_app_data->timer[i] = TIMER_INVALID_ID;
    }
  }

  if (s_app_data->app_timer != NULL) {
    app_timer_cancel(s_app_data->app_timer);
    s_app_data->app_timer = 0;
  }

  // Cancel and delete old regular timers if present
  for (int i=0; i<NUM_MAX_TIMERS; i++) {
    if (s_app_data->reg_timers[i].cb != NULL) {
      PBL_LOG(LOG_LEVEL_DEBUG, "STT deleting previous regular timer %d", i);
      regular_timer_remove_callback(&s_app_data->reg_timers[i]);
      s_app_data->reg_timers[i].cb = NULL;
    }
  }

}


// =================================================================================
void single_shot_timer_menu_cb(int index, void *ctx) {
  uint32_t zero_flags = 0;
  uint32_t expire_ms;
  int timer_idx_0 = 0;
  bool scheduled = false;

  menu_callback_prefix(index, ctx);

  // Single shot timer
  s_app_data->timer[0] = new_timer_create();
  bool success = new_timer_start(s_app_data->timer[timer_idx_0], 100, timer_callback, (void*)timer_idx_0, zero_flags);
  PBL_ASSERTN(success);

  // Make sure it's marked as scheduled
  scheduled = new_timer_scheduled(s_app_data->timer[timer_idx_0], &expire_ms);
  PBL_ASSERTN(scheduled && expire_ms <= 100);
  PBL_LOG(LOG_LEVEL_DEBUG, "STT firing in %d ms", (int)expire_ms);

  // Wait for it to fire
  psleep(300);
  PBL_ASSERTN(s_app_data->fired_time[timer_idx_0] != 0);
  scheduled = new_timer_scheduled(s_app_data->timer[timer_idx_0], &expire_ms);
  PBL_ASSERTN(!scheduled);
}


// =================================================================================
void repeating_timer_menu_cb(int index, void *ctx) {
  uint32_t expire_ms;
  int timer_idx_0 = 0;
  bool scheduled = false;

  menu_callback_prefix(index, ctx);
  
  // Repeating timer
  s_app_data->timer[timer_idx_0] = new_timer_create(); 
  bool success = new_timer_start(s_app_data->timer[timer_idx_0], 500, timer_callback, (void*)timer_idx_0, 
                                 TIMER_START_FLAG_REPEATING);
  PBL_ASSERTN(success);
  
  scheduled = new_timer_scheduled(s_app_data->timer[timer_idx_0], &expire_ms);
  PBL_ASSERTN(scheduled && expire_ms <= 500);
  PBL_LOG(LOG_LEVEL_DEBUG, "STT firing in %d ms", (int)expire_ms);
}

// =================================================================================
void two_timers_menu_cb(int index, void *ctx) {
  uint32_t zero_flags = 0;
  uint32_t expire_ms;

  menu_callback_prefix(index, ctx);
  
  // Multiple timers
  int timer_idx_0 = 0;
  s_app_data->timer[timer_idx_0] = new_timer_create();
  bool success = new_timer_start(s_app_data->timer[timer_idx_0], 300, timer_callback, (void*)timer_idx_0, zero_flags);
  PBL_ASSERTN(success);

  int timer_idx_1 = 1;
  s_app_data->timer[timer_idx_1] = new_timer_create();
  success = new_timer_start(s_app_data->timer[timer_idx_1], 100, timer_callback, (void*)timer_idx_1, zero_flags);
  PBL_ASSERTN(success);

  
  // Wait for them to fire
  psleep(500);
  PBL_ASSERTN(s_app_data->fired_time[timer_idx_0] != 0);
  PBL_ASSERTN(s_app_data->fired_time[timer_idx_1] != 0);
  PBL_ASSERTN(s_app_data->fired_time[timer_idx_1] < s_app_data->fired_time[timer_idx_0]);
  bool scheduled = new_timer_scheduled(s_app_data->timer[timer_idx_0], &expire_ms);
  PBL_ASSERTN(!scheduled);
  scheduled = new_timer_scheduled(s_app_data->timer[timer_idx_1], &expire_ms);
  PBL_ASSERTN(!scheduled);
}

// =================================================================================
void deferred_delete_menu_cb(int index, void *ctx) {
  void *cb_data = 0;
  uint32_t zero_flags = 0;

  menu_callback_prefix(index, ctx);
  
  // Deferred delete
  s_app_data->timer[0] = new_timer_create();
  bool success = new_timer_start(s_app_data->timer[0], 1, long_timer_callback, cb_data, zero_flags);
  PBL_ASSERTN(success);
  psleep(50);
  
  // Stop and then delete it
  success = new_timer_stop(s_app_data->timer[0]);
  PBL_ASSERTN(!success);    /* stop returns false if callback is running */
  new_timer_delete(s_app_data->timer[0]);
  s_app_data->timer[0] = TIMER_INVALID_ID;
}

// =================================================================================
void fail_if_executing_menu_cb(int index, void *ctx) {
  void *cb_data = 0;
  uint32_t zero_flags = 0;

  menu_callback_prefix(index, ctx);

  // fail if executing
  s_app_data->timer[0] = new_timer_create();
  bool success = new_timer_start(s_app_data->timer[0], 1, long_timer_callback, cb_data, zero_flags);
  PBL_ASSERTN(success);
  psleep(50);
  
  // try and reschedule while it's executing
  success = new_timer_start(s_app_data->timer[0], 1, long_timer_callback, cb_data, 
                                 TIMER_START_FLAG_FAIL_IF_EXECUTING);
  PBL_ASSERTN(!success);
}

// =================================================================================
void fail_if_scheduled_menu_cb(int index, void *ctx) {
  void *cb_data = 0;
  uint32_t zero_flags = 0;

  menu_callback_prefix(index, ctx);

  // fail if scheduled
  s_app_data->timer[0] = new_timer_create();
  bool success = new_timer_start(s_app_data->timer[0], 100, timer_callback, cb_data, 
                                 zero_flags);
  PBL_ASSERTN(success);
  
  // try and reschedule while it's already scheduled
  success = new_timer_start(s_app_data->timer[0], 1, timer_callback, cb_data, 
                            TIMER_START_FLAG_FAIL_IF_SCHEDULED);
  PBL_ASSERTN(!success);
  
}

// =================================================================================
void evented_timer_menu_cb(int index, void *ctx) {
  void *cb_data = 0;

  menu_callback_prefix(index, ctx);

  // Test evented_timer
  s_app_data->app_timer = app_timer_register(100 /*ms*/, timer_callback, cb_data);
  PBL_ASSERTN(s_app_data->app_timer);
}

// =================================================================================
void stuck_callback_menu_cb(int index, void *ctx) {
  void *cb_data = 0;
  uint32_t zero_flags = 0;

  menu_callback_prefix(index, ctx);

  // stuck callback
  s_app_data->timer[0] = new_timer_create(); 
  bool success = new_timer_start(s_app_data->timer[0], 100, stuck_timer_callback, cb_data, zero_flags);
  PBL_ASSERTN(success);
}

// =================================================================================
void invalid_timer_id_menu_cb(int index, void *ctx) {
  void *cb_data = 0;
  uint32_t zero_flags = 0;

  menu_callback_prefix(index, ctx);

  // invalid timer id
  new_timer_start(0x12345678, 100, timer_callback, cb_data, zero_flags);
}


// =================================================================================
void reg_timer_schedule_1sec_from_cb_menu_cb(int index, void *ctx) {
  menu_callback_prefix(index, ctx);

  s_app_data->reg_timers[0].cb = reg_timer_1_callback;
  regular_timer_add_seconds_callback(&s_app_data->reg_timers[0]);
}

// =================================================================================
void reg_timer_schedule_1min_from_cb_menu_cb(int index, void *ctx) {
  menu_callback_prefix(index, ctx);

  s_app_data->reg_timers[0].cb = reg_timer_1_callback;
  regular_timer_add_minutes_callback(&s_app_data->reg_timers[0]);

  // This should assert when the callback runs because it tries to reschedule as a seconds callback
}

// =================================================================================
void reg_timer_delete_from_cb_menu_cb(int index, void *ctx) {
  menu_callback_prefix(index, ctx);

  s_app_data->reg_timers[0].cb = reg_timer_2_callback;
  regular_timer_add_seconds_callback(&s_app_data->reg_timers[0]);
}

// =================================================================================
void reg_timer_delete_then_add_from_cb_menu_cb(int index, void *ctx) {
  menu_callback_prefix(index, ctx);

  s_reg_timer_3_callback_count = 0;
  s_app_data->reg_timers[0].cb = reg_timer_3_callback;
  regular_timer_add_seconds_callback(&s_app_data->reg_timers[0]);

  // Wait for timer to run at least twice
  PBL_LOG(LOG_LEVEL_DEBUG, "waiting for callback to run 2 times");
  psleep(2200);

  PBL_ASSERT(s_reg_timer_3_callback_count >= 2, "Callback didn't run at least twice");
}

// =================================================================================
void croak_menu_cb(int index, void *ctx) {

  menu_callback_prefix(index, ctx);
  PBL_CROAK("DIE!");
}

// =================================================================================
static void prv_window_load(Window *window) {
  TestTimersAppData *data = s_app_data;

  static const SimpleMenuItem menu_items[] = {
    {
      .title = "single-shot timer",
      .callback = single_shot_timer_menu_cb
    }, {
      .title = "repeating timer",
      .callback = repeating_timer_menu_cb
    }, {
      .title = "two timers",
      .callback = two_timers_menu_cb
    }, {
      .title = "deferred delete",
      .callback = deferred_delete_menu_cb
    }, {
      .title = "fail if executing",
      .callback = fail_if_executing_menu_cb
    }, {
      .title = "fail if scheduled",
      .callback = fail_if_scheduled_menu_cb
    }, {
      .title = "evented_timer",
      .callback = evented_timer_menu_cb
    }, {
      .title = "stuck callback",
      .callback = stuck_callback_menu_cb
    }, {
      .title = "invalid timer ID",
      .callback = invalid_timer_id_menu_cb
    }, {
      .title = "RT: sch 1 sec from cb",
      .callback = reg_timer_schedule_1sec_from_cb_menu_cb
    }, {
      .title = "RT: sch 1 min from cb",
      .callback = reg_timer_schedule_1min_from_cb_menu_cb
    }, {
      .title = "RT: delete from cb",
      .callback = reg_timer_delete_from_cb_menu_cb
    }, {
      .title = "RT: delete+add from cb",
      .callback = reg_timer_delete_then_add_from_cb_menu_cb
    }, {
      .title = "croak",
      .callback = croak_menu_cb
    }
  };
  static const SimpleMenuSection sections[] = {
    {
      .items = menu_items,
      .num_items = ARRAY_LENGTH(menu_items)
    }
  };

  Layer *window_layer = window_get_root_layer(data->window);
  GRect bounds = window_layer->bounds;
  
  data->menu_layer = simple_menu_layer_create(bounds, data->window, sections, ARRAY_LENGTH(sections), NULL);
  layer_add_child(window_layer, simple_menu_layer_get_layer(data->menu_layer));
}


// =================================================================================
// Deinitialize resources on window unload that were initialized on window load
static void prv_window_unload(Window *window) {
  simple_menu_layer_destroy(s_app_data->menu_layer);
}


// =================================================================================
static void handle_init(void) {
  TestTimersAppData *data = app_malloc_check(sizeof(TestTimersAppData));
  memset(data, 0, sizeof(TestTimersAppData));
  s_app_data = data;

  data->window = window_create();
  if (data->window == NULL) {
    return;
  }
  window_init(data->window, "");
  window_set_window_handlers(data->window, &(WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  app_window_stack_push(data->window, true /*animated*/);
}

static void handle_deinit(void) {
  // Don't bother freeing anything, the OS should be re-initing the heap.
}


// =================================================================================
static void s_main(void) {
  handle_init();
  app_event_loop();
  handle_deinit();
}

// =================================================================================
const PebbleProcessMd* test_sys_timer_app_get_info() {
  static const PebbleProcessMdSystem s_app_info = {
    .common.main_func = &s_main,
    .name = "System Timer Test"
  };
  return (const PebbleProcessMd*) &s_app_info;
}

