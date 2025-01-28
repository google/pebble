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

#include "voice.h"

#include "board/board.h"
#include "drivers/mic.h"
#include "kernel/events.h"
#include "kernel/pbl_malloc.h"
#include "os/mutex.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_manager.h"
#include "services/common/new_timer/new_timer.h"
#include "services/normal/audio_endpoint.h"
#include "services/normal/voice/transcription.h"
#include "services/normal/voice_endpoint.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "system/passert.h"
#include "system/profiler.h"
#include "util/uuid.h"

#include <string.h>

#define SPEEX_BITSTREAM_VERSION (4)

#define TIMEOUT_SESSION_SETUP (8000)
#define TIMEOUT_SESSION_RESULT  (15000)

#define VOICE_LOG(fmt, args...)   PBL_LOG_D(LOG_DOMAIN_VOICE, LOG_LEVEL_DEBUG, fmt, ## args)

typedef enum {
  SessionState_Idle = 0,
  SessionState_StartSession,
  SessionState_VoiceEndpointSetupReceived,
  SessionState_AudioEndpointSetupReceived,
  SessionState_Recording,
  SessionState_WaitForSessionResult,
} SessionState;

static SessionState s_state = SessionState_Idle;

static PebbleMutex* s_lock = NULL;

// Handle requests from apps
static bool s_from_app;
static Uuid s_app_uuid;

static AudioEndpointSessionId s_session_id = AUDIO_ENDPOINT_SESSION_INVALID_ID;
static TimerID s_timeout = TIMER_INVALID_ID;

static void prv_send_event(VoiceEventType event_type, VoiceStatus status,
                           PebbleVoiceServiceEventData *data);
static void prv_session_result_timeout(void * data);

#if defined(VOICE_DEBUG)
// printf implemented here because the ADT Speex debug library calls printf for logging
int printf(const char *template, ...) {
  va_list args;
  va_start(args, template);
  char s[100];
  vsnprintf(s, sizeof(s), template, args);
  VOICE_LOG("%s", s);

  va_end(args);
  return 0;
}
#endif

static void prv_teardown_session(void) {
#if !defined(TARGET_QEMU)
  // TODO: replace stub
#endif
}

static void prv_stop_recording(void) {
#if !defined(TARGET_QEMU)
  // TODO: replace stub
#endif

  audio_endpoint_stop_transfer(s_session_id);
  PBL_LOG(LOG_LEVEL_INFO, "Stop recording audio");
  prv_teardown_session();
}

static void prv_cancel_recording(void) {
#if !defined(TARGET_QEMU)
  // TODO: reenable
  // mic_stop(MIC);
#endif

  audio_endpoint_cancel_transfer(s_session_id);
  PBL_LOG(LOG_LEVEL_INFO, "Cancel audio recording");
  prv_teardown_session();
}

static void prv_reset(void) {
  s_state = SessionState_Idle;
  s_session_id = AUDIO_ENDPOINT_SESSION_INVALID_ID;
}

static void prv_cancel_session(void) {
  prv_cancel_recording();
  prv_reset();
}

static void prv_start_result_timeout(void) {
  new_timer_start(s_timeout, TIMEOUT_SESSION_RESULT, prv_session_result_timeout, NULL, 0);
}

static void prv_audio_transfer_stopped_handler(AudioEndpointSessionId session_id) {
  if (s_session_id != session_id) {
    PBL_LOG(LOG_LEVEL_WARNING, "Received audio transfer message when no session was in progress ("
            "%d)", session_id);
    return;
  }

  if (s_state != SessionState_Recording) {
    PBL_LOG(LOG_LEVEL_WARNING, "Received stop message from phone after audio session "
        "stopped/cancelled");
    return;
  }

  // TODO: Handle this better: there is no feedback to the UI that we've stopped recording
  s_state = SessionState_WaitForSessionResult;
  prv_stop_recording();
  prv_start_result_timeout();
}

static void prv_start_recording(void) {
#if !defined(TARGET_QEMU)
  // TODO: reenable
  // PBL_ASSERTN(mic_start(MIC, &prv_audio_data_handler, NULL, s_frame_buffer, s_frame_size));
#endif

  PBL_LOG(LOG_LEVEL_INFO, "Recording");
}

static void prv_send_event(VoiceEventType event_type, VoiceStatus status,
                           PebbleVoiceServiceEventData *data) {
  PebbleEvent event = {
    .type = PEBBLE_VOICE_SERVICE_EVENT,
    .voice_service = {
      .type = event_type,
      .status = status,
      .data = data,
    }
  };
  event_put(&event);
}

//! Expects s_lock is held by caller
static void prv_handle_subsystem_started(SessionState transition_to_state) {
  PBL_ASSERTN(transition_to_state == SessionState_VoiceEndpointSetupReceived ||
              transition_to_state == SessionState_AudioEndpointSetupReceived);

  if (s_state == SessionState_Idle) { // we error'ed out
    return;
  }

  if (s_state == SessionState_StartSession) {
    // we are still waiting for one of the subsystems to be ready
    s_state = transition_to_state;
  } else {
    PBL_ASSERTN((s_state == SessionState_VoiceEndpointSetupReceived ||
                 s_state == SessionState_AudioEndpointSetupReceived) &&
                (transition_to_state != s_state));
    s_state = SessionState_Recording;

    new_timer_stop(s_timeout);

    // Indicate to the UI that we have started recording
    PBL_LOG(LOG_LEVEL_INFO, "Session setup successfully");
    prv_send_event(VoiceEventTypeSessionSetup, VoiceStatusSuccess, NULL);

    prv_start_recording();
  }
}

static void prv_audio_transfer_setup_complete_handler(AudioEndpointSessionId session_id) {
  if (s_session_id != session_id) {
    PBL_LOG(LOG_LEVEL_WARNING, "Received audio transfer message when no session was in progress ("
            "%d)", session_id);
    return;
  }

  mutex_lock(s_lock);
  prv_handle_subsystem_started(SessionState_AudioEndpointSetupReceived);
  mutex_unlock(s_lock);
}

static void prv_session_result_timeout(void * data) {
  mutex_lock(s_lock);

  PBL_ASSERTN(s_state == SessionState_WaitForSessionResult);

  prv_reset();
  PBL_LOG(LOG_LEVEL_WARNING, "Timeout waiting for session result");

  prv_send_event(VoiceEventTypeSessionResult, VoiceStatusTimeout, NULL);

  mutex_unlock(s_lock);
}

static void prv_session_setup_timeout(void * data) {
  mutex_lock(s_lock);
  PBL_ASSERTN(s_state == SessionState_StartSession ||
              s_state == SessionState_VoiceEndpointSetupReceived ||
              s_state == SessionState_AudioEndpointSetupReceived);

  prv_cancel_session();
  PBL_LOG(LOG_LEVEL_WARNING, "Timeout waiting for session setup result ");

  prv_send_event(VoiceEventTypeSessionSetup, VoiceStatusTimeout, NULL);

  mutex_unlock(s_lock);
}

static VoiceStatus prv_get_status_from_result(VoiceEndpointResult result) {
  VoiceStatus status;
  switch (result) {
    case VoiceEndpointResultFailServiceUnavailable:
      status = VoiceStatusErrorConnectivity;
      break;
    case VoiceEndpointResultFailDisabled:
      status = VoiceStatusErrorDisabled;
      break;
    case VoiceEndpointResultFailInvalidRecognizerResponse:
      status = VoiceStatusRecognizerResponseError;
      break;
    case VoiceEndpointResultFailTimeout:
    case VoiceEndpointResultFailRecognizerError:
    case VoiceEndpointResultFailInvalidMessage:
    default:
      status = VoiceStatusErrorGeneric;
      break;
  }
  return status;
}

void voice_init(void) {
  s_lock = mutex_create();
}

// This will kick off a dictation session. After the setup session message is sent via the
// voice control endpoint, we wait for a session ready response via the
// voice_handle_session_setup_result call or a session setup timeout occurs (timer callback
// prv_session_setup_timeout)
VoiceSessionId voice_start_dictation(VoiceEndpointSessionType session_type) {
  mutex_lock(s_lock);

  if (s_state != SessionState_Idle) {
    mutex_unlock(s_lock);
    return VOICE_SESSION_ID_INVALID;
  }
  s_state = SessionState_StartSession;

  // check if we're being started from an app so we know to send the UUID when setting up a session
  s_from_app = ((pebble_task_get_current() == PebbleTask_App) &&
      !app_install_id_from_system(app_manager_get_current_app_id()));
  if (s_from_app) {
    s_app_uuid = app_manager_get_current_app_md()->uuid;
    char uuid_str[UUID_STRING_BUFFER_LENGTH];
    uuid_to_string(&s_app_uuid, uuid_str);
    PBL_LOG(LOG_LEVEL_INFO, "Starting app-initiated voice dictation session for app %s", uuid_str);
  }

#if !defined(TARGET_QEMU)
  // TODO: replace stub
#endif

  // TODO: replace fake values
  AudioTransferInfoSpeex transfer_info = (AudioTransferInfoSpeex) {
    .sample_rate = 0,
    .bit_rate = 0,
    .frame_size = 0,
    .bitstream_version = 0,
  };

  s_session_id = audio_endpoint_setup_transfer(prv_audio_transfer_setup_complete_handler,
                                               prv_audio_transfer_stopped_handler);
  PBL_ASSERTN(s_session_id != AUDIO_ENDPOINT_SESSION_INVALID_ID);


  PBL_LOG(LOG_LEVEL_INFO, "Send session setup message. Session type: %d", session_type);
  voice_endpoint_setup_session(session_type, s_session_id, &transfer_info,
      s_from_app ? &s_app_uuid : NULL);

  if (s_timeout == TIMER_INVALID_ID) {
    s_timeout = new_timer_create();
  }
  new_timer_start(s_timeout, TIMEOUT_SESSION_SETUP, prv_session_setup_timeout, NULL, 0);

  mutex_unlock(s_lock);
  return s_session_id;
}

// Calling this will end the recording, disable the mic and stop the audio transfer session. We
// expect voice_handle_dictation_result to be called next with a dictation response
void voice_stop_dictation(VoiceSessionId session_id) {
  mutex_lock(s_lock);
  if ((s_state == SessionState_Idle) ||
      (session_id != s_session_id) ||
      (session_id == VOICE_SESSION_ID_INVALID)) {
    goto unlock;
  }

  if (s_state != SessionState_Recording) {
    mutex_unlock(s_lock);
    voice_cancel_dictation(session_id);
    return;
  }

  s_state = SessionState_WaitForSessionResult;
  prv_stop_recording();
  prv_start_result_timeout();

unlock:
  mutex_unlock(s_lock);
}

void voice_cancel_dictation(VoiceSessionId session_id) {
  mutex_lock(s_lock);
  if ((session_id != s_session_id) ||
      (session_id == VOICE_SESSION_ID_INVALID)) {
    goto unlock;
  }

  if (s_state != SessionState_Idle) {
    new_timer_stop(s_timeout);
    if (s_state == SessionState_StartSession ||
        s_state == SessionState_VoiceEndpointSetupReceived ||
        s_state == SessionState_AudioEndpointSetupReceived) {
      prv_cancel_recording();
    } else if (s_state == SessionState_Recording) {
      prv_stop_recording();
    }
  }
  prv_reset();

unlock:
  mutex_unlock(s_lock);
}

// This will trigger an event to be sent to the main task indicating success or failure to set up
// a session. If the session setup result was success, the microphone will be enabled and we'll
// start sending Speex encoded data via the audio endpoint to the phone. voice_stop_dictation will
// end the recording
void voice_handle_session_setup_result(VoiceEndpointResult result,
    VoiceEndpointSessionType session_type, bool app_initiated) {
  mutex_lock(s_lock);

  if (s_state == SessionState_Idle) {
    goto unlock;
  }

  bool has_error = true;

  if (s_state != SessionState_StartSession &&
      s_state != SessionState_AudioEndpointSetupReceived) {
    PBL_LOG(LOG_LEVEL_WARNING, "Session setup result received when not expected, state=%d",
            (int)s_state);
    prv_cancel_session();
    VoiceEventType event_type = (s_state <= SessionState_StartSession) ?
        VoiceEventTypeSessionSetup : VoiceEventTypeSessionResult;
    prv_send_event(event_type, VoiceStatusErrorGeneric, NULL);
    goto done;
  }

  if (session_type >= VoiceEndpointSessionTypeCount) {
    PBL_LOG(LOG_LEVEL_WARNING, "Session setup result for invalid session type received");
    goto done;
  }

  if (result != VoiceEndpointResultSuccess) {
    prv_cancel_session();
    VoiceStatus status = prv_get_status_from_result(result);
    PBL_LOG(LOG_LEVEL_WARNING, "Error occurred setting up session: %d", result);
    prv_send_event(VoiceEventTypeSessionSetup, status, NULL);
    goto done;
  }

  if (app_initiated != s_from_app) {
    prv_cancel_session();
    if (app_initiated) {
      PBL_LOG(LOG_LEVEL_WARNING, "Received session setup result for app initiated session when it "
              "was not expected");
    } else {
      PBL_LOG(LOG_LEVEL_WARNING, "Received session setup result for non-app session when an app "
              "session result was expected");
    }
    prv_send_event(VoiceEventTypeSessionSetup, VoiceStatusErrorGeneric, NULL);
    goto done;
  }

  has_error = false;

done:
  if (has_error) {
    new_timer_stop(s_timeout);
  } else {
    prv_handle_subsystem_started(SessionState_VoiceEndpointSetupReceived);
  }
unlock:
  mutex_unlock(s_lock);
}

static bool prv_get_string_size_cb(const TranscriptionWord *word, void *data) {
  size_t *size = data;
  *size += word->length + sizeof(char); // add 1 for space or null terminator
  return true;
}

static bool prv_build_string_cb(const TranscriptionWord *word, void *data) {
  char *sentence = data;

  // if the current word is a punctuation mark strip out backspace (phone app inserts backspace
  // before punctuation mark) and do not insert a space before the word
  if (word->data[0] == '\x08') {
    strncat(sentence, (char *) &word->data[1], word->length - 1);
  } else {
    // if this is not the beginning of the string, insert a space before the word
    if (strlen(sentence) != 0) {
      strcat(sentence, " ");
    }
    strncat(sentence, (char *) word->data, word->length);
  }

  return true;
}

static bool prv_handle_dictation_nlp_result_common(VoiceEndpointResult result,
                                                   AudioEndpointSessionId session_id,
                                                   bool app_initiated, Uuid *app_uuid) {
  if (s_state == SessionState_Idle) {
    return false;
  }

  // stop timer before changing state variable
  new_timer_stop(s_timeout);

  if (s_state != SessionState_WaitForSessionResult) {
    // This handles erroneous replies from the phone app (sometimes the phone app sends a session
    // result immediately after we start streaming
    PBL_LOG(LOG_LEVEL_WARNING, "Session result when not expected (result: %d, "
        "session_id: %d)", result, session_id);
    if (s_state == SessionState_Recording) {
      prv_stop_recording();
    } else {
      prv_cancel_recording();
    }
    VoiceEventType event_type = (s_state <= SessionState_StartSession) ?
        VoiceEventTypeSessionSetup : VoiceEventTypeSessionResult;
    prv_send_event(event_type, VoiceStatusErrorGeneric, NULL);
    return false;
  }

  if (s_session_id != session_id) {
    PBL_LOG(LOG_LEVEL_WARNING, "Received session result for wrong session (Expected: "
        "%"PRIu16"; Received: %"PRIu16, s_session_id, session_id);
    prv_send_event(VoiceEventTypeSessionResult, VoiceStatusErrorGeneric, NULL);
    return false;
  }

  if (result != VoiceEndpointResultSuccess) {
    VoiceStatus status = prv_get_status_from_result(result);
    PBL_LOG(LOG_LEVEL_WARNING, "Error occurred processing result: %d", result);
    prv_send_event(VoiceEventTypeSessionResult, status, NULL);
    return false;
  }

  // Make sure that if this is an app initiated session, we're expecting a response for an app
  // initiated session and that if this is an app initiated session, the app UUID matches the
  // expected UUID
  if ((app_initiated != s_from_app) || (s_from_app && !uuid_equal(&s_app_uuid, app_uuid))) {
    if (app_initiated) {
      PBL_LOG(LOG_LEVEL_WARNING, "Received session result for app initiated session when a "
              "non-app session result was expected");
    } else {
      PBL_LOG(LOG_LEVEL_WARNING, "Received session result for non-app session when an app "
              "session result was expected");
    }
    prv_send_event(VoiceEventTypeSessionResult, VoiceStatusErrorGeneric, NULL);
    return false;
  }

  return true;
}

// receiving this ends the session, sending an event to the main task with the result
void voice_handle_dictation_result(VoiceEndpointResult result, AudioEndpointSessionId session_id,
                                   Transcription *transcription, bool app_initiated,
                                   Uuid *app_uuid) {
  mutex_lock(s_lock);

  if (!prv_handle_dictation_nlp_result_common(result, session_id, app_initiated, app_uuid)) {
    goto unlock;
  }

  // Calculate size of string
  size_t sentence_size = 0;
  transcription_iterate_words(transcription->sentences[0].words,
      transcription->sentences[0].word_count, prv_get_string_size_cb, &sentence_size);

  const size_t event_size = sizeof(PebbleVoiceServiceEventData) + sentence_size;
  PebbleVoiceServiceEventData *event_data = kernel_zalloc_check(event_size);

  // TODO: Final UI will probably demand a more sophisticated input, but this service will be
  // updated to support additional features when the final UI is implemented
  // Build string by concatenating each word in the first sentence
  transcription_iterate_words(transcription->sentences[0].words,
      transcription->sentences[0].word_count, prv_build_string_cb, event_data->sentence);

  if (app_initiated) {
    char uuid_str[UUID_STRING_BUFFER_LENGTH];
    uuid_to_string(app_uuid, uuid_str);
    PBL_LOG(LOG_LEVEL_INFO, "Transcription received (%"PRIu32" B) for app %s",
        (uint32_t)sentence_size, uuid_str);
  } else {
    PBL_LOG(LOG_LEVEL_INFO, "Transcription received (%"PRIu32" B)", (uint32_t)sentence_size);
  }

  prv_send_event(VoiceEventTypeSessionResult, VoiceStatusSuccess, event_data);

unlock:
  prv_reset();
  mutex_unlock(s_lock);
}

// receiving this ends the session, sending an event to the main task with the result
void voice_handle_nlp_result(VoiceEndpointResult result, AudioEndpointSessionId session_id,
                             char *reminder, time_t timestamp) {
  mutex_lock(s_lock);

  const bool app_initiated = false;
  Uuid *app_uuid = NULL;
  if (!prv_handle_dictation_nlp_result_common(result, session_id, app_initiated, app_uuid)) {
    goto unlock;
  }

  const size_t sentence_size = strlen(reminder) + 1;
  const size_t event_size = sizeof(PebbleVoiceServiceEventData) + sentence_size;
  PebbleVoiceServiceEventData *event_data = kernel_zalloc_check(event_size);
  *event_data = (PebbleVoiceServiceEventData) {
    .timestamp = timestamp,
  };
  strncpy(event_data->sentence, reminder, sentence_size);

  prv_send_event(VoiceEventTypeSessionResult, VoiceStatusSuccess, event_data);

unlock:
  prv_reset();
  mutex_unlock(s_lock);
}

DEFINE_SYSCALL(VoiceSessionId, sys_voice_start_dictation, VoiceEndpointSessionType session_type) {
  if (session_type >= VoiceEndpointSessionTypeCount) {
    return AUDIO_ENDPOINT_SESSION_INVALID_ID;
  }
  return voice_start_dictation(session_type);
}

DEFINE_SYSCALL(void, sys_voice_stop_dictation, VoiceSessionId session_id) {
  voice_stop_dictation(session_id);
}

DEFINE_SYSCALL(void, sys_voice_cancel_dictation, VoiceSessionId session_id) {
  voice_cancel_dictation(session_id);
}

void voice_kill_app_session(PebbleTask task) {
  if (task != PebbleTask_App) {
    return;
  }
  mutex_lock(s_lock);
  if (s_from_app && (s_session_id != AUDIO_ENDPOINT_SESSION_INVALID_ID)) {
    prv_cancel_session();
  }
  mutex_unlock(s_lock);
}
