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

//! The QEMU accelerometer driver is pretty broken, but it requires
//! a complete overhaul of both the QEMU Serial messages and all senders
//! of those messages (pebble-tool and CloudPebble, via libpebble2) to
//! fix the brokenness. Since it's not a critical feature, the
//! brokenness will stay for the time being.
//!
//! What's broken about it? The protocol is braindead: it doesn't know
//! anything about sample rates. The senders just send a sequence of
//! (x,y,z) tuples with no timing information attached. The driver then
//! plays them back one after the other at whatever sample rate the
//! accel manager happens to request. This means that depending on the
//! sample rates that the samples were recorded and the current
//! configured sample rate, the samples could be replayed anywhere from
//! 10x slower to 10x faster than they were recorded (100 Hz recording
//! with 10 Hz replay, or vice versa).
//!
//! The driver was exceptionally braindead before, buffering up all of
//! the samples it could and replaying them from a 256-sample deep
//! buffer. With a typical replay rate of 25 Hz and samples being
//! recorded at 100 Hz, that results in samples being replayed at 1/4
//! speed with ten second latency. No good.
//!
//! The way libpebble2/pebble-tool/CloudPebble sends accel samples to be
//! replayed is also braindead. It pays no attention to the
//! QemuProtocolAccelResponse messages and just sends samples as soon as
//! they're received. So for replaying samples from the command-line or
//! a file, they're all batched up and sent in a single message. Samples
//! being recorded live from a phone are taken at 100 Hz and sent to
//! QEMU as soon as they are received. By knowing how the protocol is
//! actually used, we can improve the user experience quite
//! significantly, making the driver a bit simpler in the process.
//! Instead of buffering all samples as they are received, throw out and
//! replace the sample buffer every time a new QemuProtocolAccel message
//! is received. Play those back at the driver's current sampling rate,
//! latching the last sample received if the sample buffer underruns.
//! Replaying of prerecorded accelerometer samples e.g. from a file will
//! still play back at the wrong sample rate most of the time, but live
//! replay from a phone will work in realtime with minimal latency
//! without speeding up or slowing down the signal during replay.

#include "drivers/accel.h"
#include "drivers/imu/bmi160/bmi160.h"

#include "drivers/qemu/qemu_serial.h"
#include "drivers/rtc.h"
#include "os/mutex.h"
#include "pebble_errors.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/net.h"

#include <string.h>


#define ACCEL_LOG_DEBUG(fmt, args...) PBL_LOG_D(LOG_DOMAIN_ACCEL, LOG_LEVEL_DEBUG, fmt, ## args)


static bool s_initialized;
static PebbleMutex * s_accel_mutex;

static uint32_t s_sampling_interval_ms = 0;
static const AccelRawData s_default_sample = {
  .x = 0,
  .y = 0,
  .z = -1000
};

// We copy accel data received over the QEMU serial connection into this buffer.
// This data gets moved into s_latest_reading when the s_timer_id timer callback
// executes.
#define QEMU_ACCEL_RCV_BUFFER_SAMPLES 256
static AccelRawData s_rcv_buffer[QEMU_ACCEL_RCV_BUFFER_SAMPLES];
static uint16_t     s_num_rcv_samples;
static uint16_t     s_current_rcv_sample;

static AccelRawData s_latest_reading;
static uint32_t     s_num_fifo_samples;           // # of samples in the fifo

// This timer is used to feed the FIFO
static bool         s_timer_running;
static TimerID      s_timer_id;               // timer used to copy data from s_rcv_samples into
                                              // s_latest_reading.


static void prv_construct_driver_sample(AccelDriverSample *sample) {
  time_t time_s;
  uint16_t time_ms;
  rtc_get_time_ms(&time_s, &time_ms);
  uint64_t timestamp_ms = ((uint64_t)time_s) * 1000 + time_ms;

  *sample = (AccelDriverSample) {
    .timestamp_us = timestamp_ms * 1000,
    .x = s_latest_reading.x,
    .y = s_latest_reading.y,
    .z = s_latest_reading.z,
  };
}


static void prv_stop_timer(void) {
  new_timer_stop(s_timer_id);
  s_timer_running = false;
}


// This timer runs as long as we have samples in our s_rcv_buffer or there is
// any subscription to the accel that expects samples to arrive at a given
// frequency. It feeds samples at the right rate into the s_latest_reading
// global (for peek mode) and into the accel driver.
static void prv_timer_cb(void *data) {
  mutex_lock(s_accel_mutex);

  if (s_current_rcv_sample < s_num_rcv_samples) {
    s_latest_reading = s_rcv_buffer[s_current_rcv_sample++];
  }

  // Keep it simple; this accelerometer has no FIFO.
  if (s_num_fifo_samples > 0) {
    AccelDriverSample sample;
    prv_construct_driver_sample(&sample);
    ACCEL_LOG_DEBUG("Accel sample to manager: %d, %d, %d", sample.x, sample.y, sample.z);
    accel_cb_new_sample(&sample);
  }

  if (s_num_fifo_samples == 0 && s_current_rcv_sample >= s_num_rcv_samples) {
    prv_stop_timer();
  }

  mutex_unlock(s_accel_mutex);
}


// Start/reschedule the timer that feeds the FIFO/s_latest_reading out of the
// samples received from the host
static void prv_reschedule_timer(void) {
  bool success = new_timer_start(s_timer_id, s_sampling_interval_ms,
                                 prv_timer_cb, NULL,
                                 TIMER_START_FLAG_REPEATING);
  PBL_ASSERTN(success);
  s_timer_running = true;
}


// Called by the qemu_serial driver when we receive an accel packet from the
// remote side. This copies the received data into our s_rcv_buffer buffer. It
// will gradually be pulled out of that and replayed by the timer callback.
void qemu_accel_msg_callback(const uint8_t *data, uint32_t len) {
  QemuProtocolAccelHeader *hdr = (QemuProtocolAccelHeader *)data;

  // Validate the packet
  uint32_t data_bytes = hdr->num_samples * sizeof(AccelRawData);
  if (data_bytes != len - sizeof(QemuProtocolAccelHeader)) {
    PBL_LOG(LOG_LEVEL_ERROR, "Invalid packet received");
    return;
  }
  ACCEL_LOG_DEBUG("Got accel msg from host: num samples: %d", hdr->num_samples);

  // Copy the received samples into the s_rcv_buffer
#if QEMU_ACCEL_RCV_BUFFER_SAMPLES < 256
  s_num_rcv_samples = MIN(hdr->num_samples,
                          QEMU_ACCEL_RCV_BUFFER_SAMPLES);
#else
  s_num_rcv_samples = hdr->num_samples;
#endif
  s_current_rcv_sample = 0;
  mutex_lock(s_accel_mutex);
  {
    for (uint32_t i=0; i < s_num_rcv_samples; ++i) {
      s_rcv_buffer[i].x = ntohs(hdr->samples[i].x);
      s_rcv_buffer[i].y = ntohs(hdr->samples[i].y);
      s_rcv_buffer[i].z = ntohs(hdr->samples[i].z);
      ACCEL_LOG_DEBUG("  x,y,z from host: %d, %d, %d", s_rcv_buffer[i].x,
                      s_rcv_buffer[i].y, s_rcv_buffer[i].z);
    }

    // If we have any samples at all, make sure the timer is running. This is
    // required in order to feed the data at the right speed for peek mode.
    if (!s_timer_running && s_num_rcv_samples > 0) {
      prv_reschedule_timer();
    }
  }
  mutex_unlock(s_accel_mutex);

  // Send a response, even though none of the clients care about it.
  QemuProtocolAccelResponseHeader resp = {
      .avail_space = htons(QEMU_ACCEL_RCV_BUFFER_SAMPLES),
  };
  qemu_serial_send(QemuProtocol_Accel, (uint8_t *)&resp, sizeof(resp));
}


void qemu_accel_init(void) {
  PBL_ASSERTN(!s_initialized);
  s_initialized = true;
  s_latest_reading = s_default_sample;
  s_accel_mutex = mutex_create();
  s_timer_id = new_timer_create();
}


uint32_t accel_set_sampling_interval(uint32_t interval_us) {
  mutex_lock(s_accel_mutex);
  {
    s_sampling_interval_ms = interval_us / 1000;

    if (s_timer_running) {
      // If timer is already running, update it's frequency
      prv_reschedule_timer();
    }
  }
  mutex_unlock(s_accel_mutex);
  return accel_get_sampling_interval();
}

uint32_t accel_get_sampling_interval(void) {
  return s_sampling_interval_ms * 1000;
}


void accel_set_num_samples(uint32_t num_samples) {
  mutex_lock(s_accel_mutex);
  {
    s_num_fifo_samples = num_samples;

    // Setup our timer to fire at the right frequency. If using peek mode, then
    // the timer still has to run if there are any samples received from the
    // host that we need to feed into the current peek value.
    if (num_samples > 0 || s_num_rcv_samples > 0) {
      prv_reschedule_timer();
    } else {
      prv_stop_timer();
    }
  }
  mutex_unlock(s_accel_mutex);
}


int accel_peek(AccelDriverSample *data) {
  prv_construct_driver_sample(data);
  return 0;
}


void accel_enable_shake_detection(bool on) {
}


bool accel_get_shake_detection_enabled(void) {
  return false;
}


void accel_enable_double_tap_detection(bool on) {
}


bool accel_get_double_tap_detection_enabled(void) {
  return false;
}


void accel_set_shake_sensitivity_high(bool sensitivity_high) {
}

bool accel_run_selftest(void) {
  return true;
}
