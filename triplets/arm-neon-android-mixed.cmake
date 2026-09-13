set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 28)
set(VCPKG_CMAKE_SYSTEM_PROCESSOR armv7-a)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=armv7a-linux-androideabi")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=armeabi-v7a -DANDROID_ARM_NEON=ON)
# minSdk is 26 (see android host build.gradle) while we compile against API 28.
# Bionic only exports iconv_open/iconv/iconv_close from API 28, so SDL's default
# SDL_SYSTEM_ICONV=ON makes libmain.so NEEDED-resolve those against libc and
# dlopen fails on API 26-27 devices (e.g. Galaxy Tab A T585). Force SDL's
# builtin iconv for the sdl3 port only; other ports keep the existing options.
if(PORT STREQUAL "sdl3")
  list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DSDL_SYSTEM_ICONV=OFF)
endif()
