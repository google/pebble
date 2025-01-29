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

#include "applib/graphics/gtypes.h"
#include "applib/graphics/perimeter.h"
#include "applib/fonts/fonts.h"

#include <stdint.h>
#include <stdbool.h>

//! @addtogroup Graphics
//! @{
//!   @addtogroup TextDrawing Drawing Text
//!   \brief Functions to draw text into a graphics context
//!
//! See \ref GraphicsContext for more information about the graphics context.
//!
//! Other drawing functions and related documentation:
//! * \ref Drawing
//! * \ref PathDrawing
//! * \ref GraphicsTypes
//!   @{

//! Text overflow mode controls the way text overflows when the string that is drawn does not fit
//! inside the area constraint.
//! @see graphics_draw_text
//! @see text_layer_set_overflow_mode
typedef enum {
  //! On overflow, wrap words to a new line below the current one. Once vertical space is consumed,
  //! the last line may be clipped.
  GTextOverflowModeWordWrap,
  //! On overflow, wrap words to a new line below the current one.
  //! Once vertical space is consumed, truncate as needed to fit a trailing ellipsis (...).
  //! Clipping may occur if the vertical space cannot accomodate the first line of text.
  GTextOverflowModeTrailingEllipsis,
  //! Acts like \ref GTextOverflowModeTrailingEllipsis, plus trims leading and trailing newlines,
  //! while treating all other newlines as spaces.
  GTextOverflowModeFill
} GTextOverflowMode;

//! Text alignment controls the way the text is aligned inside the box the text is drawn into.
//! @see graphics_draw_text
//! @see text_layer_set_text_alignment
typedef enum {
  //! Aligns the text to the left of the drawing box
  GTextAlignmentLeft,
  //! Aligns the text centered inside the drawing box
  GTextAlignmentCenter,
  //! Aligns the text to the right of the drawing box
  GTextAlignmentRight,
} GTextAlignment;

//! @internal
typedef enum {
  GVerticalAlignmentTop,
  GVerticalAlignmentCenter,
  GVerticalAlignmentBottom,
} GVerticalAlignment;

typedef struct {
  //! Invalidate the cache if these parameters have changed
  uint32_t hash;
  GRect box;
  GFont font;
  GTextOverflowMode overflow_mode;
  GTextAlignment alignment;
  //! Cached parameters
  GSize max_used_size; //<! Max area occupied by text in px
} TextLayout;

//! @internal
typedef struct {
  const GPerimeter *impl;
  uint8_t inset;
} TextLayoutFlowDataPerimeter;

//! @internal
typedef struct {
  GPoint origin_on_screen;
  GRangeVertical page_on_screen;
} TextLayoutFlowDataPaging;

//! @internal
typedef struct {
  TextLayoutFlowDataPerimeter perimeter;
  TextLayoutFlowDataPaging paging;
} TextLayoutFlowData;

//! @internal
//! Not supported in 2.X. This new structure is required to avoid breaking existing memory
//! contract with 2.X compiled apps and maintain compatibility.
typedef struct {
  //! Invalidate the cache if these parameters have changed
  uint32_t hash;
  GRect box;
  GFont font;
  GTextOverflowMode overflow_mode;
  GTextAlignment alignment;
  //! Cached parameters
  GSize max_used_size; //<! Max area occupied by text in px

  //! Vertical padding in px to add to the font line height when rendering
  int16_t line_spacing_delta;

  //! TODO: PBL-22653 recover TextLayoutExtended padding by reducing the below types
  //! Layout restriction callback shrinking text box to fit within perimeter
  TextLayoutFlowData flow_data;
} TextLayoutExtended;

//! Pointer to opaque text layout cache data structure
typedef TextLayout* GTextLayoutCacheRef;

//! Describes various characteristics for text rendering and measurement.
//! @see graphics_draw_text
//! @see graphics_text_attributes_create
//! @see graphics_text_attributes_enable_screen_text_flow
//! @see graphics_text_attributes_enable_paging
typedef TextLayout GTextAttributes;

//! @internal
//! Synonym for graphic_fonts_init()
void graphics_text_init(void);

//! Draw text into the current graphics context, using the context's current text color.
//! The text will be drawn inside a box with the specified dimensions and
//! configuration, with clipping occurring automatically.
//! @param ctx The destination graphics context in which to draw
//! @param text The zero terminated UTF-8 string to draw
//! @param font The font in which the text should be set
//! @param box The bounding box in which to draw the text. The first line of text will be drawn
//! against the top of the box.
//! @param overflow_mode The overflow behavior, in case the text is larger than what fits inside
//! the box.
//! @param alignment The horizontal alignment of the text
//! @param text_attributes Optional text attributes to describe the characteristics of the text
void graphics_draw_text(GContext *ctx, const char *text, GFont const font, const GRect box,
                        const GTextOverflowMode overflow_mode, const GTextAlignment alignment,
                        GTextAttributes *text_attributes);


//! Obtain the maximum size that a text with given font, overflow mode and alignment
//! occupies within a given rectangular constraint.
//! @param ctx the current graphics context
//! @param text The zero terminated UTF-8 string for which to calculate the size
//! @param font The font in which the text should be set while calculating the size
//! @param box The bounding box in which the text should be constrained
//! @param overflow_mode The overflow behavior, in case the text is larger than what fits
//! inside the box.
//! @param alignment The horizontal alignment of the text
//! @param layout Optional layout cache data. Supply `NULL` to ignore the layout caching mechanism.
//! @return The maximum size occupied by the text
//! @note Because of an implementation detail, it is necessary to pass in the current graphics
//! context,
//! even though this function does not draw anything.
//! @internal
//! @see \ref app_get_current_graphics_context()
GSize graphics_text_layout_get_max_used_size(GContext *ctx, const char *text,
                                             GFont const font, const GRect box,
                                             const GTextOverflowMode overflow_mode,
                                             const GTextAlignment alignment,
                                             GTextLayoutCacheRef layout);

//! Obtain the maximum size that a text with given font, overflow mode and alignment occupies
//! within a given rectangular constraint.
//! @param text The zero terminated UTF-8 string for which to calculate the size
//! @param font The font in which the text should be set while calculating the size
//! @param box The bounding box in which the text should be constrained
//! @param overflow_mode The overflow behavior, in case the text is larger than what fits
//! inside the box.
//! @param alignment The horizontal alignment of the text
//! @return The maximum size occupied by the text
//! @see app_graphics_text_layout_get_content_size_with_attributes
GSize app_graphics_text_layout_get_content_size(const char *text, GFont const font, const GRect box,
                                                const GTextOverflowMode overflow_mode,
                                                const GTextAlignment alignment);

//! Obtain the maximum size that a text with given font, overflow mode and alignment occupies
//! within a given rectangular constraint.
//! @param text The zero terminated UTF-8 string for which to calculate the size
//! @param font The font in which the text should be set while calculating the size
//! @param box The bounding box in which the text should be constrained
//! @param overflow_mode The overflow behavior, in case the text is larger than what fits
//! inside the box.
//! @param alignment The horizontal alignment of the text
//! @param text_attributes Optional text attributes to describe the characteristics of the text
//! @return The maximum size occupied by the text
//! @see app_graphics_text_layout_get_content_size
GSize app_graphics_text_layout_get_content_size_with_attributes(
  const char *text, GFont const font, const GRect box, const GTextOverflowMode overflow_mode,
  const GTextAlignment alignment, GTextAttributes *text_attributes);



//! @internal
//! Does the same as \ref app_graphics_text_layout_get_text_height with the provided GContext
uint16_t graphics_text_layout_get_text_height(GContext *ctx, const char *text, GFont const font,
                                              uint16_t bounds_width,
                                              const GTextOverflowMode overflow_mode,
                                              const GTextAlignment alignment);

//! @internal
//! Malloc a text layout cache
void graphics_text_layout_cache_init(GTextLayoutCacheRef *layout_cache);

//! @internal
//! Free a text layout cache
void graphics_text_layout_cache_deinit(GTextLayoutCacheRef *layout_cache);

//! Creates an instance of GTextAttributes for advanced control when rendering text.
//! @return New instance of GTextAttributes
//! @see \ref graphics_draw_text
GTextAttributes *graphics_text_attributes_create(void);

//! Destroys a previously created instance of GTextAttributes
void graphics_text_attributes_destroy(GTextAttributes *text_attributes);

//! Sets the current line spacing delta for the given layout.
//! @param layout Text layout
//! @param delta The vertical line spacing delta in pixels to set for the given layout
void graphics_text_layout_set_line_spacing_delta(GTextLayoutCacheRef layout, int16_t delta);

//! Returns the current line spacing delta for the given layout.
//! @param layout Text layout
//! @return The vertical line spacing delta for the given layout
int16_t graphics_text_layout_get_line_spacing_delta(const GTextLayoutCacheRef layout);

//! Restores text flow to the rectangular default.
//! @param text_attributes The attributes for which to disable text flow
//! @see graphics_text_attributes_enable_screen_text_flow
//! @see text_layer_restore_default_text_flow_and_paging
void graphics_text_attributes_restore_default_text_flow(GTextAttributes *text_attributes);

//! Enables text flow that follows the boundaries of the screen.
//! @param text_attributes The attributes for which text flow should be enabled
//! @param inset Additional amount of pixels to inset to the inside of the screen for text flow
//! calculation. Can be zero.
//! @see graphics_text_attributes_restore_default_text_flow
//! @see text_layer_enable_screen_text_flow_and_paging
void graphics_text_attributes_enable_screen_text_flow(GTextAttributes *text_attributes,
                                                      uint8_t inset);

//! Restores paging and locked content origin to the defaults.
//! @param text_attributes The attributes for which to restore paging and locked content origin
//! @see graphics_text_attributes_enable_paging
//! @see text_layer_restore_default_text_flow_and_paging
void graphics_text_attributes_restore_default_paging(GTextAttributes *text_attributes);

//! Enables paging and locks the text flow calculation to a fixed point on the screen.
//! @param text_attributes Attributes for which to enable paging and locked content origin
//! @param content_origin_on_screen Absolute coordinate on the screen where the text content
//!     starts before an animation or scrolling takes place. Usually the frame's origin of a layer
//!     in screen coordinates.
//! @param paging_on_screen Rectangle in absolute coordinates on the screen that describes where
//!     text content pages. Usually the container's absolute frame in screen coordinates.
//! @see graphics_text_attributes_restore_default_paging
//! @see graphics_text_attributes_enable_screen_text_flow
//! @see text_layer_enable_screen_text_flow_and_paging
//! @see layer_convert_point_to_screen
void graphics_text_attributes_enable_paging(GTextAttributes *text_attributes,
                                            GPoint content_origin_on_screen,
                                            GRect paging_on_screen);

//! @internal
const TextLayoutFlowData *graphics_text_layout_get_flow_data(GTextLayoutCacheRef layout);

//! @internal
void graphics_text_perimeter_debugging_enable(bool enable);

//!   @} // end addtogroup TextDrawing
//! @} // end addtogroup Graphics
