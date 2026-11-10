# Issue 69

This follow-up addresses the three symptoms in
[#69](https://github.com/RohanBhattacharyya/oSBM/issues/69), including the
maintainer's clarification that the reporter uses Frackin Universe (FU).
No issue comments, closures, pushes, or release uploads were made.

## Changes

- **Navigation geometry:** wait for the celestial database's system response
  before memoizing planet/moon sizes and orbit distances. Star-list chunks
  deliberately omit that data. The old readiness check accepted star
  parameters alone, permanently caching placeholders that disagreed with the
  server. The regression stages actual master/slave responses and compares
  planet, moon, and orbit-marker geometry at several universe times, in both
  the current and another visited system.
- **Lamp grid:** keep linear filtering on floating-point textures. GLES 3
  devices without `OES_texture_float_linear` now use filterable 16-bit float
  storage for those textures, rather than changing to nearest filtering.
  The bundled bicubic lightmap shader requires linear sampling. HDR values
  remain floating point; the upload path still updates existing storage when
  size and format match. Desktop GL and GLES devices with the extension
  retain their 32-bit storage. See the
  [Khronos extension specification](https://registry.khronos.org/OpenGL/extensions/OES/OES_texture_float_linear.txt),
  especially its ES 3 interactions.
- **Tooltip overflow:** clamp the final tooltip rectangle on all four sides
  after choosing its preferred side of the cursor. If a modded tooltip is
  larger than the viewport, shrink only its rendering to fit. The normal UI
  scale, display-density handling, and parent-pane input coordinates remain
  independent. Tooltips still record primitives for Switch's forced replay;
  fresh rendering restores the normal scale before subsequent drawing.

Implementation files are `source/game/StarSystemWorld.{cpp,hpp}`,
`source/application/StarRenderer_opengl.{cpp,hpp}`, and the tooltip layout,
GUI context, and pane manager in `source/windowing/`. Tests are in
`source/test/system_world_test.cpp` and `tooltip_layout_test.cpp`, with their
CMake source lists updated. No FU-specific runtime dependency is introduced.

## Frackin Universe

The isolated compatibility setup uses FU **6.5.8**, commit
`17868d86c9bb7bc00f92038fb78b48cebd1b41b8`, from its
[upstream repository](https://github.com/sayterdarkwynd/FrackinUniverse/tree/17868d86c9bb7bc00f92038fb78b48cebd1b41b8).
The reporter's exact FU version, additional mods, and saved scene are unknown.

FU replaces the character-creation layout, species tooltip, and cockpit Lua.
Its tooltip background is 260 by 260 interface pixels. Its cockpit still
calls the engine's `planetPosition`, `planetSize`, `clusterSize`, and
`orbitPosition` functions. Its `systemworld.config.patch` adds spawn-pool
entries, rather than changing the base orbit-size settings. No lighting
configuration or shader override was found in this FU revision.

The full checkout, disposable mods/saves, extracted owned game assets, logs,
and screenshots stay under the ignored `build/issue69/` directory. Neither
FU nor the user's game archive is bundled into this repository's assets or
release packages. Normal saves and the installed Workshop mods are untouched.

## Evidence and remaining checks

The intentional failing controls and final results are retained locally in
`build/issue69/`.

- `navigation-before.log`: the real staged-response regression fails against
  the old implementation, with persistent client/server mismatches in sizes,
  moon positions, and orbit markers.
- `navigation-fu-before.log`: the same regression also fails with FU's full
  asset set, rather than only vanilla assets.
- `runtime/creation-before.png`: a disposable 450 by 650 tooltip is largely
  clipped off the top-left corner in the previous preview binary.
- `fu-runtime/tooltip-before.png`: unmodified FU's species tooltip loses its
  top half in that binary, matching the reported type of clipping.
- `lighting-before.log` / `lighting-after.log`: a headless EGL/GLES probe
  compiles the actual old/new renderer and uses the bundled world fragment
  shader. Mesa llvmpipe runs with `OES_texture_float_linear` disabled. Old
  code fails interpolation checks; new code passes RGB/RGBA creation, repeated
  subimage updates, resizing, byte/float format transitions, and HDR values
  above one. All final GL error checks are zero. The world-shader gradient's
  largest adjacent-pixel jump falls from 31 to 6 (8-bit channel values).
- `lighting-supported.log`: the new renderer also passes with float-linear
  support enabled. This is an isolated host GLES test, not a device run or a
  replay of the reporter's particular lamp scene.
- `fu-runtime/tooltip-after.png`: the rebuilt Linux client displays the full
  unmodified FU Kirhos tooltip at the same pointer position that clipped it
  before. Clicking while the tooltip is visible still selects Kirhos; see
  `fu-runtime/selection-after.png`.
- `runtime/tooltip-after.png` / `tooltip-bounds.json`: the oversized fixture
  displays both its top and bottom text markers and all four border edges.
  Its measured pixel bounds are `[555, 0, 1052, 719]` in a 1280 by 720 window.
  Parent-pane geometry remains at its normal scale.

### Commands and results

Run preset builds from `source/`; the other commands below run from the
repository root. The ignored boot configs point only to disposable test
storage and the user's owned archive.

| Command | Result |
|---|---|
| `cmake --build --preset linux-launcher-release --target starbound game_tests core_tests mobile_controls_tests launcher_support_tests --parallel 2` | Passed; client and all four test binaries built. |
| `ctest --test-dir build/linux-launcher-release -L NoAssets --output-on-failure --timeout 300 --no-tests=error` | Three suites passed in 3.80 seconds: 199 core cases (including three tooltip tests), four controls cases, five launcher-support cases. |
| `dist/game_tests -bootconfig build/issue69/tests/sbinit.config` | All 15 cases in 11 suites passed, 15.742 seconds; includes both navigation regressions. |
| `dist/game_tests -bootconfig build/issue69/tests-fu/sbinit.config --gtest_filter=SystemWorldPreview.*` | Both navigation cases passed with full FU assets, 3.724 seconds. |
| `python3 build/issue69/check-platform-sources.py android-arm64-release android-armv7-release android-x64-release switch-release` | All 16 compiler syntax checks passed. Uses each preset's actual compile flags with `-fsyntax-only` on the renderer, GUI context, pane manager, and system world. No linking or packaging. |
| `python3 build/issue69/build-renderer-probe.py` and `build/issue69/gles-after` | Probe compiled against the actual renderer. With Mesa's float-linear extension disabled, all rendering checks passed; the old renderer fails. The new renderer also passed with the extension enabled. |
| `glslangValidator -S frag assets/opensb/rendering/effects/world.frag` | Passed; the shader itself is unchanged. |
| `git diff --check` | Passed. |

The headless GLES probe selects Mesa explicitly with
`__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json`,
`EGL_PLATFORM=surfaceless`, `LIBGL_ALWAYS_SOFTWARE=1`, and `LP_NUM_THREADS=2`.
The unsupported-device case additionally sets
`MESA_EXTENSION_OVERRIDE=-GL_OES_texture_float_linear`.

### Platform limits

| Platform | Highest validation reached in this follow-up |
|---|---|
| Linux x86_64 launcher | Level 6: native X11/software-GL UI runtime with full FU and the synthetic tooltip; level 4 core/game tests. Separate headless GLES renderer checks also passed. |
| Android arm64, armv7, x86_64 | Level 2: actual target compiler syntax checks. No new APK build or emulator/device run. |
| Switch | Level 2: devkitA64 compiler syntax checks. No new NRO build, Ryujinx run, or hardware validation. |
| iOS, Windows, macOS, Linux ARM64 | Level 1: shared-code/static inspection only. No build or runtime validation in this follow-up. |

Before closing #69, check the reporter's actual saved system, lamp scene, FU
revision and additional mods on the affected device. The staged-response
regression proves a cache defect and its correction; it does not replay every
modded navigation interaction. Tooltip fitting addresses the reported tooltip
overflow; larger window/pane layouts still depend on the chosen interface
scale. Switch's forced replay path retains its recording contract but has not
been exercised at runtime with this change.

The previous preview binary is from native source `0d87d3cd`; the old
navigation test binary was rebuilt from `9d999dd1` with the new regression
before applying the geometry fix. Test processes use at most two software
rendering threads; native builds use at most two low-priority compile jobs.
