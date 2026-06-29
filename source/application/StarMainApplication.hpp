#pragma once

#include "StarApplication.hpp"
#include "StarApplicationController.hpp"
#include "StarRenderer.hpp"

namespace Star {
  int runMainApplication(ApplicationUPtr application, StringList cmdLineArgs);
}

#if defined STAR_SYSTEM_WINDOWS

#include <windows.h>

#define STAR_MAIN_APPLICATION(ApplicationClass)                                   \
  int __stdcall WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {                       \
    int nArgs;                                                                    \
    LPWSTR* argsList = CommandLineToArgvW(GetCommandLineW(), &nArgs);             \
    Star::StringList args;                                                        \
    for (int i = 0; i < nArgs; ++i) args.append(Star::String(argsList[i]));       \
    if (IsDebuggerPresent() && AllocConsole()) {                                  \
      freopen("CONOUT$", "w", stdout);                                            \
      freopen("CONOUT$", "w", stderr);                                            \
    }                                                                             \
    unsigned long exceptionStackSize = 131072;                                    \
    SetThreadStackGuarantee(&exceptionStackSize);                                 \
    return Star::runMainApplication(Star::make_unique<ApplicationClass>(), args); \
  }

#elif defined STAR_SYSTEM_ANDROID

#define STAR_MAIN_APPLICATION(ApplicationClass)                                                                   \
  extern "C" int SDL_main(int argc, char** argv) {                                                                \
    return Star::runMainApplication(Star::make_unique<ApplicationClass>(), Star::StringList(argc, argv));        \
  }

#elif defined STAR_SYSTEM_IOS

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "SDL3/SDL_main.h"

#define STAR_MAIN_APPLICATION(ApplicationClass)                                                                   \
  static int starMainApplicationEntry(int argc, char** argv) {                                                    \
    return Star::runMainApplication(Star::make_unique<ApplicationClass>(), Star::StringList(argc, argv));        \
  }                                                                                                                \
  int main(int argc, char** argv) {                                                                               \
    return SDL_RunApp(argc, argv, starMainApplicationEntry, nullptr);                                             \
  }

#elif defined STAR_SYSTEM_SWITCH

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "SDL3/SDL_main.h"

#define STAR_MAIN_APPLICATION(ApplicationClass)                                                                   \
  static int starMainApplicationEntry(int argc, char** argv) {                                                    \
    return Star::runMainApplication(Star::make_unique<ApplicationClass>(), Star::StringList(argc, argv));        \
  }                                                                                                                \
  int main(int argc, char** argv) {                                                                               \
    return SDL_RunApp(argc, argv, starMainApplicationEntry, nullptr);                                             \
  }

#else

#define STAR_MAIN_APPLICATION(ApplicationClass)                                                                   \
  int main(int argc, char** argv) {                                                                               \
    return Star::runMainApplication(Star::make_unique<ApplicationClass>(), Star::StringList(argc, argv)); \
  }

#endif
