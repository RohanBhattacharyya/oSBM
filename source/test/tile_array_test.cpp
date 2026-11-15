#include "StarTileSectorArray.hpp"
#include "StarThread.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(TileSectorArrayTest, ParallelGather) {
  using TileArray = TileSectorArray<int, 32>;
  TileArray tiles({96, 64}, -1);
  tiles.loadSector({0, 0}, make_unique<TileArray::Array>(3));
  tiles.loadSector({2, 1}, make_unique<TileArray::Array>(7));
  // Exercise wrapped X coordinates, clamped Y coordinates and absent sectors
  // with concurrent render/lighting-style gathers sharing the same pool.
  auto gather = [&] {
    for (unsigned iteration = 0; iteration < 20; ++iteration) {
      auto result = tiles.tileEachResult(RectI(-9, -5, 48, 70), [](Vec2I const& pos, int tile) {
        return tile + pos[0] * 100 + pos[1] * 10000;
      });
      for (int x = -9; x < 48; ++x)
        for (int y = -5; y < 70; ++y)
          EXPECT_EQ(result(x + 9, y + 5), tiles.tile({x, y}) + x * 100 + y * 10000);
    }
  };
  auto worker = Thread::invoke("ParallelGatherTest", gather);
  gather();
  worker.finish();
}

TEST(TileSectorArrayTest, All) {
  typedef TileSectorArray<int, 32> TileArray;
  TileArray tileSectorArray({100, 100}, -1);

  EXPECT_TRUE(tileSectorArray.sectorValid(TileArray::Sector(1, 1)));
  EXPECT_TRUE(tileSectorArray.sectorValid(TileArray::Sector(3, 3)));
  EXPECT_FALSE(tileSectorArray.sectorValid(TileArray::Sector(4, 4)));

  EXPECT_TRUE(List<TileArray::Sector>({{0, 0}, {1, 0}}) == tileSectorArray.validSectorsFor(RectI(0, -32, 64, 32)));

  EXPECT_TRUE(TileArray::Sector(0, 0) == tileSectorArray.sectorFor({0, 0}));
  EXPECT_TRUE(TileArray::Sector(3, 1) == tileSectorArray.sectorFor({-1, 33}));
  EXPECT_TRUE(-1 == tileSectorArray.tile({-1, -1}));
  EXPECT_TRUE(nullptr == tileSectorArray.modifyTile({-1, -1}));
  EXPECT_TRUE(RectI(32, 32, 64, 64) == tileSectorArray.sectorRegion({1, 1}));
  EXPECT_TRUE(TileArray::Sector(0, 3) == tileSectorArray.adjacentSector({3, 3}, {1, 0}));

  tileSectorArray.loadSector({0, 0}, make_unique<TileArray::Array>(1));
  tileSectorArray.loadSector({1, 0}, make_unique<TileArray::Array>(1));
  tileSectorArray.loadSector({2, 0}, make_unique<TileArray::Array>(1));
  tileSectorArray.loadSector({3, 0}, make_unique<TileArray::Array>(1));
  tileSectorArray.loadSector({0, 1}, make_unique<TileArray::Array>(2));
  tileSectorArray.loadSector({1, 1}, make_unique<TileArray::Array>(2));
  tileSectorArray.loadSector({2, 1}, make_unique<TileArray::Array>(2));
  tileSectorArray.loadSector({3, 1}, make_unique<TileArray::Array>(2));

  Set<Vec2I> found;
  tileSectorArray.tileEach(RectI(-2, 0, 3, 1),
      [&found](Vec2I const& pos, int tile) {
        found.add(pos);
        EXPECT_TRUE(pos[0] >= -2 && pos[0] < 3);
        EXPECT_TRUE(pos[1] == 0);
        EXPECT_EQ(1, tile);
      });
  EXPECT_TRUE(found.contains(Vec2I(0, 0)));
  EXPECT_TRUE(found.contains(Vec2I(-1, 0)));
  EXPECT_TRUE(found.contains(Vec2I(-2, 0)));
  EXPECT_TRUE(found.contains(Vec2I(1, 0)));

  tileSectorArray.tileEach(RectI(-10, 0, -1, 1),
      [](Vec2I const& pos, int tile) {
        EXPECT_TRUE(pos[0] >= -10 && pos[0] < -1);
        EXPECT_EQ(1, tile);
      });

  tileSectorArray.tileEach(RectI(-10, -1, -1, 0),
      [](Vec2I const& pos, int tile) {
        EXPECT_TRUE(pos[0] >= -10 && pos[0] < -1);
        EXPECT_TRUE(pos[1] == -1);
        EXPECT_EQ(-1, tile);
      });

  found.clear();
  tileSectorArray.tileEach(RectI(110, 101, 120, 102),
      [&found](Vec2I const& pos, int tile) {
        found.add(pos);
        EXPECT_TRUE(pos[0] >= 110 && pos[0] < 120);
        EXPECT_TRUE(pos[1] == 101);
        EXPECT_EQ(-1, tile);
      });
  EXPECT_TRUE(found.contains(Vec2I(110, 101)));
  EXPECT_TRUE(found.contains(Vec2I(119, 101)));

  auto res1 = tileSectorArray.tileEachResult(RectI(110, 110, 120, 120),
      [](Vec2I const& pos, int tile) -> int {
        return (pos[0] >= 110 && pos[0] < 120 && pos[1] >= 110 && pos[1] < 120 && tile == -1) ? 1 : 0;
      });
  MultiArray<int, 2> res1comp({10, 10}, 1);

  EXPECT_TRUE(res1.size() == res1comp.size());
  res1.forEach([](Array2S const&, int elem) { EXPECT_TRUE(elem == 1); });

  auto res2 = tileSectorArray.tileEachResult(RectI(32, 32, 64, 64),
      [](Vec2I const& pos, int tile) -> int {
        return (pos[0] >= 32 && pos[0] < 64 && pos[1] >= 32 && pos[1] < 64 && tile == 2) ? 1 : 0;
      });
  MultiArray<int, 2> res2comp({32, 32}, 1);

  EXPECT_TRUE(res2.size() == res2comp.size());
  res2.forEach([](Array2S const&, int elem) { EXPECT_TRUE(elem == 1); });

  auto res3 = tileSectorArray.tileEachResult(RectI(-10, -10, 1, 1),
      [](Vec2I const& pos, int tile) -> int {
        if (pos[1] < 0)
          return tile == -1;
        else
          return tile == 1;
      });
  MultiArray<int, 2> res3comp({11, 11}, 1);

  EXPECT_TRUE(res3.size() == res3comp.size());
  res3.forEach([](Array2S const&, int elem) { EXPECT_TRUE(elem == 1); });
}
