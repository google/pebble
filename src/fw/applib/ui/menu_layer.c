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

#include "menu_layer.h"
#include "menu_layer_private.h"

#include "applib/applib_malloc.auto.h"
#include "applib/preferred_content_size.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/text.h"
#include "util/trig.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/animation_timing.h"
#include "applib/ui/click.h"
#include "applib/ui/window.h"
#include "applib/pbl_std/pbl_std.h"
#include "applib/legacy2/ui/menu_layer_legacy2.h"
#include "kernel/pbl_malloc.h"
#include "process_management/process_manager.h"
#include "shell/system_theme.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/math.h"

#include <string.h>

//! @return True if there was an animation to cancel, false otherwise
static bool prv_cancel_selection_animation(MenuLayer *menu_layer);

//////////////////////
// Menu Layer
//
// NOTES: The MenuLayer is built on top of ScrollLayer. It uses ScrollLayer's scrolling and clipping features.
// Since it easily becomes to costly in terms of RAM to hold a layer for each row in the menu in memory,
// the MenuLayer does not use layers for its rows and headers. When a row is about to be displayed,
// it will call out to the client using a callback to get that row drawn.
// Inside the MenuLayer's update_proc (Layer drawing callback), it will call out to its client for each row
// that needs to be drawn, until all visible rows have been drawn.

static void prv_menu_scroll_offset_changed_handler(ScrollLayer *scroll_layer,
                                                   MenuLayer *menu_layer) {
  // TODO: we might need to propagate this event down to MenuLayerCallbacks
}

static void prv_menu_select_click_handler(ClickRecognizerRef recognizer, MenuLayer *menu_layer) {
  // If the selection animation is running, complete it. Note that 2.x apps don't have a selection
  // animation.
  if (menu_layer->animation.animation) {
    animation_set_elapsed(menu_layer->animation.animation,
                          animation_get_duration(menu_layer->animation.animation, true, true));
  }

  // If we're in the middle of scrolling, finish scrolling immediately before handling the select
  // click. We do this to make a transition animation have a consistent position to animate from.
  // Note that animation_set_elapsed isn't supported on 2.x animations. Just skip this step, as
  // no 2.x transitions interact directly with menu layer state.
  if (!process_manager_compiled_with_legacy2_sdk() && menu_layer->scroll_layer.animation) {
    Animation *scroll_layer_animation =
        property_animation_get_animation(menu_layer->scroll_layer.animation);
    animation_set_elapsed(scroll_layer_animation,
                          animation_get_duration(scroll_layer_animation, true, true));
  }

  // Actually handle the click
  if (menu_layer->callbacks.select_click) {
    menu_layer->callbacks.select_click(menu_layer, &menu_layer->selection.index,
                                       menu_layer->callback_context);
  }
}

static void prv_menu_select_long_click_handler(ClickRecognizerRef recognizer,
    MenuLayer *menu_layer) {
  if (menu_layer->callbacks.select_long_click) {
    menu_layer->callbacks.select_long_click(menu_layer, &menu_layer->selection.index,
        menu_layer->callback_context);
  }
}

void menu_up_click_handler(ClickRecognizerRef recognizer, MenuLayer *menu_layer) {
  const bool up = true;
  const bool animated = true;
  menu_layer_set_selected_next(menu_layer, up, MenuRowAlignCenter, animated);
}

void menu_down_click_handler(ClickRecognizerRef recognizer, MenuLayer *menu_layer) {
  const bool up = false;
  const bool animated = true;
  menu_layer_set_selected_next(menu_layer, up, MenuRowAlignCenter, animated);
}

static void prv_menu_click_config_provider(MenuLayer *menu_layer) {
  // The config that gets passed in, has already the UP and DOWN buttons configured
  // we're overriding the default behavior here:
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100 /*ms*/,
      (ClickHandler)menu_up_click_handler);
  if (menu_layer->callbacks.select_click) {
    window_single_click_subscribe(BUTTON_ID_SELECT, (ClickHandler)prv_menu_select_click_handler);
  }
  if (menu_layer->callbacks.select_long_click) {
    window_long_click_subscribe(BUTTON_ID_SELECT, 0,
        (ClickHandler)prv_menu_select_long_click_handler, NULL);
  }
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100 /*ms*/,
      (ClickHandler)menu_down_click_handler);
}

static inline uint16_t prv_menu_layer_get_num_sections(MenuLayer *menu_layer) {
  if (menu_layer->callbacks.get_num_sections) {
    return menu_layer->callbacks.get_num_sections(menu_layer, menu_layer->callback_context);
  } else {
    return 1; // default
  }
}

static inline uint16_t prv_menu_layer_get_num_rows(MenuLayer *menu_layer, uint16_t section_index) {
  if (section_index == MENU_INDEX_NOT_FOUND) {
    return 0;
  }

  if (menu_layer->callbacks.get_num_rows) {
    return menu_layer->callbacks.get_num_rows(menu_layer, section_index,
        menu_layer->callback_context);
  } else {
    return 1;  // default
  }
}

static inline int16_t prv_menu_layer_get_separator_height(MenuLayer *menu_layer,
    MenuIndex *cell_index) {
  if (menu_layer->callbacks.get_separator_height) {
    return menu_layer->callbacks.get_separator_height(menu_layer, cell_index, menu_layer->callback_context);
  } else if (process_manager_compiled_with_legacy2_sdk()) {
    return MENU_CELL_LEGACY2_BASIC_SEPARATOR_HEIGHT;
  } else {
    return MENU_CELL_BASIC_SEPARATOR_HEIGHT;
  }
}

static inline int16_t prv_menu_layer_get_header_height(MenuLayer *menu_layer,
    uint16_t section_index) {
  if (menu_layer->callbacks.get_header_height) {
    return menu_layer->callbacks.get_header_height(menu_layer, section_index, menu_layer->callback_context);
  } else {
    return 0; // default
  }
}

static inline int16_t prv_menu_layer_get_cell_height(MenuLayer *menu_layer, MenuIndex
    *cell_index, bool provide_correct_selection_index) {
  if (menu_layer->callbacks.get_cell_height) {
    const MenuIndex prev_selection_index = menu_layer->selection.index;
    if (!provide_correct_selection_index) {
      menu_layer->selection.index.section = MENU_INDEX_NOT_FOUND;
    }
    const int16_t result = menu_layer->callbacks.get_cell_height(menu_layer, cell_index,
                                                                 menu_layer->callback_context);

    menu_layer->selection.index = prev_selection_index;
    return result;
  } else {
    return menu_cell_basic_cell_height();  // default
  }
}

static inline void prv_menu_layer_draw_separator(MenuLayer *menu_layer, Layer *cell_layer,
    MenuCellSpan *cursor, GContext* ctx) {
  const int16_t y = cursor->y - cursor->sep;
  if (menu_layer->callbacks.draw_separator) {
    // Save current drawing state:
    GDrawState prev_state = graphics_context_get_drawing_state(ctx);
    GRect prev_bounds = cell_layer->bounds;
    GRect new_bounds = prev_bounds;

    // Translate the drawing_box to the bounds of the layer:
    ctx->draw_state.drawing_box.origin.y += y;
    ctx->draw_state.drawing_box.size.h = cursor->h;

    // Set the height appropriately on the cell layer
    new_bounds.size.h = cursor->sep;
    layer_set_bounds(cell_layer, &new_bounds);

    // Call the client, to ask to draw the separator:
    menu_layer->callbacks.draw_separator(ctx, cell_layer, &cursor->index, menu_layer->callback_context);

    // Restore current drawing state:
    graphics_context_set_drawing_state(ctx, prev_state);

    // Restore the layer bounds:
    layer_set_bounds(cell_layer, &prev_bounds);
  } else {
    graphics_fill_rect(
        ctx, &GRect(0, y, menu_layer->scroll_layer.layer.bounds.size.w, cursor->sep));
  }
}

static void prv_prepare_row(GContext *ctx, MenuLayer *menu_layer,
                            Layer *cell_layer, bool highlight) {
  if (!process_manager_compiled_with_legacy2_sdk()) {
    GColor *colors = (highlight) ? menu_layer->highlight_colors : menu_layer->normal_colors;
    ctx->draw_state.fill_color = colors[MenuLayerColorBackground];
    ctx->draw_state.text_color = colors[MenuLayerColorForeground];
    ctx->draw_state.tint_color = colors[MenuLayerColorForeground];
    if (!gcolor_is_transparent(ctx->draw_state.fill_color)) {
      graphics_fill_rect(ctx, &cell_layer->bounds);
    }
  }
  cell_layer->is_highlighted = highlight;
}

static void prv_prepare_and_draw_row(GContext *ctx, MenuLayer *menu_layer,
                                     Layer *cell_layer, MenuCellSpan *cursor, bool highlight) {
  prv_prepare_row(ctx, menu_layer, cell_layer, highlight);
  const GRect prev_bounds = cell_layer->bounds;

  // in theory, we could decrement the origin by cell_content_origin_offset_y after the call
  // in practice once shouldn't trust the draw_row implementation
  const int16_t draw_box_origin_y = ctx->draw_state.drawing_box.origin.y;
  ctx->draw_state.drawing_box.origin.y += menu_layer->animation.cell_content_origin_offset_y;

  // Call the client, to ask to draw the row:
  menu_layer->callbacks.draw_row(ctx, cell_layer, &cursor->index, menu_layer->callback_context);

  ctx->draw_state.drawing_box.origin.y = draw_box_origin_y;
  cell_layer->bounds = prev_bounds;
}

static inline void prv_menu_layer_draw_row(MenuLayer *menu_layer, Layer *cell_layer,
    MenuCellSpan *cursor, GContext* ctx) {
  if (cursor->h == 0) {
    // cell has height 0, no need to draw anything.
    return;
  }

  cell_layer->bounds.size.h = cursor->h;
  cell_layer->frame.size.h = cursor->h;
  cell_layer->frame.origin.y = cursor->y;

  // Save current drawing state:
  GDrawState prev_state = graphics_context_get_drawing_state(ctx);

  // Translate the drawing_box to the bounds of the layer:
  ctx->draw_state.drawing_box.origin.y += cursor->y;
  ctx->draw_state.drawing_box.size.h = cursor->h;

  // Use the drawing_box as a clipper to force the content to only use
  // the space available to it and remove overflow
  const GRect *const rect_clipper = (const GRect *const)&ctx->draw_state.drawing_box;
  grect_clip((GRect *const)&ctx->draw_state.clip_box, rect_clipper);

  const bool fully_covered = grect_equal(&cell_layer->frame, &menu_layer->inverter.layer.frame);
  const bool partial = grect_overlaps_grect(&cell_layer->frame, &menu_layer->inverter.layer.frame);

  if (fully_covered || !partial) {
    prv_prepare_and_draw_row(ctx, menu_layer, cell_layer, cursor, fully_covered);
  } else {
    // Render the full cell without highlight
    prv_prepare_and_draw_row(ctx, menu_layer, cell_layer, cursor, false);

    // Set clipper to the inverter layer in clipping box coordinates
    GRect selection_clipper;
    layer_get_global_frame(&menu_layer->inverter.layer, &selection_clipper);
    grect_clip((GRect *const)&ctx->draw_state.clip_box, &selection_clipper);

    // Render with highlight
    prv_prepare_and_draw_row(ctx, menu_layer, cell_layer, cursor, true);
  }

  // Restore current drawing state:
  graphics_context_set_drawing_state(ctx, prev_state);
}

static inline void prv_menu_layer_draw_section_header(MenuLayer *menu_layer, Layer *cell_layer,
    MenuCellSpan *cursor, GContext* ctx) {
  cell_layer->bounds.size.h = cursor->h;
  cell_layer->frame.size.h = cursor->h;
  cell_layer->frame.origin.y = cursor->y;

  // Callback to get the shared cell instance filled with data:

  // Save current drawing state:
  GDrawState prev_state = graphics_context_get_drawing_state(ctx);

  // Translate the drawing_box to the bounds of the layer:
  ctx->draw_state.drawing_box.origin.y += cursor->y;
  ctx->draw_state.drawing_box.size.h = cursor->h;

  const GRect *const rect_clipper = (const GRect *const)&ctx->draw_state.drawing_box;
  grect_clip((GRect *const)&ctx->draw_state.clip_box, rect_clipper);

  prv_prepare_row(ctx, menu_layer, cell_layer, false);

  // Call the client, to ask to draw the section:
  menu_layer->callbacks.draw_header(ctx, cell_layer, cursor->index.section, menu_layer->callback_context);

  // Restore current drawing state:
  graphics_context_set_drawing_state(ctx, prev_state);
}

static void prv_menu_layer_render_section_from_iterator(MenuIterator *iterator) {
  MenuRenderIterator *it = (MenuRenderIterator*)iterator;
  const int16_t top_diff = it->it.cursor.y - it->content_top_y;
  const bool is_header_in_frame = (top_diff >= 0 && it->it.cursor.y <= it->content_bottom_y) ||
      (it->it.cell_bottom_y >= it->content_top_y && it->it.cell_bottom_y <= it->content_bottom_y);
  if (is_header_in_frame) {
    // Draw section header:
    prv_menu_layer_draw_section_header(it->it.menu_layer, &it->cell_layer, &it->it.cursor, it->ctx);
    // Draw the separator on top of the cell:
    if (top_diff >= it->it.cursor.sep) {
      prv_menu_layer_draw_separator(it->it.menu_layer, &it->cell_layer, &it->it.cursor, it->ctx);
    }
  }
}

static void prv_menu_layer_render_row_from_iterator(MenuIterator *iterator) {
  MenuRenderIterator *it = (MenuRenderIterator*)iterator;
  const int16_t iter_y = it->it.cursor.y;

  const int16_t top_diff = it->it.cursor.y - it->content_top_y;
  const bool is_row_in_frame = (top_diff >= 0 && it->it.cursor.y <= it->content_bottom_y) ||
      (it->it.cell_bottom_y >= it->content_top_y && it->it.cell_bottom_y <= it->content_bottom_y);
  if (is_row_in_frame) {
    it->cursor_in_frame = true;
    // Draw the cell
    prv_menu_layer_draw_row(it->it.menu_layer, &it->cell_layer, &it->it.cursor, it->ctx);
    // Draw the separator on top of the cell
    if (top_diff >= it->it.cursor.sep) {
      prv_menu_layer_draw_separator(it->it.menu_layer, &it->cell_layer, &it->it.cursor, it->ctx);
    }
    // Update the cache with the center-most row
    it->it.cursor.y = iter_y;
    if (false == it->cache_set) {
      it->new_cache = it->it.cursor;
      it->cache_set = true;
    }
  } else {
    if (it->cursor_in_frame) {
      it->it.should_continue = false;
    }
  }
  it->it.cursor.y = iter_y;
}

// NOTE: The following two iteration functions are asymmetrical!
// In other words, even one is going downward and the other upward, there are some subtle
// differences. Most importantly: the downward function calls the row_callback_after_geometry for
// the row the iterator's cursor is currently set to, while the upward function skips over the
// current row.
// Secondly, section_callback is only called when a sections is encountered while walking.
// For example, if the current index is (section: 0, row: 0), the section_callback for section 0
// will only be called when walking upward.

static void prv_menu_layer_walk_downward_from_iterator(MenuIterator *it) {
  const uint16_t num_sections = prv_menu_layer_get_num_sections(it->menu_layer);
  it->should_continue = true;
  for (;;) { // sections
    const uint16_t num_rows_in_section = prv_menu_layer_get_num_rows(it->menu_layer,
        it->cursor.index.section);
    for (;;) { // rows
      if (it->cursor.index.row >= num_rows_in_section) {
        // Reached last row
        break;
      }

      if (it->row_callback_before_geometry) {
        it->row_callback_before_geometry(it);
      }

      it->cursor.h = prv_menu_layer_get_cell_height(it->menu_layer, &it->cursor.index, true);
      it->cell_bottom_y = it->cursor.y + it->cursor.h;

      // ROW
      if (it->row_callback_after_geometry) {
        it->row_callback_after_geometry(it);
      }
      if (it->should_continue == false) {
        return;
      }

      // Next row:
      it->cursor.sep = prv_menu_layer_get_separator_height(it->menu_layer, &it->cursor.index);
      it->cursor.y = it->cell_bottom_y; // Bottom of previous cell is y of the next cell

      // Don't leave space for the separator for the (nonexistent) row after the last row.
      // This doesn't impact cell drawing in this loop (this condition will only trip on the last run).
      // But, other parts of the system rely on the cursor being set properly at the end of this iteration.
      if (it->cursor.index.row < num_rows_in_section - 1 || it->cursor.index.section < num_sections - 1) {
        it->cursor.y += it->cursor.sep;
      }
      ++(it->cursor.index.row);
    } // for() rows

    // Next section:
    ++(it->cursor.index.section);
    if (it->cursor.index.section >= num_sections) {
      break;
      // Reached last section
    }
    it->cursor.index.row = 0;
    it->cursor.h = prv_menu_layer_get_header_height(it->menu_layer, it->cursor.index.section);
    it->cell_bottom_y = it->cursor.y + it->cursor.h;

    // SECTION
    if (it->cursor.h > 0) {
      it->section_callback(it);
      it->cursor.sep = prv_menu_layer_get_separator_height(it->menu_layer, &it->cursor.index);
      it->cursor.y = it->cell_bottom_y + it->cursor.sep;
    }

    if (it->should_continue == false) {
      return;
    }

  } // for() sections
}

static void prv_menu_layer_walk_upward_from_iterator(MenuIterator *it) {
  it->should_continue = true;
  for (;;) { // sections
    for (;;) { // rows
      // Previous row
      if (it->cursor.index.row == 0) {
        // Reached top-most row in current section
        break;
      }
      --(it->cursor.index.row);

      if (it->row_callback_before_geometry) {
        it->row_callback_before_geometry(it);
      }

      // when walking upwards, selected_index isn't set yet here
      // hence, the heights are the sizes as they were before the selection changed
      it->cursor.h = prv_menu_layer_get_cell_height(it->menu_layer, &it->cursor.index, false);
      it->cursor.sep = prv_menu_layer_get_separator_height(it->menu_layer, &it->cursor.index);
      it->cursor.y -= it->cursor.h + it->cursor.sep;
      it->cell_bottom_y = it->cursor.y + it->cursor.h;

      // ask for height again, this time with correct selection status
      it->cursor.h = prv_menu_layer_get_cell_height(it->menu_layer, &it->cursor.index, true);

      // ROW
      if (it->row_callback_after_geometry) {
        it->row_callback_after_geometry(it);
      }

      if (it->should_continue == false) {
        break;
      }
    } // for() rows

    if (it->cursor.index.row == 0) {
      // If top-most row, layout the section header
      it->cursor.h = prv_menu_layer_get_header_height(it->menu_layer, it->cursor.index.section);
      it->cursor.sep = prv_menu_layer_get_separator_height(it->menu_layer, &it->cursor.index);

      if (it->cursor.h > 0) {
        // Bottom of previous cell is y of the next cell
        const int16_t total_height = it->cursor.h + it->cursor.sep;
        if (total_height > it->cursor.y) {
          // If the total height is greater than the cursor y, don't
          // add in space to accommodate the separator as the downwards callback
          // will add it for us.
          it->cursor.y -= it->cursor.h;
        } else {
          it->cursor.y -= total_height;
        }
        it->cell_bottom_y = it->cursor.y + it->cursor.h;

        // SECTION
        it->section_callback(it);
      }
    }

    if (it->should_continue == false) {
      return;
    }

    // Previous section:
    if (it->cursor.index.section == 0) {
      // Reached top
      break;
    }
    --(it->cursor.index.section);
    // -1 will happen when entering for() rows
    it->cursor.index.row = it->menu_layer->callbacks.get_num_rows(it->menu_layer,
        it->cursor.index.section, it->menu_layer->callback_context);

  } // for() sections
}

static void NOINLINE prv_draw_background(MenuLayer *menu_layer, GContext *ctx,
                                Layer *bg_layer, bool highlight) {
  GDrawState prev_state = graphics_context_get_drawing_state(ctx);

  const GRect *bounds = &bg_layer->bounds;
  ctx->draw_state.drawing_box.origin.y = bounds->origin.y;
  ctx->draw_state.drawing_box.size.h = bounds->size.h;

  MenuLayerDrawBackgroundCallback draw_background_cb = menu_layer->callbacks.draw_background;
  if (draw_background_cb) {
    draw_background_cb(ctx, bg_layer, false, menu_layer->callback_context);
  } else if (highlight) {
    ctx->draw_state.fill_color = menu_layer->highlight_colors[MenuLayerColorBackground];
    graphics_fill_rect(ctx, bounds);
  } else {
    ctx->draw_state.fill_color = menu_layer->normal_colors[MenuLayerColorBackground];
    graphics_fill_rect(ctx, bounds);
  }

  graphics_context_set_drawing_state(ctx, prev_state);
}

void menu_layer_update_proc(Layer *scroll_content_layer, GContext* ctx) {
  MenuLayer *menu_layer = (MenuLayer*)(((uint8_t*)scroll_content_layer) -
      offsetof(MenuLayer, scroll_layer.content_sublayer));
  const GSize frame_size = menu_layer->scroll_layer.layer.frame.size;
  const int16_t content_top_y = -scroll_layer_get_content_offset(&menu_layer->scroll_layer).y;
  const int16_t content_bottom_y = content_top_y + frame_size.h;

  if (!process_manager_compiled_with_legacy2_sdk()) {
    prv_draw_background(menu_layer, ctx, &menu_layer->scroll_layer.layer, false);
  }

  MenuRenderIterator *render_iter = applib_type_malloc(MenuRenderIterator);
  PBL_ASSERTN(render_iter);

  if (menu_layer->center_focused) {
    // in this mode, the selected row is always the best candidate for the cache
    menu_layer->cache.cursor = menu_layer->selection;
  }

  *render_iter = (MenuRenderIterator) {
    .it = {
      .menu_layer = menu_layer,
      .cursor = menu_layer->cache.cursor,
      .row_callback_after_geometry = prv_menu_layer_render_row_from_iterator,
      .section_callback = prv_menu_layer_render_section_from_iterator,
    },
    .ctx = ctx,
    .content_top_y = content_top_y,
    .content_bottom_y = content_bottom_y,
    .cache_set = false,
    .cursor_in_frame = false,
    .cell_layer = {
      .bounds = {
        .size = {
          .w = frame_size.w,
        },
      },
      .frame = {
        .size = {
          .w = frame_size.w,
        },
      },
    },
  };
  layer_add_child(&menu_layer->scroll_layer.content_sublayer, &render_iter->cell_layer);

  // Set separator color
  graphics_context_set_fill_color(ctx, GColorBlack);

  // We're caching the y-coord and index of the one row, as our "anchor" point in the menu.
  // We'll be walking downward and upward from that index until the rows fall off the screen.
  const int16_t content_center_y = (content_top_y + content_bottom_y) / 2;
  if (content_center_y >= menu_layer->cache.cursor.y) {
    // Walk downward from cache.cursor, then upward
    prv_menu_layer_walk_downward_from_iterator(&render_iter->it);
    render_iter->it.cursor = menu_layer->cache.cursor;
    prv_menu_layer_walk_upward_from_iterator(&render_iter->it);
  } else {
    // Walk upward from cache.cursor, then downward
    prv_menu_layer_walk_upward_from_iterator(&render_iter->it);
    render_iter->it.cursor = menu_layer->cache.cursor;
    prv_menu_layer_walk_downward_from_iterator(&render_iter->it);
  }
  layer_remove_from_parent(&render_iter->cell_layer);

  // Assign the new cache:
  menu_layer->cache.cursor = render_iter->new_cache;

  task_free(render_iter);
}

void menu_layer_init_scroll_layer_callbacks(MenuLayer *menu_layer) {
  ScrollLayer *scroll_layer = &menu_layer->scroll_layer;
  scroll_layer_set_callbacks(scroll_layer, (ScrollLayerCallbacks) {
    .click_config_provider = (ClickConfigProvider)prv_menu_click_config_provider,
    .content_offset_changed_handler = (ScrollLayerCallback)prv_menu_scroll_offset_changed_handler,
  });
  scroll_layer->content_sublayer.update_proc = (LayerUpdateProc)menu_layer_update_proc;
}

static void prv_set_center_focused(MenuLayer *menu_layer, bool center_focused) {
  menu_layer->center_focused = center_focused;
  scroll_layer_set_clips_content_offset(&menu_layer->scroll_layer, !center_focused);
}

void menu_layer_init(MenuLayer *menu_layer, const GRect *frame) {
  *menu_layer = (MenuLayer) {
    .pad_bottom = true,
  };

  ScrollLayer *scroll_layer = &menu_layer->scroll_layer;
  scroll_layer_init(scroll_layer, frame);
  menu_layer_init_scroll_layer_callbacks(menu_layer);
  scroll_layer_set_shadow_hidden(scroll_layer, true);
  scroll_layer_set_context(scroll_layer, menu_layer);

  menu_layer_set_normal_colors(menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(menu_layer, GColorBlack, GColorWhite);

  InverterLayer *inverter = &menu_layer->inverter;
  inverter_layer_init(inverter, &GRectZero);
  scroll_layer_add_child(scroll_layer, &inverter->layer);

  // Hide inverter layer by default for 3.0 apps
  layer_set_hidden(inverter_layer_get_layer(&menu_layer->inverter), true);

#if PBL_ROUND
  prv_set_center_focused(menu_layer, true);
#endif
}

MenuLayer* menu_layer_create(GRect frame) {
  MenuLayer *layer = applib_type_malloc(MenuLayer);
  if (layer) {
    menu_layer_init(layer, &frame);
  }
  return layer;
}

void menu_layer_pad_bottom_enable(MenuLayer *menu_layer, bool enable) {
  menu_layer->pad_bottom = enable;
}

void menu_layer_deinit(MenuLayer *menu_layer) {
  prv_cancel_selection_animation(menu_layer);
  layer_deinit(&menu_layer->inverter.layer);
  scroll_layer_deinit(&menu_layer->scroll_layer);
}

void menu_layer_destroy(MenuLayer* menu_layer) {
  if (menu_layer == NULL) {
    return;
  }
  menu_layer_deinit(menu_layer);
  applib_free(menu_layer);
}

Layer* menu_layer_get_layer(const MenuLayer *menu_layer) {
  return &((MenuLayer *)menu_layer)->scroll_layer.layer;
}

ScrollLayer* menu_layer_get_scroll_layer(const MenuLayer *menu_layer) {
  return &((MenuLayer *)menu_layer)->scroll_layer;
}

typedef struct MenuPrimeCacheIterator {
  MenuIterator it;
  bool cache_set;
} MenuPrimeCacheIterator;

static void prv_menu_layer_iterator_noop_callback(MenuIterator *it) {
  (void)it;
}

static void prv_menu_layer_iterator_prime_cache_callback(MenuIterator *iterator) {
  MenuPrimeCacheIterator *it = (MenuPrimeCacheIterator*)iterator;
  if (false == it->cache_set) {
    // Prime the cursor cache:
    it->it.menu_layer->cache.cursor = it->it.cursor;
    // Set initial selection too:
    it->it.menu_layer->selection = it->it.cursor;
    it->cache_set = true;
  }
}

//! Calculate the total height of all row cells and section headers,
//! and assign the appropriate content size to the scroll_layer.
//! Also prime the offset cache on the fly.
void menu_layer_update_caches(MenuLayer *menu_layer) {
  // Save the currently selected cell index.
  MenuIndex selected_index = menu_layer_get_selected_index(menu_layer);
  MenuPrimeCacheIterator it = {
    .it = {
      .menu_layer = menu_layer,
      .row_callback_after_geometry = prv_menu_layer_iterator_prime_cache_callback,
      .section_callback = prv_menu_layer_iterator_noop_callback,
      .should_continue = true,
      .cursor = {
        // Section header of current section (0) is not part of the walk down, set it "manually"
        .y = prv_menu_layer_get_header_height(menu_layer, 0),
        .sep = prv_menu_layer_get_separator_height(menu_layer, 0)
      },
    },
    .cache_set = false,
  };

  if (prv_menu_layer_get_header_height(menu_layer, 0) != 0) {
    // We have to add the separator height, as when drawing down -> up, we render the separator
    // for the row above before proceeding down. We only render this separator at the top if we
    // have headers on the first section.
    it.it.cursor.y += it.it.cursor.sep;
  }

  // handle special case of just one row so that calls for menu_layer_get_selected_index()
  // will already answer correctly
  if (prv_menu_layer_get_num_sections(menu_layer) == 1 &&
      prv_menu_layer_get_num_rows(menu_layer, 0) == 1) {
    menu_layer->selection.index = MenuIndex(0, 0);
  }

  prv_menu_layer_walk_downward_from_iterator(&it.it);
  int16_t total_height = it.it.cursor.y;
  if (menu_layer->pad_bottom) {
    total_height += MENU_LAYER_BOTTOM_PADDING;
  }

  // Set the content size on the scroll layer, so all the rows will fit onto the content layer:
  const GSize frame_size = menu_layer->scroll_layer.layer.frame.size;
  scroll_layer_set_content_size(&menu_layer->scroll_layer, GSize(frame_size.w, total_height));

  // Set the selected cell again:
  const bool animated = false;
  menu_layer_set_selected_index(menu_layer, selected_index, MenuRowAlignNone, animated);
}

void menu_layer_set_callbacks(MenuLayer *menu_layer, void *callback_context,
                            const MenuLayerCallbacks *callbacks) {
  if (callbacks) {
    menu_layer->callbacks = *callbacks;
    PBL_ASSERTN(menu_layer->callbacks.draw_row);
    PBL_ASSERTN(menu_layer->callbacks.get_num_rows);
  }

  menu_layer->callback_context = callback_context;

  menu_layer_reload_data(menu_layer);
}

void menu_layer_set_callbacks_by_value(MenuLayer *menu_layer, void *callback_context,
                                       MenuLayerCallbacks callbacks) {
  menu_layer_set_callbacks(menu_layer, callback_context, &callbacks);
}

void menu_layer_set_click_config_onto_window(MenuLayer *menu_layer, struct Window *window) {
  // Delegate this directly to the scroll layer:
  scroll_layer_set_click_config_onto_window(&menu_layer->scroll_layer, window);
}

//! @returns 0 if A and B are equal, 1 if A has a higher section & row combination than B or else -1
int16_t menu_index_compare(const MenuIndex *a, const MenuIndex *b) {
  const int16_t max_rows = MAX(a->row, b->row) + 1;
  const int32_t a_abs = ((a->section * max_rows) + a->row);
  const int32_t b_abs = ((b->section * max_rows) + b->row);
  if (a_abs > b_abs) {
    return 1;
  } else if (a_abs < b_abs) {
    return -1;
  } else {
    return 0;
  }
}

static void prv_selection_complete(Animation *animation, bool finished, void *context) {
  MenuLayer *menu_layer = (MenuLayer *) context;
  menu_layer->animation.animation = NULL;
}

static bool prv_cancel_selection_animation(MenuLayer *menu_layer) {
  const bool result = animation_is_scheduled(menu_layer->animation.animation);
  if (result) {
    animation_unschedule(menu_layer->animation.animation);
  }
  menu_layer->animation.animation = NULL;
  return result;
}

#define TOP_DOWN_PX  7
#define BOTTOM_DOWN_PX 10
static void prv_setup_selection_animation(MenuLayer *menu_layer, bool up) {
  // Move selection inverter layer:
  const int16_t w = menu_layer->scroll_layer.layer.frame.size.w;
  const GSize size = GSize(w, menu_layer->selection.h);

  // Step 1. Bring down TOP of cell by TOP_DOWN_PX.
  GRect from;
  if (menu_layer->animation.animation) {
    from = menu_layer->animation.target;
    prv_cancel_selection_animation(menu_layer);
  } else {
    from = menu_layer->inverter.layer.frame;
  }
  GRect target = (GRect) {
    .origin = {
      .x = 0,
      .y = from.origin.y + ((up) ? 0 : TOP_DOWN_PX),
    },
    .size = {
      .w = size.w,
      .h = size.h - TOP_DOWN_PX,
    }
  };

  Animation *a1 = (Animation *) property_animation_create_layer_frame(&menu_layer->inverter.layer,
                                                                      &from, &target);
  animation_set_duration(a1, 100);
  animation_set_curve(a1, AnimationCurveEaseOut);
  animation_set_auto_destroy(a1, true);

  // Step 2. Skip the top of the highlight down to the top of the newly selected cell,
  // and have the selection BOTTOM_DOWN_PX below the selected cell.
  from.origin.y = menu_layer->selection.y - ((up) ? BOTTOM_DOWN_PX : 0);
  from.size.h = size.h + BOTTOM_DOWN_PX;

  // Step 3. Bring up the bottom of the highlight to only cover the selected cell.
  target.origin.y = menu_layer->selection.y;
  target.size = size;

  Animation *a2 = (Animation *) property_animation_create_layer_frame(&menu_layer->inverter.layer,
                                                                      &from, &target);
  animation_set_duration(a2, 250);
  animation_set_curve(a2, AnimationCurveEaseOut);
  animation_set_auto_destroy(a2, true);

  Animation *a = animation_sequence_create(a1, a2, NULL);

  animation_set_auto_destroy(a, true); // [MJ] false?
  animation_set_handlers(a, (AnimationHandlers) { .stopped = prv_selection_complete }, menu_layer);

  menu_layer->animation.animation = a;
  menu_layer->animation.target = target;
  animation_schedule(a);
}


static void prv_menu_layer_update_selection_highlight(MenuLayer *menu_layer, bool up,
                                                      bool animated,
                                                      bool change_ongoing_animation) {
  if (menu_layer->center_focused || menu_layer->selection_animation_disabled) {
    // animation on center_focused will not happen by moving the selection
    // see prv_schedule_center_focus_animation()
    animated = false;
  }

  Animation *scroll_animation = (Animation *) menu_layer->scroll_layer.animation;
  if (change_ongoing_animation && animation_is_scheduled(scroll_animation)) {
    animation_unschedule(scroll_animation);
  }
  if (change_ongoing_animation && animated && !process_manager_compiled_with_legacy2_sdk()) {
    prv_setup_selection_animation(menu_layer, up);
  } else {
    if (change_ongoing_animation) {
      prv_cancel_selection_animation(menu_layer);
    }
    // Move selection inverter layer:
    const int16_t w = menu_layer->scroll_layer.layer.frame.size.w;
    const GSize size = GSize(w, menu_layer->selection.h);
    menu_layer->inverter.layer.bounds = (GRect) {
      .origin = { 0, 0 },
      .size = size,
    };
    menu_layer->inverter.layer.frame = (GRect) {
      .origin = {
        .x = 0,
        .y = menu_layer->selection.y,
      },
      .size = size,
    };
    layer_mark_dirty(&menu_layer->inverter.layer);
  }
}

static MenuRowAlign prv_corrected_scroll_align(MenuLayer *menu_layer, MenuRowAlign align) {
  if (menu_layer->center_focused) {
    return MenuRowAlignCenter;
  }
  return align;
}

static void prv_menu_layer_update_selection_scroll_position(MenuLayer *menu_layer,
                                                            MenuRowAlign scroll_align,
                                                            bool animated) {
  scroll_align = prv_corrected_scroll_align(menu_layer, scroll_align);

  if (scroll_align != MenuRowAlignNone) {
    int16_t y;
    const GSize frame_size = menu_layer->scroll_layer.layer.frame.size;
    // Scroll to the right position:
    switch (scroll_align) {
      case MenuRowAlignTop:
        y = - menu_layer->selection.y;
        break;

      case MenuRowAlignBottom:
        y = frame_size.h - menu_layer->selection.y - menu_layer->selection.h;
        break;

      default:
      case MenuRowAlignCenter:
        y = (frame_size.h / 2) - menu_layer->selection.y - (menu_layer->selection.h / 2);
        break;
    }

    if (menu_layer->center_focused) {
      // animation on center_focus will not happen via scrolling
      // see prv_schedule_center_focus_animation()
      animated = false;
    }
    // scroll layer will take care of clipping if necessary
    scroll_layer_set_content_offset(&menu_layer->scroll_layer, GPoint(0, y), animated);
  }
}

typedef struct MenuSelectIndexIterator {
  MenuIterator it;
  MenuCellSpan selection;
  bool did_change_selection:1;
} MenuSelectIndexIterator;

static void prv_menu_layer_iterator_selection_index_callback(MenuIterator *iterator) {
  MenuSelectIndexIterator *it = (MenuSelectIndexIterator*)iterator;
  if (!menu_index_compare(&it->it.cursor.index, &it->selection.index)) {
    it->it.menu_layer->selection = it->it.cursor;
    it->it.should_continue = false;
    it->did_change_selection = true;
  }
}

static void prv_menu_layer_iterator_update_selection(MenuIterator *iterator) {
  MenuLayer *menu_layer = iterator->menu_layer;
  if (menu_index_compare(&iterator->cursor.index, &menu_layer->selection.index) == 0) {
    menu_layer->selection = iterator->cursor;
  }
}

static void prv_walk_with_iterator(const int8_t direction, MenuIterator *it) {
  MenuLayer *menu_layer = it->menu_layer;
  const int16_t prev_selection_height = menu_layer->selection.h;
  const MenuIndex prev_selection_index = menu_layer->selection.index;

  if (menu_layer->center_focused) {
    it->row_callback_before_geometry = it->row_callback_after_geometry;
    it->row_callback_after_geometry = prv_menu_layer_iterator_update_selection;

    // invalidate current selection while iterating
    menu_layer->selection.index.section = MENU_INDEX_NOT_FOUND;
  }

  if (direction < 0) {
    // new index comes before current selection
    prv_menu_layer_walk_upward_from_iterator(it);
  } else if (direction > 0) {
    // new index comes after current selection
    prv_menu_layer_walk_downward_from_iterator(it);
  }

  // potentially restore previous state of selection
  if (menu_layer->selection.index.section == MENU_INDEX_NOT_FOUND) {
    menu_layer->selection.index = prev_selection_index;
    menu_layer->selection.h = prev_selection_height;
  }
}

typedef struct {
  MenuLayer *menu_layer;
  bool up;
} CenterFocusSelectionAnimationState;

static CenterFocusSelectionAnimationState prv_center_focus_animation_state(Animation *animation) {
  PropertyAnimation *prop_anim = (PropertyAnimation *)animation;
  CenterFocusSelectionAnimationState result = {};
  property_animation_get_subject(prop_anim, (void **)&result.menu_layer);
  property_animation_to(prop_anim, &result.up, sizeof(result.up), false);
  return result;
}

static void prv_center_focus_animation_setup(Animation *animation) {
  CenterFocusSelectionAnimationState state = prv_center_focus_animation_state(animation);
  state.menu_layer->animation.cell_content_origin_offset_y = 0;
  state.menu_layer->animation.selection_extend_top = 0;
  state.menu_layer->animation.selection_extend_bottom = 0;
}

static void prv_announce_selection_changed(MenuLayer *menu_layer, MenuIndex prev_index) {
  if (!menu_layer->callbacks.selection_changed) {
    return;
  }

  menu_layer->callbacks.selection_changed(menu_layer, menu_layer->selection.index,
                                          prev_index, menu_layer->callback_context);
}

void prv_center_focus_animation_update_impl(Animation *animation,
                                            bool second_half,
                                            AnimationProgress adjusted_progress) {
  CenterFocusSelectionAnimationState state = prv_center_focus_animation_state(animation);

  // values as seen in the design videos
  const int16_t move_in_dist = 16;
  const int16_t move_out_dist = 4;
  const int16_t abs_content_offset = second_half ?
                                     interpolate_int16(adjusted_progress, move_out_dist, 0) :
                                     interpolate_int16(adjusted_progress, 0, move_in_dist);
  const int16_t content_offset = (state.up ? abs_content_offset : -abs_content_offset) / 2;
  state.menu_layer->animation.cell_content_origin_offset_y = content_offset;

  const bool reached_second_half_before = menu_index_compare(
      &state.menu_layer->selection.index, &state.menu_layer->animation.new_selection.index) == 0;

  if (second_half) {
    if (!reached_second_half_before) {
      const MenuIndex prev_index = state.menu_layer->selection.index;
      state.menu_layer->selection = state.menu_layer->animation.new_selection;
      prv_announce_selection_changed(state.menu_layer, prev_index);
    }
    // this favors robustness over efficiency - the functions might be called multiple times
    // but instead of keeping track (which is more difficult that it seems)
    // we simply call them too often
    prv_menu_layer_update_selection_scroll_position(state.menu_layer, MenuRowAlignCenter, false);
    prv_menu_layer_update_selection_highlight(state.menu_layer, state.up, false, false);
    state.menu_layer->inverter.layer.frame.size.h += abs_content_offset;
    state.menu_layer->inverter.layer.bounds.size = state.menu_layer->inverter.layer.frame.size;

    // when scrolling up, bounce back at the top (otherwise at the bottom)
    if (!state.up) {
      state.menu_layer->inverter.layer.frame.origin.y -= abs_content_offset;
    }
  }
  layer_mark_dirty(&state.menu_layer->scroll_layer.layer);
}

void prv_center_focus_animation_update_in_and_out(Animation *animation,
                                                  const AnimationProgress progress) {
  const AnimationProgress half_progress = ANIMATION_NORMALIZED_MAX / 2;
  const bool second_half = progress >= half_progress;
  const AnimationProgress adjusted_progress = second_half ?
      animation_timing_scaled(progress, half_progress, ANIMATION_NORMALIZED_MAX) :
      animation_timing_scaled(progress, 0, half_progress);
  prv_center_focus_animation_update_impl(animation, second_half, adjusted_progress);
}

void prv_center_focus_animation_update_out_only(Animation *animation,
                                                const AnimationProgress progress) {
  // anyways only render the bounce back
  prv_center_focus_animation_update_impl(animation, true, progress);
}

static void prv_center_focus_animation_teardown(Animation *animation) {
  // usually a "redundant" call. Just in case the animation gets cancelled before finish
  prv_center_focus_animation_update_in_and_out(animation, ANIMATION_NORMALIZED_MAX);
}

static void prv_schedule_center_focus_animation(MenuLayer *menu_layer, bool up,
                                                const MenuCellSpan *prev_selection,
                                                bool was_animating) {
  // we reconfigure the current index to be the previous index so that all parties in the ongoing
  // animation will continue to reply with the proper values with respect to the selection
  // half-way through the animation we then switch (back) to the new index
  menu_layer->animation.new_selection = menu_layer->selection;
  menu_layer->selection = *prev_selection;

  // force selection + scrolling to be at the right spot, not animated since the actual animation
  // for center_focused is done via rendering offset below
  const bool selection_animated = false;
  prv_menu_layer_update_selection_highlight(menu_layer, up, selection_animated,
                                            true /* change_ongoing_animation */);
  prv_menu_layer_update_selection_scroll_position(menu_layer, MenuRowAlignNone, selection_animated);

  static const PropertyAnimationImplementation s_center_focus_selection_animation_in_out_impl = {
    .base = {
      .setup = prv_center_focus_animation_setup,
      .update = prv_center_focus_animation_update_in_and_out,
      .teardown = prv_center_focus_animation_teardown,
    }
  };
  static const PropertyAnimationImplementation s_center_focus_selection_animation_out_only_impl = {
    .base = {
      .setup = prv_center_focus_animation_setup,
      .update = prv_center_focus_animation_update_out_only,
      .teardown = prv_center_focus_animation_teardown,
    }
  };
  // when we were animating already, use the implementation that's only showing the bounce back
  const PropertyAnimationImplementation *impl = was_animating ?
                                                &s_center_focus_selection_animation_out_only_impl :
                                                &s_center_focus_selection_animation_in_out_impl;
  PropertyAnimation *const prop_anim = property_animation_create(impl, menu_layer, NULL, NULL);
  // we're (ab)using the .to value to store the direction, see prv_center_focus_animation_state()
  property_animation_to(prop_anim, &up, sizeof(up), true);
  Animation *const anim = property_animation_get_animation(prop_anim);
  menu_layer->animation.animation = anim;

  // number of frames measured in the video
  const uint32_t full_duration_ms = ANIMATION_TARGET_FRAME_INTERVAL_MS * 7;
  uint32_t duration = full_duration_ms;
  if (was_animating) {
    // only show second half of animation if uses presses repetitive
    // as it's only the bounce back, then
    duration /= 2;
    animation_set_delay(anim, duration);
  }
  animation_set_duration(anim, duration);
  animation_set_curve(anim, AnimationCurveEaseInOut);
  animation_schedule(anim);

  if (was_animating) {
    // create visual state that's already reflecting the beginning of the "out" animation
    prv_center_focus_animation_update_out_only(anim, 0);
  }
}

static void prv_apply_selection_change(MenuLayer *menu_layer, MenuRowAlign scroll_align, bool up,
                                       bool did_change, const MenuCellSpan *prev_selection,
                                       bool was_animating, bool animated) {
  if (menu_layer->center_focused && animated) {
    prv_schedule_center_focus_animation(menu_layer, up, prev_selection, was_animating);
  } else {
    prv_menu_layer_update_selection_highlight(menu_layer, up, animated, true);
    prv_menu_layer_update_selection_scroll_position(menu_layer, scroll_align, animated);

    // only call this here, on animated center focus, the announcement will happen in-between
    // as we change the selection index for real
    if (did_change) {
      prv_announce_selection_changed(menu_layer, prev_selection->index);
    }
  }
}

typedef struct {
  bool was_animating;
  MenuCellSpan prev_selection;
} MenuLayerBeforeSelectionChangeState;

static MenuLayerBeforeSelectionChangeState prv_capture_state_and_cancel_center_focus_animation(
    MenuLayer *menu_layer) {
  // it's critical to cancel the animation for center focus here so that any potential in-between
  // selection state will be cleaned up
  const bool was_animating = menu_layer->center_focused ?
                             prv_cancel_selection_animation(menu_layer) :
                             false;
  return (MenuLayerBeforeSelectionChangeState) {
    .was_animating = was_animating,
    .prev_selection = menu_layer->selection,
  };
}

void menu_layer_set_selected_index(MenuLayer *menu_layer, MenuIndex index, MenuRowAlign scroll_align, bool animated) {
  const MenuLayerBeforeSelectionChangeState before_state =
      prv_capture_state_and_cancel_center_focus_animation(menu_layer);

  // Keep the selection within a valid range
  const uint16_t num_sections = prv_menu_layer_get_num_sections(menu_layer);
  if (index.section >= num_sections) {
    index.section = num_sections - 1;
  }
  // check to make sure this callback has been set, return early if not
  if (menu_layer->callbacks.get_num_rows == NULL) {
    PBL_LOG(LOG_LEVEL_ERROR, "Please set menu layer callbacks before running menu_layer_set_selected_index.");
    return;
  }

  const uint16_t num_rows = menu_layer->callbacks.get_num_rows(menu_layer, index.section, menu_layer->callback_context);
  if (index.row >= num_rows) {
    index.row = num_rows - 1;
  }

  // when called from iteration triggered by menu_layer_set_selected_next() the
  // selection.index.section could be MENU_INDEX_NOT_FOUND (a very large value)
  // in this case, walk forward from {0, 0| to avoid a very long loop run
  const bool is_invalid_section = menu_layer->selection.index.section == MENU_INDEX_NOT_FOUND;
  const int16_t comp = is_invalid_section ? 1 :
                       menu_index_compare(&index, &menu_layer->selection.index);
  MenuSelectIndexIterator it = {
    .it = {
      .menu_layer = menu_layer,
      .row_callback_after_geometry = prv_menu_layer_iterator_selection_index_callback,
      .section_callback = prv_menu_layer_iterator_noop_callback,
      .should_continue = true,
      .cursor = is_invalid_section ? (MenuCellSpan){} : menu_layer->selection,
    },
    .selection = {
      .index = index,
    },
    .did_change_selection = false,
  };

  prv_walk_with_iterator((int8_t)comp, &it.it);

  const bool up = (comp == -1);
  prv_apply_selection_change(menu_layer, scroll_align, up, it.did_change_selection,
                             &before_state.prev_selection, before_state.was_animating, animated);
}

typedef struct MenuSelectNextIterator {
  MenuIterator it;
  uint8_t count;
  bool did_change_selection:1;
} MenuSelectNextIterator;

static void prv_menu_layer_iterator_selection_next_callback(MenuIterator *iterator) {
  MenuSelectNextIterator *it = (MenuSelectNextIterator*)iterator;
  MenuLayer *menu_layer = it->it.menu_layer;
  if (it->count == 1) {
    MenuLayerSelectionWillChangeCallback cb = menu_layer->callbacks.selection_will_change;
    it->it.should_continue = false;
    it->did_change_selection = true;
    if (cb) {
      MenuIndex new_index = it->it.cursor.index;
      cb(menu_layer, &new_index, menu_layer->selection.index, menu_layer->callback_context);
      if (menu_index_compare(&new_index, &menu_layer->selection.index) == 0) {
        // locked into old index
      } else if (menu_index_compare(&new_index, &it->it.cursor.index) == 0) {
        // new index is the index we wanted to select
        menu_layer->selection = it->it.cursor;
      } else {
        // when center focused, animation will be scheduled at the very end
        // see prv_apply_selection_change()
        const bool animated = !menu_layer->center_focused;
        // Specified an alternate index
        // This is safe since menu_layer_set_selected_index will not trigger the
        // SelectionWillChangeCallback again.
        menu_layer_set_selected_index(menu_layer, new_index, MenuRowAlignNone, animated);
        it->did_change_selection = false;
      }
    } else {
      menu_layer->selection = it->it.cursor;
    }
  } else {
    ++it->count;
  }
}

void menu_layer_set_selected_next(MenuLayer *menu_layer, bool up,
                                  MenuRowAlign scroll_align, bool animated) {
  const MenuLayerBeforeSelectionChangeState before_state =
      prv_capture_state_and_cancel_center_focus_animation(menu_layer);

  MenuSelectNextIterator it = {
    .it = {
      .menu_layer = menu_layer,
      .row_callback_after_geometry = prv_menu_layer_iterator_selection_next_callback,
      .section_callback = prv_menu_layer_iterator_noop_callback,
      .should_continue = true,
      .cursor = menu_layer->selection,
    },
    .count = up ? 1 : 0, // see asymmetry note with menu_layer_walk_downward_from_iterator()
    .did_change_selection = false,
  };

  prv_walk_with_iterator((int8_t)(up ? -1 : 1), &it.it);

  prv_apply_selection_change(menu_layer, scroll_align, up, it.did_change_selection,
                             &before_state.prev_selection, before_state.was_animating, animated);
}

MenuIndex menu_layer_get_selected_index(const MenuLayer *menu_layer) {
  return menu_layer->selection.index;
}

bool menu_layer_is_index_selected(const MenuLayer *menu_layer, MenuIndex *index) {
  MenuIndex selected_index = menu_layer_get_selected_index(menu_layer);
  return menu_index_compare(&selected_index, index) == 0;
}

//! indicates that the data behind the menu has changed and needs a re-draw
void menu_layer_reload_data(MenuLayer *menu_layer) {
  menu_layer_update_caches(menu_layer);
}

bool menu_cell_layer_is_highlighted(const Layer *cell_layer) {
  return cell_layer->is_highlighted;
}

void menu_layer_set_normal_colors(MenuLayer *menu_layer, GColor background, GColor foreground) {
  menu_layer->normal_colors[MenuLayerColorBackground] = background;
  menu_layer->normal_colors[MenuLayerColorForeground] = foreground;
}

void menu_layer_set_highlight_colors(MenuLayer *menu_layer, GColor background, GColor foreground) {
  menu_layer->highlight_colors[MenuLayerColorBackground] = background;
  menu_layer->highlight_colors[MenuLayerColorForeground] = foreground;
}

bool menu_layer_get_center_focused(MenuLayer *menu_layer) {
  return menu_layer->center_focused;
}

void menu_layer_set_center_focused(MenuLayer *menu_layer, bool center_focused) {
  if (!menu_layer) {
    return;
  }
  prv_set_center_focused(menu_layer, center_focused);
  menu_layer_update_caches(menu_layer);
}
