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

#include "process_management/pebble_process_info.h"
#include "services/common/comm_session/session_analytics.h"
#include "services/normal/activity/activity_insights.h"
#include "services/normal/timeline/item.h"
#include "util/attributes.h"
#include "util/build_id.h"
#include "util/time/time.h"
#include "util/uuid.h"

#if !PLATFORM_TINTIN
#include "services/normal/vibes/vibe_score_info.h"
#endif

// Every analytics blob we send out (device blob, app blob, or event blob) starts out with
// an 8-bit AnalyticsBlobKind followed by a 16-bit version. Here we define the format of the
// event blob. The device and app blobs are defined in analytics_metric_table.h.
// The ANALYTICS_EVENT_BLOB_VERSION value defined here will need to bumped whenever the format of
// the AnalyticsEventBlob structure changes - this includes if ANY of the unions inside of it
// change or a new AnalyticsEvent enum is added.
// Please do not cherrypick any change here into a release branch without first checking
// with Katharine, or something is very likely to break.
#define ANALYTICS_EVENT_BLOB_VERSION 32


//! Types of events that can be logged outside of a heartbeat using analytics_logging_log_event()
typedef enum {
  AnalyticsEvent_AppLaunch,
  AnalyticsEvent_PinOpen,
  AnalyticsEvent_PinAction,
  AnalyticsEvent_CannedResponseSent,
  AnalyticsEvent_CannedResponseFailed,
  AnalyticsEvent_VoiceTranscriptionAccepted,
  AnalyticsEvent_VoiceTranscriptionRejected,
  AnalyticsEvent_PinAppLaunch,
  AnalyticsEvent_BtClassicDisconnect,
  AnalyticsEvent_BtLeDisconnect,
  AnalyticsEvent_Crash,
  AnalyticsEvent_LocalBtDisconnect,
  AnalyticsEvent_BtLockupError,
  AnalyticsEvent_BtClassicConnectionComplete,
  AnalyticsEvent_BtLeConnectionComplete,
  AnalyticsEvent_PinCreated,
  AnalyticsEvent_PinUpdated,
  AnalyticsEvent_BtLeAMS,
  AnalyticsEvent_VoiceTranscriptionAutomaticallyAccepted,
  AnalyticsEvent_StationaryModeSwitch,
  AnalyticsEvent_HealthLegacySleep,
  AnalyticsEvent_HealthLegacyActivity,
  AnalyticsEvent_PutByteTime,
  AnalyticsEvent_HealthInsightCreated,
  AnalyticsEvent_HealthInsightResponse,
  AnalyticsEvent_AppCrash,
  AnalyticsEvent_VibeAccess,
  AnalyticsEvent_HealthActivitySession, // Deprecated
  AnalyticsEvent_BtAppLaunchError,
  AnalyticsEvent_BtLePairingError,
  AnalyticsEvent_BtClassicPairingError,
  AnalyticsEvent_PebbleProtocolSystemSessionEnd,
  AnalyticsEvent_PebbleProtocolAppSessionEnd,
  AnalyticsEvent_AlarmCreated,
  AnalyticsEvent_AlarmTriggered,
  AnalyticsEvent_AlarmDismissed,
  AnalyticsEvent_PPoGATTDisconnect,
  AnalyticsEvent_BtChipBoot,
  AnalyticsEvent_GetBytesStats,
  AnalyticsEvent_RockyAppCrash,
  AnalyticsEvent_AppOOMNative,
  AnalyticsEvent_AppOOMRocky,
  AnalyticsEvent_BtLeMicError,
  AnalyticsEvent_BleHrmEvent,
} AnalyticsEvent;

// AnalyticsEvent_BleHrmEvent
typedef enum {
  BleHrmEventSubtype_SharingAccepted,
  BleHrmEventSubtype_SharingDeclined,
  BleHrmEventSubtype_SharingRevoked,
  BleHrmEventSubtype_SharingTimeoutPopupPresented,
} BleHrmEventSubtype;

typedef struct PACKED {
  BleHrmEventSubtype subtype:8;
} AnalyticsEventBleHrmEvent;

// AnalyticsEvent_AppLaunch event
typedef struct PACKED {
  Uuid uuid;
} AnalyticsEventAppLaunch;

// AnalyticsEvent_PinOpen/Create/Update events
typedef struct PACKED {
  uint32_t time_utc;           // pin utc time
  Uuid parent_id;              // owner app UUID
} AnalyticsEventPinOpenCreateUpdate;

// AnalyticsEvent_PinAction events
typedef struct PACKED {
  uint32_t time_utc;           // pin utc time
  Uuid parent_id;              // owner app UUID
  uint8_t type;                // action type
} AnalyticsEventPinAction;

// AnalyticsEvent_PinAppLaunch event
typedef struct PACKED {
  uint32_t time_utc;           // pins utc time
  Uuid parent_id;              // owner app UUID
} AnalyticsEventPinAppLaunch;

// AnalyticsEvent_PinCreated events
typedef struct PACKED {
  uint32_t time_utc;           // pin utc time
  Uuid parent_id;              // owner app UUID
} AnalyticsEventPinCreated;

// AnalyticsEvent_PinUpdated events
typedef struct PACKED {
  uint32_t time_utc;           // pin utc time
  Uuid parent_id;              // owner app UUID
} AnalyticsEventPinUpdated;

// AnalyticsEvent_CannedResponse.* events.
typedef struct PACKED {
  uint8_t response_size_bytes;
} AnalyticsEventCannedResponse;

// AnalyticsEvent_VoiceResponse.* events.
typedef struct PACKED {
  uint8_t num_sessions;
  uint8_t error_count;
  uint16_t response_size_bytes;
  uint16_t response_len_chars;
  uint32_t response_len_ms;
  Uuid app_uuid;
} AnalyticsEventVoiceResponse;

typedef struct PACKED {
  uint8_t  reason; // the connection status / reason we disconnected
} AnalyticsEventBtConnectionDisconnection;

typedef struct PACKED {
  uint8_t  reason; // The reason we disconnected
  uint8_t  remote_bt_version;
  uint16_t remote_bt_company_id;
  uint16_t remote_bt_subversion_number;
  uint16_t remote_features_supported; // placeholder for supported features
} AnalyticsEventBleDisconnection;

typedef struct CommSession CommSession;

typedef struct PACKED {
  CommSessionCloseReason close_reason:8;
  uint16_t duration_minutes;
} AnalyticsEventPebbleProtocolCommonSessionClose;

typedef struct PACKED {
  AnalyticsEventPebbleProtocolCommonSessionClose common;
} AnalyticsEventPebbleProtocolSystemSessionClose;

typedef struct PACKED {
  AnalyticsEventPebbleProtocolCommonSessionClose common;
  Uuid app_uuid;
} AnalyticsEventPebbleProtocolAppSessionClose;

typedef struct PACKED {
  uint32_t error_code;
} AnalyticsEventBtError;

typedef struct PACKED {
  uint8_t crash_code;
  uint32_t link_register;
} AnalyticsEventCrash;

typedef struct PACKED {
  uint32_t lr;
  uint16_t conn_handle;
} AnalyticsEvent_LocalBTDisconnect;

typedef struct PACKED {
  uint8_t type;
  int32_t aux_info;
} AnalyticsEvent_AMSData;

typedef struct PACKED {
  time_t timestamp;
  uint8_t state_change;
} AnalyticsEvent_StationaryStateChangeData;

typedef struct PACKED {
  uint16_t start_minute;        // minute of day when sleep started (midnight is minute 0)
  uint16_t wake_minute;         // minute of day when sleep ended
  uint16_t total_minutes;       // total minutes of sleep
  uint16_t deep_minutes;        // deep minutes of sleep
} AnalyticsEvent_HealthLegacySleepData;

typedef struct PACKED {
  uint16_t duration_minutes;    // duration in minutes
  uint16_t steps;               // # of steps
} AnalyticsEvent_HealthLegacyActivityData;

typedef struct PACKED {
  uint8_t insight_type;   // numerical id of insight
  uint8_t activity_type;  // activity type, one of ActivitySessionType
  uint8_t response_id;    // numerical id of response
  uint32_t time_utc;      // insight utc time, activity start UTC if activity type is not none
} AnalyticsEvent_HealthInsightResponseData;

typedef struct PACKED {
  uint8_t insight_type;   // numerical id of insight
  uint32_t time_utc;      // insight utc time
  uint8_t percent_tier;   // above average / below average
} AnalyticsEvent_HealthInsightCreatedData;

typedef struct PACKED {
  bool     ppogatt; // true if transport is PPOGATT, else SPP
  uint8_t  conn_intvl_1_25ms; // if ppogatt, the connection interval at end of FW update
  bool     crc_good; // true if calculated CRC matches expected CRC
  uint8_t  type; // see PutBytesObjectType
  uint32_t bytes_transferred;
  uint32_t elapsed_time_ms;
  uint32_t conn_events;
  uint16_t sync_errors;
  uint16_t skip_errors;
  uint16_t other_errors;
} AnalyticsEvent_PutByteTimeData;

//! Used for both AnalyticsEvent_AppCrash and AnalyticsEvent_RockyAppCrash event types!
typedef struct PACKED {
  Uuid uuid;
  uint32_t pc;
  uint32_t lr;
  uint8_t build_id_slice[4];
} AnalyticsEvent_AppCrashData;

typedef enum VibePatternFeature {
  VibePatternFeature_Notifications = 1 << 0,
  VibePatternFeature_PhoneCalls = 1 << 1,
  VibePatternFeature_Alarms = 1 << 2,
} VibePatternFeature;

typedef struct PACKED {
  uint8_t feature;
  uint8_t vibe_pattern_id;
} AnalyticsEvent_VibeAccessData;

typedef struct PACKED {
  uint16_t activity_type;       // activity type, one of ActivitySessionType
  uint32_t start_utc;           // start time of activity, in UTC seconds
  uint32_t elapsed_sec;         // length of activity in seconds.
} AnalyticsEvent_HealthActivitySessionData; // Deprecated

typedef struct PACKED {
  uint8_t hour;
  uint8_t minute;
  bool is_smart;
  uint8_t kind;
  uint8_t scheduled_days[DAYS_PER_WEEK];
} AnalyticsEvent_AlarmData;

typedef struct PACKED AnalyticsEvent_BtChipBootData {
  uint8_t build_id[BUILD_ID_EXPECTED_LEN];
  uint32_t crash_lr;
  uint32_t reboot_reason;
} AnalyticsEvent_BtChipBootData;

typedef struct PACKED AnalyticsEvent_PPoGATTDisconnectData {
  bool successful_reconnect;
  uint32_t time_utc;           // utc time
} AnalyticsEvent_PPoGATTDisconnectData;

typedef struct PACKED AnalyticsEvent_GetBytesStatsData {
  bool     ppogatt; // true if transport is PPOGATT, else SPP
  uint8_t  conn_intvl_1_25ms; // if ppogatt, the connection interval at end of FW update
  uint8_t  type; // see GetBytesObjectType
  uint32_t bytes_transferred;
  uint32_t elapsed_time_ms;
  uint32_t conn_events;
  uint16_t sync_errors;
  uint16_t skip_errors;
  uint16_t other_errors;
} AnalyticsEvent_GetBytesStatsData;

typedef struct PACKED AnalyticsEvent_AppOomData {
  Uuid app_uuid;
  uint32_t requested_size;
  uint32_t total_size;
  uint16_t total_free;
  uint16_t largest_free_block;
} AnalyticsEvent_AppOomData;

typedef struct PACKED {
  uint8_t kind;             // set to ANALYTICS_BLOB_KIND_EVENT
  uint16_t version;         // set to ANALYTICS_EVENT_BLOB_VERSION
  AnalyticsEvent event:8;   // type of event
  uint32_t timestamp;
  union PACKED {
    AnalyticsEventBtError bt_error;
    AnalyticsEventAppLaunch app_launch;
    AnalyticsEventPinOpenCreateUpdate pin_open_create_update;
    AnalyticsEventPinAction pin_action;
    AnalyticsEventPinAppLaunch pin_app_launch;
    AnalyticsEventCannedResponse canned_response;
    AnalyticsEventVoiceResponse voice_response;
    AnalyticsEventBtConnectionDisconnection bt_connection_disconnection;
    AnalyticsEventBleDisconnection ble_disconnection;
    AnalyticsEventCrash crash_report;
    AnalyticsEvent_LocalBTDisconnect local_bt_disconnect;
    AnalyticsEvent_AMSData ams;
    AnalyticsEvent_StationaryStateChangeData sd;
    AnalyticsEvent_HealthLegacySleepData health_sleep;
    AnalyticsEvent_HealthLegacyActivityData health_activity;
    AnalyticsEvent_PutByteTimeData pb_time;
    AnalyticsEvent_HealthInsightCreatedData health_insight_created;
    AnalyticsEvent_HealthInsightResponseData health_insight_response;
    AnalyticsEvent_AppCrashData app_crash_report;
    AnalyticsEvent_VibeAccessData vibe_access_data;
    AnalyticsEvent_HealthActivitySessionData health_activity_session;
    AnalyticsEventPebbleProtocolCommonSessionClose pp_common_session_close;
    AnalyticsEventPebbleProtocolSystemSessionClose pp_system_session_close;
    AnalyticsEventPebbleProtocolAppSessionClose pp_app_session_close;
    AnalyticsEvent_AlarmData alarm;
    AnalyticsEvent_BtChipBootData bt_chip_boot;
    AnalyticsEvent_PPoGATTDisconnectData ppogatt_disconnect;
    AnalyticsEvent_GetBytesStatsData get_bytes_stats;
    AnalyticsEvent_AppOomData app_oom;
    AnalyticsEventBleHrmEvent ble_hrm;
  };
} AnalyticsEventBlob;

//! @param type AnalyticsEvent_AppOOMNative or AnalyticsEvent_AppOOMRocky
//! @param total_free Sum of free bytes
//! @param largest_free_block The largest, contiguous, free block of memory.
//! @note Intended to be called from the app/worker task (calls sys_analytics_logging_log_event).
void analytics_event_app_oom(AnalyticsEvent type,
                             uint32_t requested_size, uint32_t total_size,
                             uint32_t total_free, uint32_t largest_free_block);

//! Log an app launch event to analytics
//! @param uuid app's UUID
void analytics_event_app_launch(const Uuid *uuid);

//! Log a pin open event to analytics
//! @param timestamp the UTC timestamp of the pin
//! @param parent_id UUID of the owner of the pin
void analytics_event_pin_open(time_t timestamp, const Uuid *parent_id);

//! Log a generic pin action event (i.e. not an app launch) to analytics
//! @param timestamp the UTC timestamp of the pin
//! @param parent_id UUID of the owner of the pin
//! @param action_title the title of the action
void analytics_event_pin_action(time_t timestamp, const Uuid *parent_id,
                                TimelineItemActionType action_type);

//! Log a pin launch app event to analytics
//! @param timestamp the UTC timestamp of the pin
//! @param parent_id UUID of the owner of the pin
void analytics_event_pin_app_launch(time_t timestamp, const Uuid *parent_id);

//! Log a pin created event to analytics
//! @param timestamp the UTC timestamp of the pin
//! @param parent_id UUID of the owner of the pin
void analytics_event_pin_created(time_t timestamp, const Uuid *parent_id);

//! Log a pin updated event to analytics
//! @param timestamp the UTC timestamp of the pin
//! @param parent_id UUID of the owner of the pin
void analytics_event_pin_updated(time_t timestamp, const Uuid *parent_id);

//! Log a canned response event
//! @param response pointer to response text
//! @param successfully_sent true if successfully sent, false if a failure occurred
void analytics_event_canned_response(const char *response, bool successfully_sent);

//! Log voice transcription event
//! @param event_type event type - must be one of \ref AnalyticsEvent_VoiceTranscriptionAccepted,
//! \ref AnalyticsEvent_VoiceTranscriptionRejected, or
//! \ref AnalyticsEvent_VoiceTranscriptionAutomaticallyAccepted
//! @param response_size_bytes accepted response size in number of bytes
//! @param response_len_chars accepted response length in number of unicode characters
//! @param response_len_ms accepted response time in ms
//! @param error_count number of errors that occurred
//! @param num_sessions number of transcription sessions initiated to get the accepted user response
//! or before the user exited the UI
//! @param app_uuid pointer to app or system UUID
void analytics_event_voice_response(AnalyticsEvent event_type, uint16_t response_size_bytes,
                                    uint16_t response_len_chars, uint32_t response_len_ms,
                                    uint8_t error_count, uint8_t num_sessions, Uuid *app_uuid);

//! Log BLE HRM event
void analytics_event_ble_hrm(BleHrmEventSubtype subtype);

//! Log bluetooth disconnection event
//! @param type - AnalyticsEvent_BtLeConnectionComplete, AnalyticsEvent_BtClassicDisconnect, etc
//! @param reason - The HCI Error code representing the disconnect reason. (See
//!    "OVERVIEW OF ERROR CODES" in BT Core Specification or the HCI_ERROR_CODEs in HCITypes.h
void analytics_event_bt_connection_or_disconnection(AnalyticsEvent type, uint8_t reason);

//! Log bluetooth le disconnection event
//! @param reason - The HCI Error code associated with the disconnect
//! remote_bt_version, remote_bt_company_id & remote_bt_subversion come from the version
//! response received from the remote device
void analytics_event_bt_le_disconnection(uint8_t reason, uint8_t remote_bt_version,
                                         uint16_t remote_bt_company_id,
                                         uint16_t remote_bt_subversion);

//! Log bluetooth error
void analytics_event_bt_error(AnalyticsEvent type, uint32_t error);

//! Log when the CC2564x BT chip becomes unresponsive
void analytics_event_bt_cc2564x_lockup_error(void);

//! Log when app_launch trigger failed.
void analytics_event_bt_app_launch_error(uint8_t gatt_error);

//! Log when a Pebble Protocol session is closed.
void analytics_event_session_close(bool is_system_session, const Uuid *optional_app_uuid,
                                   CommSessionCloseReason reason, uint16_t session_duration_mins);

//! Log crash event to analytics
//! @param crash_code Reboot reason (see RebootReasonCode in reboot_reason.h)
//! @param link_register Last running function before crash
void analytics_event_crash(uint8_t crash_code, uint32_t link_register);

//! Log the reason we disconnect locally as an event. (Mainly interested in
//! seeing how often Bluetopia kills us due to unexpected L2CAP errors)
//! @param handle - the handle being disconnected
//! @param lr - the LR of the function which invoked the disconnect
void analytics_event_local_bt_disconnect(uint16_t conn_handle, uint32_t lr);

//! Log an Apple Media Service event.
//! @param type See AMSAnalyticsEvent
//! @param aux_info Additional information specific to the type of event
void analytics_event_ams(uint8_t type, int32_t aux_info);

void analytics_event_stationary_state_change(time_t timestamp, uint8_t state_change_reason);

//! Log a health insight created event
//! @param timestamp the UTC timestamp of the insight
//! @param insight_type numerical id (from \ref ActivityInsightType enum) of the insight
//! @param pct_tier numerical id (from \ref PercentageTier enum) of the percent tier from
//! average for the metric the insight is about.
void analytics_event_health_insight_created(time_t timestamp,
                                            ActivityInsightType insight_type,
                                            PercentTier pct_tier);

//! Log a health insight response event
//! @param timestamp the UTC timestamp of the insight
//! @param insight_type numerical id (from \ref ActivityInsightType enum) of the insight
//! @param activity_type type of activity, one of ActivitySessionType
//! @param response_id numerical id (from \ref ActivityInsightResponseType enum) of response
void analytics_event_health_insight_response(time_t timestamp, ActivityInsightType insight_type,
                                             ActivitySessionType activity_type,
                                             ActivityInsightResponseType response_id);

//! Tracks duration of time it takes to receive byte transfers over putbytes
//! and statistics on the type of transfer and whether the data stored was valid
//! @param session the session used to transfer the data
//! @param crc_good whether or not the CRC for the blob transferred is valid
//! @param type the PutBytesObjectType that was transferred
//! @param bytes_transferred the number of bytes transferred
//! @param elapsed_time_ms the amount of time spent transmitting the bytes
//! @param conn_events, sync_errors, other_errors LE connection event statistics (if available)
void analytics_event_put_byte_stats(
    CommSession *session, bool crc_good, uint8_t type,
    uint32_t bytes_transferred, uint32_t elapsed_time_ms,
    uint32_t conn_events, uint32_t sync_errors, uint32_t skip_errors, uint32_t other_errors);

//! Log an App Crash event to analytics
//! @param uuid app's UUID
//! @param pc Current running function before crash
//! @param lr Last running function before crash
//! @param build_id Pointer to the build_id buffer of the application (can be NULL)
void analytics_event_app_crash(const Uuid *uuid, uint32_t pc, uint32_t lr, const uint8_t *build_id,
                               bool is_rocky_app);

#if !PLATFORM_TINTIN
//! Log the user's vibration pattern
//! @param VibePatternFeature Notifications, Phone Calls, or Alarms
void analytics_event_vibe_access(VibePatternFeature vibe_feature, VibeScoreId pattern_id);
#endif

typedef struct AlarmInfo AlarmInfo;

//! Sends an analytic event about an alarm event
//! @param even_type The type of alarm analytic event that occurred
//! @param info Information about the alarm
void analytics_event_alarm(AnalyticsEvent event_type, const AlarmInfo *info);

//! Sends an analytic event about an alarm event
//! @param even_type The type of alarm analytic event that occurred
//! @param info Information about the alarm
void analytics_event_bt_chip_boot(uint8_t build_id[BUILD_ID_EXPECTED_LEN],
                                  uint32_t crash_lr, uint32_t reboot_reason_code);

//! Log forced PPoGATT disconnection caused by too many resets
void analytics_event_PPoGATT_disconnect(time_t timestamp, bool successful_reconnect);

//! Log out to analytics stats about a GetBytes transfer
void analytics_event_get_bytes_stats(
    CommSession *session, uint8_t type, uint32_t bytes_transferred, uint32_t elapsed_time_ms,
    uint32_t conn_events, uint32_t sync_errors, uint32_t skip_errors, uint32_t other_errors);
