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

#include <stdint.h>

//! @addtogroup Foundation
//! @{
//!   @addtogroup App
//!   @{

//! Application metadata flags.
//! Flags can be combined using the `|` operator.
//! @see PBL_PROCESS_INFO
typedef enum {
  //! Use to indicate the process is a "standard" app.
  //! The system will show the app in the main menu.
  PROCESS_INFO_STANDARD_APP = 0,
  //! Use to indicate the process is a watchface.
  //! The system will show the process in the watchfaces menu.
  PROCESS_INFO_WATCH_FACE = 1 << 0,
  //! Use to hide the process.
  PROCESS_INFO_VISIBILITY_HIDDEN = 1 << 1,
  //! Use to hide the process, unless there is ongoing communication with
  //! the companion smartphone application.
  PROCESS_INFO_VISIBILITY_SHOWN_ON_COMMUNICATION = 1 << 2,
  //! Use to indicate the process allows JavaScript API access
  PROCESS_INFO_ALLOW_JS = 1 << 3,
  //! Use to indicate the process should have a worker.bin installed as well.
  PROCESS_INFO_HAS_WORKER = 1 << 4,
  //! True, if process uses RockyJS APIs
  PROCESS_INFO_ROCKY_APP = 1 << 5,

  //! Bitmask, to store compile time platform
  PROCESS_INFO_PLATFORM_MASK    = 0xf << 6,
  //! SDK older than 4.2 doesn't store any value
  PROCESS_INFO_PLATFORM_UNKNOWN = 0x0 << 6,
  PROCESS_INFO_PLATFORM_APLITE  = 0x1 << 6,
  //! Values that are actually added by SDK 4.2+
  PROCESS_INFO_PLATFORM_BASALT  = 0x2 << 6,
  PROCESS_INFO_PLATFORM_CHALK   = 0x3 << 6,
  PROCESS_INFO_PLATFORM_DIORITE = 0x4 << 6,
  PROCESS_INFO_PLATFORM_EMERY   = 0x5 << 6,
} PebbleProcessInfoFlags;

//!   @} // group App
//! @} // group Foundation

// struct PebbleProcessInfo change log
// ================================
// struct_version (little endian):
// 0x0800 -- sdk_version and process_version uint16_t fields added (Grand Slam / 1.7)
// .major:0x08 .minor:0x01 -- all version fields split up into minor/major; uuid field appended (Junior Whopper / 2.0?)
// .major:0x08 .minor:0x02 -- 2.0, added resource crc and resource timestamp
// .major:0x09 .minor:0x00 -- 2.0, no more reloc_list_start
// .major:0x10 .minor:0x00 -- 2.0, added virtual_size
#define PROCESS_INFO_CURRENT_STRUCT_VERSION_MAJOR 0x10
#define PROCESS_INFO_CURRENT_STRUCT_VERSION_MINOR 0x0

// process info version for last know 1.x
// let this be a warning to engineers everywhere
// who want to design a system with fancy versioning and
// support
#define PROCESS_INFO_LEGACY_STRUCT_VERSION_MAJOR 0x08

// SDK change log
// ================================
// sdk.major:0x4 .minor:0x0  -- Bump the SDK version to make 1.x and 2.x apps distinguishable
// sdk.major:0x5 .minor:0x0  -- Bump the SDK version for breaking AppMessage changes b/t 2.x alpha and beta releases
// sdk.major:0x5 .minor:0x1  -- Added additional API functions (MenuLayer callbacks)
// sdk.major:0x5 .minor:0x2  -- Changed app heap double free behaviour.
// sdk.major:0x5 .minor:0x3  -- Added number_window_get_window (API v2.0 revision 12)
// sdk.major:0x5 .minor:0x4  -- Added gbitmap_create_blank (API v2.0 revision 13) and click_recognizer_is_repeating (rev 14)
// sdk.major:0x5 .minor:0x5  -- Added accel_raw_data_service_subscribe and related types (rev 15)
// sdk.major:0x5 .minor:0x6  -- Added background worker APIs (rev 16)
// sdk.major:0x5 .minor:0x7  -- Added heap_bytes_free / heap_bytes_used (rev 17)
// sdk.major:0x5 .minor:0x8  -- Added compass APIs (rev 18)
// sdk.major:0x5 .minor:0x9  -- Added Uuid utility APIs (rev 19)
// sdk.major:0x5 .minor:0xa  -- Fixed gpath behaviour, added gpath_draw_filled_legacy. (rev 20)
// sdk.major:0x5 .minor:0xb  -- Added custom animation curves (rev 21)
// sdk.major:0x5 .minor:0xc  -- Added API for model, color and firmware version of watch (rev 22)
// sdk.major:0x5 .minor:0xd  -- Added direct access to frame buffer (rev 23)
// sdk.major:0x5 .minor:0xe  -- Added wakeup API, app_launch_reason (rev 24)
// sdk.major:0x5 .minor:0xf  -- Added clock_is_timezone_set in preparation for timezone support (rev 25)
// sdk.major:0x5 .minor:0x10 -- Added the first i18n API: get_locale (rev 26)
// sdk.major:0x5 .minor:0x11 -- Added second i18n API: setlocale (rev 27)
// sdk.major:0x5 .minor:0x13 -- Export mktime (rev 29)
// sdk.major:0x5 .minor:0x14 -- Rev 30 was a move of several color APIs as to not conflict with release 2.9 sdk. Several of these functions were removed after rev 55 for 3.0-beta10
// sdk.major:0x5 .minor:0x15 -- Added timezone APIs (rev 31)
// sdk.major:0x5 .minor:0x16 -- Added 3.0 animation API (rev 32)
// sdk.major:0x5 .minor:0x17 -- Export new gbitmap accessors (rev 33)
// sdk.major:0x5 .minor:0x18 -- Export gbitmap_sequence API (rev 34)
// sdk.major:0x5 .minor:0x19 -- Export gbitmap_create_from_png_data API (rev 35)
// sdk.major:0x5 .minor:0x20 -- Export new complex animations API (rev 36)
// sdk.major:0x5 .minor:0x21 -- Export missing accessors animations APIs (rev 37)
// sdk.major:0x5 .minor:0x22 -- Export launch_get_args() API (rev 38)
// sdk.major:0x5 .minor:0x23 -- Export 3.0 menu_layer_create() and menu_layer_shadow_enable() (rev 39)
// sdk.major:0x5 .minor:0x24 -- Added additional calls to gbitmap_sequence (rev 40)
// sdk.major:0x5 .minor:0x25 -- Export antialiased flag and stroke width in GContext (rev 41)
// sdk.major:0x5 .minor:0x26 -- Added 3.0 ActionBar (rev 42)
// sdk.major:0x5 .minor:0x27 -- Added gbitmap_sequence_update_bitmap_by_elapsed (rev 43)
// sdk.major:0x5 .minor:0x28 -- Export menu_layer_cell_is_highlighted() and gbitmap_create_palettized_from_1bit() (rev 44)
// sdk.major:0x5 .minor:0x29 -- Export graphics_draw_rotated_bitmap() API (rev 45)
// sdk.major:0x5 .minor:0x2a -- Added action_bar_layer_set_icon_press_animation (rev 46)
// sdk.major:0x5 .minor:0x2b -- Created TextLayerLegacy2 (rev 47)
// sdk.major:0x5 .minor:0x2c -- Added GDrawCommand (rev 48)
// sdk.major:0x5 .minor:0x2d -- Added property_animation_update_uint32 (rev 49)
// sdk.major:0x5 .minor:0x2e -- Added gpath_draw_outline_open (rev 50)
// sdk.major:0x5 .minor:0x2f -- Fixed legacy time() support (rev 51)
// sdk.major:0x5 .minor:0x30 -- Deprecate inverter layer (rev 52)
// sdk.major:0x5 .minor:0x31 -- Added default menu layer colors (rev 53)
// sdk.major:0x5 .minor:0x32 -- Added menu_layer_set_callbacks (rev 54)
// sdk.major:0x5 .minor:0x33 -- API cleanup, loop_count --> play_count (rev 55)
// sdk.major:0x5 .minor:0x34 -- Added menu_layer_pad_bottom_enable (rev 56)
// sdk.major:0x5 .minor:0x35 -- Export initial version of StatusBarLayer (rev 57)
// sdk.major:0x5 .minor:0x36 -- Export difftime (rev 58)
// sdk.major:0x5 .minor:0x37 -- Fixed legacy time_ms() support (rev 59)
// sdk.major:0x5 .minor:0x38 -- Export gcolor_legible_over (rev 60)
// sdk.major:0x5 .minor:0x39 -- Export property_animation_update_gcolor8 (rev 61)
// sdk.major:0x5 .minor:0x3a -- Export app_focus_service_subscribe_handlers (rev 62)
// sdk.major:0x5 .minor:0x3b -- Export ActionMenu APIs (rev 63)
// sdk.major:0x5 .minor:0x3c -- Export Smartstrap APIs (rev 64)
// sdk.major:0x5 .minor:0x3d -- Added support for timeline resources in PBWs (rev 65)
// sdk.major:0x5 .minor:0x3e -- Update Connection Service APIs (rev 66)
// sdk.major:0x5 .minor:0x3f -- Enabled support for 8K App Messages (rev 67)
// sdk.major:0x5 .minor:0x40 -- Allow disabling of error dialogs for the voice API (rev 68)
// sdk.major:0x5 .minor:0x41 -- DEG_TO_TRIGANGLE (rev 69)
// sdk.major:0x5 .minor:0x42 -- Export gbitmap_get_data_row_info (rev 70)
// sdk.major:0x5 .minor:0x43 -- Export ContentIndicator (rev 71)
// sdk.major:0x5 .minor:0x45 -- Export grect_inset (rev 73)
// sdk.major:0x5 .minor:0x46 -- Export fill_radial, draw_arc, etc. (rev 74)
// sdk.major:0x5 .minor:0x47 -- Text flow and scroll layer pagination (rev 75)
// sdk.major:0x5 .minor:0x48 -- Export menu_layer_is_index_selected and round menu layer cell heights (rev 76)
// sdk.major:0x5 .minor:0x49 -- Added font v3 (no API changes) (rev 76)
// sdk.major:0x5 .minor:0x4a -- HealthAPI (rev 77)
// sdk.major:0x5 .minor:0x4b -- More time utility functions (rev 78)
// sdk.major:0x5 .minor:0x4c -- More Health service API calls (rev 79)
// sdk.major:0x5 .minor:0x4d -- Add health_service_get_measurement_system_for_display() (rev 80)
// sdk.major:0x5 .minor:0x4e -- Export gdraw_command_frame_get_command_list() (rev 81)
// sdk.major:0x5 .minor:0x4f -- Add new version of gcolor_equal() and deprecate old version (rev 82)
// sdk.major:0x5 .minor:0x50 -- 4.0: Add health service expansion, UnobstructedArea, AppGlance, AppExitReason, result duration (rev 83)
// sdk.major:0x5 .minor:0x51 -- Add memory_cache_flush() (rev 84)
// sdk.major:0x5 .minor:0x52 -- rocky_event_loop_with_resource (rev. 85)
// sdk.major:0x5 .minor:0x53 -- Add health_service heart rate sampling period support (rev 86)
// sdk.major:0x5 .minor:0x54 -- Add PlatformType enum and defines (rev 87)
// sdk.major:0x5 .minor:0x55 -- Preferred Content Size (rev 88)
// sdk.major:0x5 .minor:0x56 -- Add PlatformType enum and defines (rev 89)

#define PROCESS_INFO_CURRENT_SDK_VERSION_MAJOR 0x5
#define PROCESS_INFO_CURRENT_SDK_VERSION_MINOR 0x56

// The first SDK to ship with 2.x APIs
#define PROCESS_INFO_FIRST_2X_SDK_VERSION_MAJOR 0x4
#define PROCESS_INFO_FIRST_2X_SDK_VERSION_MINOR 0x0

// The first SDK to ship with 3.x APIs
#define PROCESS_INFO_FIRST_3X_SDK_VERSION_MAJOR 0x5
#define PROCESS_INFO_FIRST_3X_SDK_VERSION_MINOR 0x16

// The first SDK to ship with 4.x APIs
#define PROCESS_INFO_FIRST_4X_SDK_VERSION_MAJOR 0x5
#define PROCESS_INFO_FIRST_4X_SDK_VERSION_MINOR 0x50

// The first SDK that has a stable Rocky.js contract + stores platforms in binary
#define PROCESS_INFO_FIRST_4_2_X_SDK_VERSION_MAJOR 0x5
#define PROCESS_INFO_FIRST_4_2_X_SDK_VERSION_MINOR 0x54

#define PROCESS_NAME_BYTES 32
#define COMPANY_NAME_BYTES 32

//! @internal
//! Version data structure with minor & major versions: When making non-backwards-compatible changes,
//! the major version should get bumped. When making a change (e.g. to the PebbleProcessInfo struct) that is backwards
//! compatible (e.g. adding a field at the end), you should only bump the minor version.
typedef struct __attribute__((__packed__)) {
  uint8_t major; //!< "compatibility" version number
  uint8_t minor;
} Version;

//! @return 0 if a and b are equal, >0 if a > b, <0 if b > a
int version_compare(Version a, Version b);

//! @internal
// WARNING: changes in this struct must be reflected in:
// - tintin/waftools/inject_metadata.py
// - iOS/PebblePrivateKit/PebblePrivateKit/PBBundle.m
typedef struct __attribute__((__packed__)) {
  char header[8];                   //!< Sentinal value, should always be 'PBLAPP'
  Version struct_version;           //!< version of this structure's format
  Version sdk_version;              //!< version of the SDK used to build this process
  Version process_version;          //!< version of the process
  uint16_t load_size;               //!< size of the binary in flash, including this metadata but not the reloc table
  uint32_t offset;                  //!< The entry point of this executable
  uint32_t crc;                     //!< CRC of the data only, ie, not including this struct or the reloc table at the end
  char name[PROCESS_NAME_BYTES];    //!< Name to display on the menu
  char company[COMPANY_NAME_BYTES]; //!< Name of the maker of this process
  uint32_t icon_resource_id;        //!< Resource ID within this bank to use as a 32x32 icon
  uint32_t sym_table_addr;          //!< The system will poke the sdk's symbol table address into this field on load
  uint32_t flags;                   //!< Bitwise OR of PebbleProcessInfoFlags
  uint32_t num_reloc_entries;       //!< The number of entries in the address relocation list
  struct __attribute__((__packed__)) {
    uint8_t byte0;
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
    uint8_t byte4;
    uint8_t byte5;
    uint8_t byte6;
    uint8_t byte7;
    uint8_t byte8;
    uint8_t byte9;
    uint8_t byte10;
    uint8_t byte11;
    uint8_t byte12;
    uint8_t byte13;
    uint8_t byte14;
    uint8_t byte15;
  } uuid;                           //!< The process's UUID
  uint32_t resource_crc;            //!< CRC of the resource data only
  uint32_t resource_timestamp;      //!< timestamp of the resource data
  uint16_t virtual_size;            //!< The total amount of memory used by the process (.text + .data + .bss)
} PebbleProcessInfo;

//! @internal
typedef struct __attribute__((__packed__)) {
  char header[8];                   //!< Sentinal value, should always be 'PBLAPP'
  Version struct_version;           //!< version of this structure's format
  Version sdk_version;              //!< version of the SDK used to build this process
  Version process_version;          //!< version of the process
  uint16_t load_size;               //!< size of the binary in flash, including this metadata but not the reloc table
  uint32_t offset;                  //!< The entry point of this executable
  uint32_t crc;                     //!< CRC of the data only, ie, not including this struct or the reloc table at the end
  char name[PROCESS_NAME_BYTES];    //!< Name to display on the menu
  char company[COMPANY_NAME_BYTES]; //!< Name of the maker of this process
  uint32_t icon_resource_id;        //!< Resource ID within this process's bank to use as a 32x32 icon
  uint32_t sym_table_addr;          //!< The system will poke the sdk's symbol table address into this field on load
  uint32_t flags;                   //!< Bitwise OR of PebbleProcessInfoFlags
  uint32_t reloc_list_start;        //!< The offset of the address relocation list
  uint32_t num_reloc_entries;       //!< The number of entries in the address relocation list
  struct __attribute__((__packed__)) {
    uint8_t byte0;
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
    uint8_t byte4;
    uint8_t byte5;
    uint8_t byte6;
    uint8_t byte7;
    uint8_t byte8;
    uint8_t byte9;
    uint8_t byte10;
    uint8_t byte11;
    uint8_t byte12;
    uint8_t byte13;
    uint8_t byte14;
    uint8_t byte15;
  } uuid;                           //!< The process's UUID
} LegacyPebbleAppInfo;


