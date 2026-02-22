#pragma once

#include "StarConfig.hpp"
#include "StarString.hpp"
#include "StarDesktopService.hpp"
#include "StarExternalFileAccessService.hpp"
#include "StarLaunchConfigService.hpp"
#include "StarMobileSystemUiService.hpp"

namespace Star {

STAR_CLASS(MobilePlatformServices);

class MobilePlatformServices {
public:
  static MobilePlatformServicesUPtr create(String const& storageRoot);

  DesktopServicePtr desktopService() const;
  ExternalFileAccessServicePtr externalFileAccessService() const;
  MobileSystemUiServicePtr mobileSystemUiService() const;
  LaunchConfigServicePtr launchConfigService() const;

private:
  DesktopServicePtr m_desktopService;
  ExternalFileAccessServicePtr m_externalFileAccessService;
  MobileSystemUiServicePtr m_mobileSystemUiService;
  LaunchConfigServicePtr m_launchConfigService;
};

}
