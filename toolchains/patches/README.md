# Switch toolchain patches

The Switch build depends on three fixes to upstream devkitPro libraries that
are NOT yet upstreamed. A fresh clone of this repo + a stock devkitPro install
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

## sdl3-switch-swkbd-blocking.patch

Upstream: https://github.com/devkitPro/SDL (switch-sdl3 branch, applies at
c329016). Apply after `sdl3-switch-touch-events.patch`.

Without it: the software keyboard opens but (a) nothing typed ever reaches
the app's textboxes, and (b) after the keyboard closes ALL input (touch and
controller) is dead until the app is killed. The port used the INLINE swkbd
applet: it only forwarded the final DecidedEnter string (inline mode delivers
text via per-keystroke ChangedString events it never handled), and it stopped
pumping `swkbdInlineUpdate` the moment the keyboard was dismissed — but the
inline applet stays ALIVE and must be pumped for its state machine to release
input focus, so the invisible applet kept eating every input. The patch
replaces the inline applet with the standard blocking `swkbdShow` applet
(own text-preview field, applet-system-managed focus lifecycle) and delivers
the decided string + a synthetic Return key from the next event pump (SDL
marks text input active only after the StartTextInput hook returns, so text
sent from inside the hook would be dropped). The Return is queued on cancel
too: `SWITCH_PumpEvents` suppresses ALL touch polling while SDL text input is
active, so any field that stays active with the keyboard gone means dead
touch — Enter makes self-deactivating fields (ImGui InputText) release it.

Note the game's OWN textboxes do not use SDL text input on Switch at all
(see `ClientApplication::runSwitchKeyboardSession`): engine textboxes keep
focus until a touch blurs them, which the touch-suppression gate above turns
into a permanent input deadlock, and only the engine can pre-fill the
keyboard with the textbox's current content and replace (rather than append)
it on submit. The SDL path in this patch still matters for the ImGui
launcher's text fields.

## Applying manually

```sh
git clone https://github.com/devkitPro/libdrm_nouveau && cd libdrm_nouveau
git apply path/to/libdrm_nouveau-bufctx-ref-dedup.patch
make -j && cp lib/libdrm_nouveau.a $DEVKITPRO/portlibs/switch/lib/

git clone https://github.com/devkitPro/SDL && cd SDL     # switch-sdl3 branch
git apply path/to/sdl3-switch-touch-events.patch
git apply path/to/sdl3-switch-swkbd-blocking.patch
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
