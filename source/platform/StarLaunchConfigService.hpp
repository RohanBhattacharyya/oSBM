#pragma once

#include "StarJson.hpp"
#include "StarString.hpp"

namespace Star {

STAR_CLASS(LaunchConfigService);

class LaunchConfigService {
public:
  virtual ~LaunchConfigService() = default;

  virtual Json loadLauncherConfig() = 0;
  virtual bool saveLauncherConfig(Json const& config) = 0;
  virtual String launcherConfigPath() const = 0;
};

}
