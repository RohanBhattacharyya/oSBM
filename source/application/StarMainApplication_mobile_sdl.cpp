#include "StarMainApplication.hpp"

#include "StarApplication.hpp"
#include "StarApplicationController.hpp"
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
#endif

#include "SDL3/SDL.h"
#include "SDL3/SDL_opengles2.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#ifdef STAR_SYSTEM_ANDROID
#include <android/log.h>
#endif

namespace Star {

String getMobileStartupStatus();
void setMobileStartupStatus(String const& status);

namespace {

#ifdef STAR_SYSTEM_ANDROID
void androidLogInfo(char const* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  __android_log_vprint(ANDROID_LOG_INFO, "OpenStarbound", fmt, args);
  va_end(args);
}
#elif defined(STAR_SYSTEM_IOS)
void androidLogInfo(char const* fmt, ...) {
  if (!fmt)
    return;
  va_list args;
  va_start(args, fmt);
  fprintf(stderr, "[OpenStarbound] ");
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  fflush(stderr);
  va_end(args);
}
#else
void androidLogInfo(char const*, ...) {}
#endif

static inline void convertEventToRenderCoordinatesIfPossible(SDL_Window* window, SDL_Event* event) {
  if (!window || !event)
    return;

  // This app uses an OpenGL context directly (no SDL_Renderer), so on some
  // mobile backends SDL_GetRenderer(window) returns null. Guard conversion.
  SDL_Renderer* renderer = SDL_GetRenderer(window);
  if (renderer)
    SDL_ConvertEventToRenderCoordinates(renderer, event);
}

static inline bool isTouchDerivedMouseEvent(SDL_Event const& event) {
#ifdef SDL_TOUCH_MOUSEID
  if (event.type == SDL_EVENT_MOUSE_MOTION)
    return event.motion.which == SDL_TOUCH_MOUSEID;
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
    return event.button.which == SDL_TOUCH_MOUSEID;
  if (event.type == SDL_EVENT_MOUSE_WHEEL)
    return event.wheel.which == SDL_TOUCH_MOUSEID;
#else
  _unused(event);
#endif
  return false;
}

Maybe<Key> keyFromSdlKeyCode(SDL_Keycode sym) {
  switch (sym) {
    case SDLK_W: return Key::W;
    case SDLK_A: return Key::A;
    case SDLK_S: return Key::S;
    case SDLK_D: return Key::D;
    case SDLK_E: return Key::E;
    case SDLK_SPACE: return Key::Space;
    case SDLK_ESCAPE: return Key::Escape;
    case SDLK_RETURN: return Key::Return;
    case SDLK_TAB: return Key::Tab;
    case SDLK_BACKSPACE: return Key::Backspace;
    case SDLK_UP: return Key::Up;
    case SDLK_DOWN: return Key::Down;
    case SDLK_LEFT: return Key::Left;
    case SDLK_RIGHT: return Key::Right;
    default: return {};
  }
}

MouseButton mouseButtonFromSdlMouseButton(uint8_t button) {
  switch (button) {
    case SDL_BUTTON_LEFT: return MouseButton::Left;
    case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
    case SDL_BUTTON_RIGHT: return MouseButton::Right;
    case SDL_BUTTON_X1: return MouseButton::FourthButton;
    default: return MouseButton::FifthButton;
  }
}

ControllerAxis controllerAxisFromSdlControllerAxis(uint8_t axis) {
  switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return ControllerAxis::LeftX;
    case SDL_GAMEPAD_AXIS_LEFTY: return ControllerAxis::LeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX: return ControllerAxis::RightX;
    case SDL_GAMEPAD_AXIS_RIGHTY: return ControllerAxis::RightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return ControllerAxis::TriggerLeft;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return ControllerAxis::TriggerRight;
    default: return ControllerAxis::Invalid;
  }
}

ControllerButton controllerButtonFromSdlControllerButton(uint8_t button) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ControllerButton::A;
    case SDL_GAMEPAD_BUTTON_EAST: return ControllerButton::B;
    case SDL_GAMEPAD_BUTTON_WEST: return ControllerButton::X;
    case SDL_GAMEPAD_BUTTON_NORTH: return ControllerButton::Y;
    case SDL_GAMEPAD_BUTTON_BACK: return ControllerButton::Back;
    case SDL_GAMEPAD_BUTTON_START: return ControllerButton::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ControllerButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ControllerButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ControllerButton::DPadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ControllerButton::DPadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ControllerButton::DPadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ControllerButton::DPadRight;
    default: return ControllerButton::Invalid;
  }
}

KeyMod noMods() {
  return KeyMod::NoMod;
}

struct MobileTouchConfig {
  bool enabled = true;
  float opacity = 0.35f;
  float size = 1.0f;
  float deadzone = 0.15f;
};

struct LauncherModEntry {
  String displayName;
  String path;
  bool isDirectory = false;
  bool isPackedPak = false;
};

struct LauncherState {
  bool canLaunch = false;
  String packedPakPath;
  String lastError;
  String lastStatus;
  MobileTouchConfig touchConfig;
  bool modManagerOpen = false;
  bool modListDirty = true;
  bool modShowPackedPaks = true;
  bool modShowUnpackedFolders = true;
  char modSearchBuffer[256] = {};
  List<LauncherModEntry> modEntries;
  String pendingDeletePath;
  String pendingDeleteName;
  bool pendingDeleteIsDirectory = false;
};

class MobileTouchInputAdapter {
public:
  explicit MobileTouchInputAdapter(Vec2U* windowSize, float* displayScale = nullptr)
    : m_windowSize(windowSize), m_displayScalePtr(displayScale) {}

  void setConfig(MobileTouchConfig config) {
    m_config = config;
  }

  void beginFrame() {
    m_generatedEvents.clear();
    if (!m_config.enabled)
      clearActions();
  }

  void endFrame() {
    emitActionEdges();
  }

  void appendGeneratedEvents(List<InputEvent>& outEvents) {
    outEvents.appendAll(m_generatedEvents);
    m_generatedEvents.clear();
  }

  bool processSdlEvent(SDL_Event const& event) {
    if (!m_config.enabled)
      return false;

    if (event.type != SDL_EVENT_FINGER_DOWN
        && event.type != SDL_EVENT_FINGER_UP
        && event.type != SDL_EVENT_FINGER_MOTION) {
      return false;
    }

    Vec2F pos = toScreen(event.tfinger.x, event.tfinger.y);
    uint64_t finger = event.tfinger.fingerID;

    if (event.type == SDL_EVENT_FINGER_DOWN)
      assignFinger(finger, pos);
    else if (event.type == SDL_EVENT_FINGER_MOTION)
      updateFinger(finger, pos);
    else
      releaseFinger(finger, pos);

    return true;
  }

  void drawOverlay() {
    if (!m_config.enabled)
      return;

    float radius = controlRadius();
    updateButtonCenters(radius);

    // ImGui operates in logical pixels. On HiDPI displays (iOS Retina) the
    // window size is in physical pixels, so divide by the pixel scale before
    // handing any position or radius to the ImGui draw list.
    float ps = drawPixelScale();
    float dr = radius / ps;
    auto ip = [ps](Vec2F const& v) { return ImVec2(v[0] / ps, v[1] / ps); };

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImU32 base = IM_COL32(255, 255, 255, (int)(180.0f * std::clamp(m_config.opacity, 0.0f, 1.0f)));
    ImU32 fill = IM_COL32(80, 160, 255, (int)(140.0f * std::clamp(m_config.opacity, 0.0f, 1.0f)));

    if (m_joystickActive) {
      draw->AddCircle(ip(m_joystickOrigin), dr, base, 48, 3.0f);
      draw->AddCircleFilled(ip(m_joystickCurrent), dr * 0.45f, fill, 32);
    }

    drawButton(draw, ip(m_jumpButtonCenter), dr * 0.55f, m_jumpHeld, "J", base, fill);
    drawButton(draw, ip(m_interactButtonCenter), dr * 0.50f, m_interactHeld, "E", base, fill);
    drawButton(draw, ip(m_pauseButtonCenter), dr * 0.52f, m_pauseHeld, "ESC", base, fill);
  }

  bool overlayEnabled() const {
    return m_config.enabled;
  }

private:
  enum class FingerRole {
    None,
    Joystick,
    Aim,
    SuppressedTap,
    SecondaryHold,
    JumpButton,
    InteractButton,
    PauseButton
  };

  struct FingerState {
    FingerRole role = FingerRole::None;
    Vec2F startPos;
    Vec2F currentPos;
    int64_t downTimeMs = 0;
    bool movedTooFarForTap = false;
  };

  static bool insideCircle(Vec2F const& p, Vec2F const& center, float radius) {
    return (p - center).magnitudeSquared() <= radius * radius;
  }

  // Returns the scale factor between physical pixels (m_windowSize) and the
  // logical pixels that ImGui's draw list expects. On standard-density displays
  // (Android, desktop) this is 1.0; on Retina iOS devices it is typically 2 or 3.
  float drawPixelScale() const {
    if (!m_displayScalePtr)
      return 1.0f;
    return std::max(1.0f, std::round(*m_displayScalePtr));
  }

  float controlRadius() const {
    float shortSide = (float)std::min((*m_windowSize)[0], (*m_windowSize)[1]);
    return 56.0f * m_config.size * std::max(1.0f, shortSide / 720.0f);
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
    if (x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f) {
      return {
        x * (float)(*m_windowSize)[0],
        y * (float)(*m_windowSize)[1]
      };
    }

    return {
      x,
      y
    };
  }

  Vec2F toInputSpace(Vec2F const& pos) const {
    return {
      pos[0],
      (float)(*m_windowSize)[1] - pos[1]
    };
  }

  void assignFinger(uint64_t finger, Vec2F const& pos) {
    if (m_fingers.contains(finger))
      return;

    float radius = controlRadius();
    updateButtonCenters(radius);

    FingerState state;
    state.startPos = pos;
    state.currentPos = pos;
    state.downTimeMs = Time::monotonicMilliseconds();
    float w = (float)(*m_windowSize)[0];
    float h = (float)(*m_windowSize)[1];
    if (insideCircle(pos, m_pauseButtonCenter, radius * 0.60f)) {
      state.role = FingerRole::PauseButton;
      m_pauseHeld = true;
    } else if (insideCircle(pos, m_jumpButtonCenter, radius * 0.70f)) {
      state.role = FingerRole::JumpButton;
      m_jumpHeld = true;
    } else if (insideCircle(pos, m_interactButtonCenter, radius * 0.65f)) {
      state.role = FingerRole::InteractButton;
      m_interactHeld = true;
    } else if (pos[0] < w * 0.35f && pos[1] > h * 0.60f && !m_joystickActive) {
      state.role = FingerRole::Joystick;
      m_joystickActive = true;
      m_joystickFinger = finger;
      m_joystickOrigin = pos;
      m_joystickCurrent = pos;
      m_moveVec = {};
    } else if (m_joystickActive && insideCircle(pos, m_joystickOrigin, radius * 1.35f)) {
      // Prevent accidental aiming while thumb rides around the virtual joystick.
      // Quick taps in this region still become UI clicks on release.
      state.role = FingerRole::SuppressedTap;
    } else if (m_primaryHeld && m_aimFinger != finger && !m_secondaryHeld) {
      state.role = FingerRole::SecondaryHold;
      m_secondaryHeld = true;
      m_secondaryFinger = finger;
      m_secondaryTouchPos = m_primaryHeld ? m_primaryTouchPos : pos;

      // Let right-click behavior take precedence while secondary gesture is held.
      if (m_primaryMouseHeld) {
        emitMouseUp(m_primaryTouchPos);
        m_primaryMouseHeld = false;
        m_primaryPausedForSecondary = true;
      }

      m_secondaryMouseHeld = true;
      emitMouseMove(m_secondaryTouchPos);
      emitMouseDown(m_secondaryTouchPos, MouseButton::Right);
    } else {
      state.role = FingerRole::Aim;
      m_aimFinger = finger;
      m_primaryHeld = true;
      m_primaryMouseHeld = true;
      m_primaryTouchPos = pos;
      emitMouseMove(pos);
      emitMouseDown(pos);
    }

    m_fingers[finger] = state;
  }

  void updateFinger(uint64_t finger, Vec2F const& pos) {
    auto ptr = m_fingers.ptr(finger);
    if (!ptr)
      return;

    trackTapMotion(*ptr, pos);

    if (ptr->role == FingerRole::Joystick) {
      m_joystickCurrent = pos;
      float radius = controlRadius();
      Vec2F delta = pos - m_joystickOrigin;
      float mag = delta.magnitude();
      if (mag > radius)
        delta = delta / mag * radius;

      float normMag = delta.magnitude() / radius;
      if (normMag < m_config.deadzone)
        m_moveVec = {};
      else
        m_moveVec = delta / radius;
    } else if (ptr->role == FingerRole::Aim) {
      m_primaryTouchPos = pos;
      emitMouseMove(pos);
    } else if (ptr->role == FingerRole::SecondaryHold && m_secondaryMouseHeld) {
      m_secondaryTouchPos = m_primaryHeld ? m_primaryTouchPos : pos;
      emitMouseMove(m_secondaryTouchPos);
    }
  }

  void releaseFinger(uint64_t finger, Vec2F const& pos) {
    auto ptr = m_fingers.ptr(finger);
    if (!ptr)
      return;

    bool tap = isTap(*ptr, pos);

    switch (ptr->role) {
      case FingerRole::Joystick:
        if (tap && m_moveVec.magnitudeSquared() <= m_config.deadzone * m_config.deadzone)
          emitMouseClick(pos);
        m_joystickActive = false;
        m_moveVec = {};
        m_joystickFinger = 0;
        break;
      case FingerRole::Aim:
        if (m_primaryMouseHeld) {
          emitMouseUp(pos);
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
          emitMouseUp(m_secondaryTouchPos, MouseButton::Right);
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
          emitMouseDown(m_primaryTouchPos);
          m_primaryMouseHeld = true;
        }
        m_primaryPausedForSecondary = false;
        break;
      case FingerRole::JumpButton:
        m_jumpHeld = false;
        break;
      case FingerRole::InteractButton:
        m_interactHeld = false;
        break;
      case FingerRole::PauseButton:
        m_pauseHeld = false;
        break;
      default:
        break;
    }

    m_fingers.remove(finger);
  }

  void updateButtonCenters(float radius) {
    float w = (float)(*m_windowSize)[0];
    float h = (float)(*m_windowSize)[1];
    float pad = radius * 1.25f;

    m_jumpButtonCenter = Vec2F(w - pad * 1.2f, h - pad * 1.4f);
    m_interactButtonCenter = Vec2F(w - pad * 2.7f, h - pad * 1.8f);
    m_pauseButtonCenter = Vec2F(pad * 1.35f, pad * 1.15f);
  }

  void emitActionEdges() {
    setActionKey(m_moveVec[0] > 0.30f, Key::D, m_rightHeld);
    setActionKey(m_moveVec[0] < -0.30f, Key::A, m_leftHeld);
    setActionKey(m_moveVec[1] < -0.30f, Key::W, m_upHeld);
    setActionKey(m_moveVec[1] > 0.30f, Key::S, m_downHeld);
    setActionKey(m_jumpHeld, Key::Space, m_jumpKeyHeld);
    setActionKey(m_interactHeld, Key::E, m_interactKeyHeld);
    setActionKey(m_pauseHeld, Key::Escape, m_pauseKeyHeld);

    if (m_primaryHeld && !m_primaryMouseHeld && !m_secondaryMouseHeld) {
      m_primaryMouseHeld = true;
      emitMouseMove(m_primaryTouchPos);
      emitMouseDown(m_primaryTouchPos);
    } else if (!m_primaryHeld && m_primaryMouseHeld) {
      m_primaryMouseHeld = false;
      emitMouseUp(m_primaryTouchPos);
    }
  }

  void setActionKey(bool desired, Key key, bool& held) {
    if (desired && !held) {
      held = true;
      emitEvent(KeyDownEvent{key, noMods()});
    } else if (!desired && held) {
      held = false;
      emitEvent(KeyUpEvent{key});
    }
  }

  void clearActions() {
    setActionKey(false, Key::D, m_rightHeld);
    setActionKey(false, Key::A, m_leftHeld);
    setActionKey(false, Key::W, m_upHeld);
    setActionKey(false, Key::S, m_downHeld);
    setActionKey(false, Key::Space, m_jumpKeyHeld);
    setActionKey(false, Key::E, m_interactKeyHeld);
    setActionKey(false, Key::Escape, m_pauseKeyHeld);

    if (m_primaryMouseHeld) {
      m_primaryMouseHeld = false;
      emitMouseUp(m_primaryTouchPos);
    }
    if (m_secondaryMouseHeld) {
      m_secondaryMouseHeld = false;
      emitMouseUp(m_secondaryTouchPos, MouseButton::Right);
    }

    m_secondaryHeld = false;
    m_secondaryFinger = 0;
    m_primaryPausedForSecondary = false;
  }

  void emitMouseMove(Vec2F const& pos) {
    emitEvent(MouseMoveEvent{{0, 0}, toInputSpace(pos)});
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

  static void drawButton(ImDrawList* draw, ImVec2 center, float radius, bool held, char const* label, ImU32 base, ImU32 fill) {
    draw->AddCircle(center, radius, base, 48, 3.0f);
    if (held)
      draw->AddCircleFilled(center, radius, fill, 32);
    draw->AddText(ImVec2(center.x - radius * 0.2f, center.y - radius * 0.35f), base, label);
  }

  Vec2U* m_windowSize;
  float* m_displayScalePtr = nullptr;
  MobileTouchConfig m_config;
  List<InputEvent> m_generatedEvents;

  StableHashMap<uint64_t, FingerState> m_fingers;

  bool m_joystickActive = false;
  uint64_t m_joystickFinger = 0;
  uint64_t m_aimFinger = 0;
  Vec2F m_joystickOrigin;
  Vec2F m_joystickCurrent;
  Vec2F m_moveVec;

  Vec2F m_jumpButtonCenter;
  Vec2F m_interactButtonCenter;
  Vec2F m_pauseButtonCenter;

  bool m_primaryHeld = false;
  bool m_primaryMouseHeld = false;
  Vec2F m_primaryTouchPos;
  bool m_primaryPausedForSecondary = false;

  bool m_secondaryHeld = false;
  bool m_secondaryMouseHeld = false;
  uint64_t m_secondaryFinger = 0;
  Vec2F m_secondaryTouchPos;

  bool m_jumpHeld = false;
  bool m_interactHeld = false;
  bool m_pauseHeld = false;

  bool m_rightHeld = false;
  bool m_leftHeld = false;
  bool m_upHeld = false;
  bool m_downHeld = false;
  bool m_jumpKeyHeld = false;
  bool m_interactKeyHeld = false;
  bool m_pauseKeyHeld = false;
};

class MobilePlatform {
public:
  MobilePlatform(ApplicationUPtr application, StringList cmdLineArgs)
    : m_application(std::move(application)) {
    // Keep mobile startup arguments deterministic. RootLoader's parseOrDie()
    // terminates the process when unexpected positional arguments are present.
    // SDL injects argv[0] on Android, so we don't forward raw command-line args.
    _unused(cmdLineArgs);
  }

  int run() {
    androidLogInfo("Mobile run() start");
    setupSdl();
    setupWindowAndRenderer();
    setupImGui();

    m_storageRoot = SDL_GetPrefPath("OpenStarbound", "OpenStarbound");
    if (m_storageRoot.empty())
      m_storageRoot = "./";

    m_platformServices = MobilePlatformServices::create(m_storageRoot);

    LauncherState launcher;
    loadLauncherState(launcher);

    while (!m_quitRequested && !runLauncher(launcher)) {
      Thread::sleepPrecise(4);
    }

    if (m_quitRequested)
      return 0;

    while (!m_quitRequested) {
      if (!prepareBootConfig(launcher, launcher.lastError)) {
        if (launcher.lastError.empty())
          launcher.lastError = "Failed to prepare runtime boot configuration.";
        launcher.lastStatus = "Launch failed. Check imported files and storage permissions.";
        m_quitRequested = false;
        while (!m_quitRequested && !runLauncher(launcher))
          Thread::sleepPrecise(4);
        continue;
      }

      if (startApplication(launcher.lastError))
        break;

      launcher.lastStatus = "Launch failed. Fix the issue and try again.";
      m_quitRequested = false;

      while (!m_quitRequested && !runLauncher(launcher))
        Thread::sleepPrecise(4);
    }

    if (m_quitRequested)
      return 0;

    m_runtimeExitReason.clear();
    try {
      androidLogInfo("Entering game loop");
      runGameLoop();
    } catch (std::exception const& e) {
      auto message = strf("{}", outputException(e, true));
      Logger::error("Runtime loop failed: {}", message);
      launcher.lastError = message;
      launcher.lastStatus = "Runtime failure. Review error and try again.";
      if (m_platformServices && m_platformServices->mobileSystemUiService())
        m_platformServices->mobileSystemUiService()->showDialog("Runtime Error", message);
    } catch (...) {
      Logger::error("Runtime loop failed: unknown error");
      launcher.lastError = "Unknown runtime failure";
      launcher.lastStatus = "Runtime failure. Review error and try again.";
      if (m_platformServices && m_platformServices->mobileSystemUiService())
        m_platformServices->mobileSystemUiService()->showDialog("Runtime Error", "Unknown runtime failure");
    }
    if (launcher.lastError.empty() && !m_runtimeExitReason.empty()) {
      launcher.lastError = m_runtimeExitReason;
      launcher.lastStatus = "Returned to launcher.";
    }
    shutdownApplication();
    androidLogInfo("Returning to launcher after shutdown");
    m_runtimeExitReason.clear();
    m_softQuitRequested = false;
    m_quitRequested = false;
    while (!m_quitRequested && !runLauncher(launcher))
      Thread::sleepPrecise(4);

    shutdownImGui();
    teardownSdl();
    return 0;
  }

private:
  struct Controller final : ApplicationController {
    explicit Controller(MobilePlatform* p) : parent(p) {}

    void setTargetUpdateRate(float targetUpdateRate) override {
      parent->m_updateTicker.setTargetTickRate(targetUpdateRate);
    }

    void setUpdateTrackWindow(float updateTrackWindow) override {
      parent->m_updateTicker.setWindow(updateTrackWindow);
    }

    void setMaxFrameSkip(unsigned maxFrameSkip) override {
      parent->m_maxFrameSkip = maxFrameSkip;
    }

    void setApplicationTitle(String title) override {
      parent->m_windowTitle = std::move(title);
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
      // Avoid activity title churn on Android; keep immersive surface stable.
      _unused(parent);
#else
      SDL_SetWindowTitle(parent->m_window, parent->m_windowTitle.utf8Ptr());
#endif
    }

    void setFullscreenWindow(Vec2U) override {
      // Android manages the native surface/window bounds; forcing fullscreen
      // transitions here can race with surface recreation.
      _unused(parent);
    }

    void setNormalWindow(Vec2U size) override {
      // Ignore desktop-style window sizing on mobile.
      _unused(parent);
      _unused(size);
    }

    void setMaximizedWindow() override {
      // Ignore desktop-style maximize on mobile.
      _unused(parent);
    }

    void setBorderlessWindow() override {
      // Ignore desktop-style borderless/maximize on mobile.
      _unused(parent);
    }

    void setVSyncEnabled(bool enabled) override {
      // Avoid Android vsync receiver races during surface size/inset changes.
      _unused(enabled);
      androidLogInfo("setVSyncEnabled(%d) ignored on Android", enabled ? 1 : 0);
      parent->m_vsync = false;
    }

    void setCursorVisible(bool cursorVisible) override {
      parent->m_cursorVisible = cursorVisible;
    }

    void setCursorPosition(Vec2I cursorPosition) override {
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
      // Cursor warping is desktop-only and can trigger unstable window/input
      // interactions on mobile.
      _unused(parent);
      _unused(cursorPosition);
#else
      SDL_WarpMouseInWindow(parent->m_window, cursorPosition[0], cursorPosition[1]);
#endif
    }

    void setCursorHardware(bool cursorHardware) override {
      parent->m_cursorHardware = cursorHardware;
    }

    bool setCursorImage(String const&, ImageConstPtr const&, unsigned, Vec2I const&) override {
      return false;
    }

    void setAcceptingTextInput(bool acceptingTextInput) override {
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
      // Apply IME state from the main loop so startup and SDL lifecycle remain
      // stable on mobile while still opening the native keyboard for textboxes.
      parent->m_textInput = acceptingTextInput;
      parent->m_textInputDirty = true;
#else
      if (acceptingTextInput)
        SDL_StartTextInput(parent->m_window);
      else
        SDL_StopTextInput(parent->m_window);
      parent->m_textInput = acceptingTextInput;
      parent->m_textInputApplied = acceptingTextInput;
      parent->m_textInputDirty = false;
#endif
    }

    void setTextArea(Maybe<pair<RectI, int>> area = {}) override {
      if (!parent->m_window)
        return;
      if (area) {
        SDL_Rect rect{
          area->first.xMin(),
          (int)parent->m_windowSize.y() - area->first.yMax(),
          area->first.width(),
          area->first.height()
        };
#ifdef STAR_SYSTEM_IOS
        // SDL3 window coordinates on iOS are in logical pixels (points);
        // game interface coordinates are physical pixels, so scale down.
        float scale = std::max(1.0f, std::round(parent->m_displayScale));
        rect.x = (int)std::round((float)rect.x / scale);
        rect.y = (int)std::round((float)rect.y / scale);
        rect.w = (int)std::round((float)rect.w / scale);
        rect.h = (int)std::round((float)rect.h / scale);
        SDL_SetTextInputArea(parent->m_window, &rect, (int)std::round((float)area->second / scale));
#else
        SDL_SetTextInputArea(parent->m_window, &rect, area->second);
#endif
      } else {
        SDL_SetTextInputArea(parent->m_window, nullptr, 0);
      }
    }

    AudioFormat enableAudio() override {
      {
        std::lock_guard<std::mutex> lock(parent->m_audioMutex);
        parent->m_audioEnabled = true;
      }
      if (parent->m_sdlAudioOutputStream)
        SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(parent->m_sdlAudioOutputStream));
      else
        Logger::warn("Application: enableAudio requested but playback stream is unavailable");
      return {44100, 2};
    }

    void disableAudio() override {
      {
        std::lock_guard<std::mutex> lock(parent->m_audioMutex);
        parent->m_audioEnabled = false;
      }
      if (parent->m_sdlAudioOutputStream)
        SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(parent->m_sdlAudioOutputStream));
    }

    bool openAudioInputDevice(uint32_t, int, int, AudioCallback) override {
      return false;
    }

    bool closeAudioInputDevice() override {
      return false;
    }

    bool hasClipboard() override {
      return SDL_HasClipboardText();
    }

    bool setClipboard(String text) override {
      return SDL_SetClipboardText(text.utf8Ptr());
    }

    bool setClipboardData(StringMap<ByteArray>) override {
      return false;
    }

    bool setClipboardImage(Image const&, ByteArray*, String const*) override {
      return false;
    }

    bool setClipboardFile(String const&) override {
      return false;
    }

    Maybe<String> getClipboard() override {
      if (!SDL_HasClipboardText())
        return {};
      char* text = SDL_GetClipboardText();
      if (!text)
        return {};
      String out = text;
      SDL_free(text);
      return out;
    }

    bool isFocused() const override {
      return true;
    }

    float updateRate() const override {
      return parent->m_updateRate;
    }

    float renderFps() const override {
      return parent->m_renderRate;
    }

    float getDisplayScale() const override {
      return parent->m_displayScale;
    }

    StatisticsServicePtr statisticsService() const override {
      return {};
    }

    P2PNetworkingServicePtr p2pNetworkingService() const override {
      return {};
    }

    UserGeneratedContentServicePtr userGeneratedContentService() const override {
      return {};
    }

    DesktopServicePtr desktopService() const override {
      return parent->m_platformServices ? parent->m_platformServices->desktopService() : DesktopServicePtr{};
    }

    ExternalFileAccessServicePtr externalFileAccessService() const override {
      return parent->m_platformServices ? parent->m_platformServices->externalFileAccessService() : ExternalFileAccessServicePtr{};
    }

    MobileSystemUiServicePtr mobileSystemUiService() const override {
      return parent->m_platformServices ? parent->m_platformServices->mobileSystemUiService() : MobileSystemUiServicePtr{};
    }

    LaunchConfigServicePtr launchConfigService() const override {
      return parent->m_platformServices ? parent->m_platformServices->launchConfigService() : LaunchConfigServicePtr{};
    }

    void quit() override {
      // Treat in-engine quits as a soft return-to-launcher on mobile.
      // Hard process exits on some Android devices race SDL vsync teardown.
      parent->m_softQuitRequested = true;
      parent->m_quitRequested = false;
      parent->m_runtimeExitReason = "Game requested quit and returned to launcher.";
      androidLogInfo("Controller::quit invoked");
    }

    MobilePlatform* parent;
  };

  void setupSdl() {
#ifdef STAR_SYSTEM_IOS
    // We provide our own iOS main wrapper; mark main ready before SDL_Init.
    SDL_SetMainReady();
#endif
#ifdef STAR_SYSTEM_ANDROID
    SDL_SetHint(SDL_HINT_ANDROID_ALLOW_RECREATE_ACTIVITY, "0");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    SDL_SetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS, "0");
#if defined(SDL_HINT_TOUCH_MOUSE_EVENTS)
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
#endif
#if defined(SDL_HINT_MOUSE_TOUCH_EVENTS)
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
#endif
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
      throw ApplicationException::format("Could not initialize SDL: {}", SDL_GetError());

    SDL_AudioSpec desired = {SDL_AUDIO_S16, 2, 44100};
    m_sdlAudioOutputStream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &desired,
        [](void* userdata, SDL_AudioStream* stream, int len, int) {
          if (len <= 0)
            return;

          auto* platform = static_cast<MobilePlatform*>(userdata);
          std::lock_guard<std::mutex> lock(platform->m_audioMutex);

          platform->m_audioOutputData.resize((size_t)len);
          if (platform->m_audioEnabled && platform->m_application) {
            platform->m_application->getAudioData(
                reinterpret_cast<int16_t*>(platform->m_audioOutputData.data()),
                (size_t)len / 4);
          } else {
            std::memset(platform->m_audioOutputData.data(), 0, (size_t)len);
          }

          SDL_PutAudioStreamData(stream, platform->m_audioOutputData.data(), len);
        },
        this);

    if (!m_sdlAudioOutputStream) {
      Logger::error("Application: Could not open mobile audio device, no sound available!");
    } else {
      Logger::info("Application: Opened mobile audio device with 44.1khz / 16 bit stereo audio");
      SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(m_sdlAudioOutputStream));
    }
  }

  void setupWindowAndRenderer() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

#ifdef STAR_SYSTEM_ANDROID
    // Keep Android window mode stable to avoid Surface/VSync receiver races
    // observed during dynamic inset / resize transitions.
    Uint64 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    int windowWidth = 1280;
    int windowHeight = 720;
#elif defined(STAR_SYSTEM_IOS)
    // iOS SDL windows are effectively fullscreen already. Requesting explicit
    // fullscreen mode here can fail on some devices during launch.
    Uint64 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    int windowWidth = 0;
    int windowHeight = 0;
#else
    Uint64 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    int windowWidth = 1280;
    int windowHeight = 720;
#endif

    m_window = SDL_CreateWindow("OpenStarbound", windowWidth, windowHeight, windowFlags);
    if (!m_window)
      throw ApplicationException::format("Could not create SDL window: {}", SDL_GetError());

#ifdef STAR_SYSTEM_IOS
    StarIosBridge_setSdlWindow(m_window);
#endif

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext)
      throw ApplicationException::format("Could not create GLES context: {}", SDL_GetError());

    // Avoid touching swap interval at startup on Android. Some devices crash
    // in VsyncReceiver when swap interval state mutates while surfaces settle.
    m_vsync = false;

    m_renderer = make_shared<GlesRenderer>();

#ifdef STAR_SYSTEM_IOS
    // iOS EAGL binds a non-zero viewFramebuffer after context creation.
    // FBO 0 is the null framebuffer on iOS, not the screen. Save the real
    // screen FBO so the renderer and startup UI can target it correctly.
    {
      GLint iosFbo = 0;
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &iosFbo);
      m_renderer->setScreenFramebuffer(static_cast<GLuint>(iosFbo));
      androidLogInfo("setupWindowAndRenderer: iOS screen FBO = %d", iosFbo);
    }
#endif

    syncWindowMetrics(false);
    m_renderer->setScreenSize(m_windowSize);
  }

  void setupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.TouchExtraPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(14.0f, 10.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);
    style.ScrollbarSize = std::max(style.ScrollbarSize, 28.0f);
    refreshImGuiScale();
    ImGui_ImplSDL3_InitForOpenGL(m_window, m_glContext);
    ImGui_ImplOpenGL3_Init("#version 300 es");
  }

  void shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }

  void teardownSdl() {
    if (m_sdlAudioOutputStream) {
      SDL_CloseAudioDevice(SDL_GetAudioStreamDevice(m_sdlAudioOutputStream));
      m_sdlAudioOutputStream = nullptr;
    }
#ifdef STAR_SYSTEM_ANDROID
    // Avoid explicit SDL video teardown on Android. Some device/driver stacks
    // abort in SDL's VsyncReceiver path while teardown races in-flight callbacks.
    m_glContext = nullptr;
    m_window = nullptr;
    return;
#endif
#ifdef STAR_SYSTEM_IOS
    StarIosBridge_setSdlWindow(nullptr);
#endif
    if (m_glContext) {
      SDL_GL_DestroyContext(m_glContext);
      m_glContext = nullptr;
    }
    if (m_window) {
      SDL_DestroyWindow(m_window);
      m_window = nullptr;
    }
    if (SDL_WasInit(0))
      SDL_Quit();
  }

  List<LauncherModEntry> scanInstalledMods(String const& modsPath) {
    List<LauncherModEntry> mods;

    if (!File::isDirectory(modsPath))
      File::makeDirectoryRecursive(modsPath);

    for (auto const& entry : File::dirList(modsPath)) {
      String entryName = entry.first;
      bool isDirectory = entry.second;

      bool isPak = !isDirectory && entryName.endsWith(".pak", String::CaseInsensitive);
      if (!isDirectory && !isPak)
        continue;

      LauncherModEntry mod;
      mod.displayName = entryName;
      mod.path = File::relativeTo(modsPath, entryName);
      mod.isDirectory = isDirectory;
      mod.isPackedPak = isPak;
      mods.append(std::move(mod));
    }

    mods.sort([](LauncherModEntry const& a, LauncherModEntry const& b) {
      auto aLower = a.displayName.toLower();
      auto bLower = b.displayName.toLower();
      if (aLower == bLower)
        return a.displayName < b.displayName;
      return aLower < bLower;
    });

    return mods;
  }

  void refreshModList(LauncherState& state, String const& modsPath) {
    state.modEntries = scanInstalledMods(modsPath);
    state.modListDirty = false;
  }

  void loadLauncherState(LauncherState& state) {
    auto configService = m_platformServices->launchConfigService();
    Json config = configService ? configService->loadLauncherConfig() : JsonObject();

    state.packedPakPath = config.optString("packedPakPath").value(""
    );

    state.touchConfig.enabled = config.queryBool("touch.enabled", true);
    state.touchConfig.opacity = config.queryFloat("touch.opacity", 0.35f);
    state.touchConfig.size = config.queryFloat("touch.size", 1.0f);
    state.touchConfig.deadzone = config.queryFloat("touch.deadzone", 0.15f);

    state.canLaunch = File::isFile(state.packedPakPath);
    state.lastStatus = state.canLaunch ? "Using existing packed.pak" : "Please import packed.pak";
  }

  bool runLauncher(LauncherState& state) {
    syncWindowMetrics(false);
    processWindowEvents();
    syncWindowMetrics(false);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImVec2 displaySize = imguiDisplaySize();
    float margin = std::max(10.0f, std::min(displaySize.x, displaySize.y) * 0.0125f);
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displaySize.x - margin * 2.0f, displaySize.y - margin * 2.0f), ImGuiCond_Always);
    ImGui::Begin(
      "OpenStarbound Mobile Loader",
      nullptr,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
    );

    auto runLauncherAction = [&state](String const& actionName, std::function<void()> const& fn) {
      try {
        fn();
      } catch (std::exception const& e) {
        state.lastStatus = strf("{} failed.", actionName);
        state.lastError = strf("({}) {}", actionName, outputException(e, true));
        Logger::error("Launcher action '{}' failed: {}", actionName, state.lastError);
      } catch (...) {
        state.lastStatus = strf("{} failed.", actionName);
        state.lastError = strf("({}) Unknown runtime failure", actionName);
        Logger::error("Launcher action '{}' failed: unknown runtime failure", actionName);
      }
    };

    auto modsPath = modsDirectoryPath();
    if (!File::isDirectory(modsPath))
      File::makeDirectoryRecursive(modsPath);

    bool launchPressed = false;

    if (state.modManagerOpen) {
      if (state.modListDirty)
        refreshModList(state, modsPath);

      ImGui::Text("Mod Manager");
      ImGui::TextWrapped("Browse installed mods, import new mods, and delete entries.");
      ImGui::Separator();

      if (ImGui::Button("Back to Launcher"))
        state.modManagerOpen = false;
      ImGui::SameLine();
      if (ImGui::Button("Refresh List"))
        state.modListDirty = true;

      ImGui::InputTextWithHint("##modsearch", "Search mods...", state.modSearchBuffer, sizeof(state.modSearchBuffer));
      ImGui::Checkbox("Show .pak mods", &state.modShowPackedPaks);
      ImGui::SameLine();
      ImGui::Checkbox("Show unpacked folders", &state.modShowUnpackedFolders);
      ImGui::TextWrapped("Mods directory: %s", modsPath.utf8Ptr());

      if (ImGui::Button("Import mod (.pak)")) {
        runLauncherAction("Import mod (.pak)", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto imported = svc->importModPakFiles();
            state.lastStatus = imported.empty() ? "No mod imported." : strf("Imported {} mod(s)", imported.size());
            if (imported.empty())
              state.lastError = "No .pak selected or import failed.";
            else
              state.lastError.clear();
            state.modListDirty = true;
          } else {
            state.lastStatus = "Import unavailable.";
            state.lastError = "ExternalFileAccessService is unavailable on this platform build.";
          }
        });
      }
      if (ImGui::Button("Import mod folder (single mod)")) {
        runLauncherAction("Import mod folder", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto imported = svc->importSingleModFolder();
            state.lastStatus = imported.empty() ? "No mod folder imported." : strf("Imported {} mod(s)", imported.size());
            if (imported.empty())
              state.lastError = "No folder selected or import failed.";
            else
              state.lastError.clear();
            state.modListDirty = true;
          } else {
            state.lastStatus = "Import unavailable.";
            state.lastError = "ExternalFileAccessService is unavailable on this platform build.";
          }
        });
      }
      if (ImGui::Button("Import mods folder (all .pak and folders)")) {
        runLauncherAction("Import mods folder", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto imported = svc->importModsDirectory();
            state.lastStatus = imported.empty() ? "No mods imported." : strf("Imported {} mod(s)", imported.size());
            if (imported.empty())
              state.lastError = "No folder selected or no valid mods found.";
            else
              state.lastError.clear();
            state.modListDirty = true;
          } else {
            state.lastStatus = "Import unavailable.";
            state.lastError = "ExternalFileAccessService is unavailable on this platform build.";
          }
        });
      }

      ImGui::Separator();
      String modSearch = String(state.modSearchBuffer).trim();
      size_t shownMods = 0;
      bool requestDeletePopup = false;
      ImGui::Text("Installed mods: %u", (unsigned)state.modEntries.size());
      if (ImGui::BeginChild("ModManagerList", ImVec2(0, -90.0f), true)) {
        for (auto const& mod : state.modEntries) {
          if (mod.isPackedPak && !state.modShowPackedPaks)
            continue;
          if (mod.isDirectory && !state.modShowUnpackedFolders)
            continue;
          if (!modSearch.empty() && !mod.displayName.contains(modSearch, String::CaseInsensitive))
            continue;

          shownMods++;
          ImGui::PushID(mod.path.utf8Ptr());
          ImGui::TextUnformatted(mod.displayName.utf8Ptr());
          ImGui::SameLine();
          ImGui::TextDisabled("[%s]", mod.isDirectory ? "folder" : ".pak");
          ImGui::SameLine();
          ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 110.0f));
          if (ImGui::Button("Delete", ImVec2(90.0f, 0.0f))) {
            state.pendingDeletePath = mod.path;
            state.pendingDeleteName = mod.displayName;
            state.pendingDeleteIsDirectory = mod.isDirectory;
            requestDeletePopup = true;
          }
          ImGui::PopID();
        }

        if (shownMods == 0) {
          if (!modSearch.empty())
            ImGui::TextDisabled("No mods match your search.");
          else
            ImGui::TextDisabled("No mods are currently installed.");
        }
      }
      ImGui::EndChild();

      if (requestDeletePopup)
        ImGui::OpenPopup("Delete Mod?");

      if (ImGui::BeginPopupModal("Delete Mod?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Delete mod '%s'?", state.pendingDeleteName.utf8Ptr());
        ImGui::TextWrapped("This removes it from the game's internal mods directory.");
        if (ImGui::Button("Delete", ImVec2(140.0f, 0.0f))) {
          runLauncherAction("Delete mod", [&]() {
            if (!state.pendingDeletePath.empty()) {
              if (state.pendingDeleteIsDirectory)
                File::removeDirectoryRecursive(state.pendingDeletePath);
              else
                File::remove(state.pendingDeletePath);
              state.lastStatus = strf("Deleted mod '{}'.", state.pendingDeleteName);
              state.lastError.clear();
              state.modListDirty = true;
            }
          });
          state.pendingDeletePath.clear();
          state.pendingDeleteName.clear();
          state.pendingDeleteIsDirectory = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
          state.pendingDeletePath.clear();
          state.pendingDeleteName.clear();
          state.pendingDeleteIsDirectory = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    } else {
      ImGui::TextWrapped("Configure assets and controls before launching.");
      ImGui::Separator();

      ImGui::Text("packed.pak: %s", state.packedPakPath.empty() ? "<not selected>" : state.packedPakPath.utf8Ptr());

      if (ImGui::Button("Pick packed.pak")) {
        runLauncherAction("Pick packed.pak", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto picked = svc->pickPackedPak();
            if (picked) {
              state.packedPakPath = *picked;
              state.lastStatus = "Imported packed.pak";
              state.lastError.clear();
            } else {
              state.lastStatus = "No file selected.";
              state.lastError = "Native picker unavailable or canceled.";
            }
          } else {
            state.lastStatus = "Native picker unavailable.";
            state.lastError = "ExternalFileAccessService is unavailable on this platform build.";
          }
        });
      }

      ImGui::Separator();
      ImGui::TextWrapped("Current mods directory: %s", modsPath.utf8Ptr());
      if (ImGui::Button("Mod Manager")) {
        state.modManagerOpen = true;
        state.modListDirty = true;
      }

      ImGui::Separator();
      ImGui::Text("Touch Controls");
      ImGui::Checkbox("Enable touch overlay", &state.touchConfig.enabled);
      ImGui::SliderFloat("Overlay opacity", &state.touchConfig.opacity, 0.0f, 1.0f);
      ImGui::SliderFloat("Overlay size", &state.touchConfig.size, 0.6f, 1.8f);
      ImGui::SliderFloat("Joystick deadzone", &state.touchConfig.deadzone, 0.0f, 0.6f);

      state.canLaunch = File::isFile(state.packedPakPath);
      if (!state.canLaunch)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Launch is disabled until packed.pak is imported.");

      if (!state.canLaunch)
        ImGui::BeginDisabled();
      launchPressed = ImGui::Button("Launch");
      if (!state.canLaunch)
        ImGui::EndDisabled();
    }

    if (!state.lastStatus.empty())
      ImGui::Text("Status: %s", state.lastStatus.utf8Ptr());
    if (!state.lastError.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", state.lastError.utf8Ptr());

    ImGui::End();

    ImGui::Render();
#ifdef STAR_SYSTEM_IOS
    glBindFramebuffer(GL_FRAMEBUFFER, m_renderer->screenFramebuffer());
#endif
    glViewport(0, 0, (int)m_windowSize[0], (int)m_windowSize[1]);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);

    if (launchPressed) {
      androidLogInfo("Launch pressed");
      persistLauncherState(state);
    }

    return launchPressed;
  }

  void persistLauncherState(LauncherState const& state) {
    Json config = JsonObject{
      {"packedPakPath", state.packedPakPath},
      {"touch", JsonObject{
        {"enabled", state.touchConfig.enabled},
        {"opacity", state.touchConfig.opacity},
        {"size", state.touchConfig.size},
        {"deadzone", state.touchConfig.deadzone}
      }}
    };

    if (auto configService = m_platformServices->launchConfigService())
      configService->saveLauncherConfig(config);
  }

  bool prepareBootConfig(LauncherState const& state, String& errorMessage) {
    if (!File::isFile(state.packedPakPath)) {
      errorMessage = "Selected packed.pak was not found. Please re-select it.";
      return false;
    }

    auto modsPath = modsDirectoryPath();
    File::makeDirectoryRecursive(modsPath);

    String bundledAssetsRoot;
#if STAR_SYSTEM_ANDROID
    String bundledStorageRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    if (auto synced = AndroidFileAccessBridge::syncBundledAssets(bundledStorageRoot)) {
      bundledAssetsRoot = *synced;
      androidLogInfo("prepareBootConfig: bundled assets synced to %s", bundledAssetsRoot.utf8Ptr());
    } else {
      androidLogInfo("prepareBootConfig: bundled assets sync failed");
      errorMessage = "Failed to load bundled OpenStarbound assets from the app package.";
      return false;
    }
#elif STAR_SYSTEM_IOS
    String bundledStorageRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    if (auto synced = IosFileAccessBridge::syncBundledAssets(bundledStorageRoot)) {
      bundledAssetsRoot = *synced;
    } else {
      errorMessage = "Failed to load bundled OpenStarbound assets from the iOS app package.";
      return false;
    }
#else
    if (auto basePath = SDL_GetBasePath())
      bundledAssetsRoot = File::convertDirSeparators(File::relativeTo(basePath, "../assets"));
    else
      bundledAssetsRoot = "../assets";
#endif

    unsigned hwThreads = std::max(1u, std::thread::hardware_concurrency());
    unsigned workerPoolSize = std::clamp(hwThreads > 2 ? hwThreads - 1 : hwThreads, 2u, 6u);

    Json bootConfig = JsonObject{
      {"assetDirectories", JsonArray{bundledAssetsRoot, modsPath}},
      {"assetSources", JsonArray{state.packedPakPath}},
      {"assetsSettings", JsonObject{
        {"skipDigest", true},
        {"skipPreload", true},
        {"digestIgnore", JsonArray{".*"}},
        {"workerPoolSize", workerPoolSize}
      }},
      {"storageDirectory", m_storageRoot},
      {"logDirectory", File::relativeTo(m_storageRoot, "logs")},
      {"defaultConfiguration", JsonObject{
        {"mobile", JsonObject{
          {"touchControls", JsonObject{
            {"enabled", state.touchConfig.enabled},
            {"opacity", state.touchConfig.opacity},
            {"size", state.touchConfig.size},
            {"deadzone", state.touchConfig.deadzone},
            {"invertLook", false}
          }}
        }}
      }}
    };

    m_bootConfigPath = File::relativeTo(m_storageRoot, "sbinit.mobile.config");
    try {
      File::overwriteFileWithRename(bootConfig.printJson(2), m_bootConfigPath);
      return true;
    } catch (...) {
      return false;
    }
  }

  bool startApplication(String& errorMessage) {
    try {
      androidLogInfo("startApplication begin");
      m_runtimeArgs.clear();
      m_runtimeArgs.append("-bootconfig");
      m_runtimeArgs.append(m_bootConfigPath);

      auto startupDone = make_shared<std::atomic<bool>>(false);
      auto startupSucceeded = make_shared<std::atomic<bool>>(false);
      auto startupError = make_shared<String>();
      auto startupErrorMutex = make_shared<Mutex>();
      auto startupStatus = make_shared<String>("Preparing startup...");
      auto startupStatusMutex = make_shared<Mutex>();

      auto startupThread = Thread::invoke("Mobile::ClientStartup", [this, startupDone, startupSucceeded, startupError, startupErrorMutex, startupStatus, startupStatusMutex]() {
          try {
            {
              MutexLocker locker(*startupStatusMutex);
              *startupStatus = "Loading game assets and configuration...";
            }
            setMobileStartupStatus("Loading game assets and configuration...");
            androidLogInfo("startApplication(worker): application->startup begin");
            m_application->startup(m_runtimeArgs);
            androidLogInfo("startApplication(worker): application->startup done");
            {
              MutexLocker locker(*startupStatusMutex);
              *startupStatus = "Startup completed. Initializing game systems...";
            }
            startupSucceeded->store(true, std::memory_order_release);
          } catch (std::exception const& e) {
            MutexLocker locker(*startupErrorMutex);
            *startupError = strf("{}", outputException(e, true));
          } catch (...) {
            MutexLocker locker(*startupErrorMutex);
            *startupError = "Unknown startup failure";
          }

          startupDone->store(true, std::memory_order_release);
        });

      int64_t startupWaitStart = Time::monotonicMilliseconds();
      int64_t lastProgressLog = startupWaitStart;
      while (!startupDone->load(std::memory_order_acquire) && !m_quitRequested) {
        syncWindowMetrics(false);
        processWindowEvents();
        syncWindowMetrics(false);
        String status;
        {
          MutexLocker locker(*startupStatusMutex);
          status = *startupStatus;
        }
        auto detailedStatus = getMobileStartupStatus();
        if (!detailedStatus.empty())
          status = detailedStatus;
        renderStartupScreen(status);
        int64_t now = Time::monotonicMilliseconds();
        if (now - lastProgressLog >= 5000) {
          androidLogInfo("startApplication: waiting %lldms (%s)",
            (long long)(now - startupWaitStart), status.utf8Ptr());
          lastProgressLog = now;
        }
        Thread::sleepPrecise(8);
      }

      startupThread.finish();

      if (m_quitRequested) {
        errorMessage = "Launch canceled.";
        return false;
      }

      if (!startupSucceeded->load(std::memory_order_acquire)) {
        MutexLocker locker(*startupErrorMutex);
        errorMessage = startupError->empty() ? String("Startup failed before initialization.") : *startupError;
        return false;
      }

      setMobileStartupStatus("Initializing game controller...");
      renderStartupScreen(getMobileStartupStatus());
      androidLogInfo("startApplication: make Controller");
      m_appController = make_shared<Controller>(this);
      setMobileStartupStatus("Initializing game systems...");
      renderStartupScreen(getMobileStartupStatus());
      androidLogInfo("startApplication: applicationInit");
      m_application->applicationInit(m_appController);
      setMobileStartupStatus("Initializing renderer...");
      renderStartupScreen(getMobileStartupStatus());
      androidLogInfo("startApplication: renderInit");
      m_application->renderInit(m_renderer);
      setMobileStartupStatus("Renderer initialized...");
      renderStartupScreen(getMobileStartupStatus());
      setMobileStartupStatus("Initializing touch controls...");
      renderStartupScreen(getMobileStartupStatus());
      androidLogInfo("startApplication: create touch adapter");
      m_touchAdapter = make_unique<MobileTouchInputAdapter>(&m_windowSize, &m_displayScale);

      if (auto configService = m_platformServices->launchConfigService()) {
        auto cfg = configService->loadLauncherConfig();
        MobileTouchConfig touch;
        touch.enabled = cfg.queryBool("touch.enabled", true);
        touch.opacity = cfg.queryFloat("touch.opacity", 0.35f);
        touch.size = cfg.queryFloat("touch.size", 1.0f);
        touch.deadzone = cfg.queryFloat("touch.deadzone", 0.15f);
        m_touchAdapter->setConfig(touch);
      }

      if (m_softQuitRequested || m_quitRequested) {
        m_softQuitRequested = false;
        m_quitRequested = false;
        errorMessage = "Game initialization requested quit.";
        androidLogInfo("startApplication aborted: initialization requested quit");
        return false;
      }

      androidLogInfo("startApplication success");
      return true;
    } catch (std::exception const& e) {
      errorMessage = strf("{}", outputException(e, true));
      Logger::error("Failed starting mobile application: {}", errorMessage);
      return false;
    } catch (...) {
      errorMessage = "Unknown startup failure";
      Logger::error("Failed starting mobile application: {}", errorMessage);
      return false;
    }
  }

  void renderStartupScreen(String const& status) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImVec2 displaySize = imguiDisplaySize();
    float margin = std::max(10.0f, std::min(displaySize.x, displaySize.y) * 0.0125f);
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displaySize.x - margin * 2.0f, displaySize.y - margin * 2.0f), ImGuiCond_Always);
    ImGui::Begin(
      "OpenStarbound Mobile Loader",
      nullptr,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
    );
    ImGui::TextWrapped("%s", status.utf8Ptr());
    ImGui::Separator();
    ImGui::TextWrapped("Please wait. Assets and configuration are loading.");
    ImGui::End();

    ImGui::Render();
#ifdef STAR_SYSTEM_IOS
    glBindFramebuffer(GL_FRAMEBUFFER, m_renderer->screenFramebuffer());
#endif
    glViewport(0, 0, (int)m_windowSize[0], (int)m_windowSize[1]);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);
  }

  void runGameLoop() {
    m_updateTicker.reset();
    m_renderTicker.reset();

    while (!m_quitRequested && !m_softQuitRequested) {
#ifdef STAR_SYSTEM_IOS
      SDL_PumpEvents();
      SDL_GL_MakeCurrent(m_window, m_glContext);
#endif
      syncWindowMetrics(true);
      syncTextInputState();
      auto inputEvents = processEvents();

      for (auto const& event : inputEvents)
        m_application->processInput(event);

      bool overlayEnabled = m_touchAdapter && m_touchAdapter->overlayEnabled();
      if (overlayEnabled) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
      }

      int updatesBehind = std::max<int>(round(m_updateTicker.ticksBehind()), 1);
      updatesBehind = std::min<int>(updatesBehind, (int)m_maxFrameSkip + 1);
      for (int i = 0; i < updatesBehind; ++i) {
        if (overlayEnabled) {
          // Keep ImGui frame state consistent when we catch up multiple updates.
          if (i != 0)
            ImGui::EndFrame();
          ImGui::NewFrame();
        }
        m_application->update();
        m_updateRate = m_updateTicker.tick();
      }

      m_renderer->startFrame();
      m_application->render();

      if (overlayEnabled) {
        m_touchAdapter->drawOverlay();
        ImGui::Render();
      }

      m_renderer->finishFrame();
      if (overlayEnabled)
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      SDL_GL_SwapWindow(m_window);

      m_renderRate = m_renderTicker.tick();

      if (m_signalHandler.interruptCaught())
        m_quitRequested = true;

      int64_t spare = round(m_updateTicker.spareTime() * 1000.0);
      if (spare > 0)
        Thread::sleepPrecise(spare);
    }
  }

  void shutdownApplication() {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_audioEnabled = false;
    if (m_sdlAudioOutputStream)
      SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(m_sdlAudioOutputStream));

    if (!m_application)
      return;

    try {
      m_application->shutdown();
    } catch (...) {
    }
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    if (m_textInputApplied && m_window)
      SDL_StopTextInput(m_window);
    m_textInputApplied = false;
    m_textInputDirty = false;
#endif
    m_application.reset();
  }

  List<InputEvent> processEvents() {
    List<InputEvent> events;

    SDL_Event event;
    bool overlayEnabled = m_touchAdapter && m_touchAdapter->overlayEnabled();
    ImGuiIO* io = overlayEnabled ? &ImGui::GetIO() : nullptr;

    m_touchAdapter->beginFrame();

    while (SDL_PollEvent(&event)) {
      bool touchDerivedMouse = overlayEnabled && isTouchDerivedMouseEvent(event);
      if (event.type != SDL_EVENT_FINGER_DOWN
          && event.type != SDL_EVENT_FINGER_UP
          && event.type != SDL_EVENT_FINGER_MOTION) {
        convertEventToRenderCoordinatesIfPossible(m_window, &event);
      }
      if (overlayEnabled && !touchDerivedMouse)
        ImGui_ImplSDL3_ProcessEvent(&event);

      if (event.type == SDL_EVENT_QUIT) {
#ifdef STAR_SYSTEM_ANDROID
        // Ignore synthetic/native quit events on Android to avoid racing
        // teardown against in-flight SDL VSync callbacks.
        continue;
#else
        m_quitRequested = true;
        continue;
#endif
      }

      if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        syncWindowMetrics(true);
        continue;
      }

      if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        syncWindowMetrics(true);
        continue;
      }

      if (m_touchAdapter->processSdlEvent(event))
        continue;
      if (touchDerivedMouse)
        continue;

      Maybe<InputEvent> input;
      if (event.type == SDL_EVENT_KEY_DOWN && (!overlayEnabled || (!io->WantCaptureKeyboard || !io->WantTextInput)) && !event.key.repeat) {
        if (auto key = keyFromSdlKeyCode(event.key.key))
          input.set(KeyDownEvent{*key, (KeyMod)event.key.mod});
      } else if (event.type == SDL_EVENT_KEY_UP) {
        if (auto key = keyFromSdlKeyCode(event.key.key))
          input.set(KeyUpEvent{*key});
      } else if (event.type == SDL_EVENT_TEXT_INPUT && (!overlayEnabled || !io->WantTextInput)) {
        input.set(TextInputEvent{event.text.text});
      } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        input.set(MouseMoveEvent{{event.motion.xrel, -event.motion.yrel}, {event.motion.x, (int)m_windowSize[1] - event.motion.y}});
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && (!overlayEnabled || !io->WantCaptureMouse)) {
        input.set(MouseButtonDownEvent{mouseButtonFromSdlMouseButton(event.button.button), {event.button.x, (int)m_windowSize[1] - event.button.y}});
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && (!overlayEnabled || !io->WantCaptureMouse)) {
        input.set(MouseButtonUpEvent{mouseButtonFromSdlMouseButton(event.button.button), {event.button.x, (int)m_windowSize[1] - event.button.y}});
      } else if (event.type == SDL_EVENT_MOUSE_WHEEL && (!overlayEnabled || !io->WantCaptureMouse)) {
        input.set(MouseWheelEvent{event.wheel.y < 0 ? MouseWheel::Down : MouseWheel::Up, {event.wheel.mouse_x, (int)m_windowSize[1] - event.wheel.mouse_y}});
      } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        input.set(ControllerAxisEvent{(ControllerId)event.gaxis.which, controllerAxisFromSdlControllerAxis(event.gaxis.axis), (float)event.gaxis.value / 32768.0f});
      } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        input.set(ControllerButtonDownEvent{(ControllerId)event.gbutton.which, controllerButtonFromSdlControllerButton(event.gbutton.button)});
      } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        input.set(ControllerButtonUpEvent{(ControllerId)event.gbutton.which, controllerButtonFromSdlControllerButton(event.gbutton.button)});
      }

      if (input)
        events.append(input.take());
    }

    syncWindowMetrics(true);

    m_touchAdapter->endFrame();
    m_touchAdapter->appendGeneratedEvents(events);
    return events;
  }

  void processWindowEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      convertEventToRenderCoordinatesIfPossible(m_window, &event);
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT)
#ifdef STAR_SYSTEM_ANDROID
        continue;
#else
        m_quitRequested = true;
#endif
      if (event.type == SDL_EVENT_WINDOW_RESIZED
          || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
          || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        syncWindowMetrics(false);
      }
    }

    syncWindowMetrics(false);
  }

  bool queryWindowPixelSize(Vec2U& outSize) const {
    if (!m_window)
      return false;

    int width = 0;
    int height = 0;

    if (SDL_GetWindowSizeInPixels(m_window, &width, &height) && width > 0 && height > 0) {
      outSize = Vec2U((unsigned)width, (unsigned)height);
      return true;
    }

    width = 0;
    height = 0;
    if (SDL_GetWindowSize(m_window, &width, &height) && width > 0 && height > 0) {
      outSize = Vec2U((unsigned)width, (unsigned)height);
      return true;
    }

    return false;
  }

  bool syncWindowMetrics(bool notifyApplication) {
    Vec2U queriedSize;
    bool hasSize = queryWindowPixelSize(queriedSize);

    bool sizeChanged = false;
    if (hasSize && queriedSize != m_windowSize) {
      m_windowSize = queriedSize;
      sizeChanged = true;
    }

    if (m_window) {
      float displayScale = SDL_GetWindowDisplayScale(m_window);
      if (displayScale > 0.0f)
        m_displayScale = displayScale;
    }

    if (sizeChanged) {
      if (m_renderer)
        m_renderer->setScreenSize(m_windowSize);

      if (notifyApplication && m_application)
        m_application->windowChanged(WindowMode::Normal, m_windowSize);

      refreshImGuiScale();
    } else if (ImGui::GetCurrentContext()) {
      // Keep launcher/UI touch sizing aligned when only display scale changes.
      refreshImGuiScale();
    }

    return sizeChanged;
  }

  void refreshImGuiScale() {
    if (!ImGui::GetCurrentContext())
      return;

    auto& io = ImGui::GetIO();
#ifdef STAR_SYSTEM_IOS
    float pixelScale = std::max(1.0f, std::round(m_displayScale));
    float shortSide = (float)std::min(m_windowSize[0], m_windowSize[1]) / pixelScale;
    io.FontGlobalScale = std::clamp(shortSide / 900.0f, 0.85f, 1.15f);
#else
    float shortSide = (float)std::min(m_windowSize[0], m_windowSize[1]);
    io.FontGlobalScale = std::clamp(shortSide / 500.0f, 1.35f, 2.2f);
#endif
  }

  ImVec2 imguiDisplaySize() const {
    if (ImGui::GetCurrentContext()) {
      ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      if (displaySize.x > 0.0f && displaySize.y > 0.0f)
        return displaySize;
    }

#ifdef STAR_SYSTEM_IOS
    float pixelScale = std::max(1.0f, std::round(m_displayScale));
    return ImVec2((float)m_windowSize[0] / pixelScale, (float)m_windowSize[1] / pixelScale);
#else
    return ImVec2((float)m_windowSize[0], (float)m_windowSize[1]);
#endif
  }

  void syncTextInputState() {
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    if (!m_window)
      return;
    if (!m_textInputDirty && m_textInputApplied == m_textInput)
      return;

    if (m_textInput && !m_textInputApplied)
      SDL_StartTextInput(m_window);
    else if (!m_textInput && m_textInputApplied)
      SDL_StopTextInput(m_window);

    m_textInputApplied = m_textInput;
    m_textInputDirty = false;
#endif
  }

  String modsDirectoryPath() const {
    auto fallbackModsPath = File::relativeTo(m_storageRoot, "mods");
#if STAR_SYSTEM_ANDROID
    if (auto resolvedPath = AndroidFileAccessBridge::resolveModsDirectory(fallbackModsPath))
      return *resolvedPath;
#elif STAR_SYSTEM_IOS
    if (auto resolvedPath = IosFileAccessBridge::resolveModsDirectory(fallbackModsPath))
      return *resolvedPath;
#endif
    return fallbackModsPath;
  }

  ApplicationUPtr m_application;
  StringList m_runtimeArgs;

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_glContext = nullptr;

  String m_windowTitle = "OpenStarbound";
  Vec2U m_windowSize = {1280, 720};
  bool m_vsync = true;
  bool m_cursorVisible = true;
  bool m_cursorHardware = false;
  bool m_textInput = false;
  bool m_textInputApplied = false;
  bool m_textInputDirty = false;
  bool m_audioEnabled = false;
  SDL_AudioStream* m_sdlAudioOutputStream = nullptr;
  std::vector<uint8_t> m_audioOutputData;
  std::mutex m_audioMutex;
  bool m_quitRequested = false;
  bool m_softQuitRequested = false;
  String m_runtimeExitReason;
  float m_displayScale = 1.0f;

  String m_storageRoot;
  String m_bootConfigPath;

  TickRateApproacher m_updateTicker{60.0f, 1.0f};
  TickRateMonitor m_renderTicker{1.0f};
  float m_updateRate = 0.0f;
  float m_renderRate = 0.0f;
  unsigned m_maxFrameSkip = 5;

  SignalHandler m_signalHandler;

  shared_ptr<GlesRenderer> m_renderer;
  ApplicationControllerPtr m_appController;
  MobilePlatformServicesUPtr m_platformServices;
  unique_ptr<MobileTouchInputAdapter> m_touchAdapter;
};

} // namespace

int runMainApplication(ApplicationUPtr application, StringList cmdLineArgs) {
  try {
    MobilePlatform platform(std::move(application), std::move(cmdLineArgs));
    return platform.run();
  } catch (std::exception const& e) {
    String message = strf("{}", outputException(e, true));
#ifdef STAR_SYSTEM_ANDROID
    __android_log_print(ANDROID_LOG_ERROR, "OpenStarbound", "Unhandled exception in runMainApplication: %s", message.utf8Ptr());
#endif
    Logger::error("Unhandled exception in runMainApplication: {}", message);
    // Avoid modal SDL message boxes on iOS fatal paths; they can deadlock app
    // shutdown if a UIKit presenter is unavailable.
#if !defined(STAR_SYSTEM_IOS)
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenStarbound Mobile", message.utf8Ptr(), nullptr);
#endif
    return 1;
  } catch (...) {
#ifdef STAR_SYSTEM_ANDROID
    __android_log_print(ANDROID_LOG_ERROR, "OpenStarbound", "Unhandled unknown exception in runMainApplication");
#endif
    String message = "Unknown fatal runtime error";
    Logger::error("{}", message);
    // Avoid modal SDL message boxes on iOS fatal paths; they can deadlock app
    // shutdown if a UIKit presenter is unavailable.
#if !defined(STAR_SYSTEM_IOS)
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenStarbound Mobile", message.utf8Ptr(), nullptr);
#endif
    return 1;
  }
}

}
