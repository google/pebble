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

#include "gdraw_command_list.h"
#include "applib/graphics/graphics.h"

#include <stdint.h>
#include <stdbool.h>

//! @file graphics/gdraw_command_image.h
//! Defines the functions to manipulate \ref GDrawCommandImage objects
//! @addtogroup Graphics
//! @{
//!   @addtogroup DrawCommand Draw Commands
//!   @{


struct GDrawCommandImage;

//! Draw command images contain a list of commands that can be drawn. An image can be loaded from
//! PDC file data.
typedef struct GDrawCommandImage GDrawCommandImage;

//! Creates a GDrawCommandImage from the specified resource (PDC file)
//! @param resource_id Resource containing data to load and create GDrawCommandImage from.
//! @return GDrawCommandImage pointer if the resource was loaded, NULL otherwise
GDrawCommandImage *gdraw_command_image_create_with_resource(uint32_t resource_id);

//! @internal
GDrawCommandImage *gdraw_command_image_create_with_resource_system(ResAppNum app_num,
                                                                   uint32_t resource_id);

//! @internal
//! Copies a GDrawCommandImage into a memory buffer. The buffer length must be equal to or larger
//! than the source image.
//! @param buffer A buffer that will become a copy of the source image
//! @param buffer_length Size of the buffer in bytes
//! @param image GDrawCommandImage that will be copied from
//! @return true if the image was copied over
bool gdraw_command_image_copy(void *buffer, size_t buffer_length, GDrawCommandImage *image);

//! Creates a GDrawCommandImage as a copy from a given image
//! @param image Image to copy.
//! @return cloned image or NULL if the operation failed
GDrawCommandImage *gdraw_command_image_clone(GDrawCommandImage *image);

//! Deletes the GDrawCommandImage structure and frees associated data
//! @param image Pointer to the image to free (delete)
void gdraw_command_image_destroy(GDrawCommandImage *image);

//! @internal
//! Use to validate an image read from flash or copied from serialized data
//! @param size Size of the frame structure in memory, in bytes
bool gdraw_command_image_validate(GDrawCommandImage *image, size_t size);

//! Draw an image
//! @param ctx The destination graphics context in which to draw
//! @param image Image to draw
//! @param offset Offset from draw context origin to draw the image
void gdraw_command_image_draw(GContext *ctx, GDrawCommandImage *image, GPoint offset);

//! Draw an image after being processed by the passed in processor
//! @param ctx The destination graphics context in which to draw
//! @param image Image to draw
//! @param offset Offset from draw context origin to draw the image
//! @param processors Contains function pointers to draw modified commands in the image
void gdraw_command_image_draw_processed(GContext *ctx, GDrawCommandImage *image, GPoint offset,
                                        GDrawCommandProcessor *processor);

//! @internal
//! Get the size, in bytes, of the image in memory
size_t gdraw_command_image_get_data_size(GDrawCommandImage *image);

//! Get size of the bounding box surrounding all draw commands in the image. This bounding
//! box can be used to set the graphics context or layer bounds when drawing the image.
//! @param image \ref GDrawCommandImage from which to get the bounding box size
//! @return bounding box size
GSize gdraw_command_image_get_bounds_size(GDrawCommandImage *image);

//! Set size of the bounding box surrounding all draw commands in the image. This bounding
//! box can be used to set the graphics context or layer bounds when drawing the image.
//! @param image \ref GDrawCommandImage for which to set the bounding box size
//! @param size bounding box size
void gdraw_command_image_set_bounds_size(GDrawCommandImage *image, GSize size);

//! Get the command list of the image
//! @param image \ref GDrawCommandImage from which to get the command list
//! @return command list
GDrawCommandList *gdraw_command_image_get_command_list(GDrawCommandImage *image);

//!   @} // end addtogroup DrawCommand
//! @} // end addtogroup Graphics
