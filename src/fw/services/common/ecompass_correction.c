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

#include "util/trig.h"
#include "kernel/pbl_malloc.h"
#include "system/logging.h"
#include "util/math.h"

#include <stdint.h>
#include <stdio.h>

#define N_SAMPS    4 // four points define a unique sphere
#define N_AXIS     3
static int16_t s_samples[N_SAMPS][N_AXIS];

//
// Two basic equations of a sphere:
//  a) (x - x0)^2 + (y - y0)^2 + (z - z0)^2 = r0^2
//     (x^2 - 2*x*x0 + x0^2) + (y^2 - 2*y*x0 + y0^2) + (z^2 - 2*z*z0 + z0^2) = r^2
//     -2*x*x0 - 2*y*y0 - 2*z*z0 + (x^2 + y^2 + z^2) + r0^2 = 0
//  b) A*(x^2 + y^2 + x^2) + B*x + C*y + D*z + E = 0
//
// Ideally, we would be able to build a large cloud of points and apply a least
// squares or ellipsoid fit to that dataset. However, this quickly becomes
// expensive (from a code space perspective). Therefore, we focus on collecting
// four 'good' data points and fitting those point to a sphere
//
// The sphere fit entails solving a linear system of the form Ax = B
//
// |  (x^2 + y^2 + z^2)      x     y     z     1 |   | A |    | 0 |
// | (x0^2 + y0^2 + z0^2)    x0    y0    z0    1 |   | B |    | 0 |
// | (x1^2 + y1^2 + z1^2)    x1    y1    z1    1 | * | C | =  | 0 |
// | (x2^2 + y2^2 + z2^2)    x2    y2    z2    1 |   | D |    | 0 |
// | (x3^2 + y3^2 + z3^2)    x3    y3    z3    1 |   | E |    | 0 |
//
// to solve we want to find where det(A) = 0
//
// Using Laplace's formula for determinant expansion we can break this into a
// system of 4x4 determinants. Expanding along row 0 and using Cij to represent
// the cofactor which removes row i and column j:
//
// (x^2 + y^2 + z^2) * C_0,0 + x * C_0,1 + y * C_0,2 + z * C_0,3 + C_0,4 = 0
//
// This solution can be re-written in a form similar to eq a) above as
//
// x * (C_0,1 / C_0,0) + y * (C_0,2 / C_0,0) + z * (C_0,3 / C_0,0) +
//     (C_0,4 / C_0,0) + (x^2 + y^2 + z^2) = 0
//
// This gives us our hard iron correction estimates (i.e location of the sphere
// origin, (xo, yo, zo)) as:
//
//  (C_0,1 / C_0,0) = -2*x0 ==> x0 = (C_0,1 / C_0,0) / -2
//  (C_0,2 / C_0,0) = -2*y0 ==> y0 = (C_0,2 / C_0,0) / -2
//  (C_0,3 / C_0,0) = -2*z0 ==> z0 = (C_0,3 / C_0,0) / -2
//

static int32_t sphere_determinant4x4(int32_t **m, int32_t down_samp) {
  // assume x, y, z value fits within 14 bits & m[x][3] == 1
  // when m[x][0] == x^2 + y^2 + z^2, this val < 30 bits
  // so m[x][0] * temp is capped at 44bits leaving plenty of space
  // within 64 bits

  int64_t res = 0;
  // computes the determinant by decomposing the computation into 4 3x3 minor
  // determinants formed by removal of row 'skip_row' & the third column
  for (int skip_row = 0; skip_row < 4; skip_row++) {
    int r0 = (skip_row != 0) ? 0 : 1;
    int r1 = (skip_row < 2) ? 2 : 1;
    int r2 = (skip_row < 3) ? 3 : 2;

    int64_t temp = (m[r1][1] * m[r2][2]) - (m[r1][2] * m[r2][1]);
    int64_t det = temp * (int64_t)m[r0][0];

    temp = (m[r1][0] * m[r2][2]) - (m[r1][2] * m[r2][0]);
    det -= temp * (int64_t)m[r0][1];

    temp = (m[r1][0] * m[r2][1]) - (m[r1][1] * m[r2][0]);
    det += temp * (int64_t)m[r0][2];

    if ((skip_row % 2) == 0) {
      res += det;
    } else {
      res -=det;
    }
  }

  return (res / down_samp);
}

static bool sphere_fit(int16_t *solution) {
  bool rv = false;

  int32_t **matrix = kernel_malloc(sizeof(int32_t *) * N_SAMPS);
  for (int i = 0; i < N_SAMPS; i++) {
    matrix[i] = kernel_malloc(sizeof(int32_t) * N_AXIS);
  }

  // determine average value of x, y, & z coordinates
  // and shift by this factor to prevent overflow.
  int32_t shift_factor[N_AXIS] = { 0 };
  for (int j = 0; j < N_AXIS; j++) {
    for (int i = 0; i < N_SAMPS; i++) {
      shift_factor[j] += s_samples[i][j];
    }
    shift_factor[j] /= N_SAMPS;
  }

  int16_t raw_data[N_SAMPS][N_AXIS];
  for (int i = 0; i < N_SAMPS; i++) {
    for (int j = 0; j < 3; j++) {
      raw_data[i][j] = (s_samples[i][j] - shift_factor[j]);
      // TODO: stop here if we have a chance of overflowing
    }
  }

  int32_t r[N_SAMPS];
  for (int i = 0; i < N_SAMPS; i++) {
    r[i] = raw_data[i][0] * raw_data[i][0] +
        raw_data[i][1] * raw_data[i][1] +
        raw_data[i][2] * raw_data[i][2];
  }

  // We now find the origin by solving the linear equation discussed above

  for (int i = 0; i < N_SAMPS; i++) {
    matrix[i][0] = raw_data[i][0];
    matrix[i][1] = raw_data[i][1];
    matrix[i][2] = raw_data[i][2];
  }
  int32_t c00 = sphere_determinant4x4(matrix, 1);
  if (c00 == 0) {
    goto cleanup;
  }

  // compute cofactor01 to solve for x0
  for (int i = 0; i < N_SAMPS; i++) {
    matrix[i][0] = r[i]; // m[i][1] = y, m[i][2] = z
  }
  solution[0] = shift_factor[0] + sphere_determinant4x4(matrix, c00 * 2);

  // compute cofactor02 to solve for y0
  for (int i = 0; i < N_SAMPS; i++) {
    matrix[i][1] = raw_data[i][0]; // m[i][0] = r^2, m[i][2] = z
  }
  solution[1] = shift_factor[1] - sphere_determinant4x4(matrix, c00 * 2);

  // compute cofactor03 to solve for z0
  for (int i = 0; i < N_SAMPS; i++) {
    matrix[i][2] = raw_data[i][1]; // m[i][0] = r^2, m[i][1] = x
  }
  solution[2] = shift_factor[2] + sphere_determinant4x4(matrix, c00 * 2);

  rv = true;
cleanup:
  for (int i = 0; i < N_SAMPS; i++) {
    kernel_free(matrix[i]);
  }
  kernel_free(matrix);
  return (rv);
}

// Earths magnetic field intensity ranges from 25uT (near the equator) to 65uT
// (near the earths poles). A majority of Europe, North America, & Asia ranges
// between 35-50uT.
//
// Assuming magnetometer readings are predominantly influenced by hard iron
// distortions, we seek to find four points and fit them to a sphere in
// order to determine the offset we need to shift the raw data by to correct
// for hard iron distortions.
//
// For points A, B, C, D and a distance threshold, t, we select four points by
// satisfying the following:
//   distance_ptA_to_ptB > t
//   distance_lineAB_to_ptC > t
//   distance_planeABC_to_ptD > t.
//
// Conceptually, it makes sense that the farther points are from one another (>
// t), the less that errors due to noise, fixed point mathematics, & motion
// render bad solutions. (empirically, this seems to be the behavior as
// well). However, the greater the threshold, the more orientations one
// must put their watch through in order to get solution sets
//
// For now, select a distance metric that should work out of the box for a
// majority of the middle of the world. However, if no solution sets are found
// after 45s, fall back to a less aggressive threshold that will work
// anywhere in the world.

#define THRESH_MAX 370 /* 37 uT */
#define THRESH_MIN 220 /* 22 uT */

// Note: All of the following helper routines operate on the s_samples array
// defined above and populated by add_raw_mag_sample

static bool pt_to_pt_dist_under_thresh(int idx_a, int idx_b, int32_t thresh) {
  int32_t v_ab[3] = {
    s_samples[idx_b][0] - s_samples[idx_a][0],
    s_samples[idx_b][1] - s_samples[idx_a][1],
    s_samples[idx_b][2] - s_samples[idx_a][2]
  };

  int32_t dist_sq = v_ab[0] * v_ab[0] + v_ab[1] * v_ab[1] +
      v_ab[2] * v_ab[2];

  return (dist_sq > (thresh * thresh));
}

static bool pt_to_line_dist_under_thresh(int idx_line_a, int idx_line_b,
    int idx_pt, int32_t thresh) {

  int32_t s[3] = {
    s_samples[idx_line_b][0] - s_samples[idx_line_a][0],
    s_samples[idx_line_b][1] - s_samples[idx_line_a][1],
    s_samples[idx_line_b][2] - s_samples[idx_line_a][2]
  };

  int32_t m1[3] = {
    s_samples[idx_line_a][0] - s_samples[idx_pt][0],
    s_samples[idx_line_a][1] - s_samples[idx_pt][1],
    s_samples[idx_line_a][2] - s_samples[idx_pt][2]
  };

  int32_t m1xs[3] = {
    m1[1] * s[2] - m1[2] *s[1],
    -(m1[0] * s[2] - m1[2] * s[0]),
    m1[0]*s[1] - m1[1] * s[0]
  };

  int64_t dist_sq = ((int64_t)m1xs[0]*m1xs[0] + (int64_t)m1xs[1]*m1xs[1] +
      (int64_t)m1xs[2]*m1xs[2]) / (s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);

  return (dist_sq > (thresh * thresh));
}

static bool pt_to_plane_dist_under_thresh(int idx_pln_a, int idx_pln_b,
    int idx_pln_c, int idx_pt, int32_t thresh) {

  int32_t v_ab[3];
  int32_t v_ac[3];
  for (int i = 0; i < 3; i++) {
      v_ab[i] = s_samples[idx_pln_b][i] - s_samples[idx_pln_a][i];
      v_ac[i] = s_samples[idx_pln_c][i] - s_samples[idx_pln_a][i];
  }

  int64_t plane_eq[4];
  plane_eq[0] = v_ab[1] * v_ac[2] - v_ab[2] * v_ac[1];
  plane_eq[1] = -v_ab[0] * v_ac[2] + v_ab[2] * v_ac[0];
  plane_eq[2] = v_ab[0] * v_ac[1] - v_ab[1] * v_ac[0];

  // solve for d
  plane_eq[3] = -(plane_eq[0] * s_samples[0][0] +
      plane_eq[1] * s_samples[0][1] + plane_eq[2] * s_samples[0][2]);

  // Distance^2 = (a * xo + b * yo + c * zo + d) / (a^2 + b^2 + c^2)
  int64_t distance = ABS((plane_eq[0] * s_samples[idx_pt][0] +
      plane_eq[1] * s_samples[idx_pt][1] + plane_eq[2] * s_samples[idx_pt][2] +
      plane_eq[3]));
  int32_t r = integer_sqrt(plane_eq[0] * plane_eq[0] + plane_eq[1] * plane_eq[1]
      + plane_eq[2] * plane_eq[2]);


  return ((distance / r) > thresh);
}

static int min_max_diff(int16_t *vals, int n_vals) {
  int min = vals[0];
  int max = vals[0];

  for (int i = 0; i < n_vals; i++) {
    if (vals[i] > max) {
      max = vals[i];
    } else if (vals[i] < min) {
      min = vals[i];
    }
  }
  return (max - min);
}

#define N_COMP_SAMPS 3
static MagCalStatus check_correction_value(int16_t *solution,
    int16_t *saved_solution) {
  const int max_delta_thresh = 50;

  // stash several of the most recent calibration results. The idea here is if
  // we get multiple readings in a row close to one another than we have locked
  // onto a good set of solutions
  static int16_t calib_idx = 0;
  static int16_t calib_val[N_AXIS][N_COMP_SAMPS];
  calib_val[0][calib_idx % 3] = solution[0];
  calib_val[1][calib_idx % 3] = solution[1];
  calib_val[2][calib_idx % 3] = solution[2];
  calib_idx++;

  static int saved_sample_match = 0;
  int x_delta, y_delta, z_delta;

  // is the new solution close to what we already have saved?
  if (saved_solution != NULL) {
    x_delta = ABS(saved_solution[0] - solution[0]);
    y_delta = ABS(saved_solution[1] - solution[1]);
    z_delta = ABS(saved_solution[2] - solution[2]);
    if ((x_delta < max_delta_thresh) && (y_delta < max_delta_thresh) &&
        (z_delta < max_delta_thresh)) {
      saved_sample_match++;
      if (saved_sample_match == 3) {
        saved_sample_match = 0;
        calib_idx = 0;
        PBL_LOG(LOG_LEVEL_INFO, "Persisting previous values!");
        return (MagCalStatusSavedSampleMatch); // locked
      }
    }
  }

  // do we have several solutions in a row that are close to one another
  if (calib_idx >= N_COMP_SAMPS) {
    x_delta = min_max_diff(calib_val[0], 3);
    y_delta = min_max_diff(calib_val[1], 3);
    z_delta = min_max_diff(calib_val[2], 3);
    if ((x_delta < max_delta_thresh) && (y_delta < max_delta_thresh) &&
        (z_delta < max_delta_thresh)) {
      int corrs[N_AXIS] = { 0 };
      for (int i = 0; i < N_AXIS; i++) {
        for (int j = 0; j < N_COMP_SAMPS; j++) {
          corrs[i] += calib_val[i][j];
        }
        solution[i] = corrs[i] / N_COMP_SAMPS;
      }
      calib_idx = 0;
      return (MagCalStatusNewLockedSolutionAvail);
    }
  }

  return (MagCalStatusNewSolutionAvail);
}

static int s_sample_idx = 0;
static int s_no_fit_strikes = 0;
static int s_samples_collected_for_fit = 0;

void ecomp_corr_reset(void) {
    s_no_fit_strikes = 0;
    s_samples_collected_for_fit = 0;
    s_sample_idx = 0;
}

MagCalStatus ecomp_corr_add_raw_mag_sample(int16_t *sample,
    int16_t *saved_corr, int16_t *solution) {
  static int thresh = THRESH_MAX;
  s_samples_collected_for_fit++;

  // if we haven't gotten good samples points in 15s @ 20Hz (60s @ 5Hz)
  if (s_samples_collected_for_fit > 300) {
    if (s_sample_idx >= 2) { // there was some kind of motion
      s_no_fit_strikes++;
    }
    if (s_no_fit_strikes == 2) {
      PBL_LOG(LOG_LEVEL_INFO, "Lowering magnetometer distance threshold");
      thresh = THRESH_MIN;
    }

    s_samples_collected_for_fit = 0;
    s_sample_idx = 0;
  }

  s_samples[s_sample_idx][0] = sample[0];
  s_samples[s_sample_idx][1] = sample[1];
  s_samples[s_sample_idx][2] = sample[2];

  if (s_sample_idx == 1) {
    if (!pt_to_pt_dist_under_thresh(0, 1, thresh)) {
      return (MagCalStatusNoSolution);
    }
  } else if (s_sample_idx == 2) {
    if (!pt_to_line_dist_under_thresh(0, 1, 2, thresh)) {
      return (MagCalStatusNoSolution);
    }
  } else if (s_sample_idx == 3) {
    if (!pt_to_plane_dist_under_thresh(0, 1, 2, 3, thresh)) {
      return (MagCalStatusNoSolution);
    }
  }

  // the sample has passed its distance threshold check so add it
  PBL_LOG(LOG_LEVEL_DEBUG, "---> [%d] Adding %d %d %d \n",
      s_sample_idx, (int)sample[0], (int)sample[1], (int)sample[2]);
  s_sample_idx++;

  if (s_sample_idx != 4) {
    return (MagCalStatusNoSolution); // we need 4 pts before we can do a fit
  }

  // reset vars for next potential sphere fit
  s_samples_collected_for_fit = 0;
  s_sample_idx = 0;

  if (!sphere_fit(&solution[0])) {
    return (MagCalStatusNoSolution);
  }

  return (check_correction_value(solution, saved_corr));
}
