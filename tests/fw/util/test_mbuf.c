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

#include "util/mbuf.h"
#include "util/mbuf_iterator.h"

#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_serial.h"

extern MBuf *s_free_list;


// Setup

void test_mbuf__initialize(void) {
}

void test_mbuf__cleanup(void) {
}


// Tests

void test_mbuf__length(void) {
  // test the mbuf_get_length()/mbuf_get_chain_length() functions
  char *data = __FILE_NAME__; // dummy data
  char data_length = sizeof(__FILE_NAME__);
  MBuf mbuf1 = MBUF_EMPTY;
  MBuf mbuf2 = MBUF_EMPTY;
  MBuf mbuf3 = MBUF_EMPTY;

  // empty mbuf chain
  cl_assert(mbuf_get_chain_length(NULL) == 0);

  // single empty mbuf in the chain
  mbuf1 = MBUF_EMPTY;
  cl_assert(mbuf_get_length(&mbuf1) == 0);
  cl_assert(mbuf_get_chain_length(&mbuf1) == 0);

  // single mbuf of non-zero length
  mbuf1 = MBUF_EMPTY;
  mbuf_set_data(&mbuf1, data, data_length);
  cl_assert(mbuf_get_length(&mbuf1) == data_length);
  cl_assert(mbuf_get_chain_length(&mbuf1) == data_length);

  // three mbufs of 0 length
  mbuf1 = MBUF_EMPTY;
  mbuf2 = MBUF_EMPTY;
  mbuf3 = MBUF_EMPTY;
  mbuf_append(&mbuf1, &mbuf2);
  mbuf_append(&mbuf1, &mbuf3);
  cl_assert(mbuf_get_length(&mbuf1) == 0);
  cl_assert(mbuf_get_chain_length(&mbuf1) == 0);

  // three mbufs of non-zero length in a chain
  mbuf1 = MBUF_EMPTY;
  mbuf2 = MBUF_EMPTY;
  mbuf3 = MBUF_EMPTY;
  mbuf_set_data(&mbuf1, data, data_length);
  mbuf_set_data(&mbuf2, data, data_length);
  mbuf_set_data(&mbuf3, data, data_length);
  mbuf_append(&mbuf1, &mbuf2);
  mbuf_append(&mbuf1, &mbuf3);
  cl_assert(mbuf_get_length(&mbuf1) == data_length);
  cl_assert(mbuf_get_length(&mbuf2) == data_length);
  cl_assert(mbuf_get_length(&mbuf3) == data_length);
  cl_assert(mbuf_get_chain_length(&mbuf1) == (3 * data_length));

  // three mbufs with one of zero length
  mbuf1 = MBUF_EMPTY;
  mbuf2 = MBUF_EMPTY;
  mbuf3 = MBUF_EMPTY;
  mbuf_set_data(&mbuf1, data, data_length);
  mbuf_set_data(&mbuf3, data, data_length);
  mbuf_append(&mbuf1, &mbuf2);
  mbuf_append(&mbuf1, &mbuf3);
  cl_assert(mbuf_get_length(&mbuf1) == data_length);
  cl_assert(mbuf_get_length(&mbuf2) == 0);
  cl_assert(mbuf_get_length(&mbuf3) == data_length);
  cl_assert(mbuf_get_chain_length(&mbuf1) == (2 * data_length));
}

void test_mbuf__iter_empty(void) {
  // test iteratoring over empty mbuf chains
  MBuf mbuf1 = MBUF_EMPTY;
  MBuf mbuf2 = MBUF_EMPTY;
  MBufIterator iter;
  mbuf_append(&mbuf1, &mbuf2);
  mbuf_iterator_init(&iter, NULL);
  cl_assert(mbuf_iterator_is_finished(&iter));
  mbuf_iterator_init(&iter, &mbuf2);
  cl_assert(mbuf_iterator_is_finished(&iter));
  mbuf_iterator_init(&iter, &mbuf1);
  cl_assert(mbuf_iterator_is_finished(&iter));
  uint8_t data;
  cl_assert(!mbuf_iterator_read_byte(&iter, &data));
  cl_assert(!mbuf_iterator_get_current_mbuf(&iter));
}

void test_mbuf__iter_modify(void) {
  // modify (read and then write) the data in an mbuf chain using an mbuf iterator
  // test reading from an mbuf chain via an mbuf iterator
  uint8_t data1[] = {10, 11, 12};
  uint8_t data3[] = {13, 14, 15};
  MBufIterator write_iter, read_iter;
  MBuf mbuf1 = MBUF_EMPTY;
  MBuf mbuf2 = MBUF_EMPTY;
  MBuf mbuf3 = MBUF_EMPTY;
  mbuf_set_data(&mbuf1, data1, 3);
  mbuf_set_data(&mbuf3, data3, 3);
  mbuf_append(&mbuf1, &mbuf2);
  mbuf_append(&mbuf1, &mbuf3);
  mbuf_iterator_init(&write_iter, &mbuf1);
  mbuf_iterator_init(&read_iter, &mbuf1);
  for (int i = 0; i < 6; i++) {
    cl_assert(!mbuf_iterator_is_finished(&write_iter));
    cl_assert(!mbuf_iterator_is_finished(&read_iter));
    // check we're on the expected mbuf
    if (i < 3) {
      cl_assert(mbuf_iterator_get_current_mbuf(&write_iter) == &mbuf1);
      cl_assert(mbuf_iterator_get_current_mbuf(&read_iter) == &mbuf1);
    } else {
      cl_assert(mbuf_iterator_get_current_mbuf(&write_iter) == &mbuf3);
      cl_assert(mbuf_iterator_get_current_mbuf(&read_iter) == &mbuf3);
    }
    uint8_t data_byte = 0;
    bool have_byte = mbuf_iterator_read_byte(&read_iter, &data_byte);
    cl_assert(have_byte);
    // check that the data is what we expect
    cl_assert(data_byte == (i + 10));
    // modify the data by increasing the value by 10
    cl_assert(mbuf_iterator_write_byte(&write_iter, data_byte + 10));
  }
  cl_assert(mbuf_iterator_is_finished(&write_iter));
  cl_assert(mbuf_iterator_is_finished(&read_iter));
  // verify the final value of the data
  for (int i = 0; i < 6; i++) {
    uint8_t *data;
    int index;
    if (i < 3) {
      data = data1;
      index = i;
    } else {
      data = data3;
      index = i - 3;
    }
    cl_assert(data[index] == (i + 20));
  }
}

static int prv_get_free_list_length(void) {
  int len = 0;
  for (MBuf *m = s_free_list; m; m = mbuf_get_next(m)) {
    len++;
  }
  return len;
}

void test_mbuf__mbuf_pool(void) {
  // get an MBuf and the pool should still be empty
  MBuf *mbuf1 = mbuf_get(NULL, 0, MBufPoolUnitTest);
  cl_assert(prv_get_free_list_length() == 0);

  // free the mbuf and the pool should now contain it
  mbuf_free(mbuf1);
  cl_assert(prv_get_free_list_length() == 1);
  cl_assert(s_free_list == mbuf1);

  // get another mbuf and expect that it's the same one and the pool is empty
  MBuf *mbuf2 = mbuf_get(NULL, 0, MBufPoolUnitTest);
  cl_assert(mbuf2 == mbuf1);
  cl_assert(prv_get_free_list_length() == 0);

  // get another mbuf and expect that it's not the same as the previous one
  MBuf *mbuf3 = mbuf_get(NULL, 0, MBufPoolUnitTest);
  cl_assert(mbuf3 != mbuf2);
  cl_assert(prv_get_free_list_length() == 0);

  // free both of the mbufs (one at a time)
  mbuf_free(mbuf2);
  cl_assert(prv_get_free_list_length() == 1);
  mbuf_free(mbuf3);
  cl_assert(prv_get_free_list_length() == 2);
}
