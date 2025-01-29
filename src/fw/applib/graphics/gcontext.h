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

#include "gtypes.h"
#include "text_layout_private.h"
#include "text_resources.h"

#define GDRAWMASK_BITS_PER_PIXEL PBL_IF_COLOR_ELSE(2, 1)
#define GDRAWMASK_PIXELS_PER_BYTE (8 / GDRAWMASK_BITS_PER_PIXEL)

//! @internal
typedef struct FrameBuffer FrameBuffer;

//! @internal
typedef struct GContext {
  GBitmap dest_bitmap;

  //! Which framebuffer dest_bitmap points into. This may be null if the
  //! bitmap doesn't point into a framebuffer.
  FrameBuffer* parent_framebuffer;

  //! Number of rows between the top of the dest_bitmap and the top of it's
  //! parent framebuffer. This value is invalid if parent_framebuffer is null.
  uint8_t parent_framebuffer_vertical_offset;

  // Keep state here for drawing commands:
  GDrawState draw_state;

  TextDrawState text_draw_state;

  FontCache font_cache;

  //! When the frame buffer accessed directly all graphics functions using this
  //! context are locked
  bool lock;
} GContext;

//! @internal
typedef enum {
  GContextInitializationMode_App,
  GContextInitializationMode_System,
} GContextInitializationMode;

//! @internal
typedef enum {
  //! Pixels within the range are considered to be fully opaque
  GDrawMaskRowInfoType_Opaque,
  //! The opacity of the pixels within the range varies and needs individual checks
  GDrawMaskRowInfoType_SemiTransparent,
} GDrawMaskRowInfoType;

//! @internal
//! Describes mask values for a given scan line.
//! The sole purpose of this data structure is performance optimization so that callers don't need
//! to test every single pixel on a GDrawMask's pixel_mask_data.
typedef struct {
  //! Describes how to treat the range between .min_x and .max_x
  GDrawMaskRowInfoType type;
  //! Left-most pixel, 3.0 means that that pixel 3 is fully visible, 3.5 means it's half visible
  Fixed_S16_3 min_x;
  //! Right-most pixel, 10.7 means that pixel 10 is fully opaque
  Fixed_S16_3 max_x;
} GDrawMaskRowInfo;

//! @internal
//! Describes how draw operations in GDrawRawImplementation should treat the final opacity
//! conceptually. Each pixel's alpha value should be multiplied with the corresponding
//! .pixel_mask_data of this struct.
typedef struct GDrawMask {
  //! Describes the mask values for each of the scan lines
  GDrawMaskRowInfo *mask_row_infos;
  //! Pixel mask that follows the structure and size of the actual framebuffer
  void *pixel_mask_data;
  //! A contiguous block of data that contains .row_infos and .pixel_mask_data
  uint8_t data[];
} GDrawMask;

//! @addtogroup Graphics
//! @{

//!   @addtogroup GraphicsContext Graphics Context
//! \brief The "canvas" into which an application draws
//!
//! The Pebble OS graphics engine, inspired by several notable graphics systems, including
//! Apple’s Quartz 2D and its predecessor QuickDraw, provides your app with a canvas into
//! which to draw, namely, the graphics context. A graphics context is the target into which
//! graphics functions can paint, using Pebble drawing routines (see \ref Drawing,
//! \ref PathDrawing and \ref TextDrawing).
//!
//! A graphics context holds a reference to the bitmap into which to paint. It also holds the
//! current drawing state, like the current fill color, stroke color, clipping box, drawing box,
//! compositing mode, and so on. The GContext struct is the type representing the graphics context.
//!
//! For drawing in your Pebble watchface or watchapp, you won't need to create a GContext
//! yourself. In most cases, it is provided by Pebble OS as an argument passed into a render
//! callback (the .update_proc of a Layer).
//!
//! Your app can’t call drawing functions at any given point in time: Pebble OS will request your
//! app to render. Typically, your app will be calling out to graphics functions in
//! the .update_proc callback of a Layer.
//! @see \ref Layer
//! @see \ref Drawing
//! @see \ref PathDrawing
//! @see \ref TextDrawing
//!   @{


//! @internal
void graphics_context_init(GContext *ctx, FrameBuffer *framebuffer,
                           GContextInitializationMode init_mode);

//! @internal
void graphics_context_set_default_drawing_state(GContext *ctx,
                                                GContextInitializationMode init_mode);

//! @internal
//! Gets the current drawing state (fill/stroke/text colors, compositing mode, ...)
GDrawState graphics_context_get_drawing_state(GContext* ctx);

//! @internal
//! Sets the current drawing state (fill/stroke/text colors, compositing mode, ...)
void graphics_context_set_drawing_state(GContext* ctx, GDrawState draw_state);

//! @internal
//! Move the drawing box origin by the translation offset specified
void graphics_context_move_draw_box(GContext* ctx, GPoint offset);

//! Sets the current stroke color of the graphics context.
//! @param ctx The graphics context onto which to set the stroke color
//! @param color The new stroke color
void graphics_context_set_stroke_color(GContext* ctx, GColor color);
void graphics_context_set_stroke_color_2bit(GContext* ctx, GColor2 color);

//! Sets the current fill color of the graphics context.
//! @param ctx The graphics context onto which to set the fill color
//! @param color The new fill color
void graphics_context_set_fill_color(GContext* ctx, GColor color);
void graphics_context_set_fill_color_2bit(GContext* ctx, GColor2 color);

//! Sets the current text color of the graphics context.
//! @param ctx The graphics context onto which to set the text color
//! @param color The new text color
void graphics_context_set_text_color(GContext* ctx, GColor color);
void graphics_context_set_text_color_2bit(GContext* ctx, GColor2 color);

//! Sets the tint color of the graphics context.  This is used when drawing under
//! the GCompOpOr compositing mode.
//! @param ctx The graphics context onto which to set the tint color
//! @param color The new tint color
void graphics_context_set_tint_color(GContext *ctx, GColor color);

//! Sets the current bitmap compositing mode of the graphics context.
//! @param ctx The graphics context onto which to set the compositing mode
//! @param mode The new compositing mode
//! @see \ref GCompOp
//! @see \ref bitmap_layer_set_compositing_mode()
//! @note At the moment, this only affects the bitmaps drawing operations
//! -- \ref graphics_draw_bitmap_in_rect(), \ref graphics_draw_rotated_bitmap, and
//! anything that uses those APIs --, but it currently does not affect the filling or stroking
//! operations.
void graphics_context_set_compositing_mode(GContext* ctx, GCompOp mode);

//! Sets whether antialiasing is applied to stroke drawing
//! @param ctx The graphics context onto which to set the antialiasing
//! @param enable True = antialiasing enabled, False = antialiasing disabled
//! @note Default value is true.
void graphics_context_set_antialiased(GContext* ctx, bool enable);

//! @internal
//! Gets whether antialiasing is applied to stroke drawing
//! @param ctx The graphics context for which to get the current state of antialiasing
//! @return True if antialiasing is enabled, false otherwise
bool graphics_context_get_antialiased(GContext *ctx);

//! Sets the width of the stroke for drawing routines
//! @param ctx The graphics context onto which to set the stroke width
//! @param stroke_width Width in pixels of the stroke.
//! @note If stroke width of zero is passed, it will be ignored and will not change the value
//! stored in GContext. Currently, only odd stroke_width values are supported. If an even value
//! is passed in, the value will be stored as is, but the drawing routines will round down to the
//! previous integral value when drawing. Default value is 1.
void graphics_context_set_stroke_width(GContext* ctx, uint8_t stroke_width);

//! Instantiates and initializes a mask.
//! @param ctx The graphics context to use to initialize the new mask
//! @param transparent Whether the initial mask pixel values should all be transparent or opaque
//! @return The new clipping mask, or NULL on failure
GDrawMask *graphics_context_mask_create(const GContext *ctx, bool transparent);

//! Attaches a mask to the provided GContext for recording. Subsequent drawing operations will
//! change the mask values. The luminance of the drawing operations corresponds with the resulting
//! opacity in the mask, so the brighter a drawn pixel is, the more opaque its corresponding mask
//! value will be.
//! @param ctx The GContext to attach the mask to for recording
//! @param mask The mask to use for recording
//! @return True if the mask was successfully attached to the GContext for recording, false
//! otherwise
bool graphics_context_mask_record(GContext *ctx, GDrawMask *mask);

//! Attaches a mask to the provided GContext and activates it for subsequent drawing operations.
//! Upon activation, subsequent drawing operations will be multiplied with the given mask.
//! @param ctx The GContext to attach the mask to for use
//! @param mask The mask to use
//! @return True if the mask was successfully attached to the GContext for use, false otherwise
bool graphics_context_mask_use(GContext *ctx, GDrawMask *mask);

//! Destroys a previously created mask.
//! @param ctx The GContext the mask was used with
//! @param mask The mask to destroy
void graphics_context_mask_destroy(GContext *ctx, GDrawMask *mask);

//! @internal
//! Gets the size of the backing framebuffer for the graphics context or GSize(DISP_COLS, DISP_ROWS)
//! if there is no backing framebuffer.
GSize graphics_context_get_framebuffer_size(GContext *ctx);

//! @internal
//! Retrieves the destination bitmap for the graphics context.
//! @param ctx The graphics context to retrieve the bitmap for.
GBitmap* graphics_context_get_bitmap(GContext* ctx);

//! @internal
//! Updates the parent framebuffers dirty state based on a change to the
//! graphic context's bitmap.
void graphics_context_mark_dirty_rect(GContext* ctx, GRect rect);

//!   @} // end addtogroup GraphicsContext
//! @} // end addtogroup Graphics
