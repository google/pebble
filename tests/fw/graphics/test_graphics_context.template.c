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

#include "applib/graphics/gtypes.h"
#include "applib/graphics/graphics.h"
#include "util/attributes.h"

#include "clar.h"
#include "util.h"


// Helper Functions
////////////////////////////////////
#include "test_graphics.h"
#include "${BIT_DEPTH_NAME}/test_framebuffer.h"

// Stubs
////////////////////////////////////
#include "graphics_common_stubs.h"
#include "stubs_applib_resource.h"

// Definitions
#define SW_EVEN 4
#define SW_ODD 5

// Includes all possible fields for all shapes
// The fields that are not needed for a particular shape will be zeroed out.
typedef struct PACKED {
  char func[30];
  GContext ctx;
  GPoint p0;
  GPoint p1;
  GRect r0;
  uint16_t radius;
  uint8_t quadrant;
  GCornerMask corner_mask;
  int16_t major_axis_offset;
  Fixed_S16_3 offset_start;
  Fixed_S16_3 offset_stop;
  bool anti_aliased;
  uint16_t brightness;
  GColor color;
  GBitmap fb;
} ArgsForMock;

static ArgsForMock s_last_args_for_mock;
static GContext context;
static FrameBuffer *fb = NULL;

/////////////////////////////////////
/// FUNCTION OVERRIDES
/////////////////////////////////////
void graphics_line_draw_1px_non_aa(GContext* ctx, GPoint p0, GPoint p1) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p0,
    .p1 = p1
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_line_draw_1px_aa(GContext* ctx, GPoint p0, GPoint p1) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p0,
    .p1 = p1
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_line_draw_stroked_aa(GContext* ctx, GPoint p0, GPoint p1, uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p0,
    .p1 = p1
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_line_draw_stroked_non_aa(GContext* ctx, GPoint p0, GPoint p1, uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p0,
    .p1 = p1
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_fill_rect_non_aa(GContext* ctx, const GRect *rect, uint16_t radius,
                          GCornerMask corner_mask, GColor fill_color) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = radius,
    .corner_mask = corner_mask
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_fill_rect_aa(GContext* ctx, const GRect *rect, uint16_t radius,
                      GCornerMask corner_mask, GColor fill_color) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = radius,
    .corner_mask = corner_mask
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_draw_rect(GContext *ctx, const GRect *rect) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = 0,
    .corner_mask = GCornerNone
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_draw_rect_aa_stroked(GContext *ctx, const GRect *rect, uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = 0,
    .corner_mask = GCornerNone
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_draw_rect_stroked(GContext *ctx, const GRect *rect, uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = 0,
    .corner_mask = GCornerNone
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_circle_draw_1px_non_aa(GContext* ctx, GPoint p, uint16_t radius) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p,
    .radius = radius
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_circle_draw_1px_aa(GContext* ctx, GPoint p, uint16_t radius) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p,
    .radius = radius
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_circle_draw_stroked_aa(GContext* ctx, GPoint p, uint16_t radius, uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p,
    .radius = radius
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_circle_draw_stroked_non_aa(GContext* ctx, GPoint p, uint16_t radius, uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p,
    .radius = radius
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_internal_circle_quadrant_fill_aa(GContext* ctx, GPoint p,
                                               uint16_t radius, GCornerMask quadrant) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p,
    .radius = radius,
    .corner_mask = quadrant
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void graphics_circle_fill_non_aa(GContext* ctx, GPoint p, uint16_t radius) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .p0 = p,
    .radius = radius
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_draw_round_rect(GContext* ctx, const GRect *rect, uint16_t radius) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = radius,
    .corner_mask = GCornersAll
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_draw_round_rect_aa(GContext* ctx, const GRect *rect, uint16_t radius) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = radius,
    .corner_mask = GCornersAll
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_draw_round_rect_aa_stroked(GContext* ctx, const GRect *rect, uint16_t radius,
                                    uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = radius,
    .corner_mask = GCornersAll
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

void prv_draw_round_rect_stroked(GContext* ctx, const GRect *rect, uint16_t radius, uint8_t stroke_width) {
  s_last_args_for_mock = (ArgsForMock){
    .ctx = *ctx,
    .r0 = *rect,
    .radius = radius,
    .corner_mask = GCornersAll
  };
  strncpy(s_last_args_for_mock.func, __func__, sizeof(s_last_args_for_mock.func));
}

/////////////////////////////////////
/// HELPER FUNCTIONS
/////////////////////////////////////
// Copy over the last arguments - take the largest possible shape
static void copy_last_args_for_mock(ArgsForMock *actual_args, ArgsForMock *last_args) {
  memcpy(actual_args, last_args, sizeof(ArgsForMock));
}

// Zero out the arguments from the last run
static void reset_last_args_for_mock() {
  memset(&s_last_args_for_mock, 0x00, sizeof(s_last_args_for_mock));
}

// Validate the arguments based on the shape that is drawn
static bool validate_args(ArgsForMock *actual_args, ArgsForMock *valid_args) {
  if (memcmp(actual_args, valid_args, sizeof(ArgsForMock)) == 0) {
    return true;
  }

  return false;
}

// This macro will call the expected code_block and then validate the arguments set in code_block
// match what was expected from the last mock call (i.e. if graphics_draw_line was called, then
// internally it would call prv_draw_line... and the actual_args would be set to the arguments
// that are set in prv_draw_line... so when compared to directly calling prv_draw_line..., the two
// should match.
#define ASSERT_CALLED(code_block) \
  do {\
    ArgsForMock actual_args; \
    copy_last_args_for_mock(&actual_args, &s_last_args_for_mock); \
    reset_last_args_for_mock(); \
    code_block; \
    bool cmp_result = validate_args(&actual_args, &s_last_args_for_mock); \
    printf("1 %d %d %d %d\n", actual_args.r0.origin.x, actual_args.r0.origin.y, \
                              actual_args.r0.size.w, actual_args.r0.size.h); \
    actual_args = s_last_args_for_mock; \
    printf("2 %d %d %d %d\n", actual_args.r0.origin.x, actual_args.r0.origin.y, \
                              actual_args.r0.size.w, actual_args.r0.size.h); \
    cl_assert(cmp_result);  \
  } while(0)

// This is used to make sure nothing has changed when calling code_block
// i.e. no prv_... functions have been called
// If any prv_... functions are called then s_last_args_for_mock will be updated
#define ASSERT_NO_CHANGE(code_block) \
  do {\
    ArgsForMock actual_args; \
    reset_last_args_for_mock(); \
    copy_last_args_for_mock(&actual_args, &s_last_args_for_mock); \
    code_block; \
    bool cmp_result = validate_args(&actual_args, &s_last_args_for_mock); \
    cl_assert(cmp_result);  \
  } while(0)

static void setup_test(GContext* ctx, bool antialiased, uint8_t stroke_width, GColor stroke_color,
                       GColor fill_color, bool lock) {
  graphics_context_set_antialiased(ctx, antialiased);
  graphics_context_set_stroke_width(ctx, stroke_width);
  graphics_context_set_stroke_color(ctx, stroke_color);
  graphics_context_set_fill_color(ctx, fill_color);
  ctx->lock = lock;
  reset_last_args_for_mock();
}

// Setup
void test_graphics_context_${BIT_DEPTH_NAME}__initialize(void) {
  fb = malloc(sizeof(FrameBuffer));
  framebuffer_init(fb, &(GSize) {DISP_COLS, DISP_ROWS});
  test_graphics_context_init(&context, fb);
}

// Teardown
void test_graphics_context_${BIT_DEPTH_NAME}__cleanup(void) {
  free(fb);
}

/////////////////////////////////////
/// TESTS
/////////////////////////////////////
void test_graphics_context_${BIT_DEPTH_NAME}__set(void) {
  GDrawState draw_state;
  GColor color;

  // Stroke Color
  graphics_context_set_stroke_color(&context, GColorClear);
  cl_assert_equal_i(context.draw_state.stroke_color.argb, GColorClear.argb);

  color = GColorBlue;
  graphics_context_set_stroke_color(&context, color);
#if PBL_COLOR
  cl_assert_equal_i(context.draw_state.stroke_color.argb, GColorBlue.argb);
#else
  cl_assert_equal_i(context.draw_state.stroke_color.argb, GColorBlack.argb);
#endif

  color.a = 2;
  graphics_context_set_stroke_color(&context, color);
#if PBL_COLOR
  cl_assert_equal_i(context.draw_state.stroke_color.argb, GColorBlue.argb);
#else
  cl_assert_equal_i(context.draw_state.stroke_color.argb, GColorBlack.argb);
#endif

  color.a = 1;
  graphics_context_set_stroke_color(&context, color);
  cl_assert_equal_i(context.draw_state.stroke_color.argb, GColorClear.argb);

  color.a = 0;
  graphics_context_set_stroke_color(&context, color);
  cl_assert_equal_i(context.draw_state.stroke_color.argb, GColorClear.argb);

  // Stroke Color - 2-bit
  graphics_context_set_stroke_color_2bit(&context, GColor2Black);
  cl_assert(gcolor_equal(context.draw_state.stroke_color, GColorBlack));

  // Fill Color
  graphics_context_set_fill_color(&context, GColorClear);
  cl_assert_equal_i(context.draw_state.fill_color.argb, GColorClear.argb);

  color = GColorOrange;
  graphics_context_set_fill_color(&context, color);
#if PBL_COLOR
  cl_assert_equal_i(context.draw_state.fill_color.argb, GColorOrange.argb);
#else
  cl_assert_equal_i(context.draw_state.fill_color.argb, GColorDarkGray.argb);
#endif

  color.a = 2;
  graphics_context_set_fill_color(&context, color);
#if PBL_COLOR
  cl_assert_equal_i(context.draw_state.fill_color.argb, GColorOrange.argb);
#else
  cl_assert_equal_i(context.draw_state.fill_color.argb, GColorDarkGray.argb);
#endif

  color.a = 1;
  graphics_context_set_fill_color(&context, color);
  cl_assert_equal_i(context.draw_state.fill_color.argb, GColorClear.argb);

  color.a = 0;
  graphics_context_set_fill_color(&context, color);
  cl_assert_equal_i(context.draw_state.fill_color.argb, GColorClear.argb);

  // Fill Color - 2-bit
  graphics_context_set_fill_color_2bit(&context, GColor2White);
  cl_assert(gcolor_equal(context.draw_state.fill_color, GColorWhite));

  // Compositing Mode
  graphics_context_set_compositing_mode(&context, GCompOpOr);
  cl_assert(context.draw_state.compositing_mode == GCompOpOr);

  // Text Color
  graphics_context_set_text_color(&context, GColorClear);
  cl_assert_equal_i(context.draw_state.text_color.argb, GColorClear.argb);

  color = GColorYellow;
  graphics_context_set_text_color(&context, color);
#if PBL_COLOR
  cl_assert_equal_i(context.draw_state.text_color.argb, GColorYellow.argb);
#else
  cl_assert_equal_i(context.draw_state.text_color.argb, GColorWhite.argb);
#endif

  color.a = 2;
  graphics_context_set_text_color(&context, color);
#if PBL_COLOR
  cl_assert_equal_i(context.draw_state.text_color.argb, GColorYellow.argb);
#else
  cl_assert_equal_i(context.draw_state.text_color.argb, GColorWhite.argb);
#endif

  color.a = 1;
  graphics_context_set_text_color(&context, color);
  cl_assert_equal_i(context.draw_state.text_color.argb, GColorClear.argb);

  color.a = 0;
  graphics_context_set_text_color(&context, color);
  cl_assert_equal_i(context.draw_state.text_color.argb, GColorClear.argb);

  // Text Color - 2-bit
  graphics_context_set_text_color_2bit(&context, GColor2White);
  cl_assert(gcolor_equal(context.draw_state.text_color, GColorWhite));

#if PBL_COLOR
  // Antialiased
  graphics_context_set_antialiased(&context, true);
  cl_assert(context.draw_state.antialiased == true);
#endif
  
  // Stroke Width
  graphics_context_set_stroke_width(&context, 11);
  cl_assert(context.draw_state.stroke_width == 11);

  // Make sure setting to zero is ignored
  draw_state = graphics_context_get_drawing_state(&context);
  graphics_context_set_stroke_width(&context, 0);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);

  // Check draw state
  memset(&draw_state, 0x5A, sizeof(GDrawState));
  graphics_context_set_drawing_state(&context, draw_state);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);
}

void test_graphics_context_${BIT_DEPTH_NAME}__draw_antialiased(void) {
  // Stroke width = 1, antialiased
  setup_test(&context, true, 1, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
#if PBL_COLOR
  ASSERT_CALLED(graphics_line_draw_1px_aa(&context, GPoint(5, 5), GPoint(45, 10)));
#else
  ASSERT_CALLED(graphics_line_draw_1px_non_aa(&context, GPoint(5, 5), GPoint(45, 10)));
#endif

  setup_test(&context, true, 1, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
  ASSERT_CALLED(prv_draw_rect(&context, &GRect(10, 20, 40, 10)));

  setup_test(&context, true, 1, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
#if PBL_COLOR
  ASSERT_CALLED(graphics_circle_draw_1px_aa(&context, GPoint(50, 50), 10));
#else
  ASSERT_CALLED(graphics_circle_draw_1px_non_aa(&context, GPoint(50, 50), 10));
#endif

  setup_test(&context, true, 1, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
#if PBL_COLOR
  ASSERT_CALLED(prv_draw_round_rect_aa(&context, &GRect(20, 80, 40, 10), 4));
#else
  ASSERT_CALLED(prv_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));
#endif
}


void test_graphics_context_${BIT_DEPTH_NAME}__draw_stroke_width_1(void) {
  // Stroke width 1, non-antialiased
  setup_test(&context, false, 1, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
  ASSERT_CALLED(graphics_line_draw_1px_non_aa(&context, GPoint(5, 5), GPoint(45, 10)));

  setup_test(&context, false, 1, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
  ASSERT_CALLED(prv_draw_rect(&context, &GRect(10, 20, 40, 10)));

  setup_test(&context, false, 1, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
  ASSERT_CALLED(graphics_circle_draw_1px_non_aa(&context, GPoint(50, 50), 10));

  setup_test(&context, false, 1, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
  ASSERT_CALLED(prv_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));
}

void test_graphics_context_${BIT_DEPTH_NAME}__draw_stroke_width_2(void) {
  // Stroke width 2, non-antialiased
  setup_test(&context, false, 2, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
  ASSERT_CALLED(graphics_line_draw_stroked_non_aa(&context, GPoint(5, 5), GPoint(45, 10), 2));

  setup_test(&context, false, 2, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
  ASSERT_CALLED(prv_draw_rect(&context, &GRect(10, 20, 40, 10)));

  setup_test(&context, false, 2, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
  ASSERT_CALLED(graphics_circle_draw_stroked_non_aa(&context, GPoint(50, 50), 10, 2));

  setup_test(&context, false, 2, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
  ASSERT_CALLED(prv_draw_round_rect_stroked(&context, &GRect(20, 80, 40, 10), 4, 2));
}

void test_graphics_context_${BIT_DEPTH_NAME}__draw_stroke_width_even(void) {
  // Stroke width even > 2, non-antialiased
  setup_test(&context, false, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
  ASSERT_CALLED(graphics_line_draw_stroked_non_aa(&context, GPoint(5, 5), GPoint(45, 10), SW_EVEN));

  setup_test(&context, false, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
  ASSERT_CALLED(prv_draw_rect_stroked(&context, &GRect(10, 20, 40, 10), SW_EVEN));

  setup_test(&context, false, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
  ASSERT_CALLED(graphics_circle_draw_stroked_non_aa(&context, GPoint(50, 50), 10, SW_EVEN));

  setup_test(&context, false, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
  ASSERT_CALLED(prv_draw_round_rect_stroked(&context, &GRect(20, 80, 40, 10), 4, SW_EVEN));
}

void test_graphics_context_${BIT_DEPTH_NAME}__draw_stroke_width_odd(void) {
  // Stroke width odd > 1, non-antialiased
  setup_test(&context, false, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
  ASSERT_CALLED(graphics_line_draw_stroked_non_aa(&context, GPoint(5, 5), GPoint(45, 10), SW_ODD));

  setup_test(&context, false, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
  ASSERT_CALLED(prv_draw_rect_stroked(&context, &GRect(10, 20, 40, 10), SW_ODD));

  setup_test(&context, false, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
  ASSERT_CALLED(graphics_circle_draw_stroked_non_aa(&context, GPoint(50, 50), 10, SW_ODD));

  setup_test(&context, false, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
  ASSERT_CALLED(prv_draw_round_rect_stroked(&context, &GRect(20, 80, 40, 10), 4, SW_ODD));
}

void test_graphics_context_${BIT_DEPTH_NAME}__draw_antialiased_stroke_width_2(void) {
  // Stroke width = 2, antialiased
  setup_test(&context, true, 2, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
#if PBL_COLOR
  ASSERT_CALLED(graphics_line_draw_stroked_aa(&context, GPoint(5, 5), GPoint(45, 10), 2));
#else
  ASSERT_CALLED(graphics_line_draw_stroked_non_aa(&context, GPoint(5, 5), GPoint(45, 10), 2));
#endif
  setup_test(&context, true, 2, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
  ASSERT_CALLED(prv_draw_rect(&context, &GRect(10, 20, 40, 10)));

  setup_test(&context, true, 2, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
#if PBL_COLOR
  ASSERT_CALLED(graphics_circle_draw_stroked_aa(&context, GPoint(50, 50), 10, 2));
#else
  ASSERT_CALLED(graphics_circle_draw_stroked_non_aa(&context, GPoint(50, 50), 10, 2));
#endif

  setup_test(&context, true, 2, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
#if PBL_COLOR
  ASSERT_CALLED(prv_draw_round_rect_aa_stroked(&context, &GRect(20, 80, 40, 10), 4, 2));
#else
  ASSERT_CALLED(prv_draw_round_rect_stroked(&context, &GRect(20, 80, 40, 10), 4, 2));
#endif
}

void test_graphics_context_${BIT_DEPTH_NAME}__draw_antialiased_stroke_width_even(void) {
  // Stroke width even > 2, antialiased
  setup_test(&context, true, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
#if PBL_COLOR
  ASSERT_CALLED(graphics_line_draw_stroked_aa(&context, GPoint(5, 5), GPoint(45, 10), SW_EVEN));
#else
  ASSERT_CALLED(graphics_line_draw_stroked_non_aa(&context, GPoint(5, 5), GPoint(45, 10), SW_EVEN));
#endif

  setup_test(&context, true, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
#if PBL_COLOR
  ASSERT_CALLED(prv_draw_rect_aa_stroked(&context, &GRect(10, 20, 40, 10), SW_EVEN));
#else
  ASSERT_CALLED(prv_draw_rect_stroked(&context, &GRect(10, 20, 40, 10), SW_EVEN));
#endif

  setup_test(&context, true, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
#if PBL_COLOR
  ASSERT_CALLED(graphics_circle_draw_stroked_aa(&context, GPoint(50, 50), 10, SW_EVEN));
#else
  ASSERT_CALLED(graphics_circle_draw_stroked_non_aa(&context, GPoint(50, 50), 10, SW_EVEN));
#endif

  setup_test(&context, true, SW_EVEN, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
#if PBL_COLOR
  ASSERT_CALLED(prv_draw_round_rect_aa_stroked(&context, &GRect(20, 80, 40, 10), 4, SW_EVEN));
#else
  ASSERT_CALLED(prv_draw_round_rect_stroked(&context, &GRect(20, 80, 40, 10), 4, SW_EVEN));
#endif
}

void test_graphics_context_${BIT_DEPTH_NAME}__draw_antialiased_stroke_width_odd(void) {
  // Stroke width odd > 1, antialiased
  setup_test(&context, true, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10));
#if PBL_COLOR
  ASSERT_CALLED(graphics_line_draw_stroked_aa(&context, GPoint(5, 5), GPoint(45, 10), SW_ODD));
#else
  ASSERT_CALLED(graphics_line_draw_stroked_non_aa(&context, GPoint(5, 5), GPoint(45, 10), SW_ODD));
#endif

  setup_test(&context, true, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_rect(&context, &GRect(10, 20, 40, 10));
#if PBL_COLOR
  ASSERT_CALLED(prv_draw_rect_aa_stroked(&context, &GRect(10, 20, 40, 10), SW_ODD));
#else
  ASSERT_CALLED(prv_draw_rect_stroked(&context, &GRect(10, 20, 40, 10), SW_ODD));
#endif

  setup_test(&context, true, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_circle(&context, GPoint(50, 50), 10);
#if PBL_COLOR
  ASSERT_CALLED(graphics_circle_draw_stroked_aa(&context, GPoint(50, 50), 10, SW_ODD));
#else
  ASSERT_CALLED(graphics_circle_draw_stroked_non_aa(&context, GPoint(50, 50), 10, SW_ODD));
#endif

  setup_test(&context, true, SW_ODD, GColorBlack, GColorBlack, false);
  graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4);
#if PBL_COLOR
  ASSERT_CALLED(prv_draw_round_rect_aa_stroked(&context, &GRect(20, 80, 40, 10), 4, SW_ODD));
#else
  ASSERT_CALLED(prv_draw_round_rect_stroked(&context, &GRect(20, 80, 40, 10), 4, SW_ODD));
#endif
}

void test_graphics_context_${BIT_DEPTH_NAME}__fill(void) {
  // Fill shape, non-antialiased (Stroke width/color N/A)
  setup_test(&context, false, 5, GColorBlack, GColorBlack, false);
  graphics_fill_rect(&context, &GRect(10, 20, 40, 10));
  ASSERT_CALLED(prv_fill_rect_non_aa(&context, &GRect(10, 20, 40, 10), 0, GCornerNone, GColorBlack));

  setup_test(&context, false, 5, GColorBlack, GColorBlack, false);
  graphics_fill_circle(&context, GPoint(50, 50), 10);
  ASSERT_CALLED(graphics_circle_fill_non_aa(&context, GPoint(50, 50), 10));

  setup_test(&context, false, 5, GColorBlack, GColorBlack, false);
  graphics_fill_round_rect(&context, &GRect(20, 80, 40, 10), 4, GCornersAll);
  ASSERT_CALLED(prv_fill_rect_non_aa(&context, &GRect(20, 80, 40, 10), 4, GCornersAll, GColorBlack));
}

void test_graphics_context_${BIT_DEPTH_NAME}__fill_antialiased(void) {
  // Fill shape, antialiased (Stroke width/color N/A)

  setup_test(&context, true, 5, GColorBlack, GColorBlack, false);
  graphics_fill_rect(&context, &GRect(10, 20, 40, 10));
#if SCREEN_COLOR_DEPTH_BITS == 1
  ASSERT_CALLED(prv_fill_rect_non_aa(&context, &GRect(10, 20, 40, 10), 0, GCornerNone, GColorBlack));
#else
  ASSERT_CALLED(prv_fill_rect_aa(&context, &GRect(10, 20, 40, 10), 0, GCornerNone, GColorBlack));
#endif

  setup_test(&context, true, 5, GColorBlack, GColorBlack, false);
  graphics_fill_circle(&context, GPoint(50, 50), 10);
#if PBL_COLOR
  ASSERT_CALLED(graphics_internal_circle_quadrant_fill_aa(&context, GPoint(50, 50), 10, GCornersAll));
#else
  ASSERT_CALLED(graphics_circle_fill_non_aa(&context, GPoint(50, 50), 10));
#endif

  setup_test(&context, true, 5, GColorBlack, GColorBlack, false);
  graphics_fill_round_rect(&context, &GRect(20, 80, 40, 10), 4, GCornersAll);
#if SCREEN_COLOR_DEPTH_BITS == 1
  ASSERT_CALLED(prv_fill_rect_non_aa(&context, &GRect(20, 80, 40, 10), 4, GCornersAll, GColorBlack));
#else
  ASSERT_CALLED(prv_fill_rect_aa(&context, &GRect(20, 80, 40, 10), 4, GCornersAll, GColorBlack));
#endif
}

void test_graphics_context_${BIT_DEPTH_NAME}__lock(void) {
  // Test all the setup test combinations as above
  setup_test(&context, false, 1, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));

  setup_test(&context, false, 2, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));

  setup_test(&context, false, SW_EVEN, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));

  setup_test(&context, false, SW_ODD, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));
  ASSERT_NO_CHANGE(graphics_fill_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_fill_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_fill_round_rect(&context, &GRect(20, 80, 40, 10), 4, GCornersAll));

  setup_test(&context, true, 1, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));

  setup_test(&context, true, 2, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));

  setup_test(&context, true, SW_EVEN, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));

  setup_test(&context, true, SW_ODD, GColorBlack, GColorBlack, true);
  ASSERT_NO_CHANGE(graphics_draw_line(&context, GPoint(5, 5), GPoint(45, 10)));
  ASSERT_NO_CHANGE(graphics_draw_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_draw_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_draw_round_rect(&context, &GRect(20, 80, 40, 10), 4));
  ASSERT_NO_CHANGE(graphics_fill_rect(&context, &GRect(10, 20, 40, 10)));
  ASSERT_NO_CHANGE(graphics_fill_circle(&context, GPoint(50, 50), 10));
  ASSERT_NO_CHANGE(graphics_fill_round_rect(&context, &GRect(20, 80, 40, 10), 4, GCornersAll));
}

void test_graphics_context_${BIT_DEPTH_NAME}__lock_context(void) {
  GDrawState draw_state;
  context.lock = true;

  // Stroke Color
  draw_state = graphics_context_get_drawing_state(&context);
  graphics_context_set_stroke_color(&context, GColorBlue);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);

  // Fill Color
  draw_state = graphics_context_get_drawing_state(&context);
  graphics_context_set_fill_color(&context, GColorGreen);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);

  // Compositing Mode
  draw_state = graphics_context_get_drawing_state(&context);
  graphics_context_set_compositing_mode(&context, GCompOpOr);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);

  // Text Color
  draw_state = graphics_context_get_drawing_state(&context);
  graphics_context_set_text_color(&context, GColorRed);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);

  // Antialiased
  draw_state = graphics_context_get_drawing_state(&context);
  graphics_context_set_antialiased(&context, true);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);

  // Stroke Width
  draw_state = graphics_context_get_drawing_state(&context);
  graphics_context_set_stroke_width(&context, 11);
  cl_assert(memcmp(&draw_state, &context.draw_state, sizeof(GDrawState)) == 0);
}

void test_graphics_context_${BIT_DEPTH_NAME}__lock_framebuffer(void) {
  FrameBuffer fb = {};
  framebuffer_init(&fb, &(GSize) {DISP_COLS, DISP_ROWS});
  GContext ctx = {.dest_bitmap.info = {
    .format = GBITMAP_NATIVE_FORMAT,
    .version = GBITMAP_VERSION_CURRENT,
  }, .parent_framebuffer=&fb};
  GBitmap *framebuffer = graphics_capture_frame_buffer(&ctx);
  cl_assert(ctx.lock == true);
  cl_assert_equal_p(framebuffer, &ctx.dest_bitmap);

  // Test releasing on any platform
  cl_assert(fb.is_dirty == false);
  graphics_release_frame_buffer(&ctx, framebuffer);
  cl_assert(ctx.lock == false);
  cl_assert(fb.is_dirty == true);
};

void test_graphics_context_${BIT_DEPTH_NAME}__lock_framebuffer_8BitCircular(void) {
  // Test for locking of requested framebuffer format
  GContext ctx = {.dest_bitmap.info.format = GBitmapFormat8BitCircular};
  GBitmap *bmp = graphics_capture_frame_buffer_format(&ctx, GBitmapFormat8BitCircular);
  cl_assert_equal_p(bmp, &ctx.dest_bitmap);
  cl_assert(ctx.lock == true);
};

void test_graphics_context_${BIT_DEPTH_NAME}__lock_framebuffer_fails_from_8BitCircular(void) {
  // Test for locking of 8Bit Circular framebuffer when framebuffer is regular 8Bit
  GContext ctx = {.dest_bitmap.info.format = GBitmapFormat8BitCircular};
  GBitmap *bmp = graphics_capture_frame_buffer_format(&ctx, GBitmapFormat8Bit);
  cl_assert(ctx.lock == false);
  cl_assert_equal_p(bmp, NULL);
};

void test_graphics_context_${BIT_DEPTH_NAME}__lock_framebuffer_1Bit_on_8BitCircular_must_fail(void) {
  // Test for locking of 1Bit framebuffer when framebuffer is 8Bit Circular
  GContext ctx = {.dest_bitmap.info.format = GBitmapFormat8BitCircular};
  GBitmap *bmp = graphics_capture_frame_buffer_format(&ctx, GBitmapFormat1Bit);
  cl_assert(ctx.lock == false);
  cl_assert_equal_p(bmp, NULL);
};

void test_graphics_context_${BIT_DEPTH_NAME}__lock_framebuffer_2BitPalette_must_fail(void) {
  // Ensure that the code path for unsupported capture formats leaves the GContext unlocked.
  GContext ctx = {.dest_bitmap.info.format = GBitmapFormat8Bit};
  GBitmap *bmp = graphics_capture_frame_buffer_format(&ctx, GBitmapFormat2BitPalette);
  cl_assert(ctx.lock == false);
  cl_assert_equal_p(bmp, NULL);
}
