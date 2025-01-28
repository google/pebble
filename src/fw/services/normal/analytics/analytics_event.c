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

#include <string.h>
#include <inttypes.h>

#include "services/common/analytics/analytics.h"
#include "services/common/analytics/analytics_event.h"
#include "services/common/analytics/analytics_logging.h"
#include "services/common/analytics/analytics_storage.h"

#include "apps/system_apps/launcher/launcher_app.h"
#include "comm/ble/gap_le_connection.h"
#include "comm/bt_lock.h"
#include "comm/ble/gap_le_connection.h"
#include "kernel/pbl_malloc.h"
#include "services/common/comm_session/session_internal.h"
#include "services/normal/alarms/alarm.h"
#include "services/normal/timeline/timeline.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"
#include "util/time/time.h"

_Static_assert(sizeof(AnalyticsEventBlob) == 36,
      "When the blob format or size changes, be sure to bump up ANALYTICS_EVENT_BLOB_VERSION");

// ------------------------------------------------------------------------------------------
// Log an app launch event
static bool prv_send_uuid(AnalyticsEvent event_enum, const Uuid *uuid) {
  if (uuid_is_invalid(uuid) || uuid_is_system(uuid)) {
    // No need to log apps with invalid uuids. This is typically built-in test apps like "Light
    // config" that we don't bother to declare a UUID for
    return false;
  }

  // FIXME: The sdkshell doesn't have a launcher menu so this causes a linker error. Maybe the
  // mapping of events to analytics should also be shell-specific?
#ifndef SHELL_SDK
  // No need to log the launcher menu app
  if (uuid_equal(uuid, &launcher_menu_app_get_app_info()->uuid)) {
    return false;
  }
#endif

  return true;
}

// ------------------------------------------------------------------------------------------
// Log an out-of-memory situation for an app.

void analytics_event_app_oom(AnalyticsEvent type,
                             uint32_t requested_size, uint32_t total_size, uint32_t total_free,
                             uint32_t largest_free_block) {
  PBL_ASSERTN(type == AnalyticsEvent_AppOOMNative || type == AnalyticsEvent_AppOOMRocky);

  AnalyticsEventBlob event_blob = {
    .event = type,
    .app_oom = {
      .requested_size = requested_size,
      .total_size = total_size,
      .total_free = MIN(total_free, UINT16_MAX),
      .largest_free_block = MIN(largest_free_block, UINT16_MAX),
    },
  };
  if (!sys_process_manager_get_current_process_uuid(&event_blob.app_oom.app_uuid)) {
    // Process has no UUID
    return;
  }

#if LOG_DOMAIN_ANALYTICS
  ANALYTICS_LOG_DEBUG("app oom: is_rocky=%u, req_sz=%"PRIu32" tot_sz=%"PRIu32" free=%"PRIu32
                      " max_free=%"PRIu32,
                      (type == AnalyticsEvent_AppOOMRocky),
                      requested_size, total_size, total_free, largest_free_block);
#endif

  sys_analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log a generic app launch event
void analytics_event_app_launch(const Uuid *uuid) {
  if (!prv_send_uuid(AnalyticsEvent_AppLaunch, uuid)) {
    return;
  }

  // Format the event specifc info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_AppLaunch,
    .app_launch.uuid = *uuid,
  };

#if LOG_DOMAIN_ANALYTICS
  char uuid_string[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(uuid, uuid_string);
  ANALYTICS_LOG_DEBUG("app launch event: uuid %s", uuid_string);
#endif

  analytics_logging_log_event(&event_blob);
}


// ------------------------------------------------------------------------------------------
// Log a pin open/create/update event.
static void prv_simple_pin_event(time_t timestamp, const Uuid *parent_id,
                                 AnalyticsEvent event_enum, const char *verb) {
  // Format the event specifc info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields
  AnalyticsEventBlob event_blob = {
    .event = event_enum,
    .pin_open_create_update.time_utc = timestamp,
    .pin_open_create_update.parent_id = *parent_id,
  };

#if LOG_DOMAIN_ANALYTICS
  char uuid_string[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(&event_blob.pin_open_create_update.parent_id, uuid_string);
  ANALYTICS_LOG_DEBUG("pin %s event: timestamp: %"PRIu32", uuid:%s", verb,
                      event_blob.pin_open_create_update.time_utc, uuid_string);
#endif

  analytics_logging_log_event(&event_blob);
}


// ------------------------------------------------------------------------------------------
// Log a pin open event.
void analytics_event_pin_open(time_t timestamp, const Uuid *parent_id) {
  prv_simple_pin_event(timestamp, parent_id, AnalyticsEvent_PinOpen, "open");
}


// ------------------------------------------------------------------------------------------
// Log a pin created event.
void analytics_event_pin_created(time_t timestamp, const Uuid *parent_id) {
  prv_simple_pin_event(timestamp, parent_id, AnalyticsEvent_PinCreated, "created");
}


// ------------------------------------------------------------------------------------------
// Log a pin updated event.
void analytics_event_pin_updated(time_t timestamp, const Uuid *parent_id) {
  prv_simple_pin_event(timestamp, parent_id, AnalyticsEvent_PinUpdated, "updated");
}


// ------------------------------------------------------------------------------------------
// Log a pin action event.
void analytics_event_pin_action(time_t timestamp, const Uuid *parent_id,
                                TimelineItemActionType action_type) {
  // Format the event specifc info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_PinAction,
    .pin_action.time_utc = timestamp,
    .pin_action.parent_id = *parent_id,
    .pin_action.type = action_type,
  };

#if LOG_DOMAIN_ANALYTICS
  char uuid_string[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(&event_blob.pin_action.parent_id, uuid_string);
  ANALYTICS_LOG_DEBUG("pin action event: timestamp: %"PRIu32", uuid:%s, action:%"PRIu8,
                      event_blob.pin_action.time_utc, uuid_string, action_type);
#endif

  analytics_logging_log_event(&event_blob);
}


// ------------------------------------------------------------------------------------------
// Log a pin app launch event.
void analytics_event_pin_app_launch(time_t timestamp, const Uuid *parent_id) {
  if (!prv_send_uuid(AnalyticsEvent_PinAppLaunch, parent_id)) {
    return;
  }

  // Format the event specifc info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_PinAppLaunch,
    .pin_app_launch.time_utc = timestamp,
    .pin_app_launch.parent_id = *parent_id,
  };

#if LOG_DOMAIN_ANALYTICS
  char uuid_string[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(&event_blob.pin_app_launch.parent_id, uuid_string);
  ANALYTICS_LOG_DEBUG("pin app launch event: timestamp: %"PRIu32", uuid:%s",
                      event_blob.pin_app_launch.time_utc, uuid_string);
#endif

  analytics_logging_log_event(&event_blob);
}


// ------------------------------------------------------------------------------------------
// Log a canned response event
void analytics_event_canned_response(const char *response, bool successfully_sent) {
  // Format the event specific info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields
  AnalyticsEventBlob event_blob = {
    .event = successfully_sent ? AnalyticsEvent_CannedResponseSent
                               : AnalyticsEvent_CannedResponseFailed,
  };

  if (!response) {
    event_blob.canned_response.response_size_bytes = 0;
  } else {
    event_blob.canned_response.response_size_bytes = strlen(response);
  }

  if (successfully_sent) {
    ANALYTICS_LOG_DEBUG("canned response sent event: response_size_bytes:%d",
                        event_blob.canned_response.response_size_bytes);
  } else {
    ANALYTICS_LOG_DEBUG("canned response failed event: response_size_bytes:%d",
                        event_blob.canned_response.response_size_bytes);
  }
  analytics_logging_log_event(&event_blob);
}


// ------------------------------------------------------------------------------------------
// Log a voice response event
void analytics_event_voice_response(AnalyticsEvent event_type, uint16_t response_size_bytes,
                                    uint16_t response_len_chars, uint32_t response_len_ms,
                                    uint8_t error_count, uint8_t num_sessions, Uuid *app_uuid) {

  PBL_ASSERTN((event_type >= AnalyticsEvent_VoiceTranscriptionAccepted) &&
      (event_type <= AnalyticsEvent_VoiceTranscriptionAutomaticallyAccepted));

  // Format the event specific info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields

  AnalyticsEventBlob event_blob = {
    .event = event_type,
  };

  event_blob.voice_response = (AnalyticsEventVoiceResponse) {
      .response_size_bytes = response_size_bytes,
      .response_len_chars = response_len_chars,
      .response_len_ms = response_len_ms,
      .num_sessions = num_sessions,
      .error_count = error_count,
      .app_uuid = *app_uuid,
  };

  const char *msg = "Other";
  switch (event_type) {
    case AnalyticsEvent_VoiceTranscriptionAccepted:
      msg = "Accepted";
      break;
    case AnalyticsEvent_VoiceTranscriptionRejected:
      msg = "Rejected";
      break;
    case AnalyticsEvent_VoiceTranscriptionAutomaticallyAccepted:
      msg = "Automatically accepted";
      break;
    default:
      break;
  }

    ANALYTICS_LOG_DEBUG("voice response %s event: size: %"PRIu16"; length (chars): %"PRIu16
        "; length (ms): %"PRIu32"; Errors: %"PRIu8"; Sessions: %"PRIu8, msg,
        event_blob.voice_response.response_size_bytes, event_blob.voice_response.response_len_chars,
        event_blob.voice_response.response_len_ms, event_blob.voice_response.error_count,
        event_blob.voice_response.num_sessions);
  // Use syscall because this is called by voice_window
  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log a BLE HRM event
void analytics_event_ble_hrm(BleHrmEventSubtype subtype) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_BleHrmEvent,
    .ble_hrm = {
      .subtype = subtype,
    },
  };

  ANALYTICS_LOG_DEBUG("BLE HRM Event %u", subtype);

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log a bluetooth disconnection event
void analytics_event_bt_connection_or_disconnection(AnalyticsEvent type, uint8_t reason) {
  AnalyticsEventBlob event_blob = {
    .event = type,
  };

  event_blob.bt_connection_disconnection.reason = reason;

  ANALYTICS_LOG_DEBUG("Event %d - BT (dis)connection: Reason: %"PRIu8,
                      event_blob.event,
                      event_blob.bt_connection_disconnection.reason);

  analytics_logging_log_event(&event_blob);
}

void analytics_event_bt_le_disconnection(uint8_t reason, uint8_t remote_bt_version,
                                         uint16_t remote_bt_company_id,
                                         uint16_t remote_bt_subversion) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_BtLeDisconnect,
    .ble_disconnection = {
      .reason = reason,
      .remote_bt_version = remote_bt_version,
      .remote_bt_company_id = remote_bt_company_id,
      .remote_bt_subversion_number = remote_bt_subversion,
    }
  };

  ANALYTICS_LOG_DEBUG("Event %d - BT disconnection: Reason: %"PRIu8, event_blob.event,
                      event_blob.bt_connection_disconnection.reason);

  analytics_logging_log_event(&event_blob);
}


// ------------------------------------------------------------------------------------------
// Log a bluetooth error
void analytics_event_bt_error(AnalyticsEvent type, uint32_t error) {
  AnalyticsEventBlob event_blob = {};
  event_blob.event = type,
  event_blob.bt_error.error_code = error;

  ANALYTICS_LOG_DEBUG("bluetooth event %d - error: %"PRIu32,
                      event_blob.event,
                      event_blob.bt_error.error_code);

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
//! Log when app_launch trigger failed.
void analytics_event_bt_app_launch_error(uint8_t gatt_error) {
  analytics_event_bt_error(AnalyticsEvent_BtAppLaunchError, gatt_error);
}

// ------------------------------------------------------------------------------------------
//! Log when a Pebble Protocol session is closed.
void analytics_event_session_close(bool is_system_session, const Uuid *optional_app_uuid,
                                   CommSessionCloseReason reason, uint16_t session_duration_mins) {
  AnalyticsEventBlob event_blob = {};
  event_blob.event = (is_system_session ? AnalyticsEvent_PebbleProtocolSystemSessionEnd :
                                          AnalyticsEvent_PebbleProtocolAppSessionEnd);
  event_blob.pp_common_session_close.close_reason = reason;
  event_blob.pp_common_session_close.duration_minutes = session_duration_mins;

  if (!is_system_session && optional_app_uuid) {
    memcpy(&event_blob.pp_app_session_close.app_uuid, optional_app_uuid, sizeof(Uuid));
  }

#if LOG_DOMAIN_ANALYTICS
  char uuid_str[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(optional_app_uuid, uuid_str);
  ANALYTICS_LOG_DEBUG("Session close event. is_system_session=%u, uuid=%s, "
                      "reason=%u, duration_mins=%"PRIu16,
                      is_system_session, uuid_str, reason, session_duration_mins);
#endif

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
//! Log when the CC2564x BT chip becomes unresponsive
void analytics_event_bt_cc2564x_lockup_error(void) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_BtLockupError,
  };

  ANALYTICS_LOG_DEBUG("CC2564x lockup event");

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log a crash event
void analytics_event_crash(uint8_t crash_code, uint32_t link_register) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_Crash,
    .crash_report.crash_code = crash_code,
    .crash_report.link_register = link_register
  };

  ANALYTICS_LOG_DEBUG("Crash occurred: Code %"PRIu8" / LR: %"PRIu32,
    event_blob.crash_report.crash_code, event_blob.crash_report.link_register);

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log local bluetooth disconnection reason
void analytics_event_local_bt_disconnect(uint16_t conn_handle, uint32_t lr) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_LocalBtDisconnect,
  };

  event_blob.local_bt_disconnect.lr = lr;
  event_blob.local_bt_disconnect.conn_handle = conn_handle;

  ANALYTICS_LOG_DEBUG("Event %d - BT Disconnect: Handle:%"PRIu16" LR: %"PRIu32,
                      event_blob.event,
                      conn_handle,
                      lr);
  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log an Apple Media Service event.
void analytics_event_ams(uint8_t type, int32_t aux_info) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_BtLeAMS,
    .ams = {
      .type = type,
      .aux_info = aux_info,
    },
  };

  ANALYTICS_LOG_DEBUG("Event %d - AMS: type:%d aux_info: %"PRId32, event_blob.event, type, aux_info);
  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log stationary mode events
void analytics_event_stationary_state_change(time_t timestamp, uint8_t state_change) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_StationaryModeSwitch,
    .sd = {
      .timestamp = timestamp,
      .state_change = state_change,
    }
  };

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log a health insight created event.
void analytics_event_health_insight_created(time_t timestamp,
                                            ActivityInsightType insight_type,
                                            PercentTier pct_tier) {
  // Format the event specifc info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_HealthInsightCreated,
    .health_insight_created = {
      .time_utc = timestamp,
      .insight_type = insight_type,
      .percent_tier = pct_tier,
    }
  };

#if LOG_DOMAIN_ANALYTICS
  ANALYTICS_LOG_DEBUG("health insight created event: timestamp: %"PRIu32", type:%"PRIu8,
                      timestamp, insight_type);
#endif

  analytics_logging_log_event(&event_blob);
}


// ------------------------------------------------------------------------------------------
// Log a health insight response event.
void analytics_event_health_insight_response(time_t timestamp, ActivityInsightType insight_type,
                                             ActivitySessionType activity_type,
                                             ActivityInsightResponseType response_id) {
  // Format the event specifc info in the blob. The analytics_logging_log_event() method will fill
  // in the common fields
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_HealthInsightResponse,
    .health_insight_response = {
      .time_utc = timestamp,
      .insight_type = insight_type,
      .activity_type = activity_type,
      .response_id = response_id,
    }
  };

#if LOG_DOMAIN_ANALYTICS
  ANALYTICS_LOG_DEBUG("health insight response event: timestamp: %"PRIu32", type:%"PRIu8 \
                      ", response:%"PRIu8, timestamp, insight_type, response_id);
#endif

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
// Log an App Crash event
void analytics_event_app_crash(const Uuid *uuid, uint32_t pc, uint32_t lr,
                               const uint8_t *build_id, bool is_rocky_app) {
  AnalyticsEventBlob event_blob = {
    .event = (is_rocky_app ? AnalyticsEvent_RockyAppCrash : AnalyticsEvent_AppCrash),
    .app_crash_report = {
      .uuid = *uuid,
      .pc = pc,
      .lr = lr,
    },
  };

  if (build_id) {
    memcpy(event_blob.app_crash_report.build_id_slice, build_id,
           sizeof(event_blob.app_crash_report.build_id_slice));
  }

#if LOG_DOMAIN_ANALYTICS
  char uuid_string[UUID_STRING_BUFFER_LENGTH];
  uuid_to_string(uuid, uuid_string);
  ANALYTICS_LOG_DEBUG("App Crash event: uuid:%s, pc: %p, lr: %p",
                      uuid_string, (void *)pc, (void *)lr);
#endif

  analytics_logging_log_event(&event_blob);
}

extern bool comm_session_is_valid(const CommSession *session);

// ------------------------------------------------------------------------------------------
static bool prv_get_connection_details(CommSession *session, bool *is_ppogatt,
                                       uint16_t *conn_interval) {
  bt_lock();

  if (!session || !comm_session_is_valid(session)) {
    bt_unlock();
    return false;
  }

  const bool tmp_is_ppogatt =
      (comm_session_analytics_get_transport_type(session) == CommSessionTransportType_PPoGATT);

  uint16_t tmp_conn_interval = 0;
  if (tmp_is_ppogatt) {
    GAPLEConnection *conn = gap_le_connection_get_gateway();
    if (conn) {
      tmp_conn_interval = conn->conn_params.conn_interval_1_25ms;
    }
  }

  bt_unlock();

  if (is_ppogatt) {
    *is_ppogatt = tmp_is_ppogatt;
  }
  if (conn_interval) {
    *conn_interval = tmp_conn_interval;
  }
  return true;
}

void analytics_event_put_byte_stats(
    CommSession *session, bool crc_good, uint8_t type,
    uint32_t bytes_transferred, uint32_t elapsed_time_ms,
    uint32_t conn_events, uint32_t sync_errors, uint32_t skip_errors, uint32_t other_errors) {

  bool is_ppogatt = false;
  uint16_t conn_interval = 0;
  if (!prv_get_connection_details(session, &is_ppogatt, &conn_interval)) {
    return;
  }

  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_PutByteTime,
    .pb_time = {
      .ppogatt = is_ppogatt,
      .conn_intvl_1_25ms = MIN(conn_interval, UINT8_MAX),
      .crc_good = crc_good,
      .type = type,
      .bytes_transferred = bytes_transferred,
      .elapsed_time_ms = elapsed_time_ms,
      .conn_events = MIN(conn_events, UINT32_MAX),
      .sync_errors = MIN(sync_errors, UINT16_MAX),
      .skip_errors = MIN(skip_errors, UINT16_MAX),
      .other_errors = MIN(other_errors, UINT16_MAX),
    },
  };

  ANALYTICS_LOG_DEBUG("PutBytes event: is_ppogatt: %d, bytes: %d, time ms: %d",
                      (int)event_blob.pb_time.ppogatt,
                      (int)event_blob.pb_time.bytes_transferred,
                      (int)event_blob.pb_time.elapsed_time_ms);

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
#if !PLATFORM_TINTIN
void analytics_event_vibe_access(VibePatternFeature vibe_feature, VibeScoreId pattern_id) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_VibeAccess,
    . vibe_access_data = {
      .feature = (uint8_t) vibe_feature,
      .vibe_pattern_id = (uint8_t) pattern_id
    }
  };

  analytics_logging_log_event(&event_blob);
}
#endif

// ------------------------------------------------------------------------------------------
void analytics_event_alarm(AnalyticsEvent event_type, const AlarmInfo *info) {
  AnalyticsEventBlob event_blob = {
    .event = event_type,
    .alarm = {
      .hour = info->hour,
      .minute = info->minute,
      .is_smart = info->is_smart,
      .kind = info->kind,
    },
  };

  memcpy(event_blob.alarm.scheduled_days, info->scheduled_days,
         sizeof(event_blob.alarm.scheduled_days));

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
void analytics_event_bt_chip_boot(uint8_t build_id[BUILD_ID_EXPECTED_LEN],
                                  uint32_t crash_lr, uint32_t reboot_reason_code) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_BtChipBoot,
    .bt_chip_boot = {
      .crash_lr = crash_lr,
      .reboot_reason = reboot_reason_code,
    },
  };

  memcpy(event_blob.bt_chip_boot.build_id, build_id, sizeof(BUILD_ID_EXPECTED_LEN));

  ANALYTICS_LOG_DEBUG("BtChipBoot event: crash_lr: 0x%x, reboot_reason: %"PRIu32,
                      (int)event_blob.bt_chip_boot.crash_lr,
                      event_blob.bt_chip_boot.reboot_reason);

  analytics_logging_log_event(&event_blob);
}

// ------------------------------------------------------------------------------------------
void analytics_event_PPoGATT_disconnect(time_t timestamp, bool successful_reconnect) {
  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_PPoGATTDisconnect,
    .ppogatt_disconnect = {
      .successful_reconnect = successful_reconnect,
      .time_utc = timestamp,
    },
  };
  analytics_logging_log_event(&event_blob);
}


void analytics_event_get_bytes_stats(CommSession *session, uint8_t type,
                                     uint32_t bytes_transferred, uint32_t elapsed_time_ms,
                                     uint32_t conn_events, uint32_t sync_errors,
                                     uint32_t skip_errors, uint32_t other_errors) {
  bool is_ppogatt = false;
  uint16_t conn_interval = 0;
  if (!prv_get_connection_details(session, &is_ppogatt, &conn_interval)) {
    return;
  }

  AnalyticsEventBlob event_blob = {
    .event = AnalyticsEvent_GetBytesStats,
    .get_bytes_stats = {
      .ppogatt = is_ppogatt,
      .conn_intvl_1_25ms = MIN(conn_interval, UINT8_MAX),
      .type = type,
      .bytes_transferred = bytes_transferred,
      .elapsed_time_ms = elapsed_time_ms,
      .conn_events = conn_events,
      .sync_errors = MIN(sync_errors, UINT16_MAX),
      .skip_errors = MIN(skip_errors, UINT16_MAX),
      .other_errors = MIN(other_errors, UINT16_MAX),
    },
  };

  ANALYTICS_LOG_DEBUG("GetBytesStats event: type: 0x%x, num_bytes: %"PRIu32", elapsed_ms: %"PRIu32,
                      type, bytes_transferred, elapsed_time_ms);

  analytics_logging_log_event(&event_blob);
}
