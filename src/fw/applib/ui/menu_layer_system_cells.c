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

#include "applib/graphics/graphics.h"
#include "applib/ui/kino/kino_reel.h"
#include "applib/ui/kino/kino_reel_gbitmap_private.h"
#include "process_management/process_manager.h"
#include "shell/system_theme.h"
#include "system/passert.h"
#include "util/math.h"

/////////////////////////////////
// System Provided Cell Types
//
// NOTES: Below are the implementations of system provided cell drawing functions.
//

//////////////////////
// Basic menu cell

typedef struct MenuCellDimensions {
  int16_t basic_cell_height;
  int16_t small_cell_height;
  int16_t horizontal_inset;
  int16_t title_subtitle_left_margin;
} MenuCellDimensions;

static const MenuCellDimensions s_menu_cell_dimensions[NumPreferredContentSizes] = {
  //! @note these are the same as Medium until Small is designed
  [PreferredContentSizeSmall] = {
    .basic_cell_height = 44,
    .small_cell_height = 34,
    .horizontal_inset = 5,
    .title_subtitle_left_margin = 30,
  },
  [PreferredContentSizeMedium] = {
    .basic_cell_height = 44,
    .small_cell_height = 34,
    .horizontal_inset = 5,
    .title_subtitle_left_margin = 30,
  },
  [PreferredContentSizeLarge] = {
    .basic_cell_height = 61,
    .small_cell_height = 42,
    .horizontal_inset = 10,
    .title_subtitle_left_margin = 34,
  },
  //! @note these are the same as Large until ExtraLarge is designed
  [PreferredContentSizeExtraLarge] = {
    .basic_cell_height = 61,
    .small_cell_height = 42,
    .horizontal_inset = 10,
    .title_subtitle_left_margin = 34,
  },
};

static const MenuCellDimensions *prv_get_dimensions_for_runtime_platform_default_size(void) {
  const PreferredContentSize runtime_platform_default_size =
      system_theme_get_default_content_size_for_runtime_platform();
  return &s_menu_cell_dimensions[runtime_platform_default_size];
}

int16_t menu_cell_basic_cell_height(void) {
  return prv_get_dimensions_for_runtime_platform_default_size()->basic_cell_height;
}

int16_t menu_cell_small_cell_height(void) {
  return prv_get_dimensions_for_runtime_platform_default_size()->small_cell_height;
}

int16_t menu_cell_basic_horizontal_inset(void) {
  return prv_get_dimensions_for_runtime_platform_default_size()->horizontal_inset;
}

static int16_t prv_title_subtitle_left_margin(void) {
  return prv_get_dimensions_for_runtime_platform_default_size()->title_subtitle_left_margin;
}

static ALWAYS_INLINE GFont prv_get_cell_title_font(const MenuCellLayerConfig *config) {
  return config->title_font ?: system_theme_get_font_for_default_size(TextStyleFont_MenuCellTitle);
}

static ALWAYS_INLINE GFont prv_get_cell_subtitle_font(const MenuCellLayerConfig *config) {
  return config->subtitle_font ?:
      system_theme_get_font_for_default_size(TextStyleFont_MenuCellSubtitle);
}

static ALWAYS_INLINE GFont prv_get_cell_value_font(const MenuCellLayerConfig *config) {
  return config->value_font ?: prv_get_cell_title_font(config);
}

static ALWAYS_INLINE void prv_draw_icon(GContext *ctx, GBitmap *icon, const GRect *icon_frame,
                                        bool is_legacy2) {
  if (!is_legacy2) {
    bool tint_icon = (icon && (gbitmap_get_format(icon) == GBitmapFormat1Bit));
    if (tint_icon) {
      graphics_context_set_compositing_mode(ctx, GCompOpTint);
    } else if (ctx->draw_state.compositing_mode == GCompOpAssign) {
      graphics_context_set_compositing_mode(ctx, GCompOpSet);
    }
  }

  graphics_draw_bitmap_in_rect(ctx, icon, icon_frame);
}

static void prv_menu_cell_basic_draw_custom_rect(
    GContext *ctx, const Layer *cell_layer, const MenuCellLayerConfig *config) {
  const GRect *bounds = &cell_layer->bounds;
  const bool is_legacy2 = process_manager_compiled_with_legacy2_sdk();

  const GFont title_font = prv_get_cell_title_font(config);
  const int16_t title_height = fonts_get_font_height(title_font);
  const GFont subtitle_font = prv_get_cell_subtitle_font(config);
  const int16_t subtitle_height = config->subtitle ? fonts_get_font_height(subtitle_font) : 0;
  const int16_t full_height = title_height + subtitle_height + 10;
  const int horizontal_margin = menu_cell_basic_horizontal_inset();
  const int vertical_margin = (bounds->size.h - full_height) / 2;
  int left_margin = 0;

  GRect box;
  const GAlign icon_align = (GAlign)config->icon_align;
  if (config->icon) {
    box = (GRect) {
      .size = config->icon->bounds.size,
      .origin = bounds->origin,
    };

    if (is_legacy2) {
      static const GSize s_icon_size = { 33, 44 };
      if (icon_align == GAlignRight) {
        box.origin.x += bounds->size.w - (horizontal_margin + config->icon->bounds.size.w);
      } else { // icon on left
        box.origin.x +=  ((config->icon->bounds.size.w & 1) + // Nudge odd-width icons one right
                          ((s_icon_size.w - config->icon->bounds.size.w) / 2));
      }
      box.origin.y += (s_icon_size.h - config->icon->bounds.size.h) / 2;
    } else {
      const GRect container_rect =
          grect_inset(*bounds, GEdgeInsets(vertical_margin, horizontal_margin));
      grect_align(&box, &container_rect, icon_align, true /* clip */);
      if ((icon_align == GAlignTopLeft) || (icon_align == GAlignTop) ||
          (icon_align == GAlignTopRight)) {
        // Offset by the cap offset to match round's icon-title delta
        box.origin.y += fonts_get_font_cap_offset(title_font);
      }
    }

    if (config->icon_box_model) {
      box.origin = gpoint_add(box.origin, config->icon_box_model->offset);
    }

    left_margin = box.origin.x;
    prv_draw_icon(ctx, config->icon, &box, is_legacy2);
  }

  box = *bounds;
  if (icon_align == GAlignRight) {
    left_margin = horizontal_margin;
    box.size.w -= config->icon->bounds.size.w;
  } else {
    left_margin = !config->icon ? horizontal_margin :
        config->icon_form_fit ? (left_margin + config->icon->bounds.size.w +
                                 (config->icon_box_model ? config->icon_box_model->margin.w : 0))
                              : prv_title_subtitle_left_margin() + horizontal_margin;
  }
  box.origin.x += left_margin;
  box.size.w -= left_margin;

  GRect value_box = box;
  if (config->overflow_mode != GTextOverflowModeWordWrap) {
    box.origin.y += vertical_margin;
    box.size.h = title_height + 4;
    value_box.origin.y = box.origin.y;
  } else {
    // Value box is centered when drawing with GTextOverflowModeWordWrap.
    value_box.origin.y = MIN(box.size.h - full_height, title_height) / 2;
  }

  if (is_legacy2) {
    // Update the text color to Black for legacy apps - this is to maintain existing behavior for
    // 2.x compiled apps - no need to restore original since original 2.x did not do any restore
    ctx->draw_state.text_color = GColorBlack;
  }

  if (config->value && (icon_align != GAlignRight)) {
    value_box.size.w -= horizontal_margin;

    const GFont value_font = prv_get_cell_value_font(config);
    const GSize text_size = graphics_text_layout_get_max_used_size(
        ctx, config->value, value_font, value_box, config->overflow_mode,
        GTextAlignmentRight, NULL);
    box.size.w -= (text_size.w + horizontal_margin * 2);
    graphics_draw_text(ctx, config->value, value_font, value_box,
                       config->overflow_mode, GTextAlignmentRight, NULL);
  }

  if (config->title) {
    graphics_draw_text(ctx, config->title, title_font, box,
                       config->overflow_mode, GTextAlignmentLeft, NULL);
  }

  if (config->subtitle) {
    box.origin.y += title_height;
    box.size.h = subtitle_height + 4;
    graphics_draw_text(ctx, config->subtitle, subtitle_font, box,
                       config->overflow_mode, GTextAlignmentLeft, NULL);
  }
}

// This function duplicates `grect_inset()` but helps us save some stack space by using pointer
// arguments and always inlining the function
static ALWAYS_INLINE void prv_grect_inset(GRect *rect, GEdgeInsets *insets) {
  grect_standardize(rect);
  const int16_t new_width = rect->size.w - insets->left - insets->right;
  const int16_t new_height = rect->size.h - insets->top - insets->bottom;
  if (new_width < 0 || new_height < 0) {
    *rect = GRectZero;
  } else {
    *rect = GRect(rect->origin.x + insets->left, rect->origin.y + insets->top,
                  new_width, new_height);
  }
}

static ALWAYS_INLINE bool prv_should_render_subtitle_round(const MenuCellLayerConfig *config,
                                                           bool is_selected) {
  // If the cell isn't selected and there's no value text, then no subtitle text should be shown
  return ((is_selected || config->value) && (config->subtitle != NULL));
}

static ALWAYS_INLINE GRect prv_menu_cell_basic_draw_custom_one_column_round(
  GContext *ctx, const GRect *cell_layer_bounds, const MenuCellLayerConfig *config,
  GTextAlignment text_alignment, GAlign container_alignment, bool is_selected) {
  if (!cell_layer_bounds) {
    return GRectZero;
  }

  const GFont title_font = prv_get_cell_title_font(config);
  const uint8_t title_font_height = fonts_get_font_height(title_font);
  GSize cell_layer_bounds_size = cell_layer_bounds->size;

  // Bail out if we can't even fit a single line of the title
  if (title_font_height > cell_layer_bounds_size.h) {
    return GRectZero;
  }

  // Initialize our subtitle text height and icon frame size to 0 since we don't know yet if we will
  // render them
  int16_t subtitle_text_frame_height = 0;
  GSize icon_frame_size = GSizeZero;

  const GFont subtitle_font = prv_get_cell_subtitle_font(config);
  const bool render_subtitle = prv_should_render_subtitle_round(config, is_selected);
  if (render_subtitle) {
    subtitle_text_frame_height = fonts_get_font_height(subtitle_font);
  }
  const int subtitle_text_cap_offset =
      (config->subtitle) ? fonts_get_font_cap_offset(subtitle_font) : 0;

  const GAlign icon_align = (GAlign)config->icon_align;
  const bool render_icon = ((config->icon != NULL) && (icon_align != GAlignRight));
  const GSize icon_bitmap_size = config->icon ? config->icon->bounds.size : GSizeZero;
  if (render_icon) {
    icon_frame_size = icon_bitmap_size;
    if (config->icon_box_model) {
      icon_frame_size = gsize_add(icon_frame_size, config->icon_box_model->margin);
    }
  }

  const bool can_use_two_lines_for_title = !(render_subtitle || render_icon);
  const bool can_use_many_lines_for_title = (config->overflow_mode == GTextOverflowModeWordWrap);
  const int16_t initial_title_text_lines = can_use_two_lines_for_title ? 2 : 1;
  int16_t title_text_frame_height = can_use_many_lines_for_title
      ? graphics_text_layout_get_text_height(ctx, config->title, title_font,
                                             cell_layer_bounds_size.w, config->overflow_mode,
                                             text_alignment)
      : title_font_height * initial_title_text_lines;
  const int title_text_cap_offset = (config->title) ? fonts_get_font_cap_offset(title_font) : 0;

  int16_t container_height = title_text_frame_height + subtitle_text_frame_height;
  if (icon_align == GAlignTop) {
    // The icon is rendered above the others, add to container height
    container_height += icon_frame_size.h;
  } else if (icon_frame_size.h > cell_layer_bounds_size.h) {
    // The icon is rendered beside but does not fit, cut it out
    icon_frame_size = GSizeZero;
  }
  if (container_height > cell_layer_bounds_size.h) {
    // If we couldn't fit one line of title, subtitle, and icon, try cutting out the icon
    if (icon_align == GAlignTop) {
      container_height -= icon_frame_size.h;
    }
    if (container_height > cell_layer_bounds_size.h) {
      // If we couldn't fit one title line and the subtitle, try cutting out the subtitle instead
      container_height = title_text_frame_height + icon_frame_size.h;
      if (container_height > cell_layer_bounds_size.h) {
        // If we couldn't fit just the title and icon, try just two lines for the title
        container_height = title_font_height * 2;
        if (container_height > cell_layer_bounds_size.h) {
          // If we couldn't fit two title lines, just use one title line
          title_text_frame_height = title_font_height;
        } else {
          title_text_frame_height = container_height;
        }
        subtitle_text_frame_height = 0;
        if (icon_align == GAlignTop) {
          icon_frame_size = GSizeZero;
        }
      } else {
        subtitle_text_frame_height = 0;
      }
    } else {
      icon_frame_size = GSizeZero;
    }
  }

  // We'll reuse this rect to conserve stack space; here it is used as the title text frame
  GRect rect = (GRect) {
    .origin = cell_layer_bounds->origin,
    .size = GSize(cell_layer_bounds_size.w, title_text_frame_height),
  };

  // Update the title text frame's height using the max used size of the title text because we
  // might only have one line of text to render even though we have space for two lines
  title_text_frame_height = graphics_text_layout_get_max_used_size(
      ctx, config->title, title_font, rect, config->overflow_mode, text_alignment, NULL).h;
  // Calculate the final container height and create a rectangle for it
  container_height = title_text_frame_height + subtitle_text_frame_height;
  const bool icon_on_left = ((icon_align == GAlignLeft) || (icon_align == GAlignTopLeft));
  if (icon_align == GAlignTop) {
    // The icon is on its own line at the top, extend accordingly
    container_height += icon_frame_size.h;
  } else if (render_icon && (icon_align == GAlignLeft)) {
    // Let the icon extend the container height if it's taller than the title/subtitle combo
    container_height = MAX(icon_frame_size.h, container_height);
  }
  GRect container_rect = (GRect) { .size = GSize(cell_layer_bounds_size.w, container_height) };

  // Align the container rect in the cell
  grect_align(&container_rect, cell_layer_bounds, container_alignment, true /* clip */);

  // Here we reuse rect as the icon frame
  rect.size = icon_frame_size;
  // Align the icon frame (which might have zero height) at the top center of the container
  grect_align(&rect, &container_rect, icon_align, true /* clip */);

  // Save the title origin y before re-purposing container_rect
  int title_text_frame_origin_y = container_rect.origin.y;

  // Draw the icon (if one was provided and we have room for it)
  if (render_icon && !gsize_equal(&rect.size, &GSizeZero)) {
    // Only draw the icon if it fits within the cell
    if (gsize_equal(&rect.size, &icon_frame_size)) {
      // round has never worked with legacy2 apps
      static const bool is_legacy2 = false;
      // Reuse container_rect as icon frame
      container_rect = (GRect) {
        .origin = rect.origin,
        .size = icon_bitmap_size,
      };
      if (config->icon_box_model) {
        container_rect.origin = gpoint_add(container_rect.origin, config->icon_box_model->offset);
      }
      prv_draw_icon(ctx, config->icon, &container_rect, is_legacy2);
    }
  }

  int cell_layer_bounds_origin_x = cell_layer_bounds->origin.x;
  // Move the title and subtitle closer together to match designs
  const int16_t icon_on_left_title_subtitle_vertical_spacing_offset = -3;
  if (icon_align == GAlignTop) {
    // Set the title text's frame origin at the bottom of the icon's frame
    title_text_frame_origin_y = grect_get_max_y(&rect);
  } else if (icon_on_left) {
    // Move content to the right for the icon on the left
    cell_layer_bounds_origin_x = grect_get_max_x(&rect);
    cell_layer_bounds_size.w -= cell_layer_bounds_origin_x - cell_layer_bounds->origin.x;

    if (icon_align == GAlignLeft) {
      // Vertically center the title and subtitle within the container
      title_text_frame_origin_y =
          cell_layer_bounds->origin.y +
              ((cell_layer_bounds->size.h - title_text_frame_height -
                  subtitle_text_frame_height - 1) / 2);
      if (subtitle_text_frame_height) {
        title_text_frame_origin_y -= icon_on_left_title_subtitle_vertical_spacing_offset;
      }
    }
  }

  // Draw the subtitle (if one was provided and we have room), taking into account the cap offset
  if (render_subtitle && (subtitle_text_frame_height != 0)) {
    int16_t subtitle_text_frame_origin_y =
        (int16_t)(title_text_frame_origin_y + title_text_frame_height - subtitle_text_cap_offset);
    if (icon_align == GAlignLeft) {
      subtitle_text_frame_origin_y += icon_on_left_title_subtitle_vertical_spacing_offset;
    }
    // Reuse rect as the subtitle text frame
    rect = (GRect) {
      .origin = GPoint(cell_layer_bounds_origin_x, subtitle_text_frame_origin_y),
      .size = GSize(cell_layer_bounds_size.w, subtitle_text_frame_height)
    };
    graphics_draw_text(ctx, config->subtitle, subtitle_font, rect, config->overflow_mode,
                       text_alignment, NULL);
  }

  // Draw the title, which we're guaranteed to have room for because otherwise we would have bailed
  // out at the beginning of this function
  // Reuse rect as the title text frame
  rect = (GRect) {
    .origin = GPoint(cell_layer_bounds_origin_x, title_text_frame_origin_y),
    .size = GSize(cell_layer_bounds_size.w, title_text_frame_height)
  };
  // Accumulate the cap offsets we need to position the title properly
  int cap_offsets_to_apply = title_text_cap_offset;
  if ((icon_align == GAlignLeft) && subtitle_text_frame_height) {
    cap_offsets_to_apply += subtitle_text_cap_offset;
  }
  rect.origin.y -= cap_offsets_to_apply;
  graphics_draw_text(ctx, config->title, title_font, rect, config->overflow_mode,
                     text_alignment, NULL);
  // Add back the cap offset so functions that use the returned title text frame can position
  // themselves using the actual frame
  rect.origin.y += cap_offsets_to_apply;
  return rect;
}

static ALWAYS_INLINE void prv_menu_cell_basic_draw_custom_two_columns_round(
    GContext *ctx, const GRect *cell_layer_bounds, const MenuCellLayerConfig *config,
    bool is_selected) {
  if (!cell_layer_bounds) {
    return;
  }

  const GSize icon_size = config->icon ? config->icon->bounds.size : GSizeZero;

  // Calculate the size used by the value or icon on the right
  // NOTE: If a value and icon is provided we only draw the value so we can re-use this function for
  // drawing "icon on right" and "value"
  GSize right_element_size;
  const GFont value_font = prv_get_cell_value_font(config);
  if (config->value) {
    right_element_size = graphics_text_layout_get_max_used_size(
        ctx, config->value, value_font, *cell_layer_bounds, config->overflow_mode,
        GTextAlignmentRight, NULL);
  } else {
    right_element_size = icon_size;
  }

  // We reuse this rect to save stack space; here it is the rect of the left column content
  GRect rect = *cell_layer_bounds;
  prv_grect_inset(&rect, &GEdgeInsets(0, right_element_size.w, 0, 0));
  // We overwrite rect to store the rect of the title text frame drawn in the one-column function
  rect = prv_menu_cell_basic_draw_custom_one_column_round(ctx, &rect, config, GTextAlignmentLeft,
                                                          GAlignLeft, is_selected);
  // Don't draw the right element if we couldn't draw the title in the left column
  if (grect_equal(&rect, &GRectZero)) {
    return;
  }

  // Now we store the right element frame in rect
  rect = (GRect) {
    .origin = GPoint(grect_get_max_x(&rect), rect.origin.y),
    .size = right_element_size
  };

  if (config->value) {
    rect.origin.y -= fonts_get_font_cap_offset(value_font);
    graphics_draw_text(ctx, config->value, value_font, rect, config->overflow_mode,
                       GTextAlignmentRight, NULL);
  } else {
    // Only draw the icon if it fits within the cell after aligning it center right
    grect_clip(&rect, cell_layer_bounds);
    grect_align(&rect, cell_layer_bounds, GAlignRight, true);
    if (gsize_equal(&rect.size, &icon_size)) {
      // round has never worked with legacy2 apps
      static const bool is_legacy2 = false;
      if (config->icon_box_model) {
        rect.origin = gpoint_add(rect.origin, config->icon_box_model->offset);
      }
      prv_draw_icon(ctx, config->icon, &rect, is_legacy2);
    }
  }
}

static ALWAYS_INLINE void prv_menu_cell_basic_draw_custom_round(
    GContext* ctx, const Layer *cell_layer, const MenuCellLayerConfig *config) {
  // TODO PBL-23041: When round MenuLayer animations are enabled, we need a "is_selected" function
  const bool cell_is_selected = menu_cell_layer_is_highlighted(cell_layer);
  const bool draw_two_columns =
      (config->value || (config->icon && ((GAlign)config->icon_align == GAlignRight)));

  // Determine appropriate insets to match designs
  GRect cell_layer_bounds = cell_layer->bounds;
  const int16_t horizontal_inset = (cell_is_selected && !draw_two_columns)
                                   ? MENU_CELL_ROUND_FOCUSED_HORIZONTAL_INSET
                                   : MENU_CELL_ROUND_UNFOCUSED_HORIZONTAL_INSET;
  prv_grect_inset(&cell_layer_bounds,
                  &GEdgeInsets(0, horizontal_inset + config->horizontal_inset));

  if (draw_two_columns) {
    prv_menu_cell_basic_draw_custom_two_columns_round(ctx, &cell_layer_bounds, config,
                                                      cell_is_selected);
  } else {
    prv_menu_cell_basic_draw_custom_one_column_round(ctx, &cell_layer_bounds, config,
                                                     GTextAlignmentCenter, GAlignCenter,
                                                     cell_is_selected);
  }
}

static ALWAYS_INLINE void prv_draw_cell(GContext *ctx, const Layer *cell_layer,
                                        const MenuCellLayerConfig *config) {
  PBL_IF_RECT_ELSE(prv_menu_cell_basic_draw_custom_rect,
                   prv_menu_cell_basic_draw_custom_round)(ctx, cell_layer, config);
}

//! TODO: PBL-21467 Replace with MenuCellLayer
void menu_cell_layer_draw(GContext *ctx, const Layer *cell_layer,
                          const MenuCellLayerConfig *config) {
  prv_draw_cell(ctx, cell_layer, config);
}

static ALWAYS_INLINE void prv_draw_basic(
    GContext* ctx, const Layer *cell_layer, GFont const title_font, const char *title,
    GFont const value_font, const char *value, GFont const subtitle_font, const char *subtitle,
    GBitmap *icon, bool icon_on_right, GTextOverflowMode overflow_mode) {
  MenuCellLayerConfig config = {
    .title_font = title_font,
    .subtitle_font = subtitle_font,
    .value_font = value_font,
    .title = title,
    .subtitle = subtitle,
    .value = value,
    .icon = icon,
    .icon_align = icon_on_right ? MenuCellLayerIconAlign_Right :
                                  PBL_IF_RECT_ELSE(MenuCellLayerIconAlign_Left,
                                                   MenuCellLayerIconAlign_Top),
    .overflow_mode = overflow_mode,
  };
  prv_draw_cell(ctx, cell_layer, &config);
}

void menu_cell_basic_draw_custom(GContext* ctx, const Layer *cell_layer, GFont const title_font,
                                 const char *title, GFont const value_font, const char *value,
                                 GFont const subtitle_font, const char *subtitle, GBitmap *icon,
                                 bool icon_on_right, GTextOverflowMode overflow_mode) {
  prv_draw_basic(ctx, cell_layer, title_font, title, value_font, value, subtitle_font, subtitle,
                 icon, icon_on_right, overflow_mode);
}

void menu_cell_basic_draw_icon_right(GContext* ctx, const Layer *cell_layer,
                                     const char *title, const char *subtitle, GBitmap *icon) {
  prv_draw_basic(ctx, cell_layer, NULL, title, NULL, NULL, NULL, subtitle, icon, true,
                 GTextOverflowModeFill);
}

void menu_cell_basic_draw(GContext* ctx, const Layer *cell_layer, const char *title,
                          const char *subtitle, GBitmap *icon) {
  prv_draw_basic(ctx, cell_layer, NULL, title, NULL, NULL, NULL, subtitle, icon, false,
                 GTextOverflowModeFill);
}

//////////////////////
// Title menu cell

void menu_cell_title_draw(GContext* ctx, const Layer *cell_layer, const char *title) {
  // Title:
  if (title) {
    const bool is_legacy2 = process_manager_compiled_with_legacy2_sdk();
    if (is_legacy2) {
      // Update the text color to Black for legacy apps - this is to maintain existing behavior for
      // 2.x compiled apps - no need to restore original since original 2.x did not do any restore
      graphics_context_set_text_color(ctx, GColorBlack);
    }
    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28);
    GRect box = cell_layer->bounds;
    box.origin.x = 3;
    box.origin.y -= 4;
    box.size.w -= 3;
    graphics_draw_text(ctx, title, font, box, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
}

//////////////////////
// Basic header cell

void menu_cell_basic_header_draw(GContext* ctx, const Layer *cell_layer, const char *title) {
  // Title:
  if (title) {
    const bool is_legacy2 = process_manager_compiled_with_legacy2_sdk();
    if (is_legacy2) {
      // Update the text color to Black for legacy apps - this is to maintain existing behavior for
      // 2.x compiled apps - no need to restore original since original 2.x did not do any restore
      graphics_context_set_text_color(ctx, GColorBlack);
    }
    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    GRect box = cell_layer->bounds;
    // Pixel nudging...
    box.origin.x += 2;
    box.origin.y -= 1;
    graphics_draw_text(ctx, title, font, box, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
}

