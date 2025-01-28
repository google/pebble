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
#include "util/dict.h"
#include "util/list.h"

//! @addtogroup Foundation
//! @{
//!   @addtogroup AppMessage
//!
//!
//!
//! \brief Bi-directional communication between phone apps and Pebble watchapps
//!
//! AppMessage is a bi-directional messaging subsystem that enables communication between phone apps
//! and Pebble watchapps. This is accomplished by allowing phone and watchapps to exchange arbitrary
//! sets of key/value pairs. The key/value pairs are stored in the form of a Dictionary, the layout
//! of which is left for the application developer to define.
//!
//! AppMessage implements a push-oriented messaging protocol, enabling your app to call functions and
//! methods to push messages from Pebble to phone and vice versa. The protocol is symmetric: both Pebble
//! and the phone can send messages. All messages are acknowledged. In this context, there is no
//! client-server model, as such.
//!
//! During the sending phase, one side initiates the communication by transferring a dictionary over the air.
//! The other side then receives this message and is given an opportunity to perform actions on that data.
//! As soon as possible, the other side is expected to reply to the message with a simple acknowledgment
//! that the message was received successfully.
//!
//! PebbleKit JavaScript provides you with a set of standard JavaScript APIs that let your app receive messages
//! from the watch, make HTTP requests, and send new messages to the watch. AppMessage APIs are used to send and
//! receive data. A Pebble watchapp can use the resources of the connected phone to fetch information from web services,
//! send information to web APIs, or store login credentials. On the JavaScript side, you communicate
//! with Pebble via a Pebble object exposed in the namespace.
//!
//! Messages always need to get either ACKnowledged or "NACK'ed," that is, not acknowledged.
//! If not, messages will result in a time-out failure. The AppMessage subsystem takes care of this implicitly.
//! In the phone libraries, this step is a bit more explicit.
//!
//! The Pebble watch interfaces make a distinction between the Inbox and the Outbox calls. The Inbox
//! receives messages from the phone on the watch; the Outbox sends messages from the watch to the phone.
//! These two buffers can be managed separately.
//!
//! <h4>Warning</h4>
//! A critical constraint of AppMessage is that messages are limited in size. An ingoing (outgoing) message
//! larger than the inbox (outbox) will not be transmitted and will generate an error. You can choose your
//! inbox and outbox size when you call app_message_open().
//!
//! Pebble SDK provides a static minimum guaranteed size (APP_MESSAGE_INBOX_SIZE_MINIMUM and APP_MESSAGE_OUTBOX_SIZE_MINIMUM).
//! Requesting a buffer of the minimum guaranteed size (or smaller) is always guaranteed to succeed on all
//! Pebbles in this SDK version or higher, and with every phone.
//!
//! In some context, Pebble might be able to provide your application with larger inbox/outbox.
//! You can call app_message_inbox_size_maximum() and app_message_outbox_size_maximum() in your code to get
//! the largest possible value you can use.
//!
//! To always get the largest buffer available, follow this best practice:
//!
//! app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum())
//!
//! AppMessage uses your application heap space. That means that the sizes you pick for the AppMessage
//! inbox and outbox buffers are important in optimizing your app’s performance. The more you use for
//! AppMessage, the less space you’ll have for the rest of your app.
//!
//! To register callbacks, you should call app_message_register_inbox_received(), app_message_register_inbox_dropped(),
//! app_message_register_outbox_sent(), app_message_register_outbox_failed().
//!
//! Pebble recommends that you call them before app_message_open() to ensure you do not miss a message
//! arriving between starting AppMessage and registering the callback. You can set a context that will be passed
//! to all the callbacks with app_message_set_context().
//!
//! In circumstances that may not be ideal, when using AppMessage several types of errors may occur.
//! For example:
//!
//! * The send can’t start because the system state won't allow for a success. Several reasons
//!  you're unable to perform a send: A send() is already occurring (only one is possible at a time) or Bluetooth
//!  is not enabled or connected.
//! * The send and receive occur, but the receiver can’t accept the message. For instance, there is no app
//!   that receives such a message.
//! * The send occurs, but the receiver either does not actually receive the message or can’t handle it
//!   in a timely fashion.
//! * In the case of a dropped message, the phone sends a message to the watchapp, while there is still
//!   an unprocessed message in the Inbox.
//!
//! Other errors are possible and described by AppMessageResult. A client of the AppMessage interface
//! should use the result codes to be more robust in the face of communication problems either in the field or while debugging.
//!
//! Refer to the \htmlinclude app-phone-communication.html for a conceptual overview and code usage.
//!
//! For code examples, refer to the SDK Examples that directly use App Message. These include:
//!   * <a href="https://github.com/pebble-examples/pebblekit-js-weather">
//!     pebblekit-js-weather</a>
//!   * <a href="https://github.com/pebble-examples/pebblekit-js-quotes">
//!     pebblekit-js-quotes</a>
//!  @{
//!
//! AppMessage is a messaging subsystem that allows phone and watch applications
//! to exchange arbitrary sets of key-value pairs. The key value pairs are
//! stored in the form of a Dictionary, the layout of which is left for the
//! application developer to define.
//!
//! <h3>Communication Model</h3>
//!
//! AppMessage is a simple send-receive-reply protocol. The protocol is symmetric: both the watch and phone can start
//! sending a message and expect a reply from the other side.
//!
//! In the sending phase, one side initiates the communication by transferring a dictionary over the air. The other
//! side then receives this message and is given an opportunity to perform actions on that data.  As soon as possible,
//! the other side is expected to reply to the message with a simple acknowledgement that the message was received
//! successfully.
//!
//! In non-ideal circumstances, several errors may occur. For example:
//! * The send can't start as the system state won't allow for a success.
//! * The send and receive occur, but the receiver cannot accept the message (for example, there is no app that receives such
//!   a message).
//! * The send occurs, but the receiver either does not actually receive the message or can't handle it in a timely
//!   fashion.
//!
//! Other errors are possible, described by \ref AppMessageResult. A client of the AppMessage interface
//! can use the result codes to be more robust in the face of communication problems either in the field or while
//! debugging.
//!
//! The watch interfaces make a distinction between the Inbox and the Outbox. The Inbox receives messages from the
//! phone on the watch; the Outbox sends messages from the watch to the phone. These two objects can be managed
//! separately.
//!
//! \note Messages are actually addressed by the UUID of the watch and phone apps.  This is done automatically by the
//!       system for the convenience of the client.  However, this does require that both the watch and phone apps
//!       share their UUID.  AppMessage is not capable of 1:N or M:N communication at this time, and is merely 1:1.
//!
//! \sa AppMessageResult

// -------- Defines, Enumerations, and Structures ------------------------------------------------------------------ //

//! As long as the firmware maintains its current major version, inboxes of this size or smaller will be allowed.
//!
//! \sa app_message_inbox_size_maximum()
//! \sa APP_MESSAGE_OUTBOX_SIZE_MINIMUM
//!
#define APP_MESSAGE_INBOX_SIZE_MINIMUM 124 /* bytes */

//! As long as the firmware maintains its current major version, outboxes of this size or smaller will be allowed.
//!
//! \sa app_message_outbox_size_maximum()
//! \sa APP_MESSAGE_INBOX_SIZE_MINIMUM
//!
#define APP_MESSAGE_OUTBOX_SIZE_MINIMUM 636 /* bytes */

//! AppMessage result codes.
typedef enum {
  //! (0) All good, operation was successful.
  APP_MSG_OK = 0,

  //! (2) The other end did not confirm receiving the sent data with an (n)ack in time.
  APP_MSG_SEND_TIMEOUT = 1 << 1,

  //! (4) The other end rejected the sent data, with a "nack" reply.
  APP_MSG_SEND_REJECTED = 1 << 2,

  //! (8) The other end was not connected.
  APP_MSG_NOT_CONNECTED = 1 << 3,

  //! (16) The local application was not running.
  APP_MSG_APP_NOT_RUNNING = 1 << 4,

  //! (32) The function was called with invalid arguments.
  APP_MSG_INVALID_ARGS = 1 << 5,

  //! (64) There are pending (in or outbound) messages that need to be processed first before
  //! new ones can be received or sent.
  APP_MSG_BUSY = 1 << 6,

  //! (128) The buffer was too small to contain the incoming message.
  //! @internal
  //! @see \ref app_message_open()
  APP_MSG_BUFFER_OVERFLOW = 1 << 7,

  //! (512) The resource had already been released.
  APP_MSG_ALREADY_RELEASED = 1 << 9,

  //! (1024) The callback was already registered.
  APP_MSG_CALLBACK_ALREADY_REGISTERED = 1 << 10,

  //! (2048) The callback could not be deregistered, because it had not been registered before.
  APP_MSG_CALLBACK_NOT_REGISTERED = 1 << 11,

  //! (4096) The system did not have sufficient application memory to
  //! perform the requested operation.
  APP_MSG_OUT_OF_MEMORY = 1 << 12,

  //! (8192) App message was closed.
  APP_MSG_CLOSED = 1 << 13,

  //! (16384) An internal OS error prevented AppMessage from completing an operation.
  APP_MSG_INTERNAL_ERROR = 1 << 14,

  //! (32768) The function was called while App Message was not in the appropriate state.
  APP_MSG_INVALID_STATE = 1 << 15,
} AppMessageResult;

//! Called after an incoming message is received.
//!
//! \param[in] iterator
//!   The dictionary iterator to the received message.  Never NULL.  Note that the iterator cannot be modified or
//!   saved off.  The library may need to re-use the buffered space where this message is supplied.  Returning from
//!   the callback indicates to the library that the received message contents are no longer needed or have already
//!   been externalized outside its buffering space and iterator.
//!
//! \param[in] context
//!   Pointer to application data as specified when registering the callback.
//!
typedef void (*AppMessageInboxReceived)(DictionaryIterator *iterator, void *context);

//! Called after an incoming message is dropped.
//!
//! \param[in] result
//!   The reason why the message was dropped.  Some possibilities include \ref APP_MSG_BUSY and
//!   \ref APP_MSG_BUFFER_OVERFLOW.
//!
//! \param[in] context
//!   Pointer to application data as specified when registering the callback.
//!
//! Note that you can call app_message_outbox_begin() from this handler to prepare a new message.
//! This will invalidate the previous dictionary iterator; do not use it after calling app_message_outbox_begin().
//!
typedef void (*AppMessageInboxDropped)(AppMessageResult reason, void *context);

//! Called after an outbound message has been sent and the reply has been received.
//!
//! \param[in] iterator
//!   The dictionary iterator to the sent message.  The iterator will be in the final state that was sent.  Note that
//!   the iterator cannot be modified or saved off as the library will re-open the dictionary with dict_begin() after
//!   this callback returns.
//!
//! \param[in] context
//!   Pointer to application data as specified when registering the callback.
//!
typedef void (*AppMessageOutboxSent)(DictionaryIterator *iterator, void *context);

//! Called after an outbound message has not been sent successfully.
//!
//! \param[in] iterator
//!   The dictionary iterator to the sent message.  The iterator will be in the final state that was sent.  Note that
//!   the iterator cannot be modified or saved off as the library will re-open the dictionary with dict_begin() after
//!   this callback returns.
//!
//! \param[in] result
//!   The result of the operation.  Some possibilities for the value include \ref APP_MSG_SEND_TIMEOUT,
//!   \ref APP_MSG_SEND_REJECTED, \ref APP_MSG_NOT_CONNECTED, \ref APP_MSG_APP_NOT_RUNNING, and the combination
//!   `(APP_MSG_NOT_CONNECTED | APP_MSG_APP_NOT_RUNNING)`.
//!
//! \param context
//!   Pointer to application data as specified when registering the callback.
//!
//! Note that you can call app_message_outbox_begin() from this handler to prepare a new message.
//! This will invalidate the previous dictionary iterator; do not use it after calling app_message_outbox_begin().
//!
typedef void (*AppMessageOutboxFailed)(DictionaryIterator *iterator, AppMessageResult reason, void *context);


// -------- AppMessage Callbacks ----------------------------------------------------------------------------------- //

//! Gets the context that will be passed to all AppMessage callbacks.
//!
//! \return The current context on record.
//!
void *app_message_get_context(void);

//! Sets the context that will be passed to all AppMessage callbacks.
//!
//! \param[in] context The context that will be passed to all AppMessage callbacks.
//!
//! \return The previous context that was on record.
//!
void *app_message_set_context(void *context);

//! Registers a function that will be called after any Inbox message is received successfully.
//!
//! Only one callback may be registered at a time.  Each subsequent call to this function will replace the previous
//! callback.  The callback is optional; setting it to NULL will deregister the current callback and no function will
//! be called anymore.
//!
//! \param[in] received_callback The callback that will be called going forward; NULL to not have a callback.
//!
//! \return The previous callback (or NULL) that was on record.
//!
AppMessageInboxReceived app_message_register_inbox_received(AppMessageInboxReceived received_callback);

//! Registers a function that will be called after any Inbox message is received but dropped by the system.
//!
//! Only one callback may be registered at a time.  Each subsequent call to this function will replace the previous
//! callback.  The callback is optional; setting it to NULL will deregister the current callback and no function will
//! be called anymore.
//!
//! \param[in] dropped_callback The callback that will be called going forward; NULL to not have a callback.
//!
//! \return The previous callback (or NULL) that was on record.
//!
AppMessageInboxDropped app_message_register_inbox_dropped(AppMessageInboxDropped dropped_callback);

//! Registers a function that will be called after any Outbox message is sent and an ACK reply occurs in a timely
//! fashion.
//!
//! Only one callback may be registered at a time.  Each subsequent call to this function will replace the previous
//! callback.  The callback is optional; setting it to NULL will deregister the current callback and no function will
//! be called anymore.
//!
//! \param[in] sent_callback The callback that will be called going forward; NULL to not have a callback.
//!
//! \return The previous callback (or NULL) that was on record.
//!
AppMessageOutboxSent app_message_register_outbox_sent(AppMessageOutboxSent sent_callback);

//! Registers a function that will be called after any Outbox message is not sent with a timely ACK reply.
//! The call to \ref app_message_outbox_send() must have succeeded.
//!
//! Only one callback may be registered at a time.  Each subsequent call to this function will replace the previous
//! callback.  The callback is optional; setting it to NULL will deregister the current callback and no function will
//! be called anymore.
//!
//! \param[in] failed_callback The callback that will be called going forward; NULL to not have a callback.
//!
//! \return The previous callback (or NULL) that was on record.
//!
AppMessageOutboxFailed app_message_register_outbox_failed(AppMessageOutboxFailed failed_callback);

//! Deregisters all callbacks and their context.
//!
void app_message_deregister_callbacks(void);

// -------- AppMessage Lifecycle ----------------------------------------------------------------------------------- //

//! Programmatically determine the inbox size maximum in the current configuration.
//!
//! \return The inbox size maximum on this firmware.
//!
//! \sa APP_MESSAGE_INBOX_SIZE_MINIMUM
//! \sa app_message_outbox_size_maximum()
//!
uint32_t app_message_inbox_size_maximum(void);

//! Programmatically determine the outbox size maximum in the current configuration.
//!
//! \return The outbox size maximum on this firmware.
//!
//! \sa APP_MESSAGE_OUTBOX_SIZE_MINIMUM
//! \sa app_message_inbox_size_maximum()
//!
uint32_t app_message_outbox_size_maximum(void);

//! Open AppMessage to transfers.
//!
//! Use \ref dict_calc_buffer_size_from_tuplets() or \ref dict_calc_buffer_size() to estimate the size you need.
//!
//! \param[in] size_inbound The required size for the Inbox buffer
//! \param[in] size_outbound The required size for the Outbox buffer
//!
//! \return A result code such as \ref APP_MSG_OK or \ref APP_MSG_OUT_OF_MEMORY.
//!
//! \note It is recommended that if the Inbox will be used, that at least the Inbox callbacks should be registered
//!   before this call.  Otherwise it is possible for an Inbox message to be NACK'ed without being seen by the
//!   application.
//!
AppMessageResult app_message_open(const uint32_t size_inbound, const uint32_t size_outbound);

//! Close AppMessage to further transfers.
//!
void app_message_close(void);


// -------- AppMessage Inbox --------------------------------------------------------------------------------------- //

// Note: the Inbox has no direct functions, only callbacks.


// -------- AppMessage Outbox -------------------------------------------------------------------------------------- //

//! Begin writing to the Outbox's Dictionary buffer.
//!
//! \param[out] iterator Location to write the DictionaryIterator pointer.  This will be NULL on failure.
//!
//! \return A result code, including but not limited to \ref APP_MSG_OK, \ref APP_MSG_INVALID_ARGS or
//!   \ref APP_MSG_BUSY.
//!
//! \note After a successful call, one can add values to the dictionary using functions like \ref dict_write_data()
//!   and friends.
//!
//! \sa Dictionary
//!
AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator);

//! Sends the outbound dictionary.
//!
//! \return A result code, including but not limited to \ref APP_MSG_OK or \ref APP_MSG_BUSY.  The APP_MSG_OK code does
//!         not mean that the message was sent successfully, but only that the start of processing was successful.
//!         Since this call is asynchronous, callbacks provide the final result instead.
//!
//! \sa AppMessageOutboxSent
//! \sa AppMessageOutboxFailed
//!
AppMessageResult app_message_outbox_send(void);

//!   @} // end addtogroup AppMessage
//! @} // end addtogroup Foundation
