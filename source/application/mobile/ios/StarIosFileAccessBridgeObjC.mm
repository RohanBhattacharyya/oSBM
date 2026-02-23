#ifdef STAR_SYSTEM_IOS

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#define STAR_IOS_HAS_UNIFORM_TYPES 1
#else
#import <MobileCoreServices/MobileCoreServices.h>
#define STAR_IOS_HAS_UNIFORM_TYPES 0
#endif

#include <dispatch/dispatch.h>
#include <cstdlib>
#include <cstring>

@interface StarIosDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, strong) NSArray<NSURL*>* pickedUrls;
@property (nonatomic) dispatch_semaphore_t semaphore;
@end

@implementation StarIosDocumentPickerDelegate
- (void)signalCompletion {
  if (self.semaphore)
    dispatch_semaphore_signal(self.semaphore);
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  (void)controller;
  self.pickedUrls = urls ?: @[];
  [self signalCompletion];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentAtURL:(NSURL*)url {
  (void)controller;
  self.pickedUrls = url ? @[url] : @[];
  [self signalCompletion];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  (void)controller;
  self.pickedUrls = @[];
  [self signalCompletion];
}
@end

namespace {

constexpr NSTimeInterval PickerTimeoutSeconds = 180.0;
char PickerDelegateAssociationKey;

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
  return app.keyWindow;
#pragma clang diagnostic pop
}

static UIViewController* topViewController() {
  UIWindow* window = activeWindow();
  UIViewController* controller = window.rootViewController;
  if (!controller)
    return nil;

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

static bool clearDirectoryContents(NSString* directoryPath) {
  if (!ensureDirectory(directoryPath))
    return false;

  NSFileManager* fm = NSFileManager.defaultManager;
  NSError* listError = nil;
  NSArray<NSString*>* children = [fm contentsOfDirectoryAtPath:directoryPath error:&listError];
  if (!children)
    return listError == nil;

  for (NSString* child in children) {
    NSString* childPath = [directoryPath stringByAppendingPathComponent:child];
    if (![fm removeItemAtPath:childPath error:nil])
      return false;
  }
  return true;
}

static bool copyDirectoryUrlContents(NSURL* sourceDirUrl, NSString* targetDirPath, NSMutableArray<NSString*>* importedFiles) {
  if (!sourceDirUrl || !targetDirPath || !importedFiles)
    return false;

  if (!ensureDirectory(targetDirPath))
    return false;

  BOOL startedScopedAccess = [sourceDirUrl startAccessingSecurityScopedResource];

  NSFileManager* fm = NSFileManager.defaultManager;
  NSError* listError = nil;
  NSArray<NSURL*>* children = [fm contentsOfDirectoryAtURL:sourceDirUrl
                                includingPropertiesForKeys:@[NSURLNameKey, NSURLIsDirectoryKey]
                                                   options:0
                                                     error:&listError];
  if (!children) {
    if (startedScopedAccess)
      [sourceDirUrl stopAccessingSecurityScopedResource];
    return listError == nil;
  }

  for (NSURL* childUrl in children) {
    NSNumber* isDirectory = nil;
    [childUrl getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];

    NSString* childName = nil;
    [childUrl getResourceValue:&childName forKey:NSURLNameKey error:nil];
    childName = sanitizedFileName(childName, isDirectory.boolValue ? @"mod_folder" : @"mod_file");
    NSString* targetChild = [targetDirPath stringByAppendingPathComponent:childName];

    if (isDirectory.boolValue) {
      if (!copyDirectoryUrlContents(childUrl, targetChild, importedFiles)) {
        if (startedScopedAccess)
          [sourceDirUrl stopAccessingSecurityScopedResource];
        return false;
      }
    } else {
      if (streamCopyUrlToPathAtomic(childUrl, targetChild))
        [importedFiles addObject:targetChild];
    }
  }

  if (startedScopedAccess)
    [sourceDirUrl stopAccessingSecurityScopedResource];

  return true;
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

static NSArray<NSURL*>* presentOpenPicker(bool folderOnly) {
  __block UIDocumentPickerViewController* picker = nil;
  __block bool presented = false;
  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
  StarIosDocumentPickerDelegate* delegate = [StarIosDocumentPickerDelegate new];
  delegate.semaphore = semaphore;

  runOnMainSync(^{
    UIViewController* presenter = topViewController();
    if (!presenter)
      return;

    if (@available(iOS 14.0, *)) {
#if STAR_IOS_HAS_UNIFORM_TYPES
      if (folderOnly)
        picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[[UTType folder]] asCopy:NO];
      else
        picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[[UTType data]] asCopy:NO];
#endif
    }

    if (!picker) {
#if STAR_IOS_HAS_UNIFORM_TYPES
      NSArray<NSString*>* types = folderOnly ? @[@"public.folder"] : @[@"public.data", @"public.item"];
#else
      NSArray<NSString*>* types = folderOnly ? @[(NSString*)kUTTypeFolder] : @[(NSString*)kUTTypeData, (NSString*)kUTTypeItem];
#endif
      picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:types inMode:UIDocumentPickerModeOpen];
    }

    picker.delegate = delegate;
    picker.allowsMultipleSelection = false;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    objc_setAssociatedObject(picker, &PickerDelegateAssociationKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [presenter presentViewController:picker animated:YES completion:nil];
    presented = true;
  });

  if (!presented)
    return @[];

  if (!waitForSemaphore(semaphore, PickerTimeoutSeconds)) {
    runOnMainAsync(^{
      if (picker.presentingViewController)
        [picker dismissViewControllerAnimated:YES completion:nil];
    });
    return @[];
  }

  return delegate.pickedUrls ?: @[];
}

} // namespace

extern "C" char* StarIosBridge_syncBundledAssets(char const* targetRootDirectory) {
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

    return copyCString(targetRoot);
  }
}

extern "C" char* StarIosBridge_pickAndImportPackedPak(char const* targetPath) {
  @autoreleasepool {
    NSString* destinationPath = toNSString(targetPath);
    if (destinationPath.length == 0)
      return nullptr;

    NSArray<NSURL*>* picked = presentOpenPicker(false);
    NSURL* selected = picked.firstObject;
    if (!selected)
      return nullptr;

    if (!streamCopyUrlToPathAtomic(selected, destinationPath))
      return nullptr;

    return copyCString(destinationPath);
  }
}

extern "C" char* StarIosBridge_resolveModsDirectory(char const* fallbackModsDirectory) {
  @autoreleasepool {
    NSString* fallback = toNSString(fallbackModsDirectory);
    NSString* modsDirectory = preferredModsDirectoryPath(fallback);
    if (!modsDirectory || !ensureDirectory(modsDirectory))
      return nullptr;
    return copyCString(modsDirectory);
  }
}

extern "C" char** StarIosBridge_importModFiles(char const* modsDirectory, int* outCount) {
  @autoreleasepool {
    if (outCount)
      *outCount = 0;

    NSString* targetModsDirectory = toNSString(modsDirectory);
    if (targetModsDirectory.length == 0)
      return nullptr;
    if (!ensureDirectory(targetModsDirectory))
      return nullptr;

    NSArray<NSURL*>* picked = presentOpenPicker(true);
    NSURL* selected = picked.firstObject;
    if (!selected)
      return nullptr;

    if (!clearDirectoryContents(targetModsDirectory))
      return nullptr;

    NSMutableArray<NSString*>* imported = [NSMutableArray array];

    NSNumber* isDirectory = nil;
    [selected getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
    bool importSuccess = false;
    if (isDirectory.boolValue) {
      importSuccess = copyDirectoryUrlContents(selected, targetModsDirectory, imported);
    } else {
      NSString* fileName = sanitizedFileName(selected.lastPathComponent, @"mod_file");
      NSString* target = [targetModsDirectory stringByAppendingPathComponent:fileName];
      if (streamCopyUrlToPathAtomic(selected, target)) {
        [imported addObject:target];
        importSuccess = true;
      }
    }

    if (!importSuccess)
      return nullptr;

    int count = (int)imported.count;
    if (outCount)
      *outCount = count;
    if (count <= 0)
      return nullptr;

    char** out = static_cast<char**>(std::calloc((size_t)count, sizeof(char*)));
    if (!out)
      return nullptr;

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
}

extern "C" bool StarIosBridge_openModsDirectory(char const* modsDirectory) {
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
        UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[[UTType folder]] asCopy:NO];
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
}

extern "C" void StarIosBridge_showToast(char const* message) {
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
}

extern "C" void StarIosBridge_showDialog(char const* title, char const* message) {
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
}

extern "C" bool StarIosBridge_openAppSettings() {
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
