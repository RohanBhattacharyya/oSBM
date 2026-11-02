# Building, mining and inventory feedback

## Target indicator

Holding a material, placeable object or the standard beam mining tool shows
a translucent target outline. Tools that implement the native tile preview
interface supply their actual preview tiles; otherwise the center target
tile is outlined. Large modded brushes are capped at 1,024 displayed tiles.
The center tile is still marked red when the native mining preview returns
no tiles because it is out of reach.

For materials, each tile uses the engine's `canModifyTile` check, including
material, hue, collision override and tile-center reach. Primary operation
previews foreground; holding secondary previews background for a two-handed
material item, matching the item's normal layer rule. One-handed secondary
items are checked separately. Green means placement is valid, red means it
is blocked or out of reach. Mixed footprints can show both colors.

Object ghosts have green/red borders using the object's native `canPlace`
result. Preview cache validity now includes placement validity and walking
direction, so a change in reach or a newly blocked tile at the same cursor
position refreshes the color. No placement permissions are changed.

Labels distinguish `Out of reach`, `Blocked` and `Partially blocked`.
Mining outlines indicate reach and target area, not a guarantee that a
protected or unbreakable block can be mined. Custom scripted tools can
supply their own previews and are not claimed to be fully covered.

The indicator is hidden with HUD, open windows and action wheels. Gameplay
state is captured during update; drawing consumes the cached target data
instead of querying mutable tools during Switch HUD replay.

## Inventory actions

Three text buttons appear in a separate strip above the inventory, using
drawn backgrounds rather than depending on a particular inventory skin:

- **Ordenar** condenses stacks and sorts the currently selected bag through
  the existing inventory methods, preserving their action-bar link handling.
- **Juntar** only condenses stacks in that bag.
- **Guardar materiais** sends placeable blocks and items with the native
  `craftingMaterial` category from inventory bags to the currently open chest.
  Equipment, cursor/trash slots and materials linked to the hotbar are kept.

These buttons use normal widget focus/click handling for mouse, touch and
D-pad + RT. Actions are disabled while a cursor item or an inventory swap/
material request is pending; storing additionally requires an open chest.

Store requests run one stack at a time, independently of ordinary swap-slot
RPC results. Leftovers are stacked back into the original slot, then given
to the player through the normal overflow handling. A terminal failed reply
or synchronous exception stops the batch and restores its saved stack.
Closing/changing the chest or closing inventory stops unsent requests; an
in-flight reply is still handled by MainInterface even while the pane is
dismissed. No timeout invents a successful/failed result. As with existing
inventory RPCs, transport failures and quitting mid-request still require
multiplayer/device testing; this is not a new persistence transaction system.

## Validation and remaining checks

Targeted Windows C++17 object compilation passed for `StarInventory.cpp`,
`StarMainInterface.cpp`, `StarToolUser.cpp` and `StarMaterialItem.cpp` with
the desktop-launcher defines and the repository headers. `git diff --check`
passed. No assets-dependent gameplay tests, visual runtime verification,
linked desktop build or Android/iOS/Switch package build was run.

Before merging, check:

1. Single/multiple-block and Shift footprints; near/far targets at several
   zooms, world wrap and screen edges. Verify HUD/windows hide the overlay.
2. Foreground/background, occupied/protected tiles, unsupported placement,
   partial placement and zero inventory. Green/red must match actual use.
3. Object orientation, moving out of range and terrain changing under an
   unmoving cursor. The ghost border must refresh.
4. Touch, mouse and D-pad selection of all three buttons, including a small
   screen, alternate interface scales and custom inventory skins.
5. Separate/full stacks, sorted hotbar links and cursor items. Store into
   empty, nearly full and full chests; confirm total item counts before/after,
   original-slot leftovers and exclusion of hotbar/equipment/other items.
6. Close/switch chests, dismiss inventory, disconnect and receive a failed or
   delayed reply during a batch. Verify no additional stack is sent and no
   result is confused with a normal Shift transfer or chest-slot click.
