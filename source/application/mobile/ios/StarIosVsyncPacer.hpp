#pragma once

#include <cstdint>

namespace Star {

// Display-link frame pacer for iOS. presentRenderbuffer never blocks and the
// GLES backend has no swap-interval support, so a software-slept loop drifts
// freely across the vsync grid (repeated/dropped presents perceived as jitter
// and double-image ghosting during fast motion). This runs a CADisplayLink on
// its own dedicated thread's run loop -- the SDL main thread never spins the
// UIKit run loop reliably enough to host it -- and the game loop blocks on
// its signal instead of sleeping, locking frame starts to the display grid.
namespace IosVsyncPacer {

  // Idempotent; safe to call every frame. Returns false if the pacer could
  // not be started (callers should fall back to software pacing).
  bool ensureStarted();

  bool running();

  void stop();

  // Panel's maximum refresh rate (60, or 120 on ProMotion once the
  // CADisableMinimumFrameDurationOnPhone Info.plist key permits it).
  double displayMaxFps();

  // Block until the display-link frame counter reaches lastSeen + interval,
  // or until timeoutSeconds elapses (display link paused/backgrounded).
  // Returns the current counter value; pass it back as lastSeen next frame.
  // Returns immediately when the loop is already running behind (counter has
  // moved past the target), so slow frames slip vsyncs without extra delay.
  uint64_t waitForFrame(uint64_t lastSeen, int interval, double timeoutSeconds);

}

}
