/*
 * Nintendo Switch (libnx / newlib) POSIX compatibility shim.
 *
 * libnx's newlib does not provide a handful of POSIX functions that the shared
 * Star core code relies on. They are implemented (or stubbed) here so the
 * homebrew build links. This file is only compiled for the Switch target and
 * does not affect any other platform.
 */

#ifdef STAR_SYSTEM_SWITCH

#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

/* sysconf is referenced by abseil (page size / CPU count) but absent on libnx. */
long sysconf(int name) {
  switch (name) {
#ifdef _SC_PAGESIZE
    case _SC_PAGESIZE:
#endif
#if defined(_SC_PAGE_SIZE) && (!defined(_SC_PAGESIZE) || _SC_PAGE_SIZE != _SC_PAGESIZE)
    case _SC_PAGE_SIZE:
#endif
      return 4096;
#ifdef _SC_NPROCESSORS_ONLN
    case _SC_NPROCESSORS_ONLN:
#endif
#ifdef _SC_NPROCESSORS_CONF
    case _SC_NPROCESSORS_CONF:
#endif
      return 4; /* Switch exposes up to 3-4 application cores */
    default:
      errno = EINVAL;
      return -1;
  }
}

/* No per-thread signal masking on Switch homebrew; treat as a successful no-op. */
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
  (void)how;
  (void)set;
  (void)oldset;
  return 0;
}

/* Anonymous-only mmap/munmap backed by aligned heap allocation. abseil's
 * LowLevelAlloc (used by its synchronization primitives, pulled in via re2)
 * requires mmap; libnx/newlib has none, so emulate private anonymous mappings. */
#define STAR_SWITCH_PAGE 4096
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  (void)addr;
  (void)prot;
  (void)flags;
  (void)fd;
  (void)offset;
  void* p = memalign(STAR_SWITCH_PAGE, length);
  if (!p)
    return (void*)-1; /* MAP_FAILED */
  memset(p, 0, length);
  return p;
}

int munmap(void* addr, size_t length) {
  (void)length;
  free(addr);
  return 0;
}

/* Positioned reads/writes: emulate via save/seek/restore.
 *
 * Real pread/pwrite are ATOMIC with respect to the file offset — they never
 * disturb (or depend on) the shared fd position, so multiple threads can issue
 * positioned I/O on the same descriptor concurrently. libnx has no native
 * pread, so we emulate it with lseek/read/lseek; that sequence touches the
 * shared offset, so two threads racing on the same fd would interleave their
 * seeks and read from the wrong place. Star's asset system opens each pak once
 * and reads it from many threads at once (the loader worker parses databases
 * while the main thread streams textures from the same pak), so this race is
 * real and corrupts whatever the bad-length read feeds into the allocator.
 * Serialize the whole save/seek/io/restore under one mutex to restore pread's
 * atomicity. A single global lock is coarse but positioned I/O here is not a
 * throughput bottleneck, and correctness is what matters. */
static pthread_mutex_t g_positionedIoMutex = PTHREAD_MUTEX_INITIALIZER;

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  pthread_mutex_lock(&g_positionedIoMutex);
  ssize_t r = -1;
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur != (off_t)-1 && lseek(fd, offset, SEEK_SET) != (off_t)-1)
    r = read(fd, buf, count);
  int err = errno;
  if (cur != (off_t)-1)
    lseek(fd, cur, SEEK_SET);
  pthread_mutex_unlock(&g_positionedIoMutex);
  errno = err;
  return r;
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
  pthread_mutex_lock(&g_positionedIoMutex);
  ssize_t r = -1;
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur != (off_t)-1 && lseek(fd, offset, SEEK_SET) != (off_t)-1)
    r = write(fd, buf, count);
  int err = errno;
  if (cur != (off_t)-1)
    lseek(fd, cur, SEEK_SET);
  pthread_mutex_unlock(&g_positionedIoMutex);
  errno = err;
  return r;
}

/* No multi-process file locking on Switch; advisory locks are a no-op. */
int flock(int fd, int operation) {
  (void)fd;
  (void)operation;
  return 0;
}

/* No process spawning on Switch homebrew. */
int execvp(const char* file, char* const argv[]) {
  (void)file;
  (void)argv;
  errno = ENOSYS;
  return -1;
}

pid_t waitpid(pid_t pid, int* status, int options) {
  (void)pid;
  (void)status;
  (void)options;
  errno = ECHILD;
  return -1;
}

/* Minimal libgen replacements (operate in place like the POSIX versions). */
char* basename(char* path) {
  if (!path || !*path)
    return (char*)".";
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/')
    path[--len] = '\0';
  char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

char* dirname(char* path) {
  if (!path || !*path)
    return (char*)".";
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/')
    path[--len] = '\0';
  char* slash = strrchr(path, '/');
  if (!slash)
    return (char*)".";
  if (slash == path)
    return (char*)"/";
  *slash = '\0';
  return path;
}

#endif

/* ---- Allocation attribution (diagnostic) ----------------------------------
 * Tags every wrapped malloc/calloc/realloc block with a 16-byte header naming
 * the caller (return address), keeping per-caller live/total counts.  free()
 * validates the header magic so blocks from unwrapped allocators (memalign --
 * i.e. rpmalloc spans) pass through untouched.  Report via
 * starAllocTrackReport(); addresses are relative to __wrap_malloc so they can
 * be resolved offline with addr2line regardless of NRO load base. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

extern void* __real_malloc(size_t);
extern void __real_free(void*);
extern void* __real_realloc(void*, size_t);

#define AT_SLOTS 2048
#define AT_MAGIC 0xA110C8ED5EEDF00Dull
#define AT_MARK  0x5EEDF00Du

typedef struct {
  _Atomic uintptr_t ra;
  _Atomic long live;
  _Atomic long total;
  _Atomic long liveBytes;
} AtSlot;
static AtSlot s_atTable[AT_SLOTS];

static unsigned at_slot_for(uintptr_t ra) {
  unsigned h = (unsigned)((ra >> 2) * 2654435761u) % AT_SLOTS;
  for (unsigned probe = 0; probe < 16; ++probe) {
    unsigned idx = (h + probe) % AT_SLOTS;
    if (idx == 0) continue; /* slot 0 = overflow bucket */
    uintptr_t cur = atomic_load_explicit(&s_atTable[idx].ra, memory_order_relaxed);
    if (cur == ra)
      return idx;
    if (cur == 0) {
      uintptr_t expected = 0;
      if (atomic_compare_exchange_strong(&s_atTable[idx].ra, &expected, ra))
        return idx;
      if (expected == ra)
        return idx;
    }
  }
  return 0;
}

static void* at_tag(unsigned char* p, size_t size, uintptr_t ra) {
  unsigned idx = at_slot_for(ra);
  ((uint64_t*)p)[0] = AT_MAGIC ^ ra;
  ((uint32_t*)p)[2] = idx;
  ((uint32_t*)p)[3] = AT_MARK;
  atomic_fetch_add_explicit(&s_atTable[idx].live, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&s_atTable[idx].total, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&s_atTable[idx].liveBytes, (long)size, memory_order_relaxed);
  return p + 16;
}

void* __wrap_malloc(size_t size) {
  unsigned char* p = (unsigned char*)__real_malloc(size + 16);
  if (!p) return 0;
  return at_tag(p, size, (uintptr_t)__builtin_return_address(0));
}

void* __wrap_calloc(size_t nmemb, size_t size) {
  size_t bytes = nmemb * size;
  unsigned char* p = (unsigned char*)__real_malloc(bytes + 16);
  if (!p) return 0;
  memset(p, 0, bytes + 16);
  return at_tag(p, bytes, (uintptr_t)__builtin_return_address(0));
}

void __wrap_free(void* ptr) {
  if (!ptr) return;
  unsigned char* p = (unsigned char*)ptr - 16;
  if (((uint32_t*)p)[3] == AT_MARK) {
    uint32_t idx = ((uint32_t*)p)[2];
    if (idx < AT_SLOTS) {
      uintptr_t ra = atomic_load_explicit(&s_atTable[idx].ra, memory_order_relaxed);
      if (idx == 0 || (((uint64_t*)p)[0] ^ AT_MAGIC) == ra) {
        atomic_fetch_add_explicit(&s_atTable[idx].live, -1, memory_order_relaxed);
        ((uint32_t*)p)[3] = 0;
        __real_free(p);
        return;
      }
    }
  }
  __real_free(ptr);
}

void* __wrap_realloc(void* ptr, size_t size) {
  if (!ptr) {
    unsigned char* p = (unsigned char*)__real_malloc(size + 16);
    if (!p) return 0;
    return at_tag(p, size, (uintptr_t)__builtin_return_address(0));
  }
  unsigned char* p = (unsigned char*)ptr - 16;
  if (((uint32_t*)p)[3] == AT_MARK) {
    uint32_t idx = ((uint32_t*)p)[2];
    uintptr_t ra = idx < AT_SLOTS ? atomic_load_explicit(&s_atTable[idx].ra, memory_order_relaxed) : 1;
    if (idx < AT_SLOTS && (idx == 0 || (((uint64_t*)p)[0] ^ AT_MAGIC) == ra)) {
      unsigned char* np = (unsigned char*)__real_realloc(p, size + 16);
      if (!np) return 0;
      /* keep the original slot; live count unchanged */
      return np + 16;
    }
  }
  return __real_realloc(ptr, size);
}

/* Top callers by live allocation count, addresses relative to __wrap_malloc. */
void starAllocTrackReport(char* buf, size_t bufSize) {
  int used = 0;
  buf[0] = 0;
  for (int rank = 0; rank < 8; ++rank) {
    long bestLive = 0;
    int bestIdx = -1;
    for (int i = 0; i < AT_SLOTS; ++i) {
      long lv = atomic_load_explicit(&s_atTable[i].live, memory_order_relaxed);
      int already = 0;
      /* skip ones we've printed by marking via negative search below */
      if (lv > bestLive) {
        /* check not already emitted this pass */
        already = 0;
        bestLive = lv;
        bestIdx = i;
        (void)already;
      }
    }
    if (bestIdx < 0 || bestLive < 1000) break;
    uintptr_t ra = atomic_load_explicit(&s_atTable[bestIdx].ra, memory_order_relaxed);
    long lb = atomic_load_explicit(&s_atTable[bestIdx].liveBytes, memory_order_relaxed);
    int n = snprintf(buf + used, bufSize - used, " ra=%+lld live=%ld kB=%ld;",
        (long long)((intptr_t)ra - (intptr_t)&__wrap_malloc), bestLive, lb >> 10);
    if (n <= 0 || (size_t)(used + n) >= bufSize) break;
    used += n;
    /* exclude from next passes */
    atomic_store_explicit(&s_atTable[bestIdx].live, -bestLive, memory_order_relaxed);
  }
  /* restore live counts we negated */
  for (int i = 0; i < AT_SLOTS; ++i) {
    long lv = atomic_load_explicit(&s_atTable[i].live, memory_order_relaxed);
    if (lv < 0)
      atomic_store_explicit(&s_atTable[i].live, -lv, memory_order_relaxed);
  }
}
