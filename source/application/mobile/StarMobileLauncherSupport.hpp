#pragma once

#include "StarInputEvent.hpp"
#include "StarJson.hpp"
#include "StarString.hpp"

#include "SDL3/SDL.h"

#include <array>
#include <vector>

namespace Star {

extern char const* const LauncherLangDirectory;
extern char const* const DefaultLauncherLocale;
extern char const* const SystemLocaleMarker;
extern char const* const PreferredLauncherFontPath;
extern char const* const BundledLauncherFontPath;

void androidLogInfo(char const* fmt, ...);
String normalizeLauncherLocale(String locale);
String parseLauncherLangLine(String const& line, StringMap<String>& out);
StringMap<String> loadLauncherLangFile(String const& path);
String launcherTextStatic(String const& key, String const& fallback = {});
String loadPreferredLauncherLocale();
String resolveLauncherLocaleChoice(String const& localeChoice);
std::vector<String> launcherCjkFontCandidates();
bool samePath(String const& a, String const& b);
bool filesMatch(String const& leftPath, String const& rightPath);
void removeIfEmptyDirectory(String const& directory);
void migrateDirectoryFiles(String const& sourceDirectory, String const& targetDirectory);
#ifdef STAR_SYSTEM_ANDROID
bool setAndroidGyroSensorEnabled(bool enabled);
bool hasAndroidGyroSensor();
#endif
String defaultMobileStorageRoot();
String writableMobileStorageRoot(String const& fallbackStorageRoot);
void convertEventToRenderCoordinatesIfPossible(SDL_Window* window, SDL_Event* event);
bool isTouchDerivedMouseEvent(SDL_Event const& event);
bool shouldCancelMobileTouchState(SDL_Event const& event);
Maybe<Key> keyFromSdlKeyCode(SDL_Keycode sym);
Maybe<Key> keyFromSdlScancode(SDL_Scancode scancode);
MouseButton mouseButtonFromSdlMouseButton(uint8_t button);
ControllerAxis controllerAxisFromSdlControllerAxis(uint8_t axis);
ControllerButton controllerButtonFromSdlControllerButton(uint8_t button);
KeyMod noMods();

}
