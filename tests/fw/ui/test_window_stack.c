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

#include "applib/ui/app_window_stack.h"
#include "applib/ui/window.h"
#include "applib/ui/window_manager.h"
#include "applib/ui/window_stack.h"
#include "applib/ui/window_stack_private.h"
#include "kernel/ui/modals/modal_manager.h"

#include "applib/connection_service_private.h"
#include "applib/battery_state_service_private.h"
#include "applib/tick_timer_service_private.h"

#include "clar.h"

// Stubs
////////////////////////////////////

#include "stubs_accel_service.h"
#include "stubs_app_state.h"
#include "stubs_app_timer.h"
#include "stubs_ble_app_support.h"
#include "stubs_event_service_client.h"
#include "stubs_fonts.h"
#include "stubs_freertos.h"
#include "stubs_gbitmap.h"
#include "stubs_graphics.h"
#include "stubs_graphics_context.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_persist.h"
#include "stubs_plugin_service.h"
#include "stubs_print.h"
#include "stubs_process_manager.h"
#include "stubs_prompt.h"
#include "stubs_queue.h"
#include "stubs_resources.h"
#include "stubs_syscalls.h"
#include "stubs_unobstructed_area.h"

// Fakes
////////////////////////////////////

#include "fake_events.h"
#include "fake_pbl_malloc.h"
#include "fake_pebble_tasks.h"
#include "fake_animation.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Static Variables
////////////////////////////////////

static int16_t s_load_count = 0;
static int16_t s_unload_count = 0;
static int16_t s_appear_count = 0;
static int16_t s_disappear_count = 0;

static Window *s_last_click_configured_window;

static bool s_app_idle = false;

// Overrides
////////////////////////////////////

void battery_state_service_state_init(BatteryStateServiceState *state) {
  return;
}

void connection_service_state_init(ConnectionServiceState *state) {
}

void tick_timer_service_state_init(TickTimerServiceState *state) {
  return;
}

void framebuffer_clear(FrameBuffer* f) {
  return;
}

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

void app_idle_timeout_pause(void) {
  s_app_idle = true;
}

void app_idle_timeout_resume(void) {
  s_app_idle = false;
}

bool app_install_id_from_app_db(AppInstallId id) {
  return false;
}

void framebuffer_dirty_all(FrameBuffer *f) {
  return;
}

void framebuffer_mark_dirty_rect(FrameBuffer *f, GRect rect) {
  return;
}

bool layer_is_status_bar_layer(Layer *layer) {
  return false;
}

void status_bar_layer_render(GContext *ctx, const GRect *bounds, void *config) {
  return;
}

GDrawState graphics_context_get_drawing_state(GContext* ctx) {
  GDrawState state;
  memset(&state, 0, sizeof(GDrawState));
  return state;
}

void graphics_context_set_drawing_state(GContext* ctx, GDrawState draw_state) {
  return;
}

bool compositor_is_animating(void) {
  return false;
}

void *compositor_modal_transition_to_modal_get(bool dest) {
  return NULL;
}

void compositor_modal_render_ready(void) {
}

void compositor_transition_cancel(void) {
}

bool sys_app_is_watchface(void) {
  return false;
}

void click_manager_init(ClickManager *click_manager) {
  return;
}

void click_manager_clear(ClickManager *click_manager) {
  return;
}

void click_manager_reset(ClickManager *click_manager) {
  return;
}

void watchface_reset_click_manager(void) {
  return;
}

Animation *window_transition_default_pop_create_animation(WindowTransitioningContext *context) {
  window_transition_context_disappear(context);
  window_transition_context_appear(context);
  return animation_create();
}

const WindowTransitionImplementation window_transition_default_pop_implementation = {
  .create_animation = window_transition_default_pop_create_animation,
};

const WindowTransitionImplementation *window_transition_get_default_pop_implementation() {
  return &window_transition_default_pop_implementation;
}

Animation *window_transition_default_push_create_animation(WindowTransitioningContext *context) {
  window_transition_context_disappear(context);
  window_transition_context_appear(context);
  return animation_create();
}

const WindowTransitionImplementation window_transition_default_push_implementation = {
  .create_animation = window_transition_default_push_create_animation,
};

const WindowTransitionImplementation *window_transition_get_default_push_implementation() {
  return &window_transition_default_push_implementation;
}

Animation *window_transition_none_create_animation(WindowTransitioningContext *context) {
  window_transition_context_disappear(context);
  window_transition_context_appear(context);
  return animation_create();
}

const WindowTransitionImplementation g_window_transition_none_implementation = {
  .create_animation = window_transition_none_create_animation,
};

void compositor_transition(const CompositorTransition *type) {
  Window *window = modal_manager_get_top_window();
  if (window) {
    GContext ctx;
    memset(&ctx, 0, sizeof(GContext));
    modal_manager_render(&ctx);
  }
}

void app_click_config_setup_with_window(ClickManager *click_manager, struct Window *window) {
  s_last_click_configured_window = window;
}

// Helpers
////////////////////////////////////
static int16_t prv_get_load_unload_count(void) {
  return s_load_count - s_unload_count;
}

static int16_t prv_get_appear_disappear_count(void) {
  return s_appear_count - s_disappear_count;
}

static void prv_reset_counts(void) {
  s_load_count = 0;
  s_unload_count = 0;
  s_appear_count = 0;
  s_disappear_count = 0;
}

static void prv_click_config_provider(void *context) {
  return;
}

static void prv_window_appear(Window *window) {
  cl_check(window);
  cl_assert_equal_i(window->on_screen, true);
  s_appear_count++;
  cl_check(s_appear_count >= 1);
}

static void prv_window_disappear(Window *window) {
  cl_check(window);
  cl_assert_equal_i(window->on_screen, false);
  s_disappear_count++;
  cl_check(s_appear_count >= 0);
}

static void prv_window_load(Window *window) {
  cl_check(window);
  cl_assert_equal_i(window->on_screen, true);
  s_load_count++;
}

static void prv_window_unload(Window *window) {
  cl_check(window);
  cl_assert_equal_i(window->on_screen, false);
  s_unload_count++;
  window_destroy(window);
}

static void prv_push_window_load(Window *window) {
  prv_window_load(window);
  Window *new_window = window_create();
  window_set_window_handlers(new_window, &(WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload
  });

  cl_check(window->parent_window_stack);
  cl_assert_equal_i(window->on_screen, true);
  cl_assert_equal_i(window->is_loaded, false);

  window_stack_push(window->parent_window_stack, new_window, true);
}

static void prv_pop_window_load(Window *window) {
  prv_window_load(window);

  cl_check(window->parent_window_stack);
  cl_assert_equal_i(window->on_screen, true);
  cl_assert_equal_i(window->is_loaded, false);

  window_stack_pop(window->parent_window_stack, true);
}

static void prv_push_window_unload(Window *window) {
  WindowStack *stack = window->parent_window_stack;

  prv_window_unload(window);

  Window *new_window = window_create();
  window_set_window_handlers(new_window, &(WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
    .appear = prv_window_appear
  });

  cl_check(stack);
  cl_check(new_window);

  window_stack_push(stack, new_window, true);
}

static void prv_pop_window_unload(Window *window) {
  window_stack_remove(window, true);
  prv_window_unload(window);
}

// Setup and Teardown
////////////////////////////////////

void test_window_stack__initialize(void) {
  s_last_click_configured_window = NULL;

  WindowStack *stack = app_state_get_window_stack();
  *stack = (WindowStack) {};

  modal_manager_reset();

  prv_reset_counts();

  stub_pebble_tasks_set_current(PebbleTask_KernelMain);
}

void test_window_stack__cleanup(void) {
  stub_pebble_tasks_set_current(PebbleTask_App);

  app_window_stack_pop_all(false);

  stub_pebble_tasks_set_current(PebbleTask_KernelMain);

  modal_manager_pop_all();

  fake_animation_cleanup();

  cl_assert_equal_i(fake_pbl_malloc_num_net_allocs(), 0);
}

// Tests
////////////////////////////////////

void test_window_stack__basic_app_push(void) {
  Window *window = window_create();

  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(app_state_get_window_stack());
  cl_check(!app_state_get_window_stack()->list_head);

  app_window_stack_push(window, true);

  cl_assert_equal_i(app_window_stack_count(), 1);

  app_window_stack_pop(true);

  cl_assert_equal_i(app_window_stack_count(), 0);

  window_destroy(window);
}

void test_window_stack__basic_modal_push(void) {
  Window *window = window_create();
  WindowStack *window_stack =
      modal_manager_get_window_stack(ModalPriorityGeneric);

  cl_check(window_stack);
  cl_check(!window_stack->list_head);

  window_stack_push(window_stack, window, true);

  cl_assert_equal_i(window_stack_count(window_stack), 1);

  window_stack_pop(window_stack, true);

  cl_assert_equal_i(window_stack_count(window_stack), 0);

  window_destroy(window);
}

void test_window_stack__basic_window_pop(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();

  // Switch to app state to push windows to the Application window stack
  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(app_state_get_window_stack());
  cl_check(!app_state_get_window_stack()->list_head);

  app_window_stack_push(window1, true);

  cl_assert_equal_i(app_window_stack_count(), 1);
  cl_assert_equal_i(window1->on_screen, true);

  app_window_stack_push(window2, true);

  cl_assert_equal_i(app_window_stack_count(), 2);
  cl_assert_equal_i(window1->on_screen, false);
  cl_assert_equal_i(window2->on_screen, true);

  app_window_stack_pop(true);

  cl_assert_equal_i(window1->on_screen, true);
  cl_assert_equal_i(window2->on_screen, false);

  app_window_stack_pop(true);

  cl_assert_equal_i(window1->on_screen, false);

  window_destroy(window1);
  window_destroy(window2);
}

void test_window_stack__basic_window_pop_under(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();

  // Switch to app state to push windows to the Application window stack
  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(app_state_get_window_stack());
  cl_check(!app_state_get_window_stack()->list_head);

  app_window_stack_push(window1, true);

  cl_assert_equal_i(app_window_stack_count(), 1);
  cl_assert_equal_i(window1->on_screen, true);

  app_window_stack_push(window2, true);

  cl_assert_equal_i(app_window_stack_count(), 2);
  cl_assert_equal_i(window1->on_screen, false);
  cl_assert_equal_i(window2->on_screen, true);

  app_window_stack_remove(window1, false);

  cl_assert_equal_i(app_window_stack_count(), 1);
  cl_assert_equal_i(window2->on_screen, true);
  cl_assert_equal_i(window1->on_screen, false);

  app_window_stack_remove(window2, false);

  cl_assert_equal_i(app_window_stack_count(), 0);
  cl_assert_equal_i(window2->on_screen, false);
  cl_assert_equal_i(window1->on_screen, false);

  window_destroy(window1);
  window_destroy(window2);
}

void test_window_stack__pop_all(void) {
  WindowStack *stack = modal_manager_get_window_stack(ModalPriorityGeneric);
  Window *windows[3];

  for (uint8_t idx = 0; idx < 3; idx++) {
    windows[idx] = window_create();
  }

  window_stack_push(stack, windows[0], true);
  cl_assert_equal_i(window_stack_count(stack), 1);
  cl_assert_equal_i(windows[0]->on_screen, true);

  window_stack_push(stack, windows[1], true);
  cl_assert_equal_i(window_stack_count(stack), 2);
  cl_assert_equal_i(windows[0]->on_screen, false);
  cl_assert_equal_i(windows[1]->on_screen, true);

  window_stack_push(stack, windows[2], true);
  cl_assert_equal_i(window_stack_count(stack), 3);
  cl_assert_equal_i(windows[0]->on_screen, false);
  cl_assert_equal_i(windows[1]->on_screen, false);
  cl_assert_equal_i(windows[2]->on_screen, true);

  window_stack_pop_all(stack, true);

  cl_assert_equal_i(window_stack_count(stack), 0);
  cl_assert_equal_i(windows[0]->on_screen, false);
  cl_assert_equal_i(windows[1]->on_screen, false);
  cl_assert_equal_i(windows[2]->on_screen, false);

  for (uint8_t idx = 0; idx < 3; idx++) {
    window_destroy(windows[idx]);
  }
}

void test_window_stack__insert_next(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();

  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(app_state_get_window_stack());
  cl_check(!app_state_get_window_stack()->list_head);

  app_window_stack_push(window1, true);

  cl_assert_equal_i(app_window_stack_count(), 1);
  cl_assert_equal_i(window1->on_screen, true);

  app_window_stack_insert_next(window2);

  cl_assert_equal_i(app_window_stack_count(), 2);
  cl_assert_equal_i(window1->on_screen, true);
  cl_assert_equal_i(window2->on_screen, false);

  app_window_stack_pop(true);

  cl_assert_equal_i(app_window_stack_count(), 1);
  cl_assert_equal_i(window1->on_screen, false);
  cl_assert_equal_i(window2->on_screen, true);

  app_window_stack_pop(true);

  cl_assert_equal_i(app_window_stack_count(), 0);
  cl_assert_equal_i(window2->on_screen, false);

  window_destroy(window1);
  window_destroy(window2);
}

// Description:
// During the push of a window, we push another window in the load handler of
// the window being pushed.  This causes the loading window to disappear from
// the screen (before it even appeared) and become subverted by the new window.
void test_window_stack__push_during_window_load(void) {
  Window *window = window_create();
  window_set_window_handlers(window, &(WindowHandlers){
    .load = prv_push_window_load,
    .unload = prv_window_unload,
    .appear = prv_window_appear,
    .disappear = prv_window_disappear
  });

  WindowStack *stack = app_state_get_window_stack();

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(stack);
  cl_assert_equal_i(window_stack_count(stack), 0);

  window_stack_push(stack, window, true);

  cl_assert_equal_i(window_stack_count(stack), 2);
  cl_assert_equal_i(prv_get_load_unload_count(), 2);
  cl_assert_equal_i(prv_get_appear_disappear_count(), 0);

  cl_check(((WindowStackItem *)stack->list_head)->window != window);

  window_stack_pop_all(stack, false);

  cl_assert_equal_i(window_stack_count(stack), 0);
  cl_assert_equal_i(prv_get_load_unload_count(), 0);
  cl_assert_equal_i(prv_get_appear_disappear_count(), 0);
}

// Description:
// This test ensures that when we push windows onto modal window stacks, that
// only the appropriate window is visible at a given time.
void test_window_stack__modal_priority(void) {
  Window *windows[NumModalPriorities];
  WindowStack *window_stacks[NumModalPriorities];
  ModalPriority idx = ModalPriorityInvalid;

  do {
    ++idx;
    windows[idx] = window_create();
    window_set_window_handlers(windows[idx], &(WindowHandlers) {
      .unload = prv_window_unload
    });
    window_stacks[idx] = modal_manager_get_window_stack(idx);
  } while (idx < NumModalPriorities - 1);

  idx = ModalPriorityInvalid;

  do {
    ++idx;
    window_stack_push(window_stacks[idx], windows[idx], false);

    cl_assert_equal_i(window_stack_count(window_stacks[idx]), 1);
    cl_assert_equal_i(windows[idx]->on_screen, true);

    // All windows below the current priority should now not be on the screen
    // as the modal has subverted them.
    ModalPriority sub_idx = idx;
    do {
      sub_idx--;
      if (sub_idx == ModalPriorityInvalid) {
        break;
      }
      cl_assert_equal_i(window_stack_count(window_stacks[sub_idx]), 1);
      cl_assert_equal_i(windows[sub_idx]->on_screen, false);
    } while (true);
  } while (idx < NumModalPriorities - 1);
}

void test_window_stack__modal_properties_transparent(void) {
  const int num_windows_per_stack = 2;
  Window *windows[NumModalPriorities][num_windows_per_stack];
  WindowStack *window_stacks[NumModalPriorities];

  for (ModalPriority idx = ModalPriorityMin; idx < NumModalPriorities; idx++) {
    window_stacks[idx] = modal_manager_get_window_stack(idx);
    for (int i = 0; i < num_windows_per_stack; i++) {
      windows[idx][i] = window_create();
    }
  }

  // The following checks use integer priorities to clearly indicate stack order
  // We check to make sure the first occurrence of integer values are less than NumModalPriorities

  // Test: No top window does not result in Exists
  // Test: No top window results in Transparent and Unfocused
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Transparent | ModalProperty_Unfocused);

  // Add priority 0 (discreet) opaque window 0
  cl_assert(NumModalPriorities > 0);
  window_stack_push(window_stacks[0], windows[0][0], false);

  // A discreet window just went on-screen
  // Test: Discreet windows have no compositor transition
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_RenderRequested);

  window_stack_remove(windows[0][0], false);

  // Add priority 2 opaque window 0
  cl_assert(NumModalPriorities > 2);
  window_stack_push(window_stacks[2], windows[2][0], false);

  // An opaque window just went on-screen
  // Test: A top window results in Exists
  // Test: One opaque top window removes Transparent and Unfocused
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested);
  cl_assert_equal_i(windows[2][0]->on_screen, true);
  cl_assert_equal_i(windows[2][0]->is_click_configured, true);
  cl_assert_equal_p(s_last_click_configured_window, windows[2][0]);

  // Add priority 2 transparent window 1
  window_set_transparent(windows[2][1], true);
  window_stack_push(window_stacks[2], windows[2][1], false);

  // A transparent window is now the top window
  // Test: Opaque windows that are not the top window have no affect on transparency
  // Test: One transparent top window results in Transparent
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested | ModalProperty_Transparent);
  // Checks are listed from top to bottom
  cl_assert_equal_i(windows[2][1]->on_screen, true);
  cl_assert_equal_i(windows[2][1]->is_click_configured, true);
  cl_assert_equal_i(windows[2][0]->on_screen, false);
  cl_assert_equal_i(windows[2][0]->is_click_configured, false);
  cl_assert_equal_p(s_last_click_configured_window, windows[2][1]);

  // Add priority 3 opaque window 0
  cl_assert(NumModalPriorities > 3);
  window_stack_push(window_stacks[3], windows[3][0], false);

  // An opaque top window of a different stack is now obstructing the transparent top window
  // Top here throughout means that it is the top window of the window stack it is in
  // Test: An opaque top window above a transparent top window removes Transparent
  //       i.e. A transparent top window below an opaque top window does not result in Transparent
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested);
  cl_assert_equal_i(windows[3][0]->on_screen, true);
  cl_assert_equal_i(windows[3][0]->is_click_configured, true);
  cl_assert_equal_i(windows[2][1]->on_screen, false);
  cl_assert_equal_i(windows[2][1]->is_click_configured, false);
  cl_assert_equal_p(s_last_click_configured_window, windows[3][0]);

  // Add priority 3 transparent window 1
  window_set_transparent(windows[3][1], true);
  window_stack_push(window_stacks[3], windows[3][1], false);

  // A transparent window is now the top window, and there is another transparent window below
  // Test: Multiple transparent top windows result in Transparent
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested | ModalProperty_Transparent);
  cl_assert_equal_i(windows[3][1]->on_screen, true);
  cl_assert_equal_i(windows[3][1]->is_click_configured, true);
  cl_assert_equal_i(windows[3][0]->on_screen, false);
  cl_assert_equal_i(windows[3][0]->is_click_configured, false);
  cl_assert_equal_i(windows[2][1]->on_screen, true);
  cl_assert_equal_i(windows[2][1]->is_click_configured, false);
  cl_assert_equal_p(s_last_click_configured_window, windows[3][1]);

  // Add priority 1 opaque window 0
  cl_assert(NumModalPriorities > 1);
  window_stack_push(window_stacks[1], windows[1][0], false);

  // An opaque top window is now below two transparent top windows
  // Test: An opaque top window below a transparent top window removes Transparent
  //       i.e. A transparent top window above an opaque top window does not result in Transparent
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested);
  cl_assert_equal_i(windows[3][1]->on_screen, true);
  cl_assert_equal_i(windows[3][1]->is_click_configured, true);
  cl_assert_equal_i(windows[2][1]->on_screen, true);
  cl_assert_equal_i(windows[2][1]->is_click_configured, false);
  cl_assert_equal_i(windows[1][0]->on_screen, true);
  cl_assert_equal_i(windows[1][0]->is_click_configured, false);
  cl_assert_equal_p(s_last_click_configured_window, windows[3][1]);

  for (ModalPriority idx = ModalPriorityMin; idx < NumModalPriorities; idx++) {
    for (int i = 0; i < num_windows_per_stack; i++) {
      window_stack_remove(windows[idx][i], false);
      window_destroy(windows[idx][i]);
    }
  }
}

void test_window_stack__modal_properties_unfocused(void) {
  const int num_windows_per_stack = 2;
  Window *windows[NumModalPriorities][num_windows_per_stack];
  WindowStack *window_stacks[NumModalPriorities];

  for (ModalPriority idx = ModalPriorityMin; idx < NumModalPriorities; idx++) {
    window_stacks[idx] = modal_manager_get_window_stack(idx);
    for (int i = 0; i < num_windows_per_stack; i++) {
      windows[idx][i] = window_create();
    }
  }

  // The following checks use integer priorities to clearly indicate stack order
  // We check to make sure the first occurrence of integer values are less than NumModalPriorities

  // Test: No top window does not result in Exists
  // Test: No top window results in Transparent and Unfocused
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Transparent | ModalProperty_Unfocused);

  // Add priority 2 opaque window 0
  cl_assert(NumModalPriorities > 2);
  window_stack_push(window_stacks[2], windows[2][0], false);

  // Add priority 2 unfocusable window 1
  window_set_focusable(windows[2][1], false);
  window_stack_push(window_stacks[2], windows[2][1], false);

  // An unfocusable window is now the top window
  // Test: Opaque windows that are not the top window have no affect on unfocusable
  // Test: One unfocusable top window results in Unfocused
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested | ModalProperty_Unfocused);
  // Checks are listed from top to bottom
  cl_assert_equal_i(windows[2][1]->on_screen, true);
  cl_assert_equal_i(windows[2][1]->is_click_configured, false);
  cl_assert_equal_i(windows[2][0]->on_screen, false);
  cl_assert_equal_i(windows[2][0]->is_click_configured, false);
  cl_assert_equal_p(s_last_click_configured_window, windows[2][0]);

  // Add priority 3 opaque window 0
  cl_assert(NumModalPriorities > 3);
  window_stack_push(window_stacks[3], windows[3][0], false);

  // An opaque top window of a different stack is now obstructing the unfocusable top window
  // Top here throughout means that it is the top window of the window stack it is in
  // Test: An opaque top window above a unfocusable top window removes Unfocusable
  //       i.e. A unfocusable top window below an opaque top window does not result in Unfocusable
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested);
  cl_assert_equal_i(windows[3][0]->on_screen, true);
  cl_assert_equal_i(windows[3][0]->is_click_configured, true);
  cl_assert_equal_i(windows[2][1]->on_screen, false);
  cl_assert_equal_i(windows[2][1]->is_click_configured, false);
  cl_assert_equal_p(s_last_click_configured_window, windows[3][0]);

  // Add priority 3 unfocusable window 1
  window_set_focusable(windows[3][1], false);
  window_stack_push(window_stacks[3], windows[3][1], false);

  // A unfocusable window is now the top window, and there is another unfocusable window below
  // Test: Multiple unfocusable top windows result in Unfocusable
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested | ModalProperty_Unfocused);
  cl_assert_equal_i(windows[3][1]->on_screen, true);
  cl_assert_equal_i(windows[3][1]->is_click_configured, false);
  cl_assert_equal_i(windows[3][0]->on_screen, false);
  cl_assert_equal_i(windows[3][0]->is_click_configured, false);
  cl_assert_equal_i(windows[2][1]->on_screen, false);
  cl_assert_equal_i(windows[2][1]->is_click_configured, false);
  cl_assert_equal_p(s_last_click_configured_window, windows[3][0]);

  // Add priority 1 opaque window 0
  cl_assert(NumModalPriorities > 1);
  window_stack_push(window_stacks[1], windows[1][0], false);

  // An opaque top window is now below two unfocusable top windows
  // Test: An opaque top window below a unfocusable top window removes Unfocusable
  //       i.e. A unfocusable top window above an opaque top window does not result in Unfocusable
  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested);
  cl_assert_equal_i(windows[3][1]->on_screen, true);
  cl_assert_equal_i(windows[3][1]->is_click_configured, false);
  cl_assert_equal_i(windows[2][1]->on_screen, false);
  cl_assert_equal_i(windows[2][1]->is_click_configured, false);
  cl_assert_equal_i(windows[1][0]->on_screen, false);
  cl_assert_equal_i(windows[1][0]->is_click_configured, true);
  cl_assert_equal_p(s_last_click_configured_window, windows[1][0]);

  for (ModalPriority idx = ModalPriorityMin; idx < NumModalPriorities; idx++) {
    for (int i = 0; i < num_windows_per_stack; i++) {
      window_stack_remove(windows[idx][i], false);
      window_destroy(windows[idx][i]);
    }
  }
}

void test_window_stack__modal_properties_enable_disable(void) {
  // Enable all modals
  modal_manager_set_min_priority(ModalPriorityMin);

  Window *window1 = window_create();
  modal_window_push(window1, ModalPriorityGeneric, false);

  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions |
                    ModalProperty_RenderRequested);

  // Disable all modals
  modal_manager_set_min_priority(ModalPriorityMax);

  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Transparent | ModalProperty_Unfocused);

  // Re-enable all modals
  modal_manager_set_min_priority(ModalPriorityMin);

  modal_manager_event_loop_upkeep();
  cl_assert_equal_i(modal_manager_get_properties(),
                    ModalProperty_Exists | ModalProperty_CompositorTransitions);
}

// Description:
// This test ensures that when we push a window onto the modal window stack, then
// we push another window onto the modal window stack at a lower priority, then
// pushing the first at a lower priority than the second will bring the second onto
// the screen and subvert the first.
void test_window_stack__modal_reprioritize(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();
  uint8_t base_priority = ModalPriorityDiscreet + 3;

  window_set_click_config_provider(window1, prv_click_config_provider);

  window_set_click_config_provider(window2, prv_click_config_provider);

  modal_window_push(window1, base_priority, false);

  cl_assert_equal_i(window1->on_screen, true);

  modal_window_push(window2, base_priority - 1, false);

  cl_assert_equal_i(window1->on_screen, true);
  cl_assert_equal_i(window2->on_screen, false);

  modal_window_push(window1, base_priority - 2, false);

  modal_manager_event_loop_upkeep();

  cl_assert_equal_i(window1->on_screen, false);
  cl_assert_equal_i(window2->on_screen, true);

  window_stack_remove(window2, false);

  modal_manager_event_loop_upkeep();

  cl_assert_equal_i(window2->on_screen, false);
  cl_assert_equal_i(window1->on_screen, true);

  window_stack_remove(window1, true);

  cl_assert_equal_i(window1->on_screen, false);

  window_destroy(window1);
  window_destroy(window2);
}

// Description:
// This test ensures that we are able to work with both the modal window stacks
// and the application stack at the same time.
void test_window_stack__modal_and_app(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();

  WindowStack *app_stack = app_state_get_window_stack();
  WindowStack *modal_stack = modal_manager_get_window_stack(ModalPriorityGeneric);

  cl_check(app_stack);
  cl_check(modal_stack);

  cl_check(!app_stack->list_head);
  cl_check(!modal_stack->list_head);

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  window_stack_push(app_stack, window1, true);

  cl_assert_equal_i(window_stack_count(app_stack), 1);
  cl_assert_equal_i(window1->on_screen, true);
  cl_assert_equal_p(s_last_click_configured_window, window1);

  // Switch to the kernel to push a modal window
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);

  window_stack_push(modal_stack, window2, true);

  cl_assert_equal_i(window_stack_count(modal_stack), 1);
  cl_assert_equal_i(window2->on_screen, true);
  cl_assert_equal_p(s_last_click_configured_window, window2);

  // Switch to modal happens via the compositor
  compositor_transition(NULL);
  // Call the upkeep function so the change in state is handled
  modal_manager_event_loop_upkeep();

  // The app is now obstructed by an opaque modal, it should be idle
  cl_assert_equal_b(s_app_idle, true);

  // Assert that the window pushed onto the app stack has lost focus
  // We do this by checking the last event, which should have been a focus
  // lost event.
  PebbleEvent event = fake_event_get_last();
  cl_assert_equal_i(event.type, PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT);
  cl_assert_equal_i(event.app_focus.in_focus, false);

  cl_assert_equal_i(window_stack_count(app_stack), 1);

  // Pop the modal window off the stack
  window_stack_remove(window2, true);

  cl_assert_equal_i(window_stack_count(modal_stack), 0);
  cl_assert_equal_i(window2->on_screen, false);

  // Switch to app happens via the compositor
  compositor_transition(NULL);
  // Call the upkeep function so the change in state is handled
  modal_manager_event_loop_upkeep();

  // The app is unobstructed, it should not be idle
  cl_assert_equal_b(s_app_idle, false);

  // Assert that the window pushed onto the app stack has regained focus,
  // this is also done by checking the last event.
  event = fake_event_get_last();

  cl_assert_equal_i(event.type, PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT);
  cl_assert_equal_i(event.app_focus.in_focus, true);

  cl_assert_equal_i(window1->on_screen, true);

  window_stack_remove(window1, true);

  cl_assert_equal_i(window1->on_screen, false);
  cl_assert_equal_i(window_stack_count(app_stack), 0);

  window_destroy(window1);
  window_destroy(window2);
}

// Tests modal and app transitions with a transparent modal window
void test_window_stack__transparent_modal_and_app(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();
  cl_assert_equal_b(window_is_transparent(window2), false);
  window_set_transparent(window2, true);
  cl_assert_equal_b(window_is_transparent(window2), true);

  WindowStack *app_stack = app_state_get_window_stack();
  WindowStack *modal_stack = modal_manager_get_window_stack(ModalPriorityGeneric);

  cl_check(app_stack);
  cl_check(modal_stack);

  cl_check(!app_stack->list_head);
  cl_check(!modal_stack->list_head);

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  window_stack_push(app_stack, window1, true);

  cl_assert_equal_i(window_stack_count(app_stack), 1);
  cl_assert_equal_i(window1->on_screen, true);
  cl_assert_equal_p(s_last_click_configured_window, window1);

  // Switch to the kernel to push a modal window
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);

  window_stack_push(modal_stack, window2, true);

  cl_assert_equal_i(window_stack_count(modal_stack), 1);
  cl_assert_equal_i(window2->on_screen, true);
  cl_assert_equal_p(s_last_click_configured_window, window2);

  // Switch to modal happens via the compositor
  compositor_transition(NULL);
  // Call the upkeep function so the change in state is handled
  modal_manager_event_loop_upkeep();

  // The app is now obstructed by a transparent modal, it should remain active
  cl_assert_equal_b(s_app_idle, false);

  // Assert that the window pushed onto the app stack has lost focus
  // We do this by checking the last event, which should have been a focus
  // lost event.
  PebbleEvent event = fake_event_get_last();
  cl_assert_equal_i(event.type, PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT);
  cl_assert_equal_i(event.app_focus.in_focus, false);

  cl_assert_equal_i(window_stack_count(app_stack), 1);

  // Pop the modal window off the stack
  window_stack_remove(window2, true);

  cl_assert_equal_i(window_stack_count(modal_stack), 0);
  cl_assert_equal_i(window2->on_screen, false);

  // Switch to app happens via the compositor
  compositor_transition(NULL);
  // Call the upkeep function so the change in state is handled
  modal_manager_event_loop_upkeep();

  // The app is unobstructed, it should remain active
  cl_assert_equal_b(s_app_idle, false);

  // Assert that the window pushed onto the app stack has regained focus,
  // this is also done by checking the last event.
  event = fake_event_get_last();

  cl_assert_equal_i(event.type, PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT);
  cl_assert_equal_i(event.app_focus.in_focus, true);

  cl_assert_equal_i(window1->on_screen, true);

  window_stack_remove(window1, true);

  cl_assert_equal_i(window1->on_screen, false);
  cl_assert_equal_i(window_stack_count(app_stack), 0);

  window_destroy(window1);
  window_destroy(window2);
}

// Tests modal and app transitions with an unfocusable modal window
void test_window_stack__unfocusable_modal_and_app(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();
  cl_assert_equal_b(window_is_focusable(window2), true);
  window_set_focusable(window2, false);
  cl_assert_equal_b(window_is_focusable(window2), false);

  WindowStack *app_stack = app_state_get_window_stack();
  WindowStack *modal_stack = modal_manager_get_window_stack(ModalPriorityGeneric);

  cl_check(app_stack);
  cl_check(modal_stack);

  cl_check(!app_stack->list_head);
  cl_check(!modal_stack->list_head);

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  window_stack_push(app_stack, window1, true);

  cl_assert_equal_i(window_stack_count(app_stack), 1);
  cl_assert_equal_i(window1->on_screen, true);
  cl_assert_equal_p(s_last_click_configured_window, window1);

  // Switch to the kernel to push a modal window
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);

  window_stack_push(modal_stack, window2, true);

  cl_assert_equal_i(window_stack_count(modal_stack), 1);
  cl_assert_equal_i(window2->on_screen, true);
  cl_assert_equal_p(s_last_click_configured_window, window1);

  // Switch to modal happens via the compositor
  compositor_transition(NULL);
  // Call the upkeep function so the change in state is handled
  modal_manager_event_loop_upkeep();

  // The app is now obstructed by a unfocusable modal, it should remain active
  cl_assert_equal_b(s_app_idle, false);

  // The app should retain focus
  PebbleEvent event = fake_event_get_last();
  cl_assert_equal_i(event.type, PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT);
  cl_assert_equal_i(event.app_focus.in_focus, true);

  cl_assert_equal_i(window_stack_count(app_stack), 1);

  // Pop the modal window off the stack
  window_stack_remove(window2, true);

  cl_assert_equal_i(window_stack_count(modal_stack), 0);
  cl_assert_equal_i(window2->on_screen, false);

  // Switch to app happens via the compositor
  compositor_transition(NULL);
  // Call the upkeep function so the change in state is handled
  modal_manager_event_loop_upkeep();

  // The app is unobstructed, it should remain active
  cl_assert_equal_b(s_app_idle, false);

  // Assert that the window pushed onto the app stack has remained focused
  event = fake_event_get_last();

  cl_assert_equal_i(event.type, PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT);
  cl_assert_equal_i(event.app_focus.in_focus, true);

  cl_assert_equal_i(window1->on_screen, true);

  window_stack_remove(window1, true);

  cl_assert_equal_i(window1->on_screen, false);
  cl_assert_equal_i(window_stack_count(app_stack), 0);

  window_destroy(window1);
  window_destroy(window2);
}

// Description:
// This test ensures that the flow of adding a window to the window stack is followed
// correctly.  That is, we add the window to the window stack, its load handler is
// called, it calls to set the click config, and the click config is set properly.
void test_window_stack__window_flow(void) {
  Window *window = window_create();

  window_set_window_handlers(window, &(WindowHandlers){
    .load = prv_window_load,
    .unload = prv_window_unload,
    .appear = prv_window_appear,
    .disappear = prv_window_disappear
  });

  window_set_click_config_provider(window, prv_click_config_provider);

  cl_check(window->is_waiting_for_click_config);

  WindowStack *stack = app_state_get_window_stack();

  cl_check(stack);
  cl_check(!stack->list_head);

  // Switch to the app state to push the window
  stub_pebble_tasks_set_current(PebbleTask_App);

  window_stack_push(stack, window, true);

  cl_assert_equal_i(window->on_screen, 1);
  cl_assert_equal_i(window_stack_count(stack), 1);

  // Ensure the load handler was called
  cl_assert_equal_i(prv_get_load_unload_count(), 1);

  // Ensure the appear handler was called
  cl_assert_equal_i(prv_get_appear_disappear_count(), 1);

  // Ensure the click config handler was called
  cl_assert_equal_i(window->is_waiting_for_click_config, false);

  window_stack_pop(stack, false);

  cl_assert_equal_i(window_stack_count(stack), 0);

  // Ensure the disappear handler was called
  cl_assert_equal_i(prv_get_appear_disappear_count(), 0);

  // Ensure the unload handler was called
  cl_assert_equal_i(prv_get_load_unload_count(), 0);
}

void test_window_stack__dump(void) {
  WindowStack *stack = app_state_get_window_stack();
  Window *window1 = window_create();
  Window *window2 = window_create();
  Window *window3 = window_create();
  window1->debug_name = "Window1";
  window2->debug_name = "Window2";
  window3->debug_name = "Window3";

  window_stack_push(stack, window1, true);
  window_stack_push(stack, window2, true);
  window_stack_push(stack, window3, true);

  WindowStackDump *dump;
  size_t stack_depth = window_stack_dump(stack, &dump);
  cl_assert_equal_i(stack_depth, 3);
  cl_assert_equal_p(dump[0].addr, window3);
  cl_assert_equal_s(dump[0].name, "Window3");
  cl_assert_equal_p(dump[1].addr, window2);
  cl_assert_equal_s(dump[1].name, "Window2");
  cl_assert_equal_p(dump[2].addr, window1);
  cl_assert_equal_s(dump[2].name, "Window1");
  kernel_free(dump);
}

void test_window_stack__pop_all_modals(void) {
  Window *windows[NumModalPriorities];
  WindowStack *window_stacks[NumModalPriorities];

  for (ModalPriority idx = ModalPriorityMin; idx < NumModalPriorities; idx++) {
    window_stacks[idx] = modal_manager_get_window_stack(idx);
    windows[idx] = window_create();
    window_stack_push(window_stacks[idx], windows[idx], true);
    // Modals are visible as we push from lowest priority to top most
    cl_assert_equal_b(windows[idx]->on_screen, true);
  }

  // Only the top modal is visible
  modal_manager_event_loop_upkeep();
  for (ModalPriority idx = ModalPriorityMin; idx < NumModalPriorities; idx++) {
    cl_assert_equal_b(windows[idx]->on_screen, (idx == NumModalPriorities - 1));
  }

  // Pop all modals
  modal_manager_pop_all();
  modal_manager_event_loop_upkeep();

  _Static_assert(ModalPriorityMin == ModalPriorityDiscreet,
                 "Update the test to handle priorities below discreet.");
  // Discreet should not be popped
  cl_assert_equal_b(windows[ModalPriorityDiscreet]->on_screen, true);

  // All other modals should be popped
  for (ModalPriority idx = ModalPriorityDiscreet + 1; idx < NumModalPriorities; idx++) {
    cl_assert_equal_b(windows[idx]->on_screen, false);
  }

}

// Edge Case Tests
////////////////////////////////////

// Description:
// During the load handler of a window, we pop it.
void test_window_stack__pop_during_window_load(void) {
  Window *window = window_create();
  window_set_window_handlers(window, &(WindowHandlers){
    .load = prv_pop_window_load,
    .unload = prv_window_unload
  });

  WindowStack *stack = app_state_get_window_stack();

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(stack);
  cl_assert_equal_i(window_stack_count(stack), 0);

  window_stack_push(stack, window, true);

  cl_assert_equal_i(window_stack_count(stack), 0);

  // We popped the window off the screen, but the unload handler
  // should not have been called for it, as it hasn't finished
  // unloading.
  cl_assert_equal_i(prv_get_load_unload_count(), 1);
}

// Description:
// In this test, we push a window during the unload handler of a window.
void test_window_stack__push_during_window_unload(void) {
  Window *window = window_create();

  window_set_window_handlers(window, &(WindowHandlers){
    .load = prv_window_load,
    .unload = prv_push_window_unload
  });

  WindowStack *stack = app_state_get_window_stack();

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(stack);
  cl_assert_equal_i(window_stack_count(stack), 0);

  window_stack_push(stack, window, true);

  cl_assert_equal_i(window_stack_count(stack), 1);
  cl_assert_equal_i(window->on_screen, true);
  cl_assert_equal_i(prv_get_load_unload_count(), 1);

  window_stack_pop(stack, true);

  cl_assert_equal_i(window_stack_count(stack), 1);
  cl_assert_equal_i(prv_get_load_unload_count(), 1);
}

// Description:
// In this test we push two windows that push windows during their unload handlers.
// We want to verify that those two windows stay on the stack after calling
// `window_stack_pop_all`.
void test_window_stack__push_during_window_unload_multiple(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();

  window_set_window_handlers(window1, &(WindowHandlers){
    .load = prv_window_load,
    .unload = prv_push_window_unload,
    .appear = prv_window_appear,
    .disappear = prv_window_disappear
  });

  window_set_window_handlers(window2, &(WindowHandlers){
    .load = prv_window_load,
    .unload = prv_push_window_unload,
    .appear = prv_window_appear,
    .disappear = prv_window_disappear
  });

  WindowStack *stack = app_state_get_window_stack();

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(stack);
  cl_assert_equal_i(window_stack_count(stack), 0);

  window_stack_push(stack, window1, true);
  window_stack_push(stack, window2, true);

  cl_assert_equal_i(window_stack_count(stack), 2);
  cl_assert_equal_i(prv_get_load_unload_count(), 2);
  cl_assert_equal_i(prv_get_appear_disappear_count(), 1);

  window_stack_pop_all(stack, true);

  cl_assert_equal_i(window_stack_count(stack), 2);
  cl_assert_equal_i(prv_get_load_unload_count(), 2);
  cl_assert_equal_i(prv_get_appear_disappear_count(), 2);
  cl_assert_equal_i(s_disappear_count, 2);
  cl_assert_equal_i(s_appear_count, 4);

  window_stack_pop_all(stack, true);

  cl_assert_equal_i(window_stack_count(stack), 0);
  cl_assert_equal_i(prv_get_load_unload_count(), 0);
}

// Edge Case
// Description:
// During the unload handler of a window, we try to pop it.
void test_window_stack__pop_during_window_unload(void) {
  Window *window = window_create();

  window_set_window_handlers(window, &(WindowHandlers){
    .load = prv_window_load,
    .unload = prv_pop_window_unload,
    .appear = prv_window_appear,
    .disappear = prv_window_disappear
  });

  WindowStack *stack = app_state_get_window_stack();

  // Switch to the app state to push a window
  stub_pebble_tasks_set_current(PebbleTask_App);

  cl_check(stack);
  cl_assert_equal_i(window_stack_count(stack), 0);

  window_stack_push(stack, window, true);

  cl_assert_equal_b(animation_is_scheduled(fake_animation_get_first_animation()), true);
  cl_assert_equal_i(window_stack_count(stack), 1);
  cl_assert_equal_i(prv_get_load_unload_count(), 1);
  cl_assert_equal_i(prv_get_appear_disappear_count(), 1);

  window_stack_remove(window, true);

  cl_assert_equal_i(window_stack_count(stack), 0);
  cl_assert_equal_i(prv_get_load_unload_count(), 0);
  cl_assert_equal_i(prv_get_appear_disappear_count(), 0);
  // FIXME: PBL-25460
  //  cl_assert_equal_b(animation_is_scheduled(fake_animation_get_first_animation()), false);
}

// Push two windows back to back, before the first transition completes. This should cancel the
// first transition and instead run the second transition.
void test_window_stack__double_animated_push(void) {
  Window *window1 = window_create();
  Window *window2 = window_create();

  WindowStack *stack = app_state_get_window_stack();

  window_stack_push(stack, window1, true);
  Animation *first = fake_animation_get_first_animation();

  cl_assert(animation_is_scheduled(first));

  window_stack_push(stack, window2, true);
  Animation *second = fake_animation_get_next_animation(first);
  cl_assert(!animation_is_scheduled(first));
  cl_assert(animation_is_scheduled(second));
}
