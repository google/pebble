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

#include "applib/ui/layer.h"
#include "applib/ui/rotate_bitmap_layer.h"
#include "applib/graphics/gtypes.h"

/**
   A pair of images, one drawn white-transparent, the other black-transparent
   used to draw a single image which has white, black, and transparent regions
 **/
typedef struct {
  Layer layer;

  RotBitmapLayer white_layer;
  RotBitmapLayer black_layer;

} RotBmpPairLayer;

//! white and black *must* have the same dimensions, and *shouldn't* have any overlapp of each other
void rotbmp_pair_layer_init(RotBmpPairLayer *pair, GBitmap *white, GBitmap *black);

void rotbmp_pair_layer_deinit(RotBmpPairLayer *pair);

void rotbmp_pair_layer_set_angle(RotBmpPairLayer *pair, int32_t angle);
void rotbmp_pair_layer_increment_angle(RotBmpPairLayer *pair, int32_t angle_change);

void rotbmp_pair_layer_set_src_ic(RotBmpPairLayer *pair, GPoint ic);

//! exchanges black with white
void rotbmp_pair_layer_invert_colors(RotBmpPairLayer *pair);
