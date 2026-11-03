#include "StarAssets.hpp"
#include "StarBuffer.hpp"
#include "StarFile.hpp"
#include "StarImage.hpp"
#include "StarLogging.hpp"
#include "StarThread.hpp"

#include <cstring>

#include "gtest/gtest.h"

using namespace Star;

TEST(AssetsTest, ConcurrentImagePatches) {
  // Generated fixtures only: no packed.pak or player's saves are accessed.
  String directory = File::temporaryDirectory();
  auto cleanup = finally([&]() { File::removeDirectoryRecursive(directory); });
  auto png = make_shared<Buffer>();
  Vec4B color(20, 40, 60, 255);
  Image::filled(Vec2U(8, 8), color).writePng(png);
  StringList paths;
  for (unsigned i = 0; i < 128; ++i) {
    String name = strf("/image{}.png", i);
    paths.append(name);
    auto file = File::relativeTo(directory, name.substr(1));
    File::writeFile(png->data(), file);
    if (i % 4 == 1)
      File::writeFile(String("function patch(image) return image end"), file + ".patch.lua");
    else if (i % 4 == 2)
      File::writeFile(String("function patch(image) return image:process('?crop=0;0;4;4') end"), file + ".patch.lua");
    else if (i % 4 == 3)
      File::writeFile(String("function patch(image) return nil end"), file + ".patch.lua");
  }

  Assets::Settings settings{};
  settings.assetTimeToLive = 10;
  settings.workerPoolSize = 2;
  settings.skipDigest = true;
  settings.skipPreload = true;
  Assets assets(settings, {directory});
  for (unsigned round = 0; round < 4; ++round) {
    assets.hotReload();
    assets.queueImages(paths);
    std::atomic<unsigned> failures{0};
    List<ThreadFunction<void>> readers;
    for (unsigned lane = 0; lane < 8; ++lane) {
      readers.append(Thread::invoke("imagePatchTest", [&, lane]() {
        try {
          for (unsigned i = lane; i < paths.size(); i += 8) {
            auto image = assets.image(paths[i]);
            unsigned size = i % 4 == 2 ? 4 : 8;
            if (image->size() != Vec2U(size, size) || image->get(Vec2U()) != color)
              ++failures;
          }
        } catch (...) {
          ++failures;
        }
      }));
    }
    for (auto& reader : readers)
      reader.finish();
    EXPECT_EQ(failures.load(), 0u);
  }
}

TEST(AssetsTest, NonImagePatchReturnKeepsOriginal) {
  struct WarningSink : LogSink {
    WarningSink() { setLevel(LogLevel::Warn); }
    void log(char const* message, LogLevel level) override {
      if (level == LogLevel::Warn && std::strstr(message, "/test.png.patch.lua")
          && std::strstr(message, "non-Image value"))
        ++warnings;
    }
    std::atomic<unsigned> warnings{0};
  };
  auto warnings = make_shared<WarningSink>();
  Logger::addSink(warnings);
  auto removeSink = finally([&]() { Logger::removeSink(warnings); });
  String directory = File::temporaryDirectory();
  auto cleanup = finally([&]() { File::removeDirectoryRecursive(directory); });
  auto png = make_shared<Buffer>();
  Vec4B color(20, 40, 60, 255);
  Image::filled(Vec2U(8, 8), color).writePng(png);
  File::writeFile(png->data(), File::relativeTo(directory, "test.png"));
  File::writeFile(String("function patch(image) return 17 end"),
      File::relativeTo(directory, "test.png.patch.lua"));

  Assets::Settings settings{};
  settings.assetTimeToLive = 10;
  settings.skipDigest = true;
  settings.skipPreload = true;
  Assets assets(settings, {directory});
  auto image = assets.image("/test.png");
  EXPECT_EQ(image->size(), Vec2U(8, 8));
  EXPECT_EQ(image->get(Vec2U()), color);
  EXPECT_EQ(warnings->warnings.load(), 1u);
}

TEST(AssetsTest, All) {
  EXPECT_EQ(AssetPath::removeDirectives("/foo/bar/baz??::?:??:asdfasdf??D?"), "/foo/bar/baz");
  EXPECT_EQ(AssetPath::directory("/foo/bar/baz"), "/foo/bar/");
  EXPECT_EQ(AssetPath::directory("foo/bar/baz"), "foo/bar/");
  EXPECT_EQ(AssetPath::directory("foo"), "");
  EXPECT_EQ(AssetPath::directory("/foo"), "/");
  EXPECT_EQ(AssetPath::filename(""), "");
  EXPECT_EQ(AssetPath::filename("foo"), "foo");
  EXPECT_EQ(AssetPath::filename("/foo"), "foo");
  EXPECT_EQ(AssetPath::filename("/foo/"), "");
  EXPECT_EQ(AssetPath::filename("/foo/bar"), "bar");

  //AssetPath compare = AssetPath{"/foo/bar/baz", String("baf"), {"whoa", "there"}};
  //EXPECT_EQ(AssetPath::split("/foo/bar/baz:baf?whoa?there"), compare);

  EXPECT_EQ(
      AssetPath::relativeTo("/foo/bar/baz:baf?whoa?there", "thing:sub?directive"), "/foo/bar/thing:sub?directive");
}
