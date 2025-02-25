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

#include "comm/bt_lock.h"
#include "services/common/comm_session/session_analytics.h"
#include "services/common/comm_session/session_internal.h"
#include "services/common/comm_session/session_send_queue.h"
#include "system/passert.h"
#include "util/math.h"

// -------------------------------------------------------------------------------------------------

extern bool comm_session_is_valid(const CommSession *session);

// -------------------------------------------------------------------------------------------------
// Interface towards CommSession

void comm_session_send_queue_cleanup(CommSession *session) {
  SessionSendQueueJob *job = session->send_queue_head;
  while (job) {
    SessionSendQueueJob *next = (SessionSendQueueJob *) job->node.next;
    job->impl->free(job);
    job = next;
  }
  session->send_queue_head = NULL;
}

// -------------------------------------------------------------------------------------------------
// Interface towards Senders

void comm_session_send_queue_add_job(CommSession *session, SessionSendQueueJob **job_ptr_ptr) {
  bt_lock();
  {
    SessionSendQueueJob *job = *job_ptr_ptr;
    if (!comm_session_is_valid(session)) {
      job->impl->free(job);
      *job_ptr_ptr = NULL;
      goto unlock;
    }
    ListNode *head = (ListNode *)session->send_queue_head;
    PBL_ASSERTN(!list_contains(head, (const ListNode *)job));
    if (head) {
      list_append(head, (ListNode *)job);
    } else {
      session->send_queue_head = job;
    }
    // Schedule to let the transport to send the enqueued data:
    comm_session_send_next(session);
  }
unlock:
  bt_unlock();
}

// -------------------------------------------------------------------------------------------------
// Interface towards Transport
// bt_lock is assumed to be taken by the caller of each of the below functions:

size_t comm_session_send_queue_get_length(const CommSession *session) {
  size_t length = 0;
  const SessionSendQueueJob *job = session->send_queue_head;
  while (job) {
    length += job->impl->get_length(job);
    job = (const SessionSendQueueJob *)job->node.next;
  }
  return length;
}

size_t comm_session_send_queue_copy(CommSession *session, uint32_t start_offset,
                                    size_t length, uint8_t *data_out) {
  size_t remaining_length = length;
  const SessionSendQueueJob *job = session->send_queue_head;
  while (job && remaining_length) {
    const size_t job_length = job->impl->get_length(job);
    if (job_length <= start_offset) {
      start_offset -= job_length;
    } else {
      const size_t copied_length = job->impl->copy(job, start_offset, remaining_length, data_out);
      remaining_length -= copied_length;
      data_out += copied_length;
      start_offset = 0;
    }
    job = (SessionSendQueueJob *)job->node.next;
  }
  return (length - remaining_length);
}

size_t comm_session_send_queue_get_read_pointer(const CommSession *session,
                                                const uint8_t **data_out) {
  if (!session->send_queue_head) {
    return 0;
  }
  const SessionSendQueueJob *job = session->send_queue_head;
  return job->impl->get_read_pointer(job, data_out);
}

void comm_session_send_queue_consume(CommSession *session, size_t remaining_length) {
  // The data has successfully been sent out at this point
  comm_session_analytics_inc_bytes_sent(session, remaining_length);

  PBL_ASSERTN(session->send_queue_head);
  SessionSendQueueJob *job = session->send_queue_head;
  while (job && remaining_length) {
    const size_t job_length = job->impl->get_length(job);
    const size_t consume_length = MIN(remaining_length, job_length);
    job->impl->consume(job, consume_length);
    SessionSendQueueJob *next = (SessionSendQueueJob *)job->node.next;
    if (job_length == consume_length) {
      // job's done
      list_remove((ListNode *)job, (ListNode **)&session->send_queue_head, NULL);
      job->impl->free(job);
    }
    remaining_length -= consume_length;
    job = next;
  }
}
