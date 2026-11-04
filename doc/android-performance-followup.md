# Follow-up after the world opened under HWASan

The user reported successful world entry with diagnostic APK `2fe7c09`, but
slower startup, loading and gameplay. The WA0080 export confirms two sessions
from that source commit. It contains no HWASan report or new native tombstone;
the three exit-history entries are `SwipeUpClean`. The fatal error in
`starbound.log.3` belongs to the older `300c3e7` build, not the new sessions.
Absence of a report is not proof that the underlying corruption is fixed.

## Observations

- `Root: Loaded everything` took 71.77 and 89.46 seconds in the diagnostic
  sessions, versus 31.05 seconds in the older crashing Release log. Startup
  caches, device state and instrumentation differ; this is not a controlled
  performance benchmark.
- In the latest session, Root preparation to title menus ready took 77.15 s.
- Connection to the server's protectorate warp command took 119.44 s and
  132.95 s in the two sessions. These intervals include world preparation;
  they are not precise measurements of when the first playable frame appeared.
- The last four in-world FPS samples of the latest session were 14.8, 16.3,
  15.4 and 14.2. The other successful session ended with samples around 20–26.
- Android's `snap`, `paint` and interface-render accumulators are compiled out
  and log zero. Those fields cannot identify GPU/render cost on this device.
- The successful APK used HWASan, O1, no LTO, shared libc++ and the system
  allocator. These changes confound any attempt to blame rpmalloc alone.

The next comparison is normal optimized Release/arm64 (O3, LTO, static libc++),
with `STAR_ANDROID_SYSTEM_ALLOCATOR=ON` and no HWASan. This keeps the system
allocator as the isolation variable relative to normal Release. The user's
preference is now GitHub Actions for APK compilation; the local build was
interrupted and no local compiler remained active when checked.

The Android workflow has an opt-in `system_allocator` boolean, disabled by
default, passed to CMake and identified in the artifact name. Choose Release,
arm64 and enable that input for this comparison. Existing signing logic and
secrets are unchanged. CI and the previously installed local APK can have
different signing certificates; verify certificates before attempting an update.

## Additional confirmed UI defects

The export has no launcher `display.interfaceScale` override. The manual GUI
scale issue is explained by the unconditional Android cap in `GuiContext`;
see [android-interface-scale.md](android-interface-scale.md) for the fix and
its validation. Existing input coordinate conversion uses the effective scale.

One diagnostic session logged a nonfatal error opening voice settings:
`settings.inputMode` was nil. The voice Lua binding returns an empty object
when the backend is unavailable. The panel now dismisses before initializing
backend-dependent controls, and its update callback also tolerates that state.
A standalone test executes the actual UI script with empty/malformed settings
and both normal voice modes: five lifecycle checks passed. It uses mocked
engine callbacks, not a real audio device. This error is separate from the
earlier native crash and does not establish the cause of the low FPS.

No private ZIP, logs, saves, signing material or game `packed.pak` are committed.
Device testing of the optimized comparison and GUI change is still required.
