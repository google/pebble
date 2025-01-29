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

#include "clar.h"

#include "services/normal/music.h"
#include "services/normal/music_endpoint.h"
#include "services/normal/music_internal.h"

#include "services/common/comm_session/session.h"
#include "services/common/comm_session/session_remote_os.h"

#include "kernel/events.h"

#include "util/size.h"

// Stubs & Fakes
///////////////////////////////////////////////////////////

#include "fake_events.h"
#include "fake_rtc.h"
#include "fake_session.h"
#include "fake_system_task.h"

#include "stubs_app_manager.h"
#include "stubs_app_install_manager.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_bt_lock.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_serial.h"
#include "stubs_tick.h"

extern void music_protocol_msg_callback(CommSession *session, const uint8_t* msg, size_t length);

// Helpers
///////////////////////////////////////////////////////////

static void prv_receive_app_info_event(bool is_android) {
  const PebbleRemoteAppInfoEvent app_info_event = (const PebbleRemoteAppInfoEvent) {
    .os = is_android ? RemoteOSAndroid : RemoteOSiOS,
  };
  music_endpoint_handle_mobile_app_info_event(&app_info_event);
}

static void prv_receive_app_event(bool is_open) {
  const PebbleCommSessionEvent app_event = (const PebbleCommSessionEvent) {
    .is_open = is_open,
    .is_system = true
  };
  music_endpoint_handle_mobile_app_event(&app_event);
}

static void prv_receive_pp_data(const uint8_t *data, uint16_t length) {
  fake_event_clear_last();
  music_protocol_msg_callback(NULL, data, length);
}

static void prv_receive_and_assert_now_playing(bool expect_is_handled) {
  uint8_t msg[] = { 0x10, 3, 'o', 'n', 'e', 3, 't', 'w', 'o', 5, 't', 'h', 'r', 'e', 'e', 0xAA,
                    0x00, 0x00, 0x00, 0xAA, 0x00, 0xAA, 0x00 };
  prv_receive_pp_data(msg, sizeof(msg));

  PebbleEvent e = fake_event_get_last();
  if (expect_is_handled) {
    cl_assert_equal_i(e.type, PEBBLE_MEDIA_EVENT);
    cl_assert_equal_i(e.media.type, PebbleMediaEventTypeTrackPosChanged);

    char artist[MUSIC_BUFFER_LENGTH];
    char album[MUSIC_BUFFER_LENGTH];
    char title[MUSIC_BUFFER_LENGTH];

    music_get_now_playing(title, artist, album);

    cl_assert_equal_s(artist, "one");
    cl_assert_equal_s(album, "two");
    cl_assert_equal_s(title, "three");

    uint32_t track_position, track_duration;
    music_get_pos(&track_position, &track_duration);
    cl_assert_equal_i(track_duration, 0xAA);
  } else {
    cl_assert_equal_i(e.type, PEBBLE_NULL_EVENT);
  }
}

static void prv_receive_and_assert_play_state(bool expect_is_handled) {
  uint8_t msg[] = { 0x11, 0x01, 0xAA, 0x00, 0x00, 0x00, 0xAA, 0x00, 0x00, 0x00, 0x01, 0x01 };
  prv_receive_pp_data(msg, sizeof(msg));

  PebbleEvent e = fake_event_get_last();
  if (expect_is_handled) {
    cl_assert_equal_i(e.type, PEBBLE_MEDIA_EVENT);
    cl_assert_equal_i(e.media.type, PebbleMediaEventTypeTrackPosChanged);

    cl_assert_equal_i(music_get_playback_state(), 0x01);
    uint32_t track_position, track_duration;
    music_get_pos(&track_position, &track_duration);
    cl_assert_equal_i(track_position, 0xAA);
    cl_assert_equal_i(music_get_playback_rate_percent(), 0xAA);

  } else {
    cl_assert_equal_i(e.type, PEBBLE_NULL_EVENT);
  }
}

static void prv_receive_and_assert_volume_info(bool expect_is_handled) {
  uint8_t msg[] = { 0x12, 0x33 };
  prv_receive_pp_data(msg, sizeof(msg));

  PebbleEvent e = fake_event_get_last();
  if (expect_is_handled) {
    cl_assert_equal_i(e.type, PEBBLE_MEDIA_EVENT);
    cl_assert_equal_i(e.media.type, PebbleMediaEventTypeVolumeChanged);

    cl_assert_equal_i(music_get_volume_percent(), 0x33);

  } else {
    cl_assert_equal_i(e.type, PEBBLE_NULL_EVENT);
  }
}

static void prv_receive_and_assert_player_info(bool expect_is_handled) {
  uint8_t msg[] = { 0x13, 17, 'c', 'o', 'm', '.', 's', 'p', 'o', 't', 'i', 'f', 'y', '.', 'm',
                    'u', 's', 'i', 'c', 7, 'S', 'p', 'o', 't', 'i', 'f', 'y' };
  prv_receive_pp_data(msg, sizeof(msg));

  PebbleEvent e = fake_event_get_last();
  if (expect_is_handled) {
    cl_assert_equal_i(e.type, PEBBLE_MEDIA_EVENT);
    cl_assert_equal_i(e.media.type, PebbleMediaEventTypeNowPlayingChanged);

    char player_name[MUSIC_BUFFER_LENGTH];
    music_get_player_name(player_name);
    cl_assert_equal_s(player_name, "Spotify");

  } else {
    cl_assert_equal_i(e.type, PEBBLE_NULL_EVENT);
  }
}

static void prv_receive_and_assert_all(bool expect_is_handled) {
  prv_receive_and_assert_now_playing(expect_is_handled);
  prv_receive_and_assert_play_state(expect_is_handled);
  prv_receive_and_assert_volume_info(expect_is_handled);
  prv_receive_and_assert_player_info(expect_is_handled);
}


static const MusicServerImplementation s_dummy_server_implementation = {};
static void prv_set_dummy_server_connected(bool connected) {
  music_set_connected_server(&s_dummy_server_implementation,
                             connected /* connected */);
}

static void prv_assert_no_data_sent_cb(uint16_t endpoint_id,
                                       const uint8_t* data, unsigned int data_length) {
  cl_assert(false);
}

static bool s_now_playing_requested;
static void prv_assert_now_playing_requested_cb(uint16_t endpoint_id,
                                                const uint8_t* data, unsigned int data_length) {
  cl_assert_equal_i(data_length, 1);
  cl_assert_equal_i(data[0], 0x08);  // MusicEndpointCmdIDGetAllInfo
  s_now_playing_requested = true;
}

static bool s_next_track_command_sent;
static void prv_assert_next_track_command_sent_cb(uint16_t endpoint_id,
                                                  const uint8_t* data, unsigned int data_length) {
  cl_assert_equal_i(data_length, 1);
  cl_assert_equal_i(data[0], 0x04);  // MusicEndpointCmdIDNextTrack
  s_next_track_command_sent = true;
}

static bool s_is_playback_cmd_sent;
static uint8_t s_playback_cmd_sent;
static void prv_assert_playback_command_sent_cb(uint16_t endpoint_id,
                                                const uint8_t* data, unsigned int data_length) {
  cl_assert_equal_i(data_length, 1);
  if (!s_is_playback_cmd_sent) {
    s_playback_cmd_sent = data[0];
    s_is_playback_cmd_sent = true;
  } else {
    // Playback command is always followed by:
    cl_assert_equal_i(data[0], 0x08);  // MusicEndpointCmdIDGetAllInfo
  }
}

// Tests
///////////////////////////////////////////////////////////

Transport *s_transport;

void test_music_endpoint__initialize(void) {
  s_now_playing_requested = false;
  s_next_track_command_sent = false;
  s_is_playback_cmd_sent = false;
  s_playback_cmd_sent = ~0;
  fake_event_init();
  fake_rtc_init(0, 0);
  fake_comm_session_init();
  music_init();

  s_transport = fake_transport_create(TransportDestinationSystem, NULL, NULL);
  fake_transport_set_connected(s_transport, true /* connected */);

  // Simulate connecting Pebble mobile app:
  prv_receive_app_event(true /* is_open */);
}

void test_music_endpoint__cleanup(void) {
  fake_comm_session_process_send_next();

  // Simulate disconnecting Pebble mobile app:
  prv_receive_app_event(false /* is_open */);

  fake_comm_session_cleanup();
}

void test_music_endpoint__ignore_now_playing_while_not_connected(void) {
  // Don't connect app, but receive Now Playing info. Should be ignored:
  prv_receive_and_assert_all(false /* expect_is_handled */);
}

void test_music_endpoint__ignore_now_playing_while_other_server_connected(void) {
  // Another music server connects:
  prv_set_dummy_server_connected(true /* connected */);

  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);

  // Receive Now Playing info. Should be ignored, because other server is connected:
 prv_receive_and_assert_all(false /* expect_is_handled*/);

  // Disconnect dummy server, to clean up after ourselves:
  prv_set_dummy_server_connected(false /* connected */);
}

void test_music_endpoint__ignore_now_playing_from_ios_app(void) {
  // iOS app connects:
  prv_receive_app_info_event(false /* is_android */);
  // iOS app is not supposed to use this endpoint:
  prv_receive_and_assert_all(false /* expect_is_handled*/);
}

void test_music_endpoint__request_now_playing_upon_connect(void) {
  fake_transport_set_sent_cb(s_transport, &prv_assert_now_playing_requested_cb);

  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);

  fake_comm_session_process_send_next();
  cl_assert_equal_b(s_now_playing_requested, true);
}

void test_music_endpoint__receive_now_playing_while_connected(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  prv_receive_and_assert_all(true /* expect_is_handled*/);
}

void test_music_endpoint__ignore_unknown_message(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  uint8_t unknown_msg[] = { 0xff };
  prv_receive_pp_data(unknown_msg, sizeof(unknown_msg));
  PebbleEvent e = fake_event_get_last();
  cl_assert_equal_i(e.type, PEBBLE_NULL_EVENT);
}

void test_music_endpoint__receive_zero_length_now_playing(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  prv_receive_and_assert_all(true /* expect_is_handled*/);
  cl_assert_equal_b(music_has_now_playing(), true);

  uint8_t zero_length_now_playing[] = { 0x10, 0, 0, 0 };
  prv_receive_pp_data(zero_length_now_playing, sizeof(zero_length_now_playing));
  cl_assert_equal_b(music_has_now_playing(), false);
}

void test_music_endpoint__ignore_malformed_messages(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  const uint8_t malformed_artist[] = {
    0x10, 14, 'o', 'n', 'e', 3, 't', 'w', 'o', 5, 't', 'h', 'r', 'e', 'e'
  };
  const uint8_t malformed_album[] = {
    0x10, 3, 'o', 'n', 'e', 10, 't', 'w', 'o', 5, 't', 'h', 'r', 'e', 'e'
  };
  const uint8_t malformed_title[] = {
    0x10, 3, 'o', 'n', 'e', 3, 't', 'w', 'o', 6, 't', 'h', 'r', 'e', 'e'
  };
  const uint8_t malformed_player[] = {
    0x13, 17, 'c', 'o', 'm', '.', 's', 'p', 'o', 't', 'i', 'f', 'y', '.', 'm', 'u', 's', 'i', 'c',
    9, 'S', 'p', 'o', 't', 'i', 'f', 'y'
  };
  struct {
    const uint8_t *data;
    uint16_t length;
  } test_vectors[] = {
    { malformed_artist, sizeof(malformed_artist) },
    { malformed_album, sizeof(malformed_album) },
    { malformed_title, sizeof(malformed_title) },
    { malformed_player, sizeof(malformed_player) }
  };
  for (int i = 0; i < ARRAY_LENGTH(test_vectors); ++i) {
    prv_receive_pp_data(test_vectors[i].data, test_vectors[i].length);
    PebbleEvent e = fake_event_get_last();
    cl_assert_equal_i(e.type, PEBBLE_NULL_EVENT);
  }
}

void test_music_endpoint__supported_capabilities(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  // music_is_progress_reporting_supported() relies on a valid track duration
  prv_receive_and_assert_all(true /* expect_is_handled*/);

  cl_assert_equal_b(music_is_playback_state_reporting_supported(), true);
  cl_assert_equal_b(music_is_progress_reporting_supported(), true);
  cl_assert_equal_b(music_is_volume_reporting_supported(), true);
  cl_assert_equal_b(music_needs_user_to_start_playback_on_phone(), false);
  for (MusicCommand cmd = 0; cmd < NumMusicCommand; ++cmd) {
    bool expect_supported = true;
    if (cmd == MusicCommandAdvanceRepeatMode ||
        cmd == MusicCommandAdvanceShuffleMode ||
        cmd == MusicCommandSkipForward ||
        cmd == MusicCommandSkipBackward ||
        cmd == MusicCommandLike ||
        cmd == MusicCommandDislike ||
        cmd == MusicCommandBookmark) {
      expect_supported = false;
    }
    cl_assert_equal_b(music_is_command_supported(cmd), expect_supported);
  }
}

void test_music_endpoint__reduced_latency(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);

  cl_assert_equal_b(fake_comm_session_is_latency_reduced(), false);
  music_request_reduced_latency(true);
  cl_assert_equal_b(fake_comm_session_is_latency_reduced(), true);
  music_request_reduced_latency(false);
  cl_assert_equal_b(fake_comm_session_is_latency_reduced(), false);
}

void test_music_endpoint__low_latency_for_period(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);

  cl_assert_equal_i(fake_comm_session_get_responsiveness_max_period(), 0);
  music_request_low_latency_for_period(2000);
  cl_assert_equal_i(fake_comm_session_get_responsiveness_max_period(), 2);
}

void test_music_endpoint__send_unsupported_command(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  fake_comm_session_process_send_next();  // send out any pending data

  // Attempting to send an unsupported command should not result in any data getting sent out:
  fake_transport_set_sent_cb(s_transport, prv_assert_no_data_sent_cb);
  music_command_send(MusicCommandAdvanceRepeatMode);
  fake_comm_session_process_send_next();
}

void test_music_endpoint__send_next_track_command(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  fake_comm_session_process_send_next();  // send out any pending data

  fake_transport_set_sent_cb(s_transport, prv_assert_next_track_command_sent_cb);
  music_command_send(MusicCommandNextTrack);
  fake_comm_session_process_send_next();

  cl_assert_equal_b(s_next_track_command_sent, true);
}

void test_music_endpoint__send_playback_command(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);
  fake_comm_session_process_send_next();  // send out any pending data

  MusicCommand cmds[] = {
    MusicCommandTogglePlayPause,
    MusicCommandPause,
    MusicCommandPlay,
  };

  uint8_t opcodes[] = {
    0x1,
    0x2,
    0x3,
  };

  fake_transport_set_sent_cb(s_transport, prv_assert_playback_command_sent_cb);
  for (int i = 0; i < ARRAY_LENGTH(cmds); ++i) {
    music_command_send(cmds[i]);
    fake_comm_session_process_send_next();
    cl_assert_equal_i(s_is_playback_cmd_sent, true);
    cl_assert_equal_i(s_playback_cmd_sent, opcodes[i]);
    s_is_playback_cmd_sent = false;
  }
}

void test_music_endpoint__player_name_not_available(void) {
  // Android app connects:
  prv_receive_app_info_event(true /* is_android */);

  cl_assert_equal_b(music_get_player_name(NULL), false);
}
