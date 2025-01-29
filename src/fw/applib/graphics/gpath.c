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

#include "gpath.h"

#include "graphics.h"
#include "graphics_private.h"

#include "applib/applib_malloc.auto.h"
#include "applib/app_logging.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/swap.h"
#include "util/trig.h"

#include <string.h>
#include <stdlib.h>

#define GPATH_ERROR "Unable to allocate memory for GPath call"

void prv_fill_path_with_cb_aa(GContext *ctx, GPath *path, GPathDrawFilledCallback cb,
                              void *user_data);

typedef struct Intersection {
  Fixed_S16_3 x;
  Fixed_S16_3 delta;
} Intersection;

void gpath_init(GPath *path, const GPathInfo *init) {
  memset(path, 0, sizeof(GPath));
  path->num_points = init->num_points;
  path->points = init->points;
}

GPath* gpath_create(const GPathInfo *init) {
  // Can't pad this out because the definition itself is exported. Even if we did pad it out so
  // we can theoretically add members to the end of the struct, we'll still have to add compatibility
  // flags throughout here to check which size of struct the app is going to pass us through these
  // APIs.
  GPath* path = applib_malloc(sizeof(GPath));
  if (path) {
    gpath_init(path, init);
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, GPATH_ERROR);
  }
  return path;
}

void gpath_destroy(GPath* gpath) {
  applib_free(gpath);
}

static GPoint rotate_offset_point(const GPoint *orig, int32_t rotation, const GPoint *offset) {
  int32_t cosine = cos_lookup(rotation);
  int32_t sine = sin_lookup(rotation);
  GPoint result;
  result.x = (int32_t)orig->x * cosine / TRIG_MAX_RATIO - (int32_t)orig->y * sine / TRIG_MAX_RATIO + offset->x;
  result.y = (int32_t)orig->y * cosine / TRIG_MAX_RATIO + (int32_t)orig->x * sine / TRIG_MAX_RATIO + offset->y;
  return result;
}

static void sort16(int16_t *values, size_t length) {
  for (unsigned int i = 0; i < length; i++) {
    for (unsigned int j = i+1; j < length; j++) {
      if (values[i] > values[j]) {
        swap16(&values[i], &values[j]);
      }
    }
  }
}

static void swapIntersections(Intersection *a, Intersection *b) {
  Intersection t = *a;
  *a = *b;
  *b = t;
}

static void sortIntersections(Intersection *values, size_t length) {
  for (unsigned int i = 0; i < length; i++) {
    for (unsigned int j = i+1; j < length; j++) {
      if (values[i].x.raw_value > values[j].x.raw_value) {
        swapIntersections(&values[i], &values[j]);
      }
    }
  }
}

static inline bool prv_is_in_range(int16_t min_a, int16_t max_a, int16_t min_b, int16_t max_b) {
  return (max_a >= min_b) && (min_a <= max_b);
}

static void prv_gpath_draw_filled_cb(GContext *ctx, int16_t y,
                                     Fixed_S16_3 x_range_begin, Fixed_S16_3 x_range_end,
                                     Fixed_S16_3 delta_begin, Fixed_S16_3 delta_end,
                                     void *user_data) {

#if PBL_COLOR
  // We know that correct delta is always positive, and treat that as an input from
  // antialiased function, otherwise its treated as non-AA
  if (delta_begin.raw_value >= 0 || delta_end.raw_value >= 0) {
    x_range_begin.integer++;
    x_range_end.integer--;

    graphics_private_draw_horizontal_line_delta_aa(
        ctx, y, x_range_begin, x_range_end, delta_begin, delta_end);
    return;
  }
#endif
  graphics_fill_rect(
      ctx, &(GRect) { { x_range_begin.integer + 1, y },
                      { x_range_end.integer - x_range_begin.integer - 1, 1 } });
}

void gpath_draw_filled(GContext* ctx, GPath* path) {
#if PBL_COLOR
  // This algorithm makes sense only in 8bit mode...
  if (ctx->draw_state.antialiased) {
    prv_fill_path_with_cb_aa(ctx, path, prv_gpath_draw_filled_cb, NULL);
    return;
  }
#endif

  gpath_draw_filled_with_cb(ctx, path, prv_gpath_draw_filled_cb, NULL);
}

void gpath_draw_outline(GContext* ctx, GPath* path) {
  gpath_draw_stroke(ctx, path, false);
}

void gpath_draw_outline_open(GContext* ctx, GPath* path) {
  gpath_draw_stroke(ctx, path, true);
}

void gpath_draw_stroke(GContext* ctx, GPath* path, bool open) {
  if (!path || path->num_points < 2) {
    return;
  }
  // for each line segment (do not draw line returning to the first point if open is true)
  for (uint32_t i = 0; i < (open ? (path->num_points - 1) : path->num_points); ++i) {
    int i2 = (i + 1) % path->num_points;

    GPoint rot_start = rotate_offset_point(&path->points[i], path->rotation, &path->offset);
    GPoint rot_end = rotate_offset_point(&path->points[i2], path->rotation, &path->offset);

    graphics_draw_line(ctx, rot_start, rot_end);
  }
}

void gpath_rotate_to(GPath *path, int32_t angle) {
  if (!path) {
    return;
  }
  path->rotation = angle % TRIG_MAX_ANGLE;
}

void gpath_move_to(GPath *path, GPoint point) {
  if (!path) {
    return;
  }
  path->offset = point;
}

void gpath_move(GPath *path, GPoint delta) {
  if (!path) {
    return;
  }
  path->offset.x += delta.x;
  path->offset.y += delta.y;
}

GRect gpath_outer_rect(GPath *path) {
  if (!path) {
    return GRect(0, 0, 0, 0);
  }

  int16_t max_x = INT16_MIN;
  int16_t min_x = INT16_MAX;
  int16_t max_y = INT16_MIN;
  int16_t min_y = INT16_MAX;
  for (uint32_t i = 0; i < path->num_points; ++i) {
    if (path->points[i].x > max_x) {
      max_x = path->points[i].x;
    }
    if (path->points[i].x < min_x) {
      min_x = path->points[i].x;
    }
    if (path->points[i].y > max_y) {
      max_y = path->points[i].y;
    }
    if (path->points[i].y < min_y) {
      min_y = path->points[i].y;
    }
  }
  return GRect(min_x, min_y, (max_x - min_x), (max_y - min_y));
}

#if PBL_COLOR
void prv_fill_path_with_cb_aa(GContext *ctx, GPath *path, GPathDrawFilledCallback cb,
                              void *user_data) {
  /*
   * Filling gpaths with antialiasing for integral-coordinates based paths:
   *
   * Custom linescanner using simple mathematic trick to determine anti-aliased edges
   *  1. Rotate all points in path
   *  2. Progress line-by-line finding intersections with paths
   *  2.1 Calculate delta (angle) of the intersecting lines
   *  2.2 Sort intersections
   *  2.3 Draw lines between intersections
   *
   * This algorithm relies on few tricks:
   *  - For intersections with delta less than 1 (angle is less than 45°) we will use exact
   *      position of the intersection and fill edge pixel based on that information
   *  - For intersections with delta bigger than 1 (angle is bigger than 45°) we will use delta to
   *      draw gradient line responding to the angle
   *      + If gradient is bigger than distance from the start/end of the intersecting line
   *          we will adjust the delta to match starting/ending point and avoid nasty
   *          gradients diving in/out the path
   *      + Gradients too close to clipping rect will be properly cut off
   */


  // Protect against apps calling with no points to draw (Upright watchface)
  if (!path || path->num_points < 2) {
    return;
  }

  GPointPrecise* rot_points = applib_malloc(path->num_points * sizeof(GPointPrecise));
  if (!rot_points) {
    return;
  }

  int min_x, max_x, min_y, max_y;
  GPointPrecise rot_start, rot_end;
  bool found_start_direction = false;
  bool start_is_down = false;
  Intersection *intersections_up = NULL;
  Intersection *intersections_down = NULL;

  rot_points[0] = rot_end = GPointPreciseFromGPoint(
      rotate_offset_point(&path->points[0], path->rotation, &path->offset));
  min_x = max_x = rot_points[0].x.integer;
  min_y = max_y = rot_points[0].y.integer;

  // begin finding the last path segment's direction going backwards through the path
  // we must go backwards because we find intersections going forwards
  for (int i = path->num_points - 1; i > 0; --i) {
    rot_points[i] = rot_start = GPointPreciseFromGPoint(
        rotate_offset_point(&path->points[i], path->rotation, &path->offset));
    if (min_x > rot_points[i].x.integer) { min_x = rot_points[i].x.integer; }
    if (max_x < rot_points[i].x.integer) { max_x = rot_points[i].x.integer; }
    if (min_y > rot_points[i].y.integer) { min_y = rot_points[i].y.integer; }
    if (max_y < rot_points[i].y.integer) { max_y = rot_points[i].y.integer; }

    if (found_start_direction) {
      continue;
    }

    // use the first non-horizontal path segment's direction as the start direction
    if (rot_end.y.integer != rot_start.y.integer) {
      start_is_down = rot_end.y.integer > rot_start.y.integer;
      found_start_direction = true;
    }

    rot_end = rot_start;
  }

  const int16_t clip_min_x = ctx->draw_state.clip_box.origin.x
      - ctx->draw_state.drawing_box.origin.x;
  const int16_t clip_max_x = ctx->draw_state.clip_box.size.w + clip_min_x;
  if (!prv_is_in_range(min_x, max_x, clip_min_x, clip_max_x)) {
    goto cleanup;
  }

  // x-intersections of path segments whose direction is up
  intersections_up = applib_zalloc(path->num_points * sizeof(Intersection));
  // x-intersections of path segments whose direction is down
  intersections_down = applib_zalloc(path->num_points * sizeof(Intersection));

  // If either malloc failed, log message and cleanup
  if (!intersections_up || !intersections_down) {
    APP_LOG(APP_LOG_LEVEL_ERROR, GPATH_ERROR);
    goto cleanup;
  }

  int intersection_up_count;
  int intersection_down_count;

  // convert clip coordinates to drawing coordinates
  const int16_t clip_min_y = ctx->draw_state.clip_box.origin.y
      - ctx->draw_state.drawing_box.origin.y;
  const int16_t clip_max_y = ctx->draw_state.clip_box.size.h + clip_min_y;
  min_y = MAX(min_y, clip_min_y);
  max_y = MIN(max_y, clip_max_y);

  // filling color hack
  GColor tmp = ctx->draw_state.stroke_color;
  ctx->draw_state.stroke_color = ctx->draw_state.fill_color;

  // find all of the horizontal intersections and draw them
  for (int16_t i = min_y; i <= max_y; ++i) {
    // initialize with 0 intersections
    intersection_down_count = 0;
    intersection_up_count = 0;

    // horizontal path segments don't have a direction and depend
    // upon the last path segment's direction
    // keep track of the last path direction for horizontal path segments to use
    bool last_is_down = start_is_down;
    rot_end = rot_points[0];

    // find the intersections
    for (uint32_t j = 0; j < path->num_points; ++j) {
      rot_start = rot_points[j];
      if (j + 1 < path->num_points) {
        rot_end = rot_points[j + 1];
      } else {
        // wrap to the first point
        rot_end = rot_points[0];
      }

      // if the line is on/crosses this height
      if ((rot_start.y.integer - i) * (rot_end.y.integer - i) <= 0) {
        bool is_down = rot_end.y.integer != rot_start.y.integer ?
                       rot_end.y.integer > rot_start.y.integer : last_is_down;
        // don't count end points in the same direction to avoid double intersections
        if (!(rot_start.y.integer == i && last_is_down == is_down)) {
          // linear interpolation of the line intersection

          int16_t delta_x = rot_end.x.raw_value - rot_start.x.raw_value;
          int16_t delta_y = rot_end.y.raw_value - rot_start.y.raw_value;

          Fixed_S16_3 x = (Fixed_S16_3){.raw_value = rot_start.x.raw_value + delta_x
                          * (i * FIXED_S16_3_ONE.raw_value - rot_start.y.raw_value) / delta_y};

          Fixed_S16_3 delta = (Fixed_S16_3){.raw_value = ABS(delta_x / delta_y) *
                                                          FIXED_S16_3_ONE.raw_value};

          if (delta.integer > 1) {
            // this is where we try to fix edges diving in and out of paths
            int16_t min_x = rot_end.x.raw_value < rot_start.x.raw_value ?
                              rot_end.x.raw_value : rot_start.x.raw_value;
            int16_t max_x = rot_end.x.raw_value > rot_start.x.raw_value ?
                              rot_end.x.raw_value : rot_start.x.raw_value;

            if (x.raw_value - (delta.raw_value / 2) < min_x) {
              delta.raw_value = (x.raw_value - min_x) * 2;
            }

            if (x.raw_value + (delta.raw_value / 2) > max_x) {
              delta.raw_value = (max_x - x.raw_value) * 2;
            }
          }

          if (is_down) {
            intersections_down[intersection_down_count].x.raw_value = x.raw_value;
            intersections_down[intersection_down_count].delta = delta;
            intersection_down_count++;
          } else {
            intersections_up[intersection_up_count].x.raw_value = x.raw_value;
            intersections_up[intersection_up_count].delta = delta;
            intersection_up_count++;
          }
        }
        last_is_down = is_down;
      }
    }

    // sort the intersections
    sortIntersections(intersections_up, intersection_up_count);
    sortIntersections(intersections_down, intersection_down_count);

    // draw the line segments
    for (int j = 0; j < MIN(intersection_up_count, intersection_down_count); j++) {
      Intersection x_a = intersections_up[j];
      Intersection x_b = intersections_down[j];
      if (x_a.x.integer != x_b.x.integer) {
        if (x_a.x.integer > x_b.x.integer) {
          swapIntersections(&x_a, &x_b);
        }
        // this is done by callback now...
        // x_a.x.integer++;
        // x_b.x.integer--;

        cb(ctx, i, x_a.x, x_b.x, x_a.delta, x_b.delta, user_data);
      }
    }
  }

  // restore original stroke color
  ctx->draw_state.stroke_color = tmp;

cleanup:
  applib_free(rot_points);
  applib_free(intersections_up);
  applib_free(intersections_down);
}
#endif // PBL_COLOR

void gpath_draw_filled_with_cb(GContext *ctx, GPath *path, GPathDrawFilledCallback cb,
                               void *user_data) {
  //Protect against apps calling with no points to draw (Upright watchface)
  if (!path || path->num_points < 2) {
    return;
  }

  GPoint* rot_points = applib_malloc(path->num_points * sizeof(GPoint));
  if (!rot_points) {
    APP_LOG(APP_LOG_LEVEL_ERROR, GPATH_ERROR);
    return;
  }

  int min_x, max_x, min_y, max_y;
  GPoint rot_start, rot_end;
  bool found_start_direction = false;
  bool start_is_down = false;
  int16_t *intersections_up = NULL;
  int16_t *intersections_down = NULL;

  rot_points[0] = rot_end = rotate_offset_point(&path->points[0], path->rotation, &path->offset);
  min_x = max_x = rot_points[0].x;
  min_y = max_y = rot_points[0].y;

  // begin finding the last path segment's direction going backwards through the path
  // we must go backwards because we find intersections going forwards
  for (int i = path->num_points - 1; i > 0; --i) {
    rot_points[i] = rot_start = rotate_offset_point(&path->points[i], path->rotation, &path->offset);
    if (min_x > rot_points[i].x) { min_x = rot_points[i].x; }
    if (max_x < rot_points[i].x) { max_x = rot_points[i].x; }
    if (min_y > rot_points[i].y) { min_y = rot_points[i].y; }
    if (max_y < rot_points[i].y) { max_y = rot_points[i].y; }

    if (found_start_direction) {
      continue;
    }

    // use the first non-horizontal path segment's direction as the start direction
    if (rot_end.y != rot_start.y) {
      start_is_down = rot_end.y > rot_start.y;
      found_start_direction = true;
    }

    rot_end = rot_start;
  }


  const int16_t clip_min_x = ctx->draw_state.clip_box.origin.x
      - ctx->draw_state.drawing_box.origin.x;
  const int16_t clip_max_x = ctx->draw_state.clip_box.size.w + clip_min_x;
  if (!prv_is_in_range(min_x, max_x, clip_min_x, clip_max_x)) {
    goto cleanup;
  }

  // x-intersections of path segments whose direction is up
  intersections_up = applib_zalloc(path->num_points * sizeof(int16_t));
  // x-intersections of path segments whose direction is down
  intersections_down = applib_zalloc(path->num_points * sizeof(int16_t));

  // If either malloc failed, log message and cleanup
  if (!intersections_up || !intersections_down) {
    APP_LOG(APP_LOG_LEVEL_ERROR, GPATH_ERROR);
    goto cleanup;
  }

  int intersection_up_count;
  int intersection_down_count;

  const int16_t clip_min_y = ctx->draw_state.clip_box.origin.y
      - ctx->draw_state.drawing_box.origin.y;
  const int16_t clip_max_y = ctx->draw_state.clip_box.size.h + clip_min_y;
  min_y = MAX(min_y, clip_min_y);
  max_y = MIN(max_y, clip_max_y);

  // find all of the horizontal intersections and draw them
  for (int16_t i = min_y; i <= max_y; ++i) {
    // initialize with 0 intersections
    intersection_down_count = 0;
    intersection_up_count = 0;

    // horizontal path segments don't have a direction and depend upon the last path segment's direction
    // keep track of the last path direction for horizontal path segments to use
    bool last_is_down = start_is_down;
    rot_end = rot_points[0];

    // find the intersections
    for (uint32_t j = 0; j < path->num_points; ++j) {
      rot_start = rot_points[j];
      if (j + 1 < path->num_points) {
        rot_end = rot_points[j + 1];
      } else {
        // wrap to the first point
        rot_end = rot_points[0];
      }

      // if the line is on/crosses this height
      if ((rot_start.y - i) * (rot_end.y - i) <= 0) {
        bool is_down = rot_end.y != rot_start.y ? rot_end.y > rot_start.y : last_is_down;
        // don't count end points in the same direction to avoid double intersections
        if (!(rot_start.y == i && last_is_down == is_down)) {
          // linear interpolation of the line intersection
          int16_t x = rot_start.x + (rot_end.x - rot_start.x) * (i - rot_start.y) / (rot_end.y - rot_start.y);
          if (is_down) {
            intersections_down[intersection_down_count] = x;
            intersection_down_count++;
          } else {
            intersections_up[intersection_up_count] = x;
            intersection_up_count++;
          }
        }
        last_is_down = is_down;
      }
    }

    // sort the intersections
    sort16(intersections_up, intersection_up_count);
    sort16(intersections_down, intersection_down_count);

    // draw the line segments
    for (int j = 0; j < MIN(intersection_up_count, intersection_down_count); j++) {
      int16_t x_a = intersections_up[j];
      int16_t x_b = intersections_down[j];
      if (x_a != x_b) {
        if (x_a > x_b) {
          swap16(&x_a, &x_b);
        }
        cb(ctx, i, (Fixed_S16_3){.integer = x_a}, (Fixed_S16_3){.integer = x_b},
           (Fixed_S16_3){.integer = -1}, (Fixed_S16_3){.integer = -1}, user_data);
      }
    }
  }
cleanup:
  applib_free(rot_points);
  applib_free(intersections_up);
  applib_free(intersections_down);
}

void gpath_fill_precise_internal(GContext *ctx, GPointPrecise *points, size_t num_points) {
  if (!points) {
    return;
  }

  // Convert precise points to normal points and draw filled path with converted points
  // (no real support for filled paths with GPointPrecise, yet)
  GPoint *imprecise_points = applib_malloc(sizeof(GPoint) * num_points);
  if (!imprecise_points) {
    APP_LOG(APP_LOG_LEVEL_ERROR, GPATH_ERROR);
    return;
  }

  for (size_t i = 0; i < num_points; i++) {
    imprecise_points[i] = GPointFromGPointPrecise(points[i]);
  }
  GPath path = {
    .num_points = (uint32_t)num_points,
    .points = imprecise_points
  };
  gpath_draw_filled(ctx, &path);

  applib_free(imprecise_points);
}

void gpath_draw_outline_precise_internal(GContext *ctx, GPointPrecise *points, size_t num_points,
                                         bool open) {
  if (!points) {
    return;
  }

  // draw precise path (no real support currently for paths with GPointPrecise)
  for (uint16_t i = 0; i < (open ? (num_points - 1) : num_points); ++i) {
    size_t i2 = (i + 1) % num_points;
    graphics_line_draw_precise_stroked(ctx, points[i], points[i2]);
  }
}
