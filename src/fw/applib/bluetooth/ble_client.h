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

#include <bluetooth/bluetooth_types.h>

typedef enum {
  BLEClientServicesAdded,
  BLEClientServicesRemoved,
  BLEClientServicesInvalidateAll
} BLEClientServiceChangeUpdate;

//! Callback that is called when the services on a remote device that are
//! available to the application have changed. It gets called when:
//!  + the initial discovery of services completes
//!  + a new service has been discovered on the device after initial discovery
//!  + a service has been removed
//!  + the remote device has disconnected
//!
//! @note For convenience, the services are owned by the system and references
//! to services, characteristics and descriptors are guaranteed to remain valid
//! *until the service exposing the characteristic or descriptor is removed or
//! all services are invalidated* or until application is terminated.
//! @note References to services, characteristics and descriptors for the
//! specified device, that have been obtained in a previous callback to the
//! handler, are no longer valid. The application is responsible for cleaning up
//! these references.
//!
//! @param device The device associated with the service discovery process
//! @param update_type Defines what type of service update is being received
//! @param services Array of pointers to the discovered services. The array will
//! only contain Service UUIDs that matches the filter that was passed into the
//! ble_client_discover_services_and_characteristics() call.
//! @param num_services The number of discovered services matching the criteria.
//! @param status BTErrnoOK if the service discovery was completed successfully.
//! If the device was disconnected, BTErrnoConnectionTimeout,
//! BTErrnoRemotelyTerminated, BTErrnoLocallyTerminatedBySystem or
//! BTErrnoLocallyTerminatedByApp will specify the reason of the disconnection.
//! The number of services will be zero upon a disconnection.
typedef void (*BLEClientServiceChangeHandler)(BTDevice device,
                                              BLEClientServiceChangeUpdate update_type,
                                              const BLEService services[],
                                              uint8_t num_services,
                                              BTErrno status);

//! Registers the callback that handles service changes. After the call to
//! this function returns, the application should be ready to handle calls to
//! the handler.
//! @param handler Pointer to the function that will handle the service changes
//! @return BTErrnoOK if the handler was registered successfully,
//! or TODO....
BTErrno ble_client_set_service_change_handler(BLEClientServiceChangeHandler handler);

//! Registers the filter list of Service UUIDs.
//! @param service_uuids An array of the Service UUIDs that the application is
//! interested in and the system should filter by. Passing NULL will discover
//! all services on the device.
//! @param num_uuids The number of Uuid`s in the service_uuids array. Ignored
//! when NULL is passed for the service_uuids argument.
//! @return BTErrnoOK if the filter was set up successfully,
//! or TODO....
BTErrno ble_client_set_service_filter(const Uuid service_uuids[],
                                      uint8_t num_uuids);

//! Starts a discovery of services and characteristics on a remote device.
//! The discovered services will be delivered to the application through the
//! BLEClientServiceChangeHandler. The results will be filtered with the list
//! of Service UUIDs as configured with ble_client_set_service_filter().
//! @param device The device for which to perform service discovery.
//! @return BTErrnoOK if the service discovery started successfully,
//! BTErrnoInvalidParameter if the device was not connected,
//! BTErrnoInvalidState if service discovery was already on-going, or
//! an internal error otherwise (>= BTErrnoInternalErrorBegin).
BTErrno ble_client_discover_services_and_characteristics(BTDevice device);

//! Different subscription types that can be used with ble_client_subscribe()
typedef enum {
  //! No subscription.
  BLESubscriptionNone = 0,
  //! Notification subscription.
  BLESubscriptionNotifications = (1 << 0),
  //! Indication subscription.
  BLESubscriptionIndications = (1 << 1),
  //! Any subscription. Use this value with ble_client_subscribe(), in case
  //! the application does not care about the type of subscription. If both
  //! types are supported by the server, the notification subscription type
  //! will be used.
  BLESubscriptionAny = BLESubscriptionNotifications | BLESubscriptionIndications,
} BLESubscription;

//! Callback to receive the characteristic value, resulting from either
//! ble_client_read() and/or ble_client_subscribe().
//! @param characteristic The characteristic of the received value
//! @param value Byte-array containing the value
//! @param value_length The number of bytes the byte-array contains
//! @param value_offset The offset in bytes from the start of the characteristic
//! value that has been read.
//! @param error The error or status as returned by the remote server. If the
//! read was successful, this remote server is supposed to send
//! BLEGATTErrorSuccess.
typedef void (*BLEClientReadHandler)(BLECharacteristic characteristic,
                                     const uint8_t *value,
                                     size_t value_length,
                                     uint16_t value_offset,
                                     BLEGATTError error);

//! Callback to handle the response to a written characteristic, resulting from
//! ble_client_write().
//! @param characteristic The characteristic that was written to.
//! @param error The error or status as returned by the remote server. If the
//! write was successful, this remote server is supposed to send
//! BLEGATTErrorSuccess.
typedef void (*BLEClientWriteHandler)(BLECharacteristic characteristic,
                                      BLEGATTError error);

//! Callback to handle the confirmation of a subscription or unsubscription to
//! characteristic value changes (notifications or indications).
//! @param characteristic The characteristic for which the client is now
//! (un)subscribed.
//! @param subscription_type The type of subscription. If the client is now
//! unsubscribed, the type will be BLESubscriptionNone.
//! @param The error or status as returned by the remote server. If the
//! (un)subscription was successful, this remote server is supposed to send
//! BLEGATTErrorSuccess.
typedef void (*BLEClientSubscribeHandler)(BLECharacteristic characteristic,
                                          BLESubscription subscription_type,
                                          BLEGATTError error);

//! Callback to handle the event that the buffer for outbound data is empty.
typedef void (*BLEClientBufferEmptyHandler)(void);

//! Registers the handler for characteristic value read operations.
//! @param read_handler Pointer to the function that will handle callbacks
//! with read characteristic values as result of calls to ble_client_read().
//! @return BTErrnoOK if the handlers were successfully registered, or ... TODO
BTErrno ble_client_set_read_handler(BLEClientReadHandler read_handler);

//! Registers the handler for characteristic value write (with response)
//! operations.
//! @param write_handler Pointer to the function that will handle callbacks
//! for written characteristic values as result of calls to ble_client_write().
//! @return BTErrnoOK if the handlers were successfully registered, or ... TODO
BTErrno ble_client_set_write_response_handler(BLEClientWriteHandler write_handler);

//! Registers the handler for characteristic value subscribe operations.
//! @param subscribe_handler Pointer to the function that will handle callbacks
//! for (un)subscription confirmations as result of calls to
//! ble_client_subscribe().
//! @return BTErrnoOK if the handlers were successfully registered, or ... TODO
BTErrno ble_client_set_subscribe_handler(BLEClientSubscribeHandler subscribe_handler);

//! Registers the handler to get called back when the buffer for outbound
//! data is empty again.
//! @param empty_handler Pointer to the function that will handle callbacks
//! for "buffer empty" events.
//! @return BTErrnoOK if the handlers were successfully registered, or ... TODO
BTErrno ble_client_set_buffer_empty_handler(BLEClientBufferEmptyHandler empty_handler);

//! Gets the maximum characteristic value size that can be written. This size
//! can vary depending on the connected device.
//! @param device The device for which to get the maximum characteristic value
//! size.
//! @return The maximum characteristic value size that can be written.
uint16_t ble_client_get_maximum_value_length(BTDevice device);

//! Read the value of a characteristic.
//! A call to this function will result in a callback to the registered
//! BLEClientReadHandler handler. @see ble_client_set_read_handler.
//! @param characteristic The characteristic for which to read the value
//! @return BTErrnoOK if the operation was successfully started, or ... TODO
BTErrno ble_client_read(BLECharacteristic characteristic);

//! Write the value of a characteristic.
//! A call to this function will result in a callback to the registered
//! BLEClientWriteHandler handler. @see ble_client_set_write_response_handler.
//! @param characteristic The characteristic for which to write the value
//! @param value Buffer with the value to write
//! @param value_length Number of bytes to write
//! @note Values must not be longer than ble_client_get_maximum_value_length().
//! @return BTErrnoOK if the operation was successfully started, or ... TODO
BTErrno ble_client_write(BLECharacteristic characteristic,
                         const uint8_t *value,
                         size_t value_length);

//! Write the value of a characteristic without response.
//! @param characteristic The characteristic for which to write the value
//! @param value Buffer with the value to write
//! @param value_length Number of bytes to write
//! @note Values must not be longer than ble_client_get_maximum_value_length().
//! @return BTErrnoOK if the operation was successfully started, or ... TODO
//! If the buffer for outbound data was full, BTErrnoNotEnoughResources will
//! be returned. When the buffer is emptied, the handler that is registered
//! using ble_client_set_buffer_empty_handler() will be called.
BTErrno ble_client_write_without_response(BLECharacteristic characteristic,
                                          const uint8_t *value,
                                          size_t value_length);

//! Subscribe to be notified or indicated of value changes of a characteristic.
//!
//! The value updates are delivered to the application through the
//! BLEClientReadHandler, which should be registered before calling this
//! function using ble_client_set_read_handler().
//!
//! There are two types of subscriptions: notifications and indications.
//! For notifications there is no flow-control. This means that notifications
//! can get dropped if the rate at which they are sent is too high. Conversely,
//! each indication needs an acknowledgement from the receiver before the next
//! one can get sent and is thus more reliable. The system performs
//! acknowledgements to indications automatically. Applications do not need to
//! worry about this, nor can they affect this.
//! @param characteristic The characteristic to subscribe to.
//! @param subscription_type The type of subscription to use.
//! If BLESubscriptionAny is used as subscription_type and both types are
//! supported by the server, the notification subscription type will be used.
//! @note This call does not block and returns quickly. A callback to
//! the BLEClientSubscribeHandler will happen at a later point in time, to
//! report the success or failure of the subscription. This handler should be
//! registered before calling this function using
//! ble_client_set_subscribe_handler().
//! @note Under the hood, this API writes to the Client Characteristic
//! Configuration Descriptor's Notifications or Indications enabled/disabled
//! bit.
//! @return BTErrnoOK if the subscription request was sent sucessfully, or
//! TODO...
BTErrno ble_client_subscribe(BLECharacteristic characteristic,
                             BLESubscription subscription_type);


//! Callback to receive the descriptor value, resulting from a call to
//! ble_client_read_descriptor().
//! @param descriptor The descriptor of the received value
//! @param value Byte-array containing the value
//! @param value_length The number of bytes the byte-array contains
//! @param value_offset The offset in bytes from the start of the descriptor
//! value that has been read.
//! @param error The error or status as returned by the remote server. If the
//! read was successful, this remote server is supposed to send
//! BLEGATTErrorSuccess.
typedef void (*BLEClientReadDescriptorHandler)(BLEDescriptor descriptor,
                                               const uint8_t *value,
                                               size_t value_length,
                                               uint16_t value_offset,
                                               BLEGATTError error);

//! Callback to handle the response to a written descriptor, resulting from
//! ble_client_write_descriptor().
//! @param descriptor The descriptor that was written to.
//! @param error The error or status as returned by the remote server. If the
//! write was successful, this remote server is supposed to send
//! BLEGATTErrorSuccess.
typedef void (*BLEClientWriteDescriptorHandler)(BLEDescriptor descriptor,
                                                BLEGATTError error);

//! Registers the handlers for descriptor value write operations.
//! @param write_handler Pointer to the function that will handle callbacks
//! for written descriptor values as result of calls to
//! ble_client_write_descriptor().
//! @return BTErrnoOK if the handlers were successfully registered, or ... TODO
BTErrno ble_client_set_descriptor_write_handler(BLEClientWriteDescriptorHandler write_handler);

//! Registers the handlers for descriptor value read operations.
//! @param read_handler Pointer to the function that will handle callbacks
//! with read descriptor values as result of calls to
//! ble_client_read_descriptor().
//! @return BTErrnoOK if the handlers were successfully registered, or ... TODO
BTErrno ble_client_set_descriptor_read_handler(BLEClientReadDescriptorHandler read_handler);

//! Write the value of a descriptor.
//! A call to this function will result in a callback to the registered
//! BLEClientWriteDescriptorHandler handler.
//! @see ble_client_set_descriptor_write_handler.
//! @param descriptor The descriptor for which to write the value
//! @param value Buffer with the value to write
//! @param value_length Number of bytes to write
//! @note Values must not be longer than ble_client_get_maximum_value_length().
//! @return BTErrnoOK if the operation was successfully started, or ... TODO
BTErrno ble_client_write_descriptor(BLEDescriptor descriptor,
                                    const uint8_t *value,
                                    size_t value_length);

//! Read the value of a descriptor.
//! A call to this function will result in a callback to the registered
//! BLEClientReadDescriptorHandler handler.
//! @see ble_client_set_descriptor_read_handler.
//! @param descriptor The descriptor for which to read the value
//! @return BTErrnoOK if the operation was successfully started, or ... TODO
BTErrno ble_client_read_descriptor(BLEDescriptor descriptor);

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// (FUTURE / LATER / NOT SCOPED)
// Just to see how symmetric the Server APIs would be:


//! Opaque ATT request context
typedef void * BLERequest;

typedef void (*BLEServerWriteHandler)(BLERequest request,
                                      BLECharacteristic characteristic,
                                      BTDevice remote_device,
                                      const uint8_t *value,
                                      size_t value_length,
                                      uint16_t value_offset);

typedef void (*BLEServerReadHandler)(BLECharacteristic characteristic,
                                     BTDevice remote_device,
                                     uint16_t value_offset);

typedef void (*BLEServerSubscribeHandler)(BLECharacteristic characteristic,
                                          BTDevice remote_device,
                                          BLESubscription subscription_type);

BTErrno ble_server_set_handlers(BLEServerReadHandler read_handler,
                            BLEServerWriteHandler write_handler,
                            BLEServerSubscribeHandler subscription_handler);

BTErrno ble_server_start_service(BLEService service);

BTErrno ble_server_stop_service(BLEService service);

BTErrno ble_server_respond_to_write(BLERequest request, BLEGATTError error);

BTErrno ble_server_respond_to_read(BLERequest request, BLEGATTError error,
                                   const uint8_t *value, size_t value_length,
                                   uint16_t value_offset);

BTErrno ble_server_send_update(BLECharacteristic characteristic,
                               const uint8_t *value, size_t value_length);

BTErrno ble_server_send_update_selectively(BLECharacteristic characteristic,
                                 const uint8_t *value, size_t value_length,
                                 const BTDevice *devices, uint8_t num_devices);
