#include "StarTouchInput.hpp"
#include <cstdio>
#include <cstdlib>

using namespace Star::TouchInput;
static unsigned checks = 0;
static void check(bool value, char const* message) {
  ++checks;
  if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
  for (int degrees = 0; degrees < 360; ++degrees) {
    float angle = degrees * 0.01745329252f;
    for (float length : {0.0f, 0.14f, 0.15f, 0.3f, 0.8f, 1.5f}) {
      auto value = stick(std::cos(angle) * length, std::sin(angle) * length, .15f, 1.4f);
      check(std::isfinite(value[0]) && std::isfinite(value[1]), "finite stick");
      check(std::hypot(value[0], value[1]) <= 1.00001f, "radial clamp");
      if (length < .15f) check(value[0] == 0 && value[1] == 0, "neutral drift rejected");
      if (length > .15f) check(std::abs(value[0] * std::sin(angle) - value[1] * std::cos(angle)) < .0001f, "angle preserved");
    }
  }
  Movement movement;
  movement.update(.31f, 0); check(movement.right, "direction starts");
  for (float x : {.29f, .25f, .21f, .28f}) { movement.update(x, 0); check(movement.right, "hysteresis no flicker"); }
  movement.update(.20f, 0); check(!movement.right, "release at off threshold");
  movement.update(.8f, .31f); check(movement.right && !movement.down, "horizontal axis lock");
  movement.update(.31f, .8f); check(!movement.right && movement.down, "vertical axis lock");
  movement.update(.8f, .8f); check(movement.right && movement.down, "deliberate diagonal");
  movement.update(-.8f, -.8f); check(movement.left && movement.up && !movement.right && !movement.down, "return across origin");
  Swipe swipe;
  check(swipe.move(-25) == 0 && swipe.move(-2) == 1, "left swipe step");
  check(swipe.move(53) == -2 && swipe.accumulated == 0, "reverse swipe remainder");
  check(wheel(0, -30, 30)[1] == 1 && wheel(0, 30, 30)[1] == -1, "wheel Y up");
  check(wheel(0, 0, 0)[0] == 0, "zero wheel reach");
  check(edge(0, 50, 100, 100) && !edge(50, 50, 100, 100), "palm edge");
  check(idleFade(0) == 1 && idleFade(3500) == 1 && std::abs(idleFade(4300) - .45f) < .0001f, "idle fade bounds");
  check(minimumTarget(3) == 144 && tapThreshold(3) == 30, "density independent targets");
  AimAttack attack;
  Contact moveFinger, aimFinger;
  check(moveFinger.claim(1) && aimFinger.claim(2), "independent two thumbs");
  check(!moveFinger.claim(2) && moveFinger.owns(1), "second finger cannot steal stick");
  for (int i = 0; i < 500; ++i) {
    movement.update(i % 2 ? .8f : -.8f, 0);
    check(attack.update(AttackMode::Continuous, true), "continuous mining survives movement");
  }
  check(!attack.release(AttackMode::Continuous, false) && !attack.held, "continuous releases");
  attack.update(AttackMode::Release, true);
  check(attack.release(AttackMode::Release, false), "release attack once");
  check(!attack.release(AttackMode::Release, false), "no phantom release attack");
  attack.update(AttackMode::Release, true);
  check(!attack.release(AttackMode::Release, true), "focus loss cannot fire");
  for (int reason = 0; reason < 5; ++reason) {
    moveFinger.clear(); aimFinger.clear(); attack.release(AttackMode::Continuous, true); movement = {};
    check(!moveFinger.active && !aimFinger.active && !attack.held && !movement.left, "lifecycle reset");
    check(moveFinger.claim(1), "new contact after reset");
  }
  Gesture gesture;
  gesture.down(50, 50, 0); check(!gesture.longPress(.44) && gesture.longPress(.45), "long press threshold");
  check(!gesture.tap(), "long press suppresses tap");
  gesture.down(50, 50, 1); check(!gesture.move(55, 55, 10) && gesture.move(70, 50, 10), "drag threshold");
  check(!gesture.tap() && !gesture.longPress(2), "drag cannot tap or long press");
  gesture.cancel(); check(!gesture.move(0, 0, 1) && !gesture.tap(), "cancelled contact inert");
  Inertia inertia; inertia.velocity = 200;
  float distance = 0;
  for (int i = 0; i < 200; ++i) distance += inertia.step(1.0f / 60);
  check(inertia.velocity == 0 && distance > 19 && distance < 21, "bounded inertia decay");
  std::printf("Passed %u touch input checks\n", checks);
}
