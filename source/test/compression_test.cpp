#include "StarAlgorithm.hpp"
#include "StarCompression.hpp"
#include "StarFile.hpp"

#include <zlib.h>

#include "gtest/gtest.h"

using namespace Star;

namespace {

ByteArray compressionPayload() {
  ByteArray data;
  for (size_t i = 0; i < 200000; ++i)
    data.appendByte((char)((i * 13 + i / 127) & 255));
  return data;
}

}

TEST(CompressionTest, ReadsLegacyZlib) {
  auto data = compressionPayload();
  ByteArray compressed(compressBound(data.size()), 0);
  uLongf size = compressed.size();
  ASSERT_EQ(compress2((Bytef*)compressed.ptr(), &size, (Bytef const*)data.ptr(), data.size(), 6), Z_OK);
  compressed.resize(size);
  EXPECT_EQ(uncompressData(compressed), data);
  EXPECT_THROW(uncompressData(compressed, 32768), IOException);
  compressed.resize(compressed.size() / 2);
  EXPECT_THROW(uncompressData(compressed), IOException);
}

TEST(CompressionTest, WritesLegacyCompatibleZlib) {
  auto data = compressionPayload();
  for (auto level : {LowCompression, MediumCompression, HighCompression}) {
    auto compressed = compressData(data, level);
    ByteArray decoded(data.size(), 0);
    uLongf size = decoded.size();
    ASSERT_EQ(uncompress((Bytef*)decoded.ptr(), &size, (Bytef const*)compressed.ptr(), compressed.size()), Z_OK);
    EXPECT_EQ(size, data.size());
    EXPECT_EQ(decoded, data);
  }
  EXPECT_TRUE(compressData(ByteArray()).empty());
  EXPECT_TRUE(uncompressData(ByteArray()).empty());
}

TEST(CompressionTest, ReadsLegacyGzipAndSeeks) {
  auto dir = File::temporaryDirectory();
  auto cleanup = finally([&] { File::removeDirectoryRecursive(dir); });
  auto path = File::relativeTo(dir, "legacy.gz");
  auto data = compressionPayload();
  // Use reference zlib's gzip stream format without linking its gzFile
  // implementation: zlib-ng 2.3.2 shares a private gz_error symbol with it.
  z_stream stream{};
  ASSERT_EQ(deflateInit2(&stream, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY), Z_OK);
  ByteArray compressed(deflateBound(&stream, data.size()), 0);
  stream.next_in = (Bytef*)data.ptr();
  stream.avail_in = data.size();
  stream.next_out = (Bytef*)compressed.ptr();
  stream.avail_out = compressed.size();
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  compressed.resize(stream.total_out);
  ASSERT_EQ(deflateEnd(&stream), Z_OK);
  File::writeFile(compressed, path);

  EXPECT_EQ(uncompressDataGzip(File::readFile(path)), data);
  EXPECT_THROW(uncompressDataGzip(File::readFile(path), 32768), IOException);
  auto file = CompressedFile::open(path, IOMode::Read);
  file->seek(40000);
  char bytes[32];
  ASSERT_EQ(file->read(bytes, sizeof(bytes)), sizeof(bytes));
  EXPECT_EQ(ByteArray(bytes, sizeof(bytes)), ByteArray(data.ptr() + 40000, sizeof(bytes)));
  auto clone = file->clone();
  EXPECT_EQ(clone->pos(), file->pos());
  clone->close();
  file->close();
}

TEST(CompressionTest, WritesLegacyCompatibleGzip) {
  auto dir = File::temporaryDirectory();
  auto cleanup = finally([&] { File::removeDirectoryRecursive(dir); });
  auto path = File::relativeTo(dir, "native.gz");
  auto data = compressionPayload();
  auto file = CompressedFile::open(path, IOMode::Write);
  EXPECT_EQ(file->write(data.ptr(), data.size()), data.size());
  file->sync();
  file->close();

  auto compressed = File::readFile(path);
  ByteArray decoded(data.size(), 0);
  z_stream stream{};
  ASSERT_EQ(inflateInit2(&stream, 31), Z_OK);
  stream.next_in = (Bytef*)compressed.ptr();
  stream.avail_in = compressed.size();
  stream.next_out = (Bytef*)decoded.ptr();
  stream.avail_out = decoded.size();
  ASSERT_EQ(inflate(&stream, Z_FINISH), Z_STREAM_END);
  EXPECT_EQ(stream.total_out, data.size());
  EXPECT_EQ(inflateEnd(&stream), Z_OK);

  EXPECT_EQ(decoded, data);
  EXPECT_EQ(uncompressDataGzip(File::readFile(path)), data);
}
