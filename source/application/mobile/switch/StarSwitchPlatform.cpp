#include "StarSwitchPlatform.hpp"

#include "StarFile.hpp"
#include "StarLogging.hpp"

#ifdef STAR_SYSTEM_SWITCH
#include <switch.h>
#include <sys/stat.h>
#include <dirent.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#endif

namespace Star {

#ifdef STAR_SYSTEM_SWITCH

namespace {

  std::atomic_flag g_initStarted = ATOMIC_FLAG_INIT;
  bool g_romfsMounted = false;

  void copyDirectoryRecursive(String const& source, String const& target) {
    if (!File::isDirectory(source))
      return;
    File::makeDirectoryRecursive(target);
    for (auto const& entry : File::dirList(source)) {
      String childSource = File::relativeTo(source, entry.first);
      String childTarget = File::relativeTo(target, entry.first);
      // entry.second is true for directories; fall back to an explicit check.
      if (entry.second || File::isDirectory(childSource))
        copyDirectoryRecursive(childSource, childTarget);
      else if (!File::exists(childTarget))
        File::copy(childSource, childTarget);
    }
  }

}

void switchPlatformInit() {
  if (g_initStarted.test_and_set())
    return;

  Result rc = romfsInit();
  g_romfsMounted = R_SUCCEEDED(rc);
  switchDebugLog(g_romfsMounted ? "switchPlatformInit: romfs mounted"
                                : "switchPlatformInit: romfs init FAILED");

  // Pre-create the storage directories with plain mkdir. Star's
  // File::makeDirectoryRecursive walks parents up to "/" via dirName and tests
  // each with stat(); on libnx's fsdev default device stat("/") fails, which
  // would send makeDirectoryRecursive into unbounded recursion. Creating the
  // path components directly here means the recursion always stops at an
  // existing directory and never reaches "/".
  mkdir("/switch", 0777);
  mkdir("/switch/oSBM", 0777);
  switchDebugLog("switchPlatformInit: storage dirs ensured at /switch/oSBM");

  // Give Star a real, writable temp directory. Without this, temporaryRootDirectory()
  // falls back to newlib's P_tmpdir ("/tmp"), which does not exist on the SD card;
  // File::ephemeralFile()/temporaryDirectory() (used when loading a ship/world)
  // would makeDirectoryRecursive("/tmp"), walk up to "/", and -- since stat("/")
  // fails on libnx -- self-recurse until the stack overflows. Pre-create the dir
  // and export TMPDIR so temporaryRootDirectory() returns it directly (no walk).
  mkdir("/switch/oSBM/tmp", 0777);
  setenv("TMPDIR", "/switch/oSBM/tmp", 1);

  // Clear stale ephemeral temp files left by previous sessions. On libnx an open
  // file cannot be unlinked, so File::ephemeralFile() leaves its temp file on
  // disk for the session; wipe them here on the next launch so they do not pile
  // up. Raw readdir/remove avoids the Star File exception paths during init.
  if (DIR* tmpDir = ::opendir("/switch/oSBM/tmp")) {
    for (dirent* e = ::readdir(tmpDir); e != NULL; e = ::readdir(tmpDir)) {
      if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
        continue;
      char p[768];
      snprintf(p, sizeof(p), "/switch/oSBM/tmp/%s", e->d_name);
      ::remove(p);
    }
    ::closedir(tmpDir);
  }
  switchDebugLog("switchPlatformInit: TMPDIR set to /switch/oSBM/tmp (stale temp cleared)");

  // Bring up the libnx socket stack so libcurl / cpr networking works. Done
  // last and best-effort: the launcher and game remain usable without it, and
  // keeping it after storage setup ensures a socket failure cannot block the
  // writable root from being created.
  socketInitializeDefault();
  switchDebugLog("switchPlatformInit: socket init done");

  // Pin the main thread (which runs the game loop = client update + rendering)
  // to the first allowed core, and hint a preferred core so Horizon keeps it
  // there. Star's worker/server/lighting threads are round-robined onto the
  // OTHER allowed cores in StarThread_unix.cpp's switchDistributeCurrentThread,
  // which reserves this core[0] for the main thread. Without this, every libnx
  // pthread defaults to the same core and the engine runs effectively
  // single-threaded -- the dominant cause of the in-game 2-5 FPS.
  {
    u64 coreMask = 0;
    if (R_SUCCEEDED(svcGetInfo(&coreMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
      int firstCore = -1;
      for (int core = 0; core < 4; ++core) {
        if (coreMask & (1ull << core)) { firstCore = core; break; }
      }
      if (firstCore >= 0)
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, firstCore, coreMask);
    }
    switchDebugLog("switchPlatformInit: main thread core affinity set");
  }
}

String switchDefaultStorageRoot() {
  // Deterministic, writable location on the SD card. We use a plain absolute
  // path (NOT the "sdmc:/" devoptab prefix): libnx routes "/" to the default
  // device, which is the SD card, and Star's File path code (dirName /
  // makeDirectoryRecursive) cannot walk a colon-prefixed device path — it would
  // recurse onto "sdmc:" and fail. "/switch/oSBM" maps to sdmc:/switch/oSBM. No
  // trailing slash, so File::isDirectory/makeDirectory see the exact directory
  // that switchPlatformInit() pre-creates with mkdir.
  return "/switch/oSBM";
}

void switchDebugLog(char const* msg) {
  // Surface diagnostics to the host (Ryujinx logs this as guest output; on
  // hardware it is visible to a debugger). The shared launcher routes
  // androidLogInfo() here on Switch.
  if (msg)
    svcOutputDebugString(msg, std::strlen(msg));
}

namespace {
  // Mirrors every engine Logger message to the host debug channel so failures
  // before the on-SD log file exists (and hard faults that never reach the
  // exception handler) are still visible in the Ryujinx guest log.
  class SwitchLogSink : public LogSink {
  public:
    void log(char const* msg, LogLevel) override {
      if (msg)
        svcOutputDebugString(msg, std::strlen(msg));
    }
  };
}

namespace {
  // An uncaught exception (or a noexcept violation / throw during unwinding)
  // ends in std::terminate -> abort -> svcBreak, which Ryujinx reports as a
  // bare "guest broke execution" with no message and no engine log line. Hook
  // std::terminate so the offending exception's text reaches the host debug
  // channel before we abort, turning a silent fatal into a diagnosable one.
  void switchTerminateHandler() {
    switchDebugLog("std::terminate called");
    if (auto eptr = std::current_exception()) {
      try {
        std::rethrow_exception(eptr);
      } catch (std::exception const& e) {
        switchDebugLog((std::string("terminate: uncaught exception: ") + e.what()).c_str());
        // Also record to the engine log file (starbound.log on the SD card) so the
        // failure is diagnosable on real hardware, where the debug channel above is
        // not visible without an attached debugger.
        Logger::error("[switch] terminate: uncaught exception: {}", e.what());
      } catch (...) {
        switchDebugLog("terminate: uncaught non-std exception");
        Logger::error("[switch] terminate: uncaught non-std exception");
      }
    } else {
      switchDebugLog("terminate: no active exception (direct abort/assert)");
      Logger::error("[switch] terminate: no active exception (direct abort/assert)");
    }
    std::abort();
  }
}

void switchInstallLogSink() {
  static LogSinkPtr sink = []() {
    auto s = make_shared<SwitchLogSink>();
    s->setLevel(LogLevel::Info);
    return s;
  }();
  Logger::addSink(sink);
  std::set_terminate(&switchTerminateHandler);
}

void switchSyncBundledAssets(String const& storageRoot) {
  switchPlatformInit();
  if (!g_romfsMounted) {
    switchDebugLog("switchSyncBundledAssets: romfs not mounted, skipping");
    return;
  }
  String target = File::relativeTo(storageRoot, "bundled_assets");
  // Only the launcher lang dir + font are needed before the launcher renders;
  // they are already present, so a repeat sync is a cheap no-op (copy skips
  // existing files). The first run copies the bundled assets and can take a few
  // seconds on the SD card, so bracket it with log lines for visibility.
  switchDebugLog("switchSyncBundledAssets: start (romfs:/bundled_assets -> SD)");
  try {
    copyDirectoryRecursive("romfs:/bundled_assets", target);
    switchDebugLog("switchSyncBundledAssets: done");
  } catch (std::exception const& e) {
    Logger::error("switchSyncBundledAssets failed: {}", e.what());
    switchDebugLog("switchSyncBundledAssets: FAILED");
  }
}

#else

void switchPlatformInit() {}
void switchSyncBundledAssets(String const&) {}
String switchDefaultStorageRoot() { return {}; }
void switchDebugLog(char const*) {}
void switchInstallLogSink() {}

#endif

}
