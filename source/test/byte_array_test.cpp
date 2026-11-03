#include "StarByteArray.hpp"
#include "StarEncode.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(ByteArrayTest, SliceBounds) {
  auto bytes = ByteArray::fromCString("abcdef");
  EXPECT_EQ(bytes.sub(2, 2), ByteArray::fromCString("cd"));
  EXPECT_EQ(bytes.sub(4, 100), ByteArray::fromCString("ef"));
  EXPECT_EQ(bytes.sub(1, NPos), ByteArray::fromCString("bcdef"));
  EXPECT_TRUE(bytes.sub(6, 1).empty());
  EXPECT_TRUE(bytes.sub(NPos, 2).empty());
  EXPECT_TRUE(bytes.sub(1, 0).empty());
  EXPECT_TRUE(ByteArray().sub(0, NPos).empty());
  EXPECT_EQ(bytes.left(2), ByteArray::fromCString("ab"));
  EXPECT_EQ(bytes.right(2), ByteArray::fromCString("ef"));
  EXPECT_TRUE(bytes.right(0).empty());
  EXPECT_EQ(bytes.right(100), bytes);
}

TEST(ByteArrayTest, AppendSelfAcrossGrowth) {
  for (size_t size : {size_t(0), size_t(1), size_t(64), size_t(4096)}) {
    ByteArray bytes(size, 'x');
    bytes.append(bytes);
    EXPECT_EQ(bytes.size(), size * 2);
    for (auto c : bytes)
      EXPECT_EQ(c, 'x');
  }
}

TEST(ByteArrayTest, All) {
  auto res = ByteArray::fromCString("foobar");
  res.insert(2, 'a');
  res.insert(6, 'b');
  res.push_back('c');
  res.insert(9, 'd');
  EXPECT_EQ(res, ByteArray::fromCString("foaobabrcd"));

  auto a = hexDecode("0a0a0a");
  auto b = hexDecode("a0a0a0");
  auto c = hexDecode("818181");
  auto d = hexDecode("aaaaaa");
  auto e = hexDecode("000000");
  auto f = hexDecode("212121");

  auto g = hexDecode("a0a0a0");
  auto h = hexDecode("8181818181");
  auto i = hexDecode("2121218181");

  EXPECT_EQ(a.andWith(b), e);
  EXPECT_EQ(a.orWith(b), d);
  EXPECT_EQ(b.xorWith(c), f);
  EXPECT_EQ(g.xorWith(h), f);
  EXPECT_EQ(g.xorWith(h, true), i);
  EXPECT_EQ(h.xorWith(g, true), i);
}
