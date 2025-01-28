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

#include "attribute.h"

#include "applib/graphics/gtypes.h"
#include "applib/ui/animation.h"
#include "applib/ui/layer.h"
#include "util/uuid.h"
#include "util/time/time.h"

typedef enum {
  LayoutLayerAnchorTextDirectionUp, // for scrolling up, past mode
  LayoutLayerAnchorTextDirectionDown, // for scrolling down, future mode
} LayoutLayerAnchorTextDirection;

//! LayoutLayer is a type of Layer that is used to display templated 3.0 content
//! including \ref TimelineItem (pins, reminders, notifications) as well as AppFaces.
//! LayoutLayers depart from traditional Layers in a few meaningful way.
//! 1) LayoutLayers are modulated by a "mode", which is the context in which the LayoutLayer
//! is displayed. Examples of modes are the "card" mode which displays detailed pin info
//! and the "minimized" mode which is used to display a "toast" like mode of a pin.
//! 2) LayoutLayers expose three more generic APIs:
//! \ref layout_get_size which returns the size of the
//! content within the layout as well as a generic constructor/destructor:
//!  \ref layout_create / \ref layout_destroy.
//! 3) LayoutLayers are constructed from a set of Attributes which they are meant to display.
//! 4) Sub-types of LayoutLayer are instantiated by summoning the correct type ID rather than by
//! calling a specialized constructor / destructor as per the Layer API.

//! LayoutIds identify the type of a LayoutLayer. They are passed to the constructor to
//! instantiate a specific sub-type of LayoutLayer. Simply stated, the LayoutLayer sub-type
//! informs what kinds of attributes to expect.
typedef enum {
  LayoutIdUnknown = 0, //!< Useful for catching error - 0 is not used as an id.
  LayoutIdGeneric, //!< Generic layout (probably only for testing)
  LayoutIdCalendar, //!< Calendar Pins
  LayoutIdReminder, //!< Generic Reminders
  LayoutIdNotification, //!< Generic Notifications
  LayoutIdCommNotification, //!< Communication Notification
  LayoutIdWeather, //!< Weather Pins
  LayoutIdSports, //!< Sports Pins
  LayoutIdAlarm, //!< Alarm Pins
  LayoutIdHealth, //!< Health Pins
  NumLayoutIds,
  LayoutIdTest, //!< Layout only for unit tests with no attribute requirements
} LayoutId;

typedef struct {
  GColor primary_color;
  GColor secondary_color;
  GColor bg_color;
} LayoutColors;

//! LayoutLayerModes modulate the layout. The mode defines the
//! context in which the layout is displayed.
typedef enum {
  LayoutLayerModeNone = 0,
  LayoutLayerModePeek, //!< Overlay-style mode shown similar to a partially obstructing HUD
  LayoutLayerModePinnedFat, //!< Menu-style mode in the Timeline app (fat, first item)
  LayoutLayerModePinnedThin, //!< Menu-style mode in the Timeline app (thin, second item)
  LayoutLayerModeCard, //!< Card mode, shows details of a TimelineItem
  NumLayoutLayerModes,
} LayoutLayerMode;

//! Forward defined to break define cycle.
struct LayoutLayer;

typedef struct LayoutLayerConfig LayoutLayerConfig;

//! A destructor for a LayoutLayer
//! @param layout the LayoutLayer to destroy
typedef void (*LayoutLayerDestructor)(struct LayoutLayer *layout);

//! A constructor for a LayoutLayer
//! @param frame the frame at which the LayoutLayer should be initialized.
//! @param attributes a pointer to the list of attributes to display in the layout layer.
//! Note that each LayoutLayer type expects a specific set of attributes
//! @param mode the LayoutLayerMode for the context in which the layout is displayed
//! @param context a context pointer
//! @return A pointer to the newly minted LayoutLayer
typedef struct LayoutLayer *(*LayoutLayerConstructor)(const LayoutLayerConfig *config);

//! A verifier for a layout
//! @param existing_attributes array of booleans indicating which attributes exist
//! @return true if the attribute list satisfies the layout requirements, false otherwise
typedef bool (*LayoutVerifier)(bool existing_attributes[]);

#pragma push_macro("GSize")
#undef GSize // [FBO] ugly work around for rogue macro
//! Get the size of the content of a layout. This is defined by the length of the text and
//! the size of the icons contained within the attributes.
//! @param ctx a pointer to the GContext in which the layout is rendered
//! @param layout a pointer to the LayoutLayer
//! @return A GSize describing the size occupied by the LayoutLayer's content
typedef struct GSize (*LayoutLayerSizeGetter)(GContext *ctx, struct LayoutLayer *layout);
#pragma pop_macro("GSize")

typedef void (*LayerLayerModeSetter)(struct LayoutLayer *layout, LayoutLayerMode final_mode);

#if PBL_COLOR
typedef const LayoutColors *(*LayoutLayerColorsGetter)(const struct LayoutLayer *layout);
#endif

typedef void* (*LayoutLayerContextGetter)(struct LayoutLayer *layout);

//! methods for the LayoutLayer type.
typedef struct {
  LayoutLayerSizeGetter size_getter;
  LayoutLayerDestructor destructor;
  LayerLayerModeSetter mode_setter;
#if PBL_COLOR
  LayoutLayerColorsGetter color_getter;
#endif
  LayoutLayerContextGetter context_getter;
} LayoutLayerImpl;

//! Data structure of a LayoutLayer.
typedef struct LayoutLayer {
  Layer layer; //!< The Layer underlying the LayoutLayer
  LayoutLayerMode mode; //!< The mode the LayoutLayer was created with
  AttributeList *attributes; //!< A pointer to the LayoutLayer's Attributes
  const LayoutLayerImpl *impl; //!< The implementation (constructor, destructor, methods)
} LayoutLayer;

struct LayoutLayerConfig {
  const GRect *frame;
  AttributeList *attributes;
  LayoutLayerMode mode;
  const Uuid *app_id;
  void *context;
};

//! Call the correct \ref LayoutLayerConstructor for a given \ref LayoutId
LayoutLayer *layout_create(LayoutId id, const LayoutLayerConfig *config);

//! Verify that the required attributes are there for the layout
bool layout_verify(bool existing_attributes[], LayoutId id);

//! Call the \ref LayoutLayerSizeGetter for a given layout
GSize layout_get_size(GContext *ctx, LayoutLayer *layout);

const LayoutColors *layout_get_colors(const LayoutLayer *layout);
const LayoutColors *layout_get_notification_colors(const LayoutLayer *layout);

Animation *layout_get_animation(LayoutLayer *layout, LayoutLayerMode final_mode);

void layout_set_mode(LayoutLayer *layout, LayoutLayerMode final_mode);

//! Call the \ref LayoutLayerDestructor for a given layout
void layout_destroy(LayoutLayer *layout);

void *layout_get_context(LayoutLayer *layout);
