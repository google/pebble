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
#include "applib/graphics/gtypes.h"
#include "applib/ui/recognizer/recognizer.h"
#include "applib/ui/recognizer/recognizer_list.h"

#include <stdbool.h>

struct Layer;
struct Animation;

//! How deep our layer tree is allowed to be.
#define LAYER_TREE_STACK_SIZE 16

//! @file layer.h
//! @addtogroup UI
//! @{
//!   @addtogroup Layer Layers
//! \brief User interface layers for displaying graphic components
//!
//! Layers are objects that can be displayed on a Pebble watchapp window, enabling users to see
//! visual objects, like text or images. Each layer stores the information about its state
//! necessary to draw or redraw the object that it represents and uses graphics routines along with
//! this state to draw itself when asked. Layers can be used to display various graphics.
//!
//! Layers are the basic building blocks for your application UI. Layers can be nested inside each other.
//! Every window has a root layer which is always the topmost layer.
//! You provide a function that is called to draw the content of the layer when needed; or
//! you can use standard layers that are provided by the system, such as text layer, image layer,
//! menu layer, action bar layer, and so on.
//!
//! The Pebble layer hierarchy is the list of things that need to be drawn to the screen.
//! Multiple layers can be arranged into a hierarchy. This enables ordering (front to back),
//! layout and hierarchy. Through relative positioning, visual objects that are grouped together by
//! adding them into the same layer can be moved all at once. This means that the child layers
//! will move accordingly. If a parent layer has clipping enabled, all the children will be clipped
//! to the frame of the parent.
//!
//! Pebble OS provides convenience layers with built-in logic for displaying different graphic
//! components, like text and bitmap layers.
//!
//! Refer to the \htmlinclude UiFramework.html (chapter "Layers") for a conceptual overview
//! of Layers and relevant code examples.
//!
//! The Modules listed here contain what can be thought of conceptually as subclasses of Layer. The
//! listed types can be safely type-casted to `Layer` (or `Layer *` in case of a pointer).
//! The `layer_...` functions can then be used with the data structures of these subclasses.
//! <br/>For example, the following is legal:
//! \code{.c}
//! TextLayer *text_layer;
//! ...
//! layer_set_hidden((Layer *)text_layer, true);
//! \endcode
//!   @{

//! Function signature for a Layer's render callback (the name of the type
//! is derived from the words 'update procedure').
//! The system will call the `.update_proc` callback whenever the Layer needs
//! to be rendered.
//! @param layer The layer that needs to be rendered
//! @param ctx The destination graphics context to draw into
//! @see \ref Graphics
//! @see \ref layer_set_update_proc()
typedef void (*LayerUpdateProc)(struct Layer *layer, GContext* ctx);

typedef void (*PropertyChangedProc)(struct Layer *layer);

//! Layer contains point override function. This can replace the default implementation of
//! \ref layer_contains_point using the \ref layer_set_contains_point_override call. The override
//! function should return true if the point should be deemed within the layer and false if not.
//! The point is relative to the frame origin of the layer
//! @param layer affected layer
//! @param point point relative to the frame origin of the layer
//! @return true if point should be considered to be contained within the layer
typedef bool (*LayerContainsPointOverride)(const struct Layer *layer, const GPoint *point);

//! Data structure of a Layer.
//! It contains the following:
//! * geometry (frame, bounds)
//! * clipping, hidden flags
//! * a reference to its window
//! * a reference to its render callback
//! * references that constitute the layer hierarchy
typedef struct Layer {
  /* Geometry */

  //! @internal
  //! Internal box bounds
  GRect bounds;

  //! @internal
  //! Box bounds relative to parent layer coordinates
  GRect frame;

  union {
    uint8_t flags;
    struct {
      bool clips:1;
      bool hidden:1;
      bool has_data:1;
      bool is_highlighted:1; //!< Indicates the highlight status of a \ref MenuLayer cell
    };
  };

  /* Layer tree */
  struct Layer *next_sibling;
  struct Layer *parent;
  struct Layer *first_child;

  struct Window *window;

  //! Drawing callback
  //! can be NULL if layer doesn't draw anything
  LayerUpdateProc update_proc;

  //! Property changed callback
  PropertyChangedProc property_changed_proc;

#if CAPABILITY_HAS_TOUCHSCREEN
  //! List of attached recognizers
  RecognizerList recognizer_list;

  //! Override callback to determine whether a layer contains a point
  LayerContainsPointOverride contains_point_override;
#endif
} Layer;

typedef struct DataLayer {
  Layer layer;

  uint8_t data[];
} DataLayer;

//! Initializes the given layer and sets its frame and bounds.
//! Default values:
//! * `bounds` : origin (0, 0) and a size equal to the frame that is passed in.
//! * `clips` : `true`
//! * `hidden` : `false`
//! * `update_proc` : `NULL` (draws nothing)
//! @param layer The layer to initialize
//! @param frame The frame at which the layer should be initialized.
//! @param data_size The size (in bytes) of memory to initialize after the layer struct.
//! @see \ref layer_set_frame()
//! @see \ref layer_set_bounds()
void layer_init(Layer *layer, const GRect *frame);

//! Creates a layer on the heap and sets its frame and bounds.
//! Default values:
//! * `bounds` : origin (0, 0) and a size equal to the frame that is passed in.
//! * `clips` : `true`
//! * `hidden` : `false`
//! * `update_proc` : `NULL` (draws nothing)
//! @param frame The frame at which the layer should be initialized.
//! @see \ref layer_set_frame()
//! @see \ref layer_set_bounds()
//! @return A pointer to the layer. `NULL` if the layer could not
//! be created
Layer* layer_create(GRect frame);

//! Creates a layer on the heap with extra space for callback data, and set its frame and bounds.
//! Default values:
//! * `bounds` : origin (0, 0) and a size equal to the frame that is passed in.
//! * `clips` : `true`
//! * `hidden` : `false`
//! * `update_proc` : `NULL` (draws nothing)
//! @param frame The frame at which the layer should be initialized.
//! @param data_size The size (in bytes) of memory to allocate for callback data.
//! @see \ref layer_create()
//! @see \ref layer_set_frame()
//! @see \ref layer_set_bounds()
//! @return A pointer to the layer. `NULL` if the layer could not be created
Layer* layer_create_with_data(GRect frame, size_t data_size);

void layer_deinit(Layer *layer);

//! Destroys a layer previously created by layer_create
void layer_destroy(Layer* layer);

//! @internal
//! Renders a tree of layers to a graphics context
void layer_render_tree(Layer *root, GContext *ctx);

//! @internal
//! Process the PropertyChangedProc callback for a tree of layers
void layer_property_changed_tree(Layer *root);

//! Marks the complete layer as "dirty", awaiting to be asked by the system to redraw itself.
//! Typically, this function is called whenever state has changed that affects what the layer
//! is displaying.
//! * The layer's `.update_proc` will not be called before this function returns,
//! but will be called asynchronously, shortly.
//! * Internally, a call to this function will schedule a re-render of the window that the
//! layer belongs to. In effect, all layers in that window's layer hierarchy will be asked to redraw.
//! * If an earlier re-render request is still pending, this function is a no-op.
//! @param layer The layer to mark dirty
void layer_mark_dirty(Layer *layer);

//! Sets the layer's render function.
//! The system will call the `update_proc` automatically when the layer needs to redraw itself, see
//! also \ref layer_mark_dirty().
//! @param layer Pointer to the layer structure.
//! @param update_proc Pointer to the function that will be called when the layer needs to be rendered.
//! Typically, one performs a series of drawing commands in the implementation of the `update_proc`,
//! see \ref Drawing, \ref PathDrawing and \ref TextDrawing.
void layer_set_update_proc(Layer *layer, LayerUpdateProc update_proc);

//! Sets the frame of the layer, which is it's bounding box relative to the coordinate
//! system of its parent layer.
//! The size of the layer's bounds will be extended automatically, so that the bounds
//! cover the new frame.
//! @param layer The layer for which to set the frame
//! @param frame The new frame
//! @see \ref layer_set_bounds()
void layer_set_frame_by_value(Layer *layer, GRect frame);
void layer_set_frame(Layer *layer, const GRect *frame);

//! Gets the frame of the layer, which is it's bounding box relative to the coordinate
//! system of its parent layer.
//! If the frame has changed, \ref layer_mark_dirty() will be called automatically.
//! @param layer The layer for which to get the frame
//! @return The frame of the layer
//! @see layer_set_frame
GRect layer_get_frame_by_value(const Layer *layer);
void layer_get_frame(const Layer *layer, GRect *frame);

//! Sets the bounds of the layer, which is it's bounding box relative to its frame.
//! If the bounds has changed, \ref layer_mark_dirty() will be called automatically.
//! @param layer The layer for which to set the bounds
//! @param bounds The new bounds
//! @see \ref layer_set_frame()
void layer_set_bounds_by_value(Layer *layer, GRect bounds);
void layer_set_bounds(Layer *layer, const GRect *bounds);

//! Gets the bounds of the layer
//! @param layer The layer for which to get the bounds
//! @return The bounds of the layer
//! @see layer_set_bounds
GRect layer_get_bounds_by_value(const Layer *layer);
void layer_get_bounds(const Layer *layer, GRect *bounds);

//! Gets the window that the layer is currently attached to.
//! @param layer The layer for which to get the window
//! @return The window that this layer is currently attached to, or `NULL` if it has
//! not been added to a window's layer hierarchy.
//! @see \ref window_get_root_layer()
//! @see \ref layer_add_child()
struct Window *layer_get_window(const Layer *layer);

//! Removes the layer from its current parent layer
//! If removed successfully, the child's parent layer will be marked dirty
//! automatically.
//! @param child The layer to remove
void layer_remove_from_parent(Layer *child);

//! Removes child layers from given layer
//! If removed successfully, the child's parent layer will be marked dirty
//! automatically.
//! @param parent The layer from which to remove all child layers
void layer_remove_child_layers(Layer *parent);

//! Adds the child layer to a given parent layer, making it appear
//! in front of its parent and in front of any existing child layers
//! of the parent.
//! If the child layer was already part of a layer hierarchy, it will
//! be removed from its old parent first.
//! If added successfully, the parent (and children) will be marked dirty
//! automatically.
//! @param parent The layer to which to add the child layer
//! @param child The layer to add to the parent layer
void layer_add_child(Layer *parent, Layer *child);

//! Inserts the layer as a sibling behind another layer. If the layer to insert was
//! already part of a layer hierarchy, it will be removed from its old parent first.
//! The below_layer has to be a child of a parent layer,
//! otherwise this function will be a noop.
//! If inserted successfully, the parent (and children) will be marked dirty
//! automatically.
//! @param layer_to_insert The layer to insert into the hierarchy
//! @param below_sibling_layer The layer that will be used as the sibling layer
//! above which the insertion will take place
void layer_insert_below_sibling(Layer *layer_to_insert, Layer *below_sibling_layer);

//! Inserts the layer as a sibling in front of another layer.
//! The above_layer has to be a child of a parent layer,
//! otherwise this function will be a noop.
//! If inserted successfully, the parent (and children) will be marked dirty
//! automatically.
//! @param layer_to_insert The layer to insert into the hierarchy
//! @param above_sibling_layer The layer that will be used as the sibling layer
//! below which the insertion will take place
void layer_insert_above_sibling(Layer *layer_to_insert, Layer *above_sibling_layer);

//! Sets the visibility of the layer.
//! If the visibility has changed, \ref layer_mark_dirty() will be called automatically
//! on the parent layer.
//! @param layer The layer for which to set the visibility
//! @param hidden Supply `true` to make the layer hidden, or `false` to make it
//! non-hidden.
void layer_set_hidden(Layer *layer, bool hidden);

//! Gets the visibility of the layer.
//! @param layer The layer for which to get the visibility
//! @return True if the layer is hidden, false if it is not hidden.
bool layer_get_hidden(const Layer *layer);

//! Sets whether clipping is enabled for the layer. If enabled, whatever the layer _and
//! its children_ will draw using their `.update_proc` callbacks, will be clipped by the
//! this layer's frame.
//! If the clipping has changed, \ref layer_mark_dirty() will be called automatically.
//! @param layer The layer for which to set the clipping property
//! @param clips Supply `true` to make the layer clip to its frame, or `false`
//! to make it non-clipping.
void layer_set_clips(Layer *layer, bool clips);

//! Gets whether clipping is enabled for the layer.  If enabled, whatever the layer _and
//! its children_ will draw using their `.update_proc` callbacks, will be clipped by the
//! this layer's frame.
//! @param layer The layer for which to get the clipping property
//! @return True if clipping is enabled for the layer, false if clipping is not enabled for
//! the layer.
bool layer_get_clips(const Layer *layer);

//! Gets the data from a layer that has been created with an extra data region.
//! @param layer The layer to get the data region from.
//! @return A void pointer to the data region.
void* layer_get_data(const Layer *layer);

//! Converts a point from the layer's local coordinate system to screen coordinates.
//! @note If the layer isn't part of the view hierarchy the result is undefined.
//! @param layer The view whose coordinate system will be used to convert the value to the screen.
//! @param point A point specified in the local coordinate system (bounds) of the layer.
//! @return The point converted to the coordinate system of the screen.
GPoint layer_convert_point_to_screen(const Layer *layer, GPoint point);

//! Converts a rectangle from the layer's local coordinate system to screen coordinates.
//! @note If the layer isn't part of the view hierarchy the result is undefined.
//! @param layer The view whose coordinate system will be used to convert the value to the screen.
//! @param rect A rectangle specified in the local coordinate system (bounds) of the layer.
//! @return The rectangle converted to the coordinate system of the screen.
GRect layer_convert_rect_to_screen(const Layer *layer, GRect rect);

//! @internal
//! Get the layer's frame in global coordinates
//! @param layer The layer whose global frame you seek
//! @param[out] global_frame_out GRect pointer to write the global frame to
void layer_get_global_frame(const Layer *layer, GRect *global_frame_out);

//! Get the largest unobstructed bounds rectangle of a layer.
//! @param layer The layer for which to get the unobstructed bounds.
//! @return The unobstructed bounds of the layer.
//! @see UnobstructedArea
GRect layer_get_unobstructed_bounds_by_value(const Layer *layer);
void layer_get_unobstructed_bounds(const Layer *layer, GRect *bounds_out);

//! Return whether a point is contained within the bounds of a layer. Can be overridden by
//! \ref layer_set_contains_point_override. Default behavior is to check that the point is within
//! layer's bounds.
//! @param layer layer to be tested
//! @param point point relative to the frame origin of the layer
//! @return true if the point is contained within the bounds of the layer
bool layer_contains_point(const Layer *layer, const GPoint *point);

//! Override the function layer_contains_point with a custom function
void layer_set_contains_point_override(Layer *layer, LayerContainsPointOverride override);

//! @internal
//! Traverse the tree starting at \ref node and find the layer in the tree which:
//! - contains the specified point,
//! - has no children which also contain the point
//! - is the last layer added to it's parent if any of its siblings match the above criteria, too.
//! When traversing, a branch will only be entered if that node layer contains the point (so each
//! layer down to the first with no children or more recently added siblings must contain the
//! point).
//! \note \ref layer_contains_point is used to perform the test as to whether the point is contained
//! within the layer, which can be overridden with custom implementations
//! @param node layer to start the traversal at
//! @param point point that must be contained within any found layer
//! @return the layer found, otherwise NULL, if no layers contain the point
Layer *layer_find_layer_containing_point(const Layer *node, const GPoint *point);

//! @note Do not export until touch is supported in the SDK!
//! Attach a recognizer to a layer
//! @param layer \ref Layer to which to attach \ref Recognizer
//! @param recognizer \ref Recognizer to attach
void layer_attach_recognizer(Layer *layer, Recognizer *recognizer);

//! @note Do not export until touch is supported in the SDK!
//! Detach a recognizer from a layer
//! @param layer \ref Layer from which to remove \ref Recognizer
//! @param recognizer \ref Recognizer to detach
void layer_detach_recognizer(Layer *layer, Recognizer *recognizer);

//! @note Do not export until touch is supported in the SDK!
//! Get the recognizers attached to a layer
//! @param layer \ref Layer from which to get recognizers
//! @return recognizer list
RecognizerList *layer_get_recognizer_list(const Layer *layer);

//! Return whether or \a layer is a descendant of \a potential_ancestor
//! @param layer check this layer to see if any of it's ancestors are \a potential_ancestor
//! @param potential_ancestor check to see if this layer is an ancestor of \a layer
//! match
//! @return true if \a layer is a descendant of \a potential_ancestor
bool layer_is_descendant(const Layer *layer, const Layer *potential_ancestor);

//! @internal
//! Common Scrolling directions
typedef enum {
  ScrollDirectionDown = -1,
  ScrollDirectionNone = 0,
  ScrollDirectionUp = 1
} ScrollDirection;

//!   @} // end addtogroup Layer
//! @} // end addtogroup UI

