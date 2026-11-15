#include "StarUniverseServer.hpp"
#include "StarRoot.hpp"
#include "StarFile.hpp"
#include "StarWorldClient.hpp"
#include "StarWorldServerThread.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(ServerTest, Run) {
  UniverseServer server(Root::singleton().toStoragePath("universe"));
  server.start();
  server.stop();
  server.join();
}

TEST(ServerTest, ClientCustomWorldSurvivesThreadShutdown) {
  auto world = make_shared<WorldServer>(Vec2U(64, 64), File::ephemeralFile());
  world->setProperty("mergeTest", JsonObject{{"value", 42}});
  WorldServerThread thread(world, ClientCustomWorldId{Uuid(), "merge-test"});
  world.reset();
  thread.start();
  thread.stop();
  ASSERT_FALSE(thread.serverErrorOccurred()) << thread.lastError();
  auto snapshot = thread.takeFinalChunks();
  ASSERT_FALSE(snapshot.empty());
  WorldServer restored(snapshot);
  EXPECT_EQ(restored.getProperty("mergeTest"), (JsonObject{{"value", 42}}));
}

TEST(ServerTest, HeadlessClientLifecycle) {
  auto before = worldClientLiveCount().load();
  {
    WorldClient client(ClientSubWorldId(1));
    EXPECT_EQ(worldClientLiveCount().load(), before + 1);
    EXPECT_TRUE(client.isHeadless());
    EXPECT_FALSE(client.inWorld());
    client.update(0.016f);
  }
  EXPECT_EQ(worldClientLiveCount().load(), before);
}

TEST(ServerTest, RemovesSubWorldAfterThreadShutdown) {
  auto world = make_shared<WorldServer>(Vec2U(64, 64), File::ephemeralFile());
  WorldServerThread thread(world, ClientCustomWorldId{Uuid(), "merge-test"});
  world.reset();
  auto connection = MinClientSubWorldConnectionId;
  ClientSubWorldId subWorld = 7;
  ASSERT_TRUE(thread.addClient(connection, SpawnTargetPosition(Vec2F(32, 32)), true, true, {}, subWorld));
  thread.start();
  thread.stop();
  ASSERT_FALSE(thread.serverErrorOccurred()) << thread.lastError();
  EXPECT_EQ(thread.clientSubWorld(connection), subWorld);
  EXPECT_TRUE(thread.erroredClients().empty());
  EXPECT_FALSE(thread.playerRevivePosition(connection));
  auto packets = thread.removeClient(connection);
  ASSERT_FALSE(packets.empty());
  auto stopped = as<ClientSubWorldPackets>(packets.last());
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped->subWorldId, subWorld);
  ASSERT_FALSE(stopped->packets.empty());
  EXPECT_TRUE(as<WorldStopPacket>(stopped->packets.last()));
  EXPECT_TRUE(thread.noClients());
}
