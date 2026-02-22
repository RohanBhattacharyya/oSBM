#pragma once

#include "StarMaybe.hpp"
#include "StarString.hpp"

namespace Star {

class AndroidFileAccessBridge {
public:
  static Maybe<String> syncBundledAssets(String const& targetRootDirectory);
  static Maybe<String> pickAndImportPackedPak(String const& targetPath);
  static StringList importModFiles(String const& modsDirectory);
  static bool openModsDirectory(String const& modsDirectory);
  static void showToast(String const& message);
  static void showDialog(String const& title, String const& message);
  static bool openAppSettings();
};

}
