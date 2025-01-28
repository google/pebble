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

#include "hrm_manager.h"
#include "hrm_manager_private.h"

#include "console/prompt.h"
#include "drivers/hrm.h"
#include "kernel/pbl_malloc.h"
#include "mfg/mfg_info.h"
#include "os/tick.h"
#include "process_management/app_manager.h"
#include "process_management/worker_manager.h"
#include "services/common/analytics/analytics.h"
#include "services/common/system_task.h"
#include "syscall/syscall_internal.h"
#include "system/hexdump.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/math.h"
#include "util/size.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <stddef.h>

#define HRM_DEBUG 0

#if HRM_DEBUG
#define HRM_LOG(fmt, ...) \
  do { \
    PBL_LOG(LOG_LEVEL_DEBUG, fmt, ## __VA_ARGS__); \
  } while (0)
#define HRM_HEXDUMP(data, length) \
  do { \
    PBL_HEXDUMP(LOG_LEVEL_DEBUG, (uint8_t *)data, length); \
  } while (0)
#else
#define HRM_LOG(fmt, ...)
#define HRM_HEXDUMP(data, length)
#endif

static struct HRMManagerState s_manager_state;
static bool s_hrm_present = false;

// Forward declarations
static void prv_update_enable_timer_cb(void *context);


static bool prv_match_session_ref(ListNode *found_node, void *data) {
  const HRMSubscriberState *state = (HRMSubscriberState *)found_node;
  return (state->session_ref == (HRMSessionRef)data);
}

T_STATIC HRMSubscriberState * prv_get_subscriber_state_from_ref(HRMSessionRef session) {
  ListNode *node = list_find(s_manager_state.subscribers, prv_match_session_ref,
                             (void *)(uintptr_t)session);
  return (HRMSubscriberState *)node;
}


typedef struct {
  AppInstallId  app_id;
  PebbleTask task;
} HRMAppIdAndTask;

static bool prv_match_app_id(ListNode *found_node, void *data) {
  HRMAppIdAndTask *context = (HRMAppIdAndTask *)data;
  const HRMSubscriberState *state = (HRMSubscriberState *)found_node;
  return ((state->app_id == context->app_id) && (state->task == context->task));
}

T_STATIC HRMSubscriberState * prv_get_subscriber_state_from_app_id(PebbleTask task,
                                                                   AppInstallId app_id) {
  HRMAppIdAndTask context = {
    .app_id = app_id,
    .task = task,
  };

  ListNode *node = list_find(s_manager_state.subscribers, prv_match_app_id, &context);
  return (HRMSubscriberState *)node;
}

// Return true if this subscriber needs to be sent a HRMEvent_SubscriptionExpiring event
static bool prv_needs_expiring_event(HRMSubscriberState *state, time_t utc_now) {
  if (state->sent_expiration_event) {
    return false;
  }
  return (state->expire_utc && (utc_now >= state->expire_utc
      - MAX(HRM_SUBSCRIPTION_EXPIRING_WARNING_SEC, (int)state->update_interval_s)));
}

T_STATIC void prv_read_event_from_buffer_and_consume(CircularBuffer *buffer,
                                                     PebbleHRMEvent *event) {
  const uint16_t total_size = sizeof(*event);
  uint16_t remaining = total_size;
  uint8_t *out_buf = (uint8_t *)event;
  while (remaining > 0) {
    const uint8_t *data_out;
    uint16_t length_out;

    const bool success = circular_buffer_read(buffer, remaining, &data_out, &length_out);
    PBL_ASSERTN(success);
    memcpy(out_buf, data_out, length_out);

    out_buf += length_out;
    remaining -= length_out;
  }
  PBL_ASSERTN(remaining == 0);

  circular_buffer_consume(buffer, sizeof(*event));
}

static void prv_remove_and_free_subscription(HRMSubscriberState *state) {
  list_remove((ListNode *)state, &s_manager_state.subscribers, NULL);
  kernel_free(state);
}

#if UNITTEST
// Used by unit tests
T_STATIC TimerID prv_get_timer_id(void) {
  return s_manager_state.update_enable_timer_id;
}

// Used by unit tests
T_STATIC uint32_t prv_num_system_task_events_queued(void) {
  uint16_t avail_bytes = circular_buffer_get_read_space_remaining(
      &s_manager_state.system_task_event_buffer);
  return avail_bytes / sizeof(PebbleHRMEvent);
}
#endif

static void prv_handle_accel_data(void * data) {
  PBL_ASSERT_RUNNING_FROM_EXPECTED_TASK(PebbleTask_NewTimers);

  uint64_t timestamp_ms;
  uint32_t num_new_samples = sys_accel_manager_get_num_samples(
      s_manager_state.accel_state, &timestamp_ms);

  mutex_lock(s_manager_state.accel_data_lock);

  // Only read as many as we have space to store
  const size_t MAX_BUFFERED_SAMPLES = ARRAY_LENGTH(s_manager_state.accel_data.data);
  if ((s_manager_state.accel_data.num_samples + num_new_samples) > MAX_BUFFERED_SAMPLES) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_HRM_ACCEL_DATA_MISSING, AnalyticsClient_System);
    num_new_samples = MAX_BUFFERED_SAMPLES - s_manager_state.accel_data.num_samples;
  }

  void *write_ptr = &s_manager_state.accel_data.data[s_manager_state.accel_data.num_samples];
  memcpy(write_ptr, s_manager_state.accel_manager_buffer, num_new_samples * sizeof(AccelRawData));

  s_manager_state.accel_data.num_samples += num_new_samples;

  mutex_unlock(s_manager_state.accel_data_lock);

  sys_accel_manager_consume_samples(s_manager_state.accel_state, num_new_samples);
}

// Return true if this is a stable BPM reading. This is called each time we power the sensor off
// or receive a new HRMData update from the sensor driver. It returns true if we should trust the
// BPMData hrm_bpm and hrm_quality fields or not.
//
// In the current rev of the sensor FW, we need to take the following approach to filter out
// good readings:
// 1.) After first turning on the sensor, wait until the quality is "Good" or better, but wait
//     no more than HRM_SENSOR_SPIN_UP_SEC seconds.
// 2.) During sensor startup, the sensor will occasionally send erroneous "Excellent" readings.
//     We can tell they are erroneous because the BPM will be 0. These erroneous readings need to
//     be ignored. We ignore any reading where the BPM is below HRM_SENSOR_MIN_VALID_BPM_READING.
// 3.) Once the quality is "Good", we have to ignore all other quality readings (except off-wrist)
//     because they don't mean anything in this version of the sensor FW.
// 4.) If we suddenly go "off-wrist", wait for another "Good" or better.
//
// So, for the first 0 to HRM_SENSOR_SPIN_UP_SEC seconds after turning the sensor on or first
// contacting the wrist after being off-wrist, the readings can be unstable and this method will
// return false during that time.
//
// @param[in] data pointer to last received HRMdata or NULL if sensor powered off
// @return true if sensor is stable
static bool prv_is_sensor_stable(const HRMData *data) {
  // Passing a NULL data pointer means reset our state
  if (!data) {
    s_manager_state.sensor_stable = false;
    s_manager_state.sensor_start_ticks = 0;
    return false;
  }

  // Ignore the "no accel" quality reading samples. We seem to get these occasionally
  // and don't want them to mess up our state.
  if (data->hrm_quality == HRMQuality_NoAccel) {
    return s_manager_state.sensor_stable;
  }

  // If we were stable before, just make sure we are still stable
  if (s_manager_state.sensor_stable) {
    // If we just went on-wrist or off-wrist, reset the stable state
    bool off_wrist_now = (data->hrm_quality == HRMQuality_OffWrist);
    if (off_wrist_now != s_manager_state.off_wrist_when_stable) {
      s_manager_state.sensor_stable = false;
      s_manager_state.sensor_start_ticks = 0;
      return false;
    }
    return true;
  }

  // Start the tick counter if this is the first reading since power-on or off-wrist
  if (s_manager_state.sensor_start_ticks == 0) {
    s_manager_state.sensor_start_ticks = rtc_get_ticks();
  }

  // When first powering up, we can get "Excellent" quality readings the first few seconds, even
  // though the BPM is 0. Let's fix the quality if the BPM is too low to be valid
  HRMQuality quality = data->hrm_quality;
  if ((data->hrm_bpm < HRM_SENSOR_MIN_VALID_BPM_READING)
      && (data->hrm_quality > HRMQuality_NoSignal)) {
    quality = HRMQuality_NoSignal;
  }

  // Update our state
  if (quality >= HRMQuality_Good) {
    // Once we receive at least one good reading, we are stable
    s_manager_state.sensor_stable = true;
    s_manager_state.off_wrist_when_stable = false;
  } else {
    // We haven't yet received a good reading yet. Wait for a timeout...
    RtcTicks elapsed_ticks = rtc_get_ticks() - s_manager_state.sensor_start_ticks;
    RtcTicks max_startup_time = milliseconds_to_ticks(HRM_SENSOR_SPIN_UP_SEC * MS_PER_SECOND);
    if (elapsed_ticks >= max_startup_time) {
      // If it's been past the tolerable startup time, we have a valid reading - even though it
      // may indicate off-wrist.
      s_manager_state.sensor_stable = true;
      s_manager_state.off_wrist_when_stable = (quality == HRMQuality_OffWrist);
    }
  }

  return s_manager_state.sensor_stable;
}

T_STATIC bool prv_can_turn_sensor_on(void) {
#if IS_BIGBOARD || RECOVERY_FW
  return true;
#endif

  return s_manager_state.enabled_run_level &&
         s_manager_state.enabled_charging_state &&
         activity_prefs_heart_rate_is_enabled();
}

// Figure out if we should enable the HR sensor or not based on all subscribers and their
// desired sampling periods. Must be called from the KernelBG task.
static void prv_update_hrm_enable_system_cb(void *unused) {
  const time_t utc_now = rtc_get_time();
  PBL_ASSERT_TASK(PebbleTask_KernelBackground);
  mutex_lock_recursive(s_manager_state.lock);
  {
    bool turn_sensor_on = false;
    // How many ms until we need the sensor on again. INT32_MAX means we don't need to turn it on
    // again
    int32_t remaining_ms = INT32_MAX;

    if (prv_can_turn_sensor_on()) {
      RtcTicks cur_ticks = rtc_get_ticks();
      int32_t remaining_ticks = INT32_MAX;
      const int32_t spin_up_ticks = (int32_t)milliseconds_to_ticks(
                                             HRM_SENSOR_SPIN_UP_SEC * MS_PER_SECOND);

      // Loop through each of the subscribers and figure out when the next one needs an update
      HRMSubscriberState *state = (HRMSubscriberState *) s_manager_state.subscribers;
      for (; state != NULL; state = (HRMSubscriberState *) state->list_node.next) {
        if (state->expire_utc && (utc_now >= state->expire_utc)) {
          // Ignore expired subscriptions
          continue;
        }
        int64_t subscriber_age_ticks;
        if (state->last_valid_ticks) {
          subscriber_age_ticks = cur_ticks - state->last_valid_ticks;
        } else {
          // Never got an update yet
          subscriber_age_ticks = milliseconds_to_ticks(state->update_interval_s * MS_PER_SECOND);
        }
        int64_t subscriber_remaining_ticks =
            (int64_t)milliseconds_to_ticks(state->update_interval_s * MS_PER_SECOND)
            - subscriber_age_ticks - spin_up_ticks;
        subscriber_remaining_ticks = MAX(0, subscriber_remaining_ticks);

        remaining_ticks = MIN(remaining_ticks, subscriber_remaining_ticks);
      }

      // How many milliseconds till we need to send the next sensor reading
      remaining_ms = ticks_to_milliseconds(remaining_ticks);
      HRM_LOG("Need sensor on again in %"PRIu32" sec", remaining_ms / MS_PER_SECOND);
      turn_sensor_on = (remaining_ms <= 0);
    }

    if (turn_sensor_on && !hrm_is_enabled(HRM)) {
      // Turn on the sensor now
      HRM_LOG("Turning on HR sensor");

      s_manager_state.accel_state = sys_accel_manager_data_subscribe(
          ACCEL_SAMPLING_25HZ, prv_handle_accel_data, NULL, PebbleTask_NewTimers);
      accel_manager_set_jitterfree_sampling_rate(s_manager_state.accel_state,
                                                 HRM_MANAGER_ACCEL_RATE_MILLIHZ);
      sys_accel_manager_set_sample_buffer(
          s_manager_state.accel_state, s_manager_state.accel_manager_buffer,
          HRM_MANAGER_ACCEL_MANAGER_SAMPLES_PER_UPDATE);

      hrm_enable(HRM);

      // Don't need the re-enable timer to fire
      new_timer_stop(s_manager_state.update_enable_timer_id);

    } else if (!turn_sensor_on && hrm_is_enabled(HRM)) {
      // Turn off the sensor now
      HRM_LOG("Turning off HR sensor");
      hrm_disable(HRM);

      sys_accel_manager_data_unsubscribe(s_manager_state.accel_state);
      s_manager_state.accel_state = NULL;

      prv_is_sensor_stable(NULL);  // inform state machine that sensor got powered off

      // If we need the sensor on again later, turn on a timer to re-enable the HRM in enough time
      // to get a good reading for the next subscriber that needs one
      if (remaining_ms < INT32_MAX) {
        new_timer_start(s_manager_state.update_enable_timer_id, remaining_ms,
                        prv_update_enable_timer_cb, NULL /*context*/, 0 /*flags*/);
      } else {
        new_timer_stop(s_manager_state.update_enable_timer_id);
      }
    }
  }
  mutex_unlock_recursive(s_manager_state.lock);
}

// Timer callback that we use to re-enable the HR sensor in case we turned it off for a while
static void prv_update_enable_timer_cb(void *context) {
  system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);
}

//! The system task needs its own handler for HRM data since we can't queue up generic events.
static void prv_system_task_hrm_handler(void *context) {
  time_t utc_now = rtc_get_time();

  mutex_lock_recursive(s_manager_state.lock);
  PebbleHRMEvent event;
  prv_read_event_from_buffer_and_consume(&s_manager_state.system_task_event_buffer,  &event);

  // Send event to all KernelBG subscribers that asked for this feature
  HRMSubscriberState *state = (HRMSubscriberState *)s_manager_state.subscribers;
  for (; state != NULL; state = (HRMSubscriberState *)state->list_node.next) {
    if (!state->callback_handler) {
      // Not a KernelBG subscriber
      continue;
    }

    // If this subscription is ready to expire, send an "expiring" event
    if (prv_needs_expiring_event(state, utc_now)) {
      PebbleHRMEvent expiring_event = (PebbleHRMEvent) {
        .event_type = HRMEvent_SubscriptionExpiring,
        .expiring.session_ref = state->session_ref,
      };
      state->callback_handler(&expiring_event, state->callback_context);
      state->sent_expiration_event = true;
    }

    // See if this subscriber wants these types of events
    switch (event.event_type) {
      case HRMEvent_BPM:
        if (!(state->features & HRMFeature_BPM)) {
          continue;
        }
        break;
      case HRMEvent_LEDCurrent:
        if (!(state->features & HRMFeature_LEDCurrent)) {
          continue;
        }
        break;
      case HRMEvent_HRV:
        if (!(state->features & HRMFeature_HRV)) {
          continue;
        }
        break;
      case HRMEvent_Diagnostics:
        if (!(state->features & HRMFeature_Diagnostics)) {
          continue;
        }
        break;
      case HRMEvent_SubscriptionExpiring:
        continue;
    }

    // Send the event to the subscriber
    state->callback_handler(&event, state->callback_context);
  }
  mutex_unlock_recursive(s_manager_state.lock);
}

// Assumes that s_manager_state.lock is held
static void prv_queue_system_task_event(const PebbleHRMEvent *event) {
  const uint16_t free_space =
      circular_buffer_get_read_space_remaining(&s_manager_state.system_task_event_buffer);
  if (free_space < sizeof(PebbleHRMEvent)) {
    circular_buffer_consume(&s_manager_state.system_task_event_buffer, sizeof(PebbleHRMEvent));
    ++s_manager_state.dropped_events;
  }
  circular_buffer_write(&s_manager_state.system_task_event_buffer,
                        (const uint8_t *)event, sizeof(PebbleHRMEvent));
}

static void prv_populate_hrm_event(PebbleHRMEvent *event, HRMFeature feature, const HRMData *data) {
  switch (feature) {
    case HRMFeature_BPM:
      *event = (PebbleHRMEvent) {
        .event_type = HRMEvent_BPM,
        .bpm = {
          .bpm = data->hrm_bpm,
          .quality = data->hrm_quality,
        },
      };
      break;
    case HRMFeature_HRV:
      *event = (PebbleHRMEvent) {
        .event_type = HRMEvent_HRV,
        .hrv = {
          .ppi_ms = data->hrv_ppi_ms,
          .quality = data->hrv_quality,
        },
      };
      break;
    case HRMFeature_LEDCurrent:
      *event = (PebbleHRMEvent) {
        .event_type = HRMEvent_LEDCurrent,
        .led = {
          .current_ua = data->led_current_ua,
          .tia = data->ppg_data.tia[0]
        },
      };
      break;
    case HRMFeature_Diagnostics:
    {
      HRMDiagnosticsData *debug = kernel_zalloc_check(sizeof(HRMDiagnosticsData));
      debug->ppg_data = data->ppg_data;
      debug->accel_data = data->accel_data;
      *event = (PebbleHRMEvent) {
        .event_type = HRMEvent_Diagnostics,
        .debug = debug,
      };
      break;
    }
    default:
      WTF;
  }
}

static bool prv_event_put(HRMSubscriberState *state, PebbleHRMEvent *event) {
    bool success;
    if (state->queue) {
      PebbleEvent e = {
        .type = PEBBLE_HRM_EVENT,
        .hrm = *event,
      };
      success = xQueueSendToBack(state->queue, &e, 0);
    } else {
      prv_queue_system_task_event(event);
      success = system_task_add_callback(prv_system_task_hrm_handler, NULL);
    }
    return success;
}

T_STATIC void prv_charger_event_cb(PebbleEvent *e, void *context) {
  const PebbleBatteryStateChangeEvent *evt = &e->battery_state;
  mutex_lock_recursive(s_manager_state.lock);
  {
    s_manager_state.enabled_charging_state = !evt->new_state.is_plugged;
  }
  mutex_unlock_recursive(s_manager_state.lock);

  system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);
}

// Accept new data from the HR device driver.
void hrm_manager_new_data_cb(const HRMData *data) {
  mutex_lock_recursive(s_manager_state.lock);
  if (!prv_can_turn_sensor_on() || s_manager_state.subscribers == NULL) {
    // If the hrm manager should be disabled or we have no subscribers, this data is unwanted.
    goto unlock;
  }
  // See if the sensor signal is stable or not
  bool stable_sensor = prv_is_sensor_stable(data);

  HRM_LOG("HRM Data:");
  HRM_LOG("Status %"PRIx8, data->hrm_status);
  HRM_LOG("HRM: %"PRIu8"bpm, Quality: %d, Stable: %d", data->hrm_bpm, data->hrm_quality,
          (int)stable_sensor);
  HRM_LOG("PPG samples: %d", data->ppg_data.num_samples);
  HRM_HEXDUMP(data->ppg_data.ppg, data->ppg_data.num_samples * sizeof(uint16_t));
  HRM_LOG("TIA samples: %d", data->ppg_data.num_samples);
  HRM_HEXDUMP(data->ppg_data.tia, data->ppg_data.num_samples * sizeof(uint16_t));
  HRM_LOG("Accel samples: %"PRIu32, data->accel_data.num_samples);
  HRM_LOG("LED %"PRIu16"uA, TIA: %"PRIu16, data->led_current_ua, data->tia);

  time_t utc_now = rtc_get_time();
  RtcTicks cur_ticks = rtc_get_ticks();
  HRMFeature kernel_bg_features_sent = 0;

  HRMSubscriberState *state = (HRMSubscriberState *)s_manager_state.subscribers;
  while (state) {
    HRMSubscriberState *expired_state = NULL;

    // Update the time stamp for when this subscriber last received an update if the sensor
    // is currently stable
    if (stable_sensor) {
      state->last_valid_ticks = cur_ticks;
    }

    PebbleHRMEvent hrm_event;
    for (uint8_t i = 0; i < HRMFeatureShiftMax; ++i) {
      HRMFeature feature = (1 << i);
      if (!(state->features & feature)) {
        continue;
      }
      // Only send BPM and HRV events if the sensor is stable
      if (!stable_sensor && (feature == HRMFeature_BPM || feature == HRMFeature_HRV)) {
        continue;
      }
      if (state->callback_handler) {
        // For kernel BG subscribers, we only queue one event of each type (which is then
        // dispatched to all KernelBG subscribers from the KernelBG callback) so that we don't
        // overfill our limited size circular buffer.
        if (kernel_bg_features_sent & feature) {
          continue;
        }
        kernel_bg_features_sent |= feature;
      }
      prv_populate_hrm_event(&hrm_event, feature, data);
      PBL_ASSERTN(prv_event_put(state, &hrm_event));
    }

    // If this is an app subscription, see if we need to send an "expiring" event. We check
    // KernelBG subscribers from the system callback function (prv_system_task_hrm_handler).
    if (!state->callback_handler && prv_needs_expiring_event(state, utc_now)) {
      hrm_event = (PebbleHRMEvent) {
        .event_type = HRMEvent_SubscriptionExpiring,
        .expiring.session_ref = state->session_ref,
      };
      PBL_ASSERTN(prv_event_put(state, &hrm_event));
      state->sent_expiration_event = true;
    }

    if (state->expire_utc && (utc_now >= state->expire_utc)) {
      // This subscription has expired
      expired_state = state;
    }
    state = (HRMSubscriberState *)state->list_node.next;

    // If the prior subscription expired, remove it now
    if (expired_state) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Subscription %"PRIu32" expired", expired_state->session_ref);
      prv_remove_and_free_subscription(expired_state);
    }
  }

  // Update the HRM enable state. If no subscribers need an update for a while, we can turn off the
  // HR sensor and set a timer to turn it on again later. To avoid this overhead on every callback,
  // we only check it once every HRM_CHECK_SENSOR_DISABLE_COUNT times
  if (++s_manager_state.check_disable_counter >= HRM_CHECK_SENSOR_DISABLE_COUNT) {
    s_manager_state.check_disable_counter = 0;
    system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);
  }
unlock:
  mutex_unlock_recursive(s_manager_state.lock);
}

void hrm_manager_handle_prefs_changed(void) {
  system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);
}

void hrm_manager_init(void) {
  s_hrm_present = mfg_info_is_hrm_present();
  s_manager_state = (struct HRMManagerState) {
    .lock = mutex_create_recursive(),
    .accel_data_lock = mutex_create(),
    .update_enable_timer_id = new_timer_create(),
    .enabled_charging_state = !battery_is_usb_connected(),
    .charger_subscription = (EventServiceInfo) {
      .type = PEBBLE_BATTERY_STATE_CHANGE_EVENT,
      .handler = prv_charger_event_cb,
    },
  };
  circular_buffer_init(&s_manager_state.system_task_event_buffer,
                       s_manager_state.system_task_event_storage,
                       EVENT_STORAGE_SIZE);
  event_service_client_subscribe(&s_manager_state.charger_subscription);
}

HRMSessionRef hrm_manager_subscribe_with_callback(AppInstallId app_id, uint32_t update_interval_s,
                                                  uint16_t expire_s, HRMFeature features,
                                                  HRMSubscriberCallback callback, void *context) {
  if (!s_hrm_present) {
    return HRM_INVALID_SESSION_REF;
  }

  const PebbleTask current_task = pebble_task_get_current();
  bool is_app_subscription = false;
  if (current_task == PebbleTask_KernelBackground) {
    // KernelBG must provide a callback
    PBL_ASSERTN(callback != NULL);
  } else if (current_task == PebbleTask_KernelMain) {
    // KernelMain clients can either set a callback, or use the event_service interface.
  } else {
    PBL_ASSERTN(current_task == PebbleTask_App || current_task == PebbleTask_Worker);
    is_app_subscription = true;
  }

  mutex_lock_recursive(s_manager_state.lock);
  HRMSessionRef session_ref = HRM_INVALID_SESSION_REF;

  // If there is already an existing subscription for this app, remove the old one before we
  // add another subscription for this app.
  if (is_app_subscription) {
    HRMSubscriberState * state = prv_get_subscriber_state_from_app_id(current_task, app_id);
    if (state != NULL) {
      session_ref = state->session_ref;
      PBL_LOG(LOG_LEVEL_DEBUG, "Removing existing subscription for this app");
      prv_remove_and_free_subscription(state);
    }
  }

  // Get the session ref to use
  if (session_ref == HRM_INVALID_SESSION_REF) {
    session_ref = ++s_manager_state.next_session_ref;
  }

  HRMSubscriberState *state = kernel_malloc_check(sizeof(*state));
  *state = (HRMSubscriberState) {
    .session_ref = session_ref,
    .app_id = app_id,
    .task = current_task,
    .queue = pebble_task_get_to_queue(current_task),
    .callback_handler = callback,
    .callback_context = context,
    .update_interval_s = update_interval_s,
    .expire_utc = (expire_s != 0) ? (rtc_get_time() + expire_s) : 0,
    .features = features,
  };
  s_manager_state.subscribers =
    list_insert_before(s_manager_state.subscribers, &state->list_node);

  // Update the HR enablement state
  system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);

  mutex_unlock_recursive(s_manager_state.lock);
  return state->session_ref;
}

DEFINE_SYSCALL(HRMSessionRef, sys_hrm_manager_app_subscribe,
    AppInstallId app_id, uint32_t update_interval_s, uint16_t expire_sec, HRMFeature features) {
  return hrm_manager_subscribe_with_callback(app_id, update_interval_s, expire_sec, features, NULL,
                                             NULL);
}


DEFINE_SYSCALL(bool, sys_hrm_manager_unsubscribe, HRMSessionRef session) {
  HRM_LOG("Unsubscribing");
  bool success = false;
  mutex_lock_recursive(s_manager_state.lock);

  HRMSubscriberState *state = prv_get_subscriber_state_from_ref(session);
  if (state) {
    prv_remove_and_free_subscription(state);
    system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);
    success = true;
  }

  mutex_unlock_recursive(s_manager_state.lock);
  return success;
}

DEFINE_SYSCALL(HRMSessionRef, sys_hrm_manager_get_app_subscription, AppInstallId app_id) {
  mutex_lock_recursive(s_manager_state.lock);
  HRMSessionRef ref = HRM_INVALID_SESSION_REF;
  HRMSubscriberState *state = prv_get_subscriber_state_from_app_id(pebble_task_get_current(),
                                                                   app_id);
  if (state) {
    ref = state->session_ref;
  }
  mutex_unlock_recursive(s_manager_state.lock);
  return ref;
}


DEFINE_SYSCALL(bool, sys_hrm_manager_get_subscription_info, HRMSessionRef session,
               AppInstallId *app_id, uint32_t *update_interval_s, uint16_t *expire_s,
               HRMFeature *features) {
  mutex_lock_recursive(s_manager_state.lock);
  HRMSubscriberState *state = prv_get_subscriber_state_from_ref(session);
  if (state) {
    if (app_id) {
      *app_id = state->app_id;
    }
    if (update_interval_s) {
      *update_interval_s = state->update_interval_s;
    }
    if (expire_s) {
      int16_t expire_in_s = 0;
      if (state->expire_utc != 0) {
        expire_in_s = MAX(0, state->expire_utc - rtc_get_time());
      }
      *expire_s = MAX(0, expire_in_s);
    }
    if (features) {
      *features = state->features;
    }
  }
  mutex_unlock_recursive(s_manager_state.lock);
  return (state != NULL);
}


DEFINE_SYSCALL(bool, sys_hrm_manager_set_features, HRMSessionRef session, HRMFeature features) {
  bool success = false;
  mutex_lock_recursive(s_manager_state.lock);
  HRMSubscriberState *state = prv_get_subscriber_state_from_ref(session);
  if (state) {
    state->features = features;
    success = true;
  }
  mutex_unlock_recursive(s_manager_state.lock);
  return success;
}

DEFINE_SYSCALL(bool, sys_hrm_manager_set_update_interval, HRMSessionRef session,
               uint32_t update_interval_s, uint16_t expire_s) {
  bool success = false;
  mutex_lock_recursive(s_manager_state.lock);

  HRMSubscriberState *state = prv_get_subscriber_state_from_ref(session);
  if (state) {
    state->update_interval_s = update_interval_s;
    state->expire_utc = (expire_s != 0) ? (rtc_get_time() + expire_s) : 0;
    state->sent_expiration_event = false;
    success = true;
  }
  system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);
  mutex_unlock_recursive(s_manager_state.lock);
  return success;
}

DEFINE_SYSCALL(bool, sys_hrm_manager_is_hrm_present) {
  return s_hrm_present;
}

void hrm_manager_enable(bool on) {
  mutex_lock_recursive(s_manager_state.lock);
  s_manager_state.enabled_run_level = on;
  system_task_add_callback(prv_update_hrm_enable_system_cb, NULL);
  mutex_unlock_recursive(s_manager_state.lock);
}


static HRMSessionRef s_console_session = HRM_INVALID_SESSION_REF;
static uint8_t s_tia_count = 0;
static void prv_console_unsubscribe_callback(void *data) {
  sys_hrm_manager_unsubscribe(s_console_session);
  s_console_session = HRM_INVALID_SESSION_REF;
  prompt_command_finish();
}

static void prv_console_read_callback(PebbleHRMEvent *event, void *context) {
  if (event->event_type == HRMEvent_LEDCurrent) {
    if (s_tia_count++ == 5) { // Need to leave time for TIA to ramp up
      system_task_add_callback(prv_console_unsubscribe_callback, NULL);
      char buf[32];
      prompt_send_response_fmt(buf, 32, "TIA: %"PRIu16, event->led.tia);
      prompt_send_response_fmt(buf, 32, "LED: %"PRIu16"uA", event->led.current_ua);
    }
  }
}

void command_hrm_read(void) {
  s_tia_count = 0;
  sys_hrm_manager_unsubscribe(s_console_session);
  s_console_session = hrm_manager_subscribe_with_callback(
      INSTALL_ID_INVALID, 1 /*update_interval_s*/, 0 /*expire_s*/, HRMFeature_LEDCurrent,
      prv_console_read_callback, NULL);
  prompt_command_continues_after_returning();
}

HRMAccelData * hrm_manager_get_accel_data(void) {
  mutex_lock(s_manager_state.accel_data_lock);
  return &s_manager_state.accel_data;
}

void hrm_manager_release_accel_data(void) {
  s_manager_state.accel_data.num_samples = 0; // Reset buffer
  mutex_unlock(s_manager_state.accel_data_lock);
}

void hrm_manager_process_cleanup(PebbleTask task, AppInstallId app_id) {
  if (task != PebbleTask_App && task != PebbleTask_Worker) {
    return;
  }

  // For apps and workers, if they have a subscription still active, make sure it expires
  HRMSubscriberState *state = prv_get_subscriber_state_from_app_id(task, app_id);
  if (state == NULL) {
    return;
  }

  // Set an expiration time now
  PBL_LOG(LOG_LEVEL_DEBUG, "Setting expiration time on session for app_id %d", (int)app_id);
  sys_hrm_manager_set_update_interval(state->session_ref, state->update_interval_s,
                                      HRM_MANAGER_APP_EXIT_EXPIRATION_SEC);
}
