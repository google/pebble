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

//!  Overview:
//!    - Summary of text layout and rendering:
//!      - A line iterator is created to iterate over the lines in a text-box
//!      - The line iterator creates a word iterator to advance through the text
//!      - The word iterator creates a character iterator to advance through
//!        codepoints. This allows reserved codepoints to be used for in-line text
//!        formatting.
//!      - The character iterator uses a UTF-8 iterator to advance through the
//!        UTF-8 encoded unicode codepoints.

#include "text.h"
#include "text_layout_private.h"

#include "graphics.h"
#include "graphics_private.h"
#include "gtypes.h"
#include "text_render.h"
#include "text_resources.h"
#include "utf8.h"

#include "applib/fonts/codepoint.h"
#include "applib/fonts/fonts.h"
#include "kernel/ui/kernel_ui.h"
#include "process_state/app_state/app_state.h"
#include "applib/applib_malloc.auto.h"
#include "process_state/app_state/app_state.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/hash.h"
#include "util/iterator.h"
#include "util/math.h"

#include "process_management/process_manager.h"

#include <stdint.h>
#include <string.h>
#include <limits.h>

static bool prv_char_iter_next_start_of_word(Iterator* char_iter);

// PBL-23045 Eventually remove perimeter debugging
void graphics_text_perimeter_debugging_enable(bool enable) {
  app_state_set_text_perimeter_debugging_enabled(enable);
}

// [CTX] processing individual codepoints doesn't work for contextual writing systems.
static int8_t prv_codepoint_get_horizontal_advance(FontCache* const font_cache,
                                                   const GFont font,
                                                   const Codepoint codepoint) {
  PBL_ASSERTN(font_cache);
  int8_t horiz_advance = 0;
  if (!codepoint_is_zero_width(codepoint)) {
    horiz_advance = text_resources_get_glyph_horiz_advance(font_cache, codepoint, font);
  }
  return MAX(horiz_advance, 0);
}

////////////////////////////////////////////////////////////
// Init functions

//! @note can be init to a null-termination character
void char_iter_init(Iterator* char_iter, CharIterState* char_iter_state, const TextBoxParams* const text_box_params, utf8_t* start) {
  Iterator* utf8_iter = &char_iter_state->utf8_iter;
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state->utf8_iter_state;

  utf8_iter_init(utf8_iter, utf8_iter_state, text_box_params->utf8_bounds, start);

  char_iter_state->text_box_params = text_box_params;

  iter_init(char_iter, (IteratorCallback) char_iter_next, char_iter_prev, (IteratorState) char_iter_state);
}

typedef enum {
  WordStateStart,
  WordStateIdeograph,
  WordStateGrowing,
  WordStateJoining,
  WordStateEnd,
} WordState;

WordState word_state_update(WordState state, Codepoint codepoint) {
  WordState new_state = state;

  switch (state) {
    case WordStateStart:
      if (codepoint == NEWLINE_CODEPOINT) {
        new_state = WordStateEnd;
      } else if (codepoint_is_ideograph(codepoint)) {
        new_state = WordStateIdeograph;
      } else {
        new_state = WordStateGrowing;
      }
      break;
    case WordStateIdeograph:
      if (codepoint == WORD_JOINER_CODEPOINT) {
        new_state = WordStateJoining;
      } else {
        new_state = WordStateEnd;
      }
    case WordStateGrowing:
      if (codepoint == WORD_JOINER_CODEPOINT) {
        new_state = WordStateJoining;
      } else if (codepoint_is_ideograph(codepoint) || codepoint_is_end_of_word(codepoint)) {
        new_state = WordStateEnd;
      } else {
        new_state = WordStateGrowing;
      }
      break;
    case WordStateJoining:
      if (codepoint == NEWLINE_CODEPOINT) {
        new_state = WordStateEnd;
      } else if (codepoint_is_ideograph(codepoint)) {
        new_state = WordStateIdeograph;
      } else if (codepoint == WORD_JOINER_CODEPOINT) {
        new_state = WordStateJoining;
      } else {
        new_state = WordStateGrowing;
      }
      break;
    case WordStateEnd:
      new_state = WordStateEnd;
      break;
  }

  return new_state;
}

//! @return true if init to new word, false otherwise (ie end of text)
//! @note assumes 'start' is not NULL, but does not assume 'start' is valid start of word
bool word_init(GContext* ctx, Word* word, const TextBoxParams* const text_box_params, utf8_t* start) {
  word->width_px = 0;

  if (*start == NULL_CODEPOINT) {
    word->start = start;
    word->end = start;
    return false;
  }

  // Set up iterator
  Iterator char_iter;
  CharIterState char_iter_state;
  char_iter_init(&char_iter, &char_iter_state, text_box_params, start);
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state.utf8_iter_state;

  bool success = prv_char_iter_next_start_of_word(&char_iter);
  if (!success) {
    // We couldn't find the next start of the word, just initialize to nothing
    word->start = start;
    word->end = start;
    return false;
  }

  // Init the word & state 
  word->start = utf8_iter_state->current;
  WordState state = WordStateStart;
  state = word_state_update(state, utf8_iter_state->codepoint);

  do {
    if (state == WordStateGrowing || state == WordStateIdeograph) {
      word->width_px += prv_codepoint_get_horizontal_advance(&ctx->font_cache,
          text_box_params->font, utf8_iter_state->codepoint);
    }

    iter_next(&char_iter);
    state = word_state_update(state, utf8_iter_state->codepoint);
  } while (state != WordStateEnd);

  word->end = utf8_iter_state->current;

  return true;
}

void word_iter_init(Iterator* word_iter, WordIterState* word_iter_state, GContext* ctx,
                    const TextBoxParams* const text_box_params, utf8_t* start) {
  *word_iter_state = (WordIterState) {
    .ctx = ctx,
    .text_box_params = text_box_params
  };

  word_init(ctx, &word_iter_state->current, text_box_params, start);

  iter_init(word_iter, (IteratorCallback) word_iter_next, NULL, (IteratorState) word_iter_state);
}

void line_iter_init(Iterator* line_iter, LineIterState* line_iter_state, GContext* ctx) {
  *line_iter_state = (LineIterState) {
    .ctx = ctx,
    .current = &ctx->text_draw_state.line
  };

  WordIterState* word_iter_state = &line_iter_state->word_iter_state;
  word_iter_init(&line_iter_state->word_iter, word_iter_state, ctx,
                 &ctx->text_draw_state.text_box, ctx->text_draw_state.text_box.utf8_bounds->start);

  iter_init(line_iter, (IteratorCallback) line_iter_next, NULL, (IteratorState) line_iter_state);
}

////////////////////////////////////////////////////////////
// Private helper functions

static int16_t prv_get_line_height(const TextBoxParams *text_box_params) {
  return fonts_get_font_height(text_box_params->font) + text_box_params->line_spacing_delta;
}

static int16_t prv_layout_get_line_spacing_delta(GTextLayoutCacheRef layout) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    return 0;
  }

  return (layout ? ((TextLayoutExtended *)layout)->line_spacing_delta: 0);
}

////////////////////////////////////////////////////////////
// Iterator advance functions

//! Advance the char iterator to the start of the next word. Used by word_init
//! to find the start of the next word.
//! @return is_success
static bool prv_char_iter_next_start_of_word(Iterator* char_iter) {
  CharIterState* char_iter_state = (CharIterState*) char_iter->state;
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state->utf8_iter_state;

  // the first codepoint could be invalid, iter_next takes care of the others
  Codepoint codepoint = utf8_iter_state->codepoint;
  if (codepoint_should_skip(codepoint) || codepoint_is_formatting_indicator(codepoint)) {
    if (!iter_next(char_iter)) {
      return false;
    }
  }

  while (codepoint_is_zero_width(utf8_iter_state->codepoint)) {
    if (utf8_iter_state->codepoint == 0) {
      PBL_ASSERTN(utf8_iter_state->current == utf8_iter_state->bounds->end);
      return false;
    }

    if (!iter_next(char_iter)) {
      break;
    }
  }

  return true;
}

static bool prv_line_iter_is_vertical_overflow(const LineIterState* const line_iter_state,
                                               const TextBoxParams* const text_box_params) {
  int16_t next_line_y_extent;
  // Normally, we lay out the text one line below the regular cutoff so that it may be rendered,
  // albeit clipped.  But, if we're rendering in truncation mode (e.g. GTextOverflowModeFill or
  // GTextOverflowModeTrailingEllipsis), we can immediately cut the text off below the box height
  // if we're not rendering the first line.
  //    - This, because the user does not expect to see more text drawn below, after the '...'.
  //    - The first-line exception means that text, and therefore the telltale
  //      ellipsis, will always be visisble.
  if ((text_box_params->overflow_mode == GTextOverflowModeTrailingEllipsis ||
       text_box_params->overflow_mode == GTextOverflowModeFill) &&
      line_iter_state->current->origin.y != text_box_params->box.origin.y) {
    // We're in a truncation mode AND not on the first line.
    // So, include the full height of the current line in next_line_y_extent, so text will stop
    // being layed out immediately after it exceeds the height of the container.
    next_line_y_extent = line_iter_state->current->origin.y + prv_get_line_height(text_box_params);
  } else {
    // We're either in a non-truncating mode, or on the first line of a truncating mode.
    // So, only include the extent of the previous line in next_line_y_extent (making it more of
    // a "last_line_y_extent").
    // Putting aside the misleading variable name, this will cause us to lay out one more line than
    // will completely fit in the container - so that it may still be displayed, even if partially
    // or completely clipped.
    next_line_y_extent = line_iter_state->current->origin.y;
  }
  return (next_line_y_extent > (text_box_params->box.origin.y + text_box_params->box.size.h));
}

//! @return is_advanced
bool line_iter_next(IteratorState state) {
  LineIterState* line_iter_state = (LineIterState*) state;
  const TextBoxParams* const text_box_params = &line_iter_state->ctx->text_draw_state.text_box;

  if (prv_line_iter_is_vertical_overflow(line_iter_state, text_box_params)) {
    return false;
  }

  line_iter_state->current->origin.x = text_box_params->box.origin.x;
  line_iter_state->current->origin.y += prv_get_line_height(text_box_params);
  line_iter_state->current->width_px = 0;  // needs to be reset per line
  line_iter_state->current->max_width_px = text_box_params->box.size.w;
  line_iter_state->current->suffix_codepoint = 0;
  line_iter_state->current->start = NULL;

  return true;
}

//! @return is_advanced
bool word_iter_next(IteratorState state) {
  WordIterState* word_iter_state = (WordIterState*) state;

  Word* current_word = &word_iter_state->current;
  const TextBoxParams* const text_box_params = word_iter_state->text_box_params;
  GContext* ctx = word_iter_state->ctx;

  if (*current_word->end == NULL_CODEPOINT) {
    return false;
  }

  return word_init(ctx, current_word, text_box_params, current_word->end);
}

//! @return is_advanced
bool char_iter_next(IteratorState state) {
  CharIterState* char_iter_state = (CharIterState*) state;

  Codepoint codepoint;
  Iterator* utf8_iter = &char_iter_state->utf8_iter;
  Utf8IterState* utf8_iter_state = &char_iter_state->utf8_iter_state;

  while (true) {
    if (utf8_iter_state->current >= utf8_iter_state->bounds->end) {
      // EOS while searching for valid codepoint
      return false;
    }

    bool is_utf8_advanced = iter_next(utf8_iter);
    codepoint = utf8_iter_state->codepoint;

    if (!is_utf8_advanced) {
      return is_utf8_advanced;
    }

    PBL_ASSERTN(codepoint != 0);

    if (codepoint_is_formatting_indicator(codepoint)) {
      continue;
    }

    if (codepoint_should_skip(codepoint)) {
      continue;
    };

    return true;
  }
}

bool char_iter_prev(IteratorState state) {
  CharIterState* char_iter_state = (CharIterState*) state;

  Codepoint codepoint;
  Iterator* utf8_iter = &char_iter_state->utf8_iter;
  Utf8IterState* utf8_iter_state = &char_iter_state->utf8_iter_state;

  while (true) {
    if (utf8_iter_state->current <= utf8_iter_state->bounds->start) {
      // EOS while searching for valid codepoint
      return false;
    }

    bool is_utf8_advanced = iter_prev(utf8_iter);
    codepoint = utf8_iter_state->codepoint;

    if (!is_utf8_advanced) {
      return is_utf8_advanced;
    }

    PBL_ASSERTN(codepoint != 0);

    if (codepoint_is_formatting_indicator(codepoint)) {
      continue;
    }

    if (codepoint_should_skip(codepoint)) {
      continue;
    };

    return true;
  }
}

////////////////////////////////////////////////////////////
// Helper functions

//! Trim given codepoint from the start of the word
//! Used to remove whitespace and newlines
//! @return is_trimmed
bool word_trim_preceeding_codepoint(GContext* ctx, Word* word, const Codepoint codepoint,
                                    const TextBoxParams* const text_box_params) {
  Iterator char_iter;
  CharIterState char_iter_state;
  char_iter_init(&char_iter, &char_iter_state, text_box_params, word->start);

  Utf8IterState* utf8_iter_state = &char_iter_state.utf8_iter_state;

  if (utf8_iter_state->codepoint != codepoint) {
    return false;
  }

  bool is_advanced = iter_next(&char_iter);

  if (!is_advanced) {
    PBL_ASSERTN(*word->end == NULL_CODEPOINT);
    word->start = NULL;
    return false;
  }

  if (word->end == char_iter_state.utf8_iter_state.current) {
    // Word has been completely trimmed; init a new word
    bool is_end_of_text = (word->end == NULL_CODEPOINT ||
        char_iter_state.utf8_iter_state.current >= text_box_params->utf8_bounds->end);

    if (!is_end_of_text) {
      word_init(ctx, word, text_box_params, word->end);
    }
    return false;
  }

  // Trim
  int advance = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
      text_box_params->font, codepoint);
  PBL_ASSERTN(advance <= word->width_px); // Negative-length word not allowed

  word->width_px -= advance;
  word->start = utf8_iter_state->current;
  return true;
}

// [INTL] whitespace is more than just the space character.
void word_trim_preceeding_whitespace(GContext* ctx, Word* word, const TextBoxParams* const text_box_params) {
  while (word_trim_preceeding_codepoint(ctx, word, SPACE_CODEPOINT, text_box_params));
}

////////////////////////////////////////////////////////////
// Walk Line

typedef void (*CharVisitorCallback)(GContext* ctx, const TextBoxParams* const text_box_params,
                                    Line* line, GRect cursor, const Codepoint codepoint);

void render_chars_char_visitor_cb(GContext* ctx, const TextBoxParams* const text_box_params,
                                  Line* line, GRect cursor, const Codepoint codepoint) {
  if (codepoint_is_zero_width(codepoint)) {
    return;
  }

  render_glyph(ctx, codepoint, text_box_params->font, cursor);
}

void update_dimensions_char_visitor_cb(GContext* ctx, const TextBoxParams* const text_box_params,
                                       Line* line, GRect cursor, const Codepoint codepoint) {
  (void) ctx;
  PBL_ASSERT(cursor.origin.x >= line->origin.x, "Text cursor x=<%u> ahead of line origin x=<%u>",
      cursor.origin.x, line->origin.x);

  const int glyph_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
      text_box_params->font, codepoint);

  line->width_px = (cursor.origin.x + glyph_width_px) - line->origin.x;

  PBL_ASSERT(line->width_px <= text_box_params->box.size.w,
      "Line <%p>: max extent=<%u> exceeds text_box_params width=<%u>",
      line, line->width_px + line->origin.x, text_box_params->box.size.w);
}

//! Call char_visitor_cb on each character in the line 
//! Used to update line dimensions and render characters
//! Traverse until end of line->width_px if rendering chars, else text_box_params width
//! if updating line dimensions
//! @return utf8_t* pointer to last visited character
utf8_t* walk_line(GContext* ctx, Line* line, const TextBoxParams* const text_box_params,
                  CharVisitorCallback char_visitor_cb) {
  PBL_ASSERTN(char_visitor_cb);

  // We used to check that the line height was <= the container height here - no longer required,
  // as the vertical overflow is handled during layout.

  int available_horiz_px;
  if (char_visitor_cb == update_dimensions_char_visitor_cb) {
    // Line dimensions not yet set; use all available line space
    available_horiz_px = line->max_width_px;
  } else {
    available_horiz_px = line->width_px;
  }

  PBL_ASSERT(line->width_px <= text_box_params->box.size.w,
      "Line <%p>: max extent=<%u> exceeds text_box_params width=<%u>", line,
      line->width_px + line->origin.x, text_box_params->box.size.w);

  int suffix_width_px = 0;

  if (line->suffix_codepoint) {
    suffix_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
        text_box_params->font, line->suffix_codepoint);
  }

  if (available_horiz_px < suffix_width_px) {
    return NULL;
  }

  // Set up iterator
  Iterator char_iter;
  CharIterState char_iter_state;
  char_iter_init(&char_iter, &char_iter_state, text_box_params, line->start);
  Utf8IterState* utf8_iter_state = (Utf8IterState*) &char_iter_state.utf8_iter_state;

  bool is_newline_as_space = text_box_params->overflow_mode == GTextOverflowModeFill;
  Codepoint current_codepoint = utf8_iter_state->codepoint;
  if (current_codepoint == NEWLINE_CODEPOINT) {
    if (is_newline_as_space) {
      current_codepoint = SPACE_CODEPOINT;
    } else {
      return utf8_iter_state->current;
    }
  }

  int walked_width_px = 0;
  int next_glyph_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
      text_box_params->font, current_codepoint);

  utf8_t* last_visited_char = NULL;

  while (walked_width_px + next_glyph_width_px + suffix_width_px <= available_horiz_px) {
    GRect cursor = {
      .origin = line->origin,
      .size.w = next_glyph_width_px,
      .size.h = fonts_get_font_height(text_box_params->font)
    };
    cursor.origin.x += walked_width_px;

    char_visitor_cb(ctx, text_box_params, line, cursor, current_codepoint);

    walked_width_px += next_glyph_width_px;

    last_visited_char = utf8_iter_state->current;

    if (!iter_next(&char_iter)) {
      break;
    }

    current_codepoint = utf8_iter_state->codepoint;
    if (current_codepoint == NEWLINE_CODEPOINT) {
      if (is_newline_as_space) {
        current_codepoint = SPACE_CODEPOINT;
      } else {
        break;
      }
    }

    next_glyph_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
        text_box_params->font, current_codepoint);
  }

  // Trim trailing whitespace
  if (last_visited_char) {
    while ((current_codepoint == NEWLINE_CODEPOINT || current_codepoint == SPACE_CODEPOINT)) {
      // Newlines should not adjust the width
      if (current_codepoint == NEWLINE_CODEPOINT) {
        next_glyph_width_px = 0;
      } else {
        next_glyph_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
                                                               text_box_params->font,
                                                               current_codepoint);
      }

      // Safety check
      if (walked_width_px < next_glyph_width_px) {
        break;
      }
      walked_width_px -= next_glyph_width_px;

      if (!iter_prev(&char_iter)) {
        break;
      }
      current_codepoint = utf8_iter_state->codepoint;
    }
  }

  if (line->suffix_codepoint) {
    GRect cursor = {
      .origin = line->origin,
      .size.w = next_glyph_width_px,
      .size.h = fonts_get_font_height(text_box_params->font)
    };
    cursor.origin.x += walked_width_px;
    if (char_visitor_cb) {
      char_visitor_cb(ctx, text_box_params, line, cursor, line->suffix_codepoint);
    }
  }

  return last_visited_char;
}


////////////////////////////////////////////////////////////
// Walk Lines

void set_ellipsis_on_overflow_last_line_cb(GContext* ctx, Line* line,
                                           const TextBoxParams* const text_box_params,
                                           const bool is_text_remaining) {
  // Only set a trailing ellipsis if there is text remaining
  if (!is_text_remaining) {
    return;
  }

  // Check if outputting two lines extend beyond the text box height - then display the ellipsis
  // on the current line
  bool is_last_line = ((line->origin.y + (2 * prv_get_line_height(text_box_params))) >
                       (text_box_params->box.origin.y + text_box_params->box.size.h));
  // Check if this is the last line
  if (!is_last_line) {
    return;
  }

  line->suffix_codepoint = ELLIPSIS_CODEPOINT;

  // update the line dimensions
  walk_line(ctx, line, text_box_params, update_dimensions_char_visitor_cb);
}

void render_all_render_line_cb(GContext* ctx, Line* line, const TextBoxParams* const text_box_params) {
  walk_line(ctx, line, text_box_params, (CharVisitorCallback) render_chars_char_visitor_cb);
}

void update_all_layout_update_cb(TextLayout* layout, Line* line,
                                 const TextBoxParams* const text_box_params) {
  PBL_ASSERTN(line);
  if (layout) {
    layout->max_used_size.h = (line->origin.y - layout->box.origin.y) + line->height_px +
                              text_box_params->line_spacing_delta;
    layout->max_used_size.w = MAX(line->width_px, layout->max_used_size.w);
  }
}

//! @return is_overflow
bool is_clip_box_overflow_top_stop_condition_cb(GContext* ctx, Line* line,
                                                const TextBoxParams* const text_box_params) {
  int next_line_max_y = line->origin.y;
  int clip_box_min_y = ctx->draw_state.clip_box.origin.y;
  return (next_line_max_y < clip_box_min_y);
}

//! @return is_overflow
bool is_clip_box_overflow_bottom_stop_condition_cb(GContext* ctx, Line* line,
                                                   const TextBoxParams* const text_box_params) {
  int next_line_min_y = line->origin.y + line->height_px + text_box_params->line_spacing_delta;
  int clip_box_max_y = ctx->draw_state.clip_box.origin.y + ctx->draw_state.clip_box.size.h;
  return (next_line_min_y > clip_box_max_y);
}

//! @return is_overflow
bool is_clip_box_overflow_stop_condition_cb(GContext* ctx, Line* line,
                                            const TextBoxParams* const text_box_params) {
  return (is_clip_box_overflow_bottom_stop_condition_cb(ctx, line, text_box_params) ||
          is_clip_box_overflow_top_stop_condition_cb(ctx, line, text_box_params));
}

#define TEXT_LINE_BASE_LINE(line) ((line)->height_px)
#define TEXT_LINE_CAP_LINE(line) ((line)->height_px * 1 / 2)
// Based on Gothic fonts, DESCENDER is approx 1/5 of height (ascender + descender)
// Gothic 24 Bold ascent = 840, descent 168
// Gothic 18 Bold ascent = 840, descent 168
// Gothic 14 ascent = 864, descent 144
// Bitham ascent = 800, descend = 200
// DroidSerif Bold ascent = 1638, descent = 410
#define TEXT_LINE_DESCENDER_LINE(line) DIVIDE_CEIL((line)->height_px, 5)  // 1/5th rounded up

T_STATIC NOINLINE MOCKABLE void prv_debug_perimeter(GContext *ctx, const GRangeHorizontal *h_range,
                                                   const Line *line) {
  // PBL-23045 Eventually remove perimeter debugging
  // Draw a red horizontal line to show the range of the current lines perimeter
  if (app_state_get_text_perimeter_debugging_enabled()) {
#if !defined(UNITTEST) && !defined(PLATFORM_TINTIN)
    const Fixed_S16_3 fixed_x1 = (Fixed_S16_3) {
      .integer = h_range->origin_x,
    };
    const Fixed_S16_3 fixed_x2 = (Fixed_S16_3) {
      .integer = h_range->origin_x + h_range->size_w,
    };
    graphics_private_draw_horizontal_line_prepared(ctx, &ctx->dest_bitmap,
                                                   &ctx->dest_bitmap.bounds,
                                                   line->origin.y + TEXT_LINE_CAP_LINE(line),
                                                   fixed_x1, fixed_x2, GColorRed);
    graphics_private_draw_horizontal_line_prepared(ctx, &ctx->dest_bitmap,
                                                   &ctx->dest_bitmap.bounds,
                                                   line->origin.y + TEXT_LINE_BASE_LINE(line),
                                                   fixed_x1, fixed_x2, GColorRed);
#endif
  }
}

typedef struct {
  int16_t origin_x;
  int16_t width_px;
} OrphanLineState;

static OrphanLineState prv_capture_orphan_state(Line const* line) {
  return (OrphanLineState) {
    .origin_x = line->origin.x,
    .width_px = line->width_px,
  };
}

static void prv_apply_orphan_state(const OrphanLineState *state, Line *line) {
  line->origin.x = state->origin_x;
  line->width_px = state->width_px;
}

//! Iterate over lines in the text box
static inline void prv_walk_lines_down(Iterator* const line_iter, TextLayout* const layout,
                                       WalkLinesCallbacks* const callbacks) {
  LineIterState* line_iter_state = (LineIterState*) line_iter->state;
  GContext* ctx = line_iter_state->ctx;
  const GSize ctx_size = graphics_context_get_framebuffer_size(ctx);
  const TextBoxParams* const text_box_params = &ctx->text_draw_state.text_box;
  Line* line = line_iter_state->current;

  const TextLayoutFlowData *flow_data = graphics_text_layout_get_flow_data(layout);
  const bool uses_paging = flow_data->paging.page_on_screen.size_h != 0;
  const bool uses_perimeter = flow_data->perimeter.impl != NULL;
  const GPoint perimeter_paging_offset =
    uses_paging ? gpoint_sub(flow_data->paging.origin_on_screen, line->origin) : GPointZero;
  Word prev_line_word = WORD_EMPTY;
  while (!prv_line_iter_is_vertical_overflow(line_iter_state, text_box_params)) {
    GPoint line_in_perimeter_space = gpoint_add(line->origin, perimeter_paging_offset);

    if (uses_paging) {
      const int16_t page_max_y = flow_data->paging.page_on_screen.origin_y +
                                 flow_data->paging.page_on_screen.size_h;

      // TODO: optimize
      while (line_in_perimeter_space.y < flow_data->paging.page_on_screen.origin_y) {
        line_in_perimeter_space.y += flow_data->paging.page_on_screen.size_h;
      }
      while (line_in_perimeter_space.y >= page_max_y) {
        line_in_perimeter_space.y -= flow_data->paging.page_on_screen.size_h;
      }

      const int16_t distance_to_page_end = page_max_y - line_in_perimeter_space.y;

      if (distance_to_page_end < line->height_px + TEXT_LINE_DESCENDER_LINE(line)) {
        // If this line would exceed the page_height, shift the line origin to the next page
        line->origin.y += distance_to_page_end;
        continue;  // skip rendering this round, bypasses iter_next (no reset necessary)
      }
    }

    // PBL-23045 Eventually remove perimeter debugging
    GRangeHorizontal debug_perimeter_horizontal_range = {};

    // If we are restricting the perimeter of the draw box, restrict per line region here
    if (uses_perimeter) {
      GRangeHorizontal text_horizontal_range = {.origin_x = line_in_perimeter_space.x,
                                                .size_w = line->max_width_px};
      const GRangeVertical vertical_range = {
        .origin_y = line_in_perimeter_space.y + TEXT_LINE_CAP_LINE(line),
        .size_h = TEXT_LINE_BASE_LINE(line) - TEXT_LINE_CAP_LINE(line)
      };
      GRangeHorizontal perimeter_horizontal_range =
        flow_data->perimeter.impl->callback(flow_data->perimeter.impl, &ctx_size, vertical_range,
                                            flow_data->perimeter.inset);

      prv_debug_perimeter(ctx, &perimeter_horizontal_range, line);

      // protect against range expanding: clip perimeter to the original text range
      grange_clip((GRange*)&perimeter_horizontal_range, (GRange*)&text_horizontal_range);
      text_horizontal_range = perimeter_horizontal_range;

      // convert range back to screen space
      text_horizontal_range.origin_x -= perimeter_paging_offset.x;

      // Update line parameters for restricted horizontal range
      line->origin.x = text_horizontal_range.origin_x;
      line->max_width_px = text_horizontal_range.size_w;
    }

    // reference into the iterator's current word to easily access this attribute here and
    // later without the complicated cast
    Word *const current_word_ref = &(((WordIterState*)line_iter_state->word_iter.state)->current);
    // state that needs to be captured so we can restore it in case of an orphan
    const Word word_before_rendering = *current_word_ref;
    const OrphanLineState orphan_state = prv_capture_orphan_state(line);

    // When repeating text to prevent orhpans we could run into the situation where repeating text
    // pushes down the remaining text far enough so it ends up on yet another page. This would
    // enter an infinite loop.
    // To avoid that, we only apply this strategy, when it's "safe" to do so (in theory, there's
    // still the propability to run into this scenario if the perimeter isn't vertically symmetric).
    // The chosen number should be large enough for the previous line, the orphan line plus some
    // buffer.
    const int num_safe_lines = 3;
    const bool page_contains_enough_lines =
      (flow_data->paging.page_on_screen.size_h >= num_safe_lines * line->height_px);
    bool avoiding_orphans = uses_paging && ctx->draw_state.avoid_text_orphans &&
                            page_contains_enough_lines;

render_line: {} // this {} is just an empty statement that both C and our linter accepts
    const bool is_text_remaining = line_add_words(
        line, &line_iter_state->word_iter, callbacks->last_line_cb);
    // NOTE: Account for descender - assume descender is no more than half the line height
    const int16_t line_spacing_delta = prv_layout_get_line_spacing_delta(layout);
    const int32_t line_max_y = line->origin.y + line->height_px +
                               TEXT_LINE_DESCENDER_LINE(line) + line_spacing_delta;
    const int32_t clip_box_min_y = ctx->draw_state.clip_box.origin.y;

    if (line_max_y > clip_box_min_y) {
      if (avoiding_orphans) {
        const bool line_is_first_line_page =
          (line_in_perimeter_space.y == flow_data->paging.page_on_screen.origin_y);
        const bool is_orphan =
          (line_is_first_line_page && prev_line_word.start && !is_text_remaining);

        if (is_orphan) {
          *current_word_ref = prev_line_word;
          prv_apply_orphan_state(&orphan_state, line);
          avoiding_orphans = false; // prevent infinite loops
          goto render_line;
        }
      }
      if (callbacks->render_line_cb) {
        callbacks->render_line_cb(ctx, line, text_box_params);
      }
    }
    prev_line_word = word_before_rendering;

    if (callbacks->layout_update_cb) {
      callbacks->layout_update_cb(layout, line, text_box_params);
    }

    if (callbacks->stop_condition_cb) {
      if (callbacks->stop_condition_cb(ctx, line, text_box_params)) {
        break;
      }
    }

    if (!is_text_remaining) {
      break;
    }

    // Shouldn't have rendered the line if there was insufficient space
    PBL_ASSERTN(iter_next(line_iter));
  }
}

////////////////////////////////////////////////////////////
// Text layout

//! @return is_success
bool line_add_word(GContext* ctx, Line* line, Word* word, const TextBoxParams* const text_box_params) {
  // Horizontal overflow
  if (line->width_px > line->max_width_px) {
    return false;
  }

  // Don't set the line height if there is a vertical overflow
  const int line_height = fonts_get_font_height(text_box_params->font);

  // We used to re-check for vertical overflow here
  // but this is protected by a call to prv_line_iter_is_vertical_overflow,
  // which will handle the truncation/clipping logic.

  PBL_ASSERTN(word->start);

  bool is_newline_first_codepoint = (*word->start == NEWLINE_CODEPOINT);

  line->height_px = line_height;

  if (is_newline_first_codepoint) {
    // This trims off leading \n's from word. If we reach the end of the text while doing this, it sets
    //  word->start to NULL. 
    word_trim_preceeding_codepoint(ctx, word, NEWLINE_CODEPOINT, text_box_params);
    if (text_box_params->overflow_mode != GTextOverflowModeFill) {
      return false;
    }
    // If there is word text left (we have \n's at the end of the text), we're done
    if (word->start == NULL) {
      return false;
    }
  }

  bool is_overflow = (line->width_px + word->width_px > line->max_width_px);
  bool is_start_of_line = (line->width_px == 0);
  bool should_hyphenate = (is_overflow && is_start_of_line);

  if (is_start_of_line) {
    line->start = word->start;
  }

  if (should_hyphenate) {
    // Set suffix character
    // [CJK] - when breaking a Katakana word, you probably don't want to add a hyphen. And to
    // a Japanese user, a hyphen with Katakana looks like a long (chou-on) sound mark.
    line->suffix_codepoint = HYPHEN_CODEPOINT;
    utf8_t* last_visited = walk_line(ctx, line, text_box_params,
        (CharVisitorCallback) update_dimensions_char_visitor_cb);
    last_visited = (last_visited == NULL) ? (word->start) : last_visited;

    // Trim the word
    int suffix_width_px = prv_codepoint_get_horizontal_advance(&ctx->font_cache,
        text_box_params->font, HYPHEN_CODEPOINT);
    int truncated_word_length_px = (line->width_px - suffix_width_px);
    PBL_ASSERTN(word->width_px >= truncated_word_length_px);
    word->width_px -= truncated_word_length_px;
    word->start = utf8_get_next(last_visited);

    return false;
  }

  if (!is_overflow) {
    // Add entire word
    PBL_ASSERTN(line->suffix_codepoint == 0);
    line->width_px += word->width_px;
    return true;
  }

  // Word-wrap
  word_trim_preceeding_whitespace(ctx, word, text_box_params);
  return false;
}

static void prv_line_justify(Line* line, const TextBoxParams* const text_box_params) {
  PBL_ASSERTN(line->max_width_px >= line->width_px);

  int horiz_px_remaining = (line->max_width_px - line->width_px);

  // [RTL] in addition to left, right and center alignment, you want a "primary"
  // alignment that is left for LTR writing systems, and right for RTL.
  switch (text_box_params->alignment) {
    case GTextAlignmentCenter:
      line->origin.x = line->origin.x + (horiz_px_remaining / 2);
      break;
    case GTextAlignmentRight:
      line->origin.x = line->origin.x + horiz_px_remaining;
      break;
    case GTextAlignmentLeft:
      break;
  }
}

//! @return is_text_remaining
bool line_add_words(Line* line, Iterator* word_iter, LastLineCallback last_line_cb) {

  WordIterState* word_iter_state = (WordIterState*) word_iter->state;

  line->start = word_iter_state->current.start;

  bool is_text_remaining = (line->start != NULL);

  // PBL-22083 : max_width_px == 0 eats a character that should appear on next line
  while (is_text_remaining && line->max_width_px > 0) {
    Word next_word = word_iter_state->current;

    bool is_added = line_add_word(word_iter_state->ctx, line, &next_word,
                                  word_iter_state->text_box_params);

    if (!is_added) {
      word_iter_state->current = next_word;
      // Check if word was trimmed until the null termination
      if (next_word.start == NULL) {
        is_text_remaining = false;
      } else {
        is_text_remaining = true;
      }
      break;
    }

    is_text_remaining = iter_next(word_iter);
  }

  if (last_line_cb) {
    last_line_cb(word_iter_state->ctx, line, word_iter_state->text_box_params, is_text_remaining);
  }

  prv_line_justify(line, word_iter_state->text_box_params);

  return is_text_remaining;
}

static bool prv_text_layout_is_fresh(TextLayout* layout, GFont const font, const GRect box,
                                     const GTextOverflowMode overflow_mode,
                                     const GTextAlignment alignment, Codepoint text_hash) {
  PBL_ASSERTN(layout);

  if (text_hash != layout->hash) {
    return false;
  }

  if (!grect_equal(&box, &layout->box)) {
    return false;
  }

  if (overflow_mode != layout->overflow_mode) {
    return false;
  }

  if (alignment != layout->alignment) {
    return false;
  }

  if (font != layout->font) {
    return false;
  }

  return true;
}

static inline void prv_text_walk_lines(GContext* ctx, TextLayout* const layout,
                                       WalkLinesCallbacks* callbacks) {

  TextBoxParams *text_box = &ctx->text_draw_state.text_box;

  if (grect_is_empty(&text_box->box)) {
    return;
  }

  const Utf8Bounds *utf8_bounds = text_box->utf8_bounds;

  bool is_string_empty = (utf8_bounds->start == utf8_bounds->end);
  if (is_string_empty) {
    return;
  }

  const GTextOverflowMode overflow_mode = text_box->overflow_mode;
  bool is_ellipsis_on_overflow = (overflow_mode == GTextOverflowModeTrailingEllipsis ||
                                  overflow_mode == GTextOverflowModeFill);
  if (is_ellipsis_on_overflow) {
    callbacks->last_line_cb = set_ellipsis_on_overflow_last_line_cb;
  } else {
    callbacks->last_line_cb = NULL;
  }

  ctx->text_draw_state.line = (Line) {
    .start = utf8_bounds->start,
    // set initial bounding values for line
    .origin = text_box->box.origin, //<! Needs to be in global co-ords!
    .max_width_px = text_box->box.size.w,
    .height_px = fonts_get_font_height(text_box->font)
  };

  Iterator line_iter;
  line_iter_init(&line_iter, &ctx->text_draw_state.line_iter_state, ctx);

  prv_walk_lines_down(&line_iter, layout, callbacks);
}

static void prv_graphics_text_layout_update(GContext* ctx, const char* text, GFont const font,
                                            const GRect box, const GTextOverflowMode overflow_mode,
                                            const GTextAlignment alignment,
                                            TextLayout* const layout) {
  PBL_ASSERTN(layout);

  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, text);
  if (!success) {
    layout->max_used_size = GSizeZero;
    PBL_LOG(LOG_LEVEL_DEBUG, "Invalid UTF8");
    return;
  }

  int str_len_bytes = (utf8_bounds.end - utf8_bounds.start);
  Codepoint text_hash = hash((const uint8_t*) utf8_bounds.start, str_len_bytes);

  if (prv_text_layout_is_fresh(layout, font, box, overflow_mode, alignment, text_hash)) {
    return;
  }

  layout->max_used_size = GSizeZero;
  layout->hash = text_hash;
  layout->box = box;
  layout->overflow_mode = overflow_mode;
  layout->alignment = alignment;
  layout->font = font;

  WalkLinesCallbacks callbacks = {
    .layout_update_cb = update_all_layout_update_cb
  };

  int16_t line_spacing_delta = prv_layout_get_line_spacing_delta(layout);
  ctx->text_draw_state.text_box = (TextBoxParams) {
    .utf8_bounds = &utf8_bounds,
    .box = box,
    .font = font,
    .overflow_mode = overflow_mode,
    .alignment = alignment,
    .line_spacing_delta = line_spacing_delta,
  };

  prv_text_walk_lines(ctx, layout, &callbacks);
}

// helper macro to avoid source code duplication
// we call this instead of a true function to keep the stack as low as possible as this is
// on a critical path.
#define APP_TEXT_GET_CONTENT_SIZE(text, font, box, overflow_mode, alignment, text_attributes) \
  do { \
    GContext* ctx = app_state_get_graphics_context(); \
    return graphics_text_layout_get_max_used_size( \
      ctx, text, font, box, overflow_mode, alignment, text_attributes); \
  } while (0)

GSize app_graphics_text_layout_get_content_size_with_attributes(
  const char *text, GFont const font, const GRect box, const GTextOverflowMode overflow_mode,
  const GTextAlignment alignment, GTextAttributes *text_attributes) {
  APP_TEXT_GET_CONTENT_SIZE(text, font, box, overflow_mode, alignment, text_attributes);
}


GSize app_graphics_text_layout_get_content_size(const char *text, GFont const font, const GRect box,
                                                const GTextOverflowMode overflow_mode,
                                                const GTextAlignment alignment) {
  APP_TEXT_GET_CONTENT_SIZE(text, font, box, overflow_mode, alignment, NULL);
}

uint16_t graphics_text_layout_get_text_height(GContext *ctx, const char *text, GFont const font,
                                              uint16_t bounds_width,
                                              const GTextOverflowMode overflow_mode,
                                              const GTextAlignment alignment) {
  const int16_t LAYOUT_HEIGHT_IGNORE = SHRT_MAX;
  GRect box = {
        .origin = (GPoint) { .x = 0, .y = 0 },
        .size = (GSize) { .w = bounds_width, .h = LAYOUT_HEIGHT_IGNORE }
      };
  GSize size = graphics_text_layout_get_max_used_size(ctx, text, font,
      box, overflow_mode, alignment, NULL);
  return size.h;
}

GSize graphics_text_layout_get_max_used_size(GContext *ctx, const char *text, GFont const font,
                                             const GRect box, const GTextOverflowMode overflow_mode,
                                             const GTextAlignment alignment,
                                             GTextLayoutCacheRef const layout) {
  TextLayoutExtended stack_layout = { 0 }; // Default use extended layout
  TextLayout* text_layout = layout ? (TextLayout*) layout : (TextLayout*) &stack_layout;
  prv_graphics_text_layout_update(ctx, text, font, box, overflow_mode, alignment, text_layout);
  return text_layout->max_used_size;
}

void graphics_draw_text(GContext* ctx, const char* text, GFont const font,
                        GRect box, const GTextOverflowMode overflow_mode,
                        const GTextAlignment alignment, GTextLayoutCacheRef const layout) {
  if (ctx->lock) {
    return;
  }

  bool success = false;
  const Utf8Bounds utf8_bounds = utf8_get_bounds(&success, text);
  if (!success) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Invalid UTF8");
    return;
  }

  GRect global_box = grect_to_global_coordinates(box, ctx);

  GRect temp_box = global_box;
  grect_clip(&temp_box, &ctx->draw_state.clip_box);
  if (temp_box.size.h <= 0) {
    // the text is not ever going to make it on screen. Bail early.
    return;
  }


  if (layout) {
    layout->box.origin = global_box.origin;
  }

  WalkLinesCallbacks callbacks = {
    .render_line_cb = render_all_render_line_cb,
    .layout_update_cb = update_all_layout_update_cb,
    .stop_condition_cb = is_clip_box_overflow_bottom_stop_condition_cb
  };

  int16_t line_spacing_delta = prv_layout_get_line_spacing_delta(layout);
  ctx->text_draw_state.text_box = (TextBoxParams) {
    .utf8_bounds = &utf8_bounds,
    .box = global_box,
    .font = font,
    .overflow_mode = overflow_mode,
    .alignment = alignment,
    .line_spacing_delta = line_spacing_delta,
  };

  prv_text_walk_lines(ctx, layout, &callbacks);
}

void graphics_text_layout_cache_init(GTextLayoutCacheRef* layout) {
  if (process_manager_compiled_with_legacy2_sdk()) {
    *layout = applib_type_malloc(TextLayout);
    *((TextLayout*) *layout) = (TextLayout) { 0 };
  } else {
    *layout = applib_type_malloc(TextLayoutExtended);
    *((TextLayoutExtended*) *layout) = (TextLayoutExtended) { 0 };
  }
}

void graphics_text_layout_cache_deinit(GTextLayoutCacheRef* layout) {
  TextLayout* text_layout = (TextLayout*) *layout;
  applib_free(text_layout);
  *layout = NULL;
}

GTextAttributes *graphics_text_attributes_create(void) {
  GTextAttributes *result;
  graphics_text_layout_cache_init(&result);
  return result;
}

void graphics_text_attributes_destroy(GTextAttributes *text_attributes) {
  if (!text_attributes) {
    return;
  }

  graphics_text_layout_cache_deinit(&text_attributes);
}


static TextLayoutExtended* prv_get_writable_extended_layout(GTextLayoutCacheRef layout) {
  PBL_ASSERTN(!process_manager_compiled_with_legacy2_sdk()); // should not get here if 2.X
  PBL_ASSERTN(layout);
  // Invalidate the hash to ensure the layout gets updated when prv_graphics_text_layout_update is
  // called on the layout
  layout->hash = 0;
  return (TextLayoutExtended *)layout;
}

static TextLayoutExtended* prv_get_readable_extended_layout(GTextLayoutCacheRef layout) {
  if (!layout || process_manager_compiled_with_legacy2_sdk()) {
    return NULL;
  }
  return (TextLayoutExtended*)layout;
}


void graphics_text_layout_set_line_spacing_delta(GTextLayoutCacheRef layout, int16_t delta) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (extended) {
    extended->line_spacing_delta = delta;
  }
}

int16_t graphics_text_layout_get_line_spacing_delta(const GTextLayoutCacheRef layout) {
  return prv_layout_get_line_spacing_delta(layout);
}

void graphics_text_attributes_restore_default_text_flow(GTextLayoutCacheRef layout) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (!extended) {
    return;
  }
  extended->flow_data.perimeter.impl = NULL;
}

// this way, we don't need to pull in all the dependencies when doing unit-tests
// if you want to test this aspect, just define the symbol below in your test_*.c file
#if !defined(UNITTEST)
#define USE_DISPLAY_PERIMETER_ON_FONT_LAYOUT
#endif

void graphics_text_attributes_enable_screen_text_flow(GTextLayoutCacheRef layout, uint8_t inset) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (!extended) {
    return;
  }

#if defined(USE_DISPLAY_PERIMETER_ON_FONT_LAYOUT)
  // on rectangular screens, we can just leave the perimeter blank when we don't need an inset
  const GPerimeter *shortcut_perimeter = PBL_IF_ROUND_ELSE(g_perimeter_for_display, NULL);
  const GPerimeter *perimeter = inset > 0 ? g_perimeter_for_display : shortcut_perimeter;
#else
  const GPerimeter *perimeter = NULL;
#endif

  extended->flow_data.perimeter = (TextLayoutFlowDataPerimeter) {
    .impl = perimeter,
    .inset = inset,
  };
}

void graphics_text_attributes_restore_default_paging(GTextLayoutCacheRef layout) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (!extended) {
    return;
  }
  extended->flow_data.paging.page_on_screen.size_h = 0;
}

void graphics_text_attributes_enable_paging(
  GTextLayoutCacheRef layout, GPoint content_origin_on_screen, GRect paging_on_screen) {
  TextLayoutExtended *extended = prv_get_writable_extended_layout(layout);
  if (extended) {
    extended->flow_data.paging = (TextLayoutFlowDataPaging) {
      .origin_on_screen = content_origin_on_screen,
      .page_on_screen.origin_y = paging_on_screen.origin.y,
      .page_on_screen.size_h = paging_on_screen.size.h,
    };
  }
}

const TextLayoutFlowData *graphics_text_layout_get_flow_data(GTextLayoutCacheRef layout) {
  TextLayoutExtended *extended_layout = prv_get_readable_extended_layout(layout);
  if (extended_layout) {
    return &extended_layout->flow_data;
  } else {
    static const TextLayoutFlowData s_default_data = {
      // yes, this is basically just an empty struct but I want to be explicit here:
      .perimeter.impl = NULL, // no perimeter/inset configured
      .paging.page_on_screen.size_h = 0, // no paging or origin
    };
    return &s_default_data;
  }
}
