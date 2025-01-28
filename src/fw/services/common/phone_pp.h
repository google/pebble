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

#include <stdint.h>
#include <stdbool.h>


void pp_answer_call(uint32_t cookie);

void pp_decline_call(uint32_t cookie);

void pp_get_phone_state(void);

//! Enables or disables handling the Get Phone State responses.
//! This is part of a workaround to ignore for stray requests that can be in flight after the phone
//! call has been declined by the user from the Pebble.
void pp_get_phone_state_set_enabled(bool enabled);
