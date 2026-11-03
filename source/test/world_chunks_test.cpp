#include "StarWorldStorage.hpp"
#include "StarFile.hpp"
#include "StarDataStreamDevices.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(WorldChunks, RejectMalformedUpdateBeforeCreatingSave) {
  auto file = File::temporaryFile();
  String path = file->fileName();
  file->remove();
  WorldChunks update;
  update[ByteArray()] = ByteArray("bad", 3);
  EXPECT_THROW(WorldStorage::applyWorldChunksUpdateToFile(path, update), WorldStorageException);
  EXPECT_FALSE(File::exists(path));
  WorldStorage::applyWorldChunksUpdateToFile(path, {});
  EXPECT_FALSE(File::exists(path));
}

TEST(WorldChunks, RejectEntireMalformedUpdateWithoutChangingSave) {
  auto file = File::temporaryFile();
  auto cleanup = finally([&]() { file->remove(); });
  String path = file->fileName();
  file->close();
  ByteArray key("valid", 5);
  WorldChunks original;
  original[key] = ByteArray("original", 8);
  WorldStorage::applyWorldChunksUpdateToFile(path, original);
  auto before = File::readFile(path);
  for (size_t size : {size_t(0), size_t(4), size_t(6)}) {
    WorldChunks update;
    update[key] = ByteArray("changed", 7);
    update[ByteArray(size, 'x')] = {};
    EXPECT_THROW(WorldStorage::applyWorldChunksUpdateToFile(path, update), WorldStorageException);
    EXPECT_EQ(File::readFile(path), before);
    EXPECT_EQ(WorldStorage::getWorldChunksFromFile(path), original);
  }
}

TEST(WorldChunks, ValidInsertReplaceDeleteRoundTrip) {
  auto file = File::temporaryFile();
  auto cleanup = finally([&]() { file->remove(); });
  String path = file->fileName();
  file->close();
  ByteArray oldKey("old!!", 5), keepKey("keep!", 5), newKey("new!!", 5);
  WorldChunks initial;
  initial[oldKey] = ByteArray("delete", 6);
  initial[keepKey] = ByteArray("old", 3);
  WorldStorage::applyWorldChunksUpdateToFile(path, initial);
  WorldChunks update;
  update[oldKey] = {};
  update[keepKey] = ByteArray("replacement", 11);
  update[newKey] = ByteArray("new", 3);
  // Exercise the same serialization as ClientContextUpdate before applying it.
  update = DataStreamBuffer::deserialize<WorldChunks>(DataStreamBuffer::serialize(update));
  WorldStorage::applyWorldChunksUpdateToFile(path, update);
  WorldChunks expected;
  expected[keepKey] = *update[keepKey];
  expected[newKey] = *update[newKey];
  EXPECT_EQ(WorldStorage::getWorldChunksFromFile(path), expected);
}
