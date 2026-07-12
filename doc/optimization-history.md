# Performance Optimization History (since `f6809930` "switch port POC")

This document records the complete optimization effort between commit
`f68099305972503ecdffcff6703e59a9e68df98f` (2026-06-28, "switch port POC") and
`a91a0375` (2026-07-12, "theoretical stable 45 on switch, 45hz sim") — roughly
5,650 insertions across 88 files.

**Headline: real in-world gameplay went from ~4.5–7.5 FPS to a certified
46 FPS (pinned at the 45 Hz cap) in Ryujinx**, sustained for 20+ minutes of
continuous world traversal, with no gameplay changes and no perceptible visual
changes. Measurements below are from Ryujinx on the dev box unless noted;
real-Switch numbers were spot-checked early and tracked Ryujinx closely
(Ryujinx's ARM→x86 JIT tax ≈ Switch's weak native CPU).

---

## 1. Context and constraints

- **Target platform family:** Nintendo Switch homebrew (devkitA64/libnx NRO),
  sharing the `STAR_SYSTEM_FAMILY_MOBILE` code paths with Android and iOS.
  The guest GL driver on Switch is **Mesa/nouveau (nvc0)** over HOS nvservices
  via `libdrm_nouveau` — not NVIDIA's driver. This matters (see §5).
- **Testing:** almost exclusively via Ryujinx on a Linux host (real HW
  spot-checks early on). Test cycle ≈ 4 min build + ~2 min boot-to-planet, so
  the working style evolved into *batch analysis/edits → single verification
  run*, with heavy in-guest instrumentation (§7) so each run answers many
  questions.
- **Hard rules from the project owner:** no gameplay changes; visual changes
  only if imperceptible; all-platform (cross-platform) optimizations
  preferred; Linux desktop builds are unsupported (validate non-Switch via the
  Android `libmain.so` build instead).
- **Fixed-timestep loop:** `StarMobilePlatform.cpp::runGameLoop()` —
  `processEvents → N× update() (catch-up) → render() → swap → sleepPrecise`.
  At low FPS the catch-up multiplier is itself a death-spiral risk; several
  fixes below revolve around this.

## 2. Phase timeline

| Phase | Commits | FPS (in-world) | Theme |
|---|---|---|---|
| Baseline | `f6809930` | 2–5 | POC boots; unplayable |
| Profiling + first wins | `69e5a302` (06-29) | ~3.5 → ~7 | debris fix, profiler, thread affinity |
| World-gen & startup | `bb404247` (06-30) | — (startup time) | starter-search, dungeon gen, findPlayerStart |
| Entity/render culling | `62bb0443` (07-01) | 4.5–5 → ~6.4 | entity culling, container swaps, universal re-scoping |
| Render-path rework | `74b10d33` (07-04) | 7.4 → 12–15 | quarter-res bg FB, UI record/replay, entity cache |
| Threaded pipeline | `9021cce6` (07-06) | → 15.5 | sim∥paint worker, TL json cache, HUD FB composite |
| Sim/collision batch | `345ce09b` (07-11) | (groundwork) | collision memo/batch API, always-on throttles |
| **rpmalloc breakthrough** | `98cc4361` (07-11) | **→ 31.5 (30 Hz cap)** | engine-wide allocator swap |
| 45 Hz + leak hunt | `a91a0375` (07-12) | **→ 46.3 certified** | 45 Hz pacing, fence pacing, GL pooling, mesa bufctx leak fix |

The two discontinuous jumps — rpmalloc (§4) and the mesa bufctx leak fix
(§5) — dwarf everything else. Most other changes bought 0.5–3 ms each; these
two removed *systemic* costs that taxed every subsystem at once.

## 3. Optimization catalog by subsystem

### 3.1 Startup / world generation (mostly `bb404247`)

These fixed "minutes-long hang" bugs rather than frame rate:

- **Lazy `CelestialParameters` visitable-parameter generation**
  (`StarCelestialParameters.cpp/.hpp`): the starter-world search
  (`UniverseServer::nextStarterWorld` → `findRandomWorld(200 tries)`) eagerly
  ran full terrestrial worldgen for *every planet of every system in every
  scanned chunk*. Now generated on first access (deterministic from the seed;
  disk/net stores handle both states). **Search: 8+ min (never finished) →
  ~54 s Ryujinx / 47 s real HW.**
- **Dungeon-gen collision-dirty blowup** (`StarWorldServer.cpp`,
  `StarWorldGeneration.cpp`): per-tile `dirtyCollision` during dungeon flush
  padded every 1×1 write to 5×5 → ~25× redundant touches over ~150k writes.
  Now dirtied once per part bounding box. Tile-write phase → 94 ms for 143k
  writes.
- **`ImagePartReader::forEachTile` resolved-tile cache**
  (`StarDungeonImagePart.cpp`): was re-reading pixels + tileset hashmap per
  call (~12–14 calls/part). Now resolved once in `readAsset()`.
- **Merged `Part` connector/anchor scans; merged `placePart` pre/post passes**
  (`StarDungeonGenerator.cpp`) — the latter needed care: the shared
  `preserveTiles` accumulator must only be unioned *after* `place()` or a part
  sees its own tiles as already-preserved.
- **Parallel dungeon-definition part parsing** (`DungeonDefinition` ctor):
  chunked across worker threads. Only ~7–8 % faster (cost is PNG decode under
  a contended CPU), kept as safe.
- **`DungeonDefinitions` LRU cache 20 → 64** (`DefinitionsCacheSize`): fixes
  micro-dungeon definition thrashing during `findPlayerStart` (each sector's
  brute-force micro-dungeon scan reloaded evicted definitions; verified: 89
  sectors → only 4 cold loads).
- **0-byte save-file hardening** (`StarPlayerStorage.cpp`,
  `StarUniverseServer.cpp`): crash-truncated 0-byte `.player`/`metadata`/
  `universe.dat` silently killed boot on Switch (exceptions don't unwind on
  the run thread there). Now pre-skipped/treated as absent.
- **NOT fixed — `findPlayerStart` (§8):** still 400–700 s on first landing on
  a virgin world. 99.7 % of it is genuine first-touch procedural generation
  (`generateRegion` ≈ 24 s/call: BaseTiles noise ~1–2.5 s/sector +
  brute-force MicroDungeons placement 7–12 s/sector across 50–90 virgin
  sectors). The cheap caching win was landed; the rest needs parallel sector
  generation (single-threaded, database-backed `WorldStorage` — save
  corruption risk), faster noise, or a spawn-search strategy change
  (gameplay-visible). **This is the #1 remaining startup lever.**

### 3.2 Renderer / GL submission (`69e5a302`, `74b10d33`, `a91a0375`; mostly `StarRenderer_opengl.cpp`)

Key early discovery: **the render cost is CPU (primitive building, driver
submission), not GPU fill.** Resolution scaling changed almost nothing;
~110 draws/frame taking tens of ms is not a GPU problem.

- **Debris texture-resolve cache** (first big win, 06-29):
  `renderDebrisFields` re-parsed an `AssetPath` per debris item per frame.
  Pre-resolve per frame → debris 38 ms → ~5 ms. Same-session: rect fast-path
  in `Random2dPointGenerator::generate()` (replaced per-sector polygon SAT
  tests), index-typed star PointData.
- **`drawRays` JSON hoist** (`StarEnvironmentPainter.cpp`): 3 JSON path
  queries *per sun ray per frame* → hoisted per-frame. Ray count also halved
  on Switch (cosmetic Perlin tessellation).
- **Quarter-res background framebuffer**: `switchFrameBuffer(id)` /
  `blitFrameBufferToCurrent(id)` APIs; sky/stars/orbiters/horizon/parallax
  render into a half-size "background" FB, stretch-blit under world layers.
  (Parallax shades with a 1-frame-stale lightmap — imperceptible.)
- **Background refresh cadence**: the bg FB is marked *preserve* (not cleared
  at `startFrame`) and only re-rendered every 3rd frame (4th above 75 ms
  gap); skip frames re-blit the previous contents. Sky content moves <1 px
  per frame at 45 Hz, so 15 Hz refresh is invisible. This was the largest
  constant GPU-fill cost when the host GPU power-managed to floor clocks.
- **"main" framebuffer bypass** (`setFrameBufferBypass`): with no
  post-process layers active (vanilla mobile), the world renders directly to
  screen, saving a fullscreen blit + clear + target switch per frame.
  Re-checked every frame so post-process mods restore the intermediate.
- **One-shot screen-clear skip** (`skipNextScreenClear`): when the bg blit
  covers the whole viewport, the next `startFrame` clear is skipped;
  auto-rearms so menus/title still clear.
- **`renderGlBuffer` call reduction**: vertexTransform uniform, effect-texture
  binds, and attrib enables hoisted out of the per-VB loop; per-VB texture
  binds/size uniforms skipped when unchanged (call-local caches).
- **GL buffer/VAO pooling** (07-12, cross-platform): render buffers churn
  constantly while the world scrolls (chunk/entity buffers created and
  destroyed every frame). Buffer objects and VAOs are now pooled across
  `GlRenderBuffer` lifetimes (`s_vertexBufferPool` cap 256 best-fit,
  `s_vertexArrayPool` cap 128, flushed on renderer ctor/dtor), with
  **grow-only power-of-two byte capacities** so steady-state frames perform
  *zero* GL object allocation (was ~9 unique-size allocations/frame —
  a fragmentation tax on every mobile driver). Verified via `[perf-gl]`:
  `vbRealloc=0/f`, `texNew=0 texDel=0`.
- **Streaming texture uploads** (`uploadTextureImage(..., sameStorage)`):
  the per-frame lightmap upload was *respecifying* storage (`glTexImage2D`)
  every frame; same-size/format updates now use `glTexSubImage2D`.
- **Fence frame pacing** (`finishFrame`, mobile family): a
  `glFenceSync`/`glClientWaitSync` ring caps in-flight frames at 2. Needed
  because `SDL_GL_SetSwapInterval(0)` (see §3.6) removed present
  backpressure; without it, GPU-side queue growth turned every implicit sync
  into a wait on ever-older work. Can be disabled for bisection via
  `nofence.flag` (§7).
- **HUD framebuffer composite + two latent FB bugs** (07-06): interface
  renders into an FB composited once (iface 11.5 → 4.8 ms). Fixed en route:
  premultiplied FBs must allocate `GL_RGBA` (were `GL_RGB` → opaque-black
  composite), and `GlFrameBuffer` never set `texture->textureSize` → quads
  sampled a single corner texel.
- **UI primitive record/replay** (`begin/end/playPrimitiveRecording`,
  scissor-segmented): panes/HUD bars replay recorded primitives on most
  frames under load instead of fresh widget renders. Related lesson: a
  scissor change flushes the whole pending batch — an *empty* cursor
  ItemSlotWidget cost 2.9 ms/frame that way.
- **Rejected:** GPU shader offload for stars/debris (per-vertex time-varying
  animation doesn't fit the shared packed vertex format; ~5 % win for a risky
  rewrite); resolution scaling ("main" `sizeMul` — CPU-bound, reverted, but
  the FB `sizeMul` infra + a `blitGlFrameBuffer` bug fix remain); renderer-
  level persistent texture-bind cache (11 bind sites, id-reuse invalidation
  risk, ~1 ms).

### 3.3 Entity / world-client (client side)

- **Entity render culling** (`StarWorldClient.cpp::render`, 07-01): both
  full-entity-map passes (light sources + `entity->render()`) previously
  processed *every loaded entity*; now spatial-indexed `forEachEntity` over
  the visible tile range (+32 tiles for lights), with explicit sort-by-id to
  preserve draw order. worldClient 75–85 → 37–45 ms at 88 entities.
- **Entity query pad tightened** (universal): entity visibility query uses a
  6-tile pad instead of the ~16–20-tile tile-chunk pad it inherited.
- **Zero-copy entity render cache**: `WorldRenderData::entityDrawables` is
  `List<shared_ptr<EntityDrawables const>>`; under load, unchanged entities
  re-share their cached entry (refcount bump). **Critical lesson:** the first
  version deep-copied per hit — the `AssetPath` string copies cost ~9 ms/frame
  and cancelled the entire win.
- **Non-mutating draw path**: `drawDrawable(Drawable const&)` composes the
  camera transform at primitive-emit time (no per-drawable copy/mutate);
  entity layers skip per-drawable bound-box checks (pre-culled).
- **NetworkedAnimator appearance cache** (`StarNetworkedAnimator.cpp`):
  pre-transform resolved part drawables cached by an XXHash64 of all
  appearance inputs (bounded, FIFO evict). Helps static animators most.
- **HashMap swap for render-drawable containers**
  (`ClientRenderCallback::drawables`, `EntityDrawables::layers`): were
  `std::map` — one tree-node heap allocation per drawable per entity per
  frame. Draw order verified unaffected (downstream re-sorts).
  `TilePainter::zOrderBuffers` deliberately left as ordered `Map` (its
  ordering is load-bearing for z-order).
- **Adaptive peripheral-entity throttle**: NPC/monster render cache
  (skip 2/3), engaged by *self-measured* frame gap (>50 ms), not platform.
  Measured null at plant-dense spots (cost there is many cheap entities, not
  few expensive ones) but helps creature-dense areas.
- Assorted: per-frame `Assets::json()`/config lookups hoisted to statics or
  members throughout (`WorldPainter`, `WorldClient`, TeamBar, LabelWidget,
  `clientIPJoinable` 1-in-32, Curve25519 broadcast key, …); mouseover scan
  1-in-3; `MainInterface::updateItems` only when inventory visible.

### 3.4 Simulation / server / netcode

- **Threaded sim∥paint pipeline** (07-06, Switch): a persistent worker runs
  `universeClient->update` overlapped with world painting
  (`render(): snapshot → startSimTick → paint ∥ sim → join → UI`). Sync-frame
  HUD scheme: every 3rd frame joins early and renders fresh HUD; other frames
  replay recorded HUD primitives so the overlap window stays wide.
  (Star `ConditionVariable` has **no predicate wait** — use while loops.)
- **Thread core distribution** (`StarThread_unix.cpp`,
  `switchDistributeCurrentThread`): heavy gameplay threads
  (WorldServerThread, lightingMain, UniverseServer) round-robin onto
  non-primary allowed cores; main thread pinned to core 0. v1 (distribute
  *all* threads) broke boot by surfacing latent races in the libc shims —
  distribute only threads that are designed to run concurrently.
- **Thread-local `Assets::json` cache**: the assets mutex was a convoy
  (~250 µs average wait/call, ~5 s per 20k calls); TL path→Json memo keyed by
  a content epoch → ~2 µs.
- **SP netcode cadence** (single-player only, keyed on
  `localInterpolationMode`, not platform): client netstate send ×4; server
  local-client entity updates 1-in-2; client uses normal interpolation to
  cover it. Server-side broadcast halving stays Switch-gated (a server-wide
  timer would desync real remote MP clients).
- **Server entity tick spreading**: near-player entities 1-in-2/3 with
  accumulated dt; far entities 1-in-4 at plain dt (movement cost scales with
  dt — accumulation saves nothing there); `EntityMap` reindex skipped for
  skipped entities; monitored-entity set building gated to send ticks.
- **Collision batch work** (`345ce09b`): fused single-pass
  `getTileCollisionBlocks` (dirty-check + collect in one `tileEach`); query
  pad clamp 1.25 (poly sets 28 → 5–10, though per-query cost is mostly fixed
  ~330 µs overhead); alloc-free `collisionMove` substeps;
  `ActorMovementParameters` merge/apply memoization; a dead per-tick
  `collisionBody()` heap alloc removed. A collision-query memo keyed on
  region+change-epoch also landed but measured **hits=0** (movers shift
  integral regions every substep) — it is inert dead weight, flagged for
  removal.
- **restDuration=5** via `assets/opensb/default_movement.config.patch`: idle
  grounded actors use the engine's existing (data-dormant) rest mechanism and
  skip collision. Asset patches must ALSO be copied to
  `romfs/bundled_assets/opensb/` and the Ryujinx sdcard copy (§6).
- **1 Hz `.system` store** fix (vanilla bug) + BTree/file-layer hardening
  (`checkIfOpen` guards, file-I/O lock) from the save-corruption saga.

### 3.5 The allocator: rpmalloc engine-wide (`98cc4361`) — the 10× win

**Root cause of the "everything is slow" plateau:** newlib's globally-locked
dlmalloc under Ryujinx translation cost ~55 µs per contended allocation, and
the engine allocates thousands of times per tick across 3+ threads (Lua,
String, Json, Drawables, netcode). Every subsystem was malloc-bound — which
is why a dozen targeted 1–3 ms fixes kept "not adding up."

Proof point that led there: removing ~6 allocations from `collisionMove`
dropped it 343 → 7 µs.

Fix: **rpmalloc as the engine allocator for the whole mobile family**
(`STAR_USE_RPMALLOC` in `source/CMakeLists.txt`; `source/extern/rpmalloc.c`).
Switch port specifics (all in rpmalloc.c, `__SWITCH__`-gated):

- Span source = `memalign` from the newlib heap (libnx has no mmap);
  span-aligned; decommit is a no-op (spans stay cached).
- `malloc.c` libc-override **excluded** for the mobile family (would recurse
  through memalign; Android scudo / iOS libmalloc untouched — engine-only
  routing via the `Star::malloc` wrappers; Lua allocates through it too).
- `get_thread_id` uses **`tpidrro_el0`** on Switch — `tpidr_el0` is not
  maintained per-thread on HOS, so every thread got heap id 0 → cross-thread
  heap corruption → SIGABRT at asset load. (Android keeps `tpidr_el0`.)
- `rpmalloc_thread_finalize(1)` in `ThreadImpl::runThread` epilogue;
  span-map failures logged via `svcOutputDebugString`.

**Results:** server tick 53 → 4.0 ms; monster script 730 → 74 µs/call;
paint 17.5 → 3.4 ms; sim 33 → 1.6 ms; snap 18 → 0.8 ms; boot+warp 170 → 42 s;
fps pinned at the 30 Hz cap. After this, most earlier perceptibility-adjacent
cadence compromises were **reverted to vanilla/load-gated** (they only engage
below ~20 fps now); the pure wins stayed.

### 3.6 Pacing: 30 Hz → 45 Hz and the vsync traps (`98cc4361`, `a91a0375`)

- Originally 60 Hz sim with catch-up ran the game in perpetual slow motion at
  low fps. Interim: 30 Hz + 1-tick cap. Final: **45 Hz**
  (`GlobalTimestep = ServerGlobalTimestep = 1/45`, ticker 45 in
  `StarMobilePlatform.cpp`) — rate × timestep = 1.0, so gameplay speed is
  unchanged and physics granularity is *closer* to vanilla 60 than 30 was.
  The fps meter reads 46.3 at cap due to ticker smoothing.
- **Vsync half-rate wedge (real bug):** after any stall, present
  backpressure phase-locked `startFrame` into waiting ~27 ms for the
  next-next 60 Hz vsync slot and *never recovered* ("fps drops over time and
  stays down" reports). Fixes: `SDL_GL_SetSwapInterval(0)` on Switch (loop
  already self-paces via `sleepPrecise`) + **tick-backlog forgiveness**
  (spare < −0.25 s → `m_updateTicker.reset()`, family-wide) so stalls neither
  starve sleep nor fast-forward the sim.
- Removing swap-interval pacing is what made **fence pacing** (§3.2)
  necessary, and on the Ryujinx side the guest vsync was set to 90 Hz so
  45 fps is a clean divisor (avoids the 60 Hz nvnflinger half-rate phase
  trap). That's host config, not repo code — see §6.

## 4. The monotonic-degradation saga (07-11 → 07-12) — mesa bufctx ref leak

The last blocker for "45 stable": fps decayed 46 → 40 → 33 → … starting
~10 min in (paint grew ~1.5 ms per 2 min), and a 2h14m soak ended in a guest
`fatalThrow` (OOM). The hunt is instructive:

1. GPU util was only 26–36 % at floor clocks while a guest thread spun at
   ~94 % CPU → not fill-bound; time was *inside* GL calls.
2. Draw calls flat, texture create/delete zero, buffer reallocs zero (after
   the pooling work) → not guest object churn.
3. A/B via `autopilot-stand.flag`: growth continued while standing still →
   frame-count-driven, not content-driven. Disabling our fence ring
   (`nofence.flag`): no change → not sync objects.
4. `[perf-mem]` (mallinfo) showed the guest newlib heap growing ~160 B/frame.
   A `--wrap=malloc` tracker (§7) with per-caller live counts named the
   allocator in one run: `nouveau_bufctx_refn` (libdrm_nouveau bufctx.c:129),
   **2 refs leaked per frame, ~69 B each**.
5. Per-bin counters in a locally patched libdrm_nouveau identified bins
   `NVC0_BIND_3D_TEX(4,2)` and `TEX(4,3)` — fragment-shader texture slots 2–3.

**Root cause (an upstream Mesa bug, latent for years):** `nvc0_blit_3d`
(`nvc0_surface.c`) marks *all* bound fragment-texture slots dirty but only
resets bufctx bins `TEX(4,0)`/`TEX(4,1)` — the two it binds itself. The next
`nve4_validate_tic` re-refs slots 2 and 3 (dirty ⇒ `BCTX_REFN`) into bins
that are **never reset**. The bin/pending lists grow without bound and every
pushbuf submission walks them → linear per-frame slowdown, eventual heap
exhaustion. Desktop apps rarely 3D-blit every frame with 4 textures bound;
our quarter-res background stretch-blit (scaled ⇒ ineligible for the 2D
engine ⇒ `blit_3d` path) triggered it 45×/s with the world shader's 4
multitexture slots bound. **This affects real Switch hardware too**, not just
Ryujinx.

**Fix:** patched `libdrm_nouveau`'s `nouveau_bufctx_refn` to dedup identical
plain refs (same bo + same flags, `packet == 0`) within a bin — semantically a
no-op (refs exist only to keep BOs on the submission list) that kills the
whole leak class. ⚠️ **This patch lives in the locally installed toolchain
library** (`$DEVKITPRO/portlibs/switch/lib/libdrm_nouveau.a`), *not in this
repo* — see §6. It should be vendored and/or upstreamed
(Mesa: add `nouveau_bufctx_reset` for all slots dirtied in
`nvc0_blit_save_textures`/restore, or reset-before-refn in validate_tic).

**Certification (run 113):** 46.2–46.3 fps pinned for 20+ min of continuous
autopilot walking at 352 MHz floor GPU clocks; paint 3.3–3.7 ms flat; blit
lap 0.18 ms flat (previously grew 1.0 → 66 ms); guest heap flat; visuals
verified by self-screenshot.

## 5. Profiling truths worth keeping (hard-won)

- **Per-GL-call and per-allocation *fixed* costs dominate on this platform
  class** (translated/emulated driver, weak ARM cores). Call counts and
  allocation counts are the metrics; bytes and fill rarely are.
- Distributed 1–3 ms fixes stopped compounding until the *systemic* taxes
  (allocator lock, driver list-walk) were removed. If everything is uniformly
  slow, suspect a shared tax, not a hundred small ones.
- Exact-size `GL_STREAM_DRAW` respecification and GL object create/delete
  churn slowly fragment mobile driver heaps: pool objects, bucket sizes.
- Scissor changes flush the immediate batch; empty widgets can cost ms.
- Load-gated throttles (>50 ms) are a **trap** at target fps: they disengage
  exactly when you reach the goal, re-adding their cost. Use always-on
  cadences where imperceptible, or self-measured gates with hysteresis.
- Run-to-run fps variance is dominated by world population and host load;
  A/B via internal lap timers on like-for-like scenes, never via single-run
  fps deltas. Record host `loadavg` with every measurement.
- Ryujinx-specific: guest `glReadPixels` readback has a pitch bug (only the
  left quarter of each row is valid — still judgeable); host NVIDIA driver
  parks GPU clocks (~350 MHz) for Ryujinx's modest util and PowerMizer via
  Xwayland does nothing on Wayland; first launch after host boot sometimes
  hangs black (kill and relaunch).

## 6. State that lives OUTSIDE this repository (reproducibility hazards)

1. **Patched `libdrm_nouveau.a`** in the devkitPro root
   (`~/dkproot/opt/devkitpro/portlibs/switch/lib/`): bufctx ref dedup (§4) +
   `star_bufctx_live[512]`/`star_bufctx_report` diagnostic counters. A fresh
   toolchain install **reintroduces the FPS decay**. The patched source was a
   clone of devkitPro/libdrm_nouveau in the session scratchpad; the original
   `.a` was backed up alongside it. TODO: vendor the patch into the repo
   (e.g. `toolchains/patches/`) and upstream to Mesa/devkitPro.
   Note: ninja does not track system `.a` files — after swapping the lib,
   delete `dist/starbound.elf` to force a relink.
2. **Ryujinx host config** (`~/.config/Ryujinx/Config.json`):
   `enable_custom_vsync_interval=true`, `custom_vsync_interval=90` (45 fps
   clean divisor; the 60 Hz default re-enables the half-rate phase trap).
3. **Asset patches** under `assets/opensb/` must be mirrored to
   `romfs/bundled_assets/opensb/` in the build dir and to the Ryujinx sdcard
   (`~/.config/Ryujinx/sdcard/switch/oSBM/bundled_assets/opensb/`).
4. Test save/world state on the Ryujinx virtual sdcard (worlds are a one-time
   ~50 min generation tax per seed; reuse them — only `universe.lock` ever
   needs deleting).

## 7. Diagnostics & test infrastructure (in-tree, mostly `#ifdef STAR_SYSTEM_SWITCH`)

Deliberately log-heavy so Ryujinx runs can be sparse. All are candidates for
stripping in release builds; an inventory of what is safe to remove is in the
session notes ("cleanup-inventory").

- **`[perf]` family** (one line per ~150 frames each): `[perf]`
  frame/snap/paint/join/iface/sim/other; `[perf-loop]` events/update/start/
  render/finish/swap/sleep; `[perf-gl]` draws/texBinds/vbReuse/vbRealloc/
  texNew/texDel; `[perf-env]` env sub-phases (stars/debris/orbiters/sky/
  parallax/flush/blit); `[perf-wp]`, `[perf-wc]`, `[perf-wcr]`, `[perf-uc]`,
  `[perf-mi]`, `[perf-pm]`, `[perf-pu]`, `[perf-pt]`, `[perf-ws*]`,
  `[perf-mon]`, `[perf-mm]`, `[perf-mt]`, `[perf-am]`, `[perf-qc]`,
  `[perf-assets]`, `[perf-dg]` (world-gen phases), `[perf-mem]` (newlib
  mallinfo watermark), `[perf-alloc]`, `[perf-bufctx]`.
- **Allocation attribution** (`StarSwitchCompat.c` + `--wrap=malloc/calloc/
  realloc/free` in `source/client/CMakeLists.txt`): every wrapped allocation
  carries a 16-byte header naming the caller; per-caller live/total counts;
  `starAllocTrackReport` prints top live callers with addresses relative to
  `__wrap_malloc`. Resolve offline:
  `nm dist/starbound.elf | grep __wrap_malloc` + offset → `addr2line -e
  dist/starbound.elf -f -C <addr>`. Blocks from unwrapped allocators
  (memalign ⇒ rpmalloc spans) pass through via a magic check. This found the
  mesa leak in a single run; it is general-purpose.
- **Autopilot** (`StarClientApplication.cpp`): `autopilot.flag` on the sdcard
  → auto-load single-player, auto-beam to the orbited world (re-armable,
  survives death), then **autopilot-walk** — continuous `moveRight()` + hop
  every ~3 s, generating real exploration load (sector gen, chunk/lighting
  churn, combat). Runtime levers: `autopilot-stand.flag` halts walking
  mid-run (polled every 512 ticks); `nofence.flag` disables the pacing-fence
  ring (read once at startup).
- **Self-screenshot**: `printf 1 > <sdcard>/switch/oSBM/screenshot.ctl`
  (polled every 60 frames) → `screenshot*.png` next to it. With the main-FB
  bypass active, `screenshot_main.png` is black *by design* — use
  `screenshot.png`. Only the left ~quarter of each row is valid under
  Ryujinx (readback pitch bug). Host-side `spectacle -b -f -n -o` also works;
  xdotool/import do not (Wayland).
- **Crash-era wraps** (`StarSwitchPlatform.cpp` + client CMakeLists):
  `--wrap=fatalThrow/diagAbortWithResult/exit/_exit/appletExit`, guest
  exception fp-walk, FDTRACE fd-lifecycle logging. Note the wrap link options
  must stay **outside** the `STAR_PRECOMPILED_HEADERS` conditional.
- Launch pattern for unattended Ryujinx runs:
  `setsid bash -c "gamemoderun ryujinx '<nro>'; echo RYUJINX-EXIT=$?" > log 2>&1 < /dev/null & disown`.

## 8. Next steps (in rough priority order)

1. **Vendor + upstream the libdrm_nouveau bufctx fix** (§6.1). Until then any
   toolchain refresh silently reintroduces the decay. The proper Mesa fix is
   in `nvc0_surface.c`/`nvc0_tex.c` (reset the TEX bins for every slot the
   blit dirties). Also verify the compute-dispatch path (`bufctx_cp`) doesn't
   have a sibling leak.
2. **`findPlayerStart` / first-landing world generation** (§3.1): 400–700 s.
   Options: parallel sector generation (needs a real `WorldStorage`
   concurrency design — save-corruption risk), faster `blockInfo` noise,
   smarter micro-dungeon placement scan, or spawn-search biased toward warm
   sectors (needs owner sign-off — behavior change). Object/NPC cold-load
   during dungeon flush (~30–80 s/dungeon, one-time per content type) could
   be prefetched in parallel during the BFS phase.
3. **Real-hardware validation.** Everything since the POC has been certified
   on Ryujinx only. The bufctx leak affects HW too; the fence-pacing and
   45 Hz settings should be sanity-checked against the real nvnflinger.
4. **Diagnostics cleanup pass** for release: strip `[perf-*]` probes, the
   malloc wraps, FDTRACE, autopilot (all inventoried; keep the guest
   exception handler, rpmalloc span-fail logging, BTree close logging).
   Remove the inert collision-query memo machinery (hits=0 measured) and
   unused members (`Monster m_statusTick*`, `ClientApplication
   m_pendingInterfaceDt`/`m_interfaceUpdateCounter`).
5. **Headroom beyond 45 Hz:** the frame has ~16 ms of sleep at cap; 60 Hz
   pacing is plausibly reachable in Ryujinx on a quiet host. The known
   next costs: HUD composite full-screen pass, remaining env refresh, sim
   join on sync frames. On real HW the ceiling is unknown — measure first.
6. **Android/iOS follow-through:** the cross-platform wins (rpmalloc, GL
   pooling, buffer bucketing, bg cadence, fence pacing, tick forgiveness) are
   compiled into Android builds but never profiled there. The same
   fixed-cost-per-call logic suggests real wins; verify with an on-device
   run of the release APK.
7. **Categorical lever, unattempted:** LuaJIT (weeks of work, 5.4→5.1
   compat); NetElement serialization redesign for SP loopback. Both were
   repeatedly identified as the deepest remaining sim-side multipliers and
   deliberately deferred.

## 9. What was tried and rejected (don't re-litigate blind)

- Resolution scaling of the main render (CPU-bound, not fill-bound).
- Stars/debris GPU shader offload (vertex-format blast radius vs ~5 % win).
- Whole-tick collision-query hoisting (bigger regions → bigger sweep sets).
- Pane fresh-render staggering (replay upload cost exceeded savings).
- Distance-based entity render throttling (cost is spread across many cheap
  entities, not concentrated — proven with an extreme-setting diagnostic).
- `LocalPacketSocket` debug round-trip removal (already compiled out;
  `STAR_DEBUG` is correctly gated behind `!NDEBUG`).
- Killing the driver's texture-bind redundancy at renderer level (risk/reward).
- ResScale=2 on Ryujinx (worse at parked GPU clocks; reverted).
- Host-side GPU clock pinning (needs root; PowerMizer ineffective on Wayland).
