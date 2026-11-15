#include "gtest/gtest.h"
#include "StarFile.hpp"
#include <chrono>
#include <thread>
#include "mobile/StarMobileControls.hpp"
// Isolate input mapping from the launcher's localisation and host services.
namespace Star {
String launcherTextStatic(String const&, String const& fallback) { return fallback; }
KeyMod noMods() { return KeyMod::NoMod; }
}
using namespace Star;

TEST(MobileControls, MenusReleaseHeldKeysAndReturnTouchToSdl) {
  Vec2U size{1000, 500};
  MobileTouchInputAdapter adapter(&size);
  MobileTouchConfig config;
  config.enabled = true;
  config.directTouchGestures = false;
  adapter.setConfig(config);
  MobileTouchElement button;
  button.id = "test-button";
  button.position = {0.5f, 0.5f};
  button.action.kind = MobileTouchActionKind::Key;
  button.action.key = Key::W;
  adapter.setElements({button});
  SDL_Event down{};
  down.type = SDL_EVENT_FINGER_DOWN;
  down.tfinger.fingerID = 1;
  down.tfinger.x = 0.5f; down.tfinger.y = 0.5f;
  adapter.beginFrame();
  ASSERT_TRUE(adapter.processSdlEvent(down));
  adapter.endFrame();
  List<InputEvent> events;
  adapter.appendGeneratedEvents(events);
  ASSERT_TRUE(events.any([](auto const& e) { return e.template is<KeyDownEvent>(); }));

  adapter.beginFrame();
  adapter.setGameplayActive(false);
  adapter.appendGeneratedEvents(events);
  EXPECT_FALSE(adapter.overlayEnabled());
  EXPECT_FALSE(adapter.processSdlEvent(down));
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<KeyUpEvent>(); }));
  adapter.setGameplayActive(true);
  EXPECT_TRUE(adapter.overlayEnabled());
}

TEST(MobileControls, GameplayDoesNotOverrideDisabledTouchPreference) {
  Vec2U size{1000, 500};
  MobileTouchInputAdapter adapter(&size);
  MobileTouchConfig config;
  config.enabled = false;
  adapter.setConfig(config);
  adapter.setGameplayActive(false);
  adapter.setGameplayActive(true);
  EXPECT_FALSE(adapter.overlayEnabled());
}

TEST(MobileControls, TouchpadCanAimWhileAnotherFingerHoldsAttack) {
  Vec2U size{1000, 500};
  MobileTouchInputAdapter adapter(&size);
  MobileTouchConfig config;
  config.directTouchGestures = true;
  config.directTouchGestureMode = DirectTouchGestureMode::Touchpad;
  config.directTouchSingleAction = noneAction();
  config.directTouchTwoFingerAction = noneAction();
  adapter.setConfig(config);
  MobileTouchElement attack;
  attack.id = "attack";
  attack.kind = MobileTouchElementKind::Button;
  attack.position = {0.2f, 0.5f};
  attack.action = mouseAction(MouseButton::Left);
  adapter.setElements({attack});

  auto finger = [&](SDL_EventType type, SDL_FingerID id, float x) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.fingerID = id;
    event.tfinger.x = x;
    event.tfinger.y = 0.5f;
    return adapter.processSdlEvent(event);
  };
  auto events = [&]() {
    List<InputEvent> result;
    adapter.appendGeneratedEvents(result);
    return result;
  };
  adapter.beginFrame();
  EXPECT_TRUE(finger(SDL_EVENT_FINGER_DOWN, 1, 0.7f));
  EXPECT_FALSE(events().any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
  EXPECT_TRUE(finger(SDL_EVENT_FINGER_DOWN, 2, 0.2f));
  EXPECT_TRUE(finger(SDL_EVENT_FINGER_MOTION, 1, 0.8f));
  auto held = events();
  EXPECT_TRUE(held.any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
  EXPECT_FALSE(held.any([](auto const& e) { return e.template is<MouseButtonUpEvent>(); }));
  adapter.beginFrame();
  EXPECT_TRUE(finger(SDL_EVENT_FINGER_UP, 2, 0.2f));
  EXPECT_TRUE(events().any([](auto const& e) { return e.template is<MouseButtonUpEvent>(); }));
  EXPECT_TRUE(finger(SDL_EVENT_FINGER_MOTION, 1, 0.9f));
  EXPECT_FALSE(events().any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
  EXPECT_TRUE(finger(SDL_EVENT_FINGER_UP, 1, 0.9f));
}

TEST(MobileControls, CancelReleasesHeldMacroExactlyOnce) {
  Vec2U size{1000, 500};
  MobileTouchInputAdapter adapter(&size);
  MobileTouchConfig config;
  config.directTouchGestures = false;
  adapter.setConfig(config);
  MobileTouchElement button;
  button.id = "macro";
  button.kind = MobileTouchElementKind::Button;
  button.position = {0.5f, 0.5f};
  button.action = macroAction({Key::W, Key::Space}, false);
  adapter.setElements({button});
  SDL_Event event{};
  event.type = SDL_EVENT_FINGER_DOWN;
  event.tfinger.fingerID = 1;
  event.tfinger.x = event.tfinger.y = 0.5f;
  adapter.beginFrame();
  ASSERT_TRUE(adapter.processSdlEvent(event));
  adapter.beginFrame();
  adapter.cancelAll();
  List<InputEvent> released;
  adapter.appendGeneratedEvents(released);
  EXPECT_EQ(2u, released.size());
  EXPECT_TRUE(released.all([](auto const& e) { return e.template is<KeyUpEvent>(); }));
  adapter.beginFrame();
  adapter.cancelAll();
  released.clear();
  adapter.appendGeneratedEvents(released);
  EXPECT_TRUE(released.empty());
}

TEST(MobileControls, RevampedAndLegacyLayoutsKeepIndependentCustomizations) {
  auto legacy = defaultTouchElements(false);
  legacy.front().position = {0.3f, 0.4f};
  auto revamped = defaultTouchElements();
  revamped.front().position = {0.6f, 0.7f};
  Json config = JsonObject{{"touch", JsonObject{
    {"layout", "revamped"}, {"elements", jsonFromTouchElements(legacy)},
    {"revampedElements", jsonFromTouchElements(revamped)}}}};
  auto modern = touchElementsFromConfig(config);
  EXPECT_EQ(revamped.front().position, modern.front().position);
  EXPECT_EQ(revamped.front().sprite, modern.front().sprite);
  config = JsonObject{{"touch", JsonObject{{"layout", "legacy"}, {"elements", jsonFromTouchElements(legacy)}}}};
  EXPECT_EQ(legacy.front().position, touchElementsFromConfig(config).front().position);
  EXPECT_TRUE(touchElementsFromConfig(config).front().sprite.empty());
  // An existing install selects the new default without overwriting old data.
  config = JsonObject{{"touch", JsonObject{{"elements", jsonFromTouchElements(legacy)}}}};
  EXPECT_EQ(defaultTouchElements().front().position, touchElementsFromConfig(config).front().position);
}

namespace {
struct TouchDriver {
  Vec2U size{1000, 500};
  MobileTouchInputAdapter adapter{&size};
  List<InputEvent> finger(SDL_EventType type, int id, float x, float y = 0.5f) {
    adapter.beginFrame();
    SDL_Event e{};
    e.type = type; e.tfinger.fingerID = id; e.tfinger.x = x; e.tfinger.y = y;
    adapter.processSdlEvent(e);
    adapter.endFrame();
    List<InputEvent> result;
    adapter.appendGeneratedEvents(result);
    return result;
  }
};
}

TEST(MobileControls, AimCyclesAllFourModesAndReleasesEachHand) {
  TouchDriver d;
  MobileTouchConfig config;
  config.directTouchGestures = false;
  d.adapter.setConfig(config);
  MobileTouchElement cycle;
  cycle.id = "mode"; cycle.position = {0.2f, 0.5f};
  cycle.action.kind = MobileTouchActionKind::TouchAimModeCycle;
  cycle.pressMode = MobileTouchPressMode::SinglePress;
  MobileTouchElement aim;
  aim.id = "aim"; aim.position = {0.75f, 0.5f}; aim.kind = MobileTouchElementKind::AimJoystick;
  d.adapter.setElements({cycle, aim});
  bool left = false, right = false;
  auto track = [&](List<InputEvent> const& events) {
    for (auto const& e : events) {
      if (auto down = e.ptr<MouseButtonDownEvent>())
        (down->mouseButton == MouseButton::Left ? left : right) = true;
      if (auto up = e.ptr<MouseButtonUpEvent>())
        (up->mouseButton == MouseButton::Left ? left : right) = false;
    }
  };
  track(d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.78f));
  EXPECT_TRUE(left); EXPECT_FALSE(right);
  for (unsigned mode = 1; mode <= 4; ++mode) {
    track(d.finger(SDL_EVENT_FINGER_DOWN, 2, 0.2f));
    track(d.finger(SDL_EVENT_FINGER_UP, 2, 0.2f));
    EXPECT_EQ(mode % 4 == 0 || mode % 4 == 2, left);
    EXPECT_EQ(mode % 4 == 1 || mode % 4 == 2, right);
  }
  track(d.finger(SDL_EVENT_FINGER_CANCELED, 1, 0.78f));
  EXPECT_FALSE(left); EXPECT_FALSE(right);
}

TEST(MobileControls, OpeningWindowsReleasesHiddenControlsAndUsesPointerInput) {
  TouchDriver d;
  MobileTouchConfig config;
  config.directTouchGestures = false;
  d.adapter.setConfig(config);
  MobileTouchElement attack;
  attack.id = "attack"; attack.position = {0.8f, 0.5f};
  attack.action = mouseAction(MouseButton::Right);
  attack.hideWhenWindowsOpen = true;
  d.adapter.setElements({attack});
  auto events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.8f);
  ASSERT_TRUE(events.any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
  d.adapter.beginFrame();
  d.adapter.setInterfaceObstructed(true);
  d.adapter.appendGeneratedEvents(events);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<MouseButtonUpEvent>(); }));
  events = d.finger(SDL_EVENT_FINGER_DOWN, 2, 0.8f);
  ASSERT_FALSE(events.empty());
  for (auto const& e : events)
    if (auto down = e.template ptr<MouseButtonDownEvent>())
      EXPECT_EQ(MouseButton::Left, down->mouseButton);
  d.adapter.beginFrame();
  d.adapter.setInterfaceObstructed(false);
  events.clear();
  d.adapter.appendGeneratedEvents(events);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<MouseButtonUpEvent>(); }));
}

TEST(MobileControls, LegacyAimStickDoesNotAutoAttack) {
  TouchDriver d;
  MobileTouchConfig config;
  config.revamped = false;
  config.directTouchGestures = false;
  d.adapter.setConfig(config);
  MobileTouchElement aim;
  aim.id = "aim"; aim.position = {0.75f, 0.5f}; aim.kind = MobileTouchElementKind::AimJoystick;
  d.adapter.setElements({aim});
  auto events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.78f);
  EXPECT_FALSE(events.any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
}

TEST(MobileControls, WalkToggleDoesNotShiftClickInventoryAndResumesAfterwards) {
  TouchDriver d;
  MobileTouchElement walk;
  walk.id = "walk"; walk.position = {0.2f, 0.5f};
  walk.action = keyAction(Key::LShift); walk.pressMode = MobileTouchPressMode::Toggle;
  d.adapter.setElements({walk});
  auto events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.2f);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<KeyDownEvent>(); }));
  d.finger(SDL_EVENT_FINGER_UP, 1, 0.2f);
  d.adapter.beginFrame();
  d.adapter.setInterfaceObstructed(true);
  events.clear();
  d.adapter.appendGeneratedEvents(events);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<KeyUpEvent>(); }));
  d.adapter.beginFrame();
  d.adapter.setInterfaceObstructed(false);
  events.clear();
  d.adapter.appendGeneratedEvents(events);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<KeyDownEvent>(); }));
  events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.2f);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<KeyUpEvent>(); }));
}

TEST(MobileControls, SpriteButtonCornersTakePriorityOverSticks) {
  TouchDriver d;
  MobileTouchElement stick;
  stick.id = "stick"; stick.position = {0.5f, 0.5f}; stick.kind = MobileTouchElementKind::Joystick;
  MobileTouchElement button;
  button.id = "button"; button.position = {0.5f, 0.5f}; button.action = keyAction(Key::Space);
  d.adapter.setElements({stick, button});
  auto events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.522f, 0.544f);
  ASSERT_TRUE(events.any([](auto const& e) {
    auto key = e.template ptr<KeyDownEvent>();
    return key && key->key == Key::Space;
  }));
}

TEST(MobileControls, VisibilityCycleReleasesAttacksAndRetainsRestoreButton) {
  TouchDriver d;
  MobileTouchConfig config;
  config.directTouchGestures = false;
  d.adapter.setConfig(config);
  MobileTouchElement visibility;
  visibility.id = "hide"; visibility.position = {0.2f, 0.5f};
  visibility.action.kind = MobileTouchActionKind::OverlayVisibilityCycle;
  visibility.pressMode = MobileTouchPressMode::SinglePress;
  MobileTouchElement attack;
  attack.id = "attack"; attack.position = {0.8f, 0.5f}; attack.action = mouseAction(MouseButton::Right);
  d.adapter.setElements({visibility, attack});
  d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.8f);
  auto events = d.finger(SDL_EVENT_FINGER_DOWN, 2, 0.2f);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<MouseButtonUpEvent>(); }));
  d.finger(SDL_EVENT_FINGER_UP, 2, 0.2f);
  events = d.finger(SDL_EVENT_FINGER_DOWN, 3, 0.8f);
  EXPECT_FALSE(events.any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
  for (unsigned phase = 0; phase < 2; ++phase) {
    events = d.finger(SDL_EVENT_FINGER_DOWN, 2, 0.2f);
    EXPECT_TRUE(events.any([](auto const& e) {
      auto key = e.template ptr<KeyDownEvent>();
      return key && key->key == Key::F1;
    }));
    d.finger(SDL_EVENT_FINGER_UP, 2, 0.2f);
  }
  events = d.finger(SDL_EVENT_FINGER_DOWN, 4, 0.8f);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
}

TEST(MobileControls, RevampedMovementStickDoesNotFireOnTap) {
  TouchDriver d;
  MobileTouchElement stick;
  stick.id = "stick"; stick.position = {0.5f, 0.5f}; stick.kind = MobileTouchElementKind::Joystick;
  d.adapter.setElements({stick});
  d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.5f);
  auto events = d.finger(SDL_EVENT_FINGER_UP, 1, 0.5f);
  EXPECT_FALSE(events.any([](auto const& e) { return e.template is<MouseButtonDownEvent>(); }));
}

TEST(MobileControls, BundledSpritesExistForAllModesAndPressStates) {
  String assets = File::relativeTo(File::dirName(__FILE__), "../../assets");
  EXPECT_TRUE(File::isFile(File::relativeTo(assets, "osbm/controls/joystick_base.png")));
  for (auto const& element : defaultTouchElements()) {
    for (unsigned mode = 0; mode < 4; ++mode) {
      for (bool held : {false, true}) {
        auto path = touchElementSprite(element, held, mode, mode % 2);
        EXPECT_TRUE(File::isFile(File::relativeTo(assets, path))) << path.utf8Ptr();
      }
    }
  }
}

TEST(MobileControls, BodyTechDoubleTapsWhileMovementStickIsHeld) {
  TouchDriver d;
  MobileTouchElement stick;
  stick.id = "stick"; stick.position = {0.3f, 0.5f}; stick.kind = MobileTouchElementKind::Joystick;
  MobileTouchElement dash;
  dash.id = "dash"; dash.position = {0.8f, 0.5f}; dash.action.kind = MobileTouchActionKind::BodyTech;
  d.adapter.setElements({stick, dash});
  auto events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.27f);
  ASSERT_TRUE(events.any([](auto const& e) {
    auto down = e.template ptr<KeyDownEvent>();
    return down && down->key == Key::A;
  }));
  events = d.finger(SDL_EVENT_FINGER_DOWN, 2, 0.8f);
  EXPECT_TRUE(events.any([](auto const& e) { return e.template is<KeyUpEvent>(); }));
  EXPECT_FALSE(events.any([](auto const& e) { return e.template is<KeyDownEvent>(); }));
  unsigned presses = 0;
  auto countPresses = [&](List<InputEvent> const& batch) {
    for (auto const& e : batch)
      if (auto key = e.ptr<KeyDownEvent>()) {
        EXPECT_EQ(Key::A, key->key);
        ++presses;
      }
  };
  countPresses(events);
  for (unsigned i = 0; i < 12; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    d.adapter.beginFrame();
    d.adapter.endFrame();
    events.clear();
    d.adapter.appendGeneratedEvents(events);
    countPresses(events);
  }
  // Two taps, followed by restoring the stick's continuous movement.
  EXPECT_GE(presses, 3u);
  d.adapter.cancelAll();
}

TEST(MobileControls, BodyTechUsesFacingDirectionWhenStanding) {
  TouchDriver d;
  d.adapter.setFacingLeft(true);
  MobileTouchElement dash;
  dash.id = "dash"; dash.position = {0.8f, 0.5f}; dash.action.kind = MobileTouchActionKind::BodyTech;
  d.adapter.setElements({dash});
  auto events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.8f);
  std::this_thread::sleep_for(std::chrono::milliseconds(65));
  d.adapter.beginFrame();
  d.adapter.endFrame();
  d.adapter.appendGeneratedEvents(events);
  EXPECT_TRUE(events.any([](auto const& e) {
    auto key = e.template ptr<KeyDownEvent>();
    return key && key->key == Key::A;
  }));
}

TEST(MobileControls, HudVisibilityUsesHostActionWithoutDependingOnKeyBindings) {
  TouchDriver d;
  MobileTouchElement visibility;
  visibility.id = "hide"; visibility.position = {0.2f, 0.5f};
  visibility.action.kind = MobileTouchActionKind::OverlayVisibilityCycle;
  d.adapter.setElements({visibility});
  std::vector<bool> requests;
  d.adapter.setHudVisibilityHandler([&](bool visible) { requests.push_back(visible); });
  for (unsigned phase = 0; phase < 3; ++phase) {
    auto events = d.finger(SDL_EVENT_FINGER_DOWN, 1, 0.2f);
    EXPECT_FALSE(events.any([](auto const& e) { return e.template is<KeyDownEvent>(); }));
    d.finger(SDL_EVENT_FINGER_UP, 1, 0.2f);
  }
  ASSERT_EQ(2u, requests.size());
  EXPECT_FALSE(requests[0]);
  EXPECT_TRUE(requests[1]);
}
