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

#include "app_install_types.h"

#include "applib/graphics/gtypes.h"
#include "applib/graphics/gcolor_definitions.h"
#include "drivers/rtc.h"
#include "kernel/events.h"
#include "pebble_process_md.h"
#include "resource/resource.h"
#include "util/list.h"

//////////////////////////////////////////////////////////////////////////////
// This module is responsible for keeping track of what apps are installed,
// and acts as the abstraction which makes system and third party apps look the same
//////////////////////////////////////////////////////////////////////////////

//! There are many different representations, structures, and identifiers of an Application within
//! our firmware. This is an attempt to clear up the different types and also to document them.
//!
//! Accessing apps or their metadata
//! ----------------------------------
//! Preferred Methods
//!
//! - AppInstallEntry: Universal struct that contains all metadata for an application no matter
//!                    where it originates from (FW, Resource, Flash)
//! - AppInstallId: A number assigned to an application. Pre-assigned and negative for FW and
//!                 resource applications, assigned on install for Flash applications.
//!
//! Deprecated Methods
//!
//! - Uuid: a 16-byte identifier for an application. Every application process must contain a Uuid,
//!         but it is not a requirement for firmware applications. Should avoid whenever possible.
//!         A Uuid should only be used when in communication with the phone, since the phone should
//!         have no knowledge of AppInstallId's.
//!
//! - PebbleProcessMd: This should only be used when launching an application. No piece of code
//!                    should ask the app_install_manager for a PebbleProcessMd unless it plans on
//!                    assisting with the launch of the application (event_loop, app_manager, etc.)
//!
//! How applications are stored
//! ---------------------------
//! Serialized Data Structures
//!
//! - AppDBEntry: The phone sends over this packed data in BlobDB and is stored in flash on the
//!               watch. This data gets deserialized in the AppInstallManager and morphed into an
//!               AppInstallEntry. This structure can't change without agreeing with the phone on
//!               the changes. These entries are assigned an AppInstallId on install.
//!
//! - AppRegistryEntry: Hardcoded metadata for system application that comes packaged with the
//!                     firmware. There are two types: FW and RESOURCE. These entries are assigned
//!                     an AppInstallId by the programmer. Hardcoded and should never change.
//!     - FW:       These are the applications that have hard coded PebbleProcessMd's.
//!     - RESOURCE: These are stored in the System Resource Pack and loaded on demand.
//!
//! Misc Notes
//! ----------
//!
//! If any module wants information about an application, the process is as follows:
//! 1. It should first retrieve an AppInstallId.
//! 2. Call app_install_get_entry_for_install_id and get the entry data structure
//! 3. Use the getter functions for the entry to retrieve individual fields within the struct.


#define TIMESTAMP_INVALID ((RtcTicks)0) //!< for most_recent_communication_timestamp in AppInstallEntry

//! Max number of bytes for an application.
#define APP_NAME_SIZE_BYTES 96

typedef enum {
  AppInstallStorageInvalid = 0,
  AppInstallStorageFw = 1,
  AppInstallStorageFlash = 2,
  AppInstallStorageResources = 3,
} AppInstallStorage;

typedef struct {
  AppInstallId install_id;
  AppInstallStorage type:2; // SYSTEM/RESOURCE/FLASH
  ProcessVisibility visibility;
  ProcessType process_type; // WATCHFACE/APP
  bool has_worker;
  Uuid uuid;
  GColor color;
  char name[APP_NAME_SIZE_BYTES];
  int icon_resource_id;
  Version sdk_version;
  unsigned int record_order; //!< 0 means not in the app registry
} AppInstallEntry;

typedef enum {
  APP_AVAILABLE = 0, //< occurs on app installation
  APP_REMOVED = 1, //< occurs on app removal
  APP_ICON_NAME_UPDATED = 2, //< occurs when app (metadata) has been updated
  APP_UPGRADED = 3, //< occurs when app is getting removed prior to upgrade
  APP_DB_CLEARED = 4, //< occurs when app is getting removed prior to upgrade
  NUM_INSTALL_EVENT_TYPES,
} InstallEventType;

//! Used for the static application entries in the app registry
typedef const PebbleProcessMd* (*MdFunc) (void);

//! Used for apps listed in the system app registry
typedef struct {
  AppInstallId id;
  AppInstallStorage type;
  GColor color;
  union {
    MdFunc md_fn;
    struct {
      const char *name;
      Uuid uuid;
      int bin_resource_id;
      int icon_resource_id;
    };
  };
} AppRegistryEntry;

void app_install_manager_init(void);

//! Get AppInstallId for the provided Uuid
//! @param uuid Uuid to convert to an AppInstallId
//! @return valid AppInstallId or INSTALL_ID_INVALID
AppInstallId app_install_get_id_for_uuid(const Uuid *uuid);

//! Search the system registry for the AppInstallId for the provided Uuid
//! @param uuid Uuid to convert to an AppInstallId
//! @return valid AppInstallId or INSTALL_ID_INVALID if not a built-in app
AppInstallId app_get_install_id_for_uuid_from_registry(const Uuid *uuid);

//! Subscription callback
typedef void (*AppInstallCallback)(const AppInstallId install_id, void *data);

typedef struct AppInstallCallbackNode {
  ListNode node;
  //! Must point to data that lives at least until app_install_deregister_callback() is called:
  void *data;
  //! Array of callbacks for each event type:
  const AppInstallCallback *callbacks;
  PebbleTask registered_by;
} AppInstallCallbackNode;

//! Registers callbacks for add/remove/change events from the app install manager
//! @note Callbacks are invoked on the launcher task!
void app_install_register_callback(struct AppInstallCallbackNode *callback_info);

//! Deregisters callbacks for add/remove/change events from the app install manager
void app_install_deregister_callback(struct AppInstallCallbackNode *callback_info);

//! Deregisters callbacks that were registered on the app task
void app_install_cleanup_registered_app_callbacks(void);

//! Generates an AppInstallEntry for the install_id given and reads it into the given entry
//! pointer.
//!
//! @note If any piece of code wants to read any characteristics of an installed application,
//! it should first get an entry then call the below operations to read the fields of the
//! struct.
//! @param install_id AppInstallId of the application
//! @param entry AppInstallEntry buffer to write to
//! @return return True if the entry was successfully retrieved and written to the buffer. Will
//! return false if the ID is invalid or does not exist, or if entry is NULL.
bool app_install_get_entry_for_install_id(AppInstallId install_id, AppInstallEntry *entry);

//! Gets the corresponding Uuid given an AppInstallId. This loads an entry to obtain its
//! information, use the entry directly if your context has an AppInstallEntry.
//! @param install_id AppInstallId of the application
//! @param uuid_out pointer to a Uuid buffer where the application Uuid will be written
//! @return false if the ID is invalid or does not exist, true otherwise
bool app_install_get_uuid_for_install_id(AppInstallId install_id, Uuid *uuid_out);

//! Gets whether the app is a watchface given an AppInstallId. This loads an entry to obtain its
//! information, use \ref app_install_entry_is_watchface if your context has an AppInstallEntry.
//! @param install_id AppInstallId of the application
//! @return true if the app is a watchface, false otherwise
bool app_install_is_watchface(AppInstallId app_id);

//! Returns true if the app associated with the provided entry is a watchface
//! @param entry AppInstallEntry to check the parameters of
bool app_install_entry_is_watchface(const AppInstallEntry *entry);

//! Returns true if the app associated with the provided entry has a worker
//! @param entry AppInstallEntry to check the parameters of
bool app_install_entry_has_worker(const AppInstallEntry *entry);

//! Returns true if the app associated with the provided entry should be hidden in menus
//! @param entry AppInstallEntry to check the parameters of
bool app_install_entry_is_hidden(const AppInstallEntry *entry);

//! Gets whether the app is visible in the list of apps that can be set as a quick launch shortcut.
//! @return true if the app is only visible in quick launch, false otherwise
bool app_install_entry_is_quick_launch_visible_only(const AppInstallEntry *entry);

//! Returns true if the app associated with the provided entry is SDK compatible
//! @param entry AppInstallEntry to check the parameters of
bool app_install_entry_is_SDK_compatible(const AppInstallEntry *entry);

typedef bool(*AppInstallEnumerateCb)(AppInstallEntry *entry, void *data);

//! Enumerates all active install ids for non-hidden apps and calls the given function for each
//! install id.
//! @param cb Pointer to a function that will get called once for each app install id.
//! This callback can return false to end the enumeration prematurely, or true to continue.
//! @param data Pointer to arbitrary user data that will get passed into the function
void app_install_enumerate_entries(AppInstallEnumerateCb cb, void *data);

void app_install_clear_app_db(void);

//! These functions are not reading characteristics of the AppInstallEntry so they do not
//! require an entry to be passed in.
bool app_install_is_app_running(AppInstallId id);

bool app_install_is_worker_running(AppInstallId id);

void app_install_notify_app_closed(void);

void app_install_notify_worker_closed(void);

//! @param id The ID for the desired MD
//! @param worker True if we want the worker MD, false if we want the app MD
//! @return Returns a pointer to the PebbleProcessMd. When the caller is done with this pointer
//!         they should call app_install_md_release to release the associated memory.
const PebbleProcessMd *app_install_get_md(AppInstallId id, bool worker);

//! Release a md that was previously allocated by app_install_get_md.
void app_install_release_md(const PebbleProcessMd *md);

//! Retrieves the custom name for an application if it has sent a new application name
const char *app_install_get_custom_app_name(AppInstallId install_id);

bool app_install_is_prioritized(AppInstallId install_id);

void app_install_mark_prioritized(AppInstallId install_id, bool can_expire);

void app_install_unmark_prioritized(AppInstallId install_id);

#if UNITTEST
void app_install_manager_flush_recent_communication_timestamps(void);
#endif

//////////////////////
// Deprecated
// = Will remove once launcher is reimplemented
//////////////////////

uint32_t app_install_entry_get_icon_resource_id(const AppInstallEntry *entry);

ResAppNum app_install_get_app_icon_bank(const AppInstallEntry *entry);
