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

#include "applib/accel_service.h"
#include "applib/app_launch_button.h"
#include "applib/app_launch_reason.h"
#include "applib/app_outbox.h"
#include "applib/app_smartstrap.h"
#include "applib/bluetooth/ble_client.h"
#include "applib/health_service.h"
#include "applib/plugin_service.h"
#include "applib/tick_timer_service.h"
#include "applib/voice/dictation_session.h"
#include "applib/ui/click.h"
#include "apps/system_apps/app_fetch_ui.h"
#include "drivers/battery.h"
#include "drivers/button_id.h"
#include "process_management/app_install_types.h"
#include "services/common/battery/battery_monitor.h"
#include "services/common/bluetooth/bluetooth_ctl.h"
#include "services/common/comm_session/session_remote_os.h"
#include "services/common/comm_session/session_remote_version.h"
#include "services/common/hrm/hrm_manager.h"
#include "services/common/put_bytes/put_bytes.h"
#include "services/common/touch/touch_event.h"
#include "services/imu/units.h"
#include "services/normal/accessory/smartstrap_profiles.h"
#include "services/normal/blob_db/api.h"
#include "services/normal/music.h"
#include "services/normal/notifications/notifications.h"
#include "services/normal/voice/voice.h"
#include "services/normal/wakeup.h"
#include "services/normal/timeline/peek.h"
#include "services/normal/timeline/reminders.h"
#include "kernel/pebble_tasks.h"
#include "util/attributes.h"

#include "freertos_types.h"
#include "portmacro.h"

#include <bluetooth/bluetooth_types.h>

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef struct PebblePhoneCaller PebblePhoneCaller;

typedef enum {
  PEBBLE_NULL_EVENT = 0,
  PEBBLE_ACCEL_SHAKE_EVENT,
  PEBBLE_ACCEL_DOUBLE_TAP_EVENT,
  PEBBLE_BT_CONNECTION_EVENT,
  PEBBLE_BT_CONNECTION_DEBOUNCED_EVENT,
  PEBBLE_BUTTON_DOWN_EVENT,
  PEBBLE_BUTTON_UP_EVENT,
  //! From kernel to app, ask the app to render itself
  PEBBLE_RENDER_REQUEST_EVENT,
  //! From app to kernel, ask the compositor to render the app
  PEBBLE_RENDER_READY_EVENT,
  //! From kernel to app, notification that render was completed
  PEBBLE_RENDER_FINISHED_EVENT,
  PEBBLE_BATTERY_CONNECTION_EVENT, // TODO: this has a poor name
  PEBBLE_PUT_BYTES_EVENT,
  PEBBLE_BT_PAIRING_EVENT,
  // Emitted when the Pebble mobile app or third party app is (dis)connected
  PEBBLE_COMM_SESSION_EVENT,
  PEBBLE_MEDIA_EVENT,
  PEBBLE_TICK_EVENT,
  PEBBLE_SET_TIME_EVENT,
  PEBBLE_SYS_NOTIFICATION_EVENT,
  PEBBLE_PROCESS_DEINIT_EVENT,
  PEBBLE_PROCESS_KILL_EVENT,
  PEBBLE_PHONE_EVENT,
  PEBBLE_APP_LAUNCH_EVENT,
  PEBBLE_ALARM_CLOCK_EVENT,
  PEBBLE_SYSTEM_MESSAGE_EVENT,
  PEBBLE_FIRMWARE_UPDATE_EVENT,
  PEBBLE_BT_STATE_EVENT,
  PEBBLE_BATTERY_STATE_CHANGE_EVENT,
  PEBBLE_CALLBACK_EVENT,
  PEBBLE_NEW_APP_MESSAGE_EVENT,
  PEBBLE_SUBSCRIPTION_EVENT,
  PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT,
  PEBBLE_APP_DID_CHANGE_FOCUS_EVENT,
  PEBBLE_DO_NOT_DISTURB_EVENT,
  PEBBLE_REMOTE_APP_INFO_EVENT,
  PEBBLE_ECOMPASS_SERVICE_EVENT,
  PEBBLE_COMPASS_DATA_EVENT,
  PEBBLE_PLUGIN_SERVICE_EVENT,
  PEBBLE_WORKER_LAUNCH_EVENT,
  PEBBLE_BLE_SCAN_EVENT,
  PEBBLE_BLE_CONNECTION_EVENT,
  PEBBLE_BLE_GATT_CLIENT_EVENT,
  PEBBLE_BLE_DEVICE_NAME_UPDATED_EVENT,
  PEBBLE_BLE_HRM_SHARING_STATE_UPDATED_EVENT,
  PEBBLE_WAKEUP_EVENT,
  PEBBLE_BLOBDB_EVENT,
  PEBBLE_VOICE_SERVICE_EVENT,
  PEBBLE_DICTATION_EVENT,
  PEBBLE_APP_FETCH_EVENT,
  PEBBLE_APP_FETCH_REQUEST_EVENT,
  PEBBLE_GATHER_DEBUG_INFO_EVENT,
  PEBBLE_REMINDER_EVENT,
  PEBBLE_CALENDAR_EVENT,
  PEBBLE_PANIC_EVENT,
  PEBBLE_SMARTSTRAP_EVENT,
  //! Event sent back to the app to let them know the result of their sent message.
  PEBBLE_APP_OUTBOX_SENT_EVENT,
  //! A request from the app to the outbox service to handle a message.
  //! @note The consuming service must call app_outbox_service_consume_message() to clean up.
  //! In case the event is dropped because the queue is reset, cleanup happens by events.c in
  //! event_queue_cleanup_and_reset().
  PEBBLE_APP_OUTBOX_MSG_EVENT,
  PEBBLE_HEALTH_SERVICE_EVENT,
  PEBBLE_TOUCH_EVENT,
  PEBBLE_CAPABILITIES_CHANGED_EVENT,
  // Emitted when ANCS disconnects or is invalidated
  PEBBLE_ANCS_DISCONNECTED_EVENT,
  PEBBLE_WEATHER_EVENT,
  PEBBLE_HRM_EVENT,
  PEBBLE_UNOBSTRUCTED_AREA_EVENT,
  PEBBLE_APP_GLANCE_EVENT,
  PEBBLE_TIMELINE_PEEK_EVENT,
  PEBBLE_APP_CACHE_EVENT,
  PEBBLE_ACTIVITY_EVENT,
  PEBBLE_WORKOUT_EVENT,

  PEBBLE_NUM_EVENTS
} PebbleEventType;

typedef struct PACKED { // 9 bytes
  AppOutboxSentHandler sent_handler;
  void *cb_ctx;
  AppOutboxStatus status:8;
} PebbleAppOutboxSentEvent;

typedef struct PACKED { // 1 byte
  bool is_active; //<! ANCS has become active or has become inactive
} PebbleAncsChangedEvent;

typedef struct PACKED { // 1 byte
  bool is_active; //<! do not disturb has become active or has become inactive
} PebbleDoNotDisturbEvent;

typedef struct PACKED { // 1 byte?
  ButtonId button_id;
} PebbleButtonEvent;

typedef enum PhoneEventType {
  PhoneEventType_Invalid = 0,
  PhoneEventType_Incoming,
  PhoneEventType_Outgoing,
  PhoneEventType_Missed,
  PhoneEventType_Ring,
  PhoneEventType_Start,
  PhoneEventType_End,
  PhoneEventType_CallerID,
  PhoneEventType_Disconnect,
  PhoneEventType_Hide,
} PhoneEventType;

typedef enum PhoneCallSource {
  PhoneCallSource_PP,
  PhoneCallSource_ANCS_Legacy,
  PhoneCallSource_ANCS,
} PhoneCallSource;

typedef struct PACKED PebblePhoneEvent { // 9 bytes
  PhoneEventType type:6;
  PhoneCallSource source:2;
  uint32_t call_identifier;
  PebblePhoneCaller* caller;
} PebblePhoneEvent;

typedef enum {
  NotificationAdded,
  NotificationActedUpon,
  NotificationRemoved,
  NotificationActionResult
} PebbleSysNotificationType;

typedef struct PACKED { // 6 bytes
  PebbleSysNotificationType type:8;
  union {
    Uuid *notification_id;
    PebbleSysNotificationActionResult *action_result;
    // it won't exceed the 9 bytes
  };
} PebbleSysNotificationEvent;

typedef struct PACKED {   // 4 bytes
  time_t tick_time; //!< Needs to be converted to 'struct tm' in the event service handler
} PebbleTickEvent;

typedef struct PACKED { // 4 bytes
  uint32_t error_code;
} PebblePanicEvent;

typedef enum {
  PebbleMediaEventTypeNowPlayingChanged,
  PebbleMediaEventTypePlaybackStateChanged,
  PebbleMediaEventTypeVolumeChanged,
  PebbleMediaEventTypeServerConnected,
  PebbleMediaEventTypeServerDisconnected,
  PebbleMediaEventTypeTrackPosChanged,
} PebbleMediaEventType;

typedef struct PACKED { // 2 bytes
  PebbleMediaEventType type;
  union {
    MusicPlayState playback_state;
    uint8_t volume_percent;
  };
} PebbleMediaEvent;

typedef enum {
  PebbleBluetoothPairEventTypePairingUserConfirmation,
  PebbleBluetoothPairEventTypePairingComplete,
} PebbleBluetoothPairEventType;

typedef struct PairingUserConfirmationCtx PairingUserConfirmationCtx;

typedef struct {
  char *device_name;
  char *confirmation_token;
} PebbleBluetoothPairingConfirmationInfo;

typedef struct PACKED { // 9 bytes
  const PairingUserConfirmationCtx *ctx;
  union {
    //! Valid if type is PebbleBluetoothPairEventTypePairingUserConfirmation
    PebbleBluetoothPairingConfirmationInfo *confirmation_info;
    //! Valid if type is PebbleBluetoothPairEventTypePairingComplete
    bool success;
  };
  PebbleBluetoothPairEventType type:1;
} PebbleBluetoothPairEvent;

typedef enum {
  PebbleBluetoothConnectionEventStateConnected,
  PebbleBluetoothConnectionEventStateDisconnected,
} PebbleBluetoothConnectionEventState;

  // FIXME: This event muddles classic + LE connection events for the phone.
typedef struct PACKED { // 9 bytes
  PebbleBluetoothConnectionEventState state:1;
  bool is_ble:1;
  BTDeviceInternal device;
} PebbleBluetoothConnectionEvent;

typedef struct PACKED { // 9 bytes
  uint8_t hci_reason;
  BTBondingID bonding_id;
  uint64_t bt_device_bits:50;
  bool connected:1;
} PebbleBLEConnectionEvent;

typedef struct PACKED { // 4 bytes
  int subscription_count;
} PebbleBLEHRMSharingStateUpdatedEvent;

typedef enum {
  PebbleBLEGATTClientEventTypeServiceChange,
  PebbleBLEGATTClientEventTypeCharacteristicRead,
  PebbleBLEGATTClientEventTypeCharacteristicWrite,
  PebbleBLEGATTClientEventTypeCharacteristicSubscribe,
  PebbleBLEGATTClientEventTypeDescriptorRead,
  PebbleBLEGATTClientEventTypeDescriptorWrite,
  PebbleBLEGATTClientEventTypeNotification,
  PebbleBLEGATTClientEventTypeBufferEmpty,
  PebbleBLEGATTClientEventTypeNum,
} PebbleBLEGATTClientEventType;

typedef struct PACKED { // 9 bytes
  uintptr_t object_ref;
  union {
    uint16_t value_length;
    BLESubscription subscription_type:2;
  };
  BLEGATTError gatt_error:16;

  //! This is here to make sure we don't accidentally add more fields here without thinking:
  uint16_t zero:2;
  PebbleBLEGATTClientEventType subtype:6;
} PebbleBLEGATTClientEvent;

#define BLE_GATT_MAX_SERVICES_CHANGED_BITS (5)  // max. 31
#define BLE_GATT_MAX_SERVICES_CHANGED ((1 << BLE_GATT_MAX_SERVICES_CHANGED_BITS) - 1)

#define BLE_GATT_CLIENT_EVENT_SUBTYPE_BITS (3)

typedef enum {
  PebbleServicesRemoved,
  PebbleServicesAdded,
  PebbleServicesInvalidateAll
} PebbleServiceNotificationType;

typedef struct {
  uint8_t num_services_added;
  BLEService services[];
} PebbleBLEGATTClientServicesAdded;

typedef struct {
  BLEService service;
  Uuid uuid;
  uint8_t num_characteristics;
  uint8_t num_descriptors;
  uintptr_t char_and_desc_handles[];
} PebbleBLEGATTClientServiceHandles;

typedef struct {
  uint8_t num_services_removed;
  PebbleBLEGATTClientServiceHandles handles[];
} PebbleBLEGATTClientServicesRemoved;

typedef struct {
  PebbleServiceNotificationType type;
  BTDeviceInternal device;
  BTErrno status;
  union {
    PebbleBLEGATTClientServicesAdded services_added_data;
    PebbleBLEGATTClientServicesRemoved services_removed_data;
  };
} PebbleBLEGATTClientServiceEventInfo;

typedef struct PACKED { // 9 bytes
  PebbleBLEGATTClientServiceEventInfo *info;
  uint64_t rsvd:34;

  uint8_t subtype:BLE_GATT_CLIENT_EVENT_SUBTYPE_BITS;
} PebbleBLEGATTClientServiceEvent;

_Static_assert((1 << BLE_GATT_CLIENT_EVENT_SUBTYPE_BITS) >= PebbleBLEGATTClientEventTypeNum,
               "Not enough bits to represent all PebbleBLEGATTClientEventTypes");

#ifdef __arm__
_Static_assert(sizeof(PebbleBLEGATTClientServiceEvent) == sizeof(PebbleBLEGATTClientServiceEvent),
          "PebbleBLEGATTClientEvent and PebbleBLEGATTClientServiceEvent must be the same size");
#endif

#define PebbleEventToBTDeviceInternal(e) ((const BTDeviceInternal) { \
  .opaque = { \
    .opaque_64 = (e)->bt_device_bits \
  } \
})

typedef struct PACKED { // 3 byte?
  bool airplane;
  bool enabled;
  BtCtlModeOverride override;
} PebbleBluetoothStateEvent;


typedef enum {
  PebblePutBytesEventTypeStart,
  PebblePutBytesEventTypeCleanup,
  PebblePutBytesEventTypeProgress,
  PebblePutBytesEventTypeInitTimeout,
} PebblePutBytesEventType;

typedef struct PACKED { // 8 bytes
  PebblePutBytesEventType type:8;
  uint8_t progress_percent; // the percent complete for the current PB transfer
  PutBytesObjectType object_type:7;
  bool has_cookie:1;
  bool failed;
  union {
    // if type != PebblePutBytesEventTypeCleanup, populated with:
    uint32_t bytes_transferred; // the number of bytes transferred since the last event
    // else populated with:
    uint32_t total_size;
  };
} PebblePutBytesEvent;

typedef struct PACKED { // 1 bytes
  PreciseBatteryChargeState new_state;
} PebbleBatteryStateChangeEvent;

typedef struct PACKED { // 1 byte
  bool is_connected;
} PebbleBatteryConnectionEvent;

typedef struct PACKED { // 5 bytes
  int32_t magnetic_heading;
  uint8_t calib_status;
} PebbleCompassDataEvent;

typedef struct PACKED { // 5 bytes
  IMUCoordinateAxis axis;
  int32_t direction;
} PebbleAccelTapEvent;

//! This is fired when a PP comm session is opened or closed
typedef struct PACKED PebbleCommSessionEvent { // 1 byte
  //! indicates whether the we are connecting or disconnecting
  bool is_open:1;
  //! True if the pebble app has connected & false if a third-party app has connected
  bool is_system:1;
} PebbleCommSessionEvent;

typedef struct PACKED { // 1 bytes
  RemoteOS os;
} PebbleRemoteAppInfoEvent;

typedef enum {
  PebbleSystemMessageFirmwareUpdateStartLegacy,
  PebbleSystemMessageFirmwareUpdateStart,
  PebbleSystemMessageFirmwareUpdateComplete,
  PebbleSystemMessageFirmwareUpdateFailed,
  PebbleSystemMessageFirmwareUpToDate,
  PebbleSystemMessageFirmwareOutOfDate,
} PebbleSystemMessageEventType;

typedef struct PACKED { // 1 byte
  PebbleSystemMessageEventType type;
  uint32_t bytes_transferred;
  uint32_t total_transfer_size;
} PebbleSystemMessageEvent;

//! We need to pass a lot of data to launch an app, more than what would normally fit in a
//! PebbleEvent. This structure is heap allocated whenever we need to launch an app.
typedef struct {
  LaunchConfigCommon common;
  WakeupInfo wakeup;
} PebbleLaunchAppEventExtended;

typedef struct PACKED { // 8 bytes
  AppInstallId id;
  PebbleLaunchAppEventExtended *data;
} PebbleLaunchAppEvent;

typedef struct PACKED { // 8 bytes
  time_t alarm_time; //!< Needs to be converted to 'struct tm' in the event service handler
  const char *alarm_label;
} PebbleAlarmClockEvent;

typedef void (*CallbackEventCallback)(void *data);

typedef struct PACKED { // 8 bytes
  CallbackEventCallback callback;
  void *data;
} PebbleCallbackEvent;

typedef struct PACKED { // 4 bytes
  void* data;
} PebbleNewAppMessageEvent;

typedef struct PACKED { // 7 bytes
  bool subscribe;
  PebbleTask task:8;
  PebbleEventType event_type;
  void *event_queue;
} PebbleSubscriptionEvent;

typedef struct PACKED { // 2 bytes
  bool gracefully;
  PebbleTask task;
} PebbleKillEvent;

typedef struct PACKED { // 1 byte
  bool in_focus;
} PebbleAppFocusEvent;

typedef struct PACKED { // 9 bytes
  uint8_t  type;                // service event type
  uint16_t  service_index;      // service index
  PluginEventData data;
} PebblePluginServiceEvent;

typedef struct PACKED { // 8 bytes
  WakeupInfo wakeup_info;
} PebbleWakeupEvent;

typedef struct PACKED { // 7 bytes
  BlobDBId db_id;
  BlobDBEventType type;
  uint8_t *key;
  uint8_t key_len;
} PebbleBlobDBEvent;

typedef enum {
  VoiceEventTypeSessionSetup,
  VoiceEventTypeSessionResult,
  VoiceEventTypeSilenceDetected,
  VoiceEventTypeSpeechDetected
} VoiceEventType;

typedef struct {
  uint32_t timestamp;
  char sentence[];
} PebbleVoiceServiceEventData;

typedef struct PACKED { // 6 bytes
  VoiceEventType type:8;
  VoiceStatus status:8;
  PebbleVoiceServiceEventData *data;
} PebbleVoiceServiceEvent;

typedef struct PACKED { // 9 bytes
  DictationSessionStatus result;
  time_t timestamp;
  char *text;
} PebbleDictationEvent;

//! Possible results that come back from the INSTALL_COMMAND
typedef enum {
  AppFetchEventTypeStart,
  AppFetchEventTypeProgress,
  AppFetchEventTypeFinish,
  AppFetchEventTypeError,
} AppFetchEventType;

typedef struct PACKED { // 6 bytes
  AppFetchEventType type;
  AppInstallId id;
  union {
    uint8_t progress_percent;
    uint8_t error_code;
  };
} PebbleAppFetchEvent;

typedef struct PACKED { // 9 bytes
  AppInstallId id;
  bool with_ui;
  AppFetchUIArgs *fetch_args; //! NULL when with_ui is false, required otherwise
} PebbleAppFetchRequestEvent;

typedef enum {
  DebugInfoSourceGetBytes,
  DebugInfoSourceFWLogs,
} DebugInfoEventSource;

typedef enum {
  DebugInfoStateStarted,
  DebugInfoStateFinished,
} DebugInfoEventState;

typedef struct PACKED { // 2 bytes
  DebugInfoEventSource source;
  DebugInfoEventState state;
} PebbleGatherDebugInfoEvent;

typedef enum {
  ReminderTriggered,
  ReminderRemoved,
  ReminderUpdated,
} ReminderEventType;

typedef struct PACKED { // 4 bytes
  ReminderEventType type;
  ReminderId *reminder_id;
} PebbleReminderEvent;

typedef struct PACKED { // 9 bytes
  HealthEventData data;
  HealthEventType type:8;     // At the end so that data is word aligned.
} PebbleHealthEvent;

typedef struct PACKED { // 1 byte
  bool is_event_ongoing;
} PebbleCalendarEvent;

typedef enum {
  SmartstrapConnectionEvent,
  SmartstrapDataSentEvent,
  SmartstrapDataReceivedEvent,
  SmartstrapNotifyEvent
} SmartstrapEventType;

typedef struct PACKED { // 9 bytes
  SmartstrapEventType type:4;
  SmartstrapProfile profile:4;
  SmartstrapResult result:8;
  uint16_t read_length;
  union {
    SmartstrapServiceId service_id;
    SmartstrapAttribute *attribute;
  };
} PebbleSmartstrapEvent;

typedef struct PACKED { // 9 bytes
  int utc_time_delta;
  int gmt_offset_delta;
  bool dst_changed;
} PebbleSetTimeEvent;

typedef enum PebbleTouchEventType {
  PebbleTouchEvent_TouchesAvailable,
  PebbleTouchEvent_TouchesCancelled,
  PebbleTouchEvent_PalmDetected
} PebbleTouchEventType;

typedef struct PACKED { // 2 bytes
  PebbleTouchEventType type:8;
  TouchIdx touch_idx;
} PebbleTouchEvent;

typedef struct PACKED { // 8 bytes
  PebbleProtocolCapabilities flags_diff;
} PebbleCapabilitiesChangedEvent;

typedef enum WeatherEventType {
  WeatherEventType_WeatherDataAdded,
  WeatherEventType_WeatherDataRemoved,
  WeatherEventType_WeatherOrderChanged,
} WeatherEventType;

typedef struct PACKED PebbleWeatherEvent {
  WeatherEventType type:8;
} PebbleWeatherEvent;

typedef struct HRMBPMData { // 2 bytes
  uint8_t bpm;
  HRMQuality quality:8;
} HRMBPMData;

typedef struct HRMHRVData { // 3 bytes
  uint16_t ppi_ms; //!< Peak-to-peak interval (ms)
  HRMQuality quality:8;
} HRMHRVData;

typedef struct HRMLEDData { // 4 bytes
  uint16_t current_ua;
  uint16_t tia; //!< Transimpendance Amplifier value.
                //!< This is used with thresholds (provided by AMS) to verify the part is
                //!< functioning within specification.

} HRMLEDData;

typedef struct HRMDiagnosticsData {
  HRMPPGData ppg_data;
  HRMAccelData accel_data;
} HRMDiagnosticsData;

typedef struct HRMSubscriptionExpiringData { // 4 bytes
  HRMSessionRef session_ref;
} HRMSubscriptionExpiringData;

typedef enum HRMEventType {
  HRMEvent_BPM = 0,
  HRMEvent_HRV,
  HRMEvent_LEDCurrent,
  HRMEvent_Diagnostics,
  HRMEvent_SubscriptionExpiring
} HRMEventType;

typedef struct PACKED PebbleHRMEvent { // 5 bytes
  HRMEventType event_type;
  union {
    HRMBPMData bpm;
    HRMHRVData hrv;
    HRMLEDData led;
    HRMDiagnosticsData *debug;
    HRMSubscriptionExpiringData expiring;
  };
} PebbleHRMEvent;

typedef enum UnobstructedAreaEventType {
  UnobstructedAreaEventType_WillChange,
  UnobstructedAreaEventType_Change,
  UnobstructedAreaEventType_DidChange,
} UnobstructedAreaEventType;

typedef struct UnobstructedAreaEventData {
  GRect area;
  GRect final_area; //!< The final unobstructed area. Empty for events other than will-change.
  AnimationProgress progress;
} UnobstructedAreaEventData;

typedef struct PACKED {
  int16_t current_y;
  int16_t final_y;
  AnimationProgress progress;
  UnobstructedAreaEventType type:8; //!< At the end for alignment.
} PebbleUnobstructedAreaEvent;

#if !__clang__
_Static_assert(sizeof(PebbleUnobstructedAreaEvent) == 9,
               "PebbleUnobstructedAreaEvent size mismatch.");
#endif

typedef struct PACKED PebbleAppGlanceEvent {
  Uuid *app_uuid;
} PebbleAppGlanceEvent;

typedef struct PACKED {
  TimelineItemId *item_id;
  TimelinePeekTimeType time_type:8;
  uint8_t num_concurrent;
  bool is_first_event;
  bool is_future_empty;
} PebbleTimelinePeekEvent;

#if !__clang__
_Static_assert(sizeof(PebbleTimelinePeekEvent) == 8,
               "PebbleTimelinePeekEvent size mismatch.");
#endif

typedef enum PebbleAppCacheEventType {
  PebbleAppCacheEvent_Removed,

  PebbleAppCacheEventNum
} PebbleAppCacheEventType;

typedef struct PACKED PebbleAppCacheEvent {
  PebbleAppCacheEventType cache_event_type:8;
  AppInstallId install_id;
} PebbleAppCacheEvent;

#if !__clang__
_Static_assert(sizeof(PebbleAppCacheEvent) == 5,
               "PebbleTimelinePeekEvent size mismatch.");
#endif

typedef enum PebbleActivityEventType {
  PebbleActivityEvent_TrackingStarted,
  PebbleActivityEvent_TrackingStopped,

  PebbleActivityEventNum
} PebbleActivityEventType;

typedef struct PACKED PebbleActivityEvent {
  PebbleActivityEventType type:8;
} PebbleActivityEvent;

typedef enum PebbleWorkoutEventType {
  PebbleWorkoutEvent_Started,
  PebbleWorkoutEvent_Stopped,
  PebbleWorkoutEvent_Paused,
  PebbleWorkoutEvent_FrontendOpened,
  PebbleWorkoutEvent_FrontendClosed,
} PebbleWorkoutEventType;

typedef struct PebbleWorkoutEvent {
  PebbleWorkoutEventType type;
} PebbleWorkoutEvent;


typedef struct PACKED {
  union PACKED {
    PebblePanicEvent panic;
    PebbleButtonEvent button;
    PebbleSysNotificationEvent sys_notification;
    // TODO: kill these old events
    PebbleBatteryStateChangeEvent battery_state;
    PebbleBatteryConnectionEvent battery_connection;
    PebbleSetTimeEvent set_time_info;
    PebbleTickEvent clock_tick;
    PebbleAccelTapEvent accel_tap;
    PebbleCompassDataEvent compass_data;
    PebbleMediaEvent media;
    PebblePutBytesEvent put_bytes;
    PebblePhoneEvent phone;
    PebbleLaunchAppEvent launch_app;
    PebbleSystemMessageEvent firmware_update;
    PebbleAlarmClockEvent alarm_clock;
    PebbleAppOutboxSentEvent app_outbox_sent;
    PebbleCallbackEvent app_outbox_msg;
    union {
      PebbleBluetoothPairEvent pair;
      PebbleBluetoothConnectionEvent connection;
      PebbleCommSessionEvent comm_session_event;
      PebbleBluetoothStateEvent state;
      PebbleRemoteAppInfoEvent app_info_event;
      union {
        PebbleBLEConnectionEvent connection;
        PebbleBLEGATTClientEvent gatt_client;
        PebbleBLEGATTClientServiceEvent gatt_client_service;
        PebbleBLEHRMSharingStateUpdatedEvent hrm_sharing_state;
      } le;
    } bluetooth;
    PebbleAncsChangedEvent ancs_changed;
    PebbleDoNotDisturbEvent do_not_disturb;
    PebbleCallbackEvent callback;
    PebbleNewAppMessageEvent new_app_message;
    PebbleSubscriptionEvent subscription;
    PebbleKillEvent kill;
    PebbleAppFocusEvent app_focus;
    PebbleWakeupEvent wakeup;
    PebblePluginServiceEvent plugin_service;
    PebbleBlobDBEvent blob_db;
    PebbleVoiceServiceEvent voice_service;
    PebbleDictationEvent dictation;
    PebbleAppFetchEvent app_fetch;
    PebbleAppFetchRequestEvent app_fetch_request;
    PebbleGatherDebugInfoEvent debug_info;
    PebbleReminderEvent reminder;
    PebbleCalendarEvent calendar;
    PebbleHealthEvent health_event;
    PebbleSmartstrapEvent smartstrap;
    PebbleTouchEvent touch;
    PebbleCapabilitiesChangedEvent capabilities;
    PebbleWeatherEvent weather;
    PebbleHRMEvent hrm;
    PebbleUnobstructedAreaEvent unobstructed_area;
    PebbleAppGlanceEvent app_glance;
    PebbleTimelinePeekEvent timeline_peek;
    PebbleAppCacheEvent app_cache_event;
    PebbleActivityEvent activity_event;
    PebbleWorkoutEvent workout;
  };
  PebbleTaskBitset task_mask;  // 1 == filter out, 0 == leave in
  // NOTE: we put this 8 bit field at the end so that we can pack this structure and still keep the
  //  event data unions word aligned (and avoid unaligned access exceptions).
  PebbleEventType type:8;
} PebbleEvent;

void events_init(void);

void event_put(PebbleEvent* event);
bool event_put_isr(PebbleEvent* event);
void event_put_from_process(PebbleTask task, PebbleEvent* event);

//! Like event_put_from_app but it's allowed to fail.
bool event_try_put_from_process(PebbleTask task, PebbleEvent* event);

bool event_take_timeout(PebbleEvent* event, int timeout_ms);

//! Return a reference to the allocated buffer within an event, if applicable
void **event_get_buffer(PebbleEvent *event);

//! De-initialize an event, freeing the allocated buffer if necessary
void event_deinit(PebbleEvent *event);

//! Call to clean up after an event that has been dequeued using event_take.
void event_cleanup(PebbleEvent* event);

void event_reset_from_process_queue(PebbleTask task);

//! Get the queue for messaging to the kernel from the given task
QueueHandle_t event_get_to_kernel_queue(PebbleTask task);

QueueHandle_t event_kernel_to_kernel_event_queue(void);

//! Call to reset a queue and free all memory associated w/ the events it contains
BaseType_t event_queue_cleanup_and_reset(QueueHandle_t queue);
