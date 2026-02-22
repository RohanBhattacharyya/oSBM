#pragma once

#include "StarString.hpp"

namespace Star {

STAR_CLASS(MobileSystemUiService);

class MobileSystemUiService {
public:
  virtual ~MobileSystemUiService() = default;

  virtual void showToast(String const& message) = 0;
  virtual void showDialog(String const& title, String const& message) = 0;
  virtual bool openAppSettings() = 0;
};

}
