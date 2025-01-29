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

#include "launcher_app_glance_structured.h"

#include "launcher_app_glance_private.h"
#include "launcher_menu_layer.h"

#include "applib/graphics/gdraw_command_transforms.h"
#include "applib/ui/kino/kino_reel_custom.h"
#include "kernel/pbl_malloc.h"
#include "process_management/app_install_manager.h"
#include "resource/resource_ids.auto.h"
#include "services/normal/timeline/attribute.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/string.h"
#include "util/struct.h"

#if PLATFORM_ROBERT
#define LAUNCHER_APP_GLANCE_STRUCTURED_ICON_HORIZONTAL_MARGIN (9)
#else
#define LAUNCHER_APP_GLANCE_STRUCTURED_ICON_HORIZONTAL_MARGIN (5)
#endif

typedef struct GenericGlanceIconDrawCommandProcessor {
  GDrawCommandProcessor draw_command_processor;
  GColor8 *luminance_tint_lookup_table;
} GenericGlanceIconDrawCommandProcessor;

static void prv_structured_glance_icon_draw_command_processor_process_command(
    GDrawCommandProcessor *processor, GDrawCommand *processed_command,
    UNUSED size_t processed_command_max_size, UNUSED const GDrawCommandList *list,
    UNUSED const GDrawCommand *command) {
  GenericGlanceIconDrawCommandProcessor *processor_with_data =
      (GenericGlanceIconDrawCommandProcessor *)processor;
  const GColor8 *luminance_tint_lookup_table = processor_with_data->luminance_tint_lookup_table;

  // Luminance tint the fill color
  const GColor fill_color = gdraw_command_get_fill_color(processed_command);
  const GColor tinted_fill_color =
      gcolor_perform_lookup_using_color_luminance_and_multiply_alpha(fill_color,
                                                                     luminance_tint_lookup_table);
  gdraw_command_replace_color(processed_command, fill_color, tinted_fill_color);
  gdraw_command_set_fill_color(processed_command, tinted_fill_color);

  // Luminance tint the stroke color
  const GColor stroke_color = gdraw_command_get_stroke_color(processed_command);
  const GColor tinted_stroke_color =
      gcolor_perform_lookup_using_color_luminance_and_multiply_alpha(stroke_color,
                                                                     luminance_tint_lookup_table);
  gdraw_command_set_stroke_color(processed_command, tinted_stroke_color);
}

typedef struct GenericGlanceIconBitmapProcessor {
  GBitmapProcessor bitmap_processor;
  GCompOp saved_compositing_mode;
  GColor saved_tint_color;
  GColor desired_tint_color;
} GenericGlanceIconBitmapProcessor;

static void prv_structured_glance_icon_bitmap_processor_pre_func(
    GBitmapProcessor *processor, GContext *ctx, UNUSED const GBitmap **bitmap_to_use,
    UNUSED GRect *global_grect_to_use) {
  GenericGlanceIconBitmapProcessor *processor_with_data =
      (GenericGlanceIconBitmapProcessor *)processor;
  // Save the current compositing mode and tint color
  processor_with_data->saved_compositing_mode = ctx->draw_state.compositing_mode;
  processor_with_data->saved_tint_color = ctx->draw_state.tint_color;
  // Set the compositing mode so that we luminance tint the icon to the specified color
  ctx->draw_state.compositing_mode = GCompOpTintLuminance;
  ctx->draw_state.tint_color = processor_with_data->desired_tint_color;
}

static void prv_structured_glance_icon_bitmap_processor_post_func(
    GBitmapProcessor *processor, GContext *ctx, UNUSED const GBitmap *bitmap_used,
    UNUSED const GRect *global_clipped_grect_used) {
  GenericGlanceIconBitmapProcessor *processor_with_data =
    (GenericGlanceIconBitmapProcessor *)processor;
  // Restore the saved compositing mode and tint color
  ctx->draw_state.compositing_mode = processor_with_data->saved_compositing_mode;
  ctx->draw_state.tint_color = processor_with_data->saved_tint_color;
}

GColor launcher_app_glance_structured_get_highlight_color(
    LauncherAppGlanceStructured *structured_glance) {
  return PBL_IF_COLOR_ELSE(GColorBlack,
                           structured_glance->glance.is_highlighted ? GColorWhite : GColorBlack);
}

void launcher_app_glance_structured_draw_icon(LauncherAppGlanceStructured *structured_glance,
                                              GContext *ctx, KinoReel *icon, GPoint origin) {
  const GColor desired_tint_color =
      launcher_app_glance_structured_get_highlight_color(structured_glance);

  GenericGlanceIconBitmapProcessor structured_glance_icon_bitmap_processor = {
    .bitmap_processor = {
      .pre = prv_structured_glance_icon_bitmap_processor_pre_func,
      .post = prv_structured_glance_icon_bitmap_processor_post_func,
    },
    .desired_tint_color = desired_tint_color,
  };

  GColor8 luminance_tint_lookup_table[GCOLOR8_COMPONENT_NUM_VALUES] = {};
  gcolor_tint_luminance_lookup_table_init(desired_tint_color, luminance_tint_lookup_table);

  GenericGlanceIconDrawCommandProcessor structured_glance_icon_draw_command_processor = {
    .draw_command_processor.command =
        prv_structured_glance_icon_draw_command_processor_process_command,
    .luminance_tint_lookup_table = luminance_tint_lookup_table,
  };

  KinoReelProcessor structured_glance_icon_processor = {
    .bitmap_processor = &structured_glance_icon_bitmap_processor.bitmap_processor,
    .draw_command_processor =
        &structured_glance_icon_draw_command_processor.draw_command_processor,
  };

  // Draw the glance's icon, luminance tinting its colors according to the glance's highlight
  kino_reel_draw_processed(icon, ctx, origin, &structured_glance_icon_processor);
}

static void prv_structured_glance_icon_node_draw_cb(GContext *ctx, const GRect *rect,
                                                    UNUSED const GTextNodeDrawConfig *config,
                                                    bool render, GSize *size_out, void *user_data) {
  LauncherAppGlanceStructured *structured_glance = user_data;
  KinoReel *icon = NULL;
  if (structured_glance && structured_glance->impl && structured_glance->impl->get_icon) {
    icon = structured_glance->impl->get_icon(structured_glance);
  }

  if (render && icon) {
    // Center the frame in which we'll draw the icon
    GRect icon_frame = (GRect) { .size = kino_reel_get_size(icon) };
    grect_align(&icon_frame, rect, GAlignCenter, false /* clip */);

    // Save the GContext's clip box and override it so we clip the icon to the max icon size
    const GRect saved_clip_box = ctx->draw_state.clip_box;
    ctx->draw_state.clip_box.origin = gpoint_add(ctx->draw_state.drawing_box.origin,
                                                 rect->origin);
    ctx->draw_state.clip_box.size = rect->size;

    // Prevent drawing outside of the existing clip box
    grect_clip(&ctx->draw_state.clip_box, &saved_clip_box);

    // Draw the icon!
    launcher_app_glance_structured_draw_icon(structured_glance, ctx, icon, icon_frame.origin);

    // Restore the saved clip box
    ctx->draw_state.clip_box = saved_clip_box;
  }

  if (size_out) {
    *size_out = structured_glance->icon_max_size;
  }
}

static GTextNode *prv_structured_glance_create_text_node(
    LauncherAppGlanceStructured *structured_glance, GFont font, size_t buffer_size,
    GTextNodeTextDynamicUpdate update) {
  if (!structured_glance) {
    return NULL;
  }
  GTextNodeTextDynamic *dynamic_text_node =
      graphics_text_node_create_text_dynamic(buffer_size, update, structured_glance);
  GTextNodeText *underlying_text_node_text = &dynamic_text_node->text;
  underlying_text_node_text->font = font;
  underlying_text_node_text->color =
      launcher_app_glance_structured_get_highlight_color(structured_glance);
  underlying_text_node_text->overflow = GTextOverflowModeTrailingEllipsis;
  underlying_text_node_text->node.offset = GPoint(0, -fonts_get_font_cap_offset(font));
  underlying_text_node_text->max_size.h = fonts_get_font_height(font);
  return &underlying_text_node_text->node;
}

static void prv_structured_glance_title_dynamic_text_node_update(
    UNUSED GContext *ctx, UNUSED GTextNode *node, UNUSED const GRect *box,
    UNUSED const GTextNodeDrawConfig *config, UNUSED bool render, char *buffer, size_t buffer_size,
    void *user_data) {
  LauncherAppGlanceStructured *structured_glance = user_data;
  const char *title = NULL;
  if (structured_glance && structured_glance->impl && structured_glance->impl->get_title) {
    title = structured_glance->impl->get_title(structured_glance);
  }
  if (title) {
    strncpy(buffer, title, buffer_size);
    buffer[buffer_size - 1] = '\0';
  }
}

static GTextNode *prv_structured_glance_create_title_text_node(
    LauncherAppGlanceStructured *structured_glance) {
  return prv_structured_glance_create_text_node(
      structured_glance, structured_glance->title_font, APP_NAME_SIZE_BYTES,
      prv_structured_glance_title_dynamic_text_node_update);
}

typedef struct ScrollAnimationVars {
  int16_t total_px_to_scroll;
  int16_t current_offset;
  uint32_t duration_ms;
} ScrollAnimationVars;

//! Calculates the variables of a text scrolling animation that proceeds as follows:
//!   - Pauses a bit at the start
//!   - Scrolls the provided text at a moderate pace up to 3x the width of the provided draw_box
//!   - Pauses a bit when the end of the scrollable text is reached
//!   - Rewinds the text back to a zero offset at a rapid pace
//! Returns true if conditions are right for scrolling and output arguments should be used,
//! false otherwise.
static bool prv_get_text_scroll_vars(GContext *ctx, uint32_t cumulative_elapsed_ms,
                                     const char *text, const GRect *draw_box, GFont font,
                                     GTextAlignment text_alignment, GTextOverflowMode overflow_mode,
                                     TextLayoutExtended *layout, ScrollAnimationVars *vars_out) {
  if (!vars_out) {
    return false;
  }

  // Allow for showing up to 3x the width of the draw_box for the text
  const int16_t max_text_width = draw_box->size.w * (int16_t)3;
  const GRect max_text_box = (GRect) { .size = GSize(max_text_width, draw_box->size.h) };
  const int16_t scroll_visible_text_width =
      graphics_text_layout_get_max_used_size(ctx, text, font, max_text_box,
                                             overflow_mode, text_alignment,
                                             (GTextLayoutCacheRef)layout).w;

  if (scroll_visible_text_width <= draw_box->size.w) {
    // No need to scroll because text fits completely in the provided draw_box
    return false;
  }

  // This is the amount we'll scroll the text from start to end to show all of the
  // scroll_visible_text_width in the provided draw_box
  const int16_t total_px_to_scroll = scroll_visible_text_width - draw_box->size.w;
  vars_out->total_px_to_scroll = total_px_to_scroll;

  // These values were tuned with feedback from Design
  const uint32_t normal_scroll_speed_ms_per_px = 20;
  const uint32_t normal_scroll_duration_ms = total_px_to_scroll * normal_scroll_speed_ms_per_px;
  const uint32_t rewind_scroll_speed_ms_per_px = 2;
  const uint32_t rewind_scroll_duration_ms = total_px_to_scroll * rewind_scroll_speed_ms_per_px;
  const uint32_t pause_at_start_ms = 600;
  const uint32_t pause_at_end_ms = 750;

  const uint32_t scroll_duration_ms =
      pause_at_start_ms + normal_scroll_duration_ms + pause_at_end_ms + rewind_scroll_duration_ms;
  vars_out->duration_ms = scroll_duration_ms;

  // Technically mod isn't necessary right now, but it's needed for looping eventually (PBL-40544)
  int64_t elapsed_ms = cumulative_elapsed_ms % scroll_duration_ms;
  const uint32_t end_of_normal_scroll_duration_ms = pause_at_start_ms + normal_scroll_duration_ms;
  bool rewind = false;
  if (WITHIN(elapsed_ms, 0, end_of_normal_scroll_duration_ms)) {
    elapsed_ms = MAX(elapsed_ms - pause_at_start_ms, 0);
  } else if (elapsed_ms < end_of_normal_scroll_duration_ms + pause_at_end_ms) {
    elapsed_ms = normal_scroll_duration_ms;
  } else {
    elapsed_ms = scroll_duration_ms - elapsed_ms;
    rewind = true;
  }

  const uint32_t elapsed_normalized =
      ((uint32_t)elapsed_ms * ANIMATION_NORMALIZED_MAX) /
        (rewind ? rewind_scroll_duration_ms : normal_scroll_duration_ms);
  vars_out->current_offset = interpolate_int16(elapsed_normalized, 0, total_px_to_scroll);

  return true;
}

//! Currently the subtitle scrolling drives the duration of the overall glance selection animation
//! because we only scroll once, and since we don't know what we're scrolling until this function
//! is called, we need to record the duration of the scrolling animation in this function so the
//! glance's KinoReel reports the correct duration for the overall selection animation.
static void prv_adjust_subtitle_node_for_scrolling_animation(
    LauncherAppGlanceStructured *structured_glance, GContext *ctx, GTextNodeText *node_text,
    const char *text, const GRect *draw_box) {
  const uint32_t cumulative_elapsed_ms = structured_glance->selection_animation_elapsed_ms;

  ScrollAnimationVars vars;
  if (!prv_get_text_scroll_vars(ctx,
                                cumulative_elapsed_ms,
                                text, draw_box, node_text->font,
                                node_text->alignment, node_text->overflow,
                                &structured_glance->subtitle_scroll_calc_text_layout, &vars)) {
    // No need to scroll because text fits completely on-screen, set the selection animation
    // duration to 0 and bail out
    structured_glance->selection_animation_duration_ms = 0;
    return;
  }

  // Assumes that the default offset.x for the subtitle node is 0, which is true for generic glances
  node_text->node.offset.x = -vars.current_offset;
  // Assumes that the default margin.w for the subtitle node is 0, which is true for generic glances
  node_text->node.margin.w = (vars.current_offset != 0) ? -vars.total_px_to_scroll : (int16_t)0;

  // Record any change in the selection animation's duration
  LauncherAppGlanceService *service = structured_glance->glance.service;
  if (vars.duration_ms != structured_glance->selection_animation_duration_ms) {
    const uint32_t previous_selection_animation_duration_ms =
        structured_glance->selection_animation_duration_ms;
    structured_glance->selection_animation_duration_ms = vars.duration_ms;
    // If we're starting a new scroll or a scroll is currently in-progress, pause and then
    // play the animation so it is updated with the new duration (e.g. so we don't stop in a weird
    // place because the previous duration is shorter than the new one)
    if ((previous_selection_animation_duration_ms == 0) || (cumulative_elapsed_ms != 0)) {
      launcher_app_glance_service_pause_current_glance(service);
      launcher_app_glance_service_play_current_glance(service);
    }
  }
}

static void prv_structured_glance_subtitle_dynamic_text_node_update(
    GContext *ctx, GTextNode *node, const GRect *box, const GTextNodeDrawConfig *config,
    bool render, char *buffer, size_t buffer_size,  void *user_data) {
  LauncherAppGlanceStructured *structured_glance = user_data;
  if (structured_glance->subtitle_update) {
    structured_glance->subtitle_update(ctx, node, box, config, render, buffer, buffer_size,
                                       user_data);
  }
  GTextNodeText *node_text = (GTextNodeText *)node;
  if (!render) {
    prv_adjust_subtitle_node_for_scrolling_animation(structured_glance, ctx, node_text, buffer,
                                                     box);
  }
}

GTextNode *launcher_app_glance_structured_create_subtitle_text_node(
    LauncherAppGlanceStructured *structured_glance, GTextNodeTextDynamicUpdate update) {
  const size_t subtitle_buffer_size = ATTRIBUTE_APP_GLANCE_SUBTITLE_MAX_LEN + 1;
  structured_glance->subtitle_update = update;
  GTextNode *node = prv_structured_glance_create_text_node(
      structured_glance, structured_glance->subtitle_font, subtitle_buffer_size,
      prv_structured_glance_subtitle_dynamic_text_node_update);
  // Clip subtitle text nodes to their draw box since we scroll them if they're too long
  node->clip = true;
  return node;
}

static GTextNode *prv_create_structured_glance_title_subtitle_node(
    LauncherAppGlanceStructured *structured_glance, const GRect *glance_frame) {
  // Title node and subtitle node
  const size_t max_vertical_nodes = 2;
  GTextNodeVertical *vertical_node = graphics_text_node_create_vertical(max_vertical_nodes);
  vertical_node->vertical_alignment = GVerticalAlignmentCenter;

  GTextNode *title_node = prv_structured_glance_create_title_text_node(structured_glance);
  // We require a valid title node
  PBL_ASSERTN(title_node);
  // Push the title node a little up or down to match the relevant design spec
#if PLATFORM_ROBERT
  title_node->offset.y += 1;
#else
  title_node->offset.y -= 1;
#endif
  graphics_text_node_container_add_child(&vertical_node->container, title_node);

  GTextNode *subtitle_node = NULL;
  if (structured_glance->impl && structured_glance->impl->create_subtitle_node) {
    subtitle_node = structured_glance->impl->create_subtitle_node(structured_glance);
  }
  // The subtitle node is optional
  if (subtitle_node) {
    graphics_text_node_container_add_child(&vertical_node->container, subtitle_node);
  }

  // Set the vertical container's width to exactly what it should be so it doesn't resize based
  // on its changing content (e.g. scrolling subtitle)
  vertical_node->container.size.w =
      glance_frame->size.w - structured_glance->icon_horizontal_margin -
          structured_glance->icon_max_size.w;

  return &vertical_node->container.node;
}

// NOINLINE to save stack; on Spalding this can be enough to push us over the edge.
static NOINLINE GTextNode *prv_create_structured_glance_node(
    LauncherAppGlanceStructured *structured_glance, const GRect *glance_frame) {
  // Icon node and title/subtitle nodes
  const size_t max_horizontal_nodes = 2;
  GTextNodeHorizontal *horizontal_node = graphics_text_node_create_horizontal(max_horizontal_nodes);
  horizontal_node->horizontal_alignment = GTextAlignmentLeft;

  // This vertical node is just a container to vertically center the icon node
  const size_t max_vertical_icon_container_nodes = 1;
  GTextNodeVertical *vertical_icon_container_node =
    graphics_text_node_create_vertical(max_vertical_icon_container_nodes);
  vertical_icon_container_node->vertical_alignment = GVerticalAlignmentCenter;

  // This horizontal node is just a container to horizontally center the icon node
  const size_t max_horizontal_icon_container_nodes = 1;
  GTextNodeHorizontal *horizontal_icon_container_node =
    graphics_text_node_create_horizontal(max_horizontal_icon_container_nodes);
  horizontal_icon_container_node->horizontal_alignment = GTextAlignmentCenter;
  graphics_text_node_container_add_child(&vertical_icon_container_node->container,
                                         &horizontal_icon_container_node->container.node);

  GTextNodeCustom *icon_node =
      graphics_text_node_create_custom(prv_structured_glance_icon_node_draw_cb, structured_glance);
  icon_node->node.margin.w = structured_glance->icon_horizontal_margin;
  // The +1 is to force a rounding up. This way, 3 pixels extra will move closer to the screen
  // edge, instead of closer to the text.
  icon_node->node.offset.x -= (LAUNCHER_APP_GLANCE_STRUCTURED_ICON_HORIZONTAL_MARGIN -
                                structured_glance->icon_horizontal_margin + 1) / 2;
  graphics_text_node_container_add_child(&horizontal_icon_container_node->container,
                                         &icon_node->node);

  graphics_text_node_container_add_child(&horizontal_node->container,
                                         &vertical_icon_container_node->container.node);

  GTextNode *title_subtitle_node =
      prv_create_structured_glance_title_subtitle_node(structured_glance, glance_frame);
  graphics_text_node_container_add_child(&horizontal_node->container,
                                         title_subtitle_node);

  return &horizontal_node->container.node;
}

static void prv_draw_processed(KinoReel *reel, GContext *ctx, GPoint offset,
                               UNUSED KinoReelProcessor *processor) {
  LauncherAppGlanceStructured *structured_glance = kino_reel_custom_get_data(reel);
  if (!structured_glance) {
    return;
  }

  GRect glance_frame = (GRect) { .origin = offset, .size = structured_glance->glance.size };
#if PLATFORM_ROBERT
  const int16_t horizontal_inset = 10;
#else
  const int16_t horizontal_inset = PBL_IF_RECT_ELSE(6, 23);
#endif
  glance_frame = grect_inset_internal(glance_frame, horizontal_inset, 0);

  GTextNode *structured_glance_node = prv_create_structured_glance_node(structured_glance,
                                                                        &glance_frame);
  if (structured_glance_node) {
    graphics_text_node_draw(structured_glance_node, ctx, &glance_frame, NULL, NULL);
  }
  graphics_text_node_destroy(structured_glance_node);
}

static uint32_t prv_get_elapsed(KinoReel *reel) {
  LauncherAppGlanceStructured *structured_glance = kino_reel_custom_get_data(reel);
  return NULL_SAFE_FIELD_ACCESS(structured_glance, selection_animation_elapsed_ms, 0);
}

static bool prv_set_elapsed(KinoReel *reel, uint32_t elapsed_ms) {
  LauncherAppGlanceStructured *structured_glance = kino_reel_custom_get_data(reel);
  if (!structured_glance) {
    return false;
  }
  if (!structured_glance->selection_animation_disabled) {
    structured_glance->selection_animation_elapsed_ms = elapsed_ms;
  }
  // We assume the selection animation loops so that it's last frame is the same as its first frame,
  // so let's enforce that here so the animation update code above works properly
  if (structured_glance->selection_animation_elapsed_ms == kino_reel_get_duration(reel)) {
    structured_glance->selection_animation_elapsed_ms = 0;
  }
  return !structured_glance->selection_animation_disabled;
}

static uint32_t prv_get_duration(KinoReel *reel) {
  // TODO PBL-40544: Loop the selection animation
  LauncherAppGlanceStructured *structured_glance = kino_reel_custom_get_data(reel);
  return NULL_SAFE_FIELD_ACCESS(structured_glance, selection_animation_duration_ms, 0);
}

static void prv_destructor(KinoReel *reel) {
  LauncherAppGlanceStructured *structured_glance = kino_reel_custom_get_data(reel);
  if (structured_glance && structured_glance->impl && structured_glance->impl->destructor) {
    structured_glance->impl->destructor(structured_glance);
  }
}

static const KinoReelImpl s_launcher_app_glance_structured_reel_impl = {
  .reel_type = KinoReelTypeCustom,
  .get_size = launcher_app_glance_get_size_for_reel,
  .draw_processed = prv_draw_processed,
  .destructor = prv_destructor,
  .get_duration = prv_get_duration,
  .get_elapsed = prv_get_elapsed,
  .set_elapsed = prv_set_elapsed,
};

LauncherAppGlanceStructured *launcher_app_glance_structured_create(
    const Uuid *uuid, const LauncherAppGlanceStructuredImpl *impl, bool should_consider_slices,
    void *data) {
  PBL_ASSERTN(uuid);
  LauncherAppGlanceStructured *structured_glance = app_zalloc_check(sizeof(*structured_glance));
  const LauncherAppGlanceHandlers *base_handlers = impl ? &impl->base_handlers : NULL;
  structured_glance->impl = impl;
  structured_glance->data = data;
  structured_glance->icon_max_size = LAUNCHER_APP_GLANCE_STRUCTURED_ICON_MAX_SIZE;
  structured_glance->icon_horizontal_margin =
      LAUNCHER_APP_GLANCE_STRUCTURED_ICON_HORIZONTAL_MARGIN;
  structured_glance->title_font = fonts_get_system_font(LAUNCHER_MENU_LAYER_TITLE_FONT);
  structured_glance->subtitle_font = fonts_get_system_font(LAUNCHER_MENU_LAYER_SUBTITLE_FONT);
  KinoReel *glance_impl = kino_reel_custom_create(&s_launcher_app_glance_structured_reel_impl,
                                                  structured_glance);
  // Now that we've setup the structured glance's fields, initialize the LauncherAppGlance
  launcher_app_glance_init(&structured_glance->glance, uuid, glance_impl, should_consider_slices,
                           base_handlers);
  return structured_glance;
}

void *launcher_app_glance_structured_get_data(LauncherAppGlanceStructured *structured_glance) {
  return NULL_SAFE_FIELD_ACCESS(structured_glance, data, NULL);
}

void launcher_app_glance_structured_notify_service_glance_changed(
    LauncherAppGlanceStructured *structured_glance) {
  if (!structured_glance) {
    return;
  }
  launcher_app_glance_notify_service_glance_changed(&structured_glance->glance);
}

void launcher_app_glance_structured_set_icon_max_size(
    LauncherAppGlanceStructured *structured_glance, GSize new_size) {
  if (!structured_glance) {
    return;
  }

  structured_glance->icon_max_size = new_size;

  const int width_diff = structured_glance->icon_max_size.w -
                         LAUNCHER_APP_GLANCE_STRUCTURED_ICON_MAX_SIZE.w;
  structured_glance->icon_horizontal_margin =
      LAUNCHER_APP_GLANCE_STRUCTURED_ICON_HORIZONTAL_MARGIN - width_diff;

  if (structured_glance->icon_horizontal_margin < 0) {
    structured_glance->icon_horizontal_margin = 0;
  }
}
