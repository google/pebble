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

//! 5 main segments + 2 real corners + 2 endcaps implemented as corners (for bw)
//! Each of the 5 non-corner segments get 20% of the total
#define AMOUNT_PER_SEGMENT (HEALTH_PROGRESS_BAR_MAX_VALUE * 20 / 100)

// Found through trial and error
#define DEFAULT_MARK_WIDTH 50

#if PBL_BW
// The shape of the hexagon is slightly different on BW than on Color
static HealthProgressSegment s_activity_summary_progress_segments[] = {
  {
    // This is an endcap for BW (is a no-op on color)
    .type = HealthProgressSegmentType_Corner,
    .points = {{42, 85}, {51, 85}, {42, 85}, {51, 85}},
  },
  {
    // Left side bottom
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{42, 84}, {51, 84}, {38, 58}, {28, 58}},
  },
  {
    // Left side top
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{38, 57}, {28, 57}, {46, 26}, {56, 26}},
  },
  {
    // Top left corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{56, 26}, {46, 26}, {50, 18}, {56, 18}},
  },
  {
    // Center top
    .type = HealthProgressSegmentType_Horizontal,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH * 2,
    .points = {{55, 26}, {88, 26}, {89, 18}, {54, 18}},
  },
  {
    // Top right corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{88, 26}, {88, 18}, {92, 18}, {96, 26}},
  },
  {
    // Right side top
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{87, 26}, {96, 26}, {113, 57}, {104, 57}},
  },
  {
    // Right side bottom
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{104, 58}, {113, 58}, {99, 84}, {90, 84}},
  },
  {
    // This is an endcap for BW (is a no-op on color)
    .type = HealthProgressSegmentType_Corner,
    .points = {{99, 85}, {90, 85}, {99, 85}, {90, 85}},
  },
};
#else // Color
// The shape is the same, but the offsets are different
// Slightly adjust the points on Round
#define X_ADJ (PBL_IF_ROUND_ELSE(18, 0))
#define Y_ADJ (PBL_IF_ROUND_ELSE(6, 0))

static HealthProgressSegment s_activity_summary_progress_segments[] = {
  {
    // This is an endcap for BW (is a no-op on color)
    .type = HealthProgressSegmentType_Corner,
    .points = {{46 + X_ADJ, 81 + Y_ADJ}, {58 + X_ADJ, 81 + Y_ADJ},
               {46 + X_ADJ, 81 + Y_ADJ}, {58 + X_ADJ, 81 + Y_ADJ}},
  },
  {
    // Left side bottom
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{46 + X_ADJ, 81 + Y_ADJ}, {58 + X_ADJ, 81 + Y_ADJ},
               {41 + X_ADJ, 51 + Y_ADJ}, {29 + X_ADJ, 51 + Y_ADJ}},
  },
  {
    // Left side top
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{29 + X_ADJ, 51 + Y_ADJ}, {41 + X_ADJ, 51 + Y_ADJ},
               {57 + X_ADJ, 24 + Y_ADJ}, {45 + X_ADJ, 24 + Y_ADJ}},
  },
  {
    // Top left corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{57 + X_ADJ, 24 + Y_ADJ}, {45 + X_ADJ, 24 + Y_ADJ},
               {51 + X_ADJ, 15 + Y_ADJ}, {57 + X_ADJ, 15 + Y_ADJ}},
  },
  {
    // Center top
    .type = HealthProgressSegmentType_Horizontal,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH * 2,
    .points = {{55 + X_ADJ, 24 + Y_ADJ}, {89 + X_ADJ, 24 + Y_ADJ},
               {89 + X_ADJ, 15 + Y_ADJ}, {55 + X_ADJ, 15 + Y_ADJ}},
  },
  {
    // Top right corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{87 + X_ADJ, 24 + Y_ADJ}, {87 + X_ADJ, 15 + Y_ADJ},
               {93 + X_ADJ, 15 + Y_ADJ}, {99 + X_ADJ, 24 + Y_ADJ}},
  },
  {
    // Right side top
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{87 + X_ADJ, 24 + Y_ADJ}, {99 + X_ADJ, 24 + Y_ADJ},
               {115 + X_ADJ, 51 + Y_ADJ}, {103 + X_ADJ, 51 + Y_ADJ}},
  },
  {
    // Right side bottom
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{103 + X_ADJ, 51 + Y_ADJ}, {115 + X_ADJ, 51 + Y_ADJ},
               {98 + X_ADJ, 81 + Y_ADJ}, {86 + X_ADJ, 81 + Y_ADJ}},
  },
  {
    // This is an endcap for BW (is a no-op on color)
    .type = HealthProgressSegmentType_Corner,
    .points = {{98 + X_ADJ, 81 + Y_ADJ}, {86 + X_ADJ, 81 + Y_ADJ},
               {98 + X_ADJ, 81 + Y_ADJ}, {86 + X_ADJ, 81 + Y_ADJ}},
  },
};
#endif
