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

//! 5 main segments + 4 real corners
//! The top bar is split up into 2 segments (12am is the middle of the top bar)
//! Each of line gets 25% of the total (top line split into 2 segments which are 12.5% each)
#define AMOUNT_PER_SEGMENT (HEALTH_PROGRESS_BAR_MAX_VALUE * 25 / 100)

// Found through trial and error
#define DEFAULT_MARK_WIDTH 40

#define X_SHIFT (PBL_IF_ROUND_ELSE(23, PBL_IF_BW_ELSE(1, 0)))
#define Y_SHIFT (PBL_IF_ROUND_ELSE(8, PBL_IF_BW_ELSE(3, 0)))

// Used to shrink the thickness of the bars
#define X_SHRINK (PBL_IF_BW_ELSE(2, 0))

// These are used to shrink the shape for round
#define X_ADJ (PBL_IF_ROUND_ELSE(-12, PBL_IF_BW_ELSE(-3, 0)))
#define Y_ADJ (PBL_IF_ROUND_ELSE(-3, PBL_IF_BW_ELSE(1, 0)))


static HealthProgressSegment s_sleep_summary_progress_segments[] = {
  {
    // Top right
    .type = HealthProgressSegmentType_Horizontal,
    .amount_of_total = AMOUNT_PER_SEGMENT / 2,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{71 + X_SHIFT, 22 + Y_SHIFT},
               {116 + X_SHRINK + X_SHIFT + X_ADJ, 22 + Y_SHIFT},
               {116 + X_SHRINK + X_SHIFT + X_ADJ, 13 + Y_SHIFT},
               {71 + X_SHIFT, 13 + Y_SHIFT}},
  },
  {
    // Top right corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{115 + X_SHRINK + X_SHIFT + X_ADJ, 22 + Y_SHIFT},
               {115 + X_SHRINK + X_SHIFT + X_ADJ, 13 + Y_SHIFT},
               {127 + X_SHIFT + X_ADJ, 13 + Y_SHIFT},
               {127 + X_SHIFT + X_ADJ, 22 + Y_SHIFT}},
  },
  {
    // Right
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH + 10,
    .points = {{116 + X_SHRINK + X_SHIFT + X_ADJ, 23 + Y_SHIFT},
               {127 + X_SHIFT + X_ADJ, 23 + Y_SHIFT},
               {127 + X_SHIFT + X_ADJ, 73 + Y_SHIFT + Y_ADJ},
               {116 + X_SHRINK + X_SHIFT + X_ADJ, 73 + Y_SHIFT + Y_ADJ}},
  },
  {
    // Bottom right corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{115 + X_SHRINK + X_SHIFT + X_ADJ, 74 + Y_SHIFT + Y_ADJ},
               {127 + X_SHIFT + X_ADJ, 74 + Y_SHIFT + Y_ADJ},
               {127 + X_SHIFT + X_ADJ, 83 + Y_SHIFT + Y_ADJ},
               {115 + X_SHRINK + X_SHIFT + X_ADJ, 83 + Y_SHIFT + Y_ADJ}},
  },
  {
    // Bottom
    .type = HealthProgressSegmentType_Horizontal,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{116 + X_SHRINK + X_SHIFT + X_ADJ, 74 + Y_SHIFT + Y_ADJ},
               {27 + X_SHRINK + X_SHIFT + X_ADJ, 74 + Y_SHIFT + Y_ADJ},
               {27 + X_SHRINK + X_SHIFT + X_ADJ, 83 + Y_SHIFT + Y_ADJ},
               {116 + X_SHRINK + X_SHIFT + X_ADJ, 83 + Y_SHIFT + Y_ADJ}},
  },
  {
    // Bottom left corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{29 + -X_SHRINK + X_SHIFT, 74 + Y_SHIFT + Y_ADJ},
               {17 + X_SHIFT, 74 + Y_SHIFT + Y_ADJ},
               {17 + X_SHIFT, 83 + Y_SHIFT + Y_ADJ},
               {29 + -X_SHRINK + X_SHIFT, 83 + Y_SHIFT + Y_ADJ}},
  },
  {
    // Left
    .type = HealthProgressSegmentType_Vertical,
    .amount_of_total = AMOUNT_PER_SEGMENT,
    .mark_width = DEFAULT_MARK_WIDTH,
    .points = {{28 + -X_SHRINK + X_SHIFT, 74 + Y_SHIFT + Y_ADJ},
               {17 + X_SHIFT, 74 + Y_SHIFT + Y_ADJ},
               {17 + X_SHIFT, 23 + Y_SHIFT},
               {28 + -X_SHRINK + X_SHIFT, 23 + Y_SHIFT}},
  },
  {
    // Top left corner
    .type = HealthProgressSegmentType_Corner,
    .points = {{29 + X_SHIFT, 22 + Y_SHIFT},
               {17 + X_SHIFT, 22 + Y_SHIFT},
               {17 + X_SHIFT, 13 + Y_SHIFT},
               {29 + X_SHIFT, 13 + Y_SHIFT}},
  },
  {
    // Top left
    .type = HealthProgressSegmentType_Horizontal,
    .amount_of_total = AMOUNT_PER_SEGMENT / 2,
    .mark_width = DEFAULT_MARK_WIDTH + 10,
    .points = {{28 + -X_SHRINK + X_SHIFT, 22 + Y_SHIFT},
               {72 + X_SHIFT, 22 + Y_SHIFT},
               {72 + X_SHIFT, 13 + Y_SHIFT},
               {28 + -X_SHRINK + X_SHIFT, 13 + Y_SHIFT}},
  },
};

#define MASKING_RECT_X_SHIFT (X_SHIFT + PBL_IF_BW_ELSE(1, 0))
#define MASKING_RECT_Y_SHIFT (Y_SHIFT + PBL_IF_BW_ELSE(1, 0))
#define MASKING_RECT_X_ADJ (X_ADJ + PBL_IF_BW_ELSE(-1, 0))
#define MASKING_RECT_Y_ADJ (Y_ADJ + PBL_IF_BW_ELSE(-1, 0))

static const GRect s_sleep_summary_masking_rect = {
  .origin.x = 16 + MASKING_RECT_X_SHIFT,
  .origin.y = 11 + MASKING_RECT_Y_SHIFT,
  .size.w = 113 + MASKING_RECT_X_ADJ,
  .size.h = 75 + MASKING_RECT_Y_ADJ,
};
