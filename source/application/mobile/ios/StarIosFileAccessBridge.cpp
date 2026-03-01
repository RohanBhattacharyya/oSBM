#include "StarIosFileAccessBridge.hpp"

namespace Star {

#ifdef STAR_SYSTEM_IOS
extern "C" {
char* StarIosBridge_syncBundledAssets(char const* targetRootDirectory);
char* StarIosBridge_pickAndImportPackedPak(char const* targetPath);
char* StarIosBridge_resolveModsDirectory(char const* fallbackModsDirectory);
char** StarIosBridge_importModFiles(char const* modsDirectory, int* outCount);
bool StarIosBridge_openModsDirectory(char const* modsDirectory);
void StarIosBridge_showToast(char const* message);
void StarIosBridge_showDialog(char const* title, char const* message);
bool StarIosBridge_openAppSettings();
void StarIosBridge_freeCString(char* value);
void StarIosBridge_freeCStringArray(char** values, int count);
}

static Maybe<String> takeOwnedCString(char* value) {
  if (!value)
    return {};

  String out = value;
  StarIosBridge_freeCString(value);
  if (out.empty())
    return {};
  return out;
}
#endif

Maybe<String> IosFileAccessBridge::syncBundledAssets(String const& targetRootDirectory) {
#ifdef STAR_SYSTEM_IOS
  return takeOwnedCString(StarIosBridge_syncBundledAssets(targetRootDirectory.utf8Ptr()));
#else
  (void)targetRootDirectory;
  return {};
#endif
}

Maybe<String> IosFileAccessBridge::pickAndImportPackedPak(String const& targetPath) {
#ifdef STAR_SYSTEM_IOS
  return takeOwnedCString(StarIosBridge_pickAndImportPackedPak(targetPath.utf8Ptr()));
#else
  (void)targetPath;
  return {};
#endif
}

Maybe<String> IosFileAccessBridge::resolveModsDirectory(String const& fallbackModsDirectory) {
#ifdef STAR_SYSTEM_IOS
  return takeOwnedCString(StarIosBridge_resolveModsDirectory(fallbackModsDirectory.utf8Ptr()));
#else
  (void)fallbackModsDirectory;
  return {};
#endif
}

static StringList importModFilesInternal(String const& modsDirectory) {
#ifdef STAR_SYSTEM_IOS
  StringList out;
  int count = 0;
  char** values = StarIosBridge_importModFiles(modsDirectory.utf8Ptr(), &count);
  if (!values || count <= 0) {
    if (values)
      StarIosBridge_freeCStringArray(values, count);
    return out;
  }

  for (int i = 0; i < count; ++i) {
    if (values[i] && values[i][0] != '\0')
      out.append(String(values[i]));
  }

  StarIosBridge_freeCStringArray(values, count);
  return out;
#else
  (void)modsDirectory;
  return {};
#endif
}

StringList IosFileAccessBridge::importModPakFiles(String const& modsDirectory) {
  return importModFilesInternal(modsDirectory);
}

StringList IosFileAccessBridge::importSingleModFolder(String const& modsDirectory) {
  return importModFilesInternal(modsDirectory);
}

StringList IosFileAccessBridge::importModsDirectory(String const& modsDirectory) {
  return importModFilesInternal(modsDirectory);
}

bool IosFileAccessBridge::openModsDirectory(String const& modsDirectory) {
#ifdef STAR_SYSTEM_IOS
  return StarIosBridge_openModsDirectory(modsDirectory.utf8Ptr());
#else
  (void)modsDirectory;
  return false;
#endif
}

void IosFileAccessBridge::showToast(String const& message) {
#ifdef STAR_SYSTEM_IOS
  StarIosBridge_showToast(message.utf8Ptr());
#else
  (void)message;
#endif
}

void IosFileAccessBridge::showDialog(String const& title, String const& message) {
#ifdef STAR_SYSTEM_IOS
  StarIosBridge_showDialog(title.utf8Ptr(), message.utf8Ptr());
#else
  (void)title;
  (void)message;
#endif
}

bool IosFileAccessBridge::openAppSettings() {
#ifdef STAR_SYSTEM_IOS
  return StarIosBridge_openAppSettings();
#else
  return false;
#endif
}

}
