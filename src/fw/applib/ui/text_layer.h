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

#include "applib/ui/layer.h"
#include "applib/graphics/text.h"

//! @file text_layer.h
//! @addtogroup UI
//! @{
//!   @addtogroup Layer Layers
//!   @{
//!     @addtogroup TextLayer
//! \brief Layer that displays and formats a text string.
//!
//! ![](text_layer.png)
//! The geometric information (bounds, frame) of the Layer
//! is used as the "box" in which the text is drawn. The \ref TextLayer also has a number of
//! other properties that influence how the text is drawn. Most important of these properties are:
//! a pointer to the string to draw itself, the font, the text color, the background color of the
//! layer, the overflow mode and alignment of the text inside the layer.
//! @see Layer
//! @see TextDrawing
//! @see Fonts
//!     @{

//! The data structure of a TextLayer.
//! @note a `TextLayer *` can safely be casted to a `Layer *` and can thus be used
//! with all other functions that take a `Layer *` as an argument.
//! <br/>For example, the following is legal:
//! \code{.c}
//! TextLayer text_layer;
//! ...
//! layer_set_hidden((Layer *)&text_layer, true);
//! \endcode
typedef struct TextLayer {
  Layer layer;
  const char* text;
  GFont font;
  GTextLayoutCacheRef layout_cache;
  GColor8 text_color;
  GColor8 background_color;
  GTextOverflowMode overflow_mode:2;
  GTextAlignment text_alignment:2;
  bool should_cache_layout:1;
} TextLayer;

//! Initializes the TextLayer with given frame
//! All previous contents are erased and the following default values are set:
//! * Font: Raster Gothic Boldface 14-point or 18-point depending on the platform
//! * Text Alignment: \ref GTextAlignmentLeft
//! * Text color: \ref GColorBlack
//! * Background color: \ref GColorWhite
//! * Clips: `true`
//! * Hidden: `false`
//! * Caching: `false`
//!
//! The text layer is automatically marked dirty after this operation.
//! @param text_layer The TextLayer to initialize
//! @param frame The frame with which to initialize the TextLayer
void text_layer_init(TextLayer *text_layer, const GRect *frame);

//! Creates a new TextLayer on the heap and initializes it with the default values.
//!
//! * Font: Raster Gothic 14-point Boldface (system font)
//! * Text Alignment: \ref GTextAlignmentLeft
//! * Text color: \ref GColorBlack
//! * Background color: \ref GColorWhite
//! * Clips: `true`
//! * Hidden: `false`
//! * Caching: `false`
//!
//! The text layer is automatically marked dirty after this operation.
//! @param frame The frame with which to initialize the TextLayer
//! @return A pointer to the TextLayer. `NULL` if the TextLayer could not
//! be created
TextLayer* text_layer_create(GRect frame);

//! Destroys a TextLayer previously created by text_layer_create.
void text_layer_destroy(TextLayer* text_layer);

//! Deinitializes the TextLayer and frees any caches.
//! @param text_layer The TextLayer to deinitialize
//! @internal
//! @see text_layer_set_should_cache_layout
//! @note This MUST be called after discarding the text layer when using layout caching.
void text_layer_deinit(TextLayer *text_layer);

//! Gets the "root" Layer of the text layer, which is the parent for the sub-
//! layers used for its implementation.
//! @param text_layer Pointer to the TextLayer for which to get the "root" Layer
//! @return The "root" Layer of the text layer.
//! @internal
//! @note The result is always equal to `(Layer *) text_layer`.
Layer* text_layer_get_layer(TextLayer *text_layer);

//! Sets the pointer to the string where the TextLayer is supposed to find the text
//! at a later point in time, when it needs to draw itself.
//! @param text_layer The TextLayer of which to set the text
//! @param text The new text to set onto the TextLayer. This must be a null-terminated and valid UTF-8 string!
//! @note The string is not copied, so its buffer most likely cannot be stack allocated,
//! but is recommended to be a buffer that is long-lived, at least as long as the TextLayer
//! is part of a visible Layer hierarchy.
//! @see text_layer_get_text
void text_layer_set_text(TextLayer *text_layer, const char *text);

//! Gets the pointer to the string that the TextLayer is using.
//! @param text_layer The TextLayer for which to get the text
//! @see text_layer_set_text
const char* text_layer_get_text(TextLayer *text_layer);

//! Sets the background color of the bounding box that will be drawn behind the text
//! @param text_layer The TextLayer of which to set the background color
//! @param color The new \ref GColor to set the background to
//! @see text_layer_set_text_color
void text_layer_set_background_color(TextLayer *text_layer, GColor color);

//! Sets the color of text that will be drawn
//! @param text_layer The TextLayer of which to set the text color
//! @param color The new \ref GColor to set the text color to
//! @see text_layer_set_background_color
void text_layer_set_text_color(TextLayer *text_layer, GColor color);

//! Sets the line break mode of the TextLayer
//! @param text_layer The TextLayer of which to set the overflow mode
//! @param line_mode The new \ref GTextOverflowMode to set
void text_layer_set_overflow_mode(TextLayer *text_layer, GTextOverflowMode line_mode);

//! Sets the font of the TextLayer
//! @param text_layer The TextLayer of which to set the font
//! @param font The new \ref GFont for the TextLayer
//! @see fonts_get_system_font
//! @see fonts_load_custom_font
void text_layer_set_font(TextLayer *text_layer, GFont font);

//! Sets the alignment of the TextLayer
//! @param text_layer The TextLayer of which to set the alignment
//! @param text_alignment The new text alignment for the TextLayer
//! @see GTextAlignment
void text_layer_set_text_alignment(TextLayer *text_layer, GTextAlignment text_alignment);

//! @internal
//! Sets whether or not the text layer should cache text layout information.
//! By default, layout caching is off (false). Layout caches store the max used
//! height and width of a text layer.
//! NOTE: when using cached layouts, text_layer_deinit() MUST be called at some
//! point in time to prevent memory leaks from occurring.
void text_layer_set_should_cache_layout(TextLayer *text_layer, bool should_cache_layout);

//! @internal
//! Calculates the size occupied by the current text of the TextLayer
//! @param ctx the current graphics context
//! @param text_layer the TextLayer for which to calculate the text's size
//! @return The size occupied by the current text of the TextLayer. If the text
//! string is not valid UTF-8 or NULL, the size returned will be (0,0).
//! @note Because of an implementation detail, it is necessary to pass in the current graphics
//! context, even though this function does not draw anything.
//! @internal
//! @see \ref app_get_current_graphics_context()
GSize text_layer_get_content_size(GContext* ctx, TextLayer *text_layer);

//! Calculates the size occupied by the current text of the TextLayer
//! @param text_layer the TextLayer for which to calculate the text's size
//! @return The size occupied by the current text of the TextLayer
GSize app_text_layer_get_content_size(TextLayer *text_layer);

//! Update the size of the text layer
//! This is a convenience function to update the frame of the TextLayer.
//! @param text_layer The TextLayer of which to set the size
//! @param max_size The new size for the TextLayer
void text_layer_set_size(TextLayer *text_layer, const GSize max_size);

//! @internal
GSize text_layer_get_size(TextLayer* text_layer);

//! Set the vertical line spacing delta for the TextLayer
//! @param text_layer The TextLayer of which to set the line spacing delta
//! @param delta The vertical line spacing delta in pixels
void text_layer_set_line_spacing_delta(TextLayer *text_layer, int16_t delta);

//! Get the vertical line spacing delta for the TextLayer
//! @param text_layer The TextLayer of which to get the line spacing delta
//! @return The vertical line spacing delta in pixels
int16_t text_layer_get_line_spacing_delta(TextLayer *text_layer);

//! Enables text flow following the boundaries of the screen and pagination that introduces
//! extra line spacing at page breaks to avoid partially clipped lines for the TextLayer.
//! If the TextLayer is part of a \ref ScrollLayer the ScrollLayer's frame will be used to
//! configure paging.
//! @note Make sure the TextLayer is part of the view hierarchy before calling this function.
//!   Otherwise it has no effect.
//! @param text_layer The TextLayer for which to enable text flow and paging
//! @param inset Additional amount of pixels to inset to the inside of the screen for text flow
//! @see text_layer_restore_default_text_flow_and_paging
//! @see graphics_text_attributes_enable_screen_text_flow
//! @see graphics_text_attributes_enable_paging
void text_layer_enable_screen_text_flow_and_paging(TextLayer *text_layer, uint8_t inset);

//! Restores text flow and paging for the TextLayer to the rectangular defaults.
//! @param text_layer The TextLayer for which to restore text flow and paging
//! @see text_layer_enable_screen_text_flow_and_paging
//! @see graphics_text_attributes_restore_default_text_flow
//! @see graphics_text_attributes_restore_default_paging
void text_layer_restore_default_text_flow_and_paging(TextLayer *text_layer);

//!     @} // end addtogroup TextLayer
//!   @} // end addtogroup Layer
//! @} // end addtogroup UI

//! @internal
void text_layer_init_with_parameters(TextLayer *text_layer, const GRect *frame, const char *text,
                                     GFont font, GColor text_color, GColor back_color,
                                     GTextAlignment text_align, GTextOverflowMode overflow_mode);
