# Repository map

Where things live and which direction dependencies flow. Roles below are taken
from the `CMakeLists.txt` files, not guessed from directory names.

## Build layering

Each `source/<dir>/CMakeLists.txt` compiles an object library named
`star_<dir>` (e.g. `star_core`). Executables are assembled from sets of those
object libraries plus their own sources — there are no intermediate shared
libraries. Lower layers must not include headers from higher layers.

```
extern ─→ core ─→ base ─→ game ─┬─→ application ─→ rendering ─→ windowing ─→ frontend ─→ client
                                 ├─→ server   (desktop only: extern+core+base+game)
                                 ├─→ utility  (asset/CLI tools)
                                 └─→ test     (core_tests: extern+core; game_tests: +base+game)
platform/  = interface headers only (services implemented by application/)
```

| Directory | Role |
|---|---|
| `source/extern/` | Vendored third-party: lua, fmt, rpmalloc (`rpmalloc.c`, `rpnew.h`), xxhash, curve25519, tinyformat, fast_float, imgui lua bindings. Excluded from clang-format. |
| `source/core/` | General-purpose support: containers, JSON, files, threading, math, networking primitives, Lua scripting core. Most unit tests target this layer. |
| `source/base/` | Asset system and root plumbing shared by game and application. |
| `source/platform/` | Headers defining platform services: `StarDesktopService.hpp`, `StarExternalFileAccessService.hpp`, `StarLaunchConfigService.hpp`, `StarMobileSystemUiService.hpp`, `StarP2PNetworkingService.hpp`, `StarStatisticsService.hpp`, `StarUserGeneratedContentService.hpp`. |
| `source/game/` | Gameplay: entities, items, objects, worlds, terrain, scripting bindings. Shared by client and server. Large — search, don't browse. |
| `source/application/` | SDL application shell, renderers, platform-service implementations. See "Application layer" below. |
| `source/rendering/` | Game drawing code independent of the widget system. |
| `source/windowing/` | Panes and widgets (GUI toolkit). |
| `source/frontend/` | In-game interfaces built on windowing: inventory, chat, crafting, char creation, HUD, etc. This is the *in-game* UI — the mobile launcher is not here. |
| `source/client/` | The `starbound` target (per-platform executable/library/bundle) and packaging: Android `libmain.so` copy, iOS bundle + `ipa` target, Switch link wraps. `StarClientApplication.cpp` is the game client application class used on every platform. |
| `source/server/` | `starbound_server` dedicated server. Not built when `STAR_SYSTEM_FAMILY` is mobile. |
| `source/utility/` | `asset_packer`, `asset_unpacker`, `btree_repacker`, `dump_versioned_json`, `make_versioned_json`, `world_benchmark`. |
| `source/test/` | GoogleTest suites; see [build-and-validation.md](build-and-validation.md#unit-tests). |
| `source/json_tool/`, `source/mod_uploader/` | Qt tools, only with `STAR_BUILD_QT_TOOLS` (off in every preset except opt-in). |

## Application layer (the part oSBM adds)

- `source/application/StarMainApplication_sdl.cpp` — desktop (non-launcher) entry loop.
- `source/application/StarMainApplication_mobile_sdl.cpp` — entry for the mobile app path (`STAR_PLATFORM_MOBILE`), wraps `runMobileMainApplication`.
- `source/application/mobile/StarMobilePlatform.cpp` — the ImGui launcher itself plus the SDL/GLES app loop for the app path. Largest single mobile file.
- `source/application/mobile/StarMobileControls.*` — touch overlay, custom buttons, macros, gamepad mapping, Controls manager.
- `source/application/mobile/StarMobileLauncherSupport.*` — launcher helpers: localization, storage roots, SDL event/key translation.
- `source/application/mobile/StarMobileAndroidGyro.*` — gyro aiming (Android TU only compiled on Android).
- `source/application/mobile/android/` — JNI file-access bridge + the entire Gradle host project under `host/` (`MainActivity.java`, vendored `org/libsdl/app/` glue, manifest, `app/build.gradle`).
- `source/application/mobile/ios/` — Objective-C++ bridges: file access (`StarIosFileAccessBridgeObjC.mm`), CADisplayLink vsync pacer (`StarIosVsyncPacerObjC.mm`).
- `source/application/mobile/switch/` — `StarSwitchPlatform.cpp` (libnx glue, log sink, `__wrap_*` implementations), `StarSwitchCompat.c`.
- `source/client/ios/` — `Info.plist.in` template and `Assets.xcassets` (app icon).
- `source/application/StarRenderer_opengl.*` — desktop GL renderer (always compiled). `StarRenderer_gles.*` — GLES renderer used by the app path.
- `source/application/StarPlatformServices_pc.*` / `_mobile.*` — platform-service implementations for each path.

## Assets and runtime data

- `assets/opensb/` — OpenStarbound asset overlay (fonts, patches); bundled into every package. `assets/osbm/` — oSBM's own art.
- User-supplied `packed.pak`: desktop looks in `assets/` (drop it at `assets/packed.pak`, gitignored); Android/iOS pick it via the launcher; Switch reads `romfs:/packed.pak` (optional embed) or an SD-card path.
- Desktop runtime config template: `scripts/linux/sbinit.config` (`assetDirectories: ../assets/, ./mods/`); the launcher manages storage on mobile (`defaultMobileStorageRoot` in `StarMobileLauncherSupport`).
- Launcher translations: `source/application/mobile/android/host/app/src/main/assets/lang/*.lang` — note these are reused by the iOS bundle copy and the Switch romfs staging, not Android-only.

## Generated outputs (never edit, never commit)

- `build/<preset>/` — CMake trees, incl. per-preset `vcpkg_installed/`.
- `dist/` — desktop runtime output dir, **shared by all desktop presets** (last build wins); Switch drops `starbound.elf` here.
- Android host: `.gradle/`, `build/`, `app/build/` (APKs under `app/build/outputs/apk/`), `app/src/main/jniLibs/<abi>/libmain.so` (copied there by the CMake post-build step). All ignored via `source/application/mobile/android/host/.gitignore`.
- iOS: `build/ios-arm64-ipa/` including `ipa/Payload/` and the `.ipa`.
- Switch: `build/switch-release/romfs/`, `oSBM.nacp`, `oSBM-<version>-switch.nro`.
- Local AI-tool droppings (`.claude-flow/` dirs, `ruvector.db`, `node_modules/`) are gitignored; ignore them.

## Task → where to start

| Task | Start at |
|---|---|
| Core containers / JSON / IO / threading bug | `source/core/` + matching test in `source/test/` |
| Game logic (entities, items, worlds) | `source/game/` |
| Rendering | `source/rendering/` + `source/application/StarRenderer_*` |
| Window/input handling | `source/application/` (mind the `_sdl` vs `_mobile_sdl` split) |
| Mobile launcher UI / flow | `source/application/mobile/StarMobilePlatform.cpp` |
| Touch controls / macros / gamepad | `source/application/mobile/StarMobileControls.*` |
| Android host, manifest, Gradle, packaging | `source/application/mobile/android/host/` |
| iOS integration / bundle / plist | `source/application/mobile/ios/` + `source/client/ios/` + `source/client/CMakeLists.txt` |
| Switch-only behavior or crash | `source/application/mobile/switch/` + `toolchains/patches/README.md` + Switch blocks in `source/CMakeLists.txt` / `source/client/CMakeLists.txt` |
| In-game UI panes | `source/frontend/` (+ `source/windowing/` for widgets) |
| Build presets / configurations | `source/CMakePresets.json` |
| Packaging targets (APK/IPA/NRO) | `source/CMakeLists.txt` + `source/client/CMakeLists.txt` + Android `app/build.gradle` |
| Unit tests | `source/test/CMakeLists.txt` |
| CI | `.github/workflows/` |
| Release version bump | `VERSION` (only) |
| Performance work background | `doc/optimization-history.md` |
