// Distinct basename from any .cpp sibling to avoid Xcode object-file name
// collisions (same convention as StarIosFileAccessBridgeObjC.mm).

#include "mobile/ios/StarIosVsyncPacer.hpp"

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace {

std::mutex s_mutex;
std::condition_variable s_cv;
uint64_t s_frameCount = 0;
std::atomic<bool> s_running{false};
std::atomic<bool> s_startAttempted{false};
std::atomic<double> s_maxFps{60.0};
CFRunLoopRef s_pacerRunLoop = nullptr;

}

@interface StarVsyncPacerTarget : NSObject
- (void)onFrame:(CADisplayLink*)link;
- (void)threadMain:(id)unused;
@end

@implementation StarVsyncPacerTarget

- (void)onFrame:(CADisplayLink*)link {
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    ++s_frameCount;
  }
  s_cv.notify_all();
}

- (void)threadMain:(id)unused {
  @autoreleasepool {
    CADisplayLink* displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(onFrame:)];
    double maxFps = (double)UIScreen.mainScreen.maximumFramesPerSecond;
    if (maxFps < 30.0)
      maxFps = 60.0;
    s_maxFps.store(maxFps);
    if (@available(iOS 15.0, *)) {
      // Pin to the panel maximum; the game loop derives frame-cap intervals
      // from it rather than asking the link to throttle (a fixed link rate
      // keeps the counter a pure vsync count).
      displayLink.preferredFrameRateRange = CAFrameRateRangeMake((float)maxFps, (float)maxFps, (float)maxFps);
    }
    [displayLink addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
    s_pacerRunLoop = CFRunLoopGetCurrent();
    s_running.store(true);
    CFRunLoopRun();
    [displayLink invalidate];
    s_running.store(false);
  }
}

@end

namespace Star {
namespace IosVsyncPacer {

bool ensureStarted() {
  if (s_running.load())
    return true;
  if (s_startAttempted.exchange(true))
    return s_running.load();

  StarVsyncPacerTarget* target = [[StarVsyncPacerTarget alloc] init];
  NSThread* thread = [[NSThread alloc] initWithTarget:target selector:@selector(threadMain:) object:nil];
  thread.name = @"StarVsyncPacer";
  thread.qualityOfService = NSQualityOfServiceUserInteractive;
  [thread start];

  // Wait briefly for the run loop to come up so the first frame can pace.
  for (int i = 0; i < 100 && !s_running.load(); ++i)
    [NSThread sleepForTimeInterval:0.001];
  return s_running.load();
}

bool running() {
  return s_running.load();
}

void stop() {
  if (s_running.load() && s_pacerRunLoop) {
    CFRunLoopStop(s_pacerRunLoop);
    for (int i = 0; i < 100 && s_running.load(); ++i)
      [NSThread sleepForTimeInterval:0.001];
  }
  s_startAttempted.store(false);
}

double displayMaxFps() {
  return s_maxFps.load();
}

uint64_t waitForFrame(uint64_t lastSeen, int interval, double timeoutSeconds) {
  if (interval < 1)
    interval = 1;
  uint64_t target = lastSeen + (uint64_t)interval;
  std::unique_lock<std::mutex> lock(s_mutex);
  s_cv.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), [&] {
    return s_frameCount >= target;
  });
  return s_frameCount;
}

}
}
