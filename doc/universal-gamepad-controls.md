# Universal gamepad controls

The default layout is shared by combat, mining and building. There are no
context profiles and no automatic enemy targeting. The mobile application
path supplies this adapter on Android, iOS, Switch and desktop launcher
builds (`STAR_PLATFORM_MOBILE`). The classic desktop SDL application keeps
its existing controller path.

## Existing layout

| Input | Action |
| --- | --- |
| Left stick | Move |
| Right stick | Aim; small deflections select nearby points, full travel reaches the tool's usable radius |
| RT / R2, held | Primary attack, mine foreground or place foreground blocks |
| LT / L2, held | Secondary action / background operation, as defined by the equipped item |
| A | Jump |
| B | Tech (unchanged; not a new menu cancel binding) |
| X | Interact |
| Y / Back | Inventory |
| LB | Hold item wheel, select with either stick, release to confirm |
| RB | Hold action wheel, select with either stick, release to confirm (including Zoom + / Zoom -) |
| D-pad | Select UI widgets and inventory slots |
| Start | Escape / close a window |
| L3 | Existing Ctrl binding |
| R3 | Hold precision / Shift on new configurations; saved bindings retain their configured action |

The saved `preciseAim` setting and the old aim-toggle action remain
compatible. New configurations bind R3 to a **held Shift modifier**. Existing
configurations retain their saved R3 binding: choose **Precision / Single
Block / Quick Transfer (Shift)** in the launcher and use **Hold** to opt in.
No saved binding is silently replaced.

While this modifier is held, the default aim moves incrementally at a slower
rate from its current target. Legacy precise-cursor speed is reduced to 20%.
Shift also uses the game's smaller tool brush (single block for the standard
matter manipulator) and existing inventory/container quick-transfer behavior:
select a slot with the D-pad, hold the modifier and press RT to transfer its
stack. Custom tools may implement Shift differently. Mouse/touch Shift remains
available; gamepad fine adjustment uses the gamepad's own held modifier.

D-pad navigation fires once on press, waits 350 ms, then repeats every 100 ms
until released. This applies to navigation actions configured as SinglePress,
Hold or Repeat; Toggle and unrelated button press modes remain unchanged.

Rapid inward stick motion is treated as a possible spring return: the last
accepted target is kept until neutral. A deliberate inward adjustment is
accepted after a 100 ms dwell, and outward motion resumes immediately. This
is a heuristic, not enemy or block targeting, and needs controller testing.

### Zoom from the controller

Hold **RB / R1**, point either stick at **Zoom +** (closer) or **Zoom -**
(farther), then release RB / R1 to apply one step. Repeat to adjust further.
The wheel uses the same zoom levels and saved `zoomLevel` setting as the
Graphics menu (normally 1x through 32x), and disables a direction at its
limit. Modded zoom lists are respected. Existing keyboard shortcuts (`=`
and `-` by default) and the Graphics menu remain available; no controller
binding or gameplay profile needs to change. Each wheel change also displays
`Zoom: Nx` for two seconds using the existing message overlay.

## Input behavior

- Both axes are filtered together with the configured radial deadzone. A
  shallow diagonal retains its angle and a fully deflected diagonal has the
  same reach as a cardinal direction. SDL Y is converted at the point of
  use, so swapping which stick moves/aims no longer reverses vertical input.
- A continuous response curve gives fine control near center. Sensitivity
  changes the response curve; it does not extend the default directional
  cursor beyond tool reach. Legacy precise-cursor sensitivity still affects
  cursor speed.
- Default directional aim is anchored to the player and resolved using the
  current tool radius and camera zoom. A 0.75-tile margin keeps tile-center
  range checks valid at full deflection. Projectiles retain their normal
  range; this only places the aiming point. The last nonzero target follows
  the player when the stick returns to its deadzone, allowing a held action
  while walking. Mouse or touch aim takes ownership again when used.
- New configurations use a 20% trigger press threshold. Saved thresholds
  remain authoritative. Release uses an 8-percentage-point margin (with a
  1% floor), preventing noise near the press threshold from interrupting a
  held attack, charge, mining operation or block placement. The game keeps
  its own tool cooldowns and continuous-placement logic; no repeated clicks
  are injected for a Hold binding.
- Background/focus loss cancels gamepad input as well as touch input.
  Disconnect cancellation clears raw axes, trigger latches and active
  bindings. Releases queued by configuration changes survive the next frame.
- Inventory navigation targets the individual item slots rather than the
  whole grid or slot decorations. The visible in-game cursor follows D-pad
  focus; RT/LT use the existing left/right-click item operations. An active
  default aim stick does not overwrite focus while a window is open. Mouse
  movement resumes normal pointer behavior. Custom/scripted panes and text
  entry are not claimed to be fully controller-accessible.

## Reproducible automated checks

The standalone test uses production helpers and requires only C++17:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -I source/core \
  source/test/gamepad_input_test.cpp -o gamepad_input_tests
./gamepad_input_tests
```

It checks radial deadzones and 360-degree angle preservation, diagonal
normalization, inversion, response monotonicity, trigger holds under noisy
input (including existing 45% configurations), release, and tile-center
range boundaries. It is also a `NoAssets` CTest target named
`gamepad_input_tests` when `BUILD_TESTING` is enabled.

Validation performed for this change: 31,695 checks passed on Windows using
Zig 0.16.0's C++ compiler. The four changed implementation files
`StarMobileControls.cpp`, `StarMobilePlatform.cpp`, `StarMainInterface.cpp`
and `StarPaneManager.cpp` compiled individually to Windows objects with
`STAR_PLATFORM_MOBILE` and `STAR_PLATFORM_PC`, using SDL 3.2.28, ImGui 1.91.9
and GLEW 2.3.0 headers (the repository's pinned versions). This is a targeted
compile, not a linked desktop build. Existing unrelated deprecation/typeid
warnings were reported in frontend/windowing headers/code.

No Android APK, iOS, Switch package, full desktop executable or in-game
device session was built/run in this environment. The standalone tests do
not exercise SDL event routing or UI rendering. Before merging, test these
cases with the user's game assets on Android and a desktop launcher build:

1. Cardinal/shallow-diagonal aiming, slow near-player adjustment, full
   deflection at several zooms and render resolutions; move while holding
   the aim and then with the aim stick centered.
2. Hold RT/LT through mining, foreground/background placement, a charging
   weapon and alternate fire; fully release, then press again. Check
   configured Hold/Toggle/Repeat bindings separately.
3. Open inventory/container, use D-pad to reach empty/occupied slots and
   equipment, pick up/place a stack with RT and split with LT; close via
   Start. Check mouse and touch take over immediately after D-pad use.
4. Hold movement and a trigger, then disconnect/reconnect or background the
   app. Verify no stuck actions. Check disabled controls, inverted axes,
   swapped stick assignments and previously saved bindings.
5. Hold D-pad navigation and check initial delay, repeat, release and a
   rapid direction change. Hold R3/Shift with RT to transfer a stack between
   inventory and a chest; verify release ends Shift. Check single-block mining
   and incremental fine aim, plus the older saved R3 toggle mapping.
6. Open one wheel, press/release the other shoulder, and confirm only the
   original wheel's owner can confirm it.
7. Select Zoom + / Zoom - from the RB wheel, verify one step per release,
   both limits, fractional starting zoom, and the new value in Graphics.
   Verify cursor/tool reach at the changed zoom and persistence after restart.

Save formats, Android signing/packaging, APK compression and dependencies
are unchanged.
