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

#include "ecompass.h"

#include "applib/accel_service.h"
#include "applib/compass_service.h"
#include "util/trig.h"
#include "console/prompt.h"
#include "drivers/mag.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "services/common/battery/battery_monitor.h"
#include "services/common/event_service.h"
#include "services/common/regular_timer.h"
#include "syscall/syscall_internal.h"
#include "syscall/syscall.h"
#include "system/logging.h"
#include "system/passert.h"
#include "kernel/util/sleep.h"

#include "system/rtc_registers.h"
#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#define VALID_CORR_MARKER            0x5644
#define BITS_PER_CORRECTION_VAL      16
#define CORRECTION_VAL_MASK          ((1 << BITS_PER_CORRECTION_VAL) - 1)


static CompassStatus s_current_cal_status = CompassStatusDataInvalid;
static int16_t s_active_corr[3] = { 0 };

static bool s_service_init = false;
static bool s_saved_corr_present = false;
static int16_t s_saved_corr[3] = { 0 };

static int32_t s_last_heading = -1; // the last heading we found
#ifdef RECOVERY_FW
static MagData s_last_mag_sample = { 0 };
#endif

//////////////////////////////////////////////////////////////////////////////////
// Calibration state variables
static void prv_calibration_time_expired_cb(void* data);

static bool s_high_freq_calib_active = false;
static bool s_calib_run = false;
static RegularTimerInfo s_cb_info = { .cb = prv_calibration_time_expired_cb };


//////////////////////////////////////////////////////////////////////////////////
// Compass subscription state variables

static uint8_t s_compass_subscribers_count = 0;
static bool s_compass_subscribers[NumPebbleTask] = { 0 };


//////////////////////////////////////////////////////////////////////////////////
// Accel service state variables

static AccelServiceState *s_accel_session = NULL;
static bool s_charger_plugged = false;
static AccelRawData s_accel_data = { 0 };


//////////////////////////////////////////////////////////////////////////////////
// Private calibration handlers

static void prv_reset_saved_sample(void) {
  RTC_WriteBackupRegister(MAG_Z_CORRECTION_VAL, 0x0);
  s_saved_corr_present = false;
}

static void prv_save_calibration_values(int16_t *corr) {
  // first zero out the valid marker
  prv_reset_saved_sample();

  uint32_t valxy = (((uint32_t)corr[1]) << BITS_PER_CORRECTION_VAL) |
      (corr[0] & CORRECTION_VAL_MASK);
  uint32_t valz = (((uint32_t)VALID_CORR_MARKER) << BITS_PER_CORRECTION_VAL) |
      (corr[2] & CORRECTION_VAL_MASK);

  RTC_WriteBackupRegister(MAG_XY_CORRECTION_VALS, valxy);
  RTC_WriteBackupRegister(MAG_Z_CORRECTION_VAL, valz);

  for (int i = 0; i < 3; i++) {
    s_saved_corr[i] = corr[i];
  }
  s_saved_corr_present = true;
}

// Loads the calibration values.  Returns true if loads successfully,
// otherwise false.
static bool prv_load_calibration_values(void) {
  uint32_t valxy = RTC_ReadBackupRegister(MAG_XY_CORRECTION_VALS);
  uint32_t valz = RTC_ReadBackupRegister(MAG_Z_CORRECTION_VAL);

  bool is_valid = ((valz >> BITS_PER_CORRECTION_VAL) & CORRECTION_VAL_MASK) ==
      VALID_CORR_MARKER;

  if (is_valid) {
    s_saved_corr[0] = valxy & CORRECTION_VAL_MASK;
    s_saved_corr[1] = (valxy >> BITS_PER_CORRECTION_VAL) & CORRECTION_VAL_MASK;
    s_saved_corr[2] = valz & CORRECTION_VAL_MASK;
  }

  return (is_valid);
}

static void prv_get_roll_and_pitch(AccelRawData *d, int32_t *rollp,
    int32_t *pitchp) {
  if ((d->x == 0) && (d->y == 0) && (d->z == 0)) {
    *rollp = *pitchp = 0;
    return;
  }

  int32_t roll = atan2_lookup(d->y, d->z);

  int32_t act_ang = (roll * 360) / TRIG_MAX_ANGLE;
  if (act_ang > 180) {
    roll = roll - TRIG_MAX_ANGLE;
  }

  int32_t pitch = atan2_lookup(-d->x,
      (d->y * sin_lookup(roll) + d->z * cos_lookup(roll)) / TRIG_MAX_RATIO);

  // solution repeats every 180 degrees
  if (pitch > (TRIG_MAX_ANGLE / 4)) { // > 90 degrees
    if (pitch < ((270 * TRIG_MAX_ANGLE) / 360)) {
      pitch -= (TRIG_MAX_ANGLE / 2);
    } else {
      pitch -= TRIG_MAX_ANGLE;
    }
  }

  *rollp = roll;
  *pitchp = pitch;
}

static int32_t prv_correct_for_roll_and_pitch(AccelRawData *accel_data,
    MagData *mag_data, int32_t roll, int32_t pitch) {
  int32_t mx = mag_data->x - s_active_corr[0];
  int32_t my = mag_data->y - s_active_corr[1];
  int32_t mz = mag_data->z - s_active_corr[2];

  int32_t mx_rot, my_rot;

  // per freescale AN4249, roll is unstable close to vertical but pitch is ok
  int32_t corr = 0;
  if (TRIGANGLE_TO_DEG(pitch) > 82) {
    pitch = TRIG_MAX_ANGLE / 4;
    roll = 0;
  } else if (accel_data->z < 0) {
    // the watch has been flipped over. If someone is viewing the watch
    // at a pitch > 90 degrees, this means the heading will rotate around
    // on them (since technically, the 'front' of the watch is pointing
    // at them.) Flip the heading back around in this case.
    corr = TRIG_MAX_ANGLE / 2;
  }

  mx_rot = (mx * cos_lookup(pitch)) / TRIG_MAX_RATIO;
  mx_rot += (((my * sin_lookup(pitch)) / TRIG_MAX_RATIO) * sin_lookup(roll)) /
      TRIG_MAX_RATIO;
  mx_rot += (((mz * sin_lookup(pitch)) / TRIG_MAX_RATIO) * cos_lookup(roll)) /
      TRIG_MAX_RATIO;

  my_rot = mz * sin_lookup(roll) - my * cos_lookup(roll);
  my_rot /= TRIG_MAX_RATIO;

  int32_t heading = (atan2_lookup(-my_rot, mx_rot) + corr) % TRIG_MAX_ANGLE;
  return (heading);
}


//////////////////////////////////////////////////////////////////////////////////
// Private handlers for compass service

static void prv_calibration_time_expired_cb(void* data) {
  PBL_LOG(LOG_LEVEL_DEBUG, "Calibration time expired, complete, or app exit, "
          "dropping back to low frequency");

  if (!mag_change_sample_rate(MagSampleRate5Hz)) {
    PBL_LOG(LOG_LEVEL_WARNING, "Forcing reset to enter low freq mode");
    mag_release();
    mag_start_sampling();
  }

  s_high_freq_calib_active = false;
  regular_timer_remove_callback(&s_cb_info);
}

static void prv_accel_for_compass_handler(AccelRawData *d, uint32_t num_samples,
    uint64_t timestamp) {
  int32_t x = 0, y = 0, z = 0;

  // 1st order butterworth filter with a cutoff freq of 0.02Fs
  static int32_t xp = 0, yp = 0, zp = 0;
  static int32_t xr = 0, yr = 0, zr = 0;

  for (uint32_t i = 0; i < num_samples; i++) {
    xr = (305 * d[i].x + 305 * xp + 9391 * xr) / 10000;
    yr = (305 * d[i].y + 305 * yp + 9391 * yr) / 10000;
    zr = (305 * d[i].z + 305 * zp + 9391 * zr) / 10000;
    xp = d[i].x;    yp = d[i].y;    zp = d[i].z;
    x += xr;
    y += yr;
    z += zr;
  }

  x /= (int)num_samples;
  y /= (int)num_samples;
  z /= (int)num_samples;

  // TODO: could this ever be called in the middle of ecompass_service_handle?
  // map accel data to NED coordinate system
  s_accel_data.x = y;
  s_accel_data.y = x;
  s_accel_data.z = -z;
}

static void prv_compass_data_service_stop(PebbleTask task) {
  if (s_compass_subscribers[task]) {
    s_compass_subscribers[task] = false;

    if (--s_compass_subscribers_count == 0) {
      // If this was the last subscribed process, then stop the compass
      // service
      if (s_high_freq_calib_active) {
        prv_calibration_time_expired_cb(NULL);
        s_calib_run = false;
      }
      accel_session_data_unsubscribe(s_accel_session);
      accel_session_delete(s_accel_session);
      s_accel_session = NULL;
      mag_release();
    }
  }
  PBL_LOG(LOG_LEVEL_DEBUG, "subscribers %"PRIu8, s_compass_subscribers_count);
}

static void prv_compass_data_service_start(PebbleTask task) {
  prv_compass_data_service_stop(task);
  s_compass_subscribers[task] = true;
  if (++s_compass_subscribers_count == 1) {
    // If this is the first subscriber to the compass service, start sampling
    PBL_ASSERTN(s_accel_session == NULL);

    s_accel_session = accel_session_create();
    accel_session_raw_data_subscribe(s_accel_session, ACCEL_SAMPLING_25HZ, 5,
                                     prv_accel_for_compass_handler);

    mag_start_sampling();
  }
  PBL_LOG(LOG_LEVEL_DEBUG, "subscribers %"PRIu8, s_compass_subscribers_count);
}

//////////////////////////////////////////////////////////////////////////////////
// Public API

void ecompass_handle_battery_state_change_event(PreciseBatteryChargeState new_state) {
  if (new_state.is_plugged) {
    s_charger_plugged = true;
    s_current_cal_status = CompassStatusDataInvalid;
    memset(s_active_corr, 0x00, sizeof(s_active_corr));
    prv_reset_saved_sample();
  } else if (s_charger_plugged) {
    // we have unplugged the charger, initiate recalibration
    s_charger_plugged = false;
    s_calib_run = false; // trigger a rerun of fast compass calibration
    PBL_LOG(LOG_LEVEL_DEBUG, "Restarting calibration after charge event");
  }
}

void ecompass_service_init(void) {
  if (!s_service_init) {
    s_saved_corr_present = prv_load_calibration_values();
    if (s_saved_corr_present) {
      s_current_cal_status = CompassStatusCalibrating;
      for (int i = 0; i < 3; i++) {
        s_active_corr[i] = s_saved_corr[i];
      }
    }
    s_service_init = true;
  }

  event_service_init(PEBBLE_COMPASS_DATA_EVENT, &prv_compass_data_service_start,
     &prv_compass_data_service_stop);
}

void ecompass_service_handle(void) {
  static int samples_collected = 0;

  // read magnetometer sample
  MagData mag_data;
  MagReadStatus rv = mag_read_data(&mag_data);
  if (rv != MagReadSuccess) {
    if (rv == MagReadCommunicationFail) {
      // heavy hammer fix for now
      // FIXME: move the restart logic to driver
      PBL_LOG(LOG_LEVEL_WARNING, "Read after %d samples failed, "
              "restarting compass", samples_collected);
      mag_release();
      mag_start_sampling();
    }
    return;
  }

#ifdef RECOVERY_FW
  s_last_mag_sample = mag_data;
#endif

  // industry standard for heading coordinates uses NED convention (check out
  // Freescale's AN4248 or ST's AN3192 as examples). Therefore, we map pebbles
  // coordinate system (ENU) to NED in this service module
  int16_t my_orig = mag_data.y;
  mag_data.y = mag_data.x;
  mag_data.x = my_orig;
  mag_data.z = -mag_data.z;

  samples_collected++;

  // don't perform any calibration if the charger is plugged in
  if (!s_charger_plugged && (s_current_cal_status != CompassStatusCalibrated)) {
    // if we haven't tried to calibrate yet, run at a higher sampling rate
    // for a short duration in order to finish calibration more quickly
    if (!s_calib_run) {
      ecomp_corr_reset();
      mag_change_sample_rate(MagSampleRate20Hz);
      regular_timer_add_multiminute_callback(&s_cb_info, 2);
      s_calib_run = true;
      s_high_freq_calib_active = true;
      samples_collected = 0;
      return; // if we are switching to high freq mode, don't use 1st sample
    }

    if (samples_collected < 5) {
      return; // wait a few samples for ramp up error to stabilize
    }

    int16_t new_corr[3];
    MagCalStatus cal_status = ecomp_corr_add_raw_mag_sample((int16_t *)&mag_data,
        (s_saved_corr_present) ? s_saved_corr : NULL, new_corr);

    if (cal_status != MagCalStatusNoSolution) {
      PBL_LOG(LOG_LEVEL_INFO, "%s : %d %d %d (type = %d)", "Mag Corr",
          (int)new_corr[0], (int)new_corr[1], (int)new_corr[2], (int)cal_status);
    }

    bool locked_sol = (cal_status == MagCalStatusNewLockedSolutionAvail);
    if (locked_sol || ((cal_status == MagCalStatusNewSolutionAvail) &&
        !s_saved_corr_present)) {
      s_current_cal_status = CompassStatusCalibrating;
      for (int i = 0; i < 3; i++) {
        if ((s_active_corr[i] == 0) || locked_sol) {
          s_active_corr[i] = new_corr[i];
          continue;
        }

        // smooth out noise from solutions while we wait for a locked set
        const int alpha = 30; // greater alpha leads to less smoothing
        new_corr[i] = ((new_corr[i] - s_active_corr[i]) * alpha) / 100;
        s_active_corr[i] += new_corr[i];
      }
    }

    if (s_high_freq_calib_active && (cal_status == MagCalStatusNoSolution) &&
        ((samples_collected % 4) != 0)) {
      return; // only bubble up every 4th sample to app land with high samp rate
    }

    if ((cal_status == MagCalStatusNewLockedSolutionAvail) ||
        (cal_status == MagCalStatusSavedSampleMatch)) {
      if (s_high_freq_calib_active) {
        prv_calibration_time_expired_cb(NULL);
      }
      prv_save_calibration_values(s_active_corr);
      s_current_cal_status = CompassStatusCalibrated;
    }
  }

  // get the most recent accel readings
  AccelRawData accel_data = s_accel_data;
  int32_t roll = 0, pitch = 0;
  prv_get_roll_and_pitch(&accel_data, &roll, &pitch);

  PebbleEvent e = {
    .type = PEBBLE_COMPASS_DATA_EVENT,
    .compass_data = {
      .magnetic_heading = prv_correct_for_roll_and_pitch(&accel_data, &mag_data,
          roll, pitch),
      .calib_status = s_current_cal_status
    }
  };

  s_last_heading = e.compass_data.magnetic_heading;
  event_put(&e);
}

//////////////////////////////////////////////////////////////////////////////////
// System call handlers

DEFINE_SYSCALL(bool, sys_ecompass_service_subscribed, void) {
  PebbleTask task = pebble_task_get_current();
  return s_compass_subscribers[task];
}

DEFINE_SYSCALL(void, sys_ecompass_get_last_heading, CompassHeadingData *data) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(data, sizeof(*data));
  }

  *data = (CompassHeadingData) {
    .magnetic_heading = s_last_heading,
    .true_heading = s_last_heading,
    .compass_status = s_current_cal_status,
    .is_declination_valid = false
  };
}

//////////////////////////////////////////////////////////////////////////////////
// Recovery firmware commands

#ifdef RECOVERY_FW
static void prv_ecompass_start_callback(void *context) {
  s_accel_session = accel_session_create();
  accel_session_raw_data_subscribe(s_accel_session, ACCEL_SAMPLING_25HZ, 5,
                                   prv_accel_for_compass_handler);
  mag_start_sampling();
}

static void prv_ecompass_stop_callback(void *context) {
  accel_session_data_unsubscribe(s_accel_session);
  accel_session_delete(s_accel_session);
  mag_release();
}

//! Serial command for reading a single value from the compass.
void command_compass_peek(void) {
  int32_t prev_heading = s_last_heading;

  launcher_task_add_callback(prv_ecompass_start_callback, NULL);

  // wait for last heading to be updated
  int retries = 50; // 5 seconds should be ample time
  while ((prev_heading == s_last_heading) && retries-- > 0) {
    psleep(100);
  }

  launcher_task_add_callback(prv_ecompass_stop_callback, NULL);
  psleep(5); // give the compass some time to stop

  char buffer[40];
  prompt_send_response_fmt(buffer, sizeof(buffer), "%"PRId32" degrees",
           (s_last_heading * 360) / TRIG_MAX_ANGLE);

  prompt_send_response_fmt(buffer, sizeof(buffer), "Mx=%d, My=%d, Mz=%d",
      s_last_mag_sample.x, s_last_mag_sample.y, s_last_mag_sample.z);
}
#endif // RECOVERY_FW
