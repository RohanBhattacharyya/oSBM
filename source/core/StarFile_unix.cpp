#include "StarFile.hpp"
#include "StarFormat.hpp"
#include "StarRandom.hpp"
#include "StarEncode.hpp"

#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef STAR_SYSTEM_MACOSX
#include <mach-o/dyld.h>
#elif defined STAR_SYSTEM_FREEBSD
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#ifdef STAR_SYSTEM_SWITCH
#include <switch.h>
#include <cstdio>
#include <atomic>
#include <mutex>

// Serialize ALL file I/O on Switch. libnx's fsdev/newlib fd layer is not
// reliably safe under concurrent open/close on one thread vs pread/lseek on
// another: the post-warp ship-world shutdown save (big .shipworld preads)
// racing the system-world storage writes (open/write/close/rename) faults
// inside newlib's fd handling (_lseek_r -> mutexUnlock on a junk pointer)
// and kills the process. SD-card I/O is serial at the device level anyway,
// so a process-wide lock costs little beyond the (rare) genuine overlap.
static std::recursive_mutex& switchFileLock() {
  static std::recursive_mutex m;
  return m;
}
#define SWITCH_FILE_SERIALIZE std::lock_guard<std::recursive_mutex> _switchFileGuard(switchFileLock())
#else
#define SWITCH_FILE_SERIALIZE
#endif

namespace Star {

namespace {
  int fdFromHandle(void* ptr) {
    return (int)(intptr_t)ptr;
  }

  void* handleFromFd(int handle) {
    return (void*)(intptr_t)handle;
  }

#ifdef STAR_SYSTEM_SWITCH
  // fd-lifecycle tracing for save-file handles (player/universe/temp files):
  // the post-warp shutdown crash is a pread on a shipworld fd faulting inside
  // libnx's fd table. Track which fds belong to save files so a double-close,
  // close-then-use, or fd reuse is directly visible in the debug log.
  std::atomic<uint32_t> s_trackedFds[32]; // bitset for fds 0..1023
  bool switchTrackedPath(char const* filename) {
    return strstr(filename, "/player/") || strstr(filename, "/universe/") || strstr(filename, "tmpfile");
  }
  void switchMarkFd(int fd, char const* filename) {
    if (fd < 0 || fd >= 1024) return;
    s_trackedFds[fd / 32].fetch_or(1u << (fd % 32));
    char b[256];
    int l = snprintf(b, sizeof(b), "FDTRACE open fd=%d '%s'", fd, filename);
    if (l > 0) svcOutputDebugString(b, (size_t)l);
  }
  bool switchFdTracked(int fd) {
    if (fd < 0 || fd >= 1024) return false;
    return (s_trackedFds[fd / 32].load() >> (fd % 32)) & 1u;
  }
  // fds of save files that have been closed and not since reopened: a
  // pread/pwrite on one of these is a use-after-close (or unlucky fd reuse).
  std::atomic<uint32_t> s_closedTrackedFds[32];
  void switchUnmarkFd(int fd) {
    if (fd < 0 || fd >= 1024) return;
    if (switchFdTracked(fd)) {
      s_trackedFds[fd / 32].fetch_and(~(1u << (fd % 32)));
      s_closedTrackedFds[fd / 32].fetch_or(1u << (fd % 32));
      char b[64];
      int l = snprintf(b, sizeof(b), "FDTRACE close fd=%d", fd);
      if (l > 0) svcOutputDebugString(b, (size_t)l);
    }
  }
  void switchFdReopened(int fd) {
    if (fd < 0 || fd >= 1024) return;
    s_closedTrackedFds[fd / 32].fetch_and(~(1u << (fd % 32)));
  }
  void switchCheckUseAfterClose(int fd, char const* op) {
    if (fd < 0 || fd >= 1024) return;
    if ((s_closedTrackedFds[fd / 32].load() >> (fd % 32)) & 1u) {
      char b[96];
      int l = snprintf(b, sizeof(b), "FDTRACE USE-AFTER-CLOSE %s fd=%d", op, fd);
      if (l > 0) svcOutputDebugString(b, (size_t)l);
    }
  }
#endif

#ifdef STAR_SYSTEM_SWITCH
  // libnx's fsdev silently aborts the whole process when stat()/open() is given
  // a path whose final component does not exist on the SD card (instead of
  // failing cleanly with ENOENT). readdir DOES behave, so test a path's presence
  // by listing its parent directory. This is the reliable primitive every
  // missing-file check on Switch must go through; stat is only safe once a path
  // is known to exist.
  bool switchPathExists(String const& path) {
    // Only the SD-card default device ("/...") has the missing-path abort bug.
    // Devoptab-prefixed paths -- e.g. "romfs:/packed.pak" -- use a separate
    // read-only device whose stat() behaves correctly; a device prefix is a
    // "name:" that appears before the first '/'. Those must use stat (opendir on
    // a bare "romfs:" parent would fail and wrongly report the file missing).
    std::string s = path.utf8();
    size_t colon = s.find(':');
    size_t slash = s.find('/');
    bool devoptabPrefixed = colon != std::string::npos && (slash == std::string::npos || colon < slash);
    if (devoptabPrefixed) {
      struct stat st;
      return ::stat(path.utf8Ptr(), &st) == 0;
    }

    String parent = File::dirName(path);
    String name = File::baseName(path);
    DIR* directory = ::opendir(parent.utf8Ptr());
    if (directory == NULL)
      return false;
    bool found = false;
    for (dirent* entry = ::readdir(directory); entry != NULL; entry = ::readdir(directory)) {
      if (name == entry->d_name) {
        found = true;
        break;
      }
    }
    ::closedir(directory);
    return found;
  }
#endif

  String temporaryRootDirectory() {
    if (auto tmpDir = getenv("TMPDIR")) {
      if (tmpDir[0] != '\0')
        return String(tmpDir);
    }

#ifdef STAR_SYSTEM_ANDROID
    if (auto homeDir = getenv("HOME")) {
      if (homeDir[0] != '\0')
        return String(homeDir);
    }
    return ".";
#else
    return String(P_tmpdir);
#endif
  }

  String ensuredTemporaryRootDirectory() {
    auto directory = temporaryRootDirectory();
    if (directory.empty())
      directory = ".";

    if (!File::isDirectory(directory)) {
      try {
        File::makeDirectoryRecursive(directory);
      } catch (...) {
        directory = ".";
      }
    }

    return directory;
  }
}

String File::convertDirSeparators(String const& path) {
  return path.replace("\\", "/");
}

String File::currentDirectory() {
  char buffer[PATH_MAX];
  if (::getcwd(buffer, PATH_MAX) == NULL)
    throw IOException("getcwd failed");

  return String(buffer);
}

void File::changeDirectory(const String& dirName) {
  if (::chdir(dirName.utf8Ptr()) != 0)
    throw IOException(strf("could not change directory to {}", dirName));
}

void File::makeDirectory(String const& dirName) {
  SWITCH_FILE_SERIALIZE;
  if (::mkdir(dirName.utf8Ptr(), 0777) != 0)
    throw IOException(strf("could not create directory '{}', {}", dirName, strerror(errno)));
}

List<pair<String, bool>> File::dirList(const String& dirName, bool skipDots) {
  List<std::pair<String, bool>> fileList;
  DIR* directory = ::opendir(dirName.utf8Ptr());
  if (directory == NULL)
    throw IOException::format("dirList failed on dir: '{}'", dirName);

  for (dirent* entry = ::readdir(directory); entry != NULL; entry = ::readdir(directory)) {
    String entryString = entry->d_name;
    if (!skipDots || (entryString != "." && entryString != "..")) {
      bool isDirectory = false;
      if (entry->d_type == DT_DIR) {
        isDirectory = true;
      } else if (entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
        isDirectory = File::isDirectory(File::relativeTo(dirName, entryString));
      }
      fileList.append({entryString, isDirectory});
    }
  }
  ::closedir(directory);

  return fileList;
}

String File::baseName(const String& fileName) {
  String ret;

  std::string file = fileName.utf8();
  char* fn = new char[file.size() + 1];
  std::copy(file.begin(), file.end(), fn);
  fn[file.size()] = 0;
  ret = String(::basename(fn));
  delete[] fn;

  return ret;
}

String File::dirName(const String& fileName) {
  String ret;

  std::string file = fileName.utf8();
  char* fn = new char[file.size() + 1];
  std::copy(file.begin(), file.end(), fn);
  fn[file.size()] = 0;
  ret = String(::dirname(fn));
  delete[] fn;

  return ret;
}

String File::relativeTo(String const& relativeTo, String const& path) {
  if (path.beginsWith("/"))
    return path;
  return relativeTo.trimEnd("/") + '/' + path;
}

String File::fullPath(const String& fileName) {
  char buffer[PATH_MAX];

  if (::realpath(fileName.utf8Ptr(), buffer) == NULL)
    throw IOException::format("realpath failed on file: '{}' problem path was: '{}'", fileName, buffer);

  return String(buffer);
}

String File::temporaryFileName() {
  return relativeTo(ensuredTemporaryRootDirectory(), strf("starbound.tmpfile.{}", hexEncode(Random::randBytes(16))));
}

FilePtr File::temporaryFile() {
  return open(temporaryFileName(), IOMode::ReadWrite);
}

FilePtr File::ephemeralFile() {
  auto file = make_shared<File>();
  ByteArray path = ByteArray::fromCStringWithNull(relativeTo(ensuredTemporaryRootDirectory(), "starbound.tmpfile.XXXXXXXX").utf8Ptr());
  SWITCH_FILE_SERIALIZE;
  auto res = mkstemp(path.ptr());
  if (res < 0)
    throw IOException::format("tmpfile error: {}", strerror(errno));
#ifdef STAR_SYSTEM_SWITCH
  // libnx fsdev (FAT/exFAT on the SD card) cannot unlink a file that still has
  // an open handle -- the POSIX delete-while-open trick below fails and the
  // resulting IOException is fatal on Switch (exceptions do not unwind). This is
  // what was crashing ship-world load (WorldStorage backs its DB on an ephemeral
  // file). Leave the temp file on disk; switchPlatformInit() clears the temp
  // directory at startup so these do not accumulate across launches.
#else
  if (::unlink(path.ptr()) < 0)
    throw IOException::format("Could not remove mkstemp file when creating ephemeralFile: {}", strerror(errno));
#endif
#ifdef STAR_SYSTEM_SWITCH
  switchMarkFd(res, path.ptr());
#endif
  file->m_file = handleFromFd(res);
  file->setMode(IOMode::ReadWrite);
  return file;
}

String File::temporaryDirectory() {
  String dirname = relativeTo(ensuredTemporaryRootDirectory(), strf("starbound.tmpdir.{}", hexEncode(Random::randBytes(16))));
  makeDirectory(dirname);
  return dirname;
}

bool File::exists(String const& path) {
#ifdef STAR_SYSTEM_SWITCH
  // stat() aborts the process under libnx fsdev for a missing path; gate on a
  // readdir-based existence test (see switchPathExists).
  return switchPathExists(path);
#else
  struct stat st_buf;
  int status = stat(path.utf8Ptr(), &st_buf);
  return status == 0;
#endif
}

bool File::isFile(String const& path) {
#ifdef STAR_SYSTEM_SWITCH
  if (!switchPathExists(path))
    return false;
#endif
  struct stat st_buf;
  int status = stat(path.utf8Ptr(), &st_buf);
  if (status != 0)
    return false;

  return S_ISREG(st_buf.st_mode);
}

bool File::isDirectory(String const& path) {
#ifdef STAR_SYSTEM_SWITCH
  if (!switchPathExists(path))
    return false;
#endif
  struct stat st_buf;
  int status = stat(path.utf8Ptr(), &st_buf);
  if (status != 0)
    return false;

  return S_ISDIR(st_buf.st_mode);
}

void File::remove(String const& filename) {
  SWITCH_FILE_SERIALIZE;
  if (::remove(filename.utf8Ptr()) < 0)
    throw IOException::format("remove error: {}", strerror(errno));
}

void File::rename(String const& source, String const& target) {
  SWITCH_FILE_SERIALIZE;
#ifdef STAR_SYSTEM_SWITCH
  // libnx's fsdev rename() cannot overwrite an existing target (POSIX rename
  // replaces atomically; fsdev returns an error / misbehaves). This breaks the
  // very common overwriteFileWithRename pattern (config saves, launcher state)
  // on every run after the first. Remove the target first; on Switch homebrew
  // there is a single process so the non-atomicity is harmless.
  struct stat st;
  if (::stat(target.utf8Ptr(), &st) == 0)
    ::remove(target.utf8Ptr());
#endif
  if (::rename(source.utf8Ptr(), target.utf8Ptr()) < 0)
    throw IOException::format("rename error: {}", strerror(errno));
}

void File::overwriteFileWithRename(char const* data, size_t len, String const& filename, String const& newSuffix) {
  String newFile = filename + newSuffix;
  writeFile(data, len, newFile);
  File::rename(newFile, filename);
}

void* File::fopen(char const* filename, IOMode mode) {
  SWITCH_FILE_SERIALIZE;
  int oflag = 0;

  if (mode & IOMode::Read && mode & IOMode::Write)
    oflag |= O_RDWR | O_CREAT;
  else if (mode & IOMode::Read)
    oflag |= O_RDONLY;
  else if (mode & IOMode::Write)
    oflag |= O_WRONLY | O_CREAT;

  if (mode & IOMode::Truncate)
    oflag |= O_TRUNC;

#ifdef STAR_SYSTEM_SWITCH
  // A non-creating ::open of a missing file aborts the process under libnx fsdev
  // instead of failing with ENOENT (no C++ exception is thrown, so callers that
  // expect to catch the failure get a silent process exit). Detect the missing
  // file via readdir first and throw the normal IOException ourselves so libnx
  // is never handed a missing path.
  if (!(oflag & O_CREAT) && !switchPathExists(String(filename))) {
    errno = ENOENT;
    throw IOException::format("Error opening file '{}', error: {}", filename, strerror(errno));
  }
#endif

  int fd = ::open(filename, oflag, 0666);
  if (fd < 0)
    throw IOException::format("Error opening file '{}', error: {}", filename, strerror(errno));

  if (mode & IOMode::Append) {
    if (lseek(fd, 0, SEEK_END) < 0)
      throw IOException::format("Error opening file '{}', cannot seek: {}", filename, strerror(errno));
  }

#ifdef STAR_SYSTEM_SWITCH
  switchFdReopened(fd);
  if (switchTrackedPath(filename))
    switchMarkFd(fd, filename);
#endif

  return handleFromFd(fd);
}

void File::fseek(void* f, StreamOffset offset, IOSeek seekMode) {
  SWITCH_FILE_SERIALIZE;
  auto fd = fdFromHandle(f);
  StreamOffset retCode;
  if (seekMode == IOSeek::Relative)
    retCode = lseek(fd, offset, SEEK_CUR);
  else if (seekMode == IOSeek::Absolute)
    retCode = lseek(fd, offset, SEEK_SET);
  else
    retCode = lseek(fd, offset, SEEK_END);

  if (retCode < 0)
    throw IOException::format("Seek error: {}", strerror(errno));
}

StreamOffset File::ftell(void* f) {
  SWITCH_FILE_SERIALIZE;
  return lseek(fdFromHandle(f), 0, SEEK_CUR);
}

size_t File::fread(void* file, char* data, size_t len) {
  SWITCH_FILE_SERIALIZE;
  if (len == 0)
    return 0;

  auto fd = fdFromHandle(file);
  auto ret = ::read(fd, data, len);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EINTR)
      return 0;
    throw IOException::format("Read error: {}", strerror(errno));
  } else {
    return ret;
  }
}

size_t File::fwrite(void* file, char const* data, size_t len) {
  SWITCH_FILE_SERIALIZE;
  if (len == 0)
    return 0;

  auto fd = fdFromHandle(file);
  auto ret = ::write(fd, data, len);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EINTR)
      return 0;
    throw IOException::format("Write error: {}", strerror(errno));
  } else {
    return ret;
  }
}

void File::fsync(void* file) {
  SWITCH_FILE_SERIALIZE;
  auto fd = fdFromHandle(file);
#ifdef STAR_SYSTEM_LINUX
  ::fdatasync(fd);
#else
  ::fsync(fd);
#endif
}

void File::fclose(void* file) {
  SWITCH_FILE_SERIALIZE;
#ifdef STAR_SYSTEM_SWITCH
  switchUnmarkFd(fdFromHandle(file));
#endif
  if (::close(fdFromHandle(file)) < 0)
    throw IOException::format("Close error: {}", strerror(errno));
}

StreamOffset File::fsize(void* file) {
  SWITCH_FILE_SERIALIZE;
  StreamOffset pos = ftell(file);
  StreamOffset size = lseek(fdFromHandle(file), 0, SEEK_END);
  lseek(fdFromHandle(file), pos, SEEK_SET);
  return size;
}

size_t File::pread(void* file, char* data, size_t len, StreamOffset position) {
  SWITCH_FILE_SERIALIZE;
  int fd = fdFromHandle(file);
#ifdef STAR_SYSTEM_SWITCH
  if (fd <= 2) {
    char b[112];
    int l = snprintf(b, sizeof(b), "BADFD pread fd=%d (handle=%p) len=%zu pos=%lld", fd, file, len, (long long)position);
    if (l > 0) svcOutputDebugString(b, (size_t)l);
  }
  switchCheckUseAfterClose(fd, "pread");
#endif
  return ::pread(fd, data, len, position);
}

size_t File::pwrite(void* file, char const* data, size_t len, StreamOffset position) {
  SWITCH_FILE_SERIALIZE;
  int fd = fdFromHandle(file);
#ifdef STAR_SYSTEM_SWITCH
  if (fd <= 2) {
    char b[112];
    int l = snprintf(b, sizeof(b), "BADFD pwrite fd=%d (handle=%p) len=%zu pos=%lld", fd, file, len, (long long)position);
    if (l > 0) svcOutputDebugString(b, (size_t)l);
  }
  switchCheckUseAfterClose(fd, "pwrite");
#endif
  return ::pwrite(fd, data, len, position);
}

void File::resize(void* f, StreamOffset size) {
  SWITCH_FILE_SERIALIZE;
  if (::ftruncate(fdFromHandle(f), size) < 0)
    throw IOException::format("resize error: {}", strerror(errno));
}

}
