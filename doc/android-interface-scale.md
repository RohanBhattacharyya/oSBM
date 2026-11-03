# Android manual interface scale

The Android GUI getter previously limited **every** scale to
`max(1, renderHeight / 500)`, after multiplying by display density. At a
960x540 render size, Small (1.35), Medium (1.8) and Large (2.4) could all
become 1.08. Saving a different value therefore had no visible effect.

Manual Android values now directly represent render pixels per interface
pixel, matching the existing iOS convention. Only AUTO retains the previous
density adjustment and menu-height safety cap. Existing configuration keys
and launcher choices are preserved. The in-game graphics menu still applies
its changes with Accept; the launcher choice still takes effect next launch.
Manual sizes can intentionally exceed the space available for a large pane;
AUTO remains the fit-to-screen choice and can be restored in the launcher.

Pane recordings are invalidated when effective scale changes so their old
pixel coordinates cannot be replayed with new layout/input coordinates.
Mouse, touch and gamepad UI navigation continue to use the same effective
GUI scale for hit testing. Desktop/desktop-launcher scale and DPI behavior
are unchanged; iOS keeps its existing scale behavior. No save format changes.

The asset-free `interface_scale_tests` target exercises the production scale
policy with five render heights and five densities: manual sizes stay
distinct, can shrink again, ignore Android density multiplication, and AUTO
retains its short-screen fit. It is registered with CTest as `NoAssets`.

Validation: the standalone Windows test passed 102 checks; `StarGuiContext`,
`StarPaneManager` and `StarClientApplication` compiled in both Windows desktop
and desktop-launcher variants (six object builds). Android packaging is left
to GitHub Actions at the user's request. No iOS/Switch/device execution of
this scale change was performed.
