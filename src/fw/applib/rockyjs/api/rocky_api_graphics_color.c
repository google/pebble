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

#include "rocky_api_graphics_color.h"

#include "rocky_api_util.h"

#include "string.h"
#include "util/size.h"

#define GColorARGB8FromRGBA(red, green, blue, alpha) \
  (uint8_t)( \
    ((((alpha) >> 6) & 0b11) << 6) | \
    ((((red) >> 6) & 0b11) << 4) | \
    ((((green) >> 6) & 0b11) << 2) | \
    ((((blue) >> 6) & 0b11) << 0) \
  )

#define GColorARGB8FromRGB(red, green, blue) GColorARGB8FromRGBA(red, green, blue, 255)
#define GColorARGB8FromHEX(v) \
  GColorARGB8FromRGB(((v) >> 16) & 0xff, ((v) >> 8) & 0xff, ((v) & 0xff))

// if performance ever becomes an issue with this, we can sort the names and to a binary search
T_STATIC const RockyAPIGraphicsColorDefinition s_color_definitions[] = {
  // taken from https://developer.mozilla.org/en-US/docs/Web/CSS/color_value
  {"black", GColorARGB8FromHEX(0x000000)},
  {"silver", GColorARGB8FromHEX(0xc0c0c0)},
  {"gray", GColorARGB8FromHEX(0x808080)},
  {"white", GColorARGB8FromHEX(0xffffff)},
  {"maroon", GColorARGB8FromHEX(0x800000)},
  {"red", GColorARGB8FromHEX(0xff0000)},
//  {"purple", GColorARGBFromHEX(0x800080)}, inconsistent with Pebble color
  {"fuchsia", GColorARGB8FromHEX(0xff00ff)},
//  {"green", GColorARGBFromHEX(0x008000)}, inconsistent with Pebble color
  {"lime", GColorARGB8FromHEX(0x00ff00)},
  {"olive", GColorARGB8FromHEX(0x808000)},
  {"yellow", GColorARGB8FromHEX(0xffff00)},
  {"navy", GColorARGB8FromHEX(0x000080)},
  {"blue", GColorARGB8FromHEX(0x0000ff)},
  {"teal", GColorARGB8FromHEX(0x008080)},
  {"aqua", GColorARGB8FromHEX(0x00ffff)},
  {"antiquewhite", GColorARGB8FromHEX(0xfaebd7)},
  {"aquamarine", GColorARGB8FromHEX(0x7fffd4)},
  {"azure", GColorARGB8FromHEX(0xf0ffff)},
  {"beige", GColorARGB8FromHEX(0xf5f5dc)},
  {"bisque", GColorARGB8FromHEX(0xffe4c4)},
  {"blanchedalmond", GColorARGB8FromHEX(0xffebcd)},
  {"blueviolet", GColorARGB8FromHEX(0x8a2be2)},
  {"brown", GColorARGB8FromHEX(0xa52a2a)},
  {"burlywood", GColorARGB8FromHEX(0xdeb887)},
//  {"cadetblue", GColorARGBFromHEX(0x5f9ea0)}, inconsistent with Pebble color
  {"chartreuse", GColorARGB8FromHEX(0x7fff00)},
  {"chocolate", GColorARGB8FromHEX(0xd2691e)},
  {"coral", GColorARGB8FromHEX(0xff7f50)},
  {"cornflowerblue", GColorARGB8FromHEX(0x6495ed)},
  {"cornsilk", GColorARGB8FromHEX(0xfff8dc)},
  {"crimson", GColorARGB8FromHEX(0xdc143c)},
  {"darkblue", GColorARGB8FromHEX(0x00008b)},
  {"darkcyan", GColorARGB8FromHEX(0x008b8b)},
  {"darkgoldenrod", GColorARGB8FromHEX(0xb8860b)},
//  {"darkgray", GColorARGBFromHEX(0xa9a9a9)}, inconsistent with Pebble color
//  {"darkgreen", GColorARGBFromHEX(0x006400)}, inconsistent with Pebble color
//  {"darkgrey", GColorARGBFromHEX(0xa9a9a9)}, inconsistent with Pebble color
  {"darkkhaki", GColorARGB8FromHEX(0xbdb76b)},
  {"darkmagenta", GColorARGB8FromHEX(0x8b008b)},
  {"darkolivegreen", GColorARGB8FromHEX(0x556b2f)},
  {"darkorange", GColorARGB8FromHEX(0xff8c00)},
  {"darkorchid", GColorARGB8FromHEX(0x9932cc)},
  {"darkred", GColorARGB8FromHEX(0x8b0000)},
  {"darksalmon", GColorARGB8FromHEX(0xe9967a)},
  {"darkseagreen", GColorARGB8FromHEX(0x8fbc8f)},
  {"darkslateblue", GColorARGB8FromHEX(0x483d8b)},
  {"darkslategray", GColorARGB8FromHEX(0x2f4f4f)},
  {"darkslategrey", GColorARGB8FromHEX(0x2f4f4f)},
  {"darkturquoise", GColorARGB8FromHEX(0x00ced1)},
  {"darkviolet", GColorARGB8FromHEX(0x9400d3)},
  {"deeppink", GColorARGB8FromHEX(0xff1493)},
  {"deepskyblue", GColorARGB8FromHEX(0x00bfff)},
  {"dimgray", GColorARGB8FromHEX(0x696969)},
  {"dimgrey", GColorARGB8FromHEX(0x696969)},
  {"dodgerblue", GColorARGB8FromHEX(0x1e90ff)},
  {"firebrick", GColorARGB8FromHEX(0xb22222)},
  {"floralwhite", GColorARGB8FromHEX(0xfffaf0)},
  {"forestgreen", GColorARGB8FromHEX(0x228b22)},
  {"gainsboro", GColorARGB8FromHEX(0xdcdcdc)},
  {"ghostwhite", GColorARGB8FromHEX(0xf8f8ff)},
  {"gold", GColorARGB8FromHEX(0xffd700)},
  {"goldenrod", GColorARGB8FromHEX(0xdaa520)},
  {"greenyellow", GColorARGB8FromHEX(0xadff2f)},
  {"grey", GColorARGB8FromHEX(0x808080)},
  {"honeydew", GColorARGB8FromHEX(0xf0fff0)},
  {"hotpink", GColorARGB8FromHEX(0xff69b4)},
  {"indianred", GColorARGB8FromHEX(0xcd5c5c)},
//  {"indigo", GColorARGBFromHEX(0x4b0082)}, inconsistent with Pebble color
  {"ivory", GColorARGB8FromHEX(0xfffff0)},
  {"khaki", GColorARGB8FromHEX(0xf0e68c)},
  {"lavender", GColorARGB8FromHEX(0xe6e6fa)},
  {"lavenderblush", GColorARGB8FromHEX(0xfff0f5)},
  {"lawngreen", GColorARGB8FromHEX(0x7cfc00)},
  {"lemonchiffon", GColorARGB8FromHEX(0xfffacd)},
  {"lightblue", GColorARGB8FromHEX(0xadd8e6)},
  {"lightcoral", GColorARGB8FromHEX(0xf08080)},
  {"lightcyan", GColorARGB8FromHEX(0xe0ffff)},
  {"lightgoldenrodyellow", GColorARGB8FromHEX(0xfafad2)},
//  {"lightgray", GColorARGBFromHEX(0xd3d3d3)}, inconsistent with Pebble color
  {"lightgreen", GColorARGB8FromHEX(0x90ee90)},
//  {"lightgrey", GColorARGBFromHEX(0xd3d3d3)}, inconsistent with Pebble color
  {"lightpink", GColorARGB8FromHEX(0xffb6c1)},
  {"lightsalmon", GColorARGB8FromHEX(0xffa07a)},
  {"lightseagreen", GColorARGB8FromHEX(0x20b2aa)},
  {"lightskyblue", GColorARGB8FromHEX(0x87cefa)},
  {"lightslategray", GColorARGB8FromHEX(0x778899)},
  {"lightslategrey", GColorARGB8FromHEX(0x778899)},
  {"lightsteelblue", GColorARGB8FromHEX(0xb0c4de)},
  {"lightyellow", GColorARGB8FromHEX(0xffffe0)},
  {"limegreen", GColorARGB8FromHEX(0x32cd32)},
  {"linen", GColorARGB8FromHEX(0xfaf0e6)},
//  {"mediumaquamarine", GColorARGBFromHEX(0x66cdaa)}, inconsistent with Pebble color
  {"mediumblue", GColorARGB8FromHEX(0x0000cd)},
  {"mediumorchid", GColorARGB8FromHEX(0xba55d3)},
  {"mediumpurple", GColorARGB8FromHEX(0x9370db)},
  {"mediumseagreen", GColorARGB8FromHEX(0x3cb371)},
  {"mediumslateblue", GColorARGB8FromHEX(0x7b68ee)},
//  {"mediumspringgreen", GColorARGBFromHEX(0x00fa9a)}, inconsistent with Pebble color
  {"mediumturquoise", GColorARGB8FromHEX(0x48d1cc)},
  {"mediumvioletred", GColorARGB8FromHEX(0xc71585)},
  {"midnightblue", GColorARGB8FromHEX(0x191970)},
  {"mintcream", GColorARGB8FromHEX(0xf5fffa)},
  {"mistyrose", GColorARGB8FromHEX(0xffe4e1)},
  {"moccasin", GColorARGB8FromHEX(0xffe4b5)},
  {"navajowhite", GColorARGB8FromHEX(0xffdead)},
  {"oldlace", GColorARGB8FromHEX(0xfdf5e6)},
  {"olivedrab", GColorARGB8FromHEX(0x6b8e23)},
  {"orangered", GColorARGB8FromHEX(0xff4500)},
  {"orchid", GColorARGB8FromHEX(0xda70d6)},
  {"palegoldenrod", GColorARGB8FromHEX(0xeee8aa)},
  {"palegreen", GColorARGB8FromHEX(0x98fb98)},
  {"paleturquoise", GColorARGB8FromHEX(0xafeeee)},
  {"palevioletred", GColorARGB8FromHEX(0xdb7093)},
  {"papayawhip", GColorARGB8FromHEX(0xffefd5)},
  {"peachpuff", GColorARGB8FromHEX(0xffdab9)},
  {"peru", GColorARGB8FromHEX(0xcd853f)},
  {"pink", GColorARGB8FromHEX(0xffc0cb)},
  {"plum", GColorARGB8FromHEX(0xdda0dd)},
  {"powderblue", GColorARGB8FromHEX(0xb0e0e6)},
  {"rosybrown", GColorARGB8FromHEX(0xbc8f8f)},
  {"royalblue", GColorARGB8FromHEX(0x4169e1)},
  {"saddlebrown", GColorARGB8FromHEX(0x8b4513)},
  {"salmon", GColorARGB8FromHEX(0xfa8072)},
  {"sandybrown", GColorARGB8FromHEX(0xf4a460)},
  {"seagreen", GColorARGB8FromHEX(0x2e8b57)},
  {"seashell", GColorARGB8FromHEX(0xfff5ee)},
  {"sienna", GColorARGB8FromHEX(0xa0522d)},
  {"skyblue", GColorARGB8FromHEX(0x87ceeb)},
  {"slateblue", GColorARGB8FromHEX(0x6a5acd)},
  {"slategray", GColorARGB8FromHEX(0x708090)},
  {"slategrey", GColorARGB8FromHEX(0x708090)},
  {"snow", GColorARGB8FromHEX(0xfffafa)},
  {"springgreen", GColorARGB8FromHEX(0x00ff7f)},
  {"steelblue", GColorARGB8FromHEX(0x4682b4)},
  {"tan", GColorARGB8FromHEX(0xd2b48c)},
  {"thistle", GColorARGB8FromHEX(0xd8bfd8)},
  {"tomato", GColorARGB8FromHEX(0xff6347)},
  {"turquoise", GColorARGB8FromHEX(0x40e0d0)},
  {"violet", GColorARGB8FromHEX(0xee82ee)},
  {"wheat", GColorARGB8FromHEX(0xf5deb3)},
  {"whitesmoke", GColorARGB8FromHEX(0xf5f5f5)},
  {"yellowgreen", GColorARGB8FromHEX(0x9acd32)},

  // CSS compatibility
  {"darkgrey", GColorDarkGrayARGB8},
  {"lightgrey", GColorLightGrayARGB8},

  // special cases
  {"transparent", GColorClearARGB8},
  {"clear", GColorClearARGB8},

  // Pebble colors taken from gcolor_definitions.h
  {"black", GColorBlackARGB8},
  {"oxfordblue", GColorOxfordBlueARGB8},
  {"dukeblue", GColorDukeBlueARGB8},
  {"blue", GColorBlueARGB8},
  {"darkgreen", GColorDarkGreenARGB8},
  {"midnightgreen", GColorMidnightGreenARGB8},
  {"cobaltblue", GColorCobaltBlueARGB8},
  {"bluemoon", GColorBlueMoonARGB8},
  {"islamicgreen", GColorIslamicGreenARGB8},
  {"jaegergreen", GColorJaegerGreenARGB8},
  {"tiffanyblue", GColorTiffanyBlueARGB8},
  {"vividcerulean", GColorVividCeruleanARGB8},
  {"green", GColorGreenARGB8},
  {"malachite", GColorMalachiteARGB8},
  {"mediumspringgreen", GColorMediumSpringGreenARGB8},
  {"cyan", GColorCyanARGB8},
  {"bulgarianrose", GColorBulgarianRoseARGB8},
  {"imperialpurple", GColorImperialPurpleARGB8},
  {"indigo", GColorIndigoARGB8},
  {"electricultramarine", GColorElectricUltramarineARGB8},
  {"armygreen", GColorArmyGreenARGB8},
  {"darkgray", GColorDarkGrayARGB8},
  {"liberty", GColorLibertyARGB8},
  {"verylightblue", GColorVeryLightBlueARGB8},
  {"kellygreen", GColorKellyGreenARGB8},
  {"maygreen", GColorMayGreenARGB8},
  {"cadetblue", GColorCadetBlueARGB8},
  {"pictonblue", GColorPictonBlueARGB8},
  {"brightgreen", GColorBrightGreenARGB8},
  {"screamingreen", GColorScreaminGreenARGB8},
  {"mediumaquamarine", GColorMediumAquamarineARGB8},
  {"electricblue", GColorElectricBlueARGB8},
  {"darkcandyapplered", GColorDarkCandyAppleRedARGB8},
  {"jazzberryjam", GColorJazzberryJamARGB8},
  {"purple", GColorPurpleARGB8},
  {"vividviolet", GColorVividVioletARGB8},
  {"windsortan", GColorWindsorTanARGB8},
  {"rosevale", GColorRoseValeARGB8},
  {"purpureus", GColorPurpureusARGB8},
  {"lavenderindigo", GColorLavenderIndigoARGB8},
  {"limerick", GColorLimerickARGB8},
  {"brass", GColorBrassARGB8},
  {"lightgray", GColorLightGrayARGB8},
  {"babyblueeyes", GColorBabyBlueEyesARGB8},
  {"springbud", GColorSpringBudARGB8},
  {"inchworm", GColorInchwormARGB8},
  {"mintgreen", GColorMintGreenARGB8},
  {"celeste", GColorCelesteARGB8},
  {"red", GColorRedARGB8},
  {"folly", GColorFollyARGB8},
  {"fashionmagenta", GColorFashionMagentaARGB8},
  {"magenta", GColorMagentaARGB8},
  {"orange", GColorOrangeARGB8},
  {"sunsetorange", GColorSunsetOrangeARGB8},
  {"brilliantrose", GColorBrilliantRoseARGB8},
  {"shockingpink", GColorShockingPinkARGB8},
  {"chromeyellow", GColorChromeYellowARGB8},
  {"rajah", GColorRajahARGB8},
  {"melon", GColorMelonARGB8},
  {"richbrilliantlavender", GColorRichBrilliantLavenderARGB8},
  {"yellow", GColorYellowARGB8},
  {"icterine", GColorIcterineARGB8},
  {"pastelyellow", GColorPastelYellowARGB8},
  {"white", GColorWhiteARGB8},

  // terminator for unit-test
  {0},
};

static bool prv_parse_name(const char *color_value, GColor8 *parsed_color) {
  for (size_t i = 0; i < ARRAY_LENGTH(s_color_definitions); i++) {
    if (s_color_definitions[i].name && strcmp(s_color_definitions[i].name, color_value) == 0) {
      if (parsed_color) {
        *parsed_color = (GColor8) {.argb = s_color_definitions[i].value};
      }
      return true;
    }
  }

  return false;
}

static bool prv_parse_hex_comp(const char *color_value, size_t len, int32_t *value_out) {
  // we re-implement strtol here be able to limit the length of the str

  int32_t value = 0;
  for (size_t i = 0; i < len; i++) {
    char ch = color_value[i];
    if (ch >= '0' && ch <= '9') {
      ch = ch - '0';
    } else if (ch >= 'A' && ch <= 'F') {
      ch = ch - 'A' + 10;
    } else if (ch >= 'a' && ch <= 'f') {
      ch = ch - 'a' + 10;
    } else { // This will also catch '\0'
      return false;
    }
    value = value * 16 + ch;
  }
  *value_out = value;
  return true;
}

static bool prv_parse_hex_comps(const char *color_value, size_t len,
                                int32_t *r, int32_t *g, int32_t *b, int32_t *a) {
  return prv_parse_hex_comp(color_value + 0 * len, len, r) &&
    prv_parse_hex_comp(color_value + 1 * len, len, g) &&
    prv_parse_hex_comp(color_value + 2 * len, len, b) &&
    (a == NULL || prv_parse_hex_comp(color_value + 3 * len, len, a));
}

static bool prv_parse_hex(const char *color_value, GColor8 *parsed_color) {
  const size_t len = strlen(color_value);
  if (len < 1 || color_value[0] != '#') {
    return false;
  }

  int32_t r, g, b, a;
  switch (len) {
    case 4: { // #RGB
      if (prv_parse_hex_comps(color_value + 1, 1, &r, &g, &b, NULL)) {
        *parsed_color = GColorFromRGB(r * (255/15), g * (255/15), b * (255/15));
        return true;
      }
    }
    case 5: { // #RGBA
      if (prv_parse_hex_comps(color_value + 1, 1, &r, &g, &b, &a)) {
        *parsed_color = GColorFromRGBA(r * (255/15), g * (255/15), b * (255/15), a * (255/15));
        if (parsed_color->a == 0) {
          *parsed_color = GColorClear;
        }
        return true;
      }
    }
    case 7: { // #RRGGBB
      if (prv_parse_hex_comps(color_value + 1, 2, &r, &g, &b, NULL)) {
        *parsed_color = GColorFromRGB(r, g, b);
        return true;
      }
    }
    case 9: { // #RRGGBBAA
      if (prv_parse_hex_comps(color_value + 1, 2, &r, &g, &b, &a)) {
        *parsed_color = GColorFromRGBA(r, g, b, a);
        if (parsed_color->a == 0) {
          *parsed_color = GColorClear;
        }
        return true;
      }
    }
  }

  return false;
}

bool rocky_api_graphics_color_parse(const char *color_value, GColor8 *parsed_color) {
  return prv_parse_name(color_value, parsed_color) ||
    prv_parse_hex(color_value, parsed_color);
}

bool rocky_api_graphics_color_from_value(jerry_value_t value, GColor *result) {
  if (jerry_value_is_number(value)) {
    *result = (GColor) {.argb = jerry_get_int32_value(value)};
    return true;
  }
  if (jerry_value_is_string(value)) {
    char color_str[50] = {0};
    jerry_string_to_utf8_char_buffer(value, (jerry_char_t *)color_str, sizeof(color_str));
    return rocky_api_graphics_color_parse(color_str, result);
  }
  return false;
}
