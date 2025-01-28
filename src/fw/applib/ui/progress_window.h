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

#include "applib/ui/animation.h"
#include "applib/ui/progress_layer.h"
#include "applib/ui/window.h"
#include "applib/ui/window_stack.h"
#include "apps/system_apps/timeline/peek_layer.h"
#include "services/common/evented_timer.h"

//! @file progress_window.h
//!
//! A UI component that is a window that contains a progress bar. The state of the progress bar
//! is updated using progress_window_set_progress. When the window is first pushed, the progress
//! bar will fill on its own, faking progress until the max_fake_progress_percent threshold is
//! hit. Once the client wishes to indicate success or failure, calling
//! progress_window_set_progress_success or progress_window_set_progress_failure will cause the
//! UI to animate out to indicate the result, followed by calling the .finished callback if
//! provided. Once progress_window_set_progress_success or progress_window_set_progress_failure
//! has been called, subsequent calls will be ignored.

#define PROGRESS_WINDOW_DEFAULT_FAKE_PERCENT 15
#define PROGRESS_WINDOW_DEFAULT_FAILURE_DELAY_MS 1000

typedef struct ProgressWindow ProgressWindow;

typedef void (*ProgressWindowFinishedCallback)(ProgressWindow *window, bool success, void *context);

typedef struct {
  //! Callback for when the window has finished any animations that are triggered by
  //! progress_window_set_progress_success or progress_window_set_progress_failure.
  ProgressWindowFinishedCallback finished;
} ProgressWindowCallbacks;

typedef enum {
  ProgressWindowState_FakeProgress,
  ProgressWindowState_RealProgress,
  ProgressWindowState_Result
} ProgressWindowState;

struct ProgressWindow {
  //! UI
  Window window;
  ProgressLayer progress_layer;

  //! In the event of a failure, shows a client supplied timeline resource and message.
  //! see progress_window_set_progress_failure
  PeekLayer peek_layer;

  Animation *result_animation;

  ProgressWindowCallbacks callbacks;
  void *context; //!< context for above callbacks

  //! What state we're in.
  ProgressWindowState state;

  //! Timer to fill the bar with fake progress at the beginning
  EventedTimerID fake_progress_timer;
  //! Timer to keep the failure peek layer on screen for a bit before finishing
  EventedTimerID peek_layer_timer;
  //! The progress we've indicated so far
  int16_t progress_percent;
  //! Maximum fake progress
  int16_t max_fake_progress_percent;
  //! Whether the peek layer was used to indicate failure. We only use it if the client specifies
  //! a timeline resource or a message, otherwise we skip showing the peek layer.
  bool is_peek_layer_used;
};


void progress_window_init(ProgressWindow *data);

void progress_window_deinit(ProgressWindow *data);

ProgressWindow *progress_window_create(void);

void progress_window_destroy(ProgressWindow *window);


void progress_window_push(ProgressWindow *window, WindowStack *window_stack);

//! Helper function to push a progress window to the app window stack.
void app_progress_window_push(ProgressWindow *window);

void progress_window_pop(ProgressWindow *window);


//! Set the maximum percentage we should fake progress to until real progress is required.
void progress_window_set_max_fake_progress(ProgressWindow *window,
                                           int16_t max_fake_progress_percent);

//! Update the progress to a given percentage. This will stop any further fake progress being shown
//! the first time this is called. Note that setting progress to 100 is not the same as calling
//! one of the progress_windw_set_result_* methods.
void progress_window_set_progress(ProgressWindow *window, int16_t progress);

//! Tell the ProgressWindow it should animate in a way to show success. When the animation is
//! complete, .callbacks.finished will be called if previously provided.
void progress_window_set_result_success(ProgressWindow *window);

//! Tell the ProgressWindow it should animate in a way to show failure. When the animation is
//! complete, .callbacks.finished will be called if previously provided.
//!
//! @param timeline_res_id optional timeline resource, can be 0 if not desired
//! @param message optional message, can be NULL
//! @param delay duration of the progress bar shrinking animation in milliseconds
void progress_window_set_result_failure(ProgressWindow *window, uint32_t timeline_res_id,
                                        const char *message, uint32_t delay);

void progress_window_set_callbacks(ProgressWindow *window, ProgressWindowCallbacks callbacks,
                                   void *context);

//! @internal
void progress_window_set_back_disabled(ProgressWindow *window, bool disabled);
