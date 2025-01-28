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

#include "clar.h"
#include "test_jerry_port_common.h"
#include "test_rocky_common.h"

#include "applib/graphics/gtypes.h"
#include "applib/rockyjs/api/rocky_api.h"
#include "applib/rockyjs/api/rocky_api_global.h"
#include "applib/rockyjs/api/rocky_api_graphics.h"
#include "applib/rockyjs/api/rocky_api_graphics_text.h"
#include "applib/rockyjs/pbl_jerry_port.h"
#include "util/trig.h"

// Standard
#include "string.h"

// Fakes
#include "fake_app_timer.h"
#include "fake_time.h"

// Stubs
#include "stubs_app_manager.h"
#include "stubs_app_state.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_resources.h"
#include "stubs_sleep.h"
#include "stubs_serial.h"
#include "stubs_syscalls.h"
#include "stubs_sys_exit.h"

size_t heap_bytes_free(void) {
  return 123456;
}

static Window s_app_window_stack_get_top_window;
Window *app_window_stack_get_top_window() {
  return &s_app_window_stack_get_top_window;
}

void rocky_api_graphics_path2d_add_canvas_methods(jerry_value_t obj) {}
void rocky_api_graphics_path2d_cleanup(void) {}
void rocky_api_graphics_path2d_reset_state(void) {}

GContext s_context;

// mocks
static MockCallRecordings s_graphics_context_set_fill_color;
void graphics_context_set_fill_color(GContext* ctx, GColor color) {
  record_mock_call(s_graphics_context_set_fill_color) {
    .ctx = ctx, .color = color,
  };
  ctx->draw_state.fill_color = color;
}

static MockCallRecordings s_graphics_context_set_stroke_color;
void graphics_context_set_stroke_color(GContext* ctx, GColor color) {
  record_mock_call(s_graphics_context_set_stroke_color) {
    .ctx = ctx, .color = color,
  };
  ctx->draw_state.stroke_color = color;
}

static MockCallRecordings s_graphics_context_set_stroke_width;
void graphics_context_set_stroke_width(GContext* ctx, uint8_t stroke_width) {
  record_mock_call(s_graphics_context_set_stroke_width) {
    .ctx = ctx, .width = stroke_width,
  };
  ctx->draw_state.stroke_width = stroke_width;
}

static MockCallRecordings s_graphics_fill_rect;
static GColor s_graphics_fill_rect__color;
void graphics_fill_rect(GContext *ctx, const GRect *rect) {
  s_graphics_fill_rect__color = s_context.draw_state.fill_color;
  record_mock_call(s_graphics_fill_rect) {
   .ctx = ctx, .rect = *rect,
  };
}

static MockCallRecordings s_graphics_draw_rect_precise;
void graphics_draw_rect_precise(GContext *ctx, const GRectPrecise *rect) {
  record_mock_call(s_graphics_draw_rect_precise) {
    .ctx = ctx, .prect = *rect,
  };
}

static MockCallRecordings s_graphics_fill_radial_precise_internal;
void graphics_fill_radial_precise_internal(GContext *ctx, GPointPrecise center,
                                           Fixed_S16_3 radius_inner, Fixed_S16_3 radius_outer,
                                           int32_t angle_start, int32_t angle_end) {
  record_mock_call(s_graphics_fill_radial_precise_internal) {
    .ctx = ctx,
    .fill_radial_precise.center = center,
    .fill_radial_precise.radius_inner = radius_inner,
    .fill_radial_precise.radius_outer = radius_outer,
    .fill_radial_precise.angle_start = angle_start,
    .fill_radial_precise.angle_end = angle_end,
  };
}


void graphics_fill_round_rect_by_value(GContext *ctx, GRect rect, uint16_t corner_radius,
                                       GCornerMask corner_mask) {}
static MockCallRecordings s_layer_mark_dirty;
void layer_mark_dirty(Layer *layer) {
  record_mock_call(s_layer_mark_dirty){
    .layer = layer
  };
}

static MockCallRecordings s_fonts_get_system_font;
GFont s_fonts_get_system_font__result;
GFont fonts_get_system_font(const char *font_key) {
  record_mock_call(s_fonts_get_system_font){
    .font_key = font_key
  };
  return s_fonts_get_system_font__result;
}

static MockCallRecordings s_graphics_draw_text;
void graphics_draw_text(GContext *ctx, const char *text, GFont const font, const GRect box,
                        const GTextOverflowMode overflow_mode, const GTextAlignment alignment,
                        GTextAttributes *text_attributes) {
  record_mock_call(s_graphics_draw_text){
    .draw_text.box = box,
    .draw_text.color = ctx->draw_state.text_color,
  };
  strncpy(s_graphics_draw_text.last_call.draw_text.text,
          text, sizeof(s_graphics_draw_text.last_call.draw_text.text));
}

static MockCallRecordings s_graphics_text_attributes_destroy;
void graphics_text_attributes_destroy(GTextAttributes *text_attributes) {
  record_mock_call(s_graphics_text_attributes_destroy){};
}

static MockCallRecordings s_graphics_text_layout_get_max_used_size;
static GSize s_graphics_text_layout_get_max_used_size__result;
GSize graphics_text_layout_get_max_used_size(GContext *ctx, const char *text,
                                             GFont const font, const GRect box,
                                             const GTextOverflowMode overflow_mode,
                                             const GTextAlignment alignment,
                                             GTextLayoutCacheRef layout) {
  record_mock_call(s_graphics_text_layout_get_max_used_size){
    .max_used_size.font = font,
    .max_used_size.box = box,
    .max_used_size.overflow_mode = overflow_mode,
    .max_used_size.alignment = alignment,
  };
  strncpy(s_graphics_text_layout_get_max_used_size.last_call.max_used_size.text,
          text, sizeof(s_graphics_text_layout_get_max_used_size.last_call.max_used_size.text));

  return s_graphics_text_layout_get_max_used_size__result;
}

void test_rocky_api_graphics__initialize(void) {
  fake_app_timer_init();
  rocky_runtime_context_init();
  jerry_init(JERRY_INIT_EMPTY);

  s_app_window_stack_get_top_window = (Window){};
  s_context = (GContext){};
  s_app_state_get_graphics_context = &s_context;
  s_app_event_loop_callback = NULL;

  s_graphics_context_set_stroke_color = (MockCallRecordings){0};
  s_graphics_context_set_stroke_width = (MockCallRecordings){0};
  s_graphics_context_set_fill_color = (MockCallRecordings){0};
  s_graphics_fill_rect = (MockCallRecordings){0};
  s_graphics_fill_rect__color = GColorClear;
  s_graphics_draw_rect_precise = (MockCallRecordings){0};
  s_graphics_fill_radial_precise_internal = (MockCallRecordings){0};
  s_layer_mark_dirty = (MockCallRecordings){0};
  s_fonts_get_system_font = (MockCallRecordings){0};
  s_graphics_draw_text = (MockCallRecordings){0};
  s_graphics_text_attributes_destroy = (MockCallRecordings){0};
  s_graphics_text_layout_get_max_used_size = (MockCallRecordings){0};
  s_graphics_text_layout_get_max_used_size__result = (GSize){0};
}

void test_rocky_api_graphics__cleanup(void) {
  fake_app_timer_deinit();

  // some tests deinitialize the engine, avoid double de-init
  if (app_state_get_rocky_runtime_context() != NULL) {
    jerry_cleanup();
    rocky_runtime_context_deinit();
  }
}

static const RockyGlobalAPI *s_graphics_api[] = {
  &GRAPHIC_APIS,
  NULL,
};

extern RockyAPITextState s_rocky_text_state;

void test_rocky_api_graphics__handles_text_state(void) {
  cl_assert_equal_i(0, s_fonts_get_system_font.call_count);
  cl_assert_equal_i(0, s_graphics_text_attributes_destroy.call_count);
  rocky_global_init(s_graphics_api);
  cl_assert_equal_i(1, s_fonts_get_system_font.call_count);
  cl_assert_equal_i(0, s_graphics_text_attributes_destroy.call_count);
  rocky_global_deinit();
  cl_assert_equal_i(1, s_fonts_get_system_font.call_count);
  cl_assert_equal_i(0, s_graphics_text_attributes_destroy.call_count);

  s_rocky_text_state.text_attributes = (GTextAttributes *)123;
  rocky_global_deinit();
  cl_assert_equal_i(1, s_fonts_get_system_font.call_count);
  cl_assert_equal_i(1, s_graphics_text_attributes_destroy.call_count);
}

void test_rocky_api_graphics__request_draw(void) {
  rocky_global_init(s_graphics_api);

  cl_assert_equal_i(0, s_layer_mark_dirty.call_count);
  EXECUTE_SCRIPT("_rocky.requestDraw();");
  cl_assert_equal_i(1, s_layer_mark_dirty.call_count);
  cl_assert_equal_p(&s_app_window_stack_get_top_window.layer, s_layer_mark_dirty.last_call.layer);
}

void test_rocky_api_graphics__provides_draw_event(void) {
  rocky_global_init(s_graphics_api);

  cl_assert_equal_b(false, rocky_global_has_event_handlers("draw"));
  EXECUTE_SCRIPT("_rocky.on('draw', function() {});");
  cl_assert_equal_b(true, rocky_global_has_event_handlers("draw"));
}

void test_rocky_api_graphics__draw_event_has_ctx(void) {
  rocky_global_init(s_graphics_api);

  EXECUTE_SCRIPT(
    "var event = null;\n"
    "_rocky.on('draw', function(e) {event = e;});"
  );

  const jerry_value_t event_null = prv_js_global_get_value("event");
  cl_assert_equal_b(true, jerry_value_is_null(event_null));
  jerry_release_value(event_null);

  Layer *l = &app_window_stack_get_top_window()->layer;
  l->update_proc(l, NULL);
  const jerry_value_t event = prv_js_global_get_value("event");
  cl_assert_equal_b(true, jerry_value_is_object(event));

  const jerry_value_t context_2d = jerry_get_object_field(event, "context");
  cl_assert_equal_b(true, jerry_value_is_object(context_2d));
  jerry_release_value(context_2d);
  jerry_release_value(event);
}

jerry_value_t prv_create_canvas_context_2d_for_layer(Layer *layer);
void layer_get_unobstructed_bounds(const Layer *layer, GRect *bounds_out) {
  *bounds_out = GRect(5, 6, 7, 8);
}

void test_rocky_api_graphics__canvas_offers_size(void) {
  rocky_global_init(s_graphics_api);

  Layer l = {.bounds = GRect(1, 2, 3, 4)};
  const jerry_value_t ctx = prv_create_canvas_context_2d_for_layer(&l);
  jerry_set_object_field(jerry_get_global_object(), "ctx", ctx);

  EXECUTE_SCRIPT(
    "var w = ctx.canvas.clientWidth;\n"
    "var h = ctx.canvas.clientHeight;\n"
    "var uol = ctx.canvas.unobstructedLeft;\n"
    "var uot = ctx.canvas.unobstructedTop;\n"
    "var uow = ctx.canvas.unobstructedWidth;\n"
    "var uoh = ctx.canvas.unobstructedHeight;\n"
  );
  ASSERT_JS_GLOBAL_EQUALS_I("w", 3);
  ASSERT_JS_GLOBAL_EQUALS_I("h", 4);
  ASSERT_JS_GLOBAL_EQUALS_I("uol", 5);
  ASSERT_JS_GLOBAL_EQUALS_I("uot", 6);
  ASSERT_JS_GLOBAL_EQUALS_I("uow", 7);
  ASSERT_JS_GLOBAL_EQUALS_I("uoh", 8);
}

static const jerry_value_t prv_global_init_and_set_ctx(void) {
  rocky_global_init(s_graphics_api);

  // make this easily testable by putting it int JS context as global
  Layer l = {.bounds = GRect(0, 0, 144, 168)};
  const jerry_value_t ctx = prv_create_canvas_context_2d_for_layer(&l);
  cl_assert_equal_b(jerry_value_is_object(ctx), true);
  jerry_set_object_field(jerry_get_global_object(), "ctx", ctx);

  return ctx;
}

void test_rocky_api_graphics__drawing_rects(void) {
  prv_global_init_and_set_ctx();

  s_context.draw_state.fill_color = GColorJaegerGreen;

  EXECUTE_SCRIPT(
    "ctx.clearRect(1, 2, 3, 4);\n"
  );

  cl_assert_equal_i(1, s_graphics_fill_rect.call_count);
  cl_assert_equal_rect(GRect(1, 2, 3, 4), s_graphics_fill_rect.last_call.rect);
  cl_assert_equal_i(GColorBlackARGB8, s_graphics_fill_rect__color.argb);
  cl_assert_equal_i(GColorJaegerGreenARGB8, s_context.draw_state.fill_color.argb);

  s_graphics_fill_rect = (MockCallRecordings){};
  EXECUTE_SCRIPT(
    "ctx.fillRect(5, 6, 7, 8);\n"
  );

  cl_assert_equal_i(1, s_graphics_fill_rect.call_count);
  cl_assert_equal_rect(GRect(5, 6, 7, 8), s_graphics_fill_rect.last_call.rect);

  s_graphics_draw_rect_precise = (MockCallRecordings){};
  EXECUTE_SCRIPT(
    "ctx.strokeRect(9, 10.2, 11.5, 12.8);\n"
  );

  cl_assert_equal_i(1, s_graphics_draw_rect_precise.call_count);
  GRectPrecise expected_rect = {(int)(8.5*8), 78, (int)(11.5*8), (int)(12.8*8)};
  cl_assert_equal_rect_precise(expected_rect, s_graphics_draw_rect_precise.last_call.prect);
}

#define PP(x, y) (GPointPrecise( \
  (int16_t)((x) * FIXED_S16_3_FACTOR), \
  (int16_t)((y) * FIXED_S16_3_FACTOR)))

void test_rocky_api_graphics__fill_radial(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT(
    "ctx.rockyFillRadial(30, 40, 10, 20, 0, Math.PI);\n"
  );

  cl_assert_equal_i(1, s_graphics_fill_radial_precise_internal.call_count);
  MockCallRecording *const lc = &s_graphics_fill_radial_precise_internal.last_call;
  cl_assert_equal_point_precise(PP(29.5, 39.5), lc->fill_radial_precise.center);
  cl_assert_equal_i(10*8, lc->fill_radial_precise.radius_inner.raw_value);
  cl_assert_equal_i(20*8, lc->fill_radial_precise.radius_outer.raw_value);
  cl_assert_equal_i(TRIG_MAX_ANGLE * 1 / 4, lc->fill_radial_precise.angle_start);
  cl_assert_equal_i(TRIG_MAX_ANGLE * 3 / 4, lc->fill_radial_precise.angle_end);

  EXECUTE_SCRIPT(
    "ctx.rockyFillRadial(30, 40, 10, 30, 0, 2 * Math.PI);\n"
  );

  cl_assert_equal_i(2, s_graphics_fill_radial_precise_internal.call_count);
  cl_assert_equal_point_precise(PP(29.5, 39.5), lc->fill_radial_precise.center);
  cl_assert_equal_i(10*8, lc->fill_radial_precise.radius_inner.raw_value);
  cl_assert_equal_i(30*8, lc->fill_radial_precise.radius_outer.raw_value);
  cl_assert_equal_i(TRIG_MAX_ANGLE * 1 / 4, lc->fill_radial_precise.angle_start);
  cl_assert_equal_i(TRIG_MAX_ANGLE * 5 / 4, lc->fill_radial_precise.angle_end);

  EXECUTE_SCRIPT(
    "ctx.rockyFillRadial(30.5, 40.1, 30, 10, 0, 2 * Math.PI);\n"
  );

  cl_assert_equal_i(3, s_graphics_fill_radial_precise_internal.call_count);
  cl_assert_equal_point_precise(PP(30, 39.625), lc->fill_radial_precise.center);
  cl_assert_equal_i(10*8, lc->fill_radial_precise.radius_inner.raw_value);
  cl_assert_equal_i(30*8, lc->fill_radial_precise.radius_outer.raw_value);
}

void test_rocky_api_graphics__fill_radial_not_enough_args(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT_EXPECT_ERROR(
     "ctx.rockyFillRadial(30, 40, 10, 20, 0);\n",
     "TypeError: Not enough arguments"
  );
}

void test_rocky_api_graphics__fill_radial_type_error(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT_EXPECT_ERROR(
      "ctx.rockyFillRadial(30, 40, 10, 20, 0, false);\n",
      "TypeError: Argument at index 5 is not a Number"
  );
}

void test_rocky_api_graphics__fill_radial_range_check(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT_EXPECT_ERROR(
      "ctx.rockyFillRadial(4096, 40, 10, 20, 0, false);\n",
      "TypeError: Argument at index 0 is invalid: Value out of bounds for native type"
  );
}

void test_rocky_api_graphics__fill_radial_zero_radius(void) {
  prv_global_init_and_set_ctx();
  // inner radius = 0
  EXECUTE_SCRIPT(
    "ctx.rockyFillRadial(30, 40, 0, 20, 0, Math.PI);\n"
  );
  MockCallRecording *const lc = &s_graphics_fill_radial_precise_internal.last_call;
  cl_assert_equal_i(1, s_graphics_fill_radial_precise_internal.call_count);
  cl_assert_equal_point_precise((PP(29.5, 39.5)), lc->fill_radial_precise.center);
  cl_assert_equal_i(0, lc->fill_radial_precise.radius_inner.raw_value);
  cl_assert_equal_i(20 * 8, lc->fill_radial_precise.radius_outer.raw_value);

  // inner radius capped to >= 0
  EXECUTE_SCRIPT(
    "ctx.rockyFillRadial(30, 40, -10, 20, 0, Math.PI);\n"
  );
  cl_assert_equal_i(2, s_graphics_fill_radial_precise_internal.call_count);
  cl_assert_equal_point_precise((PP(29.5, 39.5)), lc->fill_radial_precise.center);
  cl_assert_equal_i(0, lc->fill_radial_precise.radius_inner.raw_value);
  cl_assert_equal_i(20 * 8, lc->fill_radial_precise.radius_outer.raw_value);

  // outer radius capped to >= 0
  EXECUTE_SCRIPT(
    "ctx.rockyFillRadial(30, 40, -10, -20, 0, Math.PI);\n"
  );
  cl_assert_equal_i(3, s_graphics_fill_radial_precise_internal.call_count);
  cl_assert_equal_point_precise((PP(29.5, 39.5)), lc->fill_radial_precise.center);
  cl_assert_equal_i(0, lc->fill_radial_precise.radius_inner.raw_value);
  cl_assert_equal_i(0, lc->fill_radial_precise.radius_outer.raw_value);
}

void test_rocky_api_graphics__line_styles(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT(
    "ctx.lineWidth = 8;\n"
    "var w = ctx.lineWidth;\n"
  );

  cl_assert_equal_i(1, s_graphics_context_set_stroke_width.call_count);
  cl_assert_equal_i(8, s_graphics_context_set_stroke_width.last_call.width);
  ASSERT_JS_GLOBAL_EQUALS_I("w", s_graphics_context_set_stroke_width.last_call.width);

  EXECUTE_SCRIPT(
    "ctx.lineWidth = 2.1;\n"
    "var w = ctx.lineWidth;\n"
  );
  ASSERT_JS_GLOBAL_EQUALS_I("w", 2);

  EXECUTE_SCRIPT_EXPECT_ERROR(
    "ctx.lineWidth = -4;\n",
    "TypeError: Argument at index 0 is invalid: Value out of bounds for native type"
  );
  EXECUTE_SCRIPT("var w = ctx.lineWidth;\n");
  ASSERT_JS_GLOBAL_EQUALS_I("w", 2);
}

void test_rocky_api_graphics__line_styles_check_bounds(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT_EXPECT_ERROR(
      "ctx.lineWidth = -1;",
      "TypeError: Argument at index 0 is invalid: Value out of bounds for native type"
  );

  EXECUTE_SCRIPT_EXPECT_ERROR(
      "ctx.lineWidth = 256;",
      "TypeError: Argument at index 0 is invalid: Value out of bounds for native type"
  );
}

void test_rocky_api_graphics__fill_and_stroke_styles(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT(
    "ctx.fillStyle = '#f00';\n"
    "ctx.strokeStyle = 'white';\n"
    "var c = ctx.fillStyle;\n"
  );

  cl_assert_equal_i(1, s_graphics_context_set_fill_color.call_count);
  cl_assert_equal_i(GColorRedARGB8, s_graphics_context_set_fill_color.last_call.color.argb);
  cl_assert_equal_i(1, s_graphics_context_set_stroke_color.call_count);
  cl_assert_equal_i(GColorWhiteARGB8, s_graphics_context_set_stroke_color.last_call.color.argb);

  // ignores invalid values
  EXECUTE_SCRIPT(
    "ctx.fillStyle = 'unknown';\n"
    "ctx.strokeStyle = '4%2F';\n"
  );
  cl_assert_equal_i(1, s_graphics_context_set_fill_color.call_count);
  cl_assert_equal_i(1, s_graphics_context_set_stroke_color.call_count);
}

void test_rocky_api_graphics__canvas_state(void) {
  prv_global_init_and_set_ctx();

  // calling restore if nothing was stored is a no-op
  s_context.draw_state.fill_color.argb = 1;
  EXECUTE_SCRIPT("ctx.restore()\n");
  cl_assert_equal_i(1, s_context.draw_state.fill_color.argb);

  EXECUTE_SCRIPT("ctx.save()\n"); // 1
  s_context.draw_state.fill_color.argb = 2;
  EXECUTE_SCRIPT("ctx.save()\n"); // 2
  s_context.draw_state.fill_color.argb = 3;

  EXECUTE_SCRIPT("ctx.restore()\n"); // -> 2 (one element left)
  cl_assert_equal_i(2, s_context.draw_state.fill_color.argb);

  EXECUTE_SCRIPT("ctx.restore()\n"); // -> 1 (no element left)
  cl_assert_equal_i(1, s_context.draw_state.fill_color.argb);

  EXECUTE_SCRIPT("ctx.restore()\n"); // no-op
  cl_assert_equal_i(1, s_context.draw_state.fill_color.argb);
}

static const int16_t large_int = 10000;

void test_rocky_api_graphics__fill_text(void) {
  prv_global_init_and_set_ctx();

  // we do this in C and not JS as color binding is not linked in this unit-test
  // what we want to test though is that the text color is taken from fill color
  rocky_api_graphics_get_gcontext()->draw_state.fill_color = GColorRed;
  EXECUTE_SCRIPT(
    "ctx.fillText('some text', 10, 10);\n"
  );

  cl_assert_equal_i(1, s_graphics_draw_text.call_count);
  cl_assert_equal_s("some text", s_graphics_draw_text.last_call.draw_text.text);
  cl_assert_equal_i(GColorRedARGB8, s_graphics_draw_text.last_call.draw_text.color.argb);
  cl_assert_equal_rect((GRect(10, 10, large_int, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);

  rocky_api_graphics_get_gcontext()->draw_state.fill_color = GColorBlue;
  EXECUTE_SCRIPT(
    "ctx.fillText('more text', -10.5, 5000, 60);\n"
  );

  cl_assert_equal_i(2, s_graphics_draw_text.call_count);
  cl_assert_equal_s("more text", s_graphics_draw_text.last_call.draw_text.text);
  cl_assert_equal_i(GColorBlueARGB8, s_graphics_draw_text.last_call.draw_text.color.argb);
  cl_assert_equal_rect((GRect(-11, 5000, 60, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);
}

void test_rocky_api_graphics__fill_text_coordinates(void) {
  prv_global_init_and_set_ctx();
  EXECUTE_SCRIPT("ctx.fillText('some text', 0, 1.5);");
  cl_assert_equal_rect((GRect(0, 2, large_int, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);

  EXECUTE_SCRIPT("ctx.fillText('some text', -0.2, 1.2, 10.5);");
  cl_assert_equal_rect((GRect(0, 1, 11, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);

  EXECUTE_SCRIPT("ctx.fillText('some text', -0.5, 1.2, -0.5);");
  cl_assert_equal_rect((GRect(-1, 1, -1, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);
}

void test_rocky_api_graphics__fill_text_aligned(void) {
  prv_global_init_and_set_ctx();

  // we do this in C and not JS as color binding is not linked in this unit-test
  // what we want to test though is that the text color is taken from fill color

  EXECUTE_SCRIPT(
    "ctx.textAlign = 'left';\n"
    "ctx.fillText('some text', 100, 100);\n"
  );

  cl_assert_equal_i(1, s_graphics_draw_text.call_count);
  cl_assert_equal_rect((GRect(100, 100, large_int, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);

  EXECUTE_SCRIPT(
    "ctx.textAlign = 'center';\n"
    "ctx.fillText('some text', 100, 100);\n"
  );

  cl_assert_equal_i(2, s_graphics_draw_text.call_count);
  cl_assert_equal_rect((GRect(-4900, 100, large_int, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);

  EXECUTE_SCRIPT(
    "ctx.textAlign = 'right';\n"
    "ctx.fillText('some text', 100, 100);\n"
  );

  cl_assert_equal_i(3, s_graphics_draw_text.call_count);
  cl_assert_equal_rect((GRect(-9900, 100, large_int, large_int)),
                       s_graphics_draw_text.last_call.draw_text.box);
}

void test_rocky_api_graphics__text_align(void) {
  prv_global_init_and_set_ctx();

  // initial value
  cl_assert_equal_i(GTextAlignmentLeft, s_rocky_text_state.alignment);

  s_rocky_text_state.alignment = (GTextAlignment)-1;
  // unsupported values don't change the value
  EXECUTE_SCRIPT("ctx.textAlign = 123;\n");
  cl_assert_equal_i(-1, s_rocky_text_state.alignment);
  EXECUTE_SCRIPT("ctx.textAlign = 'unknown';\n");
  cl_assert_equal_i(-1, s_rocky_text_state.alignment);


  EXECUTE_SCRIPT("ctx.textAlign = 'left';\nvar a = ctx.textAlign;\n");
  cl_assert_equal_i(GTextAlignmentLeft, s_rocky_text_state.alignment);
  ASSERT_JS_GLOBAL_EQUALS_S("a", "left");

  EXECUTE_SCRIPT("ctx.textAlign = 'right';\nvar a = ctx.textAlign;\n");
  cl_assert_equal_i(GTextAlignmentRight, s_rocky_text_state.alignment);
  ASSERT_JS_GLOBAL_EQUALS_S("a", "right");

  EXECUTE_SCRIPT("ctx.textAlign = 'center';\nvar a = ctx.textAlign;\n");
  cl_assert_equal_i(GTextAlignmentCenter, s_rocky_text_state.alignment);
  ASSERT_JS_GLOBAL_EQUALS_S("a", "center");

  // we only support LTR
  EXECUTE_SCRIPT("ctx.textAlign = 'start';\nvar a = ctx.textAlign;\n");
  cl_assert_equal_i(GTextAlignmentLeft, s_rocky_text_state.alignment);
  ASSERT_JS_GLOBAL_EQUALS_S("a", "left");

  EXECUTE_SCRIPT("ctx.textAlign = 'end';\nvar a = ctx.textAlign;\n");
  cl_assert_equal_i(GTextAlignmentRight, s_rocky_text_state.alignment);
  ASSERT_JS_GLOBAL_EQUALS_S("a", "right");
}

void test_rocky_api_graphics__text_font(void) {
  cl_assert_equal_i(0, s_fonts_get_system_font.call_count);
  s_fonts_get_system_font__result = (GFont)123;
  rocky_global_init(s_graphics_api);
  cl_assert_equal_i(1, s_fonts_get_system_font.call_count);
  cl_assert_equal_p((GFont)123, s_rocky_text_state.font);

  // make this easily testable by putting it int JS context as global
  Layer l = {.bounds = GRect(0, 0, 144, 168)};
  const jerry_value_t ctx = prv_create_canvas_context_2d_for_layer(&l);
  jerry_set_object_field(jerry_get_global_object(), "ctx", ctx);


  s_rocky_text_state.font = (GFont)-1;
  // unsupported values don't change the value
  EXECUTE_SCRIPT("ctx.font = 123;\n");
  cl_assert_equal_p((GFont)-1, s_rocky_text_state.font);
  EXECUTE_SCRIPT("ctx.font = 'unknown';\n");
  cl_assert_equal_p((GFont)-1, s_rocky_text_state.font);
  cl_assert_equal_i(1, s_fonts_get_system_font.call_count);

  EXECUTE_SCRIPT("ctx.font = '14px bold Gothic';\n");
  cl_assert_equal_i(2, s_fonts_get_system_font.call_count);
  cl_assert_equal_p(FONT_KEY_GOTHIC_14_BOLD, s_fonts_get_system_font.last_call.font_key);

  EXECUTE_SCRIPT("ctx.font = '28px Gothic';\nvar f = ctx.font;\n");
  ASSERT_JS_GLOBAL_EQUALS_S("f", "28px Gothic");
}

extern T_STATIC void prv_graphics_color_to_char_buffer(GColor8 color, char *buf_out);

#define TEST_COLOR_STRING(gcolor, expect_str) do { \
    char buf[12]; \
    prv_graphics_color_to_char_buffer(gcolor, buf); \
    cl_assert_equal_s(buf, expect_str); \
  } while(0);

void test_rocky_api_graphics__color_names(void) {
  TEST_COLOR_STRING(GColorClear, "transparent");
  TEST_COLOR_STRING((GColor){ .a = 1 }, "transparent");
  TEST_COLOR_STRING(GColorRed, "#FF0000");
  TEST_COLOR_STRING(GColorMalachite, "#00FF55");
}

extern T_STATIC const RockyAPISystemFontDefinition s_font_definitions[];
bool prv_font_definition_from_value(jerry_value_t value, RockyAPISystemFontDefinition **result);

void test_rocky_api_graphics__text_font_names_unique(void) {
  rocky_global_init(s_graphics_api);

  const RockyAPISystemFontDefinition *def = s_font_definitions;
  while (def->js_name) {
    const jerry_value_t name_js = jerry_create_string((jerry_char_t *)def->js_name);
    RockyAPISystemFontDefinition *cmp_def = NULL;
    bool actual = prv_font_definition_from_value(name_js, &cmp_def);
    cl_assert_equal_b(true, actual);
    cl_assert_equal_s(cmp_def->res_key, def->res_key);
    jerry_release_value(name_js);
    def++;
  }
}

void test_rocky_api_graphics__measure_text(void) {
  prv_global_init_and_set_ctx();

  // fill text_state with unique values we can test against
  s_rocky_text_state = (RockyAPITextState) {
    .font = (GFont)-1,
    .overflow_mode = (GTextOverflowMode)-2,
    .alignment = (GTextAlignment)-3,
    .text_attributes = (GTextAttributes *)-4,
  };

  s_graphics_text_layout_get_max_used_size__result = GSize(123, 456);
  EXECUTE_SCRIPT(
    "var tm = ctx.measureText('foo');\n"
    "var tm_w = tm.width;\n"
    "var tm_h = tm.height;\n"
  );
  ASSERT_JS_GLOBAL_EQUALS_I("tm_w", 123);
  ASSERT_JS_GLOBAL_EQUALS_I("tm_h", 456);

  cl_assert_equal_i(1, s_graphics_text_layout_get_max_used_size.call_count);
  const MockCallRecording *lc = &s_graphics_text_layout_get_max_used_size.last_call;
  cl_assert_equal_s("foo", lc->max_used_size.text);
  cl_assert_equal_p(s_rocky_text_state.font, lc->max_used_size.font);
  cl_assert_equal_rect((GRect(0, 0, INT16_MAX, INT16_MAX)), lc->max_used_size.box);
  cl_assert_equal_i(s_rocky_text_state.overflow_mode, lc->max_used_size.overflow_mode);
  cl_assert_equal_i(s_rocky_text_state.alignment, lc->max_used_size.alignment);
}

void test_rocky_api_graphics__state_initialized_between_renders(void) {
  prv_global_init_and_set_ctx();

  // fill text_state with unique values we can test against
  s_rocky_text_state = (RockyAPITextState) {
    .font = (GFont)-1,
    .overflow_mode = (GTextOverflowMode)-2,
    .alignment = (GTextAlignment)-3,
    .text_attributes = (GTextAttributes *)-4,
  };

  EXECUTE_SCRIPT("_rocky.on('draw', function(e) {});");
  Layer *l = &app_window_stack_get_top_window()->layer;
  l->update_proc(l, NULL);

  cl_assert_equal_i(1, s_fonts_get_system_font.call_count);
  cl_assert_equal_i(GTextAlignmentLeft, s_rocky_text_state.alignment);
  cl_assert_equal_i(GTextOverflowModeWordWrap, s_rocky_text_state.overflow_mode);
  cl_assert_equal_p(NULL, s_rocky_text_state.text_attributes);
}

void test_rocky_api_graphics__context_2d_prototype_wrap_function(void) {
  prv_global_init_and_set_ctx();

  EXECUTE_SCRIPT("var origFillRect = _rocky.CanvasRenderingContext2D.prototype.fillRect;\n"
                 "_rocky.CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {\n"
                 "  w *= 2;\n"
                 "  h *= 2;\n"
                 "  origFillRect.call(this, x, y, w, h);\n"
                 "};\n"
                 "ctx.fillRect(5, 6, 7, 8);\n"
                 );

  cl_assert_equal_i(1, s_graphics_fill_rect.call_count);
  cl_assert_equal_rect(GRect(5, 6, 7 * 2, 8 * 2), s_graphics_fill_rect.last_call.rect);
}
