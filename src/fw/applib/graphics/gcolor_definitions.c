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

#include "gtypes.h"

//! This is used for performing backward-compatibility conversions with 1-bit GColors.
GColor8 get_native_color(GColor2 color) {
  switch (color) {
    case GColor2Black:
      return GColorBlack;
    case GColor2White:
      return GColorWhite;
    default:
      return GColorClear; // GColorClear defined as ~0, so it is everything else we may receive
  }
}

GColor2 get_closest_gcolor2(GColor8 color) {
  if (color.a == 0) {
    return GColor2Clear;
  }

  switch (color.argb) {
    case GColorBlackARGB8:
      return GColor2Black;
    case GColorWhiteARGB8:
      return GColor2White;
    case GColorClearARGB8:
      return GColor2Clear;
    default:
      return GColor2White; // TODO: This should pick the closes color rather than just white.
  }
}

bool gcolor_equal__deprecated(GColor8 x, GColor8 y) {
  return (x.argb == y.argb);
}

bool gcolor_equal(GColor8 x, GColor8 y) {
  return ((x.argb == y.argb) || ((x.a == 0) && (y.a == 0)));
}
