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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#ifdef STAR_SYSTEM_ANDROID
#include <android/log.h>
#include <jni.h>
#endif

#ifdef STAR_SYSTEM_ANDROID
namespace {

std::mutex g_androidGyroMutex;
std::array<float, 3> g_androidGyroData{};
bool g_androidGyroHasData = false;

}

extern "C" JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_onNativeGyro(JNIEnv*, jclass, jfloat x, jfloat y, jfloat z) {
  std::lock_guard<std::mutex> lock(g_androidGyroMutex);
  g_androidGyroData = {x, y, z};
  g_androidGyroHasData = true;
}

static bool takeAndroidGyroData(std::array<float, 3>& data) {
  std::lock_guard<std::mutex> lock(g_androidGyroMutex);
  if (!g_androidGyroHasData)
    return false;

  data = g_androidGyroData;
  g_androidGyroHasData = false;
  return true;
}
#endif

namespace Star {

String getMobileStartupStatus();
void setMobileStartupStatus(String const& status);

// Physical-pixel safe-area insets (distance from each screen edge).
// Non-zero on devices whose fullscreen surface overlaps a display cutout.
struct SafeAreaInsets {
  unsigned top = 0, left = 0, bottom = 0, right = 0;
  bool operator==(SafeAreaInsets const& o) const {
    return top == o.top && left == o.left && bottom == o.bottom && right == o.right;
  }
};

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

bool samePath(String const& a, String const& b) {
  return File::convertDirSeparators(a).trimEnd("/") == File::convertDirSeparators(b).trimEnd("/");
}

bool filesMatch(String const& leftPath, String const& rightPath) {
  if (!File::isFile(leftPath) || !File::isFile(rightPath))
    return false;
  if (File::fileSize(leftPath) != File::fileSize(rightPath))
    return false;

  auto left = File::open(leftPath, IOMode::Read);
  auto right = File::open(rightPath, IOMode::Read);
  char leftBuffer[64 * 1024];
  char rightBuffer[64 * 1024];
  while (!left->atEnd()) {
    size_t leftRead = left->read(leftBuffer, sizeof(leftBuffer));
    size_t rightRead = right->read(rightBuffer, sizeof(rightBuffer));
    if (leftRead != rightRead || std::memcmp(leftBuffer, rightBuffer, leftRead) != 0)
      return false;
  }

  return right->atEnd();
}

void removeIfEmptyDirectory(String const& directory) {
  try {
    if (File::isDirectory(directory) && File::dirList(directory).empty())
      File::remove(directory);
  } catch (...) {
  }
}

void migrateDirectoryFiles(String const& sourceDirectory, String const& targetDirectory) {
  if (!File::isDirectory(sourceDirectory))
    return;

  File::makeDirectoryRecursive(targetDirectory);

  static StringSet const ignoredTopLevelEntries{"bundled_assets", "diagnostics", "logs", "tmp"};
  for (auto const& entry : File::dirList(sourceDirectory)) {
    auto const& name = entry.first;
    if (ignoredTopLevelEntries.contains(name))
      continue;

    auto sourcePath = File::relativeTo(sourceDirectory, name);
    auto targetPath = File::relativeTo(targetDirectory, name);
    try {
      if (entry.second) {
        migrateDirectoryFiles(sourcePath, targetPath);
        removeIfEmptyDirectory(sourcePath);
      } else {
        if (!File::exists(targetPath)) {
          File::makeDirectoryRecursive(File::dirName(targetPath));
          File::copy(sourcePath, targetPath);
        }

        if (filesMatch(sourcePath, targetPath))
          File::remove(sourcePath);
      }
    } catch (std::exception const& e) {
      androidLogInfo("Storage migration skipped %s: %s", sourcePath.utf8Ptr(), e.what());
    } catch (...) {
      androidLogInfo("Storage migration skipped %s: unknown error", sourcePath.utf8Ptr());
    }
  }
}

#ifdef STAR_SYSTEM_ANDROID
bool setAndroidGyroSensorEnabled(bool enabled) {
  JNIEnv* env = reinterpret_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env)
    return false;

  jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity)
    return false;

  jclass cls = env->GetObjectClass(activity);
  env->DeleteLocalRef(activity);
  if (!cls)
    return false;

  jmethodID method = env->GetStaticMethodID(cls, "setGyroSensorEnabled", "(Z)Z");
  if (!method) {
    env->DeleteLocalRef(cls);
    return false;
  }

  bool result = env->CallStaticBooleanMethod(cls, method, enabled);
  env->DeleteLocalRef(cls);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }

  return result;
}
#endif

String defaultMobileStorageRoot() {
  String fallbackStorageRoot = SDL_GetPrefPath("OpenStarbound", "OpenStarbound");
  if (fallbackStorageRoot.empty())
    fallbackStorageRoot = "./";
  return fallbackStorageRoot;
}

String writableMobileStorageRoot(String const& fallbackStorageRoot) {
  String storageRoot = fallbackStorageRoot;
#ifdef STAR_SYSTEM_ANDROID
  if (auto resolved = AndroidFileAccessBridge::resolveStorageRoot(fallbackStorageRoot))
    storageRoot = *resolved;
#endif

  try {
    File::makeDirectoryRecursive(storageRoot);
  } catch (std::exception const& e) {
    androidLogInfo("Could not create storage root %s: %s", storageRoot.utf8Ptr(), e.what());
    storageRoot = fallbackStorageRoot;
    File::makeDirectoryRecursive(storageRoot);
  }

#ifdef STAR_SYSTEM_ANDROID
  if (!samePath(fallbackStorageRoot, storageRoot)) {
    androidLogInfo("Storage root resolved to %s; migrating from %s", storageRoot.utf8Ptr(), fallbackStorageRoot.utf8Ptr());
    migrateDirectoryFiles(fallbackStorageRoot, storageRoot);
  }
#endif

  auto tempRoot = File::relativeTo(storageRoot, "tmp");
  try {
    File::makeDirectoryRecursive(tempRoot);
#ifdef STAR_SYSTEM_ANDROID
    setenv("TMPDIR", tempRoot.utf8Ptr(), 1);
    setenv("HOME", storageRoot.utf8Ptr(), 1);
#endif
  } catch (std::exception const& e) {
    androidLogInfo("Could not prepare temp root %s: %s", tempRoot.utf8Ptr(), e.what());
  }

  androidLogInfo("Using mobile storage root %s", storageRoot.utf8Ptr());
  return storageRoot;
}

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

static inline bool shouldCancelMobileTouchState(SDL_Event const& event) {
  return event.type == SDL_EVENT_WINDOW_FOCUS_LOST
      || event.type == SDL_EVENT_WINDOW_HIDDEN
      || event.type == SDL_EVENT_WINDOW_MINIMIZED
      || event.type == SDL_EVENT_WINDOW_RESIZED
      || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
      || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED
      || event.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED
      || event.type == SDL_EVENT_WILL_ENTER_BACKGROUND
      || event.type == SDL_EVENT_DID_ENTER_BACKGROUND;
}

Maybe<Key> keyFromSdlKeyCode(SDL_Keycode sym) {
  static HashMap<int, Key> KeyCodeMap{
    {SDLK_BACKSPACE, Key::Backspace},
    {SDLK_TAB, Key::Tab},
    {SDLK_CLEAR, Key::Clear},
    {SDLK_RETURN, Key::Return},
    {SDLK_PAUSE, Key::Pause},
    {SDLK_ESCAPE, Key::Escape},
    {SDLK_SPACE, Key::Space},
    {SDLK_EXCLAIM, Key::Exclaim},
    {SDLK_DBLAPOSTROPHE, Key::QuotedBL},
    {SDLK_HASH, Key::Hash},
    {SDLK_DOLLAR, Key::Dollar},
    {SDLK_AMPERSAND, Key::Ampersand},
    {SDLK_APOSTROPHE, Key::Quote},
    {SDLK_LEFTPAREN, Key::LeftParen},
    {SDLK_RIGHTPAREN, Key::RightParen},
    {SDLK_ASTERISK, Key::Asterisk},
    {SDLK_PLUS, Key::Plus},
    {SDLK_COMMA, Key::Comma},
    {SDLK_MINUS, Key::Minus},
    {SDLK_PERIOD, Key::Period},
    {SDLK_SLASH, Key::Slash},
    {SDLK_0, Key::Zero},
    {SDLK_1, Key::One},
    {SDLK_2, Key::Two},
    {SDLK_3, Key::Three},
    {SDLK_4, Key::Four},
    {SDLK_5, Key::Five},
    {SDLK_6, Key::Six},
    {SDLK_7, Key::Seven},
    {SDLK_8, Key::Eight},
    {SDLK_9, Key::Nine},
    {SDLK_COLON, Key::Colon},
    {SDLK_SEMICOLON, Key::Semicolon},
    {SDLK_LESS, Key::Less},
    {SDLK_EQUALS, Key::Equals},
    {SDLK_GREATER, Key::Greater},
    {SDLK_QUESTION, Key::Question},
    {SDLK_AT, Key::At},
    {SDLK_LEFTBRACKET, Key::LeftBracket},
    {SDLK_BACKSLASH, Key::Backslash},
    {SDLK_RIGHTBRACKET, Key::RightBracket},
    {SDLK_CARET, Key::Caret},
    {SDLK_UNDERSCORE, Key::Underscore},
    {SDLK_GRAVE, Key::Backquote},
    {SDLK_A, Key::A},
    {SDLK_B, Key::B},
    {SDLK_C, Key::C},
    {SDLK_D, Key::D},
    {SDLK_E, Key::E},
    {SDLK_F, Key::F},
    {SDLK_G, Key::G},
    {SDLK_H, Key::H},
    {SDLK_I, Key::I},
    {SDLK_J, Key::J},
    {SDLK_K, Key::K},
    {SDLK_L, Key::L},
    {SDLK_M, Key::M},
    {SDLK_N, Key::N},
    {SDLK_O, Key::O},
    {SDLK_P, Key::P},
    {SDLK_Q, Key::Q},
    {SDLK_R, Key::R},
    {SDLK_S, Key::S},
    {SDLK_T, Key::T},
    {SDLK_U, Key::U},
    {SDLK_V, Key::V},
    {SDLK_W, Key::W},
    {SDLK_X, Key::X},
    {SDLK_Y, Key::Y},
    {SDLK_Z, Key::Z},
    {SDLK_DELETE, Key::Delete},
    {SDLK_KP_0, Key::Keypad0},
    {SDLK_KP_1, Key::Keypad1},
    {SDLK_KP_2, Key::Keypad2},
    {SDLK_KP_3, Key::Keypad3},
    {SDLK_KP_4, Key::Keypad4},
    {SDLK_KP_5, Key::Keypad5},
    {SDLK_KP_6, Key::Keypad6},
    {SDLK_KP_7, Key::Keypad7},
    {SDLK_KP_8, Key::Keypad8},
    {SDLK_KP_9, Key::Keypad9},
    {SDLK_KP_PERIOD, Key::KeypadPeriod},
    {SDLK_KP_DIVIDE, Key::KeypadDivide},
    {SDLK_KP_MULTIPLY, Key::KeypadMultiply},
    {SDLK_KP_MINUS, Key::KeypadMinus},
    {SDLK_KP_PLUS, Key::KeypadPlus},
    {SDLK_KP_ENTER, Key::KeypadEnter},
    {SDLK_KP_EQUALS, Key::KeypadEquals},
    {SDLK_UP, Key::Up},
    {SDLK_DOWN, Key::Down},
    {SDLK_RIGHT, Key::Right},
    {SDLK_LEFT, Key::Left},
    {SDLK_INSERT, Key::Insert},
    {SDLK_HOME, Key::Home},
    {SDLK_END, Key::End},
    {SDLK_PAGEUP, Key::PageUp},
    {SDLK_PAGEDOWN, Key::PageDown},
    {SDLK_F1, Key::F1},
    {SDLK_F2, Key::F2},
    {SDLK_F3, Key::F3},
    {SDLK_F4, Key::F4},
    {SDLK_F5, Key::F5},
    {SDLK_F6, Key::F6},
    {SDLK_F7, Key::F7},
    {SDLK_F8, Key::F8},
    {SDLK_F9, Key::F9},
    {SDLK_F10, Key::F10},
    {SDLK_F11, Key::F11},
    {SDLK_F12, Key::F12},
    {SDLK_F13, Key::F13},
    {SDLK_F14, Key::F14},
    {SDLK_F15, Key::F15},
    {SDLK_F16, Key::F16},
    {SDLK_F17, Key::F17},
    {SDLK_F18, Key::F18},
    {SDLK_F19, Key::F19},
    {SDLK_F20, Key::F20},
    {SDLK_F21, Key::F21},
    {SDLK_F22, Key::F22},
    {SDLK_F23, Key::F23},
    {SDLK_F24, Key::F24},
    {SDLK_NUMLOCKCLEAR, Key::NumLock},
    {SDLK_CAPSLOCK, Key::CapsLock},
    {SDLK_SCROLLLOCK, Key::ScrollLock},
    {SDLK_RSHIFT, Key::RShift},
    {SDLK_LSHIFT, Key::LShift},
    {SDLK_RCTRL, Key::RCtrl},
    {SDLK_LCTRL, Key::LCtrl},
    {SDLK_RALT, Key::RAlt},
    {SDLK_LALT, Key::LAlt},
    {SDLK_RGUI, Key::RGui},
    {SDLK_LGUI, Key::LGui},
    {SDLK_MODE, Key::AltGr},
    {SDLK_APPLICATION, Key::Compose},
    {SDLK_HELP, Key::Help},
    {SDLK_PRINTSCREEN, Key::PrintScreen},
    {SDLK_SYSREQ, Key::SysReq},
    {SDLK_MENU, Key::Menu},
    {SDLK_POWER, Key::Power}
  };

  return KeyCodeMap.maybe(sym);
}

Maybe<Key> keyFromSdlScancode(SDL_Scancode scancode) {
  static HashMap<int, Key> ScanCodeMap{
    {SDL_SCANCODE_A, Key::A},
    {SDL_SCANCODE_B, Key::B},
    {SDL_SCANCODE_C, Key::C},
    {SDL_SCANCODE_D, Key::D},
    {SDL_SCANCODE_E, Key::E},
    {SDL_SCANCODE_F, Key::F},
    {SDL_SCANCODE_G, Key::G},
    {SDL_SCANCODE_H, Key::H},
    {SDL_SCANCODE_I, Key::I},
    {SDL_SCANCODE_J, Key::J},
    {SDL_SCANCODE_K, Key::K},
    {SDL_SCANCODE_L, Key::L},
    {SDL_SCANCODE_M, Key::M},
    {SDL_SCANCODE_N, Key::N},
    {SDL_SCANCODE_O, Key::O},
    {SDL_SCANCODE_P, Key::P},
    {SDL_SCANCODE_Q, Key::Q},
    {SDL_SCANCODE_R, Key::R},
    {SDL_SCANCODE_S, Key::S},
    {SDL_SCANCODE_T, Key::T},
    {SDL_SCANCODE_U, Key::U},
    {SDL_SCANCODE_V, Key::V},
    {SDL_SCANCODE_W, Key::W},
    {SDL_SCANCODE_X, Key::X},
    {SDL_SCANCODE_Y, Key::Y},
    {SDL_SCANCODE_Z, Key::Z},
    {SDL_SCANCODE_0, Key::Zero},
    {SDL_SCANCODE_1, Key::One},
    {SDL_SCANCODE_2, Key::Two},
    {SDL_SCANCODE_3, Key::Three},
    {SDL_SCANCODE_4, Key::Four},
    {SDL_SCANCODE_5, Key::Five},
    {SDL_SCANCODE_6, Key::Six},
    {SDL_SCANCODE_7, Key::Seven},
    {SDL_SCANCODE_8, Key::Eight},
    {SDL_SCANCODE_9, Key::Nine},
    {SDL_SCANCODE_MINUS, Key::Minus},
    {SDL_SCANCODE_EQUALS, Key::Equals},
    {SDL_SCANCODE_LEFTBRACKET, Key::LeftBracket},
    {SDL_SCANCODE_RIGHTBRACKET, Key::RightBracket},
    {SDL_SCANCODE_BACKSLASH, Key::Backslash},
    {SDL_SCANCODE_SEMICOLON, Key::Semicolon},
    {SDL_SCANCODE_APOSTROPHE, Key::Quote},
    {SDL_SCANCODE_GRAVE, Key::Backquote},
    {SDL_SCANCODE_COMMA, Key::Comma},
    {SDL_SCANCODE_PERIOD, Key::Period},
    {SDL_SCANCODE_SLASH, Key::Slash},
    {SDL_SCANCODE_BACKSPACE, Key::Backspace},
    {SDL_SCANCODE_TAB, Key::Tab},
    {SDL_SCANCODE_RETURN, Key::Return},
    {SDL_SCANCODE_ESCAPE, Key::Escape},
    {SDL_SCANCODE_SPACE, Key::Space},
    {SDL_SCANCODE_DELETE, Key::Delete},
    {SDL_SCANCODE_INSERT, Key::Insert},
    {SDL_SCANCODE_HOME, Key::Home},
    {SDL_SCANCODE_END, Key::End},
    {SDL_SCANCODE_PAGEUP, Key::PageUp},
    {SDL_SCANCODE_PAGEDOWN, Key::PageDown},
    {SDL_SCANCODE_UP, Key::Up},
    {SDL_SCANCODE_DOWN, Key::Down},
    {SDL_SCANCODE_LEFT, Key::Left},
    {SDL_SCANCODE_RIGHT, Key::Right},
    {SDL_SCANCODE_F1, Key::F1},
    {SDL_SCANCODE_F2, Key::F2},
    {SDL_SCANCODE_F3, Key::F3},
    {SDL_SCANCODE_F4, Key::F4},
    {SDL_SCANCODE_F5, Key::F5},
    {SDL_SCANCODE_F6, Key::F6},
    {SDL_SCANCODE_F7, Key::F7},
    {SDL_SCANCODE_F8, Key::F8},
    {SDL_SCANCODE_F9, Key::F9},
    {SDL_SCANCODE_F10, Key::F10},
    {SDL_SCANCODE_F11, Key::F11},
    {SDL_SCANCODE_F12, Key::F12},
    {SDL_SCANCODE_F13, Key::F13},
    {SDL_SCANCODE_F14, Key::F14},
    {SDL_SCANCODE_F15, Key::F15},
    {SDL_SCANCODE_F16, Key::F16},
    {SDL_SCANCODE_F17, Key::F17},
    {SDL_SCANCODE_F18, Key::F18},
    {SDL_SCANCODE_F19, Key::F19},
    {SDL_SCANCODE_F20, Key::F20},
    {SDL_SCANCODE_F21, Key::F21},
    {SDL_SCANCODE_F22, Key::F22},
    {SDL_SCANCODE_F23, Key::F23},
    {SDL_SCANCODE_F24, Key::F24},
    {SDL_SCANCODE_KP_0, Key::Keypad0},
    {SDL_SCANCODE_KP_1, Key::Keypad1},
    {SDL_SCANCODE_KP_2, Key::Keypad2},
    {SDL_SCANCODE_KP_3, Key::Keypad3},
    {SDL_SCANCODE_KP_4, Key::Keypad4},
    {SDL_SCANCODE_KP_5, Key::Keypad5},
    {SDL_SCANCODE_KP_6, Key::Keypad6},
    {SDL_SCANCODE_KP_7, Key::Keypad7},
    {SDL_SCANCODE_KP_8, Key::Keypad8},
    {SDL_SCANCODE_KP_9, Key::Keypad9},
    {SDL_SCANCODE_KP_PERIOD, Key::KeypadPeriod},
    {SDL_SCANCODE_KP_DIVIDE, Key::KeypadDivide},
    {SDL_SCANCODE_KP_MULTIPLY, Key::KeypadMultiply},
    {SDL_SCANCODE_KP_MINUS, Key::KeypadMinus},
    {SDL_SCANCODE_KP_PLUS, Key::KeypadPlus},
    {SDL_SCANCODE_KP_ENTER, Key::KeypadEnter},
    {SDL_SCANCODE_KP_EQUALS, Key::KeypadEquals},
    {SDL_SCANCODE_LCTRL, Key::LCtrl},
    {SDL_SCANCODE_RCTRL, Key::RCtrl},
    {SDL_SCANCODE_LSHIFT, Key::LShift},
    {SDL_SCANCODE_RSHIFT, Key::RShift},
    {SDL_SCANCODE_LALT, Key::LAlt},
    {SDL_SCANCODE_RALT, Key::RAlt},
    {SDL_SCANCODE_LGUI, Key::LGui},
    {SDL_SCANCODE_RGUI, Key::RGui},
    {SDL_SCANCODE_MODE, Key::AltGr},
    {SDL_SCANCODE_CAPSLOCK, Key::CapsLock},
    {SDL_SCANCODE_NUMLOCKCLEAR, Key::NumLock},
    {SDL_SCANCODE_SCROLLLOCK, Key::ScrollLock},
    {SDL_SCANCODE_PRINTSCREEN, Key::PrintScreen},
    {SDL_SCANCODE_PAUSE, Key::Pause},
    {SDL_SCANCODE_MENU, Key::Menu},
    {SDL_SCANCODE_APPLICATION, Key::Compose},
    {SDL_SCANCODE_POWER, Key::Power},
    {SDL_SCANCODE_HELP, Key::Help},
    {SDL_SCANCODE_SYSREQ, Key::SysReq}
  };

  return ScanCodeMap.maybe(scancode);
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
  bool directTouchGestures = true;
  bool gyroEnabled = false;
  float opacity = 0.35f;
  float size = 1.0f;
  float deadzone = 0.15f;
  float gyroSensitivity = 1.0f;
  bool gyroInvertX = false;
  bool gyroInvertY = false;
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
};

static String keysName(List<Key> const& keys) {
  StringList keyNames;
  for (auto key : keys)
    keyNames.append(KeyNames.getRight(key));
  return keyNames.join("+");
}

static String actionName(MobileTouchAction const& action) {
  switch (action.kind) {
    case MobileTouchActionKind::Key:
      return KeyNames.getRight(action.key);
    case MobileTouchActionKind::KeyMacro:
      return action.keys.empty() ? "Macro" : keysName(action.keys);
    case MobileTouchActionKind::MouseButton:
      return action.mouseButton == MouseButton::Left ? "Left Mouse"
          : action.mouseButton == MouseButton::Right ? "Right Mouse"
          : MouseButtonNames.getRight(action.mouseButton);
    case MobileTouchActionKind::MouseWheelUp:
      return "Scroll Up";
    case MobileTouchActionKind::MouseWheelDown:
      return "Scroll Down";
    case MobileTouchActionKind::GyroToggle:
      return "Gyro Toggle";
    default:
      return "None";
  }
}

static MobileTouchAction keyAction(Key key) {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::Key;
  action.key = key;
  return action;
}

static MobileTouchAction macroAction(List<Key> keys) {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::KeyMacro;
  action.keys = std::move(keys);
  return action;
}

static MobileTouchAction mouseAction(MouseButton button) {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::MouseButton;
  action.mouseButton = button;
  return action;
}

static MobileTouchAction wheelAction(bool up) {
  MobileTouchAction action;
  action.kind = up ? MobileTouchActionKind::MouseWheelUp : MobileTouchActionKind::MouseWheelDown;
  return action;
}

static MobileTouchAction gyroToggleAction() {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::GyroToggle;
  return action;
}

static MobileTouchAction noneAction() {
  MobileTouchAction action;
  action.kind = MobileTouchActionKind::None;
  return action;
}

static String pressModeName(MobileTouchPressMode mode) {
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

static MobileTouchPressMode pressModeFromName(String const& name, MobileTouchPressMode def = MobileTouchPressMode::Hold) {
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

static std::vector<MobileTouchElement> defaultTouchElements() {
  return {
    {"joystick", "Joystick", MobileTouchElementKind::Joystick, true, {0.14f, 0.78f}, 1.15f, keyAction(Key::Space), {}, {}, {}, {}},
    {"aimJoystick", "Aim", MobileTouchElementKind::AimJoystick, false, {0.66f, 0.78f}, 1.15f, noneAction(), {}, {}, {}, {}},
    {"leftHand", "L", MobileTouchElementKind::Button, true, {0.30f, 0.16f}, 0.92f, mouseAction(MouseButton::Left), {}, {}, {}, {}},
    {"rightHand", "R", MobileTouchElementKind::Button, true, {0.64f, 0.16f}, 0.92f, mouseAction(MouseButton::Right), {}, {}, {}, {}},
    {"jump", "J", MobileTouchElementKind::Button, true, {0.88f, 0.78f}, 1.00f, keyAction(Key::Space), {}, {}, {}, {}},
    {"interact", "E", MobileTouchElementKind::Button, true, {0.76f, 0.73f}, 0.92f, keyAction(Key::E), {}, {}, {}, {}},
    {"pause", "ESC", MobileTouchElementKind::Button, true, {0.10f, 0.15f}, 0.96f, keyAction(Key::Escape), {}, {}, {}, {}},
    {"chat", "T", MobileTouchElementKind::Button, true, {0.83f, 0.90f}, 0.72f, keyAction(Key::Return), {}, {}, {}, {}},
    {"tech", "F", MobileTouchElementKind::Button, true, {0.92f, 0.58f}, 0.86f, keyAction(Key::F), {}, {}, {}, {}},
    {"shift", "Shift", MobileTouchElementKind::Button, false, {0.66f, 0.88f}, 0.82f, keyAction(Key::LShift), {}, {}, {}, {}},
    {"ctrl", "Ctrl", MobileTouchElementKind::Button, false, {0.58f, 0.88f}, 0.82f, keyAction(Key::LCtrl), {}, {}, {}, {}},
    {"gyroToggle", "Gyro", MobileTouchElementKind::Button, false, {0.74f, 0.88f}, 0.82f, gyroToggleAction(), {}, {}, {}, {}, MobileTouchPressMode::SinglePress},
    {"dpad", "D-PAD", MobileTouchElementKind::DPad, false, {0.16f, 0.74f}, 1.05f, keyAction(Key::Space),
      keyAction(Key::W), keyAction(Key::S), keyAction(Key::A), keyAction(Key::D)}
  };
}

static String elementKindName(MobileTouchElementKind kind) {
  switch (kind) {
    case MobileTouchElementKind::Joystick:
      return "joystick";
    case MobileTouchElementKind::AimJoystick:
      return "aimJoystick";
    case MobileTouchElementKind::DPad:
      return "dpad";
    default:
      return "button";
  }
}

static MobileTouchElementKind elementKindFromName(String const& name) {
  if (name.equals("joystick", String::CaseInsensitive))
    return MobileTouchElementKind::Joystick;
  if (name.equals("aimJoystick", String::CaseInsensitive) || name.equals("aim", String::CaseInsensitive))
    return MobileTouchElementKind::AimJoystick;
  if (name.equals("dpad", String::CaseInsensitive))
    return MobileTouchElementKind::DPad;
  return MobileTouchElementKind::Button;
}

static List<Key> keysFromTouchAction(MobileTouchAction const& action) {
  if (action.kind == MobileTouchActionKind::Key)
    return {action.key};
  if (action.kind == MobileTouchActionKind::KeyMacro)
    return action.keys;
  return {};
}

static JsonArray jsonFromKeys(List<Key> const& keys) {
  JsonArray out;
  for (auto key : keys)
    out.append(KeyNames.getRight(key));
  return out;
}

static List<Key> keysFromText(String const& text) {
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

static List<Key> keysFromJson(Json const& json) {
  List<Key> keys;
  if (auto array = json.optArray("keys")) {
    for (auto const& keyJson : *array) {
      if (auto key = KeyNames.maybeLeft(keyJson.toString()))
        keys.append(*key);
    }
  }
  return keys;
}

static Json jsonFromTouchAction(MobileTouchAction const& action) {
  if (action.kind == MobileTouchActionKind::KeyMacro)
    return JsonObject{{"type", "keys"}, {"keys", jsonFromKeys(action.keys)}};
  if (action.kind == MobileTouchActionKind::MouseButton)
    return JsonObject{{"type", "mouse"}, {"button", MouseButtonNames.getRight(action.mouseButton)}};
  if (action.kind == MobileTouchActionKind::MouseWheelUp)
    return JsonObject{{"type", "wheel"}, {"direction", "up"}};
  if (action.kind == MobileTouchActionKind::MouseWheelDown)
    return JsonObject{{"type", "wheel"}, {"direction", "down"}};
  if (action.kind == MobileTouchActionKind::GyroToggle)
    return JsonObject{{"type", "gyroToggle"}};
  if (action.kind == MobileTouchActionKind::None)
    return JsonObject{{"type", "none"}};
  return JsonObject{{"type", "key"}, {"key", KeyNames.getRight(action.key)}};
}

static MobileTouchAction touchActionFromJson(Json const& json, MobileTouchAction def) {
  auto type = json.getString("type", "key");
  if (type.equals("keys", String::CaseInsensitive) || type.equals("macro", String::CaseInsensitive)) {
    auto keys = keysFromJson(json);
    return keys.empty() ? def : macroAction(keys);
  }
  if (type.equals("mouse", String::CaseInsensitive))
    return mouseAction(MouseButtonNames.valueLeft(json.getString("button", MouseButtonNames.getRight(def.mouseButton)), def.mouseButton));
  if (type.equals("wheel", String::CaseInsensitive))
    return wheelAction(!json.getString("direction", "up").equals("down", String::CaseInsensitive));
  if (type.equals("gyroToggle", String::CaseInsensitive) || type.equals("gyro", String::CaseInsensitive))
    return gyroToggleAction();
  if (type.equals("none", String::CaseInsensitive))
    return noneAction();
  auto keyName = json.getString("key", KeyNames.getRight(def.key));
  return keyAction(KeyNames.valueLeft(keyName, def.key));
}

static Json jsonFromTouchElement(MobileTouchElement const& element) {
  JsonObject out{
    {"id", element.id},
    {"label", element.label},
    {"kind", elementKindName(element.kind)},
    {"enabled", element.enabled},
    {"position", JsonArray{element.position[0], element.position[1]}},
    {"size", element.size},
    {"pressMode", pressModeName(element.pressMode)},
    {"aimSensitivity", element.aimSensitivity},
    {"action", jsonFromTouchAction(element.action)}
  };
  if (element.kind == MobileTouchElementKind::DPad) {
    out["upAction"] = jsonFromTouchAction(element.upAction);
    out["downAction"] = jsonFromTouchAction(element.downAction);
    out["leftAction"] = jsonFromTouchAction(element.leftAction);
    out["rightAction"] = jsonFromTouchAction(element.rightAction);
  }
  return out;
}

static MobileTouchElement touchElementFromJson(Json const& json, MobileTouchElement def) {
  def.id = json.getString("id", def.id);
  def.label = json.getString("label", def.label);
  def.kind = elementKindFromName(json.getString("kind", elementKindName(def.kind)));
  def.enabled = json.getBool("enabled", def.enabled);
  def.size = json.getFloat("size", def.size);
  def.pressMode = pressModeFromName(json.getString("pressMode", pressModeName(def.pressMode)), def.pressMode);
  def.aimSensitivity = json.getFloat("aimSensitivity", def.aimSensitivity);
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

static JsonArray jsonFromTouchElements(std::vector<MobileTouchElement> const& elements) {
  JsonArray out;
  for (auto const& element : elements)
    out.append(jsonFromTouchElement(element));
  return out;
}

static std::vector<MobileTouchElement> touchElementsFromConfig(Json const& config) {
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
  std::vector<MobileTouchElement> touchElements;
  bool touchManagerOpen = false;
  bool touchPreviewOpen = false;
  int selectedTouchElement = 0;
  String touchLabelBufferElementId;
  char touchLabelBuffer[64] = {};
  StringMap<std::array<char, 256>> touchActionBuffers;
  bool newTouchButtonPopup = false;
  int newTouchActionIndex = 0;
  float newTouchButtonSize = 1.0f;
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
  explicit MobileTouchInputAdapter(Vec2U* windowSize, Vec2U* renderCanvasSize = nullptr, float* displayScale = nullptr, SafeAreaInsets* safeArea = nullptr)
    : m_windowSize(windowSize), m_renderCanvasSize(renderCanvasSize), m_displayScalePtr(displayScale), m_safeAreaPtr(safeArea) {}

  void setConfig(MobileTouchConfig config) {
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
    return m_config.gyroEnabled;
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

    for (auto const& pair : m_keyHoldCounts.pairs()) {
      if (pair.second > 0)
        emitEvent(KeyUpEvent{pair.first});
    }
    for (auto const& pair : m_mouseHoldCounts.pairs()) {
      if (pair.second > 0) {
        syncVirtualAimCursor();
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
      emitMouseUp(m_primaryTouchPos);
    if (m_secondaryMouseHeld)
      emitMouseUp(m_secondaryTouchPos, MouseButton::Right);

    m_primaryHeld = false;
    m_primaryMouseHeld = false;
    m_aimFinger = 0;
    m_primaryPausedForSecondary = false;

    m_secondaryHeld = false;
    m_secondaryMouseHeld = false;
    m_secondaryFinger = 0;
  }

  void drawOverlay() {
    if (!m_config.enabled)
      return;

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
    ImU32 base = IM_COL32(255, 255, 255, (int)(180.0f * std::clamp(m_config.opacity, 0.0f, 1.0f)));
    ImU32 fill = IM_COL32(80, 160, 255, (int)(140.0f * std::clamp(m_config.opacity, 0.0f, 1.0f)));

    for (auto const& element : m_elements) {
      if (!element.enabled)
        continue;

      Vec2F center = elementCenter(element);
      float elementRadius = radius * std::clamp(element.size, 0.45f, 2.4f);
      float drawRadius = elementRadius / radiusScale;

      if (element.kind == MobileTouchElementKind::Joystick) {
        draw->AddCircle(ip(center), drawRadius, base, 48, 3.0f);
        if (m_joystickActive && m_joystickElementId == element.id)
          draw->AddCircleFilled(ip(m_joystickCurrent), drawRadius * 0.45f, fill, 32);
      } else if (element.kind == MobileTouchElementKind::AimJoystick) {
        draw->AddCircle(ip(center), drawRadius, base, 48, 3.0f);
        if (m_aimJoystickActive && m_aimJoystickElementId == element.id)
          draw->AddCircleFilled(ip(m_aimJoystickCurrent), drawRadius * 0.35f, fill, 32);
      } else if (element.kind == MobileTouchElementKind::DPad) {
        drawDPad(draw, ip(center), drawRadius, element.id, base, fill);
      } else {
        bool held = heldElement(element.id)
            || (element.action.kind == MobileTouchActionKind::GyroToggle && m_config.gyroEnabled && m_gyroRuntimeEnabled);
        drawButton(draw, ip(center), drawRadius * 0.55f, held, element.label.utf8Ptr(), base, fill);
      }
    }
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

      // Let right-click behavior take precedence while secondary gesture is held.
      if (m_primaryMouseHeld) {
        emitMouseUp(m_primaryTouchPos);
        m_primaryMouseHeld = false;
        m_primaryPausedForSecondary = true;
      }

      m_secondaryMouseHeld = true;
      emitMouseMove(m_secondaryTouchPos);
      emitMouseDown(m_secondaryTouchPos, MouseButton::Right);
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
      m_primaryTouchPos = pos;
      emitMouseMove(pos);
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
      emitMouseUp(m_primaryTouchPos);
      m_primaryMouseHeld = false;
    }
    if (m_secondaryMouseHeld) {
      emitMouseUp(m_secondaryTouchPos, MouseButton::Right);
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

    float pixelsPerRadian = controlRadius() * 5.0f * std::clamp(m_config.gyroSensitivity, 0.10f, 5.0f);
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
      float speed = controlRadius() * 0.22f * sensitivity * std::clamp(m_aimVec.magnitude(), 0.25f, 1.0f);
      delta += m_aimVec * speed;
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
    syncVirtualAimCursor(true);
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
      if (m_config.gyroEnabled) {
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
        syncVirtualAimCursor();
        emitEvent(MouseButtonDownEvent{button, mouseActionPosition()});
      }
    } else if (!desired && m_mouseActionOwners.contains(token)) {
      m_mouseActionOwners.remove(token);
      unsigned count = m_mouseHoldCounts.value(button, 0);
      if (count <= 1) {
        m_mouseHoldCounts.remove(button);
        syncVirtualAimCursor();
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
      for (auto key : action.keys)
        setKeyOwner(owner, key, desired);
    } else if (action.kind == MobileTouchActionKind::MouseButton) {
      setMouseOwner(owner, action.mouseButton, desired);
    } else if (desired && action.kind == MobileTouchActionKind::MouseWheelUp) {
      syncVirtualAimCursor();
      emitEvent(MouseWheelEvent{MouseWheel::Up, mouseActionPosition()});
    } else if (desired && action.kind == MobileTouchActionKind::MouseWheelDown) {
      syncVirtualAimCursor();
      emitEvent(MouseWheelEvent{MouseWheel::Down, mouseActionPosition()});
    }
  }

  bool actionNeedsRelease(MobileTouchAction const& action) const {
    return action.kind == MobileTouchActionKind::Key || action.kind == MobileTouchActionKind::KeyMacro || action.kind == MobileTouchActionKind::MouseButton;
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

  void syncVirtualAimCursor(bool force = false) {
    if (!m_hasAimJoystickTarget)
      return;

    Vec2F inputPosition = toInputSpace(m_aimJoystickTarget);
    if (!force && m_hasCursorInputPosition && (inputPosition - m_cursorInputPosition).magnitudeSquared() <= 0.01f)
      return;

    m_cursorInputPosition = inputPosition;
    m_hasCursorInputPosition = true;
    emitEvent(MouseMoveEvent{{0, 0}, m_cursorInputPosition});
  }

  Vec2F mouseActionPosition() const {
    return m_hasCursorInputPosition ? m_cursorInputPosition : m_lastPointerInput;
  }

  void emitMouseMove(Vec2F const& pos) {
    m_cursorInputPosition = toInputSpace(pos);
    m_hasCursorInputPosition = true;
    m_aimJoystickTarget = pos;
    m_hasAimJoystickTarget = true;
    emitEvent(MouseMoveEvent{{0, 0}, m_cursorInputPosition});
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

  void drawDPad(ImDrawList* draw, ImVec2 center, float radius, String const& id, ImU32 base, ImU32 fill) const {
    float arm = radius * 0.42f;
    auto drawArm = [&](char const* suffix, ImVec2 c, char const* label) {
      String heldId = id + ":" + suffix;
      bool held = m_dpadHeld.contains(heldId);
      draw->AddRect(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), base, arm * 0.25f, 0, 3.0f);
      if (held)
        draw->AddRectFilled(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm), fill, arm * 0.25f);
      draw->AddText(ImVec2(c.x - arm * 0.22f, c.y - arm * 0.38f), base, label);
    };

    drawArm("up", ImVec2(center.x, center.y - arm * 1.25f), "W");
    drawArm("down", ImVec2(center.x, center.y + arm * 1.25f), "S");
    drawArm("left", ImVec2(center.x - arm * 1.25f, center.y), "A");
    drawArm("right", ImVec2(center.x + arm * 1.25f, center.y), "D");
  }

  Vec2U* m_windowSize;
  Vec2U* m_renderCanvasSize = nullptr;
  [[maybe_unused]] float* m_displayScalePtr = nullptr;
  SafeAreaInsets* m_safeAreaPtr = nullptr;
  MobileTouchConfig m_config;
  std::vector<MobileTouchElement> m_elements = defaultTouchElements();
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

    m_legacyStorageRoot = defaultMobileStorageRoot();
    m_storageRoot = writableMobileStorageRoot(m_legacyStorageRoot);
    m_platformServices = MobilePlatformServices::create(m_storageRoot);

    LauncherState launcher;
    loadLauncherState(launcher);

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
        break;

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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR))
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

    state.touchConfig.enabled = config.queryBool("touch.enabled", true);
    state.touchConfig.directTouchGestures = config.queryBool("touch.directTouchGestures", true);
    state.touchConfig.gyroEnabled = config.queryBool("touch.gyroEnabled", false);
    state.touchConfig.opacity = config.queryFloat("touch.opacity", 0.35f);
    state.touchConfig.size = config.queryFloat("touch.size", 1.0f);
    state.touchConfig.deadzone = config.queryFloat("touch.deadzone", 0.15f);
    state.touchConfig.gyroSensitivity = config.queryFloat("touch.gyroSensitivity", 1.0f);
    state.touchConfig.gyroInvertX = config.queryBool("touch.gyroInvertX", false);
    state.touchConfig.gyroInvertY = config.queryBool("touch.gyroInvertY", false);
    state.touchElements = touchElementsFromConfig(config);

    state.canLaunch = File::isFile(state.packedPakPath);
    state.lastStatus = state.canLaunch ? "Using existing packed.pak" : "Please import packed.pak";
  }

  std::vector<pair<char const*, MobileTouchAction>> touchActionChoices() const {
    return {
      {"Left Hand / Mouse Left", mouseAction(MouseButton::Left)},
      {"Right Hand / Mouse Right", mouseAction(MouseButton::Right)},
      {"Jump / J (Space)", keyAction(Key::Space)},
      {"Interact / E", keyAction(Key::E)},
      {"Escape / Pause", keyAction(Key::Escape)},
      {"Chat / T (Enter)", keyAction(Key::Return)},
      {"Tech / F", keyAction(Key::F)},
      {"Swap Hotbar / X", keyAction(Key::X)},
      {"Shift", keyAction(Key::LShift)},
      {"Ctrl", keyAction(Key::LCtrl)},
      {"Move Up / W", keyAction(Key::W)},
      {"Move Down / S", keyAction(Key::S)},
      {"Move Left / A", keyAction(Key::A)},
      {"Move Right / D", keyAction(Key::D)},
      {"Inventory / I", keyAction(Key::I)},
      {"Codex / L", keyAction(Key::L)},
      {"Quest / J", keyAction(Key::J)},
      {"Crafting / C", keyAction(Key::C)},
      {"Action Bar 1", keyAction(Key::One)},
      {"Action Bar 2", keyAction(Key::Two)},
      {"Action Bar 3", keyAction(Key::Three)},
      {"Action Bar 4", keyAction(Key::Four)},
      {"Action Bar 5", keyAction(Key::Five)},
      {"Scroll Up", wheelAction(true)},
      {"Scroll Down", wheelAction(false)},
      {"Gyro Toggle", gyroToggleAction()},
      {"No Action", noneAction()}
    };
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
    return true;
  }

  int touchActionIndex(MobileTouchAction const& action) const {
    auto choices = touchActionChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
      auto const& candidate = choices[i].second;
      if (touchActionEqual(candidate, action))
        return (int)i;
    }
    return 0;
  }

  String touchActionKeysText(MobileTouchAction const& action) const {
    return keysName(keysFromTouchAction(action));
  }

  void renderTouchActionCombo(LauncherState& state, char const* label, MobileTouchAction& action, String const& bufferId) {
    auto choices = touchActionChoices();
    int index = touchActionIndex(action);
    if (ImGui::BeginCombo(label, choices[index].first)) {
      for (size_t i = 0; i < choices.size(); ++i) {
        bool selected = index == (int)i;
        if (ImGui::Selectable(choices[i].first, selected)) {
          index = (int)i;
          action = choices[i].second;
          if (auto buffer = state.touchActionBuffers.ptr(bufferId))
            std::snprintf(buffer->data(), buffer->size(), "%s", touchActionKeysText(action).utf8Ptr());
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    if (!state.touchActionBuffers.contains(bufferId)) {
      state.touchActionBuffers[bufferId] = {};
      auto& buffer = state.touchActionBuffers[bufferId];
      std::snprintf(buffer.data(), buffer.size(), "%s", touchActionKeysText(action).utf8Ptr());
    }

    auto& buffer = state.touchActionBuffers[bufferId];
    ImGui::PushID(bufferId.utf8Ptr());
    ImGui::InputTextWithHint("Custom keys", "Example: LShift+F1 or Ctrl,Alt,E", buffer.data(), buffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Apply")) {
      auto keys = keysFromText(String(buffer.data()));
      if (keys.size() == 1)
        action = keyAction(keys[0]);
      else if (!keys.empty())
        action = macroAction(keys);
    }
    ImGui::PopID();
  }

  void renderTouchPressModeCombo(MobileTouchElement& element) {
    std::vector<pair<char const*, MobileTouchPressMode>> choices{
      {"Single press", MobileTouchPressMode::SinglePress},
      {"Rapid fire / repeat", MobileTouchPressMode::Repeat},
      {"Hold", MobileTouchPressMode::Hold},
      {"Toggle", MobileTouchPressMode::Toggle}
    };
    int index = 0;
    for (int i = 0; i < (int)choices.size(); ++i) {
      if (choices[i].second == element.pressMode) {
        index = i;
        break;
      }
    }
    if (ImGui::BeginCombo("Button behavior", choices[index].first)) {
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

  void renderTouchElementPreview(LauncherState& state, ImDrawList* draw, ImVec2 canvasMin, ImVec2 canvasSize, int index) {
    auto& element = state.touchElements[index];
    if (!element.enabled)
      return;

    float baseRadius = std::min(canvasSize.x, canvasSize.y) * 0.075f * state.touchConfig.size;
    float radius = baseRadius * std::clamp(element.size, 0.45f, 2.4f);
    ImVec2 center(canvasMin.x + element.position[0] * canvasSize.x, canvasMin.y + element.position[1] * canvasSize.y);
    ImU32 base = IM_COL32(255, 255, 255, (int)(210.0f * std::clamp(state.touchConfig.opacity, 0.0f, 1.0f)));
    ImU32 fill = index == state.selectedTouchElement
        ? IM_COL32(120, 190, 255, 120)
        : IM_COL32(80, 160, 255, 70);

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
      draw->AddRect(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius), base, arm * 0.25f, 0, 3.0f);
      draw->AddText(ImVec2(center.x - radius * 0.38f, center.y - radius * 0.18f), base, "D-PAD");
    } else if (element.kind == MobileTouchElementKind::Joystick || element.kind == MobileTouchElementKind::AimJoystick) {
      draw->AddCircle(center, radius, base, 48, 3.0f);
      draw->AddCircleFilled(center, radius * 0.34f, fill, 32);
    } else {
      draw->AddCircleFilled(center, radius * 0.55f, fill, 32);
      draw->AddCircle(center, radius * 0.55f, base, 48, 3.0f);
      draw->AddText(ImVec2(center.x - radius * 0.22f, center.y - radius * 0.18f), base, element.label.utf8Ptr());
    }
  }

  void renderTouchPreview(LauncherState& state, ImVec2 displaySize) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(displaySize, ImGuiCond_Always);
    ImGui::Begin("Touch Layout Preview", nullptr,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetWindowPos();
    draw->AddRectFilled(min, ImVec2(min.x + displaySize.x, min.y + displaySize.y), IM_COL32(15, 18, 24, 235));

    ImGui::SetCursorScreenPos(ImVec2(min.x + 16.0f, min.y + 16.0f));
    ImGui::BeginChild("TouchPreviewToolbar", ImVec2(std::min(520.0f, displaySize.x - 32.0f), 150.0f), true);
    ImGui::Text("Touch Layout Preview");
    if (state.selectedTouchElement >= 0 && state.selectedTouchElement < (int)state.touchElements.size()) {
      auto& selected = state.touchElements[state.selectedTouchElement];
      ImGui::Text("Selected: %s", selected.label.utf8Ptr());
      ImGui::SliderFloat("Selected size", &selected.size, 0.45f, 2.4f);
    }
    if (ImGui::Button("Done"))
      state.touchPreviewOpen = false;
    ImGui::EndChild();

    // Register control drag hitboxes after the toolbar so overlapping controls
    // can always be pulled out from underneath it.
    for (int i = 0; i < (int)state.touchElements.size(); ++i)
      renderTouchElementPreview(state, draw, min, displaySize, i);

    ImGui::End();
  }

  void renderTouchManager(LauncherState& state) {
    ImGui::Text("Touch Controls Manager");
    ImGui::Separator();
    if (ImGui::Button("Back to Launcher"))
      state.touchManagerOpen = false;
    ImGui::SameLine();
    if (ImGui::Button("Preview / Adjust Layout"))
      state.touchPreviewOpen = true;
    ImGui::SameLine();
    if (ImGui::Button("Save"))
      persistLauncherState(state);

    ImGui::Checkbox("Enable touch overlay", &state.touchConfig.enabled);
    ImGui::Checkbox("Enable direct screen touch gestures", &state.touchConfig.directTouchGestures);
    ImGui::Checkbox("Enable gyro aim", &state.touchConfig.gyroEnabled);
    ImGui::SliderFloat("Overlay opacity", &state.touchConfig.opacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Global control size", &state.touchConfig.size, 0.6f, 1.8f);
    ImGui::SliderFloat("Joystick deadzone", &state.touchConfig.deadzone, 0.0f, 0.6f);
    ImGui::SliderFloat("Gyro sensitivity", &state.touchConfig.gyroSensitivity, 0.10f, 5.0f);
    ImGui::Checkbox("Invert gyro X axis", &state.touchConfig.gyroInvertX);
    ImGui::Checkbox("Invert gyro Y axis", &state.touchConfig.gyroInvertY);

    if (ImGui::Button("New Button")) {
      state.newTouchButtonPopup = true;
      state.newTouchActionIndex = 0;
      state.newTouchButtonSize = 1.0f;
      ImGui::OpenPopup("New Touch Button");
    }

    if (ImGui::BeginPopupModal("New Touch Button", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      auto choices = touchActionChoices();
      if (state.newTouchActionIndex < 0 || state.newTouchActionIndex >= (int)choices.size())
        state.newTouchActionIndex = 0;
      if (ImGui::BeginCombo("Interaction", choices[state.newTouchActionIndex].first)) {
        for (int i = 0; i < (int)choices.size(); ++i) {
          bool selected = state.newTouchActionIndex == i;
          if (ImGui::Selectable(choices[i].first, selected))
            state.newTouchActionIndex = i;
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::SliderFloat("Button size", &state.newTouchButtonSize, 0.45f, 2.4f);
      if (ImGui::Button("Create", ImVec2(140.0f, 0.0f))) {
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
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f)))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    ImGui::Separator();
    if (ImGui::BeginChild("TouchControlList", ImVec2(0, -70.0f), true)) {
      for (int i = 0; i < (int)state.touchElements.size(); ++i) {
        auto& element = state.touchElements[i];
        ImGui::PushID(element.id.utf8Ptr());
        ImGui::Checkbox("##enabled", &element.enabled);
        ImGui::SameLine();
        bool selected = state.selectedTouchElement == i;
        String listLabel = element.label.empty() ? String("(blank)") : element.label;
        if (ImGui::Selectable(listLabel.utf8Ptr(), selected, ImGuiSelectableFlags_AllowItemOverlap))
          state.selectedTouchElement = i;
        if (selected) {
          ImGui::Indent();
          if (state.touchLabelBufferElementId != element.id) {
            state.touchLabelBufferElementId = element.id;
            std::snprintf(state.touchLabelBuffer, sizeof(state.touchLabelBuffer), "%s", element.label.utf8Ptr());
          }
          if (element.kind == MobileTouchElementKind::Button) {
            if (ImGui::InputText("Displayed text", state.touchLabelBuffer, sizeof(state.touchLabelBuffer)))
              element.label = String(state.touchLabelBuffer).trim();
          }
          ImGui::SliderFloat("Size", &element.size, 0.45f, 2.4f);
          float position[2] = {element.position[0], element.position[1]};
          if (ImGui::SliderFloat2("Position", position, 0.03f, 0.97f))
            element.position = {position[0], position[1]};
          if (element.kind == MobileTouchElementKind::Button) {
            renderTouchActionCombo(state, "Interaction", element.action, element.id + ":action");
            renderTouchPressModeCombo(element);
          }
          else if (element.kind == MobileTouchElementKind::DPad) {
            renderTouchActionCombo(state, "Up", element.upAction, element.id + ":up");
            renderTouchActionCombo(state, "Down", element.downAction, element.id + ":down");
            renderTouchActionCombo(state, "Left", element.leftAction, element.id + ":left");
            renderTouchActionCombo(state, "Right", element.rightAction, element.id + ":right");
          } else if (element.kind == MobileTouchElementKind::AimJoystick) {
            ImGui::SliderFloat("Aim sensitivity", &element.aimSensitivity, 0.25f, 4.0f);
            ImGui::TextDisabled("Aim joystick moves the virtual cursor only.");
          } else {
            ImGui::TextDisabled("Joystick sends movement keys.");
          }
          ImGui::Unindent();
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
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

    if (state.touchManagerOpen) {
      renderTouchManager(state);
    } else if (state.modManagerOpen) {
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
      ImGui::SameLine();
      if (ImGui::Button("Touch Controls")) {
        state.touchManagerOpen = true;
        state.selectedTouchElement = std::clamp(state.selectedTouchElement, 0, (int)state.touchElements.size() - 1);
      }
      ImGui::SameLine();
      if (ImGui::Button("Export Diagnostics")) {
        runLauncherAction("Export diagnostics", [&]() {
          if (auto svc = m_platformServices->externalFileAccessService()) {
            if (svc->exportDiagnostics()) {
              state.lastStatus = "Diagnostics export opened.";
              state.lastError.clear();
            } else {
              state.lastStatus = "Diagnostics export unavailable.";
              state.lastError = "Could not open the native diagnostics share sheet.";
            }
          } else {
            state.lastStatus = "Diagnostics export unavailable.";
            state.lastError = "ExternalFileAccessService is unavailable on this platform build.";
          }
        });
      }

      ImGui::Separator();
      ImGui::Text("Touch Controls: %s", state.touchConfig.enabled ? "enabled" : "disabled");

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

    if (state.touchPreviewOpen)
      renderTouchPreview(state, displaySize);

    syncImGuiTextInputState();

    ImGui::Render();
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
      persistLauncherState(state);
    }

    return launchPressed;
  }

  void persistLauncherState(LauncherState const& state) {
    Json config = JsonObject{
      {"packedPakPath", state.packedPakPath},
      {"touch", JsonObject{
        {"enabled", state.touchConfig.enabled},
        {"directTouchGestures", state.touchConfig.directTouchGestures},
        {"gyroEnabled", state.touchConfig.gyroEnabled},
        {"opacity", state.touchConfig.opacity},
        {"size", state.touchConfig.size},
        {"deadzone", state.touchConfig.deadzone},
        {"gyroSensitivity", state.touchConfig.gyroSensitivity},
        {"gyroInvertX", state.touchConfig.gyroInvertX},
        {"gyroInvertY", state.touchConfig.gyroInvertY},
        {"elements", jsonFromTouchElements(state.touchElements)}
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
            {"directTouchGestures", state.touchConfig.directTouchGestures},
            {"gyroEnabled", state.touchConfig.gyroEnabled},
            {"opacity", state.touchConfig.opacity},
            {"size", state.touchConfig.size},
            {"deadzone", state.touchConfig.deadzone},
            {"gyroSensitivity", state.touchConfig.gyroSensitivity},
            {"gyroInvertX", state.touchConfig.gyroInvertX},
            {"gyroInvertY", state.touchConfig.gyroInvertY},
            {"elements", jsonFromTouchElements(state.touchElements)},
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
      m_touchAdapter = make_unique<MobileTouchInputAdapter>(&m_windowSize, &m_renderCanvasSize, &m_displayScale, &m_safeArea);

      if (auto configService = m_platformServices->launchConfigService()) {
        auto cfg = configService->loadLauncherConfig();
        MobileTouchConfig touch;
        touch.enabled = cfg.queryBool("touch.enabled", true);
        touch.directTouchGestures = cfg.queryBool("touch.directTouchGestures", true);
        touch.gyroEnabled = cfg.queryBool("touch.gyroEnabled", false);
        touch.opacity = cfg.queryFloat("touch.opacity", 0.35f);
        touch.size = cfg.queryFloat("touch.size", 1.0f);
        touch.deadzone = cfg.queryFloat("touch.deadzone", 0.15f);
        touch.gyroSensitivity = cfg.queryFloat("touch.gyroSensitivity", 1.0f);
        touch.gyroInvertX = cfg.queryBool("touch.gyroInvertX", false);
        touch.gyroInvertY = cfg.queryBool("touch.gyroInvertY", false);
        m_touchAdapter->setConfig(touch);
        m_touchAdapter->setElements(touchElementsFromConfig(cfg));
        syncGyroSensor(touch.gyroEnabled);
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
    glViewport((int)m_safeArea.left, (int)m_safeArea.bottom, (int)gameCanvasSize()[0], (int)gameCanvasSize()[1]);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);
  }

  void runGameLoop() {
    m_updateTicker.reset();
    m_renderTicker.reset();

#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    // Notify the game of the canvas size before the first frame.  syncWindowMetrics
    // won't fire windowChanged when the safe area is already stable from
    // setupWindowAndRenderer, so the game would otherwise use the full physical
    // screen size for UI layout while the shader clips to the smaller canvas.
    if (m_application)
      m_application->windowChanged(WindowMode::Normal, m_renderCanvasSize);
#endif

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
    syncGyroSensor(false);
    // Do not reset m_application: startup() can be called again for a new
    // session without rebuilding the entire object.
  }

  Vec2F externalMouseInputPosition(float x, float y) const {
    // External mice report physical window coordinates, while the game is
    // rendered into the safe-area canvas. Touch input already goes through the
    // touch adapter's canvas conversion; keep this path for real mouse devices.
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

  void syncGyroSensor(bool enabled) {
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
    }
    return;
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
#else
        m_quitRequested = true;
        continue;
#endif
      }

      if (event.type == SDL_EVENT_WINDOW_RESIZED
          || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
          || event.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED) {
        if (m_touchAdapter)
          m_touchAdapter->cancelAll();
        syncWindowMetrics(true);
        continue;
      }

      if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        if (m_touchAdapter)
          m_touchAdapter->cancelAll();
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
      }

      if (input)
        events.append(input.take());
    }

    syncWindowMetrics(true);

    updateGyroInput();
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
          || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED
          || event.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED) {
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
    Vec2U canvas = gameCanvasSize();
    return ImVec2((float)canvas[0] / pixelScale, (float)canvas[1] / pixelScale);
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

  void syncImGuiTextInputState() {
#if defined(STAR_SYSTEM_ANDROID) || defined(STAR_SYSTEM_IOS)
    bool wantTextInput = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
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
