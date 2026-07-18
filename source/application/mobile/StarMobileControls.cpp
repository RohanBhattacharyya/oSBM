#include "StarApplication.hpp"
#include "StarApplicationController.hpp"
#include "StarByteArray.hpp"
#include "StarFile.hpp"
#include "StarJsonExtra.hpp"
#include "StarLogging.hpp"
#include "StarRenderer_gles.hpp"
#include "StarSignalHandler.hpp"
#include "StarTickRateMonitor.hpp"
#include "StarTime.hpp"
#include "StarPlatformServices_mobile.hpp"
#if STAR_SYSTEM_ANDROID
#include "mobile/android/StarAndroidFileAccessBridge.hpp"
#elif STAR_SYSTEM_IOS
#include "mobile/ios/StarIosFileAccessBridge.hpp"
extern "C" void StarIosBridge_setSdlWindow(void* window);
extern "C" void StarIosBridge_getSafeAreaInsets(float* top, float* left, float* bottom, float* right);
extern "C" int  StarIosBridge_getInterfaceOrientation();
#endif

#include "SDL3/SDL.h"
#include "SDL3/SDL_opengles2.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#ifdef STAR_SYSTEM_ANDROID
#include <android/log.h>
#include <jni.h>
#endif

#include "mobile/StarMobileControls.hpp"
#include "mobile/StarMobileLauncherSupport.hpp"

namespace Star {

String keysName(List<Key> const& keys) {
  StringList keyNames;
  for (auto key : keys)
    keyNames.append(KeyNames.getRight(key));
  return keyNames.join("+");
}

String actionName(MobileTouchAction const& action) {
  switch (action.kind) {
    case MobileTouchActionKind::Key:
      return KeyNames.getRight(action.key);
    case MobileTouchActionKind::KeyMacro:
      return action.keys.empty() ? launcherTextStatic("touchAction.macro", "Macro") : keysName(action.keys);
    case MobileTouchActionKind::MouseButton:
      return action.mouseButton == MouseButton::Left ? launcherTextStatic("touchAction.leftMouse", "Left Mouse")
          : action.mouseButton == MouseButton::Right ? launcherTextStatic("touchAction.rightMouse", "Right Mouse")
          : MouseButtonNames.getRight(action.mouseButton);
    case MobileTouchActionKind::MouseWheelUp:
      return launcherTextStatic("touchAction.scrollUp", "Scroll Up");
    case MobileTouchActionKind::MouseWheelDown:
      return launcherTextStatic("touchAction.scrollDown", "Scroll Down");
    case MobileTouchActionKind::GyroToggle:
      return launcherTextStatic("touchAction.gyroToggle", "Gyro Toggle");
    case MobileTouchActionKind::GamepadAimModeToggle:
      return launcherTextStatic("gamepadAction.aimModeToggle", "Toggle Aim Mode");
    case MobileTouchActionKind::ActionWheel:
      return launcherTextStatic("gamepadAction.actionWheel", "Action Wheel");
    case MobileTouchActionKind::InventoryWheel:
      return launcherTextStatic("gamepadAction.inventoryWheel", "Inventory Wheel");
    case MobileTouchActionKind::UiNavigation:
      return action.uiNavigationDirection == UiNavigationDirection::Up ? launcherTextStatic("gamepadAction.selectionUp", "Selection Up")
          : action.uiNavigationDirection == UiNavigationDirection::Down ? launcherTextStatic("gamepadAction.selectionDown", "Selection Down")
          : action.uiNavigationDirection == UiNavigationDirection::Left ? launcherTextStatic("gamepadAction.selectionLeft", "Selection Left")
          : launcherTextStatic("gamepadAction.selectionRight", "Selection Right");
    default:
      return launcherTextStatic("touchAction.none", "None");
  }
}

MobileTouchAction keyAction(Key key) {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::Key;
  action.key = key;
  return action;
}

MobileTouchAction macroAction(List<Key> keys, bool sequential) {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::KeyMacro;
  action.keys = std::move(keys);
  action.macroSequential = sequential;
  return action;
}

MobileTouchAction mouseAction(MouseButton button) {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::MouseButton;
  action.mouseButton = button;
  return action;
}

MobileTouchAction wheelAction(bool up) {
  MobileTouchAction action;
  action.kind = up ? MobileTouchActionKind::MouseWheelUp : MobileTouchActionKind::MouseWheelDown;
  return action;
}

MobileTouchAction gyroToggleAction() {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::GyroToggle;
  return action;
}

MobileTouchAction gamepadAimModeToggleAction() {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::GamepadAimModeToggle;
  return action;
}

MobileTouchAction actionWheelAction() {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::ActionWheel;
  return action;
}

MobileTouchAction inventoryWheelAction() {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::InventoryWheel;
  return action;
}

MobileTouchAction uiNavigationAction(UiNavigationDirection direction) {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::UiNavigation;
  action.uiNavigationDirection = direction;
  return action;
}

MobileTouchAction noneAction() {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::None;
  return action;
}

String pressModeName(MobileTouchPressMode mode) {
  switch (mode) {
    case MobileTouchPressMode::SinglePress:
      return "single";
    case MobileTouchPressMode::Repeat:
      return "repeat";
    case MobileTouchPressMode::Toggle:
      return "toggle";
    default:
      return "hold";
  }
}

MobileTouchPressMode pressModeFromName(String const& name, MobileTouchPressMode def) {
  if (name.equals("single", String::CaseInsensitive) || name.equals("singlePress", String::CaseInsensitive))
    return MobileTouchPressMode::SinglePress;
  if (name.equals("repeat", String::CaseInsensitive) || name.equals("rapidFire", String::CaseInsensitive))
    return MobileTouchPressMode::Repeat;
  if (name.equals("toggle", String::CaseInsensitive))
    return MobileTouchPressMode::Toggle;
  if (name.equals("hold", String::CaseInsensitive))
    return MobileTouchPressMode::Hold;
  return def;
}

String perfCounterModeName(PerformanceCounterMode mode) {
  switch (mode) {
    case PerformanceCounterMode::Detailed:
      return "detailed";
    default:
      return "fps";
  }
}

PerformanceCounterMode perfCounterModeFromName(String const& name, PerformanceCounterMode def) {
  if (name.equals("detailed", String::CaseInsensitive))
    return PerformanceCounterMode::Detailed;
  if (name.equals("fps", String::CaseInsensitive))
    return PerformanceCounterMode::Fps;
  return def;
}

std::vector<MobileTouchElement> defaultTouchElements() {
  return {
    {"joystick", launcherTextStatic("touchElement.joystick", "Joystick"), MobileTouchElementKind::Joystick, true, {0.14f, 0.78f}, 1.15f, keyAction(Key::Space), {}, {}, {}, {}},
    {"aimJoystick", launcherTextStatic("touchElement.aimJoystick", "Aim"), MobileTouchElementKind::AimJoystick, true, {0.66f, 0.78f}, 1.15f, noneAction(), {}, {}, {}, {}},
    {"leftHand", launcherTextStatic("touchElement.leftHand", "L"), MobileTouchElementKind::Button, true, {0.30f, 0.16f}, 0.92f, mouseAction(MouseButton::Left), {}, {}, {}, {}},
    {"rightHand", launcherTextStatic("touchElement.rightHand", "R"), MobileTouchElementKind::Button, true, {0.64f, 0.16f}, 0.92f, mouseAction(MouseButton::Right), {}, {}, {}, {}},
    {"jump", launcherTextStatic("touchElement.jump", "J"), MobileTouchElementKind::Button, true, {0.88f, 0.78f}, 1.00f, keyAction(Key::Space), {}, {}, {}, {}},
    {"interact", launcherTextStatic("touchElement.interact", "E"), MobileTouchElementKind::Button, true, {0.76f, 0.73f}, 0.92f, keyAction(Key::E), {}, {}, {}, {}},
    {"pause", launcherTextStatic("touchElement.pause", "ESC"), MobileTouchElementKind::Button, true, {0.10f, 0.15f}, 0.96f, keyAction(Key::Escape), {}, {}, {}, {}},
    {"chat", launcherTextStatic("touchElement.chat", "T"), MobileTouchElementKind::Button, true, {0.83f, 0.90f}, 0.72f, keyAction(Key::Return), {}, {}, {}, {}},
    {"tech", launcherTextStatic("touchElement.tech", "F"), MobileTouchElementKind::Button, true, {0.92f, 0.58f}, 0.86f, keyAction(Key::F), {}, {}, {}, {}},
    {"shift", launcherTextStatic("touchElement.shift", "Shift"), MobileTouchElementKind::Button, false, {0.66f, 0.88f}, 0.82f, keyAction(Key::LShift), {}, {}, {}, {}},
    {"ctrl", launcherTextStatic("touchElement.ctrl", "Ctrl"), MobileTouchElementKind::Button, false, {0.58f, 0.88f}, 0.82f, keyAction(Key::LCtrl), {}, {}, {}, {}},
    {"gyroToggle", launcherTextStatic("touchElement.gyro", "Gyro"), MobileTouchElementKind::Button, false, {0.74f, 0.88f}, 0.82f, gyroToggleAction(), {}, {}, {}, {}, MobileTouchPressMode::SinglePress},
    {"dpad", launcherTextStatic("touchElement.dpad", "D-PAD"), MobileTouchElementKind::DPad, false, {0.16f, 0.74f}, 1.05f, keyAction(Key::Space),
      keyAction(Key::W), keyAction(Key::S), keyAction(Key::A), keyAction(Key::D)},
    {"perfCounter", launcherTextStatic("touchElement.perfCounter", "Perf"), MobileTouchElementKind::PerformanceCounter, false, {0.02f, 0.98f}, 1.0f, noneAction(), {}, {}, {}, {}}
  };
}

std::vector<MobileGamepadBinding> defaultGamepadBindings() {
  return {
    {ControllerButton::A, "A", true, keyAction(Key::Space), MobileTouchPressMode::Hold},
    {ControllerButton::B, "B", true, keyAction(Key::F), MobileTouchPressMode::Hold},
    {ControllerButton::X, "X", true, keyAction(Key::E), MobileTouchPressMode::Hold},
    {ControllerButton::Y, "Y", true, keyAction(Key::I), MobileTouchPressMode::SinglePress},
    {ControllerButton::LeftShoulder, "LB", true, inventoryWheelAction(), MobileTouchPressMode::Hold},
    {ControllerButton::RightShoulder, "RB", true, actionWheelAction(), MobileTouchPressMode::Hold},
    {ControllerButton::TriggerLeft, "LT", true, mouseAction(MouseButton::Right), MobileTouchPressMode::Hold},
    {ControllerButton::TriggerRight, "RT", true, mouseAction(MouseButton::Left), MobileTouchPressMode::Hold},
    {ControllerButton::Back, "Back", true, keyAction(Key::I), MobileTouchPressMode::Hold},
    {ControllerButton::Start, "Start", true, keyAction(Key::Escape), MobileTouchPressMode::Hold},
    {ControllerButton::Guide, "Guide", false, noneAction(), MobileTouchPressMode::SinglePress},
    {ControllerButton::LeftStick, "L3", true, keyAction(Key::LCtrl), MobileTouchPressMode::Hold},
    {ControllerButton::RightStick, "R3", true, gamepadAimModeToggleAction(), MobileTouchPressMode::SinglePress},
    {ControllerButton::DPadUp, "Up", true, uiNavigationAction(UiNavigationDirection::Up), MobileTouchPressMode::SinglePress},
    {ControllerButton::DPadDown, "Down", true, uiNavigationAction(UiNavigationDirection::Down), MobileTouchPressMode::SinglePress},
    {ControllerButton::DPadLeft, "Left", true, uiNavigationAction(UiNavigationDirection::Left), MobileTouchPressMode::SinglePress},
    {ControllerButton::DPadRight, "Right", true, uiNavigationAction(UiNavigationDirection::Right), MobileTouchPressMode::SinglePress},
    {ControllerButton::Misc1, "Share", false, noneAction(), MobileTouchPressMode::SinglePress},
    {ControllerButton::Paddle1, "P1", false, noneAction(), MobileTouchPressMode::SinglePress},
    {ControllerButton::Paddle2, "P2", false, noneAction(), MobileTouchPressMode::SinglePress},
    {ControllerButton::Paddle3, "P3", false, noneAction(), MobileTouchPressMode::SinglePress},
    {ControllerButton::Paddle4, "P4", false, noneAction(), MobileTouchPressMode::SinglePress},
    {ControllerButton::Touchpad, "Pad", false, noneAction(), MobileTouchPressMode::SinglePress}
  };
}

String elementKindName(MobileTouchElementKind kind) {
  switch (kind) {
    case MobileTouchElementKind::Joystick:
      return "joystick";
    case MobileTouchElementKind::AimJoystick:
      return "aimJoystick";
    case MobileTouchElementKind::DPad:
      return "dpad";
    case MobileTouchElementKind::PerformanceCounter:
      return "perfCounter";
    default:
      return "button";
  }
}

MobileTouchElementKind elementKindFromName(String const& name) {
  if (name.equals("joystick", String::CaseInsensitive))
    return MobileTouchElementKind::Joystick;
  if (name.equals("aimJoystick", String::CaseInsensitive) || name.equals("aim", String::CaseInsensitive))
    return MobileTouchElementKind::AimJoystick;
  if (name.equals("dpad", String::CaseInsensitive))
    return MobileTouchElementKind::DPad;
  if (name.equals("perfCounter", String::CaseInsensitive))
    return MobileTouchElementKind::PerformanceCounter;
  return MobileTouchElementKind::Button;
}

List<Key> keysFromTouchAction(MobileTouchAction const& action) {
  if (action.kind == MobileTouchActionKind::Key)
    return {action.key};
  if (action.kind == MobileTouchActionKind::KeyMacro)
    return action.keys;
  return {};
}

JsonArray jsonFromKeys(List<Key> const& keys) {
  JsonArray out;
  for (auto key : keys)
    out.append(KeyNames.getRight(key));
  return out;
}

List<Key> keysFromText(String const& text) {
  List<Key> keys;
  for (auto const& rawToken : text.replace("+", " ").replace(",", " ").replace(";", " ").splitWhitespace()) {
    auto token = rawToken.trim();
    if (token.empty())
      continue;
    if (auto key = KeyNames.maybeLeft(token))
      keys.append(*key);
  }
  return keys;
}

List<Key> keysFromJson(Json const& json) {
  List<Key> keys;
  if (auto array = json.optArray("keys")) {
    for (auto const& keyJson : *array) {
      if (auto key = KeyNames.maybeLeft(keyJson.toString()))
        keys.append(*key);
    }
  }
  return keys;
}

Json jsonFromTouchAction(MobileTouchAction const& action) {
  if (action.kind == MobileTouchActionKind::KeyMacro)
    return JsonObject{{"type", "keys"}, {"keys", jsonFromKeys(action.keys)}, {"sequential", action.macroSequential}};
  if (action.kind == MobileTouchActionKind::MouseButton)
    return JsonObject{{"type", "mouse"}, {"button", MouseButtonNames.getRight(action.mouseButton)}};
  if (action.kind == MobileTouchActionKind::MouseWheelUp)
    return JsonObject{{"type", "wheel"}, {"direction", "up"}};
  if (action.kind == MobileTouchActionKind::MouseWheelDown)
    return JsonObject{{"type", "wheel"}, {"direction", "down"}};
  if (action.kind == MobileTouchActionKind::GyroToggle)
    return JsonObject{{"type", "gyroToggle"}};
  if (action.kind == MobileTouchActionKind::GamepadAimModeToggle)
    return JsonObject{{"type", "gamepadAimModeToggle"}};
  if (action.kind == MobileTouchActionKind::ActionWheel)
    return JsonObject{{"type", "actionWheel"}};
  if (action.kind == MobileTouchActionKind::InventoryWheel)
    return JsonObject{{"type", "inventoryWheel"}};
  if (action.kind == MobileTouchActionKind::UiNavigation)
    return JsonObject{{"type", "uiNavigation"}, {"direction", UiNavigationDirectionNames.getRight(action.uiNavigationDirection)}};
  if (action.kind == MobileTouchActionKind::None)
    return JsonObject{{"type", "none"}};
  return JsonObject{{"type", "key"}, {"key", KeyNames.getRight(action.key)}};
}

MobileTouchAction touchActionFromJson(Json const& json, MobileTouchAction def) {
  auto type = json.getString("type", "key");
  if (type.equals("keys", String::CaseInsensitive) || type.equals("macro", String::CaseInsensitive)) {
    auto keys = keysFromJson(json);
    return keys.empty() ? def : macroAction(keys, json.getBool("sequential", true));
  }
  if (type.equals("mouse", String::CaseInsensitive))
    return mouseAction(MouseButtonNames.valueLeft(json.getString("button", MouseButtonNames.getRight(def.mouseButton)), def.mouseButton));
  if (type.equals("wheel", String::CaseInsensitive))
    return wheelAction(!json.getString("direction", "up").equals("down", String::CaseInsensitive));
  if (type.equals("gyroToggle", String::CaseInsensitive) || type.equals("gyro", String::CaseInsensitive))
    return gyroToggleAction();
  if (type.equals("gamepadAimModeToggle", String::CaseInsensitive) || type.equals("aimModeToggle", String::CaseInsensitive))
    return gamepadAimModeToggleAction();
  if (type.equals("actionWheel", String::CaseInsensitive) || type.equals("wheelMenu", String::CaseInsensitive))
    return actionWheelAction();
  if (type.equals("inventoryWheel", String::CaseInsensitive) || type.equals("inventoryWheelMenu", String::CaseInsensitive))
    return inventoryWheelAction();
  if (type.equals("uiNavigation", String::CaseInsensitive) || type.equals("selection", String::CaseInsensitive))
    return uiNavigationAction(UiNavigationDirectionNames.valueLeft(json.getString("direction", UiNavigationDirectionNames.getRight(def.uiNavigationDirection)), def.uiNavigationDirection));
  if (type.equals("none", String::CaseInsensitive))
    return noneAction();
  auto keyName = json.getString("key", KeyNames.getRight(def.key));
  return keyAction(KeyNames.valueLeft(keyName, def.key));
}

Json jsonFromTouchElement(MobileTouchElement const& element) {
  JsonObject out{
    {"id", element.id},
    {"label", element.label},
    {"kind", elementKindName(element.kind)},
    {"enabled", element.enabled},
    {"position", JsonArray{element.position[0], element.position[1]}},
    {"size", element.size},
    {"pressMode", pressModeName(element.pressMode)},
    {"aimSensitivity", element.aimSensitivity},
    {"preciseAim", element.preciseAim},
    {"action", jsonFromTouchAction(element.action)}
  };
  if (element.kind == MobileTouchElementKind::DPad) {
    out["upAction"] = jsonFromTouchAction(element.upAction);
    out["downAction"] = jsonFromTouchAction(element.downAction);
    out["leftAction"] = jsonFromTouchAction(element.leftAction);
    out["rightAction"] = jsonFromTouchAction(element.rightAction);
  }
  if (element.kind == MobileTouchElementKind::PerformanceCounter)
    out["perfCounterMode"] = perfCounterModeName(element.perfCounterMode);
  return out;
}

MobileTouchElement touchElementFromJson(Json const& json, MobileTouchElement def) {
  def.id = json.getString("id", def.id);
  def.label = json.getString("label", def.label);
  def.kind = elementKindFromName(json.getString("kind", elementKindName(def.kind)));
  def.enabled = json.getBool("enabled", def.enabled);
  def.size = json.getFloat("size", def.size);
  def.pressMode = pressModeFromName(json.getString("pressMode", pressModeName(def.pressMode)), def.pressMode);
  def.aimSensitivity = json.getFloat("aimSensitivity", def.aimSensitivity);
  def.preciseAim = json.getBool("preciseAim", def.preciseAim);
  def.perfCounterMode = perfCounterModeFromName(json.getString("perfCounterMode", perfCounterModeName(def.perfCounterMode)), def.perfCounterMode);
  if (auto pos = json.optArray("position")) {
    if (pos->size() >= 2)
      def.position = Vec2F(pos->get(0).toFloat(), pos->get(1).toFloat());
  }
  def.action = touchActionFromJson(json.get("action", jsonFromTouchAction(def.action)), def.action);
  def.upAction = touchActionFromJson(json.get("upAction", jsonFromTouchAction(def.upAction)), def.upAction);
  def.downAction = touchActionFromJson(json.get("downAction", jsonFromTouchAction(def.downAction)), def.downAction);
  def.leftAction = touchActionFromJson(json.get("leftAction", jsonFromTouchAction(def.leftAction)), def.leftAction);
  def.rightAction = touchActionFromJson(json.get("rightAction", jsonFromTouchAction(def.rightAction)), def.rightAction);
  return def;
}

JsonArray jsonFromTouchElements(std::vector<MobileTouchElement> const& elements) {
  JsonArray out;
  for (auto const& element : elements)
    out.append(jsonFromTouchElement(element));
  return out;
}

std::vector<MobileTouchElement> touchElementsFromConfig(Json const& config) {
  auto elements = defaultTouchElements();
  if (auto saved = config.optQueryArray("touch.elements")) {
    for (auto const& savedElementJson : *saved) {
      String id = savedElementJson.getString("id", "");
      bool merged = false;
      for (auto& element : elements) {
        if (element.id == id) {
          element = touchElementFromJson(savedElementJson, element);
          merged = true;
          break;
        }
      }
      if (!merged && !id.empty())
        elements.push_back(touchElementFromJson(savedElementJson, MobileTouchElement{}));
    }
  }
  return elements;
}

String gamepadStickModeName(MobileGamepadStickMode mode) {
  switch (mode) {
    case MobileGamepadStickMode::Aim:
      return "aim";
    default:
      return "movement";
  }
}

MobileGamepadStickMode gamepadStickModeFromName(String const& name, MobileGamepadStickMode def) {
  if (name.equals("aim", String::CaseInsensitive) || name.equals("cameraAim", String::CaseInsensitive) || name.equals("camera", String::CaseInsensitive))
    return MobileGamepadStickMode::Aim;
  if (name.equals("movement", String::CaseInsensitive) || name.equals("move", String::CaseInsensitive))
    return MobileGamepadStickMode::Movement;
  return def;
}

String directTouchGestureModeName(DirectTouchGestureMode mode) {
  switch (mode) {
    case DirectTouchGestureMode::Touchpad:
      return "touchpad";
    default:
      return "touchscreen";
  }
}

DirectTouchGestureMode directTouchGestureModeFromName(String const& name, DirectTouchGestureMode def) {
  if (name.equals("touchpad", String::CaseInsensitive))
    return DirectTouchGestureMode::Touchpad;
  if (name.equals("touchscreen", String::CaseInsensitive))
    return DirectTouchGestureMode::Touchscreen;
  return def;
}

Json jsonFromGamepadStick(MobileGamepadStickConfig const& stick) {
  return JsonObject{
    {"enabled", stick.enabled},
    {"mode", gamepadStickModeName(stick.mode)},
    {"preciseAim", stick.preciseAim},
    {"deadzone", stick.deadzone},
    {"sensitivity", stick.sensitivity},
    {"invertX", stick.invertX},
    {"invertY", stick.invertY}
  };
}

MobileGamepadStickConfig gamepadStickFromJson(Json const& json, MobileGamepadStickConfig def) {
  def.enabled = json.getBool("enabled", def.enabled);
  def.mode = gamepadStickModeFromName(json.getString("mode", gamepadStickModeName(def.mode)), def.mode);
  def.preciseAim = json.getBool("preciseAim", def.preciseAim);
  def.deadzone = json.getFloat("deadzone", def.deadzone);
  def.sensitivity = json.getFloat("sensitivity", def.sensitivity);
  def.invertX = json.getBool("invertX", def.invertX);
  def.invertY = json.getBool("invertY", def.invertY);
  return def;
}

MobileGamepadConfig gamepadConfigFromConfig(Json const& config) {
  MobileGamepadConfig out;
  out.enabled = config.queryBool("gamepad.enabled", true);
  out.triggerThreshold = config.queryFloat("gamepad.triggerThreshold", 0.45f);
  out.leftStick = gamepadStickFromJson(config.query("gamepad.leftStick", JsonObject()), out.leftStick);
  out.rightStick = gamepadStickFromJson(config.query("gamepad.rightStick", JsonObject()), out.rightStick);
  return out;
}

JsonArray jsonFromGamepadBindings(std::vector<MobileGamepadBinding> const& bindings) {
  JsonArray out;
  for (auto const& binding : bindings) {
    out.append(JsonObject{
      {"button", ControllerButtonNames.getRight(binding.button)},
      {"label", binding.label},
      {"enabled", binding.enabled},
      {"action", jsonFromTouchAction(binding.action)},
      {"pressMode", pressModeName(binding.pressMode)}
    });
  }
  return out;
}

MobileGamepadBinding gamepadBindingFromJson(Json const& json, MobileGamepadBinding def) {
  def.button = ControllerButtonNames.valueLeft(json.getString("button", ControllerButtonNames.getRight(def.button)), def.button);
  def.label = json.getString("label", def.label);
  def.enabled = json.getBool("enabled", def.enabled);
  def.action = touchActionFromJson(json.get("action", jsonFromTouchAction(def.action)), def.action);
  def.pressMode = pressModeFromName(json.getString("pressMode", pressModeName(def.pressMode)), def.pressMode);

  auto migrateKeySinglePressDefaultToHold = [&](ControllerButton button, Key key) {
    if (def.button == button && def.pressMode == MobileTouchPressMode::SinglePress && def.action.kind == MobileTouchActionKind::Key && def.action.key == key)
      def.pressMode = MobileTouchPressMode::Hold;
  };
  migrateKeySinglePressDefaultToHold(ControllerButton::A, Key::Space);
  migrateKeySinglePressDefaultToHold(ControllerButton::B, Key::Escape);
  migrateKeySinglePressDefaultToHold(ControllerButton::X, Key::E);
  migrateKeySinglePressDefaultToHold(ControllerButton::Back, Key::I);
  migrateKeySinglePressDefaultToHold(ControllerButton::Start, Key::Escape);

  if (def.button == ControllerButton::B && def.action.kind == MobileTouchActionKind::Key && def.action.key == Key::Escape) {
    def.action = keyAction(Key::F);
    def.pressMode = MobileTouchPressMode::Hold;
  }

  if (def.button == ControllerButton::Y && def.action.kind == MobileTouchActionKind::Key && def.action.key == Key::Q) {
    def.action = actionWheelAction();
    def.pressMode = MobileTouchPressMode::Hold;
  }

  if (def.button == ControllerButton::Y && def.action.kind == MobileTouchActionKind::ActionWheel) {
    def.action = keyAction(Key::I);
    def.pressMode = MobileTouchPressMode::SinglePress;
  }

  if (def.button == ControllerButton::LeftShoulder && def.action.kind == MobileTouchActionKind::MouseWheelUp) {
    def.action = inventoryWheelAction();
    def.pressMode = MobileTouchPressMode::Hold;
  }

  if (def.button == ControllerButton::RightShoulder && def.action.kind == MobileTouchActionKind::MouseWheelDown) {
    def.action = actionWheelAction();
    def.pressMode = MobileTouchPressMode::Hold;
  }

  auto migrateDPadMovementDefault = [&](ControllerButton button, Key key, UiNavigationDirection direction) {
    if (def.button == button && def.pressMode == MobileTouchPressMode::Hold && def.action.kind == MobileTouchActionKind::Key && def.action.key == key) {
      def.action = uiNavigationAction(direction);
      def.pressMode = MobileTouchPressMode::SinglePress;
    }
  };
  migrateDPadMovementDefault(ControllerButton::DPadUp, Key::W, UiNavigationDirection::Up);
  migrateDPadMovementDefault(ControllerButton::DPadDown, Key::S, UiNavigationDirection::Down);
  migrateDPadMovementDefault(ControllerButton::DPadLeft, Key::A, UiNavigationDirection::Left);
  migrateDPadMovementDefault(ControllerButton::DPadRight, Key::D, UiNavigationDirection::Right);
  return def;
}

std::vector<MobileGamepadBinding> gamepadBindingsFromConfig(Json const& config) {
  auto bindings = defaultGamepadBindings();
  if (auto saved = config.optQueryArray("gamepad.bindings")) {
    for (auto const& savedBindingJson : *saved) {
      auto button = ControllerButtonNames.maybeLeft(savedBindingJson.getString("button", ""));
      if (!button)
        continue;

      bool merged = false;
      for (auto& binding : bindings) {
        if (binding.button == *button) {
          binding = gamepadBindingFromJson(savedBindingJson, binding);
          merged = true;
          break;
        }
      }
      if (!merged)
        bindings.push_back(gamepadBindingFromJson(savedBindingJson, MobileGamepadBinding{}));
    }
  }
  return bindings;
}

class MobileTouchInputAdapter::Impl {
public:
  explicit Impl(Vec2U* windowSize, Vec2U* renderCanvasSize = nullptr, float* displayScale = nullptr, SafeAreaInsets* safeArea = nullptr)
    : m_windowSize(windowSize), m_renderCanvasSize(renderCanvasSize), m_displayScalePtr(displayScale), m_safeAreaPtr(safeArea) {}

  void setConfig(MobileTouchConfig config) {
    if (!m_gyroAvailable)
      config.gyroEnabled = false;
    if (!config.directTouchGestures && m_config.directTouchGestures)
      cancelDirectTouchGestures();
    if (config.gyroEnabled && !m_config.gyroEnabled) {
      m_gyroRuntimeEnabled = true;
      m_lastGyroFrameMs = 0;
    } else if (!config.gyroEnabled) {
      m_gyroRuntimeEnabled = false;
      m_lastGyroFrameMs = 0;
      m_hasGyroInput = false;
    }
    m_config = config;
  }

  void setGyroAvailable(bool available) {
    m_gyroAvailable = available;
    if (!m_gyroAvailable) {
      m_config.gyroEnabled = false;
      m_gyroRuntimeEnabled = false;
      m_lastGyroFrameMs = 0;
      m_hasGyroInput = false;
    }
  }

  void setElements(std::vector<MobileTouchElement> elements) {
    cancelAll();
    m_elements = std::move(elements);
  }

  void beginFrame() {
    m_generatedEvents.clear();
    if (!m_config.enabled)
      cancelAll();
  }

  void endFrame() {
    emitActionEdges();
  }

  void appendGeneratedEvents(List<InputEvent>& outEvents) {
    outEvents.appendAll(m_generatedEvents);
    m_generatedEvents.clear();
  }

  bool gyroSensorRequested() const {
    return m_gyroAvailable && m_config.gyroEnabled;
  }

  void setGyroInput(std::array<float, 3> const& data, bool hasData, SDL_DisplayOrientation orientation) {
    if (!m_config.gyroEnabled) {
      m_hasGyroInput = false;
      return;
    }

    if (hasData) {
      m_gyroInput = data;
      m_hasGyroInput = true;
      m_lastGyroInputMs = Time::monotonicMilliseconds();
    }
    m_gyroOrientation = orientation;
  }

  bool processSdlEvent(SDL_Event const& event) {
    if (!m_config.enabled)
      return false;

    if (event.type != SDL_EVENT_FINGER_DOWN
        && event.type != SDL_EVENT_FINGER_UP
        && event.type != SDL_EVENT_FINGER_MOTION
        && event.type != SDL_EVENT_FINGER_CANCELED) {
      return false;
    }

    Vec2F pos = toScreen(event.tfinger.x, event.tfinger.y);
    uint64_t finger = event.tfinger.fingerID;

    if (event.type == SDL_EVENT_FINGER_DOWN)
      assignFinger(finger, pos);
    else if (event.type == SDL_EVENT_FINGER_MOTION)
      updateFinger(finger, pos);
    else if (event.type == SDL_EVENT_FINGER_CANCELED)
      cancelFinger(finger);
    else
      releaseFinger(finger, pos);

    return true;
  }

  void cancelAll() {
    clearPulsedActions();
    m_macroEvents.clear();

    for (auto const& pair : m_keyHoldCounts.pairs()) {
      if (pair.second > 0)
        emitEvent(KeyUpEvent{pair.first});
    }
    for (auto const& pair : m_mouseHoldCounts.pairs()) {
      if (pair.second > 0) {
        syncVirtualAimCursor(false, m_config.joystickAimCursorEnabled);
        emitEvent(MouseButtonUpEvent{pair.first, mouseActionPosition()});
      }
    }

    m_fingers.clear();
    m_heldElements.clear();
    m_toggledElements.clear();
    m_dpadHeld.clear();
    m_nextDPadWheelMs.clear();
    m_nextActionRepeatMs.clear();
    m_keyActionOwners.clear();
    m_keyHoldCounts.clear();
    m_mouseActionOwners.clear();
    m_mouseHoldCounts.clear();

    m_joystickActive = false;
    m_joystickFinger = 0;
    m_joystickElementId.clear();
    m_moveVec = {};
    m_aimJoystickActive = false;
    m_aimJoystickFinger = 0;
    m_aimJoystickElementId.clear();
    m_aimVec = {};

    if (m_primaryMouseHeld)
      releaseDirectGestureAction(m_config.directTouchSingleAction, DirectTouchSingleOwner);
    if (m_secondaryMouseHeld)
      releaseDirectGestureAction(m_config.directTouchTwoFingerAction, DirectTouchTwoFingerOwner);

    m_primaryHeld = false;
    m_primaryMouseHeld = false;
    m_aimFinger = 0;
    m_primaryPausedForSecondary = false;

    m_secondaryHeld = false;
    m_secondaryMouseHeld = false;
    m_secondaryFinger = 0;
  }

  static String perfCounterText(PerformanceCounterMode mode, float fps) {
    String text = strf("{:.0f} FPS", fps);
    if (mode == PerformanceCounterMode::Detailed) {
      // Reuses the same per-frame stats the vanilla debug HUD (F3-style
      // overlay, see MainInterface::renderDebug) already computes every
      // frame via LogMap::set -- no new instrumentation needed, just read
      // it back. Falls back to just the FPS line if a key isn't populated
      // yet (e.g. the very first frames, or before a world is loaded).
      auto appendIfPresent = [&text](String const& key) {
        String value = LogMap::getValue(key);
        if (!value.empty())
          text += strf("\n{}", value);
      };
      appendIfPresent("client_update_rate");
      appendIfPresent("client_render_world_total");
      appendIfPresent("client_render_interface");
    }
    return text;
  }

  void drawOverlay(float fps) {
    if (!m_config.enabled)
      return;

    // At least one enabled PerformanceCounter element needs LogMap populated
    // for its "Detailed" tier; LogMap::set() calls are near-free early-outs
    // elsewhere in the engine when nothing observes the map, so only pay for
    // it while a detailed counter is actually on screen.
    bool wantsDetailedPerf = false;
    for (auto const& element : m_elements) {
      if (element.enabled && element.kind == MobileTouchElementKind::PerformanceCounter
          && element.perfCounterMode == PerformanceCounterMode::Detailed) {
        wantsDetailedPerf = true;
        break;
      }
    }
    LogMap::setObserved(wantsDetailedPerf);

    float radius = controlRadius();

    Vec2F drawScale = physicalToDrawScale();
    float radiusScale = std::max(1.0f, std::min(drawScale[0], drawScale[1]));
    // Convert canvas-pixel positions to ImGui window coordinates.
    // Safe-area insets are in physical pixels; divide by drawScale to get ImGui
    // units and add as an offset so buttons are placed inside the safe area.
    float saOffX = m_safeAreaPtr ? (float)m_safeAreaPtr->left / drawScale[0] : 0.0f;
    float saOffY = m_safeAreaPtr ? (float)m_safeAreaPtr->top  / drawScale[1] : 0.0f;
    auto ip = [drawScale, saOffX, saOffY](Vec2F const& v) {
      return ImVec2(saOffX + v[0] / drawScale[0], saOffY + v[1] / drawScale[1]);
    };

    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // Always-visible contrast, using Minecraft's crosshair technique: the button
    // geometry is drawn with an inversion (difference) blend so each stroke shows
    // as the photographic negative of whatever is behind it and can never wash
    // out against a matching background. The opacity setting is remapped to a
    // contrast "strength": at full strength the source is pure white (result =
    // 1 - background); as it drops, the source fades toward mid-grey, where the
    // inversion result approaches the background and the overlay fades away.
    // (The inversion blend keys off source RGB, ignoring alpha coverage, so it
    // suits solid shapes; text is handled separately with an outline below.)
    float op = std::clamp(m_config.opacity, 0.0f, 1.0f);
    auto contrast = [op](float c) { return 0.5f + (c - 0.5f) * op; };
    auto contrastColor = [&contrast](float r, float g, float b) {
      return IM_COL32((int)std::lround(contrast(r) * 255.0f),
                      (int)std::lround(contrast(g) * 255.0f),
                      (int)std::lround(contrast(b) * 255.0f), 255);
    };
    ImU32 base = contrastColor(1.0f, 1.0f, 1.0f);
    ImU32 fill = contrastColor(0.31f, 0.63f, 1.0f);
    float thickness = std::max(1.0f, m_config.outlineThickness);

    // Text can't share the inversion blend (font coverage is stored in alpha, so
    // inverting by RGB would flip whole glyph quads). Labels instead get a dark
    // outline behind a light glyph so they stay legible on any background.
    ImU32 labelColor = IM_COL32(255, 255, 255, (int)(230.0f * op));
    ImU32 labelOutline = IM_COL32(0, 0, 0, (int)(210.0f * op));

    std::vector<OverlayLabel> labels;

    // Shape pass: switch to the inversion blend for the button geometry, then
    // restore ImGui's default blend state afterwards.
    draw->AddCallback(applyContrastBlendCallback, nullptr);

    for (auto const& element : m_elements) {
      if (!element.enabled)
        continue;

      Vec2F center = elementCenter(element);
      float elementRadius = radius * std::clamp(element.size, 0.45f, 2.4f);
      float drawRadius = elementRadius / radiusScale;

      if (element.kind == MobileTouchElementKind::Joystick) {
        draw->AddCircle(ip(center), drawRadius, base, 48, thickness);
        if (m_joystickActive && m_joystickElementId == element.id)
          draw->AddCircleFilled(ip(m_joystickCurrent), drawRadius * 0.45f, fill, 32);
      } else if (element.kind == MobileTouchElementKind::AimJoystick) {
        draw->AddCircle(ip(center), drawRadius, base, 48, thickness);
        if (m_aimJoystickActive && m_aimJoystickElementId == element.id)
          draw->AddCircleFilled(ip(m_aimJoystickCurrent), drawRadius * 0.35f, fill, 32);
      } else if (element.kind == MobileTouchElementKind::DPad) {
        drawDPad(draw, ip(center), drawRadius, element.id, base, fill, thickness, labels);
      } else if (element.kind == MobileTouchElementKind::PerformanceCounter) {
        // Passive display, not a touch target: no shape, just queued text
        // drawn in the un-blended text pass below (same as other labels).
        labels.push_back({ip(center), perfCounterText(element.perfCounterMode, fps)});
      } else {
        bool held = heldElement(element.id)
            || (element.action.kind == MobileTouchActionKind::GyroToggle && m_gyroAvailable && m_config.gyroEnabled && m_gyroRuntimeEnabled);
        drawButton(draw, ip(center), drawRadius * 0.55f, held, element.label.utf8Ptr(), base, fill, thickness, labels);
      }
    }

    // Restore the default alpha blend before drawing text on top of the shapes.
    draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    for (auto const& label : labels)
      drawLabelWithOutline(draw, label.pos, label.text.utf8Ptr(), labelColor, labelOutline);
  }

  bool overlayEnabled() const {
    return m_config.enabled;
  }

private:
  enum class FingerRole {
    None,
    Joystick,
    AimJoystick,
    Aim,
    SuppressedTap,
    SecondaryHold,
    ActionButton,
    DPad
  };

  struct FingerState {
    FingerRole role = FingerRole::None;
    Vec2F startPos;
    Vec2F currentPos;
    String elementId;
    MobileTouchAction action;
    MobileTouchPressMode pressMode = MobileTouchPressMode::Hold;
    int64_t downTimeMs = 0;
    bool movedTooFarForTap = false;
    // Touchpad mode (FingerRole::Aim only): the finger's raw position as of
    // the last update, used to compute the next frame's swipe delta. Kept
    // separate from currentPos, which trackTapMotion overwrites unconditionally.
    Vec2F touchpadAnchor;
  };

  static bool insideCircle(Vec2F const& p, Vec2F const& center, float radius) {
    return (p - center).magnitudeSquared() <= radius * radius;
  }

  Vec2F canvasSize() const {
    Vec2U const& ws = *m_windowSize;
    float w = (float)ws[0];
    float h = (float)ws[1];
    if (m_safeAreaPtr) {
      if (ws[0] > m_safeAreaPtr->left + m_safeAreaPtr->right)
        w = (float)(ws[0] - m_safeAreaPtr->left - m_safeAreaPtr->right);
      if (ws[1] > m_safeAreaPtr->top + m_safeAreaPtr->bottom)
        h = (float)(ws[1] - m_safeAreaPtr->top - m_safeAreaPtr->bottom);
    }
    return {w, h};
  }

  Vec2F elementCenter(MobileTouchElement const& element) const {
    Vec2F s = canvasSize();
    return {
      std::clamp(element.position[0], 0.02f, 0.98f) * s[0],
      std::clamp(element.position[1], 0.02f, 0.98f) * s[1]
    };
  }

  MobileTouchElement const* findElement(String const& id) const {
    for (auto const& element : m_elements) {
      if (element.id == id)
        return &element;
    }
    return nullptr;
  }

  bool heldElement(String const& id) const {
    return m_heldElements.contains(id) || m_toggledElements.contains(id) || m_dpadHeld.contains(id + ":up") || m_dpadHeld.contains(id + ":down")
        || m_dpadHeld.contains(id + ":left") || m_dpadHeld.contains(id + ":right");
  }

  // ImGui draw lists use SDL window coordinates (logical points on iOS, physical
  // pixels on Android).  Derive the physical-pixel-to-ImGui-unit ratio from the
  // full SDL window size so the scale is always the true device pixel ratio,
  // independent of any safe-area inset.  Safe-area offsets are applied separately
  // in drawOverlay() so buttons land at the correct physical position.
  Vec2F physicalToDrawScale() const {
    Vec2U const& ws = *m_windowSize;

    if (ImGui::GetCurrentContext()) {
      ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      if (displaySize.x > 0.0f && displaySize.y > 0.0f)
        return {
          std::max(1.0f, (float)ws[0] / displaySize.x),
          std::max(1.0f, (float)ws[1] / displaySize.y)
        };
    }

#ifdef STAR_SYSTEM_IOS
    if (m_displayScalePtr)
      return Vec2F::filled(std::max(1.0f, std::round(*m_displayScalePtr)));
#endif

    return Vec2F::filled(1.0f);
  }

  float controlRadius() const {
    Vec2F drawScale = physicalToDrawScale();
    float radiusScale = std::max(1.0f, std::min(drawScale[0], drawScale[1]));
    Vec2F drawSize = {
      (float)(*m_windowSize)[0] / drawScale[0],
      (float)(*m_windowSize)[1] / drawScale[1]
    };
    float shortSide = std::min(drawSize[0], drawSize[1]);
    return 56.0f * radiusScale * m_config.size * std::max(1.0f, shortSide / 720.0f);
  }

  float tapMovementThreshold() const {
    return controlRadius() * 0.35f;
  }

  bool isTap(FingerState const& state, Vec2F const& releasePos) const {
    if (state.movedTooFarForTap)
      return false;
    if (Time::monotonicMilliseconds() - state.downTimeMs > 220)
      return false;
    return (releasePos - state.startPos).magnitude() <= tapMovementThreshold();
  }

  void trackTapMotion(FingerState& state, Vec2F const& pos) {
    state.currentPos = pos;
    if (!state.movedTooFarForTap && (pos - state.startPos).magnitude() > tapMovementThreshold())
      state.movedTooFarForTap = true;
  }

  Vec2F toScreen(float x, float y) const {
    // SDL finger events can be normalized [0..1] or already in render-space
    // depending on conversion/platform path.
    Vec2F pos;
    if (x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f) {
      pos = {
        x * (float)(*m_windowSize)[0],
        y * (float)(*m_windowSize)[1]
      };
    } else if (ImGui::GetCurrentContext()) {
      ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      Vec2F drawScale = physicalToDrawScale();
      if (displaySize.x > 0.0f && displaySize.y > 0.0f && (drawScale[0] > 1.0f || drawScale[1] > 1.0f)
          && x >= 0.0f && x <= displaySize.x && y >= 0.0f && y <= displaySize.y) {
        pos = { x * drawScale[0], y * drawScale[1] };
      } else {
        pos = { x, y };
      }
    } else {
      pos = { x, y };
    }

    // Translate from full-screen physical coords to game-canvas coords by
    // subtracting the safe-area insets (top-left origin offset).
    if (m_safeAreaPtr) {
      pos[0] -= (float)m_safeAreaPtr->left;
      pos[1] -= (float)m_safeAreaPtr->top;
    }
    return pos;
  }

  Vec2F toInputSpace(Vec2F const& pos) const {
    Vec2F physicalCanvas = canvasSize();
    Vec2F renderCanvas = m_renderCanvasSize ? Vec2F(*m_renderCanvasSize) : physicalCanvas;
    Vec2F scaled{
      physicalCanvas[0] > 0.0f ? pos[0] * renderCanvas[0] / physicalCanvas[0] : pos[0],
      physicalCanvas[1] > 0.0f ? pos[1] * renderCanvas[1] / physicalCanvas[1] : pos[1]
    };

    // Y-flip: game input space has y=0 at bottom; screen space has y=0 at top.
    // Use render-canvas height to match the renderer's logical resolution.
    return {scaled[0], renderCanvas[1] - scaled[1]};
  }

  Vec2F toScreenSpace(Vec2F const& pos) const {
    Vec2F physicalCanvas = canvasSize();
    Vec2F renderCanvas = m_renderCanvasSize ? Vec2F(*m_renderCanvasSize) : physicalCanvas;
    return {
      renderCanvas[0] > 0.0f ? pos[0] * physicalCanvas[0] / renderCanvas[0] : pos[0],
      renderCanvas[1] > 0.0f ? (renderCanvas[1] - pos[1]) * physicalCanvas[1] / renderCanvas[1] : pos[1]
    };
  }

  Vec2F inputCanvasSize() const {
    return m_renderCanvasSize ? Vec2F(*m_renderCanvasSize) : canvasSize();
  }

  void assignFinger(uint64_t finger, Vec2F const& pos) {
    if (auto old = m_fingers.ptr(finger)) {
      Vec2F oldPos = old->currentPos;
      releaseFinger(finger, oldPos, true);
    }

    m_lastPointerInput = toInputSpace(pos);
    float radius = controlRadius();

    FingerState state;
    state.startPos = pos;
    state.currentPos = pos;
    state.downTimeMs = Time::monotonicMilliseconds();

    bool claimedControl = false;
    for (auto const& element : m_elements) {
      if (!element.enabled)
        continue;

      Vec2F center = elementCenter(element);
      float elementRadius = radius * std::clamp(element.size, 0.45f, 2.4f);
      if (element.kind == MobileTouchElementKind::Button && insideCircle(pos, center, elementRadius * 0.70f)) {
        state.role = FingerRole::ActionButton;
        state.elementId = element.id;
        state.action = element.action;
        state.pressMode = element.pressMode;
        pressActionButton(element);
        claimedControl = true;
        break;
      } else if (element.kind == MobileTouchElementKind::DPad && insideCircle(pos, center, elementRadius * 1.15f)) {
        state.role = FingerRole::DPad;
        state.elementId = element.id;
        updateDPadFinger(state, pos);
        claimedControl = true;
        break;
      } else if (element.kind == MobileTouchElementKind::Joystick && insideCircle(pos, center, elementRadius * 1.25f) && !m_joystickActive) {
        state.role = FingerRole::Joystick;
        state.elementId = element.id;
        m_joystickActive = true;
        m_joystickFinger = finger;
        m_joystickElementId = element.id;
        m_joystickOrigin = center;
        m_joystickCurrent = pos;
        m_moveVec = {};
        claimedControl = true;
        break;
      } else if (element.kind == MobileTouchElementKind::AimJoystick && insideCircle(pos, center, elementRadius * 1.25f) && !m_aimJoystickActive) {
        state.role = FingerRole::AimJoystick;
        state.elementId = element.id;
        m_aimJoystickActive = true;
        m_aimJoystickFinger = finger;
        m_aimJoystickElementId = element.id;
        m_aimJoystickOrigin = center;
        m_aimJoystickCurrent = pos;
        ensureAimTarget();
        updateAimJoystickFinger(state, pos);
        claimedControl = true;
        break;
      }
    }

    if (claimedControl) {
      // already assigned above
    } else if (!m_config.directTouchGestures) {
      state.role = FingerRole::None;
      state.movedTooFarForTap = true;
    } else if (m_joystickActive && insideCircle(pos, m_joystickOrigin, radius * 1.35f)) {
      // Prevent accidental aiming while thumb rides around the virtual joystick.
      // Quick taps in this region still become UI clicks on release.
      state.role = FingerRole::SuppressedTap;
    } else if (m_primaryHeld && m_aimFinger != finger && !m_secondaryHeld) {
      state.role = FingerRole::SecondaryHold;
      m_secondaryHeld = true;
      m_secondaryFinger = finger;
      m_secondaryTouchPos = m_primaryHeld ? m_primaryTouchPos : pos;

      // Let the two-finger gesture's action take precedence while held.
      if (m_primaryMouseHeld) {
        releaseDirectGestureAction(m_config.directTouchSingleAction, DirectTouchSingleOwner);
        m_primaryMouseHeld = false;
        m_primaryPausedForSecondary = true;
      }

      m_secondaryMouseHeld = true;
      emitMouseMove(m_secondaryTouchPos);
      pressDirectGestureAction(m_config.directTouchTwoFingerAction, DirectTouchTwoFingerOwner);
    } else if (m_primaryHeld) {
      // A third touch should not steal ownership of the primary attack/aim
      // button or synthesize a click while the primary button is still held.
      state.role = FingerRole::SuppressedTap;
      state.movedTooFarForTap = true;
    } else {
      state.role = FingerRole::Aim;
      m_aimFinger = finger;
      m_primaryHeld = true;
      m_primaryMouseHeld = true;
      // Cursor tracking always happens regardless of what action is
      // configured below: moving your finger while touching aims the
      // reticle, independent of what fires on press.
      if (m_config.directTouchGestureMode == DirectTouchGestureMode::Touchpad) {
        // Touchpad mode: the cursor stays put on finger-down and only moves
        // by the swipe delta from here (see updateFinger), instead of
        // warping to the finger's position like touchscreen mode does.
        Vec2F cursorPos = m_hasCursorInputPosition ? toScreenSpace(m_cursorInputPosition) : canvasSize() * 0.5f;
        state.touchpadAnchor = pos;
        m_primaryTouchPos = cursorPos;
        emitMouseMove(cursorPos);
      } else {
        m_primaryTouchPos = pos;
        emitMouseMove(pos);
      }
      pressDirectGestureAction(m_config.directTouchSingleAction, DirectTouchSingleOwner);
    }

    m_fingers[finger] = state;
  }

  void updateFinger(uint64_t finger, Vec2F const& pos) {
    auto ptr = m_fingers.ptr(finger);
    if (!ptr)
      return;

    m_lastPointerInput = toInputSpace(pos);
    trackTapMotion(*ptr, pos);

    if (ptr->role == FingerRole::Joystick) {
      m_joystickCurrent = pos;
      float radius = controlRadius();
      if (auto element = findElement(ptr->elementId))
        radius *= std::clamp(element->size, 0.45f, 2.4f);
      Vec2F delta = pos - m_joystickOrigin;
      float mag = delta.magnitude();
      if (mag > radius)
        delta = delta / mag * radius;

      float normMag = delta.magnitude() / radius;
      if (normMag < m_config.deadzone)
        m_moveVec = {};
      else
        m_moveVec = delta / radius;
    } else if (ptr->role == FingerRole::AimJoystick) {
      updateAimJoystickFinger(*ptr, pos);
    } else if (ptr->role == FingerRole::Aim) {
      if (m_config.directTouchGestureMode == DirectTouchGestureMode::Touchpad) {
        Vec2F delta = pos - ptr->touchpadAnchor;
        ptr->touchpadAnchor = pos;
        Vec2F canvas = canvasSize();
        m_primaryTouchPos = Vec2F(
          std::clamp(m_primaryTouchPos[0] + delta[0], 0.0f, canvas[0]),
          std::clamp(m_primaryTouchPos[1] + delta[1], 0.0f, canvas[1]));
      } else {
        m_primaryTouchPos = pos;
      }
      emitMouseMove(m_primaryTouchPos);
    } else if (ptr->role == FingerRole::SecondaryHold && m_secondaryMouseHeld) {
      m_secondaryTouchPos = m_primaryHeld ? m_primaryTouchPos : pos;
      emitMouseMove(m_secondaryTouchPos);
    } else if (ptr->role == FingerRole::DPad) {
      updateDPadFinger(*ptr, pos);
    }
  }

  void releaseFinger(uint64_t finger, Vec2F const& pos, bool canceled = false) {
    auto ptr = m_fingers.ptr(finger);
    if (!ptr)
      return;

    bool tap = !canceled && isTap(*ptr, pos);

    switch (ptr->role) {
      case FingerRole::Joystick:
        if (tap && m_moveVec.magnitudeSquared() <= m_config.deadzone * m_config.deadzone)
          emitMouseClick(pos);
        m_joystickActive = false;
        m_joystickElementId.clear();
        m_moveVec = {};
        m_joystickFinger = 0;
        break;
      case FingerRole::AimJoystick:
        m_aimJoystickActive = false;
        m_aimJoystickElementId.clear();
        m_aimJoystickFinger = 0;
        m_aimVec = {};
        break;
      case FingerRole::Aim:
        if (m_primaryMouseHeld) {
          // The cursor position was already kept in sync via emitMouseMove
          // during updateFinger, so releasing here reads that tracked
          // position rather than the raw release pos (in touchpad mode they
          // can differ significantly -- cursor position is delta-tracked,
          // not 1:1 with the finger).
          releaseDirectGestureAction(m_config.directTouchSingleAction, DirectTouchSingleOwner);
          m_primaryMouseHeld = false;
        }
        m_primaryHeld = false;
        m_aimFinger = 0;
        break;
      case FingerRole::SuppressedTap:
        if (tap)
          emitMouseClick(pos);
        break;
      case FingerRole::SecondaryHold:
        if (m_secondaryMouseHeld && m_secondaryFinger == finger) {
          m_secondaryTouchPos = m_primaryHeld ? m_primaryTouchPos : pos;
          emitMouseMove(m_secondaryTouchPos);
          releaseDirectGestureAction(m_config.directTouchTwoFingerAction, DirectTouchTwoFingerOwner);
          m_secondaryMouseHeld = false;
        } else if (tap) {
          Vec2F target = m_primaryHeld ? m_primaryTouchPos : pos;
          emitMouseMove(target);
          emitMouseClick(target, MouseButton::Right);
        }
        m_secondaryHeld = false;
        m_secondaryFinger = 0;

        if (m_primaryPausedForSecondary && m_primaryHeld && !m_primaryMouseHeld) {
          emitMouseMove(m_primaryTouchPos);
          pressDirectGestureAction(m_config.directTouchSingleAction, DirectTouchSingleOwner);
          m_primaryMouseHeld = true;
        }
        m_primaryPausedForSecondary = false;
        break;
      case FingerRole::ActionButton:
        releaseActionButton(*ptr);
        break;
      case FingerRole::DPad:
        clearDPad(ptr->elementId);
        break;
      default:
        break;
    }

    m_fingers.remove(finger);
  }

  void cancelFinger(uint64_t finger) {
    if (auto ptr = m_fingers.ptr(finger))
      releaseFinger(finger, ptr->currentPos, true);
  }

  void cancelDirectTouchGestures() {
    if (m_primaryMouseHeld) {
      releaseDirectGestureAction(m_config.directTouchSingleAction, DirectTouchSingleOwner);
      m_primaryMouseHeld = false;
    }
    if (m_secondaryMouseHeld) {
      releaseDirectGestureAction(m_config.directTouchTwoFingerAction, DirectTouchTwoFingerOwner);
      m_secondaryMouseHeld = false;
    }

    m_primaryHeld = false;
    m_aimFinger = 0;
    m_primaryPausedForSecondary = false;
    m_secondaryHeld = false;
    m_secondaryFinger = 0;

    for (auto& pair : m_fingers)
      if (pair.second.role == FingerRole::Aim || pair.second.role == FingerRole::SecondaryHold || pair.second.role == FingerRole::SuppressedTap)
        pair.second.role = FingerRole::None;
  }

  MobileTouchAction dpadAction(MobileTouchElement const& element, String const& direction) const {
    if (direction == "up")
      return element.upAction;
    if (direction == "down")
      return element.downAction;
    if (direction == "left")
      return element.leftAction;
    if (direction == "right")
      return element.rightAction;
    return {};
  }

  String dpadDirection(MobileTouchElement const& element, Vec2F const& pos) const {
    Vec2F delta = pos - elementCenter(element);
    float radius = controlRadius() * std::clamp(element.size, 0.45f, 2.4f);
    if (delta.magnitude() < radius * 0.25f)
      return {};
    if (std::abs(delta[0]) > std::abs(delta[1]))
      return delta[0] > 0.0f ? "right" : "left";
    return delta[1] > 0.0f ? "down" : "up";
  }

  void updateDPadFinger(FingerState& state, Vec2F const& pos) {
    auto element = findElement(state.elementId);
    if (!element)
      return;

    String direction = dpadDirection(*element, pos);
    String activeId = direction.empty() ? String() : state.elementId + ":" + direction;
    for (auto const& candidate : {"up", "down", "left", "right"}) {
      String id = state.elementId + ":" + candidate;
      if (id != activeId && m_dpadHeld.contains(id)) {
        m_dpadHeld.remove(id);
        setAction(dpadAction(*element, candidate), id, false);
      }
    }

    if (!activeId.empty() && !m_dpadHeld.contains(activeId)) {
      m_dpadHeld.add(activeId);
      MobileTouchAction action = dpadAction(*element, direction);
      setAction(action, activeId, true);
      if (action.kind == MobileTouchActionKind::MouseWheelUp || action.kind == MobileTouchActionKind::MouseWheelDown)
        m_nextDPadWheelMs[activeId] = Time::monotonicMilliseconds() + 180;
    }
  }

  void updateAimJoystickFinger(FingerState& state, Vec2F const& pos) {
    state.currentPos = pos;
    float radius = controlRadius();
    if (auto element = findElement(state.elementId))
      radius *= std::clamp(element->size, 0.45f, 2.4f);

    Vec2F delta = pos - m_aimJoystickOrigin;
    float mag = delta.magnitude();
    if (mag > radius)
      delta = delta / mag * radius;

    m_aimJoystickCurrent = m_aimJoystickOrigin + delta;
    float normMag = delta.magnitude() / radius;
    if (normMag < m_config.deadzone)
      m_aimVec = {};
    else
      m_aimVec = delta / radius;
  }

  void clearDPad(String const& elementId) {
    if (auto element = findElement(elementId)) {
      for (auto const& candidate : {"up", "down", "left", "right"}) {
        String id = elementId + ":" + candidate;
        if (m_dpadHeld.contains(id)) {
          m_dpadHeld.remove(id);
          setAction(dpadAction(*element, candidate), id, false);
        }
      }
    }
  }

  void repeatDPadWheelActions() {
    int64_t now = Time::monotonicMilliseconds();
    for (auto const& held : m_dpadHeld.values()) {
      auto next = m_nextDPadWheelMs.value(held, 0);
      if (next > now)
        continue;

      auto pieces = held.split(":");
      if (pieces.size() != 2)
        continue;
      if (auto element = findElement(pieces[0])) {
        MobileTouchAction action = dpadAction(*element, pieces[1]);
        if (action.kind == MobileTouchActionKind::MouseWheelUp || action.kind == MobileTouchActionKind::MouseWheelDown) {
          setAction(action, held, true);
          m_nextDPadWheelMs[held] = now + 130;
        }
      }
    }
  }

  void emitActionEdges() {
    setKeyOwner("joystick:right", Key::D, m_moveVec[0] > 0.30f);
    setKeyOwner("joystick:left", Key::A, m_moveVec[0] < -0.30f);
    setKeyOwner("joystick:up", Key::W, m_moveVec[1] < -0.30f);
    setKeyOwner("joystick:down", Key::S, m_moveVec[1] > 0.30f);
    updateVirtualAimTarget();
    releaseExpiredPulsedActions();
    processMacroEvents();
    repeatActionButtons();
    repeatDPadWheelActions();

    if (m_primaryHeld && !m_primaryMouseHeld && !m_secondaryMouseHeld) {
      m_primaryMouseHeld = true;
      emitMouseMove(m_primaryTouchPos);
      emitMouseDown(m_primaryTouchPos);
    } else if (!m_primaryHeld && m_primaryMouseHeld) {
      m_primaryMouseHeld = false;
      emitMouseUp(m_primaryTouchPos);
    }
  }

  void ensureAimTarget() {
    if (m_hasAimJoystickTarget)
      return;

    Vec2F canvas = canvasSize();
    if (m_hasCursorInputPosition)
      m_aimJoystickTarget = toScreenSpace(m_cursorInputPosition);
    else
      m_aimJoystickTarget = {canvas[0] * 0.5f, canvas[1] * 0.5f};
    m_hasAimJoystickTarget = true;
  }

  bool aimJoystickPrecise() const {
    if (auto element = findElement(m_aimJoystickElementId))
      return element->preciseAim;
    return false;
  }

  bool directionalAimActive() const {
    return m_aimJoystickActive && !aimJoystickPrecise();
  }

  float directionalAimRadius() const {
    Vec2F canvas = inputCanvasSize();
    return std::max(1.0f, std::min(canvas[0], canvas[1]) * 0.32f);
  }

  void emitDirectionalAim(Vec2F const& screenDirection, float deflection, bool cursorVisible) {
    Vec2F inputDirection{screenDirection[0], -screenDirection[1]};
    emitEvent(DirectionalAimEvent{inputDirection * deflection, directionalAimRadius(), cursorVisible});
  }

  Vec2F screenGyroVelocity() const {
    float gx = m_gyroInput[0];
    float gy = m_gyroInput[1];

    switch (m_gyroOrientation) {
      case SDL_ORIENTATION_LANDSCAPE:
        return {-gy, gx};
      case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
        return {gy, -gx};
      case SDL_ORIENTATION_PORTRAIT_FLIPPED:
        return {-gx, -gy};
      case SDL_ORIENTATION_PORTRAIT:
      case SDL_ORIENTATION_UNKNOWN:
      default:
        return {gx, gy};
    }
  }

  Vec2F gyroAimDelta(int64_t now) {
    if (!m_config.gyroEnabled || !m_gyroRuntimeEnabled || !m_hasGyroInput || now - m_lastGyroInputMs > 90)
      return {};
    if (m_primaryHeld || m_secondaryHeld) {
      m_lastGyroFrameMs = now;
      return {};
    }

    if (m_lastGyroFrameMs == 0) {
      m_lastGyroFrameMs = now;
      return {};
    }

    float dt = std::clamp((float)(now - m_lastGyroFrameMs) / 1000.0f, 0.0f, 0.05f);
    m_lastGyroFrameMs = now;
    if (dt <= 0.0f)
      return {};

    Vec2F omega = screenGyroVelocity();
    float deadzone = 0.015f;
    if (std::abs(omega[0]) < deadzone)
      omega[0] = 0.0f;
    if (std::abs(omega[1]) < deadzone)
      omega[1] = 0.0f;
    if (omega.magnitudeSquared() <= 0.0001f)
      return {};

    float pixelsPerRadian = controlRadius() * 5.0f * std::clamp(m_config.gyroSensitivity, 0.10f, 12.0f);
    Vec2F delta{-omega[1] * pixelsPerRadian * dt, omega[0] * pixelsPerRadian * dt};
    if (m_config.gyroInvertX)
      delta[0] = -delta[0];
    if (m_config.gyroInvertY)
      delta[1] = -delta[1];
    return delta;
  }

  void updateVirtualAimTarget() {
    int64_t now = Time::monotonicMilliseconds();
    Vec2F delta;

    if (m_aimJoystickActive && m_aimVec.magnitudeSquared() > 0.0001f) {
      float sensitivity = 1.0f;
      if (auto element = findElement(m_aimJoystickElementId))
        sensitivity = std::clamp(element->aimSensitivity, 0.25f, 4.0f);
      if (!aimJoystickPrecise()) {
        float deflection = std::clamp(m_aimVec.magnitude(), 0.0f, 1.0f);
        Vec2F direction = m_aimVec / std::max(0.0001f, m_aimVec.magnitude());
        emitDirectionalAim(direction, deflection, m_config.joystickAimCursorEnabled);
        return;
      } else {
        float speed = controlRadius() * 0.22f * sensitivity * std::clamp(m_aimVec.magnitude(), 0.25f, 1.0f);
        delta += m_aimVec * speed;
      }
    }

    delta += gyroAimDelta(now);
    if (delta.magnitudeSquared() <= 0.0001f)
      return;

    ensureAimTarget();
    Vec2F canvas = canvasSize();
    m_aimJoystickTarget += delta;
    m_aimJoystickTarget[0] = std::clamp(m_aimJoystickTarget[0], 0.0f, canvas[0]);
    m_aimJoystickTarget[1] = std::clamp(m_aimJoystickTarget[1], 0.0f, canvas[1]);
    m_hasAimJoystickTarget = true;
    syncVirtualAimCursor(true, m_config.joystickAimCursorEnabled);
  }

  void repeatActionButtons() {
    int64_t now = Time::monotonicMilliseconds();
    for (auto const& pair : m_fingers.pairs()) {
      auto const& state = pair.second;
      if (state.role != FingerRole::ActionButton || state.pressMode != MobileTouchPressMode::Repeat)
        continue;
      auto next = m_nextActionRepeatMs.value(state.elementId, 0);
      if (next > now)
        continue;
      startPulsedAction(state.action, state.elementId);
      m_nextActionRepeatMs[state.elementId] = now + 110;
    }
  }

  void pressActionButton(MobileTouchElement const& element) {
    m_heldElements.add(element.id);
    if (element.action.kind == MobileTouchActionKind::GyroToggle) {
      if (m_gyroAvailable && m_config.gyroEnabled) {
        m_gyroRuntimeEnabled = !m_gyroRuntimeEnabled;
        m_lastGyroFrameMs = 0;
        if (m_gyroRuntimeEnabled)
          ensureAimTarget();
      }
      return;
    }

    if (element.pressMode == MobileTouchPressMode::Toggle) {
      if (m_toggledElements.contains(element.id)) {
        m_toggledElements.remove(element.id);
        setAction(element.action, element.id, false);
      } else {
        m_toggledElements.add(element.id);
        setAction(element.action, element.id, true);
      }
    } else if (element.pressMode == MobileTouchPressMode::SinglePress) {
      startPulsedAction(element.action, element.id);
    } else if (element.pressMode == MobileTouchPressMode::Repeat) {
      startPulsedAction(element.action, element.id);
      m_nextActionRepeatMs[element.id] = Time::monotonicMilliseconds() + 110;
    } else {
      setAction(element.action, element.id, true);
    }
  }

  void releaseActionButton(FingerState const& state) {
    m_heldElements.remove(state.elementId);
    if (state.pressMode == MobileTouchPressMode::Hold)
      setAction(state.action, state.elementId, false);
    else if (state.pressMode == MobileTouchPressMode::Repeat) {
      m_nextActionRepeatMs.remove(state.elementId);
      cancelPulsedAction(state.elementId);
    }
  }

  // Owner tokens for the two direct-touch gestures (single-finger "Aim",
  // two-finger "SecondaryHold"): distinct from any element id since these
  // gestures aren't backed by a configured MobileTouchElement.
  static constexpr char const* DirectTouchSingleOwner = "directTouchSingle";
  static constexpr char const* DirectTouchTwoFingerOwner = "directTouchTwoFinger";

  // Press/release for the two configurable direct-touch gesture actions.
  // Mirrors pressActionButton/releaseActionButton's Hold-mode + GyroToggle
  // special case, but takes the action directly since there's no backing
  // element -- these gestures are always "press on finger-down, release on
  // finger-up" (no press-mode picker; that matches how they behaved before
  // being made configurable).
  void pressDirectGestureAction(MobileTouchAction const& action, char const* owner) {
    if (action.kind == MobileTouchActionKind::GyroToggle) {
      if (m_gyroAvailable && m_config.gyroEnabled) {
        m_gyroRuntimeEnabled = !m_gyroRuntimeEnabled;
        m_lastGyroFrameMs = 0;
        if (m_gyroRuntimeEnabled)
          ensureAimTarget();
      }
      return;
    }
    setAction(action, owner, true);
  }

  void releaseDirectGestureAction(MobileTouchAction const& action, char const* owner) {
    if (action.kind == MobileTouchActionKind::GyroToggle)
      return;
    setAction(action, owner, false);
  }

  void setKeyOwner(String const& owner, Key key, bool desired) {
    String token = owner + ":" + KeyNames.getRight(key);

    if (desired && !m_keyActionOwners.contains(token)) {
      m_keyActionOwners.add(token);
      unsigned count = m_keyHoldCounts.value(key, 0);
      m_keyHoldCounts.set(key, count + 1);
      if (count == 0)
        emitEvent(KeyDownEvent{key, noMods()});
    } else if (!desired && m_keyActionOwners.contains(token)) {
      m_keyActionOwners.remove(token);
      unsigned count = m_keyHoldCounts.value(key, 0);
      if (count <= 1) {
        m_keyHoldCounts.remove(key);
        emitEvent(KeyUpEvent{key});
      } else {
        m_keyHoldCounts.set(key, count - 1);
      }
    }
  }

  void setMouseOwner(String const& owner, MouseButton button, bool desired) {
    String token = owner + ":" + MouseButtonNames.getRight(button);

    if (desired && !m_mouseActionOwners.contains(token)) {
      m_mouseActionOwners.add(token);
      unsigned count = m_mouseHoldCounts.value(button, 0);
      m_mouseHoldCounts.set(button, count + 1);
      if (count == 0) {
        if (directionalAimActive())
          updateVirtualAimTarget();
        else
          syncVirtualAimCursor(false, m_config.joystickAimCursorEnabled);
        emitEvent(MouseButtonDownEvent{button, mouseActionPosition()});
      }
    } else if (!desired && m_mouseActionOwners.contains(token)) {
      m_mouseActionOwners.remove(token);
      unsigned count = m_mouseHoldCounts.value(button, 0);
      if (count <= 1) {
        m_mouseHoldCounts.remove(button);
        if (directionalAimActive())
          updateVirtualAimTarget();
        else
          syncVirtualAimCursor(false, m_config.joystickAimCursorEnabled);
        emitEvent(MouseButtonUpEvent{button, mouseActionPosition()});
      } else {
        m_mouseHoldCounts.set(button, count - 1);
      }
    }
  }

  void setAction(MobileTouchAction const& action, String const& owner, bool desired) {
    if (action.kind == MobileTouchActionKind::Key) {
      setKeyOwner(owner, action.key, desired);
    } else if (action.kind == MobileTouchActionKind::KeyMacro) {
      if (action.macroSequential) {
        // Sequence macro: on trigger, schedule an ordered run of key taps.
        // Releasing is a no-op — the run completes on its own timeline.
        if (desired && !action.keys.empty() && !hasActiveMacro(owner))
          enqueueMacro(action.keys, owner);
      } else {
        // Chord macro: hold every key down together for the button's lifetime.
        for (auto key : action.keys)
          setKeyOwner(owner, key, desired);
      }
    } else if (action.kind == MobileTouchActionKind::MouseButton) {
      setMouseOwner(owner, action.mouseButton, desired);
    } else if (desired && action.kind == MobileTouchActionKind::MouseWheelUp) {
      syncVirtualAimCursor(false, m_config.joystickAimCursorEnabled);
      emitEvent(MouseWheelEvent{MouseWheel::Up, mouseActionPosition()});
    } else if (desired && action.kind == MobileTouchActionKind::MouseWheelDown) {
      syncVirtualAimCursor(false, m_config.joystickAimCursorEnabled);
      emitEvent(MouseWheelEvent{MouseWheel::Down, mouseActionPosition()});
    }
  }

  bool actionNeedsRelease(MobileTouchAction const& action) const {
    // Sequence macros self-terminate, so they don't need a paired release.
    if (action.kind == MobileTouchActionKind::KeyMacro)
      return !action.macroSequential;
    return action.kind == MobileTouchActionKind::Key || action.kind == MobileTouchActionKind::MouseButton;
  }

  // --- Sequential macro playback ---------------------------------------------
  // A macro run is expanded into a list of timed key up/down events. Each key in
  // the sequence is pressed for MacroKeyHoldMs then released, with MacroKeyGapMs
  // before the next key. Every key instance gets a unique owner token so it plays
  // through setKeyOwner's reference counting and coexists with other held keys.
  static constexpr int64_t MacroKeyHoldMs = 45;
  static constexpr int64_t MacroKeyGapMs = 55;

  int64_t macroRunDurationMs(size_t keyCount) const {
    return (int64_t)keyCount * (MacroKeyHoldMs + MacroKeyGapMs);
  }

  bool hasActiveMacro(String const& owner) const {
    String prefix = owner + "#macro";
    for (auto const& event : m_macroEvents) {
      if (event.ownerToken.beginsWith(prefix))
        return true;
    }
    return false;
  }

  void enqueueMacro(List<Key> const& keys, String const& owner) {
    int64_t base = Time::monotonicMilliseconds();
    int64_t instance = ++m_macroInstanceCounter;
    int64_t cursor = base;
    size_t step = 0;
    for (auto key : keys) {
      String token = strf("{}#macro{}#{}", owner, instance, step);
      m_macroEvents.append(MacroKeyEvent{key, true, cursor, token});
      m_macroEvents.append(MacroKeyEvent{key, false, cursor + MacroKeyHoldMs, token});
      cursor += MacroKeyHoldMs + MacroKeyGapMs;
      ++step;
    }
  }

  void processMacroEvents() {
    if (m_macroEvents.empty())
      return;

    int64_t now = Time::monotonicMilliseconds();
    List<MacroKeyEvent> pending;
    for (auto const& event : m_macroEvents) {
      if (event.atMs <= now)
        setKeyOwner(event.ownerToken, event.key, event.down);
      else
        pending.append(event);
    }
    m_macroEvents = std::move(pending);
  }

  void startPulsedAction(MobileTouchAction const& action, String const& owner) {
    setAction(action, owner, true);
    if (!actionNeedsRelease(action))
      return;

    m_pulsedActions[owner] = action;
    m_pulsedActionReleaseMs[owner] = Time::monotonicMilliseconds() + 55;
  }

  void cancelPulsedAction(String const& owner) {
    if (auto action = m_pulsedActions.ptr(owner))
      setAction(*action, owner, false);
    m_pulsedActions.remove(owner);
    m_pulsedActionReleaseMs.remove(owner);
  }

  void releaseExpiredPulsedActions() {
    int64_t now = Time::monotonicMilliseconds();
    StringList expired;
    for (auto const& pair : m_pulsedActionReleaseMs.pairs()) {
      if (pair.second <= now)
        expired.append(pair.first);
    }

    for (auto const& owner : expired)
      cancelPulsedAction(owner);
  }

  void clearPulsedActions() {
    StringList owners;
    for (auto const& pair : m_pulsedActions.pairs())
      owners.append(pair.first);

    for (auto const& owner : owners)
      cancelPulsedAction(owner);
  }

  void syncVirtualAimCursor(bool force = false, bool cursorVisible = true) {
    if (!m_hasAimJoystickTarget)
      return;

    Vec2F inputPosition = toInputSpace(m_aimJoystickTarget);
    if (!force && m_hasCursorInputPosition && (inputPosition - m_cursorInputPosition).magnitudeSquared() <= 0.01f)
      return;

    m_cursorInputPosition = inputPosition;
    m_hasCursorInputPosition = true;
    emitEvent(MouseMoveEvent{{0, 0}, m_cursorInputPosition, cursorVisible});
  }

  Vec2F mouseActionPosition() const {
    return m_hasCursorInputPosition ? m_cursorInputPosition : m_lastPointerInput;
  }

  void emitMouseMove(Vec2F const& pos) {
    m_cursorInputPosition = toInputSpace(pos);
    m_hasCursorInputPosition = true;
    m_aimJoystickTarget = pos;
    m_hasAimJoystickTarget = true;
    emitEvent(MouseMoveEvent{{0, 0}, m_cursorInputPosition, true});
  }

  void emitMouseDown(Vec2F const& pos, MouseButton button = MouseButton::Left) {
    emitEvent(MouseButtonDownEvent{button, toInputSpace(pos)});
  }

  void emitMouseUp(Vec2F const& pos, MouseButton button = MouseButton::Left) {
    emitEvent(MouseButtonUpEvent{button, toInputSpace(pos)});
  }

  void emitMouseClick(Vec2F const& pos, MouseButton button = MouseButton::Left) {
    emitMouseDown(pos, button);
    emitMouseUp(pos, button);
  }

  void emitEvent(InputEvent const& event) {
    m_generatedEvents.append(event);
  }

  // A deferred label to be drawn (with a contrast outline) after the inversion
  // blended shape pass has completed. Text can't use the inversion blend, so its
  // draw commands must land under ImGui's default blend state.
  struct OverlayLabel {
    ImVec2 pos;
    String text;
  };

  // ImGui render callback that switches OpenGL to the inversion (difference)
  // blend used for the always-visible button geometry. This is the same blend
  // Minecraft uses for its crosshair: result.rgb = src*(1-dst) + dst*(1-src),
  // which for a white source yields 1-dst, the negative of the background.
  static void applyContrastBlendCallback(ImDrawList const*, ImDrawCmd const*) {
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ZERO);
  }

  // Draws text with a solid dark outline behind a light glyph so labels stay
  // legible on any background (light glyph shows on dark scenery, dark outline
  // shows on light scenery) — the touch-control analogue of Minecraft's
  // drop-shadowed HUD text.
  static void drawLabelWithOutline(ImDrawList* draw, ImVec2 pos, char const* text, ImU32 color, ImU32 outline) {
    static ImVec2 const offsets[8] = {
      {-1.0f, -1.0f}, {0.0f, -1.0f}, {1.0f, -1.0f},
      {-1.0f,  0.0f},                {1.0f,  0.0f},
      {-1.0f,  1.0f}, {0.0f,  1.0f}, {1.0f,  1.0f}
    };
    for (auto const& o : offsets)
      draw->AddText(ImVec2(pos.x + o.x, pos.y + o.y), outline, text);
    draw->AddText(pos, color, text);
  }

  static void drawButton(ImDrawList* draw, ImVec2 center, float radius, bool held, char const* label, ImU32 base, ImU32 fill, float thickness, std::vector<OverlayLabel>& labels) {
    draw->AddCircle(center, radius, base, 48, thickness);
    if (held)
      draw->AddCircleFilled(center, radius, fill, 32);
    labels.push_back({ImVec2(center.x - radius * 0.2f, center.y - radius * 0.35f), label});
  }

  void drawDPad(ImDrawList* draw, ImVec2 center, float radius, String const& id, ImU32 base, ImU32 fill, float thickness, std::vector<OverlayLabel>& labels) const {
    float arm = radius * 0.42f;
    auto drawArm = [&](char const* suffix, ImVec2 c, char const* label) {
      String heldId = id + ":" + suffix;
      bool held = m_dpadHeld.contains(heldId);
      draw->AddRect(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), base, arm * 0.25f, 0, thickness);
      if (held)
        draw->AddRectFilled(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), fill, arm * 0.25f);
      labels.push_back({ImVec2(c.x - arm * 0.22f, c.y - arm * 0.38f), label});
    };

    drawArm("up", ImVec2(center.x, center.y - arm * 1.25f), launcherTextStatic("touchElement.dpadUp", "W").utf8Ptr());
    drawArm("down", ImVec2(center.x, center.y + arm * 1.25f), launcherTextStatic("touchElement.dpadDown", "S").utf8Ptr());
    drawArm("left", ImVec2(center.x - arm * 1.25f, center.y), launcherTextStatic("touchElement.dpadLeft", "A").utf8Ptr());
    drawArm("right", ImVec2(center.x + arm * 1.25f, center.y), launcherTextStatic("touchElement.dpadRight", "D").utf8Ptr());
  }

  Vec2U* m_windowSize;
  Vec2U* m_renderCanvasSize = nullptr;
  [[maybe_unused]] float* m_displayScalePtr = nullptr;
  SafeAreaInsets* m_safeAreaPtr = nullptr;
  MobileTouchConfig m_config;
  std::vector<MobileTouchElement> m_elements = defaultTouchElements();
  bool m_gyroAvailable = true;
  List<InputEvent> m_generatedEvents;

  StableHashMap<uint64_t, FingerState> m_fingers;
  StringSet m_heldElements;
  StringSet m_toggledElements;
  StringSet m_dpadHeld;
  StringMap<int64_t> m_nextDPadWheelMs;
  StringMap<int64_t> m_nextActionRepeatMs;
  StringMap<MobileTouchAction> m_pulsedActions;
  StringMap<int64_t> m_pulsedActionReleaseMs;
  StringSet m_keyActionOwners;
  HashMap<Key, unsigned> m_keyHoldCounts;
  StringSet m_mouseActionOwners;
  HashMap<MouseButton, unsigned> m_mouseHoldCounts;

  struct MacroKeyEvent {
    Key key;
    bool down;
    int64_t atMs;
    String ownerToken;
  };
  List<MacroKeyEvent> m_macroEvents;
  int64_t m_macroInstanceCounter = 0;

  bool m_joystickActive = false;
  uint64_t m_joystickFinger = 0;
  uint64_t m_aimFinger = 0;
  String m_joystickElementId;
  Vec2F m_joystickOrigin;
  Vec2F m_joystickCurrent;
  Vec2F m_moveVec;

  bool m_aimJoystickActive = false;
  uint64_t m_aimJoystickFinger = 0;
  String m_aimJoystickElementId;
  Vec2F m_aimJoystickOrigin;
  Vec2F m_aimJoystickCurrent;
  Vec2F m_aimJoystickTarget;
  Vec2F m_aimVec;
  bool m_hasAimJoystickTarget = false;
  std::array<float, 3> m_gyroInput{};
  bool m_hasGyroInput = false;
  bool m_gyroRuntimeEnabled = false;
  int64_t m_lastGyroInputMs = 0;
  int64_t m_lastGyroFrameMs = 0;
  SDL_DisplayOrientation m_gyroOrientation = SDL_ORIENTATION_UNKNOWN;

  bool m_primaryHeld = false;
  bool m_primaryMouseHeld = false;
  Vec2F m_primaryTouchPos;
  bool m_primaryPausedForSecondary = false;

  bool m_secondaryHeld = false;
  bool m_secondaryMouseHeld = false;
  uint64_t m_secondaryFinger = 0;
  Vec2F m_secondaryTouchPos;
  Vec2F m_lastPointerInput;
  Vec2F m_cursorInputPosition;
  bool m_hasCursorInputPosition = false;

};


class MobileGamepadInputAdapter::Impl {
public:
  explicit Impl(Vec2U* renderCanvasSize)
    : m_renderCanvasSize(renderCanvasSize) {
    resetCursor();
  }

  void setConfig(MobileGamepadConfig config) {
    if (!config.enabled)
      cancelAll();
    m_config = config;
  }

  void setBindings(std::vector<MobileGamepadBinding> bindings) {
    cancelAll();
    m_bindings = std::move(bindings);
  }

  void beginFrame() {
    m_generatedEvents.clear();
    if (!m_config.enabled)
      cancelAll();
  }

  void endFrame() {
    if (!m_config.enabled)
      return;

    if (m_actionWheelActive) {
      clearStickMovement("gamepad:leftStick");
      clearStickMovement("gamepad:rightStick");
      emitEvent(ActionWheelEvent{false, false, false, actionWheelDirection(), m_actionWheelType});
    } else {
      updateStickMovement("gamepad:leftStick", m_config.leftStick, m_leftStick);
      updateStickMovement("gamepad:rightStick", m_config.rightStick, m_rightStick);
      updateStickAim();
    }
    releaseExpiredPulsedActions();
    processMacroEvents();
    repeatButtons();
  }

  void appendGeneratedEvents(List<InputEvent>& outEvents) {
    outEvents.appendAll(m_generatedEvents);
    m_generatedEvents.clear();
  }

  bool processInputEvent(InputEvent const& event, List<InputEvent>& passthroughEvents) {
    if (!m_config.enabled) {
      passthroughEvents.append(event);
      return false;
    }

    if (auto axis = event.ptr<ControllerAxisEvent>()) {
      ControllerAxisEvent filtered = *axis;
      filtered.controllerAxisValue = applyAxisDeadzone(axis->controllerAxis, axis->controllerAxisValue);
      updateAxisState(filtered.controllerAxis, filtered.controllerAxisValue);

      if (axis->controllerAxis == ControllerAxis::LeftX || axis->controllerAxis == ControllerAxis::LeftY
          || axis->controllerAxis == ControllerAxis::RightX || axis->controllerAxis == ControllerAxis::RightY)
        return true;

      if (axis->controllerAxis == ControllerAxis::TriggerLeft) {
        updateVirtualButton(ControllerButton::TriggerLeft, filtered.controllerAxisValue >= std::clamp(m_config.triggerThreshold, 0.05f, 0.95f));
        return true;
      }

      if (axis->controllerAxis == ControllerAxis::TriggerRight) {
        updateVirtualButton(ControllerButton::TriggerRight, filtered.controllerAxisValue >= std::clamp(m_config.triggerThreshold, 0.05f, 0.95f));
        return true;
      }

      return true;
    }

    if (auto button = event.ptr<ControllerButtonDownEvent>()) {
      pressButton(button->controllerButton);
      return true;
    }

    if (auto button = event.ptr<ControllerButtonUpEvent>()) {
      releaseButton(button->controllerButton);
      return true;
    }

    passthroughEvents.append(event);
    return false;
  }

  void cancelAll() {
    clearPulsedActions();
    closeActionWheel(false);
    m_macroEvents.clear();

    for (auto const& pair : m_keyHoldCounts.pairs()) {
      if (pair.second > 0)
        emitEvent(KeyUpEvent{pair.first});
    }
    for (auto const& pair : m_mouseHoldCounts.pairs()) {
      if (pair.second > 0)
        emitEvent(MouseButtonUpEvent{pair.first, m_cursorPosition});
    }

    m_heldButtons.clear();
    m_toggledButtons.clear();
    m_nextButtonRepeatMs.clear();
    m_keyActionOwners.clear();
    m_keyHoldCounts.clear();
    m_mouseActionOwners.clear();
    m_mouseHoldCounts.clear();
    m_leftStick = {};
    m_rightStick = {};
  }

private:
  struct ActiveButton {
    MobileTouchAction action;
    MobileTouchPressMode pressMode = MobileTouchPressMode::SinglePress;
  };

  Vec2F canvasSize() const {
    if (m_renderCanvasSize && (*m_renderCanvasSize)[0] > 0 && (*m_renderCanvasSize)[1] > 0)
      return Vec2F(*m_renderCanvasSize);
    return {1280.0f, 720.0f};
  }

  void resetCursor() {
    Vec2F canvas = canvasSize();
    m_cursorPosition = {canvas[0] * 0.5f, canvas[1] * 0.5f};
    m_lastAimFrameMs = Time::monotonicMilliseconds();
  }

  float applyAxisDeadzone(ControllerAxis axis, float value) const {
    MobileGamepadStickConfig stick;
    bool useStick = true;
    if (axis == ControllerAxis::LeftX || axis == ControllerAxis::LeftY)
      stick = m_config.leftStick;
    else if (axis == ControllerAxis::RightX || axis == ControllerAxis::RightY)
      stick = m_config.rightStick;
    else
      useStick = false;

    if (!useStick)
      return std::clamp(value, 0.0f, 1.0f);

    float deadzone = std::clamp(stick.deadzone, 0.0f, 0.85f);
    if (std::abs(value) < deadzone)
      return 0.0f;

    float scaled = (std::abs(value) - deadzone) / std::max(0.001f, 1.0f - deadzone);
    scaled = std::copysign(std::clamp(scaled, 0.0f, 1.0f), value);
    if (axis == ControllerAxis::RightY)
      scaled = -scaled;
    if ((axis == ControllerAxis::LeftX || axis == ControllerAxis::RightX) && stick.invertX)
      scaled = -scaled;
    if ((axis == ControllerAxis::LeftY || axis == ControllerAxis::RightY) && stick.invertY)
      scaled = -scaled;
    return scaled;
  }

  void updateAxisState(ControllerAxis axis, float value) {
    if (axis == ControllerAxis::LeftX)
      m_leftStick[0] = value;
    else if (axis == ControllerAxis::LeftY)
      m_leftStick[1] = value;
    else if (axis == ControllerAxis::RightX)
      m_rightStick[0] = value;
    else if (axis == ControllerAxis::RightY)
      m_rightStick[1] = value;
  }

  MobileGamepadBinding const* bindingForButton(ControllerButton button) const {
    for (auto const& binding : m_bindings) {
      if (binding.button == button)
        return &binding;
    }
    return nullptr;
  }

  String buttonOwner(ControllerButton button) const {
    return String("gamepad:") + ControllerButtonNames.getRight(button);
  }

  void updateVirtualButton(ControllerButton button, bool desired) {
    if (desired)
      pressButton(button);
    else
      releaseButton(button);
  }

  void pressButton(ControllerButton button) {
    if (button == ControllerButton::Invalid || m_heldButtons.contains(buttonOwner(button)))
      return;
    auto binding = bindingForButton(button);
    if (!binding || !binding->enabled)
      return;

    String owner = buttonOwner(button);
    m_heldButtons.add(owner);
    ActiveButton active{binding->action, binding->pressMode};
    m_activeButtons[owner] = active;

    if (binding->action.kind == MobileTouchActionKind::GamepadAimModeToggle) {
      startPulsedAction(binding->action, owner);
      return;
    }

    if (binding->action.kind == MobileTouchActionKind::ActionWheel || binding->action.kind == MobileTouchActionKind::InventoryWheel) {
      openActionWheel(owner, binding->action.kind == MobileTouchActionKind::InventoryWheel ? ActionWheelType::Inventory : ActionWheelType::Actions);
      return;
    }

    if (binding->pressMode == MobileTouchPressMode::Toggle) {
      if (m_toggledButtons.contains(owner)) {
        m_toggledButtons.remove(owner);
        setAction(binding->action, owner, false);
      } else {
        m_toggledButtons.add(owner);
        setAction(binding->action, owner, true);
      }
    } else if (binding->pressMode == MobileTouchPressMode::Hold) {
      setAction(binding->action, owner, true);
    } else {
      startPulsedAction(binding->action, owner);
      if (binding->pressMode == MobileTouchPressMode::Repeat)
        m_nextButtonRepeatMs[owner] = Time::monotonicMilliseconds() + 120;
    }
  }

  void releaseButton(ControllerButton button) {
    String owner = buttonOwner(button);
    if (!m_heldButtons.contains(owner))
      return;

    m_heldButtons.remove(owner);
    if (auto active = m_activeButtons.ptr(owner)) {
      if (active->action.kind == MobileTouchActionKind::ActionWheel || active->action.kind == MobileTouchActionKind::InventoryWheel)
        closeActionWheel(true);
      else if (active->pressMode == MobileTouchPressMode::Hold)
        setAction(active->action, owner, false);
      else if (active->pressMode == MobileTouchPressMode::Repeat) {
        m_nextButtonRepeatMs.remove(owner);
        cancelPulsedAction(owner);
      }
    }
    m_activeButtons.remove(owner);
  }

  void repeatButtons() {
    int64_t now = Time::monotonicMilliseconds();
    for (auto const& owner : m_heldButtons.values()) {
      auto active = m_activeButtons.ptr(owner);
      if (!active || active->pressMode != MobileTouchPressMode::Repeat)
        continue;
      auto next = m_nextButtonRepeatMs.value(owner, 0);
      if (next > now)
        continue;
      startPulsedAction(active->action, owner);
      m_nextButtonRepeatMs[owner] = now + 120;
    }
  }

  void clearStickMovement(String const& owner) {
    setKeyOwner(owner + ":right", Key::D, false);
    setKeyOwner(owner + ":left", Key::A, false);
    setKeyOwner(owner + ":up", Key::W, false);
    setKeyOwner(owner + ":down", Key::S, false);
  }

  void updateStickMovement(String const& owner, MobileGamepadStickConfig const& stick, Vec2F const& value) {
    if (!stick.enabled || stick.mode != MobileGamepadStickMode::Movement) {
      clearStickMovement(owner);
      return;
    }

    setKeyOwner(owner + ":right", Key::D, value[0] > 0.30f);
    setKeyOwner(owner + ":left", Key::A, value[0] < -0.30f);
    setKeyOwner(owner + ":up", Key::W, value[1] < -0.30f);
    setKeyOwner(owner + ":down", Key::S, value[1] > 0.30f);
  }

  MobileGamepadStickConfig* activeAimStick(Vec2F& value) {
    if (m_config.rightStick.enabled && m_config.rightStick.mode == MobileGamepadStickMode::Aim && m_rightStick.magnitudeSquared() > 0.0001f) {
      value = m_rightStick;
      return &m_config.rightStick;
    }
    if (m_config.leftStick.enabled && m_config.leftStick.mode == MobileGamepadStickMode::Aim && m_leftStick.magnitudeSquared() > 0.0001f) {
      value = m_leftStick;
      return &m_config.leftStick;
    }
    return nullptr;
  }

  MobileGamepadStickConfig* aimModeToggleStick() {
    if (m_config.rightStick.mode == MobileGamepadStickMode::Aim)
      return &m_config.rightStick;
    if (m_config.leftStick.mode == MobileGamepadStickMode::Aim)
      return &m_config.leftStick;
    return &m_config.rightStick;
  }

  void updateStickAim() {
    Vec2F stickValue;
    auto stick = activeAimStick(stickValue);
    if (!stick) {
      m_lastAimFrameMs = Time::monotonicMilliseconds();
      return;
    }

    Vec2F canvas = canvasSize();
    int64_t now = Time::monotonicMilliseconds();
    float dt = std::clamp((float)(now - m_lastAimFrameMs) / 1000.0f, 0.0f, 0.05f);
    m_lastAimFrameMs = now;

    Vec2F aim{stickValue[0], stickValue[1]};
    if (stick->preciseAim) {
      float shortSide = std::max(1.0f, std::min(canvas[0], canvas[1]));
      float speed = shortSide * 1.35f * std::clamp(stick->sensitivity, 0.10f, 5.0f);
      m_cursorPosition += aim * speed * dt;
      m_cursorPosition[0] = std::clamp(m_cursorPosition[0], 0.0f, canvas[0]);
      m_cursorPosition[1] = std::clamp(m_cursorPosition[1], 0.0f, canvas[1]);
      emitEvent(MouseMoveEvent{{0.0f, 0.0f}, m_cursorPosition, m_config.joystickAimCursorEnabled});
    } else {
      Vec2F center{canvas[0] * 0.5f, canvas[1] * 0.5f};
      float radius = std::min(canvas[0], canvas[1]) * 0.34f * std::clamp(stick->sensitivity, 0.25f, 2.5f);
      m_cursorPosition = center + aim * radius;
      m_cursorPosition[0] = std::clamp(m_cursorPosition[0], 0.0f, canvas[0]);
      m_cursorPosition[1] = std::clamp(m_cursorPosition[1], 0.0f, canvas[1]);
      emitEvent(DirectionalAimEvent{aim, radius, m_config.joystickAimCursorEnabled});
    }
  }

  void setKeyOwner(String const& owner, Key key, bool desired) {
    String token = owner + ":" + KeyNames.getRight(key);
    if (desired && !m_keyActionOwners.contains(token)) {
      m_keyActionOwners.add(token);
      unsigned count = m_keyHoldCounts.value(key, 0);
      m_keyHoldCounts.set(key, count + 1);
      if (count == 0)
        emitEvent(KeyDownEvent{key, noMods()});
    } else if (!desired && m_keyActionOwners.contains(token)) {
      m_keyActionOwners.remove(token);
      unsigned count = m_keyHoldCounts.value(key, 0);
      if (count <= 1) {
        m_keyHoldCounts.remove(key);
        emitEvent(KeyUpEvent{key});
      } else {
        m_keyHoldCounts.set(key, count - 1);
      }
    }
  }

  void setMouseOwner(String const& owner, MouseButton button, bool desired) {
    String token = owner + ":" + MouseButtonNames.getRight(button);
    if (desired && !m_mouseActionOwners.contains(token)) {
      m_mouseActionOwners.add(token);
      unsigned count = m_mouseHoldCounts.value(button, 0);
      m_mouseHoldCounts.set(button, count + 1);
      if (count == 0)
        updateStickAim();
      if (count == 0)
        emitEvent(MouseButtonDownEvent{button, m_cursorPosition});
    } else if (!desired && m_mouseActionOwners.contains(token)) {
      m_mouseActionOwners.remove(token);
      unsigned count = m_mouseHoldCounts.value(button, 0);
      if (count <= 1) {
        m_mouseHoldCounts.remove(button);
        updateStickAim();
        emitEvent(MouseButtonUpEvent{button, m_cursorPosition});
      } else {
        m_mouseHoldCounts.set(button, count - 1);
      }
    }
  }

  void setAction(MobileTouchAction const& action, String const& owner, bool desired) {
    if (action.kind == MobileTouchActionKind::Key) {
      setKeyOwner(owner, action.key, desired);
    } else if (action.kind == MobileTouchActionKind::KeyMacro) {
      if (action.macroSequential) {
        if (desired && !action.keys.empty() && !hasActiveMacro(owner))
          enqueueMacro(action.keys, owner);
      } else {
        for (auto key : action.keys)
          setKeyOwner(owner, key, desired);
      }
    } else if (action.kind == MobileTouchActionKind::MouseButton) {
      setMouseOwner(owner, action.mouseButton, desired);
    } else if (desired && action.kind == MobileTouchActionKind::MouseWheelUp) {
      emitEvent(MouseWheelEvent{MouseWheel::Up, m_cursorPosition});
    } else if (desired && action.kind == MobileTouchActionKind::MouseWheelDown) {
      emitEvent(MouseWheelEvent{MouseWheel::Down, m_cursorPosition});
    } else if (desired && action.kind == MobileTouchActionKind::GamepadAimModeToggle) {
      auto stick = aimModeToggleStick();
      stick->preciseAim = !stick->preciseAim;
    } else if (action.kind == MobileTouchActionKind::ActionWheel || action.kind == MobileTouchActionKind::InventoryWheel) {
      if (desired)
        openActionWheel(owner, action.kind == MobileTouchActionKind::InventoryWheel ? ActionWheelType::Inventory : ActionWheelType::Actions);
      else
        closeActionWheel(true);
    } else if (desired && action.kind == MobileTouchActionKind::UiNavigation) {
      emitEvent(UiNavigationEvent{action.uiNavigationDirection});
    }
  }

  bool actionNeedsRelease(MobileTouchAction const& action) const {
    if (action.kind == MobileTouchActionKind::KeyMacro)
      return !action.macroSequential;
    return action.kind == MobileTouchActionKind::Key || action.kind == MobileTouchActionKind::MouseButton;
  }

  // Sequential macro playback (mirrors the touch adapter). See that adapter for
  // the design notes on timing and per-key owner tokens.
  static constexpr int64_t MacroKeyHoldMs = 45;
  static constexpr int64_t MacroKeyGapMs = 55;

  bool hasActiveMacro(String const& owner) const {
    String prefix = owner + "#macro";
    for (auto const& event : m_macroEvents) {
      if (event.ownerToken.beginsWith(prefix))
        return true;
    }
    return false;
  }

  void enqueueMacro(List<Key> const& keys, String const& owner) {
    int64_t instance = ++m_macroInstanceCounter;
    int64_t cursor = Time::monotonicMilliseconds();
    size_t step = 0;
    for (auto key : keys) {
      String token = strf("{}#macro{}#{}", owner, instance, step);
      m_macroEvents.append(MacroKeyEvent{key, true, cursor, token});
      m_macroEvents.append(MacroKeyEvent{key, false, cursor + MacroKeyHoldMs, token});
      cursor += MacroKeyHoldMs + MacroKeyGapMs;
      ++step;
    }
  }

  void processMacroEvents() {
    if (m_macroEvents.empty())
      return;

    int64_t now = Time::monotonicMilliseconds();
    List<MacroKeyEvent> pending;
    for (auto const& event : m_macroEvents) {
      if (event.atMs <= now)
        setKeyOwner(event.ownerToken, event.key, event.down);
      else
        pending.append(event);
    }
    m_macroEvents = std::move(pending);
  }

  void startPulsedAction(MobileTouchAction const& action, String const& owner) {
    setAction(action, owner, true);
    if (!actionNeedsRelease(action))
      return;
    m_pulsedActions[owner] = action;
    m_pulsedActionReleaseMs[owner] = Time::monotonicMilliseconds() + 55;
  }

  void cancelPulsedAction(String const& owner) {
    if (auto action = m_pulsedActions.ptr(owner))
      setAction(*action, owner, false);
    m_pulsedActions.remove(owner);
    m_pulsedActionReleaseMs.remove(owner);
  }

  void releaseExpiredPulsedActions() {
    int64_t now = Time::monotonicMilliseconds();
    StringList expired;
    for (auto const& pair : m_pulsedActionReleaseMs.pairs()) {
      if (pair.second <= now)
        expired.append(pair.first);
    }
    for (auto const& owner : expired)
      cancelPulsedAction(owner);
  }

  void clearPulsedActions() {
    StringList owners;
    for (auto const& pair : m_pulsedActions.pairs())
      owners.append(pair.first);
    for (auto const& owner : owners)
      cancelPulsedAction(owner);
  }

  Vec2F actionWheelDirection() const {
    if (m_rightStick.magnitudeSquared() >= m_leftStick.magnitudeSquared())
      return {m_rightStick[0], -m_rightStick[1]};
    return m_leftStick;
  }

  void openActionWheel(String const& owner, ActionWheelType type) {
    if (m_actionWheelActive)
      return;
    m_actionWheelActive = true;
    m_actionWheelOwner = owner;
    m_actionWheelType = type;
    emitEvent(ActionWheelEvent{true, false, false, actionWheelDirection(), m_actionWheelType});
  }

  void closeActionWheel(bool confirm) {
    if (!m_actionWheelActive)
      return;
    emitEvent(ActionWheelEvent{false, confirm, !confirm, actionWheelDirection(), m_actionWheelType});
    m_actionWheelActive = false;
    m_actionWheelOwner = {};
    m_actionWheelType = ActionWheelType::Actions;
  }

  void emitEvent(InputEvent const& event) {
    m_generatedEvents.append(event);
  }

  Vec2U* m_renderCanvasSize = nullptr;
  MobileGamepadConfig m_config;
  std::vector<MobileGamepadBinding> m_bindings = defaultGamepadBindings();
  List<InputEvent> m_generatedEvents;
  Vec2F m_leftStick;
  Vec2F m_rightStick;
  Vec2F m_cursorPosition;
  int64_t m_lastAimFrameMs = 0;
  StringSet m_heldButtons;
  StringSet m_toggledButtons;
  bool m_actionWheelActive = false;
  String m_actionWheelOwner;
  ActionWheelType m_actionWheelType = ActionWheelType::Actions;
  StringMap<ActiveButton> m_activeButtons;
  StringMap<int64_t> m_nextButtonRepeatMs;
  StringMap<MobileTouchAction> m_pulsedActions;
  StringMap<int64_t> m_pulsedActionReleaseMs;
  StringSet m_keyActionOwners;
  HashMap<Key, unsigned> m_keyHoldCounts;
  StringSet m_mouseActionOwners;
  HashMap<MouseButton, unsigned> m_mouseHoldCounts;

  struct MacroKeyEvent {
    Key key;
    bool down;
    int64_t atMs;
    String ownerToken;
  };
  List<MacroKeyEvent> m_macroEvents;
  int64_t m_macroInstanceCounter = 0;
};



MobileTouchInputAdapter::MobileTouchInputAdapter(Vec2U* windowSize, Vec2U* renderCanvasSize, float* displayScale, SafeAreaInsets* safeArea)
  : m_impl(make_unique<Impl>(windowSize, renderCanvasSize, displayScale, safeArea)) {}

MobileTouchInputAdapter::~MobileTouchInputAdapter() = default;

void MobileTouchInputAdapter::setConfig(MobileTouchConfig config) { m_impl->setConfig(std::move(config)); }
void MobileTouchInputAdapter::setGyroAvailable(bool available) { m_impl->setGyroAvailable(available); }
void MobileTouchInputAdapter::setElements(std::vector<MobileTouchElement> elements) { m_impl->setElements(std::move(elements)); }
void MobileTouchInputAdapter::beginFrame() { m_impl->beginFrame(); }
void MobileTouchInputAdapter::endFrame() { m_impl->endFrame(); }
void MobileTouchInputAdapter::appendGeneratedEvents(List<InputEvent>& outEvents) { m_impl->appendGeneratedEvents(outEvents); }
bool MobileTouchInputAdapter::gyroSensorRequested() const { return m_impl->gyroSensorRequested(); }
void MobileTouchInputAdapter::setGyroInput(std::array<float, 3> const& data, bool hasData, SDL_DisplayOrientation orientation) { m_impl->setGyroInput(data, hasData, orientation); }
bool MobileTouchInputAdapter::processSdlEvent(SDL_Event const& event) { return m_impl->processSdlEvent(event); }
void MobileTouchInputAdapter::cancelAll() { m_impl->cancelAll(); }
void MobileTouchInputAdapter::drawOverlay(float fps) { m_impl->drawOverlay(fps); }
bool MobileTouchInputAdapter::overlayEnabled() const { return m_impl->overlayEnabled(); }

MobileGamepadInputAdapter::MobileGamepadInputAdapter(Vec2U* renderCanvasSize)
  : m_impl(make_unique<Impl>(renderCanvasSize)) {}

MobileGamepadInputAdapter::~MobileGamepadInputAdapter() = default;

void MobileGamepadInputAdapter::setConfig(MobileGamepadConfig config) { m_impl->setConfig(std::move(config)); }
void MobileGamepadInputAdapter::setBindings(std::vector<MobileGamepadBinding> bindings) { m_impl->setBindings(std::move(bindings)); }
void MobileGamepadInputAdapter::beginFrame() { m_impl->beginFrame(); }
void MobileGamepadInputAdapter::endFrame() { m_impl->endFrame(); }
void MobileGamepadInputAdapter::appendGeneratedEvents(List<InputEvent>& outEvents) { m_impl->appendGeneratedEvents(outEvents); }
bool MobileGamepadInputAdapter::processInputEvent(InputEvent const& event, List<InputEvent>& passthroughEvents) { return m_impl->processInputEvent(event, passthroughEvents); }
void MobileGamepadInputAdapter::cancelAll() { m_impl->cancelAll(); }


}
