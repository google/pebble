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

#pragma once

#include "click.h"
#include "layer.h"
#include "property_animation.h"
#include "applib/fonts/fonts.h"

#define SELECTION_LAYER_DEFAULT_CELL_HEIGHT PBL_IF_RECT_ELSE(34, 40)

#define MAX_SELECTION_LAYER_CELLS 3

typedef char* (*SelectionLayerGetCellText)(unsigned index, void *callback_context);

typedef void (*SelectionLayerCompleteCallback)(void *callback_context);

typedef void (*SelectionLayerIncrementCallback)(unsigned selected_cell_idx, void *callback_context);

typedef void (*SelectionLayerDecrementCallback)(unsigned selected_cell_idx, void *callback_context);

typedef struct SelectionLayerCallbacks {
  SelectionLayerGetCellText get_cell_text;
  SelectionLayerCompleteCallback complete;
  SelectionLayerIncrementCallback increment;
  SelectionLayerDecrementCallback decrement;
} SelectionLayerCallbacks;


typedef struct SelectionLayer {
  Layer layer;
  unsigned num_cells;
  unsigned cell_widths[MAX_SELECTION_LAYER_CELLS];
  unsigned cell_padding;
  unsigned selected_cell_idx;

  // If is_active = false the selected cell will become invalid, and any clicks will be ignored
  bool is_active;

  GFont font;
  GColor inactive_background_color;
  GColor active_background_color;

  SelectionLayerCallbacks callbacks;
  void *callback_context;

  // Animation stuff
  Animation *value_change_animation;
  bool bump_is_upwards;
  unsigned bump_text_anim_progress;
  AnimationImplementation bump_text_impl;
  unsigned bump_settle_anim_progress;
  AnimationImplementation bump_settle_anim_impl;

  Animation *next_cell_animation;
  unsigned slide_amin_progress;
  AnimationImplementation slide_amin_impl;
  unsigned slide_settle_anim_progress;
  AnimationImplementation slide_settle_anim_impl;
} SelectionLayer;



void selection_layer_init(SelectionLayer *selection_layer, const GRect *frame, unsigned num_cells);

SelectionLayer* selection_layer_create(GRect frame, unsigned num_cells);

void selection_layer_deinit(SelectionLayer* selection_layer);

void selection_layer_destroy(SelectionLayer* selection_layer);

void selection_layer_set_cell_width(SelectionLayer *selection_layer,
                                    unsigned cell_idx, unsigned width);

void selection_layer_set_font(SelectionLayer *selection_layer, GFont font);

void selection_layer_set_inactive_bg_color(SelectionLayer *selection_layer, GColor color);

void selection_layer_set_active_bg_color(SelectionLayer *selection_layer, GColor color);

void selection_layer_set_cell_padding(SelectionLayer *selection_layer, unsigned padding);

// When transitioning from inactive -> active, the selected cell will be index 0
void selection_layer_set_active(SelectionLayer *selection_layer, bool is_active);

void selection_layer_set_click_config_onto_window(SelectionLayer *selection_layer,
                                                  struct Window *window);

void selection_layer_set_callbacks(SelectionLayer *selection_layer,
                                   void *callback_context,
                                   SelectionLayerCallbacks callbacks);

int selection_layer_default_cell_height(void);
