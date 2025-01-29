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

#include "accel_manager.h"

#include "console/prompt.h"
#include "drivers/accel.h"
#include "drivers/vibe.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "mcu/interrupts.h"
#include "os/mutex.h"
#include "services/common/analytics/analytics.h"
#include "services/common/event_service.h"
#include "services/common/system_task.h"
#include "services/imu/units.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/shared_circular_buffer.h"

#include "FreeRTOS.h"
#include "queue.h"

#include <inttypes.h>

// We use this as an argument to indicate a lookup of the current task
#define PEBBLE_TASK_CURRENT PebbleTask_Unknown

#define US_PER_SECOND (1000 * 1000)

typedef void (*ProcessDataHandler)(CallbackEventCallback *cb, void *data);

// We create one of these for each data service subscriber
typedef struct AccelManagerState {
  ListNode list_node;                       // Entry into the s_data_subscribers linked list

  //! Client pointing into s_buffer
  SubsampledSharedCircularBufferClient buffer_client;
  //! The sampling interval we've promised to this client after subsampling.
  uint32_t sampling_interval_us;
  //! The requested number of samples needed before calling data_cb_handler
  uint16_t samples_per_update;

  //! Which task we should call the data_cb_handler on
  PebbleTask task;
  CallbackEventCallback data_cb_handler;
  void*                 data_cb_context;

  uint64_t              timestamp_ms;      // timestamp of first item in the buffer
  AccelRawData          *raw_buffer;       // raw buffer allocated by subscriber
  uint8_t               num_samples;       // number of samples in raw_buffer
  bool                  event_posted;      // True if we've posted a "data ready" callback event
} AccelManagerState;

typedef struct {
  AccelRawData rawdata;
  // The exact time the sample was collected can be recovered by:
  //   time_sample_collected = s_last_empty_timestamp_ms + timestamp_delta_ms
  uint16_t timestamp_delta_ms;
} AccelManagerBufferData;
_Static_assert(offsetof(AccelManagerBufferData, rawdata) == 0,
    "AccelRawData must be first entry in AccelManagerBufferData struct");

// Statics
//! List of all registered consumers of accel data. Points to AccelManagerState objects.
static ListNode *s_data_subscribers = NULL;
//! Mutex locking all accel_manager state
static PebbleRecursiveMutex *s_accel_manager_mutex;

//! Reference count of how many shake subscribers we have. Used to turn off the feature when not
//! in use.
static uint8_t s_shake_subscribers_count = 0;
//! Reference count of how many double tap subscribers we have. Used to turn off the feature when
//! not in use.
static uint8_t s_double_tap_subscribers_count = 0;

//! Circular buffer that raw accel data is written into before being subsampled for each client
static SharedCircularBuffer s_buffer;
//! Storage for s_buffer
//! 1600 bytes (~4s of data at 50Hz)
static uint8_t s_buffer_storage[200 * sizeof(AccelManagerBufferData)];

static uint64_t s_last_empty_timestamp_ms = 0;

static uint32_t s_accel_samples_collected_count = 0;

// Accel Idle
#define ACCEL_MAX_IDLE_DELTA 100
static bool s_is_idle = false;
static AccelData s_last_analytics_position;
static AccelData s_last_accel_data;

static void prv_setup_subsampling(uint32_t sampling_interval);

static void prv_shake_add_subscriber_cb(PebbleTask task) {
  mutex_lock_recursive(s_accel_manager_mutex);
  {
    if (++s_shake_subscribers_count == 1) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Starting accel shake service");
      accel_enable_shake_detection(true);
      prv_setup_subsampling(accel_get_sampling_interval());
    }
  }
  mutex_unlock_recursive(s_accel_manager_mutex);
}

static void prv_shake_remove_subscriber_cb(PebbleTask task) {
  mutex_lock_recursive(s_accel_manager_mutex);
  {
    PBL_ASSERTN(s_shake_subscribers_count > 0);
    if (--s_shake_subscribers_count == 0) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Stopping accel shake service");
      accel_enable_shake_detection(false);
      prv_setup_subsampling(accel_get_sampling_interval());
    }
  }
  mutex_unlock_recursive(s_accel_manager_mutex);
}

static void prv_double_tap_add_subscriber_cb(PebbleTask task) {
  mutex_lock_recursive(s_accel_manager_mutex);

  if (++s_double_tap_subscribers_count == 1) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Starting accel double tap service");
    accel_enable_double_tap_detection(true);
    prv_setup_subsampling(accel_get_sampling_interval());
  }

  mutex_unlock_recursive(s_accel_manager_mutex);
}

static void prv_double_tap_remove_subscriber_cb(PebbleTask task) {
  mutex_lock_recursive(s_accel_manager_mutex);

  PBL_ASSERTN(s_double_tap_subscribers_count > 0);
  if (--s_double_tap_subscribers_count == 0) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Stopping accel double tap service");
    accel_enable_double_tap_detection(false);
    prv_setup_subsampling(accel_get_sampling_interval());
  }

  mutex_unlock_recursive(s_accel_manager_mutex);
}


//! Out of all accel subscribers, figures out:
//! @param[out] lowest_interval_us - the lowest sampling interval requested (in microseconds)
//! @param[out] max_n_samples - the max number of samples requested for batching
//! @return The longest amount of samples which can be batched assuming we are
//!   running at the lowest_sampling_interval
//!
//! @note currently the longest interval we can batch samples for is computed
//! as the minimum of (samples to batch / sample rate) out of all the active
//! subscribers. This means that if we have two subscribers, subscriber A at
//! 200ms, and subscriber B at 250ms, new samples will become available every
//! 200ms, so subscriber B's data buffer would not fill until 400ms, resulting
//! in a 150ms latency. This is how the legacy implementation worked as well
//! but is potentially something we could improve in the future if it becomes
//! a problem.
static uint32_t prv_get_sample_interval_info(uint32_t *lowest_interval_us,
                                             uint32_t *max_n_samples) {
  *lowest_interval_us = (US_PER_SECOND / ACCEL_SAMPLING_10HZ);
  *max_n_samples = 0;
  // Tracks which subscriber wants data most frequently. Note this is different than just
  // lowest_interval_us * max_n_samples as those values can come from 2 different subscribers
  // where we want to know which one subscriber wants the highest update frequency.
  uint32_t lowest_us_per_update = UINT32_MAX;

  AccelManagerState *state = (AccelManagerState *)s_data_subscribers;
  while (state) {
    *lowest_interval_us = MIN(state->sampling_interval_us, *lowest_interval_us);
    *max_n_samples = MAX(state->samples_per_update, *max_n_samples);

    if (state->samples_per_update > 0) {
      uint32_t us_per_update = state->samples_per_update * state->sampling_interval_us;
      lowest_us_per_update = MIN(lowest_us_per_update, us_per_update);
    }
    state = (AccelManagerState *)state->list_node.next;
  }

  if (lowest_us_per_update == UINT32_MAX) {
    // No one subscribing or no one who wants updates
    return 0;
  }

  uint32_t num_samples = lowest_us_per_update / (*lowest_interval_us);
  num_samples = MIN(num_samples, ACCEL_MAX_SAMPLES_PER_UPDATE);

  return num_samples;
}

static void prv_setup_subsampling(uint32_t sampling_interval) {
  // Setup the subsampling numerator and denominators
  AccelManagerState *state = (AccelManagerState *)s_data_subscribers;
  while (state) {
    uint32_t interval_gcd = gcd(sampling_interval,
                                state->sampling_interval_us);
    uint16_t numerator = sampling_interval / interval_gcd;
    uint16_t denominator = state->sampling_interval_us / interval_gcd;

    PBL_LOG(LOG_LEVEL_DEBUG,
            "set subsampling for session %p to %" PRIu16 "/%" PRIu16,
            state, numerator, denominator);
    subsampled_shared_circular_buffer_client_set_ratio(
        &state->buffer_client, numerator, denominator);
    state = (AccelManagerState *)state->list_node.next;
  }
}

//! Should be called after any change to a subscriber. Handles re-configuring
//! the accel driver to satisfy the requirements of all consumers (i.e setting
//! sampling rate and max number of samples which can be batched). If there are no
//! subscribers, chooses the lowest power configuration settings
static void prv_update_driver_config(void) {
  // TODO: Add low power support
  uint32_t lowest_interval_us;
  uint32_t max_n_samples;
  uint32_t max_batch = prv_get_sample_interval_info(&lowest_interval_us, &max_n_samples);

  // Configure the driver sampling interval and get the actual interval that the driver is going
  // to use.
  uint32_t interval_us = accel_set_sampling_interval(lowest_interval_us);

  prv_setup_subsampling(interval_us);

  PBL_LOG(LOG_LEVEL_DEBUG, "setting accel rate:%"PRIu32", num_samples:%"PRIu32,
          US_PER_SECOND / interval_us, max_batch);

  accel_set_num_samples(max_batch);
}

static bool prv_call_data_callback(AccelManagerState *state) {
  switch (state->task) {
    case PebbleTask_App:
    case PebbleTask_Worker:
    case PebbleTask_KernelMain: {
      PebbleEvent event = {
        .type = PEBBLE_CALLBACK_EVENT,
        .callback = {
          .callback = state->data_cb_handler,
          .data = state->data_cb_context,
        },
      };

      QueueHandle_t queue = pebble_task_get_to_queue(state->task);
      // Note: This call may fail if the queue is full but when a new sample
      // becomes available from the driver, we will retry anyway
      return xQueueSendToBack(queue, &event, 0);
    }
    case PebbleTask_KernelBackground:
      return system_task_add_callback(state->data_cb_handler, state->data_cb_context);
    case PebbleTask_NewTimers:
      return new_timer_add_work_callback(state->data_cb_handler, state->data_cb_context);
    default:
      WTF; // Unsupported task for the accel manager
  }
}

//! This is called every time new samples arrive from the accel driver & every
//! time data has been drained by the accel service. Its responsibility is
//! populating subscriber storage with new samples (at the requested sample
//! frequency) and generating a callback event on the subscriber's queue when
//! the requested number of samples have been batched
static void prv_dispatch_data(void) {
  mutex_lock_recursive(s_accel_manager_mutex);

  AccelManagerState * state = (AccelManagerState *)s_data_subscribers;
  while (state) {
    if (!state->raw_buffer) {
      state = (AccelManagerState *)state->list_node.next;
      continue;
    }

    // if subscribed but not looking for any samples then just drop the data
    if (state->samples_per_update == 0) {
      uint16_t len = shared_circular_buffer_get_read_space_remaining(
          &s_buffer, &state->buffer_client.buffer_client);
      shared_circular_buffer_consume(
          &s_buffer, &state->buffer_client.buffer_client, len);
      state = (AccelManagerState *)state->list_node.next;
      continue;
    }

    // If buffer has room, read more data
    uint32_t samples_drained = 0;
    while (state->num_samples < state->samples_per_update) {
      // Read available data.
      AccelManagerBufferData data;
      if (!shared_circular_buffer_read_subsampled(
          &s_buffer, &state->buffer_client, sizeof(data), &data, 1)) {
        // we have drained all available samples
        break;
      }

      // Note: the accel_service currently only buffers AccelRawData (i.e it
      // does not track the timestamp explicitly.) The accel service drains a
      // buffers worth of data at a time and asks for the starting time
      // (state->timestamp_ms) of the first sample in that buffer when it
      // does. Therefore, we provide the real time for the first sample. In
      // the future, we could phase out legacy accel code and provide the
      // exact timestamp with every sample
      if (state->num_samples == 0) {
        state->timestamp_ms = s_last_empty_timestamp_ms + data.timestamp_delta_ms;
      }

      memcpy(state->raw_buffer + state->num_samples, &data,
             sizeof(AccelRawData));
        state->num_samples++;
        samples_drained++;
    }

    // If buffer is full, notify subscriber to process it
    if (!state->event_posted && state->num_samples >= state->samples_per_update) {
      // Notify the subscriber that data is available
      state->event_posted = prv_call_data_callback(state);

      ACCEL_LOG_DEBUG("full set of %d samples for session %p", state->num_samples, state);

      if (!state->event_posted) {
        PBL_LOG(LOG_LEVEL_INFO, "Failed to post accel event to task: 0x%x", (int) state->task);
      }
    }
    state = (AccelManagerState *)state->list_node.next;
  }

  mutex_unlock_recursive(s_accel_manager_mutex);
}

#ifdef TEST_KERNEL_SUBSCRIPTION
static void prv_kernel_data_subscription_handler(AccelData *accel_data,
    uint32_t num_samples) {
  PBL_LOG(LOG_LEVEL_INFO, "Received %" PRIu32 " accel samples for KernelMain.", num_samples);
}

static void prv_kernel_tap_subscription_handler(AccelAxisType axis,
    int32_t direction) {
  PBL_LOG(LOG_LEVEL_INFO, "Received a tap event for KernelMain, axis: %d, "
      "direction: %" PRId32, axis, direction);
}
#endif

// Compute and return the device's delta position to help determine movement as idle.
static uint32_t prv_compute_delta_pos(AccelData *cur_pos, AccelData *last_pos) {
  return (abs(last_pos->x - cur_pos->x) + abs(last_pos->y - cur_pos->y) +
          abs(last_pos->z - cur_pos->z));
}

/*
 * Exported APIs
 */

// we expect this to get called once by accel_manager_init() so we have a default
// starting position.
void analytics_external_collect_accel_xyz_delta(void) {
  AccelData accel_data;

  if (sys_accel_manager_peek(&accel_data) == 0) {
    uint32_t delta = prv_compute_delta_pos(&accel_data, &s_last_analytics_position);
    s_is_idle = (delta < ACCEL_MAX_IDLE_DELTA);
    s_last_analytics_position = accel_data;
    analytics_set(ANALYTICS_DEVICE_METRIC_ACCEL_XYZ_DELTA, delta, AnalyticsClient_System);
  }
}

void analytics_external_collect_accel_samples_received(void) {
  mutex_lock_recursive(s_accel_manager_mutex);
  uint32_t samps_collected = s_accel_samples_collected_count;
  s_accel_samples_collected_count = 0;
  mutex_unlock_recursive(s_accel_manager_mutex);

  analytics_set(ANALYTICS_DEVICE_METRIC_ACCEL_SAMPLE_COUNT, samps_collected,
                AnalyticsClient_System);
}

void accel_manager_init(void) {
  s_accel_manager_mutex = mutex_create_recursive();

  shared_circular_buffer_init(&s_buffer, s_buffer_storage,
      sizeof(s_buffer_storage));

  event_service_init(PEBBLE_ACCEL_SHAKE_EVENT, &prv_shake_add_subscriber_cb,
      &prv_shake_remove_subscriber_cb);

  event_service_init(PEBBLE_ACCEL_DOUBLE_TAP_EVENT, &prv_double_tap_add_subscriber_cb,
      &prv_double_tap_remove_subscriber_cb);

  // we always listen for motion events to decide whether or not to enable the backlight
  // TODO: KernelMain could probably subscribe to the motion service to accomplish this?
  prv_shake_add_subscriber_cb(PebbleTask_KernelMain);

  analytics_external_collect_accel_xyz_delta();
}

static void prv_copy_accel_sample_to_accel_data(AccelDriverSample const *accel_sample,
                                                AccelData *accel_data) {
  *accel_data = (AccelData) {
    .x = accel_sample->x,
    .y = accel_sample->y,
    .z = accel_sample->z,
    .timestamp /* ms */ = (accel_sample->timestamp_us / 1000),
    .did_vibrate = (sys_vibe_get_vibe_strength() != VIBE_STRENGTH_OFF)
  };
}

static void prv_update_last_accel_data(AccelDriverSample const *data) {
  prv_copy_accel_sample_to_accel_data(data, &s_last_accel_data);
}

DEFINE_SYSCALL(int, sys_accel_manager_peek, AccelData *accel_data) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(accel_data, sizeof(*accel_data));
  }

  // bump peek analytics
  analytics_inc(ANALYTICS_DEVICE_METRIC_ACCEL_PEEK_COUNT, AnalyticsClient_System);
  PebbleTask task = pebble_task_get_current();
  if (task == PebbleTask_Worker || task == PebbleTask_App) {
    analytics_inc(ANALYTICS_APP_METRIC_ACCEL_PEEK_COUNT, AnalyticsClient_CurrentTask);
  }

  mutex_lock_recursive(s_accel_manager_mutex);

  AccelDriverSample data;
  int result = accel_peek(&data);
  if (result == 0 /* success */) {
    prv_copy_accel_sample_to_accel_data(&data, accel_data);
    prv_update_last_accel_data(&data);
  }

  mutex_unlock_recursive(s_accel_manager_mutex);

  return result;
}

DEFINE_SYSCALL(AccelManagerState*, sys_accel_manager_data_subscribe,
               AccelSamplingRate rate, AccelDataReadyCallback data_cb, void* context,
               PebbleTask handler_task) {
  AccelManagerState *state;

  mutex_lock_recursive(s_accel_manager_mutex);
  {
    state = kernel_malloc_check(sizeof(AccelManagerState));
    *state = (AccelManagerState) {
      .task = handler_task,
      .data_cb_handler = data_cb,
      .data_cb_context = context,
      .sampling_interval_us = (US_PER_SECOND / rate),
      .samples_per_update = ACCEL_MAX_SAMPLES_PER_UPDATE,
    };

    bool no_subscribers_before = (s_data_subscribers == NULL);
    s_data_subscribers = list_insert_before(s_data_subscribers, &state->list_node);
    if (no_subscribers_before) {
      sys_vibe_history_start_collecting();
    }

    // Add as a consumer to the accel buffer
    shared_circular_buffer_add_subsampled_client(
        &s_buffer, &state->buffer_client, 1, 1);

    // Update the sampling rate and num samples of the driver considering the new
    // subscriber's request
    prv_update_driver_config();
  }
  mutex_unlock_recursive(s_accel_manager_mutex);

  return state;
}

DEFINE_SYSCALL(bool, sys_accel_manager_data_unsubscribe, AccelManagerState *state) {
  bool event_outstanding;
  mutex_lock_recursive(s_accel_manager_mutex);
  {
    event_outstanding = state->event_posted;
    // Remove this subscriber and free up its state variables
    shared_circular_buffer_remove_subsampled_client(
        &s_buffer, &state->buffer_client);
    list_remove(&state->list_node, &s_data_subscribers /* &head */, NULL /* &tail */);
    kernel_free(state);

    if (!s_data_subscribers) {
      // If no one left using the data subscription, disable it
      sys_vibe_history_stop_collecting();
    }

    // reconfig for the common subset of requirements among remaining subscribers
    prv_update_driver_config();
  }
  mutex_unlock_recursive(s_accel_manager_mutex);
  return event_outstanding;
}

DEFINE_SYSCALL(int, sys_accel_manager_set_sampling_rate,
               AccelManagerState *state, AccelSamplingRate rate) {

  // Make sure the rate is one of our externally supported fixed rates
  switch (rate) {
    case ACCEL_SAMPLING_10HZ:
    case ACCEL_SAMPLING_25HZ:
    case ACCEL_SAMPLING_50HZ:
    case ACCEL_SAMPLING_100HZ:
      break;
    default:
      return -1;
  }

  mutex_lock_recursive(s_accel_manager_mutex);

  state->sampling_interval_us = (US_PER_SECOND / rate);
  prv_update_driver_config();

  mutex_unlock_recursive(s_accel_manager_mutex);

  // TODO: doesn't look like our API specifies what this routine should return.
  return 0;
}

uint32_t accel_manager_set_jitterfree_sampling_rate(AccelManagerState *state,
                                                    uint32_t min_rate_mHz) {
  // HACK
  // We're dumb and don't support anything other than 12.5hz for jitter-free sampling. We chose
  // this rate because it divides evenly into all the native rates we support right now.
  // Supporting a wider range of jitter-free rates is harder due to dealing with all the potential
  // combinations of different subscribers asking for different rates.
  const uint32_t ONLY_SUPPORTED_JITTERFREE_RATE_MILLIHZ = 12500;
  PBL_ASSERTN(min_rate_mHz <= ONLY_SUPPORTED_JITTERFREE_RATE_MILLIHZ);

  mutex_lock_recursive(s_accel_manager_mutex);

  state->sampling_interval_us = (US_PER_SECOND * 1000) / ONLY_SUPPORTED_JITTERFREE_RATE_MILLIHZ;
  prv_update_driver_config();

  mutex_unlock_recursive(s_accel_manager_mutex);

  return ONLY_SUPPORTED_JITTERFREE_RATE_MILLIHZ;
}

DEFINE_SYSCALL(int, sys_accel_manager_set_sample_buffer,
               AccelManagerState *state, AccelRawData *buffer, uint32_t samples_per_update) {
  if (samples_per_update > ACCEL_MAX_SAMPLES_PER_UPDATE) {
    return -1;
  }

  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(buffer, samples_per_update * sizeof(AccelRawData));
  }

  mutex_lock_recursive(s_accel_manager_mutex);
  {
    state->raw_buffer = buffer;
    state->samples_per_update = samples_per_update;
    state->num_samples = 0;
    prv_update_driver_config();
  }
  mutex_unlock_recursive(s_accel_manager_mutex);

  return 0;
}

DEFINE_SYSCALL(uint32_t, sys_accel_manager_get_num_samples,
                   AccelManagerState *state, uint64_t *timestamp_ms) {

  mutex_lock_recursive(s_accel_manager_mutex);

  uint32_t result = state->num_samples;
  *timestamp_ms = state->timestamp_ms;

  mutex_unlock_recursive(s_accel_manager_mutex);
  return result;
}

DEFINE_SYSCALL(bool, sys_accel_manager_consume_samples,
               AccelManagerState *state, uint32_t samples) {
  bool success = true;
  mutex_lock_recursive(s_accel_manager_mutex);

  if (samples > state->num_samples) {
    PBL_LOG(LOG_LEVEL_ERROR, "Consuming more samples than exist %d vs %d!",
            (int)samples, (int)state->num_samples);
    success = false;
  } else if (samples != state->num_samples) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Dropping %d accel samples", (int)(state->num_samples - samples));
    success = false;
  }

  state->event_posted = false;
  state->num_samples = 0;
  // Fill it again from circular buffer
  prv_dispatch_data();

  mutex_unlock_recursive(s_accel_manager_mutex);
  return success;
}


/*
 * TODO: APIs that still need to be implemented
 */

void accel_manager_enable(bool on) { }

void accel_manager_exit_low_power_mode(void) { }

// Return true if we are "idle", defined as seeing no movement in the last hour.
bool accel_is_idle(void) {
  // It was idle recently, see if it's still idle. Note we avoid reading the accel hardware
  // again here to keep this call as lightweight as possible. Instead we are just comparing the last
  // read value with the value last captured by analytics (which does so on an hourly heartbeat).
  return (prv_compute_delta_pos(&s_last_accel_data, &s_last_analytics_position)
                < ACCEL_MAX_IDLE_DELTA);
}

// The accelerometer should issue a shake/tap event with any slight movements when stationary.
// This will allow the watch to immediately return to normal mode, and attempt to reconnect to
// the phone.
void accel_enable_high_sensitivity(bool high_sensitivity) {
  mutex_lock_recursive(s_accel_manager_mutex);
  accel_set_shake_sensitivity_high(high_sensitivity);
  mutex_unlock_recursive(s_accel_manager_mutex);
}

/*
 * Driver Callbacks - See accel.h header for more context
 */

static bool prv_shared_buffer_empty(void) {
  bool empty = true;
  mutex_lock_recursive(s_accel_manager_mutex);
  {
    AccelManagerState *state = (AccelManagerState *)s_data_subscribers;
    while (state) {
      int left = shared_circular_buffer_get_read_space_remaining(
          &s_buffer, &state->buffer_client.buffer_client);
      if (left != 0) {
        empty = false;
        break;
      }
      state = (AccelManagerState *)state->list_node.next;
    }
  }
  mutex_unlock_recursive(s_accel_manager_mutex);
  return empty;
}

void accel_cb_new_sample(AccelDriverSample const *data) {
  prv_update_last_accel_data(data);

  s_accel_samples_collected_count++;

  if (!s_buffer.clients) {
    return; // no clients so don't buffer any data
  }

  AccelManagerBufferData accel_buffer_data;
  accel_buffer_data.rawdata.x = data->x;
  accel_buffer_data.rawdata.y = data->y;
  accel_buffer_data.rawdata.z = data->z;

  if (prv_shared_buffer_empty()) {
    s_last_empty_timestamp_ms = data->timestamp_us / 1000;
  }

  // Note: the delta value overflows if the s_buffer is not drained for ~65s,
  // but there should be more than enough time for it to drain in that window
  accel_buffer_data.timestamp_delta_ms = ((data->timestamp_us / 1000) -
      s_last_empty_timestamp_ms);

  // if we have one or more clients who fell behind reading out of the buffer,
  // we will advance them until there is enough space available for the new data
  bool rv = shared_circular_buffer_write(&s_buffer, (uint8_t *)&accel_buffer_data,
                                         sizeof(accel_buffer_data), false /*advance_slackers*/);
  if (!rv) {
    PBL_LOG(LOG_LEVEL_WARNING, "Accel subscriber fell behind, truncating data");
    rv = shared_circular_buffer_write(&s_buffer, (uint8_t *)&accel_buffer_data,
                                      sizeof(accel_buffer_data), true /*advance_slackers*/);
  }

  PBL_ASSERTN(rv);

  prv_dispatch_data();
}

void accel_cb_shake_detected(IMUCoordinateAxis axis, int32_t direction) {
  PebbleEvent e = {
    .type = PEBBLE_ACCEL_SHAKE_EVENT,
    .accel_tap = {
      .axis = axis,
      .direction = direction,
    },
  };

  event_put(&e);
}

void accel_cb_double_tap_detected(IMUCoordinateAxis axis, int32_t direction) {
  PebbleEvent e = {
    .type = PEBBLE_ACCEL_DOUBLE_TAP_EVENT,
    .accel_tap = {
      .axis = axis,
      .direction = direction,
    },
  };

  event_put(&e);
}

static void prv_handle_accel_driver_work_cb(void *data) {
  // The accel manager is responsible for handling locking
  mutex_lock_recursive(s_accel_manager_mutex);
  AccelOffloadCallback cb = data;
  cb();
  mutex_unlock_recursive(s_accel_manager_mutex);
}

void accel_offload_work_from_isr(AccelOffloadCallback cb, bool *should_context_switch) {
  PBL_ASSERTN(mcu_state_is_isr());

  *should_context_switch =
      new_timer_add_work_callback_from_isr(prv_handle_accel_driver_work_cb, cb);
}

bool accel_manager_run_selftest(void) {
  mutex_lock_recursive(s_accel_manager_mutex);
  bool rv = accel_run_selftest();
  mutex_unlock_recursive(s_accel_manager_mutex);
  return rv;
}

#if !defined(PLATFORM_SILK)
// Note: This selftest is only used for MFG today. When we start to build out a
// gyro API, we will need to come up with a more generic way to handle locking
// for a gyro only part vs gyro + accel part
extern bool gyro_run_selftest(void);
bool gyro_manager_run_selftest(void) {
  mutex_lock_recursive(s_accel_manager_mutex);
  bool rv = gyro_run_selftest();
  mutex_unlock_recursive(s_accel_manager_mutex);
  return rv;
}
#endif

void command_accel_peek(void) {
  AccelData data;

  int result = sys_accel_manager_peek(&data);
  PBL_LOG(LOG_LEVEL_DEBUG, "result: %d", result);

  char buffer[20];
  prompt_send_response_fmt(buffer, sizeof(buffer), "X: %"PRId16, data.x);
  prompt_send_response_fmt(buffer, sizeof(buffer), "Y: %"PRId16, data.y);
  prompt_send_response_fmt(buffer, sizeof(buffer), "Z: %"PRId16, data.z);
}

void command_accel_num_samples(char *num_samples) {
  int num = atoi(num_samples);
  mutex_lock_recursive(s_accel_manager_mutex);
  accel_set_num_samples(num);
  mutex_unlock_recursive(s_accel_manager_mutex);
}

#if UNITTEST
/*
 * Helper routines strictly for unit tests
 */

void test_accel_manager_get_subsample_info(AccelManagerState *state, uint16_t *num, uint16_t *den,
                                           uint16_t *samps_per_update) {
  *num = state->buffer_client.numerator;
  *den = state->buffer_client.denominator;
  *samps_per_update = state->samples_per_update;
}

void test_accel_manager_reset(void) {
  s_buffer = (SharedCircularBuffer){};
  AccelManagerState *state = (AccelManagerState *)s_data_subscribers;
  while (state) {
    AccelManagerState *free_state = state;
    state = (AccelManagerState *)state->list_node.next;
    kernel_free(free_state);
  }
  s_data_subscribers = NULL;
  s_shake_subscribers_count = 0;
  s_double_tap_subscribers_count = 0;
}

#endif
