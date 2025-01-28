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

#include "gdraw_command.h"

#include "applib/graphics/gtypes.h"
#include "applib/graphics/graphics.h"

#include <stdint.h>
#include <stdbool.h>

//! @file graphics/gdraw_command_list.h
//! Defines the functions to manipulate \ref GDrawCommandList objects
//! @addtogroup Graphics
//! @{
//!   @addtogroup DrawCommand Draw Commands
//!   @{

struct GDrawCommandList;

//! Draw command lists contain a list of commands that can be iterated over and drawn all at once
typedef struct GDrawCommandList GDrawCommandList;

typedef struct GDrawCommandProcessor GDrawCommandProcessor;

//! Callback for iterating over GDrawCommands
//! @param processor GDrawCommandProcessor that is currently iterating over the GDrawCommandList.
//! @param proccessed_command Copy of the current GDrawCommand that can be modified
//! @param processed_command_max_size Size of GDrawCommand being processed
//! @param list list of GDrawCommands that will be modified by the processor
//! @param command Current GDrawCommand being processed
typedef void (*GDrawCommandProcessCommand)(GDrawCommandProcessor *processor,
                                           GDrawCommand *processed_command,
                                           size_t processed_command_max_size,
                                           const GDrawCommandList* list,
                                           const GDrawCommand *command);

//! @internal
//! Data used by the processor
typedef struct GDrawCommandProcessor {
  // TODO: PBL-23778 processors for image, sequence, frame
  GDrawCommandProcessCommand command;
} GDrawCommandProcessor;

//! Callback for iterating over draw command list
//! @param command current \ref GDrawCommand in iteration
//! @param index index of the current command in the list
//! @param context context pointer for the iteration operation
//! @return true if the iteration should continue after this command is processed
typedef bool (*GDrawCommandListIteratorCb)(GDrawCommand *command, uint32_t index, void *context);

//! @internal
//! Use to validate a command list read from flash or copied from serialized data
//! @param size Size of the command list structure in memory, in bytes
bool gdraw_command_list_validate(GDrawCommandList *command_list, size_t size);

//! @internal
//! Iterate over all commands in a command list
//! @param command_list \ref GDrawCommandList over which to iterate
//! @param handle_command iterator callback
//! @param callback_context context pointer to be passed into the iterator callback
//! @returns pointer to the address immediately following the end of the command list
void *gdraw_command_list_iterate_private(GDrawCommandList *command_list,
                                         GDrawCommandListIteratorCb handle_command,
                                         void *callback_context);

//! Iterate over all commands in a command list
//! @param command_list \ref GDrawCommandList over which to iterate
//! @param handle_command iterator callback
//! @param callback_context context pointer to be passed into the iterator callback
void gdraw_command_list_iterate(GDrawCommandList *command_list,
                                GDrawCommandListIteratorCb handle_command, void *callback_context);

//! Draw all commands in a command list
//! @param ctx The destination graphics context in which to draw
//! @param command_list list of commands to draw
void gdraw_command_list_draw(GContext *ctx, GDrawCommandList *command_list);

//! Process and draw all commands in a command list
//! @param ctx The destination graphics context in which to draw
//! @param command_list list of commands to draw
//! @param processor Command processor required for drawing processed commands
void gdraw_command_list_draw_processed(GContext *ctx, GDrawCommandList *command_list,
                                       GDrawCommandProcessor *processor);

//! Get the command at the specified index
//! @note the specified index must be less than the number of commands in the list
//! @param command_list \ref GDrawCommandList from which to get a command
//! @param command_idx index of the command to get
//! @return pointer to \ref GDrawCommand at the specified index
GDrawCommand *gdraw_command_list_get_command(GDrawCommandList *command_list, uint16_t command_idx);

//! Get the number of commands in the list
//! @param command_list \ref GDrawCommandList from which to get the number of commands
//! @return number of commands in command list
uint32_t gdraw_command_list_get_num_commands(GDrawCommandList *command_list);

//! @internal
//! Get the total number of points in the list among all GDrawCommands
size_t gdraw_command_list_get_num_points(GDrawCommandList *command_list);

//! @internal
//! Get the size of a list in memory
size_t gdraw_command_list_get_data_size(GDrawCommandList *command_list);

//! @internal
//! Collect all the points in the draw commands list into a newly allocated buffer
//! The order is guaranteed to be the definition order of the points
//! @param command_list \ref GDrawCommandList from which to collect points
//! @param is_precise true to convert to GPointPrecise, otherwise points are converted to GPoint
//! @param num_points_out Optional pointer to uint16_t to receive the num points
GPoint *gdraw_command_list_collect_points(GDrawCommandList *command_list, bool is_precise,
    uint16_t *num_points_out);

bool gdraw_command_list_copy(void *buffer, size_t buffer_length, GDrawCommandList *src);

GDrawCommandList *gdraw_command_list_clone(GDrawCommandList *list);

void gdraw_command_list_destroy(GDrawCommandList *list);

//!   @} // end addtogroup DrawCommand
//! @} // end addtogroup Graphics
