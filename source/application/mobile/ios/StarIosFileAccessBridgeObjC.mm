#ifdef STAR_SYSTEM_IOS

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include "SDL3/SDL_video.h"
#include <zlib.h>

#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#define STAR_IOS_HAS_UNIFORM_TYPES 1
#else
#import <MobileCoreServices/MobileCoreServices.h>
#define STAR_IOS_HAS_UNIFORM_TYPES 0
#endif

#include <dispatch/dispatch.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

@interface StarIosDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, strong) NSArray<NSURL*>* pickedUrls;
@property (nonatomic, strong) dispatch_semaphore_t semaphore;
@end

@implementation StarIosDocumentPickerDelegate
- (void)signalCompletion {
  if (self.semaphore)
    dispatch_semaphore_signal(self.semaphore);
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  (void)controller;
  self.pickedUrls = urls ?: @[];
  NSLog(@"[OpenStarbound][iOSBridge] document picker selected %lu URL(s)", (unsigned long)self.pickedUrls.count);
  [self signalCompletion];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentAtURL:(NSURL*)url {
  (void)controller;
  self.pickedUrls = url ? @[url] : @[];
  NSLog(@"[OpenStarbound][iOSBridge] document picker selected %@ URL", url ? @"one" : @"no");
  [self signalCompletion];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  (void)controller;
  self.pickedUrls = @[];
  NSLog(@"[OpenStarbound][iOSBridge] document picker cancelled");
  [self signalCompletion];
}
@end

namespace {

constexpr NSTimeInterval PickerTimeoutSeconds = 180.0;
char PickerDelegateAssociationKey;
SDL_Window* g_sdlWindow = nullptr;

enum class PickerMode {
  File,
  Folder
};

static NSString* toNSString(char const* value) {
  if (!value)
    return @"";
  return [NSString stringWithUTF8String:value] ?: @"";
}

static char* copyCString(NSString* value) {
  if (!value)
    return nullptr;

  char const* utf8 = value.UTF8String;
  if (!utf8)
    return nullptr;

  size_t length = std::strlen(utf8);
  char* out = static_cast<char*>(std::malloc(length + 1));
  if (!out)
    return nullptr;

  std::memcpy(out, utf8, length + 1);
  return out;
}

static void runOnMainSync(dispatch_block_t block) {
  if (!block)
    return;

  if ([NSThread isMainThread])
    block();
  else
    dispatch_sync(dispatch_get_main_queue(), block);
}

static void runOnMainAsync(dispatch_block_t block) {
  if (!block)
    return;
  dispatch_async(dispatch_get_main_queue(), block);
}

static bool waitForSemaphore(dispatch_semaphore_t semaphore, NSTimeInterval timeoutSeconds) {
  if (!semaphore)
    return false;

  if ([NSThread isMainThread]) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSeconds];
    while (dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) != 0) {
      if ([deadline timeIntervalSinceNow] <= 0.0)
        return false;
      @autoreleasepool {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
      }
    }
    return true;
  }

  int64_t timeoutNs = static_cast<int64_t>(timeoutSeconds * NSEC_PER_SEC);
  return dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, timeoutNs)) == 0;
}

static UIWindow* activeWindow() {
  if (g_sdlWindow) {
    SDL_PropertiesID properties = SDL_GetWindowProperties(g_sdlWindow);
    UIWindow* sdlWindow = (__bridge UIWindow*)SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (sdlWindow)
      return sdlWindow;
  }

  UIApplication* app = UIApplication.sharedApplication;
  if (!app)
    return nil;

  if (@available(iOS 13.0, *)) {
    for (UIScene* scene in app.connectedScenes) {
      if (scene.activationState != UISceneActivationStateForegroundActive)
        continue;
      if (![scene isKindOfClass:[UIWindowScene class]])
        continue;

      UIWindowScene* windowScene = (UIWindowScene*)scene;
      for (UIWindow* window in windowScene.windows) {
        if (window.isKeyWindow)
          return window;
      }
      for (UIWindow* window in windowScene.windows) {
        if (!window.hidden)
          return window;
      }
    }
  }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  for (UIWindow* window in app.windows) {
    if (window.isKeyWindow)
      return window;
  }
  for (UIWindow* window in app.windows) {
    if (!window.hidden)
      return window;
  }
#pragma clang diagnostic pop

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (app.keyWindow)
    return app.keyWindow;
#pragma clang diagnostic pop

  id<UIApplicationDelegate> delegate = app.delegate;
  if (delegate && [delegate respondsToSelector:@selector(window)]) {
    @try {
      UIWindow* window = [(id)delegate valueForKey:@"window"];
      if (window)
        return window;
    } @catch (NSException* exception) {
      NSLog(@"[OpenStarbound][iOSBridge] activeWindow delegate lookup exception: %@ (%@)", exception.name, exception.reason);
    }
  }

  return nil;
}

static UIViewController* topViewController() {
  UIWindow* window = activeWindow();
  if (!window)
    return nil;

  UIViewController* controller = window.rootViewController;
  if (!controller) {
    controller = [UIViewController new];
    window.rootViewController = controller;
  }

  while (true) {
    UIViewController* next = controller.presentedViewController;
    if ([controller isKindOfClass:[UINavigationController class]]) {
      UINavigationController* nav = (UINavigationController*)controller;
      if (nav.visibleViewController)
        next = nav.visibleViewController;
    } else if ([controller isKindOfClass:[UITabBarController class]]) {
      UITabBarController* tabs = (UITabBarController*)controller;
      if (tabs.selectedViewController)
        next = tabs.selectedViewController;
    }

    if (!next || next == controller)
      break;
    controller = next;
  }

  return controller;
}

static bool ensureDirectory(NSString* path) {
  if (!path || path.length == 0)
    return false;

  NSFileManager* fm = NSFileManager.defaultManager;
  BOOL isDirectory = NO;
  if ([fm fileExistsAtPath:path isDirectory:&isDirectory])
    return isDirectory;

  return [fm createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
}

static NSString* sanitizedFileName(NSString* value, NSString* fallback) {
  NSString* name = value.length > 0 ? value : fallback;
  NSMutableCharacterSet* invalid = [NSMutableCharacterSet characterSetWithCharactersInString:@"/:\\"];
  [invalid formUnionWithCharacterSet:[NSCharacterSet illegalCharacterSet]];
  NSArray<NSString*>* parts = [name componentsSeparatedByCharactersInSet:invalid];
  NSString* joined = [parts componentsJoinedByString:@"_"];
  return joined.length > 0 ? joined : fallback;
}

static bool isPakFileName(NSString* name) {
  return name && [name.lowercaseString hasSuffix:@".pak"];
}

static bool copyLocalFileToPathAtomic(NSString* sourcePath, NSString* targetPath);

static NSString* uniqueTargetPath(NSString* parent, NSString* requestedName, bool treatAsDirectory) {
  NSString* safeName = sanitizedFileName(requestedName, treatAsDirectory ? @"mod_folder" : @"mod.pak");
  NSString* baseName = safeName;
  NSString* extension = @"";

  if (!treatAsDirectory) {
    NSString* ext = safeName.pathExtension;
    if (ext.length > 0) {
      extension = [@"." stringByAppendingString:ext];
      baseName = safeName.stringByDeletingPathExtension;
    }
  }

  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* candidate = [parent stringByAppendingPathComponent:safeName];
  if (![fm fileExistsAtPath:candidate])
    return candidate;

  for (int i = 2; i < 10000; ++i) {
    NSString* nextName = [NSString stringWithFormat:@"%@ (%d)%@", baseName, i, extension];
    candidate = [parent stringByAppendingPathComponent:nextName];
    if (![fm fileExistsAtPath:candidate])
      return candidate;
  }

  NSString* fallback = [NSString stringWithFormat:@"%@_%lld%@", baseName, (long long)[NSDate date].timeIntervalSince1970, extension];
  return [parent stringByAppendingPathComponent:fallback];
}

static bool streamCopyUrlToPathAtomic(NSURL* sourceUrl, NSString* targetPath) {
  if (!sourceUrl || !targetPath || targetPath.length == 0)
    return false;

  NSString* parent = targetPath.stringByDeletingLastPathComponent;
  if (!ensureDirectory(parent))
    return false;

  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* tempPath = [targetPath stringByAppendingString:@".tmp"];
  [fm removeItemAtPath:tempPath error:nil];

  BOOL startedScopedAccess = [sourceUrl startAccessingSecurityScopedResource];
  NSInputStream* input = [NSInputStream inputStreamWithURL:sourceUrl];
  NSOutputStream* output = [NSOutputStream outputStreamToFileAtPath:tempPath append:NO];
  if (!input || !output) {
    if (startedScopedAccess)
      [sourceUrl stopAccessingSecurityScopedResource];
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  [input open];
  [output open];

  bool success = true;
  uint8_t buffer[64 * 1024];
  while (success) {
    NSInteger readBytes = [input read:buffer maxLength:sizeof(buffer)];
    if (readBytes < 0) {
      success = false;
      break;
    }
    if (readBytes == 0)
      break;

    NSInteger offset = 0;
    while (offset < readBytes) {
      NSInteger written = [output write:buffer + offset maxLength:(NSUInteger)(readBytes - offset)];
      if (written <= 0) {
        success = false;
        break;
      }
      offset += written;
    }
  }

  if (input.streamError || output.streamError)
    success = false;

  [input close];
  [output close];

  if (startedScopedAccess)
    [sourceUrl stopAccessingSecurityScopedResource];

  if (!success) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  if ([fm fileExistsAtPath:targetPath] && ![fm removeItemAtPath:targetPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  if (![fm moveItemAtPath:tempPath toPath:targetPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  return true;
}

struct ZipCentralEntry {
  std::string name;
  uint32_t crc = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t localOffset = 0;
};

static void writeZipU16(std::ofstream& out, uint16_t value) {
  char bytes[2] = {
    static_cast<char>(value & 0xff),
    static_cast<char>((value >> 8) & 0xff)
  };
  out.write(bytes, sizeof(bytes));
}

static void writeZipU32(std::ofstream& out, uint32_t value) {
  char bytes[4] = {
    static_cast<char>(value & 0xff),
    static_cast<char>((value >> 8) & 0xff),
    static_cast<char>((value >> 16) & 0xff),
    static_cast<char>((value >> 24) & 0xff)
  };
  out.write(bytes, sizeof(bytes));
}

static uint16_t readZipU16(std::vector<uint8_t> const& data, size_t offset) {
  return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

static uint32_t readZipU32(std::vector<uint8_t> const& data, size_t offset) {
  return (uint32_t)data[offset]
      | ((uint32_t)data[offset + 1] << 8)
      | ((uint32_t)data[offset + 2] << 16)
      | ((uint32_t)data[offset + 3] << 24);
}

static std::string utf8String(NSString* value) {
  char const* utf8 = value.UTF8String;
  return utf8 ? std::string(utf8) : std::string();
}

static bool readFileBytes(NSString* path, std::vector<uint8_t>& out) {
  std::ifstream input(utf8String(path), std::ios::binary);
  if (!input)
    return false;

  input.seekg(0, std::ios::end);
  std::streamoff length = input.tellg();
  if (length < 0 || length > 0x7fffffff)
    return false;
  input.seekg(0, std::ios::beg);

  out.resize((size_t)length);
  if (!out.empty())
    input.read(reinterpret_cast<char*>(out.data()), (std::streamsize)out.size());
  return input.good() || input.eof();
}

static bool zipEntryNameIsSafe(std::string const& name) {
  if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos)
    return false;

  size_t start = 0;
  while (start <= name.size()) {
    size_t slash = name.find('/', start);
    size_t end = slash == std::string::npos ? name.size() : slash;
    std::string part = name.substr(start, end - start);
    if (part == "..")
      return false;
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }

  return true;
}

static bool addStoredFileToZip(std::ofstream& out, NSString* sourcePath, std::string const& entryName, std::vector<ZipCentralEntry>& centralEntries) {
  if (!zipEntryNameIsSafe(entryName))
    return false;

  std::vector<uint8_t> bytes;
  if (!readFileBytes(sourcePath, bytes) || bytes.size() > 0xffffffffu)
    return false;

  std::streampos offset = out.tellp();
  if (offset < 0 || offset > 0xffffffff)
    return false;

  uint32_t crc = (uint32_t)crc32(0L, bytes.empty() ? nullptr : bytes.data(), (uInt)bytes.size());
  uint32_t size = (uint32_t)bytes.size();

  writeZipU32(out, 0x04034b50);
  writeZipU16(out, 20);
  writeZipU16(out, 0x0800);
  writeZipU16(out, 0);
  writeZipU16(out, 0);
  writeZipU16(out, 0);
  writeZipU32(out, crc);
  writeZipU32(out, size);
  writeZipU32(out, size);
  writeZipU16(out, (uint16_t)entryName.size());
  writeZipU16(out, 0);
  out.write(entryName.data(), (std::streamsize)entryName.size());
  if (!bytes.empty())
    out.write(reinterpret_cast<char const*>(bytes.data()), (std::streamsize)bytes.size());

  centralEntries.push_back({entryName, crc, size, size, (uint32_t)offset});
  return out.good();
}

static bool addDirectoryToSaveZip(std::ofstream& out, NSString* rootPath, NSString* directoryPath, std::string const& prefix, std::vector<ZipCentralEntry>& centralEntries) {
  NSFileManager* fm = NSFileManager.defaultManager;
  BOOL isDirectory = NO;
  if (![fm fileExistsAtPath:directoryPath isDirectory:&isDirectory] || !isDirectory)
    return true;

  NSDirectoryEnumerator<NSString*>* enumerator = [fm enumeratorAtPath:directoryPath];
  for (NSString* relative in enumerator) {
    NSString* fullPath = [directoryPath stringByAppendingPathComponent:relative];
    BOOL childIsDirectory = NO;
    if (![fm fileExistsAtPath:fullPath isDirectory:&childIsDirectory] || childIsDirectory)
      continue;

    NSString* normalized = [relative stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    std::string entryName = prefix + "/" + utf8String(normalized);
    if (!addStoredFileToZip(out, fullPath, entryName, centralEntries))
      return false;
  }
  return true;
}

static bool createSaveZip(NSString* storageRoot, NSString* zipPath) {
  if (!storageRoot || storageRoot.length == 0 || !zipPath || zipPath.length == 0)
    return false;

  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* playerPath = [storageRoot stringByAppendingPathComponent:@"player"];
  NSString* universePath = [storageRoot stringByAppendingPathComponent:@"universe"];
  BOOL playerIsDir = NO;
  BOOL universeIsDir = NO;
  bool hasSaveData = ([fm fileExistsAtPath:playerPath isDirectory:&playerIsDir] && playerIsDir)
      || ([fm fileExistsAtPath:universePath isDirectory:&universeIsDir] && universeIsDir);
  if (!hasSaveData)
    return false;

  if (!ensureDirectory(zipPath.stringByDeletingLastPathComponent))
    return false;

  std::ofstream out(utf8String(zipPath), std::ios::binary | std::ios::trunc);
  if (!out)
    return false;

  std::vector<ZipCentralEntry> centralEntries;
  if (!addDirectoryToSaveZip(out, playerPath, playerPath, "player", centralEntries))
    return false;
  if (!addDirectoryToSaveZip(out, universePath, universePath, "universe", centralEntries))
    return false;

  std::streampos centralOffset = out.tellp();
  if (centralOffset < 0 || centralOffset > 0xffffffff)
    return false;

  for (auto const& entry : centralEntries) {
    writeZipU32(out, 0x02014b50);
    writeZipU16(out, 20);
    writeZipU16(out, 20);
    writeZipU16(out, 0x0800);
    writeZipU16(out, 0);
    writeZipU16(out, 0);
    writeZipU16(out, 0);
    writeZipU32(out, entry.crc);
    writeZipU32(out, entry.compressedSize);
    writeZipU32(out, entry.uncompressedSize);
    writeZipU16(out, (uint16_t)entry.name.size());
    writeZipU16(out, 0);
    writeZipU16(out, 0);
    writeZipU16(out, 0);
    writeZipU16(out, 0);
    writeZipU32(out, 0);
    writeZipU32(out, entry.localOffset);
    out.write(entry.name.data(), (std::streamsize)entry.name.size());
  }

  std::streampos centralEnd = out.tellp();
  if (centralEnd < centralOffset || centralEnd > 0xffffffff)
    return false;

  writeZipU32(out, 0x06054b50);
  writeZipU16(out, 0);
  writeZipU16(out, 0);
  writeZipU16(out, (uint16_t)centralEntries.size());
  writeZipU16(out, (uint16_t)centralEntries.size());
  writeZipU32(out, (uint32_t)(centralEnd - centralOffset));
  writeZipU32(out, (uint32_t)centralOffset);
  writeZipU16(out, 0);
  return out.good();
}

static bool inflateRawDeflate(uint8_t const* source, size_t sourceSize, std::vector<uint8_t>& out, size_t expectedSize) {
  out.resize(expectedSize);
  z_stream stream = {};
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
    return false;

  stream.next_in = const_cast<Bytef*>(source);
  stream.avail_in = (uInt)sourceSize;
  stream.next_out = out.data();
  stream.avail_out = (uInt)out.size();

  int result = inflate(&stream, Z_FINISH);
  bool ok = result == Z_STREAM_END && stream.total_out == expectedSize;
  inflateEnd(&stream);
  return ok;
}

static bool writeBytesToPathAtomic(std::vector<uint8_t> const& bytes, NSString* targetPath) {
  if (!targetPath || targetPath.length == 0)
    return false;
  if (!ensureDirectory(targetPath.stringByDeletingLastPathComponent))
    return false;

  NSString* tempPath = [targetPath stringByAppendingString:@".tmp"];
  NSFileManager* fm = NSFileManager.defaultManager;
  [fm removeItemAtPath:tempPath error:nil];

  std::ofstream output(utf8String(tempPath), std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  if (!bytes.empty())
    output.write(reinterpret_cast<char const*>(bytes.data()), (std::streamsize)bytes.size());
  output.close();
  if (!output.good()) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  [fm removeItemAtPath:targetPath error:nil];
  if (![fm moveItemAtPath:tempPath toPath:targetPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }
  return true;
}

static bool unzipArchiveToRoot(NSString* zipPath, NSString* targetRoot) {
  std::vector<uint8_t> data;
  if (!readFileBytes(zipPath, data) || data.size() < 22)
    return false;

  size_t searchStart = data.size() > 65557 ? data.size() - 65557 : 0;
  size_t eocd = std::string::npos;
  for (size_t i = data.size() - 22; i + 1 > searchStart; --i) {
    if (readZipU32(data, i) == 0x06054b50) {
      eocd = i;
      break;
    }
    if (i == 0)
      break;
  }
  if (eocd == std::string::npos)
    return false;

  uint16_t entryCount = readZipU16(data, eocd + 10);
  uint32_t centralOffset = readZipU32(data, eocd + 16);
  if (centralOffset >= data.size())
    return false;

  bool extractedAny = false;
  size_t offset = centralOffset;
  for (uint16_t i = 0; i < entryCount; ++i) {
    if (offset + 46 > data.size() || readZipU32(data, offset) != 0x02014b50)
      return false;

    uint16_t flags = readZipU16(data, offset + 8);
    uint16_t method = readZipU16(data, offset + 10);
    uint32_t crc = readZipU32(data, offset + 16);
    uint32_t compressedSize = readZipU32(data, offset + 20);
    uint32_t uncompressedSize = readZipU32(data, offset + 24);
    uint16_t nameLength = readZipU16(data, offset + 28);
    uint16_t extraLength = readZipU16(data, offset + 30);
    uint16_t commentLength = readZipU16(data, offset + 32);
    uint32_t localOffset = readZipU32(data, offset + 42);
    if ((flags & 0x0001) != 0 || compressedSize == 0xffffffffu || uncompressedSize == 0xffffffffu)
      return false;
    if (offset + 46 + nameLength + extraLength + commentLength > data.size())
      return false;

    std::string name(reinterpret_cast<char const*>(data.data() + offset + 46), nameLength);
    std::replace(name.begin(), name.end(), '\\', '/');
    offset += 46 + nameLength + extraLength + commentLength;

    bool isDirectory = !name.empty() && name.back() == '/';
    if (!zipEntryNameIsSafe(name))
      return false;
    if (isDirectory)
      continue;

    if (localOffset + 30 > data.size() || readZipU32(data, localOffset) != 0x04034b50)
      return false;
    uint16_t localNameLength = readZipU16(data, localOffset + 26);
    uint16_t localExtraLength = readZipU16(data, localOffset + 28);
    size_t dataOffset = (size_t)localOffset + 30 + localNameLength + localExtraLength;
    if (dataOffset + compressedSize > data.size())
      return false;

    std::vector<uint8_t> uncompressed;
    if (method == 0) {
      if (compressedSize != uncompressedSize)
        return false;
      uncompressed.assign(data.begin() + dataOffset, data.begin() + dataOffset + compressedSize);
    } else if (method == 8) {
      if (!inflateRawDeflate(data.data() + dataOffset, compressedSize, uncompressed, uncompressedSize))
        return false;
    } else {
      return false;
    }

    uint32_t actualCrc = (uint32_t)crc32(0L, uncompressed.empty() ? nullptr : uncompressed.data(), (uInt)uncompressed.size());
    if (actualCrc != crc)
      return false;

    NSString* relative = [NSString stringWithUTF8String:name.c_str()];
    if (!relative)
      return false;
    NSString* targetPath = [targetRoot stringByAppendingPathComponent:relative];
    if (!writeBytesToPathAtomic(uncompressed, targetPath))
      return false;
    extractedAny = true;
  }

  return extractedAny;
}

static bool unzipSaveArchive(NSString* zipPath, NSString* tempRoot) {
  return unzipArchiveToRoot(zipPath, tempRoot);
}

static bool shouldIgnoreModImportEntryName(NSString* name) {
  return !name
      || name.length == 0
      || [name isEqualToString:@".DS_Store"]
      || [name isEqualToString:@"__MACOSX"]
      || [name hasPrefix:@"."];
}

static NSArray<NSString*>* visibleDirectoryEntries(NSString* directory) {
  NSArray<NSString*>* children = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:nil];
  if (!children)
    return @[];

  NSMutableArray<NSString*>* visible = [NSMutableArray array];
  for (NSString* child in children) {
    if (!shouldIgnoreModImportEntryName(child))
      [visible addObject:child];
  }
  return visible;
}

static NSString* zipBaseNameForModFolder(NSURL* selected) {
  NSString* name = selected.lastPathComponent;
  if (!name || name.length == 0)
    return @"mod_folder";
  if ([[name pathExtension].lowercaseString isEqualToString:@"zip"])
    name = name.stringByDeletingPathExtension;
  return sanitizedFileName(name, @"mod_folder");
}

static NSString* findSingleModZipPayloadRoot(NSString* tempRoot, NSString** outName) {
  NSArray<NSString*>* children = visibleDirectoryEntries(tempRoot);
  if (children.count == 1) {
    NSString* child = children.firstObject;
    NSString* childPath = [tempRoot stringByAppendingPathComponent:child];
    BOOL isDirectory = NO;
    if ([NSFileManager.defaultManager fileExistsAtPath:childPath isDirectory:&isDirectory] && isDirectory) {
      if (outName)
        *outName = sanitizedFileName(child, @"mod_folder");
      return childPath;
    }
  }

  return tempRoot;
}

static bool directoryLooksLikeModsFolder(NSString* directory) {
  NSArray<NSString*>* children = visibleDirectoryEntries(directory);
  for (NSString* child in children) {
    NSString* childPath = [directory stringByAppendingPathComponent:child];
    BOOL isDirectory = NO;
    if ([NSFileManager.defaultManager fileExistsAtPath:childPath isDirectory:&isDirectory] && (isDirectory || isPakFileName(child)))
      return true;
  }
  return false;
}

static NSString* findModsZipPayloadRoot(NSString* tempRoot) {
  NSString* explicitMods = [tempRoot stringByAppendingPathComponent:@"mods"];
  BOOL isDirectory = NO;
  if ([NSFileManager.defaultManager fileExistsAtPath:explicitMods isDirectory:&isDirectory] && isDirectory)
    return explicitMods;

  NSArray<NSString*>* children = visibleDirectoryEntries(tempRoot);
  if (children.count == 1) {
    NSString* childPath = [tempRoot stringByAppendingPathComponent:children.firstObject];
    if ([NSFileManager.defaultManager fileExistsAtPath:childPath isDirectory:&isDirectory] && isDirectory && directoryLooksLikeModsFolder(childPath))
      return childPath;
  }

  return tempRoot;
}

static bool hasSavePayload(NSString* directory) {
  NSFileManager* fm = NSFileManager.defaultManager;
  BOOL isDirectory = NO;
  NSString* player = [directory stringByAppendingPathComponent:@"player"];
  if ([fm fileExistsAtPath:player isDirectory:&isDirectory] && isDirectory)
    return true;
  NSString* universe = [directory stringByAppendingPathComponent:@"universe"];
  return [fm fileExistsAtPath:universe isDirectory:&isDirectory] && isDirectory;
}

static NSString* findSavePayloadRoot(NSString* directory, int depth) {
  if (!directory || depth < 0)
    return nil;
  if (hasSavePayload(directory))
    return directory;

  NSArray<NSString*>* children = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:nil];
  for (NSString* child in children) {
    NSString* childPath = [directory stringByAppendingPathComponent:child];
    BOOL isDirectory = NO;
    if ([NSFileManager.defaultManager fileExistsAtPath:childPath isDirectory:&isDirectory] && isDirectory) {
      NSString* found = findSavePayloadRoot(childPath, depth - 1);
      if (found)
        return found;
    }
  }
  return nil;
}

static bool copyLocalTree(NSString* source, NSString* target) {
  NSFileManager* fm = NSFileManager.defaultManager;
  BOOL isDirectory = NO;
  if (![fm fileExistsAtPath:source isDirectory:&isDirectory])
    return false;
  if (isDirectory) {
    if (!ensureDirectory(target))
      return false;
    NSArray<NSString*>* children = [fm contentsOfDirectoryAtPath:source error:nil];
    for (NSString* child in children) {
      if (!copyLocalTree([source stringByAppendingPathComponent:child], [target stringByAppendingPathComponent:child]))
        return false;
    }
    return true;
  }
  return copyLocalFileToPathAtomic(source, target);
}

static void importAllModsFromLocalDirectory(NSString* sourceDirectory, NSString* targetModsDirectory, NSMutableArray<NSString*>* importedFiles) {
  if (!sourceDirectory || !targetModsDirectory || !importedFiles)
    return;

  NSArray<NSString*>* entries = visibleDirectoryEntries(sourceDirectory);
  NSFileManager* fm = NSFileManager.defaultManager;
  for (NSString* entryName in entries) {
    NSString* sourcePath = [sourceDirectory stringByAppendingPathComponent:entryName];
    BOOL isDirectory = NO;
    if (![fm fileExistsAtPath:sourcePath isDirectory:&isDirectory])
      continue;

    if (isDirectory) {
      NSString* targetModRoot = uniqueTargetPath(targetModsDirectory, sanitizedFileName(entryName, @"mod_folder"), true);
      if (copyLocalTree(sourcePath, targetModRoot))
        [importedFiles addObject:targetModRoot];
    } else if (isPakFileName(entryName)) {
      NSString* targetPak = uniqueTargetPath(targetModsDirectory, sanitizedFileName(entryName, @"mod.pak"), false);
      if (copyLocalFileToPathAtomic(sourcePath, targetPak))
        [importedFiles addObject:targetPak];
    }
  }
}

static bool installSavePayload(NSString* payloadRoot, NSString* storageRoot) {
  if (!hasSavePayload(payloadRoot) || !ensureDirectory(storageRoot))
    return false;

  NSArray<NSString*>* saveDirectories = @[@"player", @"universe"];
  NSString* backupRoot = [storageRoot stringByAppendingPathComponent:[NSString stringWithFormat:@".save-import-backup-%lld", (long long)[NSDate date].timeIntervalSince1970]];
  NSMutableArray<NSString*>* moved = [NSMutableArray array];
  bool success = false;

  @try {
    NSFileManager* fm = NSFileManager.defaultManager;
    for (NSString* name in saveDirectories) {
      NSString* source = [payloadRoot stringByAppendingPathComponent:name];
      BOOL isDirectory = NO;
      if (![fm fileExistsAtPath:source isDirectory:&isDirectory] || !isDirectory)
        continue;

      NSString* target = [storageRoot stringByAppendingPathComponent:name];
      if ([fm fileExistsAtPath:target]) {
        if (!ensureDirectory(backupRoot))
          return false;
        NSString* backup = [backupRoot stringByAppendingPathComponent:name];
        if (![fm moveItemAtPath:target toPath:backup error:nil])
          return false;
        [moved addObject:name];
      }

      if (!copyLocalTree(source, target))
        return false;
    }

    success = true;
    return true;
  } @finally {
    NSFileManager* fm = NSFileManager.defaultManager;
    if (success) {
      [fm removeItemAtPath:backupRoot error:nil];
    } else {
      for (NSString* name in moved) {
        NSString* target = [storageRoot stringByAppendingPathComponent:name];
        NSString* backup = [backupRoot stringByAppendingPathComponent:name];
        [fm removeItemAtPath:target error:nil];
        [fm moveItemAtPath:backup toPath:target error:nil];
      }
      [fm removeItemAtPath:backupRoot error:nil];
    }
  }
}

static NSString* bundledOpensbPath() {
  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* resourcePath = NSBundle.mainBundle.resourcePath;
  if (!resourcePath || resourcePath.length == 0)
    return nil;

  NSArray<NSString*>* candidates = @[
    [resourcePath stringByAppendingPathComponent:@"assets/opensb"],
    [resourcePath stringByAppendingPathComponent:@"opensb"]
  ];

  for (NSString* candidate in candidates) {
    BOOL isDirectory = NO;
    if ([fm fileExistsAtPath:candidate isDirectory:&isDirectory] && isDirectory)
      return candidate;
  }

  return nil;
}

static bool copyBundleTreeIfMissing(NSString* sourcePath, NSString* targetPath) {
  NSFileManager* fm = NSFileManager.defaultManager;

  BOOL sourceIsDirectory = NO;
  if (![fm fileExistsAtPath:sourcePath isDirectory:&sourceIsDirectory])
    return false;

  if (sourceIsDirectory) {
    if (!ensureDirectory(targetPath))
      return false;

    NSArray<NSString*>* children = [fm contentsOfDirectoryAtPath:sourcePath error:nil];
    if (!children)
      return false;

    for (NSString* child in children) {
      NSString* childSource = [sourcePath stringByAppendingPathComponent:child];
      NSString* childTarget = [targetPath stringByAppendingPathComponent:child];
      if (!copyBundleTreeIfMissing(childSource, childTarget))
        return false;
    }
    return true;
  }

  if ([fm fileExistsAtPath:targetPath])
    return true;

  NSString* parent = targetPath.stringByDeletingLastPathComponent;
  if (!ensureDirectory(parent))
    return false;

  NSString* tempPath = [targetPath stringByAppendingString:@".tmp"];
  [fm removeItemAtPath:tempPath error:nil];
  if (![fm copyItemAtPath:sourcePath toPath:tempPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  if (![fm moveItemAtPath:tempPath toPath:targetPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  return true;
}

static NSString* preferredModsDirectoryPath(NSString* fallbackPath) {
  NSArray<NSString*>* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  NSString* base = docs.firstObject;
  if (!base || base.length == 0)
    base = fallbackPath;
  if (!base || base.length == 0)
    return nil;

  if (docs.firstObject && docs.firstObject.length > 0)
    return [base stringByAppendingPathComponent:@"mods"];
  return base;
}

static NSString* documentsDirectoryPath() {
  NSArray<NSString*>* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  return docs.firstObject;
}

static bool copyLocalFileToPathAtomic(NSString* sourcePath, NSString* targetPath) {
  if (!sourcePath || !targetPath || sourcePath.length == 0 || targetPath.length == 0)
    return false;

  if ([sourcePath isEqualToString:targetPath])
    return true;

  NSString* parent = targetPath.stringByDeletingLastPathComponent;
  if (!ensureDirectory(parent))
    return false;

  NSFileManager* fm = NSFileManager.defaultManager;
  NSString* tempPath = [targetPath stringByAppendingString:@".tmp"];
  [fm removeItemAtPath:tempPath error:nil];
  if (![fm copyItemAtPath:sourcePath toPath:tempPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  if ([fm fileExistsAtPath:targetPath] && ![fm removeItemAtPath:targetPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  if (![fm moveItemAtPath:tempPath toPath:targetPath error:nil]) {
    [fm removeItemAtPath:tempPath error:nil];
    return false;
  }

  return true;
}

static bool importPackedPakFromDocuments(NSString* destinationPath) {
  NSString* docs = documentsDirectoryPath();
  if (!docs || docs.length == 0)
    return false;

  NSArray<NSString*>* candidates = @[
    [docs stringByAppendingPathComponent:@"packed.pak"],
    [docs stringByAppendingPathComponent:@"assets/packed.pak"],
    [docs stringByAppendingPathComponent:@"Starbound/assets/packed.pak"]
  ];

  NSFileManager* fm = NSFileManager.defaultManager;
  for (NSString* candidate in candidates) {
    BOOL isDirectory = NO;
    if ([fm fileExistsAtPath:candidate isDirectory:&isDirectory] && !isDirectory)
      return copyLocalFileToPathAtomic(candidate, destinationPath);
  }

  return false;
}

static NSArray<NSURL*>* presentOpenPicker(PickerMode mode) {
  __block UIDocumentPickerViewController* picker = nil;
  __block bool presented = false;
  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
  StarIosDocumentPickerDelegate* delegate = [StarIosDocumentPickerDelegate new];
  delegate.semaphore = semaphore;

  runOnMainSync(^{
    UIViewController* presenter = topViewController();
    if (!presenter) {
      NSLog(@"[OpenStarbound][iOSBridge] presentOpenPicker: no presenter available");
      return;
    }

    if (@available(iOS 14.0, *)) {
#if STAR_IOS_HAS_UNIFORM_TYPES
      if (mode == PickerMode::Folder)
        picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
      else
        picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData, UTTypeItem] asCopy:YES];
#endif
    }

    if (!picker) {
#if STAR_IOS_HAS_UNIFORM_TYPES
      NSArray<NSString*>* types = mode == PickerMode::Folder ? @[@"public.folder"] : @[@"public.data", @"public.item"];
#else
      NSArray<NSString*>* types = mode == PickerMode::Folder ? @[(NSString*)kUTTypeFolder] : @[(NSString*)kUTTypeData, (NSString*)kUTTypeItem];
#endif
      UIDocumentPickerMode pickerMode = mode == PickerMode::Folder ? UIDocumentPickerModeOpen : UIDocumentPickerModeImport;
      picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:types inMode:pickerMode];
    }

    picker.delegate = delegate;
    picker.allowsMultipleSelection = mode == PickerMode::Folder;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    picker.popoverPresentationController.sourceView = presenter.view;
    picker.popoverPresentationController.sourceRect = presenter.view.bounds;
    picker.popoverPresentationController.permittedArrowDirections = 0;
    objc_setAssociatedObject(picker, &PickerDelegateAssociationKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [presenter presentViewController:picker animated:YES completion:nil];
    presented = true;
  });

  if (!presented) {
    NSLog(@"[OpenStarbound][iOSBridge] presentOpenPicker: picker was not presented");
    return @[];
  }

  if (!waitForSemaphore(semaphore, PickerTimeoutSeconds)) {
    NSLog(@"[OpenStarbound][iOSBridge] presentOpenPicker: timed out waiting for picker completion");
    runOnMainAsync(^{
      if (picker.presentingViewController)
        [picker dismissViewControllerAnimated:YES completion:nil];
    });
    return @[];
  }

  return delegate.pickedUrls ?: @[];
}

} // namespace

extern "C" void StarIosBridge_setSdlWindow(void* window) {
  g_sdlWindow = static_cast<SDL_Window*>(window);
}

extern "C" void StarIosBridge_getSafeAreaInsets(float* top, float* left, float* bottom, float* right) {
  __block UIEdgeInsets insets = UIEdgeInsetsZero;
  runOnMainSync(^{
    UIWindow* window = activeWindow();
    if (window)
      insets = window.safeAreaInsets;
  });
  if (top)    *top    = (float)insets.top;
  if (left)   *left   = (float)insets.left;
  if (bottom) *bottom = (float)insets.bottom;
  if (right)  *right  = (float)insets.right;
}

// Returns the current interface orientation as an integer:
//   1 = Portrait             (notch/DI at top)
//   2 = LandscapeLeft        (notch/DI on left  — device rotated left)
//   3 = LandscapeRight       (notch/DI on right — device rotated right)
//   4 = PortraitUpsideDown   (notch/DI at bottom)
//   0 = unknown
extern "C" int StarIosBridge_getInterfaceOrientation() {
  __block int result = 0;
  runOnMainSync(^{
    for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
      UIWindowScene* ws = (UIWindowScene*)[scene isKindOfClass:[UIWindowScene class]] ? (UIWindowScene*)scene : nil;
      if (!ws) continue;
      switch (ws.interfaceOrientation) {
        case UIInterfaceOrientationPortrait:           result = 1; break;
        case UIInterfaceOrientationLandscapeLeft:      result = 2; break;
        case UIInterfaceOrientationLandscapeRight:     result = 3; break;
        case UIInterfaceOrientationPortraitUpsideDown: result = 4; break;
        default: break;
      }
      break;
    }
  });
  return result;
}

extern "C" char* StarIosBridge_syncBundledAssets(char const* targetRootDirectory) {
  @try {
    @autoreleasepool {
      NSString* targetRoot = toNSString(targetRootDirectory);
      if (targetRoot.length == 0 || !ensureDirectory(targetRoot))
        return nullptr;

      NSString* sourceOpensb = bundledOpensbPath();
      if (!sourceOpensb)
        return nullptr;

      NSString* targetOpensb = [targetRoot stringByAppendingPathComponent:@"opensb"];
      if (!copyBundleTreeIfMissing(sourceOpensb, targetOpensb))
        return nullptr;

      NSString* resourcePath = NSBundle.mainBundle.resourcePath;
      NSArray<NSString*>* langCandidates = @[
        [resourcePath stringByAppendingPathComponent:@"assets/lang"],
        [resourcePath stringByAppendingPathComponent:@"lang"]
      ];
      for (NSString* sourceLang in langCandidates) {
        BOOL isDirectory = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:sourceLang isDirectory:&isDirectory] && isDirectory) {
          NSString* targetLang = [targetRoot stringByAppendingPathComponent:@"lang"];
          if (!copyBundleTreeIfMissing(sourceLang, targetLang))
            return nullptr;
          break;
        }
      }

      return copyCString(targetRoot);
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] syncBundledAssets exception: %@ (%@)", exception.name, exception.reason);
    return nullptr;
  }
}

extern "C" char* StarIosBridge_pickAndImportPackedPak(char const* targetPath) {
  @try {
    @autoreleasepool {
      NSString* destinationPath = toNSString(targetPath);
      if (destinationPath.length == 0)
        return nullptr;

      NSArray<NSURL*>* picked = presentOpenPicker(PickerMode::File);
      NSURL* selected = picked.firstObject;
      if (!selected) {
        if (importPackedPakFromDocuments(destinationPath))
          return copyCString(destinationPath);
        return nullptr;
      }

      if (!streamCopyUrlToPathAtomic(selected, destinationPath))
        return nullptr;

      return copyCString(destinationPath);
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] pickAndImportPackedPak exception: %@ (%@)", exception.name, exception.reason);
    return nullptr;
  }
}

extern "C" char* StarIosBridge_resolveModsDirectory(char const* fallbackModsDirectory) {
  @try {
    @autoreleasepool {
      NSString* fallback = toNSString(fallbackModsDirectory);
      NSString* modsDirectory = preferredModsDirectoryPath(fallback);
      if (!modsDirectory || !ensureDirectory(modsDirectory))
        return nullptr;
      return copyCString(modsDirectory);
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] resolveModsDirectory exception: %@ (%@)", exception.name, exception.reason);
    return nullptr;
  }
}

static char** copyImportedFileArray(NSArray<NSString*>* imported, int* outCount) {
  int count = (int)imported.count;
  if (outCount)
    *outCount = count;
  if (count <= 0)
    return nullptr;

  char** out = static_cast<char**>(std::calloc((size_t)count, sizeof(char*)));
  if (!out) {
    if (outCount)
      *outCount = 0;
    return nullptr;
  }

  for (int i = 0; i < count; ++i) {
    out[i] = copyCString(imported[(NSUInteger)i]);
    if (!out[i]) {
      for (int j = 0; j <= i; ++j)
        std::free(out[j]);
      std::free(out);
      if (outCount)
        *outCount = 0;
      return nullptr;
    }
  }

  return out;
}

extern "C" char** StarIosBridge_importModPakFiles(char const* modsDirectory, int* outCount) {
  @try {
    @autoreleasepool {
      if (outCount)
        *outCount = 0;

      NSString* targetModsDirectory = toNSString(modsDirectory);
      if (targetModsDirectory.length == 0)
        return nullptr;
      if (!ensureDirectory(targetModsDirectory))
        return nullptr;

      NSArray<NSURL*>* picked = presentOpenPicker(PickerMode::File);
      NSURL* selected = picked.firstObject;
      if (!selected)
        return nullptr;

      NSMutableArray<NSString*>* imported = [NSMutableArray array];
      NSString* name = selected.lastPathComponent;
      if (!name || name.length == 0)
        name = @"mod.pak";
      if (!isPakFileName(name))
        name = [name stringByAppendingString:@".pak"];

      NSString* target = uniqueTargetPath(targetModsDirectory, name, false);
      if (streamCopyUrlToPathAtomic(selected, target))
        [imported addObject:target];

      return copyImportedFileArray(imported, outCount);
    }
  } @catch (NSException* exception) {
    if (outCount)
      *outCount = 0;
    NSLog(@"[OpenStarbound][iOSBridge] importModPakFiles exception: %@ (%@)", exception.name, exception.reason);
    return nullptr;
  }
}

extern "C" char** StarIosBridge_importSingleModFolder(char const* modsDirectory, int* outCount) {
  @try {
    @autoreleasepool {
      if (outCount)
        *outCount = 0;

      NSString* targetModsDirectory = toNSString(modsDirectory);
      if (targetModsDirectory.length == 0)
        return nullptr;
      if (!ensureDirectory(targetModsDirectory))
        return nullptr;

      NSArray<NSURL*>* picked = presentOpenPicker(PickerMode::File);
      NSURL* selected = picked.firstObject;
      if (!selected)
        return nullptr;
      if (![[selected.pathExtension lowercaseString] isEqualToString:@"zip"])
        return nullptr;

      NSMutableArray<NSString*>* imported = [NSMutableArray array];
      NSFileManager* fm = NSFileManager.defaultManager;
      long long stamp = (long long)(NSDate.date.timeIntervalSince1970 * 1000.0);
      NSString* tempZip = [targetModsDirectory stringByAppendingPathComponent:[NSString stringWithFormat:@".mod-folder-import-%lld.zip", stamp]];
      NSString* tempRoot = [targetModsDirectory stringByAppendingPathComponent:[NSString stringWithFormat:@".mod-folder-import-%lld", stamp]];

      @try {
        if (!streamCopyUrlToPathAtomic(selected, tempZip))
          return nullptr;
        if (!ensureDirectory(tempRoot))
          return nullptr;
        if (!unzipArchiveToRoot(tempZip, tempRoot))
          return nullptr;

        NSString* rootName = zipBaseNameForModFolder(selected);
        NSString* payloadRoot = findSingleModZipPayloadRoot(tempRoot, &rootName);
        if (!payloadRoot)
          return nullptr;

        NSString* targetModRoot = uniqueTargetPath(targetModsDirectory, rootName, true);
        if (copyLocalTree(payloadRoot, targetModRoot))
          [imported addObject:targetModRoot];
      } @finally {
        [fm removeItemAtPath:tempZip error:nil];
        [fm removeItemAtPath:tempRoot error:nil];
      }

      return copyImportedFileArray(imported, outCount);
    }
  } @catch (NSException* exception) {
    if (outCount)
      *outCount = 0;
    NSLog(@"[OpenStarbound][iOSBridge] importSingleModFolder exception: %@ (%@)", exception.name, exception.reason);
    return nullptr;
  }
}

extern "C" char** StarIosBridge_importModsDirectory(char const* modsDirectory, int* outCount) {
  @try {
    @autoreleasepool {
      if (outCount)
        *outCount = 0;

      NSString* targetModsDirectory = toNSString(modsDirectory);
      if (targetModsDirectory.length == 0)
        return nullptr;
      if (!ensureDirectory(targetModsDirectory))
        return nullptr;

      NSArray<NSURL*>* picked = presentOpenPicker(PickerMode::File);
      NSURL* selected = picked.firstObject;
      if (!selected)
        return nullptr;
      if (![[selected.pathExtension lowercaseString] isEqualToString:@"zip"])
        return nullptr;

      NSMutableArray<NSString*>* imported = [NSMutableArray array];
      NSFileManager* fm = NSFileManager.defaultManager;
      long long stamp = (long long)(NSDate.date.timeIntervalSince1970 * 1000.0);
      NSString* tempZip = [targetModsDirectory stringByAppendingPathComponent:[NSString stringWithFormat:@".mods-folder-import-%lld.zip", stamp]];
      NSString* tempRoot = [targetModsDirectory stringByAppendingPathComponent:[NSString stringWithFormat:@".mods-folder-import-%lld", stamp]];

      @try {
        if (!streamCopyUrlToPathAtomic(selected, tempZip))
          return nullptr;
        if (!ensureDirectory(tempRoot))
          return nullptr;
        if (!unzipArchiveToRoot(tempZip, tempRoot))
          return nullptr;

        NSString* payloadRoot = findModsZipPayloadRoot(tempRoot);
        importAllModsFromLocalDirectory(payloadRoot, targetModsDirectory, imported);
      } @finally {
        [fm removeItemAtPath:tempZip error:nil];
        [fm removeItemAtPath:tempRoot error:nil];
      }

      return copyImportedFileArray(imported, outCount);
    }
  } @catch (NSException* exception) {
    if (outCount)
      *outCount = 0;
    NSLog(@"[OpenStarbound][iOSBridge] importModsDirectory exception: %@ (%@)", exception.name, exception.reason);
    return nullptr;
  }
}

extern "C" bool StarIosBridge_openModsDirectory(char const* modsDirectory) {
  @try {
    @autoreleasepool {
      NSString* resolved = toNSString(modsDirectory);
      if (resolved.length == 0)
        resolved = preferredModsDirectoryPath(@"");
      if (!resolved || !ensureDirectory(resolved))
        return false;

      __block bool presented = false;
      runOnMainSync(^{
        UIViewController* presenter = topViewController();
        if (!presenter)
          return;

        NSURL* modsUrl = [NSURL fileURLWithPath:resolved isDirectory:YES];
        if (@available(iOS 14.0, *)) {
#if STAR_IOS_HAS_UNIFORM_TYPES
          UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
          picker.directoryURL = modsUrl;
          picker.allowsMultipleSelection = false;
          picker.modalPresentationStyle = UIModalPresentationFormSheet;
          [presenter presentViewController:picker animated:YES completion:nil];
          presented = true;
          return;
#endif
        }

        UIActivityViewController* activity = [[UIActivityViewController alloc] initWithActivityItems:@[modsUrl] applicationActivities:nil];
        [presenter presentViewController:activity animated:YES completion:nil];
        presented = true;
      });

      return presented;
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] openModsDirectory exception: %@ (%@)", exception.name, exception.reason);
    return false;
  }
}

extern "C" bool StarIosBridge_openSaveDirectory(char const* storageRootDirectory) {
  @try {
    @autoreleasepool {
      NSString* resolved = toNSString(storageRootDirectory);
      if (resolved.length == 0 || !ensureDirectory(resolved))
        return false;

      __block bool presented = false;
      runOnMainSync(^{
        UIViewController* presenter = topViewController();
        if (!presenter)
          return;

        NSURL* saveUrl = [NSURL fileURLWithPath:resolved isDirectory:YES];
        if (@available(iOS 14.0, *)) {
#if STAR_IOS_HAS_UNIFORM_TYPES
          UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
          picker.directoryURL = saveUrl;
          picker.allowsMultipleSelection = false;
          picker.modalPresentationStyle = UIModalPresentationFormSheet;
          [presenter presentViewController:picker animated:YES completion:nil];
          presented = true;
          return;
#endif
        }

        UIActivityViewController* activity = [[UIActivityViewController alloc] initWithActivityItems:@[saveUrl] applicationActivities:nil];
        [presenter presentViewController:activity animated:YES completion:nil];
        presented = true;
      });

      return presented;
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] openSaveDirectory exception: %@ (%@)", exception.name, exception.reason);
    return false;
  }
}

extern "C" bool StarIosBridge_importSaveZip(char const* storageRootDirectory) {
  @try {
    @autoreleasepool {
      NSString* storageRoot = toNSString(storageRootDirectory);
      if (storageRoot.length == 0 || !ensureDirectory(storageRoot))
        return false;

      NSArray<NSURL*>* picked = presentOpenPicker(PickerMode::File);
      NSURL* selected = picked.firstObject;
      if (!selected)
        return false;

      NSString* tempZip = [storageRoot stringByAppendingPathComponent:[NSString stringWithFormat:@".save-import-%lld.zip", (long long)[NSDate date].timeIntervalSince1970]];
      NSString* tempRoot = [storageRoot stringByAppendingPathComponent:[NSString stringWithFormat:@".save-import-%lld", (long long)[NSDate date].timeIntervalSince1970]];

      NSFileManager* fm = NSFileManager.defaultManager;
      @try {
        if (!streamCopyUrlToPathAtomic(selected, tempZip))
          return false;
        if (!ensureDirectory(tempRoot))
          return false;
        if (!unzipSaveArchive(tempZip, tempRoot))
          return false;
        NSString* payloadRoot = findSavePayloadRoot(tempRoot, 3);
        if (!payloadRoot)
          return false;
        return installSavePayload(payloadRoot, storageRoot);
      } @finally {
        [fm removeItemAtPath:tempZip error:nil];
        [fm removeItemAtPath:tempRoot error:nil];
      }
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] importSaveZip exception: %@ (%@)", exception.name, exception.reason);
    return false;
  }
}

extern "C" bool StarIosBridge_exportSaveZip(char const* storageRootDirectory) {
  @try {
    @autoreleasepool {
      NSString* storageRoot = toNSString(storageRootDirectory);
      if (storageRoot.length == 0)
        return false;

      NSString* exportDir = [storageRoot stringByAppendingPathComponent:@"save-exports"];
      NSString* zipPath = [exportDir stringByAppendingPathComponent:[NSString stringWithFormat:@"openstarbound-save-%lld.zip", (long long)[NSDate date].timeIntervalSince1970]];
      if (!createSaveZip(storageRoot, zipPath))
        return false;

      NSURL* zipUrl = [NSURL fileURLWithPath:zipPath isDirectory:NO];
      __block bool presented = false;
      runOnMainSync(^{
        UIViewController* presenter = topViewController();
        if (!presenter)
          return;

        UIActivityViewController* activity = [[UIActivityViewController alloc] initWithActivityItems:@[zipUrl] applicationActivities:nil];
        activity.popoverPresentationController.sourceView = presenter.view;
        activity.popoverPresentationController.sourceRect = presenter.view.bounds;
        activity.popoverPresentationController.permittedArrowDirections = 0;
        [presenter presentViewController:activity animated:YES completion:nil];
        presented = true;
      });

      return presented;
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] exportSaveZip exception: %@ (%@)", exception.name, exception.reason);
    return false;
  }
}

extern "C" void StarIosBridge_showToast(char const* message) {
  @try {
    @autoreleasepool {
      NSString* text = toNSString(message);
      if (text.length == 0)
        return;

      runOnMainAsync(^{
        UIViewController* presenter = topViewController();
        if (!presenter)
          return;

        UIAlertController* alert = [UIAlertController alertControllerWithTitle:nil
                                                                       message:text
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [presenter presentViewController:alert animated:YES completion:nil];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
          if (alert.presentingViewController)
            [alert dismissViewControllerAnimated:YES completion:nil];
        });
      });
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] showToast exception: %@ (%@)", exception.name, exception.reason);
  }
}

extern "C" void StarIosBridge_showDialog(char const* title, char const* message) {
  @try {
    @autoreleasepool {
      NSString* nsTitle = toNSString(title);
      NSString* nsMessage = toNSString(message);

      runOnMainAsync(^{
        UIViewController* presenter = topViewController();
        if (!presenter)
          return;

        UIAlertController* alert = [UIAlertController alertControllerWithTitle:nsTitle
                                                                       message:nsMessage
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [presenter presentViewController:alert animated:YES completion:nil];
      });
    }
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] showDialog exception: %@ (%@)", exception.name, exception.reason);
  }
}

extern "C" bool StarIosBridge_openAppSettings() {
  @try {
    __block bool opened = false;
    runOnMainSync(^{
      UIApplication* app = UIApplication.sharedApplication;
      NSURL* settingsUrl = [NSURL URLWithString:UIApplicationOpenSettingsURLString];
      if (!app || !settingsUrl)
        return;
      if (![app canOpenURL:settingsUrl])
        return;

      [app openURL:settingsUrl options:@{} completionHandler:nil];
      opened = true;
    });
    return opened;
  } @catch (NSException* exception) {
    NSLog(@"[OpenStarbound][iOSBridge] openAppSettings exception: %@ (%@)", exception.name, exception.reason);
    return false;
  }
}

extern "C" void StarIosBridge_freeCString(char* value) {
  std::free(value);
}

extern "C" void StarIosBridge_freeCStringArray(char** values, int count) {
  if (!values)
    return;

  int limit = count > 0 ? count : 0;
  for (int i = 0; i < limit; ++i)
    std::free(values[i]);
  std::free(values);
}

#endif
