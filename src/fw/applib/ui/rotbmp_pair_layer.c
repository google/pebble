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

#include "rotbmp_pair_layer.h"
#include "system/logging.h"
#include "system/passert.h"

static void set_compositing(RotBmpPairLayer *pair) {
  rot_bitmap_set_compositing_mode(&pair->white_layer, GCompOpOr);
  rot_bitmap_set_compositing_mode(&pair->black_layer, GCompOpClear);
}

void rotbmp_pair_layer_init(RotBmpPairLayer *pair, GBitmap *white, GBitmap *black) {
  if (white->bounds.size.w != black->bounds.size.w
      && white->bounds.size.h != black->bounds.size.h) {
    PBL_LOG(LOG_LEVEL_ERROR, "rotbmp_pair inited with unmatching bitmaps");
    return;
  }

  rot_bitmap_layer_init(&pair->white_layer, white);
  rot_bitmap_layer_init(&pair->black_layer, black);

  set_compositing(pair);

  layer_init(&pair->layer, &pair->white_layer.layer.frame);

  layer_add_child(&pair->layer, &pair->white_layer.layer);
  layer_add_child(&pair->layer, &pair->black_layer.layer);
}

void rotbmp_pair_layer_deinit(RotBmpPairLayer *pair) {
  layer_deinit(&pair->white_layer.layer);
  layer_deinit(&pair->black_layer.layer);
  layer_deinit(&pair->layer);
}

void rotbmp_pair_layer_set_angle(RotBmpPairLayer *pair, int32_t angle) {
  rot_bitmap_layer_set_angle(&pair->white_layer, angle);
  rot_bitmap_layer_set_angle(&pair->black_layer, angle);
}
  
void rotbmp_pair_layer_increment_angle(RotBmpPairLayer *pair, int32_t angle_change) {
  rot_bitmap_layer_increment_angle(&pair->white_layer, angle_change);
  rot_bitmap_layer_increment_angle(&pair->black_layer, angle_change);
}

void rotbmp_pair_layer_set_src_ic(RotBmpPairLayer *pair, GPoint ic) {
  rot_bitmap_set_src_ic(&pair->white_layer, ic);
  rot_bitmap_set_src_ic(&pair->black_layer, ic);

  layer_set_frame(&pair->layer, &(GRect) { pair->layer.frame.origin,
                                           pair->white_layer.layer.bounds.size });
}

void rotbmp_pair_layer_invert_colors(RotBmpPairLayer *pair) {
  RotBitmapLayer temp = pair->black_layer;
  pair->black_layer = pair->white_layer;
  pair->white_layer = temp;


  set_compositing(pair);
}
