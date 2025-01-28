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

/************************************************************************************************
 * New Logging
 *
 * The NewLogging system provides in place hashing during the compile stage of building, removing
 * logging strings from the source code and replacing them with a unique token, conserving space in
 * the firmware.
 *
 * The unique token is (well, see below) actually just a pointer to a new section in the .elf
 * named .log_strings. The .log_strings section is mapped to an unused portion of memory and
 * isn't compiled into the final firmware binary image.
 *
 * Token format:
 *   The token is actually a packed uint32_t. The format is as follows:
 *   31-29: num fmt conversions  [0-7]
 *   28-26: string index 2       [0-7], 1 based. 0 if no second string. 1-7 otherwise
 *   25-23: string index 1       [0-7], 1 based. 0 if no first string. 1-7 otherwise
 *   22-20: log level            [0-5] mapped onto LOG_LEVEL_ALWAYS through LOG_LEVEL_DEBUG_VERBOSE
 *      19: reserved
 *   18- 0: Offset info .log_strings section. This allows 512 KB of strings.
 *
 * Note: it might not be necessary to use so many bits for the log level. Dynamic flitering might
 * not be so important, and 'log to flash' could be 1 bit, or Curried to a set of function calls.
 * These changes would require more work in the logging infrastructure.
 *
 * .log_strings Section is formatted as follows:
 *  - .log_string.header: "NL<M><m>:<offset-mask>=<token-list>"
 *    where:
 *      - <M> is XX major version -- increase means not backwards compatible change
 *      - <m> is YY minor version -- increase means backwards compatible change
 *      - <offset-mask> defines the number of bits used in the token for the section offset
 *      - <token-list>: <token>:<token-list>
 *                    : '\0'
 *      - <token>: <file>
 *               : <line>
 *               : <level>
 *               : <color>
 *               : <fmt>
 *  - .log_core_number: "CORE<C>"
 *    where:
 *      - <C> is the core number. This will be two bits.
 *         For now, the primary core will be 00; the Bluetooth chip will be 01.
 *         These definitions will be different for every system -- all that matters is that they're
 *         internally consistent.
 *  - .log_string
 *    which is a list of <token-list> representing the log strings from the source code.
 *
 * Note: this code must be compiled with -Os or the codesize will explode!
 *
 * Limitations:
 * - maximum 7 format conversions per print
 * - maximum 2 string conversions per print
 * - string parameters may not be flagged or formatted in any way --'%s' only.
 * - printing the '%' is not supported --  '%%' is not allowed.
 * - only 32 bit (or fewer) parameters currently supported automatically. Multi-word parameters
 *     require special handling.
 * - errors are not automatically detected. This will have to be done later by a script. Sorry.
 *
 * Extensions (to be implemented at some point):
 * - Colour groups/overrides
 * - MAC/BT address print: format specifier: %M/m
 * - ENUM print: %u[<enum name>]
 *
 * LogHash.dict:
 *   See https://pebbletechnology.atlassian.net/wiki/display/DEV/New+Logging.
 ***********************************************************************************************/
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "util/attributes.h"

#define NEW_LOG_VERSION "0101"

#define LOG_STRINGS_SECTION_ADDRESS 0xC0000000

#define PACKED_CORE_OFFSET 30        // 2 bits - Core number
#define PACKED_CORE_MASK   0x03

#define PACKED_NUM_FMT_OFFSET 29     // 3 bits - Number format conversions
#define PACKED_NUM_FMT_MASK 0x07
#define PACKED_STR1FMT_OFFSET 26     // 3 bits - indicies of string parameter 1 format conversion
#define PACKED_STR1FMT_MASK 0x07
#define PACKED_STR2FMT_OFFSET 23     // 3 bits - indicies of string parameter 2 format conversion
#define PACKED_STR2FMT_MASK 0x07
#define PACKED_STRFMTS_OFFSET 23     // 6 bits - indicies of string parameters 1 & 2.
#define PACKED_STRFMTS_MASK 0x3f
#define PACKED_LEVEL_OFFSET 20       // 3 bits  - log level
#define PACKED_LEVEL_MASK 0x07
#define PACKED_HASH_OFFSET 0
#define PACKED_HASH_MASK 0x7FFFF     // 19 bits - string table offset (512 KB)

#define MSGID_STR_AND_HASH_MASK ((PACKED_STRFMTS_MASK << PACKED_STRFMTS_OFFSET) | \
                                 (PACKED_HASH_MASK << PACKED_HASH_OFFSET))
#define MSGID_CORE_AND_HASH_MASK ((PACKED_CORE_MASK << PACKED_CORE_OFFSET) | \
                                  (PACKED_HASH_MASK << PACKED_HASH_OFFSET))

#ifndef STRINGIFY
  #define STRINGIFY_NX(a) #a
  #define STRINGIFY(a) STRINGIFY_NX(a)
#endif // STRINGIFY

/* Printf Format argument checking.
 *
 * NB: it's critical that the 'if (false)' tag is included before the call to
 * PBL_LOG_x_printf_arg_check(). Without this obviously useless check, PBL_LOG_x_printf_arg_check()
 * would not be optimised out and cause a) a linker error (missing function body), b) take up
 * code space & time, and c) would cause the arguments passed to PBL_LOG (NEW_LOG_HASH) to be
 * evaluated twice. This is fine with normal parameters, but could result in macros or functions
 * being called twice and messing up globals in unexpected ways.
 */
void PBL_LOG_x_printf_arg_check(const char *fmt, ...) FORMAT_PRINTF(1, 2);

#define NEW_LOG_HASH(logfunc, level, color, fmt, ...) \
{ \
  static const char str[] __attribute__((nocommon, section(".log_strings"))) = \
    __FILE__ ":" STRINGIFY(__LINE__) ":" STRINGIFY(level) ":" color ":" fmt; \
  logfunc((uint32_t)&str[LOG_SECTION_OFFSET(level, fmt)], ##__VA_ARGS__); \
  if (0) PBL_LOG_x_printf_arg_check(fmt, ##__VA_ARGS__); \
}

ALWAYS_INLINE static uint32_t LOG_SECTION_OFFSET(const uint8_t level, const char *fmt) {
  const char *p1 = NULL, *p2 = NULL, *p3 = NULL, *p4 = NULL;
  const char *p5 = NULL, *p6 = NULL, *p7 = NULL, *p8 = NULL;
  const char *s1 = NULL, *s2 = NULL, *s3 = NULL, *s4 = NULL;
  const char *s5 = NULL, *s6 = NULL, *s7 = NULL;

  // Search for % characters in fmt. p1-p8 point to the character immediately succeeding the first
  // 8 % characters in fmt (or NULL, if there aren't 8 % characters in fmt).
  p1 = __builtin_strchr(fmt, '%') ? (__builtin_strchr(fmt, '%') + 1) : NULL;
  if (p1) p2 = __builtin_strchr(p1, '%') ? (__builtin_strchr(p1, '%') + 1) : NULL;
  if (p2) p3 = __builtin_strchr(p2, '%') ? (__builtin_strchr(p2, '%') + 1) : NULL;
  if (p3) p4 = __builtin_strchr(p3, '%') ? (__builtin_strchr(p3, '%') + 1) : NULL;
  if (p4) p5 = __builtin_strchr(p4, '%') ? (__builtin_strchr(p4, '%') + 1) : NULL;
  if (p5) p6 = __builtin_strchr(p5, '%') ? (__builtin_strchr(p5, '%') + 1) : NULL;
  if (p6) p7 = __builtin_strchr(p6, '%') ? (__builtin_strchr(p6, '%') + 1) : NULL;
  if (p7) p8 = __builtin_strchr(p7, '%') ? (__builtin_strchr(p7, '%') + 1) : NULL;

  // Check that fmt doesn't contain the escaped % symbol, '%%'. It's too hard to handle correctly
  // in every case.
  if ((p1 + 1 == p2) || (p2 + 1 == p3) || (p3 + 1 == p4) || (p4 + 1 == p5) ||
      (p5 + 1 == p6) || (p6 + 1 == p7) || (p7 + 1 == p8)) {
    return 0;
  }

  // Count number of valid pointers by bool-inversion-twice
  uint8_t num_params = !!p1 + !!p2 + !!p3 + !!p4 + !!p5 + !!p6 + !!p7 + !!p8;

  // Check that there aren't more than 7 format conversions. We have only 3 bits per string index.
  if (num_params > 7) {
    return 0;
  }

  // Search for an 's' character succeeding the % characters in fmt. s1-s7 point to the first 's'
  // charactres in fmt after the previously found % characters (or NULL if there aren't 7 's'
  // characters in fmt).
  if (p1) s1 = __builtin_strchr(p1, 's');
  if (p2) s2 = __builtin_strchr(p2, 's');
  if (p3) s3 = __builtin_strchr(p3, 's');
  if (p4) s4 = __builtin_strchr(p4, 's');
  if (p5) s5 = __builtin_strchr(p5, 's');
  if (p6) s6 = __builtin_strchr(p6, 's');
  if (p7) s7 = __builtin_strchr(p7, 's');

  // See if the 's' characters immediately succeed the '%' characters. If so, set flag psX.
  const int ps1 = p1 ? (p1 == s1) : 0;
  const int ps2 = p2 ? (p2 == s2) : 0;
  const int ps3 = p3 ? (p3 == s3) : 0;
  const int ps4 = p4 ? (p4 == s4) : 0;
  const int ps5 = p5 ? (p5 == s5) : 0;
  const int ps6 = p6 ? (p6 == s6) : 0;
  const int ps7 = p7 ? (p7 == s7) : 0;

  // Count the number of '%s' parameters
  const int num_s_params = ps1 + ps2 + ps3 + ps4 + ps5 + ps6 + ps7;

  // We currently support only 2 string parameters.
  if (num_s_params > 2) {
    return 0;
  }

  // Format the (maximum) two string parameter indicies as:
  // (1-based index of first %s << 3) | (1-based index of second %s << 0)
  // If there is only one %s parameter, the index will be formatted as:
  // (1-based index of first %s << 0)
  const int a1 = ps1 ? 1 : 0;
  const int a2 = ps2 ? (a1 << 3) + 2 : a1;
  const int a3 = ps3 ? (a2 << 3) + 3 : a2;
  const int a4 = ps4 ? (a3 << 3) + 4 : a3;
  const int a5 = ps5 ? (a4 << 3) + 5 : a4;
  const int a6 = ps6 ? (a5 << 3) + 6 : a5;
  const int a7 = ps7 ? (a6 << 3) + 7 : a6;
  const int string_indicies = a7;


  // Convert level to packed_level
  int packed_level = LOG_LEVEL_ALWAYS;
  if (level == LOG_LEVEL_ERROR) {
    packed_level = 1;
  } else if (level == LOG_LEVEL_WARNING) {
    packed_level = 2;
  } else if (level == LOG_LEVEL_INFO) {
    packed_level = 3;
  } else if (level == LOG_LEVEL_DEBUG) {
    packed_level = 4;
  } else if (level == LOG_LEVEL_DEBUG_VERBOSE) {
    packed_level = 5;
  }

  const uint32_t offset =  (((num_params & PACKED_NUM_FMT_MASK) << PACKED_NUM_FMT_OFFSET) |
                            ((packed_level & PACKED_LEVEL_MASK) << PACKED_LEVEL_OFFSET) |
                            ((string_indicies & PACKED_STRFMTS_MASK) << PACKED_STRFMTS_OFFSET));

  return (offset - LOG_STRINGS_SECTION_ADDRESS);
}
