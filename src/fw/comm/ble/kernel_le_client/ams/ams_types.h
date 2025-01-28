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

#include "util/attributes.h"
#include "util/math.h"

#include <stdint.h>

//! Dumping ground for Apple Media Service types
//! All these values come from the Appendix in the specification:
//! https://developer.apple.com/library/ios/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Appendix/Appendix.html#//apple_ref/doc/uid/TP40014716-CH3-SW2

////////////////////////////////////////////////////////////////////////////////////////////////////
// Enumerations

//! When writing to any characteristic, or when reading the Entity Attribute,
//! the client may receive the following AMS-specific error codes:
typedef enum {
  //! The MR has not properly set up the AMS, e.g. it wrote to the Entity Update or Entity
  //! Attribute characteristic without subscribing to GATT notifications for the Entity Update
  //! characteristic
  AMSErrorInvalidState = 0xA0,

  //! The command was improperly formatted.
  AMSErrorInvalidCommand = 0xA1,

  //! The corresponding attribute is empty.
  AMSErrorAbsentAttributes = 0xA2,
} AMSError;

//! Command IDs that can be sent to the AMS
typedef enum {
  AMSRemoteCommandIDPlay = 0,
  AMSRemoteCommandIDPause = 1,
  AMSRemoteCommandIDTogglePlayPause = 2,
  AMSRemoteCommandIDNextTrack = 3,
  AMSRemoteCommandIDPreviousTrack = 4,
  AMSRemoteCommandIDVolumeUp = 5,
  AMSRemoteCommandIDVolumeDown = 6,
  AMSRemoteCommandIDAdvanceRepeatMode = 7,
  AMSRemoteCommandIDAdvanceShuffleMode = 8,
  AMSRemoteCommandIDSkipForward = 9,
  AMSRemoteCommandIDSkipBackward = 10,
  AMSRemoteCommandIDLike = 11,
  AMSRemoteCommandIDDislike = 12,
  AMSRemoteCommandIDBookmark = 13,

  AMSRemoteCommandIDInvalid = 0xff,
} AMSRemoteCommandID;

//! Entity IDs to represent the entities on the AMS
typedef enum {
  AMSEntityIDPlayer = 0,
  AMSEntityIDQueue = 1,
  AMSEntityIDTrack = 2,

  NumAMSEntityID,
  AMSEntityIDInvalid = NumAMSEntityID,
} AMSEntityID;

typedef enum {
  AMSEntityUpdateFlagTruncated = (1 << 0),
  AMSEntityUpdateFlagReserved = ~((1 << 1) - 1),
} AMSEntityUpdateFlag;

typedef enum {
  //! A string containing the localized name of the app.
  AMSPlayerAttributeIDName = 0,

  //! A concatenation of three comma-separated values:
  //! - PlaybackState as string (see AMSPlaybackState)
  //! - PlaybackRate floating point as string
  //! - ElapsedTime floating point as string
  //! @see AMSPlaybackInfoIdx
  AMSPlayerAttributeIDPlaybackInfo = 1,

  //! Volume floating point as string, ranging from 0 (silent) to 1 (full volume)
  AMSPlayerAttributeIDVolume = 2,

  //! A string containing the bundle identifier of the app.
  //! @note Available since iOS 8.3
  AMSPlayerAttributeIDBundleIdentifier = 3,

  NumAMSPlayerAttributeID,
} AMSPlayerAttributeID;

typedef enum {
  AMSPlaybackInfoIdxState,
  AMSPlaybackInfoIdxRate,
  AMSPlaybackInfoIdxElapsedTime,
} AMSPlaybackInfoIdx;

typedef enum {
  AMSPlaybackStatePaused = 0,
  AMSPlaybackStatePlaying = 1,
  AMSPlaybackStateRewinding = 2,
  AMSPlaybackStateForwarding = 3,
} AMSPlaybackState;

typedef enum {
  //! A string containing the integer value of the queue index, zero-based.
  AMSQueueAttributeIDIndex = 0,

  //! A string containing the integer value of the total number of items in the queue.
  AMSQueueAttributeIDCount = 1,

  //! A string containing the integer value of the shuffle mode. See AMSShuffleMode.
  AMSQueueAttributeIDShuffleMode = 2,

  //! A string containing the integer value of the repeat mode. See AMSRepeatMode.
  AMSQueueAttributeIDRepeatMode = 3,

  NumAMSQueueAttributeID,
} AMSQueueAttributeID;

typedef enum {
  AMSShuffleModeOff = 0,
  AMSShuffleModeOne = 1,
  AMSShuffleModeAll = 2,
} AMSShuffleMode;

typedef enum {
  AMSRepeatModeOff = 0,
  AMSRepeatModeOne = 1,
  AMSRepeatModeAll = 2,
} AMSRepeatMode;

typedef enum {
  //! A string containing the name of the artist.
  AMSTrackAttributeIDArtist = 0,

  //! A string containing the name of the album.
  AMSTrackAttributeIDAlbum = 1,

  //! A string containing the title of the track.
  AMSTrackAttributeIDTitle = 2,

  //! A string containing the floating point value of the total duration of the track in seconds.
  AMSTrackAttributeIDDuration = 3,

  NumAMSTrackAttributeID,
} AMSTrackAttributeID;

#define AMS_MAX_NUM_ATTRIBUTE_ID (MAX(MAX((int)NumAMSTrackAttributeID, \
                                      (int)NumAMSQueueAttributeID), (int)NumAMSPlayerAttributeID))

////////////////////////////////////////////////////////////////////////////////////////////////////
// Packet Formats

//! Written (with Response) to the Remote Command characteristic,
//! to execute the specified command on the AMS.
typedef struct PACKED {
  AMSRemoteCommandID command_id:8;
} AMSRemoteCommand;

//! Written (without Response) to the Entity Update characteristic,
//! to indicate that the client is interested in receiving updates for the specified entity
//! and attributes.
typedef struct PACKED {
  AMSEntityID entity_id:8;

  //! Array of Attribute IDs for which the client wants to receive updates.
  //! Can be of type AMSPlayerAttributeID, AMSQueueAttributeID, AMSTrackAttributeID, depending on
  //! the value of `entity_id`.
  uint8_t attributes[];
} AMSEntityUpdateCommand;

//! Notification from the Entity Update characteristic,
//! sent to notify the client of an updated attribute value.
typedef struct PACKED {
  AMSEntityID entity_id:8;

  //! The Attribute ID of the updated value.
  //! Can be of type AMSPlayerAttributeID, AMSQueueAttributeID, AMSTrackAttributeID, depending on
  //! the value of `entity_id`.
  uint8_t attribute_id;

  AMSEntityUpdateFlag flags:8;

  //! The updated value.
  //! @note The string is never zero-terminated, so cannot be used as a C-string, as-is.
  char value_str[];
} AMSEntityUpdateNotification;
