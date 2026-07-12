#include "StarThread.hpp"
#include "StarTime.hpp"
#include "StarLogging.hpp"

#if defined(STAR_SYSTEM_FAMILY_MOBILE) && defined(STAR_USE_RPMALLOC)
#include "rpmalloc.h"
#endif

#include <limits.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef STAR_SYSTEM_SWITCH
#include <dlfcn.h>
#endif
#include <dirent.h>
#include <pthread.h>
#ifdef STAR_SYSTEM_FREEBSD
#include <pthread_np.h>
#endif
#include <sys/time.h>
#include <errno.h>

#ifdef STAR_SYSTEM_SWITCH
#include <switch.h>
#include <atomic>
#endif

#ifdef MAXCOMLEN
#define MAX_THREAD_NAMELEN MAXCOMLEN
#else
#define MAX_THREAD_NAMELEN 16
#endif

#if !defined(STAR_SYSTEM_MACOS) && !defined(STAR_SYSTEM_IOS)
#define STAR_RECURSIVE_MUTEX_TIMED
#endif

namespace Star {

#ifdef STAR_SYSTEM_SWITCH
// On the Switch, libnx newlib pthreads are all created on the process default
// core; they do NOT spread across CPUs on their own. Left alone, the main game
// loop, the per-world server simulation and the async lighting calculation all
// serialize onto ONE of the 4 Tegra cores while the rest sit idle -- the
// dominant cause of the 2-5 FPS seen in-game.
//
// We deliberately distribute ONLY the small set of heavy, long-lived threads
// that Starbound is architecturally designed to run concurrently with the
// client (the same threads desktop already runs on separate cores): the
// per-world server thread, the async world-lighting thread, and the universe
// server. These are created only once a world is entered, so the asset-load
// phase and the Splash->Title transition behave EXACTLY as before -- important
// because forcing true parallelism across every thread (loaders, workers, etc.)
// surfaces latent data races in the Switch's hand-written libc/syscall shims
// (the lseek-based pread, the malloc-backed mmap, etc.) that the effectively
// single-core execution had been masking, and that crashed the PlayerStorage
// constructor during startup.
//
// We only ever pick cores reported as allowed by the kernel (svcGetInfo
// InfoType_CoreMask), so this stays correct on retail HW where core 3 is
// reserved. We set a preferred core but pass the full allowed mask, letting
// Horizon migrate threads for load-balancing rather than hard-pinning.
static bool switchThreadWantsOwnCore(String const& name) {
  return name.beginsWith("WorldServerThread")
      || name == "WorldClient::lightingMain"
      || name == "UniverseServer";
}

static void switchDistributeCurrentThread(String const& name) {
  bool isSimTick = name == "ClientApplication::simTick";
  if (!isSimTick && !switchThreadWantsOwnCore(name))
    return;

  static std::atomic<unsigned> threadCounter{0};

  u64 coreMask = 0;
  if (R_FAILED(svcGetInfo(&coreMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return;

  int allowedCores[4];
  int allowedCount = 0;
  for (int core = 0; core < 4; ++core) {
    if (coreMask & (1ull << core))
      allowedCores[allowedCount++] = core;
  }

  static std::atomic<bool> maskLogged{false};
  if (!maskLogged.exchange(true))
    Logger::info("Switch thread placement: allowed core mask {:#x} ({} cores)", coreMask, allowedCount);

  // Nothing to distribute across (single permitted core); leave it alone.
  if (allowedCount <= 1)
    return;

  // Reserve allowedCores[0] for the main thread (game loop = GL render +
  // interface); it is explicitly pinned there in switchPlatformInit.
  //
  // The client sim worker runs concurrently with the main thread's paint
  // every frame and is frame-critical, so it gets allowedCores[1] to itself;
  // the remaining heavy gameplay threads (lighting, universe/world server)
  // spread round-robin over whatever is left so they don't time-slice with
  // the sim tick.
  int preferred;
  if (isSimTick) {
    preferred = allowedCores[1];
  } else {
    int spreadBase = allowedCount > 2 ? 2 : 1;
    int spreadCount = allowedCount - spreadBase;
    preferred = allowedCores[spreadBase + (threadCounter.fetch_add(1) % spreadCount)];
  }
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferred, coreMask);
}
#endif

struct ThreadImpl {
  static void* runThread(void* data) {
    ThreadImpl* ptr = static_cast<ThreadImpl*>(data);
#ifdef STAR_SYSTEM_SWITCH
    switchDistributeCurrentThread(ptr->name);
#endif
    try {
#if defined(STAR_SYSTEM_MACOS) || defined(STAR_SYSTEM_IOS)
      // ensure the name is under the max allowed
      char tname[MAX_THREAD_NAMELEN];
      snprintf(tname, sizeof(tname), "%s", ptr->name.utf8Ptr());

      pthread_setname_np(tname);
#endif
      ptr->function();
    } catch (std::exception const& e) {
      if (ptr->name.empty())
        Logger::error("Exception caught in Thread: {}", outputException(e, true));
      else
        Logger::error("Exception caught in Thread {}: {}", ptr->name, outputException(e, true));
    } catch (...) {
      if (ptr->name.empty())
        Logger::error("Unknown exception caught in Thread");
      else
        Logger::error("Unknown exception caught in Thread {}", ptr->name);
    }
    ptr->stopped = true;
#if defined(STAR_SYSTEM_FAMILY_MOBILE) && defined(STAR_USE_RPMALLOC)
    // Without the libc-override preload hooks (not installed on Switch),
    // nothing releases this thread's rpmalloc heap on exit; do it here so
    // short-lived threads (per-world server threads etc.) don't leak their
    // span caches.
    rpmalloc_thread_finalize(1);
#endif
    return nullptr;
  }

  ThreadImpl(std::function<void()> function, String name)
    : function(std::move(function)), name(std::move(name)), stopped(true), joined(true) {}

  bool start() {
    MutexLocker mutexLocker(mutex);
    if (!joined)
      return false;

    stopped = false;
    joined = false;
#if defined(STAR_SYSTEM_IOS)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    int ret = pthread_create(&pthread, &attr, &runThread, (void*)this);
    pthread_attr_destroy(&attr);
#elif defined(STAR_SYSTEM_SWITCH)
    // libnx's default newlib pthread stack (~128KB) is far too small for Star's
    // deeply recursive JSON / world / asset loading. Loading the ship world
    // overflows it and faults past the stack guard page (observed as a libnx
    // fsdev readdir tipping an already-exhausted stack over). Use a larger stack.
    // NOT the 8MB iOS uses: libnx maps every pthread stack as a svcMapMemory
    // "stack mirror", so 8MB x (many worker/universe/network threads) would
    // strain the homebrew stack-mirror address space. 4MB is ample for the
    // recursion while staying conservative; fall back to the default stack if
    // the larger one cannot be allocated rather than failing hard.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    int ret = pthread_create(&pthread, &attr, &runThread, (void*)this);
    pthread_attr_destroy(&attr);
    if (ret != 0)
      ret = pthread_create(&pthread, NULL, &runThread, (void*)this);
#else
    int ret = pthread_create(&pthread, NULL, &runThread, (void*)this);
#endif
    if (ret != 0) {
      stopped = true;
      joined = true;
      throw StarException(strf("Failed to create thread, error {}", ret));
    }

    // ensure the name is under the max allowed
    char tname[MAX_THREAD_NAMELEN];
    snprintf(tname, sizeof(tname), "%s", name.utf8Ptr());

#ifdef STAR_SYSTEM_FREEBSD
    pthread_set_name_np(pthread, tname);
#elif defined(STAR_SYSTEM_NETBSD)
    pthread_setname_np(pthread, "%s", tname);
#elif defined(STAR_SYSTEM_IOS)
    // iOS only supports naming the current thread; this is handled in runThread.
#elif !defined(STAR_SYSTEM_MACOS) && !defined(STAR_SYSTEM_SWITCH)
    pthread_setname_np(pthread, tname);
#endif
    return true;
  }

  bool join() {
    MutexLocker mutexLocker(mutex);
    if (joined)
      return false;
    int ret = pthread_join(pthread, NULL);
    if (ret != 0)
      throw StarException(strf("Failed to join thread, error {}", ret));
    joined = true;
    return true;
  }

  std::function<void()> function;
  String name;
  pthread_t pthread;
  atomic<bool> stopped;
  bool joined;
  Mutex mutex;
};

struct ThreadFunctionImpl : ThreadImpl {
  ThreadFunctionImpl(std::function<void()> function, String name)
    : ThreadImpl(wrapFunction(std::move(function)), std::move(name)) {}

  std::function<void()> wrapFunction(std::function<void()> function) {
    return [function = std::move(function), this]() {
      try {
        function();
      } catch (...) {
        exception = std::current_exception();
      }
    };
  }

  std::exception_ptr exception;
};

struct MutexImpl {
  MutexImpl() {
    pthread_mutexattr_t mutexattr;
    pthread_mutexattr_init(&mutexattr);

    pthread_mutex_init(&mutex, &mutexattr);

    pthread_mutexattr_destroy(&mutexattr);
  }

  ~MutexImpl() {
    pthread_mutex_destroy(&mutex);
  }

  void lock() {
#if defined(STAR_MUTEX_TIMED) && !defined(STAR_SYSTEM_IOS) && !defined(STAR_SYSTEM_SWITCH)
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 15;
    if (pthread_mutex_timedlock(&mutex, &ts) != 0) {
      printStack("Mutex::lock is TAKING TOO LONG 🎃");
#else
    {
#endif
      pthread_mutex_lock(&mutex);
    }
  }

  void unlock() {
    pthread_mutex_unlock(&mutex);
  }

  bool tryLock() {
    if (pthread_mutex_trylock(&mutex) == 0)
      return true;
    else
      return false;
  }

  pthread_mutex_t mutex;
};

struct ConditionVariableImpl {
  ConditionVariableImpl() {
    pthread_cond_init(&condition, NULL);
  }

  ~ConditionVariableImpl() {
    pthread_cond_destroy(&condition);
  }

  void wait(Mutex& mutex) {
    pthread_cond_wait(&condition, &mutex.m_impl->mutex);
  }

  void wait(Mutex& mutex, unsigned millis) {
    int64_t time = Time::millisecondsSinceEpoch() + millis;

    timespec ts;
    ts.tv_sec = time / 1000;
    ts.tv_nsec = (time % 1000) * 1000000;

    pthread_cond_timedwait(&condition, &mutex.m_impl->mutex, &ts);
  }

  void signal() {
    pthread_cond_signal(&condition);
  }

  void broadcast() {
    pthread_cond_broadcast(&condition);
  }

  pthread_cond_t condition;
};

struct RecursiveMutexImpl {
  RecursiveMutexImpl() {
    pthread_mutexattr_t mutexattr;
    pthread_mutexattr_init(&mutexattr);

    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);

    pthread_mutex_init(&mutex, &mutexattr);

    pthread_mutexattr_destroy(&mutexattr);
  }

  ~RecursiveMutexImpl() {
    pthread_mutex_destroy(&mutex);
  }

  void lock() {
#if defined(STAR_RECURSIVE_MUTEX_TIMED) && !defined(STAR_SYSTEM_IOS) && !defined(STAR_SYSTEM_SWITCH)
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 15;
    if (pthread_mutex_timedlock(&mutex, &ts) != 0) {
      printStack("RecursiveMutex::lock is TAKING TOO LONG 🎃");
#else
    {
#endif
      pthread_mutex_lock(&mutex);
    }
  }

  void unlock() {
    pthread_mutex_unlock(&mutex);
  }

  bool tryLock() {
    if (pthread_mutex_trylock(&mutex) == 0)
      return true;
    else
      return false;
  }

  pthread_mutex_t mutex;
};

void Thread::sleepPrecise(unsigned msecs) {
  int64_t now = Time::monotonicMilliseconds();
  int64_t deadline = now + msecs;

  while (deadline - now > 10) {
    usleep((deadline - now - 10) * 1000);
    now = Time::monotonicMilliseconds();
  }

  while (deadline > now) {
    usleep((deadline - now) * 500);
    now = Time::monotonicMilliseconds();
  }
}

void Thread::sleep(unsigned msecs) {
  usleep(msecs * 1000);
}

void Thread::yield() {
  sched_yield();
}

unsigned Thread::numberOfProcessors() {
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  if (nprocs < 1)
    throw StarException(strf("Could not determine number of CPUs online: {}\n", strerror(errno)));
  return nprocs;
}

Thread::Thread(String const& name) {
  m_impl.reset(new ThreadImpl([this]() {
      run();
    }, name));
}

Thread::Thread(Thread&&) = default;

Thread::~Thread() {}

Thread& Thread::operator=(Thread&&) = default;

bool Thread::start() {
  return m_impl->start();
}

bool Thread::join() {
  return m_impl->join();
}

String Thread::name() {
  return m_impl->name;
}

bool Thread::isJoined() const {
  return m_impl->joined;
}

bool Thread::isRunning() const {
  return !m_impl->stopped;
}

ThreadFunction<void>::ThreadFunction() {}

ThreadFunction<void>::ThreadFunction(ThreadFunction&&) = default;

ThreadFunction<void>::ThreadFunction(function<void()> function, String const& name) {
  m_impl.reset(new ThreadFunctionImpl(std::move(function), name));
  m_impl->start();
}

ThreadFunction<void>::~ThreadFunction() {
  finish();
}

ThreadFunction<void>& ThreadFunction<void>::operator=(ThreadFunction&&) = default;

void ThreadFunction<void>::finish() {
  if (m_impl) {
    m_impl->join();

    if (m_impl->exception)
      std::rethrow_exception(take(m_impl->exception));
  }
}

bool ThreadFunction<void>::isFinished() const {
  return !m_impl || m_impl->joined;
}

bool ThreadFunction<void>::isRunning() const {
  return m_impl && !m_impl->stopped;
}

ThreadFunction<void>::operator bool() const {
  return !isFinished();
}

String ThreadFunction<void>::name() {
  if (m_impl)
    return m_impl->name;
  else
    return "";
}

Mutex::Mutex()
  : m_impl(new MutexImpl()) {}

Mutex::Mutex(Mutex&&) = default;

Mutex::~Mutex() {}

Mutex& Mutex::operator=(Mutex&&) = default;

void Mutex::lock() {
  m_impl->lock();
}

bool Mutex::tryLock() {
  return m_impl->tryLock();
}

void Mutex::unlock() {
  m_impl->unlock();
}

ConditionVariable::ConditionVariable()
  : m_impl(new ConditionVariableImpl()) {}

ConditionVariable::ConditionVariable(ConditionVariable&&) = default;

ConditionVariable::~ConditionVariable() {}

ConditionVariable& ConditionVariable::operator=(ConditionVariable&&) = default;

void ConditionVariable::wait(Mutex& mutex, Maybe<unsigned> millis) {
  if (millis)
    m_impl->wait(mutex, *millis);
  else
    m_impl->wait(mutex);
}

void ConditionVariable::signal() {
  m_impl->signal();
}

void ConditionVariable::broadcast() {
  m_impl->broadcast();
}

RecursiveMutex::RecursiveMutex()
  : m_impl(new RecursiveMutexImpl()) {}

RecursiveMutex::RecursiveMutex(RecursiveMutex&&) = default;

RecursiveMutex::~RecursiveMutex() {}

RecursiveMutex& RecursiveMutex::operator=(RecursiveMutex&&) = default;

void RecursiveMutex::lock() {
  m_impl->lock();
}

bool RecursiveMutex::tryLock() {
  return m_impl->tryLock();
}

void RecursiveMutex::unlock() {
  m_impl->unlock();
}

}
