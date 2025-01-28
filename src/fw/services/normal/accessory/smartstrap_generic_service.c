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

#include "applib/app_smartstrap.h"
#include "drivers/accessory.h"
#include "kernel/events.h"
#include "kernel/pebble_tasks.h"
#include "os/mutex.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "services/normal/accessory/smartstrap_attribute.h"
#include "services/normal/accessory/smartstrap_comms.h"
#include "services/normal/accessory/smartstrap_link_control.h"
#include "services/normal/accessory/smartstrap_state.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/mbuf.h"

#define MAX_SERVICES                    10
#define MIN_SERVICE_ID                  0x100
#define GENERIC_SERVICE_VERSION         1
#define TIMEOUT_MS                      100
//! The largest message for attributes which we support internally (the app has its own buffer)
#define BUFFER_LENGTH                   20
#define MIN_SERVICE_DISCOVERY_INTERVAL  1

typedef struct {
  uint16_t service_id;
  uint16_t attribute_id;
} ReadInfo;

typedef struct PACKED {
  uint8_t version;
  uint16_t service_id;
  uint16_t attribute_id;
  uint8_t type;
  uint8_t error;
  uint16_t length;
} FrameInfo;

typedef struct PACKED {
  uint16_t service_id;
  uint16_t attribute_id;
} NotificationInfoData;

typedef enum {
  GenericServiceResultOk = 0,
  GenericServiceResultNotSupported = 1,
  NumGenericServiceResults
} GenericServiceResult;

typedef enum {
  GenericServiceTypeRead = 0,
  GenericServiceTypeWrite = 1,
  GenericServiceTypeWriteRead = 2,
  NumGenericServiceTypes
} GenericServiceType;

typedef enum {
  ReservedServiceManagement = 0x0101,
  ReservedServiceControl = 0x0102,
  ReservedServiceMax = 0x0fff
} ReservedService;

typedef enum {
  ManagementServiceAttributeServiceDiscovery = 0x0001,
  ManagementServiceAttributeNotificationInfo = 0x0002
} ManagementServiceAttribute;

typedef enum {
  ControlServiceAttributeLaunchApp = 0x0001,
  ControlServiceAttributeButtonEvent = 0x0002
} ControlServiceAttribute;

static MBuf *s_reserved_read_mbuf;
static MBuf *s_read_header_mbuf;
static uint8_t s_read_header[sizeof(FrameInfo)];
static ReadInfo s_read_info;
static PebbleMutex *s_read_lock;
static uint8_t s_read_buffer[BUFFER_LENGTH];
static bool s_has_done_service_discovery;


static void prv_init(void) {
  s_read_lock = mutex_create();
}

static SmartstrapResult prv_do_send(GenericServiceType type, uint16_t service_id,
                                    uint16_t attribute_id, MBuf *write_mbuf, MBuf *read_mbuf,
                                    uint16_t timeout_ms) {
  if (s_read_header_mbuf) {
    // already a read in progress
    return SmartstrapResultBusy;
  }

  mutex_lock(s_read_lock);
  // populate the read info
  s_read_info = (ReadInfo) {
    .service_id = service_id,
    .attribute_id = attribute_id,
  };

  // add the header
  FrameInfo header = (FrameInfo) {
    .version = GENERIC_SERVICE_VERSION,
    .type = type,
    .error = GenericServiceResultOk,
    .service_id = service_id,
    .attribute_id = attribute_id,
    .length = mbuf_get_chain_length(write_mbuf)
  };
  MBuf send_header_mbuf = MBUF_EMPTY;
  mbuf_set_data(&send_header_mbuf, &header, sizeof(header));
  mbuf_append(&send_header_mbuf, write_mbuf);

  // setup the MBuf chain for reading
  s_read_header_mbuf = mbuf_get(&s_read_header, sizeof(s_read_header), MBufPoolSmartstrap);
  mbuf_append(s_read_header_mbuf, read_mbuf);

  SmartstrapResult result = smartstrap_send(SmartstrapProfileGenericService, &send_header_mbuf,
                                            s_read_header_mbuf, timeout_ms);
  if (result != SmartstrapResultOk) {
    mbuf_free(s_read_header_mbuf);
    s_read_header_mbuf = NULL;
  }
  mutex_unlock(s_read_lock);
  return result;
}

static void prv_send_service_discovery(void *context) {
  const uint16_t service_id = ReservedServiceManagement;
  const uint16_t attribute_id = ManagementServiceAttributeServiceDiscovery;
  if (s_reserved_read_mbuf) {
    // already a read in progress
    return;
  }
  s_reserved_read_mbuf = mbuf_get(s_read_buffer, sizeof(s_read_buffer), MBufPoolSmartstrap);
  SmartstrapResult result = prv_do_send(GenericServiceTypeRead, service_id, attribute_id, NULL,
                                        s_reserved_read_mbuf, TIMEOUT_MS);
  if (result != SmartstrapResultOk) {
    mbuf_free(s_reserved_read_mbuf);
    s_reserved_read_mbuf = NULL;
  }
  PBL_LOG(LOG_LEVEL_DEBUG, "Sent service discovery message (result=%d)", result);
}

static void prv_set_connected(bool connected) {
  s_has_done_service_discovery = false;
}

static bool prv_handle_management_attribute_read(bool success, ManagementServiceAttribute attr,
                                                 void *data, uint32_t length) {
  if (!success) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Read of management attribute was not successful (0x%x)", attr);
    return false;
  }
  if (attr == ManagementServiceAttributeServiceDiscovery) {
    if (length % sizeof(uint16_t)) {
      PBL_LOG(LOG_LEVEL_WARNING, "Service discovery response is invalid length: %"PRIu32, length);
      return false;
    }
    // validate and mark the services as connected
    uint32_t i;
    uint16_t *services = data;
    bool has_valid_service = false;
    for (i = 0; i < length / sizeof(uint16_t); i++) {
      if ((services[i] > ReservedServiceMax) || (services[i] == ReservedServiceControl)) {
        has_valid_service = true;
        smartstrap_connection_state_set_by_service(services[i], true);
      } else {
        PBL_LOG(LOG_LEVEL_DEBUG, "Skipping invalid service_id 0x%x", services[i]);
      }
    }
    if (has_valid_service) {
      s_has_done_service_discovery = true;
    } else {
      return false;
    }
  } else if (attr == ManagementServiceAttributeNotificationInfo) {
    if (length != sizeof(NotificationInfoData)) {
      PBL_LOG(LOG_LEVEL_WARNING, "Notification info response is invalid length: %"PRIu32, length);
      return false;
    }
    const NotificationInfoData *notification_info = data;
    if (notification_info->service_id <= ReservedServiceMax) {
      // Currently we only support the control service and its launch app attribute
      if (notification_info->service_id == ReservedServiceControl &&
          notification_info->attribute_id == ControlServiceAttributeLaunchApp) {
        s_reserved_read_mbuf = mbuf_get(s_read_buffer, sizeof(s_read_buffer), MBufPoolSmartstrap);
        SmartstrapResult result = prv_do_send(GenericServiceTypeRead, notification_info->service_id,
                                              notification_info->attribute_id, NULL,
                                              s_reserved_read_mbuf, TIMEOUT_MS);
        if (result != SmartstrapResultOk) {
          mbuf_free(s_reserved_read_mbuf);
          s_reserved_read_mbuf = NULL;
          return false;
        }
        return true;
      } else {
        // Unsupported reserved service and/or attribute
        return false;
      }
    } else {
      // Notification wasn't for a reserved service, send a notification event
      smartstrap_attribute_send_event(SmartstrapNotifyEvent, SmartstrapProfileGenericService,
                                      SmartstrapResultOk, notification_info->service_id,
                                      notification_info->attribute_id, 0);
    }
  } else {
    WTF;
  }
  return true;
}

static bool prv_handle_control_attribute_read(bool success, ControlServiceAttribute attr,
                                              void *data, uint32_t length) {
  if (!success) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Read of control attribute was not successful (0x%x)", attr);
    return false;
  }
  if (attr == ControlServiceAttributeLaunchApp) {
    if (length != UUID_SIZE) {
      PBL_LOG(LOG_LEVEL_WARNING, "Launch app response is invalid length: %"PRIu32, length);
      return false;
    }

    Uuid *app_uuid = (Uuid *)data;
    AppInstallId app_id = app_install_get_id_for_uuid(app_uuid);
    if (app_id == INSTALL_ID_INVALID) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Attempting to launch an invalid app");
      return false;
    }
    app_manager_put_launch_app_event(&(AppLaunchEventConfig) {
      .id = app_id,
      .common.reason = APP_LAUNCH_SMARTSTRAP,
    });
  } else if (attr == ControlServiceAttributeButtonEvent) {
    // TODO: PBL-38311
    return false;
  } else {
    WTF;
  }
  return true;
}

static bool prv_read_complete(bool success, uint32_t length) {
  FrameInfo *header = mbuf_get_data(s_read_header_mbuf);
  // get the length of the data buffer(s) which is the max length of data we could have received
  const uint32_t data_length = mbuf_get_chain_length(mbuf_get_next(s_read_header_mbuf));
  mbuf_free(s_read_header_mbuf);
  s_read_header_mbuf = NULL;
  SmartstrapResult result = SmartstrapResultOk;
  if (!success) {
    result = SmartstrapResultTimeOut;
  } else if ((length < sizeof(FrameInfo)) ||
      (header->length != length - sizeof(FrameInfo)) ||
      (header->length > data_length) ||
      (header->version != GENERIC_SERVICE_VERSION) ||
      (header->error >= NumGenericServiceResults) ||
      (header->service_id != s_read_info.service_id) ||
      (header->attribute_id != s_read_info.attribute_id)) {
    success = false;
    // TODO: We just got a bad frame, but time-out is the best error we have right now. Ideally,
    // we could validate the generic service header and drop this frame before we stop the read
    // timeout and then keep looking for a valid frame until the timeout is hit.
    result = SmartstrapResultTimeOut;
  } else if (success && (header->error != GenericServiceResultOk)) {
    // The response was completely valid, but there was a non-Ok error returned
    success = false;
    // translate the error code to the appropriate SmartstrapResult
    if (header->error == GenericServiceResultNotSupported) {
      result = SmartstrapResultAttributeUnsupported;
    } else {
      WTF;
    }
  }

  const uint16_t service_id = s_read_info.service_id;
  const uint16_t attribute_id = s_read_info.attribute_id;
  if (success) {
    length = header->length;
  } else {
    length = 0;
  }

  if (service_id <= ReservedServiceMax) {
    // This is a reserved service read which we should handle internally
    void *data = mbuf_get_data(s_reserved_read_mbuf);
    mbuf_free(s_reserved_read_mbuf);
    s_reserved_read_mbuf = NULL;
    if (service_id == ReservedServiceManagement) {
      success = prv_handle_management_attribute_read(success, attribute_id, data, length);
    } else if (service_id == ReservedServiceControl) {
      success = prv_handle_control_attribute_read(success, attribute_id, data, length);
    } else {
      WTF;
    }
  } else {
    smartstrap_attribute_send_event(SmartstrapDataReceivedEvent, SmartstrapProfileGenericService,
                                    result, service_id, attribute_id, length);
  }

  return success;
}

static void prv_handle_notification(void) {
  // follow-up with a notification info frame
  const uint16_t service_id = ReservedServiceManagement;
  const uint16_t attribute_id = ManagementServiceAttributeNotificationInfo;
  if (s_reserved_read_mbuf) {
    // already a read in progress
    return;
  }
  s_reserved_read_mbuf = mbuf_get(s_read_buffer, sizeof(s_read_buffer), MBufPoolSmartstrap);
  SmartstrapResult result = prv_do_send(GenericServiceTypeRead, service_id, attribute_id, NULL,
                                        s_reserved_read_mbuf, TIMEOUT_MS);
  if (result != SmartstrapResultOk) {
    mbuf_free(s_reserved_read_mbuf);
    s_reserved_read_mbuf = NULL;
  }
}

static SmartstrapResult prv_send(const SmartstrapRequest *request) {
  if (!s_has_done_service_discovery || !sys_smartstrap_is_service_connected(request->service_id)) {
    return SmartstrapResultServiceUnavailable;
  }

  GenericServiceType type;
  if (request->write_mbuf && request->read_mbuf) {
    type = GenericServiceTypeWriteRead;
  } else if (request->write_mbuf) {
    type = GenericServiceTypeWrite;
  } else if (request->read_mbuf) {
    type = GenericServiceTypeRead;
  } else {
    type = GenericServiceTypeRead; // stop lint from complaining
    WTF;
  }
  return prv_do_send(type, request->service_id, request->attribute_id, request->write_mbuf,
                     request->read_mbuf, request->timeout_ms);
}

static bool prv_send_control(void) {
  // make sure we're not spamming the smartstrap with service discovery messages
  static time_t s_last_service_discovery_time = 0;
  const time_t current_time = rtc_get_time();
  if (!s_has_done_service_discovery &&
      (current_time > s_last_service_discovery_time + MIN_SERVICE_DISCOVERY_INTERVAL) &&
      smartstrap_link_control_is_profile_supported(SmartstrapProfileGenericService)) {
    prv_send_service_discovery(NULL);
    s_last_service_discovery_time = current_time;
    return true;
  }
  return false;
}

static void prv_read_aborted(void) {
  mbuf_free(s_read_header_mbuf);
  s_read_header_mbuf = NULL;
  mbuf_free(s_reserved_read_mbuf);
  s_reserved_read_mbuf = NULL;
}

const SmartstrapProfileInfo *smartstrap_generic_service_get_info(void) {
  static const SmartstrapProfileInfo s_generic_service_info = {
    .profile = SmartstrapProfileGenericService,
    .max_services = MAX_SERVICES,
    .min_service_id = MIN_SERVICE_ID,
    .init = prv_init,
    .connected = prv_set_connected,
    .send = prv_send,
    .read_complete = prv_read_complete,
    .notify = prv_handle_notification,
    .control = prv_send_control,
    .read_aborted = prv_read_aborted,
  };
  return &s_generic_service_info;
}
