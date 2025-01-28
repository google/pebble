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
#include "content_indicator.h"
#include "layer.h"
#include "property_animation.h"

//! @file scroll_layer.h
//! @addtogroup UI
//! @{
//!   @addtogroup Layer Layers
//!   @{
//!     @addtogroup ScrollLayer
//! \brief Layer that scrolls its contents, animated.
//!
//! ![](scroll_layer.png)
//! <h3>Key Points</h3>
//! * Facilitates vertical scrolling of a layer sub-hierarchy zero or more
//! arbitrary layers. The example image shows a scroll layer containing one
//! large TextLayer.
//! * Shadows to indicate that there is more content are automatically drawn
//! on top of the content. When the end of the scroll layer is reached, the
//! shadow will automatically be retracted.
//! * Scrolling from one offset to another is animated implicitly by default.
//! * The scroll layer contains a "content" sub-layer, which is the layer that
//! is actually moved up an down. Any layer that is a child of this "content"
//! sub-layer, will be moved as well. Effectively, an entire layout of layers
//! can be scrolled this way. Use the convenience function
//! \ref scroll_layer_add_child() to add child layers to the "content" sub-layer.
//! * The scroll layer needs to be informed of the total size of the contents,
//! in order to calculate from and to what point it should be able to scroll.
//! Use \ref scroll_layer_set_content_size() to set the size of the contents.
//! * The button behavior is set up, using the convenience function
//! \ref scroll_layer_set_click_config_onto_window(). This will associate the
//! UP and DOWN buttons with scrolling up and down.
//! * The SELECT button can be configured by installing a click configuration
//! provider using \ref scroll_layer_set_callbacks().
//! * To scroll programatically to a certain offset, use
//! \ref scroll_layer_set_content_offset().
//! * It is possible to get called back for each scrolling increment, by
//! installing the `.content_offset_changed_handler` callback using
//! \ref scroll_layer_set_callbacks().
//! * Only vertical scrolling is supported at the moment.
//!     @{

struct Window;
struct ScrollLayer;

//! Function signature for the `.content_offset_changed_handler` callback.
typedef void (*ScrollLayerCallback)(struct ScrollLayer *scroll_layer, void *context);

//! All the callbacks that the ScrollLayer exposes for use by applications.
//! @note The context parameter can be set using scroll_layer_set_context() and
//! gets passed in as context with all of these callbacks.
typedef struct ScrollLayerCallbacks {

  //! Provider function to set up the SELECT button handlers. This will be
  //! called after the scroll layer has configured the click configurations for
  //! the up/down buttons, so it can also be used to modify the default up/down
  //! scrolling behavior.
  ClickConfigProvider click_config_provider;

  //! Called every time the the content offset changes. During a scrolling
  //! animation, it will be called for each intermediary offset as well
  ScrollLayerCallback content_offset_changed_handler;

} ScrollLayerCallbacks;

//! Data structure of a scroll layer.
//! @note a `ScrollLayer *` can safely be casted to a `Layer *` and can thus be
//! used with all other functions that take a `Layer *` as an argument.
//! <br/>For example, the following is legal:
//! \code{.c}
//! ScrollLayer scroll_layer;
//! ...
//! layer_set_hidden((Layer *)&scroll_layer, true);
//! \endcode
//! @note However, there are a few caveats:
//! * To add content layers, you must use \ref scroll_layer_add_child().
//! * To change the frame of a scroll layer, use \ref scroll_layer_set_frame().
//! * Do not try to change to bounds of a scroll layer.
typedef struct ScrollLayer {
  //! @internal
  //! Empty container layer that is parent to the content & shadow sublayers.
  Layer layer;

  //! @internal
  //! The layer that is actually scrolled. Use scroll_layer_add_child() to add
  //! your content.
  Layer content_sublayer;

  union {
    //! @internal
    //! Layer that draws "more content" shadows when appropriate.
    Layer shadow_sublayer;  // deprecated
    //! @internal
    //! Paging data aligned with shadow_sublayer struct to save bytes
    //! Do not change the position or size of these items
    struct ScrollPaging {
      uint8_t padding[16];  // padding to align paging data with shadow_sublayer
      union {
        uint8_t flags;
        struct {
          // Do not move these bit-fields!
          bool paging_disabled:1;  // mutually exclusive of shadow clips
          bool shadow_hidden:1;  // shadow_hidden in same position as layer hidden
        };
      };
    } paging;
  };

  //! @internal
  //! Scrolling animation state. Configured automatically.
  PropertyAnimation *animation;

  //! @internal
  //! Application supplied callbacks.
  //! Use \ref scroll_layer_set_callbacks() to assign the callbacks.
  ScrollLayerCallbacks callbacks;

  //! @internal
  //! Application supplied callback context.
  //! Use \ref scroll_layer_set_context() to assign the callback context.
  void *context;
} ScrollLayer;

#ifndef __clang__
_Static_assert(offsetof(struct ScrollPaging, flags) == offsetof(Layer, flags),
               "ScrollPaging struct alignment with shadow_sublayer broken");
#endif

//! @internal
//! Initializes the given scroller, sets its frame and bounds and resets it
//! to the defaults:
//! * Clips: `true`
//! * Hidden: `false`
//! * Content size: `frame.size`
//! * Content offset: \ref GPointZero
//! * Callbacks: None (`NULL` for each one)
//! * Callback context: `NULL`
//! The layer is marked dirty automatically.
//! @param scroll_layer The ScrollLayer to initialize
//! @param frame The frame with which to initialize the ScrollLayer
void scroll_layer_init(ScrollLayer *scroll_layer, const GRect *frame);

//! Creates a new ScrollLayer on the heap and initializes it with the default values:
//! * Clips: `true`
//! * Hidden: `false`
//! * Content size: `frame.size`
//! * Content offset: \ref GPointZero
//! * Callbacks: None (`NULL` for each one)
//! * Callback context: `NULL`
//! @return A pointer to the ScrollLayer. `NULL` if the ScrollLayer could not
//! be created
ScrollLayer* scroll_layer_create(GRect frame);

void scroll_layer_deinit(ScrollLayer *scroll_layer);

//! Destroys a ScrollLayer previously created by scroll_layer_create.
void scroll_layer_destroy(ScrollLayer *scroll_layer);

//! Gets the "root" Layer of the scroll layer, which is the parent for the sub-
//! layers used for its implementation.
//! @param scroll_layer Pointer to the ScrollLayer for which to get the "root" Layer
//! @return The "root" Layer of the scroll layer.
//! @internal
//! @note The result is always equal to `(Layer *) scroll_layer`.
Layer* scroll_layer_get_layer(const ScrollLayer *scroll_layer);

//! Adds the child layer to the content sub-layer of the ScrollLayer.
//! This will make the child layer part of the scrollable contents.
//! The content sub-layer of the ScrollLayer will become the parent of the
//! child layer.
//! @param scroll_layer The ScrollLayer to which to add the child layer.
//! @param child The Layer to add to the content sub-layer of the ScrollLayer.
//! @note You may need to update the size of the scrollable contents using
//! \ref scroll_layer_set_content_size().
void scroll_layer_add_child(ScrollLayer *scroll_layer, Layer *child);

//! Convenience function to set the \ref ClickConfigProvider callback on the
//! given window to scroll layer's internal click config provider. This internal
//! click configuration provider, will set up the default UP & DOWN
//! scrolling behavior.
//! This function calls \ref window_set_click_config_provider_with_context to
//! accomplish this.
//!
//! If you application has set a `.click_config_provider`
//! callback using \ref scroll_layer_set_callbacks(), this will be called
//! by the internal click config provider, after configuring the UP & DOWN
//! buttons. This allows your application to configure the SELECT button
//! behavior and optionally override the UP & DOWN
//! button behavior. The callback context for the SELECT click recognizer is
//! automatically set to the scroll layer's context (see
//! \ref scroll_layer_set_context() ). This context is passed into
//! \ref ClickHandler callbacks. For the UP and DOWN buttons, the scroll layer
//! itself is passed in by default as the callback context in order to deal with
//! those buttons presses to scroll up and down automatically.
//! @param scroll_layer The ScrollLayer that needs to receive click events.
//! @param window The window for which to set the click configuration.
//! @see \ref Clicks
//! @see window_set_click_config_provider_with_context
void scroll_layer_set_click_config_onto_window(ScrollLayer *scroll_layer, struct Window *window);

//! Sets the callbacks that the scroll layer exposes.
//! The `context` as set by \ref scroll_layer_set_context() is passed into each
//! of the callbacks. See \ref ScrollLayerCallbacks for the different callbacks.
//! @note If the `context` is NULL, a pointer to scroll_layer is used
//! as context parameter instead when calling callbacks.
//! @param scroll_layer The ScrollLayer for which to assign new callbacks.
//! @param callbacks The new callbacks.
void scroll_layer_set_callbacks(ScrollLayer *scroll_layer, ScrollLayerCallbacks callbacks);

//! Sets a new callback context. This context is passed into the scroll layer's
//! callbacks and also the \ref ClickHandler for the SELECT button.
//! If `NULL` or not set, the context defaults to a pointer to the ScrollLayer
//! itself.
//! @param scroll_layer The ScrollLayer for which to assign the new callback
//! context.
//! @param context The new callback context.
//! @see scroll_layer_set_click_config_onto_window
//! @see scroll_layer_set_callbacks
void scroll_layer_set_context(ScrollLayer *scroll_layer, void *context);

//! Scrolls to the given offset, optionally animated.
//! @note When scrolling down, the offset's `.y` decrements. When scrolling up,
//! the offset's `.y` increments. If scrolled completely to the top, the offset
//! is \ref GPointZero.
//! @note The `.x` field must be `0`. Horizontal scrolling is not supported.
//! @param scroll_layer The ScrollLayer for which to set the content offset
//! @param offset The final content offset
//! @param animated Pass in `true` to animate to the new content offset, or
//! `false` to set the new content offset without animating.
//! @see scroll_layer_get_content_offset
void scroll_layer_set_content_offset(ScrollLayer *scroll_layer, GPoint offset, bool animated);

//! Gets the point by which the contents are offset.
//! @param scroll_layer The ScrollLayer for which to get the content offset
//! @see scroll_layer_set_content_offset
GPoint scroll_layer_get_content_offset(ScrollLayer *scroll_layer);

//! Sets the size of the contents layer. This determines the area that is
//! scrollable. At the moment, this needs to be set "manually" and is not
//! derived from the geometry of the contents layers.
//! @param scroll_layer The ScrollLayer for which to set the content size.
//! @param size The new content size.
//! @see scroll_layer_get_content_size
void scroll_layer_set_content_size(ScrollLayer *scroll_layer, GSize size);

//! Gets the size of the contents layer.
//! @param scroll_layer The ScrollLayer for which to get the content size
//! @see scroll_layer_set_content_size
GSize scroll_layer_get_content_size(const ScrollLayer *scroll_layer);

//! Set the frame of the scroll layer and adjusts the internal layers' geometry
//! accordingly. The scroll layer is marked dirty automatically.
//! @param scroll_layer The ScrollLayer for which to set the frame
//! @param frame The new frame
void scroll_layer_set_frame(ScrollLayer *scroll_layer, GRect frame);

//! The click handlers for the UP button that the scroll layer will install as
//! part of \ref scroll_layer_set_click_config_onto_window().
//! @note This handler is exposed, in case one wants to implement an alternative
//! handler for the UP button, as a way to invoke the default behavior.
//! @param recognizer The click recognizer for which the handler is called
//! @param context A void pointer to the ScrollLayer that is the context of the click event
void scroll_layer_scroll_up_click_handler(ClickRecognizerRef recognizer, void *context);

//! The click handlers for the DOWN button that the scroll layer will install as
//! part of \ref scroll_layer_set_click_config_onto_window().
//! @note This handler is exposed, in case one wants to implement an alternative
//! handler for the DOWN button, as a way to invoke the default behavior.
//! @param recognizer The click recognizer for which the handler is called
//! @param context A void pointer to the ScrollLayer that is the context of the click event
void scroll_layer_scroll_down_click_handler(ClickRecognizerRef recognizer, void *context);

//! Sets the visibility of the scroll layer shadow.
//! If the visibility has changed, \ref layer_mark_dirty() will be called automatically
//! on the scroll layer.
//! @param scroll_layer The scroll layer for which to set the shadow visibility
//! @param hidden Supply `true` to make the shadow hidden, or `false` to make it
//! non-hidden.
void scroll_layer_set_shadow_hidden(ScrollLayer *scroll_layer, bool hidden);

//! Gets the visibility of the scroll layer shadow.
//! @param scroll_layer The scroll layer for which to get the visibility
//! @return True if the shadow is hidden, false if it is not hidden.
bool scroll_layer_get_shadow_hidden(const ScrollLayer *scroll_layer);

//! @internal
void scroll_layer_scroll(ScrollLayer *scroll_layer, ScrollDirection direction, bool animated);

//! Enables or disables paging of the ScrollLayer (default: disabled). When enabled, every button
//! press will change the scroll offset by the frame's height.
//! @param scroll_layer The scroll layer for which to enable or disable paging
//! @param paging_enabled True, if paging should be enabled. False to enable.
void scroll_layer_set_paging(ScrollLayer *scroll_layer, bool paging_enabled);

//! Check whether or not the ScrollLayer uses paging when pressing buttons.
//! @param scroll_layer The scroll layer for which to get the paging behavior.
//! @return True, if paging is enabled; false otherwise.
bool scroll_layer_get_paging(ScrollLayer *scroll_layer);

//! Gets the ContentIndicator for a ScrollLayer.
//! @param scroll_layer The ScrollLayer for which to get the ContentIndicator
//! @return A pointer to the ContentIndicator, or `NULL` upon failure.
ContentIndicator *scroll_layer_get_content_indicator(ScrollLayer *scroll_layer);

//! @internal
//! Updates the ContentIndicator for a ScrollLayer.
//! @param scroll_layer The ScrollLayer for which to update the ContentIndicator
void scroll_layer_update_content_indicator(ScrollLayer *scroll_layer);

//! @internal
//! Controls if the ScrollLayer respects its content_size when scrolling (default: true).
//! If set to false, any value for content_offset will be accepted.
//! @see \ref scroll_layer_get_clips_content_offset
//! @see \ref scroll_layer_set_content_offset
void scroll_layer_set_clips_content_offset(ScrollLayer *scroll_layer, bool clips);

//! @internal
//! True, if the ScrollLayer respects the content_size when scrolling; otherwise, false.
//! @see \ref scroll_layer_get_clips_content_offset
bool scroll_layer_get_clips_content_offset(ScrollLayer *scroll_layer);

//! @internal
//! True, if the passed layer is a scroll_layer; false otherwise.
bool scroll_layer_is_instance(const Layer *layer);

//!     @} // end addtogroup ScrollLayer
//!   @} // end addtogroup Layer
//! @} // end addtogroup UI
