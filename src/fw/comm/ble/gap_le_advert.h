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

#include <bluetooth/bluetooth_types.h>

typedef enum {
  GAPLEAdvertisingJobTagLegacy = 1,
  GAPLEAdvertisingJobTagDiscovery,
  GAPLEAdvertisingJobTagReconnection,
  GAPLEAdvertisingJobTagiOSAppLaunch,
} GAPLEAdvertisingJobTag;

struct GAPLEAdvertisingJob;

//! Opaque reference to an advertising job.
typedef struct GAPLEAdvertisingJob * GAPLEAdvertisingJobRef;

// Each GAPLEAdvertisingJob consists of 1 or more term.
typedef struct GAPLEAdvertisingJobTerm {
  //! The number of seconds this term is going to last for.
  //! @note Use GAPLE_ADVERTISING_DURATION_INFINITE to indicate the term should last forever.
  //! @note Use GAPLE_ADVERTISING_DURATION_LOOP_AROUND to indicate that the scheduler
  //! should loop back to the first term.
  uint16_t duration_secs;

  union {
    struct {
      //! Advertising interval range in slots:
      //! @note Use GAPLE_ADVERTISING_INFINITE_INTERVAL_SLOTS to indicate
      //! the term should be "silent".
      uint16_t min_interval_slots;
      uint16_t max_interval_slots;
    };
    //! The index to loop back to.
    //! @note only valid when duration_secs is GAPLE_ADVERTISING_DURATION_LOOP_AROUND.
    uint16_t loop_around_index;
  };
} GAPLEAdvertisingJobTerm;

//! Function pointer to callback to handle the unscheduling of a job.
//! In the callback, the client can clear its reference to the
//! job and update any other state. There can be 3 reasons for a job to get
//! unscheduled: 1) the desired job duration has been reached 2) the job was
//! manually unscheduled by calling gap_le_advert_unschedule 3) the advertising
//! subsystem was torn down, for example when the user put the device into
//! Airplane Mode.
//! @param job The advertising job that is unscheduled.
//! @param completed True if the job was unscheduled automatically because the
//! duration that it was supposed to be on-air, has been reached. False if it
//! was unscheduled and had not reached its duration yet (because it was
//! unscheduled using gap_le_advert_unschedule() or gap_le_advert_deinit()).
//! For infinite jobs, the value will always be false when unscheduled.
//! @param cb_data Pointer to client data as passed into gap_le_advert_schedule
typedef void (*GAPLEAdvertisingJobUnscheduleCallback)(GAPLEAdvertisingJobRef job,
                                                      bool completed,
                                                      void *cb_data);

//! Constant to use with gap_le_advert_schedule to schedule an advertisement job
//! with infinite duration.
#define GAPLE_ADVERTISING_DURATION_INFINITE ((uint16_t) ~0)

//! Constant to use with gap_le_advert_schedule to indicate that the job
//! scheduler should loop back to the first term.
#define GAPLE_ADVERTISING_DURATION_LOOP_AROUND ((uint16_t) 0)

//! Constant to use with gap_le_advert_schedule to schedule a "silence" term.
#define GAPLE_ADVERTISING_SILENCE_INTERVAL_SLOTS ((uint16_t) 0)

//! Schedules an advertisement & scan response job.
//! Based on the given minimum and maximum interval values, an interval is
//! used depending on other time related tasks the Bluetooth controller has to
//! perform.
//! @discussion Note that scheduled jobs will be unscheduled when the Bluetooth
//! stack is torn down (e.g. when going into Airplane Mode).
//! @param payload The payload with the advertising and scan response data to
//! be scheduled for air-time. @see ble_ad_parse.h for functions to build the
//! payload.
//! @param terms A combination of minimum advertisement interval, maximum advertisement
//! interval and duration. Each term is run in the order that they appear in the terms array.
//! The minimum advertisement interval for each term must be at minimum 32 slots (20ms), or
//! 160 slots (100ms) when there is a scan response. The maximum advertisement interval must
//! be larger than or equal to its corresponding min_interval_slots. The duration is the
//! minimum number of seconds that the term will be active. The sum of all the durations is
//! the minimum number of seconds that the advertisement payload has to be on-air.
//! The job is not guaranteed to get a consecutive period of air-time nor is it guaranteed that
//! it will get air-time immediately after returning from this function.
//! @param callback Pointer to a function that should be called when the job
//! is unscheduled. Note: bt_lock() *WILL* be held during the callback to
//! prevent subtle concurrency problems that can cause out-of-order state
//! updates.
//! @see GAPLEAdvertisingJobUnscheduleCallback for more info.
//! @param callback_data Pointer to arbitrary client data that is passed as an
//! argument with the unschedule callback.

//! @param tag A tag that will be used for debug logging.
//! @return Reference to the scheduled job, or NULL if the parameters were not
//! valid.
GAPLEAdvertisingJobRef gap_le_advert_schedule(const BLEAdData *payload,
                            const GAPLEAdvertisingJobTerm *terms,
                            uint8_t num_terms,
                            GAPLEAdvertisingJobUnscheduleCallback callback,
                            void *callback_data,
                            GAPLEAdvertisingJobTag tag);

//! Unschedules an existing advertisement job.
//! It is safe to call this function with a reference to a non-existing job.
//! @param advertisement_job Reference to the job to unschedule.
void gap_le_advert_unschedule(GAPLEAdvertisingJobRef advertisement_job);

//! Unschedules existing advertisement jobs of particular tag types. Only
//! reschedules advertisements after all the requested tag types have been
//! removed
//! @param types an array of tags for the Advertisement Types to remove
//! @param num_types the length of the 'types' list
void gap_le_advert_unschedule_job_types(
    GAPLEAdvertisingJobTag *tag_types, size_t num_types);

//! Convenience function to get the transmission power level in dBm for
//! advertising channels.
int8_t gap_le_advert_get_tx_power(void);

//! Initialize the advertising scheduler.
//! This should be called when setting up the Bluetooth stack.
void gap_le_advert_init(void);

//! Tear down the advertising scheduler and any current jobs.
//! This should be called when tearing down the Bluetooth stack.
void gap_le_advert_deinit(void);

//! The BT controller stops advertising automatically when the master connects
//! to it (the local device being the slave). This should be called so that
//! gap_le_advert can update its internal state and start advertising
//! non-connectable advertisements after the connection is established.
void gap_le_advert_handle_connect_as_slave(void);

//! This should be called so that gap_le_advert can update its internal state
//! and start advertising connectable advertisements.
void gap_le_advert_handle_disconnect_as_slave(void);
