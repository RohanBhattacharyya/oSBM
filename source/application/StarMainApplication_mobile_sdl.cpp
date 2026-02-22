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
#include <mutex>
#include <thread>
#include <vector>
#ifdef STAR_SYSTEM_ANDROID
#include <android/log.h>
#endif

namespace Star {

namespace {

#ifdef STAR_SYSTEM_ANDROID
void androidLogInfo(char const* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  __android_log_vprint(ANDROID_LOG_INFO, "OpenStarbound", fmt, args);
  va_end(args);
}
#else
void androidLogInfo(char const*, ...) {}
#endif

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

struct LauncherState {
  bool canLaunch = false;
  String packedPakPath;
  String packedPakImportSource;
  String modImportSource;
  String lastError;
  String lastStatus;
  MobileTouchConfig touchConfig;
};

class MobileTouchInputAdapter {
public:
  explicit MobileTouchInputAdapter(Vec2U* windowSize)
    : m_windowSize(windowSize) {}

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

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImU32 base = IM_COL32(255, 255, 255, (int)(180.0f * std::clamp(m_config.opacity, 0.0f, 1.0f)));
    ImU32 fill = IM_COL32(80, 160, 255, (int)(140.0f * std::clamp(m_config.opacity, 0.0f, 1.0f)));

    if (m_joystickActive) {
      draw->AddCircle(ImVec2(m_joystickOrigin[0], m_joystickOrigin[1]), radius, base, 48, 3.0f);
      draw->AddCircleFilled(ImVec2(m_joystickCurrent[0], m_joystickCurrent[1]), radius * 0.45f, fill, 32);
    }

    drawButton(draw, m_jumpButtonCenter, radius * 0.55f, m_jumpHeld, "J", base, fill);
    drawButton(draw, m_interactButtonCenter, radius * 0.50f, m_interactHeld, "E", base, fill);
    drawButton(draw, m_altButtonCenter, radius * 0.50f, m_altHeld, "A", base, fill);
    drawButton(draw, m_pauseButtonCenter, radius * 0.52f, m_pauseHeld, "ESC", base, fill);
  }

  bool overlayEnabled() const {
    return m_config.enabled;
  }

private:
  enum class FingerRole {
    None,
    Joystick,
    Aim,
    JumpButton,
    InteractButton,
    AltButton,
    PauseButton
  };

  struct FingerState {
    FingerRole role = FingerRole::None;
  };

  static bool insideCircle(Vec2F const& p, Vec2F const& center, float radius) {
    return (p - center).magnitudeSquared() <= radius * radius;
  }

  float controlRadius() const {
    float shortSide = (float)std::min((*m_windowSize)[0], (*m_windowSize)[1]);
    return 56.0f * m_config.size * std::max(1.0f, shortSide / 720.0f);
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
    } else if (insideCircle(pos, m_altButtonCenter, radius * 0.65f)) {
      state.role = FingerRole::AltButton;
      m_altHeld = true;
      if (!m_altMouseHeld) {
        m_altMouseHeld = true;
        emitMouseDown(m_altButtonCenter, MouseButton::Right);
      }
    } else if (pos[0] < w * 0.35f && pos[1] > h * 0.60f && !m_joystickActive) {
      state.role = FingerRole::Joystick;
      m_joystickActive = true;
      m_joystickFinger = finger;
      m_joystickOrigin = pos;
      m_joystickCurrent = pos;
      m_moveVec = {};
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
    }
  }

  void releaseFinger(uint64_t finger, Vec2F const& pos) {
    auto ptr = m_fingers.ptr(finger);
    if (!ptr)
      return;

    switch (ptr->role) {
      case FingerRole::Joystick:
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
      case FingerRole::JumpButton:
        m_jumpHeld = false;
        break;
      case FingerRole::InteractButton:
        m_interactHeld = false;
        break;
      case FingerRole::AltButton:
        m_altHeld = false;
        if (m_altMouseHeld) {
          m_altMouseHeld = false;
          emitMouseUp(m_altButtonCenter, MouseButton::Right);
        }
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
    m_altButtonCenter = Vec2F(w - pad * 2.0f, h - pad * 3.1f);
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

    if (m_altHeld && !m_altMouseHeld) {
      m_altMouseHeld = true;
      emitMouseDown(m_altButtonCenter, MouseButton::Right);
    } else if (!m_altHeld && m_altMouseHeld) {
      m_altMouseHeld = false;
      emitMouseUp(m_altButtonCenter, MouseButton::Right);
    }

    if (m_primaryHeld && !m_primaryMouseHeld) {
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

    if (m_altMouseHeld) {
      m_altMouseHeld = false;
      emitMouseUp(m_altButtonCenter, MouseButton::Right);
    }

    if (m_primaryMouseHeld) {
      m_primaryMouseHeld = false;
      emitMouseUp(m_primaryTouchPos);
    }
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

  void emitEvent(InputEvent const& event) {
    m_generatedEvents.append(event);
  }

  static void drawButton(ImDrawList* draw, Vec2F const& center, float radius, bool held, char const* label, ImU32 base, ImU32 fill) {
    draw->AddCircle(ImVec2(center[0], center[1]), radius, base, 48, 3.0f);
    if (held)
      draw->AddCircleFilled(ImVec2(center[0], center[1]), radius, fill, 32);
    draw->AddText(ImVec2(center[0] - radius * 0.2f, center[1] - radius * 0.35f), base, label);
  }

  Vec2U* m_windowSize;
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
  Vec2F m_altButtonCenter;
  Vec2F m_pauseButtonCenter;

  bool m_primaryHeld = false;
  bool m_primaryMouseHeld = false;
  Vec2F m_primaryTouchPos;
  bool m_jumpHeld = false;
  bool m_interactHeld = false;
  bool m_altHeld = false;
  bool m_pauseHeld = false;

  bool m_rightHeld = false;
  bool m_leftHeld = false;
  bool m_upHeld = false;
  bool m_downHeld = false;
  bool m_jumpKeyHeld = false;
  bool m_interactKeyHeld = false;
  bool m_pauseKeyHeld = false;
  bool m_altMouseHeld = false;
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
#ifdef STAR_SYSTEM_ANDROID
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
#ifdef STAR_SYSTEM_ANDROID
      // Cursor warping is desktop-only and can trigger unstable window/input
      // interactions on Android.
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
#ifdef STAR_SYSTEM_ANDROID
      // Keep mobile startup/input path stable by avoiding direct SDL IME
      // toggles during runtime initialization.
      parent->m_textInput = acceptingTextInput;
#else
      if (acceptingTextInput)
        SDL_StartTextInput(parent->m_window);
      else
        SDL_StopTextInput(parent->m_window);
      parent->m_textInput = acceptingTextInput;
#endif
    }

    void setTextArea(Maybe<pair<RectI, int>> area = {}) override {
#ifdef STAR_SYSTEM_ANDROID
      _unused(parent);
      _unused(area);
#else
      if (area) {
        SDL_Rect rect{
          area->first.xMin(),
          (int)parent->m_windowSize.y() - area->first.yMax(),
          area->first.width(),
          area->first.height()
        };
        SDL_SetTextInputArea(parent->m_window, &rect, area->second);
      } else {
        SDL_SetTextInputArea(parent->m_window, nullptr, 0);
      }
#endif
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
#ifdef STAR_SYSTEM_ANDROID
    SDL_SetHint(SDL_HINT_ANDROID_ALLOW_RECREATE_ACTIVITY, "0");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    SDL_SetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS, "0");
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
#else
    Uint64 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif

    m_window = SDL_CreateWindow("OpenStarbound", 1280, 720, windowFlags);
    if (!m_window)
      throw ApplicationException::format("Could not create SDL window: {}", SDL_GetError());

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext)
      throw ApplicationException::format("Could not create GLES context: {}", SDL_GetError());

    // Avoid touching swap interval at startup on Android. Some devices crash
    // in VsyncReceiver when swap interval state mutates while surfaces settle.
    m_vsync = false;

    int w = 0, h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    m_windowSize = Vec2U((unsigned)w, (unsigned)h);

    m_displayScale = SDL_GetWindowDisplayScale(m_window);

    m_renderer = make_shared<GlesRenderer>();
    m_renderer->setScreenSize(m_windowSize);
  }

  void setupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
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
    processWindowEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)m_windowSize[0] - 48.0f, (float)m_windowSize[1] - 48.0f), ImGuiCond_Always);
    ImGui::Begin(
      "OpenStarbound Mobile Loader",
      nullptr,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
    );

    ImGui::TextWrapped("Configure assets and controls before launching.");
    ImGui::Separator();

    ImGui::Text("packed.pak: %s", state.packedPakPath.empty() ? "<not selected>" : state.packedPakPath.utf8Ptr());

    if (ImGui::Button("Pick packed.pak")) {
      if (auto svc = m_platformServices->externalFileAccessService()) {
        auto picked = svc->pickPackedPak();
        if (picked) {
          state.packedPakPath = *picked;
          state.lastStatus = "Imported packed.pak";
          state.lastError.clear();
        } else {
          state.lastError = "Native picker unavailable or canceled.";
        }
      }
    }

    ImGui::Separator();

    if (ImGui::Button("Import mods (native picker)")) {
      if (auto svc = m_platformServices->externalFileAccessService()) {
        auto imported = svc->importModFiles();
        state.lastStatus = imported.empty() ? "No mods imported" : strf("Imported {} mod(s)", imported.size());
      }
    }

    if (ImGui::Button("Open mods directory")) {
      if (auto svc = m_platformServices->externalFileAccessService())
        svc->openModsLocationInSystemBrowser();
    }

    ImGui::Separator();
    ImGui::Text("Touch Controls");
    ImGui::Checkbox("Enable touch overlay", &state.touchConfig.enabled);
    ImGui::SliderFloat("Overlay opacity", &state.touchConfig.opacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Overlay size", &state.touchConfig.size, 0.6f, 1.8f);
    ImGui::SliderFloat("Joystick deadzone", &state.touchConfig.deadzone, 0.0f, 0.6f);

    state.canLaunch = File::isFile(state.packedPakPath);
    if (!state.lastStatus.empty())
      ImGui::Text("Status: %s", state.lastStatus.utf8Ptr());
    if (!state.lastError.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", state.lastError.utf8Ptr());

    if (!state.canLaunch)
      ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "Launch is disabled until packed.pak is imported.");

    if (!state.canLaunch)
      ImGui::BeginDisabled();
    bool launchPressed = ImGui::Button("Launch");
    if (!state.canLaunch)
      ImGui::EndDisabled();

    ImGui::End();

    ImGui::Render();
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

    auto modsPath = File::relativeTo(m_storageRoot, "mods");
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
        processWindowEvents();
        String status;
        {
          MutexLocker locker(*startupStatusMutex);
          status = *startupStatus;
        }
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

      androidLogInfo("startApplication: make Controller");
      m_appController = make_shared<Controller>(this);
      androidLogInfo("startApplication: applicationInit");
      m_application->applicationInit(m_appController);
      androidLogInfo("startApplication: renderInit");
      m_application->renderInit(m_renderer);
      androidLogInfo("startApplication: create touch adapter");
      m_touchAdapter = make_unique<MobileTouchInputAdapter>(&m_windowSize);

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

    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)m_windowSize[0] - 48.0f, (float)m_windowSize[1] - 48.0f), ImGuiCond_Always);
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
    m_application.reset();
  }

  List<InputEvent> processEvents() {
    List<InputEvent> events;

    SDL_Event event;
    bool overlayEnabled = m_touchAdapter && m_touchAdapter->overlayEnabled();
    ImGuiIO* io = overlayEnabled ? &ImGui::GetIO() : nullptr;

    m_touchAdapter->beginFrame();

    while (SDL_PollEvent(&event)) {
      if (event.type != SDL_EVENT_FINGER_DOWN
          && event.type != SDL_EVENT_FINGER_UP
          && event.type != SDL_EVENT_FINGER_MOTION) {
        SDL_ConvertEventToRenderCoordinates(SDL_GetRenderer(m_window), &event);
      }
      if (overlayEnabled)
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
        m_windowSize = Vec2U((unsigned)event.window.data1, (unsigned)event.window.data2);
        m_renderer->setScreenSize(m_windowSize);
        m_application->windowChanged(WindowMode::Normal, m_windowSize);
        continue;
      }

      if (m_touchAdapter->processSdlEvent(event))
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

    m_touchAdapter->endFrame();
    m_touchAdapter->appendGeneratedEvents(events);
    return events;
  }

  void processWindowEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      SDL_ConvertEventToRenderCoordinates(SDL_GetRenderer(m_window), &event);
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT)
#ifdef STAR_SYSTEM_ANDROID
        continue;
#else
        m_quitRequested = true;
#endif
      if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        m_windowSize = Vec2U((unsigned)event.window.data1, (unsigned)event.window.data2);
    }
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
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenStarbound Mobile", message.utf8Ptr(), nullptr);
    return 1;
  } catch (...) {
#ifdef STAR_SYSTEM_ANDROID
    __android_log_print(ANDROID_LOG_ERROR, "OpenStarbound", "Unhandled unknown exception in runMainApplication");
#endif
    String message = "Unknown fatal runtime error";
    Logger::error("{}", message);
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenStarbound Mobile", message.utf8Ptr(), nullptr);
    return 1;
  }
}

}
