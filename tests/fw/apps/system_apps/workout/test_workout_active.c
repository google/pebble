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

#include "apps/system_apps/workout/workout.h"
#include "apps/system_apps/workout/workout_active.h"
#include "apps/system_apps/workout/workout_data.h"
#include "apps/system_apps/workout/workout_dialog.h"

#include "services/normal/activity/health_util.h"

#include "test_workout_app_includes.h"

#include "stubs_window_manager.h"

bool s_hrm_is_present;

// Fakes
/////////////////////
extern void prv_cycle_scrollable_metrics(WorkoutActiveWindow *active_window);

bool activity_is_hrm_present(void) {
  return s_hrm_is_present;
}

uint16_t time_ms(time_t *tloc, uint16_t *out_ms) {
  return 0;
}

void workout_push_summary_window(void) {
  return;
}

static WorkoutData s_workout_data;

static WorkoutController s_workout_controller = {
  .is_paused = workout_service_is_paused,
  .pause = workout_service_pause_workout,
  .stop = workout_service_stop_workout,
  .update_data = workout_data_update,
  .metric_to_string = workout_data_fill_metric_value,
  .get_metric_value = workout_data_get_metric_value,
  .get_distance_string = health_util_get_distance_string,
};

typedef struct SportsData {
  int32_t current_bpm;
  char *duration_string;
  char *distance_string;
  char *pace_string;
  char *custom_label_string;
  char *custom_value_string;
} SportsData;

static SportsData s_sports_data;

static bool prv_is_sports_paused(void) { return false; }
static bool prv_sports_pause(bool should_be_paused) { return false; }
static void prv_metric_to_string(WorkoutMetricType type, char *buffer, size_t buffer_size,
                                void *i18n_owner, void *sports_data) {
  SportsData *data = sports_data;

  switch (type) {
    case WorkoutMetricType_Hr:
    {
      snprintf(buffer, buffer_size, "%d", data->current_bpm);
      break;
    }
    case WorkoutMetricType_Speed:
    case WorkoutMetricType_Pace:
    {
      strncpy(buffer, data->pace_string, buffer_size);
      break;
    }
    case WorkoutMetricType_Distance:
    {
      strncpy(buffer, data->distance_string, buffer_size);
      break;
    }
    case WorkoutMetricType_Duration:
    {
      strncpy(buffer, data->duration_string, buffer_size);
      break;
    }
    case WorkoutMetricType_Custom:
    {
      strncpy(buffer, data->custom_value_string, buffer_size);
      break;
    }
    // Not supported by the sports API
    case WorkoutMetricType_Steps:
    case WorkoutMetricType_AvgPace:
    case WorkoutMetricType_None:
    case WorkoutMetricTypeCount:
      break;
  }
}

static int32_t prv_sports_get_value(WorkoutMetricType type, void *sports_data) {
  SportsData *data = sports_data;
  switch (type) {
    case WorkoutMetricType_Hr:
      return data->current_bpm;
    default:
      return 0;
  }
}

static char *prv_get_custom_metric_label_string(void) {
  return s_sports_data.custom_label_string;
}

static WorkoutController s_sports_controller = {
  .is_paused = prv_is_sports_paused,
  .pause = prv_sports_pause,
  .stop = NULL,
  .update_data = NULL,
  .metric_to_string = prv_metric_to_string,
  .get_metric_value = prv_sports_get_value,
  .get_distance_string = health_util_get_distance_string,
  .get_custom_metric_label_string = prv_get_custom_metric_label_string,
};

// Setup and Teardown
////////////////////////////////////

static GContext s_ctx;
static FrameBuffer s_fb;

GContext *graphics_context_get_current_context(void) {
  return &s_ctx;
}

void test_workout_active__initialize(void) {
  s_hrm_is_present = true;

  s_workout_data = (WorkoutData) {};
  s_sports_data = (SportsData) {};

  // Setup graphics context
  framebuffer_init(&s_fb, &(GSize) {DISP_COLS, DISP_ROWS});
  framebuffer_clear(&s_fb);
  graphics_context_init(&s_ctx, &s_fb, GContextInitializationMode_App);
  s_app_state_get_graphics_context = &s_ctx;

  // Setup resources
  fake_spi_flash_init(0 /* offset */, 0x1000000 /* length */);
  pfs_init(false /* run filesystem check */);
  pfs_format(true /* write erase headers */);
  load_resource_fixture_in_flash(RESOURCES_FIXTURE_PATH, SYSTEM_RESOURCES_FIXTURE_NAME,
                                 false /* is_next */);
  resource_init();

  // Setup content indicator
  ContentIndicatorsBuffer *buffer = content_indicator_get_current_buffer();
  content_indicator_init_buffer(buffer);
}

void test_workout_active__cleanup(void) {
}

// Helpers
//////////////////////

static void prv_create_window_and_render(WorkoutActiveWindow *active_window,
                                         int secondary_metric_idx) {
  for (int i = 0; i < secondary_metric_idx; i++) {
    prv_cycle_scrollable_metrics(active_window);
  }

  Window *window = (Window *)active_window;
  window_set_on_screen(window, true, true);
  window_render(window, &s_ctx);
}

// Workout Tests
//////////////////////

void test_workout_active__workout_render_no_data(void) {
  s_workout_data = (WorkoutData) {};
  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Run, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_walk(void) {
  s_workout_data = (WorkoutData) {
    .steps = 567,
    .duration_s = 84,
    .distance_m = 1234,
    .avg_pace = health_util_get_pace(84, 1234),
    .bpm = 71,
    .hr_zone = 0,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Walk, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_walk_no_hrm(void) {
  s_hrm_is_present = false;

  s_workout_data = (WorkoutData) {
    .steps = 567,
    .duration_s = 84,
    .distance_m = 1234,
    .avg_pace = health_util_get_pace(84, 1234),
    .bpm = 71,
    .hr_zone = 0,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Walk, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_run(void) {
  s_workout_data = (WorkoutData) {
    .steps = 567,
    .duration_s = 84,
    .distance_m = 1234,
    .avg_pace = health_util_get_pace(84, 1234),
    .bpm = 71,
    .hr_zone = 0,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Run, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_run_no_hrm(void) {
  s_hrm_is_present = false;

  s_workout_data = (WorkoutData) {
    .steps = 567,
    .duration_s = 84,
    .distance_m = 1234,
    .avg_pace = health_util_get_pace(84, 1234),
    .bpm = 71,
    .hr_zone = 0,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Run, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_open_workout(void) {
  s_workout_data = (WorkoutData) {
    .steps = 0,
    .duration_s = 84,
    .distance_m = 0,
    .avg_pace = health_util_get_pace(84, 0),
    .bpm = 92,
    .hr_zone = 0,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Open, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_open_workout_no_hrm(void) {
  s_hrm_is_present = false;

  s_workout_data = (WorkoutData) {
    .steps = 0,
    .duration_s = 84,
    .distance_m = 0,
    .avg_pace = health_util_get_pace(84, 0),
    .bpm = 92,
    .hr_zone = 0,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Open, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_hr_zone_1(void) {
  s_workout_data = (WorkoutData) {
    .steps = 567,
    .duration_s = 789,
    .distance_m = 234,
    .avg_pace = health_util_get_pace(789, 234),
    .bpm = 148,
    .hr_zone = 1,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Run, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_hr_zone_2(void) {
  s_workout_data = (WorkoutData) {
    .steps = 567,
    .duration_s = 789,
    .distance_m = 234,
    .avg_pace = health_util_get_pace(789, 234),
    .bpm = 167,
    .hr_zone = 2,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Run, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_hr_zone_3(void) {
  s_workout_data = (WorkoutData) {
    .steps = 567,
    .duration_s = 789,
    .distance_m = 234,
    .avg_pace = health_util_get_pace(789, 234),
    .bpm = 197,
    .hr_zone = 3,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Run, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__workout_render_very_slow_pace(void) {
  s_workout_data = (WorkoutData) {
    .steps = 0,
    .duration_s = SECONDS_PER_HOUR,
    .distance_m = 1609,
    .avg_pace = health_util_get_pace(SECONDS_PER_HOUR, 1609),
    .bpm = 0,
    .hr_zone = 0,
  };

  WorkoutActiveWindow *active_window = workout_active_create_for_activity_type(
      ActivitySessionType_Walk, &s_workout_data, &s_workout_controller);
  prv_create_window_and_render(active_window, 2);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}



// Workout Tests
//////////////////////

void test_workout_active__sports_pace(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 71,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Hr};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_pace_long_values(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 71,
    .duration_string = "04:20:39",
    .distance_string = "115.12",
    .pace_string = "12:34",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Hr};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_speed(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 71,
    .duration_string = "20:00",
    .distance_string = "18.9",
    .pace_string = "35.3",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Speed,
                                            WorkoutMetricType_Hr};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_no_hrm(void) {
  s_hrm_is_present = false;

  s_sports_data = (SportsData) {
    .current_bpm = 71,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 0);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_hr_z0(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 71,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Hr};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 1);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_hr_z1(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 135,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Hr};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 1);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_hr_z2(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 165,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Hr};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 1);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_hr_z3(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 180,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Hr};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 1);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_custom_field(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 71,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
    .custom_label_string = "CUSTOM",
    .custom_value_string = "000000",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Custom};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 1);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_custom_long_values(void) {
  s_sports_data = (SportsData) {
    .current_bpm = 71,
    .duration_string = "30:00",
    .distance_string = "5.0",
    .pace_string = "6:00",
    .custom_label_string = "CUSTOM FIELD LABEL",
    .custom_value_string = "0000000000000000000",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Custom};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 1);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_workout_active__sports_custom_hanging_label(void) {
  s_sports_data = (SportsData) {
      .current_bpm = 71,
      .duration_string = "30:00",
      .distance_string = "5.0",
      .pace_string = "6:00",
      .custom_label_string = "Hanging Field",
      .custom_value_string = "000000",
  };

  WorkoutMetricType top_metric = WorkoutMetricType_Duration;
  WorkoutMetricType middle_metric = WorkoutMetricType_Distance;
  WorkoutMetricType scrollable_metrics[] = {WorkoutMetricType_Pace,
                                            WorkoutMetricType_Custom};

  WorkoutActiveWindow *active_window = workout_active_create_tripple_layout(
      top_metric, middle_metric, ARRAY_LENGTH(scrollable_metrics), scrollable_metrics,
      &s_sports_data, &s_sports_controller);
  prv_create_window_and_render(active_window, 1);
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}
