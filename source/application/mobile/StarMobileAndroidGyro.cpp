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

#include "mobile/StarMobileAndroidGyro.hpp"

namespace Star {

#ifdef STAR_SYSTEM_ANDROID
namespace {

std::mutex g_androidGyroMutex;
std::array<float, 3> g_androidGyroData{};
bool g_androidGyroHasData = false;

}

static void recordAndroidGyroData(jfloat x, jfloat y, jfloat z) {
  std::lock_guard<std::mutex> lock(g_androidGyroMutex);
  g_androidGyroData = {x, y, z};
  g_androidGyroHasData = true;
}

extern "C" JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_onNativeGyro(JNIEnv*, jclass, jfloat x, jfloat y, jfloat z) {
  recordAndroidGyroData(x, y, z);
}

extern "C" JNIEXPORT void JNICALL Java_io_github_openstarbound_mobile_MainActivity_onNativeGyroAim(JNIEnv*, jclass, jfloat x, jfloat y, jfloat z) {
  recordAndroidGyroData(x, y, z);
}

bool takeAndroidGyroData(std::array<float, 3>& data) {
  std::lock_guard<std::mutex> lock(g_androidGyroMutex);
  if (!g_androidGyroHasData)
    return false;

  data = g_androidGyroData;
  g_androidGyroHasData = false;
  return true;
}
#endif


}
