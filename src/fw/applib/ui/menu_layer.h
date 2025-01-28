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

#include "inverter_layer.h"
#include "menu_cell_layer.h"
#include "scroll_layer.h"

#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/text.h"

#include <stdint.h>
#include <stddef.h>

//! @file menu_layer.h
//! @addtogroup UI
//! @{
//!   @addtogroup Layer Layers
//!   @{
//!     @addtogroup MenuLayer
//! \brief Layer that displays a standard list menu. Data is provided using
//! callbacks.
//!
//! ![](menu_layer.png)
//! <h3>Key Points</h3>
//! * The familiar list-style menu widget, as used throughout the Pebble user
//! interface.
//! * Built on top of \ref ScrollLayer, inheriting all its goodness like
//! animated scrolling, automatic "more content" shadow indicators, etc.
//! * All data needed to render the menu is requested on-demand via callbacks,
//! to avoid the need to keep a lot of data in memory.
//! * Support for "sections". A section is a group of items, visually separated
//! by a header with the name at the top of the section.
//! * Variable heights: each menu item cell and each section header can have
//! its own height. The heights are provided by callbacks.
//! * Deviation from the Layer system for cell drawing: Each menu item does
//! _not_ have its own Layer (to minimize memory usage). Instead, a
//! drawing callback is set onto the \ref MenuLayer that is responsible
//! for drawing each menu item. The \ref MenuLayer will call this callback for each
//! menu item that is visible and needs to be rendered.
//! * Cell and header drawing can be customized by implementing a custom drawing
//! callback.
//! * A few "canned" menu cell drawing functions are provided for convenience,
//! which support the default menu cell layout with a title, optional subtitle
//! and icon.
//!
//! For short, static list menus, consider using \ref SimpleMenuLayer.
//!     @{

//! @internal
//! Constant to indicate that a menu item index is not found
#define MENU_INDEX_NOT_FOUND ((const uint16_t) ~0)

//////////////////////
// Menu Layer

struct MenuLayer;

//! Data structure to represent an menu item's position in a menu, by specifying
//! the section index and the row index within that section.
typedef struct MenuIndex {
  //! The index of the section
  uint16_t section;
  //! The index of the row within the section with index `.section`
  uint16_t row;
} MenuIndex;

//! Macro to create a MenuIndex
#define MenuIndex(section, row) ((MenuIndex){ (section), (row) })

//! Comparator function to determine the order of two MenuIndex values.
//! @param a Pointer to the menu index of the first item
//! @param b Pointer to the menu index of the second item
//! @return 0 if A and B are equal, 1 if A has a higher section & row
//! combination than B or else -1
int16_t menu_index_compare(const MenuIndex *a, const MenuIndex *b);

//! @internal
//! Data structure with geometric information of a cell at specific menu index.
//! This is used internally for caching.
typedef struct MenuCellSpan {
  int16_t y;
  int16_t h;
  int16_t sep;
  MenuIndex index;
} MenuCellSpan;

//! Function signature for the callback to get the number of sections in a menu.
//! @param menu_layer The \ref MenuLayer for which the data is requested
//! @param callback_context The callback context
//! @return The number of sections in the menu
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef uint16_t (*MenuLayerGetNumberOfSectionsCallback)(struct MenuLayer *menu_layer,
                                                         void *callback_context);

//! Function signature for the callback to get the number of rows in a
//! given section in a menu.
//! @param menu_layer The \ref MenuLayer for which the data is requested
//! @param section_index The index of the section of the menu for which the
//! number of items it contains is requested
//! @param callback_context The callback context
//! @return The number of rows in the given section in the menu
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef uint16_t (*MenuLayerGetNumberOfRowsInSectionsCallback)(struct MenuLayer *menu_layer,
                                                               uint16_t section_index,
                                                               void *callback_context);

//! Function signature for the callback to get the height of the menu cell
//! at a given index.
//! @param menu_layer The \ref MenuLayer for which the data is requested
//! @param cell_index The MenuIndex for which the cell height is requested
//! @param callback_context The callback context
//! @return The height of the cell at the given MenuIndex
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef int16_t (*MenuLayerGetCellHeightCallback)(struct MenuLayer *menu_layer,
                                                  MenuIndex *cell_index,
                                                  void *callback_context);

//! Function signature for the callback to get the height of the section header
//! at a given section index.
//! @param menu_layer The \ref MenuLayer for which the data is requested
//! @param section_index The index of the section for which the header height is
//! requested
//! @param callback_context The callback context
//! @return The height of the section header at the given section index
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef int16_t (*MenuLayerGetHeaderHeightCallback)(struct MenuLayer *menu_layer,
                                                    uint16_t section_index,
                                                    void *callback_context);

//! Function signature for the callback to get the height of the separator
//! at a given index.
//! @param menu_layer The \ref MenuLayer for which the data is requested
//! @param cell_index The MenuIndex for which the cell height is requested
//! @param callback_context The callback context
//! @return The height of the separator at the given MenuIndex
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef int16_t (*MenuLayerGetSeparatorHeightCallback)(struct MenuLayer *menu_layer,
                                                       MenuIndex *cell_index,
                                                       void *callback_context);

//! Function signature for the callback to render the menu cell at a given
//! MenuIndex.
//! @param ctx The destination graphics context to draw into
//! @param cell_layer The cell's layer, containing the geometry of the cell
//! @param cell_index The MenuIndex of the cell that needs to be drawn
//! @param callback_context The callback context
//! @note The `cell_layer` argument is provided to make it easy to re-use an
//! `.update_proc` implementation in this callback. Only the bounds and frame
//! of the `cell_layer` are actually valid and other properties should be
//! ignored.
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef void (*MenuLayerDrawRowCallback)(GContext* ctx,
                                         const Layer *cell_layer,
                                         MenuIndex *cell_index,
                                         void *callback_context);

//! Function signature for the callback to render the section header at a given
//! section index.
//! @param ctx The destination graphics context to draw into
//! @param cell_layer The header cell's layer, containing the geometry of the
//! header cell
//! @param section_index The section index of the section header that needs to
//! be drawn
//! @param callback_context The callback context
//! @note The `cell_layer` argument is provided to make it easy to re-use an
//! `.update_proc` implementation in this callback. Only the bounds and frame
//! of the `cell_layer` are actually valid and other properties should be
//! ignored.
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef void (*MenuLayerDrawHeaderCallback)(GContext* ctx,
                                            const Layer *cell_layer,
                                            uint16_t section_index,
                                            void *callback_context);

//! Function signature for the callback to render the separator at a given
//! MenuIndex.
//! @param ctx The destination graphics context to draw into
//! @param cell_layer The cell's layer, containing the geometry of the cell
//! @param cell_index The MenuIndex of the separator that needs to be drawn
//! @param callback_context The callback context
//! @note The `cell_layer` argument is provided to make it easy to re-use an
//! `.update_proc` implementation in this callback. Only the bounds and frame
//! of the `cell_layer` are actually valid and other properties should be
//! ignored.
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef void (*MenuLayerDrawSeparatorCallback)(GContext* ctx,
                                               const Layer *cell_layer,
                                               MenuIndex *cell_index,
                                               void *callback_context);

//! Function signature for the callback to handle the event that a user hits
//! the SELECT button.
//! @param menu_layer The \ref MenuLayer for which the selection event occured
//! @param cell_index The MenuIndex of the cell that is selected
//! @param callback_context The callback context
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef void (*MenuLayerSelectCallback)(struct MenuLayer *menu_layer,
                                        MenuIndex *cell_index,
                                        void *callback_context);

//! Function signature for the callback to handle a change in the current
//! selected item in the menu.
//! @param menu_layer The \ref MenuLayer for which the selection event occured
//! @param new_index The MenuIndex of the new item that is selected now
//! @param old_index The MenuIndex of the old item that was selected before
//! @param callback_context The callback context
//! @see \ref menu_layer_set_callbacks()
//! @see \ref MenuLayerCallbacks
typedef void (*MenuLayerSelectionChangedCallback)(struct MenuLayer *menu_layer,
                                                  MenuIndex new_index,
                                                  MenuIndex old_index,
                                                  void *callback_context);

//! Function signature for the callback which allows or changes selection behavior of the menu.
//! In order to change the cell that should be selected, modify the passed in new_index.
//! Preventing the selection from changing, new_index can be assigned the value of old_index.
//! @param menu_layer The \ref MenuLayer for which the selection event that occured
//! @param new_index Pointer to the index that the MenuLayer is going to change selection to.
//! @param old_index The index that is being unselected.
//! @param callback_context The callback context
//! @note \ref menu_layer_set_selected_index will not trigger this callback when
//! the selection changes, but \ref menu_layer_set_selected_next will.
typedef void (*MenuLayerSelectionWillChangeCallback)(struct MenuLayer *menu_layer,
                                                     MenuIndex *new_index,
                                                     MenuIndex old_index,
                                                     void *callback_context);

//! Function signature for the callback which draws the menu's background.
//! The background is underneath the cells of the menu, and is visible in the
//! padding below the bottom cell, or if a cell's background color is set to \ref GColorClear.
//! @param ctx The destination graphics context to draw into.
//! @param bg_layer The background's layer, containing the geometry of the background.
//! @param highlight Whether this should be rendered as highlighted or not. Highlight style
//! should match the highlight style of cells, since this color can be used for animating selection.
typedef void (*MenuLayerDrawBackgroundCallback)(GContext* ctx,
                                                const Layer *bg_layer,
                                                bool highlight,
                                                void *callback_context);

//! Data structure containing all the callbacks of a \ref MenuLayer.
typedef struct MenuLayerCallbacks {
  //! Callback that gets called to get the number of sections in the menu.
  //! This can get called at various moments throughout the life of a menu.
  //! @note When `NULL`, the number of sections defaults to 1.
  MenuLayerGetNumberOfSectionsCallback get_num_sections;

  //! Callback that gets called to get the number of rows in a section. This
  //! can get called at various moments throughout the life of a menu.
  //! @note Must be set to a valid callback; `NULL` causes undefined behavior.
  MenuLayerGetNumberOfRowsInSectionsCallback get_num_rows;

  //! Callback that gets called to get the height of a cell.
  //! This can get called at various moments throughout the life of a menu.
  //! @note When `NULL`, the default height of \ref MENU_CELL_BASIC_CELL_HEIGHT pixels is used.
  //! Developers may wish to use \ref MENU_CELL_ROUND_FOCUSED_SHORT_CELL_HEIGHT
  //! and \ref MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT on a round display
  //! to respect the system aesthetic.
  MenuLayerGetCellHeightCallback get_cell_height;

  //! Callback that gets called to get the height of a section header.
  //! This can get called at various moments throughout the life of a menu.
  //! @note When `NULL`, the default height of 0 pixels is used. This disables
  //! section headers.
  MenuLayerGetHeaderHeightCallback get_header_height;

  //! Callback that gets called to render a menu item.
  //! This gets called for each menu item, every time it needs to be
  //! re-rendered.
  //! @note Must be set to a valid callback; `NULL` causes undefined behavior.
  MenuLayerDrawRowCallback draw_row;

  //! Callback that gets called to render a section header.
  //! This gets called for each section header, every time it needs to be
  //! re-rendered.
  //! @note Must be set to a valid callback, unless `.get_header_height` is
  //! `NULL`. Causes undefined behavior otherwise.
  MenuLayerDrawHeaderCallback draw_header;

  //! Callback that gets called when the user triggers a click with the SELECT
  //! button.
  //! @note When `NULL`, click events for the SELECT button are ignored.
  MenuLayerSelectCallback select_click;

  //! Callback that gets called when the user triggers a long click with the
  //! SELECT button.
  //! @note When `NULL`, long click events for the SELECT button are ignored.
  MenuLayerSelectCallback select_long_click;

  //! Callback that gets called whenever the selection changes.
  //! @note When `NULL`, selection change events are ignored.
  MenuLayerSelectionChangedCallback selection_changed;

  //! Callback that gets called to get the height of a separator
  //! This can get called at various moments throughout the life of a menu.
  //! @note When `NULL`, the default height of 0 is used.
  MenuLayerGetSeparatorHeightCallback get_separator_height;

  //! Callback that gets called to render a separator.
  //! This gets called for each separator, every time it needs to be
  //! re-rendered.
  //! @note Must be set to a valid callback, unless `.get_separator_height` is
  //! `NULL`. Causes undefined behavior otherwise.
  MenuLayerDrawSeparatorCallback draw_separator;

  //! Callback that gets called before the selected cell changes.
  //! This gets called before the selected item in the MenuLayer is changed,
  //! and will allow for the selected cell to be overridden.
  //! This allows for skipping cells in the menu, locking selection onto a given item,
  MenuLayerSelectionWillChangeCallback selection_will_change;

  //! Callback that gets called before any cells are drawn.
  //! This supports two states, either highlighted or not highlighted.
  //! If highlighted is specified, it is expected to be colored in the same
  //! style as the menu's cells are.
  //! If this callback is not specified, it will default to the colors set with
  //! \ref menu_layer_set_normal_colors and \ref menu_layer_set_highlight_colors.
  MenuLayerDrawBackgroundCallback draw_background;
} MenuLayerCallbacks;

enum {
  MenuLayerColorBackground = 0,
  MenuLayerColorForeground,
  MenuLayerColor_Count,
};
#ifndef __clang__
_Static_assert(MenuLayerColor_Count == 2, "Bad enum MenuLayerColor");
#endif

//! Data structure of a MenuLayer.
//! @note a `MenuLayer *` can safely be casted to a `Layer *` and
//! `ScrollLayer *` and can thus be used with all other functions that take a
//! `Layer *` or `ScrollLayer *`, respectively, as an argument.
//! <br/>For example, the following is legal:
//! \code{.c}
//! MenuLayer menu_layer;
//! ...
//! layer_set_hidden((Layer *)&menu_layer, true);
//! \endcode
//! @note However there are a few caveats:
//! * Do not try to change to bounds or frame of a \ref MenuLayer, after
//! initializing it.
typedef struct MenuLayer {
  ScrollLayer scroll_layer;
  InverterLayer inverter;
  //! @internal
  struct {
    //! @internal
    //! Cell index + geometry cache of a cell that was in frame during the last redraw
    MenuCellSpan cursor;
  } cache;
  //! @internal
  //! Selected cell index + geometry cache of the selected cell
  MenuCellSpan selection;
  MenuLayerCallbacks callbacks;
  void *callback_context;

  //! Default colors to be used for \ref MenuLayer.
  //! Use MenuLayerColorNormal and MenuLayerColorHighlight for indexing.
  GColor normal_colors[MenuLayerColor_Count];
  GColor highlight_colors[MenuLayerColor_Count];

  //! Animation used for selection. Note this is only used in 3.x+ apps, legacy2 apps don't
  //! use this.
  struct {
    Animation *animation;
    GRect target; //! The target frame of the animation
    //! cell_layer's bounds.origin will be modified by this to allow for
    //! content scrolling without scrolling the actual cells
    int16_t cell_content_origin_offset_y;
    //! used to express "bouncing" of the highlight
    int16_t selection_extend_top;
    //! same as selection_extend_top but for the bottom
    int16_t selection_extend_bottom;
    //! some animations (e.g. center focused) will use this field to postpone the update of
    //! menulayer.selection (especially for the .index)
    MenuCellSpan new_selection;
  } animation;

  //! @internal
  //! If true, there will be padding after the bottom item in the menu
  //! Defaults to 'true'
  bool pad_bottom:1;

  //! If true, the MenuLayer will generally scroll the content so that the selected row is
  //! on the center of the screen.
  bool center_focused:1;

  //! If true, the MenuLayer will not perform the selection cell clipping animation. This is
  //! independent of the scrolling animation.
  bool selection_animation_disabled:1;

  //! Add some padding to keep track of the \ref MenuLayer size budget.
  //! As long as the size stays within this budget, 2.x apps can safely use the 3.x MenuLayer type.
  //! When padding is removed, the assertion below should also be removed.
  uint8_t padding[44];
} MenuLayer;

//! Padding used below the last item in pixels
#define MENU_LAYER_BOTTOM_PADDING 20

//! Initializes a \ref MenuLayer with given frame
//! All previous contents are erased and the following default values are set:
//! * Clips: `true`
//! * Hidden: `false`
//! * Content size: `frame.size`
//! * Content offset: \ref GPointZero
//! * Callbacks: None (`NULL` for each one)
//! * Callback context: `NULL`
//! * After the relevant callbacks are called to populate the menu, the item at MenuIndex(0, 0)
//!   will be selected initially.
//! The layer is marked dirty automatically.
//! @param menu_layer The \ref MenuLayer to initialize
//! @param frame The frame with which to initialze the \ref MenuLayer
void menu_layer_init(MenuLayer *menu_layer, const GRect *frame);

//! Creates a new \ref MenuLayer on the heap and initalizes it with the default values.
//!
//! * Clips: `true`
//! * Hidden: `false`
//! * Content size: `frame.size`
//! * Content offset: \ref GPointZero
//! * Callbacks: None (`NULL` for each one)
//! * Callback context: `NULL`
//! * After the relevant callbacks are called to populate the menu, the item at MenuIndex(0, 0)
//!   will be selected initially.
//! @return A pointer to the \ref MenuLayer. `NULL` if the \ref MenuLayer could not
//! be created
MenuLayer* menu_layer_create(GRect frame);

void menu_layer_deinit(MenuLayer* menu_layer);

//! Destroys a \ref MenuLayer previously created by menu_layer_create.
void menu_layer_destroy(MenuLayer* menu_layer);

//! Gets the "root" Layer of the \ref MenuLayer, which is the parent for the sub-
//! layers used for its implementation.
//! @param menu_layer Pointer to the MenuLayer for which to get the "root" Layer
//! @return The "root" Layer of the \ref MenuLayer.
//! @internal
//! @note The result is always equal to `(Layer *) menu_layer`.
Layer* menu_layer_get_layer(const MenuLayer *menu_layer);

//! Gets the ScrollLayer of the \ref MenuLayer, which is the layer responsible for
//! the scrolling of the \ref MenuLayer.
//! @param menu_layer Pointer to the \ref MenuLayer for which to get the ScrollLayer
//! @return The ScrollLayer of the \ref MenuLayer.
//! @internal
//! @note The result is always equal to `(ScrollLayer *) menu_layer`.
ScrollLayer* menu_layer_get_scroll_layer(const MenuLayer *menu_layer);

//! @internal
//! This function replaces \ref menu_layer_set_callbacks_by_value in order to change the callbacks
//! parameter to be passed by a pointer instead of being passed by value. Callers consume much less
//! code space when passing a pointer compared to passing structs by value.
//! @see menu_layer_set_callbacks_by_value
void menu_layer_set_callbacks(MenuLayer *menu_layer,
                              void *callback_context,
                              const MenuLayerCallbacks *callbacks);

//! Sets the callbacks for the MenuLayer.
//! @param menu_layer Pointer to the \ref MenuLayer for which to set the callbacks
//! and callback context.
//! @param callback_context The new callback context. This is passed into each
//! of the callbacks and can be set to point to application provided data.
//! @param callbacks The new callbacks for the \ref MenuLayer. The storage for this
//! data structure must be long lived. Therefore, it cannot be stack-allocated.
//! @see MenuLayerCallbacks
void menu_layer_set_callbacks_by_value(MenuLayer *menu_layer, void *callback_context,
                                       MenuLayerCallbacks callbacks);

//! Convenience function to set the \ref ClickConfigProvider callback on the
//! given window to the \ref MenuLayer internal click config provider. This internal
//! click configuration provider, will set up the default UP & DOWN
//! scrolling / menu item selection behavior.
//! This function calls \ref scroll_layer_set_click_config_onto_window to
//! accomplish this.
//!
//! Click and long click events for the SELECT button can be handled by
//! installing the appropriate callbacks using \ref menu_layer_set_callbacks().
//! This is a deviation from the usual click configuration provider pattern.
//! @param menu_layer The \ref MenuLayer that needs to receive click events.
//! @param window The window for which to set the click configuration.
//! @see \ref Clicks
//! @see \ref window_set_click_config_provider_with_context()
//! @see \ref scroll_layer_set_click_config_onto_window()
void menu_layer_set_click_config_onto_window(MenuLayer *menu_layer,
                                             struct Window *window);

//! This enables or disables padding at the bottom of the \ref MenuLayer.
//! Padding at the bottom of the layer keeps the bottom item from being at the very bottom of the
//! screen.
//! Padding is turned on by default for all MenuLayers.
//! The color of the padded area will be the background color set using
//! \ref menu_layer_set_normal_colors(). To color the padding a different color, use
//! \ref MenuLayerDrawBackgroundCallback.
//! @param menu_layer The menu layer for which to enable or disable the padding.
//! @param enable True = enable padding, False = disable padding.
void menu_layer_pad_bottom_enable(MenuLayer *menu_layer, bool enable);

//! Values to specify how a (selected) row should be aligned relative to the
//! visible area of the \ref MenuLayer.
typedef enum {
  //! Don't align or update the scroll offset of the \ref MenuLayer.
  MenuRowAlignNone,

  //! Scroll the contents of the \ref MenuLayer in such way that the selected row
  //! is centered relative to the visible area.
  MenuRowAlignCenter,

  //! Scroll the contents of the \ref MenuLayer in such way that the selected row
  //! is at the top of the visible area.
  MenuRowAlignTop,

  //! Scroll the contents of the \ref MenuLayer in such way that the selected row
  //! is at the bottom of the visible area.
  MenuRowAlignBottom,
} MenuRowAlign;

//! Selects the next or previous item, relative to the current selection.
//! @param menu_layer The \ref MenuLayer for which to select the next item
//! @param up Supply `false` to select the next item in the list (downwards),
//! or `true` to select the previous item in the list (upwards).
//! @param scroll_align The alignment of the new selection
//! @param animated Supply `true` to animate changing the selection, or `false`
//! to change the selection instantly.
//! @note If there is no next/previous item, this function is a no-op.
void menu_layer_set_selected_next(MenuLayer *menu_layer,
                                  bool up,
                                  MenuRowAlign scroll_align,
                                  bool animated);

//! Selects the item with given \ref MenuIndex.
//! @param menu_layer The \ref MenuLayer for which to change the selection
//! @param index The index of the item to select
//! @param scroll_align The alignment of the new selection
//! @param animated Supply `true` to animate changing the selection, or `false`
//! to change the selection instantly.
//! @note If the section and/or row index exceeds the available number of sections
//! or resp. rows, the exceeding index/indices will be capped, effectively
//! selecting the last section and/or row, resp.
void menu_layer_set_selected_index(MenuLayer *menu_layer,
                                   MenuIndex index, MenuRowAlign scroll_align,
                                   bool animated);

//! Gets the MenuIndex of the currently selected menu item.
//! @param menu_layer The \ref MenuLayer for which to get the current selected index.
//! @see menu_cell_layer_is_highlighted
//! @note This function should not be used to determine whether a cell should be
//! highlighted or not. See \ref menu_cell_layer_is_highlighted for more
//! information.
MenuIndex menu_layer_get_selected_index(const MenuLayer *menu_layer);

//! Returns whether or not the specified cell index is currently selected.
//! @param menu_layer The \ref MenuLayer to use when determining if the index is selected.
//! @param index The \ref MenuIndex of the cell to check for selection.
//! @note This function should not be used to determine whether a cell is highlighted or not.
//! See \ref menu_cell_layer_is_highlighted for more information.
bool menu_layer_is_index_selected(const MenuLayer *menu_layer, MenuIndex *index);

//! Reloads the data of the menu. This causes the menu to re-request the menu
//! item data, by calling the relevant callbacks.
//! The current selection and scroll position will not be changed. See the
//! note with \ref menu_layer_set_selected_index() for the behavior if the
//! old selection is no longer valid.
//! @param menu_layer The \ref MenuLayer for which to reload the data.
void menu_layer_reload_data(MenuLayer *menu_layer);

//! Set the default colors to be used for cells when it is in a normal state (not highlighted).
//! The GContext's text and fill colors will be set appropriately prior to calling the `.draw_row`
//! callback.
//! If this function is not explicitly called on a \ref MenuLayer, it will default to white
//! background with black foreground.
//! @param menu_layer The \ref MenuLayer for which to set the colors.
//! @param background The color to be used for the background of the cell.
//! @param foreground The color to be used for the foreground and text of the cell.
//! @see \ref menu_layer_set_highlight_colors
void menu_layer_set_normal_colors(MenuLayer *menu_layer, GColor background, GColor foreground);

//! Set the default colors to be used for cells when it is in a highlighted state.
//! The GContext's text and fill colors will be set appropriately prior to calling the `.draw_row`
//! callback.
//! If this function is not explicitly called on a \ref MenuLayer, it will default to black
//! background with white foreground.
//! @param menu_layer The \ref MenuLayer for which to set the colors.
//! @param background The color to be used for the background of the cell.
//! @param foreground The color to be used for the foreground and text of the cell.
//! @see \ref menu_layer_set_normal_colors
void menu_layer_set_highlight_colors(MenuLayer *menu_layer, GColor background, GColor foreground);


//! True, if the \ref MenuLayer generally scrolls such that the selected row is in the center.
//! @see \ref menu_layer_set_center_focused
bool menu_layer_get_center_focused(MenuLayer *menu_layer);

//! Controls if the \ref MenuLayer generally scrolls such that the selected row is in the center.
//! For platforms with a round display (PBL_ROUND) the default is true,
//! otherwise false is the default
//! @param menu_layer The menu layer for which to enable or disable the behavior.
//! @param center_focused true = enable the mode, false = disable it.
//! @see \ref menu_layer_get_center_focused
void menu_layer_set_center_focused(MenuLayer *menu_layer, bool center_focused);


//!     @} // end addtogroup MenuLayer
//!   @} // end addtogroup Layer
//! @} // end addtogroup UI

