#pragma once

#include <algorithm>
#include <cmath>

namespace Star {

// Explicit Android scales are renderer pixels per interface pixel, as on iOS.
// Only AUTO uses display density and the short-screen menu safety limit.
inline float androidInterfaceScale(float scale, bool automatic, float displayScale, unsigned windowHeight) {
  if (!automatic)
    return scale;
  float density = std::max(1.0f, std::round(displayScale));
  return std::min(scale * density, std::max(1.0f, windowHeight / 500.0f));
}

}
