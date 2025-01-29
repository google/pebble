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

#include <stdbool.h>
#include <stdint.h>


//! new_timer.h
//!
//! NewTimer is a very high priority thread that's used for executing timers and high-priority
//! work for our drivers from interrupts. The timer functionality is just a wrapper around
//! task_timer.h.
//!
//! Note: As of now most of our timers are executed in this thread, even if it's just a short
//! callback to send an event to another thread with evented_timer.h. In the future these other
//! threads will probably use their own TaskTimerManager instances instead and this thread will
//! will get a whole lot less busy and reserved for only high priority work. At that time this
//! thread will probably get renamed something like KernelHighPriority.

typedef void (*NewTimerCallback)(void *data);

typedef uint32_t TimerID;
#define TIMER_INVALID_ID 0

//! Flags for new_timer_start() 
//! TIMER_START_FLAG_REPEATING          make this a repeating timer
//!
//! TIMER_START_FLAG_FAIL_IF_EXECUTING  If the timer callback is currently executing, do not
//! schedule the timer and return false from new_timer_start. This can be helpful in usage patterns
//! where the timer callback might be blocked on a semaphore owned by the task issuing the start. 
//!
//! TIMER_START_FLAG_FAIL_IF_SCHEDULED If the timer is already scheduled, do not reschedule it and
//! return false from new_timer_start. 
#define TIMER_START_FLAG_REPEATING          0x01
#define TIMER_START_FLAG_FAIL_IF_EXECUTING  0x02
#define TIMER_START_FLAG_FAIL_IF_SCHEDULED  0x04


//! Creates a new timer object. This timer will start out in the stopped state.
//! @return the non-zero timer id or TIMER_INVALID_ID if OOM
TimerID new_timer_create(void);

//! Schedule an existing timer to execute in timeout_ms. If the timer was already started, it will be 
//! rescheduled for the new time. 
//! @param[in] timer ID
//! @param[in] timeout_ms timeout in milliseconds
//! @param[in] cb pointer to the user's callback procedure
//! @param[in] cb_data reference data for the callback
//! @param[in] flags one or more TIMER_START_FLAG_.* flags
//! @return True if succesful, false if timer was not rescheduled. Note that it will never return
//!     false if none of the FAIL_IF_* flags are set.
bool new_timer_start(TimerID timer, uint32_t timeout_ms, NewTimerCallback cb, void *cb_data, 
                     uint32_t flags);

//! Stop a timer. For repeating timers, even if this method returns false (callback is currently
//! executing) the timer will not run again. Safe to call on timers that aren't currently started.
//! @param[in] timer ID
//! @return False if timer's callback is current executing, true if not.
bool new_timer_stop(TimerID timer);

//! Get scheduled status of a timer
//! @param[in] timer ID
//! @param[out] expire_ms_p if not NULL, the number of milliseconds until this timer will fire is returned in
//!              *expire_ms_p. If the timer is not scheduled (return value is false), this value should be ignored.
//! @return True if timer is scheduled, false if not
bool new_timer_scheduled(TimerID timer, uint32_t *expire_ms_p);

//! Delete a timer
//! @param[in] timer ID
void new_timer_delete(TimerID timer);


// Timer watchdog uses this
void* new_timer_debug_get_current_callback(void);


typedef void (*NewTimerWorkCallback)(void *data);

//! Push a piece of work onto the new timer thread from an ISR. Used to handle time sensitive
//! hardware events.
//! @return True if the caller should trigger a context switch
bool new_timer_add_work_callback_from_isr(NewTimerWorkCallback cb, void *data);

//! @return True if there was space in the queue, false otherwise
bool new_timer_add_work_callback(NewTimerWorkCallback cb, void *data);
