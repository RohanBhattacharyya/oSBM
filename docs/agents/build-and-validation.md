# Build & validation reference

All commands below are transcribed from `source/CMakePresets.json`,
`source/CMakeLists.txt`, `source/client/CMakeLists.txt`, `README.md`, and the
workflows under `.github/workflows/` — the authoritative sources if anything
drifts. Only `cmake --list-presets` was executed while writing this document;
treat the rest as derived-from-config, and read a command's preset/workflow
before relying on subtle details.

## Conventions

- Run `cmake --preset <name>` / `cmake --build --preset <name>` /
  `ctest --preset <name>` **from `source/`**. `cmake --list-presets` there
  shows the configure presets.
- Configure preset `X` builds into `build/X/` at the repository root.
- Desktop executables land in `dist/` — **shared by every desktop preset**;
  the last build overwrites `dist/starbound`. The Switch build writes
  `dist/starbound.elf`. Check which preset produced a binary before trusting it.
- Environment: `VCPKG_ROOT` (all vcpkg presets), `ANDROID_SDK_ROOT` +
  `ANDROID_NDK_HOME` (Android), `DEVKITPRO` (Switch).
- The **first configure of any vcpkg preset compiles all dependencies from
  source** — expensive (tens of minutes). Re-configures reuse
  `build/<preset>/vcpkg_installed/`. Prefer existing trees; never delete a
  `build/<preset>/` dir to "clean up".
- `scripts/distclean.sh` runs `rm -rf build dist` — it destroys every
  configured tree. Do not run it without explicit user authorization.
- ccache/sccache are auto-detected by `source/CMakeLists.txt`; when active,
  precompiled headers are disabled.

## Desktop (Linux / Windows / macOS)

Configure presets: `linux-release`, `linux-release-clang`, `windows-release`,
`windows-release-VS2022`, `macos-release`, `macos-arm-release`, and the
launcher variants `linux-launcher-release`, `windows-launcher-release`,
`macos-launcher-release`, `macos-arm-launcher-release`
(launcher = `STAR_PC_MOBILE_LAUNCHER=ON`; these are what `pc-manual.yml`
ships). Windows/macOS presets only work on those host OSes.

```sh
cd source
cmake --preset linux-release                                     # expensive first time
cmake --build --preset linux-release --target starbound --parallel   # routine
```

A build without `--target` also builds the server, utilities, and both test
binaries. Running the game needs an `sbinit.config` (template:
`scripts/linux/sbinit.config`) whose `assetDirectories` reach `assets/`, plus
a user-supplied `packed.pak` (conventionally `assets/packed.pak`, gitignored).

## Unit tests

`BUILD_TESTING` is ON via the base preset for desktop presets and explicitly
OFF for Android and Switch presets. Two GoogleTest binaries
(`source/test/CMakeLists.txt`), both registered with ctest and executed from
`dist/`:

- `core_tests` — core-layer tests, labeled `NoAssets`; runs on a clean checkout.
- `game_tests` — boots a full `Root` plus a local universe
  (`StarTestUniverse.cpp`); requires real game assets reachable from `dist/`,
  which a clean checkout does not have.

The test presets (`linux-release`, `windows-release`, `macos-release`,
`macos-arm-release`) filter to the `NoAssets` label, i.e. they run
`core_tests` only:

```sh
cd source
cmake --build --preset linux-release --target core_tests --parallel
ctest --preset linux-release            # runs core_tests (NoAssets filter)
```

List or narrow: `ctest --test-dir ../build/linux-release -N`, or run the gtest
binary directly: `dist/core_tests --gtest_filter='Json*'`. Because test
binaries also live in the shared `dist/`, rebuild the test target from the
preset you mean to test before running ctest.

Tests never cover: launcher behavior, touch/gamepad input, packaging, JNI/
Objective-C++/libnx glue, or mobile runtime issues. Those need levels 5–6
(package + device) from `AGENTS.md`.

## Android

Prereqs (mirrors `android-manual.yml` and `README.md`): CMake 3.23+, Ninja,
Java 17, vcpkg, Android SDK with `platforms;android-35`, `build-tools;35.0.0`,
`ndk;26.3.11579264`; env `VCPKG_ROOT`, `ANDROID_SDK_ROOT`, `ANDROID_NDK_HOME`.

Presets: `android-arm64-{debug,release}`, `android-armv7-{debug,release}`
(NEON, for 32-bit-only devices), `android-x64-{debug,release}` (emulators/
Chromebooks). Release presets add LTO and release-only vcpkg artifacts.

Per-ABI APK:

```sh
cd source
cmake --preset android-arm64-release          # expensive first time
cmake --build --preset android-arm64-release --target android_apk --parallel
```

Pipeline: `starbound` builds as a shared library → post-build copies/strips it
to `source/application/mobile/android/host/app/src/main/jniLibs/<abi>/libmain.so`
→ the `android_apk` target invokes Gradle (`:app:assembleRelease` or `Debug`)
with `STAR_ANDROID_ABIS=<abi>`. Output:
`source/application/mobile/android/host/app/build/outputs/apk/{release,debug}/oSBM-<version>-android-<abi>[-debug].apk`.

Universal APK (release-only): first build **all three** release presets
(`android-armv7-release`, `android-arm64-release`, `android-x64-release`) so
each ABI's `libmain.so` exists, then:

```sh
cmake --build --preset android-universal-release   # target android_apk_universal
```

Gradle hard-fails naming any ABI whose `libmain.so` is missing. The universal
build runs `:app:clean` first, which also wipes previously packaged per-ABI
APKs from the outputs dir — copy APKs elsewhere between builds (CI does).

Signing: release APKs are signed with the debug-keystore identity
(`app/build.gradle` `signingConfigs.osbmRelease`; `STAR_SIGNING_KEYSTORE` env
override, else `~/.android/debug.keystore`). A locally built release APK is
signed with *your* keystore and **cannot install over the published releases**
(`INSTALL_FAILED_UPDATE_INCOMPATIBLE`). CI restores the real keystore from a
secret and verifies its certificate fingerprint before building.

Runtime validation: install on a device/emulator (`adb install -r <apk>`) and
select a user-supplied `packed.pak` in the launcher. Compile success ≠ APK
built ≠ runs on device.

## iOS (macOS host required)

Compile-only check:

```sh
cd source
cmake --preset ios-arm64-debug
cmake --build --preset ios-arm64-debug --target starbound --parallel
```

Packaged unsigned IPA (matches `ios-manual.yml`): configure with the Xcode
generator from the repo root — full flag set in `README.md` ("Building → iOS")
and in the workflow; key flags are `-G Xcode -DCMAKE_SYSTEM_NAME=iOS
-DCMAKE_OSX_SYSROOT=iphoneos -DVCPKG_TARGET_TRIPLET=arm64-ios-mixed
-DVCPKG_OVERLAY_TRIPLETS=$PWD/triplets` plus the three
`CODE_SIGNING_*` disables — then:

```sh
cmake --build build/ios-arm64-ipa --config Release --parallel --target ipa
# → build/ios-arm64-ipa/ipa/OpenStarbound-iOS-Release-unsigned.ipa
```

The `ipa` target (`source/client/CMakeLists.txt`) always deletes and re-stages
`Payload/` from the current app bundle. **Use it; never hand-stage or reuse an
old `Payload/`.** The CI workflow builds `--target starbound` and packages the
IPA itself with equivalent fresh-`Payload` logic, naming it
`oSBM-<version>-ios.ipa`.

Levels are distinct: compile (`ios-arm64-debug`) < app bundle < unsigned IPA <
signed install on a device. Signing/sideloading happens outside this repo.

## Nintendo Switch

Prereqs: `DEVKITPRO` pointing at a devkitPro root with devkitA64, libnx,
switch-tools, and the cross-compiled portlibs (SDL3, imgui, re2, cpr, zstd,
abseil, opus, freetype, mesa/nouveau GL stack). **Three of those libraries
must carry the repo's patches** — run `scripts/switch/apply-toolchain-patches.sh`
and read `toolchains/patches/README.md` first. An unpatched toolchain
produces: monotonically decaying FPS, a dead touchscreen, a broken software
keyboard, and (under Ryujinx) every C++ `throw` crashing.

```sh
cd source
cmake --preset switch-release      # uses $DEVKITPRO/cmake/Switch.cmake, no vcpkg
cmake --build --preset switch-release --parallel
# → build/switch-release/oSBM-<version>-switch.nro  (target starbound_nro, part of ALL)
```

- Do **not** set `STAR_SWITCH_PACKED_PAK` for normal builds — NROs ship
  without an embedded pak. If a previous configure set it, check
  `build/switch-release/CMakeCache.txt`; the configure step removes a stale
  staged pak when the variable is unset.
- After swapping any portlib `.a`, delete `dist/starbound.elf` before
  rebuilding — Ninja does not track system-library timestamps.
- Runtime validation: real hardware or the Ryujinx emulator. There is no
  Switch unit-test path (`BUILD_TESTING=false`).

## What can be validated on which host

- **Linux host** (this machine): linux presets + ctest; Android configure/
  build/APK if SDK/NDK env is present; Switch only with a patched devkitPro.
- **macOS host**: macos presets + ctest; iOS compile and IPA.
- **Windows host**: windows presets + ctest.
- **Nothing local** validates on-device behavior; say so rather than implying it.

## CI

Three `workflow_dispatch` (manual) workflows; no push-triggered CI:

- `android-manual.yml` — arch choice (arm64/armv7/x86_64/universal) × build
  type; enforces the release signing identity via a fingerprint-checked
  keystore secret.
- `ios-manual.yml` — Release/Debug unsigned IPA artifact on macOS runners.
- `pc-manual.yml` — the four desktop *launcher* presets (all or one OS).

When a CI run fails with symptoms identical to the bug you just fixed,
verify the fix was actually pushed (`git status`, `git log origin/main..`)
before debugging further.
