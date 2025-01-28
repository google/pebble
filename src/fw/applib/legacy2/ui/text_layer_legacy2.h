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

//! @file text_layer_legacy2.h
//! @addtogroup UI
//! @{
//!   @addtogroup Layer Layers
//!   @{
//!     @addtogroup TextLayerLegacy2
//! \brief Layer that displays and formats a text string.
//!
//! ![](text_layer.png)
//! The geometric information (bounds, frame) of the Layer
//! is used as the "box" in which the text is drawn. The \ref TextLayerLegacy2 also has a number of
//! other properties that influence how the text is drawn. Most important of these properties are:
//! a pointer to the string to draw itself, the font, the text color, the background color of the
//! layer, the overflow mode and alignment of the text inside the layer.
//! @see Layer
//! @see TextDrawing
//! @see Fonts
//!     @{

//! The data structure of a TextLayerLegacy2.
//! @note a `TextLayerLegacy2 *` can safely be casted to a `Layer *` and can thus be used
//! with all other functions that take a `Layer *` as an argument.
//! <br/>For example, the following is legal:
//! \code{.c}
//! TextLayerLegacy2 text_layer;
//! ...
//! layer_set_hidden((Layer *)&text_layer, true);
//! \endcode
typedef struct TextLayerLegacy2 {
  Layer layer;
  const char* text;
  GFont font;
  GTextLayoutCacheRef layout_cache;
  GColor2 text_color:2;
  GColor2 background_color:2;
  GTextOverflowMode overflow_mode:2;
  GTextAlignment text_alignment:2;
  bool should_cache_layout:1;
} TextLayerLegacy2;

//! Initializes the TextLayerLegacy2 with given frame
//! All previous contents are erased and the following default values are set:
//! * Font: Raster Gothic 14-point Boldface (system font)
//! * Text Alignment: \ref GTextAlignmentLeft
//! * Text color: \ref GColorBlack
//! * Background color: \ref GColorWhite
//! * Clips: `true`
//! * Hidden: `false`
//! * Caching: `false`
//!
//! The text layer is automatically marked dirty after this operation.
//! @param text_layer The TextLayerLegacy2 to initialize
//! @param frame The frame with which to initialize the TextLayerLegacy2
void text_layer_legacy2_init(TextLayerLegacy2 *text_layer, const GRect *frame);

//! Creates a new TextLayerLegacy2 on the heap and initializes it with the default values.
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
//! @param frame The frame with which to initialize the TextLayerLegacy2
//! @return A pointer to the TextLayerLegacy2. `NULL` if the TextLayerLegacy2 could not
//! be created
TextLayerLegacy2* text_layer_legacy2_create(GRect frame);

//! Destroys a TextLayerLegacy2 previously created by text_layer_legacy2_create.
void text_layer_legacy2_destroy(TextLayerLegacy2* text_layer);

//! Deinitializes the TextLayerLegacy2 and frees any caches.
//! @param text_layer The TextLayerLegacy2 to deinitialize
//! @internal
//! @see text_layer_legacy2_set_should_cache_layout
//! @note This MUST be called after discarding the text layer when using layout caching.
void text_layer_legacy2_deinit(TextLayerLegacy2 *text_layer);

//! Gets the "root" Layer of the text layer, which is the parent for the sub-
//! layers used for its implementation.
//! @param text_layer Pointer to the TextLayerLegacy2 for which to get the "root" Layer
//! @return The "root" Layer of the text layer.
//! @internal
//! @note The result is always equal to `(Layer *) text_layer`.
Layer* text_layer_legacy2_get_layer(TextLayerLegacy2 *text_layer);

//! Sets the pointer to the string where the TextLayerLegacy2 is supposed to find the text
//! at a later point in time, when it needs to draw itself.
//! @param text_layer The TextLayerLegacy2 of which to set the text
//! @param text The new text to set onto the TextLayerLegacy2. This must be a null-terminated and
//! valid UTF-8 string!
//! @note The string is not copied, so its buffer most likely cannot be stack allocated,
//! but is recommended to be a buffer that is long-lived, at least as long as the TextLayerLegacy2
//! is part of a visible Layer hierarchy.
//! @see text_layer_legacy2_get_text
void text_layer_legacy2_set_text(TextLayerLegacy2 *text_layer, const char *text);

//! Gets the pointer to the string that the TextLayerLegacy2 is using.
//! @param text_layer The TextLayerLegacy2 for which to get the text
//! @see text_layer_legacy2_set_text
const char* text_layer_legacy2_get_text(TextLayerLegacy2 *text_layer);

//! Sets the background color of bounding box that will be drawn behind the text
//! @param text_layer The TextLayerLegacy2 of which to set the background color
//! @param color The new \ref GColor to set the background to
//! @see text_layer_legacy2_set_text_color
void text_layer_legacy2_set_background_color_2bit(TextLayerLegacy2 *text_layer, GColor2 color);

//! Sets the color of text that will be drawn
//! @param text_layer The TextLayerLegacy2 of which to set the text color
//! @param color The new \ref GColor to set the text color to
//! @see text_layer_legacy2_set_background_color
void text_layer_legacy2_set_text_color_2bit(TextLayerLegacy2 *text_layer, GColor2 color);

//! Sets the line break mode of the TextLayerLegacy2
//! @param text_layer The TextLayerLegacy2 of which to set the overflow mode
//! @param line_mode The new \ref GTextOverflowMode to set
void text_layer_legacy2_set_overflow_mode(TextLayerLegacy2 *text_layer,
                                          GTextOverflowMode line_mode);

//! Sets the font of the TextLayerLegacy2
//! @param text_layer The TextLayerLegacy2 of which to set the font
//! @param font The new \ref GFont for the TextLayerLegacy2
//! @see fonts_get_system_font
//! @see fonts_load_custom_font
void text_layer_legacy2_set_font(TextLayerLegacy2 *text_layer, GFont font);

//! Sets the alignment of the TextLayerLegacy2
//! @param text_layer The TextLayerLegacy2 of which to set the alignment
//! @param text_alignment The new text alignment for the TextLayerLegacy2
//! @see GTextAlignment
void text_layer_legacy2_set_text_alignment(TextLayerLegacy2 *text_layer,
                                           GTextAlignment text_alignment);

//! @internal
//! Sets whether or not the text layer should cache text layout information.
//! By default, layout caching is off (false). Layout caches store the max used
//! height and width of a text layer.
//! NOTE: when using cached layouts, text_layer_legacy2_deinit() MUST be called at some
//! point in time to prevent memory leaks from occuring.
void text_layer_legacy2_set_should_cache_layout(TextLayerLegacy2 *text_layer,
                                                bool should_cache_layout);

//! @internal
//! Calculates the size occupied by the current text of the TextLayerLegacy2
//! @param ctx the current graphics context
//! @param text_layer the TextLayerLegacy2 for which to calculate the text's size
//! @return The size occupied by the current text of the TextLayerLegacy2. If the text
//! string is not valid UTF-8 or NULL, the size returned will be (0,0).
//! @note Because of an implementation detail, it is necessary to pass in the current graphics
//! context, even though this function does not draw anything.
//! @internal
//! @see \ref app_get_current_graphics_context()
GSize text_layer_legacy2_get_content_size(GContext* ctx, TextLayerLegacy2 *text_layer);

//! Calculates the size occupied by the current text of the TextLayerLegacy2
//! @param text_layer the TextLayerLegacy2 for which to calculate the text's size
//! @return The size occupied by the current text of the TextLayerLegacy2
GSize app_text_layer_legacy2_get_content_size(TextLayerLegacy2 *text_layer);

//! Update the size of the text layer
//! This is a convenience function to update the frame of the TextLayerLegacy2.
//! @param text_layer The TextLayerLegacy2 of which to set the size
//! @param max_size The new size for the TextLayerLegacy2
void text_layer_legacy2_set_size(TextLayerLegacy2 *text_layer, const GSize max_size);

GSize text_layer_legacy2_get_size(TextLayerLegacy2* text_layer);

//!     @} // end addtogroup TextLayerLegacy2
//!   @} // end addtogroup Layer
//! @} // end addtogroup UI
