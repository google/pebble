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

#include "selection_layer.h"

#include "applib/applib_malloc.auto.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/text.h"
#include "applib/ui/window_private.h"
#include "process_management/process_manager.h"
#include "shell/system_theme.h"
#include "system/logging.h"

// Look and feel
#define DEFAULT_CELL_PADDING 10
#define DEFAULT_SELECTED_INDEX 0
#define DEFAULT_ACTIVE_COLOR GColorWhite
#define DEFAULT_INACTIVE_COLOR GColorDarkGray

#define BUTTON_HOLD_REPEAT_MS 100

// Animation - I was told the video that was provides was at 28fps. This means each frame is 35.7ms
// 3 frames in the video
#define BUMP_TEXT_DURATION_MS 107
// 6 frames in the video
#define BUMP_SETTLE_DURATION_MS 214

// In the video this is 3, but I don't think that's enough (also even numbers work better)
#define SETTLE_HEIGHT_DIFF 6

// 3 frames in the video
#define SLIDE_DURATION_MS 107
// 5 frames in the video
#define SLIDE_SETTLE_DURATION_MS 179

typedef struct SelectionSizeConfig {
  const char *font_key;

  int default_cell_height;
} SelectionSizeConfig;

static const SelectionSizeConfig s_selection_config_medium = {
  .font_key = FONT_KEY_GOTHIC_28_BOLD,

  .default_cell_height = PBL_IF_RECT_ELSE(34, 40),
};

static const SelectionSizeConfig s_selection_config_large = {
  .font_key = FONT_KEY_GOTHIC_36_BOLD,

  .default_cell_height = 54,
};

static const SelectionSizeConfig *s_selection_configs[NumPreferredContentSizes] = {
  [PreferredContentSizeSmall] = &s_selection_config_medium,
  [PreferredContentSizeMedium] = &s_selection_config_medium,
  [PreferredContentSizeLarge] = &s_selection_config_large,
  [PreferredContentSizeExtraLarge] = &s_selection_config_large,
};

static const SelectionSizeConfig *prv_selection_config(void) {
  const PreferredContentSize runtime_platform_default_size =
      system_theme_get_default_content_size_for_runtime_platform();
  return s_selection_configs[runtime_platform_default_size];
}

int selection_layer_default_cell_height(void) {
  return prv_selection_config()->default_cell_height;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Drawing helpers

static int prv_get_pixels_for_bump_settle(int anim_percent_complete) {
  if (anim_percent_complete) {
    return SETTLE_HEIGHT_DIFF - ((SETTLE_HEIGHT_DIFF * anim_percent_complete) / 100);
  } else {
    return 0;
  }
}

static int prv_get_font_top_padding(GFont font) {
  if (font == fonts_get_system_font(FONT_KEY_GOTHIC_36_BOLD)) {
    return 14;
  } else if (font == fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD)) {
    return 10;
  } else if (font == fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)) {
    return 10;
  } else {
    return 0;
  }
}
// Assumes numbers / capital letters
static int prv_get_y_offset_which_vertically_centers_font(GFont font, int height) {
  int font_height = 0;
  int font_top_padding = prv_get_font_top_padding(font);
  if (font == fonts_get_system_font(FONT_KEY_GOTHIC_36_BOLD)) {
    font_height = 22;
  } else if (font == fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD)) {
    font_height = 18;
  } else if (font == fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD)) {
    font_height = 14;
  }

  return (height / 2) - (font_height / 2) - font_top_padding;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Drawing the layer

typedef struct {
  GCornerMask corner_mask;
  uint16_t corner_radius;
} CellCornerInfo;

static CellCornerInfo prv_get_cell_corner_info(int16_t cell_height) {
  return (CellCornerInfo) {
    .corner_mask = PBL_IF_RECT_ELSE(GCornerNone, GCornersAll),
    .corner_radius = PBL_IF_RECT_ELSE(1, (cell_height / 2) - 1),
  };
}

static int16_t prv_centered_offset_x(SelectionLayer *selection_layer) {
  uint16_t total_width = 0;
  for (uint32_t i = 0; i < selection_layer->num_cells; i++) {
    if (selection_layer->cell_widths[i] == 0) {
      continue;
    }
    total_width += selection_layer->cell_widths[i];
    if (i + 1 < selection_layer->num_cells) {
      total_width += selection_layer->cell_padding;
    }
  }
  return (selection_layer->layer.bounds.size.w - total_width) / 2;
}

static void prv_draw_cell_backgrounds(SelectionLayer *selection_layer, GContext *ctx) {
  int16_t start_x_offset = prv_centered_offset_x(selection_layer);
  // Loop over each cell and draw the background rectangles
  for (unsigned i = 0, current_x_offset = start_x_offset; i < selection_layer->num_cells; i++) {
    if (selection_layer->cell_widths[i] == 0) {
      continue;
    }

    // The y-offset for each cell defaults to 0 (the box is drawn from the top of the frame).
    // If we are currently doing the increment animation, the y-offset will be above the frame
    // (negative).
    int y_offset = 0;
    if (selection_layer->selected_cell_idx == i && selection_layer->bump_is_upwards) {
      y_offset = -prv_get_pixels_for_bump_settle(selection_layer->bump_settle_anim_progress);
    }

    // The height of each cell default to the height of the frame.
    int original_height = selection_layer->layer.frame.size.h;
    int adjusted_height = original_height;
    if (selection_layer->selected_cell_idx == i) {
      // If we are currently doing the increment animation then the height must be increased so
      // that the bottom of the cell stays fixed as the top jumps up.
      // If we are currently doing the decrement animation then the height must be increased so
      // that we draw below the cell
      adjusted_height += prv_get_pixels_for_bump_settle(selection_layer->bump_settle_anim_progress);
    }

    // No animations currently change the width of the cell. The slide animation is drawn later
    // over top of this.
    int width = selection_layer->cell_widths[i];

    // The cell rectangle has been constructed
    const GRect rect = GRect(current_x_offset, y_offset, width, adjusted_height);

    // Draw the cell as inactive by default
    GColor bg_color = selection_layer->inactive_background_color;

    // If the slide animation is in progress, then don't set the background color. The slide
    // will be drawn over top of this later
    if (selection_layer->selected_cell_idx == i && !selection_layer->slide_amin_progress) {
      bg_color = selection_layer->active_background_color;
    }
    graphics_context_set_fill_color(ctx, bg_color);
    // Use the original height to determine the cell's corner info
    const CellCornerInfo cell_corner_info = prv_get_cell_corner_info(original_height);
    graphics_fill_round_rect(ctx, &rect, cell_corner_info.corner_radius,
                             cell_corner_info.corner_mask);

    // Update the x-offset so we are ready for the next cell
    current_x_offset += selection_layer->cell_widths[i] + selection_layer->cell_padding;
  }
}

static void prv_draw_slider_slide(SelectionLayer *selection_layer, GContext *ctx) {
  // Find the active cell's x-offset
  int starting_x_offset = prv_centered_offset_x(selection_layer);
  for (unsigned i = 0; i < selection_layer->num_cells; i++) {
    if (selection_layer->selected_cell_idx == i) {
      break;
    }
    starting_x_offset += selection_layer->cell_widths[i] + selection_layer->cell_padding;
  }

  // The slider moves horizontally (to the right only) from 1 cell to another.
  // In total we need to slide from our current x offset, to the x offset of the next cell
  int next_cell_width = selection_layer->cell_widths[selection_layer->selected_cell_idx + 1];
  int slide_distance = next_cell_width + selection_layer->cell_padding;

  // The current distance we have moved depends on how far we are through the animation
  int current_slide_distance = (slide_distance * selection_layer->slide_amin_progress) / 100;

  // Finally our current x-offset is our starting offset plus our current distance
  int current_x_offset = starting_x_offset + current_slide_distance;


  // As the cell slides the width of the cell also changes...
  // It starts as the width of the current active cell.
  int cur_cell_width = selection_layer->cell_widths[selection_layer->selected_cell_idx];

  // It then morphs to the size of the next cell + padding causing the illusion that the selector
  // overshoot it's mark (it will settle back to the correct size in a different animation).
  // This means the width change is the width difference between the two cells plus padding
  int total_cell_width_change = next_cell_width - cur_cell_width + selection_layer->cell_padding;

  // The current width change depends on how far we are through the animation
  int current_cell_width_change =
      (total_cell_width_change * (int) selection_layer->slide_amin_progress) / 100;

  // And finally our current width is the starting width plus any change
  int current_cell_width = cur_cell_width + current_cell_width_change;

  const GRect rect =
      GRect(current_x_offset, 0, current_cell_width, selection_layer->layer.frame.size.h);
  graphics_context_set_fill_color(ctx, selection_layer->active_background_color);
  const CellCornerInfo cell_corner_info = prv_get_cell_corner_info(rect.size.h);
  graphics_fill_round_rect(ctx, &rect, cell_corner_info.corner_radius,
                           cell_corner_info.corner_mask);
}

static void prv_draw_slider_settle(SelectionLayer *selection_layer, GContext *ctx) {
  // Find the active cell's x-offset.
  int starting_x_offset = prv_centered_offset_x(selection_layer);
  for (unsigned i = 0; i < selection_layer->num_cells; i++) {
    if (selection_layer->selected_cell_idx == i) {
      break;
    }
    starting_x_offset += selection_layer->cell_widths[i] + selection_layer->cell_padding;
  }

  // After the slider is done sliding then active cell is updated and filled in with the
  // correct background color.
  // This animation is responsible for the settle effect. It removes the extra width
  // (padding width) which was drawn to create an overshoot effect.

  // We need to increase the cell's width by the receding padding
  int original_width = selection_layer->cell_widths[selection_layer->selected_cell_idx];
  int receding_padding =
    (selection_layer->cell_padding * selection_layer->slide_settle_anim_progress) / 100;
  int adjusted_width = original_width + receding_padding;

  const GRect rect =
      GRect(starting_x_offset, 0, adjusted_width, selection_layer->layer.frame.size.h);
  graphics_context_set_fill_color(ctx, selection_layer->active_background_color);
  const CellCornerInfo cell_corner_info = prv_get_cell_corner_info(rect.size.h);
  graphics_fill_round_rect(ctx, &rect, cell_corner_info.corner_radius,
                           cell_corner_info.corner_mask);
}

static void prv_draw_text(SelectionLayer *selection_layer, GContext *ctx) {
  int16_t start_x_offset = prv_centered_offset_x(selection_layer);
  // Loop over each cell and draw the text
  for (unsigned i = 0, current_x_offset = start_x_offset; i < selection_layer->num_cells; i++) {
    if (selection_layer->callbacks.get_cell_text) {
      // Potential optimization: cache the cell text somewhere as this function gets called
      // a lot (because of animations). The current users of this modules just call snprintf()
      // in the get_cell_text() function, so it isn't a big deal for now
      char *text = selection_layer->callbacks.get_cell_text(i, selection_layer->callback_context);
      if (text) {
        // We need to figure out the height of the box that the text is being drawn in so that
        // it can be vertically centered
        int height = selection_layer->layer.frame.size.h;
        if (selection_layer->selected_cell_idx == i) {
          // See prv_draw_cell_backgrounds() for reasoning
          height += prv_get_pixels_for_bump_settle(selection_layer->bump_settle_anim_progress);
        }
        // The text should be be vertically centered, unless we are performing an increment /
        // decrement animation.
        int y_offset =
            prv_get_y_offset_which_vertically_centers_font(selection_layer->font, height);

        // We might not be drawing from the top of the frame, compensate if needed
        if (selection_layer->selected_cell_idx == i && selection_layer->bump_is_upwards) {
          y_offset -= prv_get_pixels_for_bump_settle(selection_layer->bump_settle_anim_progress);
        }
        // If we are performing an increment or decrement animation then update the
        // y offset with our progress
        if (selection_layer->selected_cell_idx == i) {
          int delta = (selection_layer->bump_text_anim_progress *
              prv_get_font_top_padding(selection_layer->font)) / 100;
          if (selection_layer->bump_is_upwards) {
            delta *= -1;
          }
          y_offset += delta;
        }

        GRect rect = GRect(current_x_offset, y_offset, selection_layer->cell_widths[i], height);
        graphics_draw_text(ctx, text, selection_layer->font,
            rect, GTextOverflowModeFill, GTextAlignmentCenter, NULL);
      }
    }
    // Update the x-offset so we are ready for the next cell
    current_x_offset += selection_layer->cell_widths[i] + selection_layer->cell_padding;
  }
}

static void prv_draw_selection_layer(SelectionLayer *selection_layer, GContext *ctx) {
  // The first thing that is drawn is the background for each cell
  prv_draw_cell_backgrounds(selection_layer, ctx);

  // If the slider is in motion draw it. This is above the backgrounds, but below the text
  if (selection_layer->slide_amin_progress) {
    prv_draw_slider_slide(selection_layer, ctx);
  }
  if (selection_layer->slide_settle_anim_progress) {
    prv_draw_slider_settle(selection_layer, ctx);
  }

  // Finally the text is drawn over everything
  prv_draw_text(selection_layer, ctx);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Increment / Decrement Animation

//! This animation causes the active cell to "bump" when the user presses the up button.
//! This animation has two parts:
//! 1) The "text to cell edge"
//! 2) The "background settle"

//! The "text to cell edge" (bump_text) moves the text until it hits the top / bottom of the cell.

//! The "background settle" (bump_settle) is a reaction to the "text to cell edge" animation.
//! The top of the cell immediately expands down giving the impression that the text "pushed" the
//! cell making it bigger. The cell then shrinks / settles back to its original height
//! with the text vertically centered

static void prv_bump_text_impl(struct Animation *animation,
                               const AnimationProgress distance_normalized) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);

  // Update the completion percent of the animation
  selection_layer->bump_text_anim_progress = (100 * distance_normalized) / ANIMATION_NORMALIZED_MAX;
  layer_mark_dirty(&selection_layer->layer);
}

static void prv_update_cell_value(SelectionLayer *selection_layer, bool increment) {
  // The text value is updated halfway through the animation
  SelectionLayerIncrementCallback func = (selection_layer->bump_is_upwards)
                                             ? selection_layer->callbacks.increment
                                             : selection_layer->callbacks.decrement;
  if (func) {
    func(selection_layer->selected_cell_idx, selection_layer->callback_context);
  }
  layer_mark_dirty(&selection_layer->layer);
}

static void prv_bump_text_stopped(Animation *animation, bool finished, void *context) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);
  selection_layer->bump_text_anim_progress = 0;

  prv_update_cell_value(selection_layer, selection_layer->bump_is_upwards);
}

static void prv_bump_settle_impl(struct Animation *animation,
                                 const AnimationProgress distance_normalized) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);

  // Update the completion percent of the animation
  selection_layer->bump_settle_anim_progress =
      (100 * distance_normalized) / ANIMATION_NORMALIZED_MAX;
  layer_mark_dirty(&selection_layer->layer);
}

static void prv_bump_settle_stopped(Animation *animation, bool finished, void *context) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);
  selection_layer->bump_settle_anim_progress = 0;
}

static Animation* prv_create_bump_text_animation(SelectionLayer *selection_layer) {
  Animation *animation = animation_create();
  animation_set_curve(animation, AnimationCurveEaseIn);
  animation_set_duration(animation, BUMP_TEXT_DURATION_MS);
  AnimationHandlers anim_handler = {
    .stopped = prv_bump_text_stopped,
  };
  animation_set_handlers(animation, anim_handler, selection_layer);

  selection_layer->bump_text_impl = (AnimationImplementation) {
    .update = prv_bump_text_impl,
  };
  animation_set_implementation(animation, &selection_layer->bump_text_impl);

  return animation;
}

static Animation* prv_create_bump_settle_animation(SelectionLayer *selection_layer) {
  Animation *animation = animation_create();
  animation_set_curve(animation, AnimationCurveEaseOut);
  animation_set_duration(animation, BUMP_SETTLE_DURATION_MS);
  AnimationHandlers anim_handler = {
    .stopped = prv_bump_settle_stopped,
  };
  animation_set_handlers(animation, anim_handler, selection_layer);

  selection_layer->bump_settle_anim_impl = (AnimationImplementation) {
    .update = prv_bump_settle_impl,
  };
  animation_set_implementation(animation, &selection_layer->bump_settle_anim_impl);

  return animation;
}

static void prv_run_value_change_animation(SelectionLayer *selection_layer) {
#if !PLATFORM_TINTIN
  Animation *bump_text = prv_create_bump_text_animation(selection_layer);
  Animation *bump_settle = prv_create_bump_settle_animation(selection_layer);
  selection_layer->value_change_animation = animation_sequence_create(bump_text, bump_settle, NULL);
  animation_schedule(selection_layer->value_change_animation);
#else
  prv_update_cell_value(selection_layer, selection_layer->bump_is_upwards);
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Slide Animation

//! This animation moves the "selection box" (active color) to the next cell to the right.
//! This animation has two parts:
//! 1) The "move and expand"
//! 2) The "settle"

//! The "move and expand" (slide) moves the selection box from the currently active cell to
//! the next cell to the right. At the same time the width is changed to be the size of the
//! next cell plus the size of the padding. This creates an overshoot effect.

//! The "settle" (slide_settle) decreases the selection box's width back by the extra width that was
//! added in the "move and expand" step.

static void prv_slide_impl(struct Animation *animation,
                           const AnimationProgress distance_normalized) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);

  // Update the completion percent of the animation
  selection_layer->slide_amin_progress = (100 * distance_normalized) / ANIMATION_NORMALIZED_MAX;
  layer_mark_dirty(&selection_layer->layer);
}

static void prv_slide_stopped(Animation *animation, bool finished, void *context) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);
  selection_layer->slide_amin_progress = 0;
  selection_layer->selected_cell_idx++;
}

static void prv_slide_settle_impl(struct Animation *animation,
                                  const AnimationProgress distance_normalized) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);

  // Update the completion percent of the animation. This is a reverse animation. It starts
  // fully drawn, then the amount drawn decreases
  selection_layer->slide_settle_anim_progress =
      100 - (100 * distance_normalized) / ANIMATION_NORMALIZED_MAX;
  layer_mark_dirty(&selection_layer->layer);
}

static void prv_slide_settle_stopped(Animation *animation, bool finished, void *context) {
  SelectionLayer *selection_layer = (SelectionLayer*) animation_get_context(animation);
  selection_layer->slide_settle_anim_progress = 0;
}

static Animation* prv_create_slide_animation(SelectionLayer *selection_layer) {
  Animation *animation = animation_create();
  animation_set_curve(animation, AnimationCurveEaseIn);
  animation_set_duration(animation, SLIDE_DURATION_MS);
  AnimationHandlers anim_handler = {
    .stopped = prv_slide_stopped,
  };
  animation_set_handlers(animation, anim_handler, selection_layer);

  selection_layer->slide_amin_impl = (AnimationImplementation) {
    .update = prv_slide_impl,
  };
  animation_set_implementation(animation, &selection_layer->slide_amin_impl);

  return animation;
}

static Animation* prv_create_slide_settle_animation(SelectionLayer *selection_layer) {
  Animation *animation = animation_create();
  animation_set_curve(animation, AnimationCurveEaseOut);
  animation_set_duration(animation, SLIDE_SETTLE_DURATION_MS);
  AnimationHandlers anim_handler = {
    .stopped = prv_slide_settle_stopped,
  };
  animation_set_handlers(animation, anim_handler, selection_layer);

  selection_layer->slide_settle_anim_impl = (AnimationImplementation) {
    .update = prv_slide_settle_impl,
  };
  animation_set_implementation(animation, &selection_layer->slide_settle_anim_impl);

  return animation;
}

static void prv_run_slide_animation(SelectionLayer *selection_layer) {
#if !PLATFORM_TINTIN
  Animation *over_animation = prv_create_slide_animation(selection_layer);
  Animation *settle_animation = prv_create_slide_settle_animation(selection_layer);
  selection_layer->next_cell_animation =
      animation_sequence_create(over_animation, settle_animation, NULL);

  animation_schedule(selection_layer->next_cell_animation);
#else
  selection_layer->selected_cell_idx++;
  layer_mark_dirty(&selection_layer->layer);
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Click handlers
void prv_up_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  SelectionLayer *selection_layer = (SelectionLayer*) context;
  bool is_up = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);

  if (selection_layer->is_active) {
    if (click_recognizer_is_repeating(recognizer)) {
      // Don't animate if the button is being held down. Just update the text
      SelectionLayerIncrementCallback func = (is_up)
                                              ? selection_layer->callbacks.increment
                                              : selection_layer->callbacks.decrement;
      if (func) {
        func(selection_layer->selected_cell_idx, selection_layer->callback_context);
      }
      layer_mark_dirty(&selection_layer->layer);
    } else {
      // Run the animation. The decrement callback will be run halfway through
      selection_layer->bump_is_upwards = is_up;
      prv_run_value_change_animation(selection_layer);
    }
  }
}

void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  SelectionLayer *selection_layer = (SelectionLayer*) context;
  if (selection_layer->is_active) {
    animation_unschedule(selection_layer->next_cell_animation);
    if (selection_layer->selected_cell_idx == selection_layer->num_cells - 1) {
      selection_layer->selected_cell_idx = 0;
      selection_layer->callbacks.complete(selection_layer->callback_context);
    } else {
      prv_run_slide_animation(selection_layer);
    }
  }
}

static void prv_click_config_provider(SelectionLayer *selection_layer) {
  // Config UP / DOWN button behavior:
  window_set_click_context(BUTTON_ID_UP, selection_layer);
  window_set_click_context(BUTTON_ID_DOWN, selection_layer);
  window_set_click_context(BUTTON_ID_SELECT, selection_layer);

  window_single_repeating_click_subscribe(BUTTON_ID_UP,
                                          BUTTON_HOLD_REPEAT_MS,
                                          prv_up_down_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN,
                                          BUTTON_HOLD_REPEAT_MS,
                                          prv_up_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//! API
void selection_layer_init(SelectionLayer *selection_layer, const GRect *frame,
                          unsigned num_cells) {
  if (num_cells > MAX_SELECTION_LAYER_CELLS) {
    num_cells = MAX_SELECTION_LAYER_CELLS;
  }

  // Set layer defaults
  *selection_layer = (SelectionLayer) {
    .num_cells = num_cells,
    .cell_padding = DEFAULT_CELL_PADDING,
    .selected_cell_idx = DEFAULT_SELECTED_INDEX,
    .font = fonts_get_system_font(prv_selection_config()->font_key),
    .active_background_color = DEFAULT_ACTIVE_COLOR,
    .inactive_background_color = DEFAULT_INACTIVE_COLOR,
    .is_active = true,
  };
  for (unsigned i = 0; i < num_cells; i++) {
    selection_layer->cell_widths[i] = 0;
  }

  layer_create_with_data(*frame, sizeof(selection_layer));
  layer_set_frame(&selection_layer->layer, frame);
  layer_set_clips(&selection_layer->layer, false);
  layer_set_update_proc(&selection_layer->layer, (LayerUpdateProc) prv_draw_selection_layer);
}

SelectionLayer* selection_layer_create(GRect frame, unsigned num_cells) {
  // Note: Not yet exported to 3rd party apps so no padding needed
  SelectionLayer *selection_layer = applib_malloc(sizeof(SelectionLayer));
  if (selection_layer) {
    selection_layer_init(selection_layer, &frame, num_cells);
  }
  return selection_layer;
}

void selection_layer_deinit(SelectionLayer* selection_layer) {
  animation_unschedule(selection_layer->next_cell_animation);
  animation_unschedule(selection_layer->value_change_animation);
}

void selection_layer_destroy(SelectionLayer* selection_layer) {
  if (selection_layer) {
    selection_layer_deinit(selection_layer);
    applib_free(selection_layer);
  }
}

void selection_layer_set_cell_width(SelectionLayer *selection_layer, unsigned idx, unsigned width) {
  if (selection_layer && idx < selection_layer->num_cells) {
    selection_layer->cell_widths[idx] = width;
  }
}

void selection_layer_set_font(SelectionLayer *selection_layer, GFont font) {
  if (selection_layer) {
    selection_layer->font = font;
  }
}

void selection_layer_set_inactive_bg_color(SelectionLayer *selection_layer, GColor color) {
  if (selection_layer) {
    selection_layer->inactive_background_color = color;
  }
}

void selection_layer_set_active_bg_color(SelectionLayer *selection_layer, GColor color) {
  if (selection_layer) {
    selection_layer->active_background_color = color;
  }
}

void selection_layer_set_cell_padding(SelectionLayer *selection_layer, unsigned padding) {
  if (selection_layer) {
    selection_layer->cell_padding = padding;
  }
}

void selection_layer_set_active(SelectionLayer *selection_layer, bool is_active) {
  if (selection_layer) {
    if (is_active && !selection_layer->is_active) {
      selection_layer->selected_cell_idx = 0;
    } if (!is_active && selection_layer->is_active) {
      selection_layer->selected_cell_idx = MAX_SELECTION_LAYER_CELLS + 1;
    }
    selection_layer->is_active = is_active;
    layer_mark_dirty(&selection_layer->layer);
  }
}

void selection_layer_set_click_config_onto_window(SelectionLayer *selection_layer,
                                                  struct Window *window) {
  if (selection_layer && window) {
    window_set_click_config_provider_with_context(window,
        (ClickConfigProvider) prv_click_config_provider, selection_layer);
  }
}

void selection_layer_set_callbacks(SelectionLayer *selection_layer, void *callback_context,
                                   SelectionLayerCallbacks callbacks) {
  selection_layer->callbacks = callbacks;
  selection_layer->callback_context = callback_context;
}
