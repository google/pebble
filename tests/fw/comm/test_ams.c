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

#include "comm/ble/kernel_le_client/ams/ams.h"

#include "comm/bt_conn_mgr.h"

#include "services/normal/music.h"
#include "services/normal/music_internal.h"

#include "clar.h"

// Stubs & Fakes
///////////////////////////////////////////////////////////

#include "fake_events.h"
#include "fake_gatt_client_operations.h"
#include "fake_gatt_client_subscriptions.h"
#include "fake_pebble_tasks.h"
#include "fake_rtc.h"

#include "stubs_app_install_manager.h"
#include "stubs_app_manager.h"
#include "stubs_bt_lock.h"
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_serial.h"
#include "stubs_tick.h"

void analytics_event_ams(uint8_t type, int32_t aux_info) {
}

struct {
  ResponseTimeState state;
  uint16_t max_period_secs;
} s_conn_mgr_states[NumBtConsumer];

void conn_mgr_set_ble_conn_response_time(GAPLEConnection *hdl, BtConsumer consumer,
                                         ResponseTimeState state, uint16_t max_period_secs) {
  s_conn_mgr_states[consumer].state = state;
  s_conn_mgr_states[consumer].max_period_secs = max_period_secs;
}

GAPLEConnection *gap_le_connection_by_device(const BTDeviceInternal *device) {
  return NULL;
}

BTDeviceInternal gatt_client_characteristic_get_device(BLECharacteristic characteristic_ref) {
  return (BTDeviceInternal) {
    .address.octets = {
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
    },
  };
}

static void (*s_launcher_task_callback)(void *data);
static void *s_launcher_task_callback_data;

void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  // Simple fake, can only handle one call
  cl_assert_equal_p(s_launcher_task_callback, NULL);

  s_launcher_task_callback = callback;
  s_launcher_task_callback_data = data;
}

// Tests: Discover AMS
///////////////////////////////////////////////////////////
#define NUM_AMS_INSTANCES 2
static BLECharacteristic s_characteristics[NUM_AMS_INSTANCES][NumAMSCharacteristic] = {
  // AMS instance one:
  [0] = {
    [AMSCharacteristicRemoteCommand] = 1,
    [AMSCharacteristicEntityUpdate] = 2,
    [AMSCharacteristicEntityAttribute] = 3,
  },
  // AMS instance two:
  [1] = {
    [AMSCharacteristicRemoteCommand] = 4,
    [AMSCharacteristicEntityUpdate] = 5,
    [AMSCharacteristicEntityAttribute] = 6,
  },
};

static const BLECharacteristic s_unknown_characteristic = 999;

static void prv_assert_can_handle_characteristics(uint32_t instance_idx, bool expect_can_handle) {
  for (AMSCharacteristic c = 0; c < NumAMSCharacteristic; ++c) {
    cl_assert_equal_b(ams_can_handle_characteristic(s_characteristics[instance_idx][c]),
                      expect_can_handle);
  }
}

static void prv_discover_ams(uint8_t num_instances) {
  cl_assert(num_instances <= 2);
  if (num_instances) {
    for (int i = 0; i < num_instances && i < NUM_AMS_INSTANCES; i++) {
      ams_handle_service_discovered(&s_characteristics[i][0]);
    }
  } else {
    ams_invalidate_all_references();
  }
}

void test_ams__cannot_handle_any_characteristic_after_destroy(void) {
  prv_discover_ams(1);
  ams_destroy();
  prv_assert_can_handle_characteristics(0, false /* expect_can_handle */);
}

void test_ams__cannot_handle_unknown_characteristic(void) {
  prv_discover_ams(1);
  cl_assert_equal_b(ams_can_handle_characteristic(s_unknown_characteristic), false);
}

void test_ams__discover_of_ams_should_subscribe_to_entity_update_characteristic(void) {
  // Pass in 2 instances, it should be able to cope with this
  prv_discover_ams(2 /* num_instances */);

  // Assert ams.c can handle the characteristic reference for the first instance:
  prv_assert_can_handle_characteristics(0, true /* expect_can_handle */);

  // Assert ams.c cannot handle the characteristic reference for the second instance:
  prv_assert_can_handle_characteristics(1, false /* expect_can_handle */);

  // The first instance is expected to be used.
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  fake_gatt_client_subscriptions_assert_subscribe(entity_update,
                                                  BLESubscriptionNotifications, GAPLEClientKernel);
}

void test_ams__connect_to_music_service_upon_subscribing_entity_update_characteristic(void) {
  prv_discover_ams(1 /* num_instances */);
  // Not connected yet (still need to subscribe):
  cl_assert_equal_s(music_get_connected_server_debug_name(), NULL);

  // Simulate successful subscription:
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_subscribe(entity_update, BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Music service should be connected now:
  cl_assert_equal_s(music_get_connected_server_debug_name(), ams_music_server_debug_name());

  // Rediscovery will disconnect music service (until resubscribed):
  ams_invalidate_all_references();
  prv_discover_ams(1 /* num_instances */);
  cl_assert_equal_s(music_get_connected_server_debug_name(), NULL);
}

void test_ams__dont_connect_music_service_if_subscribe_entity_update_characteristic_fails(void) {
  prv_discover_ams(1 /* num_instances */);

  // Simulate failed subscription:
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_subscribe(entity_update, BLESubscriptionNotifications, BLEGATTErrorUnlikelyError);

  // Not connected because subscription failed:
  cl_assert_equal_s(music_get_connected_server_debug_name(), NULL);
}

void test_ams__update_characteristics_ams_not_found(void) {
  // AMS Found:
  prv_discover_ams(1 /* num_instances */);
  // AMS Disappeared:
  prv_discover_ams(0 /* num_instances */);
  // Verify ams.c cannot handle the previous characteristics any more:
  prv_assert_can_handle_characteristics(0, false /* expect_can_handle */);
}

// Tests: Register for Entity Updates
///////////////////////////////////////////////////////////

static const uint8_t s_register_player_entity[] = {0x00,
                                                  // Apple bug #21283910
                                                  // See ams.c, prv_get_registration_cmd_for_entity
                                                  // 0x00,
                                                  0x01, 0x02};

static const uint8_t s_register_queue_entity[] = {0x01, 0x00, 0x01, 0x02, 0x03};

static const uint8_t s_register_track_entity[] = {0x02, 0x00, 0x01, 0x02, 0x03};

void test_ams__register_for_entity_updates(void) {
  prv_discover_ams(1 /* num_instances */);

  cl_assert_equal_b(ams_is_registered_for_all_entity_updates(), false);

  // Simulate successful subscription:
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_subscribe(entity_update, BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Expect to have written the command to register for the Player entity:
  fake_gatt_client_op_assert_write(entity_update, s_register_player_entity,
                                   sizeof(s_register_player_entity), GAPLEClientKernel,
                                   true /* is_response_required */);
  ams_handle_write_response(entity_update, BLEGATTErrorSuccess);

  cl_assert_equal_b(ams_is_registered_for_all_entity_updates(), false);

  // Expect to have written the command to register for the Queue entity:
  fake_gatt_client_op_assert_write(entity_update, s_register_queue_entity,
                                   sizeof(s_register_queue_entity), GAPLEClientKernel,
                                   true /* is_response_required */);
  ams_handle_write_response(entity_update, BLEGATTErrorSuccess);

  cl_assert_equal_b(ams_is_registered_for_all_entity_updates(), false);

  // Expect to have written the command to register for the Track entity:
  fake_gatt_client_op_assert_write(entity_update, s_register_track_entity,
                                   sizeof(s_register_track_entity), GAPLEClientKernel,
                                   true /* is_response_required */);
  ams_handle_write_response(entity_update, BLEGATTErrorSuccess);

  cl_assert_equal_b(ams_is_registered_for_all_entity_updates(), true);

  // After GATT re-discovery, the registration needs to happen again
  ams_invalidate_all_references();
  prv_discover_ams(1 /* num_instances */);
  cl_assert_equal_b(ams_is_registered_for_all_entity_updates(), false);
}

void test_ams__register_for_entity_updates_retry_if_out_of_resources(void) {
  prv_discover_ams(1 /* num_instances */);

  // Simulate not having enough resources to process the request:
  fake_gatt_client_op_set_write_return_value(BTErrnoNotEnoughResources);

  // Simulate successful subscription:
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_subscribe(entity_update, BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Nothing should have been written because out of resources:
  fake_gatt_client_op_assert_no_write();

  // Resources come available again:
  fake_gatt_client_op_set_write_return_value(BTErrnoOK);

  // Simulate processing the callback to retry:
  cl_assert(s_launcher_task_callback != NULL);
  s_launcher_task_callback(s_launcher_task_callback_data);

  // Expect to have written the command to register for the Player entity:
  fake_gatt_client_op_assert_write(entity_update, s_register_player_entity,
                                   sizeof(s_register_player_entity), GAPLEClientKernel,
                                   true /* is_response_required */);
  ams_handle_write_response(entity_update, BLEGATTErrorSuccess);
}

static const MusicServerImplementation s_dummy_server_implementation = {};
static void prv_set_dummy_server_connected(bool connected) {
  music_set_connected_server(&s_dummy_server_implementation,
                             connected /* connected */);
}

void test_ams__dont_register_if_another_music_server_is_already_connected(void) {
  prv_discover_ams(1 /* num_instances */);
  // Not connected yet (still need to subscribe):
  cl_assert_equal_s(music_get_connected_server_debug_name(), NULL);

  prv_set_dummy_server_connected(true /* connected */);

  // Simulate successful subscription:
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_subscribe(entity_update, BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Nothing should have been written, because there's already a music server connected.
  fake_gatt_client_op_assert_no_write();

  // Clean up after ourselves:
  prv_set_dummy_server_connected(false /* connected */);
}

// Tests: Sending Remote Commands
///////////////////////////////////////////////////////////

static void prv_connect_ams(void) {
  prv_discover_ams(1 /* num_instances */);

  // Simulate successful subscription:
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_subscribe(entity_update, BLESubscriptionNotifications, BLEGATTErrorSuccess);

  // Simulate successful write responses (for the Entity Update registration write requests):
  ams_handle_write_response(entity_update, BLEGATTErrorSuccess);
  ams_handle_write_response(entity_update, BLEGATTErrorSuccess);
  ams_handle_write_response(entity_update, BLEGATTErrorSuccess);

  cl_assert_equal_b(ams_is_registered_for_all_entity_updates(), true);

  fake_gatt_client_op_clear_write_list();
}

static uint8_t prv_ams_command_for_music_command(MusicCommand command) {
  switch (command) {
    case MusicCommandPlay:
      return 0x00;
    case MusicCommandPause:
      return 0x01;
    case MusicCommandTogglePlayPause:
      return 0x02;
    case MusicCommandNextTrack:
      return 0x03;
    case MusicCommandPreviousTrack:
      return 0x04;
    case MusicCommandVolumeUp:
      return 0x05;
    case MusicCommandVolumeDown:
      return 0x06;
    case MusicCommandAdvanceRepeatMode:
      return 0x07;
    case MusicCommandAdvanceShuffleMode:
      return 0x08;
    case MusicCommandSkipForward:
      return 0x09;
    case MusicCommandSkipBackward:
      return 0x0A;
    case MusicCommandLike:
      return 0x0B;
    case MusicCommandDislike:
      return 0x0C;
    case MusicCommandBookmark:
      return 0x0D;
    default:
      cl_assert(false);
      return 0xFF;
  }
}

void test_ams__send_remote_command(void) {
  prv_connect_ams();

  // Exercise all MusicCommand types (currently they are all supported):
  for (MusicCommand music_cmd = 0; music_cmd < NumMusicCommand; ++music_cmd) {
    music_command_send(music_cmd);

    BLECharacteristic remote_command = s_characteristics[0][AMSCharacteristicRemoteCommand];
    const uint8_t ams_cmd = prv_ams_command_for_music_command(music_cmd);
    fake_gatt_client_op_assert_write(remote_command, &ams_cmd, sizeof(ams_cmd),
                                     GAPLEClientKernel, true /* is_response_required */);

    // Simulate receiving the response:
    ams_handle_write_response(remote_command, BLEGATTErrorSuccess);
  }

  // Invalid/Unsupported command:
  music_command_send(NumMusicCommand);
  fake_gatt_client_op_assert_no_write();
}

void test_ams__send_remote_command_non_kernel_main_task(void) {
  prv_connect_ams();

  // Simulate Music app calling music_command_send():
  stub_pebble_tasks_set_current(PebbleTask_App);
  music_command_send(MusicCommandPlay);

  // Process the KernelMain callback:
  cl_assert(s_launcher_task_callback != NULL);
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);
  s_launcher_task_callback(s_launcher_task_callback_data);

  BLECharacteristic remote_command = s_characteristics[0][AMSCharacteristicRemoteCommand];
  const uint8_t ams_cmd = prv_ams_command_for_music_command(MusicCommandPlay);
  fake_gatt_client_op_assert_write(remote_command, &ams_cmd, sizeof(ams_cmd),
                                   GAPLEClientKernel, true /* is_response_required */);
}

void test_ams__send_remote_command_non_kernel_main_task_then_disconnect(void) {
  prv_connect_ams();

  // Simulate Music app calling music_command_send():
  stub_pebble_tasks_set_current(PebbleTask_App);
  music_command_send(MusicCommandPlay);

  // Simulate disconnecting:
  ams_destroy();

  // Process the KernelMain callback:
  cl_assert(s_launcher_task_callback != NULL);
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);
  s_launcher_task_callback(s_launcher_task_callback_data);

  // No crashes, no writes
  fake_gatt_client_op_assert_no_write();
}

// Tests: music_needs_user_to_start_playback_on_phone
///////////////////////////////////////////////////////////

void test_ams__music_needs_user_to_start_playback_on_phone(void) {
  prv_connect_ams();

  // No metadata received yet, user needs to start playback on phone
  cl_assert_equal_b(music_needs_user_to_start_playback_on_phone(), true);

  // Received metadata
//  cl_assert_equal_b(music_needs_user_to_start_playback_on_phone(), false);

  // Metadata cleared
  cl_assert_equal_b(music_needs_user_to_start_playback_on_phone(), true);
}

// Tests: music_request_..._latency
///////////////////////////////////////////////////////////

void test_ams__music_request_reduced_latency(void) {
  prv_connect_ams();

  music_request_reduced_latency(true /* reduced_latency */);
  cl_assert_equal_i(s_conn_mgr_states[BtConsumerMusicServiceIndefinite].state,
                    ResponseTimeMiddle);
  cl_assert_equal_i(s_conn_mgr_states[BtConsumerMusicServiceIndefinite].max_period_secs,
                    MAX_PERIOD_RUN_FOREVER);

  music_request_reduced_latency(false /* reduced_latency */);
  cl_assert_equal_i(s_conn_mgr_states[BtConsumerMusicServiceIndefinite].state,
                    ResponseTimeMax);
}

void test_ams__music_request_low_latency_for_period(void) {
  prv_connect_ams();

  const uint32_t period_s = 1234;
  music_request_low_latency_for_period(period_s * 1000);
  cl_assert_equal_i(s_conn_mgr_states[BtConsumerMusicServiceMomentary].state,
                    ResponseTimeMin);
  cl_assert_equal_i(s_conn_mgr_states[BtConsumerMusicServiceMomentary].max_period_secs,
                    period_s);
}

// Tests: Receiving Player updates (the happy paths)
///////////////////////////////////////////////////////////

static void prv_receive_entity_update(const uint8_t *update, uint16_t update_length) {
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_read_or_notification(entity_update, update, update_length, BLEGATTErrorSuccess);
}

void test_ams__receive_player_name_update(void) {
  prv_connect_ams();

  // 0000  00 00 00 4d 75 73 69 63                            ...Music
  uint8_t player_name_update[] = {
    0x00, 0x00, 0x00, 0x4d, 0x75, 0x73, 0x69, 0x63,
  };
  prv_receive_entity_update(player_name_update, sizeof(player_name_update));

  char player_name[MUSIC_BUFFER_LENGTH];
  cl_assert_equal_b(music_get_player_name(player_name), true);
  cl_assert_equal_s(player_name, "Music");
}

void test_ams__receive_player_playback_info_update(void) {
  prv_connect_ams();

  // Receive: playing, 200% playback rate, elapsed time 184.755s
  // 0000  00 01 00 31 2c 32 2e 30  2c 31 38 34 2e 37 35 35   ...1,2.0 ,184.755
  uint8_t playback_info_update[] = {
    0x00, 0x01, 0x00, 0x31, 0x2c, 0x32, 0x2e, 0x30,
    0x2c, 0x31, 0x38, 0x34, 0x2e, 0x37, 0x35, 0x35,
  };
  prv_receive_entity_update(playback_info_update, sizeof(playback_info_update));

  cl_assert_equal_i(music_get_playback_state(), MusicPlayStatePlaying);

  // music_get_pos relies on having a sensible track duration, so simulate receiving this too:
  // 02 03 00 33 31 39 2e 35  30 37                     ...319.5 07
  uint8_t track_duration_update[] = {
    0x02, 0x03, 0x00, 0x33, 0x31, 0x39, 0x2e, 0x35,
    0x30, 0x37,
  };
  prv_receive_entity_update(track_duration_update, sizeof(track_duration_update));

  uint32_t track_pos_ms;
  uint32_t track_duration_ms;
  music_get_pos(&track_pos_ms, &track_duration_ms);
  cl_assert_equal_i(track_pos_ms, 184755);
  cl_assert_equal_i(track_duration_ms, 319507);

  cl_assert_equal_i(music_get_playback_rate_percent(), 200);
}

void test_ams__receive_player_playback_info_update_nulls(void) {
  prv_connect_ams();

  // Receive: paused, empty string, empty string
  // 0000  00 01 00 30 2c 2c                                  ...0,,
  uint8_t playback_info_update[] = {
    0x00, 0x01, 0x00, 0x30, 0x2c, 0x2c,
  };
  prv_receive_entity_update(playback_info_update, sizeof(playback_info_update));

  cl_assert_equal_i(music_get_playback_state(), MusicPlayStatePaused);

  uint32_t track_pos_ms;
  uint32_t track_duration_ms;
  music_get_pos(&track_pos_ms, &track_duration_ms);
  cl_assert_equal_i(track_pos_ms, 0);
  cl_assert_equal_i(track_duration_ms, 0);

  cl_assert_equal_i(music_get_playback_rate_percent(), 0);
}

void test_ams__receive_player_volume_update(void) {
  prv_connect_ams();

  // Receive volume of 0.604925
  // 00 02 00 30 2e 36 30 34  39 32 35                  ...0.604 925
  uint8_t volume_update[] = {
    0x00, 0x02, 0x00, 0x30, 0x2e, 0x36, 0x30, 0x34,
    0x39, 0x32, 0x35,
  };
  prv_receive_entity_update(volume_update, sizeof(volume_update));

  cl_assert_equal_i(music_get_volume_percent(), 60);
}

// Tests: Receiving Player updates (the unhappy paths)
///////////////////////////////////////////////////////////

static void prv_assert_initial_playback_state(void) {
  cl_assert_equal_i(music_get_playback_state(), MusicPlayStateUnknown);

  uint32_t track_pos_ms;
  uint32_t track_duration_ms;
  music_get_pos(&track_pos_ms, &track_duration_ms);
  cl_assert_equal_i(track_pos_ms, 0);
  cl_assert_equal_i(track_duration_ms, 0);

  cl_assert_equal_i(music_get_playback_rate_percent(), 0);
}

void test_ams__receive_non_numeric_player_playback_info_update(void) {
  prv_connect_ams();

  // Receive: 'A', 'B.0' playback rate, elapsed time 184.755s
  // 0000  00 01 00 41 2c 42 2e 30  2c 31 38 34 2e 37 35 35   ...A,B.0 ,184.755
  uint8_t nan_playback_info_update[] = {
    0x00, 0x01, 0x00, 0x41, 0x2c, 0x42, 0x2e, 0x30,
    0x2c, 0x31, 0x38, 0x34, 0x2e, 0x37, 0x35, 0x35,
  };
  prv_receive_entity_update(nan_playback_info_update, sizeof(nan_playback_info_update));

  prv_assert_initial_playback_state();
}

void test_ams__receive_incomplete_csv_list_player_playback_info_update(void) {
  prv_connect_ams();

  // Receive: playing, 200% playback rate
  // 0000  00 01 00 31 2c 32 2e 30    ...1,2.0
  uint8_t incomplete_playback_info_update[] = {
    0x00, 0x01, 0x00, 0x31, 0x2c, 0x32, 0x2e, 0x30,
  };
  prv_receive_entity_update(incomplete_playback_info_update,
                            sizeof(incomplete_playback_info_update));

  prv_assert_initial_playback_state();
}

void test_ams__receive_malformed_player_volume_update(void) {
  prv_connect_ams();

  cl_assert_equal_i(music_get_volume_percent(), 0);

  // Receive volume of 0.604925
  // 00 02 00 30 2e 41 30 34  39 32 35                  ...0.A04 925
  uint8_t volume_update[] = {
    0x00, 0x02, 0x00, 0x30, 0x2e, 0x41, 0x30, 0x34,
    0x39, 0x32, 0x35,
  };
  prv_receive_entity_update(volume_update, sizeof(volume_update));

  cl_assert_equal_i(music_get_volume_percent(), 0);
}

// Tests: Receiving Track updates
///////////////////////////////////////////////////////////

void test_ams__receive_track_artist_update(void) {
  prv_connect_ams();

  // 0000  02 00 00 4d 69 6c 65 73  20 44 61 76 69 73         ...Miles  Davis
  uint8_t track_artist_update[] = {
    0x02, 0x00, 0x00, 0x4d, 0x69, 0x6c, 0x65, 0x73,
    0x20, 0x44, 0x61, 0x76, 0x69, 0x73,
  };
  prv_receive_entity_update(track_artist_update, sizeof(track_artist_update));

  char artist[MUSIC_BUFFER_LENGTH];
  music_get_now_playing(NULL, artist, NULL);

  cl_assert_equal_s(artist, "Miles Davis");
}

void test_ams__receive_track_title_update(void) {
  prv_connect_ams();

  // 0000  02 02 00 53 6f 20 57 68  61 74                     ...So Wh at
  uint8_t track_title_update[] = {
    0x02, 0x02, 0x00, 0x53, 0x6f, 0x20, 0x57, 0x68,
    0x61, 0x74,
  };
  prv_receive_entity_update(track_title_update, sizeof(track_title_update));

  char title[MUSIC_BUFFER_LENGTH];
  music_get_now_playing(title, NULL, NULL);

  cl_assert_equal_s(title, "So What");
}

void test_ams__receive_track_album_update(void) {
  prv_connect_ams();

  // 0000  02 01 00 4b 69 6e 64 20  4f 66 20 42 6c 75 65 20   ...Kind  Of Blue
  // 0010  28 4c 65 67 61 63 79 20  45 64 69 74 69 6f 6e 29   (Legacy  Edition)
  uint8_t track_album_update[] = {
    0x02, 0x01, 0x00, 0x4b, 0x69, 0x6e, 0x64, 0x20,
    0x4f, 0x66, 0x20, 0x42, 0x6c, 0x75, 0x65, 0x20,
    0x28, 0x4c, 0x65, 0x67, 0x61, 0x63, 0x79, 0x20,
    0x45, 0x64, 0x69, 0x74, 0x69, 0x6f, 0x6e, 0x29,
  };
  prv_receive_entity_update(track_album_update, sizeof(track_album_update));

  char album[MUSIC_BUFFER_LENGTH];
  music_get_now_playing(NULL, NULL, album);

  cl_assert_equal_s(album, "Kind Of Blue (Legacy Edition)");
}

// Tests: Music service capabilities
///////////////////////////////////////////////////////////

void test_ams__supported_capabilities(void) {
  prv_connect_ams();

  // music_is_progress_reporting_supported() relies on a valid track duration
  uint8_t track_duration_update[] = {
    0x02, 0x03, 0x00, 0x33, 0x31, 0x39, 0x2e, 0x35,
    0x30, 0x37,
  };
  prv_receive_entity_update(track_duration_update, sizeof(track_duration_update));

  cl_assert_equal_b(music_is_playback_state_reporting_supported(), true);
  cl_assert_equal_b(music_is_progress_reporting_supported(), true);
  cl_assert_equal_b(music_is_volume_reporting_supported(), true);
  cl_assert_equal_b(music_needs_user_to_start_playback_on_phone(), true);
  for (MusicCommand cmd = 0; cmd < NumMusicCommand; ++cmd) {
    cl_assert_equal_b(music_is_command_supported(cmd), true);
  }

  cl_assert_equal_b(music_is_command_supported(NumMusicCommand), false);
}

// Tests: Create & Destroy
///////////////////////////////////////////////////////////

void test_ams__create_again_trips_assert(void) {
  cl_assert_passert(ams_create());
}

void test_ams__create_works_again_after_destroy(void) {
  ams_destroy();
  ams_create();
  // No assert hit.
}

void test_ams__destroy_after_destroy_is_fine(void) {
  ams_destroy();
  ams_destroy();
  // No assert hit.
}

void test_ams__destroy_disconnects_from_music_service(void) {
  prv_discover_ams(1 /* num_instances */);

  // Simulate successful subscription:
  BLECharacteristic entity_update = s_characteristics[0][AMSCharacteristicEntityUpdate];
  ams_handle_subscribe(entity_update, BLESubscriptionNotifications, BLEGATTErrorSuccess);

  ams_destroy();
  cl_assert_equal_s(music_get_connected_server_debug_name(), NULL);
}

// Test setup
///////////////////////////////////////////////////////////

void test_ams__initialize(void) {
  s_launcher_task_callback = NULL;
  memset(s_conn_mgr_states, 0, sizeof(s_conn_mgr_states));
  fake_rtc_init(1234, 5678);
  fake_event_init();
  fake_gatt_client_subscriptions_init();
  fake_gatt_client_op_init();
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);
  ams_create();
  cl_assert_equal_s(music_get_connected_server_debug_name(), NULL);
}

void test_ams__cleanup(void) {
  ams_destroy();
  fake_gatt_client_op_deinit();
  fake_gatt_client_subscriptions_deinit();
}
