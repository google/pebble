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

#include "comm/ble/gatt_service_changed.h"
#include "comm/ble/gap_le_connection.h"

#include "kernel/events.h"

#include "clar.h"

#include <btutil/bt_device.h>

// Fakes
///////////////////////////////////////////////////////////

#include "fake_GAPAPI.h"
#include "fake_GATTAPI.h"
#include "fake_GATTAPI_test_vectors.h"
#include "fake_pbl_malloc.h"
#include "fake_new_timer.h"
#include "fake_rtc.h"
#include "fake_system_task.h"

// Stubs
///////////////////////////////////////////////////////////

#include "stubs_bluetopia_interface.h"
#include "stubs_bt_driver_gatt.h"
#include "stubs_bt_lock.h"
#include "stubs_events.h"
#include "stubs_gatt_client_subscriptions.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_prompt.h"
#include "stubs_rand_ptr.h"
#include "stubs_regular_timer.h"

extern bool prv_contains_service_changed_characteristic(
    GAPLEConnection *connection,
    const GATT_Service_Discovery_Indication_Data_t *event);

void core_dump_reset(bool is_forced) {
}

static GAPLEConnection s_connection;

GAPLEConnection *gap_le_connection_by_device(const BTDeviceInternal *addr) {
  return &s_connection;
}

GAPLEConnection *gap_le_connection_by_addr(const BTDeviceAddress *addr) {
  return &s_connection;
}

GAPLEConnection *gap_le_connection_by_gatt_id(unsigned int connection_id) {
  return &s_connection;
}

bool gap_le_connection_is_valid(const GAPLEConnection *conn) {
  return true;
}

GAPLEConnection *gap_le_connection_any(void) {
  return NULL;
}

uint16_t gaps_get_starting_att_handle(void) {
  return 4;
}

GAPLEConnection *gatt_client_characteristic_get_connection(BLECharacteristic characteristic_ref) {
  return NULL;
}

BLEService gatt_client_att_handle_get_service(
    GAPLEConnection *connection, uint16_t att_handle, const GATTServiceNode **service_node_out) {
  return 0;
}

uint8_t gatt_client_copy_service_refs_by_discovery_generation(
    const BTDeviceInternal *device, BLEService services_out[],
    uint8_t num_services, uint8_t discovery_gen) { return 0;}

void gatt_client_service_get_all_characteristics_and_descriptors(
    GAPLEConnection *connection, GATTService *service,
    BLECharacteristic *characteristic_hdls_out,
    BLEDescriptor *descriptor_hdls_out) { }

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

// FIXME: PBL-23945
void fake_kernel_malloc_mark(void) { }
void fake_kernel_malloc_mark_assert_equal(void) { }

// Helpers
///////////////////////////////////////////////////////////

#define TEST_GATT_CONNECTION_ID (1234)
#define TEST_BT_STACK_ID (1)

typedef enum {
  Unknown,
  Handled,
  Unhandled,
} HandleResult;

static HandleResult s_last_handle_discovery_result;

static void prv_bluetopia_service_discovery_cb(unsigned int stack_id,
                                               GATT_Service_Discovery_Event_Data_t *event,
                                               unsigned long callback_param) {
  cl_assert_equal_i(stack_id, TEST_BT_STACK_ID);
  if (event->Event_Data_Type == etGATT_Service_Discovery_Indication) {
    const GATT_Service_Discovery_Indication_Data_t *indication =
                                           event->Event_Data.GATT_Service_Discovery_Indication_Data;
    cl_assert_equal_i(s_connection.gatt_connection_id, TEST_GATT_CONNECTION_ID);
    cl_assert_equal_i(indication->ConnectionID, TEST_GATT_CONNECTION_ID);

    s_last_handle_discovery_result =
        prv_contains_service_changed_characteristic(&s_connection, indication) ? Handled : Unhandled;
  }
}

// Tests
///////////////////////////////////////////////////////////

void test_gatt_service_changed_client__initialize(void) {
  s_last_handle_discovery_result = Unknown;
  fake_gatt_init();
  s_connection = (GAPLEConnection) {
    .gatt_connection_id = TEST_GATT_CONNECTION_ID,
    .gatt_service_changed_att_handle = 0,
  };
  GATT_Start_Service_Discovery_Handle_Range(TEST_BT_STACK_ID, TEST_GATT_CONNECTION_ID, NULL, 0, NULL,
                               prv_bluetopia_service_discovery_cb, 0);
}

void test_gatt_service_changed_client__cleanup(void) {
}

// Discovery
///////////////////////////////////////////////////////////

void test_gatt_service_changed_client__handle_non_gatt_profile_service(void) {
  fake_gatt_put_discovery_indication_blood_pressure_service(TEST_GATT_CONNECTION_ID);
  cl_assert_equal_i(s_last_handle_discovery_result, Unhandled);
}

void test_gatt_service_changed_client__handle_gatt_profile_service(void) {
  fake_gatt_put_discovery_indication_gatt_profile_service(TEST_GATT_CONNECTION_ID,
                                                     true /* has_service_changed_characteristic */);
  cl_assert_equal_i(s_last_handle_discovery_result, Handled);

  // Verify the CCCD of the Service Changed characteristic has been written:
  cl_assert_equal_i(fake_gatt_write_last_written_handle(),
                    fake_gatt_gatt_profile_service_service_changed_cccd_att_handle());

  // Simulate getting a Write Response confirmation for the written CCCD:
  fake_gatt_put_write_response_for_last_write();

  // Today we don't really do anything upon getting the confirmation
}

void test_gatt_service_changed_client__handle_gatt_profile_service_missing_service_changed(void) {
  fake_gatt_put_discovery_indication_gatt_profile_service(TEST_GATT_CONNECTION_ID,
                                                    false /* has_service_changed_characteristic */);
  cl_assert_equal_i(s_last_handle_discovery_result, Handled);
}

// Characteristic Value Indications
///////////////////////////////////////////////////////////

void test_gatt_service_changed_client__handle_indication_non_service_changed(void) {
  fake_gatt_put_discovery_indication_gatt_profile_service(TEST_GATT_CONNECTION_ID,
                                                     true /* has_service_changed_characteristic */);
  const uint8_t value;
  const bool handled = gatt_service_changed_client_handle_indication(&s_connection, 0xfffe,
                                                                     &value, sizeof(value));
  cl_assert_equal_b(handled, false);
}

void test_gatt_service_changed_client__handle_indication_service_changed(void) {
  fake_gatt_put_discovery_indication_gatt_profile_service(TEST_GATT_CONNECTION_ID,
                                                     true /* has_service_changed_characteristic */);
  const uint16_t att_handle = fake_gatt_gatt_profile_service_service_changed_att_handle();

  fake_kernel_malloc_mark();

  const int start_count_before_indication = fake_gatt_is_service_discovery_start_count();

  const uint16_t handle_range[2] = {
    [0] = 0x1,
    [1] = 0xfffe,
  };
  const bool handled = gatt_service_changed_client_handle_indication(&s_connection, att_handle,
                                              (const uint8_t *) handle_range, sizeof(uint16_t) * 2);
  // Re-discovery is trigger on KernelBG:
  fake_system_task_callbacks_invoke_pending();

  // The KernelBG trip uses kernel_malloc, make sure it's cleaning up properly:
  fake_kernel_malloc_mark_assert_equal();
  cl_assert_equal_b(handled, true);

  // Expect service discovery to be started once more:
  cl_assert_equal_i(start_count_before_indication + 1,
                    fake_gatt_is_service_discovery_start_count());
}

void test_gatt_service_changed_client__handle_indication_service_changed_malformed(void) {
  fake_gatt_put_discovery_indication_gatt_profile_service(TEST_GATT_CONNECTION_ID,
                                                     true /* has_service_changed_characteristic */);
  const uint16_t att_handle = fake_gatt_gatt_profile_service_service_changed_att_handle();

  const uint16_t handle_range[1] = {
    [0] = 0x1,
  };
  const bool handled = gatt_service_changed_client_handle_indication(&s_connection, att_handle,
                                              (const uint8_t *) handle_range, sizeof(uint16_t) * 1);
  cl_assert_equal_b(handled, true);
}
