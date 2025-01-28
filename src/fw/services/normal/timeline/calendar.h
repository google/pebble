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

#include "event.h"

//! This module puts events that report the current state of calendar events.
//! The states are:
//! - "no calendar events ongoing"
//! - "one or more calendar events ongoing"
//! Not every calendar event start / stop produces an event, but every transition is guaranteed
//! to put an event.


const TimelineEventImpl *calendar_get_event_service(void);

//! Used to determine if there is currently an event going on, used for Smart DND
bool calendar_event_is_ongoing(void);

#if UNITTEST
#include "services/common/new_timer/new_timer.h"
TimerID get_calendar_timer_id(void);
void set_calendar_timer_id(TimerID id);
#endif
