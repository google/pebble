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

#include "gatt_client_discovery.h"
#include "gatt_service_changed.h"
#include "gap_le_connection.h"

#include "ble_log.h"

#include "comm/bt_lock.h"
#include "comm/bt_conn_mgr.h"

#include "kernel/core_dump.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "gatt_client_accessors.h"
#include "system/logging.h"

#include <bluetooth/gatt.h>
#include <bluetooth/gatt_discovery.h>
#include <btutil/bt_device.h>

// TODO: virtualize the gatt_client_discovery_discover_all() call

//! Defined in gatt_client_subscriptions.c. Should only be called when receiving
//! notification of a service change
extern void gatt_client_subscription_cleanup_by_att_handle_range(
    struct GAPLEConnection *connection, ATTHandleRange *range);

//! Defined in gatt_client_accessors.c. Should only be needed in this module
extern BLEService gatt_client_att_handle_get_service(
    GAPLEConnection *connection, uint16_t att_handle, GATTServiceNode **service_node_out);

// -------------------------------------------------------------------------------------------------
// Static function prototypes

static BTErrno prv_run_next_job(GAPLEConnection *connection);

// -------------------------------------------------------------------------------------------------
// Wrappers around Bluetopia's API

#define MIN_ATT_HANDLE 0x1
#define MAX_ATT_HANDLE 0xFFFF

typedef struct DiscoveryJobQueue {
  ListNode node;
  ATTHandleRange hdl;
} DiscoveryJobQueue;

// Assumes we are holding the BT lock
static void prv_add_discovery_job(
    GAPLEConnection *connection, ATTHandleRange *hdl_range) {
  DiscoveryJobQueue *node = kernel_zalloc_check(sizeof(DiscoveryJobQueue));
  if (hdl_range) {
    node->hdl = *hdl_range;
  } else { // discover everything
    node->hdl = (ATTHandleRange) {
      .start = MIN_ATT_HANDLE,
      .end = MAX_ATT_HANDLE
    };
  }

  if (!connection->discovery_jobs) {
    list_init(&node->node);
    connection->discovery_jobs = node;
  } else {
    list_append((ListNode *)connection->discovery_jobs, (ListNode *)node);
  }
}

void gatt_client_discovery_discover_range(GAPLEConnection *connection, ATTHandleRange *hdl_range) {
  bt_lock();
  {
    prv_add_discovery_job(connection, hdl_range);
    if (!connection->gatt_is_service_discovery_in_progress) {
      prv_run_next_job(connection);
    }
  }
  bt_unlock();
}

// assumes bt lock is held
static BTErrno prv_run_next_job(GAPLEConnection *connection) {
  DiscoveryJobQueue *node = connection->discovery_jobs;
  if (!node) {
    return BTErrnoOK; // no more jobs to run
  }

  // Note, that the job only gets removed from the list after discovery
  // has finished or error'ed out. That way the watchdog retry mechanism
  // can simply call this routine again to kick off another discovery attempt

  PBL_LOG(LOG_LEVEL_INFO, "Starting BLE Service Discovery: 0x%x to 0x%x",
          node->hdl.start, node->hdl.end);
  ATTHandleRange hdl = {
    .start = node->hdl.start,
    .end = node->hdl.end
  };

  BTErrno rv = bt_driver_gatt_start_discovery_range(connection, &hdl);

  if (rv == BTErrnoOK) {
    // if we are back here because a timeout occurred, let the
    // driver handle resetting the watchdog timer (cc2564x issue)
    connection->gatt_is_service_discovery_in_progress = true;
  }

  return rv;
}

// This function returns true if a retry started. If a retry did not start
// it sets e to BTErrnoOK if discovery completed or the actual error that happened
// which should be forwarded on
static bool prv_discovery_handle_timeout(GAPLEConnection *connection, BTErrno *e) {
  bool retry_started = false;
  BTErrno finalize_result = BTErrnoOK;
  // Executing on NewTimer task, so need to bt_lock():
  PBL_LOG(LOG_LEVEL_WARNING, "Service Discovery Watchdog Timeout");
  bt_lock();
  {
    if (!gap_le_connection_is_valid(connection)) {
      goto unlock;
    }

    if (bt_driver_gatt_stop_discovery(connection) != BTErrnoOK) {
      // Handle the race: Bluetopia service discovery has stopped in the mean time, for example
      // because of a disconnection, internal error or it completed right when the timer fired.
      goto unlock;
    }

    if (connection->gatt_service_discovery_retries == GATT_CLIENT_DISCOVERY_MAX_RETRY) {
#if !RELEASE && !UNITTEST
      core_dump_reset(true /* is_forced */);
#endif
      // Done retrying, just error out:
      finalize_result = BTErrnoServiceDiscoveryTimeout;
      goto unlock;
    }

    // Retry transparently (don't let the clients know):
    BTErrno ret_val = prv_run_next_job(connection);
    if (ret_val != BTErrnoOK) {
      // Start failed, just error out
      finalize_result = ret_val;
      goto unlock;
    }

    ++connection->gatt_service_discovery_retries;
    retry_started = true;
  }
unlock:
  *e = finalize_result;
  bt_unlock();

  return retry_started;
}

// -------------------------------------------------------------------------------------------------
extern uint8_t gatt_client_copy_service_refs_by_discovery_generation(
    const BTDeviceInternal *device, BLEService services_out[],
    uint8_t num_services, uint8_t discovery_gen);

static void prv_send_event(PebbleBLEGATTClientServiceEventInfo *info) {
  PebbleEvent e = (const PebbleEvent) {
    .type = PEBBLE_BLE_GATT_CLIENT_EVENT,
    .task_mask = 0,
    .bluetooth = {
      .le = {
        .gatt_client_service = {
          .info = info,
          .subtype = PebbleBLEGATTClientEventTypeServiceChange,
        },
      },
    },
  };
  // TODO: send only to tasks that are connected virtually
  event_put(&e);
}

static void prv_send_services_added_event(
    const GAPLEConnection *connection, BTErrno status) {

  uint8_t num_services_changed = (status == BTErrnoOK) ?
      list_count(&connection->gatt_remote_services->node) : 0;

  if (num_services_changed > BLE_GATT_MAX_SERVICES_CHANGED) {
    PBL_LOG(LOG_LEVEL_ERROR, "Remote has %u services, more than we can handle.",
            num_services_changed);
    num_services_changed = BLE_GATT_MAX_SERVICES_CHANGED;
  }

  size_t space_needed = num_services_changed * sizeof(PebbleBLEGATTClientServiceHandles)
      + sizeof(PebbleBLEGATTClientServiceEventInfo);

  PebbleBLEGATTClientServiceEventInfo *info = kernel_zalloc_check(space_needed);

  *info = (PebbleBLEGATTClientServiceEventInfo) {
    .type = PebbleServicesAdded,
    .device = connection->device,
    .status = status
  };

  info->services_added_data.num_services_added =
      gatt_client_copy_service_refs_by_discovery_generation(
          &connection->device, &info->services_added_data.services[0],
          BLE_GATT_MAX_SERVICES_CHANGED, connection->gatt_service_discovery_generation);

  prv_send_event(info);
}

static void prv_send_services_invalidate_all_event(
    const GAPLEConnection *connection, BTErrno status) {
  PebbleBLEGATTClientServiceEventInfo *info =
      kernel_zalloc_check(sizeof(PebbleBLEGATTClientServiceEventInfo));

  *info = (PebbleBLEGATTClientServiceEventInfo) {
    .type = PebbleServicesInvalidateAll,
    .device = connection->device,
    .status = status
  };

  prv_send_event(info);
}

extern void gatt_client_service_get_all_characteristics_and_descriptors(
    GAPLEConnection *connection, GATTService *service,
    BLECharacteristic *characteristics_hdls_out,
    BLEDescriptor *descriptor_hdls_out);

//! @note bt_lock is assumed to be taken by the caller
void gatt_client_discovery_handle_service_range_change(
    GAPLEConnection *connection, ATTHandleRange *range) {
  GATTServiceNode *service_node;
  BLEService service = gatt_client_att_handle_get_service(connection, range->start, &service_node);

  if (service == BLE_SERVICE_INVALID) {
    // Must be a new service
    return;
  }

  int memory_needed = service_node->service->num_characteristics * sizeof(BLECharacteristic) +
      service_node->service->num_descriptors * sizeof(BLEDescriptor);
  memory_needed +=
      sizeof(PebbleBLEGATTClientServiceEventInfo) + sizeof(PebbleBLEGATTClientServiceHandles);

  PebbleBLEGATTClientServiceEventInfo *info = kernel_zalloc_check(memory_needed);
  *info = (PebbleBLEGATTClientServiceEventInfo) {
    .type = PebbleServicesRemoved,
    .device = connection->device,
    .status = BTErrnoOK
  };

  info->services_removed_data.num_services_removed = 1;

  PebbleBLEGATTClientServiceHandles *remove_hdl = &info->services_removed_data.handles[0];

  remove_hdl->service = service;
  remove_hdl->uuid = service_node->service->uuid;
  remove_hdl->num_characteristics = service_node->service->num_characteristics;
  remove_hdl->num_descriptors = service_node->service->num_descriptors;
  gatt_client_service_get_all_characteristics_and_descriptors(
      connection, service_node->service,
      &remove_hdl->char_and_desc_handles[0],
      &remove_hdl->char_and_desc_handles[service_node->service->num_characteristics]);

  // a service has been removed/updated
  gatt_client_subscription_cleanup_by_att_handle_range(connection, range);
  ListNode **head = (ListNode **) &connection->gatt_remote_services;
  list_remove((ListNode *)service_node, head, NULL);
  kernel_free(service_node->service);
  service_node->service = NULL;
  kernel_free(service_node);

  prv_send_event(info);
}

static void prv_free_service_nodes(GAPLEConnection *connection) {
  GATTServiceNode *node = connection->gatt_remote_services;
  while (node) {
    GATTServiceNode *next = (GATTServiceNode *) node->node.next;
    kernel_free(node->service);
    node->service = NULL;
    kernel_free(node);
    node = next;
  }
  connection->gatt_remote_services = NULL;
}

static void prv_remove_current_discovery_job(GAPLEConnection *connection) {
  DiscoveryJobQueue *node = connection->discovery_jobs;
  if (!node) {
    return;
  }
  list_remove((ListNode *)connection->discovery_jobs,
              (ListNode **)&connection->discovery_jobs, NULL);
  kernel_free(node);

  // Handle the case where we are have received service change indication
  // messages for the same range in quick succession and have multiple jobs
  // scheduled as a result. This shouldn't be a frequent occurrence but see
  // PBL-24741 as an example

  DiscoveryJobQueue *new_job = connection->discovery_jobs;
  if (!new_job) {
    return; // nothing to do
  }

  if ((new_job->hdl.start == MIN_ATT_HANDLE) && (new_job->hdl.end == MAX_ATT_HANDLE)) {
    // we are rediscovering all services so flush everything
    prv_free_service_nodes(connection);
    prv_send_services_invalidate_all_event(
        connection, BTErrnoServiceDiscoveryDatabaseChanged);
  } else { // we are rediscovering one service
    gatt_client_discovery_handle_service_range_change(connection, &new_job->hdl);
  }
}

void gatt_client_cleanup_discovery_jobs(GAPLEConnection *connection) {
  bt_lock();
  {
    while (connection->discovery_jobs != NULL) {
      prv_remove_current_discovery_job(connection);
    }
  }
  bt_unlock();
}

static void prv_finalize_discovery(GAPLEConnection *connection, BTErrno errno) {
  if (errno != BTErrnoOK) {
    // Handle failure -- cleanup and dispatch event:
    prv_free_service_nodes(connection);
    gatt_client_subscriptions_cleanup_by_connection(connection, false /* should_unsubscribe */);
  }

  prv_remove_current_discovery_job(connection);
  connection->gatt_is_service_discovery_in_progress = false;
  connection->gatt_service_discovery_retries = 0;
  if (errno == BTErrnoServiceDiscoveryDatabaseChanged) {
    prv_send_services_invalidate_all_event(connection, errno);
  } else {
    prv_send_services_added_event(connection, errno);
  }
  ++connection->gatt_service_discovery_generation;
  prv_run_next_job(connection);
}

void bt_driver_cb_gatt_client_discovery_handle_indication(
    GAPLEConnection *connection, GATTService *service, BTErrno error) {

  // We experienced some kind of conversion error, pass it on
  if (error != BTErrnoOK) {
    prv_send_services_added_event(connection, error);
    return;
  }

  GATTServiceNode *node = kernel_zalloc_check(sizeof(GATTServiceNode));
  node->service = service;
  // tag the service with the generation it was discovered as a part of
  node->service->discovery_generation = connection->gatt_service_discovery_generation;

  bt_lock();
  {
    ListNode **head = (ListNode **) &connection->gatt_remote_services;
    if (*head) {
      list_append(*head, &node->node);
    } else {
      *head = &node->node;
    }
  }
  bt_unlock();
}

bool bt_driver_cb_gatt_client_discovery_complete(GAPLEConnection *connection, BTErrno errno) {
  bool finalize_discovery = true;
  bt_lock();
  {
    if (errno == BTErrnoServiceDiscoveryTimeout) {
      if (prv_discovery_handle_timeout(connection, &errno)) {
        // if a retry started, don't generate any events yet
        finalize_discovery = false;
        goto unlock;
      }
      // it's possible the discovery completed before we handled the timeout, in which case
      // we get a BTErrnoOK which means we will get a completion event already
      finalize_discovery = (errno != BTErrnoOK);
    }
    // Completion of service discovery implies we are about to have more BLE
    // traffic (for example, ANCS notifications, PPoG communication). Keep the
    // channel at a high throughput speed for a little bit longer to handle these bursts.
    conn_mgr_set_ble_conn_response_time(connection, BtConsumerLeServiceDiscovery,
                                        ResponseTimeMin, 10);
    if (finalize_discovery) {
      prv_finalize_discovery(connection, errno);
    }
  }
unlock:
  bt_unlock();
  return finalize_discovery;
}

BTErrno gatt_client_discovery_discover_all(const BTDeviceInternal *device) {
  BTErrno ret_val = BTErrnoOK;
  bt_lock();
  {
    GAPLEConnection *connection = gap_le_connection_by_device(device);
    if (!connection) {
      ret_val = BTErrnoInvalidParameter;
      goto unlock;
    }
    if (connection->gatt_is_service_discovery_in_progress) {
      ret_val = BTErrnoInvalidState;
      goto unlock;
    }
    if (connection->gatt_remote_services) {
      // Already discovered, no need to do it again!
      prv_send_services_added_event(connection, BTErrnoOK);
      goto unlock;
    }
    conn_mgr_set_ble_conn_response_time(connection, BtConsumerLeServiceDiscovery,
                                        ResponseTimeMin, 30);
    prv_add_discovery_job(connection, NULL);
    // if we get here there is no discovery in progress so dispatch the job
    ret_val = prv_run_next_job(connection);
  }
unlock:
  bt_unlock();
  return ret_val;
}

//! extern for gap_le_connection.c
//! Cleans up any state and frees the associated memory of all the things this module might have
//! created for a given connection.
//! bt_lock() is assumed to be taken by the caller
void gatt_client_discovery_cleanup_by_connection(GAPLEConnection *connection, BTErrno reason) {
  if (connection->gatt_is_service_discovery_in_progress) {
    // Assuming "disconnection" reason is appropriate here:
    prv_finalize_discovery(connection, reason);
    bt_driver_gatt_handle_discovery_abandoned();
  } else {
    prv_free_service_nodes(connection);
  }
}

//! extern for gatt_service_changed.c
//! Same as gatt_client_discovery_discover_all, but cleans up existing service discovery
//! state and stops any existing service discovery process.
BTErrno gatt_client_discovery_rediscover_all(const BTDeviceInternal *device) {
  BTErrno ret_val = BTErrnoServiceDiscoveryDisconnected;
  bt_lock();
  {
    GAPLEConnection *connection = gap_le_connection_by_device(device);
    if (connection) {
      if (connection->gatt_is_service_discovery_in_progress) {
        // Remove any partial jobs which may be pending
        // since we are going to rediscover everything
        gatt_client_cleanup_discovery_jobs(connection);
        bt_driver_gatt_stop_discovery(connection);
      } else {
        // Queue up CCCD writes to unsubscribe all the subscriptions:
        gatt_client_subscriptions_cleanup_by_connection(connection, true /* should_unsubscribe */);
      }
      prv_finalize_discovery(connection, BTErrnoServiceDiscoveryDatabaseChanged);
      ret_val = gatt_client_discovery_discover_all(device);
    }
  }
  bt_unlock();
  return ret_val;
}
