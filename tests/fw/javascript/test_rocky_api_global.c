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

#include "applib/rockyjs/api/rocky_api_global.h"
#include "applib/rockyjs/pbl_jerry_port.h"

// Standard
#include "string.h"

// Fakes
#include "fake_app_timer.h"
#include "fake_time.h"
#include "fake_logging.h"

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

static int s_prv_api_init__callcount;
static void prv_api_init(void) {
  s_prv_api_init__callcount++;
}

static int s_prv_api_add__callcount;
static bool s_prv_api_add__result;
static bool prv_api_add(const char *event_name, jerry_value_t handler) {
  s_prv_api_add__callcount++;
  return s_prv_api_add__result;
}

static int s_prv_api_remove__callcount;
static void prv_api_remove(const char *event_name, jerry_value_t handler) {
  s_prv_api_remove__callcount++;
}

int s_prv_listener_a1__callcount;
JERRY_FUNCTION(prv_listener_a1) {
  s_prv_listener_a1__callcount++;
  return jerry_create_undefined();
}

int s_prv_listener_a2__callcount;
JERRY_FUNCTION(prv_listener_a2) {
  s_prv_listener_a2__callcount++;
  return jerry_create_undefined();
}

int s_prv_listener_b__callcount;
JERRY_FUNCTION(prv_listener_b) {
  s_prv_listener_b__callcount++;
  return jerry_create_undefined();
}

void test_rocky_api_global__initialize(void) {
  fake_app_timer_init();
  rocky_runtime_context_init();
  jerry_init(JERRY_INIT_EMPTY);
  s_app_window_stack_get_top_window = (Window){};

  s_app_event_loop_callback = NULL;
  s_log_internal__expected = NULL;

  s_prv_api_init__callcount = 0;
  s_prv_api_add__callcount = 0;
  s_prv_api_add__result = false;
  s_prv_api_remove__callcount = 0;

  s_prv_listener_a1__callcount = 0;
  s_prv_listener_a2__callcount = 0;
  s_prv_listener_b__callcount = 0;
}

void test_rocky_api_global__cleanup(void) {
  fake_app_timer_deinit();
  s_log_internal__expected = NULL;

  jerry_cleanup();
  rocky_runtime_context_deinit();
  rocky_global_deinit();
}

void test_rocky_api_global__global(void) {
  char test_object[] = "var t = typeof _rocky";

  // global doesn't exist in plain Jerry context
  EXECUTE_SCRIPT(test_object);
  ASSERT_JS_GLOBAL_EQUALS_S("t", "undefined");

  // rocky_global_init() injects global...
  static const RockyGlobalAPI *apis[] = {
    NULL,
  };
  rocky_global_init(apis);

  EXECUTE_SCRIPT(test_object);
  ASSERT_JS_GLOBAL_EQUALS_S("t", "object");

  // ...which also has a method .on()
  EXECUTE_SCRIPT("var t = typeof _rocky.on");
  ASSERT_JS_GLOBAL_EQUALS_S("t", "function");

  /// ...which is an alias of .addEventListener()
  EXECUTE_SCRIPT("var a = (_rocky.on === _rocky.addEventListener);");
  ASSERT_JS_GLOBAL_EQUALS_B("a", true);
}

void test_rocky_api_global__calls_init_and_notifies_about_apis(void) {
  static const RockyGlobalAPI api = {
    .init = prv_api_init,
    .add_handler = prv_api_add,
  };
  static const RockyGlobalAPI *apis[] = {
    &api,
    NULL,
  };
  rocky_global_init(apis);
  cl_assert_equal_i(1, s_prv_api_init__callcount);

  s_prv_api_add__result = true;
  s_log_internal__expected = (const char *[]){NULL};
  EXECUTE_SCRIPT("_rocky.on('foo', function(){})");
  cl_assert_equal_i(1, s_prv_api_add__callcount);
  cl_assert_equal_b(true, rocky_global_has_event_handlers("foo"));

  s_prv_api_add__result = false;
  s_log_internal__expected = (const char *[]){
    "Unknown event 'bar'",
    NULL };
  EXECUTE_SCRIPT("_rocky.on('bar', function(){})");
  cl_assert_equal_i(2, s_prv_api_add__callcount);
  cl_assert_equal_b(false, rocky_global_has_event_handlers("bar"));
  cl_assert(*s_log_internal__expected == NULL);
}

void test_rocky_api_global__can_unsubscribe_event_handlers(void) {
  static const RockyGlobalAPI api = {
    .add_handler = prv_api_add,
    .remove_handler = prv_api_remove,
  };
  static const RockyGlobalAPI *apis[] = {
    &api,
    NULL,
  };
  rocky_global_init(apis);

  s_prv_api_add__result = true;
  EXECUTE_SCRIPT(
    "var f1 = function(){};\n"
    "var f2 = function(){};\n"
    "_rocky.on('foo', f1)\n"
  );
  cl_assert_equal_i(1, s_prv_api_add__callcount);
  cl_assert_equal_b(true, rocky_global_has_event_handlers("foo"));

  // variables f1, f2 continue to exist between EXECUTE_SCRIPT calls
  EXECUTE_SCRIPT("var t = typeof f2;");
  ASSERT_JS_GLOBAL_EQUALS_S("t", "function");

  // rocky.off exists
  EXECUTE_SCRIPT(
    "t = typeof _rocky.off;\n"
    "var eq = _rocky.off === _rocky.removeEventListener;\n"
  );
  ASSERT_JS_GLOBAL_EQUALS_S("t", "function");
  ASSERT_JS_GLOBAL_EQUALS_B("eq", true);

  // from MDN docs:
  // Calling removeEventListener() with arguments that do not identify
  // any currently registered EventListener on the EventTarget has no effect.
  EXECUTE_SCRIPT(
    "_rocky.off('foo', f2);\n"
    "_rocky.off('unknownevent', f1);\n"
  );
  cl_assert_equal_i(0, s_prv_api_remove__callcount);
  cl_assert_equal_b(true, rocky_global_has_event_handlers("foo"));

  EXECUTE_SCRIPT("_rocky.off('foo', f1);\n");
  cl_assert_equal_i(1, s_prv_api_remove__callcount);
  cl_assert_equal_b(false, rocky_global_has_event_handlers("foo"));
}

void prv_add_event_listener_to_list(const char *event_name, jerry_value_t listener);
int jerry_obj_refcount(jerry_value_t o);

void test_rocky_api_global__refcount(void) {
  jerry_value_t o = jerry_create_object();
  cl_assert_equal_i(1, jerry_obj_refcount(o));
  jerry_acquire_value(o);
  cl_assert_equal_i(2, jerry_obj_refcount(o));
  jerry_acquire_value(o);
  cl_assert_equal_i(3, jerry_obj_refcount(o));
  jerry_release_value(o);
  cl_assert_equal_i(2, jerry_obj_refcount(o));
  jerry_release_value(o);
  cl_assert_equal_i(1, jerry_obj_refcount(o));
  jerry_release_value(o);
  cl_assert_equal_i(0, jerry_obj_refcount(o));
}

void test_rocky_api_global__calls_listeners(void) {
  static const RockyGlobalAPI *apis[] = {NULL};
  rocky_global_init(apis);

  prv_add_event_listener_to_list("a", jerry_create_external_function(prv_listener_a1));
  cl_assert_equal_b(true, rocky_global_has_event_handlers("a"));
  cl_assert_equal_b(false, rocky_global_has_event_handlers("b"));


  prv_add_event_listener_to_list("b", jerry_create_external_function(prv_listener_b));
  cl_assert_equal_b(true, rocky_global_has_event_handlers("a"));
  cl_assert_equal_b(true, rocky_global_has_event_handlers("b"));

  prv_add_event_listener_to_list("a", jerry_create_external_function(prv_listener_a2));
  cl_assert_equal_b(true, rocky_global_has_event_handlers("a"));
  cl_assert_equal_b(true, rocky_global_has_event_handlers("b"));

  jerry_value_t a_event = rocky_global_create_event("a");
  rocky_global_call_event_handlers(a_event);
  cl_assert_equal_i(1, s_prv_listener_a1__callcount);
  cl_assert_equal_i(1, s_prv_listener_a2__callcount);
  cl_assert_equal_i(0, s_prv_listener_b__callcount);
  jerry_release_value(a_event);

  jerry_value_t b_event = rocky_global_create_event("b");
  rocky_global_call_event_handlers(b_event);
  cl_assert_equal_i(1, s_prv_listener_a1__callcount);
  cl_assert_equal_i(1, s_prv_listener_a2__callcount);
  cl_assert_equal_i(1, s_prv_listener_b__callcount);
  jerry_release_value(b_event);
}

void test_rocky_api_global__adds_listener_only_once(void) {
  static const RockyGlobalAPI *apis[] = {NULL};
  rocky_global_init(apis);

  const jerry_value_t f = jerry_create_external_function(prv_listener_a1);
  prv_add_event_listener_to_list("a", f);
  prv_add_event_listener_to_list("a", f);
  cl_assert_equal_b(true, rocky_global_has_event_handlers("a"));

  jerry_value_t a_event = rocky_global_create_event("a");
  rocky_global_call_event_handlers(a_event);
  // as second .on('a', f) "replaces" first, f will only be called once
  cl_assert_equal_i(1, s_prv_listener_a1__callcount);
  jerry_release_value(a_event);
}

void test_rocky_api_global__event_constructor(void) {
  static const RockyGlobalAPI *apis[] = {NULL};
  rocky_global_init(apis);

  EXECUTE_SCRIPT(
     "_rocky.Event.prototype.myCustomThing = 'xyz';\n"
     "var e = new _rocky.Event('myevent');\n"
     "var t = e.type;\n"
     "var c = e.myCustomThing;\n"
   );
  ASSERT_JS_GLOBAL_EQUALS_S("t", "myevent");
  ASSERT_JS_GLOBAL_EQUALS_S("c", "xyz");
}

void test_rocky_api_global__call_event_handlers_async(void) {
  static const RockyGlobalAPI api = {
    .init = prv_api_init,
    .add_handler = prv_api_add,
  };
  static const RockyGlobalAPI *apis[] = {
    &api,
    NULL,
  };
  rocky_global_init(apis);

  s_prv_api_add__result = true;
  EXECUTE_SCRIPT("var is_called = false; _rocky.on('a', function(e) { is_called = true; });");
  jerry_value_t a_event = rocky_global_create_event("a");
  rocky_global_call_event_handlers_async(a_event);
  ASSERT_JS_GLOBAL_EQUALS_B("is_called", false);

  s_process_manager_callback(s_process_manager_callback_data);
  ASSERT_JS_GLOBAL_EQUALS_B("is_called", true);
}
