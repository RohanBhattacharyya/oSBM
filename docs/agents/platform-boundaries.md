# Platform boundaries

How platform selection actually works in this codebase, and where a change on
one platform needs matching handling on the others. Definitions live in
`source/CMakeLists.txt` (flag discovery + `set_flag` calls); source gating
lives in each directory's `CMakeLists.txt` and in-file `#ifdef`s.

## The flag taxonomy

| Flag | Meaning | Set when |
|---|---|---|
| `STAR_SYSTEM_ANDROID` / `_IOS` / `_SWITCH` / `_LINUX` / `_WINDOWS` / `_MACOS` / `_FREEBSD` / `_NETBSD` | The actual OS | From `CMAKE_SYSTEM_NAME` |
| `STAR_SYSTEM_FAMILY_MOBILE` | Real mobile-family device | `STAR_SYSTEM` ∈ {android, ios, switch} |
| `STAR_SYSTEM_FAMILY_UNIX` / `_WINDOWS` | Desktop families | Everything else |
| `STAR_PLATFORM_MOBILE` | The oSBM **app path**: ImGui launcher, touch/gamepad adapters, mobile platform services | Family is mobile, **or** the `STAR_PC_MOBILE_LAUNCHER` option is ON |
| `STAR_PLATFORM_PC` | PC renderer/allocator/GUI-lib branches | Family is **not** mobile |

Consequences that key off the **family** (not the app path):

- Renderer libs: GLES/EGL on Android, OpenGLES framework on iOS,
  mesa/nouveau static libs on Switch; GLEW + full GL on desktop (GL must stay
  *after* static GLEW on the link line — see the comment in
  `source/CMakeLists.txt`).
- Allocator: family-mobile defaults `STAR_USE_RPMALLOC=ON` **except iOS**,
  which deliberately stays on the system allocator (rpmalloc's process-wide
  `operator new` replacement dies under LiveContainer — comment in
  `source/CMakeLists.txt`). Desktop presets choose per-OS (jemalloc on Linux,
  rpmalloc on Windows).
- `source/server/` is not built on the mobile family.
- Precompiled headers are disabled on the app path (mobile family or PC
  launcher) and whenever ccache is active — so never rely on the PCH to
  provide an include; include what you use.

## Desktop launcher builds are still PC builds

The `*-launcher-release` presets set `STAR_PC_MOBILE_LAUNCHER=ON`: they get
`STAR_PLATFORM_MOBILE` (launcher + touch UI + mobile services) while keeping
`STAR_PLATFORM_PC`, the desktop GL/GLEW renderer, desktop allocators, and
**no** `STAR_SYSTEM_FAMILY_MOBILE`. So:

- Code gated on `STAR_PLATFORM_MOBILE` runs on desktop too — it must not
  assume a phone, a touchscreen, GLES, or Android/iOS APIs.
- Code gated on `STAR_SYSTEM_FAMILY_MOBILE` or a `STAR_SYSTEM_*` never runs on
  a desktop launcher build.
- A desktop launcher build is a cheap smoke test for launcher *logic*, but is
  **not** evidence that Android/iOS/Switch behavior works.

## How mobile sources are selected

`source/application/CMakeLists.txt` is the gate:

- `STAR_PLATFORM_MOBILE` adds `StarMainApplication_mobile_sdl.cpp`,
  `StarPlatformServices_mobile.cpp`, `StarRenderer_gles.cpp`, and
  `mobile/{StarMobilePlatform,StarMobileControls,StarMobileLauncherSupport}.cpp`
  (else the `_sdl` / `_pc` equivalents).
- Inside that, OS-native translation units are added only for their OS:
  Android (`mobile/StarMobileAndroidGyro.cpp`,
  `mobile/android/StarAndroidFileAccessBridge.cpp`), iOS (the `.mm` files,
  compiled with ARC), Switch (`mobile/switch/StarSwitchPlatform.cpp`,
  `StarSwitchCompat.c`). On a desktop launcher build these files are simply
  absent — their symbols may only be referenced from inside
  `STAR_SYSTEM_ANDROID` / `_IOS` / `_SWITCH` guards, or the link breaks.

## Android: native ↔ host boundary

- `starbound` is built as a **shared library** named `libmain.so`
  (`source/client/CMakeLists.txt`), copied and stripped into
  `host/app/src/main/jniLibs/<abi>/` post-build. Gradle packages whatever is
  in that tree for the ABIs listed in `STAR_ANDROID_ABIS`.
- Host app: `io.github.openstarbound.mobile.MainActivity` extending the
  vendored SDL glue (`org/libsdl/app/`, must match the pinned `sdl3` vcpkg
  version). Identity: `applicationId com.devoflife.osbm.android`, minSdk 26,
  targetSdk 34, compileSdk 35, `versionCode`/`versionName` derived from
  `VERSION`.
- APK assets: `app/build.gradle` `assets.srcDirs` pulls in the host
  `src/main/assets/` (launcher `lang/` files) **and the repo `assets/`
  directory** (so `opensb/` ships inside the APK).
- The published release signing identity is the repo owner's debug keystore;
  Gradle resolves it explicitly (`STAR_SIGNING_KEYSTORE` env or
  `~/.android/debug.keystore`) so a missing keystore fails loudly instead of
  AGP silently minting a new key. Changing `applicationId` or the signing key
  strands every existing install.
- armeabi-v7a builds force `-D_FILE_OFFSET_BITS=64` (paks exceed 2 GB on a
  32-bit `off_t`) — see the comment in `source/CMakeLists.txt`.
- OS back navigation arrives via `OnBackInvokedCallback`
  (`enableOnBackInvokedCallback` in the manifest), not as an SDL back key.

## iOS: bundle and packaging

- `starbound` is a `MACOSX_BUNDLE` executable named `OpenStarbound`; bundle id
  `com.devoflife.osbm.ios`; `Info.plist` generated from
  `source/client/ios/Info.plist.in` with Xcode substitutions;
  `MARKETING_VERSION` comes from `VERSION`. All set in
  `source/client/CMakeLists.txt`.
- Post-build copies `assets/opensb/` and the Android host `lang/` dir into the
  bundle. The `ipa` target is the only supported packaging path; it recreates
  `Payload/` from the current bundle every run.
- Objective-C++ lives in `.mm` files with distinct basenames from their `.cpp`
  counterparts (Xcode object-file collision) and builds with `-fobjc-arc`.
- iOS keeps the **system allocator** (no rpmalloc) and avoids modal SDL
  message boxes on fatal paths; there is a `launch-trace.txt` breadcrumb
  mechanism for crashes before logging exists
  (`StarIosBridge_launchTrace`). The app must remain LiveContainer-tolerant.
- Frame pacing is driven by a CADisplayLink vsync pacer
  (`StarIosVsyncPacerObjC.mm`) — do not "simplify" the render-phase timing.

## Switch: the most constrained target

Everything is static, there is no vcpkg, and several non-obvious fixes are
load-bearing. **Read `toolchains/patches/README.md` and the Switch comment
blocks in `source/CMakeLists.txt` + `source/client/CMakeLists.txt` before
touching anything here.** Highlights:

- Required toolchain patches (libdrm_nouveau ref dedup, SDL3 touch events,
  SDL3 blocking swkbd) and a libgcc GCS binary scrub for Ryujinx — applied via
  `scripts/switch/apply-toolchain-patches.sh`.
- Link-time workarounds on the `starbound` target: `-Wl,--eh-frame-hdr`,
  `--wrap=_Unwind_Find_FDE` (without these, every C++ `throw` aborts),
  `--wrap=fatalThrow/diagAbortWithResult/exit/_exit/appletExit` (shutdown/
  fatal attribution), and a `--start-group`/`--end-group` around all libs
  (abseil's circular archives). The opt-in `STAR_SWITCH_ALLOC_ATTRIBUTION`
  malloc wraps stay **off** — they break DWARF unwinding.
- `gnu++17` (not `c++17`) so libnx's BSD types stay visible; `LLONG_MAX`
  supplied manually for Lua; opus resolved without pkg-config (relocated
  toolchain).
- Packaging: launcher assets + `assets/opensb/` staged into
  `build/switch-release/romfs/bundled_assets/`; `nx_create_nro` emits
  `oSBM-<version>-switch.nro`. `packed.pak` embedding is opt-in and off by
  default; the launcher falls back to an SD-card path.
- Engine textboxes use their own swkbd session
  (`ClientApplication::runSwitchKeyboardSession`), not SDL text input.

## Change one, check all

Shared launcher/controls work usually has one portable implementation plus
per-OS expressions. When you touch any of these, check every column:

- **File access / pak selection**: `StarAndroidFileAccessBridge` (JNI/SAF) ·
  `StarIosFileAccessBridge*` (document picker) · Switch romfs/SD paths ·
  desktop native SDL file dialog.
- **Bundled-asset sync** (three copies of the same staging idea): Gradle
  `assets.srcDirs` · iOS bundle post-build copy · Switch romfs staging in
  `source/CMakeLists.txt`. All pull `assets/opensb/` + the host `lang/` dir.
- **Back/forward navigation**: Android `OnBackInvokedCallback` · iOS edge-swipe
  recognizers (`StarIosBridge_installLauncherEdgeSwipes`) · gamepad/keyboard on
  Switch/desktop.
- **Text input**: SDL text input on most platforms · Switch swkbd (SDL patch
  for launcher fields, engine-run session for game textboxes).
- **Storage roots, logging, frame pacing, safe-area/cutout handling** — each
  has per-OS branches in `StarMobilePlatform.cpp` / `StarMobileLauncherSupport.cpp`.

If you change the portable branch, grep for the sibling `#ifdef` branches in
the same function/file and update or consciously exempt each one.
