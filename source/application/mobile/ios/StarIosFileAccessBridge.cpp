#include "StarIosFileAccessBridge.hpp"

namespace Star {

Maybe<String> IosFileAccessBridge::pickAndImportPackedPak(String const&) {
  // TODO(iOS): Use UIDocumentPickerViewController and security-scoped bookmarks.
  return {};
}

StringList IosFileAccessBridge::importModFiles(String const&) {
  // TODO(iOS): Import selected files/folders into app-local mods directory.
  return {};
}

bool IosFileAccessBridge::openModsDirectory(String const&) {
  // TODO(iOS): Expose mods location through Files app integration.
  return false;
}

void IosFileAccessBridge::showToast(String const&) {
  // TODO(iOS): Present lightweight transient message.
}

void IosFileAccessBridge::showDialog(String const&, String const&) {
  // TODO(iOS): Present UIAlertController.
}

bool IosFileAccessBridge::openAppSettings() {
  // TODO(iOS): UIApplicationOpenSettingsURLString bridge.
  return false;
}

}
