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

#include "gtypes.h"
#include "gtransform.h"
#include "util/trig.h"

#include <string.h>

//////////////////////////////////////
/// Creating Transforms
//////////////////////////////////////
// Note that int64_t casting is required since cos/sin values are already in a 32-bit fixed point
// representation and when passed to this function. They need to be scaled by the
// Fixed_S32_16 precision (16-bits) before dividing by the TRIG_MAX_RATIO. This multiply is what
// makes the int64_t casting necessary to avoid overflowing across 32-bits.
GTransform gtransform_init_rotation(int32_t angle) {
  if (angle != 0) {
    int32_t cosine = cos_lookup(angle);
    int32_t sine = sin_lookup(angle);
    int64_t cosine_val = (cosine * ((int64_t)GTransformNumberOne.raw_value)) / TRIG_MAX_RATIO;
    int64_t sine_val = (sine * ((int64_t)GTransformNumberOne.raw_value)) / TRIG_MAX_RATIO;
    GTransformNumber a = (GTransformNumber) { .raw_value = cosine_val };
    GTransformNumber b = (GTransformNumber) { .raw_value = -sine_val };
    GTransformNumber c = (GTransformNumber) { .raw_value = sine_val };
    GTransformNumber d = (GTransformNumber) { .raw_value = cosine_val };
    return GTransform(a, b, c, d, GTransformNumberZero, GTransformNumberZero);
  } else {
    return GTransformIdentity();
  }
}

//////////////////////////////////////
/// Evaluating Transforms
//////////////////////////////////////
bool gtransform_is_identity(const GTransform * const t) {
  if (!t) {
    return false;
  }

  GTransform t_c = GTransformIdentity();

  if (memcmp(t, &t_c, sizeof(GTransform)) == 0) {
    return true;
  }

  return false;
}

bool gtransform_is_only_scale(const GTransform * const t) {
  if (!t) {
    return false;
  }

  if ((t->b.raw_value == GTransformNumberZero.raw_value) &&
      (t->c.raw_value == GTransformNumberZero.raw_value) &&
      (t->tx.raw_value == GTransformNumberZero.raw_value) &&
      (t->ty.raw_value == GTransformNumberZero.raw_value)) {
    return true;
  }

  return false;
}

bool gtransform_is_only_translation(const GTransform * const t) {
  if (!t) {
    return false;
  }

  if ((t->a.raw_value == GTransformNumberOne.raw_value) &&
      (t->b.raw_value == GTransformNumberZero.raw_value) &&
      (t->c.raw_value == GTransformNumberZero.raw_value) &&
      (t->d.raw_value == GTransformNumberOne.raw_value)) {
    return true;
  }

  return false;
}

bool gtransform_is_only_scale_or_translation(const GTransform * const t) {
  if (!t) {
    return false;
  }

  if ((t->b.raw_value != GTransformNumberZero.raw_value) ||
      (t->c.raw_value != GTransformNumberZero.raw_value)) {
    return true;
  }

  return false;
}

bool gtransform_is_equal(const GTransform * const t1, const GTransform * const t2) {
  if ((!t1) || (!t2)) {
    return false;
  }

  return memcmp(t1, t2, sizeof(GTransform)) == 0;
}

//////////////////////////////////////
/// Modifying Transforms
//////////////////////////////////////
// Note that t_new can be set to either of t1 or t2 safely to do in place multiplication
// Note this operation is not commutative. The operation is as follows t_new = t1 * t2
void gtransform_concat(GTransform *t_new, const GTransform *t1, const GTransform * t2) {
  if ((!t_new) || (!t1) || (!t2)) {
    return;
  }

  Fixed_S32_16 a_a = Fixed_S32_16_mul(t1->a, t2->a);
  Fixed_S32_16 b_c = Fixed_S32_16_mul(t1->b, t2->c);

  Fixed_S32_16 a_b = Fixed_S32_16_mul(t1->a, t2->b);
  Fixed_S32_16 b_d = Fixed_S32_16_mul(t1->b, t2->d);

  Fixed_S32_16 c_a = Fixed_S32_16_mul(t1->c, t2->a);
  Fixed_S32_16 d_c = Fixed_S32_16_mul(t1->d, t2->c);

  Fixed_S32_16 c_b = Fixed_S32_16_mul(t1->c, t2->b);
  Fixed_S32_16 d_d = Fixed_S32_16_mul(t1->d, t2->d);

  Fixed_S32_16 tx_a = Fixed_S32_16_mul(t1->tx, t2->a);
  Fixed_S32_16 ty_c = Fixed_S32_16_mul(t1->ty, t2->c);

  Fixed_S32_16 tx_b = Fixed_S32_16_mul(t1->tx, t2->b);
  Fixed_S32_16 ty_d = Fixed_S32_16_mul(t1->ty, t2->d);

  t_new->a = Fixed_S32_16_add(a_a, b_c);
  t_new->b = Fixed_S32_16_add(a_b, b_d);
  t_new->c = Fixed_S32_16_add(c_a, d_c);
  t_new->d = Fixed_S32_16_add(c_b, d_d);
  t_new->tx = Fixed_S32_16_add3(tx_a, ty_c, t2->tx);
  t_new->ty = Fixed_S32_16_add3(tx_b, ty_d, t2->ty);
}

void gtransform_scale(GTransform *t_new, GTransform *t, GTransformNumber sx, GTransformNumber sy) {
  if ((!t_new) || (!t)) {
    return;
  }

  // Copy over t to t_new and update as necessary
  if (t_new != t) {
    memcpy(t_new, t, sizeof(GTransform));
  }

  // t_new = ts*t

  // Scale X vector (a and b)
  t_new->a = Fixed_S32_16_mul(sx, t->a);
  t_new->b = Fixed_S32_16_mul(sx, t->b);

  // Scale Y vector (c and d)
  t_new->c = Fixed_S32_16_mul(sy, t->c);
  t_new->d = Fixed_S32_16_mul(sy, t->d);
}

void gtransform_translate(GTransform *t_new, GTransform *t,
                          GTransformNumber tx, GTransformNumber ty) {
  if ((!t_new) || (!t)) {
    return;
  }

  // Copy over t to t_new and update as necessary
  if (t_new != t) {
    memcpy(t_new, t, sizeof(GTransform));
  }

  // t_new = tt*t
  Fixed_S32_16 tx_a = Fixed_S32_16_mul(tx, t->a);
  Fixed_S32_16 ty_c = Fixed_S32_16_mul(ty, t->c);

  Fixed_S32_16 tx_b = Fixed_S32_16_mul(tx, t->b);
  Fixed_S32_16 ty_d = Fixed_S32_16_mul(ty, t->d);

  t_new->tx = Fixed_S32_16_add3(tx_a, ty_c, t->tx);
  t_new->ty = Fixed_S32_16_add3(tx_b, ty_d, t->ty);
}

void gtransform_rotate(GTransform *t_new, GTransform *t, int32_t angle) {
  if ((!t_new) || (!t)) {
    return;
  }

  // t_new = tr*t
  GTransform tR = gtransform_init_rotation(angle);
  gtransform_concat(t_new, &tR, t);
}

bool gtransform_invert(GTransform *t_new, GTransform *t) {
  if ((!t_new) || (!t)) {
    return false;
  }

  memcpy(t_new, t, sizeof(GTransform));
  // FIXME: NYI - copy original into t_new for now
  return false;
}

//////////////////////////////////////
/// Applying Transformations
//////////////////////////////////////
GPointPrecise gpoint_transform(GPoint point, const GTransform * const t) {
  GPointPrecise pointP = GPointPreciseFromGPoint(point);

  if (!t) {
    return pointP;
  }

  Fixed_S16_3 x_a = Fixed_S16_3_S32_16_mul(pointP.x, t->a);
  Fixed_S16_3 y_c = Fixed_S16_3_S32_16_mul(pointP.y, t->c);
  Fixed_S16_3 one_tx = Fixed_S16_3_S32_16_mul(FIXED_S16_3_ONE, t->tx);

  Fixed_S16_3 x_b = Fixed_S16_3_S32_16_mul(pointP.x, t->b);
  Fixed_S16_3 y_d = Fixed_S16_3_S32_16_mul(pointP.y, t->d);
  Fixed_S16_3 one_ty = Fixed_S16_3_S32_16_mul(FIXED_S16_3_ONE, t->ty);

  Fixed_S16_3 sum_x = Fixed_S16_3_add3(x_a, y_c, one_tx);
  Fixed_S16_3 sum_y = Fixed_S16_3_add3(x_b, y_d, one_ty);

  return GPointPrecise(sum_x.raw_value, sum_y.raw_value);
}

GVectorPrecise gvector_transform(GVector vector, const GTransform * const t) {
  GVectorPrecise vectorP = GVectorPreciseFromGVector(vector);

  if (!t) {
    return vectorP;
  }

  Fixed_S16_3 x_a = Fixed_S16_3_S32_16_mul(vectorP.dx, t->a);
  Fixed_S16_3 y_c = Fixed_S16_3_S32_16_mul(vectorP.dy, t->c);
  Fixed_S16_3 one_tx = Fixed_S16_3_S32_16_mul(FIXED_S16_3_ONE, t->tx);

  Fixed_S16_3 x_b = Fixed_S16_3_S32_16_mul(vectorP.dx, t->b);
  Fixed_S16_3 y_d = Fixed_S16_3_S32_16_mul(vectorP.dy, t->d);
  Fixed_S16_3 one_ty = Fixed_S16_3_S32_16_mul(FIXED_S16_3_ONE, t->ty);

  Fixed_S16_3 sum_x = Fixed_S16_3_add3(x_a, y_c, one_tx);
  Fixed_S16_3 sum_y = Fixed_S16_3_add3(x_b, y_d, one_ty);

  return GVectorPrecise(sum_x.raw_value, sum_y.raw_value);
}
