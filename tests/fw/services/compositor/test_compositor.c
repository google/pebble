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

#include "applib/graphics/gcontext.h"
#include "applib/graphics/gtypes.h"
#include "kernel/events.h"
#include "kernel/ui/modals/modal_manager.h"
#include "services/common/compositor/compositor.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_compiled_with_legacy2_sdk.h"
#include "stubs_compositor_dma.h"
#include "stubs_framebuffer.h"
#include "stubs_gbitmap.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_timeline_peek.h"

extern void prv_handle_display_update_complete(void);

static int s_count_animation_create = 0;
Animation* animation_create(void) {
  ++s_count_animation_create;

  return (Animation*) (uintptr_t) s_count_animation_create;
}

static int s_count_animation_schedule = 0;
static Animation *s_scheduled_animation;
bool animation_schedule(Animation *animation) {
  ++s_count_animation_schedule;
  s_scheduled_animation = animation;

  return true;
}

bool animation_set_auto_destroy(Animation *animation, bool auto_destroy) {
  return true;
}

bool animation_is_scheduled(Animation *animation_h) {
  return animation_h && animation_h == s_scheduled_animation;
}

static int s_count_animation_destroy = 0;
bool animation_unschedule(Animation *animation) {
  ++s_count_animation_destroy;
  s_scheduled_animation = NULL;
  return true;
}

bool animation_destroy(Animation *animation) {
  ++s_count_animation_destroy;
  s_scheduled_animation = NULL;
  return true;
}


static int s_app_window_render_count;
FrameBuffer* app_state_get_framebuffer(void) {
  // Not a great proxy for app rendering but good enough. This gets called twice per app render.
  ++s_app_window_render_count;

  return compositor_get_framebuffer();
}

void app_manager_get_framebuffer_size(GSize *size) {
  *size = (GSize) {DISP_COLS, DISP_ROWS};
}

void bitblt_bitmap_into_bitmap(GBitmap* dest_bitmap, const GBitmap* src_bitmap, GPoint dest_offset,
                               GCompOp compositing_mode, GColor tint_color) {
}

void compositor_dot_transition_app_to_app_init(Animation *animation) {
}

bool compositor_dot_transition_app_to_app_update_func(
    GContext *ctx, Animation *animation, uint32_t distance_normalized) {
  return true;
}

static bool s_modal_window_present = false;
Window* modal_manager_get_top_window(void) {
  return (Window*)(uintptr_t)s_modal_window_present;
}

static int s_modal_manager_render_count;
void modal_manager_render(GContext *ctx) {
  ++s_modal_manager_render_count;
}

ModalProperty modal_manager_get_properties(void) {
  return s_modal_window_present ? ModalProperty_Exists : ModalPropertyDefault;
}

GContext* kernel_ui_get_graphics_context(void) {
  static GContext s_context;
  return &s_context;
}

void framebuffer_clear(FrameBuffer* f) {
}

void framebuffer_dirty_all(FrameBuffer *fb) {
}

GBitmap framebuffer_get_as_bitmap(FrameBuffer *fb, const GSize *size) {
  return (GBitmap) { };
}

void framebuffer_set_line(FrameBuffer* f, uint8_t y, const uint8_t* buffer) {
}

GDrawState graphics_context_get_drawing_state(GContext* ctx) {
  return (GDrawState) { };
}

void graphics_context_set_drawing_state(GContext* ctx, GDrawState draw_state) {
}

AnimationPrivate *animation_private_animation_find(Animation *handle) {
  return NULL;
}

static int s_count_display_update = 0;
void compositor_display_update(void (*handle_update_complete_cb)(void)) {
  ++s_count_display_update;
}

static bool s_display_update_in_progress = false;
bool compositor_display_update_in_progress(void) {
  return s_display_update_in_progress;
}

static PebbleEvent s_last_event;
void event_put(PebbleEvent* event) {
  s_last_event = *event;
}

static bool s_render_pending;
bool process_manager_send_event_to_process(PebbleTask task, PebbleEvent *event) {
  if (event->type == PEBBLE_RENDER_FINISHED_EVENT) {
    s_render_pending = false;
  }
  s_last_event = *event;
  return true;
}

static const AnimationImplementation *s_animation_implementation;
bool animation_set_implementation(Animation *animation,
                                  const AnimationImplementation *implementation) {
  s_animation_implementation = implementation;
  return true;
}

static int s_count_compositor_init_func_a = 0;
static void prv_compositor_init_func_a(Animation *animation) {
  ++s_count_compositor_init_func_a;
}

static void prv_compositor_update_func_a(GContext *ctx, Animation *animation,
                                         uint32_t distance_normalized) {
}

static int s_count_compositor_init_func_b = 0;
static void prv_compositor_init_func_b(Animation *animation) {
  ++s_count_compositor_init_func_b;
}

static void prv_compositor_update_func_b(GContext *ctx, Animation *animation,
                                         uint32_t distance_normalized) {
}

static const CompositorTransition s_transition_a = {
  .init = prv_compositor_init_func_a,
  .update = prv_compositor_update_func_a
};

static const CompositorTransition s_transition_b = {
  .init = prv_compositor_init_func_b,
  .update = prv_compositor_update_func_b
};

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
}


// Tests
///////////////////////////////////////////////////////////

void test_compositor__initialize(void) {
  s_animation_implementation = NULL;

  s_last_event = (PebbleEvent) { .type = 0 };

  s_modal_window_present = false;

  s_count_animation_create = 0;
  s_count_animation_schedule = 0;
  s_count_animation_destroy = 0;
  s_scheduled_animation = NULL;

  s_count_display_update = 0;
  s_count_compositor_init_func_a = 0;
  s_count_compositor_init_func_b = 0;

  s_app_window_render_count = 0;
  s_modal_manager_render_count = 0;

  s_display_update_in_progress = false;

  s_render_pending = false;

  compositor_init();
}

void test_compositor__cleanup(void) {
}

void test_compositor__simple(void) {
  compositor_transition(&s_transition_a);

  // The animation should be created but not scheduled, as we're waiting for the app to render
  cl_assert_equal_i(s_count_animation_create, 1);
  cl_assert_equal_i(s_count_compositor_init_func_a, 1);
  cl_assert_equal_i(s_count_animation_schedule, 0);

  // Make the app render, now the animation should be scheduled
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_animation_schedule, 1);

  compositor_transition(&s_transition_b);

  // We should create a second animation, calling the b's transition init func. The animation
  // should not be scheduled as we're waiting for the interrupted app to render
  cl_assert_equal_i(s_count_animation_create, 2);
  cl_assert_equal_i(s_count_animation_schedule, 1);
  cl_assert_equal_i(s_count_compositor_init_func_a, 1);
  cl_assert_equal_i(s_count_compositor_init_func_b, 1);

  // Make the app render, now the animation should be scheduled
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_animation_schedule, 2);

  // Push a modal window mid transition, the resulting animation should be scheduled immediately.
  s_modal_window_present = true;
  compositor_transition(&s_transition_b);
  cl_assert_equal_i(s_count_animation_create, 3);
  cl_assert_equal_i(s_count_animation_schedule, 3);
  cl_assert_equal_i(s_count_compositor_init_func_a, 1);
  cl_assert_equal_i(s_count_compositor_init_func_b, 2);
}

void test_compositor__app_render_busy(void) {
  // Set the display as busy and then render the app. Nothing should update.
  s_display_update_in_progress = true;
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_display_update, 0);

  // Now fake the display update completing. The app should know draw.
  s_display_update_in_progress = false;
  prv_handle_display_update_complete();
  cl_assert_equal_i(s_count_display_update, 1);

  // Subsequent app updates should now draw straight through.
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_display_update, 2);

  // test animation updates
  s_display_update_in_progress = true;
  // start a transition
  compositor_transition(&s_transition_a);
  cl_assert_equal_i(s_count_display_update, 2);

  // app render will be handled before transition
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_display_update, 2);
  cl_assert_equal_i(s_count_animation_schedule, 0);

  // transition is started from the deferred transition event
  s_display_update_in_progress = false;
  prv_handle_display_update_complete();
  cl_assert_equal_i(s_count_display_update, 3);

  // subsequent app render starts animation
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_animation_schedule, 1);
  cl_assert_equal_i(s_count_display_update, 3);
}

void test_compositor__modal_transition_cancels_deferred_app(void) {
  // Set the display as busy and then render the app. Nothing should update.
  s_display_update_in_progress = true;
  s_render_pending = true;
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_display_update, 0);
  cl_assert_equal_i(s_render_pending, true);

  // Now transition to a modal. The app framebuffer should be released. No animation should be started.
  s_modal_window_present = true;
  compositor_transition(&s_transition_a);
  cl_assert_equal_i(s_render_pending, false);
  cl_assert_equal_i(s_count_animation_create, 0);

  // Start the animation
  s_display_update_in_progress = false;
  prv_handle_display_update_complete();
  cl_assert_equal_i(s_count_animation_create, 1);
  cl_assert_equal_i(s_count_animation_schedule, 1);
}

void test_compositor__app_no_animation(void) {
  // Start a transition. We shouldn't update the screen because the app hasn't rendered
  // yet.
  compositor_transition(NULL);
  cl_assert_equal_i(s_count_display_update, 0);
  cl_assert_equal_i(s_count_animation_create, 0);

  // Now the app has rendered something and we should actually update the display.
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_display_update, 1);
}

void test_compositor__app_not_ready_modal_push_pop(void) {
  // If a modal window is popped revealing an app that has not yet rendered itself for the first
  // time we shouldn't render the app immediately. We need to still wait for the app to render
  // itself for the first time. This is a known issue that doesn't seem to happen too frequently
  // in practice, but we should fix it up at some point.

  // Start a null window transition to an app. It should wait for the app to report ready
  compositor_transition(NULL);
  cl_assert_equal_i(s_count_display_update, 0);
  cl_assert_equal_i(s_count_animation_create, 0);

  // Push a modal window with an animation and then pop it without an animation. The app still
  // shouldn't be ready.
  s_modal_window_present = true;
  compositor_transition(&s_transition_a);
  cl_assert_equal_i(s_count_animation_create, 1);

  s_modal_window_present = false;
  compositor_transition(NULL);

  // previous animation is unscheduled
  cl_assert_equal_i(s_count_animation_destroy, 1);

  // Throughout pushing this modal the app never reported it was ready, so we still shouldn't have
  // rendered from the app frame buffer.
  cl_assert_equal_i(s_app_window_render_count, 0);

  // Now the app has rendered something and we should actually update the display.
  compositor_app_render_ready();
  cl_assert_equal_i(s_app_window_render_count, 2);
}

void test_compositor__app_not_ready_cancelled_animation_deferred(void) {
  // Show a modal window
  s_modal_window_present = true;
  compositor_transition(NULL);

  // Now pop it with a transition
  s_modal_window_present = false;
  compositor_transition(&s_transition_a);
  cl_assert_equal_i(s_count_animation_create, 1);

  // Pretend we're in the middle of copying a frame to the display hardware
  s_display_update_in_progress = true;

  // Start a null window transition to an app while the modal is popping. It should wait for the
  // app to report ready. This shouldn't cancel any animation, as the animation isn't started yet,
  // as it should be waiting for the app to render for the first time before starting.
  compositor_transition(NULL);
  cl_assert_equal_i(s_count_animation_destroy, 1);
  cl_assert_equal_i(s_app_window_render_count, 0);

  // Now complete the animation teardown by finishing the copy to the display. We shouldn't render
  // the app because it hasn't rendered anything yet.
  s_display_update_in_progress = false;
  prv_handle_display_update_complete();
  cl_assert_equal_i(s_app_window_render_count, 0);
  cl_assert_equal_i(s_count_animation_destroy, 1);

  // Now the app has rendered something and we should actually update the display.
  compositor_app_render_ready();
  cl_assert_equal_i(s_app_window_render_count, 2);
  cl_assert_equal_i(s_count_animation_destroy, 1);
}

void test_compositor__cancel_modal_to_app_with_another_modal(void) {
  // Show a modal window
  s_modal_window_present = true;
  compositor_transition(NULL);
  cl_assert_equal_i(s_count_display_update, 1);

  // Now pop it with a transition
  s_modal_window_present = false;
  compositor_transition(&s_transition_a);
  cl_assert_equal_i(s_count_animation_create, 1);
  cl_assert_equal_i(s_count_display_update, 1);
  cl_assert_equal_i(s_count_animation_schedule, 0);

  // Have the app render once so the animation can start
  s_render_pending = true;
  compositor_app_render_ready();
  cl_assert_equal_i(s_count_animation_schedule, 1);
  // Don't allow the app to render while we're animating to it
  cl_assert_equal_i(s_render_pending, true);

  // Now before the previous animation completes, transition to a different modal
  s_modal_window_present = true;
  compositor_transition(&s_transition_b);
  // Create and schedule a new modal animation, destroying the old one
  cl_assert_equal_i(s_count_animation_create, 2);
  cl_assert_equal_i(s_count_animation_schedule, 2);
  cl_assert_equal_i(s_count_animation_destroy, 1);
  // The app framebuffer should still be locked while we're animating away
  cl_assert_equal_i(s_render_pending, true);

  // Finish the animation
  s_animation_implementation->teardown((Animation*) (uintptr_t) s_count_animation_create);
  // App should be free to render again
  cl_assert_equal_i(s_render_pending, false);
}
