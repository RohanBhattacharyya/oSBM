#include "StarPlatformServices_mobile.hpp"

#include "StarFile.hpp"
#include "StarJson.hpp"
#include "StarJsonExtra.hpp"

#ifdef STAR_SYSTEM_ANDROID
#include "mobile/android/StarAndroidFileAccessBridge.hpp"
#endif

#ifdef STAR_SYSTEM_IOS
#include "mobile/ios/StarIosFileAccessBridge.hpp"
#endif

namespace Star {

namespace {

class MobileExternalFileAccessService final : public ExternalFileAccessService {
public:
  explicit MobileExternalFileAccessService(String storageRoot)
    : m_storageRoot(std::move(storageRoot)) {}

  Maybe<String> pickPackedPak() override {
    auto packedPakTarget = File::relativeTo(m_storageRoot, "assets/packed.pak");
    File::makeDirectoryRecursive(File::dirName(packedPakTarget));

#ifdef STAR_SYSTEM_ANDROID
    return AndroidFileAccessBridge::pickAndImportPackedPak(packedPakTarget);
#elif defined(STAR_SYSTEM_IOS)
    return IosFileAccessBridge::pickAndImportPackedPak(packedPakTarget);
#else
    return {};
#endif
  }

  StringList importModFiles() override {
    auto modsDirectory = File::relativeTo(m_storageRoot, "mods");
    File::makeDirectoryRecursive(modsDirectory);

#ifdef STAR_SYSTEM_ANDROID
    return AndroidFileAccessBridge::importModFiles(modsDirectory);
#elif defined(STAR_SYSTEM_IOS)
    return IosFileAccessBridge::importModFiles(modsDirectory);
#else
    return {};
#endif
  }

  bool openModsLocationInSystemBrowser() override {
    auto modsDirectory = File::relativeTo(m_storageRoot, "mods");
    File::makeDirectoryRecursive(modsDirectory);

#ifdef STAR_SYSTEM_ANDROID
    return AndroidFileAccessBridge::openModsDirectory(modsDirectory);
#elif defined(STAR_SYSTEM_IOS)
    return IosFileAccessBridge::openModsDirectory(modsDirectory);
#else
    return false;
#endif
  }

private:
  String m_storageRoot;
};

class MobileSystemUiServiceImpl final : public MobileSystemUiService {
public:
  void showToast(String const& message) override {
#ifdef STAR_SYSTEM_ANDROID
    AndroidFileAccessBridge::showToast(message);
#elif defined(STAR_SYSTEM_IOS)
    IosFileAccessBridge::showToast(message);
#else
    _unused(message);
#endif
  }

  void showDialog(String const& title, String const& message) override {
#ifdef STAR_SYSTEM_ANDROID
    AndroidFileAccessBridge::showDialog(title, message);
#elif defined(STAR_SYSTEM_IOS)
    IosFileAccessBridge::showDialog(title, message);
#else
    _unused(title);
    _unused(message);
#endif
  }

  bool openAppSettings() override {
#ifdef STAR_SYSTEM_ANDROID
    return AndroidFileAccessBridge::openAppSettings();
#elif defined(STAR_SYSTEM_IOS)
    return IosFileAccessBridge::openAppSettings();
#else
    return false;
#endif
  }
};

class LaunchConfigServiceImpl final : public LaunchConfigService {
public:
  explicit LaunchConfigServiceImpl(String configPath)
    : m_configPath(std::move(configPath)) {}

  Json loadLauncherConfig() override {
    if (!File::isFile(m_configPath))
      return JsonObject();

    try {
      return Json::parseJson(File::readFileString(m_configPath));
    } catch (...) {
      return JsonObject();
    }
  }

  bool saveLauncherConfig(Json const& config) override {
    try {
      File::makeDirectoryRecursive(File::dirName(m_configPath));
      File::overwriteFileWithRename(config.printJson(2), m_configPath);
      return true;
    } catch (...) {
      return false;
    }
  }

  String launcherConfigPath() const override {
    return m_configPath;
  }

private:
  String m_configPath;
};

}

MobilePlatformServicesUPtr MobilePlatformServices::create(String const& storageRoot) {
  auto services = make_unique<MobilePlatformServices>();
  services->m_externalFileAccessService = make_shared<MobileExternalFileAccessService>(storageRoot);
  services->m_mobileSystemUiService = make_shared<MobileSystemUiServiceImpl>();
  services->m_launchConfigService = make_shared<LaunchConfigServiceImpl>(File::relativeTo(storageRoot, "mobile_launcher.json"));
  return services;
}

DesktopServicePtr MobilePlatformServices::desktopService() const {
  return m_desktopService;
}

ExternalFileAccessServicePtr MobilePlatformServices::externalFileAccessService() const {
  return m_externalFileAccessService;
}

MobileSystemUiServicePtr MobilePlatformServices::mobileSystemUiService() const {
  return m_mobileSystemUiService;
}

LaunchConfigServicePtr MobilePlatformServices::launchConfigService() const {
  return m_launchConfigService;
}

}
