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

#include "clar.h"

#include "applib/graphics/gtypes.h"
#include "applib/graphics/gtransform.h"
#include "applib/ui/layer.h"
#include "applib/ui/property_animation.h"
#include "applib/ui/property_animation_private.h"
#include "applib/ui/animation.h"
#include "applib/ui/animation_private.h"
#include "applib/legacy2/ui/animation_private_legacy2.h"
#include "util/math.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

///////////////////////////////////////////////////////////
// Stubs
#include "stubs_logging.h"
#include "stubs_passert.h"

#include "stubs_app_state.h"
#include "stubs_app_timer.h"
#include "stubs_freertos.h"
#include "stubs_heap.h"
#include "stubs_mutex.h"
#include "stubs_resources.h"
#include "stubs_serial.h"
#include "stubs_syscalls.h"
#include "stubs_unobstructed_area.h"

// Fakes
#include "fake_new_timer.h"
#include "fake_pebble_tasks.h"
#include "fake_pbl_malloc.h"
#include "fake_rtc.h"
#include "fake_events.h"

#define TEST_INCLUDE_BASIC
#define TEST_INCLUDE_COMPLEX

#define DEBUG_TEST
#ifdef DEBUG_TEST
#define DPRINTF(fmt, ...)                                       \
    do { printf("%s: " fmt , __func__, ## __VA_ARGS__); \
    } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

// Use our own macro instead of the abs function to avoid issues with non-int values.
#define ABSOLUTE_VALUE(x) (((x) > 0) ? (x) : -(x))

#define MIN_FRAME_INTERVAL_MS  33

#define TEST_ANIMATION_NORMALIZED_HIGH 50000
#define TEST_ANIMATION_NORMALIZED_LOW  5000

static PebbleEvent s_last_event;

bool process_manager_send_event_to_process(PebbleTask task, PebbleEvent* e) {
  s_last_event = *e;
  return true;
}

bool process_manager_compiled_with_legacy2_sdk(void) {
  return false;
}

GDrawState graphics_context_get_drawing_state(GContext *ctx) {
  return (GDrawState) { };
}

bool graphics_release_frame_buffer(GContext *ctx, GBitmap *buffer) {
  return false;
}

void graphics_context_set_drawing_state(GContext *ctx, GDrawState draw_state) {
}

void window_schedule_render(struct Window *window) {
}

TimerID animation_service_test_get_timer_id(void);


// --------------------------------------------------------------------------------------
static void cl_assert_equal_point(const GPoint a, const GPoint b) {
  cl_assert_equal_i(a.x, b.x);
  cl_assert_equal_i(a.y, b.y);
}


// --------------------------------------------------------------------------------------
static void cl_assert_equal_rect(const GRect a, const GRect b) {
  cl_assert_equal_point(a.origin, b.origin);
  cl_assert_equal_i(a.size.w, b.size.w);
  cl_assert_equal_i(a.size.h, b.size.h);
}


// --------------------------------------------------------------------------------------
static void cl_assert_equal_gtransform(const GTransform a, const GTransform b) {
  cl_assert_equal_i(a.a.raw_value, b.a.raw_value);
  cl_assert_equal_i(a.b.raw_value, b.b.raw_value);
  cl_assert_equal_i(a.c.raw_value, b.c.raw_value);
  cl_assert_equal_i(a.d.raw_value, b.d.raw_value);
  cl_assert_equal_i(a.tx.raw_value, b.tx.raw_value);
  cl_assert_equal_i(a.ty.raw_value, b.ty.raw_value);
}

static void cl_assert_close_gtransform(const GTransform a, const GTransform b) {
  cl_assert(abs(a.a.integer - b.a.integer) < 10);
  cl_assert(abs(a.b.integer - b.b.integer) < 10);
  cl_assert(abs(a.c.integer - b.c.integer) < 10);
  cl_assert(abs(a.d.integer - b.d.integer) < 10);
  cl_assert(abs(a.tx.integer - b.tx.integer) < 10);
  cl_assert(abs(a.ty.integer - b.ty.integer) < 10);
}


// --------------------------------------------------------------------------------------
static void cl_assert_equal_fixed_s32_16(const Fixed_S32_16 a, const Fixed_S32_16 b) {
  cl_assert_equal_i(a.raw_value, b.raw_value);
}

static void cl_assert_close_fixed_s32_16(const Fixed_S32_16 a, const Fixed_S32_16 b) {
  cl_assert(abs(a.integer - b.integer) < 10);
}


// --------------------------------------------------------------------------------------
// Get current time in ms.
static uint64_t prv_now_ms(void) {
  RtcTicks ticks = rtc_get_ticks();
  return (ticks * 1000 + RTC_TICKS_HZ / 2) / RTC_TICKS_HZ;
}

// --------------------------------------------------------------------------------------
// Advance current time by N ms. This does NOT check to see if the timer should fire
// If you want to advance time and fire all timers that would have fired during that time
// call prv_advance_to_ms_with_timers()
static void prv_advance_by_ms_no_timers(uint64_t ms_delta) {
  uint64_t target_ms = prv_now_ms() + ms_delta;

  // Compensate for rounding errors
  uint64_t new_ticks = rtc_get_ticks() + (ms_delta * RTC_TICKS_HZ + 500 ) / 1000;
  uint64_t new_ms = (new_ticks * 1000 + RTC_TICKS_HZ / 2) / RTC_TICKS_HZ;
  if (new_ms == target_ms - 1) {
    new_ticks++;
  } else if (new_ms == target_ms + 1) {
    new_ticks--;
  }
  new_ms = (new_ticks * 1000 + RTC_TICKS_HZ / 2) / RTC_TICKS_HZ;
  cl_assert(new_ms == target_ms);
  fake_rtc_set_ticks(new_ticks);
}


// --------------------------------------------------------------------------------------
// Fire the timer used by the animation service. Before doing so, advance the time to when
// the timer would fire next.
static void prv_fire_animation_timer(void) {
  TimerID sys_timer_id = animation_service_test_get_timer_id();
  if (!sys_timer_id) {
    DPRINTF("timer not scheduled\n");
    return;
  }
  if (!stub_new_timer_is_scheduled(sys_timer_id)) {
    DPRINTF("timer not scheduled\n");
    return;
  }

  // Advance time
  uint32_t ms_delta = stub_new_timer_timeout(sys_timer_id);
  prv_advance_by_ms_no_timers(ms_delta);

  // This posts a callback event to the KernelMain event queue
  stub_new_timer_fire(sys_timer_id);

  // Get the callback event and process it
  PebbleEvent evt = fake_event_get_last();
  cl_assert_equal_i(evt.type, PEBBLE_CALLBACK_EVENT);
  evt.callback.callback(evt.callback.data);
}


// --------------------------------------------------------------------------------------
// Advance to the given time, firing all timers that are scheduled along the way
static void prv_advance_to_ms_with_timers(uint64_t dst_time) {
  uint64_t  now = prv_now_ms();

  while (now < dst_time) {
    TimerID sys_timer_id = animation_service_test_get_timer_id();
    if (!sys_timer_id) {
      DPRINTF("timer not created\n");
      prv_advance_by_ms_no_timers(dst_time - now);
      return;
    }

    if (!stub_new_timer_is_scheduled(sys_timer_id)) {
      DPRINTF("timer not scheduled\n");
      prv_advance_by_ms_no_timers(dst_time - now);
      return;
    }

    // Advance time to when timer would fire and fire it
    uint32_t ms_delta = stub_new_timer_timeout(sys_timer_id);
    if (ms_delta < dst_time - now) {
      prv_fire_animation_timer();
    } else {
      prv_advance_by_ms_no_timers(dst_time - now);
      return;
    }

    now = prv_now_ms();
  }
}


// =============================================================================================
// Started. stopped, setup, and teardown handler call histories. Every time a handler runs, we
// append the time animation handle and timestamp to the history list.
typedef struct {
  uint64_t  fired_time_ms;
  uint32_t fire_order;
  bool finished;        // only applicable for stopped handlers
  void *context;        // For update handler, this is the distance arg
  Animation *animation; // which animation
} AnimTestHandlerEntry;
#define MAX_HANDLER_CALLS   500
typedef struct {
  uint32_t  num_calls;
  AnimTestHandlerEntry entries[MAX_HANDLER_CALLS];
} AnimTestHandlerHistory;

static AnimTestHandlerHistory s_started_handler_calls;
static AnimTestHandlerHistory s_stopped_handler_calls;
static AnimTestHandlerHistory s_setup_handler_calls;
static AnimTestHandlerHistory s_teardown_handler_calls;
static AnimTestHandlerHistory s_update_handler_calls;
static uint32_t s_fire_order_index;

// -------------------------------------------------------------------------
// Clear all handler history
static void prv_clear_handler_histories(void) {
  memset(&s_started_handler_calls, 0, sizeof(AnimTestHandlerHistory));
  memset(&s_stopped_handler_calls, 0, sizeof(AnimTestHandlerHistory));
  memset(&s_setup_handler_calls, 0, sizeof(AnimTestHandlerHistory));
  memset(&s_teardown_handler_calls, 0, sizeof(AnimTestHandlerHistory));
  memset(&s_update_handler_calls, 0, sizeof(AnimTestHandlerHistory));
}

// -------------------------------------------------------------------------
// Add an entry to the history
static void prv_add_handler_entry(AnimTestHandlerHistory *history, Animation *animation,
                                  bool finished, void *context) {
  cl_assert(history->num_calls < MAX_HANDLER_CALLS);
  history->entries[history->num_calls++] = (AnimTestHandlerEntry) {
    .fired_time_ms = prv_now_ms(),
    .fire_order = s_fire_order_index++,
    .finished = finished,
    .context = context,
    .animation = animation
  };
}

// -------------------------------------------------------------------------
// Count how many entries were entered for the given animation
static uint32_t prv_count_handler_entries(AnimTestHandlerHistory *history, Animation *animation) {
  uint32_t  count = 0;

  for (int i = 0; i < history->num_calls; i++) {
    if (!animation || history->entries[i].animation == animation) {
      count++;
    }
  }
  return count;
}

// -------------------------------------------------------------------------
// Get the last entry for the given handle
static AnimTestHandlerEntry *prv_last_handler_entry(AnimTestHandlerHistory *history,
          Animation *animation) {
  int last_entry=-1;
  for (int i = 0; i < history->num_calls; i++) {
    if (!animation || history->entries[i].animation == animation) {
      last_entry = i;
    }
  }
  if (last_entry == -1) {
    return NULL;
  } else {
    return &history->entries[last_entry];
  }
}

// -------------------------------------------------------------------------
// Get the last distance from an update handler
static uint32_t prv_last_update_distance(Animation *animation) {
  AnimTestHandlerEntry *entry = prv_last_handler_entry(&s_update_handler_calls, animation);
  if (entry) {
    return (uint32_t)entry->context;
  } else {
    return 0;
  }
}


// =============================================================================================
// Handlers

// --------------------------------------------------------------------------------------
// Started handler
static void prv_started_handler(Animation *animation, void *context) {
  prv_add_handler_entry(&s_started_handler_calls, animation, false, context);
  DPRINTF("%"PRIu64" ms: Executing started handler for %d\n", prv_now_ms(), (int)animation);
}

// --------------------------------------------------------------------------------------
// Stopped handler
static void prv_stopped_handler(Animation *animation, bool finished, void *context) {
  prv_add_handler_entry(&s_stopped_handler_calls, animation, finished, context);
  DPRINTF("%"PRIu64" ms: Executing stopped handler for %d\n", prv_now_ms(), (int)animation);
}

// --------------------------------------------------------------------------------------
// Stopped handler with check for finish
static void prv_stopped_handler_check_finished(Animation *animation, bool finished, void *context) {
  prv_add_handler_entry(&s_stopped_handler_calls, animation, finished, context);
  DPRINTF("%"PRIu64" ms: Executing stopped handler for %d\n", prv_now_ms(), (int)animation);
  cl_assert(finished);
  AnimationPrivate *animation_private = animation_private_animation_find(animation);
  if (animation_private) {
    // Flag should now get reset to false before entering stopped handler
    cl_assert(animation_private->is_completed == false);
  }
}

// --------------------------------------------------------------------------------------
// Stopped handler that calls reschedule the first time it is called
static void prv_stopped_handler_reschedule(Animation *animation, bool finished, void *context) {
  prv_add_handler_entry(&s_stopped_handler_calls, animation, finished, context);

  if (s_stopped_handler_calls.num_calls == 1) {
    DPRINTF("%"PRIu64" ms: Rescheduling from stopped handler for %d\n", prv_now_ms(),
            (int)animation);
    animation_schedule(animation);
  } else {
    DPRINTF("%"PRIu64" ms: NOT rescheduling from stopped handler for %d\n", prv_now_ms(),
            (int)animation);
  }

}

// --------------------------------------------------------------------------------------
// setup handler
void prv_setup_handler(Animation *animation) {
  prv_add_handler_entry(&s_setup_handler_calls, animation, false, NULL);
  DPRINTF("%"PRIu64" ms: Executing setup handler for %d\n", prv_now_ms(), (int)animation);
}

// --------------------------------------------------------------------------------------
// teardown handler
void prv_teardown_handler(Animation *animation) {
  prv_add_handler_entry(&s_teardown_handler_calls, animation, false, NULL);
  DPRINTF("%"PRIu64" ms: Executing teardown handler for %d\n", prv_now_ms(), (int)animation);
}

// --------------------------------------------------------------------------------------
// update handler

// implemented in Animation.c
AnimationPrivate *prv_animation_get_current(void);

void prv_update_handler(Animation *animation, const AnimationProgress distance) {
  // always ensure that animation state gives access to the current animation
  cl_assert_equal_p(animation_private_animation_find(animation), prv_animation_get_current());

  prv_add_handler_entry(&s_update_handler_calls, animation, false,
                        (void *)(uintptr_t)distance /*context*/);
  DPRINTF("%"PRIu64" ms: Executing update handler for %d, distance: %d\n", prv_now_ms(),
          (int)animation, (int)distance);
}


// --------------------------------------------------------------------------------------
static int s_custom_curve_call_count;
static AnimationProgress prv_custom_curve(AnimationProgress distance) {
  // Input is a value from 0 to 65535 (ANIMATION_NORMALIZED_MAX)
  // Output is a value from 0 to 65535
  s_custom_curve_call_count++;
  return distance;
}


// --------------------------------------------------------------------------------------
// Count how many animations have been allocated
static uint32_t prv_count_animations(void) {
  AnimationState *state = kernel_applib_get_animation_state();
  return list_count(state->unscheduled_head) + list_count(state->scheduled_head);
}

// --------------------------------------------------------------------------------------
// Count how many animations have been scheduled
static uint32_t prv_count_scheduled_animations(void) {
  AnimationState *state = kernel_applib_get_animation_state();
  return list_count(state->scheduled_head);
}

// --------------------------------------------------------------------------------------
static void prv_int16_setter(int16_t *p, int16_t value) {
  *p = value;
}

static int16_t prv_int16_getter(int16_t *p) {
  return *p;
}

// --------------------------------------------------------------------------------------
static void prv_gpoint_setter(GPoint *p, GPoint value) {
  *p = value;
}

static GPoint prv_gpoint_getter(GPoint *p) {
  return *p;
}


// --------------------------------------------------------------------------------------
static void prv_gtransform_setter(GTransform *p, GTransform value) {
  *p = value;
}

static GTransform prv_gtransform_getter(GTransform *p) {
  return *p;
}


// --------------------------------------------------------------------------------------
static void prv_gcolor8_setter(GColor8 *p, GColor8 value) {
  *p = value;
}

static GColor8 prv_gcolor8_getter(GColor8 *p) {
  return *p;
}


// --------------------------------------------------------------------------------------
static void prv_fixed_s32_16_setter(Fixed_S32_16 *p, Fixed_S32_16 value) {
  *p = value;
}

static Fixed_S32_16 prv_fixed_s32_16_getter(Fixed_S32_16 *p) {
  return *p;
}

// --------------------------------------------------------------------------------------
static void prv_uint32_setter(int32_t *p, uint32_t value) {
  *p = value;
}

static uint32_t prv_uint32_getter(uint32_t *p) {
  return *p;
}



// --------------------------------------------------------------------------------------
// Helper function for creating a int16 property animation
static Animation *prv_create_test_animation(void) {
  Animation *h;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  static const AnimationImplementation implementation = {
    .setup = prv_setup_handler,
    .update = prv_update_handler,
    .teardown = prv_teardown_handler
  };

  h = animation_create();
  cl_assert(h != NULL);
  void *context = h;
  animation_set_handlers(h, handlers, context);
  animation_set_implementation(h, &implementation);

  return h;
} 


// --------------------------------------------------------------------------------------
// Setup. Called before each of the tests execute
void test_animation__initialize(void) {
  fake_rtc_init(1024*200 /*ticks */, 200 /*seconds */);

  AnimationState *state = kernel_applib_get_animation_state();
  animation_private_state_init(state);

  // Insure that at least some time elapsed after init so that state->last_frame_time is
  // in the past.
  prv_advance_by_ms_no_timers(10);

  // Clear handler histories
  prv_clear_handler_histories();
}


// -------------------------------------------------------------------------------------
// Cleanup, called after each test executes
void test_animation__cleanup(void) {
  // Make sure no animations were left over
  cl_assert_equal_i(prv_count_animations(), 0);
}



// --------------------------------------------------------------------------------------
// Test a basic layer_frame property animation
// Test that the started and stopped handlers get called at the right time
void test_animation__property_layer_frame(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  Layer layer;
  GRect from_r;
  GRect to_r;
  const int duration = 100;
  GRect r;
  void *subject;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  memset(&layer, 0, sizeof(layer));
  from_r = GRect(0, 0, 100, 200);     // x, y, width, height
  to_r = GRect(1000, 2000, 100, 200); // x, y, width, height
  prop_h = property_animation_create_layer_frame(&layer, &from_r, &to_r);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &layer;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);
  animation_set_auto_destroy(h, false);


  // Clone it and make sure the clone is correct
  PropertyAnimation *clone_h = (PropertyAnimation *)animation_clone((Animation *)prop_h);
  property_animation_get_from_grect(clone_h, &r);
  cl_assert_equal_rect(from_r, r);
  property_animation_get_to_grect(clone_h, &r);
  cl_assert_equal_rect(to_r, r);
  property_animation_get_subject(prop_h, &subject);
  cl_assert(subject == &layer);
  property_animation_destroy(clone_h);


  prv_clear_handler_histories();

  animation_schedule(h);
  int max_loops = 20;
  uint64_t  start_ms = prv_now_ms();
  uint64_t  time_ms;
  while (s_stopped_handler_calls.num_calls == 0) {
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("%"PRIu64": frame at: %d, %d, %d %d\n", time_ms - start_ms,
            layer.frame.origin.x, layer.frame.origin.y,
            layer.frame.size.w, layer.frame.size.h);

    cl_assert(max_loops > 0);
    max_loops--;
  }

  // Make sure the frame reached the "to" state
  cl_assert_equal_point(layer.frame.origin, to_r.origin);

  // Make sure our started and stopped handlers got called
  cl_assert_equal_i(s_started_handler_calls.num_calls, 1);;
  cl_assert(s_started_handler_calls.entries[0].fired_time_ms - start_ms <= 1);
  cl_assert(s_started_handler_calls.entries[0].context == context);

  cl_assert_equal_i(s_stopped_handler_calls.num_calls, 1);;
  cl_assert(s_stopped_handler_calls.entries[0].fired_time_ms - start_ms >= duration);
  cl_assert(s_stopped_handler_calls.entries[0].context == context);
  cl_assert(s_stopped_handler_calls.entries[0].finished);


  // -------------------------------------------------------------------------------------------
  // Test the accessor functions
  property_animation_get_from_grect(prop_h, &r);
  cl_assert_equal_rect(from_r, r);

  property_animation_get_to_grect(prop_h, &r);
  cl_assert_equal_rect(to_r, r);

  property_animation_get_subject(prop_h, &subject);
  cl_assert(subject == &layer);


  GRect set_r = GRect(1, 2, 3, 4);
  property_animation_set_from_grect(prop_h, &set_r);
  r = GRect(0, 0, 0, 0);
  property_animation_get_from_grect(prop_h, &r);
  cl_assert_equal_rect(set_r, r);

  set_r = GRect(5, 6, 7, 8);
  property_animation_set_to_grect(prop_h, &set_r);
  r = GRect(0, 0, 0, 0);
  property_animation_get_to_grect(prop_h, &r);
  cl_assert_equal_rect(set_r, r);

  subject = (void *)0x11223344;
  property_animation_set_subject(prop_h, &subject);
  subject = NULL;
  property_animation_get_subject(prop_h, &subject);
  cl_assert(subject == (void *)0x11223344);


  // Destroy it
  animation_destroy(h);
#endif
}


// --------------------------------------------------------------------------------------
// Test a basic int16 property animation
// Test that the started and stopped handlers get called at the right time
// Test that the setup and teardown handlers get called at the right time
// Test that delay works
// Test that duration works
// Test that custom curve works
void test_animation__property_int16(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  int16_t value = 0;
  int16_t start_value = 0, end_value = 100;
  const int duration = 200;
  const int delay = 25;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  static const PropertyAnimationImplementation implementation = {
    .base = {
      .setup = prv_setup_handler,
      .update = (AnimationUpdateImplementation) property_animation_update_int16,
      .teardown = prv_teardown_handler
    },
    .accessors = {
      .setter = { .int16 = (const Int16Setter) prv_int16_setter, },
      .getter = { .int16 = (const Int16Getter) prv_int16_getter },
    },
  };

  prop_h = property_animation_create(&implementation, &value, &start_value, &end_value);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &value;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);
  animation_set_delay(h, delay);
  animation_set_custom_curve(h, prv_custom_curve);
  animation_set_auto_destroy(h, false);

  prv_clear_handler_histories();
  s_custom_curve_call_count = 0;

  animation_schedule(h);

  int max_loops = 20;
  int num_loops = 0;
  uint64_t  start_ms = prv_now_ms();
  uint64_t  time_ms;
  while (s_stopped_handler_calls.num_calls == 0) {
    num_loops++;
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("%"PRIu64": value at: %d\n", time_ms - start_ms, value);

    cl_assert(max_loops > 0);
    max_loops--;
  }

  // Make sure the frame reached the "to" state
  cl_assert_equal_i(value, 100);

  // Make sure our started and stopped handlers got called
  cl_assert_equal_i(s_started_handler_calls.num_calls, 1);;
  cl_assert(ABSOLUTE_VALUE(s_started_handler_calls.entries[0].fired_time_ms - start_ms - delay) <= 1);

  cl_assert_equal_i(s_setup_handler_calls.num_calls, 1);;
  cl_assert(s_setup_handler_calls.entries[0].fired_time_ms - start_ms <= 1);

  cl_assert_equal_i(s_stopped_handler_calls.num_calls, 1);;
  cl_assert(s_stopped_handler_calls.entries[0].fired_time_ms - start_ms >= duration);
  cl_assert(s_stopped_handler_calls.entries[0].finished);

  cl_assert_equal_i(s_teardown_handler_calls.num_calls, 1);;
  cl_assert(s_teardown_handler_calls.entries[0].fired_time_ms - start_ms >= duration);

  // Make sure the custom curve function got called
  cl_assert_equal_i(num_loops, s_custom_curve_call_count);


  // -------------------------------------------------------------------------------------------
  // Test the int16 accessor functions
  int16_t test_value;
  property_animation_get_from_int16(prop_h, &test_value);
  cl_assert_equal_i(test_value, start_value);

  property_animation_get_to_int16(prop_h, &test_value);
  cl_assert_equal_i(test_value, end_value);


  int16_t set_value;
  set_value = 42;
  property_animation_set_from_int16(prop_h, &set_value);
  property_animation_get_from_int16(prop_h, &test_value);
  cl_assert_equal_i(test_value, set_value);

  set_value = 43;
  property_animation_set_to_int16(prop_h, &set_value);
  property_animation_get_to_int16(prop_h, &test_value);
  cl_assert_equal_i(test_value, set_value);

  // Destroy it
  animation_destroy(h);
#endif
}


// --------------------------------------------------------------------------------------
// Test a basic gpoint property animation
void test_animation__property_gpoint(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  GPoint value;
  GPoint start_value, end_value;
  const int duration = 200;
  const int delay = 25;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  static const PropertyAnimationImplementation implementation = {
    .base = {
      .update = (AnimationUpdateImplementation) property_animation_update_gpoint,
    },
    .accessors = {
      .setter = { .gpoint = (const GPointSetter) prv_gpoint_setter, },
      .getter = { .gpoint = (const GPointGetter) prv_gpoint_getter },
    },
  };

  start_value = GPoint(0, 0);
  end_value = GPoint(100, 200);
  prop_h = property_animation_create(&implementation, &value, &start_value, &end_value);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &value;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);
  animation_set_delay(h, delay);
  animation_set_auto_destroy(h, false);

  prv_clear_handler_histories();

  animation_schedule(h);

  int max_loops = 20;
  int num_loops = 0;
  uint64_t  start_ms = prv_now_ms();
  uint64_t  time_ms;
  while (s_stopped_handler_calls.num_calls == 0) {
    num_loops++;
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("%"PRIu64": value at: (%d, %d)\n", time_ms - start_ms, value.x, value.y);

    cl_assert(max_loops > 0);
    max_loops--;
  }

  // Make sure the frame reached the "to" state
  cl_assert_equal_point(value, end_value);


  // -------------------------------------------------------------------------------------------
  // Test the GPoint accessor functions
  GPoint test_value;
  property_animation_get_from_gpoint(prop_h, &test_value);
  cl_assert_equal_point(test_value, start_value);

  property_animation_get_to_gpoint(prop_h, &test_value);
  cl_assert_equal_point(test_value, end_value);

  GPoint set_value;
  set_value = GPoint(42, 43);
  property_animation_set_from_gpoint(prop_h, &set_value);
  property_animation_get_from_gpoint(prop_h, &test_value);
  cl_assert_equal_point(test_value, set_value);

  set_value = GPoint(44, 45);
  property_animation_set_to_gpoint(prop_h, &set_value);
  property_animation_get_to_gpoint(prop_h, &test_value);
  cl_assert_equal_point(test_value, set_value);

  // Destroy it
  animation_destroy(h);
#endif
}



// --------------------------------------------------------------------------------------
// Test a basic gtransform property animation
void test_animation__property_gtransform(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  GTransform value;
  GTransform start_value, end_value, mid_value;
  const int duration = 1000;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  // NOTE: We are not exposing the GTransform in the public SDK, so the setter and getter
  // must be typecast
  static const PropertyAnimationImplementation implementation = {
    .base = {
      .update = (AnimationUpdateImplementation) property_animation_update_gtransform,
    },
    .accessors = {
      .setter = { .int16 = (const Int16Setter) prv_gtransform_setter, },
      .getter = { .int16 = (const Int16Getter) prv_gtransform_getter },
    },
  };

  start_value = GTransformFromNumbers(1, 2, 3, 4, 5, 6);
  end_value = GTransformFromNumbers(100, 200, 300, 400, 500, 600);
  mid_value = GTransformFromNumbers(50, 101, 151, 202, 252, 303);
  value = end_value;
  prop_h = property_animation_create(&implementation, &value, &start_value, NULL);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &value;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(h);


  // Test the accessor functions
  GTransform test_value;
  property_animation_get_from_gtransform(prop_h, &test_value);
  cl_assert_equal_gtransform(test_value, start_value);

  property_animation_get_to_gtransform(prop_h, &test_value);
  cl_assert_equal_gtransform(test_value, end_value);

  GTransform set_value;
  set_value = GTransformIdentity();
  property_animation_set_from_gtransform(prop_h, &set_value);
  property_animation_get_from_gtransform(prop_h, &test_value);
  cl_assert_equal_gtransform(test_value, GTransformIdentity());
  property_animation_set_from_gtransform(prop_h, &start_value);


  // Start, we should start at the start values
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_gtransform(value, start_value);

  // Halfway through
  prv_advance_to_ms_with_timers(start_ms + duration/2);
  cl_assert_close_gtransform(value, mid_value);

  // End
  prv_advance_to_ms_with_timers(start_ms + duration + MIN_FRAME_INTERVAL_MS*2);
  cl_assert_equal_gtransform(value, end_value);

#endif
}


// --------------------------------------------------------------------------------------
// Test a basic Fixed_S32_16 property animation
void test_animation__property_fixed_s32_16(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  Fixed_S32_16 value;
  Fixed_S32_16 start_value, end_value, mid_value;
  const int duration = 1000;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  // NOTE: We are not exposing the GTransform in the public SDK, so the setter and getter
  // must be typecast
  static const PropertyAnimationImplementation implementation = {
    .base = {
      .update = (AnimationUpdateImplementation) property_animation_update_fixed_s32_16,
    },
    .accessors = {
      .setter = { .int16 = (const Int16Setter) prv_fixed_s32_16_setter },
      .getter = { .int16 = (const Int16Getter) prv_fixed_s32_16_getter },
    },
  };

  start_value = ((Fixed_S32_16){ .integer = 1, .fraction = 0 });
  end_value = ((Fixed_S32_16){ .integer = 100, .fraction = 0 });
  mid_value = ((Fixed_S32_16){ .integer = 50, .fraction = 0 });
  value = end_value;
  prop_h = property_animation_create(&implementation, &value, &start_value, NULL);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &value;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(h);


  // Test the accessor functions
  Fixed_S32_16 test_value;
  property_animation_get_from_fixed_s32_16(prop_h, &test_value);
  cl_assert_equal_fixed_s32_16(test_value, start_value);

  property_animation_get_to_fixed_s32_16(prop_h, &test_value);
  cl_assert_equal_fixed_s32_16(test_value, end_value);

  Fixed_S32_16 set_value;
  set_value = FIXED_S32_16_ONE;
  property_animation_set_from_fixed_s32_16(prop_h, &set_value);
  property_animation_get_from_fixed_s32_16(prop_h, &test_value);
  cl_assert_equal_fixed_s32_16(test_value, FIXED_S32_16_ONE);
  property_animation_set_from_fixed_s32_16(prop_h, &start_value);


  // Start, we should start at the start values
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_fixed_s32_16(value, start_value);

  // Halfway through
  prv_advance_to_ms_with_timers(start_ms + duration/2);
  cl_assert_close_fixed_s32_16(value, mid_value);

  // End
  prv_advance_to_ms_with_timers(start_ms + duration + MIN_FRAME_INTERVAL_MS*2);
  cl_assert_equal_fixed_s32_16(value, end_value);

#endif
}


// --------------------------------------------------------------------------------------
// Test a basic uint32_t property animation
void test_animation__property_uint32(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  uint32_t value;
  uint32_t start_value, end_value, mid_value;
  const int duration = 1000;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  // NOTE: We are not exposing the GTransform in the public SDK, so the setter and getter
  // must be typecast
  static const PropertyAnimationImplementation implementation = {
    .base = {
      .update = (AnimationUpdateImplementation) property_animation_update_uint32,
    },
    .accessors = {
      .setter = { .uint32 = (const UInt32Setter) prv_uint32_setter },
      .getter = { .uint32 = (const UInt32Getter) prv_uint32_getter },
    },
  };

  start_value = 1;
  end_value = 100;
  mid_value = 50;
  value = end_value;
  prop_h = property_animation_create(&implementation, &value, &start_value, NULL);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &value;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(h);


  // Test the accessor functions
  uint32_t test_value;
  property_animation_get_from_uint32(prop_h, &test_value);
  cl_assert_equal_i(test_value, start_value);

  property_animation_get_to_uint32(prop_h, &test_value);
  cl_assert_equal_i(test_value, end_value);

  uint32_t set_value = 1;
  property_animation_set_from_uint32(prop_h, &set_value);
  property_animation_get_from_uint32(prop_h, &test_value);
  cl_assert_equal_i(test_value, 1);
  property_animation_set_from_uint32(prop_h, &start_value);


  // Start, we should start at the start values
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(value, start_value);

  // Halfway through
  prv_advance_to_ms_with_timers(start_ms + duration/2);
  cl_assert(abs((int32_t)value - (int32_t)mid_value) < 10);

  // End
  prv_advance_to_ms_with_timers(start_ms + duration + MIN_FRAME_INTERVAL_MS*2);
  cl_assert_equal_i(value, end_value);

#endif
}


// --------------------------------------------------------------------------------------
// Test a basic gcolor8 property animation
void test_animation__property_gcolor8(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  GColor8 value;
  GColor8 start_value, end_value, mid_value;
  const int duration = 1000;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  static const PropertyAnimationImplementation implementation = {
    .base = {
      .update = (AnimationUpdateImplementation) property_animation_update_gcolor8,
    },
    .accessors = {
      .setter = { .gcolor8 = (const GColor8Setter) prv_gcolor8_setter, },
      .getter = { .gcolor8 = (const GColor8Getter) prv_gcolor8_getter },
    },
  };

  start_value = (GColor8) {.a=0, .r=0, .g=0, .b=0};
  end_value = (GColor8) {.a=3, .r=3, .g=3, .b=3};
  mid_value = (GColor8) {.a=1, .r=1, .g=1, .b=1};
  value = end_value;
  prop_h = property_animation_create(&implementation, &value, &start_value, NULL);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &value;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(h);


  // Test the accessor functions
  GColor8 test_value;
  property_animation_get_from_gcolor8(prop_h, &test_value);
  cl_assert(gcolor_equal(test_value, start_value));

  property_animation_get_to_gcolor8(prop_h, &test_value);
  cl_assert(gcolor_equal(test_value, end_value));

  GColor8 set_value;
  set_value = (GColor8) {.a=0, .r=1, .g=2, .b=3};
  property_animation_set_from_gcolor8(prop_h, &set_value);
  property_animation_get_from_gcolor8(prop_h, &test_value);
  cl_assert(gcolor_equal(test_value, set_value));
  property_animation_set_from_gcolor8(prop_h, &start_value);


  // Start, we should start at the start values
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert(gcolor_equal(value, start_value));

  // Halfway through
  prv_advance_to_ms_with_timers(start_ms + duration/2);
  cl_assert(gcolor_equal(value, mid_value));

  // End
  prv_advance_to_ms_with_timers(start_ms + duration + MIN_FRAME_INTERVAL_MS*2);
  cl_assert(gcolor_equal(value, end_value));
#endif
}



// --------------------------------------------------------------------------------------
// Test that the schedule/unschedule calls work correctly.
// We should be able to unschedule an animation partway through
void test_animation__unschedule(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  Layer layer;
  GRect from_r;
  GRect to_r;
  GRect stopped_at_r;
  const int duration = 500;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  memset(&layer, 0, sizeof(layer));
  from_r = GRect(0, 0, 100, 200);     // x, y, width, height
  to_r = GRect(1000, 2000, 100, 200); // x, y, width, height
  prop_h = property_animation_create_layer_frame(&layer, &from_r, &to_r);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &layer;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);
  animation_set_auto_destroy(h, false);

  prv_clear_handler_histories();

  animation_schedule(h);
  uint64_t  start_ms = prv_now_ms();
  uint64_t  unschedule_time = 0;
  uint64_t  time_ms;
  for (int num_loops = 0; num_loops < 10; num_loops++) {
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("%"PRIu64": frame at: %d, %d, %d %d\n", time_ms - start_ms,
            layer.frame.origin.x, layer.frame.origin.y,
            layer.frame.size.w, layer.frame.size.h);

    // Unschedule after 2 iterations
    if (num_loops == 2) {
        DPRINTF("%"PRIu64": Unscheduling now\n", prv_now_ms());
        animation_unschedule(h);
        stopped_at_r = layer.frame;
        unschedule_time = prv_now_ms();
    }
  }

  // Make sure the frame stopped at the state it was in when we unscheduled it
  cl_assert_equal_point(layer.frame.origin, stopped_at_r.origin);

  // Make sure our started and stopped handlers got called at
  cl_assert_equal_i(s_started_handler_calls.num_calls, 1);;
  cl_assert(s_started_handler_calls.entries[0].fired_time_ms - start_ms <= 1);
  cl_assert(s_started_handler_calls.entries[0].context == context);

  cl_assert_equal_i(s_stopped_handler_calls.num_calls, 1);;
  cl_assert(s_stopped_handler_calls.entries[0].fired_time_ms - start_ms < duration);
  cl_assert(ABSOLUTE_VALUE(s_stopped_handler_calls.entries[0].fired_time_ms - unschedule_time) < 1);
  cl_assert(!s_stopped_handler_calls.entries[0].finished);

  // Destroy it
  animation_destroy(h);
#endif
}


// --------------------------------------------------------------------------------------
// Test that we can reschedule an animation after it completes and have it run again
void test_animation__reschedule(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  Layer layer;
  GRect from_r;
  GRect to_r;
  const int duration = 100;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  memset(&layer, 0, sizeof(layer));
  from_r = GRect(0, 0, 100, 200);     // x, y, width, height
  to_r = GRect(1000, 2000, 100, 200); // x, y, width, height
  prop_h = property_animation_create_layer_frame(&layer, &from_r, &to_r);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &layer;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);
  animation_set_auto_destroy(h, false);

  prv_clear_handler_histories();

  animation_schedule(h);
  int max_loops = 20;
  uint64_t  start_ms = prv_now_ms();
  uint64_t  time_ms;
  while (s_stopped_handler_calls.num_calls == 0) {
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("%"PRIu64": frame at: %d, %d, %d %d\n", time_ms - start_ms,
            layer.frame.origin.x, layer.frame.origin.y,
            layer.frame.size.w, layer.frame.size.h);

    cl_assert(max_loops > 0);
    max_loops--;
  }

  // Make sure the frame reached the "to" state
  cl_assert_equal_point(layer.frame.origin, to_r.origin);

  // -------------------------------------------------------------------------------
  // Now, reschedule it
  prv_advance_by_ms_no_timers(10);
  prv_clear_handler_histories();

  animation_schedule(h);
  max_loops = 20;
  start_ms = prv_now_ms();
  while (s_stopped_handler_calls.num_calls == 0) {
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("%"PRIu64": frame at: %d, %d, %d %d\n", time_ms - start_ms,
            layer.frame.origin.x, layer.frame.origin.y,
            layer.frame.size.w, layer.frame.size.h);

    cl_assert(max_loops > 0);
    max_loops--;
  }


  // Make sure our started and stopped handlers got called
  cl_assert_equal_i(s_started_handler_calls.num_calls, 1);;
  cl_assert(s_started_handler_calls.entries[0].fired_time_ms - start_ms <= 1);

  cl_assert_equal_i(s_stopped_handler_calls.num_calls, 1);;
  cl_assert(s_stopped_handler_calls.entries[0].fired_time_ms - start_ms >= duration);
  cl_assert(s_stopped_handler_calls.entries[0].finished);

  // Destroy it
  animation_destroy(h);
#endif
}


// --------------------------------------------------------------------------------------
// Test that we can reschedule an animation from the stopped handler
void test_animation__reschedule_from_stopped_handler(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  Layer layer;
  GRect from_r;
  GRect to_r;
  const int duration = 100;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler_reschedule
  };

  memset(&layer, 0, sizeof(layer));
  from_r = GRect(0, 0, 100, 200);     // x, y, width, height
  to_r = GRect(1000, 2000, 100, 200); // x, y, width, height
  prop_h = property_animation_create_layer_frame(&layer, &from_r, &to_r);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &layer;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);

  prv_clear_handler_histories();

  animation_schedule(h);
  int max_loops = 20;
  uint64_t  start_ms = prv_now_ms();
  bool detected_reset_of_elapsed = false;
  bool reached_end_elapsed = false;
  uint64_t  time_ms;
  while (s_stopped_handler_calls.num_calls < 2) {
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("rescheduled count: %d\n", s_stopped_handler_calls.num_calls);
    DPRINTF("%"PRIu64": frame at: %d, %d, %d %d\n", time_ms - start_ms,
            layer.frame.origin.x, layer.frame.origin.y,
            layer.frame.size.w, layer.frame.size.h);

    if (layer.frame.origin.x == to_r.origin.x && layer.frame.origin.y == to_r.origin.y) {
      reached_end_elapsed = true;
    }
    if (reached_end_elapsed && s_stopped_handler_calls.num_calls == 1
        && layer.frame.origin.x < to_r.origin.x) {
      detected_reset_of_elapsed = true;
    }

    cl_assert(max_loops > 0);
    max_loops--;
  }

  // Make sure we detected a reset of the elapsed after rescheduling
  cl_assert(s_stopped_handler_calls.num_calls == 2);
  cl_assert(detected_reset_of_elapsed);

  // Make sure the frame reached the "to" state
  cl_assert_equal_point(layer.frame.origin, to_r.origin);

#endif
}


// --------------------------------------------------------------------------------------
// Test that auto-destroy works correctly
void test_animation__auto_destroy(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  Layer layer;
  GRect from_r;
  GRect to_r;
  const int duration = 100;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  memset(&layer, 0, sizeof(layer));
  from_r = GRect(0, 0, 100, 200);     // x, y, width, height
  to_r = GRect(1000, 2000, 100, 200); // x, y, width, height
  prop_h = property_animation_create_layer_frame(&layer, &from_r, &to_r);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &layer;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);

  prv_clear_handler_histories();

  // Before we run, the context should be non NULL
  cl_assert(animation_get_context(h) == context);

  animation_schedule(h);
  int max_loops = 20;
  uint64_t  start_ms = prv_now_ms();
  uint64_t  time_ms;
  while (s_stopped_handler_calls.num_calls == 0) {
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("%"PRIu64": frame at: %d, %d, %d %d\n", time_ms - start_ms,
            layer.frame.origin.x, layer.frame.origin.y,
            layer.frame.size.w, layer.frame.size.h);

    cl_assert(max_loops > 0);
    max_loops--;
  }

  // This should return a NULL context now if the animation got destroyed
  cl_assert(animation_get_context(h) == NULL);

  // Make sure no animations exist
  cl_assert_equal_i(prv_count_animations(), 0);
#endif
}


// --------------------------------------------------------------------------------------
// Test that we can reschedule an animation from the stopped handler that has auto-destroy on
void test_animation__auto_destroy_reschedule(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  Layer layer;
  GRect from_r;
  GRect to_r;
  const int duration = 100;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler_reschedule
  };

  memset(&layer, 0, sizeof(layer));
  from_r = GRect(0, 0, 100, 200);     // x, y, width, height
  to_r = GRect(1000, 2000, 100, 200); // x, y, width, height
  prop_h = property_animation_create_layer_frame(&layer, &from_r, &to_r);
  Animation *h = property_animation_get_animation(prop_h);
  void *context = &layer;
  animation_set_handlers(h, handlers, context);
  animation_set_duration(h, duration);

  prv_clear_handler_histories();

  animation_schedule(h);
  int max_loops = 20;
  uint64_t  start_ms = prv_now_ms();
  bool detected_reset_of_elapsed = false;
  bool reached_end_elapsed = false;
  uint64_t  time_ms;
  while (s_stopped_handler_calls.num_calls < 2) {
    prv_fire_animation_timer();
    time_ms = prv_now_ms();
    DPRINTF("rescheduled count: %d\n", s_stopped_handler_calls.num_calls);
    DPRINTF("%"PRIu64": frame at: %d, %d, %d %d\n", time_ms - start_ms,
            layer.frame.origin.x, layer.frame.origin.y,
            layer.frame.size.w, layer.frame.size.h);

    if (layer.frame.origin.x == to_r.origin.x && layer.frame.origin.y == to_r.origin.y) {
      reached_end_elapsed = true;
    }
    if (reached_end_elapsed && s_stopped_handler_calls.num_calls == 1
        && layer.frame.origin.x < to_r.origin.x) {
      detected_reset_of_elapsed = true;
    }

    cl_assert(max_loops > 0);
    max_loops--;
  }

  // Make sure we detected a reset of the elapsed after rescheduling
  cl_assert(s_stopped_handler_calls.num_calls == 2);
  cl_assert(detected_reset_of_elapsed);

  // Make sure the frame reached the "to" state
  cl_assert_equal_point(layer.frame.origin, to_r.origin);

  // This should return a NULL context now if the animation got destroyed
  cl_assert(animation_get_context(h) == NULL);

  // Make sure no animations exist
  cl_assert_equal_i(prv_count_animations(), 0);
#endif
}


// --------------------------------------------------------------------------------------
// Stopped handler that calls destroy
static void prv_stopped_handler_destroy(Animation *animation, bool finished, void *context) {
  prv_add_handler_entry(&s_stopped_handler_calls, animation, finished, context);
  animation_destroy(animation);
}


// --------------------------------------------------------------------------------------
// Test that animation_destroy can be called from the stopped handler
static void prv_test_destroy_from_stopped_handler(bool auto_destroy) {
  Animation *h;
  const int duration = 100;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler_destroy
  };

  static const AnimationImplementation implementation = {
    .setup = prv_setup_handler,
    .update = prv_update_handler,
    .teardown = prv_teardown_handler
  };

  h = animation_create();
  cl_assert(h != NULL);
  void *context = h;
  animation_set_handlers(h, handlers, context);
  animation_set_implementation(h, &implementation);

  animation_set_duration(h, duration);
  animation_set_auto_destroy(h, auto_destroy);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(h);
  prv_advance_to_ms_with_timers(start_ms + duration + 2 * MIN_FRAME_INTERVAL_MS);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, h), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, h), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, h), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, h), 1);

  // Make sure the frame reached the "to" state
  cl_assert_equal_i(prv_last_update_distance(h), ANIMATION_NORMALIZED_MAX);

  // This should return NULL now if the animation got destroyed
  cl_assert(animation_private_animation_find(h) == NULL);

  // Make sure no animations exist
  cl_assert_equal_i(prv_count_animations(), 0);
}


// --------------------------------------------------------------------------------------
// Test that animation_destroy can be called from the stopped handler
void test_animation__destroy_from_stopped_handler_with_auto_destroy(void) {
#ifdef TEST_INCLUDE_BASIC
  prv_test_destroy_from_stopped_handler(true);
#endif
}


// --------------------------------------------------------------------------------------
// Test that animation_destroy can be called from the stopped handler
void test_animation__destroy_from_stopped_handler_without_auto_destroy(void) {
#ifdef TEST_INCLUDE_BASIC
  prv_test_destroy_from_stopped_handler(false);
#endif
}


// --------------------------------------------------------------------------------------
// Stopped handler that calls unschedule
static void prv_stopped_handler_unschedule(Animation *animation, bool finished, void *context) {
  prv_add_handler_entry(&s_stopped_handler_calls, animation, finished, context);
  animation_unschedule(animation);
}


// --------------------------------------------------------------------------------------
// Test that animation_unschedule can be called from the stopped handler
void test_animation__unschedule_from_stopped_handler(void) {
  Animation *h;
  const int duration = 100;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler_unschedule
  };

  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration);
  animation_set_handlers(a, handlers, a);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(a);
  prv_advance_to_ms_with_timers(start_ms + duration + 2 * MIN_FRAME_INTERVAL_MS);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  // Make sure no animations exist
  cl_assert_equal_i(prv_count_animations(), 0);
}



// --------------------------------------------------------------------------------------
// Test setting a play count of 0
void test_animation__basic_play_count_0(void) {
#ifdef TEST_INCLUDE_BASIC

  const int duration_a = 300;
  Animation *a = prv_create_test_animation();
  animation_set_play_count(a, 0);
  animation_set_duration(a, duration_a);

  prv_clear_handler_histories();
  animation_schedule(a);

  uint64_t  start_ms = prv_now_ms();
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);

  // Should not have run at all
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 0);

  // Should have been deleted automatically
  cl_assert_equal_i(prv_count_animations(), 0);

#endif
}


// --------------------------------------------------------------------------------------
// Test setting a duration of infinite duration
void test_animation__basic_infinite_duration(void) {
#ifdef TEST_INCLUDE_BASIC
  // A long time, but not so long as to use up our 500 capacity callback history limit
  const uint32_t test_duration = 10000;
  const uint32_t duration_a = ANIMATION_DURATION_INFINITE;
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  prv_clear_handler_histories();
  animation_schedule(a);

  uint64_t  start_ms = prv_now_ms();
  prv_advance_to_ms_with_timers(start_ms + test_duration);

  // Should still be running
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert(prv_count_handler_entries(&s_update_handler_calls, a)
            >= test_duration/MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 0);

  // The distance should always be at 0
  cl_assert_equal_i(prv_last_update_distance(a), 0);

  // Destroy it
  animation_destroy(a);

#endif
}


// --------------------------------------------------------------------------------------
// Test a simple sequence animation
void test_animation__simple_sequence(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int play_count_b = 2;
  int duration_total = duration_a + play_count_b * duration_b;

  // Create 2 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_play_count(b, play_count_b);

  // Create a sequence
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);

  // Setup started/stopped handlers for the sequence
  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };
  animation_set_handlers(seq, handlers, seq);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);

  // Start A
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 0);


  // Just before A completes
  prv_advance_to_ms_with_timers(start_ms + duration_a - 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 0);


  // Complete A and start B
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 0);

  // The stopped handler for A should fire before the started handler for B
  cl_assert(prv_last_handler_entry(&s_stopped_handler_calls, a)->fire_order
            < prv_last_handler_entry(&s_started_handler_calls, b)->fire_order);


  // Just before B completes the 2nd play
  prv_advance_to_ms_with_timers(start_ms + duration_total - 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 0);


  // Complete B
  prv_advance_to_ms_with_timers(start_ms + duration_total + 5 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 2);
  cl_assert_equal_i(prv_last_update_distance(b), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 1);
#endif
}


static Animation *s_parent_for_sequence_unschedule_from_child;
static void prv_unschedule_parent(Animation *animation, bool finished, void *context) {
  DPRINTF("%"PRIu64" ms: Executing prv_unschedule_parent handler for %d\n", prv_now_ms(),
          (int)animation);
  animation_unschedule(s_parent_for_sequence_unschedule_from_child);
}

// --------------------------------------------------------------------------------------
// Test calling unschedule on the sequence from the stopped handler of one of its children
void test_animation__sequence_unschedule_from_child(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int play_count_b = 2;
  int duration_total = duration_a + play_count_b * duration_b;

  // Create 2 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  // Setup stopped handler for the first child that unschedules the parent
  const AnimationHandlers special_handlers = {
    .started = NULL,
    .stopped = prv_unschedule_parent
  };
  animation_set_handlers(a, special_handlers, NULL);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_play_count(b, play_count_b);

  // Create a sequence
  Animation *seq = animation_sequence_create(a, b, NULL);
  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };
  animation_set_handlers(seq, handlers, seq);
  s_parent_for_sequence_unschedule_from_child = seq;

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);


  // Complete A and start B. This should unschedule the parent
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 1);


  // Everything should have been freed
  cl_assert_equal_i(prv_count_animations(), 0);

#endif
}


// --------------------------------------------------------------------------------------
// Test a seeking in a basic sequence animation
void test_animation__simple_sequence_set_elapsed(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int duration_c = 400;
  const int play_count_b = 2;
  int duration_total = duration_a + play_count_b * duration_b;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };


  // Create 2 property animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_play_count(b, play_count_b);

  // Create a sequence out of them.
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);


  // Create a shorter animation to play in parallel
  Animation *c = prv_create_test_animation();
  cl_assert(c != NULL);
  animation_set_duration(c, duration_c);

  Animation *complex = animation_spawn_create(seq, c, NULL);
  cl_assert(complex != NULL);
  animation_set_handlers(complex, handlers, complex);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(complex);

  // -------------------------------------------------------------------------------------
  // Start A
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 0);


  // -------------------------------------------------------------------------------------
  // Execute about half of A
  prv_advance_to_ms_with_timers(start_ms + duration_a/2);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 0);


  // -------------------------------------------------------------------------------------
  // Seek to about the middle of B
  // Save the current update elapsed
  animation_set_elapsed(complex, duration_a + duration_b/2);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 1);

  // A should be at the end
  int32_t update_a_after = prv_last_update_distance(a);
  cl_assert_equal_i(update_a_after, ANIMATION_NORMALIZED_MAX);

  // B should be in the middle
  int32_t update_b_after = prv_last_update_distance(b);
  cl_assert(abs(update_b_after - ANIMATION_NORMALIZED_MAX/2) < 5000);

  // C should be at the end
  int32_t update_c_after = prv_last_update_distance(c);
  cl_assert_equal_i(update_c_after, ANIMATION_NORMALIZED_MAX);


  // -------------------------------------------------------------------------------------
  // Seek to just before the end of the second B
  // Save the current update elapsed
  animation_set_elapsed(complex, duration_total - 2 * MIN_FRAME_INTERVAL_MS);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 1);


  // -------------------------------------------------------------------------------------
  // animation a has completed, but it shouldn't be deleted yet until the top-level
  // animation is done.
  uint32_t duration = animation_get_duration(a, false, false);
  cl_assert_equal_i(duration, duration_a);


  // -------------------------------------------------------------------------------------
  // Advance to the end
  animation_set_elapsed(complex, duration_total);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, complex), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 1);

  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_last_update_distance(b), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_last_update_distance(c), ANIMATION_NORMALIZED_MAX);

  // Make sure each animation got to the end
  AnimTestHandlerEntry *entry;
  entry = prv_last_handler_entry(&s_update_handler_calls, c);
  cl_assert_equal_i((uint32_t)entry->context, ANIMATION_NORMALIZED_MAX);
  
#endif
}


// --------------------------------------------------------------------------------------
// Test unscheduling a complex animation
void test_animation__sequence_unschedule(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 200;
  const int duration_c = 500;
  const int repeat_count = 5;
  int duration_total = duration_a + MAX(duration_b, duration_c);

  // Create 3 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);

  Animation *c = prv_create_test_animation();
  animation_set_duration(c, duration_c);

  // Create a spawn out of b and c
  Animation *spawn = animation_spawn_create(b, c, NULL);
  cl_assert(spawn != NULL);

  // Create a sequence by putting a in front
  // We now have a -> (b | c)
  Animation *seq = animation_sequence_create(a, spawn, NULL);

  // Make it repeat
  animation_set_play_count(seq, repeat_count);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);

  // Start A
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, c), 0);


  // Execute to the start of B and C
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 0);


  // Execute to the end of B & C
  prv_advance_to_ms_with_timers(start_ms + duration_total + 1 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 1);


  // If we keep going, we should repeat the whole sequence
  prv_advance_to_ms_with_timers(start_ms + 2 * (duration_total + 4 * MIN_FRAME_INTERVAL_MS));
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 3);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 2);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 2);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 2);

  // Unschedule the top-level
  animation_unschedule(seq);


  // Keep going, nothing new should happen except the stop handler for a (which we started)
  prv_advance_to_ms_with_timers(start_ms + 5 * (duration_total + 3 * MIN_FRAME_INTERVAL_MS));
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 3);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 3);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, c), 1);
#endif
}


// --------------------------------------------------------------------------------------
// Test using clone and reverse in a complex animation
void test_animation__complex_reverse(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_total = 2 * duration_a;
  const int repeat_count = 2;
  uint32_t distance;

  // Create animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = animation_clone(a);
  animation_set_reverse(b, true);

  // Create a sequence
  Animation *seq = animation_sequence_create(a, b, NULL);
  animation_set_play_count(seq, repeat_count);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);

  // Start A
  prv_advance_to_ms_with_timers(start_ms + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // A should start out low
  distance = prv_last_update_distance(a);
  cl_assert(distance < TEST_ANIMATION_NORMALIZED_LOW);


  // Execute to the start of B
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  // A should end high
  distance = prv_last_update_distance(a);
  cl_assert(distance > TEST_ANIMATION_NORMALIZED_HIGH);

  // B should start high
  distance = prv_last_update_distance(b);
  cl_assert(distance > TEST_ANIMATION_NORMALIZED_HIGH);


  // Execute to the end of B
  prv_advance_to_ms_with_timers(start_ms + duration_total + 1 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);

  // B should end low
  distance = prv_last_update_distance(b);
  cl_assert(distance < TEST_ANIMATION_NORMALIZED_LOW);


  // If we keep going, we should repeat the whole sequence
  prv_advance_to_ms_with_timers(start_ms + 2 * (duration_total + 10 * MIN_FRAME_INTERVAL_MS));
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 1);

  // A should end up at max
  // A should end high
  distance = prv_last_update_distance(a);
  cl_assert_equal_i(distance, ANIMATION_NORMALIZED_MAX);

  // B should start high
  distance = prv_last_update_distance(b);
  cl_assert_equal_i(distance, ANIMATION_NORMALIZED_MIN);

#endif
}


// --------------------------------------------------------------------------------------
// Test cloning complex animation
void test_animation__complex_clone(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 200;
  const int duration_c = 500;
  const int repeat_count = 5;
  int duration_total = duration_a + MAX(duration_b, duration_c);

  // Create 3 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);

  Animation *c = prv_create_test_animation();
  animation_set_duration(c, duration_c);

  // Create a spawn out of b and c
  Animation *spawn = animation_spawn_create(b, c, NULL);
  cl_assert(spawn != NULL);

  // Create a sequence by putting a in front and repeat it 5 times
  // We now have a -> (b | c)
  Animation *seq = animation_sequence_create(a, spawn, NULL);
  animation_set_play_count(seq, repeat_count);


  // Now, clone it
  Animation *clone = animation_clone(seq);

  // Destroy the original
  animation_destroy(seq);

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(clone);

  // Start A
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, NULL), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, NULL), 0);

  // Execute to the start of B and C
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, NULL), 3);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, NULL), 1);

  // Execute to the end of B & C
  prv_advance_to_ms_with_timers(start_ms + duration_total + 1 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, NULL), 3);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, NULL), 3);

  // If we keep going, we should repeat the whole sequence another time
  prv_advance_to_ms_with_timers(start_ms + 2 * (duration_total + 4 * MIN_FRAME_INTERVAL_MS));
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, NULL), 7);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, NULL), 6);

  // Unschedule the top-level
  animation_unschedule(clone);


  // Keep going, nothing new should happen except stop handlers for each component
  prv_advance_to_ms_with_timers(start_ms + 5 * (duration_total + 3 * MIN_FRAME_INTERVAL_MS));
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, NULL), 7);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, NULL), 7);
#endif
}


// --------------------------------------------------------------------------------------
// Test scheduling a sequence of 2 spawns. Insure that ALL of the primitives in the first spawn,
// finish before the primitives from the 2nd spawn start.
static void prv_test_sequence_of_spawns(int create_order[4]) {
  const int duration_a = 150;
  const int duration_total = 2 * duration_a;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  // Simulate some delay incurred on every call to rtc_get_ticks()
  fake_rtc_auto_increment_ticks(3);

  Animation *a0, *a1, *a2, *a3;
  Animation *b0, *b1, *b2, *b3;
  Animation *spawn_a, *spawn_b;

  for (int i=0; i<4; i++) {
    switch(create_order[i]) {
      case 0:
        a0 = prv_create_test_animation();
        animation_set_duration(a0, duration_a);
        a1 = animation_clone(a0);
        a2 = animation_clone(a0);
        a3 = animation_clone(a0);
        break;

      case 1:
        b0 = prv_create_test_animation();
        animation_set_duration(b0, duration_a);
        b1 = animation_clone(b0);
        b2 = animation_clone(b0);
        b3 = animation_clone(b0);
        break;

      case 2:
        spawn_a = animation_spawn_create(a0, a1, a2, a3, NULL);
        animation_set_handlers(spawn_a, handlers, (void *)spawn_a);
        break;

      case 3:
        spawn_b = animation_spawn_create(b0, b1, b2, b3, NULL);
        animation_set_handlers(spawn_b, handlers, (void *)spawn_a);
        break;
    }
  }

  // Create the sequence
  Animation *seq = animation_sequence_create(spawn_a, spawn_b, NULL);

  // Schedule it
  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);

  fake_rtc_auto_increment_ticks(0);

  // Let the first spawn finish
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a0), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a0), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a1), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a1), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a2), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a2), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a3), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a3), 1);

  // None of the b's should finish yet
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b0), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b1), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b2), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b3), 0);


  // Let it finish completely
  prv_advance_to_ms_with_timers(start_ms + duration_total + 5 * MIN_FRAME_INTERVAL_MS);

  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b0), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b1), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b2), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b3), 1);


  // Make sure the all the spawn a stopped handlers got called before any of the spawn b
  // started handlers
  uint32_t last_fire_a = 0;
  last_fire_a = MAX(last_fire_a,
                    prv_last_handler_entry(&s_stopped_handler_calls, a0)->fire_order);
  last_fire_a = MAX(last_fire_a,
                    prv_last_handler_entry(&s_stopped_handler_calls, a1)->fire_order);
  last_fire_a = MAX(last_fire_a,
                    prv_last_handler_entry(&s_stopped_handler_calls, a2)->fire_order);
  last_fire_a = MAX(last_fire_a,
                    prv_last_handler_entry(&s_stopped_handler_calls, a3)->fire_order);
  last_fire_a = MAX(last_fire_a,
                    prv_last_handler_entry(&s_stopped_handler_calls, spawn_a)->fire_order);

  uint32_t first_fire_b = prv_last_handler_entry(&s_started_handler_calls, b0)->fire_order;
  first_fire_b = MIN(first_fire_b,
                    prv_last_handler_entry(&s_started_handler_calls, b1)->fire_order);
  first_fire_b = MIN(first_fire_b,
                    prv_last_handler_entry(&s_started_handler_calls, b2)->fire_order);
  first_fire_b = MIN(first_fire_b,
                    prv_last_handler_entry(&s_started_handler_calls, b3)->fire_order);
  first_fire_b = MIN(first_fire_b,
                    prv_last_handler_entry(&s_started_handler_calls, spawn_b)->fire_order);

  cl_assert(last_fire_a < first_fire_b);

  cl_assert_equal_i(prv_count_animations(), 0);
}

// --------------------------------------------------------------------------------------
// Test scheduling a sequence of 2 spawns. Insure that ALL of the primitives in the first spawn,
// finish before the primitives from the 2nd spawn start.
void test_animation__sequence_of_spawns(void) {
#ifdef TEST_INCLUDE_COMPLEX
  int order_a[4] = {0, 1, 2, 3};
  prv_test_sequence_of_spawns(order_a);

  int order_b[4] = {1, 0, 2, 3};
  prv_test_sequence_of_spawns(order_b);

  int order_c[4] = {1, 0, 3, 2};
  prv_test_sequence_of_spawns(order_c);
#endif
}


// --------------------------------------------------------------------------------------
// Test delays in sequence animation
void test_animation__sequence_delay(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int delay_a = 100;
  const int duration_b = 500;
  const int delay_b = 200;
  const int delay_seq = 150;
  int duration_total = duration_a + duration_b + delay_a + delay_b + delay_seq;


  // Create 2 test animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);

  // Create a sequence
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);
  animation_set_delay(seq, delay_seq);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);


  // Test the elapsed
  int32_t elapsed_ms;
  animation_get_elapsed(seq, &elapsed_ms);
  cl_assert_equal_i(elapsed_ms, -1 * (delay_seq));

  animation_get_elapsed(a, &elapsed_ms);
  cl_assert_equal_i(elapsed_ms, -1 * (delay_seq + delay_a));

  animation_get_elapsed(b, &elapsed_ms);
  cl_assert_equal_i(elapsed_ms, -1 * (delay_seq + delay_a + duration_a + delay_b));

  // Start
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Start A after delay
  prv_advance_to_ms_with_timers(start_ms + delay_seq + delay_a + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Just before A completes
  prv_advance_to_ms_with_timers(start_ms + delay_seq + delay_a + duration_a - 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Complete A and start B
  prv_advance_to_ms_with_timers(start_ms + duration_a + delay_seq + delay_a + delay_b
                                 + 2 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);

  // Complete B
  prv_advance_to_ms_with_timers(start_ms + duration_total + 5 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);
  cl_assert_equal_i(prv_last_update_distance(b), ANIMATION_NORMALIZED_MAX);

#endif
}


// --------------------------------------------------------------------------------------
// Test delays in spawn animation
void test_animation__spawn_delay(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int delay_a = 100;
  const int duration_b = 500;
  const int delay_b = 200;
  const int delay_spawn = 150;
  int duration_total = MAX(duration_a + delay_a, duration_b + delay_b) + delay_spawn;


  // Create 2 test animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);

  // Create a spawn
  Animation *spawn = animation_spawn_create(a, b, NULL);
  cl_assert(spawn != NULL);
  animation_set_delay(spawn, delay_spawn);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(spawn);


  // Test the elapsed
  int32_t elapsed_ms;
  animation_get_elapsed(spawn, &elapsed_ms);
  cl_assert_equal_i(elapsed_ms, -1 * (delay_spawn));

  animation_get_elapsed(a, &elapsed_ms);
  cl_assert_equal_i(elapsed_ms, -1 * (delay_spawn + delay_a));

  animation_get_elapsed(b, &elapsed_ms);
  cl_assert_equal_i(elapsed_ms, -1 * (delay_spawn + delay_b));

  // Start
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Start A
  prv_advance_to_ms_with_timers(start_ms + delay_spawn + delay_a + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  // Start B
  prv_advance_to_ms_with_timers(start_ms + delay_spawn + MAX(delay_a, delay_b)
                                + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  // Complete A and start B
  prv_advance_to_ms_with_timers(start_ms + delay_spawn + duration_a + delay_a
                                + 2 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);

  // Complete B
  prv_advance_to_ms_with_timers(start_ms + duration_total + 5 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);
  cl_assert_equal_i(prv_last_update_distance(b), ANIMATION_NORMALIZED_MAX);

#endif
}


// --------------------------------------------------------------------------------------
// Test a sequence animation with a component that has a play count of 0
void test_animation__sequence_with_0_component(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int play_count_seq = 2;
  int duration_total = play_count_seq * duration_a;

  // Create 2 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_play_count(b, 0);

  // Create a sequence
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);
  animation_set_play_count(seq, play_count_seq);

  // Check the duration
  cl_assert_equal_i(animation_get_duration(seq, true, true), play_count_seq * duration_a);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);

  // Start A
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Just before A completes
  prv_advance_to_ms_with_timers(start_ms + duration_a - 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Complete A the first time
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  // Complete sequence
  prv_advance_to_ms_with_timers(start_ms + duration_total + 5 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 0);

  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);

#endif
}


// --------------------------------------------------------------------------------------
// Test a spawn animation with a component that has a play count of 0
void test_animation__spawn_with_0_component(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int play_count_spawn = 2;
  int duration_total = play_count_spawn * duration_a;

  // Create 2 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_play_count(b, 0);

  // Create a spawn that repeats
  Animation *spawn = animation_spawn_create(a, b, NULL);
  cl_assert(spawn != NULL);
  animation_set_play_count(spawn, play_count_spawn);

  // Check the duration
  cl_assert_equal_i(animation_get_duration(spawn, true, true), play_count_spawn * duration_a);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(spawn);

  // Start A
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Just before A completes
  prv_advance_to_ms_with_timers(start_ms + duration_a - 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Complete A the first time
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  // Complete spawn
  prv_advance_to_ms_with_timers(start_ms + duration_total + 5 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 2);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 0);

  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);

#endif
}


// --------------------------------------------------------------------------------------
// Test a sequence animation with a play count of 0
void test_animation__sequence_with_0_play_count(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int play_count_seq = 0;

  // Create 2 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);

  // Create a sequence
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);
  animation_set_play_count(seq, play_count_seq);

  // Check the duration
  cl_assert_equal_i(animation_get_duration(seq, true, true), 0);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);

  // Start
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Complete sequence
  prv_advance_to_ms_with_timers(start_ms + duration_a + duration_b + 5 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 0);

  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MIN);
  cl_assert_equal_i(prv_last_update_distance(b), ANIMATION_NORMALIZED_MIN);

#endif
}


// --------------------------------------------------------------------------------------
// Test a sequence within a sequence where the imbedded one has a play count of 0
void test_animation__nested_sequence_with_0_play_count(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int duration_c = 200;
  const int duration_d = 400;
  const int total_duration = duration_c + duration_d;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  // Create the inner sequence with a play count of 0
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);

  Animation *inner_seq = animation_sequence_create(a, b, NULL);
  animation_set_play_count(inner_seq, 0);
  animation_set_handlers(inner_seq, handlers, inner_seq);

  // Create the outer sequence
  Animation *c = prv_create_test_animation();
  animation_set_duration(c, duration_c);

  Animation *d = prv_create_test_animation();
  animation_set_duration(d, duration_d);

  Animation *seq = animation_sequence_create(inner_seq, c, d, NULL);
  animation_set_handlers(seq, handlers, seq);


  // Play it
  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();
  animation_schedule(seq);

  prv_advance_to_ms_with_timers(start_ms + total_duration + 5 * MIN_FRAME_INTERVAL_MS);

  // Make sure neither inner_seq, a, nor b played
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, inner_seq), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, inner_seq), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, b), 0);


  // Make sure seq, c, and d completed
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, c), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, d), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, d), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, d), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, d), 1);
#endif
}


// --------------------------------------------------------------------------------------
// Test a spawn animation with a play count of 0
void test_animation__spawn_with_0_play_count(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int duration_b = 500;
  const int play_count_spawn = 0;

  // Create 2 animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);

  // Create a spawn
  Animation *spawn = animation_sequence_create(a, b, NULL);
  cl_assert(spawn != NULL);
  animation_set_play_count(spawn, play_count_spawn);

  // Check the duration
  cl_assert_equal_i(animation_get_duration(spawn, true, true), 0);

  prv_clear_handler_histories();

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(spawn);

  // Start
  prv_advance_to_ms_with_timers(start_ms + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_update_handler_calls, b), 0);

  // Complete sequence
  prv_advance_to_ms_with_timers(start_ms + duration_a + duration_b + 5 * MIN_FRAME_INTERVAL_MS + 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MIN);
  cl_assert_equal_i(prv_last_update_distance(b), ANIMATION_NORMALIZED_MIN);

#endif
}


// --------------------------------------------------------------------------------------
// Test the get_duration call on a sequence animation
void test_animation__sequence_get_duration(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int delay_a = 100;
  const int play_count_a = 1;
  const int total_duration_a = play_count_a * (delay_a + duration_a);

  const int duration_b = 500;
  const int delay_b = 200;
  const int play_count_b = 3;
  const int total_duration_b = play_count_b * (delay_b + duration_b);

  const int delay_seq = 150;
  const int play_count_seq = 2;

  int duration_total = play_count_seq * (total_duration_a + total_duration_b + delay_seq);


  // Create 2 test animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);
  animation_set_play_count(a, play_count_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);
  animation_set_play_count(b, play_count_b);

  // Create a sequence
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);
  animation_set_delay(seq, delay_seq);
  animation_set_play_count(seq, play_count_seq);


  // Check durations
  cl_assert_equal_i(animation_get_duration(a, false, false), duration_a);
  cl_assert_equal_i(animation_get_duration(a, false, true), play_count_a * duration_a);
  cl_assert_equal_i(animation_get_duration(a, true, false), delay_a + duration_a);
  cl_assert_equal_i(animation_get_duration(a, true, true), total_duration_a);

  cl_assert_equal_i(animation_get_duration(b, false, false), duration_b);
  cl_assert_equal_i(animation_get_duration(b, false, true), play_count_b * duration_b);
  cl_assert_equal_i(animation_get_duration(b, true, false), delay_b + duration_b);
  cl_assert_equal_i(animation_get_duration(b, true, true), total_duration_b);

  cl_assert_equal_i(animation_get_duration(seq, false, false), total_duration_a + total_duration_b);
  cl_assert_equal_i(animation_get_duration(seq, false, true),
                    play_count_seq * (total_duration_a + total_duration_b));
  cl_assert_equal_i(animation_get_duration(seq, true, false),
                    delay_seq + total_duration_a + total_duration_b);
  cl_assert_equal_i(animation_get_duration(seq, true, true), duration_total);

  animation_destroy(seq);

#endif
}


// --------------------------------------------------------------------------------------
// Test the get_duration call on a spawn animation
void test_animation__spawn_get_duration(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int delay_a = 100;
  const int play_count_a = 1;
  const int total_duration_a = play_count_a * (delay_a + duration_a);

  const int duration_b = 500;
  const int delay_b = 200;
  const int play_count_b = 3;
  const int total_duration_b = play_count_b * (delay_b + duration_b);

  const int delay_spawn = 150;
  const int play_count_spawn = 2;

  int duration_total = play_count_spawn * (MAX(total_duration_a, total_duration_b) + delay_spawn);


  // Create 2 test animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);
  animation_set_play_count(a, play_count_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);
  animation_set_play_count(b, play_count_b);

  // Create a spawn
  Animation *spawn = animation_spawn_create(a, b, NULL);
  cl_assert(spawn != NULL);
  animation_set_delay(spawn, delay_spawn);
  animation_set_play_count(spawn, play_count_spawn);


  // Check durations
  cl_assert_equal_i(animation_get_duration(a, false, false), duration_a);
  cl_assert_equal_i(animation_get_duration(a, false, true), play_count_a * duration_a);
  cl_assert_equal_i(animation_get_duration(a, true, false), delay_a + duration_a);
  cl_assert_equal_i(animation_get_duration(a, true, true), total_duration_a);

  cl_assert_equal_i(animation_get_duration(b, false, false), duration_b);
  cl_assert_equal_i(animation_get_duration(b, false, true), play_count_b * duration_b);
  cl_assert_equal_i(animation_get_duration(b, true, false), delay_b + duration_b);
  cl_assert_equal_i(animation_get_duration(b, true, true), total_duration_b);

  cl_assert_equal_i(animation_get_duration(spawn, false, false),
                    MAX(total_duration_a, total_duration_b));
  cl_assert_equal_i(animation_get_duration(spawn, false, true),
                    play_count_spawn * (MAX(total_duration_a, total_duration_b)));
  cl_assert_equal_i(animation_get_duration(spawn, true, false),
                    delay_spawn + MAX(total_duration_a, total_duration_b));
  cl_assert_equal_i(animation_get_duration(spawn, true, true), duration_total);

  animation_destroy(spawn);

#endif
}


// --------------------------------------------------------------------------------------
// Test unschedule all when we have multiple animations, some complex
void test_animation__unschedule_all(void) {
#ifdef TEST_INCLUDE_COMPLEX

  // Create animations

  // Create a sequence
  Animation *a = prv_create_test_animation();
  Animation *b = prv_create_test_animation();
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);

  // Create a spawn
  Animation *c = prv_create_test_animation();
  Animation *d = prv_create_test_animation();
  Animation *spawn = animation_spawn_create(c, d, NULL);
  cl_assert(spawn != NULL);

  // Create a primitive one
  Animation *e = prv_create_test_animation();

  // Schedule them all
  animation_schedule(seq);
  animation_schedule(spawn);
  animation_schedule(e);

  // Verify count
  cl_assert_equal_i(prv_count_scheduled_animations(), 7);

  // Unschedule all
  animation_unschedule_all();
  cl_assert_equal_i(prv_count_scheduled_animations(), 0);


  // Make sure just the setup and teardown handlers were called
  cl_assert_equal_i(prv_count_handler_entries(&s_setup_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_teardown_handler_calls, a), 1);

#endif
}


// --------------------------------------------------------------------------------------
// Test that we fail if we try and put a component in more than 1 complex animation
void test_animation__reuse_components(void) {
#ifdef TEST_INCLUDE_COMPLEX

  // Create animations

  // Create a sequence out of a and b
  Animation *a = prv_create_test_animation();
  Animation *b = prv_create_test_animation();
  Animation *seq = animation_sequence_create(a, b, NULL);
  cl_assert(seq != NULL);

  // Try to create a spawn out of b and c
  Animation *c = prv_create_test_animation();
  Animation *spawn = animation_spawn_create(c, b, NULL);
  cl_assert(spawn == NULL);

  // We should be able to create one out of c and d
  Animation *d = prv_create_test_animation();
  spawn = animation_spawn_create(c, d, NULL);
  cl_assert(spawn != NULL);

  animation_destroy(seq);
  animation_destroy(spawn);

#endif
}

// Test all the accessors
void test_animation__accessors(void) {
#ifdef TEST_INCLUDE_BASIC
  PropertyAnimation *prop_h;
  int16_t value = 0;
  int16_t start_value = 0, end_value = 100;
  const int duration = 200;
  const int delay = 25;

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  static const PropertyAnimationImplementation implementation = {
    .base = {
      .setup = prv_setup_handler,
      .update = (AnimationUpdateImplementation) property_animation_update_int16,
      .teardown = prv_teardown_handler
    },
    .accessors = {
      .setter = { .int16 = (const Int16Setter) prv_int16_setter, },
      .getter = { .int16 = (const Int16Getter) prv_int16_getter },
    },
  };

  prop_h = property_animation_create(&implementation, &value, &start_value, &end_value);
  Animation *h = property_animation_get_animation(prop_h);

  cl_assert(animation_set_auto_destroy(h, false) == true);

  // Handlers
  void *context = &value;
  animation_set_handlers(h, handlers, context);
  AnimationHandlers c_handlers = animation_get_handlers(h);
  cl_assert(memcmp(&c_handlers, &handlers, sizeof(AnimationHandlers)) == 0);

  // Context
  cl_assert(animation_get_context(h) == context);

  // Duration
  cl_assert(animation_get_duration(h, true, true) == 250); // default value
  animation_set_duration(h, duration);
  cl_assert(animation_get_duration(h, true, true) == duration);

  // Delay
  cl_assert(animation_get_delay(h) == 0);
  animation_set_delay(h, delay);
  cl_assert(animation_get_delay(h) == delay);
  cl_assert(animation_get_duration(h, true, true) == (duration + delay));

  // Play count
  cl_assert_equal_i(animation_get_play_count(h), 1);
  animation_set_play_count(h, 2);
  cl_assert_equal_i(animation_get_play_count(h), 2);
  cl_assert(animation_get_duration(h, true, true) == 2*(duration + delay));

  // Curve
  cl_assert(animation_get_curve(h) == AnimationCurveDefault);
  animation_set_curve(h, AnimationCurveEaseOut);
  cl_assert(animation_get_curve(h) == AnimationCurveEaseOut);
  
  static const PropertyAnimationImplementation implementation2 = {
    .base = {
      .setup = prv_setup_handler,
      .update = (AnimationUpdateImplementation) property_animation_update_gpoint,
      .teardown = prv_teardown_handler
    },
    .accessors = {
      .setter = { .int16 = (const Int16Setter) prv_gpoint_setter, },
      .getter = { .int16 = (const Int16Getter) prv_gpoint_getter },
    },
  };

  // Implementation
  cl_assert(animation_get_implementation(h) == &implementation.base);
  animation_set_implementation(h, &implementation2.base);
  cl_assert(animation_get_implementation(h) != &implementation.base);
  cl_assert(animation_get_implementation(h) == &implementation2.base);

  // Custom Curve
  cl_assert(animation_get_custom_curve(h) != prv_custom_curve);
  animation_set_custom_curve(h, prv_custom_curve);
  cl_assert(animation_get_custom_curve(h) == prv_custom_curve);
  
  // Reverse
  cl_assert(animation_get_reverse(h) == false);
  animation_set_reverse(h, true);
  cl_assert(animation_get_reverse(h) == true);
  animation_set_reverse(h, false);

  // Position
  int32_t elapsed_ms = 0;
  AnimationProgress progress = 0;
  animation_schedule(h);
  cl_must_pass(animation_get_elapsed(h, &elapsed_ms));
  cl_assert_equal_i(elapsed_ms, -delay);
  cl_assert_passert(animation_get_progress(h, &progress));
  animation_set_elapsed(h, 0);
  cl_must_pass(animation_get_elapsed(h, &elapsed_ms));
  cl_assert_equal_i(elapsed_ms, 0);
  cl_must_pass(animation_get_progress(h, &progress));
  cl_assert_equal_i(progress, 0);
  animation_set_elapsed(h, duration / 2);
  cl_must_pass(animation_get_elapsed(h, &elapsed_ms));
  cl_assert_equal_i(elapsed_ms, duration / 2);
  cl_must_pass(animation_get_progress(h, &progress));
  cl_assert_equal_i(progress, 32768); // Rounding occurs within, this is close to MAX / 2


  animation_destroy(h);
#endif
}

void test_animation__completed(void) {
#ifdef TEST_INCLUDE_BASIC
  const int duration_a = 300;

  // Create 1 property animations
  Animation *a = prv_create_test_animation();

  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler_check_finished
  };
  animation_set_handlers(a, handlers, animation_get_context(a));
  animation_set_duration(a, duration_a);

  prv_clear_handler_histories();
  uint64_t start_ms = prv_now_ms();
  animation_schedule(a);

  // Seek to just after the end of the second A
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);
#endif
}



// --------------------------------------------------------------------------------------
// Test creating a sequence where the first argument is already scheduled and started
//
// Here's a graph of what we are doing
//
// 0      60      310  360           380           880
// |       |       |    |             |             |
// -----------------------------------------------------------------------------------
// delay_a | duration_a |
//                      |   delay_b   | duration_b  |
//                 | seq scheduled
//
void test_animation__sequence_of_already_scheduled_started(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;
  const int delay_a = 60;
  const int leftover_a = 50;

  const int duration_b = 500;
  const int delay_b = 20;

  const int leftover_seq = 40;
  const int delay_seq = 30;


  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();

  // Create a property animation and advance it
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);

  animation_schedule(a);

  // -------------------------------------------------------------------------------------
  // Start A and advance it
  prv_advance_to_ms_with_timers(start_ms + delay_a + MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);
  animation_set_elapsed(a, duration_a - leftover_a);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);


  // -------------------------------------------------------------------------------------
  // Build up a sequence out of the leftover a + b
  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);

  // Should be an error trying to use a scheduled animation not in the first position
  Animation *seq = animation_sequence_create(b, a, NULL);
  cl_assert(seq == NULL);

  seq = animation_sequence_create(a, b, NULL);
  animation_set_delay(seq, delay_seq); // This delay not applicable since a was already scheduled
  animation_set_handlers(seq, handlers, seq);
  animation_schedule(seq);

  // The duration of seq should include all of a and b
  uint32_t duration = animation_get_duration(seq, true, true);
  cl_assert_equal_i(duration, duration_a + delay_a + duration_b + delay_b);

  // The position of seq should be the amount we already played of a, including the 'a' delay
  //  since a is embedded within seq
  int32_t position;
  animation_get_elapsed(seq, &position);
  cl_assert_equal_i(position, duration_a + delay_a - leftover_a);

  // Now, advance sequence to almost the end of seq. Positions don't include the delay, so
  // pass false for 'include_delay'
  animation_set_elapsed(seq, animation_get_duration(seq, false /*delay*/, true /*play_count*/)
                              - leftover_seq);


  // Verify that a finished and that a's stop handler got called before B's start handler
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 0);

  cl_assert(prv_last_handler_entry(&s_stopped_handler_calls, a)->fire_order
            < prv_last_handler_entry(&s_started_handler_calls, b)->fire_order);

  // Finish the sequence
  prv_advance_to_ms_with_timers(prv_now_ms() + leftover_seq + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 1);

#endif
}


// --------------------------------------------------------------------------------------
// Test creating a sequence where the first argument is already scheduled, but not started
//  yet (still in the delay portion).
//
// Here's a graph of what we are doing
//
// 0  100   200          360           380           880
// |   |     |            |             |             |
// -----------------------------------------------------------------------------------
// delay_a   | duration_a |
//                        |   delay_b   | duration_b  |
//     | seq scheduled
//
void test_animation__sequence_of_already_scheduled_not_started(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 160;
  const int delay_a = 200;

  const int duration_b = 500;
  const int delay_b = 20;
  const int leftover_seq = 50;


  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  prv_clear_handler_histories();

  // Create a property animation and advance it
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);

  uint64_t  start_ms = prv_now_ms();
  animation_schedule(a);

  // -------------------------------------------------------------------------------------
  // Got partway through the delayof a
  prv_advance_to_ms_with_timers(start_ms + 100);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 0);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 0);


  // -------------------------------------------------------------------------------------
  // Build up a sequence out of the leftover a + b
  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);

  Animation *seq = animation_sequence_create(a, b, NULL);
  animation_set_handlers(seq, handlers, seq);
  animation_schedule(seq);

  // The duration of seq should include all of a and b
  uint32_t duration = animation_get_duration(seq, true, true);
  cl_assert_equal_i(duration, duration_a + delay_a + duration_b + delay_b);

  // The position of seq should be the amount we already played of a, including the 'a' delay
  //  since a is embedded within seq
  int32_t position;
  animation_get_elapsed(seq, &position);
  cl_assert_equal_i(position, 100);

  // Now, advance sequence to almost the end of seq. Positions don't include the delay, so
  // pass false for 'include_delay'
  animation_set_elapsed(seq, animation_get_duration(seq, false /*delay*/, true /*play_count*/)
                              - leftover_seq);


  // Verify that a finished and that a's stop handler got called before B's start handler
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 0);

  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, seq), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 0);

  cl_assert(prv_last_handler_entry(&s_stopped_handler_calls, a)->fire_order
            < prv_last_handler_entry(&s_started_handler_calls, b)->fire_order);

  // Finish the sequence
  prv_advance_to_ms_with_timers(prv_now_ms() + leftover_seq + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 1);

#endif
}



// --------------------------------------------------------------------------------------
// Test creating a sequence where the first argument is already completed.
// We will first create animation 'a' and run it to the end
// We will then create a sequence out of 'a' + 'b' and verify that if we advance that 'b' runs
//  correctly.
void test_animation__sequence_of_already_completed(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;

  const int duration_b = 500;
  const int delay_b = 20;

  const int delay_seq = 30;


  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();

  // Create a property animation and play it to the end
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);

  animation_schedule(a);

  // -------------------------------------------------------------------------------------
  // Start A and play to the end
  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_last_update_distance(a), ANIMATION_NORMALIZED_MAX);


  // -------------------------------------------------------------------------------------
  // Build up a sequence out of a + b
  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);

  Animation *seq = animation_sequence_create(a, b, NULL);
  animation_set_delay(seq, delay_seq);
  animation_set_handlers(seq, handlers, seq);
  animation_schedule(seq);

  // The duration of seq should include all of b
  uint32_t duration = animation_get_duration(seq, true, true);
  cl_assert_equal_i(duration, duration_b + delay_b + delay_seq);

  // The position of seq should be at -delay_seq
  int32_t position;
  animation_get_elapsed(seq, &position);
  cl_assert_equal_i(position, -delay_seq);

  // Finish the sequence
  prv_advance_to_ms_with_timers(prv_now_ms() + delay_b + duration_b + delay_seq
                                +  2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);
  cl_assert_equal_i(prv_last_update_distance(b), ANIMATION_NORMALIZED_MAX);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, seq), 1);

#endif
}



// --------------------------------------------------------------------------------------
// Test creating a spawn where where some children are already scheduled and some have
// already completed.
//
// Here's a graph of what we are doing
//
// 0      10           310  320  330  500   680   730  850   950     1080           1300
// |       |            |    |    |    |     |     |    |     |       |              |
// -----------------------------------------------------------------------------------
// delay_a | duration a |                    |
//                      | delay_b | duration_b     |
//                                     | delay_c   |   duration_c     |
//                                           | delay_s  | delay_d     | duration_d   |
// -----------------------------------------------------------------------------------
//                                           | spawn scheduled here
void test_animation__spawn_of_already_scheduled(void) {
#ifdef TEST_INCLUDE_COMPLEX
  const int duration_a = 300;   // This one will complete
  const int delay_a = 10;

  const int duration_b = 400;   // This one will have 50 ms left on it
  const int delay_b = 20;

  const int duration_c = 350;   
  const int delay_c = 230;

  const int duration_d = 220;   // This one won't be scheduled yet
  const int delay_d = 230;

  const int delay_spawn = 170;


  const AnimationHandlers handlers = {
    .started = prv_started_handler,
    .stopped = prv_stopped_handler
  };

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();

  // Create the animations
  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_b);
  animation_set_delay(b, delay_b);

  Animation *c = prv_create_test_animation();
  animation_set_duration(c, duration_c);
  animation_set_delay(c, delay_c);

  Animation *d = prv_create_test_animation();
  animation_set_duration(d, duration_d);
  animation_set_delay(d, delay_d);


  // -------------------------------------------------------------------------------------
  // Run A to completion
  animation_schedule(a);
  prv_advance_to_ms_with_timers(start_ms + 20);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, a), 1);
  prv_advance_to_ms_with_timers(start_ms + 310);

  // Schedule B now and run for a little
  animation_schedule(b);
  prv_advance_to_ms_with_timers(start_ms + 330);

  // Schedule C now and run for a while
  prv_advance_to_ms_with_timers(start_ms + 500);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, a), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, b), 1);
  animation_schedule(c);

  // Create the spawn using a, b, c, and d
  prv_advance_to_ms_with_timers(start_ms + 680);
  Animation *spawn = animation_spawn_create(a, b, c, d, NULL);
  animation_set_delay(spawn, delay_spawn);
  animation_set_handlers(spawn, handlers, spawn);
  animation_schedule(spawn);


  // Check the duration and position of the spawn
  uint32_t duration = animation_get_duration(spawn, true /*delay*/, true /*play_count*/);
  cl_assert_equal_i(duration, 1300 - 310);

  int32_t position;
  animation_get_elapsed(spawn, &position);
  cl_assert_equal_i(position, (680 - 310));


  // Run to the completion of B, start of C
  prv_advance_to_ms_with_timers(start_ms + 730 + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, b), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, spawn), 1);


  // Run to the completion of C, start of D
  prv_advance_to_ms_with_timers(start_ms + 1080 + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, c), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_started_handler_calls, d), 1);

  // Run to the completion of D
  prv_advance_to_ms_with_timers(start_ms + 1300 + 2 * MIN_FRAME_INTERVAL_MS);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, d), 1);
  cl_assert_equal_i(prv_count_handler_entries(&s_stopped_handler_calls, spawn), 1);

#endif
}

void prv_update_unschedule_all_handler(Animation *animation, const AnimationProgress distance) {
  prv_add_handler_entry(&s_update_handler_calls, animation, false,
                        (void *)(uintptr_t)distance /*context*/);
  DPRINTF("%"PRIu64" ms: Executing update handler for %d, distance: %d\n", prv_now_ms(),
          (int)animation, (int)distance);
  if (distance > ANIMATION_NORMALIZED_MAX / 2) {
    animation_unschedule_all();
  }
}

// --------------------------------------------------------------------------------------
// Test unscheduling animations arbitrarily in an update handler.
static void prv_unschedule_all_in_update_handler(bool auto_destroy) {
  const int duration_a = 300;   // This one will complete
  const int delay_a = 10;

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();

  static const AnimationImplementation implementation = {
    .setup = prv_setup_handler,
    .update = prv_update_unschedule_all_handler,
    .teardown = prv_teardown_handler
  };

  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);
  animation_set_auto_destroy(a, auto_destroy);
  animation_schedule(a);

  Animation *b = prv_create_test_animation();
  animation_set_implementation(b, &implementation);
  animation_set_duration(b, duration_a);
  animation_set_delay(b, delay_a);
  animation_set_auto_destroy(b, auto_destroy);
  animation_schedule(b);

  Animation *c = prv_create_test_animation();
  animation_set_duration(c, duration_a);
  animation_set_delay(c, delay_a);
  animation_set_auto_destroy(c, auto_destroy);
  animation_schedule(c);

  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);

  animation_destroy(a);
  animation_destroy(b);
  animation_destroy(c);
}

// --------------------------------------------------------------------------------------
// Test unscheduling animations arbitrarily in an update handler with auto destroy.
void test_animation__unschedule_all_in_update_handler_with_auto_destroy(void) {
  prv_unschedule_all_in_update_handler(true);
}

// --------------------------------------------------------------------------------------
// Test unscheduling animations arbitrarily in an update handler without auto destroy.
void test_animation__unschedule_all_in_update_handler_without_auto_destroy(void) {
  prv_unschedule_all_in_update_handler(false);
}

static void prv_stopped_unschedule_all_handler(Animation *animation, bool finished, void *context) {
  prv_add_handler_entry(&s_stopped_handler_calls, animation, finished, context);
  DPRINTF("%"PRIu64" ms: Executing stopped handler for %d\n", prv_now_ms(), (int)animation);
  animation_unschedule_all();
}

// --------------------------------------------------------------------------------------
// Test unscheduling animations arbitrarily in a stopped handler with/without auto destroy.
void prv_test_unschedule_all_in_stopped_handler(bool auto_destroy) {
  const int duration_a = 300;   // This one will complete
  const int delay_a = 10;

  prv_clear_handler_histories();
  uint64_t  start_ms = prv_now_ms();

  const AnimationHandlers handlers = {
    .stopped = prv_stopped_unschedule_all_handler,
  };

  void *context = NULL;

  Animation *a = prv_create_test_animation();
  animation_set_duration(a, duration_a);
  animation_set_delay(a, delay_a);
  animation_set_handlers(a, handlers, context);
  animation_set_auto_destroy(a, auto_destroy);
  animation_schedule(a);

  Animation *b = prv_create_test_animation();
  animation_set_duration(b, duration_a);
  animation_set_delay(b, delay_a);
  animation_set_handlers(b, handlers, context);
  animation_set_auto_destroy(b, auto_destroy);
  animation_schedule(b);

  Animation *c = prv_create_test_animation();
  animation_set_duration(c, duration_a);
  animation_set_delay(c, delay_a);
  animation_set_handlers(c, handlers, context);
  animation_set_auto_destroy(c, auto_destroy);
  animation_schedule(c);

  prv_advance_to_ms_with_timers(start_ms + duration_a + 2 * MIN_FRAME_INTERVAL_MS);

  animation_destroy(a);
  animation_destroy(b);
  animation_destroy(c);
}

// --------------------------------------------------------------------------------------
// Test unscheduling animations arbitrarily in a stopped handler with auto destroy.
void test_animation__unschedule_all_in_stopped_handler_with_auto_destroy(void) {
  prv_test_unschedule_all_in_stopped_handler(true);
}

// --------------------------------------------------------------------------------------
// Test unscheduling animations arbitrarily in a stopped handler without auto destroy.
void test_animation__unschedule_all_in_stopped_handler_without_auto_destroy(void) {
  prv_test_unschedule_all_in_stopped_handler(false);
}

void test_animation__custom_functions(void) {
  // just some pointer to compare against
  AnimationCurveFunction curve = (void*)1;
  InterpolateInt64Function interpolation = (void*)2;

  Animation *a = prv_create_test_animation();
  cl_assert_equal_p(animation_get_custom_curve(a), NULL);
  cl_assert_equal_p(animation_get_custom_interpolation(a), NULL);
  cl_assert_equal_i(animation_get_curve(a), AnimationCurveDefault);

  animation_set_custom_curve(a, curve);
  cl_assert_equal_p(animation_get_custom_curve(a), curve);
  cl_assert_equal_p(animation_get_custom_interpolation(a), NULL);
  cl_assert_equal_i(animation_get_curve(a), AnimationCurveCustomFunction);

  animation_set_custom_interpolation(a, interpolation);
  cl_assert_equal_p(animation_get_custom_curve(a), NULL);
  cl_assert_equal_p(animation_get_custom_interpolation(a), interpolation);
  cl_assert_equal_i(animation_get_curve(a), AnimationCurveCustomInterpolationFunction);

  animation_set_curve(a, AnimationCurveDefault);
  cl_assert_equal_p(animation_get_custom_curve(a), NULL);
  cl_assert_equal_p(animation_get_custom_interpolation(a), NULL);
  cl_assert_equal_i(animation_get_curve(a), AnimationCurveDefault);

  animation_destroy(a);
}

void test_animation__current_interpolate_override(void) {
  // just some pointer to compare against
  AnimationCurveFunction curve = (void*)1;
  InterpolateInt64Function interpolation = (void*)2;

  AnimationState *state = kernel_applib_get_animation_state();
  cl_assert_equal_p(state->aux->current_animation, NULL);
  cl_assert_equal_p(animation_private_current_interpolate_override(), NULL);

  Animation *a = prv_create_test_animation();
  AnimationPrivate *a_p = animation_private_animation_find(a);
  state->aux->current_animation = a_p;
  cl_assert_equal_p(animation_private_current_interpolate_override(), NULL);

  animation_set_custom_interpolation(a, interpolation);
  cl_assert_equal_p(animation_private_current_interpolate_override(), interpolation);

  animation_set_custom_curve(a, curve);
  cl_assert_equal_p(animation_private_current_interpolate_override(), NULL);

  animation_destroy(a);
}
