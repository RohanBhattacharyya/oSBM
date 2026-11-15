# OpenStarbound 0.1.15.1 integration

## Scope

The engine is merged through upstream tag `v0.1.15.1`, commit
`e604e078e89310f1212dd20f5cd01bd59d25300b`. oSBM's release `VERSION` remains
`1.1`. xStarbound and upstream PR #666 are excluded.

Upstream rewrote its history after this fork. Before merging, the local
fork-point `e1a6fdeac89a19fcab92566ee1340b4c685276e4` and upstream
`f51d7e7e5b48c807d644a30801fc080f893f1676` were verified to have the identical
source tree `2b485aba21d032543dcafb2b642f4345b7745be2`. The `Link upstream
history` merge records that equivalence without changing any files. The
subsequent normal merge imports the release, preserving a useful base for
future upstream merges.

The release brings custom worlds and client subworlds, animated parallax,
HDR and double-buffered framebuffer support, additional Lua callbacks, and
upstream fixes. OpenProtocol/stream compatibility advances from 14 to 17;
features requiring a newer peer retain upstream's version gates. This is not
a claim that every new feature works with older servers.

## Integration changes

- `source/CMakeLists.txt`, `source/core/CMakeLists.txt`, `source/vcpkg.json`,
  `source/core/StarCompression.*`: use native `zng_*` APIs on every target.
  Core object-library compilation receives the imported native library's
  includes and definitions. Gzip seeks use the native API's 64-bit offset.
- `scripts/switch/build-zlib-ng.sh`: checksum-pinned static zlib-ng 2.3.3 in
  an isolated prefix. It coexists with the SDK's ordinary zlib; SDK libraries
  and toolchain patches are not replaced. Non-Switch targets resolve native
  zlib-ng 2.3.2 from the existing vcpkg baseline. The zlib license is compatible
  with the existing dependency set; `doc/OPENSOURCE.md` acknowledges it.
- Preserve the existing vcpkg baseline, SDL 3.2.28 and Java glue, supported
  Android ABIs, platform identities, allocators, SDK versions, signing, and
  existing CI. PR shell checks include the new Switch dependency helper;
  the imported legacy upstream workflow is not reintroduced.
- `source/application/StarRenderer_opengl.*`: combine upstream framebuffer
  features with oSBM render scaling, safe-area offsets, preservation,
  premultiplied overlays, and float-lighting fallback. GLES HDR uses RGBA16F
  only when color-buffer extensions are available. Fixed-size targets use
  their actual viewport/blit dimensions; alternate attachments resize and
  release correctly. HDR/MSAA reloads retain ordinary effect textures.
- `source/game/StarWorldClient.*`, `StarUniverseClient.*`,
  `StarUniverseServer.*`, `StarWorldServerThread.*`: preserve lighting worker
  teardown, culling, interpolation, memory domains/cache release, and Switch
  timings while integrating the new world lifecycles. Client custom worlds
  receive final chunk snapshots before the world object is destroyed.
  Stopped worlds retain subworld metadata and can send final removal packets
  without dereferencing the destroyed world. Failed custom worlds do not
  overwrite cached chunks with an empty teardown snapshot.
- Adapt pinned ImGui APIs, the mobile Windows host's controller interface,
  the radial menu's rarity borders, and the returned Lua audio handle to the
  new upstream interfaces.
- `source/test/compression_test.cpp`: reference-zlib interoperability in both
  directions for zlib and gzip, decode limits/truncation, seek and clone.
  New world tests cover headless construction/destruction, custom-world
  snapshots, stopped-subworld removal, and a client custom-world round trip.
  The test-universe helper now creates a human character explicitly and
  checks connection errors and warp deadlines.
- `source/game/scripting/StarRootLuaBindings.cpp` uses the existing stack-backed
  XXHash32 helper with unchanged terrain-variant results. On Unix, the blank
  inventory cache uses pthread TLS with thread-exit cleanup; it remains private
  to each thread. Adding upstream's compiler TLS objects caused a reproducible
  Switch SDK file/path failure during a clean bundled-asset refresh. Removing
  those objects avoids that failure; the precise SDK/compiler interaction is
  not established. Tests check terrain golden vectors and per-thread cache
  isolation.
- `source/core/StarSectorArray2D.hpp` guarantees at least one tile worker.
  Switch reports zero for `hardware_concurrency()`, so upstream's new pool
  otherwise queues render/lighting jobs forever without a worker. The same
  hang was reproduced independently on Linux by using GDB to force that
  return value to zero. `TileSectorArrayTest.ParallelGather` exercises
  concurrent gathers with wrapped coordinates and sparse sectors.
- Build instructions are updated in `README.md`,
  `toolchains/patches/README.md`, and `docs/agents/build-and-validation.md`.

## Validation

All generated probes, logs, emulator profiles and disposable saves are in the
ignored `build/upstream-merge/` directory. Tests use only the maintainer's
owned game archive. No archive or normal user save is committed or packaged.
Builds use low process priority and two jobs per invocation on this shared
machine.

### Builds and automated tests

Commands were run from the repository root unless a preset requires `source/`.
Build invocations used `nice -n 15` and `--parallel 2`.

| Check | Command / target | Actual result |
| --- | --- | --- |
| Linux launcher and server | `cmake --build --preset linux-launcher-release --target starbound starbound_server core_tests game_tests mobile_controls_tests launcher_support_tests --parallel 2` | Passed |
| Server version CLI | `dist/starbound_server -version` | Exit 0; reports OpenStarbound Server v0.1.15.1 |
| Asset-independent suites | `ctest --test-dir build/linux-launcher-release -L NoAssets --output-on-failure --timeout 300 --no-tests=error` | 3 suites passed: 203 core, 4 controls, 5 launcher cases |
| Vanilla game suite | `dist/game_tests -bootconfig build/upstream-merge/tests/sbinit.config` (absolute config path) | All 22 cases passed, 25.0 seconds |
| Full Frackin' Universe 6.5.8 | `dist/game_tests -bootconfig build/upstream-merge/tests-fu/sbinit.config --gtest_filter=SystemWorldPreview.*:ServerTest.*:SpawnTest.ClientCustomWorldRoundTrip:RootTest.MaterialVariantCompatibility:ItemTest.BlankInventoryIsPerThread:TileSectorArrayTest.*` | All 11 focused cases passed, 21.2 seconds |
| Android packages | `cmake --build --preset android-{x64,arm64,armv7}-release --target android_apk --parallel 2` (three separate invocations) | All three APKs passed |
| Android contents | `python3 scripts/ci/check-package.py apk build/upstream-merge/oSBM-1.1-android-*.apk` | All passed; ABI names match contents, bundled assets present, no owned archive |
| Android imports | `bash scripts/ci/check-android-imports.sh build/upstream-merge/oSBM-1.1-android-*.apk` with NDK `llvm-readelf` | All passed; no API-28 iconv imports |
| Switch native dependency | `scripts/switch/build-zlib-ng.sh build/switch-zlib-ng` with existing devkitPro SDK | Static native library installed successfully |
| Switch package | `cmake --build --preset switch-release --target starbound_nro --parallel 2` | NRO built; isolated Abseil/native zlib-ng prefixes, no embedded owned archive |
| Switch contents | `python3 scripts/ci/check-package.py switch build/switch-release/romfs` | Passed |
| Packaging regression tests | `python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v` | All 6 passed |
| Workflow / shell syntax | `build/stability-tools/actionlint -shellcheck= -pyflakes=` and `bash -n scripts/ci/*.sh scripts/switch/*.sh` | Passed |

The zero-CPU-count regression was also replayed under GDB, forcing
`std::thread::hardware_concurrency()` to return zero. The pre-fix binary hung
in `TileSectorArrayTest.All` until its 25-second timeout (exit 124). With the
fix, both `TileSectorArrayTest.*` cases passed in 23 ms and exited normally.
The GDB scripts and before/after backtraces are retained with the local logs.

The native dependency builds used manifest mode and the repository's pinned
vcpkg baseline. Ordinary zlib remains a transitive dependency of other
libraries; engine compression calls use the native prefixed API. Non-Switch
zlib-ng is 2.3.2; the isolated Switch helper pins 2.3.3. No supported ABI,
minimum SDK, signing identity, or global SDK library was changed.
Compared with the available `0d87d3cd` stability APKs, final APK size increased
by 328,940 bytes on x86_64 and 308,628 bytes on ARM64 (about 1%). This measures
the complete upstream integration and dependency changes together, not the
isolated cost of zlib-ng. No comparable prior ARMv7/IPA package was measured.

The classic Linux SDL host translation unit also passed `-fsyntax-only`
using the current launcher compilation database flags with the mobile app-path
define removed. This is level 2 coverage of that file; a complete classic
Linux configuration was not built.

### Runtime and renderer checks

- **Linux x86_64 launcher:** loaded a disposable copy of an existing character,
  ship and universe; cycled ship/outpost worlds; resized the window; exercised
  pause and orderly game shutdown back to the launcher. A separate run with
  the full Frackin' Universe mod loaded and cycled the same worlds. These are
  smoke tests, not comprehensive coverage of FU's progression or content.
- **Android x86_64:** API-35 emulator, two virtual CPUs and 2 GiB RAM, using
  SwiftShader and the maintainer's owned archive. Loaded the existing save,
  cycled ship/outpost worlds, paused/resumed, applied HDR settings, returned
  from the background, and saved/quit to the title screen. The final APK was
  installed again and entered gameplay successfully. No Android ARM device
  runtime was available.
- **Isolated GLES renderer probe:** compiled the actual renderer/core on the
  host and ran it on Mesa's surfaceless GLES implementation. Passed float
  light-map allocation/subimage/resize/byte fallback, the bundled world
  shader's gradient (maximum adjacent byte step 6), HDR storage, render
  scaling and safe-area offsets, fixed-size viewport, preservation, feedback
  double buffering, and texture survival across HDR reload. Repeated with
  HDR color-buffer extensions disabled: fallback passed. No GL errors.
  This host probe does not validate Apple or Android GPU drivers.

- **Switch (Ryujinx):** final NRO, isolated Vulkan emulator profile with the
  maintainer's owned archive. Started without an extracted bundled-assets
  directory/stamp, completed a full refresh, loaded the copied old character
  and universe with HDR enabled, and completed two ship/outpost round trips.
  Controller navigation opened the pause menu; save/quit returned to title.
  No engine errors or exceptions were logged during that final run. This is
  emulator coverage, not validation on physical Switch hardware.
- **Switch quit-path limitation:** with Ryujinx's SDL3 host audio backend,
  choosing Quit at the title screen produced a black screen. Guest GDB
  traces found the main thread blocked in `SDL_PauseAudioDevice` while the
  audio thread waited in `audrenWaitFrame`. The pre-merge `0d87d3cd` NRO
  returned correctly in the same profile. The integrated NRO subsequently
  returned to the launcher correctly with Ryujinx's OpenAL backend, including
  `Root: Shutting down Root` and `Returning to launcher after shutdown`.
  This is an observed backend-dependent result, not proof of its root cause
  or of unaffected hardware. No SDK/audio workaround was added; physical
  Switch audio/shutdown testing remains necessary.

### Coverage limits

| Platform | Strongest validation reached |
| --- | --- |
| Linux x86_64 launcher | Level 6: runtime; full client/server compile and unit suites also passed |
| Android x86_64 | Level 6: emulator runtime and APK |
| Android ARM64 / ARMv7 | Level 5: each native build and APK |
| Switch | Level 6: Ryujinx runtime and NRO; no hardware run |
| Windows, macOS Intel/ARM, iOS, Linux ARM64 | Level 1: source/build configuration inspection only |

Windows, macOS (Intel/Apple Silicon), iOS and Linux ARM64 have **not** been
built or run locally. Their retained platform CI is the next build gate;
no current CI result is claimed before pushing. Android ARM64/ARMv7 reached
package-build level, with runtime coverage limited to Android x86_64.
No physical Switch or Apple device was tested. New upstream mod APIs,
multiplayer combinations and long-running custom-world workloads still need
real-user coverage. Imported upstream files retain existing upstream
whitespace; local integration edits pass `git diff --check`.

