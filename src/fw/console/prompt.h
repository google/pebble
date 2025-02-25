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

//! @file prompt.h
//!
//! This file handles the prompt mode of our serial console. This allows a user to enter and
//! execute commands. It also has support for other modules to execute their own commands by
//! supplying their own PromptContext
//!
//! TODO: We should probably split this in the future so there's one module to handle the
//! dbgserial part and another module to handle executing commands.

#include "util/attributes.h"

#include <stdbool.h>
#include <stddef.h>

typedef void (*PromptResponseCallback)(const char *response);
typedef void (*PromptCommandCompleteCallback)(void);

#define PROMPT_BUFFER_SIZE_BYTES 128
typedef struct PromptContext {
  //! Function to call to send the response text from executed commands.
  PromptResponseCallback response_callback;
  //! Function to call when the command has completed.
  PromptCommandCompleteCallback command_complete_callback;

  //! Which index we were currently writing to, should never be greater than
  //! PROMPT_BUFFER_SIZE_BYTES - 1.
  unsigned int write_index;

  char buffer[PROMPT_BUFFER_SIZE_BYTES + 1]; // Leave space for a trailing null always.
} PromptContext;

//! Asks the console to switch to prompt mode. Used by other console modes to flip back to the
//! prompt when they're done.
void console_switch_to_prompt(void);

//! Called on an ISR. Handles a new character from the dbgserial when we're in prompt mode.
void prompt_handle_character(char c, bool* should_context_switch);

//! Appends a character to a given context.
//! @return true if the character fits, false if the buffer is full
bool prompt_context_append_char(PromptContext *context, char c);

//! Executes a command in the given context.
void prompt_context_execute(PromptContext *context);

//! Feed the task watchdog for the thread that commands run on. Call this regularly if your
//! command takes a long time (multiple seconds).
void prompt_watchdog_feed(void);

//! Use this from a prompt command to respond to a command. The output will directed out the
//! appropriate output terminal depending on who ran the command (dbgserial or accessory
//! connector).
//! @param response NULL-terminated string
void prompt_send_response(const char* response);

//! Use this from a prompt command to respond to a command. The output will directed out the
//! appropriate output terminal depending on who ran the command (dbgserial or accessory
//! connector). This option allows the use of printf style formatters to create output.
void prompt_send_response_fmt(char* buffer, size_t buffer_size, const char* fmt, ...)
    FORMAT_PRINTF(3, 4);

//! Finishes the currently running prompt command, and sends the prompt command complete message.
//! This is only to be used if \ref prompt_command_continues_after_returning has been called,
//! or if the command cannot possibly return.
void prompt_command_finish(void);

//! Holds the prompt open after the currently executing command callback returns.
//! This allows for responses to be sent back from callbacks.
//! Make sure to finish the command with \ref prompt_command_finish
void prompt_command_continues_after_returning(void);

//! Returns true if there is currently a prompt command executing, false otherwise.
bool prompt_command_is_executing(void);
