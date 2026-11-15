#pragma once

#include "StarJson.hpp"
#include "StarVector.hpp"

#include <algorithm>
#include <cmath>

namespace Star {

// Leave room for the mobile HUD's corner buttons without overriding the
// player's UI scale. Smaller windows may need a smaller hotbar.
inline float mobileHotbarScale(float windowWidth, float barWidth) {
  if (barWidth <= 0)
    return 1.0f;
  float margins = std::min(96.0f, windowWidth * 0.4f);
  float scale = std::clamp((windowWidth - margins) / barWidth, 0.1f, 1.0f);
  // Limit texture variants while a desktop window is dragged between sizes.
  return std::max(0.1f, std::floor(scale * 64.0f) / 64.0f);
}

inline Vec2I mobileHotbarOffset(float windowWidth, float fullBarWidth) {
  // Below the player HUD, center within the area to the left of the right
  // corner buttons (8-unit left inset, 88-unit right inset).
  return windowWidth < fullBarWidth + 320 ? Vec2I{-40, -100} : Vec2I{};
}

// Scale the asset-space layout before constructing widgets. Rendering and hit
// testing then use the same ordinary widget geometry, including item slots.
inline Json scaleHudLayout(Json const& value, float factor) {
  if (value.type() != Json::Type::Object)
    return value;
  JsonObject result;
  for (auto const& pair : value.toObject()) {
    auto const& key = pair.first;
    auto const& child = pair.second;
    bool image = key == "file" || key == "fileBody" || key == "fileHeader" || key == "fileFooter"
        || key == "base" || key == "hover" || key == "pressed" || key == "baseImageChecked"
        || key == "hoverImageChecked" || key == "backingImage" || key == "background" || key == "inner"
        || key == "begin" || key == "end";
    bool vector = key == "position" || key == "size" || key == "pressedOffset"
        || key == "actionBarSelectOffset" || key == "countMidAnchor";
    auto number = [](Json const& v) { return v.type() == Json::Type::Int || v.type() == Json::Type::Float; };
    if (image && child.type() == Json::Type::String && child.toString().beginsWith("/")) {
      result[key] = child.toString() + strf("?scalenearest={}", factor);
    } else if (vector && child.type() == Json::Type::Array && child.toArray().size() == 2
        && number(child.get(0)) && number(child.get(1))) {
      result[key] = JsonArray{(int)std::round(child.get(0).toFloat() * factor), (int)std::round(child.get(1).toFloat() * factor)};
    } else if (key == "itemScale" && number(child)) {
      result[key] = child.toFloat() * factor;
    } else if (key == "fontSize" && number(child)) {
      result[key] = std::max(1, (int)std::round(child.toFloat() * factor));
    } else {
      result[key] = scaleHudLayout(child, factor);
    }
  }
  return result;
}

}
