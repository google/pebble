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

#include "applib/app_comm.h"
#include "applib/app_exit_reason.h"
#include "applib/app_inbox.h"
#include "applib/app_outbox.h"
#include "applib/app_logging.h"
#include "applib/app_timer.h"
#include "applib/app_watch_info.h"
#include "applib/app_worker.h"
#include "applib/bluetooth/ble_client.h"
#include "applib/data_logging.h"
#include "applib/event_service_client.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/window_stack_animation.h"

#include "comm/ble/gap_le_scan.h"

#include "drivers/mag.h"
#include "drivers/rtc.h"

#include "kernel/events.h"
#include "kernel/logging_private.h"
#include "services/normal/wakeup.h"
#include "services/common/analytics/analytics.h"
#include "services/common/analytics/analytics_event.h"
#include "services/common/comm_session/session.h"
#include "services/common/evented_timer.h"
#include "services/normal/activity/activity.h"
#include "services/normal/app_glances/app_glance_service.h"

#include "process_management/pebble_process_info.h"

#include "util/attributes.h"

#include <bluetooth/bluetooth_types.h>

//! @internal
//! Just a dummy syscall that we use in the user mode test app. Remove eventually.
int sys_test(int arg);

time_t sys_get_time(void);
void sys_get_time_ms(time_t *t, uint16_t *out_ms);
RtcTicks sys_get_ticks(void);
struct tm* sys_gmtime_r(const time_t *timep, struct tm *result);
struct tm* sys_localtime_r(const time_t *timep, struct tm *result);
void sys_copy_timezone_abbr(char* timezone_abbr, time_t time);
time_t sys_time_start_of_today(void);


void sys_evented_timer_consume(TimerID timer_id, EventedTimerCallback* out_cb, void** out_cb_data);

void sys_send_pebble_event_to_kernel(PebbleEvent* event);
void sys_current_process_schedule_callback(CallbackEventCallback async_cb, void *ctx);
uint32_t sys_process_events_waiting(PebbleTask task);
void sys_get_pebble_event(PebbleEvent* event);

void sys_pbl_log(LogBinaryMessage* log_message, bool async);

NORETURN sys_app_fault(uint32_t stashed_lr);

bool sys_resource_is_valid(ResAppNum app_num, uint32_t resource_id);
size_t sys_resource_size(ResAppNum app_num, uint32_t handle);
size_t sys_resource_load_range(ResAppNum app_num, uint32_t h, uint32_t start_bytes, uint8_t *buffer, size_t num_bytes);
void sys_resource_mapped_use(void);
void sys_resource_mapped_release(void);

bool sys_resource_bytes_are_readonly(void *bytes);
const uint8_t * sys_resource_read_only_bytes(ResAppNum app_num, uint32_t resource_id,
                                             size_t *num_bytes_out);

void sys_resource_mapped_use(void);
void sys_resource_mapped_release_many(uint32_t num);

uint32_t sys_resource_get_and_cache(ResAppNum app_num, uint32_t resource_id);

NORETURN sys_exit(void);

GFont sys_font_get_system_font(const char *font_key);
void sys_font_reload_font(FontInfo *fontinfo);

bool sys_vibe_pattern_enqueue_step_raw(uint32_t step_duration_ms, int32_t strength);
bool sys_vibe_pattern_enqueue_step(uint32_t step_duration_ms, bool on);
void sys_vibe_pattern_trigger_start(void);
void sys_vibe_pattern_clear(void);
void sys_vibe_history_start_collecting(void);
void sys_vibe_history_stop_collecting(void);
bool sys_vibe_history_was_vibrating(uint64_t time_search);
int32_t sys_vibe_get_vibe_strength(void);


void sys_get_app_uuid(Uuid *uuid);
bool sys_app_is_watchface(void);
AppInstallId sys_app_manager_get_current_app_id(void);
AppInstallId sys_worker_manager_get_current_worker_id(void);

//! Return the resource number of the current context. If the kernel is asking, SYSTEM_APP is
//! returned. If the app is asking, the current bank is returned.
ResAppNum sys_get_current_resource_num(void);

Version sys_get_current_app_sdk_version(void);
PlatformType sys_get_current_app_sdk_platform(void);

bool sys_get_current_app_is_js_allowed(void);
bool sys_get_current_app_is_rocky_app(void);

void sys_app_log(size_t length, void *log_buffer);

void sys_event_service_client_subscribe(EventServiceInfo *handler);
void sys_event_service_client_unsubscribe(EventServiceInfo *state, EventServiceInfo *handler);
void sys_event_service_cleanup(PebbleEvent *e);

int sys_ble_scan_start(void);
int sys_ble_scan_stop(void);
bool sys_ble_scan_is_scanning(void);
bool sys_ble_consume_scan_results(uint8_t *buffer, uint16_t *size_in_out);
int8_t sys_ble_get_advertising_tx_power(void);

BTErrno sys_ble_central_connect(BTDevice device, bool auto_reconnect, bool is_pairing_required);
BTErrno sys_ble_central_cancel_connect(BTDevice device);

BTErrno sys_ble_client_discover_services_and_characteristics(BTDevice device);
uint8_t sys_ble_client_copy_services(BTDeviceInternal device,
                                     BLEService services[], uint8_t num_services);
uint16_t sys_ble_client_get_maximum_value_length(BTDevice device);
BTErrno sys_ble_client_read(BLECharacteristic characteristic);
bool sys_ble_client_get_notification_value_length(uint16_t *value_length_out);
void sys_ble_client_consume_read(uintptr_t object_ref,
                                 uint8_t value_out[],
                                 uint16_t *value_length_in_out);
bool sys_ble_client_consume_notification(uintptr_t *object_ref_out,
                                         uint8_t value_out[],
                                         uint16_t *value_length_in_out,
                                         bool *has_more_out);
BTErrno sys_ble_client_write(BLECharacteristic characteristic,
                             const uint8_t *value,
                             size_t value_length);
BTErrno sys_ble_client_write_without_response(BLECharacteristic characteristic,
                                              const uint8_t *value,
                                              size_t value_length);
BTErrno sys_ble_client_subscribe(BLECharacteristic characteristic,
                                 BLESubscription subscription_type);
BTErrno sys_ble_client_write_descriptor(BLEDescriptor descriptor,
                                        const uint8_t *value,
                                        size_t value_length);
BTErrno sys_ble_client_read_descriptor(BLEDescriptor descriptor);

uint8_t sys_ble_service_get_characteristics(BLEService service,
                                            BLECharacteristic characteristics_out[],
                                            uint8_t num_characteristics);
void sys_ble_service_get_uuid(Uuid *uuid, BLEService service);
void sys_ble_service_get_device(BTDevice *device, BLEService service);
uint8_t sys_ble_service_get_included_services(BLEService service,
                                              BLEService included_services_out[],
                                              uint8_t num_services);

void sys_ble_characteristic_get_uuid(Uuid *uuid, BLECharacteristic characteristic);
BLEAttributeProperty sys_ble_characteristic_get_properties(BLECharacteristic characteristic);
BLEService sys_ble_characteristic_get_service(BLECharacteristic characteristic);
void sys_ble_characteristic_get_device(BTDevice *device, BLECharacteristic characteristic);
uint8_t sys_ble_characteristic_get_descriptors(BLECharacteristic characteristic,
                                               BLEDescriptor descriptors_out[],
                                               uint8_t num_descriptors);

void sys_ble_descriptor_get_uuid(Uuid *uuid, BLEDescriptor descriptor);
BLECharacteristic sys_ble_descriptor_get_characteristic(BLEDescriptor descriptor);

int16_t sys_event_service_get_plugin_service_index(const Uuid * uuid);

DataLoggingSessionRef sys_data_logging_create(uint32_t tag, DataLoggingItemType type,
                                              uint16_t item_size, void *buffer, bool resume);
void sys_data_logging_finish(DataLoggingSessionRef logging_session);
DataLoggingResult sys_data_logging_log(DataLoggingSessionRef logging_session, const void *data, uint32_t num_items);

bool sys_clock_is_24h_style(void);
int sys_strftime(char* s, size_t maxsize, const char* format, const struct tm* tim_p, char *locale);

BatteryChargeState sys_battery_get_charge_state(void);

bool sys_activity_get_metric(ActivityMetric metric, uint32_t history_len, int32_t *history);
bool sys_activity_get_minute_history(HealthMinuteData *minute_data, uint32_t *num_records,
                                     time_t *utc_start);
bool sys_activity_get_step_averages(DayInWeek day_of_week, ActivityMetricAverages *averages);
bool sys_activity_get_sessions(uint32_t *session_entries, ActivitySession *sessions);
bool sys_activity_sessions_is_session_type_ongoing(ActivitySessionType type);
bool sys_activity_prefs_heart_rate_is_enabled(void);

void sys_app_comm_set_responsiveness(SniffInterval interval);
SniffInterval sys_app_comm_get_sniff_interval(void);

void sys_light_enable_interaction(void);
void sys_light_enable(bool enable);
void sys_light_enable_respect_settings(bool enable);
void sys_light_reset_to_timed_mode(void);

bool sys_mobile_app_is_connected_debounced(void);
bool sys_pebblekit_is_connected_debounced(void);


bool sys_app_inbox_service_register(uint8_t *storage, size_t storage_size,
                                    AppInboxMessageHandler message_handler,
                                    AppInboxDroppedHandler dropped_handler);
uint32_t sys_app_inbox_service_unregister(uint8_t *storage);
void sys_app_inbox_service_consume(AppInboxConsumerInfo *consumer_info);

void sys_app_outbox_send(const uint8_t *data, size_t length,
                         AppOutboxSentHandler sent_handler, void *cb_ctx);

void sys_app_pp_app_message_analytics_count_drop(void);

bool sys_app_pp_send_data(CommSession *session, uint16_t endpoint_id,
                          const uint8_t* data, uint16_t length);

CommSession * sys_app_pp_get_comm_session(void);

bool sys_app_pp_has_capability(CommSessionCapability capability);

bool sys_system_pp_has_capability(CommSessionCapability capability);

bool sys_app_glance_update(const Uuid *uuid, const AppGlance *glance);

//! Waits for a certain amount of milliseconds
//! @param millis The number of milliseconds to wait for
void sys_psleep(int millis);

void sys_analytics_set(AnalyticsMetric metric, uint64_t value, AnalyticsClient client);
void sys_analytics_set_entire_array(AnalyticsMetric metric, const void *value, AnalyticsClient client);
void sys_analytics_add(AnalyticsMetric metric, uint64_t increment, AnalyticsClient client);
void sys_analytics_inc(AnalyticsMetric metric, AnalyticsClient client);
void sys_analytics_max(AnalyticsMetric metric, int64_t val, AnalyticsClient client);
void sys_analytics_stopwatch_start(AnalyticsMetric metric, AnalyticsClient client);
void sys_analytics_stopwatch_stop(AnalyticsMetric metric);
void sys_analytics_logging_log_event(AnalyticsEventBlob *event_blob);

bool sys_app_worker_is_running(void);
AppWorkerResult sys_app_worker_launch(void);
AppWorkerResult sys_app_worker_kill(void);
void sys_launch_app_for_worker(void);

WakeupId sys_wakeup_schedule(time_t timestamp, int32_t reason, bool notify_if_missed);
void sys_wakeup_delete(WakeupId wakeup_id);
void sys_wakeup_cancel_all_for_app(void);
time_t sys_wakeup_query(WakeupId wakeup_id);

AppLaunchReason sys_process_get_launch_reason(void);
ButtonId sys_process_get_launch_button(void);
uint32_t sys_process_get_launch_args(void);
AppExitReason sys_process_get_exit_reason(void);
void sys_process_set_exit_reason(AppExitReason exit_reason);
void sys_process_get_wakeup_info(WakeupInfo *info);

//! Get the meta-data for the current process
const PebbleProcessMd* sys_process_manager_get_current_process_md(void);

//! Copy UUID for the current process.
//! @return True if the UUID was successfully copied.
bool sys_process_manager_get_current_process_uuid(Uuid *uuid_out);

//! Get the AppInstallId for the current process
AppInstallId sys_process_manager_get_current_process_id(void);

uint32_t sys_stack_free_bytes(void);

void sys_i18n_get_locale(char *buf);
void sys_i18n_get_with_buffer(const char *string, char *buffer, size_t length);
size_t sys_i18n_get_length(const char *string);

//! @addtogroup Foundation
//! @{
//!   @addtogroup WallTime
//!   @{

//! If timezone is set, copies the current timezone long name (e.g. America/Chicago)
//! to user-provided buffer.
//! @param timezone A pointer to the buffer to copy the timezone long name into
//! @param buffer_size Size of the allocated buffer to copy the timezone long name into
//! @note timezone buffer should be at least TIMEZONE_NAME_LENGTH bytes
void sys_clock_get_timezone(char *timezone, const size_t buffer_size);

//!   @} // end addtogroup WallTime
//! @} // end addtogroup Foundation

//! @addtogroup Foundation
//! @{
//!   @addtogroup WatchInfo
//!   @{

//! Provides the color of the watch.
//! @return {@link WatchInfoColor} representing the color of the watch.
WatchInfoColor sys_watch_info_get_color(void);

//!   @} // end addtogroup WatchInfo
//! @} // end addtogroup Foundation

//! @addtogroup Preferences

//! Users can toggle Quiet Time manually or on schedule. Watchfaces and apps should respect this
//! choice and avoid disturbing actions such as vibration if quiet time is active.
//! @return True, if Quiet Time is currently active.
bool sys_do_not_disturb_is_active(void);
//! @} // end addtogroup Preferences
