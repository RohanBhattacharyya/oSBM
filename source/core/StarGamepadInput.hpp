#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace Star::GamepadInput {

// A spring return can cross several nonzero SDL samples before neutral.
// Keep the last accepted target during a rapid inward sweep. A deliberate
// inward adjustment still works after a short dwell, or immediately on
// renewed outward motion. Time is supplied by the caller for deterministic
// tests and independence from render rate.
struct AimReturnFilter {
  float previous = 0;
  double previousTime = 0;
  double settleUntil = 0;
  bool returning = false;

  bool accept(float magnitude, double seconds) {
    double elapsed = seconds - previousTime;
    previousTime = seconds;
    if (magnitude <= 0.01f) {
      previous = 0;
      returning = false;
      return false;
    }
    if (previous > 0 && elapsed > 0 && elapsed < 0.1
        && previous - magnitude > 0.02f && (previous - magnitude) / elapsed > 5.0) {
      returning = true;
      settleUntil = seconds + 0.10;
    } else if (magnitude > previous + 0.04f) {
      returning = false;
    }
    previous = magnitude;
    if (returning && seconds < settleUntil)
      return false;
    returning = false;
    return true;
  }
};

// SDL stick space: right and down are positive. Filter the pair, never each
// axis separately: axial deadzones distort angles and lose shallow diagonals.
inline std::array<float, 2> radialStick(float x, float y, float deadzone, bool invertX = false, bool invertY = false) {
  x = std::clamp(x, -1.0f, 1.0f);
  y = std::clamp(y, -1.0f, 1.0f);
  float length = std::hypot(x, y);
  deadzone = std::clamp(deadzone, 0.0f, 0.85f);
  if (length <= deadzone || length == 0.0f)
    return {};
  float magnitude = (std::min(length, 1.0f) - deadzone) / (1.0f - deadzone);
  float scale = magnitude / length;
  return {x * scale * (invertX ? -1.0f : 1.0f), y * scale * (invertY ? -1.0f : 1.0f)};
}

// A nonzero slope near center permits single-tile adjustments. Full travel
// still reaches the outer radius; sensitivity changes response, not reach.
inline float aimResponse(float magnitude, float sensitivity) {
  magnitude = std::clamp(magnitude, 0.0f, 1.0f);
  float exponent = 1.5f / std::sqrt(std::clamp(sensitivity, 0.25f, 4.0f));
  return 0.25f * magnitude + 0.75f * std::pow(magnitude, exponent);
}

inline bool triggerPressed(float value, float threshold, bool wasPressed) {
  threshold = std::clamp(threshold, 0.05f, 0.95f);
  float releaseThreshold = std::max(0.01f, threshold - 0.08f);
  return wasPressed ? value > releaseThreshold : value >= threshold;
}

// Tool range tests use the tile CENTER and a strict '<'. Leave enough room
// for that rounding in every direction, including diagonals.
inline float aimRadius(float toolRadius) {
  return std::max(0.0f, toolRadius - 0.75f);
}

}
