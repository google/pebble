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

//! @file ui/bitmap_layer.h
//!

#pragma once
#include "applib/ui/layer.h"
#include "applib/graphics/gtypes.h"

//! @file bitmap_layer.h
//! @addtogroup UI
//! @{
//!   @addtogroup Layer Layers
//!   @{
//!     @addtogroup BitmapLayer
//! \brief Layer that displays a bitmap image.
//!
//! ![](bitmap_layer.png)
//! BitmapLayer is a Layer subtype that draws a GBitmap within its frame. It uses an alignment property
//! to specify how to position the bitmap image within its frame. Optionally, when the
//! background color is not GColorClear, it draws a solid background color behind the
//! bitmap image, filling areas of the frame that are not covered by the bitmap image.
//! Lastly, using the compositing mode property of the BitmapLayer, determines the way the
//! bitmap image is drawn on top of what is underneath it (either the background color, or
//! the layers beneath it).
//!
//! <h3>Inside the Implementation</h3>
//! The implementation of BitmapLayer is fairly straightforward and relies heavily on the
//! functionality as exposed by the core drawing functions (see \ref Drawing).
//! \ref BitmapLayer's drawing callback uses \ref graphics_draw_bitmap_in_rect()
//! to perform the actual drawing of the \ref GBitmap. It uses \ref grect_align() to perform
//! the layout of the image and it uses \ref graphics_fill_rect() to draw the background plane.
//!     @{

//! The data structure of a BitmapLayer, containing a Layer data structure, a pointer to
//! the GBitmap, and all necessary state to draw itself (the background color, alignment and
//! the compositing mode).
//! @note a `BitmapLayer *` can safely be casted to a `Layer *` and can thus be used
//! with all other functions that take a `Layer *` as an argument.
//! <br/>For example, the following is legal:
//! \code{.c}
//! BitmapLayer bitmap_layer;
//! ...
//! layer_set_hidden((Layer *)&bitmap_layer, true);
//! \endcode

typedef struct BitmapLayer {
  Layer layer;
  const GBitmap *bitmap;
  GColor8 background_color;
  GAlign alignment:4;
  GCompOp compositing_mode:3;
} BitmapLayer;

//! Initializes the BitmapLayer
//! All previous contents are erased and the following default values are set:
//! * Bitmap: `NULL` (none)
//! * Background color: \ref GColorClear
//! * Compositing mode: \ref GCompOpAssign
//! * Clips: `true`
//!
//! The bitmap layer is automatically marked dirty after this operation.
//! @param bitmap_layer The BitmapLayer to initialize
//! @param frame The frame with which to initialize the BitmapLayer
void bitmap_layer_init(BitmapLayer *bitmap_layer, const GRect *frame);

//! Creates a new bitmap layer on the heap and initalizes it the default values.
//!
//! * Bitmap: `NULL` (none)
//! * Background color: \ref GColorClear
//! * Compositing mode: \ref GCompOpAssign
//! * Clips: `true`
//! @return A pointer to the BitmapLayer. `NULL` if the BitmapLayer could not
//! be created
BitmapLayer* bitmap_layer_create(GRect frame);

//! De-initializes the BitmapLayer
//! Removes the layer from the parent layer.
//! @param bitmap_layer The BitmapLayer to de-initialize
void bitmap_layer_deinit(BitmapLayer *bitmap_layer);

//! Destroys a window previously created by bitmap_layer_create
void bitmap_layer_destroy(BitmapLayer* bitmap_layer);

//! Gets the "root" Layer of the bitmap layer, which is the parent for the sub-
//! layers used for its implementation.
//! @param bitmap_layer Pointer to the BitmapLayer for which to get the "root" Layer
//! @return The "root" Layer of the bitmap layer.
//! @internal
//! @note The result is always equal to `(Layer *) bitmap_layer`.
Layer* bitmap_layer_get_layer(const BitmapLayer *bitmap_layer);

//! Gets the pointer to the bitmap image that the BitmapLayer is using.
//!
//! @param bitmap_layer The BitmapLayer for which to get the bitmap image
//! @return A pointer to the bitmap image that the BitmapLayer is using
const GBitmap* bitmap_layer_get_bitmap(BitmapLayer *bitmap_layer);

//! Sets the bitmap onto the BitmapLayer. The bitmap is set by reference (no deep
//! copy), thus the caller of this function has to make sure the bitmap is kept
//! in memory.
//!
//! The bitmap layer is automatically marked dirty after this operation.
//! @param bitmap_layer The BitmapLayer for which to set the bitmap image
//! @param bitmap The new \ref GBitmap to set onto the BitmapLayer
void bitmap_layer_set_bitmap(BitmapLayer *bitmap_layer, const GBitmap *bitmap);

//! Sets the alignment of the image to draw with in frame of the BitmapLayer.
//! The alignment parameter specifies which edges of the bitmap should overlap
//! with the frame of the BitmapLayer.
//! If the bitmap is smaller than the frame of the BitmapLayer, the background
//! is filled with the background color.
//!
//! The bitmap layer is automatically marked dirty after this operation.
//! @param bitmap_layer The BitmapLayer for which to set the alignment
//! @param alignment The new alignment for the image inside the BitmapLayer
void bitmap_layer_set_alignment(BitmapLayer *bitmap_layer, GAlign alignment);

//! Sets the background color of bounding box that will be drawn behind the image
//! of the BitmapLayer.
//!
//! The bitmap layer is automatically marked dirty after this operation.
//! @param bitmap_layer The BitmapLayer for which to set the background color
//! @param color The new \ref GColor to set the background to
void bitmap_layer_set_background_color(BitmapLayer *bitmap_layer, GColor color);
void bitmap_layer_set_background_color_2bit(BitmapLayer *bitmap_layer, GColor2 color);

//! Sets the compositing mode of how the bitmap image is composited onto the
//! BitmapLayer's background plane, or how it is composited onto what has been
//! drawn beneath the BitmapLayer.
//!
//! The compositing mode only affects the drawing of the bitmap and not the
//! drawing of the background color.
//!
//! For Aplite, there is no notion of "transparency" in the graphics system. However, the effect of
//! transparency can be created by masking and using compositing modes.
//!
//! For Basalt, when drawing \ref GBitmap images, \ref GCompOpSet will be required to apply any
//! transparency.
//!
//! The bitmap layer is automatically marked dirty after this operation.
//! @param bitmap_layer The BitmapLayer for which to set the compositing mode
//! @param mode The compositing mode to set
//! @see See \ref GCompOp for visual examples of the different compositing modes.
void bitmap_layer_set_compositing_mode(BitmapLayer *bitmap_layer, GCompOp mode);

//!     @} // end addtogroup BitmapLayer
//!   @} // end addtogroup Layer
//! @} // end addtogroup UI
