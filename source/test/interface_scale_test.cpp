#include "StarInterfaceScale.hpp"

#include <cstdio>
#include <cstdlib>

static unsigned checks = 0;
static void check(bool value, char const* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

int main() {
  using Star::androidInterfaceScale;
  // Regression: at 960x540 the old unconditional limit collapsed every
  // launcher size (1.35, 1.8, 2.4) to 1.08, so the UI never grew.
  for (unsigned height : {360u, 540u, 720u, 1080u, 2160u}) {
    for (float density : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
      float small = androidInterfaceScale(1.35f, false, density, height);
      float medium = androidInterfaceScale(1.8f, false, density, height);
      float large = androidInterfaceScale(2.4f, false, density, height);
      check(small < medium && medium < large, "manual sizes must visibly differ");
      check(large == 2.4f, "manual scale must not be clamped or multiplied by density");
      check(androidInterfaceScale(1.0f, false, density, height) == 1.0f,
          "manual scale can shrink again");
      float fit = androidInterfaceScale(2.4f, true, density, height);
      check(480.0f * fit <= std::max(500.0f, float(height)),
          "AUTO must retain space for the character creation menu");
    }
  }
  check(androidInterfaceScale(1.35f, true, 3.0f, 540) == 1.08f,
      "switching back to AUTO must restore the old short-screen fit");
  check(androidInterfaceScale(1.8f, false, 3.0f, 540) == 1.8f,
      "manual selection must leave the AUTO safety cap");
  std::printf("Passed %u interface scale checks\n", checks);
}
