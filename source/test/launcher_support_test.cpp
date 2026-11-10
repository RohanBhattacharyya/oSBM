#include "gtest/gtest.h"
#include "StarFile.hpp"
#include "StarApplication.hpp"
#include "mobile/StarMobileLauncherSupport.hpp"

using namespace Star;

TEST(LauncherStorage, MigrationPreservesConflictsAndSkipsRuntimeFiles) {
  auto root = File::temporaryDirectory();
  auto oldRoot = File::relativeTo(root, "old");
  auto newRoot = File::relativeTo(root, "new");
  File::makeDirectoryRecursive(File::relativeTo(oldRoot, "player"));
  File::makeDirectoryRecursive(File::relativeTo(oldRoot, "logs"));
  File::makeDirectoryRecursive(File::relativeTo(newRoot, "player"));
  File::writeFile("old save", File::relativeTo(oldRoot, "player/conflict"));
  File::writeFile("new save", File::relativeTo(newRoot, "player/conflict"));
  File::writeFile("same", File::relativeTo(oldRoot, "player/same"));
  File::writeFile("same", File::relativeTo(newRoot, "player/same"));
  File::writeFile("unique", File::relativeTo(oldRoot, "player/unique"));
  File::writeFile("log", File::relativeTo(oldRoot, "logs/old.log"));
  migrateDirectoryFiles(oldRoot, newRoot);
  EXPECT_EQ("new save", File::readFileString(File::relativeTo(newRoot, "player/conflict")));
  EXPECT_EQ("old save", File::readFileString(File::relativeTo(oldRoot, "player/conflict")));
  EXPECT_EQ("unique", File::readFileString(File::relativeTo(newRoot, "player/unique")));
  EXPECT_FALSE(File::exists(File::relativeTo(oldRoot, "player/unique")));
  EXPECT_FALSE(File::exists(File::relativeTo(oldRoot, "player/same")));
  EXPECT_FALSE(File::exists(File::relativeTo(newRoot, "logs/old.log")));
  // Repeating an interrupted migration must not overwrite either save.
  migrateDirectoryFiles(oldRoot, newRoot);
  EXPECT_EQ("old save", File::readFileString(File::relativeTo(oldRoot, "player/conflict")));
  File::removeDirectoryRecursive(root);
}

TEST(LauncherStorage, ExplicitRootAndWritableDirectories) {
  auto root = File::temporaryDirectory();
  char const* previous = SDL_getenv("OSBM_STORAGE_DIRECTORY");
  String saved = previous ? previous : "";
  bool hadPrevious = previous != nullptr;
  SDL_setenv_unsafe("OSBM_STORAGE_DIRECTORY", root.utf8Ptr(), 1);
  EXPECT_EQ(root, defaultMobileStorageRoot());
  EXPECT_EQ(root, writableMobileStorageRoot(root));
  EXPECT_TRUE(File::isDirectory(File::relativeTo(root, "tmp")));
  if (hadPrevious)
    SDL_setenv_unsafe("OSBM_STORAGE_DIRECTORY", saved.utf8Ptr(), 1);
  else
    SDL_unsetenv_unsafe("OSBM_STORAGE_DIRECTORY");
  File::removeDirectoryRecursive(root);
}

#ifdef STAR_SYSTEM_LINUX
TEST(LauncherStorage, SelectionIsPersistentAndDoesNotMoveSaves) {
  // On Linux the preference path can be isolated without touching a user's
  // real launcher preferences. Other OS preference providers need native tests.
  struct Environment {
    char const* name;
    bool present;
    String value;
    explicit Environment(char const* key) : name(key), present(SDL_getenv(key) != nullptr),
        value(SDL_getenv(key) ? SDL_getenv(key) : "") {}
    ~Environment() {
      if (present) SDL_setenv_unsafe(name, value.utf8Ptr(), 1);
      else SDL_unsetenv_unsafe(name);
    }
  } storageEnv("OSBM_STORAGE_DIRECTORY"), prefEnv("XDG_DATA_HOME");
  auto root = File::temporaryDirectory();
  SDL_unsetenv_unsafe("OSBM_STORAGE_DIRECTORY");
  SDL_setenv_unsafe("XDG_DATA_HOME", root.utf8Ptr(), 1);
  auto portableRoot = File::relativeTo(SDL_GetBasePath(), "storage");
  EXPECT_EQ(portableRoot, defaultMobileStorageRoot());
  auto first = File::relativeTo(root, "first");
  auto second = File::relativeTo(root, "second");
  setDesktopStorageRoot(first);
  File::writeFile("save", File::relativeTo(first, "sentinel.player"));
  EXPECT_EQ(first, defaultMobileStorageRoot());
  setDesktopStorageRoot(second);
  EXPECT_EQ(second, defaultMobileStorageRoot());
  EXPECT_TRUE(File::isFile(File::relativeTo(first, "sentinel.player")));
  EXPECT_FALSE(File::exists(File::relativeTo(second, "sentinel.player")));
  SDL_setenv_unsafe("OSBM_STORAGE_DIRECTORY", first.utf8Ptr(), 1);
  EXPECT_EQ(first, defaultMobileStorageRoot());
  SDL_unsetenv_unsafe("OSBM_STORAGE_DIRECTORY");
  setDesktopStorageRoot("");
  EXPECT_EQ(portableRoot, defaultMobileStorageRoot());
  File::removeDirectoryRecursive(root);
}
#endif

TEST(LauncherInput, BackgroundAndFocusChangesCancelTouch) {
  for (auto type : {SDL_EVENT_WINDOW_FOCUS_LOST, SDL_EVENT_WINDOW_HIDDEN,
      SDL_EVENT_WINDOW_MINIMIZED, SDL_EVENT_WILL_ENTER_BACKGROUND, SDL_EVENT_DID_ENTER_BACKGROUND}) {
    SDL_Event event{};
    event.type = type;
    EXPECT_TRUE(shouldCancelMobileTouchState(event));
  }
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  EXPECT_FALSE(shouldCancelMobileTouchState(event));
}

TEST(LauncherWindow, BorderlessModeSurvivesSizeNotifications) {
  // SDL's dummy backend exercises real window state without a display server.
  ASSERT_TRUE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
  ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
  auto window = SDL_CreateWindow("window mode test", 640, 480, SDL_WINDOW_HIDDEN);
  ASSERT_NE(nullptr, window) << SDL_GetError();
  EXPECT_EQ(WindowMode::Normal, desktopWindowMode(window));
  ASSERT_TRUE(SDL_SetWindowFullscreenMode(window, nullptr));
  ASSERT_TRUE(SDL_SetWindowFullscreen(window, true)) << SDL_GetError();
  EXPECT_EQ(WindowMode::Borderless, desktopWindowMode(window));
  // Dummy has no exclusive display modes. Exclusive fullscreen is checked
  // separately in the native desktop runtime pass.
  ASSERT_TRUE(SDL_SetWindowFullscreen(window, false));
  EXPECT_EQ(WindowMode::Normal, desktopWindowMode(window));
  SDL_DestroyWindow(window);
  SDL_Quit();
}
