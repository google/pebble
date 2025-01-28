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

#include "applib/graphics/gtypes.h"

#include <inttypes.h>

#define LOCAL_HOUR_HAND_LENGTH_DEFAULT 51
#define LOCAL_HOUR_HAND_THICKNESS_DEFAULT 6
#define LOCAL_HOUR_HAND_COLOR_DEFAULT GColorWhite
#define LOCAL_HOUR_HAND_BACK_EXT_DEFAULT 0

#define LOCAL_MINUTE_HAND_LENGTH_DEFAULT 58
#define LOCAL_MINUTE_HAND_THICKNESS_DEFAULT 6
#define LOCAL_MINUTE_HAND_COLOR_DEFAULT GColorWhite
#define LOCAL_MINUTE_HAND_BACK_EXT_DEFAULT 0

#define LOCAL_BOB_RADIUS_DEFAULT 6
#define LOCAL_BOB_COLOR_DEFAULT GColorRed

#define NON_LOCAL_HOUR_HAND_LENGTH_DEFAULT 11
#define NON_LOCAL_HOUR_HAND_WIDTH_DEFAULT 3

#define NON_LOCAL_MINUTE_HAND_LENGTH_DEFAULT 21
#define NON_LOCAL_MINUTE_HAND_WIDTH_DEFAULT 3

#define NUM_NON_LOCAL_CLOCKS 3

#define GLANCE_TIME_OUT_MS 8000

typedef enum {
  CLOCK_TEXT_TYPE_NONE = 0,
  CLOCK_TEXT_TYPE_TIME,
  CLOCK_TEXT_TYPE_DATE,
} ClockTextType;

typedef enum {
  CLOCK_TEXT_LOCATION_NONE = 0,
  CLOCK_TEXT_LOCATION_BOTTOM,
  CLOCK_TEXT_LOCATION_LEFT,
} ClockTextLocation;

typedef enum {
  CLOCK_HAND_STYLE_ROUNDED = 0,
  CLOCK_HAND_STYLE_ROUNDED_WITH_HIGHLIGHT,
  CLOCK_HAND_STYLE_POINTED,
} ClockHandStyle;

typedef enum {
  CLOCK_LOCATION_CENTER,
  CLOCK_LOCATION_LEFT,
  CLOCK_LOCATION_BOTTOM,
  CLOCK_LOCATION_RIGHT,
  CLOCK_LOCATION_TOP,
} ClockLocation;

typedef struct {
  uint16_t length;
  uint16_t thickness;
  uint16_t backwards_extension;
  int32_t angle;
  GColor color;
  ClockHandStyle style;
} ClockHand;

typedef struct {
  ClockHand hour_hand;
  ClockHand minute_hand;
  uint16_t bob_radius;
  uint16_t bob_center_radius;
  GColor bob_color;
  GColor bob_center_color;
  ClockLocation location;
} ClockFace;

typedef struct {
  ClockFace face;
  char buffer[4];
  int32_t utc_offset;
  GColor text_color;
} NonLocalClockFace;

typedef struct {
  ClockTextType type;
  ClockTextLocation location;
  char buffer[10]; // FIXME magic number
  GColor color;
} ClockText;

typedef struct {
  ClockFace local_clock;
  uint32_t num_non_local_clocks;
  NonLocalClockFace non_local_clock[NUM_NON_LOCAL_CLOCKS];
  ClockText text;
  uint32_t bg_bitmap_id;
} ClockModel;

void watch_model_init(void);

void watch_model_handle_change(ClockModel *model);

void watch_model_start_intro(void);

void watch_model_cleanup(void);
