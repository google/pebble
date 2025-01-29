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

#include <bluetooth/gatt.h>

#include "gatt_client_operations.h"
#include "gatt_client_accessors.h"

#include "comm/bt_lock.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "util/list.h"
#include "system/passert.h"

// -------------------------------------------------------------------------------------------------
//! @see gatt_client_accessors.c / gatt_client_subscriptions.c
// These calls require the caller to own the bt_lock while calling the
// function and for as long as the result is being used / accessed.

extern uint16_t gatt_client_characteristic_get_handle_and_connection(
                                                               BLECharacteristic characteristic_ref,
                                                               GAPLEConnection **connection_out);

extern uint16_t gatt_client_descriptor_get_handle_and_connection(BLEDescriptor descriptor_ref,
                                                                 GAPLEConnection **connection_out);

extern void gatt_client_subscriptions_handle_write_cccd_response(BLEDescriptor cccd,
                                                                 BLEGATTError error);

// -------------------------------------------------------------------------------------------------

typedef struct {
  ListNode node;

  //! This is redundant, PebbleEvent already has this info, but added as integrity check.
  uintptr_t object_ref;

  uint16_t length;
  uint8_t value[];
} ReadResponseData;

typedef struct GattClientEventContext {
  ListNode node;
  PebbleBLEGATTClientEventType subtype;
  GAPLEClient client;
  uintptr_t obj_ref;
} GattClientEventContext;

static ReadResponseData *s_read_responses[GAPLEClientNum];

//! Keeps track of the current outstanding GattClientOperations (reads/writes). Useful for freeing
//! the outstanding op's memory when a connection dies in the middle.
static GattClientEventContext *s_client_event_ctxs[GAPLEClientNum];

static void prv_send_event(PebbleBLEGATTClientEventType subtype, GAPLEClient client,
                           uintptr_t object_ref, uint16_t value_length, BLEGATTError gatt_error) {
    PebbleEvent e = {
      .type = PEBBLE_BLE_GATT_CLIENT_EVENT,
      .task_mask = ~(gap_le_pebble_task_bit_for_client(client)),
      .bluetooth = {
        .le = {
          .gatt_client = {
            .subtype = subtype,
            .object_ref = object_ref,
            .gatt_error = gatt_error,
            .value_length = value_length,
          },
        },
      },
    };
    event_put(&e);
}

static void prv_internal_write_cccd_response_cb(GattClientOpResponseHdr *event) {
  const GattClientEventContext *data = event->context;
  const BLEDescriptor cccd = data->obj_ref;
  const BLEGATTError error = event->error_code;
  gatt_client_subscriptions_handle_write_cccd_response(cccd, error);
}

static BLEGATTError prv_handle_response(const GattClientOpReadResponse *resp,
                                        const GattClientEventContext *data,
                                        uint16_t *gatt_value_length) {
  uint16_t val_len = resp->value_length;
  if (val_len) {
    // Only create ReadResponseData node if length is not 0
    ReadResponseData *read_response = kernel_malloc(sizeof(ReadResponseData) + val_len);
    if (!read_response) {
      *gatt_value_length = 0;
      return BLEGATTErrorLocalInsufficientResources;
    }
    *read_response = (const ReadResponseData) {
      .object_ref = data->obj_ref,
      .length = val_len,
    };
    memcpy(read_response->value, resp->value, val_len);
    if (s_read_responses[data->client]) {
      list_append(&s_read_responses[data->client]->node, &read_response->node);
    } else {
      s_read_responses[data->client] = read_response;
    }
  }
  *gatt_value_length = val_len;
  return BLEGATTErrorSuccess;
}

static bool prv_ctx_in_client_event_ctxs(GattClientEventContext *context) {
  const bool exists =
      (list_contains(&s_client_event_ctxs[GAPLEClientApp]->node, &context->node) ||
       list_contains(&s_client_event_ctxs[GAPLEClientKernel]->node, &context->node));
  return exists;
}

void bt_driver_cb_gatt_client_operations_handle_response(GattClientOpResponseHdr *event) {
  const GattClientEventContext *data = event->context;
  bt_lock();
  {
    //! Special case: writes to the "Client Characteristic Configuration Descriptor" are handled by
    //! the gatt_client_subscriptions.c module.
    if (data->client == GAPLEClientKernel
        && data->subtype == PebbleBLEGATTClientEventTypeCharacteristicSubscribe) {
      prv_internal_write_cccd_response_cb(event);
      goto cleanup;
    }

    //! There is a time when we have disconnected, but there are still outstanding responses
    //! coming back to the MCU (e.g. HcProtocol). When we disconnect, we call gatt_client_op_cleanup
    //! which cleans up all of the memory for the nodes in the lists in `s_client_event_ctxs`.
    //! Here, we check if the context related to the response coming in has already been cleaned up,
    //! and if it has, we instantly unlock and continue on.
    if (!prv_ctx_in_client_event_ctxs(event->context)) {
      goto unlock;
    }

    // Default values
    uint16_t gatt_value_length = 0;
    uint16_t gatt_err_code = BLEGATTErrorSuccess;

    if (event->error_code != BLEGATTErrorSuccess) {
      gatt_err_code = event->error_code;
    } else {
      switch (event->type) {
        case GattClientOpResponseRead: {
          const GattClientOpReadResponse *resp = (GattClientOpReadResponse *)event;
          PBL_ASSERTN(data->subtype == PebbleBLEGATTClientEventTypeCharacteristicRead ||
                      data->subtype == PebbleBLEGATTClientEventTypeDescriptorRead);
            gatt_err_code = prv_handle_response(resp, data, &gatt_value_length);
          break;
        }
        case GattClientOpResponseWrite: {
          PBL_ASSERTN(data->subtype == PebbleBLEGATTClientEventTypeCharacteristicWrite ||
                      data->subtype == PebbleBLEGATTClientEventTypeDescriptorWrite);
          break;
        }
        default:
          WTF;
      }
    }

    prv_send_event(data->subtype, data->client, data->obj_ref, gatt_value_length, gatt_err_code);

  }

cleanup:
  list_remove(event->context, (ListNode **)&s_client_event_ctxs[data->client], NULL);
  kernel_free(event->context);
unlock:
  bt_unlock();
}

typedef uint16_t (*HandleAndConnectionGetter)(uintptr_t obj_ref,
                                              GAPLEConnection **connection_out);

static GattClientEventContext *prv_create_event_context(GAPLEClient client) {
  GattClientEventContext *evt_ctx = kernel_zalloc(sizeof(GattClientEventContext));
  if (evt_ctx) {
    s_client_event_ctxs[client] =
        (GattClientEventContext *)list_prepend(&s_client_event_ctxs[client]->node, &evt_ctx->node);
  }

  return evt_ctx;
}

static BTErrno prv_read(uintptr_t obj_ref, GAPLEClient client,
                        HandleAndConnectionGetter handle_getter,
                        PebbleBLEGATTClientEventType subtype) {
  BTErrno ret_val = BTErrnoOK;
  bt_lock();
  {
    GAPLEConnection *connection;
    const uint16_t att_handle = handle_getter(obj_ref, &connection);
    if (!att_handle) {
      ret_val = BTErrnoInvalidParameter;
      goto unlock;
    }

    GattClientEventContext *data = prv_create_event_context(client);
    if (!data) {
      ret_val = BTErrnoNotEnoughResources;
      goto unlock;
    }

    // Zero'd out and added to list in `prv_create_event_context`
    data->client = client;
    data->subtype = subtype;
    data->obj_ref = obj_ref;

    ret_val = bt_driver_gatt_read(connection, att_handle, data);
  }
unlock:
  bt_unlock();
  return ret_val;
}

static BTErrno prv_write(uintptr_t obj_ref, const uint8_t *value, size_t value_length,
                         GAPLEClient client, HandleAndConnectionGetter handle_getter,
                         PebbleBLEGATTClientEventType subtype) {
  BTErrno ret_val = BTErrnoOK;
  bt_lock();
  {
    GAPLEConnection *connection;
    const uint16_t att_handle = handle_getter(obj_ref, &connection);
    if (!att_handle) {
      ret_val = BTErrnoInvalidParameter;
      goto unlock;
    }

    GattClientEventContext *data = prv_create_event_context(client);
    if (!data) {
      ret_val = BTErrnoNotEnoughResources;
      goto unlock;
    }

    // Zero'd out and added to list in `prv_create_event_context`
    data->client = client;
    data->subtype = subtype;
    data->obj_ref = obj_ref;

    ret_val = bt_driver_gatt_write(connection, value, value_length, att_handle, data);
  }
unlock:
  bt_unlock();
  return ret_val;
}

BTErrno gatt_client_op_read(BLECharacteristic characteristic,
                            GAPLEClient client) {
  return prv_read(characteristic, client,
                  gatt_client_characteristic_get_handle_and_connection,
                  PebbleBLEGATTClientEventTypeCharacteristicRead);
}

void gatt_client_consume_read_response(uintptr_t object_ref,
                                       uint8_t value_out[],
                                       uint16_t value_length,
                                       GAPLEClient client) {
  bt_lock();
  {
    // For responses with 0 length, no ReadResponseData is created therefore
    // should not be attempted to be consumed.
    PBL_ASSERTN(value_length);

    PBL_ASSERTN(s_read_responses[client]);
    ReadResponseData *read_response = s_read_responses[client];
    PBL_ASSERTN(value_length == read_response->length);
    PBL_ASSERTN(object_ref == read_response->object_ref);
    if (value_out) {
      memcpy(value_out, read_response->value, read_response->length);
    }
    list_remove(&read_response->node, (ListNode **) &s_read_responses[client], NULL);
    kernel_free(read_response);
  }
  bt_unlock();
}

BTErrno gatt_client_op_write(BLECharacteristic characteristic,
                             const uint8_t *value,
                             size_t value_length,
                             GAPLEClient client) {
  return prv_write(characteristic, value, value_length, client,
                   gatt_client_characteristic_get_handle_and_connection,
                   PebbleBLEGATTClientEventTypeCharacteristicWrite);
}

BTErrno gatt_client_op_write_without_response(BLECharacteristic characteristic,
                                              const uint8_t *value,
                                              size_t value_length,
                                              GAPLEClient client) {
  BTErrno ret_val = BTErrnoOK;
  bt_lock();
  {
    GAPLEConnection *connection;
    const uint16_t att_handle =
        gatt_client_characteristic_get_handle_and_connection(characteristic, &connection);
    if (!att_handle) {
      ret_val = BTErrnoInvalidParameter;
      goto unlock;
    }

    ret_val = bt_driver_gatt_write_without_response(connection, value, value_length, att_handle);
  }
unlock:
  bt_unlock();
  return ret_val;
}

BTErrno gatt_client_op_write_descriptor(BLEDescriptor descriptor,
                                        const uint8_t *value,
                                        size_t value_length,
                                        GAPLEClient client) {
  return prv_write(descriptor, value, value_length, client,
                   gatt_client_descriptor_get_handle_and_connection,
                   PebbleBLEGATTClientEventTypeDescriptorWrite);
}

BTErrno gatt_client_op_read_descriptor(BLEDescriptor descriptor,
                                       GAPLEClient client) {
  return prv_read(descriptor, client,
                  gatt_client_descriptor_get_handle_and_connection,
                  PebbleBLEGATTClientEventTypeDescriptorRead);
}

BTErrno gatt_client_op_write_descriptor_cccd(BLEDescriptor cccd, const uint16_t *value) {
  return prv_write(cccd, (const uint8_t *) value, sizeof(*value), GAPLEClientKernel,
                   gatt_client_descriptor_get_handle_and_connection,
                   PebbleBLEGATTClientEventTypeCharacteristicSubscribe);
}

static bool prv_deinit_ctx_list(ListNode *node, void *unused) {
  kernel_free(node);
  return true;
}

void gatt_client_op_cleanup(GAPLEClient client) {
  bt_lock();
  {
    // Free all memory associated with outstanding operations
    list_foreach(&s_client_event_ctxs[client]->node, prv_deinit_ctx_list, NULL);
    s_client_event_ctxs[client] = NULL;

    ReadResponseData *read_response = s_read_responses[client];
    while (read_response) {
      ReadResponseData *next_read_response = (ReadResponseData *) read_response->node.next;
      kernel_free(read_response);
      read_response = next_read_response;
    }
    s_read_responses[client] = NULL;
  }
  bt_unlock();
}
