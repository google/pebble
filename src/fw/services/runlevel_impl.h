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

// Definitions used to implement runlevels.
//
// The set of runlevels is defined in the runlevel.def X-Macro file. These
// definitions are used to construct two enums, RunLevel (in runlevel.h) and
// RunLevelBit (in this header).
//
// The set of runlevels for which a service should be enabled is defined by
// bitwise-OR-ing the RunLevelBit constants for every runlevel that the service
// should be enabled to form an enable-mask. Testing whether a service should
// be enabled for a given runlevel is simply
// (enable_mask & (1 << runlevel) != 0).
//
// The RunLevelBit constants take the form R_<name> to minimize visual clutter
// when defining enable-masks. Since this header is only included in the source
// files for which enable-masks are defined, the potential for namespace
// pollution is minimized.

#define RUNLEVEL(number, name) _Static_assert( \
      0 <= number && number <= 31, \
      "The numeric value of runlevel " #name " (" #number ")" \
      " is out of range. Only runlevels in the range 0 <= level <= 31" \
      " are supported.");
#include "runlevel.def"
#undef RUNLEVEL

typedef enum RunLevelBit {
#define RUNLEVEL(number, name) R_##name = (1 << number),
#include "runlevel.def"
#undef RUNLEVEL
} RunLevelBit;

struct ServiceRunLevelSetting {
  void (*set_enable_fn)(bool);
  RunLevelBit enable_mask;
};
