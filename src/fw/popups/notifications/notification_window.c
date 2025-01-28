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

#include "notification_window.h"

#include "notifications_presented_list.h"
#include "notification_window_private.h"

#include "applib/fonts/fonts.h"
#include "applib/ui/action_button.h"
#include "applib/ui/action_menu_window.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/dialogs/dialog.h"
#include "applib/ui/dialogs/expandable_dialog.h"
#include "applib/ui/dialogs/simple_dialog.h"
#include "applib/ui/ui.h"
#include "applib/ui/window.h"
#include "applib/ui/window_manager.h"
#include "applib/ui/window_stack.h"
#include "apps/system_apps/timeline/peek_layer.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/modals/modal_manager.h"
#include "os/mutex.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource_ids.auto.h"
#include "services/common/analytics/analytics.h"
#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/common/comm_session/session.h"
#include "services/common/evented_timer.h"
#include "services/common/i18n/i18n.h"
#include "services/common/light.h"
#include "services/common/regular_timer.h"
#include "services/normal/blob_db/ios_notif_pref_db.h"
#include "services/normal/blob_db/pin_db.h"
#include "services/normal/blob_db/reminder_db.h"
#include "services/normal/notifications/alerts.h"
#include "services/normal/notifications/alerts_preferences.h"
#include "services/normal/notifications/alerts_private.h"
#include "services/normal/notifications/ancs/ancs_filtering.h"
#include "services/normal/notifications/do_not_disturb.h"
#include "services/normal/notifications/notification_storage.h"
#include "services/normal/notifications/notification_types.h"
#include "services/normal/notifications/notifications.h"
#include "services/normal/timeline/attribute.h"
#include "services/normal/timeline/notification_layout.h"
#include "services/normal/timeline/swap_layer.h"
#include "services/normal/timeline/timeline.h"
#include "services/normal/timeline/timeline_actions.h"
#include "services/normal/timeline/timeline_resources.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"
#include "util/trig.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if CAPABILITY_HAS_VIBE_SCORES
#include "services/normal/vibes/vibe_client.h"
#include "services/normal/vibes/vibe_score.h"
#endif

#define NOTIFICATION_PRIORITY (ModalPriorityNotification)

#define NUM_MOOOK_SOFT_MID_FRAMES PBL_IF_RECT_ELSE(6, 4)

#define FIRST_PEEK_DELAY PBL_IF_RECT_ELSE(500, 200)

// pop timer for window. Refreshed during any point of activity (button clicks)
static const unsigned int QUICK_DND_HOLD_MS = 800;

T_STATIC NotificationWindowData s_notification_window_data;

static bool s_in_use = false;
PebbleMutex *s_notification_window_mutex;

static bool prv_should_provide_action_menu_for_item(NotificationWindowData *data,
                                                    const TimelineItem *item);

static void prv_handle_notification_removed_common(Uuid *, NotificationType);

static bool prv_should_pop_due_to_inactivity(void);

/////////////////////
// Helpers
/////////////////////

static AlertType prv_alert_type_for_notification_type(NotificationType type) {
  switch (type) {
    case NotificationMobile:
      return AlertMobile;
    case NotificationPhoneCall:
      return AlertPhoneCall;
    case NotificationOther:
      return AlertOther;
    case NotificationReminder:
      return AlertReminder;
    default:
      return AlertInvalid;
  }
}

static void prv_toggle_dnd_from_back_click(ClickRecognizerRef recognizer, void *data) {
  do_not_disturb_manual_toggle_with_dialog();
}

static void prv_toggle_dnd_from_action_menu(ActionMenu *action_menu,
                                            const ActionMenuItem *item,
                                            void *context) {
  // This function handles first-time use tutorial logic
  do_not_disturb_toggle_manually_enabled(ManualDNDFirstUseSourceActionMenu);
}

static TimelineItem *prv_get_current_notification(NotificationWindowData *data) {
  if (!notifications_presented_list_current()) {
    return NULL;
  }

  LayoutLayer *current = swap_layer_get_current_layout(&data->swap_layer);
  TimelineItem *item = (TimelineItem *)layout_get_context(current);
  return item;
}

static void prv_draw_dnd_icon(struct Layer *layer, GContext *ctx) {
  if (!s_in_use) {
    return;
  }

  NotificationWindowData *data = &s_notification_window_data;
  if (!data->dnd_icon_visible) {
    return;
  }

  graphics_context_set_tint_color(ctx, status_bar_layer_get_foreground_color(&data->status_layer));
  graphics_context_set_compositing_mode(ctx, GCompOpTint);
  graphics_draw_bitmap_in_rect(ctx, &data->dnd_icon, &data->dnd_icon.bounds);
}

static void prv_update_status_layer(NotificationWindowData *data) {
  const int notif_count = notifications_presented_list_count();
  if (notif_count <= 1) {
    // if less than one notification, clear the status bar info
    status_bar_layer_reset_info(&data->status_layer);
  } else {
    // if more than one, then show the current index in relation to the total number
    status_bar_layer_set_info_progress(&data->status_layer,
                                   notifications_presented_list_current_idx() + 1,
                                   notif_count);
  }
}

static void prv_cleanup_timer(EventedTimerID *timer_id) {
  if (*timer_id != EVENTED_TIMER_INVALID_ID) {
    evented_timer_cancel(*timer_id);
    *timer_id = EVENTED_TIMER_INVALID_ID;
  }
}

static void prv_cancel_reminder_watchdog(NotificationWindowData *data) {
  if (regular_timer_is_scheduled(&data->reminder_watchdog_timer_id)) {
    regular_timer_remove_callback(&data->reminder_watchdog_timer_id);
  }
}

static void prv_cleanup_timers(NotificationWindowData *data) {
  prv_cleanup_timer(&data->pop_timer_id);
  prv_cancel_reminder_watchdog(data);
  prv_cleanup_timer(&data->peek_layer_timer);
}

static void prv_pop_notification_window(NotificationWindowData *data) {
  if (data->window_frozen || !s_in_use) {
    return;
  }

  // This calls through to our window_unload() callback, which cancels our timer and clears s_in_use
  window_stack_remove(&data->window, true /* animated */);
}

static int prv_reminders_on_top_comparator(void *a, void *b) {
  NotifList *notif_a = (NotifList*) a;
  NotifList *notif_b = (NotifList*) b;
  NotificationType type_a = notif_a->notif.type;
  NotificationType type_b = notif_b->notif.type;

  // Reminders come first, then everything else. More recent reminders should appear before older
  // reminders and more recent notifications should appear before older notifications
  if (type_b == NotificationReminder) {
    return 1;
  } else if (type_b != NotificationReminder && type_a != NotificationReminder) {
    return 1;
  } else {
    return -1;
  }
}

static void prv_notification_window_add_notification(Uuid *id, NotificationType type) {
  if (do_not_disturb_is_active()) {
    notifications_presented_list_add_sorted(id, type, prv_reminders_on_top_comparator, false);
  } else {
    notifications_presented_list_add(id, type);
  }
}

static void prv_reload_swap_layer(NotificationWindowData *data) {
  // If the action menu is on the screen, then don't reload the swap layer.
  // The action menu's context is just a pointer to the swap layer's layout layer's context.
  // Reloading the swap layer will give the action menu a bogus timeline item pointer.
  // Also if the action menu is up, we don't need to reload the swap layer until the
  // notification window appears again
  if (data->action_menu && window_is_loaded((Window *)data->action_menu)) {
    data->notifications_modified = true;
  } else {
    swap_layer_reload_data(&data->swap_layer);
  }
}

/////////////////////
// Dismiss All
/////////////////////

static void prv_handle_dismiss_all_complete(bool succeeded, void *cb_data) {
  NotificationWindowData *window_data = (NotificationWindowData*) cb_data;
  window_data->window_frozen = false;
  if (s_in_use && succeeded) {
    prv_pop_notification_window(window_data);
  }
}

static void prv_dismiss_all(void *data, ActionMenu *action_menu) {
  NotificationWindowData *window_data = (NotificationWindowData*) data;

  const int num_notifications = notifications_presented_list_count();
  if (num_notifications == 0) {
    return;
  }

  Uuid *first_id = notifications_presented_list_first();

  NotificationInfo *notif_list = kernel_malloc_check(sizeof(NotificationInfo) * num_notifications);

  for (int i = 0; i < num_notifications; i++) {
    Uuid *id = notifications_presented_list_relative(first_id, i);
    memcpy(&notif_list[i].id, id, sizeof(Uuid));
    notif_list[i].type = notifications_presented_list_get_type(id);
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Dismissing %d notifications", num_notifications);
  window_data->window_frozen = true;
  timeline_actions_dismiss_all(notif_list,
                               num_notifications,
                               action_menu,
                               prv_handle_dismiss_all_complete,
                               data);

  kernel_free(notif_list);
}

static void prv_dismiss_all_action_cb(ActionMenu *action_menu,
                                      const ActionMenuItem *item,
                                      void *context) {
  NotificationWindowData *window_data = (NotificationWindowData*) item->action_data;
  prv_dismiss_all(window_data, action_menu);
}

static int64_t prv_interpolate_moook_peek_animation(int32_t normalized, int64_t from, int64_t to) {
  return interpolate_moook_soft(normalized, from, to, NUM_MOOOK_SOFT_MID_FRAMES);
}

// scroll = true, then scroll the layer up dy
// scroll = false, then shrink the layer's size by dy
static Animation *prv_create_anim_frame(Layer *layer, int16_t dy, bool scroll) {
  GRect *start = &layer->frame;
  GRect stop = *start;
  if (scroll) {
    stop.origin.y += dy;
  } else {
    stop.size.h += dy;
  }

  PropertyAnimation *prop_anim = property_animation_create_layer_frame(layer, start, &stop);
  Animation *animation = property_animation_get_animation(prop_anim);
  animation_set_duration(animation, interpolate_moook_soft_duration(NUM_MOOOK_SOFT_MID_FRAMES));
  animation_set_custom_interpolation(animation, prv_interpolate_moook_peek_animation);
  return animation;
}

/////////////////////
// Peek Layer
/////////////////////
static void prv_peek_anim_stopped(Animation *animation, bool finished, void *context) {
  NotificationWindowData *data = context;
  data->first_notif_loaded = true;
  peek_layer_destroy(data->peek_layer);
  data->peek_layer = NULL;
  TimelineItem *item = prv_get_current_notification(data);
  layer_set_hidden((Layer *)&data->action_button_layer,
                   !prv_should_provide_action_menu_for_item(data, item));
}

static void prv_hide_peek_layer(void *context) {
  NotificationWindowData *data = context;

  // get the frame of the swap_layer and set its destination
  const GRect *swap_frame = &data->swap_layer.layer.frame;
  int16_t swap_frame_animation_dy = STATUS_BAR_LAYER_HEIGHT - swap_frame->origin.y;

  // duration of animation of both peek layer and swap layer moving up to the top
  int16_t peek_frame_animation_dy = swap_frame_animation_dy;
#if PBL_ROUND
  // Needed because the peek layer's background and the screen have different sizes,
  // so the peek layer needs to move a different number of pixels vs the swap layer
  const int16_t peek_circle_vertical_offset = (BANNER_CIRCLE_RADIUS - (DISP_ROWS / 2)) / 2;
  peek_frame_animation_dy -= peek_circle_vertical_offset;
#endif
  Animation *peek_up = prv_create_anim_frame((Layer *)data->peek_layer, peek_frame_animation_dy,
                                             false /* scroll */);
  Animation *swap_up = prv_create_anim_frame((Layer *)&data->swap_layer, swap_frame_animation_dy,
                                             true /* scroll */);
  Animation *spawn = animation_spawn_create(peek_up, swap_up, NULL);
  AnimationHandlers anim_handlers = {
    .started = NULL,
    .stopped = prv_peek_anim_stopped,
  };
  animation_set_handlers(spawn, anim_handlers, data);

  // move the icon to where it should be in the swap_layer's notification_layout
  PeekLayer *peek_layer = data->peek_layer;
  GRect frame = peek_layer->layer.frame;
  frame.origin.x = (frame.size.w / 2) - (NOTIFICATION_TINY_RESOURCE_SIZE.w / 2);
  frame.origin.y = CARD_ICON_UPPER_PADDING + STATUS_BAR_LAYER_HEIGHT;
  frame.size = NOTIFICATION_TINY_RESOURCE_SIZE;

  const bool align_in_frame = true;
  peek_layer_set_scale_to_image(peek_layer, &data->peek_icon_info, TimelineResourceSizeTiny, frame,
                                align_in_frame);

  // set peek_layer clips to true so I can resize the peek_layer's background
  layer_set_clips(&peek_layer->layer, true);

  peek_layer_play(peek_layer);
  data->peek_animation = spawn;
  animation_schedule(spawn);
}


static void prv_play_peek_layer(void *context) {
  NotificationWindowData *data = context;
  // play the peek layer unfold sequence
  peek_layer_play(data->peek_layer);
  const uint16_t PEEK_LAYER_HIDE_DELAY = PBL_IF_RECT_ELSE(500, 400);
  data->peek_layer_timer = evented_timer_register_or_reschedule(data->peek_layer_timer,
                                                                PEEK_LAYER_HIDE_DELAY,
                                                                prv_hide_peek_layer,
                                                                data);
}

static void prv_show_peek_for_notification(NotificationWindowData *data, Uuid *id,
                                           bool is_first_notification) {
  // reload everything, doesn't matter since it will be covered by the peek layer
  notification_window_focus_notification(id, false);

  // if the peek animation is already in progress, we've done all we need
  // data->peek_layer is only ever not null between the start and end of the peek
  // animation; it's cleaned up by prv_peek_anim_stopped, and initialized here
  if (data->peek_layer) {
    return;
  }
  // get root layer of window and make the peek layer the full size
  const GRect *peek_layer_frame = &window_get_root_layer(&data->window)->frame;
  data->peek_layer = peek_layer_create(*peek_layer_frame);
  if (!data->peek_layer) {
    if (is_first_notification) {
      // we don't have enough memory, no peek. Just push the modal window.
      modal_window_push(&data->window, NOTIFICATION_PRIORITY, true /* animated */);
    }
    return;
  }

  // get the current layout so we can get the color and icon
  LayoutLayer *layout = swap_layer_get_current_layout(&data->swap_layer);
  if (!layout) {
    return;
  }

  // Get color and icon
  const LayoutColors *colors = layout_get_notification_colors(layout);
  TimelineItem *item = prv_get_current_notification(data);
  TimelineResourceId timeline_res_id;
  const TimelineResourceId fallback_icon_id = notification_layout_get_fallback_icon_id(
      item->header.type);
  timeline_res_id = attribute_get_uint32(&item->attr_list, AttributeIdIconTiny,
                                         fallback_icon_id);

  data->peek_icon_info = (TimelineResourceInfo) {
    .res_id = timeline_res_id,
    .app_id = &data->notification_app_id, // This is set earlier when we reload the layout
    .fallback_id = fallback_icon_id,
  };
  peek_layer_set_icon(data->peek_layer, &data->peek_icon_info);
  peek_layer_set_background_color(data->peek_layer, colors->bg_color);

  // This is so that only the banner of the swap_layer is sticking out from the bottom
  GRect swap_frame = ((Layer *)&data->swap_layer)->frame;
  swap_frame.origin.y = swap_frame.origin.y + swap_frame.size.h -
      PBL_IF_RECT_ELSE(LAYOUT_BANNER_HEIGHT_RECT, LAYOUT_TOP_BANNER_HEIGHT_ROUND);
  layer_set_frame((Layer *)&data->swap_layer, &swap_frame);

  // play the peek layer after the delay, more delay for the first notification
  // because we're coming from the compositor modal transition
  const uint16_t peek_layer_play_delay = is_first_notification ? FIRST_PEEK_DELAY : 100;
  data->peek_layer_timer = evented_timer_register_or_reschedule(data->peek_layer_timer,
                                                                peek_layer_play_delay,
                                                                prv_play_peek_layer,
                                                                data);

  // insert below status bar but above everything else.
  Window *window = &data->window;
  layer_add_child(window_get_root_layer(window), (Layer *)data->peek_layer);
  layer_insert_below_sibling((Layer *)data->peek_layer, (Layer *)&data->status_layer);
}

///////////////////////
// SwapLayer Callbacks
///////////////////////

static void prv_remove_notification(NotificationWindowData *data, Uuid *notif_id,
                                    const bool should_close_am) {
  // We have to check if the current presented notification is the one being
  // viewed.  If it is, then we check if we have an action menu and close it.
  // If we have an action menu and it's frozen we are waiting on an action result
  // from that menu; this will cleanup the action menu on completion anyway so don't do it here
  if (should_close_am && uuid_equal(notifications_presented_list_current(), notif_id) &&
      data->action_menu && ((Window *)data->action_menu)->on_screen &&
      !action_menu_is_frozen(data->action_menu)) {
    action_menu_close(data->action_menu, true);
  }

  // Setting the next ID is handled by the service
  notifications_presented_list_remove(notif_id);

  if ((notifications_presented_list_current_idx() < 0) && s_in_use)  {
    prv_pop_notification_window(data);
    return;
  }

  prv_reload_swap_layer(data);
}

static void prv_layout_removed_handler(SwapLayer *swap_layer, LayoutLayer *layout, void *context) {
  TimelineItem *item = layout_get_context(layout);
  timeline_item_destroy(item);
  layout_destroy(layout);
}

T_STATIC LayoutLayer *prv_get_layout_handler(SwapLayer *swap_layer, int8_t rel_position,
                                             void *context) {
  NotificationWindowData *data = context;
  Uuid *id = notifications_presented_list_relative(
      notifications_presented_list_current(), rel_position);

  // if no layers, don't return one
  if (uuid_is_invalid(id)) {
    return NULL;
  }

  NotificationType type = notifications_presented_list_get_type(id);

  TimelineItem *item = task_zalloc_check(sizeof(TimelineItem));

  if (type == NotificationMobile) {
    if (!notification_storage_get(id, item)) {
      PBL_LOG(LOG_LEVEL_ERROR, "Failed to read notification");
      goto cleanup;
    }
  } else if (type == NotificationReminder) {
    // validate reminder
    int rv = reminder_db_read_item(item, id);
    if (rv != S_SUCCESS) {
      PBL_LOG(LOG_LEVEL_ERROR, "Failed to read reminder");
      goto cleanup;
    }
  }

  // Determine if the icon isn't a system resource (meaning we have to load its associated app id)
  TimelineResourceId icon = attribute_get_uint32(&item->attr_list, AttributeIdIconTiny,
                                                 TIMELINE_RESOURCE_INVALID);
  TimelineItem pin;
  if (timeline_resources_is_system(icon) ||
      pin_db_read_item_header(&pin, &item->header.parent_id) != S_SUCCESS) {
    data->notification_app_id = (Uuid)UUID_INVALID;
  } else {
    data->notification_app_id = pin.header.parent_id;
  }

  const LayoutId layout_id = (type == NotificationMobile) ? LayoutIdNotification : LayoutIdReminder;
  NotificationLayoutInfo layout_info = (NotificationLayoutInfo) {
    .item = item,
    .show_notification_timestamp = !prv_should_pop_due_to_inactivity()
  };
  const LayoutLayerConfig config = {
    .frame = &data->window.layer.bounds,
    .attributes = &item->attr_list,
    .mode = LayoutLayerModeCard,
    .app_id = &data->notification_app_id,
    .context = &layout_info
  };
  NotificationLayout *notification_layout = (NotificationLayout *)layout_create(layout_id, &config);
  return &notification_layout->layout;

cleanup:
  timeline_item_destroy(item);
  return NULL;
}

//////////////////////
// Timer Functions
//////////////////////

static bool prv_should_pop_due_to_inactivity(void) {
  // If not a modal, then we are in the notification history app and the pop timer makes no sense
  // If in DND mode we want to keep the notifications on the screen
  return s_in_use && s_notification_window_data.is_modal && !do_not_disturb_is_active();
}

static void prv_pop_timer_callback(void *data) {
  NotificationWindowData *window_data = data;
  window_data->pop_timer_id = EVENTED_TIMER_INVALID_ID;

  // It's possible that our timeout expired at the same time the window was dismissed
  // through a button press or something like that. So, ignore this CALLBACK event posted
  // by our timer if our window is already down (s_in_use false)
  if (s_in_use) {
    prv_pop_notification_window(window_data);
  }
}

static void prv_refresh_pop_timer_with_timeout(NotificationWindowData *data, uint32_t timeout,
                                               bool final) {
  if (!prv_should_pop_due_to_inactivity()) {
    return;
  }

  if (data->action_menu == NULL) {
    // If the user has an action menu open, then we don't want to refresh the pop timeout,
    // as they are still interacting with the Notification stack
    data->pop_timer_is_final = final;
    data->pop_timer_id = evented_timer_register_or_reschedule(data->pop_timer_id, timeout,
                                                              prv_pop_timer_callback, data);
  }
}

static void prv_refresh_pop_timer(NotificationWindowData *data) {
  if (data->pop_timer_is_final) {
    return;
  }

  const uint32_t timeout_ms = alerts_get_notification_window_timeout_ms();
  prv_refresh_pop_timer_with_timeout(data, timeout_ms, false);
}

static void prv_pop_notification_window_after_delay(NotificationWindowData *data,
                                                    uint32_t delay_ms) {
  prv_refresh_pop_timer_with_timeout(data, delay_ms, true);
}

static time_t prv_get_stale_time(TimelineItem *item) {
  // Reminders become stale 10 minutes after their start time, or when the event is over
  return item->header.timestamp + MAX(10, item->header.duration) * SECONDS_PER_MINUTE;
}

static void prv_clear_if_stale_reminder(Uuid *id, NotificationType type, void *cb_data) {
  NotificationWindowData *window_data = cb_data;

  if (type != NotificationReminder) {
    return;
  }

  TimelineItem reminder;
  if (S_SUCCESS != reminder_db_read_item(&reminder, id)) {
    return;
  }
  timeline_item_free_allocated_buffer(&reminder);

  TimelineItem item;
  if (S_SUCCESS != pin_db_get(&reminder.header.parent_id, &item)) {
    return;
  }
  timeline_item_free_allocated_buffer(&item);

  // Use the latest stale time to auto-hide the reminder.
  const time_t reminder_stale_time = prv_get_stale_time(&reminder);
  const time_t event_stale_time = prv_get_stale_time(&item);
  const time_t stale_time = MAX(reminder_stale_time, event_stale_time);
  const time_t now = rtc_get_time();

  if (stale_time <= now && window_data->is_modal) {
    PBL_LOG(LOG_LEVEL_INFO, "Removing stale reminder from notification popup window");
    prv_remove_notification(window_data, id, true /* close am */);
  }
}

static void prv_clear_stale_reminders(void *data) {
  notifications_presented_list_each(prv_clear_if_stale_reminder, data);
}

static void prv_clear_stale_reminders_timer_cb(void *data) {
  // This functionality only exists for popups (modal windows) which currently all
  // run on Kernel Main
  launcher_task_add_callback(prv_clear_stale_reminders, data);
}

static void prv_setup_reminder_watchdog(NotificationWindowData *data) {
  if (!data->is_modal || regular_timer_is_scheduled(&data->reminder_watchdog_timer_id) ||
      !do_not_disturb_is_active()) {
    return;
  }

  data->reminder_watchdog_timer_id = (const RegularTimerInfo) {
    .cb = prv_clear_stale_reminders_timer_cb,
    .cb_data = data,
  };

  regular_timer_add_minutes_callback(&data->reminder_watchdog_timer_id);
}

///////////////////////
// Clicks
///////////////////////

static bool prv_should_show_action_in_action_menu(NotificationWindowData *data,
                                                  const TimelineItem *item,
                                                  const TimelineItemAction *action) {
  if (timeline_item_is_ancs_notif(item)) {
    if (data->is_modal) {
      // If we are in the modal popup show all available actions. We are fairly certain that the
      // notification will still be in the notification center at this point so all ANCS actions
      // should work
      return true;
    } else {
      // If we are in the notifications app, only show non ANCS actions. Pre iOS9 we can't really
      // know if the notification is still in the notification center or not, so we play it safe
      // and only show non ACNS actions. Once iOS9 is more widespread we can look at updating this
      return !timeline_item_action_is_ancs(action);
    }
  } else { // Android
    // Show all actions unless the item has already been acted upon, in which case show none
    return (!item->header.actioned &&
            !item->header.dismissed &&
            (data->is_modal ||
             comm_session_has_capability(comm_session_get_system_session(),
                                         CommSessionExtendedNotificationService)));
  }
}

static bool prv_should_provide_action_menu_for_item(NotificationWindowData *data,
                                                    const TimelineItem *item) {
  for (int i = 0; i < item->action_group.num_actions; i++) {
    TimelineItemAction *action = &item->action_group.actions[i];
    if (prv_should_show_action_in_action_menu(data, item, action)) {
      return true;
    }
  }
  return false;
}

static WindowStack *prv_get_window_stack(void) {
  if (pebble_task_get_current() == PebbleTask_App) {
    return app_state_get_window_stack();
  }
  return modal_manager_get_window_stack(ModalPriorityNotification);
}

static void prv_push_snooze_dialog(void) {
  SimpleDialog *simple_dialog = simple_dialog_create("Snooze");
  Dialog *dialog = simple_dialog_get_dialog(simple_dialog);
  const char *msg = i18n_get("Snoozed", dialog);
  dialog_set_text(dialog, msg);
  dialog_set_icon(dialog, RESOURCE_ID_REMINDER_SNOOZE);
  i18n_free(msg, dialog);
  dialog_set_text_color(dialog, GColorWhite);
  dialog_set_fullscreen(dialog, true);
  dialog_set_background_color(dialog, GColorBlueMoon);
  dialog_set_timeout(dialog, 1700);
  simple_dialog_push(simple_dialog, prv_get_window_stack());
}

static void prv_snooze_reminder_cb(ActionMenu *action_menu,
                                   const ActionMenuItem *action_menu_item,
                                   void *context) {
  NotificationWindowData *window_data = (NotificationWindowData *) action_menu_item->action_data;
  TimelineItem *item = prv_get_current_notification(window_data);

  // Snooze reminder.
  // It's highly unlikely we'll get E_INVALID_OPERATION based on the snooze logic parameters.
  if (reminders_snooze((Reminder *) item) == S_SUCCESS) {
    prv_push_snooze_dialog();
  }

  // Dismiss reminder
  const TimelineItemAction *action = timeline_item_find_dismiss_action(item);
  if (action) {
    timeline_invoke_action(item, action, NULL);
  }
}

static void prv_push_muted_dialog(void) {
  SimpleDialog *simple_dialog = simple_dialog_create("Muted");
  Dialog *dialog = simple_dialog_get_dialog(simple_dialog);

  const char *msg = i18n_get("Muted", dialog);
  dialog_set_text(dialog, msg);
  dialog_set_icon(dialog, RESOURCE_ID_RESULT_MUTE_LARGE);
  dialog_set_timeout(dialog, DIALOG_TIMEOUT_DEFAULT);
  i18n_free(msg, dialog);
  simple_dialog_push(simple_dialog, prv_get_window_stack());
}

static void prv_mute_notification(const ActionMenuItem *action_menu_item,
                                  uint8_t muted_bitfield) {
  NotificationWindowData *window_data = action_menu_item->action_data;
  TimelineItem *item = prv_get_current_notification(window_data);

  const char *app_id = attribute_get_string(&item->attr_list, AttributeIdiOSAppIdentifier, "");
  if (!*app_id) {
    PBL_LOG(LOG_LEVEL_ERROR, "Could not mute notification. Unknown app_id");
    return;
  }

  const int app_id_len = strlen(app_id);
  iOSNotifPrefs *notif_prefs = ios_notif_pref_db_get_prefs((uint8_t *)app_id, app_id_len);
  if (notif_prefs && attribute_find(&notif_prefs->attr_list, AttributeIdMuteDayOfWeek)) {
    attribute_list_add_uint8(&notif_prefs->attr_list, AttributeIdMuteDayOfWeek, muted_bitfield);
    ios_notif_pref_db_store_prefs((uint8_t *)app_id, app_id_len,
                                  &notif_prefs->attr_list, &notif_prefs->action_group);

    TimelineItemAction *dismiss = timeline_item_find_dismiss_action(item);
    if (dismiss) {
      timeline_invoke_action(item, dismiss, NULL);
    }
    prv_push_muted_dialog();
    analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_ANCS_MUTED_COUNT, AnalyticsClient_System);
  } else {
    // This is a very unlikely case. We store some default prefs which includes the mute
    // attribute when we receive the notification so either someone deleted the entry
    // in the DB or the mute attribute (neither of which should happen)
    PBL_LOG(LOG_LEVEL_WARNING, "Could not mute notification. No prefs or mute attribute");
  }

  ios_notif_pref_db_free_prefs(notif_prefs);
}

static void prv_mute_notification_always(ActionMenu *action_menu,
                                         const ActionMenuItem *action_menu_item,
                                         void *context) {
  prv_mute_notification(action_menu_item, MuteBitfield_Always);
}

static void prv_mute_notification_weekdays(ActionMenu *action_menu,
                                           const ActionMenuItem *action_menu_item,
                                           void *context) {
  prv_mute_notification(action_menu_item, MuteBitfield_Weekdays);
}

static void prv_mute_notification_weekends(ActionMenu *action_menu,
                                           const ActionMenuItem *action_menu_item,
                                           void *context) {
  prv_mute_notification(action_menu_item, MuteBitfield_Weekends);
}

static bool prv_has_mute_action(TimelineItem *item) {
  PebbleProtocolCapabilities capabilities;
  bt_persistent_storage_get_cached_system_capabilities(&capabilities);

  return timeline_item_is_ancs_notif(item) && capabilities.notification_filtering_support;
}

static ActionMenuLevel *prv_create_action_menu_for_item(TimelineItem *item,
                                                        NotificationWindowData *window_data,
                                                        TimelineItemActionSource source) {
  // Determine action menu properties
  int num_timeline_actions = 0;
  TimelineItemAction *dismiss_action = NULL;

  for (int i = 0; i < item->action_group.num_actions; i++) {
    TimelineItemAction *action = &item->action_group.actions[i];
    if (prv_should_show_action_in_action_menu(window_data, item, action)) {
      num_timeline_actions++;
      if (timeline_item_action_is_dismiss(action)) {
        dismiss_action = action;
      }
    }
  }

  // Snooze is not needed for Reminders App items
  Uuid items_originator_id;
  timeline_get_originator_id(item, &items_originator_id);
  const bool has_snooze_action = ((item->header.type == TimelineItemTypeReminder) &&
                      !uuid_equal(&(Uuid)UUID_REMINDERS_DATA_SOURCE, &items_originator_id) &&
                      reminders_can_snooze(item));

  const bool has_dismiss_all_action = ((dismiss_action) &&
                                       (notifications_presented_list_count() > 1));
  const bool has_quiet_time_action = true; // Always true
  const bool has_ancs_mute_action = prv_has_mute_action(item);

  uint8_t num_local_actions = 0;
  num_local_actions += (has_snooze_action) ? 1 : 0;
  num_local_actions += (has_dismiss_all_action) ? 1 : 0;
  num_local_actions += (has_quiet_time_action) ? 1 : 0;
  num_local_actions += (has_ancs_mute_action) ? 1 : 0;

  uint8_t num_item_specific_actions = num_timeline_actions;
  num_item_specific_actions += (has_snooze_action) ? 1 : 0;
  num_item_specific_actions += (has_ancs_mute_action) ? 1 : 0;

  // Create root level
  uint8_t num_actions = num_timeline_actions + num_local_actions;
  uint8_t separator_index = num_actions > num_item_specific_actions ? num_item_specific_actions : 0;
  ActionMenuLevel *root_level = timeline_actions_create_action_menu_root_level(num_actions,
                                                                               separator_index,
                                                                               source);

  // Add actions in order
  // [0] Dismiss (if applicable)
  // [1] Snooze (if applicable)
  // [2] Other mobile actions
  // ... Other mobile actions
  // [n] Other mobile actions
  // [n + 1] ANCS Mute (if applicable)
  // [n + 2] Dismiss all (if applicable)
  // [n + 3] Toggle Quiet Time
  if (dismiss_action) {
    timeline_actions_add_action_to_root_level(dismiss_action, root_level);
  }
  if (has_snooze_action) {
    action_menu_level_add_action(root_level,
                                 i18n_get("Snooze", root_level),
                                 prv_snooze_reminder_cb,
                                 window_data);
  }
  for (int i = 0; i < item->action_group.num_actions; i++) {
    TimelineItemAction *action = &item->action_group.actions[i];
    if (prv_should_show_action_in_action_menu(window_data, item, action) &&
        action != dismiss_action) {
      timeline_actions_add_action_to_root_level(action, root_level);
    }
  }
  if (has_ancs_mute_action) {
    const char *app_id = attribute_get_string(&item->attr_list, AttributeIdiOSAppIdentifier, "");
    iOSNotifPrefs *notif_prefs = ios_notif_pref_db_get_prefs((uint8_t *)app_id, strlen(app_id));
    char *display_name = "";
    if (notif_prefs) {
      display_name = (char *)attribute_get_string(&notif_prefs->attr_list, AttributeIdAppName, "");
    }

    const char *mute_label = i18n_noop("Mute %s");
    static char mute_label_buf[32];
    snprintf(mute_label_buf, sizeof(mute_label_buf),
            i18n_get(mute_label, root_level), display_name);

    const uint8_t mute_option = ancs_filtering_get_mute_type(notif_prefs);
    const bool is_mute_weekdays = mute_option == MuteBitfield_Weekdays;
    const bool is_mute_weekends = mute_option == MuteBitfield_Weekends;

    if (is_mute_weekdays || is_mute_weekends) {
      action_menu_level_add_action(root_level,
                                   mute_label_buf,
                                   prv_mute_notification_always,
                                   window_data);
    } else {
      const uint8_t number_mute_actions = 3;
      ActionMenuLevel *mute_level = action_menu_level_create(number_mute_actions);

      action_menu_level_add_child(root_level,
                                  mute_level,
                                  mute_label_buf);

      action_menu_level_add_action(mute_level,
                                   i18n_get("Mute Always", root_level),
                                   prv_mute_notification_always,
                                   window_data);

      action_menu_level_add_action(mute_level,
                                   i18n_get("Mute Weekends", root_level),
                                   prv_mute_notification_weekends,
                                   window_data);

      action_menu_level_add_action(mute_level,
                                   i18n_get("Mute Weekdays", root_level),
                                   prv_mute_notification_weekdays,
                                   window_data);
    }

    ios_notif_pref_db_free_prefs(notif_prefs);
  }

  if (has_dismiss_all_action) {
    action_menu_level_add_action(root_level,
                                 i18n_get("Dismiss All", root_level),
                                 prv_dismiss_all_action_cb,
                                 window_data);
  }
  if (has_quiet_time_action) {
    action_menu_level_add_action(root_level,
                                 do_not_disturb_is_active() ?
                                     i18n_get("End Quiet Time", root_level) :
                                     i18n_get("Start Quiet Time", root_level),
                                 prv_toggle_dnd_from_action_menu,
                                 window_data);
  }

  return root_level;
}

static void prv_action_menu_did_close(ActionMenu *action_menu, const ActionMenuItem *item,
                                      void *context) {
  NotificationWindowData *data = &s_notification_window_data;
  data->action_menu = NULL;
}

static void prv_select_single_click_handler(ClickRecognizerRef recognizer, void *data) {
  NotificationWindowData *window_data = data;

  TimelineItem *item = prv_get_current_notification(window_data);
  if (!prv_should_provide_action_menu_for_item(window_data, item)) {
    return;
  }

  LayoutLayer *layout = swap_layer_get_current_layout(&window_data->swap_layer);
  const LayoutColors *colors = layout_get_notification_colors(layout);

  ActionMenuConfig config = {
    .context = item,
    .colors.background = colors->bg_color,
    .did_close = prv_action_menu_did_close,
  };

  TimelineItemActionSource source = (window_data->is_modal ?
            TimelineItemActionSourceModalNotification : TimelineItemActionSourceNotificationApp);

  config.root_level = prv_create_action_menu_for_item(item, window_data, source);
  if (config.root_level == NULL) {
    PBL_LOG(LOG_LEVEL_ERROR, "Couldn't create notification action menu");
    return;
  }

  window_data->action_menu = timeline_actions_push_action_menu(
      &config, window_manager_get_window_stack(NOTIFICATION_PRIORITY));
}

static void prv_select_long_click_handler(ClickRecognizerRef recognizer, void *data) {
  prv_dismiss_all(data, NULL);
}

static void prv_back_button_single_click_handler(ClickRecognizerRef recognizer, void *data) {
  NotificationWindowData *window_data = data;
  if (window_data->is_modal) {
    analytics_inc(ANALYTICS_DEVICE_METRIC_NOTIFICATION_CLOSED_COUNT, AnalyticsClient_System);
  }
  prv_pop_notification_window(window_data);
}

static void prv_click_config_provider(void *data) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_single_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 1000, prv_select_long_click_handler, NULL);
  window_set_click_context(BUTTON_ID_SELECT, data);

  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_button_single_click_handler);
  window_set_click_context(BUTTON_ID_BACK, data);

  if (s_notification_window_data.is_modal) {
    Window *window = &s_notification_window_data.window;
    window_set_overrides_back_button(window, true);
    ClickManager *mgr = modal_manager_get_click_manager();
    ClickConfig *cfg = &mgr->recognizers[BUTTON_ID_BACK].config;
    cfg->long_click.delay_ms = QUICK_DND_HOLD_MS;
    cfg->long_click.handler = prv_toggle_dnd_from_back_click;
  }
}

///////////////////////
// Window Callbacks
///////////////////////

static void prv_window_appear(Window *window) {
  // check if we still have any notifications to display. If not, pop!
  NotificationWindowData *data = window_get_user_data(window);
  if (notifications_presented_list_current_idx() < 0) {
    prv_pop_notification_window_after_delay(data, 0);
    return;
  }
  prv_setup_reminder_watchdog(data);

  prv_refresh_pop_timer(data);

  // update status bar to the current info
  prv_update_status_layer(data);
  // Reload notification data from notification_storage in case of an action/remove
  if (data->notifications_modified) {
    data->notifications_modified = false;
    prv_reload_swap_layer(data);
  }
}

static void prv_window_disappear(Window *window) {
  NotificationWindowData *data = window_get_user_data(window);
  prv_cleanup_timer(&data->pop_timer_id);
}

static void prv_handle_presented_notif_deinit(Uuid *id, NotificationType type, void *not_used) {
  if (type == NotificationReminder) {
    // the reminder has been shown so delete it.
    // don't send an event, because there might be more reminders than queue slots
    reminder_db_delete_item(id, false /* send_event */);
  }
}

static void prv_window_unload(Window *window) {
  NotificationWindowData *data = window_get_user_data(window);
  if (!data) {
    return;
  }

  vibes_cancel();
  prv_cleanup_timers(data);

  // clean up peek layer
  if (data->peek_layer) {
    peek_layer_destroy(data->peek_layer);
    data->peek_layer = NULL;
  }
  animation_unschedule(data->peek_animation);

  swap_layer_deinit(&data->swap_layer);
  status_bar_layer_deinit(&data->status_layer);
  notifications_presented_list_deinit(prv_handle_presented_notif_deinit, NULL);
  gbitmap_deinit(&data->dnd_icon);
  layer_deinit(&data->dnd_icon_layer);

  window_deinit(window);

  i18n_free_all(data);
  s_in_use = false;
}

//////////////////////
// Callback Handlers
//////////////////////

static void prv_layout_did_appear_handler(SwapLayer *swap_layer, LayoutLayer *layout,
                                          int8_t rel_change, void *context) {
  NotificationWindowData *data = context;
  TimelineItem *n = layout_get_context(layout);
  Uuid *id = &n->header.id;
  notifications_presented_list_set_current(id);
  if (data->first_notif_loaded || !data->is_modal) {
    layer_set_hidden(&data->action_button_layer, !prv_should_provide_action_menu_for_item(data, n));
  }
  // update status bar to the current info
  prv_update_status_layer(data);
  kino_layer_play(&((NotificationLayout *)layout)->icon_layer);
}

static void prv_update_colors_handler(SwapLayer *swap_layer, GColor bg_color,
                                      bool status_bar_filled, void *context) {
  NotificationWindowData *data = context;
  GColor status_color = (status_bar_filled) ? bg_color : GColorWhite;
  // Status bar is clear on round, because the banner is rendered under it
  status_bar_layer_set_colors(&data->status_layer, PBL_IF_ROUND_ELSE(GColorClear, status_color),
                              gcolor_legible_over(status_color));
}

static void prv_interaction_handler(SwapLayer *swap_layer, void *context) {
  NotificationWindowData *data = context;
  prv_refresh_pop_timer(data);
}

static void prv_set_dnd_icon_visible(bool is_visible) {
  NotificationWindowData *data = &s_notification_window_data;
  if (is_visible == data->dnd_icon_visible) {
    // nothing to do here
    return;
  }

  const GRect icon_rect = gbitmap_get_bounds(&data->dnd_icon);
#if PBL_ROUND
  GRect new_status_frame = data->status_layer.layer.frame;

  const int16_t icon_text_horizontal_spacing = 4;
  const int16_t window_bounds_width = window_get_root_layer(&data->window)->bounds.size.w;
  const int16_t title_width = status_layer_get_title_text_width(&data->status_layer);

  const int16_t status_offset = (icon_rect.size.w + icon_text_horizontal_spacing) / 2;

  new_status_frame.origin.x += is_visible ? status_offset : -status_offset;
  const int16_t new_icon_layer_x_offset = ((window_bounds_width - title_width) / 2) - status_offset;

  layer_set_frame(&data->status_layer.layer, &new_status_frame);
#endif
  const uint16_t icon_layer_x_offset = PBL_IF_ROUND_ELSE(new_icon_layer_x_offset, 6);
  const uint16_t icon_layer_y_offset = PBL_IF_ROUND_ELSE(10, 2);
  const GRect dnd_frame = (GRect) {
    .origin = GPoint(icon_layer_x_offset, icon_layer_y_offset),
    .size = icon_rect.size,
  };
  layer_set_frame(&data->dnd_icon_layer, &dnd_frame);

  data->dnd_icon_visible = is_visible;
}

static void prv_dnd_status_changed(bool dnd_is_active) {
  if (s_notification_window_data.is_modal && s_in_use) {
    prv_set_dnd_icon_visible(dnd_is_active);
  }
}

///////////////////////////
// Notification Window API
///////////////////////////

static void prv_init_notification_window(bool is_modal) {
  NotificationWindowData *data = &s_notification_window_data;

  // init_notification_window() can be called from KernelMain when displaying an incoming
  // notification and also from the notifications.c application task. Grab a mutex here so
  // that we don't ever get two instances of it at a time.
  mutex_lock(s_notification_window_mutex);
  if (s_in_use) {
    goto fail;
  }

  s_in_use = true;
  data->pop_timer_is_final = false;
  data->is_modal = is_modal;
  data->notification_app_id = UUID_INVALID;
  data->peek_layer_timer = EVENTED_TIMER_INVALID_ID;
  data->peek_animation = NULL;
  data->peek_layer = NULL;
  data->peek_icon_info = (TimelineResourceInfo) {
    .res_id = TIMELINE_RESOURCE_INVALID,
    .app_id = NULL,
    .fallback_id = TIMELINE_RESOURCE_INVALID
  };
  data->action_menu = NULL;
  data->dnd_icon_visible = false;

  Window *window = &data->window;
  window_init(window, "Notification Window");
  window_set_window_handlers(window, &(WindowHandlers) {
      .appear = prv_window_appear,
      .disappear = prv_window_disappear,
      .unload = prv_window_unload,
  });
  window_set_user_data(window, data);

  // Initialize some variables early
  Layer *root_layer = window_get_root_layer(window);
  const GRect *window_frame = &root_layer->frame;

  // prepare the swap layer frame using notification_layout values including the status bar
  const GRect swap_frame = GRect(0, STATUS_BAR_LAYER_HEIGHT, window_frame->size.w,
                                 LAYOUT_HEIGHT + LAYOUT_ARROW_HEIGHT);

  SwapLayer *swap_layer = &data->swap_layer;
  swap_layer_init(swap_layer, &swap_frame);
  swap_layer_set_callbacks(swap_layer, data, (SwapLayerCallbacks) {
    .get_layout_handler = prv_get_layout_handler,
    .layout_removed_handler = prv_layout_removed_handler,
    .layout_did_appear_handler = prv_layout_did_appear_handler,
#if PBL_COLOR
    .update_colors_handler = prv_update_colors_handler,
#endif
    .interaction_handler = prv_interaction_handler,
    .click_config_provider = prv_click_config_provider,
  });
  swap_layer_set_click_config_onto_window(swap_layer, window);
  layer_add_child(root_layer, swap_layer_get_layer(swap_layer));

  StatusBarLayer *status_layer = &data->status_layer;
  status_bar_layer_init(status_layer);
  status_bar_layer_set_colors(status_layer, PBL_IF_RECT_ELSE(GColorBlack, GColorClear),
                              PBL_IF_RECT_ELSE(GColorWhite, GColorBlack));
  status_bar_layer_set_separator_mode(status_layer, StatusBarLayerSeparatorModeNone);
  layer_add_child(root_layer, (Layer *)status_layer);

  // bubble on right for action button
  layer_init(&data->action_button_layer, &data->window.layer.bounds);
  data->action_button_layer.update_proc = action_button_update_proc;
  layer_add_child(root_layer, &data->action_button_layer);

  layer_set_hidden((Layer *)&data->action_button_layer, true);

  // Ideally this gets moved into the status layer in the future. See data struct comment
  gbitmap_init_with_resource(&data->dnd_icon, RESOURCE_ID_QUIET_TIME_STATUS_BAR);

  // actual frame of the icon layer is calculated in prv_set_dnd_icon_visible()
  layer_init(&data->dnd_icon_layer, &GRectZero);
  layer_set_update_proc(&data->dnd_icon_layer, prv_draw_dnd_icon);
  layer_add_child(root_layer, &data->dnd_icon_layer);
  prv_dnd_status_changed(do_not_disturb_is_active());

  // set up the notification presented list service
  notifications_presented_list_init();

fail:
  mutex_unlock(s_notification_window_mutex);
}

void notification_window_init(bool is_modal) {
  prv_init_notification_window(is_modal);

  if (is_modal && notification_window_is_modal()) {
    // If we didn't ask for a modal window, it means some other task already created it,
    // so no need to push it
    modal_window_push(&s_notification_window_data.window,
                      NOTIFICATION_PRIORITY, true /* animated */);
  }
}

void notification_window_show() {
  if (s_notification_window_data.is_modal) {
    return;
  }

  const bool animated = true;
  app_window_stack_push(&s_notification_window_data.window, animated);
}

bool notification_window_is_modal(void) {
  return s_notification_window_data.is_modal;
}

void notification_window_add_notification_by_id(Uuid *id) {
  prv_notification_window_add_notification(id, NotificationMobile);
}

//! The animate mode slides the notification in from the top as if it was a new notification.
void notification_window_focus_notification(Uuid *id, bool animated) {
  NotificationWindowData *data = &s_notification_window_data;

  if (animated) {
#if PBL_RECT
    Uuid *second_id = notifications_presented_list_relative(
        notifications_presented_list_first(), +1);
    if (second_id) {
      // On rectangular displays, get the notification below the one we want to focus,
      // set it as the current notification, then swap up. This allows us
      // to accomplish the animation effect, while still pleasing the SwapLayer
      // when it wants to retrieve the layouts it wants to.
      notifications_presented_list_set_current(second_id);
      swap_layer_attempt_layer_swap(&data->swap_layer, ScrollDirectionUp);
      return;
    }
#else
    // On round displays, just set the new notification as the current one and show
    // the peek animation
    notifications_presented_list_set_current(id);
    prv_show_peek_for_notification(data, id, false /* is_first_notification */);
    return;
#endif
  }

  // Animated was set to false or there was no notification after the focusing one.
  // Just set the current notification and reload data.
  notifications_presented_list_set_current(id);
  prv_reload_swap_layer(data);
}

void notification_window_service_init(void) {
  s_notification_window_mutex = mutex_create();
  s_notification_window_data.pop_timer_id = EVENTED_TIMER_INVALID_ID;
}


//////////////////
// Event Handlers
//////////////////

static void prv_handle_action_result(PebbleSysNotificationActionResult *action_result) {
  if (action_result->type != ActionResultTypeSuccess &&
      action_result->type != ActionResultTypeSuccessANCSDismiss) {
    return;
  }

  // the notification has been acted on. Remove it.
  NotificationWindowData *data = &s_notification_window_data;
  notification_storage_set_status(&action_result->id, TimelineItemStatusActioned);
  data->notifications_modified = true;

  if (data->is_modal) {
    // Don't remove the action menu here. The timeline actions module also handles this event
    // and will remove it as well as put a result dialog
    prv_remove_notification(data, &action_result->id, false /* close am */);
    prv_refresh_pop_timer(data);
  }
}

static void prv_handle_notification_removed_common(Uuid *id, NotificationType type) {
  NotificationWindowData *data = &s_notification_window_data;

  if (s_in_use && data->is_modal && !data->window_frozen) {
    prv_remove_notification(data, id, true /* close am */);
  }
}

static void prv_handle_notification_acted_upon(Uuid *id) {
  NotificationWindowData *data = &s_notification_window_data;

  if (data->is_modal) {
    prv_remove_notification(data, id, true /* close am */);
  } else {
    data->notifications_modified = true;
  }
}

static void prv_handle_notification_added_common(Uuid *id, NotificationType type) {
  if (!alerts_should_notify_for_type(prv_alert_type_for_notification_type(type))) {
    return;
  }

  NotificationWindowData *data = &s_notification_window_data;

  // will fail and return early if already init'ed.
  prv_init_notification_window(true /*is_modal*/);

  if (data->is_modal) {
    WindowStack *window_stack = modal_manager_get_window_stack(NOTIFICATION_PRIORITY);
    bool is_new = !window_stack_contains_window(window_stack, &data->window);
    bool in_view = window_is_on_screen(&data->window);

    prv_notification_window_add_notification(id, type);

    if (is_new) {
      data->first_notif_loaded = false;
      prv_show_peek_for_notification(data, id, true /* is_first_notification */);
      modal_window_push(&data->window, NOTIFICATION_PRIORITY, true /* animated */);
    } else if (in_view) {
      // Only focus the new notification if it becomes the new front of the list.
      // In DND mode notifications can get inserted into the middle of the list and we don't
      // want to change focus in this use case
      if (notifications_presented_list_current() != notifications_presented_list_first()) {
        const bool should_animate = !do_not_disturb_is_active();
        notification_window_focus_notification(id, should_animate);
      } else {
        // If we are inserting into the middle of this list, just reaload the swap layer so the
        // number of notifications displayed is correct
        prv_reload_swap_layer(data);
      }
    }
  }

  alerts_incoming_alert_analytics();
  if (alerts_should_vibrate_for_type(prv_alert_type_for_notification_type(type))) {
#if CAPABILITY_HAS_VIBE_SCORES
    VibeScore *score = vibe_client_get_score(VibeClient_Notifications);
    if (score) {
      vibe_score_do_vibe(score);
      vibe_score_destroy(score);
    }
#else
    vibes_short_pulse();
#endif
    // Timestamp set after call to vibrate since if something fails,
    // its better to have no vibe blocking then vibe blocking and no vibrations.
    alerts_set_notification_vibe_timestamp();
  }

  if (alerts_should_enable_backlight_for_type(prv_alert_type_for_notification_type(type))) {
    light_enable_interaction();
  }

  prv_refresh_pop_timer(data);
}

static bool prv_is_item_loaded(Uuid *id) {
  return (uuid_equal(id, notifications_presented_list_current()) ||
          uuid_equal(id, notifications_presented_list_next()));
}

static void prv_handle_reminder_updated(Uuid *id) {
  NotificationWindowData *data = &s_notification_window_data;

  // We only need to reload from flash if the item is already in memory
  // (ie. the current item or the next/peek item)
  if (data->is_modal && prv_is_item_loaded(id)) {
    prv_reload_swap_layer(data);
  }
}

void notification_window_handle_reminder(PebbleReminderEvent *e) {
  switch (e->type) {
    case ReminderTriggered:
      prv_handle_notification_added_common(e->reminder_id, NotificationReminder);
      break;
    case ReminderRemoved:
      prv_handle_notification_removed_common(e->reminder_id, NotificationReminder);
      break;
    case ReminderUpdated:
      prv_handle_reminder_updated(e->reminder_id);
      break;
  }
}

void notification_window_handle_notification(PebbleSysNotificationEvent *e) {
  if (!s_in_use && e->type != NotificationAdded) {
    return;
  }
  switch (e->type) {
    case NotificationActionResult:
      prv_handle_action_result(e->action_result);
      break;
    case NotificationAdded:
      prv_handle_notification_added_common(e->notification_id, NotificationMobile);
      break;
    case NotificationActedUpon:
      prv_handle_notification_acted_upon(e->notification_id);
      break;
    case NotificationRemoved:
      prv_handle_notification_removed_common(e->notification_id, NotificationMobile);
      break;
  }
}

void notification_window_handle_dnd_event(PebbleDoNotDisturbEvent *e) {
  if (!s_notification_window_data.is_modal || !s_in_use) {
    return;
  }

  prv_dnd_status_changed(e->is_active);

  if (e->is_active) {
    prv_setup_reminder_watchdog(&s_notification_window_data);
  } else {
    prv_cancel_reminder_watchdog(&s_notification_window_data);
  }

  if (prv_should_pop_due_to_inactivity()) {
    // Re-schedule the window pop timer after leaving DND mode
    prv_refresh_pop_timer(&s_notification_window_data);
  }
}

// This function is only used by the notifications_app.
// When it calls this function, it knows it is a valid notification already.
void app_notification_window_add_new_notification_by_id(Uuid *id) {
  if (do_not_disturb_is_active()) {
    return;
  }

  NotificationWindowData *data = &s_notification_window_data;
  if (!s_in_use || data->is_modal) {
    return;
  }

  bool should_focus = (app_window_stack_get_top_window() == &data->window);
  notification_window_add_notification_by_id(id);
  if (should_focus) {
    const bool animated = true;
    notification_window_focus_notification(id, animated);
  }
}

// This function is only used by the notifications_app.
// When it calls this function, it knows it is a valid notification already.
void app_notification_window_remove_notification_by_id(Uuid *id) {
  NotificationWindowData *data = &s_notification_window_data;
  if (!s_in_use || data->is_modal) {
    return;
  }

  prv_remove_notification(data, id, true /* close am */);
}

// This function is only used by the notifications_app.
// When it calls this function, it knows it is a valid notification already.
void app_notification_window_handle_notification_acted_upon_by_id(Uuid *id) {
  NotificationWindowData *data = &s_notification_window_data;
  if (!s_in_use || data->is_modal) {
    return;
  }
  prv_reload_swap_layer(data);
}
