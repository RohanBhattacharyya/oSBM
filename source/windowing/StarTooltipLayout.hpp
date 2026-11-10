#pragma once

#include "StarVector.hpp"

namespace Star {

struct TooltipLayout {
  float scale;
  Vec2I position;
};

// Tooltips have no input targets, so an oversized modded tooltip can be drawn
// at a smaller scale without changing the rest of the UI or its hit testing.
inline TooltipLayout tooltipLayout(Vec2U screenSize, float interfaceScale, Vec2I size, Vec2I mouse, Vec2I offset) {
  float scale = std::min(interfaceScale, std::min(
      std::max(1u, screenSize[0]) / (float)std::max(1, size[0]),
      std::max(1u, screenSize[1]) / (float)std::max(1, size[1])));
  Vec2I window = Vec2I::floor(Vec2F(screenSize) / scale);
  mouse = Vec2I::floor(Vec2F(mouse) * (interfaceScale / scale));
  offset = Vec2I::ceil(Vec2F(offset) * (interfaceScale / scale));
  Vec2I position = mouse + offset - Vec2I(0, size[1]);
  if (position[0] + size[0] > window[0])
    position[0] = mouse[0] - offset[0] - size[0];
  if (position[1] < 0)
    position[1] = mouse[1] - offset[1];
  for (size_t axis = 0; axis < 2; ++axis)
    position[axis] = clamp(position[axis], 0, std::max(0, window[axis] - size[axis]));
  return {scale, position};
}

}
