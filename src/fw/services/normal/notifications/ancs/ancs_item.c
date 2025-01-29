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

#include "ancs_item.h"

#include "ancs_notifications_util.h"

#include "applib/graphics/utf8.h"
#include "kernel/pbl_malloc.h"
#include "services/common/i18n/i18n.h"
#include "system/logging.h"
#include "util/string.h"

#include <stdio.h>

//! Fits the maximum string "sent an attachment" and i18n translations,
//! plus the emoji, newline and quotes when there is a text message in addition to media.
#define MULTIMEDIA_INDICATOR_LENGTH 64
#define MULTIMEDIA_EMOJI "🎁"

// AttributeIdTitle + AttributeIdAncsAction. See prv_fill_native_ancs_action
static const int NUM_NATIVE_ANCS_ACTION_ATTRS = 2;

static const char s_utf8_ellipsis[] = UTF8_ELLIPSIS_STRING;

static int prv_add_ellipsis(char *buffer, int length) {
  // Note: s_utf8_ellipsis is null terminated
  memcpy(&buffer[length], s_utf8_ellipsis, sizeof(s_utf8_ellipsis));
  return strlen(s_utf8_ellipsis);
}

static bool prv_should_add_sender_attr(const ANCSAttribute *app_id, const ANCSAttribute *title) {
  return ((ancs_notifications_util_is_sms(app_id) || ancs_notifications_util_is_phone(app_id)) &&
          title && title->length > 0);
}

//! @param buffer The buffer into which to copy the pstring attr. The buffer
//! is assumed to be large enough to contain the string plus an optional
//! ellipsis plus the zero terminator.
//! @param add_ellipsis True if ellipsis must be added to buffer
static char *prv_copy_pstring_and_add_ellipsis(const PascalString16 *pstring, char *buffer,
                                               bool add_ellipsis) {
  size_t bytes_added = pstring->str_length + 1;
  pstring_pstring16_to_string(pstring, buffer);
  if (add_ellipsis) {
    bytes_added += prv_add_ellipsis(buffer, pstring->str_length);
  }
  return buffer + bytes_added;
}

static size_t prv_max_ellipsified_cstring_size(const ANCSAttribute *attr) {
  if ((attr == NULL) || (attr->length == 0)) {
    return 0;
  }
  return (size_t) attr->length + strlen(s_utf8_ellipsis) + 1 /* zero terminator */;
}

static uint8_t *prv_add_pstring_to_attribute(uint8_t *buffer, const ANCSAttribute *ancs_attr,
                                             int max_length, Attribute *attribute,
                                             AttributeId attribute_id) {
  attribute_init_string(attribute, (char *)buffer, attribute_id);
  return (uint8_t *)prv_copy_pstring_and_add_ellipsis(&ancs_attr->pstr, (char *)buffer,
                                                      (ancs_attr->length == max_length));
}

static uint8_t *prv_add_action_msg_to_attribute(
    uint8_t *buffer, const ANCSAttribute *sender, int sender_max_length,
    const ANCSAttribute *caption, int caption_max_length, const char *action_msg,
    Attribute *attribute, AttributeId attribute_id) {
  // Sets an attribute to <sender> ' ' <action_msg> ( '\n' '"' <caption> '"' )
  // For example, sender="Huy Tran", action_msg="sent an attachment", caption="Check this out!"
  // The attribute becomes 'Huy Tran sent an attachment\n"Check this out!"'
  attribute_init_string(attribute, (char *)buffer, attribute_id);

  const char *stripped_caption = NULL;
  char caption_buf[caption ? caption->length + 1 : 0];
  if (caption && caption->length > 0) {
    pstring_pstring16_to_string(&caption->pstr, caption_buf);
    // Inserting a caption to an image can easily cause accidental leading whitespace
    stripped_caption = string_strip_leading_whitespace(caption_buf);
  }

  const size_t max_msg_length =
      sender->length + (stripped_caption ? strlen(stripped_caption) : 0) +
      MULTIMEDIA_INDICATOR_LENGTH + strlen(s_utf8_ellipsis) + 1;

  // Sender and action message
  int pos = snprintf((char *)buffer, max_msg_length, "%.*s %s",
                     sender->length, (char *)sender->value, action_msg);
  if (pos < (int)max_msg_length && stripped_caption && !IS_EMPTY_STRING(stripped_caption)) {
    // Quoted caption
    pos += snprintf((char *)buffer + pos, max_msg_length - pos, "\n\"%s\"",
                    stripped_caption);
    if (caption->length == caption_max_length) {
      // Overwrite the last quote with ellipsis
      const size_t quote_len = 1;
      pos += prv_add_ellipsis((char *)buffer, pos - quote_len);
    }
  }
  return buffer + pos + 1;
}

static int prv_set_multimedia_action_msg(char *buffer, size_t length) {
#if PLATFORM_TINTIN
  const char *emoji_str = "";
#else
  const char *emoji_str = PBL_IF_RECT_ELSE(" " MULTIMEDIA_EMOJI, "\n" MULTIMEDIA_EMOJI);
#endif
  return snprintf(buffer, length, "%s%s", i18n_get("sent an attachment", __FILE__),
                  emoji_str);
}

//! @param buffer Pointer to a buffer large enough to hold all attribute strings required by
//! the action. If buffer points to null, a new buffer will be allocated
//! @return Pointer to the end of the buffer (*buffer + size of strings)
static uint8_t *prv_fill_native_ancs_action(uint8_t **buffer,
                                            TimelineItemAction *action,
                                            ActionId ancs_action_id,
                                            const ANCSAttribute *title,
                                            const ANCSAttribute *app_id,
                                            ANCSProperty properties) {
  const bool is_phone_app = ancs_notifications_util_is_phone(app_id);
  const bool is_voice_mail = (is_phone_app && (properties & ANCSProperty_VoiceMail));
  if (ancs_action_id == ActionIDNegative) {
    action->type = is_voice_mail ? TimelineItemActionTypeAncsDelete :
                                   TimelineItemActionTypeAncsNegative;
  } else {
    action->type = is_phone_app ? TimelineItemActionTypeAncsDial :
                                  TimelineItemActionTypeAncsPositive;
  }

  action->attr_list.attributes[0].id = AttributeIdAncsAction;
  action->attr_list.attributes[0].uint8 = ancs_action_id;

  // Allocate a new buffer if none provided
  if (!(*buffer)) {
    *buffer = task_malloc_check(prv_max_ellipsified_cstring_size(title));
  }

  uint8_t *rv = prv_add_pstring_to_attribute(*buffer, title, ACTION_MAX_LENGTH,
                                             &action->attr_list.attributes[1], AttributeIdTitle);

  // We want to rename the "Clear" action to "Dismiss"
  if (strcmp(action->attr_list.attributes[1].cstring, "Clear") == 0) {
    // TODO: PBL-23915
    // We leak this i18n'd string because not leaking it is really hard.
    // We make sure we only ever allocate it once though, so it's not the end of the world.
    action->attr_list.attributes[1].cstring = (char*)i18n_get("Dismiss", __FILE__);
  }

  // We want to rename the "Dial" action to "Call Back"
  if (strcmp(action->attr_list.attributes[1].cstring, "Dial") == 0) {
    // TODO: PBL-23915
    // We leak this i18n'd string because not leaking it is really hard.
    // We make sure we only ever allocate it once though, so it's not the end of the world.
    action->attr_list.attributes[1].cstring = (char*)i18n_get("Call Back", __FILE__);
  }

  return rv;
}

static uint8_t *prv_fill_pebble_ancs_action(uint8_t **buffer,
                                            uint8_t *buf_end,
                                            TimelineItemAction *action,
                                            TimelineItemAction *pbl_action) {
  action->type = pbl_action->type;
  action->id = pbl_action->id;
  Attribute *cur_attribute = &action->attr_list.attributes[0];

  for (int i = 0; i < pbl_action->attr_list.num_attributes; i++) {
    attribute_copy(cur_attribute, &pbl_action->attr_list.attributes[i], buffer, buf_end);
    cur_attribute++;
  }

  return *buffer;
}

static void prv_populate_attributes(TimelineItem *item,
                                    uint8_t **buffer,
                                    const ANCSAttribute *title,
                                    const ANCSAttribute *display_name,
                                    const ANCSAttribute *subtitle,
                                    const ANCSAttribute *message,
                                    const ANCSAttribute *app_id,
                                    const ANCSAppMetadata *app_metadata,
                                    bool has_multimedia) {
  int attr_idx = 0;

  if (prv_should_add_sender_attr(app_id, title)) {
    // Copy the title into the sender attribute so we don't lose it in the multimedia case
    *buffer = prv_add_pstring_to_attribute(*buffer, title, TITLE_MAX_LENGTH,
                                           &item->attr_list.attributes[attr_idx],
                                           AttributeIdSender);
    attr_idx++;
  }

  // The sender is in the title for iMessage
  const ANCSAttribute *sender = title;
  if (has_multimedia) {
    // Move the title (sender) to the body for MMS
    if (ancs_notifications_util_is_group_sms(app_id, subtitle)) {
      // Promote the subtitle (group name) for Group MMS
      title = subtitle;
      subtitle = NULL;
    } else {
      title = NULL;
    }
  }

  if (title && title->length > 0) {
    *buffer = prv_add_pstring_to_attribute(*buffer, title, TITLE_MAX_LENGTH,
                                           &item->attr_list.attributes[attr_idx],
                                           AttributeIdTitle);
    attr_idx++;
  }
  if (display_name && display_name->length > 0) {
    *buffer = prv_add_pstring_to_attribute(*buffer, display_name, TITLE_MAX_LENGTH,
                                           &item->attr_list.attributes[attr_idx],
                                           AttributeIdAppName);
    attr_idx++;
  }
  if (subtitle && subtitle->length > 0) {
    *buffer = prv_add_pstring_to_attribute(*buffer, subtitle, SUBTITLE_MAX_LENGTH,
                                           &item->attr_list.attributes[attr_idx],
                                           AttributeIdSubtitle);
    attr_idx++;
  }
  if (sender && sender->length > 0 && has_multimedia) {
    char action_msg[MULTIMEDIA_INDICATOR_LENGTH];
    prv_set_multimedia_action_msg(action_msg, sizeof(action_msg));
    *buffer = prv_add_action_msg_to_attribute(*buffer, sender, TITLE_MAX_LENGTH,
                                              message, MESSAGE_MAX_LENGTH,
                                              action_msg, &item->attr_list.attributes[attr_idx],
                                              AttributeIdBody);
    attr_idx++;
  } else if (message && message->length > 0) {
    *buffer = prv_add_pstring_to_attribute(*buffer, message, MESSAGE_MAX_LENGTH,
                                           &item->attr_list.attributes[attr_idx],
                                           AttributeIdBody);
    attr_idx++;
  }
  if (app_id && app_id->length > 0) {
    *buffer = prv_add_pstring_to_attribute(*buffer, app_id, APP_ID_MAX_LENGTH,
                                           &item->attr_list.attributes[attr_idx],
                                           AttributeIdiOSAppIdentifier);
    attr_idx++;
  }

  // add the icon attribute
  item->attr_list.attributes[attr_idx].id = AttributeIdIconTiny;
  item->attr_list.attributes[attr_idx].uint32 = app_metadata->icon_id;
  attr_idx++;

#if PBL_COLOR
  if (app_metadata->app_color != 0) {
    item->attr_list.attributes[attr_idx].id = AttributeIdBgColor;
    item->attr_list.attributes[attr_idx].uint8 = app_metadata->app_color;
  }
#endif
}

static bool prv_should_hide_reply_because_group_sms(const TimelineItemAction *action,
                                                    const ANCSAttribute *app_id,
                                                    const ANCSAttribute *subtitle) {
  if (action->type != TimelineItemActionTypeAncsResponse) {
    return false;
  }

  return ancs_notifications_util_is_group_sms(app_id, subtitle);
}

static void prv_populate_actions(TimelineItem *item,
                                 uint8_t **buffer,
                                 uint8_t *buf_end,
                                 const ANCSAttribute *positive_action,
                                 const ANCSAttribute *negative_action,
                                 const ANCSAttribute *subtitle,
                                 const ANCSAttribute *app_id,
                                 const TimelineItemActionGroup *pebble_actions,
                                 ANCSProperty properties) {
  TimelineItemAction *action = item->action_group.actions;

  // The order the actions get filled is important. See comment in ancs_item_create_and_populate
  if (positive_action) {
    *buffer = prv_fill_native_ancs_action(buffer, action, ActionIDPositive,
                                          positive_action, app_id, properties);
    action++;
  }
  if (negative_action) {
    *buffer = prv_fill_native_ancs_action(buffer, action, ActionIDNegative,
                                          negative_action, app_id, properties);
    action++;
  }
  if (pebble_actions) {
    for (int i = 0; i < pebble_actions->num_actions; i++) {
      TimelineItemAction *pbl_action = &pebble_actions->actions[i];
      if (prv_should_hide_reply_because_group_sms(pbl_action, app_id, subtitle)) {
        continue;
      }

      *buffer = prv_fill_pebble_ancs_action(buffer, buf_end, action, pbl_action);
      action++;
    }
  }
}

TimelineItem *ancs_item_create_and_populate(ANCSAttribute *notif_attributes[],
                                            ANCSAttribute *app_attributes[],
                                            const ANCSAppMetadata *app_metadata,
                                            iOSNotifPrefs *notif_prefs,
                                            time_t timestamp,
                                            ANCSProperty properties) {
  const ANCSAttribute *app_id = notif_attributes[FetchedNotifAttributeIndexAppID];
  const ANCSAttribute *display_name = app_attributes[FetchedAppAttributeIndexDisplayName];
  const ANCSAttribute *title = notif_attributes[FetchedNotifAttributeIndexTitle];
  const bool has_multimedia = (ancs_notifications_util_is_sms(app_id) &&
                               (properties & ANCSProperty_MultiMedia));

  if (display_name) {
    // dedupe title & display name, they often are the same
    if (pstring_equal(&display_name->pstr, &title->pstr)) {
      title = NULL;
    }

    // Hide display name if we have custom app metadata for this app.
    // If the app_metadata, does not have a name, then we have the generic app metadata.
    if (app_metadata->app_id) {
      display_name = NULL;
    }
  }

  const ANCSAttribute *subtitle = notif_attributes[FetchedNotifAttributeIndexSubtitle];
  const ANCSAttribute *message = notif_attributes[FetchedNotifAttributeIndexMessage];

  // Action labels (optional)
  const ANCSAttribute *positive_action =
      notif_attributes[FetchedNotifAttributeIndexPositiveActionLabel];
  if (positive_action && positive_action->length == 0) {
    positive_action = NULL;
  }
  const ANCSAttribute *negative_action =
      notif_attributes[FetchedNotifAttributeIndexNegativeActionLabel];
  if (negative_action && negative_action->length == 0) {
    negative_action = NULL;
  }

  // See if we support any additional actions (beyond what ANCS supports) for this type of notif
  if (app_id && app_id->length == 0) {
    app_id = NULL;
  }

  // At this point we know that the attributes we have extracted are valid and the sizes thereof
  // can be trusted (no error from ancs_util_attr_ptrs). If the length of any of the strings is the
  // max allowed by ANCS, assume that the message has been truncated and add an ellipsis to the
  // string

  size_t required_space_for_strings = 0;

  required_space_for_strings += prv_max_ellipsified_cstring_size(title);
  required_space_for_strings += prv_max_ellipsified_cstring_size(display_name);
  required_space_for_strings += prv_max_ellipsified_cstring_size(subtitle);
  if (has_multimedia) {
    required_space_for_strings += MULTIMEDIA_INDICATOR_LENGTH;
  }
  required_space_for_strings += prv_max_ellipsified_cstring_size(message);
  required_space_for_strings += prv_max_ellipsified_cstring_size(positive_action);
  required_space_for_strings += prv_max_ellipsified_cstring_size(negative_action);
  required_space_for_strings += prv_max_ellipsified_cstring_size(app_id);

  if (prv_should_add_sender_attr(app_id, title)) {
    required_space_for_strings += prv_max_ellipsified_cstring_size(title);
  }

  int num_pebble_actions = 0;
  if (notif_prefs) {
    for (int i = 0; i < notif_prefs->action_group.num_actions; i++) {
      TimelineItemAction *action = &notif_prefs->action_group.actions[i];
      if (prv_should_hide_reply_because_group_sms(action, app_id, subtitle)) {
        continue;
      }
      required_space_for_strings += attribute_list_get_buffer_size(&action->attr_list);
      num_pebble_actions++;
    }
  }

  int num_native_actions = (positive_action ? 1 : 0) + (negative_action ? 1 : 0);
  int num_actions = num_native_actions + num_pebble_actions;

  const int max_num_actions = 8; // Arbitrarily chosen
  uint8_t attributes_per_action[max_num_actions];
  int action_idx = 0;

  // Order of actions: ANCS positive, ANCS negative, Custom Pebble Actions
  // The order the actions get populated in prv_populate_actions must be the same
  if (positive_action) {
    attributes_per_action[action_idx++] = NUM_NATIVE_ANCS_ACTION_ATTRS;
  }
  if (negative_action) {
    attributes_per_action[action_idx++] = NUM_NATIVE_ANCS_ACTION_ATTRS;
  }
  if (notif_prefs) {
    for (int i = 0; i < notif_prefs->action_group.num_actions; i++) {
      TimelineItemAction *action = &notif_prefs->action_group.actions[i];
      if (prv_should_hide_reply_because_group_sms(action, app_id, subtitle)) {
        continue;
      }

      attributes_per_action[action_idx++] = action->attr_list.num_attributes;
    }
  }

  int num_attr = ((title && title->length > 0 && !has_multimedia) ? 1 : 0) +
                 ((display_name && display_name->length > 0) ? 1 : 0) +
                 ((subtitle->length > 0) ? 1 : 0) +
                 ((app_id->length > 0) ? 1 : 0) +
                 ((message->length > 0 || has_multimedia) ? 1 : 0) +
                 (prv_should_add_sender_attr(app_id, title) ? 1 : 0) +
#if PBL_COLOR
                 ((app_metadata->app_color != 0) ? 1 : 0) + // for color
#endif
                 1; // for icon

  uint8_t *buffer;
  TimelineItem *item = timeline_item_create(num_attr, num_actions, attributes_per_action,
                                            required_space_for_strings, &buffer);

  if (!item) {
    // Out of memory - we do not croak on out of memory for notifications (PBL-10521)
    PBL_LOG(LOG_LEVEL_WARNING, "Ignoring ANCS notification (out of memory)");
    return NULL;
  }

  item->header.timestamp = timestamp;

  prv_populate_attributes(item, &buffer, title, display_name, subtitle, message, app_id,
                          app_metadata, has_multimedia);

  uint8_t *buf_end = buffer + required_space_for_strings;
  prv_populate_actions(item, &buffer, buf_end, positive_action, negative_action, subtitle,
                       app_id, notif_prefs ? &notif_prefs->action_group : NULL, properties);

  return item;
}

// Replace the dismiss action of a timeline item with the ancs negative action
void ancs_item_update_dismiss_action(TimelineItem *item, uint32_t uid,
                                      const ANCSAttribute *attr_action_neg) {
  TimelineItemAction *dismiss = timeline_item_find_dismiss_action(item);

  if (dismiss) {
    attribute_list_init_list(NUM_NATIVE_ANCS_ACTION_ATTRS + 1, &dismiss->attr_list);

    uint8_t *string_buffer = NULL;
    prv_fill_native_ancs_action(&string_buffer, dismiss, ActionIDNegative, attr_action_neg,
                                NULL, ANCSProperty_None);

    // Add ancs ID as attribute since reminder's parent needs to be the associated pin
    dismiss->attr_list.attributes[NUM_NATIVE_ANCS_ACTION_ATTRS].id = AttributeIdAncsId;
    dismiss->attr_list.attributes[NUM_NATIVE_ANCS_ACTION_ATTRS].uint32 = uid;

    // Copy the timeline item to move the new action back into the single buffer
    TimelineItem *new_item = timeline_item_copy(item);

    task_free(string_buffer);
    attribute_list_destroy_list(&dismiss->attr_list);

    timeline_item_free_allocated_buffer(item);
    *item = *new_item;
    new_item->allocated_buffer = NULL;
    timeline_item_destroy(new_item);
  }
}
