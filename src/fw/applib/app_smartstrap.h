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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! @addtogroup Smartstrap
//! @{
//!

//! As long as the firmware maintains its current major version, attributes of this length or
//! less will be allowed. Note that the maximum number of bytes which may be read from the
//! smartstrap before a timeout occurs depends on the baud rate and implementation efficiency of the
//! underlying UART protocol on the smartstrap.
#define SMARTSTRAP_ATTRIBUTE_LENGTH_MAXIMUM 65535

//! The default request timeout in milliseconds (see \ref smartstrap_set_timeout).
#define SMARTSTRAP_TIMEOUT_DEFAULT 250

//! The service_id to specify in order to read/write raw data to the smartstrap.
#define SMARTSTRAP_RAW_DATA_SERVICE_ID 0

//! The attribute_id to specify in order to read/write raw data to the smartstrap.
#define SMARTSTRAP_RAW_DATA_ATTRIBUTE_ID 0

// convenient macros to distinguish between smartstrap and no smartstrap.
// TODO: PBL-21978 remove redundant comments as a workaround around for SDK generator
#if defined(PBL_SMARTSTRAP)

//! Convenience macro to switch between two expressions depending on smartstrap support.
//! On platforms with a smartstrap the first expression will be chosen, the second otherwise.
#define PBL_IF_SMARTSTRAP_ELSE(if_true, if_false) (if_true)

#else

//! Convenience macro to switch between two expressions depending on smartstrap support.
//! On platforms with a smartstrap the first expression will be chosen, the second otherwise.
#define PBL_IF_SMARTSTRAP_ELSE(if_true, if_false) (if_false)
#endif

//! Error values which may be returned from the smartstrap APIs.
typedef enum {
  //! No error occurred.
  SmartstrapResultOk = 0,
  //! Invalid function arguments were supplied.
  SmartstrapResultInvalidArgs,
  //! The smartstrap port is not present on this watch.
  SmartstrapResultNotPresent,
  //! A request is already pending on the specified attribute.
  SmartstrapResultBusy,
  //! Either a smartstrap is not connected or the connected smartstrap does not support the
  //! specified service.
  SmartstrapResultServiceUnavailable,
  //! The smartstrap reported that it does not support the requested attribute.
  SmartstrapResultAttributeUnsupported,
  //! A time-out occurred during the request.
  SmartstrapResultTimeOut,
} SmartstrapResult;

//! A type representing a smartstrap ServiceId.
typedef uint16_t SmartstrapServiceId;

//! A type representing a smartstrap AttributeId.
typedef uint16_t SmartstrapAttributeId;

//! A type representing an attribute of a service provided by a smartstrap. This type is used when
//! issuing requests to the smartstrap.
typedef struct SmartstrapAttribute SmartstrapAttribute;

//! The type of function which is called after the smartstrap connection status changes.
//! @param service_id The ServiceId for which the availability changed.
//! @param is_available Whether or not this service is now available.
typedef void (*SmartstrapServiceAvailabilityHandler)(SmartstrapServiceId service_id,
                                                     bool is_available);

//! The type of function which can be called when a read request is completed.
//! @note Any write request made to the same attribute within this function will fail with
//! SmartstrapResultBusy.
//! @param attribute The attribute which was read.
//! @param result The result of the read.
//! @param data The data read from the smartstrap or NULL if the read was not successful.
//! @param length The length of the data or 0 if the read was not successful.
typedef void (*SmartstrapReadHandler)(SmartstrapAttribute *attribute, SmartstrapResult result,
                                      const uint8_t *data, size_t length);

//! The type of function which can be called when a write request is completed.
//! @param attribute The attribute which was written.
//! @param result The result of the write.
typedef void (*SmartstrapWriteHandler)(SmartstrapAttribute *attribute, SmartstrapResult result);

//! The type of function which can be called when the smartstrap sends a notification to the watch
//! @param attribute The attribute which the notification came from.
typedef void (*SmartstrapNotifyHandler)(SmartstrapAttribute *attribute);

//! Handlers which are passed to smartstrap_subscribe.
typedef struct {
  //! The connection handler is called after the connection state changes.
  SmartstrapServiceAvailabilityHandler availability_did_change;
  //! The read handler is called whenever a read is complete or the read times-out.
  SmartstrapReadHandler did_read;
  //! The did_write handler is called when a write has completed.
  SmartstrapWriteHandler did_write;
  //! The notified handler is called whenever a notification is received for an attribute.
  SmartstrapNotifyHandler notified;
} SmartstrapHandlers;

//! Subscribes handlers to be called after certain smartstrap events occur.
//! @note Registering an availability_did_change handler will cause power to be applied to the
//! smartstrap port and connection establishment to begin.
//! @see smartstrap_unsubscribe
//! @returns `SmartstrapResultNotPresent` if the watch does not have a smartstrap port or
//! `SmartstrapResultOk` otherwise.
SmartstrapResult app_smartstrap_subscribe(SmartstrapHandlers handlers);

//! Unsubscribes the handlers. The handlers will no longer be called, but in-flight requests will
//! otherwise be unaffected.
//! @note If power was being applied to the smartstrap port and there are no attributes have been
//! created (or they have all been destroyed), this will cause the smartstrap power to be turned
//! off.
void app_smartstrap_unsubscribe(void);

//! Changes the value of the timeout which is used for smartstrap requests. This timeout is started
//! after the request is completely sent to the smartstrap and will be canceled only if the entire
//! response is received before it triggers. The new timeout value will take affect only for
//! requests made after this API is called.
//! @param timeout_ms The duration of the timeout to set, in milliseconds.
//! @note The maximum allowed timeout is currently 1000ms. If a larger value is passed, it will be
//! internally lowered to the maximum.
//! @see SMARTSTRAP_TIMEOUT_DEFAULT
void app_smartstrap_set_timeout(uint16_t timeout_ms);

//! Creates and returns a SmartstrapAttribute for the specified service and attribute. This API
//! will allocate an internal buffer of the requested length on the app's heap.
//! @note Creating an attribute will result in power being applied to the smartstrap port (if it
//! isn't already) and connection establishment to begin.
//! @param service_id The ServiceId to create the attribute for.
//! @param attribute_id The AttributeId to create the attribute for.
//! @param buffer_length The length of the internal buffer which will be used to store the read
//! and write requests for this attribute.
//! @returns The newly created SmartstrapAttribute or NULL if an internal error occurred or if the
//! specified length is greater than SMARTSTRAP_ATTRIBUTE_LENGTH_MAXIMUM.
SmartstrapAttribute *app_smartstrap_attribute_create(SmartstrapServiceId service_id,
                                                     SmartstrapAttributeId attribute_id,
                                                     size_t buffer_length);

//! Destroys a SmartstrapAttribute. No further handlers will be called for this attribute and it
//! may not be used for any future requests.
//! @param[in] attribute The SmartstrapAttribute which should be destroyed.
//! @note If power was being applied to the smartstrap port, no availability_did_change handler is
//! subscribed, and the last attribute is being destroyed, this will cause the smartstrap power to
//! be turned off.
void app_smartstrap_attribute_destroy(SmartstrapAttribute *attribute);

//! Checks whether or not the specified service is currently supported by a connected smartstrap.
//! @param service_id The SmartstrapServiceId of the service to check for availability.
//! @returns Whether or not the service is available.
bool app_smartstrap_service_is_available(SmartstrapServiceId service_id);

//! Returns the ServiceId which the attribute was created for (see \ref
//! smartstrap_attribute_create).
//! @param attribute The SmartstrapAttribute for which to obtain the service ID.
//! @returns The SmartstrapServiceId which the attribute was created with.
SmartstrapServiceId app_smartstrap_attribute_get_service_id(SmartstrapAttribute *attribute);

//! Gets the AttributeId which the attribute was created for (see \ref smartstrap_attribute_create).
//! @param attribute The SmartstrapAttribute for which to obtain the attribute ID.
//! @returns The SmartstrapAttributeId which the attribute was created with.
SmartstrapAttributeId app_smartstrap_attribute_get_attribute_id(SmartstrapAttribute *attribute);

//! Performs a read request for the specified attribute. The `did_read` callback will be called when
//! the response is received from the smartstrap or when an error occurs.
//! @param attribute The attribute to be perform the read request on.
//! @returns `SmartstrapResultOk` if the read operation was started. The `did_read` callback will
//! be called once the read request has been completed.
SmartstrapResult app_smartstrap_attribute_read(SmartstrapAttribute *attribute);

//! Begins a write request for the specified attribute and returns a buffer into which the app
//! should write the data before calling smartstrap_attribute_end_write.
//! @note The buffer must not be used after smartstrap_attribute_end_write is called.
//! @param[in] attribute The attribute to begin writing for.
//! @param[out] buffer The buffer to write the data into.
//! @param[out] buffer_length The length of the buffer in bytes.
//! @returns `SmartstrapResultOk` if a write operation was started and the `buffer` and
//! `buffer_length` parameters were set, or an error otherwise.
SmartstrapResult app_smartstrap_attribute_begin_write(SmartstrapAttribute *attribute,
                                                      uint8_t **buffer, size_t *buffer_length);

//! This should be called by the app when it is done writing to the buffer provided by
//! smartstrap_begin_write and the data is ready to be sent to the smartstrap.
//! @param[in] attribute The attribute to begin writing for.
//! @param write_length The length of the data to be written, in bytes.
//! @param request_read Whether or not a read request on this attribute should be
//! automatically triggered following a successful write request.
//! @returns `SmartstrapResultOk` if a write operation was queued to be sent to the smartstrap. The
//! `did_write` handler will be called when the request is written to the smartstrap, and if
//! `request_read` was set to true, the `did_read` handler will be called when the read is complete.
SmartstrapResult app_smartstrap_attribute_end_write(SmartstrapAttribute *attribute,
                                                    size_t write_length, bool request_read);

//! @} // end addtogroup Smartstrap
