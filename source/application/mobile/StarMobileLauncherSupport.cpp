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

#include "mobile/StarMobileLauncherSupport.hpp"

namespace Star {

char const* const LauncherLangDirectory = "lang";
char const* const DefaultLauncherLocale = "en_US";
char const* const SystemLocaleMarker = "__system__";
char const* const PreferredLauncherFontPath = "hobo.ttf";
char const* const BundledLauncherFontPath = "opensb/hobo.ttf";

#ifdef STAR_SYSTEM_ANDROID
void androidLogInfo(char const* fmt, ...);
#elif defined(STAR_SYSTEM_IOS)
void androidLogInfo(char const* fmt, ...);
#else
inline void androidLogInfo(char const*, ...) {}
#endif

String normalizeLauncherLocale(String locale) {
  locale = locale.trim();
  if (locale.empty())
    return DefaultLauncherLocale;

  locale = locale.replace("-", "_");
  auto parts = locale.split('_');
  if (parts.empty())
    return DefaultLauncherLocale;

  auto language = parts.at(0).toLower();
  if (language.empty())
    return DefaultLauncherLocale;

  String region;
  if (parts.size() >= 2)
    region = parts.at(1).toUpper();

  if (region.empty()) {
    if (language == "zh")
      region = "CN";
    else if (language == "en")
      region = "US";
  }

  return region.empty() ? language : strf("{}_{}", language, region);
}

String parseLauncherLangLine(String const& line, StringMap<String>& out) {
  auto trimmed = line.trim();
  if (trimmed.empty() || trimmed.beginsWith("#") || trimmed.beginsWith("//"))
    return {};

  auto separator = trimmed.find('=');
  if (separator == NPos)
    return trimmed;

  String key = trimmed.substr(0, separator).trim();
  String value = trimmed.substr(separator + 1).trim();
  if (!key.empty())
    out[key] = value;
  return {};
}

StringMap<String> loadLauncherLangFile(String const& path) {
  StringMap<String> out;
  if (!File::isFile(path))
    return out;

  for (auto const& line : File::readFileString(path).splitLines()) {
    if (auto invalid = parseLauncherLangLine(line, out); !invalid.empty())
      androidLogInfo("Ignoring invalid launcher lang line in %s: %s", path.utf8Ptr(), invalid.utf8Ptr());
  }

  return out;
}

String launcherTextStatic(String const& key, String const& fallback) {
  static StringMap<String> translations = loadLauncherLangFile(File::relativeTo(LauncherLangDirectory, strf("{}.lang", DefaultLauncherLocale)));
  if (auto value = translations.ptr(key))
    return *value;
  return fallback.empty() ? key : fallback;
}

String loadPreferredLauncherLocale() {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = reinterpret_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env)
    return DefaultLauncherLocale;

  jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity)
    return DefaultLauncherLocale;

  jclass cls = env->GetObjectClass(activity);
  if (!cls) {
    env->DeleteLocalRef(activity);
    return DefaultLauncherLocale;
  }

  jmethodID method = env->GetStaticMethodID(cls, "getPreferredLocales", "()Ljava/lang/String;");
  if (!method) {
    androidLogInfo("loadPreferredLauncherLocale: getPreferredLocales method missing");
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return DefaultLauncherLocale;
  }

  jstring result = (jstring)env->CallStaticObjectMethod(cls, method);
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    androidLogInfo("loadPreferredLauncherLocale: Java exception, fallback to default");
    return DefaultLauncherLocale;
  }

  String locale = DefaultLauncherLocale;
  if (result) {
    char const* utf = env->GetStringUTFChars(result, nullptr);
    if (utf)
      locale = utf;
    if (utf)
      env->ReleaseStringUTFChars(result, utf);
    env->DeleteLocalRef(result);
  }

  auto locales = locale.split(',');
  auto normalized = normalizeLauncherLocale(locales.empty() ? locale : locales.at(0));
  androidLogInfo("loadPreferredLauncherLocale: raw=%s normalized=%s", locale.utf8Ptr(), normalized.utf8Ptr());
  return normalized;
#elif defined(STAR_SYSTEM_IOS)
  int localeCount = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&localeCount);
  String locale = DefaultLauncherLocale;
  if (locales && localeCount > 0 && locales[0] && locales[0]->language) {
    locale = locales[0]->country ? strf("{}_{}", locales[0]->language, locales[0]->country) : String(locales[0]->language);
  }
  if (locales)
    SDL_free(locales);
  return normalizeLauncherLocale(locale);
#else
  return DefaultLauncherLocale;
#endif
}

String resolveLauncherLocaleChoice(String const& localeChoice) {
  auto choice = localeChoice.trim();
  if (choice.empty() || choice == SystemLocaleMarker)
    return loadPreferredLauncherLocale();
  return normalizeLauncherLocale(choice);
}

std::vector<String> launcherCjkFontCandidates() {
  return {
#ifdef STAR_SYSTEM_ANDROID
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansSC-Regular.otf",
    "/system/fonts/NotoSansHans-Regular.otf",
    "/system/fonts/DroidSansFallback.ttf",
    "/system/fonts/SourceHanSansCN-Regular.otf",
#elif defined(STAR_SYSTEM_IOS)
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/LanguageSupport/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
#endif
  };
}

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

bool hasAndroidGyroSensor() {
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

  jmethodID method = env->GetStaticMethodID(cls, "hasGyroSensor", "()Z");
  if (!method) {
    env->DeleteLocalRef(cls);
    return false;
  }

  bool result = env->CallStaticBooleanMethod(cls, method);
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

void convertEventToRenderCoordinatesIfPossible(SDL_Window* window, SDL_Event* event) {
  if (!window || !event)
    return;

  // This app uses an OpenGL context directly (no SDL_Renderer), so on some
  // mobile backends SDL_GetRenderer(window) returns null. Guard conversion.
  SDL_Renderer* renderer = SDL_GetRenderer(window);
  if (renderer)
    SDL_ConvertEventToRenderCoordinates(renderer, event);
}

bool isTouchDerivedMouseEvent(SDL_Event const& event) {
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

bool shouldCancelMobileTouchState(SDL_Event const& event) {
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
    case SDL_GAMEPAD_BUTTON_GUIDE: return ControllerButton::Guide;
    case SDL_GAMEPAD_BUTTON_START: return ControllerButton::Start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ControllerButton::LeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ControllerButton::RightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ControllerButton::LeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ControllerButton::RightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ControllerButton::DPadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ControllerButton::DPadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ControllerButton::DPadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ControllerButton::DPadRight;
    case SDL_GAMEPAD_BUTTON_MISC1: return ControllerButton::Misc1;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return ControllerButton::Paddle1;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return ControllerButton::Paddle2;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return ControllerButton::Paddle3;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return ControllerButton::Paddle4;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return ControllerButton::Touchpad;
    default: return ControllerButton::Invalid;
  }
}

KeyMod noMods() {
  return KeyMod::NoMod;
}


}
