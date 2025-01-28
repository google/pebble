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

#include "applib/graphics/text.h"

// TODO: PBL-22261 Rename GTextNode et. al. with a proper Prefix, e.g. TimelineTextNode

//! GTextNode implements a stacking layout primarily for displaying complex compositions of text.
//!
//! TextNode supports sequential stacking with its horizontal and vertical containers. The
//! containers themselves can be aligned within their drawing box if enough excess space permits.
//!
//! For example, to display two consecutive strings of text with different fonts that are centered,
//! add two Text TextNodes with their respective fonts into a Horizontal TextNode with center
//! alignment. Draw the Horizontal TextNode with a drawing box as wide as the screen, and the two
//! text nodes will be side-by-side in the center of the screen. Nested containers can be used to
//! achieve more complicated layouts, such as the sports layout.
//!
//! Text flow and paging is applied to a TextNode hierarchy as a whole rather than a per-text basis.
//! When drawing a TextNode hierarchy, a draw config can be optionally specified with text flow and
//! paging parameters. Underneath the hood, text flow and paging is calculated per text node as
//! when normally rendering text with the usual text APIs.
//!
//! TextNode drawing uses an iterative implementation of depth-first traversal, so it is relatively
//! safe to use where drawing text normally occurs. Note that drawing text itself is still a stack
//! intensive process.

//! @internal
typedef enum {
  //! Text TextNode which displays text, supporting both text flow and paging. The text it displays
  //! can either be a pointer to a long-lived string, or a pointer to its own optional text buffer.
  GTextNodeType_Text = 0,
  //! TextDynamic TextNode which is similar to the Text TextNode with the addition of a user-defined
  //! text update callback is used to update the node's text buffer immediately before drawing,
  //! enabling the display of dynamically updating text, such as relative time e.g. "7 seconds ago".
  GTextNodeType_TextDynamic,
  //! Horizontal TextNode which is a sequentially stacking container that stacks its children along
  //! the x-axis. When drawn with a drawing box larger than its size which is dependent on the
  //! children it contains, this node can be aligned horizontally.
  GTextNodeType_Horizontal,
  //! Vertical TextNode which is a sequentially stacking container that stacks its children along
  //! the y-axis. When drawn with a drawing box larger than its size which is dependent on the
  //! children it contains, this node can be aligned vertically.
  GTextNodeType_Vertical,
  //! Custom TextNode which calls a user-defined function for both its size calculation and
  //! rendering, allowing the user to create any node that is not a base type. For example, an
  //! image node can be created with a function that reports the image size or renders the image.
  //! The Custom TextNode can also be used to change the draw state at certain points of the
  //! node hierarchy, or to reposition non-node elements to behave similar to nodes such as Layers.
  GTextNodeType_Custom,
  GTextNodeTypeCount,
} GTextNodeType;

//! @internal
typedef struct {
  const GRect *page_frame;
  const GPoint *origin_on_screen;
  uint8_t content_inset;
  bool text_flow;
  bool paging;
} GTextNodeDrawConfig;

typedef struct GTextNode GTextNode;

//! @internal
typedef void (*GTextNodeDrawCallback)(GContext *ctx, const GRect *box,
                                      const GTextNodeDrawConfig *config, bool render,
                                      GSize *size_out, void *user_data);

//! @internal
typedef void (*GTextNodeTextDynamicUpdate)(GContext *ctx, GTextNode *node, const GRect *box,
                                           const GTextNodeDrawConfig *config, bool render,
                                           char *buffer, size_t buffer_size, void *user_data);

//! @internal
struct GTextNode {
  //! Points to the next sibling in the parent container for use in iterative tree traversal.
  GTextNode *sibling;
  //! Denotes the type of TextNode.
  GTextNodeType type;
  //! Offset relatively positions the node without affecting the layout of any other nodes. The
  //! content will not be clipped unless inside of a clipping agent, such as a layer. The offset
  //! can be used in combination with margin to achieve top-left or all-side margins.
  GPoint offset;
  //! Margin affects the size of the node, increasing size if positive and decreasing if negative.
  //! Containers will treat the size of a node as the raw size plus the margin. Used alone, it
  //! adjusts the bottom-right margin, causing extra space to be between this node and nodes to the
  //! bottom and/or right.
  //!
  //! Used in-conjunction with offset, many of the normal layout position behavior in traditional
  //! systems can be achieved. Below are diagrams of a node (denoted with Xs) with its boundary
  //! increased by `a`, a certain length.
  //!
  //!        +----+              +----+              +----+            +----+
  //!        |XX  |              |  XX|              |    |            |    |
  //!        |XX  |              |  XX|              |  XX|            |XX  |
  //!        |    |              |    |              |  XX|            |XX  |
  //!        +----+              +----+              +----+            +----+
  //!
  //!   offset = { 0, 0 }   offset = { a, 0 }   offset = { a, a }   offset = { 0, a }
  //!   margin = { a, a }   margin = { a, a }   margin = { a, a }   margin = { a, a }
  //!
  //! If it is desired to draw the node outside of the margin, use an offset with values outside
  //! of the boundary of the rectangle { .origin = { 0, 0 }, .size = { a, a } }.
  //!
  //! Finally, centering can be achieved with offset = { a / 2, a / 2 } and margin = { a, a }, but
  //! it is recommended to use a Horizontal or Vertical TextNode's center alignment if such a
  //! container already exists in the hierarchy.
  GSize margin;
  //! TextNode assumes that a node's size does not change over time by default. This allows size
  //! calculation to only occur once, and the resulting size is stored in cached_size.
  GSize cached_size;
  //! Indicates whether this struct should be freed when being destroyed directly or as part of
  //! a TextNode hierarchy that is being destroy. Destroying a node results in all its child nodes
  //! being deeply destroyed.
  bool free_on_destroy;
  //! Whether to apply the node's draw box as the clip box as well. When `clip` is set, the
  //! clipping box will be reduced to the text node unmodified drawing box, and then restored after
  //! the text node is done drawing. Using the unmodified drawing box means that the node's
  //! `offset` and `margin` are not applied.
  bool clip;
};

//! @internal
typedef struct {
  GTextNode node;
  //! Exact size to force the container to constrain to. Normally, size is dynamically calculated
  //! to be the sequential sum of all children's size and margins along the container's stacking
  //! axis, and the max in the other axis. Setting either the width or height of size to a non-zero
  //! value will pin the container's respective non-zero size dimension, allowing the children to
  //! align themselves differently within the container.
  GSize size;
  size_t num_nodes; //!< Current number of attached nodes
  size_t max_nodes; //!< Maximum capacity of nodes
  GTextNode **nodes;
} GTextNodeContainer;

//! @internal
typedef struct {
  GTextNode node;
  //! Pointer to a UTF-8 string for drawing. If the node was allocated with
  //! \ref graphics_create_text_node having been called with a positive integer, text will be
  //! pointing to a writable text buffer that is pointing to the end of the node's memory.
  const char *text;
  GFont font;
  //! Maximum size the text can naturally grow to. Normally, the text is constrained by the draw box
  //! given to the text node. If this node is within a container, the draw box would be the draw box
  //! given to the container reduced by all of the previous siblings and equal to the draw box if
  //! this is the first sibling. Setting either the width or height of the maximum size to a
  //! non-zero value will replace the respective non-zero dimension of the draw box size when passed
  //! to the text layout max used size calculation, limiting that dimension.
  GSize max_size;
  int16_t line_spacing_delta;
  GColor color; //!< Text color to draw the text with
  GTextOverflowMode overflow;
  GTextAlignment alignment; //!< Alignment to use within the draw box given to the text node
} GTextNodeText;

//! @internal
typedef struct {
  GTextNodeText text;
  //! User-defined update function that will be called before every size and render update,
  //! usually to modify the node's text buffer.
  GTextNodeTextDynamicUpdate update;
  void *user_data; //!< User data that will be passed to the user-defined update function
  //! Size of the buffer that will be passed to the update callback. If the node was allocated with
  //! \ref graphics_create_text_node_dynamic, this is the buffer size that was passed to the buffer,
  //! and the buffer it describes is at the end of the node's memory.
  size_t buffer_size;
  //! If the node was created with \ref graphics_create_text_node_dynamic, this is the buffer
  //! described by `.buffer_size`.
  union { uint32_t _align; } buffer[];
} GTextNodeTextDynamic;

//! @internal
typedef struct {
  GTextNodeContainer container;
  GTextAlignment horizontal_alignment;
} GTextNodeHorizontal;

//! @internal
typedef struct {
  GTextNodeContainer container;
  GVerticalAlignment vertical_alignment;
} GTextNodeVertical;

//! @internal
typedef struct {
  GTextNode node;
  //! User-defined update function that will be called before every size and render update
  GTextNodeDrawCallback callback;
  void *user_data; //!< User data that will be passed to the user-defined update function
} GTextNodeCustom;

//! @internal
//! Allocates a single block of memory that ends with a zero-initialized string buffer.
//! Initializes `.text` with a pointer to the buffer if buffer_size not 0.
GTextNodeText *graphics_text_node_create_text(size_t buffer_size);

//! @internal
//! Allocates a single block of memory that ends with a zero-initialized string buffer.
//! Initializes `.text` with a pointer to the buffer if buffer_size not 0.
GTextNodeTextDynamic *graphics_text_node_create_text_dynamic(
    size_t buffer_size, GTextNodeTextDynamicUpdate update, void *user_data);

//! @internal
//! Allocates a single block of memory that ends with a zero-initialized node pointer buffer.
//! Initializes `.container.nodes` with a pointer to the buffer if buffer_size not 0.
GTextNodeHorizontal *graphics_text_node_create_horizontal(size_t max_nodes);

//! @internal
//! Allocates a single block of memory that ends with a zero-initialized node pointer buffer.
//! Initializes `.container.nodes` with a pointer to the buffer if buffer_size not 0.
GTextNodeVertical *graphics_text_node_create_vertical(size_t max_nodes);

//! @internal
GTextNodeCustom *graphics_text_node_create_custom(GTextNodeDrawCallback callback,
                                                  void *user_data);

//! @internal
//! Deeply destroys a TextNode and all its children
void graphics_text_node_destroy(GTextNode *node);

//! @internal
//! @returns true if the child was added to the parent, false otherwise
bool graphics_text_node_container_add_child(GTextNodeContainer *parent, GTextNode *child);

//! @internal
void graphics_text_node_get_size(GTextNode *node, GContext *ctx, const GRect *box,
                                 const GTextNodeDrawConfig *config, GSize *size_out);

//! @internal
void graphics_text_node_draw(GTextNode *node, GContext *ctx, const GRect *box,
                             const GTextNodeDrawConfig *config, GSize *size_out);
