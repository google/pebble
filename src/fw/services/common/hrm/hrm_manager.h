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

#include "services/common/accel_manager_types.h"
#include "process_management/app_install_types.h"
#include "util/time/time.h"

#include <stdbool.h>
#include <stdint.h>
#include <kernel/pebble_tasks.h>

typedef enum {
  HRMQuality_NoAccel = -2,
  HRMQuality_OffWrist = -1,

  HRMQuality_NoSignal = 0,
  HRMQuality_Worst,
  HRMQuality_Poor,
  HRMQuality_Acceptable,
  HRMQuality_Good,
  HRMQuality_Excellent,
} HRMQuality;

typedef enum {
  HRMFeatureShift_BPM = 0,
  HRMFeatureShift_HRV = 1,
  HRMFeatureShift_LEDCurrent = 2,
  HRMFeatureShift_Diagnostics = 3,

  HRMFeatureShiftMax
} HRMFeatureShift;

typedef enum {
  HRMFeature_BPM = (1 << HRMFeatureShift_BPM), //!< Collect heartrate BPM.
  HRMFeature_HRV = (1 << HRMFeatureShift_HRV), //!< Collect heartrate variability.
  HRMFeature_LEDCurrent = (1 << HRMFeatureShift_LEDCurrent), //!< Collect the LED current
                                                             //!< consumption (uA). This should not
                                                             //!< be made public by the HRM service,
                                                             //!< and should only be used internally
  HRMFeature_Diagnostics = (1 << 3), //!< Collect PPG & Accel data
  HRMFeatureMax
} HRMFeature;

// Hold enough data for 2s worth of samples just in case we miss a handshake
#define HRM_MANAGER_ACCEL_RATE_MILLIHZ (12500)
#define HRM_MANAGER_MAX_ACCEL_SAMPLES ((2 * HRM_MANAGER_ACCEL_RATE_MILLIHZ) / 1000)

// When an app exits, we change its subscription (if any) to expire in this many seconds
#define HRM_MANAGER_APP_EXIT_EXPIRATION_SEC  SECONDS_PER_HOUR

typedef struct {
  AccelRawData data[HRM_MANAGER_MAX_ACCEL_SAMPLES];
  uint32_t num_samples;
} HRMAccelData;

//! Grab the buffer containing accel data for the last 1 second period.
//! This locks the accel sample buffer that lives in the hrm manager.
HRMAccelData * hrm_manager_get_accel_data(void);

//! Unlock the accel sample buffer.
void hrm_manager_release_accel_data(void);

typedef uint32_t HRMSessionRef;
#define HRM_INVALID_SESSION_REF 0

// Send a HRMEvent_SubscriptionExpiring event to the subscriber at least this many seconds before
// the subscription expires (or one subscription interval, whichever is greater)
#define HRM_SUBSCRIPTION_EXPIRING_WARNING_SEC 5

void hrm_manager_init(void);

void hrm_manager_handle_prefs_changed(void);

//! Enable the HRM and subscribe to updates from an app or worker task.
//! This should not be used by KernelBG or KernelMain clients. For KernelBG client subscriptions,
//! please see \ref hrm_manager_subscribe_with_callback. KernelMain clients are not yet supported.
//! If the app/worker is already subscribed, this will update the subscription based on the passed
//! in arguments and return the preexisting HRMSessionRef.
//! @param app_id the application's AppInstallId
//! @param update_interval_s requested update interval
//! @param expire_s after this many seconds, this subscription will automatically expire
//! @param features A bitfield of the features the subscriber would like updates for.
//! @return the HRMSessionRef for this subscription. NULL on failure
HRMSessionRef sys_hrm_manager_app_subscribe(AppInstallId app_id, uint32_t update_interval_s,
                                            uint16_t expire_s, HRMFeature features);

//! Return the HRMSessionRef for an app or worker subscription, if it exists. This call cannot
//! be used for KernelBG subscriptions
//! @param app_id the application's AppInstallId
//! @return the HRMSessionRef for this subscription, or NULL if no subscription exists
HRMSessionRef sys_hrm_manager_get_app_subscription(AppInstallId app_id);

//! Unsubscribe from updates, disabling the HRM device if appropriate.
//! @param session the HRMSessionRef returned by sys_hrm_manager_app_subscribe
//! @return true on success, false on failure
bool sys_hrm_manager_unsubscribe(HRMSessionRef session);

//! Set the enabled features for the given HRM subscription
//! @param session the HRMSessionRef returned by sys_hrm_manager_app_subscribe
//! @param features the desired features
//! @return true on success, false on failure
bool sys_hrm_manager_set_features(HRMSessionRef session, HRMFeature features);

//! Set update interval and expiration time for an existing subscription
//! @param session the HRMSessionRef returned by sys_hrm_manager_app_subscribe
//! @param update_interval_s requested update interval
//! @param expire_s after this many seconds, this subscription will automatically expire
//! @return true on success, false on failure
bool sys_hrm_manager_set_update_interval(HRMSessionRef session, uint32_t update_interval_s,
                                         uint16_t expire_s);

//! Get info on a subscription.
//! @param[in] session the HRMSessionRef returned by sys_hrm_manager_app_subscribe
//! @param[out] app_id if not NULL, the app_id belonging to this subscription is returned here
//! @param[out] update_interval_s if not NULL, the requested update interval is returned here
//! @param[out] expire_s if not NULL, the number of seconds that this subscription will expire in
//! @return true if succss, false if subscription was not found
bool sys_hrm_manager_get_subscription_info(HRMSessionRef session, AppInstallId *app_id,
                                           uint32_t *update_interval_s, uint16_t *expire_s,
                                           HRMFeature *features);

//! Returns true if there is an HRM present & accessible to the HRM Manager.
//! @return true on success, false on failure
bool sys_hrm_manager_is_hrm_present(void);

//! Enable or disable the HRM manager.
//! Disabling the HRM manager does not remove subscribers, however subscribers
//! will no longer receive updates until the hrm manager is enabled again.
//! @param on Whether the HRM should be turned on or off.
void hrm_manager_enable(bool on);

//------------------------------------------------------------------------------
// HRM Driver Interface
//------------------------------------------------------------------------------
// The driver needs to provide new data to the service and needs to pull accel data.

#define MAX_PPG_SAMPLES 20

typedef struct {
  int num_samples;
  uint8_t indexes[MAX_PPG_SAMPLES];
  uint16_t ppg[MAX_PPG_SAMPLES];
  uint16_t tia[MAX_PPG_SAMPLES];
} HRMPPGData;

//! HRMData will contain all HRM information that is currently available from the device.
typedef struct {
  uint16_t led_current_ua;

  uint8_t hrm_bpm;
  HRMQuality hrm_quality;

  uint16_t hrv_ppi_ms;
  HRMQuality hrv_quality;
  uint8_t hrm_status;

  HRMAccelData accel_data;
  HRMPPGData ppg_data;
} HRMData;

//! Callback used by HRM Driver to indicate that new data is available.
//! It can be expected that this will be called approx. 1x per second while the HRM is enabled.
//! This will take the new data and queue up appropriate events given subscribers and features.
void hrm_manager_new_data_cb(const HRMData *data);

//! Cleanup for the given app, which has just exited
void hrm_manager_process_cleanup(PebbleTask task, AppInstallId app_id);
