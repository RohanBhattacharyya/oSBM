#include "StarApplication.hpp"
#include "StarApplicationController.hpp"
#include "StarByteArray.hpp"
#include "StarFile.hpp"
#include "StarImage.hpp"
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
// Installs native UIScreenEdgePanGestureRecognizers on the SDL window so that a
// true iOS left/right screen-edge swipe drives launcher back/forward navigation.
extern "C" void StarIosBridge_installLauncherEdgeSwipes();
// Appends a line to Documents/launch-trace.txt (user-retrievable via the
// Files app even when the app dies instantly at launch).
extern "C" void StarIosBridge_launchTrace(char const* msg);
#elif STAR_SYSTEM_SWITCH
#include "mobile/switch/StarSwitchPlatform.hpp"
#endif

#include "SDL3/SDL.h"
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
// Real mobile devices resolve GL entry points via EGL/SDL's GLES2 header.
// On a desktop launcher build the GL symbols come from GLEW (pulled in via
// StarRenderer_gles.hpp -> StarRenderer_opengl.hpp); including SDL_opengles2.h
// there would clash with GLEW's function-pointer macros for the same names.
#include "SDL3/SDL_opengles2.h"
#endif

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#ifdef STAR_SYSTEM_ANDROID
#include <jni.h>
#endif

#include "mobile/StarMobileAndroidGyro.hpp"
#include "mobile/StarMobileControls.hpp"
#include "mobile/StarMobileLauncherSupport.hpp"
#include "mobile/StarMobilePlatform.hpp"

#ifdef STAR_SYSTEM_ANDROID
std::mutex g_androidImGuiEditKeyMutex;
constexpr int AndroidImGuiEditKeyCount = 9;
constexpr std::array<ImGuiKey, AndroidImGuiEditKeyCount> AndroidImGuiKeys = {
  ImGuiKey_Backspace, ImGuiKey_Delete, ImGuiKey_Enter, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
  ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Home, ImGuiKey_End
};
struct AndroidImGuiKeyEvent {
  int key = 0;
};
std::vector<AndroidImGuiKeyEvent> g_androidImGuiEditKeyTaps;
std::array<bool, AndroidImGuiEditKeyCount> g_androidImGuiEditKeyDown = {};
std::atomic_bool g_androidImGuiTextInputActive = false;

ImGuiKey androidImGuiEditKey(int key) {
  if (key < 0 || key >= AndroidImGuiEditKeyCount)
    return ImGuiKey_None;
  return AndroidImGuiKeys[(size_t)key];
}

void setAndroidImGuiTextInputActive(bool active) {
  g_androidImGuiTextInputActive.store(active, std::memory_order_release);
}

void drainAndroidImGuiEditKeys() {
  if (!ImGui::GetCurrentContext())
    return;

  auto& io = ImGui::GetIO();
  bool releasedKey = false;
  for (size_t key = 0; key < g_androidImGuiEditKeyDown.size(); ++key) {
    if (g_androidImGuiEditKeyDown[key]) {
      if (auto imguiKey = androidImGuiEditKey((int)key); imguiKey != ImGuiKey_None)
        io.AddKeyEvent(imguiKey, false);
      g_androidImGuiEditKeyDown[key] = false;
      releasedKey = true;
    }
  }
  if (releasedKey)
    return;

  AndroidImGuiKeyEvent event;
  {
    std::lock_guard<std::mutex> lock(g_androidImGuiEditKeyMutex);
    if (g_androidImGuiEditKeyTaps.empty())
      return;
    event = g_androidImGuiEditKeyTaps.front();
    g_androidImGuiEditKeyTaps.erase(g_androidImGuiEditKeyTaps.begin());
  }

  auto imguiKey = androidImGuiEditKey(event.key);
  if (imguiKey == ImGuiKey_None)
    return;

  if (ImGui::IsKeyDown(imguiKey)) {
    io.AddKeyEvent(imguiKey, false);
    std::lock_guard<std::mutex> lock(g_androidImGuiEditKeyMutex);
    g_androidImGuiEditKeyTaps.insert(g_androidImGuiEditKeyTaps.begin(), event);
    return;
  }

  io.AddKeyEvent(imguiKey, true);
  g_androidImGuiEditKeyDown[(size_t)event.key] = true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_libsdl_app_SDLInputConnection_nativeTapEditingKey(JNIEnv*, jclass, jint key) {
  if (androidImGuiEditKey(key) == ImGuiKey_None || !g_androidImGuiTextInputActive.load(std::memory_order_acquire))
    return JNI_FALSE;

  std::lock_guard<std::mutex> lock(g_androidImGuiEditKeyMutex);
  g_androidImGuiEditKeyTaps.push_back(AndroidImGuiKeyEvent{(int)key});
  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_libsdl_app_SDLInputConnection_nativeIsEditingKeyTarget(JNIEnv*, jclass) {
  return g_androidImGuiTextInputActive.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}
#else
void setAndroidImGuiTextInputActive(bool) {}
void drainAndroidImGuiEditKeys() {}
#endif

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
// Pending launcher navigation requested from a native OS gesture/button. Written
// from the platform callback thread (Android JNI / iOS UIKit main thread) and
// consumed by the launcher loop. -1 = back, +1 = forward, 0 = none.
//
// This is deliberately sourced from real OS events rather than a synthetic touch
// recognizer: the Android system back button and its predictive-back edge-swipe
// gesture both arrive through MainActivity, and iOS screen-edge pans arrive via
// the Objective-C bridge. Both platforms funnel through starMobileRequestLauncherNav.
std::atomic<int> g_launcherNativeNavRequest{0};

extern "C" void starMobileRequestLauncherNav(int direction) {
  g_launcherNativeNavRequest.store(direction, std::memory_order_relaxed);
}
#endif

#ifdef STAR_SYSTEM_ANDROID
// Invoked from MainActivity's OnBackInvokedCallback (API 33+) and its
// onBackPressed() fallback (API < 33) whenever the OS delivers a back — whether
// from the navigation bar / hardware button or the edge-swipe back gesture.
extern "C" JNIEXPORT void JNICALL Java_io_github_openstarbound_mobile_MainActivity_nativeOnLauncherBack(JNIEnv*, jclass) {
  starMobileRequestLauncherNav(-1);
}
#endif

namespace Star {

// Defined in game/StarGameTypes.cpp; declared here to avoid pulling the game
// module's include path into the application module.
extern float GlobalTimestep;
extern float ServerGlobalTimestep;

String getMobileStartupStatus();
void setMobileStartupStatus(String const& status);

namespace {

struct LauncherModEntry {
  String displayName;
  String path;
  bool isDirectory = false;
  bool isPackedPak = false;
};

struct LauncherUiConfig {
  float scale = 1.0f;
  std::array<float, 3> accentColor = {0.28f, 0.55f, 1.0f};
};

struct LauncherActionResult {
  String status;
  String error;
  bool modListDirty = false;
  Maybe<String> packedPakPath;
};

struct LauncherTexture {
  GLuint textureId = 0;
  Vec2U size;
};

enum class GamepadGlyphFamily {
  Xbox,
  Playstation,
  Switch,
  SteamDeck
};

// Logical launcher navigation destinations. Each maps deterministically onto the
// LauncherState open-flags so that edge-swipe / back-button navigation and the
// in-panel buttons stay in sync through a single browser-style history stack.
enum class LauncherScreen {
  Main,
  UiSettings,
  TouchManager,
  GamepadManager,
  ModManager,
  SaveManager
};

static int64_t const LauncherQuickStartHoldMs = 1000;

struct LauncherState {
  ~LauncherState() {
    if (asyncActionThread.joinable())
      asyncActionThread.join();
  }

  bool canLaunch = false;
  String packedPakPath;
  String lastError;
  String lastStatus;
  LauncherUiConfig uiConfig;
  String locale;
  String systemLocale;
  bool uiSettingsOpen = false;
  MobileTouchConfig touchConfig;
  std::vector<MobileTouchElement> touchElements;
  MobileGamepadConfig gamepadConfig;
  std::vector<MobileGamepadBinding> gamepadBindings;
  bool touchManagerOpen = false;
  bool gamepadManagerOpen = false;
  bool touchPreviewOpen = false;
  int selectedTouchElement = 0;
  String touchLabelBufferElementId;
  char touchLabelBuffer[64] = {};
  // Virtual-keyboard macro editor state. Only one editor is open at a time; it is
  // keyed by the action-editor's bufferId so the modal applies back to the right
  // action. macroEditorKeys is the working, ordered key sequence being built.
  String macroEditorBufferId;
  bool macroEditorOpen = false;
  bool macroEditorSequential = true;
  std::vector<Key> macroEditorKeys;
  bool newTouchButtonPopup = false;
  int newTouchActionIndex = 0;
  float newTouchButtonSize = 1.0f;
  bool modManagerOpen = false;
  bool saveManagerOpen = false;
  bool modListDirty = true;
  bool modShowPackedPaks = true;
  bool modShowUnpackedFolders = true;
  char modSearchBuffer[256] = {};
  List<LauncherModEntry> modEntries;
  String pendingDeletePath;
  String pendingDeleteName;
  bool pendingDeleteIsDirectory = false;
  std::thread asyncActionThread;
  std::mutex asyncActionMutex;
  bool asyncActionRunning = false;
  bool asyncActionCompleted = false;
  String asyncActionName;
  LauncherActionResult asyncActionResult;

  // Browser-style navigation history for edge-swipe / back-button navigation.
  // currentScreen mirrors the active open-flag combination; navBackStack holds
  // the screens a "back" gesture unwinds through, navForwardStack the screens a
  // "forward" gesture re-enters. These persist for the launcher's lifetime.
  LauncherScreen currentScreen = LauncherScreen::Main;
  std::vector<LauncherScreen> navBackStack;
  std::vector<LauncherScreen> navForwardStack;
};


// Launch-stage breadcrumb. On iOS a crash during launcher init is otherwise
// completely silent: the engine log only starts once the game Root exists,
// and the in-app diagnostics exporter needs a running app. The trace names
// the last stage reached so an instant close is attributable from the Files
// app without a Mac. No-op elsewhere (Android has logcat, Switch has its own
// relaunch trace).
static void launchBreadcrumb(char const* stage) {
#ifdef STAR_SYSTEM_IOS
  StarIosBridge_launchTrace(stage);
#else
  _unused(stage);
#endif
}

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
    launchBreadcrumb("run start");
    setupSdl();
    launchBreadcrumb("sdl initialized");
    setupWindowAndRenderer();
    launchBreadcrumb("window + gl context ready");

    m_legacyStorageRoot = defaultMobileStorageRoot();
    m_storageRoot = writableMobileStorageRoot(m_legacyStorageRoot);
    launchBreadcrumb("storage root ready");
    m_platformServices = MobilePlatformServices::create(m_storageRoot);
    launchBreadcrumb("platform services ready");
    syncLauncherBundledAssets();
    launchBreadcrumb("bundled assets synced");
    reloadLauncherLocalization();
    setupImGui();
    launchBreadcrumb("imgui ready");

    LauncherState launcher;
    loadLauncherState(launcher);
    launchBreadcrumb("launcher state loaded; entering launcher");

    while (!m_quitRequested && !runLauncher(launcher)) {
      Thread::sleepPrecise(4);
    }

    if (m_quitRequested)
      return 0;

    // Outer loop: allows the user to return to the launcher after a game
    // session and relaunch without restarting the process.
    while (!m_quitRequested) {
      // Inner loop: show launcher on startup/config failures; break on success.
      while (!m_quitRequested) {
        relaunchBreadcrumb("prepare-boot-config");
        if (!prepareBootConfig(launcher, launcher.lastError)) {
          if (launcher.lastError.empty())
            launcher.lastError = launcherText("runtime.prepareBootConfigFailed", "Failed to prepare runtime boot configuration.");
          launcher.lastStatus = launcherText("runtime.launchFailedCheckFiles", "Launch failed. Check imported files and storage permissions.");
          m_quitRequested = false;
          while (!m_quitRequested && !runLauncher(launcher))
            Thread::sleepPrecise(4);
          continue;
        }

        if (startApplication(launcher.lastError))
          break;

        launcher.lastStatus = launcherText("runtime.launchFailedFixIssue", "Launch failed. Fix the issue and try again.");
        m_quitRequested = false;

        while (!m_quitRequested && !runLauncher(launcher))
          Thread::sleepPrecise(4);
      }

      if (m_quitRequested)
        break;

      m_runtimeExitReason.clear();
      try {
        relaunchBreadcrumb("game-loop-enter");
        androidLogInfo("Entering game loop");
#ifdef STAR_SYSTEM_SWITCH
        // In-game only (the launcher shouldn't burn battery at boost clocks).
        switchApplyClockBoost();
#endif
        runGameLoop();
      } catch (std::exception const& e) {
        auto message = strf("{}", outputException(e, true));
        Logger::error("Runtime loop failed: {}", message);
        launcher.lastError = message;
        launcher.lastStatus = launcherText("runtime.reviewErrorAndRetry", "Runtime failure. Review error and try again.");
        if (m_platformServices && m_platformServices->mobileSystemUiService())
          m_platformServices->mobileSystemUiService()->showDialog(launcherText("runtime.dialogTitle", "Runtime Error"), message);
      } catch (...) {
        Logger::error("Runtime loop failed: unknown error");
        launcher.lastError = launcherText("runtime.unknownFailure", "Unknown runtime failure");
        launcher.lastStatus = launcherText("runtime.reviewErrorAndRetry", "Runtime failure. Review error and try again.");
        if (m_platformServices && m_platformServices->mobileSystemUiService())
          m_platformServices->mobileSystemUiService()->showDialog(launcherText("runtime.dialogTitle", "Runtime Error"), launcherText("runtime.unknownFailure", "Unknown runtime failure"));
      }
      if (launcher.lastError.empty() && !m_runtimeExitReason.empty()) {
        launcher.lastError = m_runtimeExitReason;
        launcher.lastStatus = launcherText("runtime.returnedToLauncher", "Returned to launcher.");
      }
      shutdownApplication();
#if !defined(STAR_SYSTEM_ANDROID) && !defined(STAR_SYSTEM_IOS) && !defined(STAR_SYSTEM_SWITCH)
      // Restore the OS pointer for the ImGui launcher (runGameLoop hid it).
      SDL_ShowCursor();
#endif
#ifdef STAR_SYSTEM_SWITCH
      switchRestoreClocks();
#endif
      relaunchBreadcrumb("shutdown-application-done");
      androidLogInfo("Returning to launcher after shutdown");
      m_runtimeExitReason.clear();
      m_softQuitRequested = false;
      m_quitRequested = false;
      // Show the post-game launcher. If the user clicks Launch the outer loop
      // continues and starts a new session; if they quit m_quitRequested is set
      // and the outer loop exits to teardown.
      while (!m_quitRequested && !runLauncher(launcher))
        Thread::sleepPrecise(4);
    }

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

    void setMaxFrameRate(float maxFrameRate) override {
      parent->m_maxFrameRate = maxFrameRate;
      if (maxFrameRate > 0.0f)
        parent->m_framePacer.setTargetTickRate(maxFrameRate);
    }

    float updateTickFraction() const override {
      // Live phase: the accumulator residual plus time elapsed since it was
      // sampled, read at the moment the render snapshot consumes it. Using
      // the loop-start residual alone leaves a per-frame error equal to the
      // (variable) delay between loop start and snapshot, visible as
      // alternating sub-tick judder when frame times vary.
      double live = parent->m_tickAccum;
      if (parent->m_lastLoopTime > 0.0)
        live += (Time::monotonicTime() - parent->m_lastLoopTime) * parent->m_updateTicker.targetTickRate();
      return (float)clamp(live, 0.0, 1.0);
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

    void setFullscreenWindow(Vec2U size) override {
      // Mobile surfaces stay fullscreen; the Starbound resolution setting maps
      // to the logical render canvas that is upscaled into the safe area.
      parent->setRequestedRenderResolution(size);
    }

    void setNormalWindow(Vec2U size) override {
      parent->setRequestedRenderResolution(size);
    }

    void setMaximizedWindow() override {
      parent->setRequestedRenderResolution({});
    }

    void setBorderlessWindow() override {
      parent->setRequestedRenderResolution({});
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
#ifdef STAR_SYSTEM_SWITCH
      // Switch: deliberately NOT wired to SDL_StartTextInput. The game runs
      // its own blocking software-keyboard session per textbox focus
      // (ClientApplication::runSwitchKeyboardSession) because the SDL switch
      // port suppresses all touch polling while SDL text input is active --
      // with engine textboxes that keep focus until a (touch) blur, that is
      // an input deadlock the moment the keyboard closes. SDL text input
      // stays reserved for the ImGui launcher path, whose fields deactivate
      // themselves on the synthetic Return the SDL driver queues.
      parent->m_textInput = acceptingTextInput;
      parent->m_textInputApplied = acceptingTextInput;
      parent->m_textInputDirty = false;
#else
      // Android/iOS (native IME) and desktop (hardware keyboard): apply the
      // state from the main loop via syncTextInputState so SDL_StartTextInput
      // fires and SDL_EVENT_TEXT_INPUT is delivered while a textbox/chat has
      // focus. Without this, key presses arrive but typed characters do not.
      parent->m_textInput = acceptingTextInput;
      parent->m_textInputDirty = true;
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
      Logger::info("QUIT PATH: ApplicationController::quit (in-engine quit/return-to-launcher)");
      parent->m_softQuitRequested = true;
      parent->m_quitRequested = false;
      parent->m_runtimeExitReason = "Game requested quit and returned to launcher.";
      androidLogInfo("Controller::quit invoked");
    }

    MobilePlatform* parent;
  };

  void setupSdl() {
    m_signalHandler.setHandleInterrupt(true);
    m_signalHandler.setHandleFatal(true);

#ifdef STAR_SYSTEM_IOS
    // We provide our own iOS main wrapper; mark main ready before SDL_Init.
    SDL_SetMainReady();
#endif
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    // Declare all four orientations so SDL's platform orientation handler honours rotation.
    // Must be set before SDL_Init; platform manifests alone are not enough for
    // SDL3 to request both portrait and landscape families.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait PortraitUpsideDown LandscapeLeft LandscapeRight");
#endif
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
    // Synthesize mouse events from touch (and vice versa). On Switch this is
    // what makes the launcher's ImGui UI tappable: the raw finger events are
    // never translated to clicks by the ImGui SDL3 backend on their own, so
    // touches only ever moved the gamepad-nav highlight without activating.
#if defined(SDL_HINT_TOUCH_MOUSE_EVENTS)
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
#endif
#if defined(SDL_HINT_MOUSE_TOUCH_EVENTS)
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
#endif
#endif
#ifdef STAR_SYSTEM_ANDROID
    SDL_SetHint(SDL_HINT_ANDROID_ALLOW_RECREATE_ACTIVITY, "0");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    SDL_SetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS, "0");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR))
      throw ApplicationException::format("Could not initialize SDL: {}", SDL_GetError());
    openExistingGamepads();

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
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
    // Real mobile devices: an OpenGL ES 3.0 context (resolved via EGL).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // Desktop launcher build: a Core 3.0 context, matching the standard
    // desktop SDL entrypoint, so GLEW resolves entry points and the desktop
    // ImGui OpenGL3 backend (built for desktop GL) works. The shared
    // OpenGlRenderer targets the GL3/GLES3 common subset, so it runs on both.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

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
    // Attach native iOS screen-edge pan recognizers for launcher back/forward
    // navigation, so edge swipes are driven by real UIKit gestures.
    StarIosBridge_installLauncherEdgeSwipes();
    // Explicitly request fullscreen so the SDL window covers the entire screen
    // on all iOS versions. On iOS 26+ the implicit "0x0 = fullscreen" behaviour
    // is not guaranteed, and without this call the window may be inset,
    // producing black bars around the content.
    if (!SDL_SetWindowFullscreen(m_window, true))
      androidLogInfo("setupWindowAndRenderer: SDL_SetWindowFullscreen failed: %s", SDL_GetError());
#endif

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext)
      throw ApplicationException::format("Could not create GLES context: {}", SDL_GetError());

#ifdef STAR_SYSTEM_SWITCH
    // Present-paced to the panel: with the frame target now 60fps (a clean
    // divisor of the 60Hz panel and of Ryujinx's 120Hz custom vsync), vsync
    // pacing gives perfectly even displayed cadence. The old swap-interval-0
    // workaround targeted the non-divisor 30/45fps eras, where a stall
    // phase-locked presents into the next-next vsync slot -- with two
    // free-running 60Hz clocks (sleepPrecise vs panel) it instead caused a
    // continuous slow drift of dropped/doubled frames, i.e. visible motion
    // judder on hardware. Stall recovery is covered by the tick-backlog
    // forgiveness and the GPU fence ring.
    SDL_GL_SetSwapInterval(1);
#endif

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
    // syncWindowMetrics already set the viewport and logical render size when
    // metrics changed; repeat it here to cover first startup with stable sizes.
    m_renderer->setScreenOffset(Vec2U(m_safeArea.left, m_safeArea.bottom));
    m_renderer->setScreenViewportSize(gameCanvasSize());
    m_renderer->setScreenSize(m_renderCanvasSize);
  }

  void setupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTrickleEventQueue = false;

    loadLauncherFont(io);
    ImGui::StyleColorsDark();
    applyLauncherUiStyle();
    refreshImGuiScale();
    ImGui_ImplSDL3_InitForOpenGL(m_window, m_glContext);
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS) || defined(STAR_SYSTEM_SWITCH)
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    // Desktop Core 3.0 context: GLSL 1.30 is the matching shader version for
    // the desktop ImGui OpenGL3 backend.
    ImGui_ImplOpenGL3_Init("#version 130");
#endif
    if ((uintptr_t)io.Fonts->TexID != 0) {
      glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)io.Fonts->TexID);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
  }

  static ImVec4 launcherColor(std::array<float, 3> const& color, float alpha) {
    return ImVec4(color[0], color[1], color[2], alpha);
  }

  static ImVec4 launcherColorScaled(std::array<float, 3> const& color, float multiplier, float alpha) {
    return ImVec4(
        std::clamp(color[0] * multiplier, 0.0f, 1.0f),
        std::clamp(color[1] * multiplier, 0.0f, 1.0f),
        std::clamp(color[2] * multiplier, 0.0f, 1.0f),
        alpha);
  }

  float launcherUiScale() const {
    return std::clamp(m_launcherUiConfig.scale, 0.75f, 1.75f);
  }

  void applyLauncherUiStyle() {
    if (!ImGui::GetCurrentContext())
      return;

    ImGui::StyleColorsDark();

    float scale = launcherUiScale();
    auto& style = ImGui::GetStyle();
    style.TouchExtraPadding = ImVec2(12.0f * scale, 12.0f * scale);
    style.WindowPadding = ImVec2(18.0f * scale, 16.0f * scale);
    style.FramePadding = ImVec2(15.0f * scale, 10.0f * scale);
    style.ItemSpacing = ImVec2(14.0f * scale, 12.0f * scale);
    style.ScrollbarSize = 28.0f * scale;

    auto& colors = style.Colors;
    auto const& accent = m_launcherUiConfig.accentColor;
    colors[ImGuiCol_Button] = launcherColor(accent, 0.62f);
    colors[ImGuiCol_ButtonHovered] = launcherColorScaled(accent, 1.15f, 0.86f);
    colors[ImGuiCol_ButtonActive] = launcherColorScaled(accent, 0.82f, 0.96f);
    colors[ImGuiCol_FrameBg] = launcherColor(accent, 0.36f);
    colors[ImGuiCol_FrameBgHovered] = launcherColorScaled(accent, 1.12f, 0.62f);
    colors[ImGuiCol_FrameBgActive] = launcherColorScaled(accent, 0.90f, 0.78f);
    colors[ImGuiCol_Header] = launcherColor(accent, 0.45f);
    colors[ImGuiCol_HeaderHovered] = launcherColor(accent, 0.68f);
    colors[ImGuiCol_HeaderActive] = launcherColor(accent, 0.86f);
    colors[ImGuiCol_CheckMark] = launcherColorScaled(accent, 1.20f, 1.0f);
    colors[ImGuiCol_SliderGrab] = launcherColorScaled(accent, 1.10f, 0.82f);
    colors[ImGuiCol_SliderGrabActive] = launcherColorScaled(accent, 1.25f, 1.0f);
    colors[ImGuiCol_SeparatorActive] = launcherColor(accent, 0.85f);
    colors[ImGuiCol_SeparatorHovered] = launcherColor(accent, 0.65f);
  }

  void applyLauncherUiConfig(LauncherUiConfig const& config) {
    m_launcherUiConfig = config;
    applyLauncherUiStyle();
    refreshImGuiScale();
  }

  void shutdownImGui() {
    clearLauncherTextures();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }

  void teardownSdl() {
    m_sdlGamepads.clear();
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

  void syncLauncherBundledAssets() {
#if STAR_SYSTEM_ANDROID
    String bundledStorageRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    if (auto synced = AndroidFileAccessBridge::syncBundledAssets(bundledStorageRoot)) {
      auto syncedRoot = *synced;
      auto bundledLangDirectory = File::relativeTo(syncedRoot, LauncherLangDirectory);
      auto bundledFontPath = File::relativeTo(syncedRoot, PreferredLauncherFontPath);
      if (File::isDirectory(bundledLangDirectory))
        m_launcherLangDirectory = bundledLangDirectory;
      if (File::isFile(bundledFontPath))
        m_launcherFontPath = bundledFontPath;
    } else {

    }
#elif STAR_SYSTEM_IOS
    String bundledStorageRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    if (auto synced = IosFileAccessBridge::syncBundledAssets(bundledStorageRoot)) {
      auto syncedRoot = *synced;
      auto bundledLangDirectory = File::relativeTo(syncedRoot, LauncherLangDirectory);
      auto bundledFontPath = File::relativeTo(syncedRoot, PreferredLauncherFontPath);
      if (File::isDirectory(bundledLangDirectory))
        m_launcherLangDirectory = bundledLangDirectory;
      if (File::isFile(bundledFontPath))
        m_launcherFontPath = bundledFontPath;
    }
#elif STAR_SYSTEM_SWITCH
    // Stage romfs:/bundled_assets into the SD-card storage root, mirroring the
    // app-package sync the Android/iOS hosts perform.
    switchSyncBundledAssets(m_storageRoot);
    String syncedRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    auto bundledLangDirectory = File::relativeTo(syncedRoot, LauncherLangDirectory);
    auto bundledFontPath = File::relativeTo(syncedRoot, PreferredLauncherFontPath);
    if (File::isDirectory(bundledLangDirectory))
      m_launcherLangDirectory = bundledLangDirectory;
    if (File::isFile(bundledFontPath))
      m_launcherFontPath = bundledFontPath;
#else
    // Desktop launcher build: there is no app package to sync from. Try to
    // locate the launcher font next to the game assets (../assets relative to
    // the executable, matching prepareBootConfig's desktop asset root). If it
    // isn't found the launcher falls back to ImGui's built-in font and the
    // English fallback strings baked into every launcherText() call, so the
    // UI still renders and functions.
    if (auto basePath = SDL_GetBasePath()) {
      String assetsRoot = File::convertDirSeparators(File::relativeTo(basePath, "../assets"));
      auto candidateFont = File::relativeTo(assetsRoot, BundledLauncherFontPath);
      if (File::isFile(candidateFont))
        m_launcherFontPath = candidateFont;
      auto candidateLang = File::relativeTo(assetsRoot, strf("opensb/{}", LauncherLangDirectory));
      if (File::isDirectory(candidateLang))
        m_launcherLangDirectory = candidateLang;
    }
#endif
  }

  void reloadLauncherLocalization(String const& preferredLocale = {}) {
    m_launcherLocale = resolveLauncherLocaleChoice(preferredLocale);
    if (!File::isDirectory(m_launcherLangDirectory))
      m_launcherLangDirectory = File::relativeTo(m_storageRoot, strf("bundled_assets/{}", LauncherLangDirectory));

    auto defaultPath = File::relativeTo(m_launcherLangDirectory, strf("{}.lang", DefaultLauncherLocale));
    auto localePath = File::relativeTo(m_launcherLangDirectory, strf("{}.lang", m_launcherLocale));

    m_launcherTranslations = loadLauncherLangFile(defaultPath);
    if (m_launcherLocale != DefaultLauncherLocale) {
      for (auto const& pair : loadLauncherLangFile(localePath).pairs())
        m_launcherTranslations[pair.first] = pair.second;
    }
  }

  String launcherText(String const& key, String const& fallback = {}) const {
    if (auto value = m_launcherTranslations.ptr(key))
      return *value;
    return fallback;
  }

  String launcherBundledAssetsRoot() const {
    if (!m_launcherLangDirectory.empty())
      return File::dirName(m_launcherLangDirectory);
    return File::relativeTo(m_storageRoot, "bundled_assets");
  }

  String launcherBundledAssetPath(String const& relativePath) const {
    return File::relativeTo(launcherBundledAssetsRoot(), relativePath);
  }

  ImTextureID imguiTextureId(LauncherTexture const& texture) const {
    return (ImTextureID)(intptr_t)texture.textureId;
  }

  LauncherTexture const* launcherTexture(String const& relativePath) {
    if (auto texture = m_launcherTextureCache.ptr(relativePath))
      return texture;

    auto path = launcherBundledAssetPath(relativePath);
    if (!File::isFile(path))
      return nullptr;

    try {
      Image image = Image::readPng(File::open(path, IOMode::Read));
      if (image.pixelFormat() != PixelFormat::RGBA32)
        image = image.convert(PixelFormat::RGBA32);

      GLuint textureId = 0;
      glGenTextures(1, &textureId);
      if (textureId == 0)
        return nullptr;

      glBindTexture(GL_TEXTURE_2D, textureId);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());

      LauncherTexture texture;
      texture.textureId = textureId;
      texture.size = image.size();
      return &m_launcherTextureCache.set(relativePath, std::move(texture));
    } catch (std::exception const& e) {
      Logger::warn("Could not load launcher controller texture '{}': {}", path, outputException(e, false));
      return nullptr;
    }
  }

  void renderLauncherTexture(LauncherTexture const& texture, ImVec2 size) const {
    ImGui::Image(imguiTextureId(texture), size, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
  }

  void clearLauncherTextures() {
    for (auto const& pair : m_launcherTextureCache) {
      GLuint textureId = pair.second.textureId;
      if (textureId != 0)
        glDeleteTextures(1, &textureId);
    }
    m_launcherTextureCache.clear();
  }

  void loadLauncherFont(ImGuiIO& io) {
    io.Fonts->Clear();

    String fontPath;
    if (!m_launcherFontPath.empty() && File::isFile(m_launcherFontPath)) {
      fontPath = m_launcherFontPath;
    } else {
      fontPath = File::relativeTo(m_storageRoot, strf("bundled_assets/{}", PreferredLauncherFontPath));
      if (!File::isFile(fontPath)) {
        fontPath = File::relativeTo(m_storageRoot, strf("bundled_assets/{}", BundledLauncherFontPath));
        if (!File::isFile(fontPath))
          fontPath.clear();
      }
    }

    m_launcherFontData.clear();
    if (!fontPath.empty()) {
      m_launcherFontData = File::readFile(fontPath);
      ImFontConfig config{};
      config.FontDataOwnedByAtlas = false;
      io.Fonts->AddFontFromMemoryTTF(m_launcherFontData.ptr(), (int)m_launcherFontData.size(), 18.0f, &config);
    }

    if (io.Fonts->Fonts.empty())
      io.Fonts->AddFontDefault();

    m_launcherFallbackFontData.clear();
    for (auto const& candidate : launcherCjkFontCandidates()) {
      if (!File::isFile(candidate))
        continue;

      m_launcherFallbackFontData.push_back(File::readFile(candidate));
      auto& fontData = m_launcherFallbackFontData.back();
      ImFontConfig config{};
      config.FontDataOwnedByAtlas = false;
      config.MergeMode = true;
      if (io.Fonts->AddFontFromMemoryTTF(fontData.ptr(), (int)fontData.size(), 18.0f, &config, io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
        androidLogInfo("Loaded launcher CJK fallback font: %s", candidate.utf8Ptr());
        break;
      }
      m_launcherFallbackFontData.pop_back();
    }
  }

  void reloadLauncherFontAtlas() {
    if (!ImGui::GetCurrentContext())
      return;

    auto& io = ImGui::GetIO();
    loadLauncherFont(io);
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
    if ((uintptr_t)io.Fonts->TexID != 0) {
      glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)io.Fonts->TexID);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
  }

  void loadLauncherState(LauncherState& state) {
    auto configService = m_platformServices->launchConfigService();
    Json config = configService ? configService->loadLauncherConfig() : JsonObject();

    state.packedPakPath = config.optString("packedPakPath").value(""
    );

    state.uiConfig.scale = std::clamp(config.queryFloat("launcherUi.scale", 1.0f), 0.75f, 1.75f);
    if (auto accentColor = config.optQueryArray("launcherUi.accentColor")) {
      if (accentColor->size() >= 3) {
        state.uiConfig.accentColor = {
          std::clamp(accentColor->get(0).toFloat(), 0.0f, 1.0f),
          std::clamp(accentColor->get(1).toFloat(), 0.0f, 1.0f),
          std::clamp(accentColor->get(2).toFloat(), 0.0f, 1.0f)
        };
      }
    }
    m_perfSimRate = config.queryInt("performance.simRate", 60) == 30 ? 30 : 60;
    m_perfFpsCap = std::max(0, (int)config.queryInt("performance.fpsCap", 0));
    m_fullscreenRender = config.queryBool("display.fullscreenRender", false);
#ifdef STAR_SYSTEM_SWITCH
    // The Switch panel is fixed 60Hz: rendering past it can't be displayed
    // and only beats against the panel (a ~5Hz judder at the 62-69fps this
    // hardware reaches uncapped) while costing battery. Map "unlimited"
    // (including configs saved by earlier builds whose default was 0) to a
    // 60fps cap; explicit lower caps are honored as-is.
    if (m_perfFpsCap == 0)
      m_perfFpsCap = 60;
#endif

    String savedLocale = config.queryString("launcherUi.locale", SystemLocaleMarker);
    state.locale = savedLocale.empty() || savedLocale == SystemLocaleMarker ? String(SystemLocaleMarker) : normalizeLauncherLocale(savedLocale);
    state.systemLocale = loadPreferredLauncherLocale();
    reloadLauncherLocalization(state.locale);
    reloadLauncherFontAtlas();
    m_launcherUiConfig = state.uiConfig;
    applyLauncherUiStyle();
    refreshImGuiScale();

    if (!samePath(m_legacyStorageRoot, m_storageRoot)) {
      auto legacyRoot = File::convertDirSeparators(m_legacyStorageRoot).trimEnd("/");
      auto currentRoot = File::convertDirSeparators(m_storageRoot).trimEnd("/");
      auto packedPakPath = File::convertDirSeparators(state.packedPakPath);
      if (packedPakPath.beginsWith(legacyRoot + "/")) {
        auto migratedPath = currentRoot + packedPakPath.substr(legacyRoot.size());
        if (File::isFile(migratedPath)) {
          state.packedPakPath = migratedPath;
          androidLogInfo("Rebased packed.pak path to migrated storage root: %s", migratedPath.utf8Ptr());
        }
      }
    }

#ifdef STAR_SYSTEM_SWITCH
    // A packed.pak may be embedded directly into the NRO's romfs at build time
    // (STAR_SWITCH_PACKED_PAK), producing a fully self-contained build. Prefer
    // it whenever there is no valid persisted pak so the launcher needs no
    // in-app file selection (the Switch has no native file picker).
    //
    // Release NROs are NOT built with an embedded pak, so the more common
    // path is the SD-card fallback below: the same "<storage root>/assets/
    // packed.pak" location Android/iOS's file-picker import copies into (see
    // MobileExternalFileAccessService::pickPackedPak), so a user just has to
    // drop the file at the matching SD path themselves since there's no
    // picker to do it for them. state.packedPakPath ends up unset if neither
    // is found; the "please place packed.pak" hint below covers that case.
    if (state.packedPakPath.empty() || !File::isFile(state.packedPakPath)) {
      if (File::isFile("romfs:/packed.pak"))
        state.packedPakPath = "romfs:/packed.pak";
      else {
        auto sdPakPath = File::relativeTo(m_storageRoot, "assets/packed.pak");
        if (File::isFile(sdPakPath))
          state.packedPakPath = sdPakPath;
      }
    }
#elif !defined(STAR_SYSTEM_ANDROID) && !defined(STAR_SYSTEM_IOS)
    // Desktop launcher build: auto-detect a packed.pak the user dropped at
    // "<storage root>/assets/packed.pak" (the same location the file picker
    // imports into), so it need not be re-picked every launch. The picker
    // remains available for choosing one elsewhere.
    if (state.packedPakPath.empty() || !File::isFile(state.packedPakPath)) {
      auto localPakPath = File::relativeTo(m_storageRoot, "assets/packed.pak");
      if (File::isFile(localPakPath))
        state.packedPakPath = localPakPath;
    }
#endif

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    // Phones/tablets: touch overlay on and direct gestures on by default -- the
    // touchscreen is the primary input.
    state.touchConfig.enabled = config.queryBool("touch.enabled", true);
    state.touchConfig.directTouchGestures = config.queryBool("touch.directTouchGestures", true);
#elif defined(STAR_SYSTEM_SWITCH)
    // Default the on-screen touch control OVERLAY off on Switch: the d-pad/button
    // elements are unwanted on a console, but disabling the overlay still leaves
    // touch passthrough intact (finger taps fall through as mouse clicks, so the
    // user can tap buttons directly). Matches the in-game adapter default below;
    // both sites must agree because this value is what gets persisted into the
    // launcher config that the adapter then reads back. Still user-togglable.
    state.touchConfig.enabled = config.queryBool("touch.enabled", false);
    state.touchConfig.directTouchGestures = config.queryBool("touch.directTouchGestures", true);
#else
    // Desktop PC launcher build: no touchscreen, so both the on-screen overlay
    // and direct touch gestures default OFF (the user drives with mouse +
    // keyboard + gamepad). Still user-togglable for touchscreen monitors.
    state.touchConfig.enabled = config.queryBool("touch.enabled", false);
    state.touchConfig.directTouchGestures = config.queryBool("touch.directTouchGestures", false);
#endif
    state.touchConfig.directTouchGestureMode = directTouchGestureModeFromName(config.queryString("touch.directTouchGestureMode", "touchscreen"));
    state.touchConfig.directTouchSingleAction = touchActionFromJson(
        config.query("touch.directTouchSingleAction", jsonFromTouchAction(state.touchConfig.directTouchSingleAction)), state.touchConfig.directTouchSingleAction);
    state.touchConfig.directTouchTwoFingerAction = touchActionFromJson(
        config.query("touch.directTouchTwoFingerAction", jsonFromTouchAction(state.touchConfig.directTouchTwoFingerAction)), state.touchConfig.directTouchTwoFingerAction);
    state.touchConfig.joystickAimCursorEnabled = config.queryBool("touch.joystickAimCursorEnabled", true);
    state.touchConfig.gyroEnabled = config.queryBool("touch.gyroEnabled", false);
    if (!platformGyroAvailable())
      state.touchConfig.gyroEnabled = false;
    state.touchConfig.opacity = config.queryFloat("touch.opacity", 0.35f);
    state.touchConfig.size = config.queryFloat("touch.size", 1.0f);
    state.touchConfig.outlineThickness = config.queryFloat("touch.outlineThickness", 1.5f);
    state.touchConfig.deadzone = config.queryFloat("touch.deadzone", 0.15f);
    state.touchConfig.gyroSensitivity = config.queryFloat("touch.gyroSensitivity", 1.0f);
    state.touchConfig.gyroInvertX = config.queryBool("touch.gyroInvertX", false);
    state.touchConfig.gyroInvertY = config.queryBool("touch.gyroInvertY", false);
    state.touchElements = touchElementsFromConfig(config);
    state.gamepadConfig = gamepadConfigFromConfig(config);
    // touchConfig is the single source of truth for this setting -- see
    // MobileTouchConfig::joystickAimCursorEnabled.
    state.gamepadConfig.joystickAimCursorEnabled = state.touchConfig.joystickAimCursorEnabled;
    state.gamepadBindings = gamepadBindingsFromConfig(config);

    state.canLaunch = File::isFile(state.packedPakPath);
    state.lastStatus = state.canLaunch ? launcherText("status.packedPakReady", "Using existing packed.pak") : launcherText("status.packedPakMissing", "Please import packed.pak");
  }

  std::vector<pair<String, MobileTouchAction>> touchActionChoices() const {
    return {
      {launcherText("touchActionChoice.leftHandMouseLeft", "Left Hand / Mouse Left"), mouseAction(MouseButton::Left)},
      {launcherText("touchActionChoice.rightHandMouseRight", "Right Hand / Mouse Right"), mouseAction(MouseButton::Right)},
      {launcherText("touchActionChoice.jump", "Jump / J (Space)"), keyAction(Key::Space)},
      {launcherText("touchActionChoice.interact", "Interact / E"), keyAction(Key::E)},
      {launcherText("touchActionChoice.escapePause", "Escape / Pause"), keyAction(Key::Escape)},
      {launcherText("touchActionChoice.chat", "Chat / T (Enter)"), keyAction(Key::Return)},
      {launcherText("touchActionChoice.tech", "Tech / F"), keyAction(Key::F)},
      {launcherText("touchActionChoice.swapHotbar", "Swap Hotbar / X"), keyAction(Key::X)},
      {launcherText("touchActionChoice.dropItem", "Drop Item / Q"), keyAction(Key::Q)},
      {launcherText("touchActionChoice.shift", "Shift"), keyAction(Key::LShift)},
      {launcherText("touchActionChoice.ctrl", "Ctrl"), keyAction(Key::LCtrl)},
      {launcherText("touchActionChoice.moveUp", "Move Up / W"), keyAction(Key::W)},
      {launcherText("touchActionChoice.moveDown", "Move Down / S"), keyAction(Key::S)},
      {launcherText("touchActionChoice.moveLeft", "Move Left / A"), keyAction(Key::A)},
      {launcherText("touchActionChoice.moveRight", "Move Right / D"), keyAction(Key::D)},
      {launcherText("touchActionChoice.inventory", "Inventory / I"), keyAction(Key::I)},
      {launcherText("touchActionChoice.codex", "Codex / L"), keyAction(Key::L)},
      {launcherText("touchActionChoice.quest", "Quest / J"), keyAction(Key::J)},
      {launcherText("touchActionChoice.crafting", "Crafting / C"), keyAction(Key::C)},
      {launcherText("touchActionChoice.actionBar1", "Action Bar 1"), keyAction(Key::One)},
      {launcherText("touchActionChoice.actionBar2", "Action Bar 2"), keyAction(Key::Two)},
      {launcherText("touchActionChoice.actionBar3", "Action Bar 3"), keyAction(Key::Three)},
      {launcherText("touchActionChoice.actionBar4", "Action Bar 4"), keyAction(Key::Four)},
      {launcherText("touchActionChoice.actionBar5", "Action Bar 5"), keyAction(Key::Five)},
      {launcherText("touchActionChoice.scrollUp", "Scroll Up"), wheelAction(true)},
      {launcherText("touchActionChoice.scrollDown", "Scroll Down"), wheelAction(false)},
      {launcherText("touchActionChoice.gyroToggle", "Gyro Toggle"), gyroToggleAction()},
      {launcherText("touchActionChoice.noAction", "No Action"), noneAction()}
    };
  }

  std::vector<pair<String, MobileTouchAction>> gamepadActionChoices() const {
    auto choices = touchActionChoices();
    choices.insert(choices.end() - 1, {launcherText("gamepadActionChoice.aimModeToggle", "Toggle Aim Mode"), gamepadAimModeToggleAction()});
    choices.insert(choices.end() - 1, {launcherText("gamepadActionChoice.actionWheel", "Action Wheel"), actionWheelAction()});
    choices.insert(choices.end() - 1, {launcherText("gamepadActionChoice.inventoryWheel", "Inventory Wheel"), inventoryWheelAction()});
    choices.insert(choices.end() - 1, {launcherText("gamepadActionChoice.selectionUp", "Selection Up"), uiNavigationAction(UiNavigationDirection::Up)});
    choices.insert(choices.end() - 1, {launcherText("gamepadActionChoice.selectionDown", "Selection Down"), uiNavigationAction(UiNavigationDirection::Down)});
    choices.insert(choices.end() - 1, {launcherText("gamepadActionChoice.selectionLeft", "Selection Left"), uiNavigationAction(UiNavigationDirection::Left)});
    choices.insert(choices.end() - 1, {launcherText("gamepadActionChoice.selectionRight", "Selection Right"), uiNavigationAction(UiNavigationDirection::Right)});
    return choices;
  }

  bool touchActionEqual(MobileTouchAction const& a, MobileTouchAction const& b) const {
    if (a.kind != b.kind)
      return false;
    if (a.kind == MobileTouchActionKind::Key)
      return a.key == b.key;
    if (a.kind == MobileTouchActionKind::KeyMacro)
      return a.keys == b.keys;
    if (a.kind == MobileTouchActionKind::MouseButton)
      return a.mouseButton == b.mouseButton;
    if (a.kind == MobileTouchActionKind::UiNavigation)
      return a.uiNavigationDirection == b.uiNavigationDirection;
    return true;
  }

  int actionIndex(MobileTouchAction const& action, std::vector<pair<String, MobileTouchAction>> const& choices) const {
    for (size_t i = 0; i < choices.size(); ++i) {
      auto const& candidate = choices[i].second;
      if (touchActionEqual(candidate, action))
        return (int)i;
    }
    return 0;
  }

  int touchActionIndex(MobileTouchAction const& action) const {
    return actionIndex(action, touchActionChoices());
  }


  float imguiButtonWidth(char const* label, ImVec2 size = {}) const {
    if (size.x > 0.0f)
      return size.x;

    auto const& style = ImGui::GetStyle();
    return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
  }

  void sameLineIfNextFits(float nextItemWidth) const {
    auto const& style = ImGui::GetStyle();
    float nextItemMaxX = ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextItemWidth;
    float contentMaxX = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    if (nextItemMaxX <= contentMaxX)
      ImGui::SameLine();
  }

  ImVec4 launcherSafeAreaInImguiUnits(ImVec2 displaySize) const {
    float scaleX = m_windowSize[0] > 0 ? displaySize.x / (float)m_windowSize[0] : 1.0f;
    float scaleY = m_windowSize[1] > 0 ? displaySize.y / (float)m_windowSize[1] : 1.0f;
    return ImVec4(
      (float)m_safeArea.left * scaleX,
      (float)m_safeArea.top * scaleY,
      (float)m_safeArea.right * scaleX,
      (float)m_safeArea.bottom * scaleY
    );
  }

  void setNextLauncherWindowRect(ImVec2 displaySize) const {
    float shortSide = std::min(displaySize.x, displaySize.y);
    float margin = std::max(10.0f, shortSide * 0.0125f);
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displaySize.x - margin * 2.0f, displaySize.y - margin * 2.0f), ImGuiCond_Always);
  }

  void beginLauncherContentArea(char const* id, ImVec2 displaySize, bool compact = false) const {
    float scale = launcherUiScale();
    float edgePadding = (compact ? 6.0f : 10.0f) * scale;
    ImVec4 safe = launcherSafeAreaInImguiUnits(displaySize);

    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 min = ImGui::GetCursorScreenPos();
    ImVec2 max(windowPos.x + ImGui::GetContentRegionMax().x, windowPos.y + ImGui::GetContentRegionMax().y);

    if (safe.x > 0.0f)
      min.x = std::max(min.x, safe.x + edgePadding);
    if (safe.y > 0.0f)
      min.y = std::max(min.y, safe.y + edgePadding);
    if (safe.z > 0.0f)
      max.x = std::min(max.x, displaySize.x - safe.z - edgePadding);
    if (safe.w > 0.0f)
      max.y = std::min(max.y, displaySize.y - safe.w - edgePadding);

    if (max.x <= min.x)
      max.x = min.x + std::max(80.0f, ImGui::GetContentRegionAvail().x);
    if (max.y <= min.y)
      max.y = min.y + std::max(80.0f, ImGui::GetContentRegionAvail().y);

    ImGui::SetCursorScreenPos(min);
    ImGui::BeginChild(id, ImVec2(max.x - min.x, max.y - min.y), false, ImGuiWindowFlags_NoScrollbar);
  }

  void renderLauncherBusyIndicator(LauncherState const& state) const {
    if (!state.asyncActionRunning)
      return;

    char const frames[] = "|/-\\";
    int frame = (int)std::floor(ImGui::GetTime() * 10.0) & 3;
    String label = state.asyncActionName.empty() ? launcherText("common.importing", "Importing") : state.asyncActionName;
    ImGui::Text("%c %s", frames[frame], label.utf8Ptr());
  }

  void renderLauncherStatusText(LauncherState const& state) const {
    if (state.lastStatus.empty() && state.lastError.empty())
      return;

    ImGui::Separator();
    if (!state.lastStatus.empty())
      ImGui::TextWrapped("%s: %s", launcherText("common.status", "Status").utf8Ptr(), state.lastStatus.utf8Ptr());
    if (!state.lastError.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
      ImGui::TextWrapped("%s", state.lastError.utf8Ptr());
      ImGui::PopStyleColor();
    }
  }

  void beginLauncherScrollArea(char const* id, LauncherState const& state, bool reserveFooter = true) const {
    float reservedFooterHeight = 0.0f;
    if (reserveFooter) {
      if (!state.lastStatus.empty())
        reservedFooterHeight += ImGui::GetTextLineHeightWithSpacing();
      if (!state.lastError.empty())
        reservedFooterHeight += ImGui::GetTextLineHeightWithSpacing();
      if (reservedFooterHeight > 0.0f)
        reservedFooterHeight += ImGui::GetStyle().ItemSpacing.y;
    }

    float height = std::max(80.0f, ImGui::GetContentRegionAvail().y - reservedFooterHeight);
    ImGui::BeginChild(id, ImVec2(0.0f, height), false, ImGuiWindowFlags_NoScrollbar);
  }

  // Human-readable name for a single key, reused for macro chips.
  String keyLabel(Key key) const {
    List<Key> single;
    single.append(key);
    return keysName(single);
  }

  // The on-screen keyboard used by the macro editor. Each entry is a short
  // display label paired with the Key it inserts. Rows mirror a physical
  // keyboard so keys are easy to find; the editor wraps rows that don't fit.
  static std::vector<std::vector<pair<char const*, Key>>> const& virtualKeyboardRows() {
    static std::vector<std::vector<pair<char const*, Key>>> const rows = {
      {{"Esc", Key::Escape}, {"Tab", Key::Tab}, {"`", Key::Backquote},
       {"F1", Key::F1}, {"F2", Key::F2}, {"F3", Key::F3}, {"F4", Key::F4},
       {"F5", Key::F5}, {"F6", Key::F6}, {"F7", Key::F7}, {"F8", Key::F8},
       {"F9", Key::F9}, {"F10", Key::F10}, {"F11", Key::F11}, {"F12", Key::F12}},
      {{"1", Key::One}, {"2", Key::Two}, {"3", Key::Three}, {"4", Key::Four}, {"5", Key::Five},
       {"6", Key::Six}, {"7", Key::Seven}, {"8", Key::Eight}, {"9", Key::Nine}, {"0", Key::Zero},
       {"-", Key::Minus}, {"=", Key::Equals}},
      {{"Q", Key::Q}, {"W", Key::W}, {"E", Key::E}, {"R", Key::R}, {"T", Key::T}, {"Y", Key::Y},
       {"U", Key::U}, {"I", Key::I}, {"O", Key::O}, {"P", Key::P}, {"[", Key::LeftBracket}, {"]", Key::RightBracket}},
      {{"A", Key::A}, {"S", Key::S}, {"D", Key::D}, {"F", Key::F}, {"G", Key::G}, {"H", Key::H},
       {"J", Key::J}, {"K", Key::K}, {"L", Key::L}, {";", Key::Semicolon}, {"'", Key::Quote}},
      {{"Z", Key::Z}, {"X", Key::X}, {"C", Key::C}, {"V", Key::V}, {"B", Key::B}, {"N", Key::N},
       {"M", Key::M}, {",", Key::Comma}, {".", Key::Period}, {"/", Key::Slash}},
      {{"Shift", Key::LShift}, {"Ctrl", Key::LCtrl}, {"Alt", Key::LAlt}, {"Space", Key::Space},
       {"Enter", Key::Return}, {"Bksp", Key::Backspace}, {"Del", Key::Delete}},
      {{"Up", Key::Up}, {"Down", Key::Down}, {"Left", Key::Left}, {"Right", Key::Right}}
    };
    return rows;
  }

  void openMacroEditor(LauncherState& state, MobileTouchAction const& action, String const& bufferId) {
    state.macroEditorBufferId = bufferId;
    state.macroEditorOpen = true;
    state.macroEditorKeys.clear();
    for (auto key : keysFromTouchAction(action))
      state.macroEditorKeys.push_back(key);
    state.macroEditorSequential = action.kind == MobileTouchActionKind::KeyMacro ? action.macroSequential : true;
    ImGui::OpenPopup(launcherText("macroEditor.title", "Macro Editor").utf8Ptr());
  }

  // Full-featured on-screen macro editor. Replaces the old free-text key entry
  // (which the mobile soft keyboard could not reliably feed) with a virtual
  // keyboard: tap keys to append, reorder or remove them, and choose whether the
  // keys play as an ordered sequence or a held chord.
  void renderMacroEditorModal(LauncherState& state, MobileTouchAction& action, String const& bufferId) {
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(display.x * 0.94f, 760.0f), std::min(display.y * 0.90f, 640.0f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(launcherText("macroEditor.title", "Macro Editor").utf8Ptr(), nullptr, ImGuiWindowFlags_NoCollapse))
      return;

    // Guard against a stale target: only the editor that opened the popup edits.
    if (state.macroEditorBufferId != bufferId) {
      ImGui::EndPopup();
      return;
    }

    ImGui::TextWrapped("%s", launcherText("macroEditor.help", "Tap keys below to add them to the macro. Reorder or remove them in the list above.").utf8Ptr());

    if (ImGui::RadioButton(launcherText("macroEditor.sequence", "Play in order (sequence)").utf8Ptr(), state.macroEditorSequential))
      state.macroEditorSequential = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(launcherText("macroEditor.chord", "Hold together (chord)").utf8Ptr(), !state.macroEditorSequential))
      state.macroEditorSequential = false;

    ImGui::Separator();
    ImGui::Text("%s (%u)", launcherText("macroEditor.sequenceLabel", "Macro keys").utf8Ptr(), (unsigned)state.macroEditorKeys.size());

    float listHeight = ImGui::GetTextLineHeightWithSpacing() * 4.5f;
    ImGui::BeginChild("MacroSequenceList", ImVec2(0.0f, listHeight), true);
    if (state.macroEditorKeys.empty()) {
      ImGui::TextDisabled("%s", launcherText("macroEditor.empty", "No keys yet — tap the keyboard below.").utf8Ptr());
    } else {
      int removeIndex = -1, moveUpIndex = -1, moveDownIndex = -1;
      for (int i = 0; i < (int)state.macroEditorKeys.size(); ++i) {
        ImGui::PushID(i);
        ImGui::Text("%d.", i + 1);
        ImGui::SameLine();
        ImGui::TextUnformatted(keyLabel(state.macroEditorKeys[i]).utf8Ptr());
        float rowRight = ImGui::GetWindowContentRegionMax().x;
        float btnW = ImGui::GetFrameHeight();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), rowRight - btnW * 3.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::Button("^", ImVec2(btnW, 0.0f)))
          moveUpIndex = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == (int)state.macroEditorKeys.size() - 1);
        if (ImGui::Button("v", ImVec2(btnW, 0.0f)))
          moveDownIndex = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(btnW, 0.0f)))
          removeIndex = i;
        ImGui::PopID();
      }
      if (moveUpIndex > 0)
        std::swap(state.macroEditorKeys[moveUpIndex], state.macroEditorKeys[moveUpIndex - 1]);
      if (moveDownIndex >= 0 && moveDownIndex < (int)state.macroEditorKeys.size() - 1)
        std::swap(state.macroEditorKeys[moveDownIndex], state.macroEditorKeys[moveDownIndex + 1]);
      if (removeIndex >= 0)
        state.macroEditorKeys.erase(state.macroEditorKeys.begin() + removeIndex);
    }
    ImGui::EndChild();

    ImGui::Separator();
    float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("MacroKeyboard", ImVec2(0.0f, -footerHeight), true);
    float avail = ImGui::GetContentRegionAvail().x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float minKeyW = ImGui::GetFrameHeight() * 1.35f;
    for (auto const& row : virtualKeyboardRows()) {
      float lineX = 0.0f;
      bool firstInLine = true;
      for (auto const& entry : row) {
        float keyW = std::max(minKeyW, ImGui::CalcTextSize(entry.first).x + ImGui::GetStyle().FramePadding.x * 2.0f);
        if (!firstInLine && lineX + spacing + keyW > avail) {
          lineX = 0.0f;
          firstInLine = true;
        }
        if (!firstInLine)
          ImGui::SameLine();
        ImGui::PushID(&entry);
        if (ImGui::Button(entry.first, ImVec2(keyW, ImGui::GetFrameHeight() * 1.35f)))
          state.macroEditorKeys.push_back(entry.second);
        ImGui::PopID();
        lineX += (firstInLine ? 0.0f : spacing) + keyW;
        firstInLine = false;
      }
    }
    ImGui::EndChild();

    auto closeEditor = [&state]() {
      state.macroEditorOpen = false;
      state.macroEditorBufferId.clear();
      state.macroEditorKeys.clear();
      ImGui::CloseCurrentPopup();
    };

    if (ImGui::Button(launcherText("macroEditor.removeLast", "Remove Last").utf8Ptr()) && !state.macroEditorKeys.empty())
      state.macroEditorKeys.pop_back();
    ImGui::SameLine();
    if (ImGui::Button(launcherText("macroEditor.clear", "Clear").utf8Ptr()))
      state.macroEditorKeys.clear();
    ImGui::SameLine();
    if (ImGui::Button(launcherText("common.cancel", "Cancel").utf8Ptr()))
      closeEditor();
    ImGui::SameLine();
    if (ImGui::Button(launcherText("common.save", "Save").utf8Ptr())) {
      List<Key> keys;
      for (auto key : state.macroEditorKeys)
        keys.append(key);
      if (keys.size() == 1)
        action = keyAction(keys[0]);
      else if (!keys.empty())
        action = macroAction(keys, state.macroEditorSequential);
      else
        action = noneAction();
      closeEditor();
    }

    ImGui::EndPopup();
  }

  void renderActionCombo(LauncherState& state, char const* label, MobileTouchAction& action, String const& bufferId, std::vector<pair<String, MobileTouchAction>> const& choices) {
    ImGui::PushID(bufferId.utf8Ptr());

    // Preview reflects a custom macro so it isn't misrepresented as a preset.
    bool isMacro = action.kind == MobileTouchActionKind::KeyMacro && !action.keys.empty();
    int index = actionIndex(action, choices);
    String preview = isMacro
        ? strf("{} {}", action.macroSequential ? launcherText("macroEditor.previewSequence", "Macro:") : launcherText("macroEditor.previewChord", "Chord:"), keysName(action.keys))
        : choices[index].first;

    if (ImGui::BeginCombo(label, preview.utf8Ptr())) {
      for (size_t i = 0; i < choices.size(); ++i) {
        bool selected = !isMacro && index == (int)i;
        if (ImGui::Selectable(choices[i].first.utf8Ptr(), selected))
          action = choices[i].second;
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    // Virtual-keyboard macro editor: the touch-friendly replacement for typing
    // key names into a text box.
    String summary = isMacro ? keysName(action.keys) : launcherText("macroEditor.none", "none set");
    if (ImGui::Button(launcherText("macroEditor.editButton", "Edit macro keys...").utf8Ptr()))
      openMacroEditor(state, action, bufferId);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", summary.utf8Ptr());

    renderMacroEditorModal(state, action, bufferId);
    ImGui::PopID();
  }

  void renderTouchActionCombo(LauncherState& state, char const* label, MobileTouchAction& action, String const& bufferId) {
    renderActionCombo(state, label, action, bufferId, touchActionChoices());
  }

  void renderGamepadActionCombo(LauncherState& state, char const* label, MobileTouchAction& action, String const& bufferId) {
    renderActionCombo(state, label, action, bufferId, gamepadActionChoices());
  }

  void renderTouchPressModeCombo(MobileTouchElement& element) {
    std::vector<pair<char const*, MobileTouchPressMode>> choices{
      {launcherText("touchManager.pressMode.single", "Single press").utf8Ptr(), MobileTouchPressMode::SinglePress},
      {launcherText("touchManager.pressMode.repeat", "Rapid fire / repeat").utf8Ptr(), MobileTouchPressMode::Repeat},
      {launcherText("touchManager.pressMode.hold", "Hold").utf8Ptr(), MobileTouchPressMode::Hold},
      {launcherText("touchManager.pressMode.toggle", "Toggle").utf8Ptr(), MobileTouchPressMode::Toggle}
    };
    int index = 0;
    for (int i = 0; i < (int)choices.size(); ++i) {
      if (choices[i].second == element.pressMode) {
        index = i;
        break;
      }
    }
    if (ImGui::BeginCombo(launcherText("touchManager.buttonBehavior", "Button behavior").utf8Ptr(), choices[index].first)) {
      for (int i = 0; i < (int)choices.size(); ++i) {
        bool selected = index == i;
        if (ImGui::Selectable(choices[i].first, selected))
          element.pressMode = choices[i].second;
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  void renderGamepadPressModeCombo(MobileGamepadBinding& binding) {
    MobileTouchElement temp;
    temp.pressMode = binding.pressMode;
    renderTouchPressModeCombo(temp);
    binding.pressMode = temp.pressMode;
  }

  bool activeGamepadIsPlaystation() const {
    return m_activeGamepadType == SDL_GAMEPAD_TYPE_PS3
        || m_activeGamepadType == SDL_GAMEPAD_TYPE_PS4
        || m_activeGamepadType == SDL_GAMEPAD_TYPE_PS5;
  }

  bool activeGamepadIsNintendo() const {
    return m_activeGamepadType == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO
        || m_activeGamepadType == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT
        || m_activeGamepadType == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT
        || m_activeGamepadType == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
  }

  bool activeGamepadIsSteamDeck() const {
    return m_activeGamepadName.contains("steam deck", String::CaseInsensitive)
        || m_activeGamepadName.contains("steamdeck", String::CaseInsensitive);
  }

  GamepadGlyphFamily activeGamepadGlyphFamily() const {
    if (activeGamepadIsPlaystation())
      return GamepadGlyphFamily::Playstation;
    if (activeGamepadIsNintendo())
      return GamepadGlyphFamily::Switch;
    if (activeGamepadIsSteamDeck())
      return GamepadGlyphFamily::SteamDeck;
    return GamepadGlyphFamily::Xbox;
  }

  String activeGamepadLayoutName() const {
    if (m_activeGamepadType != SDL_GAMEPAD_TYPE_UNKNOWN) {
      char const* type = SDL_GetGamepadStringForType(m_activeGamepadType);
      if (type && type[0])
        return String(type);
    }
    return m_activeGamepadName.empty() ? launcherText("gamepadManager.layoutGeneric", "Generic gamepad") : m_activeGamepadName;
  }

  String gamepadBindingDisplayLabel(MobileGamepadBinding const& binding) const {
    if (activeGamepadIsPlaystation()) {
      if (binding.button == ControllerButton::A)
        return "Cross";
      if (binding.button == ControllerButton::B)
        return "Circle";
      if (binding.button == ControllerButton::X)
        return "Square";
      if (binding.button == ControllerButton::Y)
        return "Triangle";
      if (binding.button == ControllerButton::LeftShoulder)
        return "L1";
      if (binding.button == ControllerButton::RightShoulder)
        return "R1";
      if (binding.button == ControllerButton::TriggerLeft)
        return "L2";
      if (binding.button == ControllerButton::TriggerRight)
        return "R2";
      if (binding.button == ControllerButton::Back)
        return "Share";
      if (binding.button == ControllerButton::Start)
        return "Options";
      if (binding.button == ControllerButton::Touchpad)
        return "Touchpad";
    } else if (activeGamepadIsNintendo()) {
      if (binding.button == ControllerButton::A)
        return "B";
      if (binding.button == ControllerButton::B)
        return "A";
      if (binding.button == ControllerButton::X)
        return "Y";
      if (binding.button == ControllerButton::Y)
        return "X";
      if (binding.button == ControllerButton::Back)
        return "-";
      if (binding.button == ControllerButton::Start)
        return "+";
      if (binding.button == ControllerButton::Misc1)
        return "Capture";
    }

    return binding.label.empty() ? ControllerButtonNames.getRight(binding.button) : binding.label;
  }

  String gamepadGlyphFolder(GamepadGlyphFamily family) const {
    switch (family) {
      case GamepadGlyphFamily::Playstation:
        return "ps5";
      case GamepadGlyphFamily::Switch:
        return "switch";
      case GamepadGlyphFamily::SteamDeck:
        return "steamdeck";
      case GamepadGlyphFamily::Xbox:
      default:
        return "xbox";
    }
  }

  String controllerDiagramAssetPath(GamepadGlyphFamily family) const {
    switch (family) {
      case GamepadGlyphFamily::Xbox:
        return "opensb/interface/mobile/controller/xbox/diagram.png";
      case GamepadGlyphFamily::Playstation:
        return "opensb/interface/mobile/controller/ps5/diagram.png";
      case GamepadGlyphFamily::Switch:
        return "opensb/interface/mobile/controller/switch/Controllers.png";
      case GamepadGlyphFamily::SteamDeck:
      default:
        return "";
    }
  }

  String gamepadGlyphName(ControllerButton button, GamepadGlyphFamily family) const {
    if (family == GamepadGlyphFamily::Playstation) {
      switch (button) {
        case ControllerButton::A: return "Cross";
        case ControllerButton::B: return "Circle";
        case ControllerButton::X: return "Square";
        case ControllerButton::Y: return "Triangle";
        case ControllerButton::LeftShoulder: return "L1";
        case ControllerButton::RightShoulder: return "R1";
        case ControllerButton::TriggerLeft: return "L2";
        case ControllerButton::TriggerRight: return "R2";
        case ControllerButton::Back: return "Share";
        case ControllerButton::Start: return "Options";
        case ControllerButton::LeftStick: return "Left_Stick_Click";
        case ControllerButton::RightStick: return "Right_Stick_Click";
        case ControllerButton::DPadUp: return "Dpad_Up";
        case ControllerButton::DPadDown: return "Dpad_Down";
        case ControllerButton::DPadLeft: return "Dpad_Left";
        case ControllerButton::DPadRight: return "Dpad_Right";
        case ControllerButton::Touchpad: return "Touch_Pad";
        case ControllerButton::Misc1: return "Microphone";
        default: return "";
      }
    }

    if (family == GamepadGlyphFamily::Switch) {
      switch (button) {
        case ControllerButton::A: return "B";
        case ControllerButton::B: return "A";
        case ControllerButton::X: return "Y";
        case ControllerButton::Y: return "X";
        case ControllerButton::LeftShoulder: return "LB";
        case ControllerButton::RightShoulder: return "RB";
        case ControllerButton::TriggerLeft: return "LT";
        case ControllerButton::TriggerRight: return "RT";
        case ControllerButton::Back: return "Minus";
        case ControllerButton::Start: return "Plus";
        case ControllerButton::Guide: return "Home";
        case ControllerButton::Misc1: return "Square";
        case ControllerButton::LeftStick: return "Left_Stick_Click";
        case ControllerButton::RightStick: return "Right_Stick_Click";
        case ControllerButton::DPadUp: return "Dpad_Up";
        case ControllerButton::DPadDown: return "Dpad_Down";
        case ControllerButton::DPadLeft: return "Dpad_Left";
        case ControllerButton::DPadRight: return "Dpad_Right";
        default: return "";
      }
    }

    if (family == GamepadGlyphFamily::SteamDeck) {
      switch (button) {
        case ControllerButton::A: return "A";
        case ControllerButton::B: return "B";
        case ControllerButton::X: return "X";
        case ControllerButton::Y: return "Y";
        case ControllerButton::LeftShoulder: return "L1";
        case ControllerButton::RightShoulder: return "R1";
        case ControllerButton::TriggerLeft: return "L2";
        case ControllerButton::TriggerRight: return "R2";
        case ControllerButton::Back: return "Dots";
        case ControllerButton::Guide: return "Steam";
        case ControllerButton::Start: return "Menu";
        case ControllerButton::Paddle1: return "R4";
        case ControllerButton::Paddle2: return "L4";
        case ControllerButton::Paddle3: return "R5";
        case ControllerButton::Paddle4: return "L5";
        case ControllerButton::LeftStick: return "Left_Stick_Click";
        case ControllerButton::RightStick: return "Right_Stick_Click";
        case ControllerButton::DPadUp: return "Dpad_Up";
        case ControllerButton::DPadDown: return "Dpad_Down";
        case ControllerButton::DPadLeft: return "Dpad_Left";
        case ControllerButton::DPadRight: return "Dpad_Right";
        default: return "";
      }
    }

    switch (button) {
      case ControllerButton::A: return "A";
      case ControllerButton::B: return "B";
      case ControllerButton::X: return "X";
      case ControllerButton::Y: return "Y";
      case ControllerButton::LeftShoulder: return "LB";
      case ControllerButton::RightShoulder: return "RB";
      case ControllerButton::TriggerLeft: return "LT";
      case ControllerButton::TriggerRight: return "RT";
      case ControllerButton::Back: return "View";
      case ControllerButton::Guide: return "Menu";
      case ControllerButton::Start: return "Menu";
      case ControllerButton::Misc1: return "Share";
      case ControllerButton::LeftStick: return "Left_Stick_Click";
      case ControllerButton::RightStick: return "Right_Stick_Click";
      case ControllerButton::DPadUp: return "Dpad_Up";
      case ControllerButton::DPadDown: return "Dpad_Down";
      case ControllerButton::DPadLeft: return "Dpad_Left";
      case ControllerButton::DPadRight: return "Dpad_Right";
      default: return "";
    }
  }

  String gamepadGlyphAssetPath(ControllerButton button, GamepadGlyphFamily family) const {
    String glyphName = gamepadGlyphName(button, family);
    if (!glyphName.empty())
      return strf("opensb/interface/mobile/controller/{}/{}.png", gamepadGlyphFolder(family), glyphName);
    return "";
  }

  bool renderGamepadGlyph(ControllerButton button, GamepadGlyphFamily family, ImVec2 size) {
    String path = gamepadGlyphAssetPath(button, family);
    if (!path.empty()) {
      if (auto texture = launcherTexture(path)) {
        renderLauncherTexture(*texture, size);
        return true;
      }
    }
    return false;
  }

  void renderLauncherQuickStartPrompt(float progress, bool enabled) {
    GamepadGlyphFamily family = activeGamepadGlyphFamily();
    float scale = std::clamp(m_launcherUiConfig.scale, 0.75f, 1.75f);
    ImVec2 glyphSize(24.0f * scale, 24.0f * scale);

    if (!enabled)
      ImGui::BeginDisabled();

    ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    ImGui::TextUnformatted(launcherText("launcher.quickStartHoldPrefix", "Hold").utf8Ptr());
    ImGui::SameLine();
    String glyphPath = gamepadGlyphAssetPath(ControllerButton::A, family);
    if (!glyphPath.empty()) {
      if (auto texture = launcherTexture(glyphPath)) {
        float lineHeight = ImGui::GetTextLineHeight();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImVec2 imageMin(cursor.x, cursor.y + (lineHeight - glyphSize.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(imguiTextureId(*texture), imageMin, ImVec2(imageMin.x + glyphSize.x, imageMin.y + glyphSize.y), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        ImGui::Dummy(ImVec2(glyphSize.x, lineHeight));
      } else {
        ImGui::TextUnformatted(gamepadBindingDisplayLabel({ControllerButton::A, "A", true, noneAction(), MobileTouchPressMode::Hold}).utf8Ptr());
      }
    } else {
      ImGui::TextUnformatted(gamepadBindingDisplayLabel({ControllerButton::A, "A", true, noneAction(), MobileTouchPressMode::Hold}).utf8Ptr());
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(launcherText("launcher.quickStartHoldSuffix", "for 1 second to launch").utf8Ptr());

    if (m_launcherQuickStartButtonHeld) {
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
      ImGui::ProgressBar(std::clamp(progress, 0.0f, 1.0f), ImVec2(std::min(280.0f * scale, ImGui::GetContentRegionAvail().x), 0.0f), "");
      ImGui::PopStyleColor();
    }

    if (!enabled)
      ImGui::EndDisabled();
  }

  Maybe<ImVec2> gamepadDiagramAnchor(ControllerButton button, GamepadGlyphFamily family) const {
    if (family == GamepadGlyphFamily::Playstation) {
      switch (button) {
        case ControllerButton::A: return ImVec2(0.78f, 0.59f);
        case ControllerButton::B: return ImVec2(0.86f, 0.50f);
        case ControllerButton::X: return ImVec2(0.72f, 0.50f);
        case ControllerButton::Y: return ImVec2(0.79f, 0.40f);
        case ControllerButton::LeftShoulder: return ImVec2(0.27f, 0.16f);
        case ControllerButton::RightShoulder: return ImVec2(0.73f, 0.16f);
        case ControllerButton::TriggerLeft: return ImVec2(0.27f, 0.08f);
        case ControllerButton::TriggerRight: return ImVec2(0.73f, 0.08f);
        case ControllerButton::Back: return ImVec2(0.37f, 0.39f);
        case ControllerButton::Start: return ImVec2(0.63f, 0.39f);
        case ControllerButton::LeftStick: return ImVec2(0.35f, 0.69f);
        case ControllerButton::RightStick: return ImVec2(0.65f, 0.69f);
        case ControllerButton::DPadUp: return ImVec2(0.22f, 0.42f);
        case ControllerButton::DPadDown: return ImVec2(0.22f, 0.58f);
        case ControllerButton::DPadLeft: return ImVec2(0.16f, 0.50f);
        case ControllerButton::DPadRight: return ImVec2(0.28f, 0.50f);
        case ControllerButton::Touchpad: return ImVec2(0.50f, 0.34f);
        case ControllerButton::Misc1: return ImVec2(0.50f, 0.53f);
        default: return {};
      }
    }

    if (family == GamepadGlyphFamily::Xbox) {
      switch (button) {
        case ControllerButton::A: return ImVec2(0.73f, 0.54f);
        case ControllerButton::B: return ImVec2(0.82f, 0.47f);
        case ControllerButton::X: return ImVec2(0.70f, 0.45f);
        case ControllerButton::Y: return ImVec2(0.76f, 0.37f);
        case ControllerButton::LeftShoulder: return ImVec2(0.25f, 0.14f);
        case ControllerButton::RightShoulder: return ImVec2(0.75f, 0.14f);
        case ControllerButton::TriggerLeft: return ImVec2(0.25f, 0.07f);
        case ControllerButton::TriggerRight: return ImVec2(0.75f, 0.07f);
        case ControllerButton::Back: return ImVec2(0.45f, 0.41f);
        case ControllerButton::Start: return ImVec2(0.57f, 0.41f);
        case ControllerButton::Guide: return ImVec2(0.51f, 0.32f);
        case ControllerButton::Misc1: return ImVec2(0.51f, 0.48f);
        case ControllerButton::LeftStick: return ImVec2(0.25f, 0.45f);
        case ControllerButton::RightStick: return ImVec2(0.58f, 0.62f);
        case ControllerButton::DPadUp: return ImVec2(0.36f, 0.54f);
        case ControllerButton::DPadDown: return ImVec2(0.36f, 0.68f);
        case ControllerButton::DPadLeft: return ImVec2(0.30f, 0.61f);
        case ControllerButton::DPadRight: return ImVec2(0.42f, 0.61f);
        default: return {};
      }
    }

    return {};
  }

  void renderGamepadBindingCallout(ImDrawList* draw, ImVec2 imageMin, ImVec2 imageSize, MobileGamepadBinding const& binding, GamepadGlyphFamily family) const {
    if (!binding.enabled)
      return;

    auto anchor = gamepadDiagramAnchor(binding.button, family);
    if (!anchor)
      return;

    ImVec2 marker(imageMin.x + anchor->x * imageSize.x, imageMin.y + anchor->y * imageSize.y);
    ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    ImU32 line = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 bg = IM_COL32(12, 14, 18, 220);
    String label = actionName(binding.action);
    ImVec2 textSize = ImGui::CalcTextSize(label.utf8Ptr());
    float side = anchor->x < 0.5f ? -1.0f : 1.0f;
    ImVec2 labelPos(marker.x + side * 18.0f, marker.y - textSize.y * 0.5f);
    if (side < 0.0f)
      labelPos.x -= textSize.x + 12.0f;
    labelPos.x = std::clamp(labelPos.x, imageMin.x, imageMin.x + imageSize.x - textSize.x - 12.0f);
    labelPos.y = std::clamp(labelPos.y, imageMin.y, imageMin.y + imageSize.y - textSize.y - 8.0f);

    draw->AddLine(marker, ImVec2(labelPos.x + (side < 0.0f ? textSize.x + 8.0f : 0.0f), labelPos.y + textSize.y * 0.5f), line, 1.5f);
    draw->AddCircleFilled(marker, 4.0f, accent, 16);
    draw->AddRectFilled(ImVec2(labelPos.x - 4.0f, labelPos.y - 3.0f), ImVec2(labelPos.x + textSize.x + 8.0f, labelPos.y + textSize.y + 3.0f), bg, 4.0f);
    draw->AddText(labelPos, text, label.utf8Ptr());
  }

  void renderGamepadReferencePanel(LauncherState& state) {
    GamepadGlyphFamily family = activeGamepadGlyphFamily();
    if (!ImGui::CollapsingHeader(launcherText("gamepadManager.controllerMap", "Controller Map").utf8Ptr(), ImGuiTreeNodeFlags_DefaultOpen))
      return;

    String diagramPath = controllerDiagramAssetPath(family);
    if (!diagramPath.empty()) {
      if (auto texture = launcherTexture(diagramPath)) {
        float availableWidth = ImGui::GetContentRegionAvail().x;
        float maxWidth = family == GamepadGlyphFamily::Switch ? 180.0f : 520.0f;
        float width = std::min(availableWidth, maxWidth * std::clamp(m_launcherUiConfig.scale, 0.75f, 1.75f));
        float height = width * (float)texture->size[1] / (float)std::max(1u, texture->size[0]);
        ImVec2 imageMin = ImGui::GetCursorScreenPos();
        ImVec2 imageSize(width, height);
        renderLauncherTexture(*texture, imageSize);

        if (family == GamepadGlyphFamily::Xbox || family == GamepadGlyphFamily::Playstation) {
          auto* draw = ImGui::GetWindowDrawList();
          for (auto const& binding : state.gamepadBindings)
            renderGamepadBindingCallout(draw, imageMin, imageSize, binding, family);
        }
      }
    }

    float glyphSize = 28.0f * std::clamp(m_launcherUiConfig.scale, 0.75f, 1.75f);
    for (auto const& binding : state.gamepadBindings) {
      if (!binding.enabled)
        continue;

      String mapId = String("map") + ControllerButtonNames.getRight(binding.button);
      ImGui::PushID(mapId.utf8Ptr());
      if (renderGamepadGlyph(binding.button, family, ImVec2(glyphSize, glyphSize)))
        ImGui::SameLine();
      String displayLabel = gamepadBindingDisplayLabel(binding);
      ImGui::Text("%s  %s", displayLabel.utf8Ptr(), actionName(binding.action).utf8Ptr());
      ImGui::PopID();
    }
  }

  void renderGamepadStickModeCombo(MobileGamepadStickConfig& stick) {
    std::vector<pair<String, MobileGamepadStickMode>> choices{
      {launcherText("gamepadManager.modeMovement", "Movement"), MobileGamepadStickMode::Movement},
      {launcherText("gamepadManager.modeAim", "Camera Aim"), MobileGamepadStickMode::Aim}
    };

    int index = 0;
    for (int i = 0; i < (int)choices.size(); ++i) {
      if (choices[i].second == stick.mode) {
        index = i;
        break;
      }
    }

    if (ImGui::BeginCombo(launcherText("gamepadManager.mode", "Mode").utf8Ptr(), choices[index].first.utf8Ptr())) {
      for (int i = 0; i < (int)choices.size(); ++i) {
        bool selected = index == i;
        if (ImGui::Selectable(choices[i].first.utf8Ptr(), selected))
          stick.mode = choices[i].second;
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  void renderGamepadStickSettings(char const* label, MobileGamepadStickConfig& stick) {
    if (ImGui::TreeNode(label)) {
      ImGui::Checkbox(launcherText("gamepadManager.stickEnabled", "Enabled").utf8Ptr(), &stick.enabled);
      renderGamepadStickModeCombo(stick);
      ImGui::SliderFloat(launcherText("gamepadManager.deadzone", "Deadzone").utf8Ptr(), &stick.deadzone, 0.0f, 0.85f);
      ImGui::Checkbox(launcherText("gamepadManager.invertX", "Invert X").utf8Ptr(), &stick.invertX);
      sameLineIfNextFits(ImGui::CalcTextSize(launcherText("gamepadManager.invertY", "Invert Y").utf8Ptr()).x + ImGui::GetFrameHeight());
      ImGui::Checkbox(launcherText("gamepadManager.invertY", "Invert Y").utf8Ptr(), &stick.invertY);
      if (stick.mode == MobileGamepadStickMode::Aim) {
        ImGui::SliderFloat(launcherText("gamepadManager.sensitivity", "Sensitivity").utf8Ptr(), &stick.sensitivity, 0.10f, 5.0f);
        ImGui::Checkbox(launcherText("gamepadManager.preciseAim", "Precise cursor aim").utf8Ptr(), &stick.preciseAim);
      }
      ImGui::TreePop();
    }
  }

  void renderGamepadButtonBindings(LauncherState& state) {
    ImGui::TextUnformatted(launcherText("gamepadManager.bindings", "Button Bindings").utf8Ptr());
    GamepadGlyphFamily family = activeGamepadGlyphFamily();
    float glyphSize = 28.0f * std::clamp(m_launcherUiConfig.scale, 0.75f, 1.75f);

    for (auto& binding : state.gamepadBindings) {
      String displayLabel = gamepadBindingDisplayLabel(binding);
      String summary = strf("{} -> {}", displayLabel, actionName(binding.action));

      ImGui::PushID(ControllerButtonNames.getRight(binding.button).utf8Ptr());
      if (renderGamepadGlyph(binding.button, family, ImVec2(glyphSize, glyphSize)))
        ImGui::SameLine();
      bool open = ImGui::TreeNodeEx(summary.utf8Ptr(), ImGuiTreeNodeFlags_DefaultOpen);
      if (open) {
        ImGui::Checkbox(launcherText("gamepadManager.bindingEnabled", "Binding enabled").utf8Ptr(), &binding.enabled);
        ImGui::Text("%s: %s", launcherText("gamepadManager.button", "Button").utf8Ptr(), displayLabel.utf8Ptr());
        renderGamepadActionCombo(state, launcherText("touchManager.interaction", "Interaction").utf8Ptr(), binding.action, String("gamepad:") + ControllerButtonNames.getRight(binding.button));
        renderGamepadPressModeCombo(binding);
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
  }

  void renderGamepadManager(LauncherState& state) {
    ImGui::TextUnformatted(launcherText("gamepadManager.title", "Gamepad Controls").utf8Ptr());
    ImGui::Separator();

    beginLauncherScrollArea("GamepadManagerScroll", state);
    if (ImGui::Button(launcherText("common.backToControls", "Back to Controls").utf8Ptr()))
      state.gamepadManagerOpen = false;
    sameLineIfNextFits(imguiButtonWidth(launcherText("common.save", "Save").utf8Ptr()));
    if (ImGui::Button(launcherText("common.save", "Save").utf8Ptr()))
      persistLauncherState(state);

    String connected = m_activeGamepadName.empty() ? launcherText("gamepadManager.noController", "No controller connected") : m_activeGamepadName;
    ImGui::Text("%s: %s", launcherText("gamepadManager.connected", "Connected").utf8Ptr(), connected.utf8Ptr());
    ImGui::Text("%s: %s", launcherText("gamepadManager.layout", "Layout").utf8Ptr(), activeGamepadLayoutName().utf8Ptr());

    renderGamepadReferencePanel(state);

    ImGui::Checkbox(launcherText("gamepadManager.enable", "Enable gamepad controls").utf8Ptr(), &state.gamepadConfig.enabled);
    ImGui::SliderFloat(launcherText("gamepadManager.triggerThreshold", "Trigger press threshold").utf8Ptr(), &state.gamepadConfig.triggerThreshold, 0.05f, 0.95f);
    renderGamepadStickSettings(launcherText("gamepadManager.leftStick", "Left Stick").utf8Ptr(), state.gamepadConfig.leftStick);
    renderGamepadStickSettings(launcherText("gamepadManager.rightStick", "Right Stick").utf8Ptr(), state.gamepadConfig.rightStick);

    ImGui::Separator();
    renderGamepadButtonBindings(state);

    ImGui::EndChild();
  }

  // Deferred label for the preview, drawn with a contrast outline after the
  // inversion-blended shape pass (see renderTouchPreview).
  struct PreviewLabel {
    ImVec2 pos;
    String text;
  };

  // Same inversion (difference) blend the in-game overlay uses for its
  // always-visible button geometry (Minecraft's crosshair technique).
  static void applyTouchPreviewContrastBlend(ImDrawList const*, ImDrawCmd const*) {
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ZERO);
  }

  static void drawPreviewLabelWithOutline(ImDrawList* draw, ImVec2 pos, char const* text, ImU32 color, ImU32 outline) {
    static ImVec2 const offsets[8] = {
      {-1.0f, -1.0f}, {0.0f, -1.0f}, {1.0f, -1.0f},
      {-1.0f,  0.0f},                {1.0f,  0.0f},
      {-1.0f,  1.0f}, {0.0f,  1.0f}, {1.0f,  1.0f}
    };
    for (auto const& o : offsets)
      draw->AddText(ImVec2(pos.x + o.x, pos.y + o.y), outline, text);
    draw->AddText(pos, color, text);
  }

  void renderTouchElementPreview(LauncherState& state, ImDrawList* draw, ImVec2 canvasMin, ImVec2 canvasSize, int index, std::vector<PreviewLabel>& labels) {
    auto& element = state.touchElements[index];
    if (!element.enabled)
      return;

    float baseRadius = std::min(canvasSize.x, canvasSize.y) * 0.075f * state.touchConfig.size;
    float radius = baseRadius * std::clamp(element.size, 0.45f, 2.4f);
    ImVec2 center(canvasMin.x + element.position[0] * canvasSize.x, canvasMin.y + element.position[1] * canvasSize.y);

    // Match the in-game overlay: opaque contrast colours (the inversion blend
    // ignores alpha) with the opacity setting remapped to a contrast strength,
    // and the configurable outline thickness.
    float op = std::clamp(state.touchConfig.opacity, 0.0f, 1.0f);
    auto contrast = [op](float c) { return 0.5f + (c - 0.5f) * op; };
    auto contrastColor = [&contrast](float r, float g, float b) {
      return IM_COL32((int)std::lround(contrast(r) * 255.0f),
                      (int)std::lround(contrast(g) * 255.0f),
                      (int)std::lround(contrast(b) * 255.0f), 255);
    };
    ImU32 base = contrastColor(1.0f, 1.0f, 1.0f);
    ImU32 fill = index == state.selectedTouchElement
        ? contrastColor(0.47f, 0.75f, 1.0f)
        : contrastColor(0.31f, 0.63f, 1.0f);
    float thickness = std::max(1.0f, state.touchConfig.outlineThickness);

    ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
    ImGui::PushID(element.id.utf8Ptr());
    ImGui::InvisibleButton("##touchControl", ImVec2(radius * 2.0f, radius * 2.0f));
    if (ImGui::IsItemClicked())
      state.selectedTouchElement = index;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      ImVec2 delta = ImGui::GetIO().MouseDelta;
      element.position[0] = std::clamp(element.position[0] + delta.x / std::max(1.0f, canvasSize.x), 0.03f, 0.97f);
      element.position[1] = std::clamp(element.position[1] + delta.y / std::max(1.0f, canvasSize.y), 0.03f, 0.97f);
    }
    ImGui::PopID();

    if (element.kind == MobileTouchElementKind::DPad) {
      float arm = radius * 0.38f;
      draw->AddRectFilled(ImVec2(center.x - arm, center.y - radius), ImVec2(center.x + arm, center.y + radius), fill, arm * 0.2f);
      draw->AddRectFilled(ImVec2(center.x - radius, center.y - arm), ImVec2(center.x + radius, center.y + arm), fill, arm * 0.2f);
      draw->AddRect(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius), base, arm * 0.25f, 0, thickness);
      labels.push_back({ImVec2(center.x - radius * 0.38f, center.y - radius * 0.18f), launcherText("touchElement.dpad", "D-PAD")});
    } else if (element.kind == MobileTouchElementKind::Joystick || element.kind == MobileTouchElementKind::AimJoystick) {
      draw->AddCircle(center, radius, base, 48, thickness);
      draw->AddCircleFilled(center, radius * 0.34f, fill, 32);
    } else if (element.kind == MobileTouchElementKind::PerformanceCounter) {
      // No shape at runtime (it's a passive display, not a touch target) --
      // show representative placeholder text instead, so the preview looks
      // like what actually renders in-game while positioning it.
      String placeholder = element.perfCounterMode == PerformanceCounterMode::Detailed
          ? String("60 FPS\n60.00Hz\n00512µs\n00048µs")
          : String("60 FPS");
      labels.push_back({ImVec2(center.x - radius * 0.55f, center.y - radius * 0.4f), placeholder});
    } else {
      draw->AddCircleFilled(center, radius * 0.55f, fill, 32);
      draw->AddCircle(center, radius * 0.55f, base, 48, thickness);
      labels.push_back({ImVec2(center.x - radius * 0.22f, center.y - radius * 0.18f), element.label});
    }
  }

  void renderTouchPreview(LauncherState& state, ImVec2 displaySize) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(displaySize, ImGuiCond_Always);
    ImGui::Begin(launcherText("touchPreview.windowTitle", "Touch Layout Preview").utf8Ptr(), nullptr,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetWindowPos();
    draw->AddRectFilled(min, ImVec2(min.x + displaySize.x, min.y + displaySize.y), IM_COL32(15, 18, 24, 235));

    ImGui::SetCursorScreenPos(ImVec2(min.x + 16.0f, min.y + 16.0f));
    auto const& style = ImGui::GetStyle();
    bool selectedIsPerfCounter = state.selectedTouchElement >= 0 && state.selectedTouchElement < (int)state.touchElements.size()
        && state.touchElements[state.selectedTouchElement].kind == MobileTouchElementKind::PerformanceCounter;
    float toolbarHeight = style.WindowPadding.y * 2.0f
        + ImGui::GetTextLineHeightWithSpacing() * 2.0f
        + ImGui::GetFrameHeightWithSpacing()
        + ImGui::GetFrameHeight()
        + (selectedIsPerfCounter ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
    ImGui::BeginChild("TouchPreviewToolbar", ImVec2(std::min(520.0f, displaySize.x - 32.0f), toolbarHeight), true);
    ImGui::TextUnformatted(launcherText("touchPreview.title", "Touch Layout Preview").utf8Ptr());
    if (state.selectedTouchElement >= 0 && state.selectedTouchElement < (int)state.touchElements.size()) {
      auto& selected = state.touchElements[state.selectedTouchElement];
      ImGui::Text("%s: %s", launcherText("touchPreview.selectedLabel", "Selected").utf8Ptr(), selected.label.utf8Ptr());
      ImGui::SliderFloat(launcherText("touchPreview.selectedSize", "Selected size").utf8Ptr(), &selected.size, 0.45f, 2.4f);
      if (selected.kind == MobileTouchElementKind::PerformanceCounter) {
        bool detailed = selected.perfCounterMode == PerformanceCounterMode::Detailed;
        // Store as String, not char const*: .utf8Ptr() on a launcherText()
        // temporary dangles the instant the initializer statement ends, since
        // the String owning that buffer is destroyed right there -- BeginCombo
        // below would read freed memory (this was the "dropdown shows its own
        // label instead of the selected value" bug).
        String modeLabel = detailed
            ? launcherText("touchManager.perfCounterModeDetailed", "Detailed")
            : launcherText("touchManager.perfCounterModeFps", "FPS");
        if (ImGui::BeginCombo(launcherText("touchManager.perfCounterMode", "Display mode").utf8Ptr(), modeLabel.utf8Ptr())) {
          for (int i = 0; i < 2; ++i) {
            bool isDetailed = i == 1;
            bool itemSelected = detailed == isDetailed;
            String label = isDetailed
                ? launcherText("touchManager.perfCounterModeDetailed", "Detailed")
                : launcherText("touchManager.perfCounterModeFps", "FPS");
            if (ImGui::Selectable(label.utf8Ptr(), itemSelected))
              selected.perfCounterMode = isDetailed ? PerformanceCounterMode::Detailed : PerformanceCounterMode::Fps;
            if (itemSelected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      }
    }
    if (ImGui::Button(launcherText("common.done", "Done").utf8Ptr()))
      state.touchPreviewOpen = false;
    ImGui::EndChild();

    // Register control drag hitboxes after the toolbar so overlapping controls
    // can always be pulled out from underneath it. Shapes are drawn with the
    // same always-visible inversion blend as the in-game overlay; labels are
    // drawn afterwards under the default blend with a contrast outline.
    std::vector<PreviewLabel> labels;
    draw->AddCallback(applyTouchPreviewContrastBlend, nullptr);
    for (int i = 0; i < (int)state.touchElements.size(); ++i)
      renderTouchElementPreview(state, draw, min, displaySize, i, labels);
    draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    float op = std::clamp(state.touchConfig.opacity, 0.0f, 1.0f);
    ImU32 labelColor = IM_COL32(255, 255, 255, (int)(230.0f * op));
    ImU32 labelOutline = IM_COL32(0, 0, 0, (int)(210.0f * op));
    for (auto const& label : labels)
      drawPreviewLabelWithOutline(draw, label.pos, label.text.utf8Ptr(), labelColor, labelOutline);

    ImGui::End();
  }

  void renderLauncherUiSettings(LauncherState& state) {
    static String lastLoggedLocale;
    static String lastLoggedLanguageLabel;
    beginLauncherScrollArea("LauncherUiSettingsScroll", state);

    ImGui::TextUnformatted(launcherText("uiSettings.title", "Settings").utf8Ptr());
    ImGui::Separator();

    if (ImGui::Button(launcherText("common.backToLauncher", "Back to Launcher").utf8Ptr()))
      state.uiSettingsOpen = false;
    sameLineIfNextFits(imguiButtonWidth(launcherText("common.save", "Save").utf8Ptr()));
    if (ImGui::Button(launcherText("common.save", "Save").utf8Ptr())) {
      persistLauncherState(state);
      reloadLauncherLocalization(state.locale);
    }

    std::vector<pair<String, String>> localeChoices{
      {launcherText("uiSettings.language.system", "System default"), SystemLocaleMarker},
      {launcherText("uiSettings.language.english", "English"), String("en_US")},
      {launcherText("uiSettings.language.simplifiedChinese", "简体中文"), String("zh_CN")}
    };
    int localeIndex = 0;
    for (int i = 0; i < (int)localeChoices.size(); ++i) {
      if (localeChoices[i].second == state.locale) {
        localeIndex = i;
        break;
      }
      if (i == 0 && state.locale == state.systemLocale)
        localeIndex = 0;
    }
    if (ImGui::BeginCombo(launcherText("uiSettings.language", "Language").utf8Ptr(), localeChoices[localeIndex].first.utf8Ptr())) {
      for (int i = 0; i < (int)localeChoices.size(); ++i) {
        bool selected = localeIndex == i;
        if (ImGui::Selectable(localeChoices[i].first.utf8Ptr(), selected)) {
          state.locale = localeChoices[i].second;
          reloadLauncherLocalization(state.locale);
          persistLauncherState(state);
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SliderFloat(launcherText("uiSettings.scale", "Launcher UI size").utf8Ptr(), &state.uiConfig.scale, 0.75f, 1.75f, "%.2fx");
    ImGui::ColorEdit3(launcherText("uiSettings.accentColor", "Accent color").utf8Ptr(), state.uiConfig.accentColor.data(),
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha);

    if (ImGui::Button(launcherText("uiSettings.reset", "Reset UI Settings").utf8Ptr())) {
      state.uiConfig = LauncherUiConfig();
      applyLauncherUiConfig(state.uiConfig);
    }

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextUnformatted(launcherText("uiSettings.display", "Display").utf8Ptr());
    ImGui::Separator();
    {
      bool fullscreen = m_fullscreenRender;
      if (ImGui::Checkbox(launcherText("uiSettings.fullscreenRender", "Fullscreen rendering").utf8Ptr(), &fullscreen)) {
        m_fullscreenRender = fullscreen;
        persistLauncherState(state);
        syncWindowMetrics(true);
      }
      ImGui::TextDisabled("%s", launcherText("uiSettings.fullscreenRenderHint",
          "Off: the view avoids the notch / camera cutout (black bar). On: the game fills the whole screen and the cutout may cover content.").utf8Ptr());
    }
#endif

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextUnformatted(launcherText("uiSettings.performance", "Performance").utf8Ptr());
    ImGui::Separator();

    // Simulation tick rate: 60 Hz matches vanilla physics/animation
    // granularity; 30 Hz halves per-second sim cost for weaker devices
    // (gameplay speed is identical -- rate * timestep = 1 either way).
    {
      int simIndex = m_perfSimRate == 30 ? 0 : 1;
      char const* simLabels[] = {"30 Hz", "60 Hz"};
      if (ImGui::BeginCombo(launcherText("uiSettings.simRate", "Simulation rate").utf8Ptr(), simLabels[simIndex])) {
        for (int i = 0; i < 2; ++i) {
          bool selected = simIndex == i;
          if (ImGui::Selectable(simLabels[i], selected)) {
            m_perfSimRate = i == 0 ? 30 : 60;
            persistLauncherState(state);
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::TextDisabled("%s", launcherText("uiSettings.simRateHint", "60 Hz = smoother physics; 30 Hz = lighter on weak devices. Game speed is the same.").utf8Ptr());
    }

    // Rendered-FPS cap, independent of the simulation rate. Unlimited lets a
    // powerful device render as fast as it can; a cap saves battery/thermals.
    {
#ifdef STAR_SYSTEM_SWITCH
      // No "Unlimited" on Switch: the panel is 60Hz (see loadLauncherState).
      static int const fpsCapChoices[] = {30, 45, 60};
#else
      static int const fpsCapChoices[] = {0, 30, 45, 60, 90, 120, 144, 240};
#endif
      int capIndex = 0;
      for (int i = 0; i < (int)(sizeof(fpsCapChoices) / sizeof(fpsCapChoices[0])); ++i) {
        if (fpsCapChoices[i] == m_perfFpsCap) {
          capIndex = i;
          break;
        }
      }
      auto capLabel = [&](int cap) {
        return cap == 0 ? launcherText("uiSettings.fpsCapUnlimited", "Unlimited") : strf("{}", cap);
      };
      if (ImGui::BeginCombo(launcherText("uiSettings.fpsCap", "FPS cap").utf8Ptr(), capLabel(fpsCapChoices[capIndex]).utf8Ptr())) {
        for (int i = 0; i < (int)(sizeof(fpsCapChoices) / sizeof(fpsCapChoices[0])); ++i) {
          bool selected = capIndex == i;
          if (ImGui::Selectable(capLabel(fpsCapChoices[i]).utf8Ptr(), selected)) {
            m_perfFpsCap = fpsCapChoices[i];
            persistLauncherState(state);
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    }

    ImGui::EndChild();
  }

  void renderTouchManager(LauncherState& state) {
    bool gyroAvailable = platformGyroAvailable();
    if (!gyroAvailable)
      state.touchConfig.gyroEnabled = false;

    ImGui::TextUnformatted(launcherText("touchManager.title", "Controls/Overlay").utf8Ptr());
    ImGui::Separator();

    beginLauncherScrollArea("TouchManagerScroll", state);
    if (ImGui::Button(launcherText("common.backToLauncher", "Back to Launcher").utf8Ptr()))
      state.touchManagerOpen = false;
    sameLineIfNextFits(imguiButtonWidth(launcherText("gamepadManager.open", "Gamepad Controls").utf8Ptr()));
    if (ImGui::Button(launcherText("gamepadManager.open", "Gamepad Controls").utf8Ptr()))
      state.gamepadManagerOpen = true;
    sameLineIfNextFits(imguiButtonWidth(launcherText("touchManager.previewAdjust", "Preview / Adjust Layout").utf8Ptr()));
    if (ImGui::Button(launcherText("touchManager.previewAdjust", "Preview / Adjust Layout").utf8Ptr()))
      state.touchPreviewOpen = true;
    sameLineIfNextFits(imguiButtonWidth(launcherText("common.save", "Save").utf8Ptr()));
    if (ImGui::Button(launcherText("common.save", "Save").utf8Ptr()))
      persistLauncherState(state);

    ImGui::TextUnformatted(launcherText("touchManager.touchSection", "Controls").utf8Ptr());
    ImGui::Checkbox(launcherText("touchManager.enableOverlay", "Enable touch overlay").utf8Ptr(), &state.touchConfig.enabled);
    ImGui::Checkbox(launcherText("touchManager.enableDirectGestures", "Enable direct screen touch gestures").utf8Ptr(), &state.touchConfig.directTouchGestures);
    if (state.touchConfig.directTouchGestures) {
      bool touchpad = state.touchConfig.directTouchGestureMode == DirectTouchGestureMode::Touchpad;
      // Store as String, not char const*: .utf8Ptr() on a launcherText()
      // temporary dangles the instant this initializer statement ends.
      String modeLabel = touchpad
          ? launcherText("touchManager.gestureModeTouchpad", "Touchpad")
          : launcherText("touchManager.gestureModeTouchscreen", "Touchscreen");
      ImGui::Indent();
      if (ImGui::BeginCombo(launcherText("touchManager.gestureMode", "Gesture mode").utf8Ptr(), modeLabel.utf8Ptr())) {
        for (int i = 0; i < 2; ++i) {
          bool isTouchpad = i == 1;
          bool selected = touchpad == isTouchpad;
          String label = isTouchpad
              ? launcherText("touchManager.gestureModeTouchpad", "Touchpad")
              : launcherText("touchManager.gestureModeTouchscreen", "Touchscreen");
          if (ImGui::Selectable(label.utf8Ptr(), selected))
            state.touchConfig.directTouchGestureMode = isTouchpad ? DirectTouchGestureMode::Touchpad : DirectTouchGestureMode::Touchscreen;
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::TextDisabled("%s", launcherText("touchManager.gestureModeHint",
          "Touchscreen: cursor tracks your finger directly. Touchpad: swipe to move the cursor relatively, like a laptop touchpad.").utf8Ptr());
      renderTouchActionCombo(state, launcherText("touchManager.singleTouchAction", "Single-touch action").utf8Ptr(),
          state.touchConfig.directTouchSingleAction, "directTouchSingleAction");
      renderTouchActionCombo(state, launcherText("touchManager.twoFingerTouchAction", "Two-finger touch action").utf8Ptr(),
          state.touchConfig.directTouchTwoFingerAction, "directTouchTwoFingerAction");
      ImGui::TextDisabled("%s", launcherText("touchManager.directTouchActionHint",
          "What single-finger and two-finger taps/holds do. Defaults match the classic behavior (aim/attack, secondary aim/use). Pick No Action to disable a gesture.").utf8Ptr());
      ImGui::Unindent();
    }
    ImGui::Checkbox(launcherText("touchManager.joystickAimCursor", "Enable joystick aim cursor").utf8Ptr(), &state.touchConfig.joystickAimCursorEnabled);
    ImGui::TextDisabled("%s", launcherText("touchManager.joystickAimCursorHint",
        "Shows the aim cursor while aiming with a joystick, from the touch-screen aim joystick or a physical controller.").utf8Ptr());
    ImGui::BeginDisabled(!gyroAvailable);
    ImGui::Checkbox(launcherText("touchManager.enableGyroAim", "Enable gyro aim").utf8Ptr(), &state.touchConfig.gyroEnabled);
    ImGui::EndDisabled();
    if (!gyroAvailable) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", launcherText("touchManager.noGyroFound", "No gyro found").utf8Ptr());
    }
    ImGui::SliderFloat(launcherText("touchManager.overlayOpacity", "Overlay opacity").utf8Ptr(), &state.touchConfig.opacity, 0.0f, 1.0f);
    ImGui::SliderFloat(launcherText("touchManager.globalControlSize", "Global control size").utf8Ptr(), &state.touchConfig.size, 0.6f, 1.8f);
    ImGui::SliderFloat(launcherText("touchManager.outlineThickness", "Button outline thickness").utf8Ptr(), &state.touchConfig.outlineThickness, 1.0f, 5.0f);
    ImGui::SliderFloat(launcherText("touchManager.joystickDeadzone", "Joystick deadzone").utf8Ptr(), &state.touchConfig.deadzone, 0.0f, 0.6f);
    ImGui::BeginDisabled(!gyroAvailable);
    ImGui::SliderFloat(launcherText("touchManager.gyroSensitivity", "Gyro sensitivity").utf8Ptr(), &state.touchConfig.gyroSensitivity, 0.10f, 12.0f);
    ImGui::Checkbox(launcherText("touchManager.invertGyroX", "Invert gyro X axis").utf8Ptr(), &state.touchConfig.gyroInvertX);
    ImGui::Checkbox(launcherText("touchManager.invertGyroY", "Invert gyro Y axis").utf8Ptr(), &state.touchConfig.gyroInvertY);
    ImGui::EndDisabled();

    if (ImGui::Button(launcherText("touchManager.newButton", "New Button").utf8Ptr())) {
      state.newTouchButtonPopup = true;
      state.newTouchActionIndex = 0;
      state.newTouchButtonSize = 1.0f;
      ImGui::OpenPopup(launcherText("touchManager.newButtonPopupTitle", "New Touch Button").utf8Ptr());
    }

    if (ImGui::BeginPopupModal(launcherText("touchManager.newButtonPopupTitle", "New Touch Button").utf8Ptr(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      auto choices = touchActionChoices();
      if (state.newTouchActionIndex < 0 || state.newTouchActionIndex >= (int)choices.size())
        state.newTouchActionIndex = 0;
      if (ImGui::BeginCombo(launcherText("touchManager.interaction", "Interaction").utf8Ptr(), choices[state.newTouchActionIndex].first.utf8Ptr())) {
        for (int i = 0; i < (int)choices.size(); ++i) {
          bool selected = state.newTouchActionIndex == i;
          if (ImGui::Selectable(choices[i].first.utf8Ptr(), selected))
            state.newTouchActionIndex = i;
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::SliderFloat(launcherText("touchManager.buttonSize", "Button size").utf8Ptr(), &state.newTouchButtonSize, 0.45f, 2.4f);
      if (ImGui::Button(launcherText("common.create", "Create").utf8Ptr(), ImVec2(140.0f, 0.0f))) {
        auto action = choices[state.newTouchActionIndex].second;
        MobileTouchElement element;
        element.id = strf("custom{}", state.touchElements.size() + 1);
        element.label = actionName(action);
        element.kind = MobileTouchElementKind::Button;
        element.enabled = true;
        element.position = {0.50f, 0.50f};
        element.size = state.newTouchButtonSize;
        element.action = action;
        state.touchElements.push_back(element);
        state.selectedTouchElement = (int)state.touchElements.size() - 1;
        state.touchPreviewOpen = true;
        ImGui::CloseCurrentPopup();
      }
      sameLineIfNextFits(imguiButtonWidth(launcherText("common.cancel", "Cancel").utf8Ptr(), ImVec2(140.0f, 0.0f)));
      if (ImGui::Button(launcherText("common.cancel", "Cancel").utf8Ptr(), ImVec2(140.0f, 0.0f)))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    ImGui::Separator();
    float listHeight = std::max(240.0f, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y);
    if (ImGui::BeginChild("TouchControlList", ImVec2(0.0f, listHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
      for (int i = 0; i < (int)state.touchElements.size(); ++i) {
        auto& element = state.touchElements[i];
        ImGui::PushID(element.id.utf8Ptr());
        ImGui::Checkbox("##enabled", &element.enabled);
        ImGui::SameLine();
        bool selected = state.selectedTouchElement == i;
        String listLabel = element.label.empty() ? launcherText("touchManager.blankLabel", "(blank)") : element.label;
        if (ImGui::Selectable(listLabel.utf8Ptr(), selected, ImGuiSelectableFlags_AllowItemOverlap))
          state.selectedTouchElement = i;
        if (selected) {
          ImGui::Indent();
          if (state.touchLabelBufferElementId != element.id) {
            state.touchLabelBufferElementId = element.id;
            std::snprintf(state.touchLabelBuffer, sizeof(state.touchLabelBuffer), "%s", element.label.utf8Ptr());
          }
          if (element.kind == MobileTouchElementKind::Button) {
            if (ImGui::InputText(launcherText("touchManager.displayedText", "Displayed text").utf8Ptr(), state.touchLabelBuffer, sizeof(state.touchLabelBuffer)))
              element.label = String(state.touchLabelBuffer).trim();
          }
          ImGui::SliderFloat(launcherText("touchManager.elementSize", "Size").utf8Ptr(), &element.size, 0.45f, 2.4f);
          float position[2] = {element.position[0], element.position[1]};
          if (ImGui::SliderFloat2(launcherText("touchManager.position", "Position").utf8Ptr(), position, 0.03f, 0.97f))
            element.position = {position[0], position[1]};
          if (element.kind == MobileTouchElementKind::Button) {
            renderTouchActionCombo(state, launcherText("touchManager.interaction", "Interaction").utf8Ptr(), element.action, element.id + ":action");
            renderTouchPressModeCombo(element);
          }
          else if (element.kind == MobileTouchElementKind::DPad) {
            renderTouchActionCombo(state, launcherText("touchManager.directionUp", "Up").utf8Ptr(), element.upAction, element.id + ":up");
            renderTouchActionCombo(state, launcherText("touchManager.directionDown", "Down").utf8Ptr(), element.downAction, element.id + ":down");
            renderTouchActionCombo(state, launcherText("touchManager.directionLeft", "Left").utf8Ptr(), element.leftAction, element.id + ":left");
            renderTouchActionCombo(state, launcherText("touchManager.directionRight", "Right").utf8Ptr(), element.rightAction, element.id + ":right");
          } else if (element.kind == MobileTouchElementKind::AimJoystick) {
            ImGui::Checkbox(launcherText("touchManager.preciseAim", "Precise aim").utf8Ptr(), &element.preciseAim);
            ImGui::SliderFloat(launcherText("touchManager.aimSensitivity", "Aim sensitivity").utf8Ptr(), &element.aimSensitivity, 0.25f, 4.0f);
            if (element.preciseAim)
              ImGui::TextDisabled("%s", launcherText("touchManager.preciseAimHint", "Precise aim moves the virtual cursor.").utf8Ptr());
            else
              ImGui::TextDisabled("%s", launcherText("touchManager.directionalAimHint", "Directional aim points around the player.").utf8Ptr());
          } else if (element.kind == MobileTouchElementKind::PerformanceCounter) {
            bool detailed = element.perfCounterMode == PerformanceCounterMode::Detailed;
            // Store as String, not char const*: .utf8Ptr() on a launcherText()
            // temporary dangles the instant this initializer statement ends.
            String modeLabel = detailed
                ? launcherText("touchManager.perfCounterModeDetailed", "Detailed")
                : launcherText("touchManager.perfCounterModeFps", "FPS");
            if (ImGui::BeginCombo(launcherText("touchManager.perfCounterMode", "Display mode").utf8Ptr(), modeLabel.utf8Ptr())) {
              for (int i = 0; i < 2; ++i) {
                bool isDetailed = i == 1;
                bool itemSelected = detailed == isDetailed;
                String label = isDetailed
                    ? launcherText("touchManager.perfCounterModeDetailed", "Detailed")
                    : launcherText("touchManager.perfCounterModeFps", "FPS");
                if (ImGui::Selectable(label.utf8Ptr(), itemSelected))
                  element.perfCounterMode = isDetailed ? PerformanceCounterMode::Detailed : PerformanceCounterMode::Fps;
                if (itemSelected)
                  ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
          } else {
            ImGui::TextDisabled("%s", launcherText("touchManager.joystickHint", "Joystick sends movement keys.").utf8Ptr());
          }
          ImGui::Unindent();
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    ImGui::EndChild();
  }

  // Derive the active logical screen from the LauncherState open-flags. The
  // priority here must match the render dispatch in runLauncher exactly.
  LauncherScreen deriveLauncherScreen(LauncherState const& state) const {
    if (state.uiSettingsOpen)
      return LauncherScreen::UiSettings;
    if (state.touchManagerOpen && state.gamepadManagerOpen)
      return LauncherScreen::GamepadManager;
    if (state.touchManagerOpen)
      return LauncherScreen::TouchManager;
    if (state.modManagerOpen)
      return LauncherScreen::ModManager;
    if (state.saveManagerOpen)
      return LauncherScreen::SaveManager;
    return LauncherScreen::Main;
  }

  // Set the open-flags so that deriveLauncherScreen() would return `screen`.
  // Mirrors the side effects the in-panel navigation buttons perform on entry.
  void applyLauncherScreen(LauncherState& state, LauncherScreen screen) {
    bool touch = screen == LauncherScreen::TouchManager || screen == LauncherScreen::GamepadManager;
    state.uiSettingsOpen = screen == LauncherScreen::UiSettings;
    state.touchManagerOpen = touch;
    state.gamepadManagerOpen = screen == LauncherScreen::GamepadManager;
    state.modManagerOpen = screen == LauncherScreen::ModManager;
    state.saveManagerOpen = screen == LauncherScreen::SaveManager;

    if (screen == LauncherScreen::ModManager)
      state.modListDirty = true;
    if (touch)
      state.selectedTouchElement = std::clamp(state.selectedTouchElement, 0, std::max(0, (int)state.touchElements.size() - 1));

    state.currentScreen = screen;
  }

  // Move one step back through the navigation history. The touch-layout preview
  // is a transient overlay, so a back gesture dismisses it first. Returns true
  // if the gesture consumed a navigation step.
  bool navigateLauncherBack(LauncherState& state) {
    if (state.touchPreviewOpen) {
      state.touchPreviewOpen = false;
      return true;
    }
    if (state.navBackStack.empty())
      return false;

    LauncherScreen previous = state.navBackStack.back();
    state.navBackStack.pop_back();
    state.navForwardStack.push_back(state.currentScreen);
    applyLauncherScreen(state, previous);
    return true;
  }

  // Re-enter a screen a previous back gesture unwound out of.
  bool navigateLauncherForward(LauncherState& state) {
    if (state.touchPreviewOpen || state.navForwardStack.empty())
      return false;

    LauncherScreen next = state.navForwardStack.back();
    state.navForwardStack.pop_back();
    state.navBackStack.push_back(state.currentScreen);
    applyLauncherScreen(state, next);
    return true;
  }

  // Consume a back/forward request queued by the hardware back button, Escape,
  // a gamepad face button, or an edge swipe. Modal popups and text input keep
  // their own Escape/back semantics, so navigation is suppressed while they are
  // active to avoid pulling a screen out from under them.
  void applyLauncherPendingNavigation(LauncherState& state) {
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    // Fold in any back/forward requested by a native OS gesture or button since
    // the last frame. A native request takes precedence over an in-app one.
    if (int nativeNav = ::g_launcherNativeNavRequest.exchange(0, std::memory_order_relaxed))
      m_launcherPendingNav = nativeNav;
#endif

    int pending = m_launcherPendingNav;
    m_launcherPendingNav = 0;
    if (pending == 0)
      return;

    if (ImGui::GetCurrentContext()) {
      if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;
      if (ImGui::GetIO().WantTextInput)
        return;
    }

    if (pending < 0)
      navigateLauncherBack(state);
    else
      navigateLauncherForward(state);
  }

  // After a frame renders, the in-panel buttons may have changed the open-flags
  // directly. Fold that change into the history stack so button navigation and
  // gesture navigation share one consistent back/forward model: navigating to
  // the screen sitting on top of the back stack (e.g. "Back to Launcher") is
  // treated as a back step; any other change is a new forward navigation.
  void reconcileLauncherNavigation(LauncherState& state) {
    LauncherScreen derived = deriveLauncherScreen(state);
    if (derived == state.currentScreen)
      return;

    if (!state.navBackStack.empty() && state.navBackStack.back() == derived) {
      state.navBackStack.pop_back();
      state.navForwardStack.push_back(state.currentScreen);
    } else {
      state.navBackStack.push_back(state.currentScreen);
      state.navForwardStack.clear();
    }
    state.currentScreen = derived;
  }

  bool runLauncher(LauncherState& state) {
    static bool loggedLauncherFrame = false;
#ifdef STAR_SYSTEM_SWITCH
    // Test autopilot: skip the launcher entirely when the sentinel file exists.
    // Used only for automated performance testing; absent for normal users.
    static bool s_autoLaunched = false;
    if (!s_autoLaunched && File::isFile("/switch/oSBM/autopilot.flag")) {
      s_autoLaunched = true;
      return true;
    }
#endif
    // Opt-in auto-launch: when STAR_LAUNCHER_AUTOLAUNCH is set in the
    // environment and a valid packed.pak is already resolved, skip the
    // launcher and boot straight into the game. Off unless explicitly set, so
    // normal users always see the launcher; handy for desktop power users who
    // want a one-step start and for automated testing.
    {
      static bool s_envAutoLaunched = false;
      if (!s_envAutoLaunched && getenv("STAR_LAUNCHER_AUTOLAUNCH")
          && !state.packedPakPath.empty() && File::isFile(state.packedPakPath)) {
        s_envAutoLaunched = true;
        return true;
      }
    }
    syncWindowMetrics(false);
    processWindowEvents();
    syncWindowMetrics(false);
    applyLauncherPendingNavigation(state);
    applyLauncherUiConfig(state.uiConfig);

    if (!loggedLauncherFrame) {
      androidLogInfo("runLauncher: uiSettingsOpen=%d modManagerOpen=%d touchManagerOpen=%d locale=%s status=%s",
        state.uiSettingsOpen ? 1 : 0, state.modManagerOpen ? 1 : 0, state.touchManagerOpen ? 1 : 0,
        state.locale.utf8Ptr(), state.lastStatus.utf8Ptr());
      loggedLauncherFrame = true;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    drainAndroidImGuiEditKeys();
    ImGui::NewFrame();

    ImVec2 displaySize = imguiDisplaySize();
    setNextLauncherWindowRect(displaySize);
    ImGui::Begin(
      launcherText("launcher.title", "OpenStarbound Mobile Loader").utf8Ptr(),
      nullptr,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
    );
    beginLauncherContentArea("LauncherContent", displaySize);
    // Visible build identity: makes "which binary am I actually running?"
    // answerable at a glance when iterating over slow transfer channels.
    ImGui::TextDisabled("Build %s %s", __DATE__, __TIME__);

    auto runLauncherAction = [this, &state](String const& actionName, std::function<void()> const& fn) {
      try {
        fn();
      } catch (std::exception const& e) {
        state.lastStatus = strf("{} {}", actionName, launcherText("runtime.actionFailedSuffix", "failed."));
        state.lastError = strf("({}) {}", actionName, outputException(e, true));
        Logger::error("Launcher action '{}' failed: {}", actionName, state.lastError);
      } catch (...) {
        state.lastStatus = strf("{} {}", actionName, launcherText("runtime.actionFailedSuffix", "failed."));
        state.lastError = strf("({}) {}", actionName, launcherText("runtime.unknownFailure", "Unknown runtime failure"));
        Logger::error("Launcher action '{}' failed: unknown runtime failure", actionName);
      }
    };

    auto applyAsyncLauncherAction = [&state]() {
      LauncherActionResult result;
      bool completed = false;
      {
        std::lock_guard<std::mutex> lock(state.asyncActionMutex);
        if (state.asyncActionCompleted) {
          result = state.asyncActionResult;
          state.asyncActionResult = {};
          state.asyncActionCompleted = false;
          state.asyncActionRunning = false;
          state.asyncActionName.clear();
          completed = true;
        }
      }

      if (!completed)
        return;

      if (state.asyncActionThread.joinable())
        state.asyncActionThread.join();

      state.lastStatus = result.status;
      state.lastError = result.error;
      if (result.modListDirty)
        state.modListDirty = true;
      if (result.packedPakPath) {
        state.packedPakPath = *result.packedPakPath;
        state.canLaunch = File::isFile(state.packedPakPath);
      }
    };

    auto runLauncherActionAsync = [this, &state](String const& actionName, std::function<LauncherActionResult()> const& fn) {
      if (state.asyncActionRunning) {
        state.lastStatus = launcherText("status.nativePickerAlreadyOpen", "Native file picker is already open.");
        state.lastError = launcherText("error.finishCurrentPickerFirst", "Finish or cancel the current picker before starting another import.");
        return;
      }

      if (state.asyncActionThread.joinable())
        state.asyncActionThread.join();

      String progressName = actionName;
      if (actionName == "Import mod (.pak)")
        progressName = launcherText("status.importingModPak", "Importing mod (.pak)");
      else if (actionName == "Import mod folder (.zip)")
        progressName = launcherText("status.importingModFolderZip", "Importing mod folder (.zip)");
      else if (actionName == "Import mods folder (.zip)")
        progressName = launcherText("status.importingModsFolderZip", "Importing mods folder (.zip)");
      else if (actionName == "Import packed.pak")
        progressName = launcherText("status.importingPackedPak", "Importing packed.pak");

      {
        std::lock_guard<std::mutex> lock(state.asyncActionMutex);
        state.asyncActionRunning = true;
        state.asyncActionCompleted = false;
        state.asyncActionName = progressName;
        state.asyncActionResult = {};
      }
      state.lastStatus = strf("{}...", progressName);
      state.lastError.clear();

      state.asyncActionThread = std::thread([this, &state, actionName, fn]() {
        LauncherActionResult result;
        try {
          result = fn();
        } catch (std::exception const& e) {
          result.status = strf("{} {}", actionName, launcherText("runtime.actionFailedSuffix", "failed."));
          result.error = strf("({}) {}", actionName, outputException(e, true));
          Logger::error("Launcher action '{}' failed: {}", actionName, result.error);
        } catch (...) {
          result.status = strf("{} {}", actionName, launcherText("runtime.actionFailedSuffix", "failed."));
          result.error = strf("({}) {}", actionName, launcherText("runtime.unknownFailure", "Unknown runtime failure"));
          Logger::error("Launcher action '{}' failed: unknown runtime failure", actionName);
        }

        std::lock_guard<std::mutex> lock(state.asyncActionMutex);
        state.asyncActionResult = result;
        state.asyncActionCompleted = true;
      });
    };

    applyAsyncLauncherAction();

    auto modsPath = modsDirectoryPath();
    if (!File::isDirectory(modsPath))
      File::makeDirectoryRecursive(modsPath);

    bool launchPressed = false;
    bool mainLauncherOpen = !state.uiSettingsOpen && !state.touchManagerOpen && !state.modManagerOpen && !state.saveManagerOpen;

    if (state.uiSettingsOpen) {
      renderLauncherUiSettings(state);
    } else if (state.touchManagerOpen && state.gamepadManagerOpen) {
      renderGamepadManager(state);
    } else if (state.touchManagerOpen) {
      renderTouchManager(state);
    } else if (state.modManagerOpen) {
      if (state.modListDirty)
        refreshModList(state, modsPath);

      bool requestDeletePopup = false;
      beginLauncherScrollArea("ModManagerScroll", state);

      ImGui::TextUnformatted(launcherText("modManager.title", "Mod Manager").utf8Ptr());
      ImGui::TextWrapped("%s", launcherText("modManager.description", "Browse installed mods, import new mods, and delete entries.").utf8Ptr());
      ImGui::Separator();

      if (ImGui::Button(launcherText("common.backToLauncher", "Back to Launcher").utf8Ptr()))
        state.modManagerOpen = false;
      sameLineIfNextFits(imguiButtonWidth(launcherText("common.refreshList", "Refresh List").utf8Ptr()));
      if (ImGui::Button(launcherText("common.refreshList", "Refresh List").utf8Ptr()))
        state.modListDirty = true;

      ImGui::InputTextWithHint("##modsearch", launcherText("modManager.searchHint", "Search mods...").utf8Ptr(), state.modSearchBuffer, sizeof(state.modSearchBuffer));
      ImGui::Checkbox(launcherText("modManager.showPakMods", "Show .pak mods").utf8Ptr(), &state.modShowPackedPaks);
      sameLineIfNextFits(ImGui::CalcTextSize(launcherText("modManager.showUnpackedFolders", "Show unpacked folders").utf8Ptr()).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
      ImGui::Checkbox(launcherText("modManager.showUnpackedFolders", "Show unpacked folders").utf8Ptr(), &state.modShowUnpackedFolders);
      ImGui::TextWrapped("%s: %s", launcherText("modManager.modsDirectoryLabel", "Mods directory").utf8Ptr(), modsPath.utf8Ptr());

      renderLauncherBusyIndicator(state);
      if (state.asyncActionRunning)
        ImGui::TextWrapped("%s", launcherText("modManager.asyncBlockedHint", "Finish or cancel the native picker to continue.").utf8Ptr());
      ImGui::BeginDisabled(state.asyncActionRunning);
      if (ImGui::Button(launcherText("modManager.importPak", "Import mod (.pak)").utf8Ptr())) {
#ifdef STAR_SYSTEM_IOS
        auto svc = m_platformServices->externalFileAccessService();
        runLauncherActionAsync("Import mod (.pak)", [this, svc]() {
          if (svc) {
            auto imported = svc->importModPakFiles();
            return LauncherActionResult{
              imported.empty() ? launcherText("status.noModImported", "No mod imported.") : strf("{} {}", launcherText("status.imported", "Imported"), strf("{} mod(s)", imported.size())),
              imported.empty() ? launcherText("error.noPakSelectedOrImportFailed", "No .pak selected or import failed.") : "",
              true,
              {}
            };
          }

          return LauncherActionResult{
            launcherText("status.importUnavailable", "Import unavailable."),
            launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build."),
            false,
            {}
          };
        });
#else
        runLauncherAction("Import mod (.pak)", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto imported = svc->importModPakFiles();
            state.lastStatus = imported.empty() ? launcherText("status.noModImported", "No mod imported.") : strf("{} {}", launcherText("status.imported", "Imported"), strf("{} mod(s)", imported.size()));
            if (imported.empty())
              state.lastError = launcherText("error.noPakSelectedOrImportFailed", "No .pak selected or import failed.");
            else
              state.lastError.clear();
            state.modListDirty = true;
          } else {
            state.lastStatus = launcherText("status.importUnavailable", "Import unavailable.");
            state.lastError = launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build.");
          }
        });
#endif
      }
      if (ImGui::Button(
#ifdef STAR_SYSTEM_IOS
            launcherText("modManager.importSingleZip", "Import mod folder (.zip)").utf8Ptr()
#else
            launcherText("modManager.importSingleFolder", "Import mod folder (single mod)").utf8Ptr()
#endif
          )) {
#ifdef STAR_SYSTEM_IOS
        auto svc = m_platformServices->externalFileAccessService();
        runLauncherActionAsync("Import mod folder (.zip)", [this, svc]() {
          if (svc) {
            auto imported = svc->importSingleModFolder();
            return LauncherActionResult{
              imported.empty() ? launcherText("status.noSingleModZipImported", "No mod folder zip imported.") : strf("{} {}", launcherText("status.imported", "Imported"), strf("{} mod(s)", imported.size())),
              imported.empty() ? launcherText("error.noZipSelectedOrImportFailed", "No .zip selected or import failed.") : "",
              true,
              {}
            };
          }

          return LauncherActionResult{
            launcherText("status.importUnavailable", "Import unavailable."),
            launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build."),
            false,
            {}
          };
        });
#else
        runLauncherAction("Import mod folder", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto imported = svc->importSingleModFolder();
            state.lastStatus = imported.empty() ? launcherText("status.noSingleModFolderImported", "No mod folder imported.") : strf("{} {}", launcherText("status.imported", "Imported"), strf("{} mod(s)", imported.size()));
            if (imported.empty())
              state.lastError = launcherText("error.noFolderSelectedOrImportFailed", "No folder selected or import failed.");
            else
              state.lastError.clear();
            state.modListDirty = true;
          } else {
            state.lastStatus = launcherText("status.importUnavailable", "Import unavailable.");
            state.lastError = launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build.");
          }
        });
#endif
      }
      if (ImGui::Button(
#ifdef STAR_SYSTEM_IOS
            launcherText("modManager.importAllZip", "Import mods folder (.zip)").utf8Ptr()
#else
            launcherText("modManager.importAllFolder", "Import mods folder (all .pak and folders)").utf8Ptr()
#endif
          )) {
#ifdef STAR_SYSTEM_IOS
        auto svc = m_platformServices->externalFileAccessService();
        runLauncherActionAsync("Import mods folder (.zip)", [this, svc]() {
          if (svc) {
            auto imported = svc->importModsDirectory();
            return LauncherActionResult{
              imported.empty() ? launcherText("status.noModsZipImported", "No mods zip imported.") : strf("{} {}", launcherText("status.imported", "Imported"), strf("{} mod(s)", imported.size())),
              imported.empty() ? launcherText("error.noZipSelectedOrNoValidMods", "No .zip selected or no valid mods found.") : "",
              true,
              {}
            };
          }

          return LauncherActionResult{
            launcherText("status.importUnavailable", "Import unavailable."),
            launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build."),
            false,
            {}
          };
        });
#else
        runLauncherAction("Import mods folder", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto imported = svc->importModsDirectory();
            state.lastStatus = imported.empty() ? launcherText("status.noModsImported", "No mods imported.") : strf("{} {}", launcherText("status.imported", "Imported"), strf("{} mod(s)", imported.size()));
            if (imported.empty())
              state.lastError = launcherText("error.noFolderSelectedOrNoValidMods", "No folder selected or no valid mods found.");
            else
              state.lastError.clear();
            state.modListDirty = true;
          } else {
            state.lastStatus = launcherText("status.importUnavailable", "Import unavailable.");
            state.lastError = launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build.");
          }
        });
#endif
      }
      ImGui::EndDisabled();

      ImGui::Separator();
      String modSearch = String(state.modSearchBuffer).trim();
      size_t shownMods = 0;
      ImGui::Text("%s: %u", launcherText("modManager.installedModsLabel", "Installed mods").utf8Ptr(), (unsigned)state.modEntries.size());
      float modListHeight = std::max(180.0f, ImGui::GetContentRegionAvail().y - 90.0f);
      if (ImGui::BeginChild("ModManagerList", ImVec2(0.0f, modListHeight), true, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
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
          sameLineIfNextFits(ImGui::CalcTextSize(mod.isDirectory ? "[folder]" : "[.pak]").x);
          ImGui::TextDisabled("[%s]", mod.isDirectory ? launcherText("modManager.typeFolder", "folder").utf8Ptr() : ".pak");
          sameLineIfNextFits(imguiButtonWidth(launcherText("common.delete", "Delete").utf8Ptr(), ImVec2(90.0f, 0.0f)));
          if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + 90.0f <= ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x)
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 110.0f));
          if (ImGui::Button(launcherText("common.delete", "Delete").utf8Ptr(), ImVec2(90.0f, 0.0f))) {
            state.pendingDeletePath = mod.path;
            state.pendingDeleteName = mod.displayName;
            state.pendingDeleteIsDirectory = mod.isDirectory;
            requestDeletePopup = true;
          }
          ImGui::PopID();
        }

        if (shownMods == 0) {
          if (!modSearch.empty())
            ImGui::TextDisabled("%s", launcherText("modManager.noSearchMatches", "No mods match your search.").utf8Ptr());
          else
            ImGui::TextDisabled("%s", launcherText("modManager.noInstalledMods", "No mods are currently installed.").utf8Ptr());
        }
      }
      ImGui::EndChild();
      ImGui::EndChild();

      if (requestDeletePopup)
        ImGui::OpenPopup(launcherText("modManager.deletePopupTitle", "Delete Mod?").utf8Ptr());

      if (ImGui::BeginPopupModal(launcherText("modManager.deletePopupTitle", "Delete Mod?").utf8Ptr(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", launcherText("modManager.deleteConfirm", "Delete mod '{name}'?").replace("{name}", state.pendingDeleteName).utf8Ptr());
        ImGui::TextWrapped("%s", launcherText("modManager.deleteWarning", "This removes it from the game's internal mods directory.").utf8Ptr());
        if (ImGui::Button(launcherText("common.delete", "Delete").utf8Ptr(), ImVec2(140.0f, 0.0f))) {
          runLauncherAction("Delete mod", [&]() {
            if (!state.pendingDeletePath.empty()) {
              if (state.pendingDeleteIsDirectory)
                File::removeDirectoryRecursive(state.pendingDeletePath);
              else
                File::remove(state.pendingDeletePath);
              state.lastStatus = launcherText("modManager.deletedMod", "Deleted mod '{name}'.").replace("{name}", state.pendingDeleteName);
              state.lastError.clear();
              state.modListDirty = true;
            }
          });
          state.pendingDeletePath.clear();
          state.pendingDeleteName.clear();
          state.pendingDeleteIsDirectory = false;
          ImGui::CloseCurrentPopup();
        }
        sameLineIfNextFits(imguiButtonWidth(launcherText("common.cancel", "Cancel").utf8Ptr(), ImVec2(140.0f, 0.0f)));
        if (ImGui::Button(launcherText("common.cancel", "Cancel").utf8Ptr(), ImVec2(140.0f, 0.0f))) {
          state.pendingDeletePath.clear();
          state.pendingDeleteName.clear();
          state.pendingDeleteIsDirectory = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    } else if (state.saveManagerOpen) {
      beginLauncherScrollArea("SaveManagerScroll", state);

      ImGui::TextUnformatted(launcherText("saveManager.title", "Save Manager").utf8Ptr());
      ImGui::TextWrapped("%s", launcherText("saveManager.description", "Copy, import, and export player and universe save data.").utf8Ptr());
      ImGui::Separator();

      if (ImGui::Button(launcherText("common.backToLauncher", "Back to Launcher").utf8Ptr()))
        state.saveManagerOpen = false;

      ImGui::TextWrapped("%s: %s", launcherText("saveManager.locationLabel", "Save location").utf8Ptr(), m_storageRoot.utf8Ptr());
      ImGui::TextWrapped("%s", launcherText("saveManager.exportNote", "Exports include player and universe save data only. Assets and mods are not included.").utf8Ptr());

      if (ImGui::Button(launcherText("saveManager.copyPath", "Copy Save Path").utf8Ptr())) {
        runLauncherAction("Copy save path", [&]() {
          if (SDL_SetClipboardText(m_storageRoot.utf8Ptr())) {
            state.lastStatus = launcherText("status.savePathCopied", "Save location copied to clipboard!");
            state.lastError.clear();
          } else {
            state.lastStatus = launcherText("status.copySavePathUnavailable", "Copy save path unavailable.");
            state.lastError = launcherText("error.copySavePathFailed", "Could not copy the save location to the clipboard.");
          }
        });
      }

      if (ImGui::Button(launcherText("saveManager.importZip", "Import Save Zip").utf8Ptr())) {
        runLauncherAction("Import save zip", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            if (svc->importSaveZip()) {
              state.lastStatus = launcherText("status.saveZipImportFinished", "Save zip import finished.");
              state.lastError.clear();
            } else {
              state.lastStatus = launcherText("status.noSaveZipImported", "No save zip imported.");
              state.lastError = launcherText("error.invalidSaveZipOrImportFailed", "No .zip selected, invalid save archive, or import failed.");
            }
          } else {
            state.lastStatus = launcherText("status.importUnavailable", "Import unavailable.");
            state.lastError = launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build.");
          }
        });
      }
      sameLineIfNextFits(imguiButtonWidth(launcherText("saveManager.exportZip", "Export Save Zip").utf8Ptr()));
      if (ImGui::Button(launcherText("saveManager.exportZip", "Export Save Zip").utf8Ptr())) {
        runLauncherAction("Export save zip", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            if (svc->exportSaveZip()) {
              state.lastStatus = launcherText("status.saveExportOpened", "Save export opened.");
              state.lastError.clear();
            } else {
              state.lastStatus = launcherText("status.saveExportUnavailable", "Save export unavailable.");
              state.lastError = launcherText("error.noSaveDataOrShareUnavailable", "No save data found or the native share sheet could not be opened.");
            }
          } else {
            state.lastStatus = launcherText("status.exportUnavailable", "Export unavailable.");
            state.lastError = launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build.");
          }
        });
      }
      ImGui::EndChild();
    } else {
      beginLauncherScrollArea("LauncherMainScroll", state, false);

      ImGui::TextWrapped("%s", launcherText("launcher.configureHint", "Configure assets and controls before launching.").utf8Ptr());
      ImGui::Separator();

      ImGui::Text("%s: %s", launcherText("launcher.packedPakLabel", "packed.pak").utf8Ptr(), state.packedPakPath.empty() ? launcherText("launcher.notSelected", "<not selected>").utf8Ptr() : state.packedPakPath.utf8Ptr());
#ifdef STAR_SYSTEM_SWITCH
      // Switch builds don't embed packed.pak and have no native file picker
      // (see the SD-card fallback check in loadLauncherState), so this is the
      // only way a user finds out where to put the file. Path is shown
      // separately from the translated sentence so a translation can't drop
      // or mangle it (no embedded {} placeholder to lose in translation).
      if (state.packedPakPath.empty()) {
        ImGui::TextWrapped("%s", launcherText("launcher.packedPakSwitchHint",
            "Copy packed.pak to this path on the SD card, then restart oSBM:").utf8Ptr());
        ImGui::TextWrapped("%s", File::relativeTo(m_storageRoot, "assets/packed.pak").utf8Ptr());
      }
#elif !defined(STAR_SYSTEM_ANDROID) && !defined(STAR_SYSTEM_IOS)
      // Desktop launcher: the in-app picker isn't wired to a native dialog, so
      // guide the user to drop the file at the auto-detected location (see the
      // desktop fallback in loadLauncherState).
      if (state.packedPakPath.empty()) {
        ImGui::TextWrapped("%s", launcherText("launcher.packedPakDesktopHint",
            "Copy packed.pak to this path, then relaunch (or use Pick packed.pak):").utf8Ptr());
        ImGui::TextWrapped("%s", File::relativeTo(m_storageRoot, "assets/packed.pak").utf8Ptr());
      }
#endif
      renderLauncherBusyIndicator(state);

      ImGui::BeginDisabled(state.asyncActionRunning);
      if (ImGui::Button(launcherText("launcher.pickPackedPak", "Pick packed.pak").utf8Ptr())) {
#ifdef STAR_SYSTEM_IOS
        auto svc = m_platformServices->externalFileAccessService();
        // [this] for launcherText() -- same worker-thread capture pattern as
        // the mod-import async actions above (translations are immutable
        // while the launcher runs).
        runLauncherActionAsync("Import packed.pak", [this, svc]() {
          if (svc) {
            auto picked = svc->pickPackedPak();
            if (picked) {
              return LauncherActionResult{
                launcherText("status.importedPackedPak", "Imported packed.pak"),
                "",
                false,
                picked
              };
            }

            return LauncherActionResult{
              launcherText("status.noFileSelected", "No file selected."),
              launcherText("error.nativePickerUnavailableCanceled", "Native picker unavailable or canceled."),
              false,
              {}
            };
          }

          return LauncherActionResult{
            launcherText("status.nativePickerUnavailable", "Native picker unavailable."),
            launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build."),
            false,
            {}
          };
        });
#else
        runLauncherAction("Pick packed.pak", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            auto picked = svc->pickPackedPak();
            if (picked) {
              state.packedPakPath = *picked;
              state.lastStatus = launcherText("status.importedPackedPak", "Imported packed.pak");
              state.lastError.clear();
            } else {
              state.lastStatus = launcherText("status.noFileSelected", "No file selected.");
              state.lastError = launcherText("error.nativePickerUnavailableCanceled", "Native picker unavailable or canceled.");
            }
          } else {
            state.lastStatus = launcherText("status.nativePickerUnavailable", "Native picker unavailable.");
            state.lastError = launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build.");
          }
        });
#endif
      }
      ImGui::EndDisabled();

      ImGui::Separator();
      ImGui::TextWrapped("%s: %s", launcherText("launcher.modsDirectoryLabel", "Current mods directory").utf8Ptr(), modsPath.utf8Ptr());
      if (ImGui::Button(launcherText("launcher.modManager", "Mod Manager").utf8Ptr())) {
        state.modManagerOpen = true;
        state.modListDirty = true;
      }
      sameLineIfNextFits(imguiButtonWidth(launcherText("launcher.saveManager", "Save Manager").utf8Ptr()));
      if (ImGui::Button(launcherText("launcher.saveManager", "Save Manager").utf8Ptr()))
        state.saveManagerOpen = true;
      sameLineIfNextFits(imguiButtonWidth(launcherText("launcher.uiSettings", "Settings").utf8Ptr()));
      if (ImGui::Button(launcherText("launcher.uiSettings", "Settings").utf8Ptr()))
        state.uiSettingsOpen = true;
      sameLineIfNextFits(imguiButtonWidth(launcherText("launcher.touchControls", "Controls/Overlay").utf8Ptr()));
      if (ImGui::Button(launcherText("launcher.touchControls", "Controls/Overlay").utf8Ptr())) {
        state.touchManagerOpen = true;
        state.gamepadManagerOpen = false;
        state.selectedTouchElement = std::clamp(state.selectedTouchElement, 0, (int)state.touchElements.size() - 1);
      }
      sameLineIfNextFits(imguiButtonWidth(launcherText("launcher.exportDiagnostics", "Export Diagnostics").utf8Ptr()));
      if (ImGui::Button(launcherText("launcher.exportDiagnostics", "Export Diagnostics").utf8Ptr())) {
        runLauncherAction("Export diagnostics", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            if (svc->exportDiagnostics()) {
              state.lastStatus = launcherText("status.diagnosticsExportOpened", "Diagnostics export opened.");
              state.lastError.clear();
            } else {
              state.lastStatus = launcherText("status.diagnosticsExportUnavailable", "Diagnostics export unavailable.");
              state.lastError = launcherText("error.diagnosticsShareUnavailable", "Could not open the native diagnostics share sheet.");
            }
          } else {
            state.lastStatus = launcherText("status.diagnosticsExportUnavailable", "Diagnostics export unavailable.");
            state.lastError = launcherText("error.externalFileAccessUnavailable", "ExternalFileAccessService is unavailable on this platform build.");
          }
        });
      }

      ImGui::Separator();
      ImGui::Text("%s: %s / %s", launcherText("launcher.touchControlsStatusLabel", "Controls").utf8Ptr(),
        state.touchConfig.enabled ? launcherText("common.touchOn", "touch on").utf8Ptr() : launcherText("common.touchOff", "touch off").utf8Ptr(),
        state.gamepadConfig.enabled ? launcherText("common.gamepadOn", "gamepad on").utf8Ptr() : launcherText("common.gamepadOff", "gamepad off").utf8Ptr());

      state.canLaunch = File::isFile(state.packedPakPath);
      if (!state.canLaunch)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "%s", launcherText("launcher.launchDisabledHint", "Launch is disabled until packed.pak is imported.").utf8Ptr());

      bool launchDisabled = !state.canLaunch || state.asyncActionRunning;
      if (launchDisabled)
        ImGui::BeginDisabled();
      launchPressed = ImGui::Button(launcherText("launcher.launch", "Launch").utf8Ptr());
      if (launchDisabled)
        ImGui::EndDisabled();

      bool quickStartAvailable = !m_activeGamepadName.empty();
      bool quickStartEnabled = quickStartAvailable && !launchDisabled;
      if (quickStartAvailable) {
        int64_t heldMs = m_launcherQuickStartButtonHeld ? Time::monotonicMilliseconds() - m_launcherQuickStartStartMs : 0;
        renderLauncherQuickStartPrompt((float)heldMs / (float)LauncherQuickStartHoldMs, quickStartEnabled);
        if (quickStartEnabled && heldMs >= LauncherQuickStartHoldMs)
          launchPressed = true;
      }

      renderLauncherStatusText(state);
      ImGui::EndChild();
    }

    // Fold any screen change made by the in-panel buttons this frame into the
    // navigation history so button and gesture navigation stay consistent.
    reconcileLauncherNavigation(state);

    if (!mainLauncherOpen || state.asyncActionRunning || !state.canLaunch || m_activeGamepadName.empty())
      resetLauncherQuickStart();

    if (!mainLauncherOpen)
      renderLauncherStatusText(state);

    ImGui::EndChild();
    ImGui::End();

    if (state.touchPreviewOpen)
      renderTouchPreview(state, displaySize);

    syncImGuiTextInputState();

    ImGui::Render();
    m_launcherImGuiClickConsumed = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
#ifdef STAR_SYSTEM_IOS
    glBindFramebuffer(GL_FRAMEBUFFER, m_renderer->screenFramebuffer());
#endif
    glViewport((int)m_safeArea.left, (int)m_safeArea.bottom, (int)gameCanvasSize()[0], (int)gameCanvasSize()[1]);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);

    if (launchPressed) {
      androidLogInfo("Launch pressed");
      resetLauncherQuickStart();
      persistLauncherState(state);
    }

    return launchPressed;
  }

  void persistLauncherState(LauncherState const& state) {
    Json config = JsonObject{
      {"packedPakPath", state.packedPakPath},
      {"launcherUi", JsonObject{
        {"scale", state.uiConfig.scale},
        {"locale", state.locale},
        {"accentColor", JsonArray{
          state.uiConfig.accentColor[0],
          state.uiConfig.accentColor[1],
          state.uiConfig.accentColor[2]
        }}
      }},
      {"touch", JsonObject{
        {"enabled", state.touchConfig.enabled},
        {"directTouchGestures", state.touchConfig.directTouchGestures},
        {"directTouchGestureMode", directTouchGestureModeName(state.touchConfig.directTouchGestureMode)},
        {"directTouchSingleAction", jsonFromTouchAction(state.touchConfig.directTouchSingleAction)},
        {"directTouchTwoFingerAction", jsonFromTouchAction(state.touchConfig.directTouchTwoFingerAction)},
        {"joystickAimCursorEnabled", state.touchConfig.joystickAimCursorEnabled},
        {"gyroEnabled", state.touchConfig.gyroEnabled},
        {"opacity", state.touchConfig.opacity},
        {"size", state.touchConfig.size},
        {"outlineThickness", state.touchConfig.outlineThickness},
        {"deadzone", state.touchConfig.deadzone},
        {"gyroSensitivity", state.touchConfig.gyroSensitivity},
        {"gyroInvertX", state.touchConfig.gyroInvertX},
        {"gyroInvertY", state.touchConfig.gyroInvertY},
        {"elements", jsonFromTouchElements(state.touchElements)}
      }},
      {"performance", JsonObject{
        {"simRate", m_perfSimRate},
        {"fpsCap", m_perfFpsCap}
      }},
      {"display", JsonObject{
        {"fullscreenRender", m_fullscreenRender}
      }},
      {"gamepad", JsonObject{
        {"enabled", state.gamepadConfig.enabled},
        {"triggerThreshold", state.gamepadConfig.triggerThreshold},
        {"leftStick", jsonFromGamepadStick(state.gamepadConfig.leftStick)},
        {"rightStick", jsonFromGamepadStick(state.gamepadConfig.rightStick)},
        {"bindings", jsonFromGamepadBindings(state.gamepadBindings)}
      }}
    };

    if (auto configService = m_platformServices->launchConfigService())
      configService->saveLauncherConfig(config);
  }

  bool prepareBootConfig(LauncherState const& state, String& errorMessage) {
    if (!File::isFile(state.packedPakPath)) {
      errorMessage = launcherText("runtime.missingPackedPak", "Selected packed.pak was not found. Please re-select it.");
      return false;
    }

    auto modsPath = modsDirectoryPath();
    File::makeDirectoryRecursive(modsPath);

    String bundledAssetsRoot;
#if STAR_SYSTEM_ANDROID
    String bundledStorageRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    if (auto synced = AndroidFileAccessBridge::syncBundledAssets(bundledStorageRoot)) {
      bundledAssetsRoot = *synced;
    } else {
      errorMessage = launcherText("runtime.failedBundledAssetsApp", "Failed to load bundled OpenStarbound assets from the app package.");
      return false;
    }
#elif STAR_SYSTEM_IOS
    String bundledStorageRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    if (auto synced = IosFileAccessBridge::syncBundledAssets(bundledStorageRoot)) {
      bundledAssetsRoot = *synced;
    } else {
      errorMessage = launcherText("runtime.failedBundledAssetsIos", "Failed to load bundled OpenStarbound assets from the iOS app package.");
      return false;
    }
#elif STAR_SYSTEM_SWITCH
    switchSyncBundledAssets(m_storageRoot);
    bundledAssetsRoot = File::relativeTo(m_storageRoot, "bundled_assets");
    if (!File::isDirectory(bundledAssetsRoot)) {
      errorMessage = launcherText("runtime.failedBundledAssetsSwitch", "Failed to load bundled OpenStarbound assets from romfs.");
      return false;
    }
#else
    if (auto basePath = SDL_GetBasePath())
      bundledAssetsRoot = File::convertDirSeparators(File::relativeTo(basePath, "../assets"));
    else
      bundledAssetsRoot = "../assets";
#endif

    unsigned hwThreads = std::max(1u, std::thread::hardware_concurrency());
    unsigned workerPoolSize = std::min(hwThreads, 2u);

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
      {"controllerInput", false},
      {"defaultConfiguration", JsonObject{
        {"controllerInput", false},
        {"mobile", JsonObject{
          {"touchControls", JsonObject{
            {"enabled", state.touchConfig.enabled},
            {"directTouchGestures", state.touchConfig.directTouchGestures},
            {"directTouchGestureMode", directTouchGestureModeName(state.touchConfig.directTouchGestureMode)},
            {"directTouchSingleAction", jsonFromTouchAction(state.touchConfig.directTouchSingleAction)},
            {"directTouchTwoFingerAction", jsonFromTouchAction(state.touchConfig.directTouchTwoFingerAction)},
            {"joystickAimCursorEnabled", state.touchConfig.joystickAimCursorEnabled},
            {"gyroEnabled", state.touchConfig.gyroEnabled},
            {"opacity", state.touchConfig.opacity},
            {"size", state.touchConfig.size},
            {"outlineThickness", state.touchConfig.outlineThickness},
            {"deadzone", state.touchConfig.deadzone},
            {"gyroSensitivity", state.touchConfig.gyroSensitivity},
            {"gyroInvertX", state.touchConfig.gyroInvertX},
            {"gyroInvertY", state.touchConfig.gyroInvertY},
            {"elements", jsonFromTouchElements(state.touchElements)},
            {"invertLook", false}
          }},
          {"gamepadControls", JsonObject{
            {"enabled", state.gamepadConfig.enabled},
            {"triggerThreshold", state.gamepadConfig.triggerThreshold},
            {"leftStick", jsonFromGamepadStick(state.gamepadConfig.leftStick)},
            {"rightStick", jsonFromGamepadStick(state.gamepadConfig.rightStick)},
            {"bindings", jsonFromGamepadBindings(state.gamepadBindings)}
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
      relaunchBreadcrumb("start-application-enter");
      androidLogInfo("startApplication begin");
      m_runtimeArgs.clear();
      m_runtimeArgs.append("-bootconfig");
      m_runtimeArgs.append(m_bootConfigPath);

      auto startupDone = make_shared<std::atomic<bool>>(false);
      auto startupSucceeded = make_shared<std::atomic<bool>>(false);
      auto startupError = make_shared<String>();
      auto startupErrorMutex = make_shared<Mutex>();
      auto startupStatus = make_shared<String>(launcherText("startup.preparing", "Preparing startup..."));
      auto startupStatusMutex = make_shared<Mutex>();

      auto startupThread = Thread::invoke("Mobile::ClientStartup", [this, startupDone, startupSucceeded, startupError, startupErrorMutex, startupStatus, startupStatusMutex]() {
          try {
            {
              MutexLocker locker(*startupStatusMutex);
              *startupStatus = launcherText("startup.loadingAssets", "Loading game assets and configuration...");
            }
            setMobileStartupStatus(launcherText("startup.loadingAssets", "Loading game assets and configuration..."));
            relaunchBreadcrumb("worker-startup-begin");
            androidLogInfo("startApplication(worker): application->startup begin");
            m_application->startup(m_runtimeArgs);
            relaunchBreadcrumb("worker-startup-done");
            androidLogInfo("startApplication(worker): application->startup done");
            {
              MutexLocker locker(*startupStatusMutex);
              *startupStatus = launcherText("startup.initSystems", "Startup completed. Initializing game systems...");
            }
            startupSucceeded->store(true, std::memory_order_release);
          } catch (std::exception const& e) {
            MutexLocker locker(*startupErrorMutex);
            *startupError = strf("{}", outputException(e, true));
          } catch (...) {
            MutexLocker locker(*startupErrorMutex);
            *startupError = launcherText("startup.failedUnknown", "Unknown startup failure");
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
        errorMessage = launcherText("startup.canceled", "Launch canceled.");
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
      setMobileStartupStatus(launcherText("startup.initRenderer", "Initializing renderer..."));
      renderStartupScreen(getMobileStartupStatus());
      androidLogInfo("startApplication: renderInit");
      m_application->renderInit(m_renderer);
      setMobileStartupStatus("Renderer initialized...");
      renderStartupScreen(getMobileStartupStatus());
      setMobileStartupStatus(launcherText("startup.initTouch", "Initializing touch controls..."));
      renderStartupScreen(getMobileStartupStatus());
      androidLogInfo("startApplication: create touch adapter");
      m_touchAdapter = make_unique<MobileTouchInputAdapter>(&m_windowSize, &m_renderCanvasSize, &m_displayScale, &m_safeArea);
      m_touchAdapter->setGyroAvailable(platformGyroAvailable());
      m_gamepadAdapter = make_unique<MobileGamepadInputAdapter>(&m_renderCanvasSize);

      if (auto configService = m_platformServices->launchConfigService()) {
        auto cfg = configService->loadLauncherConfig();
        MobileTouchConfig touch;
        // Defaults must match loadLauncherState (see the same #if there): the
        // launcher persists these into the config this reads back.
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
        touch.enabled = cfg.queryBool("touch.enabled", true);
        touch.directTouchGestures = cfg.queryBool("touch.directTouchGestures", true);
#elif defined(STAR_SYSTEM_SWITCH)
        touch.enabled = cfg.queryBool("touch.enabled", false);
        touch.directTouchGestures = cfg.queryBool("touch.directTouchGestures", true);
#else
        touch.enabled = cfg.queryBool("touch.enabled", false);
        touch.directTouchGestures = cfg.queryBool("touch.directTouchGestures", false);
#endif
        touch.directTouchGestureMode = directTouchGestureModeFromName(cfg.queryString("touch.directTouchGestureMode", "touchscreen"));
        touch.directTouchSingleAction = touchActionFromJson(
            cfg.query("touch.directTouchSingleAction", jsonFromTouchAction(touch.directTouchSingleAction)), touch.directTouchSingleAction);
        touch.directTouchTwoFingerAction = touchActionFromJson(
            cfg.query("touch.directTouchTwoFingerAction", jsonFromTouchAction(touch.directTouchTwoFingerAction)), touch.directTouchTwoFingerAction);
        touch.joystickAimCursorEnabled = cfg.queryBool("touch.joystickAimCursorEnabled", true);
        touch.gyroEnabled = cfg.queryBool("touch.gyroEnabled", false);
        if (!platformGyroAvailable())
          touch.gyroEnabled = false;
        touch.opacity = cfg.queryFloat("touch.opacity", 0.35f);
        touch.size = cfg.queryFloat("touch.size", 1.0f);
        touch.outlineThickness = cfg.queryFloat("touch.outlineThickness", 1.5f);
        touch.deadzone = cfg.queryFloat("touch.deadzone", 0.15f);
        touch.gyroSensitivity = cfg.queryFloat("touch.gyroSensitivity", 1.0f);
        touch.gyroInvertX = cfg.queryBool("touch.gyroInvertX", false);
        touch.gyroInvertY = cfg.queryBool("touch.gyroInvertY", false);
        m_touchAdapter->setConfig(touch);
        m_touchAdapter->setElements(touchElementsFromConfig(cfg));
        syncGyroSensor(touch.gyroEnabled);
        MobileGamepadConfig gamepadConfig = gamepadConfigFromConfig(cfg);
        // touch is the single source of truth for this setting -- see
        // MobileTouchConfig::joystickAimCursorEnabled.
        gamepadConfig.joystickAimCursorEnabled = touch.joystickAimCursorEnabled;
        m_gamepadAdapter->setConfig(gamepadConfig);
        m_gamepadAdapter->setBindings(gamepadBindingsFromConfig(cfg));
      }

      if (m_softQuitRequested || m_quitRequested) {
        m_softQuitRequested = false;
        m_quitRequested = false;
        errorMessage = launcherText("runtime.initRequestedQuit", "Game initialization requested quit.");
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
      errorMessage = launcherText("startup.failedUnknown", "Unknown startup failure");
      Logger::error("Failed starting mobile application: {}", errorMessage);
      return false;
    }
  }

  void renderStartupScreen(String const& status) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    drainAndroidImGuiEditKeys();
    ImGui::NewFrame();

    ImVec2 displaySize = imguiDisplaySize();
    setNextLauncherWindowRect(displaySize);
    ImGui::Begin(
      launcherText("launcher.title", "OpenStarbound Mobile Loader").utf8Ptr(),
      nullptr,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
    );
    beginLauncherContentArea("StartupContent", displaySize, true);
    ImGui::TextWrapped("%s", status.utf8Ptr());
    ImGui::Separator();
    ImGui::TextWrapped("%s", launcherText("launcher.loadingHint", "Please wait. Assets and configuration are loading.").utf8Ptr());
    ImGui::EndChild();
    ImGui::End();

    ImGui::Render();
    setAndroidImGuiTextInputActive(false);
#ifdef STAR_SYSTEM_IOS
    glBindFramebuffer(GL_FRAMEBUFFER, m_renderer->screenFramebuffer());
#endif
    glViewport((int)m_safeArea.left, (int)m_safeArea.bottom, (int)gameCanvasSize()[0], (int)gameCanvasSize()[1]);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);
  }

  void applyPerformanceSettings() {
    // Launcher-selected sim rate and FPS cap win over starbound.config pacing
    // keys on the mobile family (the launcher UI is the settings surface the
    // user actually sees). Re-asserted whenever they drift so the application
    // layer's own config-driven startup can't override them mid-boot.
    // rate * timestep = 1.0, so gameplay speed is unchanged at either rate.
    float simRate = (float)m_perfSimRate;
    if (GlobalTimestep != 1.0f / simRate) {
      GlobalTimestep = 1.0f / simRate;
      ServerGlobalTimestep = 1.0f / simRate;
      m_updateTicker.setTargetTickRate(simRate);
    }
    if (m_maxFrameRate != (float)m_perfFpsCap) {
      m_maxFrameRate = (float)m_perfFpsCap;
      if (m_maxFrameRate > 0.0f)
        m_framePacer.setTargetTickRate(m_maxFrameRate);
    }
  }

  void runGameLoop() {
    applyPerformanceSettings();
    m_updateTicker.reset();
    m_renderTicker.reset();
    m_framePacer.reset();

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    // Notify the game of the canvas size before the first frame.  syncWindowMetrics
    // won't fire windowChanged when the safe area is already stable from
    // setupWindowAndRenderer, so the game would otherwise use the full physical
    // screen size for UI layout while the shader clips to the smaller canvas.
    if (m_application)
      m_application->windowChanged(WindowMode::Normal, m_renderCanvasSize);
#endif

#if !defined(STAR_SYSTEM_ANDROID) && !defined(STAR_SYSTEM_IOS) && !defined(STAR_SYSTEM_SWITCH)
    // Desktop: the game renders its own in-world cursor, so hide the OS pointer
    // while in-game -- otherwise the real cursor and the game cursor stack on
    // top of each other. Restored when control returns to the ImGui launcher
    // (which drives its UI with the OS pointer). See the SDL_ShowCursor in run().
    SDL_HideCursor();
#endif

    while (!m_quitRequested && !m_softQuitRequested) {
#ifdef STAR_SYSTEM_SWITCH
      // Outer-loop phase breakdown, logged every ~150 frames; complements the
      // in-render pipeline laps by accounting for events/update/GL frame
      // bracketing/swap/sleep time that those cannot see.
      static int64_t s_lpFrames = 0, s_lpEvents = 0, s_lpUpdate = 0, s_lpStart = 0, s_lpRender = 0, s_lpFinish = 0, s_lpSwap = 0, s_lpSleep = 0;
      int64_t lpLapTime = Time::monotonicMicroseconds();
      auto lpLap = [&lpLapTime](int64_t& acc) {
        int64_t n = Time::monotonicMicroseconds();
        acc += n - lpLapTime;
        lpLapTime = n;
      };
#endif
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
        drainAndroidImGuiEditKeys();
      } else {
        setAndroidImGuiTextInputActive(false);
      }

      // Re-assert launcher pacing settings if the application layer's own
      // config-driven startup overrode them (cheap no-op compare otherwise).
      applyPerformanceSettings();
      // The render rate is decoupled from the update rate: frames may run
      // zero updates (re-rendering the latest state when rendering faster
      // than the sim rate) or several (catch-up after a slow frame). The
      // tick-backlog forgiveness below prevents catch-up death spirals; the
      // old hard 1-update/frame cap is gone (a sustained sub-60 frame rate
      // at a 60Hz sim would otherwise run the game in permanent slow motion).
      // Fixed-timestep accumulator: the tick count and the interpolation
      // fraction both come from m_tickAccum, so the render alpha cannot
      // drift out of phase with tick execution. (The previous scheme
      // measured alpha against a wall-clock tick schedule while ticks were
      // scheduled by the windowed-rate ticker; the two clocks disagree
      // slightly, the schedule ratchets behind real time, and alpha sits
      // clamped at 1.0 -- interpolation silently off. Observed on Switch
      // hardware as persistent walk jitter with alpha=[1.00,1.00].)
      double loopNow = Time::monotonicTime();
      double tickRate = m_updateTicker.targetTickRate();
      if (m_lastLoopTime > 0.0 && loopNow > m_lastLoopTime) {
        // Stall forgiveness: write off anything beyond a quarter second so
        // a world load or app pause doesn't trigger a fast-forward burst.
        m_tickAccum += std::min(loopNow - m_lastLoopTime, 0.25) * tickRate;
      }
      m_lastLoopTime = loopNow;
      int updatesBehind = std::min<int>((int)m_tickAccum, (int)m_maxFrameSkip + 1);
#ifdef STAR_SYSTEM_SWITCH
      lpLap(s_lpEvents);
#endif
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
      m_tickAccum -= updatesBehind;
      // If the frame-skip cap truncated a large backlog, drop the excess
      // instead of carrying slow-motion debt into future frames.
      if (m_tickAccum > m_maxFrameSkip + 1)
        m_tickAccum = 1.0;
      // Render-only frame: ImGui still needs exactly one open frame.
      if (overlayEnabled && updatesBehind == 0)
        ImGui::NewFrame();
#ifdef STAR_SYSTEM_SWITCH
      lpLap(s_lpUpdate);
#endif
      m_renderer->startFrame();
#ifdef STAR_SYSTEM_SWITCH
      lpLap(s_lpStart);
#endif
      m_application->render();
#ifdef STAR_SYSTEM_SWITCH
      lpLap(s_lpRender);
#endif

      if (overlayEnabled) {
        m_touchAdapter->drawOverlay(m_renderRate);
        ImGui::Render();
        setAndroidImGuiTextInputActive(ImGui::GetIO().WantTextInput);
      }

      m_renderer->finishFrame();
#ifdef STAR_SYSTEM_SWITCH
      lpLap(s_lpFinish);
#endif
      if (overlayEnabled)
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      SDL_GL_SwapWindow(m_window);
#ifdef STAR_SYSTEM_SWITCH
      lpLap(s_lpSwap);
#endif

      m_renderRate = m_renderTicker.tick();

      if (m_signalHandler.interruptCaught()) {
        Logger::info("QUIT PATH: signal handler interrupt");
        m_quitRequested = true;
      }

      // Frame pacing: with a cap, sleep to the next frame slot (updates run
      // on their own schedule within whatever frames occur). Uncapped, the
      // loop free-runs and the platform's swap behavior is the only limiter.
#ifdef STAR_SYSTEM_SWITCH
      // The display owns the cadence here: Switch swaps at vsync (interval 1,
      // set explicitly in setupWindowAndRenderer via SDL_GL_SetSwapInterval),
      // which genuinely blocks SwapWindow at the panel rate. A software pacer
      // on top does not throttle further, it only phase-jitters against the
      // present clock. Below the panel rate the pacer still runs, so a low
      // cap (e.g. 30) still throttles -- swap won't block under the refresh
      // rate.
      bool presentPaced = m_maxFrameRate >= 59.0f;
#else
      // iOS was included here for one session under the (wrong) assumption
      // that SDL_GL_SwapWindow blocks to the display on iOS the way it does
      // on Switch. It does not: UIKit_GL_SwapWindow just calls
      // [EAGLContext presentRenderbuffer:], which is fire-and-forget with no
      // vsync/CADisplayLink wait, and we never call SDL_GL_SetSwapInterval
      // for iOS. With presentPaced=true there the loop free-ran, uncapped and
      // unpaced -- worse jitter than before, at any sim/fps combo, not just
      // high caps (measured on-device: "FPS not a clean number", persists
      // regardless of config). Real vsync-locked pacing on iOS needs a
      // CADisplayLink hooked into the run loop (a bigger, untested change) --
      // until that lands, iOS uses the same software pacer as Android, which
      // measurably keeps ticks caught up (gap=0) and produces small, smooth
      // interpolation offsets during actual movement (verified on a real
      // Android device at the Outpost).
      constexpr bool presentPaced = false;
#endif
      if (m_maxFrameRate > 0.0f && !presentPaced) {
        m_framePacer.tick();
        double frameSpare = m_framePacer.spareTime();
        if (frameSpare < -0.25) {
          m_framePacer.reset();
          frameSpare = 0.0;
        }
        int64_t spare = round(frameSpare * 1000.0);
        if (spare > 0)
          Thread::sleepPrecise((unsigned)std::min<int64_t>(spare, 1000));
      } else {
        // Uncapped: never sleep, but yield once per frame so same-core
        // threads (audio, driver callbacks, workers) aren't starved by a
        // loop that otherwise never leaves the run queue.
        Thread::yield();
      }
#ifdef STAR_SYSTEM_SWITCH
      if (m_quitRequested || m_softQuitRequested)
        Logger::info("GAME LOOP EXITING: quitRequested={} softQuitRequested={}", m_quitRequested, m_softQuitRequested);
      lpLap(s_lpSleep);
      if (++s_lpFrames >= 150) {
        Logger::info("[perf-loop] events={:.1f}ms update={:.1f} start={:.1f} render={:.1f} finish={:.1f} swap={:.1f} sleep={:.1f}",
            s_lpEvents / 1e3 / s_lpFrames, s_lpUpdate / 1e3 / s_lpFrames,
            s_lpStart / 1e3 / s_lpFrames, s_lpRender / 1e3 / s_lpFrames,
            s_lpFinish / 1e3 / s_lpFrames, s_lpSwap / 1e3 / s_lpFrames, s_lpSleep / 1e3 / s_lpFrames);
        s_lpFrames = 0;
        s_lpEvents = s_lpUpdate = s_lpStart = s_lpRender = s_lpFinish = s_lpSwap = s_lpSleep = 0;
      }
#endif
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
    syncGyroSensor(false);
    if (m_gamepadAdapter)
      m_gamepadAdapter->cancelAll();
    // Do not reset m_application: startup() can be called again for a new
    // session without rebuilding the entire object.
  }

  Vec2F externalMouseInputPosition(float x, float y) const {
    // External mice report window POINTS, while the game canvas / safe area /
    // render canvas are all in framebuffer PIXELS. Scale points -> pixels
    // first (1:1 on non-HiDPI); without this the cursor lands offset toward the
    // origin on HiDPI displays. Touch input takes the touch adapter's own
    // conversion path instead.
    x *= m_windowPointToPixel[0];
    y *= m_windowPointToPixel[1];
    Vec2U physicalCanvas = gameCanvasSize();
    Vec2U renderCanvas = m_renderCanvasSize;
    Vec2F physical{
      x - (float)m_safeArea.left,
      y - (float)m_safeArea.top
    };
    Vec2F logical{
      physicalCanvas[0] ? physical[0] * (float)renderCanvas[0] / (float)physicalCanvas[0] : physical[0],
      physicalCanvas[1] ? physical[1] * (float)renderCanvas[1] / (float)physicalCanvas[1] : physical[1]
    };
    return {
      logical[0],
      (float)renderCanvas[1] - logical[1]
    };
  }

  Vec2F legacyWindowMouseInputPosition(float x, float y) const {
    return {x, (float)m_windowSize[1] - y};
  }

  Vec2F mouseInputPosition(float x, float y, bool touchDerivedMouse) const {
    return touchDerivedMouse ? legacyWindowMouseInputPosition(x, y) : externalMouseInputPosition(x, y);
  }

  SDL_DisplayOrientation currentDisplayOrientation() const {
    if (!m_window)
      return SDL_ORIENTATION_UNKNOWN;

    SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
    if (!display)
      return SDL_ORIENTATION_UNKNOWN;
    return SDL_GetCurrentDisplayOrientation(display);
  }

  bool platformGyroAvailable() const {
#ifdef STAR_SYSTEM_ANDROID
    return hasAndroidGyroSensor();
#else
    int count = 0;
    SDL_SensorID* sensors = SDL_GetSensors(&count);
    if (!sensors)
      return false;

    bool available = false;
    for (int i = 0; i < count; ++i) {
      if (SDL_GetSensorTypeForID(sensors[i]) == SDL_SENSOR_GYRO) {
        available = true;
        break;
      }
    }
    SDL_free(sensors);
    return available;
#endif
  }

  void syncGyroSensor(bool enabled) {
    if (enabled && !platformGyroAvailable()) {
      if (m_touchAdapter)
        m_touchAdapter->setGyroAvailable(false);
      enabled = false;
    }

    if (!enabled) {
      m_nextGyroSensorRetryMs = 0;
#ifdef STAR_SYSTEM_ANDROID
      if (m_androidGyroSensorEnabled) {
        setAndroidGyroSensorEnabled(false);
        m_androidGyroSensorEnabled = false;
      }
#endif
      if (m_gyroSensor) {
        SDL_CloseSensor(m_gyroSensor);
        m_gyroSensor = nullptr;
      }
      return;
    }

#ifdef STAR_SYSTEM_ANDROID
    if (!m_androidGyroSensorEnabled) {
      m_androidGyroSensorEnabled = setAndroidGyroSensorEnabled(true);
      androidLogInfo("Android gyro sensor %s through Java bridge", m_androidGyroSensorEnabled ? "enabled" : "failed to enable");
    }
#endif

    if (m_gyroSensor)
      return;

    int count = 0;
    SDL_SensorID* sensors = SDL_GetSensors(&count);
    if (!sensors)
      return;

    for (int i = 0; i < count; ++i) {
      if (SDL_GetSensorTypeForID(sensors[i]) == SDL_SENSOR_GYRO) {
        m_gyroSensor = SDL_OpenSensor(sensors[i]);
        if (m_gyroSensor)
          androidLogInfo("Opened SDL gyroscope sensor: %s", SDL_GetSensorName(m_gyroSensor));
        else
          androidLogInfo("Failed to open SDL gyroscope sensor: %s", SDL_GetError());
        break;
      }
    }
    SDL_free(sensors);
  }

  void updateGyroInput() {
    if (!m_touchAdapter)
      return;

    bool gyroAvailable = platformGyroAvailable();
    m_touchAdapter->setGyroAvailable(gyroAvailable);
    if (!gyroAvailable) {
      syncGyroSensor(false);
      m_touchAdapter->setGyroInput({}, false, currentDisplayOrientation());
      return;
    }

    if (m_touchAdapter->gyroSensorRequested()) {
      int64_t now = Time::monotonicMilliseconds();
#ifdef STAR_SYSTEM_ANDROID
      if (!m_androidGyroSensorEnabled && now >= m_nextGyroSensorRetryMs) {
        syncGyroSensor(true);
        m_nextGyroSensorRetryMs = now + 1000;
      }
#else
      if (!m_gyroSensor && now >= m_nextGyroSensorRetryMs) {
        syncGyroSensor(true);
        m_nextGyroSensorRetryMs = now + 1000;
      }
#endif
    }

    std::array<float, 3> data{};
    bool hasData = false;

#ifdef STAR_SYSTEM_ANDROID
    hasData = takeAndroidGyroData(data);
#endif

    if (!hasData && m_gyroSensor) {
      SDL_UpdateSensors();
      hasData = SDL_GetSensorData(m_gyroSensor, data.data(), (int)data.size());
    }

    m_touchAdapter->setGyroInput(data, hasData, currentDisplayOrientation());
  }

  List<InputEvent> processEvents() {
    List<InputEvent> events;

    SDL_Event event;
    bool overlayEnabled = m_touchAdapter && m_touchAdapter->overlayEnabled();
    ImGuiIO* io = overlayEnabled ? &ImGui::GetIO() : nullptr;

    m_touchAdapter->beginFrame();
    if (m_gamepadAdapter)
      m_gamepadAdapter->beginFrame();

    while (SDL_PollEvent(&event)) {
      bool touchDerivedMouse = isTouchDerivedMouseEvent(event);
      if (event.type != SDL_EVENT_FINGER_DOWN
          && event.type != SDL_EVENT_FINGER_UP
          && event.type != SDL_EVENT_FINGER_MOTION
          && event.type != SDL_EVENT_FINGER_CANCELED) {
        convertEventToRenderCoordinatesIfPossible(m_window, &event);
      }
      if (overlayEnabled && !touchDerivedMouse)
        ImGui_ImplSDL3_ProcessEvent(&event);

      if (event.type == SDL_EVENT_QUIT) {
#ifdef STAR_SYSTEM_ANDROID
        // Ignore synthetic/native quit events on Android to avoid racing
        // teardown against in-flight SDL VSync callbacks.
        continue;
#elif defined(STAR_SYSTEM_SWITCH)
        // SDL's Switch backend (SDL_switchvideo.c) posts SDL_EVENT_QUIT once
        // libnx appletMainLoop() returns false, i.e. on AppletMessage_ExitRequest
        // from the system. Once that happens it is STICKY (SDL re-posts QUIT on
        // every PumpEvents), so we must NOT ignore-and-continue (that turns the
        // event-drain loop into an infinite spin = freeze). Honor it as a quit.
        Logger::info("QUIT PATH: SDL_EVENT_QUIT received (system exit request)");
        m_quitRequested = true;
        continue;
#else
        m_quitRequested = true;
        continue;
#endif
      }

      if (event.type == SDL_EVENT_WINDOW_RESIZED
          || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
          || event.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED) {
        syncWindowMetrics(true);
        continue;
      }

      if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        syncWindowMetrics(true);
        continue;
      }

      if (overlayEnabled && shouldCancelMobileTouchState(event))
        m_touchAdapter->cancelAll();

      if (m_touchAdapter->processSdlEvent(event))
        continue;
      if (overlayEnabled && touchDerivedMouse)
        continue;

      Maybe<InputEvent> input;
      if (event.type == SDL_EVENT_KEY_DOWN && (!overlayEnabled || (!io->WantCaptureKeyboard || !io->WantTextInput)) && !event.key.repeat) {
        auto key = keyFromSdlKeyCode(event.key.key);
        if (!key)
          key = keyFromSdlScancode(event.key.scancode);
        if (key)
          input.set(KeyDownEvent{*key, (KeyMod)event.key.mod});
      } else if (event.type == SDL_EVENT_KEY_UP) {
        auto key = keyFromSdlKeyCode(event.key.key);
        if (!key)
          key = keyFromSdlScancode(event.key.scancode);
        if (key)
          input.set(KeyUpEvent{*key});
      } else if (event.type == SDL_EVENT_TEXT_INPUT && (!overlayEnabled || !io->WantTextInput)) {
        input.set(TextInputEvent{event.text.text});
      } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        input.set(MouseMoveEvent{{event.motion.xrel, -event.motion.yrel}, mouseInputPosition(event.motion.x, event.motion.y, touchDerivedMouse)});
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && (!overlayEnabled || !io->WantCaptureMouse)) {
        input.set(MouseButtonDownEvent{mouseButtonFromSdlMouseButton(event.button.button), mouseInputPosition(event.button.x, event.button.y, touchDerivedMouse)});
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && (!overlayEnabled || !io->WantCaptureMouse)) {
        input.set(MouseButtonUpEvent{mouseButtonFromSdlMouseButton(event.button.button), mouseInputPosition(event.button.x, event.button.y, touchDerivedMouse)});
      } else if (event.type == SDL_EVENT_MOUSE_WHEEL && (!overlayEnabled || !io->WantCaptureMouse)) {
        input.set(MouseWheelEvent{event.wheel.y < 0 ? MouseWheel::Down : MouseWheel::Up, mouseInputPosition(event.wheel.mouse_x, event.wheel.mouse_y, touchDerivedMouse)});
      } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        input.set(ControllerAxisEvent{(ControllerId)event.gaxis.which, controllerAxisFromSdlControllerAxis(event.gaxis.axis), (float)event.gaxis.value / 32768.0f});
      } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        input.set(ControllerButtonDownEvent{(ControllerId)event.gbutton.which, controllerButtonFromSdlControllerButton(event.gbutton.button)});
      } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        input.set(ControllerButtonUpEvent{(ControllerId)event.gbutton.which, controllerButtonFromSdlControllerButton(event.gbutton.button)});
      } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        openGamepad(event.gdevice.which);
      } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        closeGamepad(event.gdevice.which);
      }

      if (input) {
        if (m_gamepadAdapter)
          m_gamepadAdapter->processInputEvent(input.take(), events);
        else
          events.append(input.take());
      }
    }

    syncWindowMetrics(true);

    updateGyroInput();
    m_touchAdapter->endFrame();
    m_touchAdapter->appendGeneratedEvents(events);
    if (m_gamepadAdapter) {
      m_gamepadAdapter->endFrame();
      m_gamepadAdapter->appendGeneratedEvents(events);
    }
    return events;
  }

  ImVec2 launcherTouchPosition(float x, float y) const {
    ImVec2 displaySize = imguiDisplaySize();
    if (x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)
      return ImVec2(x * displaySize.x, y * displaySize.y);
    return ImVec2(x, y);
  }

  bool processLauncherTouchEvent(SDL_Event const& event) {
    if (event.type != SDL_EVENT_FINGER_DOWN
        && event.type != SDL_EVENT_FINGER_MOTION
        && event.type != SDL_EVENT_FINGER_UP
        && event.type != SDL_EVENT_FINGER_CANCELED)
      return false;

    if (!ImGui::GetCurrentContext())
      return true;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos = launcherTouchPosition(event.tfinger.x, event.tfinger.y);
    uint64_t finger = event.tfinger.fingerID;

#ifdef STAR_SYSTEM_SWITCH
    // Touch pipeline diagnostics: launcher taps are rare, log them all so a
    // pulled log shows exactly what the driver delivered.
    Logger::info("[touch] type={} finger={} pos=({:.1f},{:.1f}) active={} trackedFinger={}",
        event.type == SDL_EVENT_FINGER_DOWN ? "down"
            : event.type == SDL_EVENT_FINGER_UP ? "up"
            : event.type == SDL_EVENT_FINGER_CANCELED ? "cancel" : "motion",
        finger, pos.x, pos.y, m_launcherTouchActive, m_launcherTouchFinger);
#endif

    if (event.type == SDL_EVENT_FINGER_DOWN) {
      if (!m_launcherTouchActive) {
        m_launcherTouchActive = true;
        m_launcherTouchFinger = finger;
        m_launcherTouchLastPos = pos;
        m_launcherTouchDragDistance = 0.0f;
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(pos.x, pos.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      }
      return true;
    }

    if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
      // Release on ANY finger-up while a touch is active: some touch drivers
      // (Switch hid) report a different fingerID on release than on press.
      // Discarding the mismatched UP wedged this state machine with the ImGui
      // mouse button held down forever -- the first tap stayed "highlighted"
      // and every later tap was ignored.
      if (m_launcherTouchActive) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(pos.x, pos.y);
        if (!m_launcherImGuiClickConsumed)
          io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        m_launcherTouchActive = false;
        m_launcherTouchFinger = 0;
        m_launcherTouchDragDistance = 0.0f;
      }
      return true;
    }

    if (!m_launcherTouchActive || m_launcherTouchFinger != finger)
      return true;

    if (event.type == SDL_EVENT_FINGER_MOTION) {
      io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
      io.AddMousePosEvent(pos.x, pos.y);

      float dx = pos.x - m_launcherTouchLastPos.x;
      float dy = pos.y - m_launcherTouchLastPos.y;
      m_launcherTouchLastPos = pos;
      m_launcherTouchDragDistance += std::abs(dx) + std::abs(dy);

      if (m_launcherTouchDragDistance > 6.0f && std::abs(dy) > 0.25f)
        io.AddMouseWheelEvent(0.0f, dy / 48.0f);

      return true;
    }

    return true;
  }

  // Crash-window tracer for the quit->relaunch path: session 2 of an
  // in-process relaunch can die before the game log opens, leaving no
  // evidence. Each stage is appended + fsync'd so the trace survives a hard
  // fault. Read /switch/oSBM/relaunch-trace.txt after a crash.
  void relaunchBreadcrumb(char const* stage) {
#ifdef STAR_SYSTEM_SWITCH
    if (FILE* f = fopen("/switch/oSBM/relaunch-trace.txt", "a")) {
      fprintf(f, "%lld %s\n", (long long)Time::monotonicMilliseconds(), stage);
      fflush(f);
      fsync(fileno(f));
      fclose(f);
    }
#else
    (void)stage;
#endif
  }

  void resetLauncherQuickStart() {
    m_launcherQuickStartButtonHeld = false;
    m_launcherQuickStartStartMs = 0;
  }

  void processWindowEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (!processLauncherTouchEvent(event)) {
        if (isTouchDerivedMouseEvent(event))
          continue;
        convertEventToRenderCoordinatesIfPossible(m_window, &event);
        ImGui_ImplSDL3_ProcessEvent(&event);
      }
      if (event.type == SDL_EVENT_QUIT) {
#ifdef STAR_SYSTEM_ANDROID
        continue;
#elif defined(STAR_SYSTEM_SWITCH)
        // See runGameLoop's handler: appletMainLoop()->false is sticky and SDL
        // re-posts QUIT every PumpEvents, so break out of the drain loop (a
        // `continue` would spin forever and freeze) and honor the quit.
        Logger::info("QUIT PATH: SDL_EVENT_QUIT in processWindowEvents (system exit request)");
        m_quitRequested = true;
        break;
#else
        m_quitRequested = true;
#endif
      }
      if (event.type == SDL_EVENT_WINDOW_RESIZED
          || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
          || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED
          || event.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED) {
        syncWindowMetrics(false);
      } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        // A physical keyboard Escape requests a launcher "back". The Android
        // system back button and edge-swipe gesture do NOT arrive here: the app
        // opts into the predictive-back dispatcher, so they are delivered
        // natively through MainActivity -> nativeOnLauncherBack (see the JNI
        // bridge below) and folded in via applyLauncherPendingNavigation.
        if (event.key.key == SDLK_ESCAPE) {
          if (!ImGui::GetCurrentContext() || !ImGui::GetIO().WantTextInput)
            m_launcherPendingNav = -1;
        }
      } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        openGamepad(event.gdevice.which);
      } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        closeGamepad(event.gdevice.which);
      } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        ControllerButton button = controllerButtonFromSdlControllerButton(event.gbutton.button);
        if (button == ControllerButton::A && !m_launcherQuickStartButtonHeld) {
          m_launcherQuickStartButtonHeld = true;
          m_launcherQuickStartStartMs = Time::monotonicMilliseconds();
        } else if (button == ControllerButton::B || button == ControllerButton::Back) {
          // B / View act as the launcher "back" navigation for gamepads.
          m_launcherPendingNav = -1;
        }
      } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        if (controllerButtonFromSdlControllerButton(event.gbutton.button) == ControllerButton::A)
          resetLauncherQuickStart();
      }
    }

    syncWindowMetrics(false);
  }

  void openGamepad(SDL_JoystickID id) {
    if (m_sdlGamepads.contains(id))
      return;

    SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
    if (!gamepad) {
      Logger::warn("Controller device '{}' could not be opened: {}", id, SDL_GetError());
      return;
    }

    m_sdlGamepads.insert_or_assign(id, SDLGamepadUPtr(gamepad, SDL_CloseGamepad));
    refreshActiveGamepadInfo();
    Logger::info("Controller device '{}' added", SDL_GetGamepadName(gamepad) ? SDL_GetGamepadName(gamepad) : "Unknown Controller");
  }

  void closeGamepad(SDL_JoystickID id) {
    auto find = m_sdlGamepads.find(id);
    if (find != m_sdlGamepads.end()) {
      if (SDL_Gamepad* gamepad = find->second.get())
        Logger::info("Controller device '{}' removed", SDL_GetGamepadName(gamepad) ? SDL_GetGamepadName(gamepad) : "Unknown Controller");
      m_sdlGamepads.erase(id);
    }
    refreshActiveGamepadInfo();
    if (m_gamepadAdapter)
      m_gamepadAdapter->cancelAll();
    resetLauncherQuickStart();
  }

  void refreshActiveGamepadInfo() {
    m_activeGamepadName.clear();
    m_activeGamepadType = SDL_GAMEPAD_TYPE_UNKNOWN;
    for (auto const& pair : m_sdlGamepads) {
      if (SDL_Gamepad* gamepad = pair.second.get()) {
        char const* name = SDL_GetGamepadName(gamepad);
        m_activeGamepadName = name ? String(name) : String("Unknown Controller");
        m_activeGamepadType = SDL_GetGamepadType(gamepad);
        return;
      }
    }
  }

  void openExistingGamepads() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids)
      return;
    for (int i = 0; i < count; ++i)
      openGamepad(ids[i]);
    SDL_free(ids);
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

  // Returns the physical-pixel game canvas size (window minus safe area).
  Vec2U gameCanvasSize() const {
    Vec2U s = m_windowSize;
    if (s[0] > m_safeArea.left + m_safeArea.right)
      s[0] -= m_safeArea.left + m_safeArea.right;
    if (s[1] > m_safeArea.top + m_safeArea.bottom)
      s[1] -= m_safeArea.top + m_safeArea.bottom;
    return s;
  }

  Vec2U requestedRenderCanvasSize(Vec2U physicalCanvas) const {
    if (physicalCanvas[0] == 0 || physicalCanvas[1] == 0)
      return physicalCanvas;
    if (m_requestedRenderResolution[0] == 0 || m_requestedRenderResolution[1] == 0)
      return physicalCanvas;

    Vec2U request = m_requestedRenderResolution;
    if ((physicalCanvas[0] >= physicalCanvas[1]) != (request[0] >= request[1]))
      std::swap(request[0], request[1]);

    if (request[0] >= physicalCanvas[0] && request[1] >= physicalCanvas[1])
      return physicalCanvas;

    float scale = std::min((float)request[0] / (float)physicalCanvas[0], (float)request[1] / (float)physicalCanvas[1]);
    scale = std::clamp(scale, 0.1f, 1.0f);
    return {
      std::max(320u, (unsigned)std::round((float)physicalCanvas[0] * scale)),
      std::max(240u, (unsigned)std::round((float)physicalCanvas[1] * scale))
    };
  }

  void setRequestedRenderResolution(Vec2U resolution) {
    m_requestedRenderResolution = resolution;
    syncWindowMetrics(true);
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

      // Compute the exact framebuffer-pixels-per-window-point ratio from the
      // two window-size queries (handles fractional/HiDPI scaling precisely,
      // rather than trusting the display content scale). Mouse events arrive
      // in points; the render canvas is in pixels, so external-mouse coords
      // are scaled by this in externalMouseInputPosition.
      int pointW = 0, pointH = 0;
      if (hasSize && SDL_GetWindowSize(m_window, &pointW, &pointH) && pointW > 0 && pointH > 0) {
        m_windowPointToPixel = {
          (float)queriedSize[0] / (float)pointW,
          (float)queriedSize[1] / (float)pointH
        };
      }
    }

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    // Re-query safe-area insets every time (they change on rotation).
    {
#ifdef STAR_SYSTEM_ANDROID
      unsigned saTop = 0, saLeft = 0, saBottom = 0, saRight = 0;
      AndroidFileAccessBridge::getSafeAreaInsets(&saTop, &saLeft, &saBottom, &saRight);
      SafeAreaInsets newSA{saTop, saLeft, saBottom, saRight};
#else
      float saTop = 0, saLeft = 0, saBottom = 0, saRight = 0;
      StarIosBridge_getSafeAreaInsets(&saTop, &saLeft, &saBottom, &saRight);

      // iOS reports symmetric insets on both landscape edges even though only
      // one edge has the actual notch / Dynamic Island.  Query the current
      // interface orientation so we apply an inset ONLY on the hardware-notch
      // edge and leave all other edges at zero.
      //
      // Orientation values: 1=Portrait, 2=LandscapeLeft (notch on left),
      //                     3=LandscapeRight (notch on right), 4=PortraitUD
      int orient = StarIosBridge_getInterfaceOrientation();
      switch (orient) {
        case 2: // LandscapeLeft  — notch/DI is on the RIGHT of the screen
          saLeft   = 0.0f;
          saTop    = 0.0f;
          saBottom = 0.0f;
          break;
        case 3: // LandscapeRight — notch/DI is on the LEFT of the screen
          saRight  = 0.0f;
          saTop    = 0.0f;
          saBottom = 0.0f;
          break;
        case 4: // PortraitUpsideDown — notch/DI at bottom
          saTop    = 0.0f;
          saLeft   = 0.0f;
          saRight  = 0.0f;
          break;
        default: // Portrait (1) or unknown (0) — notch/DI at top
          saLeft   = 0.0f;
          saRight  = 0.0f;
          saBottom = 0.0f;
          break;
      }

      float scale = std::max(1.0f, std::round(m_displayScale));
      SafeAreaInsets newSA{
          (unsigned)std::round(saTop    * scale),
          (unsigned)std::round(saLeft   * scale),
          (unsigned)std::round(saBottom * scale),
          (unsigned)std::round(saRight  * scale)
      };
#endif
      // Fullscreen rendering: ignore the insets entirely -- the viewport
      // covers the whole panel and the cutout overlaps content.
      if (m_fullscreenRender)
        newSA = SafeAreaInsets{};
      if (!(newSA == m_safeArea)) {
        m_safeArea = newSA;
        sizeChanged = true;
      }
    }
#endif

    Vec2U physicalCanvas = gameCanvasSize();
    Vec2U renderCanvas = requestedRenderCanvasSize(physicalCanvas);
    if (renderCanvas != m_renderCanvasSize) {
      m_renderCanvasSize = renderCanvas;
      sizeChanged = true;
    }

    if (sizeChanged) {
      if (m_renderer) {
        // Viewport origin: safe-area left offset (x) and bottom offset (y) in
        // OpenGL convention where y=0 is the physical bottom of the screen.
        m_renderer->setScreenOffset(Vec2U(m_safeArea.left, m_safeArea.bottom));
        m_renderer->setScreenViewportSize(physicalCanvas);
        m_renderer->setScreenSize(renderCanvas);
        // Full drawable size: startFrame needs it to know whether safe-area
        // margins exist outside the viewport (they must be cleared per frame
        // or overlay pixels smear into the notch bar).
        m_renderer->setWindowSurfaceSize(m_windowSize);
      }

      if (notifyApplication && m_application)
        m_application->windowChanged(WindowMode::Normal, renderCanvas);

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
    Vec2U canvas = gameCanvasSize();
    float shortSide = (float)std::min(canvas[0], canvas[1]) / pixelScale;
    float baseScale = std::clamp(shortSide / 900.0f, 0.85f, 1.15f);
#else
    float pixelScale = std::max(1.0f, m_displayScale > 0.0f ? m_displayScale : 1.5f);
    float shortSide = (float)std::min(m_windowSize[0], m_windowSize[1]) / pixelScale;
    float baseScale = std::clamp(shortSide / 900.0f, 0.85f, 1.15f);
#endif
    io.FontGlobalScale = std::clamp(baseScale * std::clamp(m_launcherUiConfig.scale, 0.75f, 1.75f), 0.65f, 3.0f);
  }

  ImVec2 imguiDisplaySize() const {
    if (ImGui::GetCurrentContext()) {
      ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      if (displaySize.x > 0.0f && displaySize.y > 0.0f)
        return displaySize;
    }

#ifdef STAR_SYSTEM_IOS
    float pixelScale = std::max(1.0f, std::round(m_displayScale));
    Vec2U canvas = gameCanvasSize();
    return ImVec2((float)canvas[0] / pixelScale, (float)canvas[1] / pixelScale);
#else
    return ImVec2((float)m_windowSize[0], (float)m_windowSize[1]);
#endif
  }

  void syncTextInputState() {
    // Everything except Switch drives SDL text input (Switch uses its own
    // blocking swkbd session -- see setAcceptingTextInput). Desktop needs this
    // exactly like Android/iOS so hardware-keyboard typing reaches textboxes.
#ifndef STAR_SYSTEM_SWITCH
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

  void syncImGuiTextInputState() {
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    bool wantTextInput = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
#ifdef STAR_SYSTEM_ANDROID
    setAndroidImGuiTextInputActive(wantTextInput);
#endif
    if (m_textInput != wantTextInput) {
      m_textInput = wantTextInput;
      m_textInputDirty = true;
    }
    syncTextInputState();
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

  typedef std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)> SDLGamepadUPtr;
  StableHashMap<SDL_JoystickID, SDLGamepadUPtr> m_sdlGamepads;
  String m_activeGamepadName;
  SDL_GamepadType m_activeGamepadType = SDL_GAMEPAD_TYPE_UNKNOWN;

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_glContext = nullptr;
  SDL_Sensor* m_gyroSensor = nullptr;
#ifdef STAR_SYSTEM_ANDROID
  bool m_androidGyroSensorEnabled = false;
#endif
  int64_t m_nextGyroSensorRetryMs = 0;

  String m_windowTitle = "OpenStarbound";
  Vec2U m_windowSize = {1280, 720};
  Vec2U m_renderCanvasSize = {1280, 720};
  Vec2U m_requestedRenderResolution = {0, 0};
  // Framebuffer-pixels per window-point. m_windowSize / canvases are in pixels
  // (SDL_GetWindowSizeInPixels), but SDL mouse events report window points, so
  // external-mouse coordinates must be scaled by this before mapping into the
  // pixel-space render canvas. 1.0 on non-HiDPI displays.
  Vec2F m_windowPointToPixel = {1.0f, 1.0f};
  SafeAreaInsets m_safeArea;
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
  String m_legacyStorageRoot;
  String m_bootConfigPath;
  String m_launcherLocale = DefaultLauncherLocale;
  String m_launcherLangDirectory;
  String m_launcherFontPath;
  StringMap<String> m_launcherTranslations;
  StringMap<LauncherTexture> m_launcherTextureCache;
  ByteArray m_launcherFontData;
  std::vector<ByteArray> m_launcherFallbackFontData;
  LauncherUiConfig m_launcherUiConfig;

  TickRateApproacher m_updateTicker{60.0f, 1.0f};
  float m_maxFrameRate = 0.0f; // 0 = unlimited
  TickRateApproacher m_framePacer{60.0f, 1.0f};
  // Fixed-timestep accumulator (in ticks) driving both tick execution and
  // the render interpolation fraction; see the game loop.
  double m_tickAccum = 0.0;
  double m_lastLoopTime = 0.0;
  // Launcher performance settings (persisted in the launcher config; these
  // override starbound.config pacing keys on the mobile family, since the
  // launcher UI is the settings surface users actually see here).
  int m_perfSimRate = 60; // simulation tick rate: 30 or 60
  int m_perfFpsCap = 0;   // rendered FPS cap; 0 = unlimited
  // Fullscreen rendering ignores safe-area insets: the game fills the whole
  // panel and the notch / camera cutout may overlap content. Default OFF so
  // existing installs keep the inset (letterboxed) behavior they have today.
  bool m_fullscreenRender = false;
  TickRateMonitor m_renderTicker{1.0f};
  float m_updateRate = 0.0f;
  float m_renderRate = 0.0f;
  unsigned m_maxFrameSkip = 5;

  SignalHandler m_signalHandler;

  shared_ptr<GlesRenderer> m_renderer;
  ApplicationControllerPtr m_appController;
  MobilePlatformServicesUPtr m_platformServices;
  unique_ptr<MobileTouchInputAdapter> m_touchAdapter;
  unique_ptr<MobileGamepadInputAdapter> m_gamepadAdapter;
  bool m_launcherTouchActive = false;
  uint64_t m_launcherTouchFinger = 0;
  ImVec2 m_launcherTouchLastPos = ImVec2(0.0f, 0.0f);
  float m_launcherTouchDragDistance = 0.0f;
  // Queued launcher navigation intent: -1 = back, +1 = forward, 0 = none.
  // Populated from real input events (gamepad/keyboard) and, on mobile, from
  // native OS back/edge-swipe callbacks folded in via g_launcherNativeNavRequest.
  int m_launcherPendingNav = 0;
  bool m_launcherImGuiClickConsumed = false;
  bool m_launcherQuickStartButtonHeld = false;
  int64_t m_launcherQuickStartStartMs = 0;
};


} // namespace

int runMobileMainApplication(ApplicationUPtr application, StringList cmdLineArgs) {
  MobilePlatform platform(std::move(application), std::move(cmdLineArgs));
  return platform.run();
}

}
