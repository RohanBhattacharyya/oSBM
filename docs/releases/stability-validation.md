# Stability release preparation

This pass continues `main` at `b282bc6e`. The eight stability commits now live
on `main` in the primary oSBM checkout; the temporary worktree and its branch
have been removed. Uncommitted v2 source edits are preserved in the stash named
`v2 before stability on main`; generated packages, Rust build output, and design
scratch files are retained in the ignored `build/retired-v2/` directory, with
recovery instructions in its `README.txt`.

`VERSION` remains `1.1`; these are preview builds, not a published release.
No issue comments, closures, pushes, tags, or release uploads were made. Commits
use the maintainer's existing name, email, and short message style.

Validation date: November 9, 2026. This is evidence for a release candidate,
not evidence that every open issue is fixed.

## Changes

- Keep touch controls off the title screen, character selection, and startup
  screens. Cancel held touch actions on gameplay transitions while preserving
  the user's enabled/disabled preference. This is a small backport of the
  relevant v2 work, not adoption of its application architecture.
- Exclude plain layout containers and decorative progress/fuel/stretch-image
  widgets from controller focus candidates. Interactive children remain
  reachable. This addresses part of #60; it does not establish that every
  controller screen is correct.
- Preserve the actual desktop window mode when SDL reports size changes.
  Testing found that the mobile app host was reporting `Normal` after entering
  fullscreen, which silently cleared the saved fullscreen preference.
- Create a newly selected desktop storage directory before canonicalizing it.
  The old order failed on Unix when `realpath` was given a nonexistent folder.
- Package and refresh `osbm` alongside `opensb` on Android, iOS, and Switch.
  Android only records a successful bundled-assets refresh when all required
  asset trees and the font were copied.
- Add regression coverage for controls, storage, fullscreen state, and the
  existing visited-system geometry cache fix on main.
- Build a pinned, patched Switch Abseil in a separate prefix. The installed
  archives on this machine omitted allocator/thread/semaphore definitions and
  could not link the game. The helper does not modify the installed SDK.
- Clear a reused temporary-file descriptor's closed marker on Switch. Without
  this, valid `mkstemp` handles generated repeated false `USE-AFTER-CLOSE`
  diagnostics. This is a tracing correction, not a claim of fixing every
  Switch file-lifecycle bug.
- Release the host's audio mutex before querying/pausing the SDL stream during
  shutdown. The final Switch quit test exposed an inverted lock order against
  SDL's audio callback: gameplay and Save & Quit worked, but returning from
  the title screen to the launcher stalled on a black screen.
- Add automatic PR checks and full-platform package CI; see below.
- Move the iOS job from the retired `macos-14` image to `macos-15-intel`,
  retaining an Intel host. [GitHub's retirement notice](https://github.blog/changelog/2026-10-01-github-actions-macos-14-runner-image-retirement/)
  confirms the old image stopped being available on November 2, 2026.

## Issue disposition

Read against the open issues and relevant comments on the validation date.
The table is a maintainer checklist; nothing was posted to GitHub.

| Issue | Evidence from this pass | Release/closure gate |
|---|---|---|
| [#52 Keyboard input while typing](https://github.com/RohanBhattacharyya/oSBM/issues/52) | Main already removes the hard-coded movement fallback and respects text focus. Linux and Android x86_64 emulator chat/crafting tests passed with movement controls before/after. Linux unbound WASD/rebound arrows passed. | Candidate for the stability release. Confirm on the reported Samsung pogo/Bluetooth keyboards, including remapped keys. Fixed virtual-key mappings still need a controller/touch remapping pass. |
| [#54 Visited-system navigation](https://github.com/RohanBhattacharyya/oSBM/issues/54) | Real-asset test checks different systems, planets/moons, cold/warm geometry, delayed data, and 100 warm frames without DB queries. It passes main; an isolated binary with the old cache restriction fails. | Candidate for the stability release. Replay the actual visited-star navigation and ship-position scenario on Android; the test does not measure rendered UI or a moving foreign ship. |
| [#43 Linux fullscreen](https://github.com/RohanBhattacharyya/oSBM/issues/43) | Found and fixed preference reset after SDL resize. Native X11 entry, restart while fullscreen, and return to the saved windowed size passed. | Candidate for the stability release after the packaged CI build is checked on an ordinary desktop session. Wayland remains untested. |
| [#56 Windows storage/fullscreen](https://github.com/RohanBhattacharyya/oSBM/issues/56) | Shared fixes and Linux storage persistence/migration tests pass. | Keep open until Windows package, native folder picker, fullscreen/restart, and existing-save behavior are checked on Windows. |
| [#51 Custom save path](https://github.com/RohanBhattacharyya/oSBM/issues/51) | Desktop selection persists, new directories work, explicit environment overrides win, and changing the selection does not silently move saves. | Desktop implementation is covered on Linux. Arbitrary Android/iOS shared storage and synchronization permissions are separate work; do not close a general mobile request on desktop evidence. |
| [#65 Touch controls](https://github.com/RohanBhattacharyya/oSBM/issues/65) | Title/menu obstruction fixed; title has no overlay and gameplay restores it on Linux and Android. Touchpad plus a second attack finger has a regression test. | Partial only. A broader touch-control redesign remains open. |
| [#60 Controller focus](https://github.com/RohanBhattacharyya/oSBM/issues/60) | Decorative/container filtering backported and compiled for Linux, Android, and Switch. | Keep open pending physical-controller traversal of inventory, crafting, HUD, quick wheel, and nested panes. No physical controller was validated here. |
| [#48 Switch Outpost/teleport crashes](https://github.com/RohanBhattacharyya/oSBM/issues/48), [#46 Switch freezes](https://github.com/RohanBhattacharyya/oSBM/issues/46) | NRO builds with patched dependencies; boot exception self-check passes. Runtime results are recorded below. | Keep open pending long Outpost/teleport soak on hardware. Short emulator runs cannot rule out the previously reported hour-scale memory growth. |
| [#49 Android armeabi-v7a](https://github.com/RohanBhattacharyya/oSBM/issues/49) | Screenshot identifies `iconv_open` missing on old Android. Main already disables system iconv in all Android SDL triplets. New APK import check passes arm64/x64 and rejects the actual published v1.1 armv7 APK. Changed shared sources also pass armv7 compiler syntax checks. | Strong root-cause evidence, but no local armv7 APK or T585 runtime validation. Keep open until the CI-built armv7 package installs/starts on the affected device. |
| [#69 Lighting/navigation/UI overflow](https://github.com/RohanBhattacharyya/oSBM/issues/69) | Report screenshots inspected; no independent reproduction. The overflow image shows an extended species-selection interface and a clipped tooltip, as well as the title-screen touch overlay fixed here. #54's cache test does not establish that all orbit/layout complaints are fixed. | Triage the three symptoms separately. Compare unmodified game assets with the reporter's mod set, then test UI scale/tooltip bounds and a saved visited-star scene. |
| [#11 Rendering resolution](https://github.com/RohanBhattacharyya/oSBM/issues/11) | No render-resolution benchmark in this pass. | Keep separate from desktop fullscreen: lowering the logical UI size is not proof that the physical rendering cost fell. |

Other open requests remain a subsequent milestone: auto-attack (#67), Steam
friends/joining (#66), optional launcher (#62), controller UX (#61), networking
authority (#58), Raspberry Pi/Batocera packaging (#55), voice chat/control
editing (#53), multi-screen (#45), Steam account/workshop integration (#44,
#34), volume buttons (#27), and drag behavior (#23). None has been treated as
resolved by this release preparation. In particular, Steam service features
were not pulled across from v2.

After the stability candidate, work through that backlog in small increments:

1. **Input and controller UX (#65, #61, #60, #67, #27, #23):** keep the shared
   adapter path and define acceptance cases for simultaneous move/aim/attack,
   drag/release, remapped bindings, text focus, menu cancellation, quick wheel,
   and controller focus. Test on a touch device and a real controller before
   expanding the editor or adding auto-attack/volume-button options.
2. **Launcher and storage (#51, #56, #62, control editing in #53):** finish the
   native desktop picker tests, then specify mobile directory access and
   permission persistence explicitly. Treat optional automatic launch as a
   preference with a reliable route back to settings and recovery from missing
   assets. Desktop path strings do not solve Android document-tree or iOS
   access permissions.
3. **Rendering/navigation (#11, #54, #69):** retain before/after screenshots,
   visited-star saves and mod lists. Measure actual framebuffer dimensions,
   frame times and object positions; distinguish UI scale from render scale
   and a cold asset load from repeated work on every frame.
4. **Networking and Steam (#66, #58, #44, #34):** first establish a two-client
   direct-IP regression scenario (join, reconnect, host loss, assets/mod
   mismatch). Keep Steam login, invitations and workshop services optional and
   separate from ordinary game startup. Evaluate each platform's support
   before adopting more of the v2 service code.
5. **Additional platforms/features (#55, #45, voice chat in #53):** use the
   Linux ARM64 CI package as a starting point for Pi testing, then verify its
   graphics stack and Batocera integration on hardware. Multi-screen behavior
   and voice chat need separate designs and dependency/platform reviews;
   neither should hold up the initial stability release.

## Local validation

Builds used at most two compile jobs and lowered process priority. GUI tests
ran in a private Xvfb/Openbox session; Android and Ryujinx used isolated data
directories and were run sequentially. Only disposable characters/saves were
used. The user's own game archive supplied runtime assets; it is absent from
all distributable packages and from git.

| Platform | Highest level reached | Result and limits |
|---|---|---|
| Linux x86_64 launcher | 6: native runtime | Client build, asset-free tests, real-asset game tests, chat/crafting input, rebinding, storage, fullscreen/restart, and clean native shutdown passed. Software GL/X11 in isolated display; Steam integration disabled for local builds. Not a Wayland test. |
| Android arm64 | 5: APK | Full native build and signed Release APK passed. Required bundled assets and ABI checked; APK signature verifies. No ARM device runtime. |
| Android x86_64 | 6: emulator runtime | Release APK built, installed, launched and loaded gameplay on API 35 emulator. Bundled `opensb/osbm/lang/font` sync succeeded. Title-to-gameplay overlay, background/resume, relaunch, chat and crafting focus passed. No Samsung hardware keyboard or physical touch device tested. |
| Android armv7 | 2: compiler syntax | Actual target compile flags, `-fsyntax-only`, on changed controls/platform/client/pane sources passed. Not a complete compile, link, APK, or runtime test. |
| Switch | 6: Ryujinx runtime | NRO packaging and exception self-check passed; see soak results below. No physical hardware validation. |
| Windows, Linux ARM64, macOS x64/ARM64 | 1: static inspection | Workflows/presets inspected and workflow syntax linted. No local build or runtime validation. |
| iOS | 1: static inspection | CMake asset packaging, Objective-C++ refresh, and unsigned IPA workflow inspected. No Apple compiler, IPA build, signing, LiveContainer, or device validation. |

### Automated and native results

- `ctest -L NoAssets`: all three suites passed: 196 core cases, four mobile
  controls cases, and five Linux launcher-support cases (205 total).
- `game_tests`: all 14 cases in 11 suites passed using owned
  game assets. Includes asset/root/item/world and local/TCP connection tests.
  Initial run: 14.6 seconds; final rebuilt run: 15.3 seconds.
- `SystemWorldPreview.VisitedSystemsDoNotShareOrbitGeometry` passed. Replacing
  only the preview-cache behavior with the previous current-system-only
  restriction in a separate test binary made it fail as expected, including
  thousands of repeated DB lookups. Production sources/objects were not
  changed for this experiment.
- Linux chat: three-second movement-key hold produced zero horizontal motion;
  after closing chat the control moved 21.73 tiles. Crafting search accepted
  `wasd e` and produced zero motion; movement after closing it worked.
- Linux rebinding: unbound D produced zero motion; rebound Right moved 17.07
  tiles. Fullscreen survived process restart and returned to the original
  windowed size.
- The stripped Linux preview also loaded gameplay from its own adjacent
  packaged `assets/opensb` and `assets/osbm` trees, using an external owned game
  archive and disposable storage, then exited cleanly with status zero. This
  was repeated on the final audio-fix build; its three `NoAssets` suites also
  passed again (3.96 seconds total).
- Android injected keyboard events: holding A for 2.5 seconds in chat and in
  crafting search produced zero horizontal motion in each case. The same
  gameplay control moved 15.30 tiles; D after leaving crafting moved 34.22
  tiles. The interaction key reached the search field without closing it.
  An observation-only Lua position probe lived in the isolated test mods
  directory, outside the repository and release packages. Android tests used
  both the stock IME and a second pass with the guest IME disabled so the
  injected keyboard events reached the native text field directly.
- The final Android x86_64 APK was installed again after the audio fix. It
  loaded gameplay, saved to title, returned to the launcher, started another
  session in the same process, and returned to the launcher again. The final
  shutdown checks used an explicit landscape rotation lock on the isolated AVD.
- `python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v`: five passed.
  Rejection cases include missing universal ABIs, mislabeled APKs, empty IPA
  executable, missing asset trees, game-archive leakage, and unsafe ZIP paths.
- `actionlint 1.7.12 -shellcheck= -pyflakes=`: passed for all workflows.
  `bash -n` passed for all shell helpers; `git diff --check` passed.
- `check-android-imports.sh` passed both new APKs. The downloaded published
  v1.1 armv7 APK failed as expected with all three undefined symbols:
  `iconv_open@LIBC_P`, `iconv_close@LIBC_P`, `iconv@LIBC_P` (exit status one).
  No old APK was executed or added to git. Full Android CI now runs this
  specific pre-API-28 compatibility check using the existing NDK's readelf.
- Package checker passed the actual arm64/x86_64 APKs, Linux staging tree,
  and Switch romfs. `apksigner verify --verbose` passed both APKs (v2 signature,
  one signer). This does not test an upgrade over a published installation.
  The local APKs use this machine's existing debug certificate
  (`05ce05ee44b4c28a038a2819ee016acc6a2355d5feb9510ac9d6157f7ec0d36a`),
  which differs from the published certificate enforced by CI. They are for
  isolated testing and cannot upgrade an installation signed by the published
  key. Release candidates must come from CI's fingerprint-checked signing path.

### Reproduction commands

Run configure/build presets from `source/`. Normal clean builds use the
manifest and pinned dependencies. This host reused the existing per-preset
vcpkg dependency installations to avoid rebuilding them alongside other work:

```sh
env VCPKG_ROOT="$HOME/.local/opt/vcpkg" cmake --preset linux-launcher-release \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DVCPKG_INSTALLED_DIR=/home/rohan/Documents/Code/oSBM/build/linux-launcher-release/vcpkg_installed \
  -DSTAR_ENABLE_STEAM_INTEGRATION=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build --preset linux-launcher-release \
  --target starbound core_tests game_tests mobile_controls_tests launcher_support_tests --parallel 2
ctest --test-dir ../build/linux-launcher-release -L NoAssets \
  --output-on-failure --timeout 300 --no-tests=error
```

`game_tests` additionally used an ignored `dist/sbinit.config` pointing to the
local assets directory and a disposable storage root, with `skipDigest=true`
and `workerPoolSize=2`. Run `../dist/game_tests` from `dist/` after providing
owned assets. Never commit that archive or assume it exists on CI.

Android used `android-arm64-release` and `android-x64-release`, the same
per-preset dependency reuse, the existing SDK/NDK/JDK/Gradle versions, and
`--target android_apk --parallel 2`. Each APK was copied out immediately:
building another ABI replaces the Gradle output directory. The emulator was
an isolated API 35 Google APIs x86_64 AVD, two cores, 3072 MB RAM, SwiftShader,
no audio/snapshots, serial `emulator-5580`. All adb operations selected that
serial; the shared adb server was not stopped.

Switch validation used an isolated dependency prefix. To reproduce it in this
checkout, keep that prefix under the repository's ignored build directory:

```sh
# From the repository root:
bash scripts/switch/build-abseil.sh "$PWD/build/switch-abseil"
cd source
cmake --preset switch-release -DSTAR_SWITCH_PACKED_PAK= \
  -Dabsl_DIR="$PWD/../build/switch-abseil/install/lib/cmake/absl" \
  -DCMAKE_PREFIX_PATH="$PWD/../build/switch-abseil/install"
cmake --build --preset switch-release --target starbound_nro --parallel 2
```

The installed devkitPro SDK was already patched as documented in
`toolchains/patches/README.md`. The Abseil build completed all 248 install
steps. An explicit `absl_DIR` is necessary when reusing a build directory with
a cached old package path; changing `CMAKE_PREFIX_PATH` alone did not replace
it on this host.

### Switch runtime scope

Ryujinx 1.3.3 used a private root/SD directory, Vulkan, two logical processors
exposed to .NET, and a 30 FPS cap. The initial 10 minute 34 second route run
made 19 scripted warp requests. The server logged 20 warp completions including
initial spawn and repeated own-ship requests: five Outpost arrivals, two lunar
base arrivals, and 13 ship arrivals. Both the FDE/throw self-check and repeated
temporary-file reuse passed; there were no false closed-descriptor warnings
after the tracing fix.

An additional run visited the Outpost, traversed its hub, and tested emulated
touch-stick movement. Server timestamps establish an 11 minute 1 second
continuous Outpost visit (emulator elapsed 01:08 to 12:09). SAIL reported
starvation before the character returned to the ship; a survival respawn is
the likely explanation, not a proven diagnosis. The rest of the roughly
16-minute run was ship/title time and is not counted as Outpost dwell.
The final touch-driven Save & Quit reached the title screen and hid the
overlay correctly. Quitting the title then exposed the audio shutdown hang
described above. After the audio fix, a rebuilt NRO successfully returned to
the launcher twice, with a second session launched in the same process.
Teardown took 231 ms and 95 ms respectively, measured from the game-loop-exit
breadcrumb to the launcher-return log. The second title had no touch overlay.
The long route/dwell runs preceded this final shutdown-only fix; the final NRO
was rebuilt and its shutdown/relaunch path specifically retested.
A keyboard-to-gamepad injection attempt in the private
Ryujinx profile did not move the player; its cause was not established, and
controller input is not reported as validated. Touch and the existing test
autopilot were used for the soak.

The FPS cap and emulator host load make this a stability smoke/soak test,
not an uncapped performance benchmark. A short successful run does not rule
out hour-scale leaks or hardware-specific freezes. Keep #48/#46 open until
the longer hardware acceptance test passes. A longer unattended soak also
needs a fed/casual test character so hunger does not end the Outpost visit.
During the actual Outpost visit the engine's logged memory report ranged
from 1228 to 1570 MB, and FPS samples ranged from 24.4 to 31.2 against a target
of 30. These are observations from this particular scene and emulator setup.

## CI and release gates

PRs targeting `main` get workflow lint and one Linux launcher build, three
asset-free test suites, shell syntax checks, and package-checker unit tests. They use no signing
secrets or game archive. Asset-dependent tests compile but are not run on CI.

Pushes to `main`, version tags, and manual dispatch start full package CI:
Android universal (all three ABIs), iOS unsigned IPA, five desktop launcher
targets (Linux x64/ARM64, Windows x64, macOS x64/ARM64), and Switch NRO.
Desktop jobs run the asset-free suites. Packages are checked for required
assets/executables and accidental inclusion of the game archive. Full CI
also checks the known Android iconv import regression. It does not publish a
release.

These workflows have been linted locally, not executed on GitHub. Switch
requires a trusted self-hosted runner labeled `osbm-switch`, with the SDK and
static dependencies provisioned per `docs/agents/build-and-validation.md`.
No runner was registered by this pass; full-platform CI will wait until one
exists. Android needs the existing signing secret. Android app/signing,
SDK/NDK/Gradle/Java settings and supported ABIs remain at their existing
values. The iOS runner image changed as described above; its current Xcode
environment still needs validation on CI.

Recommended release sequence:

1. Review these commits and run the full package workflow. Provision the
   Switch runner first; resolve any clean-runner or Apple/Windows failures.
2. Test the resulting packaged artifacts, rather than relying solely on
   local incremental builds. Confirm Windows storage/fullscreen, iOS bundled
   assets/startup, Android armv7 startup, Samsung keyboard focus/rebinding,
   touch/gamepad navigation, and Android visited-star rendering.
3. Run a multi-hour Switch hardware soak with repeated ship/Outpost/mission
   travel and an extended Outpost dwell. Track memory and responsiveness;
   do not close #48/#46 on a short emulator run.
4. Triage #69's separate visual reports and #11's render-resolution behavior.
   Decide which are stability blockers with reproducible scenes.
5. Choose the release version explicitly, build the candidate from that
   commit, and collect device results. Close only issues whose acceptance
   scenario passed on the affected platform. Deprecate v2 separately in the
   public project messaging when that decision is ready.

Local previews and evidence are retained under the original checkout's ignored
`build/stability-artifacts/0d87d3cd/`. The Linux tarball is a host-built preview;
its glibc/toolchain requirements are not a claim of portable Linux support.
The local test environment, firmware, keys, game archive, probe mod, and saves
are not included in preview packages.

Desktop upgrade notes must explain the existing change from the old SDL
preference directory to `<executable>/storage`. Selecting a different root
does not automatically transfer saves/mods. Test an old-release storage folder
and show users how to select it before they interpret an empty character list
as data loss; keep the original files intact. This remains a Windows upgrade
acceptance case, not something the Linux directory-selection test establishes.

## Changed files

The following paths comprise this stability pass relative to `b282bc6e`:

- `.github/actionlint.yaml`
- `.github/workflows/android-manual.yml`
- `.github/workflows/full-platform.yml`
- `.github/workflows/ios-manual.yml`
- `.github/workflows/pc-manual.yml`
- `.github/workflows/pr-checks.yml`
- `.github/workflows/switch-manual.yml`
- `.gitignore`
- `AGENTS.md`
- `docs/agents/build-and-validation.md`
- `docs/agents/platform-boundaries.md`
- `docs/releases/stability-validation.md`
- `scripts/ci/check-android-imports.sh`
- `scripts/ci/check-package.py`
- `scripts/ci/install-linux-deps.sh`
- `scripts/ci/test_check_package.py`
- `scripts/switch/build-abseil.sh`
- `source/CMakeLists.txt`
- `source/application/mobile/StarMobileControls.cpp`
- `source/application/mobile/StarMobileControls.hpp`
- `source/application/mobile/StarMobileLauncherSupport.cpp`
- `source/application/mobile/StarMobileLauncherSupport.hpp`
- `source/application/mobile/StarMobilePlatform.cpp`
- `source/application/mobile/android/host/app/build.gradle`
- `source/application/mobile/android/host/app/src/main/java/io/github/openstarbound/mobile/MainActivity.java`
- `source/application/mobile/ios/StarIosFileAccessBridgeObjC.mm`
- `source/client/CMakeLists.txt`
- `source/client/StarClientApplication.cpp`
- `source/core/StarFile_unix.cpp`
- `source/test/CMakeLists.txt`
- `source/test/launcher_support_test.cpp`
- `source/test/mobile_controls_test.cpp`
- `source/test/system_world_test.cpp`
- `source/windowing/StarPaneManager.cpp`
- `toolchains/patches/README.md`
- `toolchains/patches/abseil-switch.patch`
- `toolchains/switch/include/sys/mman.h`
