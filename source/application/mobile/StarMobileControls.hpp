#pragma once

#include "StarInputEvent.hpp"
#include "StarJson.hpp"
#include "StarString.hpp"
#include "StarVector.hpp"

#include "SDL3/SDL.h"

#include <array>
#include <memory>
#include <vector>

namespace Star {

struct SafeAreaInsets {
  unsigned top = 0, left = 0, bottom = 0, right = 0;
  bool operator==(SafeAreaInsets const& o) const {
    return top == o.top && left == o.left && bottom == o.bottom && right == o.right;
  }
};

struct MobileTouchConfig {
  bool enabled = true;
  bool directTouchGestures = true;
  bool gyroEnabled = false;
  float opacity = 0.35f;
  float size = 1.0f;
  float deadzone = 0.15f;
  float gyroSensitivity = 1.0f;
  bool gyroInvertX = false;
  bool gyroInvertY = false;
};

enum class MobileGamepadStickMode {
  Movement,
  Aim
};

struct MobileGamepadStickConfig {
  bool enabled = true;
  MobileGamepadStickMode mode = MobileGamepadStickMode::Movement;
  bool preciseAim = false;
  float deadzone = 0.18f;
  float sensitivity = 1.0f;
  bool invertX = false;
  bool invertY = false;
};

struct MobileGamepadConfig {
  bool enabled = true;
  float triggerThreshold = 0.45f;
  MobileGamepadStickConfig leftStick;
  MobileGamepadStickConfig rightStick;

  MobileGamepadConfig() {
    rightStick.mode = MobileGamepadStickMode::Aim;
  }
};

enum class MobileTouchElementKind {
  Joystick,
  AimJoystick,
  Button,
  DPad
};

enum class MobileTouchActionKind {
  Key,
  KeyMacro,
  MouseButton,
  MouseWheelUp,
  MouseWheelDown,
  GyroToggle,
  GamepadAimModeToggle,
  ActionWheel,
  InventoryWheel,
  UiNavigation,
  None
};

enum class MobileTouchPressMode {
  SinglePress,
  Repeat,
  Hold,
  Toggle
};

struct MobileTouchAction {
  MobileTouchActionKind kind = MobileTouchActionKind::Key;
  Key key = Key::Space;
  MouseButton mouseButton = MouseButton::Left;
  UiNavigationDirection uiNavigationDirection = UiNavigationDirection::Down;
  List<Key> keys;
};

struct MobileTouchElement {
  String id;
  String label;
  MobileTouchElementKind kind = MobileTouchElementKind::Button;
  bool enabled = true;
  Vec2F position;
  float size = 1.0f;
  MobileTouchAction action;
  MobileTouchAction upAction;
  MobileTouchAction downAction;
  MobileTouchAction leftAction;
  MobileTouchAction rightAction;
  MobileTouchPressMode pressMode = MobileTouchPressMode::Hold;
  float aimSensitivity = 1.0f;
  bool preciseAim = false;
};

struct MobileGamepadBinding {
  ControllerButton button = ControllerButton::A;
  String label;
  bool enabled = true;
  MobileTouchAction action;
  MobileTouchPressMode pressMode = MobileTouchPressMode::SinglePress;
};


String keysName(List<Key> const& keys);
String actionName(MobileTouchAction const& action);
MobileTouchAction keyAction(Key key);
MobileTouchAction macroAction(List<Key> keys);
MobileTouchAction mouseAction(MouseButton button);
MobileTouchAction wheelAction(bool up);
MobileTouchAction gyroToggleAction();
MobileTouchAction gamepadAimModeToggleAction();
MobileTouchAction actionWheelAction();
MobileTouchAction inventoryWheelAction();
MobileTouchAction uiNavigationAction(UiNavigationDirection direction);
MobileTouchAction noneAction();
String pressModeName(MobileTouchPressMode mode);
MobileTouchPressMode pressModeFromName(String const& name, MobileTouchPressMode def = MobileTouchPressMode::Hold);
std::vector<MobileTouchElement> defaultTouchElements();
std::vector<MobileGamepadBinding> defaultGamepadBindings();
String elementKindName(MobileTouchElementKind kind);
MobileTouchElementKind elementKindFromName(String const& name);
List<Key> keysFromTouchAction(MobileTouchAction const& action);
JsonArray jsonFromKeys(List<Key> const& keys);
List<Key> keysFromText(String const& text);
List<Key> keysFromJson(Json const& json);
Json jsonFromTouchAction(MobileTouchAction const& action);
MobileTouchAction touchActionFromJson(Json const& json, MobileTouchAction def);
Json jsonFromTouchElement(MobileTouchElement const& element);
MobileTouchElement touchElementFromJson(Json const& json, MobileTouchElement def);
JsonArray jsonFromTouchElements(std::vector<MobileTouchElement> const& elements);
std::vector<MobileTouchElement> touchElementsFromConfig(Json const& config);
String gamepadStickModeName(MobileGamepadStickMode mode);
MobileGamepadStickMode gamepadStickModeFromName(String const& name, MobileGamepadStickMode def = MobileGamepadStickMode::Movement);
Json jsonFromGamepadStick(MobileGamepadStickConfig const& stick);
MobileGamepadStickConfig gamepadStickFromJson(Json const& json, MobileGamepadStickConfig def);
MobileGamepadConfig gamepadConfigFromConfig(Json const& config);
JsonArray jsonFromGamepadBindings(std::vector<MobileGamepadBinding> const& bindings);
MobileGamepadBinding gamepadBindingFromJson(Json const& json, MobileGamepadBinding def);
std::vector<MobileGamepadBinding> gamepadBindingsFromConfig(Json const& config);

class MobileTouchInputAdapter {
public:
  explicit MobileTouchInputAdapter(Vec2U* windowSize, Vec2U* renderCanvasSize = nullptr, float* displayScale = nullptr, SafeAreaInsets* safeArea = nullptr);
  ~MobileTouchInputAdapter();

  void setConfig(MobileTouchConfig config);
  void setGyroAvailable(bool available);
  void setElements(std::vector<MobileTouchElement> elements);
  void beginFrame();
  void endFrame();
  void appendGeneratedEvents(List<InputEvent>& outEvents);
  bool gyroSensorRequested() const;
  void setGyroInput(std::array<float, 3> const& data, bool hasData, SDL_DisplayOrientation orientation);
  bool processSdlEvent(SDL_Event const& event);
  void cancelAll();
  void drawOverlay();
  bool overlayEnabled() const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

class MobileGamepadInputAdapter {
public:
  explicit MobileGamepadInputAdapter(Vec2U* renderCanvasSize);
  ~MobileGamepadInputAdapter();

  void setConfig(MobileGamepadConfig config);
  void setBindings(std::vector<MobileGamepadBinding> bindings);
  void beginFrame();
  void endFrame();
  void appendGeneratedEvents(List<InputEvent>& outEvents);
  bool processInputEvent(InputEvent const& event, List<InputEvent>& passthroughEvents);
  void cancelAll();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}
