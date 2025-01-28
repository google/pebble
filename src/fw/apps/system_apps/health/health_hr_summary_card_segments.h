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

#include "health_progress.h"

//! 4 main segments + 4 real corners
//! Each of the 4 non-corner segments get 25% of the total
#define AMOUNT_PER_SEGMENT (HEALTH_PROGRESS_BAR_MAX_VALUE * 25 / 100)

// The shape is the same, but the offsets are different
// Slightly adjust the points on Round
#define X_SHIFT (PBL_IF_ROUND_ELSE(18, 0))
#define Y_SHIFT (PBL_IF_ROUND_ELSE(6, 0))

static HealthProgressSegment s_hr_summary_progress_segments[] = {
  {
    // Bottom corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{71 + X_SHIFT, 88 + Y_SHIFT}, {64 + X_SHIFT, 94 + Y_SHIFT},
               {72 + X_SHIFT, 101 + Y_SHIFT}, {80 + X_SHIFT, 93 + Y_SHIFT}},
  },
  {
    // Left side bottom
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .type = HealthProgressSegmentType_Vertical,
    .points = {{65 + X_SHIFT, 95 + Y_SHIFT}, {72 + X_SHIFT, 89 + Y_SHIFT},
               {42 + X_SHIFT, 58 + Y_SHIFT}, {35 + X_SHIFT, 65 + Y_SHIFT}},
  },
  {
    // Left corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{43 + X_SHIFT, 58 + Y_SHIFT}, {36 + X_SHIFT, 50 + Y_SHIFT},
               {29 + X_SHIFT, 58 + Y_SHIFT}, {36 + X_SHIFT, 66 + Y_SHIFT}},
  },
  {
    // Left side top
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .type = HealthProgressSegmentType_Vertical,
    .points = {{36 + X_SHIFT, 51 + Y_SHIFT}, {44 + X_SHIFT, 58 + Y_SHIFT},
               {72 + X_SHIFT, 29 + Y_SHIFT}, {65 + X_SHIFT, 22 + Y_SHIFT}},
  },
  {
    // Top corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{71 + X_SHIFT, 30 + Y_SHIFT}, {79 + X_SHIFT, 23 + Y_SHIFT},
               {71 + X_SHIFT, 16 + Y_SHIFT}, {65 + X_SHIFT, 22 + Y_SHIFT}},
  },
  {
    // Right side top
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .type = HealthProgressSegmentType_Vertical,
    .points = {{78 + X_SHIFT, 22 + Y_SHIFT}, {71 + X_SHIFT, 28 + Y_SHIFT},
               {102 + X_SHIFT, 60 + Y_SHIFT}, {108 + X_SHIFT, 53 + Y_SHIFT}},
  },
  {
    // Right corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{100 + X_SHIFT, 56 + Y_SHIFT}, {108 + X_SHIFT, 66 + Y_SHIFT},
               {114 + X_SHIFT, 59 + Y_SHIFT}, {106 + X_SHIFT, 50 + Y_SHIFT}},
  },
  {
    // Right side bottom
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .type = HealthProgressSegmentType_Vertical,
    .points = {{102 + X_SHIFT, 57 + Y_SHIFT}, {108 + X_SHIFT, 64 + Y_SHIFT},
               {78 + X_SHIFT, 95 + Y_SHIFT}, {71 + X_SHIFT, 89 + Y_SHIFT}},
  },
};
