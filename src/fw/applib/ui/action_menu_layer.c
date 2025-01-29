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

#include "action_menu_layer.h"
#include "action_menu_window_private.h"

#include "applib/applib_malloc.auto.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/text.h"
#include "applib/ui/animation.h"
#include "applib/ui/menu_layer.h"
#include "applib/ui/property_animation.h"
#include "applib/ui/window_private.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/kernel_ui.h"
#include "resource/resource_ids.auto.h"
#include "shell/system_theme.h"
#include "system/passert.h"
#include "util/math.h"

#include <string.h>

#define INDICATOR "»"

static const int VERTICAL_PADDING = PBL_IF_COLOR_ELSE(2, 4);
static const int EXTRA_PADDING_1_BIT = 2;
static const int SHORT_COL_COUNT = 3;
static const int MAX_NUM_VISIBLE_LINES = 2;
static const int SHORT_ITEM_MAX_ROWS_SPALDING = 3;

static GFont prv_get_item_font(void) {
  return system_theme_get_font(TextStyleFont_MenuCellTitle);
}

//! Only used on round displays to achieve a fish-eye effect
static GFont prv_get_unfocused_item_font(void) {
  return system_theme_get_font(TextStyleFont_Header);
}

static uint16_t prv_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                 void *callback_context) {
  ActionMenuLayer *aml = callback_context;
  return (uint16_t)(aml->num_items +
      (aml->num_short_items + SHORT_COL_COUNT - 1) / SHORT_COL_COUNT);
}

static void prv_cell_column_draw(GContext *ctx, struct Layer const *cell_layer,
                                 ActionMenuLayer *aml, ActionMenuItem *items,
                                 int num_items, int sel_idx) {
  const GFont font = aml->layout_cache.font;
  const int16_t font_height = fonts_get_font_height(font);
  const GRect *layer_bounds = &cell_layer->bounds;
  GRect r = *layer_bounds;
#if PBL_ROUND
  // more narrow on round
  r = grect_inset_internal(r, 25, 0);
  // center the columns horizontally if there's only one row
  const bool is_single_short_row = aml->num_short_items <= SHORT_COL_COUNT;
  r.size.w /= is_single_short_row ? num_items : SHORT_COL_COUNT;
#else
  r.size.w /= SHORT_COL_COUNT;
#endif
  r.origin.y += (r.size.h - font_height) / 2 - 4;

  for (int i = 0; i < num_items; i++) {
    if (!items[i].label) {
      break;
    }

    if (sel_idx == i) {
      graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
#if SCREEN_COLOR_DEPTH_BITS == 1
      // We only want to have a background on non-color platforms, while leaving this in with
      // a PBL_IF_COLOR_ELSE makes this a no-op, we'll save some cycles and code space just
      // skipping it.
      graphics_context_set_fill_color(ctx, GColorWhite);

      const int16_t y_offset = 1;
      const int16_t padding = r.size.w / 6;
      const uint16_t corner_radius = 4;
      GRect bg_rect = r;
      bg_rect.origin.y = layer_bounds->origin.y;
      bg_rect.size.h = layer_bounds->size.h;
      bg_rect = grect_inset_internal(bg_rect, padding, y_offset);
      graphics_fill_round_rect(ctx, &bg_rect, corner_radius, GCornersAll);
#endif
    } else {
      graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
    }

    graphics_draw_text(ctx, items[i].label, font, r, GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter, NULL);
    r.origin.x += r.size.w;
  }
}

static const ActionMenuItem  *prv_get_item_for_index(ActionMenuLayer *aml, int idx) {
  if (!aml->num_items && !aml->num_short_items) {
    return NULL;
  }

  PBL_ASSERTN(idx >= 0);

  if (idx < aml->num_items) {
    return &aml->items[idx];
  } else {
    const int short_items_idx = idx - aml->num_items;
    PBL_ASSERTN(short_items_idx < aml->num_short_items);
    return &aml->short_items[short_items_idx];
  }
}

static int16_t prv_get_item_line_height(ActionMenuLayer *aml, int idx) {
  const GFont font = aml->layout_cache.font;
  const ActionMenuItem *item = prv_get_item_for_index(aml, idx);
  GRect box = menu_layer_get_layer(&aml->menu_layer)->bounds;
  // In calculating the item line height for round displays, we need to horizontally inset by the
  // standard focused cell inset since that's the horizontal inset of the cells where we show
  // the vertical scrolling animation of long text cells (where the height is crucial to be correct)
  const int inset = PBL_IF_ROUND_ELSE(MENU_CELL_ROUND_FOCUSED_HORIZONTAL_INSET,
                                      menu_cell_basic_horizontal_inset());
  // Tintin has a rounded rectangle highlight
  box = grect_inset_internal(box, PBL_IF_COLOR_ELSE(inset, 2 * inset), 0);

  // We offset the text 5 pixels from the left of the cell.  If the indicator is
  // present, the indicator also will be offset, so we add 5 pixels more spacing
  // between the text and the indicator. This extra padding isn't needed for round.
  const int nudge = PBL_IF_ROUND_ELSE(0, menu_cell_basic_horizontal_inset());
  GContext *ctx = graphics_context_get_current_context();
  // On rectangular displays, if the indicator is present, the indicator also will be offset,
  // so we add another nudge between the text and the indicator.
#if PBL_RECT
  if (!item->is_leaf) {
    const GSize indicator_size = graphics_text_layout_get_max_used_size(ctx, INDICATOR,
                                                                        font, box,
                                                                        GTextOverflowModeWordWrap,
                                                                        GTextAlignmentRight, NULL);
    box.size.w -= (indicator_size.w + nudge);
  }
#endif
  return graphics_text_layout_get_text_height(ctx, item->label, font, box.size.w,
      GTextOverflowModeWordWrap, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
}

// Item Scroll Animation
///////////////////////////////////

static int16_t prv_get_cell_offset(void *subject) {
  ActionMenuLayer *aml = subject;
  return aml->item_animation.current_offset_y;
}

T_STATIC void prv_set_cell_offset(void *subject, int16_t value) {
  ActionMenuLayer *aml = subject;
  aml->item_animation.current_offset_y = value;
  layer_mark_dirty(&aml->layer);
}

static void prv_cell_animation_stopped_handler(Animation *animation, bool finished, void *context) {
  ActionMenuLayer *aml = context;
  if (finished) {
    prv_set_cell_offset(aml, aml->item_animation.bottom_offset_y);
  }
}

static const PropertyAnimationImplementation s_item_animation_implementation = {
  .base = {
    .update = (AnimationUpdateImplementation)property_animation_update_int16
  },
  .accessors = {
    .setter = { .int16 = prv_set_cell_offset },
    .getter = { .int16 = prv_get_cell_offset }
  }
};

static void prv_unschedule_item_animation(ActionMenuLayer *aml) {
  animation_unschedule(aml->item_animation.animation);
  aml->item_animation.animation = NULL;
}

static void prv_animate_cell(ActionMenuLayer *aml, GRect *label_text_frame, bool *draw_top_shading,
                             bool *draw_bottom_shading) {
  // Check to see if this item spans more than max number of visible lines,
  // in which case we want to make it scroll.
  const int16_t item_height = aml->layout_cache.item_heights[aml->selected_index];
  const int16_t line_height = fonts_get_font_height(aml->layout_cache.font);

#if SCREEN_COLOR_DEPTH_BITS == 1
  // We need to force it to scroll a little extra for 1 bit
  label_text_frame->origin.y -= EXTRA_PADDING_1_BIT;
#endif
  // On rect displays, calculate the visible item height based on a desired number of visible lines
  // On round displays, use the height of the provided box since it might be inset for the indicator
  const int16_t max_visible_item_height = PBL_IF_RECT_ELSE(MAX_NUM_VISIBLE_LINES * line_height,
                                                           label_text_frame->size.h);
  if (item_height > max_visible_item_height) {
    // Compute the limit at which we should bounce back to the top of the layer.  Since
    // there are at most MAX_NUM_VISIBLE_LINES shown at a given time, we want to stop
    // when there are that number of lines in view and no more lines remaining below.
    const int16_t max_scroll_distance = item_height - max_visible_item_height;
    ActionMenuItemAnimation *item_animation = &aml->item_animation;
    if (item_animation->animation == NULL) {
      const int16_t DELAY_PER_LINE = 600; /* milliseconds to delay per line */

      // Top offset represents when the text has scrolled to its minimum y value so the last line of
      // text is visible. Bottom offset represents when the text has scrolled all the way to its
      // maximum y so the first line of text is visible.
      item_animation->top_offset_y = -max_scroll_distance;
      item_animation->bottom_offset_y = 0;
      item_animation->current_offset_y = 0;

      // Create the animation that will scroll us up in the cell
      PropertyAnimation *animation = property_animation_create(&s_item_animation_implementation,
          (void *)aml, NULL, &item_animation->top_offset_y);

      animation_set_duration((Animation *)animation, DELAY_PER_LINE * (item_height / line_height));
      animation_set_curve((Animation *)animation, AnimationCurveLinear);
      animation_set_handlers((Animation *)animation, (AnimationHandlers){0}, aml);

      // Create the animation that stalls when we have auto-scrolled up completely
      PropertyAnimation *s_animation = property_animation_create(&s_item_animation_implementation,
          (void *)aml, &item_animation->top_offset_y, &item_animation->top_offset_y);

      animation_set_duration((Animation *)s_animation, DELAY_PER_LINE /* ms to wait */);
      animation_set_handlers((Animation *)s_animation, (AnimationHandlers){0}, aml);

      // Create the reverse animation that takes us from the scrolled up position back down
      PropertyAnimation *r_animation = property_animation_create(&s_item_animation_implementation,
          (void *)aml, &item_animation->top_offset_y, &item_animation->bottom_offset_y);

      animation_set_duration((Animation *)r_animation,
          (DELAY_PER_LINE / 4) * (item_height / line_height));
      animation_set_curve((Animation *)r_animation, AnimationCurveEaseInOut);
      animation_set_handlers((Animation *)r_animation, (AnimationHandlers){0}, aml);

      item_animation->animation = animation_sequence_create((Animation *)animation,
          (Animation *)s_animation, (Animation *)r_animation);

      animation_set_handlers(item_animation->animation,
                             (AnimationHandlers){ .stopped = prv_cell_animation_stopped_handler },
                             aml);
      animation_set_play_count(item_animation->animation, PLAY_COUNT_INFINITE);
      animation_set_delay(item_animation->animation, DELAY_PER_LINE /* ms */);
      animation_schedule(item_animation->animation);
    }
    *draw_top_shading = (item_animation->current_offset_y != item_animation->bottom_offset_y);
    *draw_bottom_shading = (item_animation->current_offset_y != item_animation->top_offset_y);

    // update the rect height and offset based on the current animation state
    label_text_frame->origin.y += item_animation->current_offset_y;
    label_text_frame->size.h = item_height;
  }
}

// Menu Layer Drawing Routines
///////////////////////////////

static bool prv_should_center(ActionMenuLayer *aml) {
  // We only center an ActionMenuLayer's items if the user has specified to
  // center the items or there is only one item in the ActionMenuLayer.
  if (aml->num_items == 1 || aml->layout_cache.align == ActionMenuAlignCenter) {
    return true;
  }
  return false;
}

static void prv_cell_item_content_draw_rect(GContext *ctx, const Layer *cell_layer,
                                            const ActionMenuLayer *aml, const ActionMenuItem *item,
                                            bool selected, GRect *content_box) {
  char *indicator = NULL;
  const int16_t horizontal_padding = menu_cell_basic_horizontal_inset();
  const GFont font = aml->layout_cache.font;
  if (!item->is_leaf) {
    // If an item is not a leaf, then there would be an indicator when it is focused.  Either
    // we draw the indicator or we force the box to be smaller to force the text to render as
    // if the indicator was present in case it would line wrap.
    if (selected) {
      indicator = INDICATOR;
    } else {
      const GSize indicator_size = graphics_text_layout_get_max_used_size(
          ctx, INDICATOR, font, *content_box, GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
      content_box->size.w -= (indicator_size.w + (2 * horizontal_padding));
    }
  } else {
    content_box->size.w -= horizontal_padding;
  }

#if SCREEN_COLOR_DEPTH_BITS == 1
  // Fill in the background layer.  This effectively does nothing on watches where we have the
  // ability to draw with color, but on others, it will render a background behind the selected
  // cell.
  const int x_offset = horizontal_padding;
  const int y_padding = EXTRA_PADDING_1_BIT;
  const uint16_t corner_radius = 4;
  GRect bg_box = grect_inset_internal(cell_layer->bounds, x_offset, 0);
  bg_box.size.h -= y_padding;
  graphics_fill_round_rect(ctx, &bg_box, corner_radius, GCornersAll);
  // We have to adjust the box to compensate for the padding we added.  Note that we can't call
  // inset as it will discard our offset when it standardizes.
  content_box->origin.x += x_offset;
  content_box->size.w -= (2 * x_offset);
  content_box->size.h -= (2 * y_padding);
#endif

  // Cast the cell layer so we can briefly modify its bounds. We do this because we're
  // desperate for stack space and we understand the call hierarchy. We'll restore the state below.
  Layer *mutable_cell_layer = (Layer *)cell_layer;
  const GRect saved_bounds = mutable_cell_layer->bounds;
  mutable_cell_layer->bounds = *content_box;

  // Draw the menu cell specifying that we're allowing word wrapping
  const GTextOverflowMode overflow_mode = GTextOverflowModeWordWrap;
  menu_cell_basic_draw_custom(ctx, mutable_cell_layer, font, item->label, font, indicator, font,
                              NULL, NULL, false, overflow_mode);

  // Restore the cell layer's bounds
  mutable_cell_layer->bounds = saved_bounds;
}

static void prv_cell_item_content_draw_round(GContext *ctx, const Layer *cell_layer,
                                             const ActionMenuLayer *aml, const ActionMenuItem *item,
                                             bool selected, GRect *content_box) {
  const int16_t horizontal_inset = selected ? MENU_CELL_ROUND_FOCUSED_HORIZONTAL_INSET :
                                              MENU_CELL_ROUND_UNFOCUSED_HORIZONTAL_INSET;
  *content_box = grect_inset(*content_box, GEdgeInsets(0, horizontal_inset));

  // Use a smaller font for the unfocused cells to achieve a fish-eye effect
  const GFont font = selected ? aml->layout_cache.font : prv_get_unfocused_item_font();
  const GTextOverflowMode overflow_mode = selected ? GTextOverflowModeWordWrap :
                                                     GTextOverflowModeTrailingEllipsis;
  const GTextAlignment text_alignment = GTextAlignmentCenter;
  const GSize text_size = graphics_text_layout_get_max_used_size(ctx, item->label, font,
                                                                 *content_box, overflow_mode,
                                                                 text_alignment, NULL);
  GRect text_box = (GRect) { .size = text_size };
  const GAlign item_label_text_alignment = GAlignCenter;
  grect_align(&text_box, content_box, item_label_text_alignment, true /* clip */);
  text_box.origin.y -= fonts_get_font_cap_offset(font);

  graphics_draw_text(ctx, item->label, font, text_box, overflow_mode, text_alignment, NULL);
}

static int16_t prv_get_indicator_height(const ActionMenuLayer *aml) {
  // This magic factor is an approximation of the indicator height in relation to the font line
  // height; it Just Works(tm)
  return fonts_get_font_height(aml->layout_cache.font) * 40 / 100;
}

static void prv_draw_indicator_round(GContext *ctx, const ActionMenuLayer *aml,
                                     const GRect *label_text_container) {
  const int indicator_height = fonts_get_font_height(aml->layout_cache.font);
  const int text_height = aml->layout_cache.item_heights[aml->selected_index];
  const int content_height = MIN(label_text_container->size.h, text_height + indicator_height);

  GRect content_frame = (GRect) {
    .size = GSize(label_text_container->size.w, content_height)
  };
  GRect indicator_frame = (GRect) {
    .size = GSize(label_text_container->size.w, indicator_height)
  };

  grect_align(&content_frame, label_text_container, GAlignCenter, true);
  grect_align(&indicator_frame, &content_frame, GAlignBottom, true);

  graphics_draw_text(ctx, INDICATOR, aml->layout_cache.font, indicator_frame,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void prv_cell_item_draw(GContext *ctx, const Layer *cell_layer,
                               ActionMenuLayer *aml, const ActionMenuItem *item,
                               bool selected) {
  GRect label_text_container = cell_layer->bounds;
  // bottom_inset won't be used on black and white, using UNUSED here quiets the linter
  UNUSED int16_t bottom_inset = 0;
#if PBL_ROUND
  // On round displays, inset the box from the bottom to account for drawing the indicator at the
  // bottom center, and then draw the indicator
  const bool selected_with_indicator = (selected && !item->is_leaf);
  if (selected_with_indicator) {
    prv_draw_indicator_round(ctx, aml, &label_text_container);

    const int16_t indicator_text_margin = 7;
    bottom_inset = prv_get_indicator_height(aml) + indicator_text_margin;
    label_text_container.size.h -= bottom_inset;
  }
#endif

  GRect label_text_frame = label_text_container;
  bool draw_top_shading = false;
  bool draw_bottom_shading = false;
  // If we are the selected index, check to see if we have started scrolling.
  // If we have, use our internal box to draw the layer, otherwise use the
  // layer box.
  if (selected) {
    prv_animate_cell(aml, &label_text_frame, &draw_top_shading, &draw_bottom_shading);
#if !defined(RECOVERY_FW) && SCREEN_COLOR_DEPTH_BITS == 8
    // Replace the clip box with a clip box that will render the item in the right place with the
    // right size, without menu layer's selection clipping. Menu layer will responsible for cleaning
    // up the changes made to this clip box.
    ctx->draw_state.clip_box.origin = ctx->draw_state.drawing_box.origin;
    ctx->draw_state.clip_box.size = cell_layer->bounds.size;
    // We have to update the clip box of the drawing state to account for text padding to
    // force it to clip around the shadow.
    if (draw_top_shading) {
      ctx->draw_state.clip_box.origin.y += VERTICAL_PADDING;
      ctx->draw_state.clip_box.size.h -= VERTICAL_PADDING;
    }
    if (draw_bottom_shading) {
      ctx->draw_state.clip_box.size.h -= VERTICAL_PADDING + bottom_inset;
    }
    // Prevent drawing outside of the context bitmap
    grect_clip(&ctx->draw_state.clip_box, &ctx->dest_bitmap.bounds);
#endif
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
  }

  PBL_IF_RECT_ELSE(prv_cell_item_content_draw_rect,
                   prv_cell_item_content_draw_round)(ctx, cell_layer, aml, item, selected,
                                                     &label_text_frame);

#if !defined(RECOVERY_FW) && SCREEN_COLOR_DEPTH_BITS == 8
  const int16_t fade_height = 10;
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  if (draw_top_shading) {
    GRect top_bounds = label_text_container;
    top_bounds.origin.y += VERTICAL_PADDING;
    top_bounds.size.h = fade_height;
    graphics_draw_bitmap_in_rect(ctx, &aml->item_animation.fade_top, &top_bounds);
  }

  if (draw_bottom_shading) {
    GRect bottom_bounds = label_text_container;
    bottom_bounds.size.h = fade_height;
    bottom_bounds.origin.y = grect_get_max_y(&label_text_container) -
                             (fade_height + VERTICAL_PADDING);
    graphics_draw_bitmap_in_rect(ctx, &aml->item_animation.fade_bottom, &bottom_bounds);
  }
#endif
}

static void prv_draw_row(GContext* ctx, const Layer *cell_layer, MenuIndex *cell_index,
                         void *callback_context) {
  ActionMenuLayer *aml = callback_context;

  if (cell_index->row < aml->num_items) {
    const ActionMenuItem *item = prv_get_item_for_index(aml, cell_index->row);
    const bool selected = menu_layer_is_index_selected(&aml->menu_layer, cell_index);
    prv_cell_item_draw(ctx, cell_layer, aml, item, selected);
  } else {
    const int base_idx = (cell_index->row - aml->num_items) * SHORT_COL_COUNT;
    const int sel_idx = aml->selected_index - (base_idx + aml->num_items);
    const int num_items = CLIP(aml->num_short_items - base_idx, 0, SHORT_COL_COUNT);
    prv_cell_column_draw(ctx, cell_layer, aml, (ActionMenuItem *)&aml->short_items[base_idx],
                         num_items, sel_idx);
  }
}

static int prv_get_menu_layer_row(ActionMenuLayer *aml, int item_index) {
  if (item_index < aml->num_items) {
    return item_index;
  } else {
    return aml->num_items + (item_index - aml->num_items) / SHORT_COL_COUNT;
  }
}

T_STATIC void prv_set_selected_index(ActionMenuLayer *aml, int new_selected_index, bool animated) {
  new_selected_index = CLIP(new_selected_index, 0, aml->num_items + aml->num_short_items - 1);

  if (new_selected_index != aml->selected_index) {
    // Unschedule any running item animation but don't NULL the pointer, to prevent another
    // animation from being accidentally re-scheduled.
    animation_unschedule(aml->item_animation.animation);
  }

  if (new_selected_index >= aml->num_items) {
    // For short columns, aml->selected_index needs to be updated here, because the column index
    // will be lost in the menu layer selection changed callback. Otherwise, it will be updated
    // in prv_selection_changed_cb() to ensure the correct index is used by the draw functions.
    aml->selected_index = new_selected_index;
  }

  const int menu_layer_index = prv_get_menu_layer_row(aml, new_selected_index);
  menu_layer_set_selected_index(&aml->menu_layer, MenuIndex(0, menu_layer_index),
                                MenuRowAlignCenter, animated);
}

static void prv_scroll_handler(ClickRecognizerRef recognizer, void *context) {
  ActionMenuLayer *aml = context;
  const bool up = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
  const int new_idx = aml->selected_index + (up ? -1 : 1);
  prv_set_selected_index(aml, new_idx, true /* animated */);
}

static void prv_select_handler(ClickRecognizerRef recognizer, void *context) {
  ActionMenuLayer *aml = context;
  const ActionMenuItem *item = prv_get_item_for_index(aml, aml->selected_index);
  if (item && aml->cb) {
    aml->cb(item, aml->context);
  }
}

static bool prv_aml_is_short(ActionMenuLayer *aml) {
  return (aml->num_short_items != 0 || aml->num_items == 0);
}

static int16_t prv_get_cell_padding(ActionMenuLayer *aml) {
  const int16_t default_sep_height = 10;
#if PBL_ROUND
  // when showing columns, set cells further apart
  return prv_aml_is_short(aml) ? default_sep_height : 1;
#elif SCREEN_COLOR_DEPTH_BITS == 1
  return default_sep_height;
#else
  const int16_t line_height = fonts_get_font_height(aml->layout_cache.font);
  const int16_t sep_height = MAX(menu_cell_small_cell_height() - line_height,
                                 default_sep_height) + 1;
  return sep_height;
#endif
}

static int16_t prv_get_cell_height_cb(struct MenuLayer *menu_layer,
                                      MenuIndex *cell_index,
                                      void *context) {
  ActionMenuLayer *aml = (ActionMenuLayer *)context;
  const int16_t line_height = fonts_get_font_height(aml->layout_cache.font);
  // If we have short items, just return the line height.
  if (prv_aml_is_short(aml)) {
    return line_height;
  }

#if PBL_ROUND
  return menu_layer_is_index_selected(menu_layer, cell_index) ?
            MENU_CELL_ROUND_FOCUSED_SHORT_CELL_HEIGHT :
            MENU_CELL_ROUND_UNFOCUSED_TALL_CELL_HEIGHT;
#else
  const int16_t max_visible_height = line_height * MAX_NUM_VISIBLE_LINES;
  const int16_t actual_height = aml->layout_cache.item_heights[cell_index->row];
  return (VERTICAL_PADDING * 2) + MIN(max_visible_height, actual_height);
#endif
}

static int16_t prv_get_separator_height_cb(struct MenuLayer *menu_layer, MenuIndex *cell_index,
                                           void *callback_context) {
  // We use the separator to pad the cells (insert spacing), so we compute the height
  // needed for each separator here.
  ActionMenuLayer *aml = callback_context;
  return prv_get_cell_padding(aml);
}

typedef struct ActionMenuSeparatorConfig {
  GSize separator;
} ActionMenuSeparatorConfig;

static const ActionMenuSeparatorConfig s_separator_configs[NumPreferredContentSizes] = {
  [PreferredContentSizeSmall] = {
    .separator = {100, 1},
  },
  [PreferredContentSizeMedium] = {
    .separator = {100, 1},
  },
  [PreferredContentSizeLarge] = {
    .separator = {162, 2},
  },
  [PreferredContentSizeExtraLarge] = {
    .separator = {162, 2},
  },
};

static void prv_draw_separator_cb(GContext *ctx, const Layer *cell_layer,
                                  MenuIndex *cell_index, void *callback_context) {
  ActionMenuLayer *aml = callback_context;
  if (aml->separator_index && cell_index->row == aml->separator_index) {
    const PreferredContentSize runtime_platform_default_size =
        system_theme_get_default_content_size_for_runtime_platform();
    const ActionMenuSeparatorConfig *config = &s_separator_configs[runtime_platform_default_size];

    // If this index is the separator index, we want to draw the separator line
    // in the vertical center of the separator
    const int16_t nudge_down = PBL_IF_RECT_ELSE(3, 0);
    const int16_t nudge_right = menu_cell_basic_horizontal_inset() + 1;

    const int16_t separator_width = config->separator.w;
    const GRect *cell_layer_bounds = &cell_layer->bounds;
    const int16_t offset_x = PBL_IF_RECT_ELSE(nudge_right,
                                              (cell_layer->bounds.size.w - separator_width) / 2);
    const int16_t offset_y = (cell_layer_bounds->size.h / 2) + nudge_down;
    GPoint separator_start_point = gpoint_add(cell_layer_bounds->origin,
                                              GPoint(offset_x, offset_y));
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));

    separator_start_point.y += config->separator.h;
    for (int i = 0; i < config->separator.h; i++) {
      // First point from bottom will be +0, second +1, third +0, etc.
      separator_start_point.y--;
      separator_start_point.x += i & 1;
      graphics_draw_horizontal_line_dotted(ctx, separator_start_point, separator_width);
      separator_start_point.x -= i & 1;
    }
  }
}

static int16_t prv_get_header_height_cb(struct MenuLayer *menu_layer, uint16_t second_index,
                                        void *callback_context) {
  ActionMenuLayer *aml = callback_context;
  if (!prv_should_center(aml) || prv_aml_is_short(aml) || aml->num_items == 0) {
    return 0;
  }

  const int16_t line_height = fonts_get_font_height(aml->layout_cache.font);
  const int16_t padding = prv_get_cell_padding(aml);
  const int16_t max_visible_height = line_height * MAX_NUM_VISIBLE_LINES;

  const GRect *bounds = &aml->layer.bounds;
  int16_t total_h = 0;

  for (int16_t idx = 0; idx < aml->num_items; idx++) {
    int16_t item_height = aml->layout_cache.item_heights[idx];
    total_h += MIN(max_visible_height, item_height);
  }

  const int16_t header_padding = 6 * aml->num_items;
  const int16_t header_height = ((bounds->size.h - total_h) / 2) - padding;
  return MAX(header_height - header_padding, 0);
}

static void prv_draw_header_cb(GContext *ctx, const Layer *cell_layer, uint16_t section_index,
                               void *callback_context) {
  // The header here is just being used for padding, so we don't actually need to draw anything.
  return;
}

static void prv_selection_changed_cb(struct MenuLayer *menu_layer, MenuIndex new_index,
                                     MenuIndex old_index, void *callback_context) {
  ActionMenuLayer *aml = callback_context;
  if (new_index.row < aml->num_items) {
    // Enable a new item animation to be scheduled
    prv_unschedule_item_animation(aml);
    aml->selected_index = new_index.row;
  }
}

static void prv_changed_proc(Layer *layer) {
  ActionMenuLayer *aml = (ActionMenuLayer *)layer;
  const GRect *aml_bounds = &layer->bounds;
  GRect menu_layer_frame = *aml_bounds;
#if PBL_ROUND
  if (prv_aml_is_short(aml)) {
    // clip the menu layer to show exactly SHORT_ITEM_MAX_ROWS_SPALDING lines at a time
    const int16_t font_height = fonts_get_font_height(aml->layout_cache.font);
    const int16_t cell_padding = prv_get_cell_padding(aml);
    const int num_visible_rows = MIN(prv_get_num_rows(&aml->menu_layer, 0, aml),
                                     SHORT_ITEM_MAX_ROWS_SPALDING);
    menu_layer_frame.size.h = (font_height * num_visible_rows) +
                              (cell_padding * (num_visible_rows - 1));
    grect_align(&menu_layer_frame, aml_bounds, GAlignCenter, true /* clip */);
  }
#endif
  layer_set_frame(menu_layer_get_layer(&aml->menu_layer), &menu_layer_frame);
}

static void prv_update_proc(Layer *layer, GContext *ctx) {
#if PBL_ROUND
  ActionMenuLayer *aml = (ActionMenuLayer *)layer;
  const int num_rows = prv_get_num_rows(&aml->menu_layer, 0, aml);
  if (prv_aml_is_short(aml) && (num_rows > SHORT_ITEM_MAX_ROWS_SPALDING)) {
    // draw some "content indicator" arrows
    const GRect *aml_bounds = &layer->bounds;
    const GRect *menu_layer_frame = &menu_layer_get_layer(&aml->menu_layer)->frame;
    const int16_t arrow_layer_height = (aml_bounds->size.h - menu_layer_frame->size.h) / 2;

    const int row = prv_get_menu_layer_row(aml, aml->selected_index);
    const GColor bg_color = GColorBlack;
    const GColor fg_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite);

    GRect arrow_rect = (GRect) { .size = GSize(aml_bounds->size.w, arrow_layer_height) };
    if (row >= SHORT_ITEM_MAX_ROWS_SPALDING - 1) {
      grect_align(&arrow_rect, aml_bounds, GAlignTop, true /* clip */);
      content_indicator_draw_arrow(ctx, &arrow_rect, ContentIndicatorDirectionUp, fg_color,
                                   bg_color, GAlignTop);
    }
    if (num_rows - row >= SHORT_ITEM_MAX_ROWS_SPALDING) {
      grect_align(&arrow_rect, aml_bounds, GAlignBottom, true /* clip */);
      content_indicator_draw_arrow(ctx, &arrow_rect, ContentIndicatorDirectionDown, fg_color,
                                   bg_color, GAlignBottom);
    }
  }
#endif
}

static void prv_update_aml_cache(ActionMenuLayer *aml, int selected_index) {
  prv_unschedule_item_animation(aml);

  if (aml->layout_cache.item_heights != NULL) {
    applib_free(aml->layout_cache.item_heights);
    aml->layout_cache.item_heights = NULL;
  }

  if (aml->num_items > 0) {
    // Update the cache of heights.  We do this here to avoid recomputing the same
    // values repeatedly when we call the menu layer height callback.
    aml->layout_cache.item_heights = applib_zalloc(aml->num_items * sizeof(int16_t));
    for (int idx = 0; idx < aml->num_items; idx++) {
      aml->layout_cache.item_heights[idx] = prv_get_item_line_height(aml, idx);
    }
  }

#if PBL_ROUND
  const bool center_focused = !prv_aml_is_short(aml);
  menu_layer_set_center_focused(&aml->menu_layer, center_focused);
#endif

  layer_mark_dirty(&aml->layer);
  menu_layer_reload_data(&aml->menu_layer);
  prv_set_selected_index(aml, selected_index, false /* animated */);
}

// Public API
/////////////////////

void action_menu_layer_click_config_provider(ActionMenuLayer *aml) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_scroll_handler);
  window_set_click_context(BUTTON_ID_UP, aml);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_scroll_handler);
  window_set_click_context(BUTTON_ID_DOWN, aml);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_handler);
  window_set_click_context(BUTTON_ID_SELECT, aml);
}

void action_menu_layer_set_callback(ActionMenuLayer *aml,
                                    ActionMenuLayerCallback cb,
                                    void *context) {
  aml->cb = cb;
  aml->context = context;
}

void action_menu_layer_init(ActionMenuLayer *aml, const GRect *frame) {
  layer_init(&aml->layer, frame);

  // Since menu_layer_set_callbacks() will call the menu functions, we need to initialize
  // the ActionMenuLayer attributes before setting the callbacks onto the menu.
  aml->item_animation = (ActionMenuItemAnimation){};
  aml->layout_cache = (ActionMenuLayoutCache){
    .font = prv_get_item_font()
  };
  aml->layer.property_changed_proc = prv_changed_proc;
  aml->layer.update_proc = prv_update_proc;

  menu_layer_init(&aml->menu_layer, &aml->layer.bounds);
  menu_layer_set_normal_colors(&aml->menu_layer, GColorBlack,
                               PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
#if PBL_ROUND
  menu_layer_pad_bottom_enable(&aml->menu_layer, false);
#endif
  menu_layer_set_callbacks(&aml->menu_layer, aml, &(MenuLayerCallbacks){
      .get_num_rows = prv_get_num_rows,
      .draw_row = prv_draw_row,
      .get_cell_height = prv_get_cell_height_cb,
      .get_separator_height = prv_get_separator_height_cb,
      .draw_separator = prv_draw_separator_cb,
      .get_header_height = prv_get_header_height_cb,
      .draw_header = prv_draw_header_cb,
      .selection_changed = prv_selection_changed_cb
  });

#if !defined(RECOVERY_FW)
  gbitmap_init_with_resource_system(&aml->item_animation.fade_top, SYSTEM_APP,
                                    RESOURCE_ID_ACTION_MENU_FADE_TOP);
  gbitmap_init_with_resource_system(&aml->item_animation.fade_bottom, SYSTEM_APP,
                                    RESOURCE_ID_ACTION_MENU_FADE_BOTTOM);
#endif

  layer_add_child(&aml->layer, menu_layer_get_layer(&aml->menu_layer));
  layer_set_hidden((Layer *)&aml->menu_layer.inverter, true);
  aml->menu_layer.selection_animation_disabled = true;
}

void action_menu_layer_deinit(ActionMenuLayer *aml) {
  if (aml->layout_cache.item_heights) {
    applib_free(aml->layout_cache.item_heights);
  }

  prv_unschedule_item_animation(aml);

#ifndef RECOVERY_FW
  gbitmap_deinit(&aml->item_animation.fade_top);
  gbitmap_deinit(&aml->item_animation.fade_bottom);
#endif

  menu_layer_deinit(&aml->menu_layer);
}

ActionMenuLayer *action_menu_layer_create(GRect frame) {
  ActionMenuLayer *aml = applib_zalloc(sizeof(ActionMenuLayer));
  if (!aml) {
    return NULL;
  }

  action_menu_layer_init(aml, &frame);
  return aml;
}

void action_menu_layer_destroy(ActionMenuLayer *aml) {
  if (!aml) {
    return;
  }

  action_menu_layer_deinit(aml);
  applib_free(aml);
}

void action_menu_layer_set_align(ActionMenuLayer *aml, ActionMenuAlign align) {
  if (!aml) {
    return;
  }
  aml->layout_cache.align = align;
}

void action_menu_layer_set_items(ActionMenuLayer *aml, const ActionMenuItem* items, int num_items,
                                 unsigned default_selected_item, unsigned separator_index) {
  aml->items = items;
  aml->num_items = num_items;
  aml->separator_index = separator_index;
  prv_update_aml_cache(aml, default_selected_item);
}

void action_menu_layer_set_short_items(ActionMenuLayer *aml, const ActionMenuItem* items,
                                       int num_items, unsigned default_selected_item) {
  aml->short_items = items;
  aml->separator_index = 0;
  aml->num_short_items = num_items;
  prv_update_aml_cache(aml, default_selected_item);
}
