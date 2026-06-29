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
