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
#include "drivers/ambient_light.h"
#include "services/common/hrm/hrm_manager.h"
#include "util/time/time.h"

//! @addtogroup Foundation
//! @{
//!   @addtogroup EventService
//!   @{
//!     @addtogroup HealthService
//!
//! \brief Get access to health information like step count, sleep totals, etc.
//!
//! The HealthService provides your app access to the step count and sleep activity of the user.
//!
//! @{

// convenient macros to distinguish between health and no health.
// TODO: PBL-21978 remove redundant comments as a workaround around for SDK generator
#if defined(PBL_HEALTH)

//! Convenience macro to switch between two expressions depending on health support.
//! On platforms with health support the first expression will be chosen, the second otherwise.
#define PBL_IF_HEALTH_ELSE(if_true, if_false) (if_true)

#else

//! Convenience macro to switch between two expressions depending on health support.
//! On platforms with health support the first expression will be chosen, the second otherwise.
#define PBL_IF_HEALTH_ELSE(if_true, if_false) (if_false)
#endif

//! Health metric values used to retrieve health data.
//! For example, using \ref health_service_sum().
typedef enum {
  //! The number of steps counted.
  HealthMetricStepCount,
  //! The number of seconds spent active (i.e. not resting).
  HealthMetricActiveSeconds,
  //! The distance walked, in meters.
  HealthMetricWalkedDistanceMeters,
  //! The number of seconds spent sleeping.
  HealthMetricSleepSeconds,
  //! The number of sleep seconds in the 'restful' or deep sleep state.
  HealthMetricSleepRestfulSeconds,
  //! The number of kcal (Calories) burned while resting due to resting metabolism.
  HealthMetricRestingKCalories,
  //! The number of kcal (Calories) burned while active.
  HealthMetricActiveKCalories,
  //! The heart rate, in beats per minute. This is a filtered value that is at most 15 minutes old.
  HealthMetricHeartRateBPM,
  //! The raw heart rate value of the most recent sample, in beats per minute.
  HealthMetricHeartRateRawBPM,
} HealthMetric;

//! Type used to represent HealthMetric values
typedef int32_t HealthValue;

//! Type used as a handle to a registered metric alert (returned by
//! \ref health_service_register_metric_alert)
struct HealthMetricAlert;
typedef struct HealthMetricAlert HealthMetricAlert;


//! Return the sum of a \ref HealthMetric's values over a time range.
//! The `time_start` and `time_end` parameters define the range of time you want the sum for.
//! @note The value returned will be based on daily totals, weighted for the length of the
//! specified time range. This may change in the future.
//! @param metric The metric to query for data.
//! @param time_start UTC time of the earliest data item to incorporate into the sum.
//! @param time_end UTC time of the most recent data item to incorporate into the sum.
//! @return The sum of that metric over the given time range, if available.
HealthValue health_service_sum(HealthMetric metric, time_t time_start, time_t time_end);

//! Convenience wrapper for \ref health_service_sum() that returns the sum for today.
//! @param metric The metric to query.
//! @return The sum of that metric's data for today, if available.
HealthValue health_service_sum_today(HealthMetric metric);

//! Convenience function for peeking at the current value of a metric. This is useful for metrics
//! like \ref HealthMetricHeartRateBPM that represent instantaneous values. It is NOT applicable for
//! metrics like \ref HealthMetricStepCount that must be accumulated over time (it will return 0 if
//! passed that type of metric). This call is equivalent to calling
//! `health_service_aggregate_averaged(metric, time(NULL), time(NULL), HealthAggregationAvg,
//! HealthServiceTimeScopeOnce)`
//! @param metric The metric to query.
//! @return The current value of that metric, if available.
HealthValue health_service_peek_current_value(HealthMetric metric);

//! Used by \ref health_service_sum_averaged() to specify how the average is computed.
typedef enum {
  //! No average computed. The result is the same as calling \ref health_service_sum().
  HealthServiceTimeScopeOnce,
  //! Compute average using the same day from each week. For example, every Monday if the passed
  //! in time range falls on a Monday.
  HealthServiceTimeScopeWeekly,
  //! Compute average using either weekdays (Monday to Friday) or weekends (Saturday and Sunday),
  //! depending on which day the passed in time range falls.
  HealthServiceTimeScopeDailyWeekdayOrWeekend,
  //! Compute average across all days of the week.
  HealthServiceTimeScopeDaily,
} HealthServiceTimeScope;

//! Return the value of a metric's sum over a given time range between `time_start`
//! and `time_end`. Using this call you can specify the time range that you are interested in
//! getting the average for, as well as a `scope` specifier on how to compute an average of the sum.
//! For example, if you want to get the average number of steps taken from 12 AM (midnight) to 9 AM
//! across all days you would specify:
//! \code{.c}
//! time_t time_start = time_start_of_today();
//! time_t time_end = time_start + (9 * SECONDS_PER_HOUR);
//! HealthValue value = health_service_sum_averaged(HealthMetricStepCount, time_start,
//!    time_end, HealthServiceTimeScopeDaily);
//! \endcode
//! If you want the average number of steps taken on a weekday (Monday to Friday) and today is a
//! Monday (in the local timezone) you would specify:
//! \code{.c}
//! time_start = time_start_of_today();
//! time_end = time_start + SECONDS_PER_DAY;
//! HealthValue value = health_service_sum_averaged(HealthMetricStepCount, time_start,
//!    time_end, HealthServiceTimeScopeDailyWeekdayOrWeekend);
//! \endcode
//!
//! Note that this call is the same as calling `health_service_aggregate_averaged(metric,
//! time_start, time_end, HealthAggregationSum, scope)`
//!
//! @param metric Which \ref HealthMetric to query.
//! @param time_start UTC time of the start of the query interval.
//! @param time_end UTC time of the end of the query interval.
//! @param scope \ref HealthServiceTimeScope value describing how the average should be computed.
//! @return The average of the sum of the given metric over the given time range, if available.
HealthValue health_service_sum_averaged(HealthMetric metric, time_t time_start, time_t time_end,
                                        HealthServiceTimeScope scope);


//! Used by \ref health_service_aggregate_averaged() to specify what type of aggregation to perform.
//! This aggregation is applied to the metric before the average is computed.
typedef enum {
  //! Sum the metric. The result is the same as calling \ref health_service_sum_averaged(). This
  //! operation is only applicable for metrics that accumulate, like HealthMetricStepCount,
  //! HealthMetricActiveSeconds, etc.
  HealthAggregationSum,
  //! Use the average of the metric. This is only applicable for metrics that measure instantaneous
  //! values, like HealthMetricHeartRateBPM
  HealthAggregationAvg,
  //! Use the minimum value of the metric. This is only applicable for metrics that measure
  //! instantaneous values, like HealthMetricHeartRateBPM
  HealthAggregationMin,
  //! Use the maximum value of the metric. This is only applicable for metrics that measure
  //! instantaneous values, like HealthMetricHeartRateBPM
  HealthAggregationMax
} HealthAggregation;


//! Return the value of an aggregated metric over a given time range. This call is more
//! flexible than health_service_sum_averaged because it lets you specify which aggregation function
//! to perform.
//!
//! The aggregation function `aggregation` is applied to the metric `metric` over the given time
//! range `time_start` to `time_end` first, and then an average is computed based on the passed in
//! `scope`.
//!
//! For example, if you want to get the average number of steps taken from 12 AM (midnight) to 9 AM
//! across all days you would specify:
//! \code{.c}
//! time_t time_start = time_start_of_today();
//! time_t time_end = time_start + (9 * SECONDS_PER_HOUR);
//! HealthValue value = health_service_aggregate_averaged(HealthMetricStepCount, time_start,
//!    time_end, HealthAggregationSum, HealthServiceTimeScopeDaily);
//! \endcode
//!
//! If you want to compute the average heart rate on Mondays and today is a Monday, you would
//! specify:
//! \code{.c}
//! time_t time_start = time_start_of_today(),
//! time_t time_end = time_start + SECONDS_PER_DAY,
//! HealthValue value = health_service_aggregate_averaged(HealthMetricHeartRateBPM, time_start,
//!    time_end, HealthAggregationAvg, HealthServiceTimeScopeWeekly);
//! \endcode
//!
//! To get the average of the minimum heart rate seen on Mondays for example, you would instead
//! pass in `HealthAggregationMin`
//!
//! Certain HealthAggregation operations are only applicable to certain types of metrics. See the
//! notes above on \ref HealthAggregation for details. Use
//! \ref health_service_metric_aggregate_averaged_accessible to check for applicability at run
//! time.
//!
//! @param metric Which \ref HealthMetric to query.
//! @param time_start UTC time of the start of the query interval.
//! @param time_end UTC time of the end of the query interval.
//! @param aggregation the aggregation function to perform on the metric. This operation is
//!   performed across the passed in time range `time_start` to `time_end`.
//! @param scope \ref HealthServiceTimeScope value describing how the average should be computed.
//!  Use `HealthServiceTimeScopeOnce` to not compute an average.
//! @return The average of the aggregation performed on the given metric over the given time range,
//!  if available.
HealthValue health_service_aggregate_averaged(HealthMetric metric, time_t time_start,
                                              time_t time_end, HealthAggregation aggregation,
                                              HealthServiceTimeScope scope);


//! Health-related activities that can be accessed using
// \ref health_service_peek_current_activities() and \ref health_service_activities_iterate().
typedef enum {
  //! No special activity.
  HealthActivityNone = 0,
  //! The 'sleeping' activity.
  HealthActivitySleep = 1 << 0,
  //! The 'restful sleeping' activity.
  HealthActivityRestfulSleep = 1 << 1,
  //! The 'walk' activity.
  HealthActivityWalk = 1 << 2,
  //! The 'run' activity.
  HealthActivityRun = 1 << 3,
  //! The 'generic' activity.
  HealthActivityOpenWorkout = 1 << 4,
} HealthActivity;

//! A mask value representing all available activities
#define HealthActivityMaskAll ((HealthActivityOpenWorkout << 1) - 1)

//! Expresses a set of \ref HealthActivity values as a bitmask.
typedef uint32_t HealthActivityMask;

//! Return a \ref HealthActivityMask containing a set of bits, one set for each
//! activity that is currently active.
//! @return A bitmask with zero or more \ref HealthActivityMask bits set as appropriate.
HealthActivityMask   health_service_peek_current_activities(void);

//! Callback used by \ref health_service_activities_iterate().
//! @param activity Which activity the caller is being informed about.
//! @param time_start Start UTC time of the activity.
//! @param time_end End UTC time of the activity.
//! @param context The `context` parameter initially passed
//!     to \ref health_service_activities_iterate().
//! @return `true` if you are interested in more activities, or `false` to stop iterating.
typedef bool (*HealthActivityIteratorCB)(HealthActivity activity, time_t time_start,
                                         time_t time_end, void *context);

//! Iteration direction, passed to \ref health_service_activities_iterate().
//! When iterating backwards (`HealthIterationDirectionPast`), activities that have a greater value
//! for `time_end` come first.
//! When iterating forward (`HealthIterationDirectionFuture`), activities that have a smaller value
//! for `time_start` come first.
typedef enum {
  //! Iterate into the past.
  HealthIterationDirectionPast,
  //! Iterate into the future.
  HealthIterationDirectionFuture,
} HealthIterationDirection;

//! Iterates backwards or forward within a given time span to list all recorded activities.
//! For example, this can be used to find the last recorded sleep phase or all deep sleep phases in
//! a given time range. Any activity that overlaps with `time_start` and `time_end` will be
//! included, even if the start time starts before `time_start` or end time ends after `time_end`.
//! @param activity_mask A bitmask containing set of activities you are interested in.
//! @param time_start UTC time of the earliest time you are interested in.
//! @param time_end UTC time of the latest time you are interested in.
//! @param direction The direction in which to iterate.
//! @param callback Developer-supplied callback that is called for each activity iterated over.
//! @param context Developer-supplied context pointer that is passed to the callback.
void health_service_activities_iterate(HealthActivityMask activity_mask, time_t time_start,
                                       time_t time_end, HealthIterationDirection direction,
                                       HealthActivityIteratorCB callback, void *context);

//! Possible values returned by \ref health_service_metric_accessible().
//! The values are used in combination as a bitmask.
//! For example, to check if any data is available for a given request use:
//! bool any_data_available = value & HealthServiceAccessibilityMaskAvailable;
typedef enum {
  //! Return values are available and represent the collected health information.
  HealthServiceAccessibilityMaskAvailable = 1 << 0,
  //! The user hasn't granted permission.
  HealthServiceAccessibilityMaskNoPermission = 1 << 1,
  //! The queried combination of time span and \ref HealthMetric or \ref HealthActivityMask
  //! is currently unsupported.
  HealthServiceAccessibilityMaskNotSupported = 1 << 2,
  //! No samples were recorded for the given time span.
  HealthServiceAccessibilityMaskNotAvailable = 1 << 3,
} HealthServiceAccessibilityMask;

//! Check if a certain combination of metric and time span is accessible using
//! \ref health_service_sum by returning a value of \ref HealthServiceAccessibilityMask. Developers
//! should check if the return value is \ref HealthServiceAccessibilityMaskAvailable before calling
//! \ref health_service_sum.
//!
//! Note that this call is the same as calling `health_service_metric_averaged_accessible(metric,
//! time_start, time_end, HealthServiceTimeScopeOnce)`
//!
//! @param metric The metric to query for data.
//! @param time_start Earliest UTC time you are interested in.
//! @param time_end Latest UTC time you are interested in.
//! @return A \ref HealthServiceAccessibilityMask representing the accessible metrics
//! in this time range.
HealthServiceAccessibilityMask health_service_metric_accessible(
    HealthMetric metric, time_t time_start, time_t time_end);

//! Check if a certain combination of metric, time span, and scope is accessible for calculating
//! summed, averaged data by returning a value of \ref HealthServiceAccessibilityMask. Developers
//! should check if the return value is \ref HealthServiceAccessibilityMaskAvailable before calling
//! \ref health_service_sum_averaged.
//!
//! Note that this call is the same as calling
//! `health_service_metric_aggregate_averaged_accessible(metric, time_start, time_end,
//!  HealthAggregationSum, HealthServiceTimeScopeOnce)`
//!
//! @param metric The metric to query for averaged data.
//! @param time_start Earliest UTC time you are interested in.
//! @param time_end Latest UTC time you are interested in.
//! @param scope \ref HealthServiceTimeScope value describing how the average should be computed.
//! @return A \HealthServiceAccessibilityMask value describing whether averaged data is available.
HealthServiceAccessibilityMask health_service_metric_averaged_accessible(
    HealthMetric metric, time_t time_start, time_t time_end, HealthServiceTimeScope scope);

//! Check if a certain combination of metric, time span, aggregation operation, and scope is
//! accessible for calculating aggregated, averaged data by returning a value of
//! \ref HealthServiceAccessibilityMask. Developers should check if the return value is
//! \ref HealthServiceAccessibilityMaskAvailable before calling
//! \ref health_service_aggregate_averaged.
//! @param metric The metric to query for averaged data.
//! @param time_start Earliest UTC time you are interested in.
//! @param time_end Latest UTC time you are interested in.
//! @param aggregation The aggregation to perform
//! @param scope \ref HealthServiceTimeScope value describing how the average should be computed.
//! @return A \HealthServiceAccessibilityMask value describing whether averaged data is available.
HealthServiceAccessibilityMask health_service_metric_aggregate_averaged_accessible(
    HealthMetric metric, time_t time_start, time_t time_end, HealthAggregation aggregation,
    HealthServiceTimeScope scope);

//! Check if a certain combination of metric, \ref HealthActivityMask and time span is
//! accessible. Developers should check if the return value is
//! \ref HealthServiceAccessibilityMaskAvailable before calling any other HealthService APIs that
//! involve the given activities.
//! @param activity_mask A bitmask of activities you are interested in.
//! @param time_start Earliest UTC time you are interested in.
//! @param time_end Latest UTC time you are interested in.
//! @return A \ref HealthServiceAccessibilityMask representing which of the
//! passed \ref HealthActivityMask values are available under the given constraints.
HealthServiceAccessibilityMask health_service_any_activity_accessible(
    HealthActivityMask activity_mask, time_t time_start, time_t time_end);

//! Health event enum. Passed into the \ref HealthEventHandler.
typedef enum {
  //! All data is considered as outdated and apps should re-read all health data.
  //! This happens after an app is subscribed via \ref health_service_events_subscribe(),
  //! on a change of the day, or in other cases that significantly change the underlying data.
  HealthEventSignificantUpdate = 0,
  //! Recent values around \ref HealthMetricStepCount, \ref HealthMetricActiveSeconds,
  //! or \ref HealthMetricWalkedDistanceMeters have changed.
  HealthEventMovementUpdate = 1,
  //! Recent values around \ref HealthMetricSleepSeconds, \ref HealthMetricSleepRestfulSeconds,
  //! \ref HealthActivitySleep, and \ref HealthActivityRestfulSleep changed.
  HealthEventSleepUpdate = 2,
  //! A metric has crossed the threshold set by \ref health_service_register_metric_alert.
  HealthEventMetricAlert = 3,
  //! Value of \ref HealthMetricHeartRateBPM or \ref HealthMetricHeartRateRawBPM has changed.
  HealthEventHeartRateUpdate = 4,
} HealthEventType;

//! Developer-supplied event handler, called when a health-related event occurs after subscribing
//! via \ref health_service_events_subscribe();
//! @param event The type of health-related event that occured.
//! @param context The developer-supplied context pointer.
typedef void (*HealthEventHandler)(HealthEventType event, void *context);

//! Subscribe to HealthService events. This allocates a cache on the application's heap of up
//! to 2048 bytes that will be de-allocated if you call \ref health_service_events_unsubscribe().
//! If there's not enough heap available, this function will return `false` and will not
//! subscribe to any events.
//! @param handler Developer-supplied event handler function.
//! @param context Developer-supplied context pointer.
//! @return `true` on success, `false` on failure.
bool health_service_events_subscribe(HealthEventHandler handler, void *context);

//! Unsubscribe from HealthService events.
//! @return `true` on success, `false` on failure.
bool health_service_events_unsubscribe(void);

//! Set the desired sampling period for heart rate readings. Normally, the system will sample the
//! heart rate using a sampling period that is automatically chosen to provide useful information
//! without undue battery drain (it automatically samples more often during periods of intense
//! activity, and less often when the user is idle). If desired though, an application can request a
//! specific sampling period using this call. The system will use this as a suggestion, but does not
//! guarantee that the requested period will be used. The actual sampling period may be greater or
//! less due to system needs or heart rate sensor reading quality issues.
//!
//! Each time a new heart rate reading becomes available, a `HealthEventHeartRateUpdate` event will
//! be sent to the application's `HealthEventHandler`. The sample period request will remain in
//! effect the entire time the app is running unless it is explicitly cancelled (by calling this
//! method again with 0 as the desired interval). If the app exits without first cancelling the
//! request, it will remain in effect even for a limited time afterwards. To determine how long it
//! will remain active after the app exits, use
//! `health_service_get_heart_rate_sample_period_expiration_sec`.
//!
//! Unless the app explicitly needs to access to historical high-resolution heart rate data, it is
//! best practice to always cancel the sample period request before exiting in order to maximize
//! battery life. Historical heart rate data can be accessed using the
//! `health_service_get_minute_history` call.
//! @param interval_sec desired interval between heart rate reading updates. Pass 0 to
//!   go back to automatically chosen intervals.
//! @return `true` on success, `false` on failure
bool health_service_set_heart_rate_sample_period(uint16_t interval_sec);

//! Return how long a heart rate sample period request (sent via
//! `health_service_set_heart_rate_sample_period`) will remain active after the app exits. If
//! there is no current request by this app, this call will return 0.
//! @return The number of seconds the heart rate sample period request will remain active after
//! the app exits, or 0 if there is no active request by this app.
uint16_t health_service_get_heart_rate_sample_period_expiration_sec(void);

//! Register for an alert when a metric crosses the given threshold. When the metric crosses this
//! threshold (either goes above or below it), a \ref HealthEventMetricAlert event will be
//! generated. To cancel this registration, pass the returned \ref HealthMetricAlert value to
//! \ref health_service_cancel_metric_alert. The only metric currently supported by this call is
//! \ref HealthMetricHeartRateBPM, but future versions may support additional metrics. To see if a
//! specific metric is supported by this call, use:
//! \code{.c}
//! time_t now = time(NULL);
//! HealthServiceAccessibilityMask accessible =
//!     health_service_metric_aggregate_averaged_accessible(metric, now, now, HealthAggregationAvg,
//!     HealthServiceTimeScopeOnce);
//! bool alert_supported = (accessible & HealthServiceAccessibilityMaskAvailable);
//! \endcode
//!
//! In the current implementation, only one alert per metric can be registered at a time. Future
//! implementations may support two or more simulataneous alert registrations per metric. To change
//! the alert threshold in the current implementation, cancel the original registration
//! using `health_service_cancel_metric_alert` before registering the new threshold.
//! @param metric Which \ref HealthMetric to query.
//! @param threshold The threshold value.
//! @return handle to the alert registration on success, NULL on failure
HealthMetricAlert *health_service_register_metric_alert(HealthMetric metric, HealthValue threshold);

//! Cancel an metric alert previously created with \ref health_service_register_metric_alert.
//! @param alert the \ref HealthMetricAlert previously returned by
//!  \ref health_service_register_metric_alert
//! @return `true` on success, `false` on failure
bool health_service_cancel_metric_alert(HealthMetricAlert *alert);

//! Structure representing a single minute data record returned
//! by \ref health_service_get_minute_history().
//! The `orientation` field encodes the angle of the watch in the x-y plane (the "yaw") in the
//! lower 4 bits (360 degrees linearly mapped to 1 of 16 different values) and the angle to the
//! z axis (the "pitch") in the upper 4 bits.
//! The `vmc` value is a measure of the total amount of movement seen by the watch. More vigorous
//! movement yields higher VMC values.
typedef struct {
  uint8_t steps;              //!< Number of steps taken in this minute.
  uint8_t orientation;        //!< Quantized average orientation.
  uint16_t vmc;               //!< Vector Magnitude Counts (vmc).
  bool is_invalid: 1;         //!< `true` if the item doesn't represents actual data
                              //!< and should be ignored.
  AmbientLightLevel light: 3; //!< Instantaneous light level during this minute.
  uint8_t padding: 4;
  uint8_t heart_rate_bpm;     //!< heart rate in beats per minute
  uint8_t reserved[6];        //!< Reserved for future use.
} HealthMinuteData;

//! Return historical minute data records. This fills in the `minute_data` array parameter with
//! minute by minute statistics of the user's steps, average watch orientation, etc. The data is
//! returned in time order, with the oldest minute data returned at `minute_data[0]`.
//! @param minute_data Pointer to an array of \ref HealthMinuteData records that will be filled
//!      in with the historical minute data.
//! @param max_records The maximum number of records the `minute_data` array can hold.
//! @param[in,out] time_start On entry, the UTC time of the first requested record. On exit,
//!      the UTC time of the first second of the first record actually returned.
//!      If `time_start` on entry is somewhere in the middle of a minute interval, this function
//!      behaves as if the caller passed in the start of that minute.
//! @param[in,out] time_end On entry, the UTC time of the end of the requested range of records. On
//!      exit, the UTC time of the end of the last record actually returned (i.e. start time of last
//!      record + 60). If `time_end` on entry is somewhere in the middle of a minute interval, this
//!      function behaves as if the caller passed in the end of that minute.
//! @return Actual number of records returned. May be less then the maximum requested.
//! @note If the return value is zero, `time_start` and `time_end` are meaningless.
//!      It's not guaranteed that all records contain valid data, even if the return value is
//!      greater than zero. Check `HealthMinuteData.is_invalid` to see if a given record contains
//!      valid data.
uint32_t health_service_get_minute_history(HealthMinuteData *minute_data, uint32_t max_records,
                                           time_t *time_start, time_t *time_end);

//! Types of measurement system a \ref HealthMetric may be measured in.
typedef enum {
  //! The measurement system is unknown, or does not apply to the chosen metric.
  MeasurementSystemUnknown,
  //! The metric measurement system.
  MeasurementSystemMetric,
  //! The imperial measurement system.
  MeasurementSystemImperial,
} MeasurementSystem;

//! Get the preferred measurement system for a given \ref HealthMetric, if the user has chosen
//! a preferred system and it is applicable to that metric.
//! @param metric A metric value chosen from \ref HealthMetric.
//! @return A value from \ref MeasurementSystem if applicable, else \ref MeasurementSystemUnknown.
MeasurementSystem health_service_get_measurement_system_for_display(HealthMetric metric);

// -------------------------------------------------------------------------------------------------
// The following types are internally used to implement the cache for the health service.
// They are declared here and not in health_service_private.h to avoid a cyclic dependency between
// events.h and their declaration.

//! @internal
// Auxiliary data passed into HealthEventHandler along with the health event type.
typedef struct {
  uint32_t steps;                           //!< Total number of steps for today
} HealthEventMovementUpdateData;

//! @internal
typedef struct {
  uint32_t total_seconds;                   //!< Total number of seconds of sleep for today
  uint32_t total_restful_seconds;           //!< Total number of restful seconds
} HealthEventSleepUpdateData;

//! @internal
typedef struct {
  uint16_t day_id;                          //!< The new day_id for today
} HealthEventSignificantUpdateData;

//! @internal
typedef struct {
  uint8_t current_bpm;
  uint8_t resting_bpm;
  HRMQuality quality:8;
  bool is_filtered;
} HealthEventHeartRateUpdateData;

//! @internal
typedef struct  {
  union {
    HealthEventMovementUpdateData movement_update;
    HealthEventSleepUpdateData sleep_update;
    HealthEventSignificantUpdateData significant_update;
    HealthEventHeartRateUpdateData heart_rate_update;
  };
} HealthEventData;

//!     @} // end addtogroup HealthService
//!   @} // end addtogroup EventService
//! @} // end addtogroup Foundation
