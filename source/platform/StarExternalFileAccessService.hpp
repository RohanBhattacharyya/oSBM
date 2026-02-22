#pragma once

#include "StarMaybe.hpp"
#include "StarString.hpp"

namespace Star {

STAR_CLASS(ExternalFileAccessService);

class ExternalFileAccessService {
public:
  virtual ~ExternalFileAccessService() = default;

  // Returns absolute imported path in app-local storage if selection succeeds.
  virtual Maybe<String> pickPackedPak() = 0;

  // Imports one or more mod files/directories into app-local mods storage.
  // Returns paths that were imported.
  virtual StringList importModFiles() = 0;

  // Opens the app's mods folder in a native file browser if available.
  virtual bool openModsLocationInSystemBrowser() = 0;
};

}
