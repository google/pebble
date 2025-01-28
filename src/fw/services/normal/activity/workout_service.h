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

#pragma once

#include "activity.h"
#include "hr_util.h"

#include "kernel/events.h"

#include <stdbool.h>

//! Workouts are very similar to ActivitySessions, the only difference is that they are manually
//! started / stopped, and update more frequently than automatically detected activities.

//! Note: If a workout is in progress, then we disable automatic activity detection.
//! Note: Only 1 workout at a time is supported

void workout_service_init(void);

//! Called by the frontend application to signal that the app has been opened.
//! @note Must be called from PebbleTask_App
void workout_service_frontend_opened(void);

//! Called by the frontend application to signal that the app has been closed.
//! @note Must be called from PebbleTask_App
void workout_service_frontend_closed(void);

//! Event handler for Health events
void workout_service_health_event_handler(PebbleHealthEvent *event);

//! Event handler for Activity events
void workout_service_activity_event_handler(PebbleActivityEvent *event);

//! Event handler for Workout events
void workout_service_workout_event_handler(PebbleWorkoutEvent *event);

//! Returns true if there is an ongoing workout
bool workout_service_is_workout_ongoing(void);

//! Returns true if the activity type is a supported workout
bool workout_service_is_workout_type_supported(ActivitySessionType type);

//! Start a new workout
//! This stops / saves all onoing automatically detected activity sessions
//! All workouts must eventually get stopped
bool workout_service_start_workout(ActivitySessionType type);

//! Pause / unpause the correct workout
bool workout_service_pause_workout(bool should_be_paused);

//! Stops the current workout. Resumes automatic activity session detection
bool workout_service_stop_workout(void);

//! Starts a workout using the data from the given activity session
bool workout_service_takeover_activity_session(ActivitySession *session);

//! Returns true if there is a paused workout
bool workout_service_is_paused(void);

//! Get the current workout type
//! Returns true if a workout is going on
bool workout_service_get_current_workout_type(ActivitySessionType *type_out);

//! Dumps the current state of the workout
bool workout_service_get_current_workout_info(int32_t *steps_out, int32_t *duration_s_out,
                                              int32_t *distance_m_out, int32_t *current_bpm_out,
                                              HRZone *current_hr_zone_out);
