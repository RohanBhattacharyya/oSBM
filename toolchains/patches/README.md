# Switch toolchain patches

The Switch build depends on two fixes to upstream devkitPro libraries that are
NOT yet upstreamed. A fresh clone of this repo + a stock devkitPro install
will exhibit the bugs below until these patches are applied — run
`scripts/switch/apply-toolchain-patches.sh` (or follow the manual steps) after
installing/updating the toolchain.

## libdrm_nouveau-bufctx-ref-dedup.patch

Upstream: https://github.com/devkitPro/libdrm_nouveau (applies to master; the
underlying bug also exists in mainline Mesa's users of this API).

Without it: FPS decays monotonically from the moment a world renders
(~46 -> ~24 over 10-30 min under emulation; same on hardware) and the game
eventually dies of guest heap exhaustion after ~2h. Root cause: Mesa's
`nvc0_blit_3d` marks every bound fragment-texture slot dirty but only resets
the two bufctx bins it binds itself; `nve4_validate_tic` then re-refs the
other slots every frame into bins that are never reset, so the bufctx
bin/pending lists grow forever and every GPU submission walks them. The patch
dedups identical plain refs (same bo + flags, no reloc packet) within a bin —
semantically a no-op, refs exist only to keep buffers on the submission list.

## sdl3-switch-touch-events.patch

Upstream: https://github.com/devkitPro/SDL (switch-sdl3 branch, applies at
c329016).

Without it: the touchscreen is dead in-game and in the launcher (ImGui taps
highlight but never click). The port's `SWITCH_PollTouch` passes `false` where
this SDL revision's `SDL_SendTouch` takes an `SDL_EventType` (so finger-down
was submitted as event type 0 and no FINGER_DOWN was ever generated), and both
finger-matching loops assign `found = false` on a match (inverted), spamming
bogus per-poll up events for held fingers.

## Applying manually

```sh
git clone https://github.com/devkitPro/libdrm_nouveau && cd libdrm_nouveau
git apply path/to/libdrm_nouveau-bufctx-ref-dedup.patch
make -j && cp lib/libdrm_nouveau.a $DEVKITPRO/portlibs/switch/lib/

git clone https://github.com/devkitPro/SDL && cd SDL     # switch-sdl3 branch
git apply path/to/sdl3-switch-touch-events.patch
cmake -B build-switch <usual switch toolchain args> && cmake --build build-switch
cp build-switch/libSDL3.a $DEVKITPRO/portlibs/switch/lib/
```

After swapping either `.a`, delete `dist/starbound.elf` before rebuilding —
ninja does not track system library timestamps, so the game will not relink
on its own.

## packed.pak note

The game's packed.pak needs NO patching and should remain stock. (One dev-box
pak was historically repacked with a `restDuration` movement experiment baked
in; the mechanism is now disabled in engine code — `StarMovementController.cpp`,
rest-entry block — precisely so that stray data like that can never activate
it and stock paks behave identically.)
