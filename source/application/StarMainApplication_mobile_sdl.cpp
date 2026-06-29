#include "StarMainApplication.hpp"

#include "StarLogging.hpp"
#include "mobile/StarMobilePlatform.hpp"

#include "SDL3/SDL.h"

#ifdef STAR_SYSTEM_ANDROID
#include <android/log.h>
#endif

#ifdef STAR_SYSTEM_SWITCH
#include "mobile/switch/StarSwitchPlatform.hpp"
#endif

namespace Star {

int runMainApplication(ApplicationUPtr application, StringList cmdLineArgs) {
#ifdef STAR_SYSTEM_SWITCH
  switchInstallLogSink();
#endif
  try {
    return runMobileMainApplication(std::move(application), std::move(cmdLineArgs));
  } catch (std::exception const& e) {
    String message = strf("{}", outputException(e, true));
#ifdef STAR_SYSTEM_ANDROID
    __android_log_print(ANDROID_LOG_ERROR, "OpenStarbound", "Unhandled exception in runMainApplication: %s", message.utf8Ptr());
#endif
    Logger::error("Unhandled exception in runMainApplication: {}", message);
#ifdef STAR_SYSTEM_SWITCH
    switchDebugLog((String("FATAL exception in runMainApplication: ") + message).utf8Ptr());
#endif
    // Avoid modal SDL message boxes on iOS fatal paths; they can deadlock app
    // shutdown if a UIKit presenter is unavailable.
#if !defined(STAR_SYSTEM_IOS) && !defined(STAR_SYSTEM_SWITCH)
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
#ifdef STAR_SYSTEM_SWITCH
    switchDebugLog("FATAL: unknown exception in runMainApplication");
#endif
    // Avoid modal SDL message boxes on iOS fatal paths; they can deadlock app
    // shutdown if a UIKit presenter is unavailable.
#if !defined(STAR_SYSTEM_IOS) && !defined(STAR_SYSTEM_SWITCH)
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenStarbound Mobile", message.utf8Ptr(), nullptr);
#endif
    return 1;
  }
}

}
