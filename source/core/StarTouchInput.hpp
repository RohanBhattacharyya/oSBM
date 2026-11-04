#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

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
inline float aimTravel(float radius, float sensitivity, float seconds) {
  return radius * 13.2f * std::clamp(sensitivity, 0.25f, 4.0f) * std::clamp(seconds, 0.0f, 0.05f);
}

// Radius in SDL/ImGui logical units; shared with the layout editor.
inline float controlRadius(float shortSide, float scale) {
  return 56.0f * scale * std::max(1.0f, shortSide / 720.0f);
}

// Alterar layout não altera ações, rótulos ou identificadores existentes.
inline void preset(Point& position, float& size, int kind) {
  if (kind == 1) size = std::clamp(size * 0.8f, 0.45f, 2.4f);
  if (kind == 2) size = std::clamp(size * 1.25f, 0.45f, 2.4f);
  if (kind == 3) position[0] = 1.0f - position[0];
}
// Explicit opt-in layout: only geometry is supplied; callers keep actions,
// enabled flags, labels and opacity. Unknown/custom IDs are left untouched.
inline bool comfortablePreset(std::string_view id, Point& position, float& size) {
  struct Placement { std::string_view id; Point position; float size; };
  static constexpr Placement placements[] = {
    {"joystick", {.14f, .80f}, 1.15f},
    {"aimJoystick", {.78f, .80f}, 1.15f},
    {"leftHand", {.87f, .56f}, 1.05f},
    {"rightHand", {.95f, .56f}, 1.05f},
    {"jump", {.94f, .82f}, 1.40f},
    {"interact", {.88f, .70f}, 1.15f},
    {"tech", {.95f, .40f}, 1.00f},
    {"pause", {.06f, .32f}, .90f},
    {"chat", {.06f, .48f}, .82f},
    {"inventoryWheel", {.14f, .53f}, .95f},
    {"actionWheel", {.06f, .64f}, .95f},
    {"singleBlock", {.24f, .53f}, .80f},
    {"swapAimHand", {.24f, .70f}, .80f},
    {"targetLock", {.34f, .53f}, .80f},
    {"targetNext", {.44f, .53f}, .80f},
    {"dpad", {.14f, .80f}, 1.15f},
    {"shift", {.42f, .82f}, .82f},
    {"ctrl", {.50f, .82f}, .82f},
    {"gyroToggle", {.58f, .82f}, .82f}
  };
  for (auto const& placement : placements) {
    if (id == placement.id) {
      position = placement.position;
      size = placement.size;
      return true;
    }
  }
  return false;
}
template <typename Elements, typename Assign>
void mergeLayout(Elements& elements, Elements const& incoming, Assign assign) {
  for (auto const& value : incoming) {
    auto it = std::find_if(elements.begin(), elements.end(), [&](auto const& old) { return old.id == value.id; });
    if (it == elements.end()) elements.push_back(value);
    else assign(*it, value);
  }
}
inline bool overlaps(float x, float y, float rx, float ry, float left, float top, float right, float bottom) {
  return x + rx > left && x - rx < right && y + ry > top && y - ry < bottom;
}

enum class AttackMode { AimOnly, Release, Continuous };
struct AimAttack {
  bool held = false, armed = false;
  // Retorna a borda desejada; o adaptador mantém contagem de donos do botão.
  bool update(AttackMode mode, bool outside) {
    armed = outside;
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
