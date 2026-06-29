#pragma once

#include "StarString.hpp"

namespace Star {

// Switch-specific platform integration for the shared mobile launcher path.
//
// These helpers are the Switch analogue of the Android/iOS file-access bridges:
// they mount the embedded romfs image, prepare a writable storage root on the
// SD card, and stage the launcher's bundled assets (fonts + language files) so
// the Dear ImGui launcher and the game behave exactly as they do on the other
// mobile targets. They are only compiled for STAR_SYSTEM_SWITCH and never alter
// the Android/iOS code paths.

// Mounts the embedded romfs (idempotent) and brings up the libnx socket stack
// so that networking (cpr/libcurl) works. Safe to call multiple times.
void switchPlatformInit();

// Recursively copies romfs:/bundled_assets into <storageRoot>/bundled_assets,
// mirroring the bundled-asset sync the Android/iOS hosts perform from their app
// packages. Mounts romfs first if necessary.
void switchSyncBundledAssets(String const& storageRoot);

// Deterministic, writable storage root on the SD card.
String switchDefaultStorageRoot();

// Writes a diagnostic line to the host debug channel (svcOutputDebugString).
// The shared launcher's androidLogInfo() funnels here on Switch.
void switchDebugLog(char const* msg);

// Registers a Logger sink that mirrors all engine log output to the host debug
// channel. Install as early as possible so renderer/startup logs are visible.
void switchInstallLogSink();

}
