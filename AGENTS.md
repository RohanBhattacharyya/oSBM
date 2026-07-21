# oSBM — Agent Operating Guide

Operating contract for coding agents working in this repository. Deeper
references live in `docs/agents/`. If anything written here disagrees with the
build files it describes, the build files win — update the docs, not your
assumptions.

## What this repository is

oSBM (OpenStarbound Mobile) is a mobile-focused port of the OpenStarbound
engine — an open-source Starbound engine that requires the player's own copy
of the game's assets. One C++17 engine tree under `source/` builds every
target:

- **Android** (primary): APKs for `arm64-v8a`, `armeabi-v7a`, `x86_64`, or one universal APK
- **iOS** (primary): an app bundle, packaged into an unsigned IPA
- **Nintendo Switch homebrew**: a devkitA64/libnx `.nro` (no vcpkg; static portlibs)
- **Desktop** (Windows/Linux/macOS): the classic client, plus "launcher" presets that
  run the same ImGui-launcher/touch app path as mobile on a desktop OS

Engine code is shared; platform host code is thin and explicitly gated
(Android Gradle/JNI host, iOS Objective-C++ bridges, Switch platform layer).
Base game assets are **not** distributed with the repository: users supply a
legally owned `packed.pak`. The repository has **no git submodules**;
third-party code is vendored in `source/extern/` or comes from vcpkg
(desktop/Android/iOS) or devkitPro portlibs (Switch).

## Authoritative files

| Question | File |
|---|---|
| Release version | `VERSION` — single source of truth, read by CMake, Gradle, and CI |
| Platform detection, flags, allocators, APK/NRO packaging targets | `source/CMakeLists.txt` |
| Every configure/build/test preset | `source/CMakePresets.json` |
| Dependencies (non-Switch) | `source/vcpkg.json`, `source/vcpkg-configuration.json`; overlays in `ports/`, `triplets/` |
| The `starbound` target per platform, iOS `ipa` target, Switch link wraps | `source/client/CMakeLists.txt` |
| Android app identity, signing, APK naming | `source/application/mobile/android/host/app/build.gradle` |
| Switch toolchain prerequisites and their rationale | `toolchains/patches/README.md` |
| CI behavior | `.github/workflows/android-manual.yml`, `ios-manual.yml`, `pc-manual.yml` |

## Repository map

- `source/extern/` — vendored third-party code (lua, fmt, rpmalloc, xxhash, imgui lua bindings)
- `source/core/` → `source/base/` → `source/game/` — engine layers (utilities → asset/root plumbing → gameplay), shared by client and server
- `source/platform/` — platform-service interface headers only (implemented by the application layer)
- `source/application/` — SDL app shell and renderers (`StarRenderer_opengl` desktop GL, `StarRenderer_gles` mobile app path), plus `mobile/` (ImGui launcher, touch/gamepad controls, per-OS glue under `mobile/android|ios|switch/`)
- `source/rendering/`, `source/windowing/`, `source/frontend/` — game drawing, widget system, in-game UI panes
- `source/client/` — final `starbound` target and per-platform packaging; `source/server/` — desktop-only dedicated server; `source/utility/` — asset/CLI tools; `source/test/` — GoogleTest suites
- `assets/opensb/`, `assets/osbm/` — the only bundled, distributable assets
- `cmake/`, `triplets/`, `ports/`, `toolchains/`, `scripts/` — build support
- `doc/` — modder-facing Lua/JSON docs and `doc/optimization-history.md` (the record of the Switch/mobile performance work)
- Generated, never edit or commit: `build/`, `dist/`, and under the Android host `.gradle/`, `build/`, `app/build/`, `app/src/main/jniLibs/`

Details and a task-to-directory lookup: [docs/agents/repository-map.md](docs/agents/repository-map.md).

## Platform flags are not interchangeable

- `STAR_SYSTEM_<OS>` (`_ANDROID`, `_IOS`, `_SWITCH`, `_LINUX`, `_WINDOWS`, `_MACOS`) — the actual operating system.
- `STAR_SYSTEM_FAMILY_MOBILE` — a real mobile device (android | ios | switch): GLES renderer branches, rpmalloc default (deliberately **not** on iOS), no dedicated server, no precompiled headers.
- `STAR_PLATFORM_MOBILE` — the oSBM *app path* (launcher + touch controls + mobile platform services). On for the mobile family **and** for desktop `*-launcher-release` presets via the `STAR_PC_MOBILE_LAUNCHER` option.
- `STAR_PLATFORM_PC` — set whenever the family is not mobile, so desktop launcher builds have **both** `STAR_PLATFORM_MOBILE` and `STAR_PLATFORM_PC` defined.

Never treat "mobile launcher" as implying "mobile OS". Full wiring:
[docs/agents/platform-boundaries.md](docs/agents/platform-boundaries.md).

## Working rules

- Before editing shared code, find out which platforms compile it: the source
  lists in the directory's `CMakeLists.txt` plus `#ifdef STAR_SYSTEM_*` /
  `STAR_PLATFORM_*` guards. An API available on desktop may not exist on
  Android, iOS, or Switch.
- Comments explaining platform workarounds are load-bearing (Switch exception
  unwinding and `--wrap` link options, iOS allocator/LiveContainer constraint,
  GLEW/GL link order, 32-bit Android file-offset defines). Read them before
  touching nearby code, keep them accurate, and do not remove
  "apparently redundant" code that sits behind a platform conditional without
  checking every branch it serves.
- Keep changes narrowly scoped; no opportunistic refactoring or reformatting.
  `.clang-format` exists but the tree is not uniformly formatted — match the
  file you are in, and never run `scripts/format-source.sh` as a side effect
  of another change.
- Do not change without an explicit task: application/bundle identifiers,
  signing configuration, min/target/compile SDK levels, NDK/AGP/Gradle/Java
  versions, the supported ABI list, devkitPro expectations, or `VERSION`.
- When modifying shared launcher/controls behavior, keep Android, iOS, Switch,
  and desktop-launcher branches aligned — most launcher features have three or
  four platform expressions (see the boundaries doc).
- Preserve unrelated changes already present in the working tree.
- Never report a platform as validated unless you actually built or ran it.

## Dependency policy

Desktop/Android/iOS dependencies use vcpkg manifest mode: `source/vcpkg.json`
with per-platform expressions (e.g. `"platform": "!android & !ios"`), a pinned
registry baseline, overlay triplets in `triplets/` and an overlay port in
`ports/openssl/`. Switch does **not** use vcpkg: dependencies are static
devkitPro portlibs plus locally cross-compiled libraries, three of which
require repo-carried patches (`toolchains/patches/`).

Before adding or upgrading a dependency, evaluate: availability (or an
explicit exclusion) on all four platform groups; static-link viability on
Switch; Android ABI coverage (arm64/armv7/x86_64); Apple toolchain support;
APK/IPA size impact; build-time cost (CI compiles vcpkg dependencies from
source); license compatibility. The Android host vendors SDL3's Java glue
(`org/libsdl/app/`) — keep it in step with the pinned `sdl3` version if you
touch either side.

## Assets and licensing boundaries

- Only `assets/opensb/` and `assets/osbm/` ship with the app; the build copies
  them into APK assets, the iOS app bundle, and the Switch romfs.
- `packed.pak` is the user's own Starbound asset archive. Never commit one
  (`/assets/packed.pak` is gitignored for local desktop use), never embed one
  in CI or release artifacts, and never write code, tests, or docs that assume
  it is distributable or present. Switch builds default to **not** embedding a
  pak (`STAR_SWITCH_PACKED_PAK` unset).
- No secrets in the repo. Android release-signing material exists only in CI
  secrets and local keystores; never generate, commit, or rotate signing
  identities as part of another task.

## Validation expectations

Levels, weakest to strongest:

1. Static inspection (reading, searching)
2. Syntax/format check
3. Targeted compile (one preset, `--target starbound` or narrower)
4. Unit tests (`ctest` on a desktop preset runs `core_tests`; `game_tests` additionally needs real game assets)
5. Platform package build (APK / IPA / NRO)
6. Runtime validation on a device/emulator with a user-supplied `packed.pak`

State exactly which level was reached, for each platform the change touches.
A change that compiles on Linux proves nothing about Android, iOS, or Switch;
a passing `core_tests` proves nothing about launcher or packaging behavior.
Commands, costs, and what is possible on which host:
[docs/agents/build-and-validation.md](docs/agents/build-and-validation.md).

## Completion reporting

Every task report must state: files changed; behavior changed; commands run
with their actual results; platforms validated; platforms **not** validated;
remaining risk.
