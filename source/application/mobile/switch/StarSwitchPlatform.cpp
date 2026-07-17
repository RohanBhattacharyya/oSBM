#include "StarSwitchPlatform.hpp"

#include "StarFile.hpp"
#include "StarLogging.hpp"

#ifdef STAR_SYSTEM_SWITCH
#include <switch.h>

// Guest exception handler: when any thread faults (data abort, etc.), log
// the faulting PC/LR as module-relative offsets before libnx's fatal path
// runs -- Ryujinx's own fatal report often carries a zeroed context, which
// makes crashes on worker threads undiagnosable without this.
#include <fcntl.h>
#include <unistd.h>

extern "C" char __start__; // module base (libnx runtime symbol)

// ---- C++ exception unwinding repair -----------------------------------------
// libgcc's FDE lookup (registered-object search in unwind-dw2-fde) returns
// NULL for valid PCs in this binary (verified: the FDE table is complete and
// well-formed via readelf, a manual __register_frame_info + lookup still
// misses, while a minimal NRO with a small table works). The unwinder then
// fails its gcc_assert in uw_init_context_1 and abort()s -- every single
// C++ `throw` killed the process ("The software was closed because an error
// occurred"), first noticed when FrackinUniverse's json-patch test operations
// (throw-as-control-flow) crashed the boot.
//
// Repair: wrap _Unwind_Find_FDE (--wrap intercepts even libgcc-internal
// calls, same as the fatalThrow wrap). Try the real lookup first; when it
// misses, binary-search the linker's own .eh_frame_hdr table (built by ld
// --eh-frame-hdr; correct by construction) between the linker-script bounds
// __eh_frame_hdr_start/__eh_frame_hdr_end.
struct StarDwarfEhBases { void* tbase; void* dbase; void* func; };
extern "C" void* __real__Unwind_Find_FDE(void*, StarDwarfEhBases*);
extern "C" char __eh_frame_hdr_start[];
extern "C" char __eh_frame_hdr_end[];

extern "C" void* __wrap__Unwind_Find_FDE(void* pc, StarDwarfEhBases* bases) {
  if (void* fde = __real__Unwind_Find_FDE(pc, bases))
    return fde;

  unsigned char const* hdr = (unsigned char const*)__eh_frame_hdr_start;
  if (__eh_frame_hdr_end - __eh_frame_hdr_start < 16)
    return nullptr;
  // Layout emitted by GNU ld: version 1; eh_frame_ptr encoded pcrel|sdata4
  // (0x1b); fde_count udata4 (0x03); table datarel|sdata4 (0x3b) sorted by
  // initial location. Bail on anything else rather than misparse.
  if (hdr[0] != 1 || hdr[1] != 0x1b || hdr[2] != 0x03 || hdr[3] != 0x3b)
    return nullptr;
  unsigned fdeCount;
  memcpy(&fdeCount, hdr + 8, 4);
  struct Entry { int32_t initialLoc; int32_t fde; };
  Entry const* table = (Entry const*)(hdr + 12);
  if (fdeCount == 0 || (unsigned char const*)(table + fdeCount) > (unsigned char const*)__eh_frame_hdr_end)
    return nullptr;

  intptr_t rel = (intptr_t)pc - (intptr_t)hdr; // pc as datarel offset
  if (rel < table[0].initialLoc)
    return nullptr;
  unsigned lo = 0, hi = fdeCount;
  while (hi - lo > 1) {
    unsigned mid = lo + (hi - lo) / 2;
    if (table[mid].initialLoc <= rel)
      lo = mid;
    else
      hi = mid;
  }
  if (bases) {
    bases->tbase = nullptr;
    bases->dbase = (void*)hdr;
    bases->func = (void*)(hdr + table[lo].initialLoc);
  }
  return (void*)(hdr + table[lo].fde);
}

// Best-effort persistent crash breadcrumb: svcOutputDebugString is invisible
// on real hardware ("The software was closed because an error occurred" with
// no report), so mirror crash-handler output to a file the user can pull.
// Raw fd I/O only -- no stdio buffering, no engine types, crash-context safe
// as far as the fs layer allows.
static void starSwitchCrashLog(char const* msg, size_t len) {
  int fd = open("/switch/oSBM/crash.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
    return;
  write(fd, msg, len);
  write(fd, "\n", 1);
  fsync(fd);
  close(fd);
}

extern "C" {
alignas(16) unsigned char __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
// Override libnx's fatalThrow (strong symbol beats the library's) purely to
// log WHO throws it: intermittent libnx-module fatals (e.g. 345-8
// NotInitialized) otherwise present as a spontaneous clean exit with a
// zeroed context and no attribution whatsoever.
void NX_NORETURN __real_fatalThrow(Result err);

// Diagnostics for svcBreak-style aborts (Ryujinx reports "guest program
// broke execution" with a zeroed context, hiding the cause). These catch
// the two most likely sources on a worker thread: an uncaught C++ exception
// (std::terminate) and a stack-canary failure (__stack_chk_fail).
extern "C" void __stack_chk_fail(void) {
  const char* m = "ABORT: __stack_chk_fail (stack buffer overflow detected)";
  svcOutputDebugString(m, strlen(m));
  starSwitchCrashLog(m, strlen(m));
  for (;;) svcSleepThread(1000000000ull);
}
static void starSwitchTerminateHandler() {
  const char* m = "ABORT: std::terminate";
  svcOutputDebugString(m, strlen(m));
  starSwitchCrashLog(m, strlen(m));
  if (auto ex = std::current_exception()) {
    try { std::rethrow_exception(ex); }
    catch (std::exception const& e) {
      char buf[256];
      int len = snprintf(buf, sizeof(buf), "ABORT: uncaught exception: %s", e.what());
      if (len > 0) {
        svcOutputDebugString(buf, (size_t)len);
        starSwitchCrashLog(buf, (size_t)len);
      }
    } catch (...) {
      const char* m2 = "ABORT: uncaught non-std exception";
      svcOutputDebugString(m2, strlen(m2));
      starSwitchCrashLog(m2, strlen(m2));
    }
  }
  for (;;) svcSleepThread(1000000000ull);
}
struct StarSwitchTerminateInstaller {
  StarSwitchTerminateInstaller() { std::set_terminate(starSwitchTerminateHandler); }
};
static StarSwitchTerminateInstaller s_starSwitchTerminateInstaller;

void NX_NORETURN __wrap_fatalThrow(Result err) {
  extern char __start__;
  u64 base = (u64)&__start__;
  u64 caller = (u64)__builtin_return_address(0);
  char buf[192];
  int len = snprintf(buf, sizeof(buf), "FATALTHROW: result=0x%x (%u-%u) caller=0x%llx (mod+0x%llx)",
      err, (unsigned)(2000 + R_MODULE(err)), (unsigned)R_DESCRIPTION(err),
      (unsigned long long)caller, (unsigned long long)(caller - base));
  if (len > 0) {
    svcOutputDebugString(buf, (size_t)len);
    starSwitchCrashLog(buf, (size_t)len);
  }
  __real_fatalThrow(err);
  for (;;) svcSleepThread(1000000000ull);
}

// libnx's INTERNAL result assertions (e.g. in applet.o's message pump) abort
// via diagAbortWithResult, NOT the public fatalThrow -- so wrapping fatalThrow
// never saw them. --wrap redirects even the intra-libnx calls at final link.
// The post-warp "2345-0008 NotInitialized" fatal comes through here: an async
// applet focus/operation-mode message hits an uninitialised service under
// Ryujinx. Log WHO aborts (result + caller offset + a few frames), then SWALLOW
// it (return) -- these applet ops are non-critical, and continuing as if the
// call succeeded keeps the game alive instead of taking a spurious fatal.
extern "C" void NX_NORETURN __real_diagAbortWithResult(Result res);
extern "C" void __wrap_diagAbortWithResult(Result res) {
  extern char __start__;
  u64 base = (u64)&__start__;
  u64 c0 = (u64)__builtin_return_address(0);
  char buf[256];
  int len = snprintf(buf, sizeof(buf),
      "DIAGABORT (swallowed): result=0x%x (%u-%u) caller=0x%llx (mod+0x%llx)",
      res, (unsigned)(2000 + R_MODULE(res)), (unsigned)R_DESCRIPTION(res),
      (unsigned long long)c0, (unsigned long long)(c0 - base));
  if (len > 0) {
    svcOutputDebugString(buf, (size_t)len);
    starSwitchCrashLog(buf, (size_t)len);
  }
  // mini frame-pointer backtrace for the abort site
  u64 fp = (u64)__builtin_frame_address(0);
  for (int i = 0; i < 5 && fp >= 0x1000; ++i) {
    u64 frame[2];
    memcpy(frame, (void*)fp, sizeof(frame));
    u64 ret = frame[1];
    if (ret < base || ret > base + 0x40000000ull) break;
    int l2 = snprintf(buf, sizeof(buf), "  DIAGABORT frame[%d] ret=0x%llx (mod+0x%llx)",
        i, (unsigned long long)ret, (unsigned long long)(ret - base));
    if (l2 > 0)
      svcOutputDebugString(buf, (size_t)l2);
    fp = frame[0];
  }
  // Swallow: return to caller as though the failed libnx op had succeeded.
}

// The post-warp shutdown presents as the libnx appletExit() atexit sequence
// (SetFocusHandlingMode -> SetOutOfFocusSuspendingEnabled -> Am::Exit) followed
// by a teardown fatal -- i.e. the PROCESS is exiting, but none of our quit-path
// logs fire. Something calls exit()/_exit() directly. Wrap them (and appletExit)
// to capture the caller chain so we can attribute the spurious shutdown.
static void starSwitchLogBacktrace(const char* tag) {
  extern char __start__;
  u64 base = (u64)&__start__;
  char buf[256];
  int len = snprintf(buf, sizeof(buf), "%s: caller=0x%llx (mod+0x%llx)", tag,
      (unsigned long long)(u64)__builtin_return_address(0),
      (unsigned long long)((u64)__builtin_return_address(0) - base));
  if (len > 0) svcOutputDebugString(buf, (size_t)len);
  u64 fp = (u64)__builtin_frame_address(0);
  for (int i = 0; i < 8 && fp >= 0x1000; ++i) {
    u64 frame[2];
    memcpy(frame, (void*)fp, sizeof(frame));
    u64 ret = frame[1];
    if (ret >= base && ret <= base + 0x40000000ull) {
      int l2 = snprintf(buf, sizeof(buf), "  %s frame[%d] ret=mod+0x%llx", tag, i,
          (unsigned long long)(ret - base));
      if (l2 > 0) svcOutputDebugString(buf, (size_t)l2);
    }
    if (frame[0] <= fp) break;
    fp = frame[0];
  }
}
extern "C" void NX_NORETURN __real_exit(int code);
extern "C" void NX_NORETURN __wrap_exit(int code) {
  char b[64]; int l = snprintf(b, sizeof(b), "EXITCALL: exit(%d)", code);
  if (l > 0) svcOutputDebugString(b, (size_t)l);
  starSwitchLogBacktrace("EXITCALL");
  __real_exit(code);
  for (;;) svcSleepThread(1000000000ull);
}
extern "C" void NX_NORETURN __real__exit(int code);
extern "C" void NX_NORETURN __wrap__exit(int code) {
  char b[64]; int l = snprintf(b, sizeof(b), "EXITCALL: _exit(%d)", code);
  if (l > 0) svcOutputDebugString(b, (size_t)l);
  starSwitchLogBacktrace("_EXITCALL");
  __real__exit(code);
  for (;;) svcSleepThread(1000000000ull);
}
extern "C" void __real_appletExit(void);
extern "C" void __wrap_appletExit(void) {
  svcOutputDebugString("APPLETEXIT", 10);
  starSwitchLogBacktrace("APPLETEXIT");
  __real_appletExit();
}

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  extern char __start__;
  u64 base = (u64)&__start__;
  char buf[320];
  int len = snprintf(buf, sizeof(buf),
      "GUEST EXCEPTION: desc=0x%x pc=0x%llx (mod+0x%llx) lr=0x%llx (mod+0x%llx) far=0x%llx sp=0x%llx x0=0x%llx x8=0x%llx x19=0x%llx",
      ctx->error_desc,
      (unsigned long long)ctx->pc.x, (unsigned long long)(ctx->pc.x - base),
      (unsigned long long)ctx->lr.x, (unsigned long long)(ctx->lr.x - base),
      (unsigned long long)ctx->far.x, (unsigned long long)ctx->sp.x,
      (unsigned long long)ctx->cpu_gprs[0].x, (unsigned long long)ctx->cpu_gprs[8].x,
      (unsigned long long)ctx->cpu_gprs[19].x);
  if (len > 0) {
    svcOutputDebugString(buf, (size_t)len);
    starSwitchCrashLog(buf, (size_t)len);
  }
  // Also walk a few stack frames via the frame pointer for a mini backtrace.
  u64 fp = ctx->fp.x;
  for (int i = 0; i < 6 && fp >= 0x1000; ++i) {
    u64 frame[2];
    // The fault handler runs on its own stack; the faulting thread's frames
    // are still mapped, but guard against garbage fp values.
    if (fp & 7)
      break;
    memcpy(frame, (void*)fp, sizeof(frame));
    len = snprintf(buf, sizeof(buf), "GUEST EXCEPTION FRAME %d: lr=0x%llx (mod+0x%llx)", i,
        (unsigned long long)frame[1], (unsigned long long)(frame[1] - base));
    if (len > 0) {
      svcOutputDebugString(buf, (size_t)len);
      starSwitchCrashLog(buf, (size_t)len);
    }
    fp = frame[0];
  }
}
}

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

  {
    // Boot marker in the crash file so any handler entries that follow can be
    // attributed to a specific launch/build. Also record the runtime address
    // of a known function: crash-handler backtraces print raw addresses, and
    // subtracting this anchor (vs its ELF vaddr from nm) symbolizes them even
    // when __start__ resolves uselessly under a given loader.
    char b[192];
    int l = snprintf(b, sizeof(b), "---- boot %s %s ---- anchor switchPlatformInit=%p __start__=%p",
        __DATE__, __TIME__, (void*)&switchPlatformInit, (void*)&__start__);
    if (l > 0) {
      starSwitchCrashLog(b, (size_t)l);
      svcOutputDebugString(b, (size_t)l);
    }
  }

  // Unwinder self-check: exercise a real throw/catch once at boot and record
  // the result. If exception unwinding regresses again (toolchain update,
  // link-flag change), this makes it visible in crash.txt/logs immediately
  // instead of as an unexplained crash on the first gameplay throw.
  {
    StarDwarfEhBases bases{};
    void* pc = (void*)((char*)&switchPlatformInit + 8);
    void* fde = __wrap__Unwind_Find_FDE(pc, &bases);
    bool caught = false;
    try {
      throw std::runtime_error("probe");
    } catch (std::exception const&) {
      caught = true;
    }
    char b[96];
    int l = snprintf(b, sizeof(b), "unwind self-check: fde=%p throw=%s",
        fde, caught ? "OK" : "NOT CAUGHT");
    if (l > 0) {
      svcOutputDebugString(b, (size_t)l);
      starSwitchCrashLog(b, (size_t)l);
    }
  }

  // Capture newlib stderr/stdout: mesa (the statically linked GL driver), C
  // assert(), and other third-party fatal paths print their reason there and
  // then _exit(1) -- without this redirect those messages vanish and a driver
  // fatal presents as a silent crash with no report. stderr is unbuffered so
  // the message survives an _exit that skips stdio flushing.
  if (freopen("/switch/oSBM/stderr.txt", "w", stderr))
    setvbuf(stderr, nullptr, _IONBF, 0);
  if (freopen("/switch/oSBM/stdout.txt", "w", stdout))
    setvbuf(stdout, nullptr, _IOLBF, 0);

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

  // Initialize the time service explicitly: libnx routes gettimeofday /
  // clock_gettime(CLOCK_REALTIME) through it and fatalThrows
  // LibnxError_NotInitialized (345-8) from any thread that asks for wall
  // clock time when it is missing -- which presents as a spontaneous clean
  // process exit with a zeroed fatal context, minutes into gameplay, when
  // the first epoch-timestamped storage write happens.
  {
    Result rc = timeInitialize();
    if (R_FAILED(rc))
      switchDebugLog("switchPlatformInit: timeInitialize FAILED");
    else
      switchDebugLog("switchPlatformInit: time service initialized");
  }

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

namespace {
  struct ClockBoostState {
    bool active = false;
    u32 prevCpu = 0;
    u32 prevGpu = 0;
    u32 prevEmc = 0;
  };
  ClockBoostState g_clockBoost;

  // Sets a pcv module clock, but only ever RAISES it (docked profiles are
  // already above the targets and must not be pulled down). Returns the rate
  // that was in effect before the call, 0 on failure.
  u32 clockRaise(PcvModuleId module, u32 targetHz, char const* name) {
    ClkrstSession session;
    Result rc = clkrstOpenSession(&session, module, 3);
    char buf[160];
    if (R_FAILED(rc)) {
      snprintf(buf, sizeof(buf), "clockRaise: %s open session failed (0x%x)", name, rc);
      switchDebugLog(buf);
      return 0;
    }
    u32 before = 0;
    clkrstGetClockRate(&session, &before);
    if (targetHz > before)
      rc = clkrstSetClockRate(&session, targetHz);
    u32 after = 0;
    clkrstGetClockRate(&session, &after);
    clkrstCloseSession(&session);
    snprintf(buf, sizeof(buf), "clockRaise: %s %u -> %u MHz (rc=0x%x)",
        name, before / 1000000u, after / 1000000u, rc);
    switchDebugLog(buf);
    Logger::info("[switch-clk] {}: {} -> {} MHz", name, before / 1000000u, after / 1000000u);
    return before;
  }
}

void switchApplyClockBoost() {
  if (g_clockBoost.active)
    return;
  Result rc = clkrstInitialize();
  if (R_FAILED(rc)) {
    switchDebugLog("switchApplyClockBoost: clkrst unavailable");
    Logger::info("[switch-clk] clkrst unavailable (0x{:x}); running at system clocks", rc);
    return;
  }
  // Top official handheld-legal profile: CPU 1224MHz (vs 1020 default), GPU
  // 460.8MHz (vs 307/384 handheld default), EMC 1600MHz (vs 1331). Docked
  // rates are already at or above these and are left untouched (raise-only).
  // Walking on a planet is CPU- and GPU-bound at handheld floor clocks
  // (measured: ~22ms CPU frame + pacing-fence waits); these rates are what
  // boost-mode games use and are well within the console's cooling envelope.
  g_clockBoost.prevCpu = clockRaise(PcvModuleId_CpuBus, 1224000000u, "cpu");
  g_clockBoost.prevGpu = clockRaise(PcvModuleId_GPU, 460800000u, "gpu");
  g_clockBoost.prevEmc = clockRaise(PcvModuleId_EMC, 1600000000u, "mem");
  g_clockBoost.active = true;
  clkrstExit();
}

void switchRestoreClocks() {
  if (!g_clockBoost.active)
    return;
  g_clockBoost.active = false;
  if (R_FAILED(clkrstInitialize()))
    return;
  auto restore = [](PcvModuleId module, u32 prevHz) {
    if (prevHz == 0)
      return;
    ClkrstSession session;
    if (R_FAILED(clkrstOpenSession(&session, module, 3)))
      return;
    u32 cur = 0;
    clkrstGetClockRate(&session, &cur);
    if (cur > prevHz)
      clkrstSetClockRate(&session, prevHz);
    clkrstCloseSession(&session);
  };
  restore(PcvModuleId_CpuBus, g_clockBoost.prevCpu);
  restore(PcvModuleId_GPU, g_clockBoost.prevGpu);
  restore(PcvModuleId_EMC, g_clockBoost.prevEmc);
  clkrstExit();
  switchDebugLog("switchRestoreClocks: restored pre-boost clocks");
}

bool switchShowKeyboard(String const& initialText, String& outText) {
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_FAILED(rc)) {
    Logger::warn("[swkbd] swkbdCreate failed (0x{:x})", rc);
    return false;
  }
  swkbdConfigMakePresetDefault(&kbd);
  if (!initialText.empty())
    swkbdConfigSetInitialText(&kbd, initialText.utf8Ptr());
  char result[1024];
  result[0] = '\0';
  rc = swkbdShow(&kbd, result, sizeof(result));
  swkbdClose(&kbd);
  if (R_FAILED(rc))
    return false; // canceled (or applet unavailable)
  outText = String(result);
  return true;
}

void switchSyncBundledAssets(String const& storageRoot) {
  switchPlatformInit();
  if (!g_romfsMounted) {
    switchDebugLog("switchSyncBundledAssets: romfs not mounted, skipping");
    return;
  }
  String target = File::relativeTo(storageRoot, "bundled_assets");
  // The recursive copy skips files that already exist -- a cheap no-op on
  // repeat launches, but it silently leaves stale copies of files a NEW BUILD
  // changed (the engine then runs against old bundled configs/scripts).
  // Detect a build change via a stamp file and wipe + recopy when it moves;
  // the full copy only costs a few SD-card seconds once per build.
  switchDebugLog("switchSyncBundledAssets: start (romfs:/bundled_assets -> SD)");
  try {
    String stamp = String(__DATE__) + " " + __TIME__;
    String stampPath = File::relativeTo(storageRoot, "bundled_assets.stamp");
    String existing;
    try {
      if (File::isFile(stampPath))
        existing = File::readFileString(stampPath);
    } catch (std::exception const&) {}
    if (existing != stamp) {
      switchDebugLog("switchSyncBundledAssets: build changed, refreshing bundled assets");
      if (File::isDirectory(target))
        File::removeDirectoryRecursive(target);
    }
    copyDirectoryRecursive("romfs:/bundled_assets", target);
    if (existing != stamp)
      File::writeFile(stamp.utf8Ptr(), stamp.utf8Size(), stampPath);
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
void switchApplyClockBoost() {}
void switchRestoreClocks() {}
bool switchShowKeyboard(String const&, String&) { return false; }

#endif

}
