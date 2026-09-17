#include "StarMemoryUsage.hpp"
#include "StarFormat.hpp"
#ifdef STAR_USE_RPMALLOC
#include "rpmalloc.h"
#endif
#include "StarThread.hpp"
#include "StarTime.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

// Everything that is not one of the four special cases below reads /proc.
#if defined(STAR_SYSTEM_SWITCH)
#include <malloc.h>
#include <switch.h>
#elif defined(STAR_SYSTEM_MACOS) || defined(STAR_SYSTEM_IOS)
#include <mach/mach.h>
#if defined(STAR_SYSTEM_IOS)
#include <os/proc.h>
#endif
#elif defined(STAR_SYSTEM_WINDOWS)
#include <windows.h>
#include <psapi.h>
#else
#define STAR_MEMORY_USAGE_PROCFS
#include <unistd.h>
#endif

namespace Star {

namespace {
  std::atomic<uint64_t> s_categoryBytes[(size_t)MemoryCategory::Count];
  std::atomic<uint64_t> s_reportedProcessBytes{0};

#ifdef STAR_MEMORY_USAGE_PROCFS
  // Sum of the "kB" values of the named /proc/meminfo keys. Returns 0 if the
  // file is unreadable, which callers treat as "no budget known".
  uint64_t procMeminfoBytes(char const* key) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f)
      return 0;

    uint64_t result = 0;
    char line[256];
    size_t keyLen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, key, keyLen) == 0 && line[keyLen] == ':') {
        unsigned long long kb = 0;
        if (sscanf(line + keyLen + 1, "%llu", &kb) == 1)
          result = (uint64_t)kb * 1024;
        break;
      }
    }
    fclose(f);
    return result;
  }

  uint64_t procResidentBytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f)
      return 0;

    unsigned long long total = 0, resident = 0;
    int read = fscanf(f, "%llu %llu", &total, &resident);
    fclose(f);
    if (read != 2)
      return 0;
    return (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE);
  }
#endif
}

bool MemoryUsage::valid() const {
  return used != 0;
}

float MemoryUsage::fraction() const {
  if (budget == 0)
    return 0.0f;
  return (float)((double)used / (double)budget);
}

MemoryUsage memoryUsage() {
  MemoryUsage usage;

#if defined(STAR_SYSTEM_SWITCH)
  // The homebrew process gets a fixed application pool (~3.2GB in
  // title-takeover mode) and every byte -- engine allocations via rpmalloc AND
  // the mesa/nouveau GL driver -- is carved out of the one newlib heap, so the
  // kernel's per-process figures are the right ceiling to measure against.
  //
  // Live bytes come from whoever last called memoryUsageReportProcessBytes --
  // see the header for why neither mallinfo() nor svcGetInfo(UsedMemorySize)
  // can be called here directly. Until that report lands, rpmalloc's mapped
  // total is a usable lower bound; leaving used at 0 makes every pressure
  // check look like "no pressure".
  usage.used = s_reportedProcessBytes.load(std::memory_order_relaxed);
  if (usage.used == 0)
    usage.used = memoryAllocatorMappedBytes();

  uint64_t total = 0;
  if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)))
    usage.budget = total;
#elif defined(STAR_SYSTEM_MACOS) || defined(STAR_SYSTEM_IOS)
  task_vm_info_data_t info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
    usage.used = (uint64_t)info.phys_footprint;
#if defined(STAR_SYSTEM_IOS)
  // iOS gives no total, only "how much more this app may take before jetsam".
  if (size_t available = os_proc_available_memory())
    usage.budget = usage.used + (uint64_t)available;
#endif
#elif defined(STAR_SYSTEM_WINDOWS)
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters)))
    usage.used = (uint64_t)counters.PrivateUsage;

  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status))
    usage.budget = usage.used + (uint64_t)status.ullAvailPhys;
#else
  usage.used = procResidentBytes();
  // Headroom, not installed RAM: MemAvailable is what this process can still
  // get without pushing the system into swap/OOM-kill, which is the number a
  // player watching the indicator actually cares about.
  if (uint64_t available = procMeminfoBytes("MemAvailable"))
    usage.budget = usage.used + available;
  else if (uint64_t total = procMeminfoBytes("MemTotal"))
    usage.budget = total;
#endif

  return usage;
}

MemoryUsage memoryUsageCached() {
  static Mutex s_mutex;
  static MemoryUsage s_cached;
  static int64_t s_cachedAt = 0;

  MutexLocker locker(s_mutex);
  int64_t now = Time::monotonicMilliseconds();
  if (s_cachedAt == 0 || now - s_cachedAt >= 500) {
    s_cached = memoryUsage();
    s_cachedAt = now;
  }
  return s_cached;
}

void memoryAccountAdd(MemoryCategory category, int64_t bytes) {
  s_categoryBytes[(size_t)category].fetch_add((uint64_t)bytes, std::memory_order_relaxed);
}

void memoryUsageReportProcessBytes(uint64_t liveBytes) {
  s_reportedProcessBytes.store(liveBytes, std::memory_order_relaxed);
}

void memoryAccountSet(MemoryCategory category, uint64_t bytes) {
  s_categoryBytes[(size_t)category].store(bytes, std::memory_order_relaxed);
}

uint64_t memoryAccountBytes(MemoryCategory category) {
  return s_categoryBytes[(size_t)category].load(std::memory_order_relaxed);
}

uint64_t memoryAllocatorMappedBytes() {
#if defined(STAR_USE_RPMALLOC) && defined(STAR_SYSTEM_SWITCH)
  return (uint64_t)rpmalloc_mapped_bytes();
#else
  return 0;
#endif
}

uint64_t memoryReleaseAllocatorCaches() {
#ifdef STAR_USE_RPMALLOC
  return (uint64_t)rpmalloc_release_caches();
#else
  return 0;
#endif
}

uint64_t memoryReleaseThreadCaches() {
#ifdef STAR_USE_RPMALLOC
  uint64_t released = (uint64_t)rpmalloc_release_thread_caches();
#ifdef STAR_SYSTEM_SWITCH
  // Also return any unrelated free newlib top chunk. rpmalloc mappings stay
  // in its coalescing pool so later worlds reuse them without fragmentation.
  malloc_trim(0);
#endif
  return released;
#else
  return 0;
#endif
}

String memoryUsageSummary() {
  auto usage = memoryUsage();
  auto mb = [](uint64_t bytes) { return bytes / (1024 * 1024); };

  String base;
  if (!usage.valid())
    base = "mem unavailable";
  else if (usage.budget)
    base = strf("{}/{}MB {:.0f}%", mb(usage.used), mb(usage.budget), usage.fraction() * 100.0f);
  else
    base = strf("{}MB", mb(usage.used));

  // rpmapped is what the allocator holds; used-minus-rpmapped is everything
  // allocated straight against libc, which on Switch is the GL driver and libnx.
  return strf("{} (assets {}MB, textures {}MB, rpmapped {}MB)", base,
      mb(memoryAccountBytes(MemoryCategory::AssetCache)),
      mb(memoryAccountBytes(MemoryCategory::TextureAtlas)),
      mb(memoryAllocatorMappedBytes()));
}

}
