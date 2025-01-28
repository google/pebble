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

#include "poll_remote.h"
#include "services/common/comm_session/protocol.h"
#include "services/common/comm_session/session.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/attributes.h"

#include <string.h>

/*
 * Private
 */

static const uint8_t MIN_INTERVAL_MINUTES = 1;
static const uint16_t ENDPOINT_ID = 0xcafe;
static bool s_running = false;

typedef enum {
  CMD_POLL = 0x0, // Formerly command poll mail
  LEGACY_CMD_REQUEST_INTERVAL = 0x1, // for backwards compatibility
  CMD_SET_INTERVAL = 0x2,
  CMD_REQUEST_POLL = 0x3,
} PollRemoteCommand;

// Deprecated -- used to set the mail poll interval
typedef struct PACKED {
  uint8_t cmd;
  uint8_t interval_minutes;
} PollLegacySetIntervalMessage;

// Poll a service at a specific interval
typedef struct PACKED {
  PollRemoteCommand cmd;
  PollRemoteService service;
  uint8_t interval_minutes;
} PollSetIntervalMessage;

// Request to poll a service now
typedef struct PACKED {
  PollRemoteCommand cmd;
  PollRemoteService service;
} PollRequestMessage;

typedef struct PACKED {
  PollRemoteCommand cmd;
  PollRemoteService service;
} PollRemoteMessage;

//! Struct that holds all the state required for a remote polling subsystem.
typedef struct {
  PollRemoteService service;
  //! The minimum interval between two "poll services" requests.
  //! Calls to poll_remote_send_request() will be no-ops if min_interval_minutes has not been reached.
  uint8_t min_interval_minutes;

  //! The maximum interval between two "poll services" requests.
  //! The automatic sending of poll requests will only occur when max_interval_minutes is reached.
  uint8_t max_interval_minutes;

  uint8_t counted_minutes; //!< Number of minutes passed since the last request
} PollRemoteContext;

static void poll_service_timer_callback();

static RegularTimerInfo s_poll_timer = {
  .cb = poll_service_timer_callback,
  .cb_data = NULL,
};

static PollRemoteContext s_poll_remote_contexts[NUM_POLL_REMOTE_SERVICES];

static void for_each_context(void (*poll_remote_function)(PollRemoteContext *ctx)) {
  for (int i = 0; i < NUM_POLL_REMOTE_SERVICES; i++) {
    poll_remote_function(&s_poll_remote_contexts[i]);
  }
}

static bool has_min_interval_passed(PollRemoteContext *ctx) {
  return (ctx->counted_minutes >= ctx->min_interval_minutes);
}

static bool has_max_interval_passed(PollRemoteContext *ctx) {
  return (ctx->counted_minutes >= ctx->max_interval_minutes);
}

// SystemTaskCallback
static void prv_send_request(PollRemoteContext *ctx) {
  if (has_min_interval_passed(ctx) == false) {
    return;
  }
  CommSession *session = comm_session_get_system_session();
  if (!session) {
    return;
  }
  // [MT]: comm_session_send_data() doesn't make the link active,
  // which is what we want here. If this changes in the future
  // we need to take measures here to make sure we don't pull the link active.
  const PollRemoteMessage msg = {
    .cmd = CMD_POLL,
    .service = ctx->service
  };
  comm_session_send_data(session, ENDPOINT_ID, (const uint8_t *)&msg, sizeof(PollRemoteMessage),
                         COMM_SESSION_DEFAULT_TIMEOUT);
  ctx->counted_minutes = 0;
}

static void context_interval_check(PollRemoteContext *ctx) {
  if (ctx->max_interval_minutes == 0) {
    return;
  }

  ctx->counted_minutes++;

  if (has_max_interval_passed(ctx)) {
    prv_send_request(ctx);
  }
}


static void start(PollRemoteContext *ctx) {
  ctx->counted_minutes = 0;
}

static void set_intervals(PollRemoteContext *ctx, const uint8_t min_interval_minutes, const uint8_t max_interval_minutes) {
  ctx->min_interval_minutes = min_interval_minutes;
  ctx->max_interval_minutes = max_interval_minutes;
}

void comm_poll_remote_protocol_msg_callback(CommSession *session, const uint8_t* data, size_t length) {
  PollRemoteCommand cmd = data[0];

  switch (cmd) {
    case CMD_REQUEST_POLL: {
      PollRequestMessage *msg = (PollRequestMessage *)data;
      if (msg->service >= NUM_POLL_REMOTE_SERVICES) { return; }
      prv_send_request(&s_poll_remote_contexts[msg->service]);
      break;
    }
    case LEGACY_CMD_REQUEST_INTERVAL: {
      PollLegacySetIntervalMessage *msg = (PollLegacySetIntervalMessage *)data;
      poll_remote_set_intervals(POLL_REMOTE_SERVICE_MAIL, MIN_INTERVAL_MINUTES, msg->interval_minutes);
      break;
    }
    case CMD_SET_INTERVAL: {
      PollSetIntervalMessage *msg = (PollSetIntervalMessage *)data;
      if (msg->service >= NUM_POLL_REMOTE_SERVICES) { return; }
      poll_remote_set_intervals(msg->service, MIN_INTERVAL_MINUTES, msg->interval_minutes);
      break;
    }
    default: {
      PBL_LOG(LOG_LEVEL_ERROR, "Invalid command.");
      return;
    }
  }
}

/*
 * Public
 */

static void poll_service_system_task_callback(void *data) {
  PBL_ASSERTN(s_running);
  for_each_context(context_interval_check);
}

static void poll_service_timer_callback(void *data) {
  system_task_add_callback(poll_service_system_task_callback, data);
}

void poll_remote_init(void) {
  for (int i = 0; i < NUM_POLL_REMOTE_SERVICES; i++) {
    s_poll_remote_contexts[i].service = i;
  }
}

void poll_remote_send_request(PollRemoteService service) {
  prv_send_request(&s_poll_remote_contexts[service]);
}

void poll_remote_start(void) {
  if (s_running) {
    return;
  }

  s_running = true;
  for_each_context(start);
  regular_timer_add_minutes_callback(&s_poll_timer);
}

void poll_remote_stop(void) {
  if (!s_running) {
    return;
  }

  s_running = false;
  regular_timer_remove_callback(&s_poll_timer);
}

void poll_remote_set_intervals(PollRemoteService service, const uint8_t min_interval_minutes, const uint8_t max_interval_minutes) {
  set_intervals(&s_poll_remote_contexts[service], min_interval_minutes, max_interval_minutes);
  (max_interval_minutes == 0) ? poll_remote_stop() : poll_remote_start();
}
