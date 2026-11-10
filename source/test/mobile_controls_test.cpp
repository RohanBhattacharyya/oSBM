#include "gtest/gtest.h"
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
