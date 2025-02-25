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

#include "util/trig.h"

#include "util/math.h"


// lookup points on the first quarter sin wave
// generated by tintin/tools/trigTable.py
static const uint16_t SIN_LOOKUP[] = {
0,
401,
801,
1202,
1602,
2002,
2403,
2803,
3203,
3603,
4003,
4403,
4802,
5202,
5601,
6000,
6399,
6797,
7195,
7593,
7991,
8389,
8786,
9182,
9579,
9975,
10371,
10766,
11161,
11555,
11949,
12343,
12736,
13129,
13521,
13913,
14304,
14694,
15085,
15474,
15863,
16251,
16639,
17026,
17413,
17798,
18184,
18568,
18952,
19335,
19717,
20099,
20480,
20860,
21239,
21618,
21996,
22372,
22749,
23124,
23498,
23872,
24244,
24616,
24987,
25356,
25725,
26093,
26460,
26826,
27191,
27555,
27918,
28280,
28641,
29000,
29359,
29717,
30073,
30428,
30783,
31136,
31487,
31838,
32188,
32536,
32883,
33229,
33574,
33917,
34259,
34600,
34939,
35278,
35614,
35950,
36284,
36617,
36949,
37279,
37608,
37935,
38261,
38585,
38908,
39230,
39550,
39869,
40186,
40502,
40816,
41128,
41439,
41749,
42057,
42363,
42668,
42971,
43273,
43573,
43871,
44168,
44463,
44757,
45048,
45338,
45627,
45913,
46198,
46482,
46763,
47043,
47321,
47597,
47872,
48144,
48415,
48684,
48951,
49217,
49480,
49742,
50002,
50260,
50516,
50770,
51023,
51273,
51521,
51768,
52013,
52255,
52496,
52735,
52972,
53207,
53440,
53670,
53899,
54126,
54351,
54574,
54794,
55013,
55230,
55444,
55657,
55867,
56076,
56282,
56486,
56688,
56888,
57086,
57281,
57475,
57666,
57856,
58043,
58228,
58410,
58591,
58769,
58945,
59119,
59291,
59461,
59628,
59793,
59956,
60116,
60275,
60431,
60585,
60736,
60886,
61033,
61177,
61320,
61460,
61598,
61734,
61867,
61998,
62126,
62253,
62377,
62498,
62618,
62735,
62850,
62962,
63072,
63179,
63285,
63388,
63488,
63586,
63682,
63775,
63866,
63955,
64041,
64125,
64206,
64286,
64362,
64436,
64508,
64578,
64645,
64709,
64771,
64831,
64889,
64943,
64996,
65046,
65094,
65139,
65182,
65222,
65260,
65295,
65328,
65359,
65387,
65413,
65436,
65457,
65475,
65491,
65504,
65515,
65524,
65530,
65534,
65535
};

// lookup points on the first quarter atan wave
// generated by tintin/tools/trigTable.py
static const uint16_t ATAN_LOOKUP[] = {
0,
328,
648,
976,
1296,
1624,
1944,
2272,
2600,
2920,
3248,
3568,
3896,
4216,
4544,
4864,
5192,
5512,
5832,
6160,
6480,
6800,
7128,
7448,
7768,
8088,
8416,
8736,
9056,
9376,
9696,
10016,
10336,
10656,
10976,
11296,
11616,
11928,
12248,
12568,
12880,
13200,
13520,
13832,
14152,
14464,
14776,
15096,
15408,
15720,
16032,
16344,
16656,
16968,
17280,
17592,
17904,
18216,
18520,
18832,
19136,
19448,
19752,
20056,
20368,
20672,
20976,
21280,
21584,
21888,
22192,
22488,
22792,
23096,
23392,
23696,
23992,
24288,
24584,
24888,
25184,
25480,
25768,
26064,
26360,
26656,
26944,
27240,
27528,
27816,
28104,
28400,
28688,
28976,
29256,
29544,
29832,
30112,
30400,
30680,
30960,
31248,
31528,
31808,
32088,
32368,
32640,
32920,
33192,
33472,
33744,
34024,
34296,
34568,
34840,
35112,
35376,
35648,
35920,
36184,
36448,
36720,
36984,
37248,
37512,
37776,
38040,
38296,
38560,
38816,
39080,
39336,
39592,
39848,
40104,
40360,
40616,
40864,
41120,
41368,
41624,
41872,
42120,
42368,
42616,
42864,
43112,
43352,
43600,
43840,
44088,
44328,
44568,
44808,
45048,
45288,
45520,
45760,
45992,
46232,
46464,
46696,
46928,
47160,
47392,
47624,
47856,
48080,
48312,
48536,
48768,
48992,
49216,
49440,
49664,
49880,
50104,
50328,
50544,
50768,
50984,
51200,
51416,
51632,
51848,
52064,
52272,
52488,
52696,
52912,
53120,
53328,
53536,
53744,
53952,
54160,
54368,
54568,
54776,
54976,
55184,
55384,
55584,
55784,
55984,
56184,
56384,
56576,
56776,
56968,
57168,
57360,
57552,
57744,
57936,
58128,
58320,
58512,
58704,
58888,
59080,
59264,
59448,
59632,
59824,
60008,
60184,
60368,
60552,
60736,
60912,
61096,
61272,
61456,
61632,
61808,
61984,
62160,
62336,
62512,
62680,
62856,
63032,
63200,
63368,
63544,
63712,
63880,
64048,
64216,
64384,
64552,
64720,
64880,
65048,
65208,
65376,
65535,
};

int32_t sin_lookup(int32_t angle) {
  int32_t mult = 1;

  // modify the input angle and output multiplier for use in a first quadrant sine lookup
  if (angle < 0) {
    angle = -angle;
    mult = -mult;
  }
  if (angle >= TRIG_MAX_ANGLE) {
    angle %= TRIG_MAX_ANGLE;
  }
  if (angle >= TRIG_MAX_ANGLE / 2) {
    mult = -mult;
    angle -= TRIG_MAX_ANGLE / 2;
  }
  if (angle >= TRIG_MAX_ANGLE / 4 && angle <= TRIG_MAX_ANGLE / 2) {
    angle = TRIG_MAX_ANGLE / 2 - angle;
  }

  // if I can interpolate linearly
  int32_t lookup_angle =  angle * 4 / 0xff;
  if ((uint32_t)(lookup_angle + 1) < sizeof(SIN_LOOKUP) / sizeof(int32_t)) {
    return  mult * (SIN_LOOKUP[lookup_angle] + ((angle * 4) % 0xff) * (SIN_LOOKUP[lookup_angle + 1] - SIN_LOOKUP[lookup_angle]) / 0xff);
  }

  return mult * SIN_LOOKUP[lookup_angle];
}

int32_t cos_lookup(int32_t angle) {
  return sin_lookup(angle + TRIG_MAX_ANGLE / 4);
}

#define ATAN_LUT_STRIDE 0xff

int32_t atan2_lookup(int16_t y, int16_t x) {
  // inspired by http://www.coranac.com/documents/arctangent/
  if (y == 0) {
    return (x >= 0 ? 0 : TRIG_PI);
  }

  // moving x and y values into bigger containers to avoid overflows
  int32_t x_wide = x;
  int32_t y_wide = y;
  int32_t angle = 0;
  // Find octant
  if (y_wide < 0) {
    x_wide = -x_wide;
    y_wide = -y_wide;
    angle += 4;
  }
  if (x_wide <= 0) {
    uint32_t t = x_wide;
    x_wide = y_wide;
    y_wide = -t;
    angle += 2;
  }
  if (x_wide <= y_wide) {
    uint32_t t = y_wide - x_wide;
    x_wide = x_wide + y_wide;
    y_wide = t;
    angle += 1;
  }

  angle *= TRIG_PI / 4;
  uint32_t ratio = (y_wide << TRIG_FP) / x_wide;

  return (angle + ATAN_LOOKUP[ratio / ATAN_LUT_STRIDE] / 8);
}

uint32_t normalize_angle(int32_t angle) {
  uint32_t normalized_angle = ABS(angle) % TRIG_MAX_ANGLE;
  if (angle < 0) {
    normalized_angle = TRIG_MAX_ANGLE - normalized_angle;
  }
  return normalized_angle;
}
