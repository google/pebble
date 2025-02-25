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

#include "health_data.h"

typedef struct HealthData {
  //!< Current step / activity info
  int32_t step_data[DAYS_PER_WEEK]; //!< Step history for today and the previous 6 days
  int32_t current_distance_meters;
  int32_t current_calories;

  //!< Typical step info
  ActivityMetricAverages step_averages; //!< The step averages for the current day
  int32_t current_step_average; //!< The current step average so far
  int32_t step_average_last_updated_time; //!< The time at which current_step_average was updated

  int32_t monthly_step_average;

  int32_t sleep_data[DAYS_PER_WEEK]; //!< Sleep history for the past week
  int32_t typical_sleep; //! Typical sleep for the current week day
  int32_t deep_sleep; //!< Amount of deep sleep last night

  int32_t sleep_start; //!< When the user went to sleep (seconds after midnight)
  int32_t sleep_end; //!< When the user woke up (seconds after midnight)
  int32_t typical_sleep_start; //!< When the user typically goes to sleep
  int32_t typical_sleep_end; //!< When the user typically wakes up

  int32_t monthly_sleep_average;

  uint32_t num_activity_sessions; //!< Number of activity sessions returned by the API
  ActivitySession activity_sessions[ACTIVITY_MAX_ACTIVITY_SESSIONS_COUNT]; //!< Activity sessions

  int32_t current_hr_bpm; //!< Current BPM
  int32_t resting_hr_bpm; //!< Resting BPM
  time_t hr_last_updated; //!< Time at which HR data was last updated

  int32_t hr_zone1_minutes;
  int32_t hr_zone2_minutes;
  int32_t hr_zone3_minutes;
} HealthData;
