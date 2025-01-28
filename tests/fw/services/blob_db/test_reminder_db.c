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

#include "services/normal/blob_db/reminder_db.h"

// Fixture
////////////////////////////////////////////////////////////////

// Fakes
////////////////////////////////////////////////////////////////
#include "fake_settings_file.h"

// Stubs
////////////////////////////////////////////////////////////////
#include "stubs_analytics.h"
#include "stubs_blob_db_sync.h"
#include "stubs_hexdump.h"
#include "stubs_layout_layer.h"
#include "stubs_logging.h"
#include "stubs_mutex.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_prompt.h"
#include "stubs_rand_ptr.h"
#include "stubs_regular_timer.h"
#include "stubs_reminders.h"
#include "stubs_sleep.h"
#include "stubs_task_watchdog.h"

void reminders_handle_reminder_removed(const Uuid *reminder_id) {
}

static TimelineItem item1 = {
  .header = {
    .id = {0x6b, 0xf6, 0x21, 0x5b, 0xc9, 0x7f, 0x40, 0x9e,
             0x8c, 0x31, 0x4f, 0x55, 0x65, 0x72, 0x22, 0xb4},
    .parent_id = {0xff, 0xf6, 0x21, 0x5b, 0xc9, 0x7f, 0x40, 0x9e,
             0x8c, 0x31, 0x4f, 0x55, 0x65, 0x72, 0x22, 0x01},
    .timestamp = 1,
    .duration = 0,
    .type = TimelineItemTypeReminder,
    .layout = LayoutIdTest,
    // don't care about the rest
  }
};

static TimelineItem item2 = {
  .header = {
    .id = {0x55, 0xcb, 0x7c, 0x75, 0x8a, 0x35, 0x44, 0x87,
             0x90, 0xa4, 0x91, 0x3f, 0x1f, 0xa6, 0x76, 0x01},
    .parent_id = {0xff, 0xf6, 0x21, 0x5b, 0xc9, 0x7f, 0x40, 0x9e,
             0x8c, 0x31, 0x4f, 0x55, 0x65, 0x72, 0x22, 0x01},
    .timestamp = 3,
    .duration = 0,
    .type = TimelineItemTypeReminder,
    .layout = LayoutIdTest,
  },
};

static TimelineItem item3 = {
  .header = {
    .id = {0x7c, 0x65, 0x2e, 0xb9, 0x26, 0xd6, 0x44, 0x2c,
             0x98, 0x68, 0xa4, 0x36, 0x79, 0x7d, 0xe2, 0x05},
    .parent_id = {0xff, 0xf6, 0x21, 0x5b, 0xc9, 0x7f, 0x40, 0x9e,
             0x8c, 0x31, 0x4f, 0x55, 0x65, 0x72, 0x22, 0x02},
    .timestamp = 4,
    .duration = 0,
    .type = TimelineItemTypeReminder,
    .layout = LayoutIdTest,
  }
};

static TimelineItem item4 = {
  .header = {
    .id = {0x8c, 0x65, 0x2e, 0xb9, 0x26, 0xd6, 0x44, 0x2c,
             0x98, 0x68, 0xa4, 0x36, 0x79, 0x7d, 0xe2, 0x05},
    .parent_id = {0xff, 0xf6, 0x21, 0x5b, 0xc9, 0x7f, 0x40, 0x9e,
             0x8c, 0x31, 0x4f, 0x55, 0x65, 0x72, 0x22, 0x03},
    .timestamp = 4,
    .duration = 0,
    .type = TimelineItemTypeReminder,
    .layout = LayoutIdTest,
  }
};

static SerializedTimelineItemHeader bad_item = {
  .common = {
    .id = {0x8c, 0x65, 0x2e, 0xb9, 0x26, 0xd6, 0x42, 0x2c,
             0x98, 0x68, 0xa4, 0x36, 0x79, 0x7d, 0xe2, 0x05},
    .timestamp = 3,
    .duration = 0,
    .type = TimelineItemTypeReminder,
    .layout = LayoutIdTest,
  },
  .num_attributes = 3,
};

static TimelineItem title_item1 = {
  .header = {
    .id = {0x9c, 0x65, 0x2e, 0xb9, 0x26, 0xd6, 0x44, 0x2c,
             0x98, 0x68, 0xa4, 0x36, 0x79, 0x7d, 0xe2, 0x05},
    .timestamp = 1,
    .duration = 0,
    .type = TimelineItemTypeReminder,
    .layout = LayoutIdTest,
  },
  .attr_list = (AttributeList) {
    .num_attributes = 1,
    .attributes = (Attribute[1]) {{ .id = AttributeIdTitle, .cstring = "test 1" }}
  }
};

static TimelineItem title_item2 = {
  .header = {
    .id = {0xac, 0x65, 0x2e, 0xb9, 0x26, 0xd6, 0x44, 0x2c,
             0x98, 0x68, 0xa4, 0x36, 0x79, 0x7d, 0xe2, 0x05},
    .timestamp = 1,
    .duration = 0,
    .type = TimelineItemTypeReminder,
    .layout = LayoutIdTest,
  },
  .attr_list = (AttributeList) {
    .num_attributes = 1,
    .attributes = (Attribute[1]) {{ .id = AttributeIdTitle, .cstring = "test 2" }}
  }
};

static void prv_insert_default_reminders(void) {
  // add all four explicitly out of order
  cl_assert_equal_i(S_SUCCESS, reminder_db_insert_item(&item4));

  cl_assert_equal_i(S_SUCCESS, reminder_db_insert_item(&item2));

  cl_assert_equal_i(S_SUCCESS, reminder_db_insert_item(&item1));

  cl_assert_equal_i(S_SUCCESS, reminder_db_insert_item(&item3));
}

// Setup
////////////////////////////////////////////////////////////////

void test_reminder_db__initialize(void) {
  reminder_db_init();
}

void test_reminder_db__cleanup(void) {
  reminder_db_deinit();
  fake_settings_file_reset();
}

// Tests
////////////////////////////////////////////////////////////////

void test_reminder_db__basic_test(void) {
  prv_insert_default_reminders();

  // confirm all three are there
  cl_assert(reminder_db_get_len((uint8_t*)&item1.header.id, sizeof(Uuid)) > 0);
  cl_assert(reminder_db_get_len((uint8_t*)&item2.header.id, sizeof(Uuid)) > 0);
  cl_assert(reminder_db_get_len((uint8_t*)&item3.header.id, sizeof(Uuid)) > 0);

  // remove #1 and confirm it's deleted
  cl_assert(S_SUCCESS == reminder_db_delete((uint8_t*)&item1.header.id, sizeof(Uuid)));
  cl_assert(reminder_db_get_len((uint8_t *)&item1.header.id, sizeof(Uuid)) == 0);

  // add 1 back so it's clean
  cl_assert(S_SUCCESS == reminder_db_insert_item(&item1));
  TimelineItem temp = {{{0}}};
  cl_assert(S_SUCCESS == reminder_db_read((uint8_t*)&item1.header.id, sizeof(Uuid), (uint8_t*)&temp,
      sizeof(CommonTimelineItemHeader)));

  // Note: we set things to null because it makes it easier to compare two
  // TimelineItems with memcmp
  // check item 1
  memset(&temp, 0, sizeof(TimelineItem));
  cl_assert(S_SUCCESS == reminder_db_next_item_header(&temp));
  cl_assert(uuid_equal(&item1.header.id, &temp.header.id));
  temp.attr_list.attributes = NULL;
  cl_assert(memcmp(&item1, &temp, sizeof(TimelineItem)) == 0);
  cl_assert(S_SUCCESS == reminder_db_delete_item(&temp.header.id, true /* send_event */));
  cl_assert(reminder_db_get_len((uint8_t *)&item1.header.id, sizeof(Uuid)) == 0);

  // check item 2
  memset(&temp, 0, sizeof(TimelineItem));
  cl_assert(S_SUCCESS == reminder_db_next_item_header(&temp));
  cl_assert(uuid_equal(&item2.header.id, &temp.header.id));
  temp.attr_list.attributes = NULL;
  cl_assert(memcmp(&item2, &temp, sizeof(TimelineItem)) == 0);
  cl_assert(S_SUCCESS == reminder_db_delete_item(&temp.header.id, true /* send_event */));
  cl_assert(reminder_db_get_len((uint8_t *)&item2.header.id, sizeof(Uuid)) == 0);

  // check item 3 or 4
  memset(&temp, 0, sizeof(TimelineItem));
  cl_assert(S_SUCCESS == reminder_db_next_item_header(&temp));
  if (uuid_equal(&item3.header.id, &temp.header.id)) {
    temp.attr_list.attributes = NULL;
    timeline_item_free_allocated_buffer(&temp);
    cl_assert(memcmp(&item3, &temp, sizeof(TimelineItem)) == 0);
    cl_assert(S_SUCCESS == reminder_db_delete_item(&temp.header.id, true /* send_event */));
    cl_assert(reminder_db_get_len((uint8_t *) &item3, sizeof(Uuid)) == 0);

    memset(&temp, 0, sizeof(TimelineItem));
    cl_assert(S_SUCCESS == reminder_db_next_item_header(&temp));
    temp.attr_list.attributes = NULL;
    cl_assert(memcmp(&item4, &temp, sizeof(TimelineItem)) == 0);
    cl_assert(S_SUCCESS == reminder_db_delete_item(&temp.header.id, true /* send_event */));
    cl_assert(reminder_db_get_len((uint8_t *)&item4.header.id, sizeof(Uuid)) == 0);
  } else {
    temp.attr_list.attributes = NULL;
    cl_assert(memcmp(&item4, &temp, sizeof(TimelineItem)) == 0);
    cl_assert(S_SUCCESS == reminder_db_delete_item(&temp.header.id, true /* send_event */));
    cl_assert(reminder_db_get_len((uint8_t *) &item4, sizeof(Uuid)) == 0);

    memset(&temp, 0, sizeof(TimelineItem));
    cl_assert(S_SUCCESS == reminder_db_next_item_header(&temp));
    temp.attr_list.attributes = NULL;
    cl_assert(memcmp(&item3, &temp, sizeof(TimelineItem)) == 0);
    cl_assert(S_SUCCESS == reminder_db_delete_item(&temp.header.id, true /* send_event */));
    cl_assert(reminder_db_get_len((uint8_t *)&item3.header.id, sizeof(Uuid)) == 0);
  }

  cl_assert(S_NO_MORE_ITEMS == reminder_db_next_item_header(&temp));
}

void test_reminder_db__size_test(void) {
  prv_insert_default_reminders();

  cl_assert(sizeof(SerializedTimelineItemHeader) == reminder_db_get_len((uint8_t*) &item1.header.id, sizeof(TimelineItemId)));

  cl_assert(sizeof(SerializedTimelineItemHeader) == reminder_db_get_len((uint8_t*) &item2.header.id, sizeof(TimelineItemId)));

  cl_assert(sizeof(SerializedTimelineItemHeader) == reminder_db_get_len((uint8_t*) &item3.header.id, sizeof(TimelineItemId)));
}

void test_reminder_db__wrong_type_test(void) {
  TimelineItem not_a_reminder = {
    .header = {
      .id = {0x99, 0xcb, 0x7c, 0x75, 0x8a, 0x35, 0x44, 0x87,
               0x90, 0xa4, 0x91, 0x3f, 0x1f, 0xa6, 0x76, 0x01},
      .timestamp = 0,
      .duration = 0,
      .type = TimelineItemTypeNotification
    }
  };

  cl_assert(E_INVALID_ARGUMENT == reminder_db_insert_item(&not_a_reminder));
}

void test_reminder_db__delete_parent(void) {
  prv_insert_default_reminders();

  const TimelineItemId *parent_id = &item1.header.parent_id;
  // confirm the two are here
  cl_assert(reminder_db_get_len((uint8_t *)&item1.header.id, sizeof(Uuid)) > 0);
  cl_assert(reminder_db_get_len((uint8_t *)&item2.header.id, sizeof(Uuid)) > 0);
  // remove the two that share a parent
  cl_assert_equal_i(reminder_db_delete_with_parent(parent_id), S_SUCCESS);
  // confirm the two are gone
  cl_assert(reminder_db_get_len((uint8_t *)&item1.header.id, sizeof(Uuid)) == 0);
  cl_assert(reminder_db_get_len((uint8_t *)&item2.header.id, sizeof(Uuid)) == 0);
  // confirm the others are still here
  cl_assert(reminder_db_get_len((uint8_t*)&item3.header.id, sizeof(Uuid)) > 0);
  cl_assert(reminder_db_get_len((uint8_t*)&item4.header.id, sizeof(Uuid)) > 0);
}

void test_reminder_db__bad_item(void) {
  cl_assert(S_SUCCESS != reminder_db_insert((uint8_t *)&bad_item.common.id, UUID_SIZE, (uint8_t *)&bad_item, sizeof(bad_item)));
}

void test_reminder_db__read_nonexistant(void) {
  TimelineItem item = {{{0}}};
  cl_assert_equal_i(E_DOES_NOT_EXIST, reminder_db_read_item(&item, &bad_item.common.id));
}

void test_reminder_db__find_by_timestamp_title(void) {
  prv_insert_default_reminders();

  // Add items with title attributes for searching (out of order for worst-case scenario)
  cl_assert(S_SUCCESS == reminder_db_insert_item(&title_item2));
  cl_assert(S_SUCCESS == reminder_db_insert_item(&title_item1));

  TimelineItem reminder;

  // Test non-matching title and timestamp
  cl_assert_equal_b(reminder_db_find_by_timestamp_title(0, "nonexistent title", NULL, &reminder),
                    false);

  // Test matching timstamp, but not title
  cl_assert_equal_b(reminder_db_find_by_timestamp_title(title_item1.header.timestamp,
      "nonexistent title", NULL, &reminder), false);

  // Test matching title, but not timestamp
  cl_assert_equal_b(reminder_db_find_by_timestamp_title(0,
      title_item1.attr_list.attributes[0].cstring, NULL, &reminder), false);

  // Confirm proper item is returned for search criteria
  cl_assert_equal_b(reminder_db_find_by_timestamp_title(title_item1.header.timestamp,
      title_item1.attr_list.attributes[0].cstring, NULL, &reminder), true);
  cl_assert(uuid_equal(&reminder.header.id, &title_item1.header.id));
}

void test_reminder_db__is_dirty_insert_from_phone(void) {
  // Insert a bunch of reminders "from the phone"
  // They should NOT be dirty (the phone is the source of truth)
  reminder_db_insert((uint8_t *)&item1.header.id, sizeof(TimelineItemId),
                     (uint8_t *)&item1, sizeof(TimelineItem));
  reminder_db_insert((uint8_t *)&item2.header.id, sizeof(TimelineItemId),
                     (uint8_t *)&item2, sizeof(TimelineItem));
  reminder_db_insert((uint8_t *)&item3.header.id, sizeof(TimelineItemId),
                     (uint8_t *)&item3, sizeof(TimelineItem));
  reminder_db_insert((uint8_t *)&item4.header.id, sizeof(TimelineItemId),
                     (uint8_t *)&item4, sizeof(TimelineItem));

  bool is_dirty = true;
  cl_assert_equal_i(reminder_db_is_dirty(&is_dirty), S_SUCCESS);
  cl_assert(!is_dirty);

  BlobDBDirtyItem *dirty_list = reminder_db_get_dirty_list();
  cl_assert(!dirty_list);
}

void test_reminder_db__is_dirty_insert_locally(void) {
  // Insert a bunch of reminders "from the watch"
  // These should be dirty (the phone is the source of truth)
  const int num_reminders = 4;
  reminder_db_insert_item(&item1);
  reminder_db_insert_item(&item2);
  reminder_db_insert_item(&item3);
  reminder_db_insert_item(&item4);

  bool is_dirty = false;
  cl_assert_equal_i(reminder_db_is_dirty(&is_dirty), S_SUCCESS);
  cl_assert(is_dirty);

  BlobDBDirtyItem *dirty_list = reminder_db_get_dirty_list();
  cl_assert(dirty_list);
  cl_assert(list_count((ListNode *)dirty_list) == num_reminders);

  // Mark some items as synced
  reminder_db_mark_synced((uint8_t *)&item1.header.id, sizeof(TimelineItemId));
  reminder_db_mark_synced((uint8_t *)&item3.header.id, sizeof(TimelineItemId));

  // We should now only have 2 dirty items
  cl_assert_equal_i(reminder_db_is_dirty(&is_dirty), S_SUCCESS);
  cl_assert(is_dirty);

  dirty_list = reminder_db_get_dirty_list();
  cl_assert(dirty_list);
  cl_assert_equal_i(list_count((ListNode *)dirty_list), 2);

  // Mark the final 2 items as synced
  reminder_db_mark_synced((uint8_t *)&item2.header.id, sizeof(TimelineItemId));
  reminder_db_mark_synced((uint8_t *)&item4.header.id, sizeof(TimelineItemId));

  // And nothing should be dirty
  cl_assert_equal_i(reminder_db_is_dirty(&is_dirty), S_SUCCESS);
  cl_assert(!is_dirty);

  dirty_list = reminder_db_get_dirty_list();
  cl_assert(!dirty_list);
}

void test_reminder_db__set_status_bits(void) {
  reminder_db_insert_item(&item1);
  SerializedTimelineItemHeader item;
  cl_must_pass(reminder_db_read((uint8_t *)&item1.header.id, sizeof(Uuid), (uint8_t *)&item,
                                sizeof(SerializedTimelineItemHeader)));
  cl_assert_equal_i(item.common.status & 0xFF, 0);

  cl_must_pass(reminder_db_set_status_bits(&item1.header.id, TimelineItemStatusReminded));
  cl_must_pass(reminder_db_read((uint8_t *)&item1.header.id, sizeof(Uuid), (uint8_t *)&item,
                                sizeof(SerializedTimelineItemHeader)));
  cl_assert_equal_i(item.common.status & 0xFF, TimelineItemStatusReminded);
}
