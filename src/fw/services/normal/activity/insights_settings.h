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

#include "activity.h"
#include "services/normal/filesystem/pfs.h"
#include "util/attributes.h"

#define ACTIVITY_INSIGHTS_SETTINGS_SLEEP_REWARD "sleep_reward"
#define ACTIVITY_INSIGHTS_SETTINGS_SLEEP_SUMMARY "sleep_summary"
#define ACTIVITY_INSIGHTS_SETTINGS_ACTIVITY_REWARD "activity_reward"
#define ACTIVITY_INSIGHTS_SETTINGS_ACTIVITY_SUMMARY "activity_summary"
#define ACTIVITY_INSIGHTS_SETTINGS_ACTIVITY_SESSION "activity_session"

typedef struct PACKED ActivityRewardSettings {
  // Note: these parameters are the number of days in addition to 'today' that we want to look at
  uint8_t min_days_data;                //!< How many days of the metric's history we require
  uint8_t continuous_min_days_data;     //!< How many consecutive days of history we require
  uint8_t target_qualifying_days;       //!< Days that must be above target (on top of 'today')

  uint16_t target_percent_of_median;    //!< Percentage of median qualifying days must hit
  uint32_t notif_min_interval_seconds;  //!< How often we allow this insight to be shown

  // Insight-specific values
  union {
    struct PACKED {
      uint16_t trigger_after_wakeup_seconds; //!< Time we wait before showing sleep reward
    } sleep;

    struct PACKED {
      uint8_t trigger_active_minutes;   //!< Time we must be currently active before showing reward
      uint8_t trigger_steps_per_minute; //!< Steps per minute required for an 'active' minute
    } activity;
  };
} ActivityRewardSettings;

typedef struct PACKED ActivitySummarySettings {
  int8_t above_avg_threshold;           //!< Values greater than this are counted as above avg
                                        //!< In relation to 100% (eg 105% would be 5)
  int8_t below_avg_threshold;           //!< Values less than this are counted as above avg
                                        //!< In relation to 100% (eg 93% would be -7)
  int8_t fail_threshold;                //!< Values less than this are counted as fail
                                        //!< In relation to 100% (e.g. 55% would be -45)

  union {
    struct PACKED {
      uint16_t trigger_minute;                //!< Minute of the day that we trigger the pin
      uint16_t update_threshold_steps;        //!< Step delta that will cause the pin to update
      uint32_t update_max_interval_seconds;   //!< Max time we'll go without updating the pin
      bool show_notification;                 //!< Whether to show a notification
      uint16_t max_fail_steps;                //!< Don't show negative if walked more than X steps
    } activity;

    struct PACKED {
      uint16_t max_fail_minutes;       //!< Don't show negative if slept more than X minutes
      uint16_t trigger_notif_seconds;  //!< Time in seconds after wakeup to notify about sleep
      uint16_t trigger_notif_activity; //!< Minimum amount of steps per minute to trigger the
                                       //!< Sleep summary notification
      uint8_t trigger_notif_active_minutes; //!< Minimum amount of active minutes to trigger the
                                            //!< Sleep summary notification
    } sleep;
  };
} ActivitySummarySettings;

typedef struct PACKED ActivitySessionSettings {
  bool show_notification;                   //!< Whether to show a notification

  union {
    struct PACKED {
      uint16_t trigger_elapsed_minutes;     //!< Minimum length of a walk to be given an insight
      uint16_t trigger_cooldown_minutes;    //!< Minutes wait after end of session before notifying
    } activity;
  };
} ActivitySessionSettings;

typedef struct PACKED ActivityInsightSettings {
  // Common parameters
  uint8_t version;                      //!< Current version of the struct - must be first

  bool enabled;                         //!< Insight enabled
  uint8_t unused;                       //!< Unused

  union {
    ActivityRewardSettings reward;
    ActivitySummarySettings summary;
    ActivitySessionSettings session;
  };
} ActivityInsightSettings;


//! Read a setting from the insights settings
//! @param insights_name the name of the insight for which to get a setting
//! @param[out] settings out an ActivityInsightSettings struct to which the data will be written
//! @returns true if the setting was found and the data is valid, false otherwise
//! @note if this function returns false, settings_out will be zeroed out.
bool activity_insights_settings_read(const char *insight_name,
                                     ActivityInsightSettings *settings_out);

//! Write a setting to the insights settings (used for testing)
//! @param insights_name the name of the insight for which to get a setting
//! @param settings an ActivityInsightSettings struct which contains the data to be written
//! @returns true if the setting was successfully saved
bool activity_insights_settings_write(const char *insight_name,
                                      ActivityInsightSettings *settings);

//! Get the current version of the insights settings
//! @return the version number for the current insights settings
//! @note this is separate from the struct version
uint16_t activity_insights_settings_get_version(void);

//! Initialize insights settings
void activity_insights_settings_init(void);

//! Watch the insights settings file. The callback is called whenever the file is closed with
//! modifications or deleted
//! @param callback Function to call when the file has been modified
//! @return Callback handle for passing into \ref activity_insights_settings_unwatch
PFSCallbackHandle activity_insights_settings_watch(PFSFileChangedCallback callback);

//! Stop watching the settings file
//! @param cb_handle Callback handle which was returned by \ref activity_insights_settings_watch
void activity_insights_settings_unwatch(PFSCallbackHandle cb_handle);
