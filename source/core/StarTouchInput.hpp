#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Star::TouchInput {

// Lógica sem estado de jogo e sem alocações. SDL usa Y positivo para baixo.
using Point = std::array<float, 2>;
inline Point stick(float x, float y, float deadzone, float exponent = 1.0f) {
  float length = std::hypot(x, y);
  deadzone = std::clamp(deadzone, 0.0f, 0.85f);
  if (length <= deadzone || length <= 0.00001f)
    return {};
  float value = std::pow(std::clamp((length - deadzone) / (1.0f - deadzone), 0.0f, 1.0f),
      std::clamp(exponent, 0.5f, 3.0f));
  return {x / length * value, y / length * value};
}

struct Movement {
  bool right = false, left = false, up = false, down = false;
  void update(float x, float y) {
    right = x > (right ? 0.20f : 0.30f);
    left = -x > (left ? 0.20f : 0.30f);
    up = -y > (up ? 0.20f : 0.30f);
    down = y > (down ? 0.20f : 0.30f);
    if ((right || left) && (up || down)) {
      if (std::abs(y) < std::abs(x) * 0.60f)
        up = down = false;
      else if (std::abs(x) < std::abs(y) * 0.45f)
        right = left = false;
    }
  }
};

struct Swipe {
  float accumulated = 0;
  int move(float delta, float step = 26) {
    accumulated += delta;
    step = std::max(1.0f, step);
    int steps = (int)(accumulated / step);
    accumulated -= steps * step;
    return -steps;
  }
};

inline Point wheel(float dx, float dy, float reach) {
  reach = std::max(1.0f, reach);
  auto result = stick(dx / reach, dy / reach, 0);
  result[1] = -result[1]; // saída canônica: Y para cima.
  return result;
}
inline bool edge(float x, float y, float width, float height) {
  return x < width * 0.02f || x > width * 0.98f || y > height * 0.965f;
}
inline float idleFade(float idleMs) {
  return 1.0f - 0.55f * std::clamp((idleMs - 3500.0f) / 800.0f, 0.0f, 1.0f);
}
inline float tapThreshold(float pixelsPerDp) {
  return 10.0f * std::clamp(pixelsPerDp, 1.0f, 4.0f);
}
inline float minimumTarget(float pixelsPerDp) {
  return 48.0f * std::clamp(pixelsPerDp, 1.0f, 4.0f);
}

enum class AttackMode { AimOnly, Release, Continuous };
struct AimAttack {
  bool held = false, armed = false;
  // Retorna a borda desejada; o adaptador mantém contagem de donos do botão.
  bool update(AttackMode mode, bool outside) {
    armed |= outside;
    held = mode == AttackMode::Continuous && outside;
    return held;
  }
  bool release(AttackMode mode, bool canceled) {
    bool fire = mode == AttackMode::Release && armed && !canceled;
    held = armed = false;
    return fire;
  }
};

struct Contact {
  bool active = false;
  uint64_t owner = 0;
  bool claim(uint64_t id) {
    if (active)
      return false;
    active = true;
    owner = id;
    return true;
  }
  bool owns(uint64_t id) const { return active && id == owner; }
  void clear() { active = false; owner = 0; }
};

struct Inertia {
  float velocity = 0;
  float step(float dt) {
    dt = std::clamp(dt, 0.0f, 0.05f);
    float displacement = velocity * (1.0f - std::exp(-10.0f * dt)) / 10.0f;
    velocity *= std::exp(-10.0f * dt);
    if (std::abs(velocity) < 2)
      velocity = 0;
    return displacement;
  }
};

// Bloqueia novas ações de um contato cancelado até o próximo down.
struct Gesture {
  enum class State { Idle, Pending, Drag, LongPress, Consumed };
  State state = State::Idle;
  double downAt = 0;
  Point start{};
  void down(float x, float y, double now) { start = {x, y}; downAt = now; state = State::Pending; }
  bool move(float x, float y, float threshold) {
    if (state == State::Pending && std::hypot(x - start[0], y - start[1]) > threshold)
      state = State::Drag;
    return state == State::Drag;
  }
  bool longPress(double now) {
    if (state != State::Pending || now - downAt < 0.45)
      return false;
    state = State::LongPress;
    return true;
  }
  bool tap() const { return state == State::Pending; }
  void cancel() { state = State::Consumed; }
};
}
