#include "StarAssets.hpp"
#include "StarCelestialDatabase.hpp"
#include "StarRoot.hpp"
#include "StarWorldClient.hpp"
#include "StarWorldTemplate.hpp"

#include "StarTestUniverse.hpp"
#include "gtest/gtest.h"

using namespace Star;

void validateWorld(TestUniverse& testUniverse) {
  testUniverse.update(100);

  // Just make sure the test world draws something for now, this will grow to
  // include more than this.
  EXPECT_GE(testUniverse.currentClientDrawables().size(), 1u) << strf("world: {}", testUniverse.currentPlayerWorld());

  auto assets = Root::singleton().assets();
  for (auto const& drawable : testUniverse.currentClientDrawables()) {
    if (drawable.isImage())
      assets->image(drawable.imagePart().image);
  }
}

TEST(SpawnTest, RandomCelestialWorld) {
  CelestialMasterDatabase celestialDatabase;
  Maybe<CelestialCoordinate> celestialWorld = celestialDatabase.findRandomWorld(10, 50, [&](CelestialCoordinate const& coord) {
      return celestialDatabase.parameters(coord)->isVisitable();
    });
  ASSERT_TRUE((bool)celestialWorld);

  TestUniverse testUniverse(Vec2U(100, 100));
  WorldId worldId = CelestialWorldId(*celestialWorld);
  testUniverse.warpPlayer(worldId);
  EXPECT_EQ(testUniverse.currentPlayerWorld(), worldId);
  validateWorld(testUniverse);
}

TEST(SpawnTest, RandomInstanceWorld) {
  auto& root = Root::singleton();
  StringList instanceWorlds = root.assets()->json("/instance_worlds.config").toObject().keys();
  ASSERT_GT(instanceWorlds.size(), 0u);
  WorldId instanceWorld = InstanceWorldId(Random::randFrom(instanceWorlds));

  TestUniverse testUniverse(Vec2U(100, 100));
  testUniverse.warpPlayer(instanceWorld);
  EXPECT_EQ(testUniverse.currentPlayerWorld(), instanceWorld);
  validateWorld(testUniverse);
}

TEST(SpawnTest, ClientCustomWorldRoundTrip) {
  TestUniverse testUniverse(Vec2U(100, 100));
  auto client = testUniverse.client();
  auto uuid = client->clientContext()->playerUuid();
  client->createCustomWorld("merge-test", WorldTemplate(Vec2U(64, 64)).store());
  WorldId customWorld = ClientCustomWorldId{uuid, "merge-test"};
  testUniverse.warpPlayer(customWorld);
  ASSERT_EQ(testUniverse.currentPlayerWorld(), customWorld);
  client->worldClient()->setProperty("mergeTest", 42);
  testUniverse.update(20);
  testUniverse.warpPlayer(ClientShipWorldId(uuid));
  testUniverse.warpPlayer(customWorld);
  EXPECT_EQ(testUniverse.currentPlayerWorld(), customWorld);
  EXPECT_EQ(client->worldClient()->getProperty("mergeTest"), Json(42));
}
