#pragma once

#include "StarMaybe.hpp"
#include "StarString.hpp"

namespace Star {

class IosFileAccessBridge {
public:
  static Maybe<String> syncBundledAssets(String const& targetRootDirectory);
  static Maybe<String> pickAndImportPackedPak(String const& targetPath);
  static Maybe<String> resolveModsDirectory(String const& fallbackModsDirectory);
  static StringList importModPakFiles(String const& modsDirectory);
  static StringList importSingleModFolder(String const& modsDirectory);
  static StringList importModsDirectory(String const& modsDirectory);
  static bool openModsDirectory(String const& modsDirectory);
  static void showToast(String const& message);
  static void showDialog(String const& title, String const& message);
  static bool openAppSettings();
};

}
