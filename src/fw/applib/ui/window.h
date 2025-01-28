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

#include "click.h"

#include "applib/graphics/gtypes.h"
#include "applib/ui/layer.h"
#include "applib/ui/recognizer/recognizer.h"
#include "applib/ui/recognizer/recognizer_manager.h"

struct Window;
struct WindowStack;

//! @file window.h
//! @addtogroup UI
//! @{
//!   @addtogroup Window
//! \brief The basic building block of the user interface
//!
//! Windows are the top-level elements in the UI hierarchy and the basic building blocks for a Pebble
//! UI. A single window is always displayed at a time on Pebble, with the exception of when animating
//! from one window to the other, which, in that case, is managed by the window stack. You can stack
//! windows on top of each other, but only the topmost window will be visible.
//!
//! Users wearing a Pebble typically interact with the content and media displayed in a window, clicking
//! and pressing buttons on the watch, depending on what they see and wish to respond to in a window.
//!
//! Windows serve to display a hierarchy of layers on the screen and handle user input. When a window is
//! visible, its root Layer (and all its child layers) are drawn onto the screen automatically.
//!
//! You need a window, which always fills the entire screen, to display images, text, and graphics in
//! your Pebble app. A layer by itself doesn’t display on Pebble; it must be in the current window’s
//! layer hierarchy to be visible.
//!
//! The Window Stack serves as the global manager of what window is presented and makes sure that input
//! events are forwarded to the topmost window.
//!
//! Refer to the \htmlinclude UiFramework.html (chapter "Window") for a conceptual
//! overview of Window, the Window Stack and relevant code examples.
//!   @{


//! Function signature for a handler that deals with transition events of a window.
//! @see WindowHandlers
//! @see \ref window_set_window_handlers()
typedef void (*WindowHandler)(struct Window *window);

//! WindowHandlers
//! These handlers are called by the \ref WindowStack as windows get pushed on / popped.
//! All these handlers use \ref WindowHandler as their function signature.
//! @see \ref window_set_window_handlers()
//! @see \ref WindowStack
typedef struct WindowHandlers {
  //! Called when the window is pushed to the screen when it's not loaded.
  //! This is a good moment to do the layout of the window.
  WindowHandler load;

  //! Called when the window comes on the screen (again). E.g. when
  //! second-top-most window gets revealed (again) after popping the top-most
  //! window, but also when the window is pushed for the first time. This is a
  //! good moment to start timers related to the window, or reset the UI, etc.
  WindowHandler appear;

  //! Called when the window leaves the screen, e.g. when another window
  //! is pushed, or this window is popped. Good moment to stop timers related
  //! to the window.
  WindowHandler disappear;

  //! Called when the window is deinited, but could be used in the future to
  //! free resources bound to windows that are not on screen.
  WindowHandler unload;
} WindowHandlers;

//! Data structure of a window.
typedef struct Window {
  //! The root layer of the window.
  //! By default, the `.update_proc` will draw the background color of the window.
  Layer layer;

  //! The handlers that are called by the system whenever there are window transitions
  //! happening (load, appear, disappear and unload).
  //! @see \ref window_set_window_handlers()
  WindowHandlers window_handlers;

  //! The callback that will be called by the system to get the ClickRecognizer s
  //! set up for this window.
  //! @see \ref window_set_click_config_provider()
  //! @see \ref window_set_click_config_provider_with_context()
  ClickConfigProvider click_config_provider;

  //! Pointer to application specific data that will be passed into the
  //! `click_config_provider` callback.
  //! @see \ref window_set_click_config_provider_with_context()
  void *click_config_context;

  //! @internal
  //! Pointer to application specific data that the app can assign to a window.
  //! @see \ref window_set_user_data() and \ref window_get_user_data()
  void *user_data;

  //! The background color that will be used to fill the background of the window.
  //! @see \ref window_set_background_color()
  GColor8 background_color;

  bool is_render_scheduled:1;
  bool on_screen:1;
  bool is_loaded:1;
  bool overrides_back_button:1;
  bool is_fullscreen:1;
  bool in_click_config_provider:1;

  //! @internal
  //! If a click config provider was changed while the window was covered by a modal,
  //! this flag is used to indicate that it should be called when uncovered.
  bool is_waiting_for_click_config:1;

  //! @internal
  //! If the window has configured its click config provider. This flag is used to automatically
  //! click configure a window if a modal from above relinquishes focus either by going off screen
  //! or becoming unfocusable. This is necessary because windows can be unfocusable, thus whether
  //! they are on screen does not indicate whether their click has been configured.
  bool is_click_configured:1;

  //! @internal
  //! If the window can visually expose window stacks below it. This property decides whether the
  //! window stack would expose window stacks below it. A window stack is transparent if the top
  //! window in the window stack is transparent. Windows that are not the top window in their
  //! respective window stack are never shown, therefore transparent windows can only expose
  //! the top windows of window stacks below it, not windows within the same window stack.
  //! @note Currently, the app window stack is the lowest stack, is_transparent has no affect on
  //! app windows.
  bool is_transparent:1;

  //! @internal
  //! If the window passes input to the next window stack with a top focusable window. A window
  //! stack is unfocusable if the top window in the window stack is unfocusable. Windows that are
  //! not the top window in their respective window stack never receive input, therefore
  //! unfocusable windows can only pass input to the top windows of window stacks below it, not
  //! windows within the same window stack.
  bool is_unfocusable:1;

  //! @internal
  //! Back pointer to the window stack that this Window is residing on.
  //! @see \ref WindowStack
  struct WindowStack *parent_window_stack;

  const char* debug_name;
} Window;

#ifdef RELEASE
#define WINDOW_NAME(x) ""
#else
#define WINDOW_NAME(x) x
#endif

//! Initializes a window and resets its members to the default values:
//!
//! * Background color : `GColorWhite`
//! * Root layer's `update_proc` : function that fills the window's background using `background_color`.
//! * `click_config_provider` : `NULL`
//! * `window_handlers` : all `NULL`
//! @param window The window to initialize
//! @param debug_name The window's debug name
void window_init(Window *window, const char* debug_name);

//! Creates a new Window on the heap and initializes it with the default values.
//!
//! * Background color : `GColorWhite`
//! * Root layer's `update_proc` : function that fills the window's background using `background_color`.
//! * `click_config_provider` : `NULL`
//! * `window_handlers` : all `NULL`
//! @return A pointer to the window. `NULL` if the window could not
//! be created
Window* window_create(void);

//! Destroys a Window previously created by window_create.
void window_destroy(Window* window);

//! Deinitializes the window.
//! Removes the window from the screen, removes its child layers from the root layer and
//! finally calls the `.unload` window handler.
//! @note This function removes the window from the screen, even though it might still
//! be presented. Normally, the window stack will call this function automatically
//! after a window has been popped (removed) from the window stack.
//! @param window The window to deinitialize
void window_deinit(Window *window);

//! Sets the click configuration provider callback function on the window.
//! This will automatically setup the input handlers of the window as well to use
//! the click recognizer subsystem.
//! @param window The window for which to set the click config provider
//! @param click_config_provider The callback that will be called to configure the click recognizers with the window
//! @see Clicks
//! @see ClickConfigProvider
void window_set_click_config_provider(Window *window, ClickConfigProvider click_config_provider);

//! Same as window_set_click_config_provider(), but will assign a custom context pointer
//! (instead of the window pointer) that will be passed into the ClickHandler click event handlers.
//! @param window The window for which to set the click config provider
//! @param click_config_provider The callback that will be called to configure the click recognizers with the window
//! @param context Pointer to application specific data that will be passed to the click configuration provider callback (defaults to the window).
//! @see Clicks
//! @see window_set_click_config_provider
void window_set_click_config_provider_with_context(Window *window, ClickConfigProvider click_config_provider, void *context);

//! Set the context that will be passed to handlers for the given button's events. By default the context passed to handlers
//! is equal to the \ref ClickConfigProvider context (defaults to the window).
//! @note Must be called from within the \ref ClickConfigProvider.
//! @param button_id The button to set the context for.
//! @param context Set the context that will be passed to handlers for the given button's events.
void window_set_click_context(ButtonId button_id, void *context);

//! Subscribe to single click events.
//! @note Must be called from the \ref ClickConfigProvider.
//! @note \ref window_single_click_subscribe() and \ref window_single_repeating_click_subscribe() conflict, and cannot both be used on the same button.
//! @note When there is a multi_click and/or long_click setup, there will be a delay before the single click
//! @param button_id The button events to subscribe to.
//! @param handler The \ref ClickHandler to fire on this event.
//! handler will get fired. On the other hand, when there is no multi_click nor long_click setup, the single click handler will fire directly on button down.
//! @see ButtonId
//! @see Clicks
//! @see window_single_repeating_click_subscribe
void window_single_click_subscribe(ButtonId button_id, ClickHandler handler);

//! Subscribe to single click event, with a repeat interval. A single click is detected every time "repeat_interval_ms" has been reached.
//! @note Must be called from the \ref ClickConfigProvider.
//! @note \ref window_single_click_subscribe() and \ref window_single_repeating_click_subscribe() conflict, and cannot both be used on the same button.
//! @note The back button cannot be overridden with a repeating click.
//! @param button_id The button events to subscribe to.
//! @param repeat_interval_ms When holding down, how many milliseconds before the handler is fired again.
//! A value of 0ms means "no repeat timer". The minimum is 30ms, and values below will be disregarded.
//! If there is a long-click handler subscribed on this button, `repeat_interval_ms` will not be used.
//! @param handler The \ref ClickHandler to fire on this event.
//! @see window_single_click_subscribe
void window_single_repeating_click_subscribe(ButtonId button_id, uint16_t repeat_interval_ms, ClickHandler handler);

//! Subscribe to multi click events.
//! @note Must be called from the \ref ClickConfigProvider.
//! @param button_id The button events to subscribe to.
//! @param min_clicks Minimum number of clicks before handler is fired. Defaults to 2.
//! @param max_clicks Maximum number of clicks after which the click counter is reset. A value of 0 means use "min" also as "max".
//! @param timeout The delay after which a sequence of clicks is considered finished, and the click counter is reset. A value of 0 means to use the system default 300ms.
//! @param last_click_only Defaults to false. When true, only the handler for the last multi-click is called.
//! @param handler The \ref ClickHandler to fire on this event. Fired for multi-clicks, as "filtered" by the `last_click_only`, `min`, and `max` parameters.
void window_multi_click_subscribe(ButtonId button_id, uint8_t min_clicks, uint8_t max_clicks, uint16_t timeout, bool last_click_only, ClickHandler handler);

//! Subscribe to long click events.
//! @note Must be called from the \ref ClickConfigProvider.
//! @note The back button cannot be overridden with a long click.
//! @param button_id The button events to subscribe to.
//! @param delay_ms Milliseconds after which "handler" is fired. A value of 0 means to use the system default 500ms.
//! @param down_handler The \ref ClickHandler to fire as soon as the button has been held for `delay_ms`. This may be NULL to have no down handler.
//! @param up_handler The \ref ClickHandler to fire on the release of a long click. This may be NULL to have no up handler.
void window_long_click_subscribe(ButtonId button_id, uint16_t delay_ms, ClickHandler down_handler, ClickHandler up_handler);

//! Subscribe to raw click events.
//! @note Must be called from within the \ref ClickConfigProvider.
//! @note The back button cannot be overridden with a raw click.
//! @param button_id The button events to subscribe to.
//! @param down_handler The \ref ClickHandler to fire as soon as the button has been pressed. This may be NULL to have no down handler.
//! @param up_handler The \ref ClickHandler to fire on the release of the button. This may be NULL to have no up handler.
//! @param context If this context is not NULL, it will override the general context.
void window_raw_click_subscribe(ButtonId button_id, ClickHandler down_handler, ClickHandler up_handler, void *context);

//! Gets the current click configuration provider of the window.
//! @param window The window for which to get the click config provider
ClickConfigProvider window_get_click_config_provider(const Window *window);

//! Gets the current click configuration provider context of the window.
//! @param window The window for which to get the click config provider context
void *window_get_click_config_context(Window *window);

//! @internal
//! This function replaces \ref window_set_window_handlers_by_value in order to change the handlers
//! parameter to be passed by a pointer instead of being passed by value. Callers consume much less
//! code space when passing a pointer compared to passing structs by value.
//! @see window_set_window_handlers_by_value
void window_set_window_handlers(Window *window, const WindowHandlers *handlers);

//! Sets the window handlers of the window.
//! These handlers get called e.g. when the user enters or leaves the window.
//! @param window The window for which to set the window handlers
//! @param handlers The handlers for the specified window
//! @see \ref WindowHandlers
void window_set_window_handlers_by_value(Window *window, WindowHandlers handlers);

//! Sets a pointer to developer-supplied data that the window uses, to
//! provide a means to access the data at later times in one of the window event handlers.
//! @see window_get_user_data
//! @param window The window for which to set the user data
//! @param data A pointer to user data.
void window_set_user_data(Window *window, void *data);

//! Gets the pointer to developer-supplied data that was previously
//! set using window_set_user_data().
//! @see window_set_user_data
//! @param window The window for which to get the user data
void* window_get_user_data(const Window *window);

//! Gets the root Layer of the window.
//! The root layer is the layer at the bottom of the layer hierarchy for this window.
//! It is the window's "canvas" if you will. By default, the root layer only draws
//! a solid fill with the window's background color.
//! @param window The window for which to get the root layer
//! @return The window's root layer
struct Layer* window_get_root_layer(const Window *window);

//! Sets the background color of the window, which is drawn automatically by the
//! root layer of the window.
//! @param window The window for which to set the background color
//! @param background_color The new background color
//! @see \ref window_get_root_layer()
void window_set_background_color(Window *window, GColor background_color);
void window_set_background_color_2bit(Window *window, GColor2 background_color);

//! Sets whether or not the window is fullscreen, consequently hiding the sytem status bar.
//! @note This needs to be called before pushing a window to the window stack.
//! @param window The window for which to set its full-screen property
//! @param enabled True to make the window full-screen or false to leave space for the system status bar.
//! @see \ref window_get_fullscreen()
void window_set_fullscreen(Window *window, bool enabled);

//! Gets whether the window is full-screen, consequently hiding the sytem status bar.
//! @param window The window for which to get its full-screen property
//! @return True if the window is marked as fullscreen, false if it is not marked as fullscreen.
bool window_get_fullscreen(const Window *window);

//! Assigns an icon (max. 16x16 pixels) that can be displayed in the system status bar.
//! When no icon is assigned, the icon of the previous window on the window stack is used.
//! @note This needs to be called before pushing a window to the window stack.
//! @param window The window for which to set the status bar icon
//! @param icon The new status bar icon
void window_set_status_bar_icon(Window *window, const GBitmap *icon);

//! @internal
//! Internal window layer update proc to be called in window subclass layer update procs
void window_do_layer_update_proc(Layer *window, GContext* ctx);

//! @internal
//! This function gets called by the default render event handler.
//! When implementing a custom render event handler, call this function
//! to render the window into the context that gets passed in.
//! @see PebbleAppRenderEventHandler
//! @param window The window to render
void window_render(Window *window, GContext *ctx);

//! @internal
//! Returns true if the window is currently on top of the window stack, thus on screen.
//! @param window The window for which to set the `on_screen` boolean
bool window_is_on_screen(Window *window);

//! Gets whether the window has been loaded.
//! If a window is loaded, its `.load` handler has been called (and the `.unload` handler
//! has not been called since).
//! @return true if the window is currently loaded or false if not.
//! @param window The window to query its loaded status
//! @see \ref WindowHandlers
bool window_is_loaded(Window *window) ;

//! @internal
//! Sets whether a window is transparent. Transparent windows that are at the top of their stack
//! allow the next visible window stack beneath to show through transparent areas.
//! @note This currently has no affect on App windows.
void window_set_transparent(Window *window, bool transparent);

//! @internal
//! @return true if the window is transparent, otherwise false
bool window_is_transparent(Window *window);

//! @internal
//! Sets whether a window is focusable (e.g. can receive button events). Windows that are not
//! focusable that are at the top of their stack allow the next window stack beneath to receive
//! focus instead.
//! @note This is not recommended for use on App windows as it results in undefined behavior.
void window_set_focusable(Window *window, bool focusable);

//! @internal
//! @return true if the window is focusable, otherwise false
bool window_is_focusable(Window *window);

//! @internal
//! @return A name used to identify the window. If RELEASE is defined will just return "?"
//! @param window The window for which to get the debug name
const char* window_get_debug_name(Window *window);

//! @internal
void window_call_click_config_provider(Window *window, void *context);

//! Attach a recognizer to the window
//! @param window \ref Window to which to attach the \ref Recognizer
//! @param recognizer \ref Recognizer to attach
void window_attach_recognizer(Window *window, Recognizer *recognizer);

//! Detach a recognizer from the window
//! @param window \ref Window from which to detach the \ref Recognizer
//! @param recognizer \ref Recognizer to detach
void window_detach_recognizer(Window *window, Recognizer *recognizer);

//! Get the recognizers attached to a window
//! @param window \ref Window from which to get recognizers
//! @return recognizer list
RecognizerList *window_get_recognizer_list(Window *window);

//! Get the recognizer manager that manages recognizers attached to this window and all layers
//! attached to the window
RecognizerManager *window_get_recognizer_manager(Window *window);

//!   @} // end addtogroup Window
//! @} // end addtogroup UI
