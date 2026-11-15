# Mobile controls

The launcher defaults to **Controls/Overlay → Control layout → Revamped**.
Choose **Legacy** to restore the original controls. Each layout has its own
saved positions, sizes and bindings; switching layouts preserves both.
Existing `touch.elements` settings belong to Legacy. The new layout is stored
in `touch.revampedElements` and selected by `touch.layout`.

The revamped layout uses Ballefroide's original art from
[issue #65](https://github.com/RohanBhattacharyya/oSBM/issues/65). Controls are
hidden on the title screen. Opening an inventory or other window hides and
releases the right-hand controls, leaving ordinary pointer input for the UI.

- The aim-mode button cycles **left hand → right hand → both → aim only**.
  Deflecting the aim stick uses the selected hand(s); releasing it stops them.
- Hotbar arrows scroll slots. The numbered hotbar button switches groups.
- Walk toggles Shift, which also provides precise matter-manipulator mining.
  It suspends while windows are open so inventory taps are not Shift-clicks.
- Look around holds Ctrl; jump uses Space; head tech uses F.
- Dash sends a directional double tap in the movement/facing direction. It
  briefly releases the movement stick's held key so the two taps register.
  This follows vanilla body-tech controls. Mods with different activation
  schemes can use custom buttons/macros or the Legacy layout.
- Hide controls cycles **full → sticks → hidden HUD**. The restore button and
  pause button remain accessible. HUD visibility does not depend on the F1
  key binding.

The official **oSBM: Mobile HUD** mod is bundled and enabled automatically.
Disable it in the launcher's **Mod Manager** and relaunch the game to restore
the original HUD. This keeps the touch artwork available. The mod enlarges
the player HUD and hotbar, moves crafting and secondary panes into a quick
menu, and keeps the mech button off planets. It does not require an external
Quickbar mod or replace the player's UI-scale preference.

Settings → Credits identifies the maintainer, oSBM contributors and artist.
See [contributing.md](contributing.md) when accepting future contributions.

Asset-sandbox and control ownership regressions run in the desktop `NoAssets`
suites. They require only the bundled distributable files, never `packed.pak`.
Actual game rendering, touch feel, device lifecycle and mod combinations
still require runtime testing.
