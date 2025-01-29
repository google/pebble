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

#include "applib/ui/kino/kino_player.h"
#include "applib/ui/kino/kino_reel.h"
#include "applib/ui/kino/kino_reel_gbitmap.h"
#include "applib/ui/kino/kino_reel_gbitmap_sequence.h"
#include "applib/ui/kino/kino_reel_pdci.h"
#include "applib/ui/kino/kino_reel_pdcs.h"
#include "applib/ui/kino/kino_reel_custom.h"

#include "clar.h"

// Fakes
////////////////////////////////////
#include "fake_resource_syscalls.h"

// Stubs
////////////////////////////////////
#include "stubs_app_state.h"
#include "stubs_applib_resource.h"
#include "stubs_compiled_with_legacy2_sdk.h"
#include "stubs_gpath.h"
#include "stubs_graphics.h"
#include "stubs_graphics_context.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_memory_layout.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_resources.h"
#include "stubs_ui_window.h"
#include "stubs_unobstructed_area.h"

void graphics_context_move_draw_box(GContext* ctx, GPoint offset) {}
typedef uint16_t ResourceId;
const uint8_t *resource_get_builtin_bytes(ResAppNum app_num, uint32_t resource_id,
                                          uint32_t *num_bytes_out) { return NULL; }

typedef struct TestReelData {
  uint32_t elapsed_ms;
  uint32_t duration_ms;
} TestReelData;

static int s_num_destructor_calls;

static void prv_destructor(KinoReel *reel) {
  s_num_destructor_calls++;
  free(kino_reel_custom_get_data(reel));
}

static uint32_t prv_elapsed_getter(KinoReel *reel) {
  return ((TestReelData*)kino_reel_custom_get_data(reel))->elapsed_ms;
}

static bool prv_elapsed_setter(KinoReel *reel, uint32_t elapsed_ms) {
  ((TestReelData*)kino_reel_custom_get_data(reel))->elapsed_ms = elapsed_ms;
  return true;
}

static uint32_t prv_duration_getter(KinoReel *reel) {
  return ((TestReelData*)kino_reel_custom_get_data(reel))->duration_ms;
}

static struct TestReelData *test_reel_data;
static KinoReelImpl *test_reel_impl = NULL;
static KinoReel *test_reel = NULL;
static KinoPlayer *test_player = NULL;

// Setup
void test_kino_player__initialize(void) {
  test_reel_data = malloc(sizeof(TestReelData));
  memset(test_reel_data, 0, sizeof(TestReelData));

  s_num_destructor_calls = 0;

  test_reel_impl = malloc(sizeof(KinoReelImpl));
  *test_reel_impl = (KinoReelImpl) {
    .destructor = prv_destructor,
    .set_elapsed = prv_elapsed_setter,
    .get_elapsed = prv_elapsed_getter,
    .get_duration = prv_duration_getter
  };

  test_reel = kino_reel_custom_create(test_reel_impl, test_reel_data);
  cl_assert(test_reel != NULL);

  test_player = malloc(sizeof(KinoPlayer));
  memset(test_player, 0, sizeof(KinoPlayer));

  kino_player_set_reel(test_player, test_reel, true);
}

// Teardown
void test_kino_player__cleanup(void) {
  kino_reel_destroy(test_reel);
}

// Tests
////////////////////////////////////

extern void prv_play_animation_update(Animation *animation, const AnimationProgress normalized);

void test_kino_player__finite_animation_finite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  kino_player_play(test_player);

  animation_set_elapsed(test_player->animation, 1234);  // intentionally bad value
  prv_play_animation_update(test_player->animation, ANIMATION_NORMALIZED_MAX * 20 / 300);

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__create_finite_animation_finite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_elapsed(animation, 1234);  // intentionally bad value
  prv_play_animation_update(animation, ANIMATION_NORMALIZED_MAX * 20 / 300);

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__finite_animation_finite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  kino_player_play(test_player);

  animation_set_reverse(test_player->animation, true);
  animation_set_elapsed(test_player->animation, 1234);  // intentionally bad value
  prv_play_animation_update(test_player->animation,
                            ANIMATION_NORMALIZED_MAX - ANIMATION_NORMALIZED_MAX * 20 / 300);

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 300 - 20);
}

void test_kino_player__create_finite_animation_finite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_reverse(animation, true);
  animation_set_elapsed(animation, 1234);  // intentionally bad value
  prv_play_animation_update(animation,
                            ANIMATION_NORMALIZED_MAX - ANIMATION_NORMALIZED_MAX * 20 / 300);

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 300 - 20);
}

void test_kino_player__finite_animation_infinite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  kino_player_play(test_player);

  animation_set_elapsed(test_player->animation, 20);
  animation_set_duration(test_player->animation, 300);
  prv_play_animation_update(test_player->animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__create_finite_animation_infinite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_elapsed(animation, 20);
  animation_set_duration(animation, 300);
  prv_play_animation_update(animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__infinite_animation_finite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  kino_player_play(test_player);

  animation_set_elapsed(test_player->animation, 20);
  animation_set_duration(test_player->animation, ANIMATION_DURATION_INFINITE);
  prv_play_animation_update(test_player->animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__create_infinite_animation_finite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_elapsed(animation, 20);
  animation_set_duration(animation, ANIMATION_DURATION_INFINITE);
  prv_play_animation_update(animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__infinite_animation_infinite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  kino_player_play(test_player);

  animation_set_elapsed(test_player->animation, 20);
  animation_set_duration(test_player->animation, ANIMATION_DURATION_INFINITE);
  prv_play_animation_update(test_player->animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__create_infinite_animation_infinite_reel_forward(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_elapsed(animation, 20);
  animation_set_duration(animation, ANIMATION_DURATION_INFINITE);
  prv_play_animation_update(animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 20);
}

void test_kino_player__infinite_animation_finite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  kino_player_play(test_player);

  animation_set_reverse(test_player->animation, true);
  animation_set_duration(test_player->animation, ANIMATION_DURATION_INFINITE);
  animation_set_elapsed(test_player->animation, 20);
  prv_play_animation_update(test_player->animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 300 - 20);
}

void test_kino_player__create_infinite_animation_finite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = 300;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_reverse(animation, true);
  animation_set_duration(animation, ANIMATION_DURATION_INFINITE);
  animation_set_elapsed(animation, 20);
  prv_play_animation_update(animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), 300 - 20);
}

void test_kino_player__finite_animation_infinite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  kino_player_play(test_player);

  animation_set_reverse(test_player->animation, true);
  animation_set_duration(test_player->animation, 300);
  animation_set_elapsed(test_player->animation, 20);
  prv_play_animation_update(test_player->animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), ANIMATION_DURATION_INFINITE);
}

void test_kino_player__create_finite_animation_infinite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_reverse(animation, true);
  animation_set_duration(animation, 300);
  animation_set_elapsed(animation, 20);
  prv_play_animation_update(animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), ANIMATION_DURATION_INFINITE);
}

void test_kino_player__infinite_animation_infinite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  kino_player_play(test_player);

  animation_set_reverse(test_player->animation, true);
  animation_set_duration(test_player->animation, ANIMATION_DURATION_INFINITE);
  animation_set_elapsed(test_player->animation, 20);
  prv_play_animation_update(test_player->animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), ANIMATION_DURATION_INFINITE);
}

void test_kino_player__create_infinite_animation_infinite_reel_reverse(void) {
  // Choose duration and elapsed to have clean division for
  // ANIMATION_NORMALIZED_MAX * elapsed / duration = whole_number
  test_reel_data->duration_ms = ANIMATION_DURATION_INFINITE;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation_schedule(animation);
  animation_set_reverse(animation, true);
  animation_set_duration(animation, ANIMATION_DURATION_INFINITE);
  animation_set_elapsed(animation, 20);
  prv_play_animation_update(animation, 0);  // intentionally bad value

  cl_assert_equal_i(kino_reel_get_elapsed(test_reel), ANIMATION_DURATION_INFINITE);
}

void test_kino_player__create_animation_is_immutable(void) {
  test_reel_data->duration_ms = 300;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);
  cl_assert_equal_i(animation_is_immutable(animation), true);
}

void test_kino_player__create_animation_is_unscheduled_by_play_pause_rewind(void) {
  test_reel_data->duration_ms = 300;
  Animation *animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);
  animation_schedule(animation);

  // Play plays a new animation
  cl_assert_equal_i(animation_is_scheduled(animation), true);
  kino_player_play(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  // Create play animation unschedules previous animation
  animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);
  animation_schedule(animation);

  // Pause unschedules the current animation
  cl_assert_equal_i(animation_is_scheduled(animation), true);
  kino_player_pause(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);

  animation = (Animation *)kino_player_create_play_animation(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);
  animation_schedule(animation);

  // Rewind unschedules the current animation
  cl_assert_equal_i(animation_is_scheduled(animation), true);
  kino_player_rewind(test_player);
  cl_assert_equal_i(animation_is_scheduled(animation), false);
}

void test_kino_player__set_reel_calls_destructor(void) {
  kino_player_set_reel(test_player, test_reel, true);
  cl_assert_equal_p(test_player->reel, test_reel);
  cl_assert_equal_i(s_num_destructor_calls, 0);

  kino_player_set_reel(test_player, NULL, true);
  cl_assert_equal_p(test_player->reel, NULL);
  cl_assert_equal_i(s_num_destructor_calls, 1);

  kino_player_set_reel(test_player, NULL, true);
  cl_assert_equal_p(test_player->reel, NULL);
  cl_assert_equal_i(s_num_destructor_calls, 1);

  test_reel = NULL;
}

void test_kino_player__set_reel_does_not_call_destructor(void) {
  test_player->owns_reel = false;

  kino_player_set_reel(test_player, test_reel, false);
  cl_assert_equal_p(test_player->reel, test_reel);
  cl_assert_equal_i(s_num_destructor_calls, 0);

  kino_player_set_reel(test_player, NULL, true);
  cl_assert_equal_p(test_player->reel, NULL);
  cl_assert_equal_i(s_num_destructor_calls, 0);
}
