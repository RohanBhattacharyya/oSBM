#include "StarGamepadInput.hpp"

#include <cstdlib>
#include <iostream>

using namespace Star::GamepadInput;

namespace {
unsigned checks = 0;
void check(bool condition, char const* message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
bool near(float a, float b, float tolerance = 0.00001f) {
  return std::abs(a - b) < tolerance;
}
}

int main() {
  AimReturnFilter release;
  check(release.accept(0.9f, 0), "initial aim accepted");
  check(!release.accept(0.6f, 0.016), "spring return does not pull target inward");
  check(!release.accept(0.25f, 0.032), "intermediate spring sample stays latched");
  check(!release.accept(0, 0.048), "neutral preserves last accepted target");
  check(release.accept(0.4f, 0.064), "new aim after neutral accepted immediately");
  check(!release.accept(0.2f, 0.080), "rapid inward movement waits for intent");
  check(release.accept(0.2f, 0.190), "deliberate inward dwell accepted");
  check(release.accept(0.18f, 0.206), "slow inward correction remains available");
  check(release.accept(0.8f, 0.222), "outward movement resumes immediately");
  check(!release.accept(0.5f, 0.238), "second rapid return detected");
  check(release.accept(0.7f, 0.254), "renewed outward aim cancels return suppression");
  release = {};
  check(release.accept(0.3f, 1), "cancel/reset clears return state");
  for (int fps : {30, 60, 120, 240}) {
    AimReturnFilter spring;
    spring.accept(0.9f, 0);
    for (int frame = 1; frame <= fps / 10; ++frame) {
      float t = (float)frame / fps;
      float magnitude = std::max(0.0f, 0.9f - t * 15.0f);
      check(!spring.accept(magnitude, t), "spring suppression across render rates");
    }
  }

  for (float deadzone : {0.0f, 0.18f, 0.85f}) {
    auto center = radialStick(0, 0, deadzone);
    check(center[0] == 0 && center[1] == 0, "center is finite and still");
    auto edge = radialStick(deadzone, 0, deadzone);
    check(edge[0] == 0 && edge[1] == 0, "deadzone boundary is still");
    auto outside = radialStick(deadzone + 0.00001f, 0, deadzone);
    check(outside[0] > 0 && outside[0] < 0.001f, "continuous exit from deadzone");

    for (int angle = 0; angle < 360; ++angle) {
      float radians = angle * 0.01745329252f;
      for (float magnitude : {0.1f, 0.5f, 1.0f}) {
        float x = std::cos(radians) * magnitude;
        float y = std::sin(radians) * magnitude;
        auto value = radialStick(x, y, deadzone);
        float length = std::hypot(value[0], value[1]);
        check(length <= 1.00001f, "radial output never exceeds one");
        if (magnitude <= deadzone) {
          check(length == 0, "all directions inside deadzone are still");
        } else {
          check(near(x * value[1] - y * value[0], 0), "filter preserves angle");
          check(near(length, (magnitude - deadzone) / (1 - deadzone)), "equal travel at every angle");
        }
        auto inverted = radialStick(x, y, deadzone, true, true);
        check(near(inverted[0], -value[0]) && near(inverted[1], -value[1]), "both inversions preserve magnitude");
      }
    }
  }
  auto diagonal = radialStick(1, 1, 0.18f);
  check(near(std::hypot(diagonal[0], diagonal[1]), 1), "square-stick corner is capped radially");
  auto shallow = radialStick(0.8f, 0.1f, 0.18f);
  check(shallow[1] > 0, "shallow diagonal is not snapped to horizontal");
  auto drift = radialStick(0.10f, -0.10f, 0.18f);
  check(drift[0] == 0 && drift[1] == 0, "two-axis neutral drift is rejected");

  for (float sensitivity : {0.1f, 0.25f, 1.0f, 4.0f, 5.0f}) {
    float previous = -1;
    for (int i = 0; i <= 1000; ++i) {
      float response = aimResponse(i / 1000.0f, sensitivity);
      check(std::isfinite(response) && response >= 0 && response <= 1, "response is finite and bounded");
      check(response > previous, "response is strictly monotonic");
      previous = response;
    }
    check(aimResponse(0, sensitivity) == 0 && aimResponse(1, sensitivity) == 1, "sensitivity keeps full travel");
  }
  check(aimResponse(0.2f, 1) < 0.2f, "small deflection has finer control");

  // A noisy long pull must produce ONE press and ONE release, not repeated
  // clicks that would reset mining, charge attacks or split an item stack.
  for (float threshold : {0.05f, 0.20f, 0.45f, 0.95f}) {
    bool held = false;
    unsigned presses = 0, releases = 0;
    for (float value : {0.0f, threshold - 0.01f, threshold, threshold + 0.01f,
                       threshold - 0.01f, threshold, threshold - 0.02f, 1.0f, 0.0f, 0.0f}) {
      bool next = triggerPressed(value, threshold, held);
      presses += next && !held;
      releases += !next && held;
      held = next;
    }
    check(presses == 1 && releases == 1 && !held, "trigger hysteresis preserves a continuous hold");
    check(!triggerPressed(0, threshold, true), "fully released trigger always releases");
  }
  check(!triggerPressed(-1, 0.2f, true), "negative trigger input releases");

  // The center of any targeted tile remains inside the strict tool radius,
  // regardless of player sub-tile position or direction at maximum travel.
  for (float reach : {2.0f, 5.0f, 10.0f}) {
    for (float px : {0.0f, 0.25f, 0.99f}) {
      for (float py : {0.0f, 0.25f, 0.99f}) {
        for (int angle = 0; angle < 360; ++angle) {
          float radians = angle * 0.01745329252f;
          float x = px + std::cos(radians) * aimRadius(reach);
          float y = py + std::sin(radians) * aimRadius(reach);
          check(std::hypot(std::floor(x) + 0.5f - px, std::floor(y) + 0.5f - py) < reach,
              "tile-center rounding stays within tool reach");
        }
      }
    }
  }
  check(aimRadius(0) == 0, "zero reach does not reverse aim");
  std::cout << "Passed " << checks << " gamepad input checks\n";
}
