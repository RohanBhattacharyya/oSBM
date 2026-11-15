#include "StarTestUniverse.hpp"
#include "StarFile.hpp"
#include "StarQuests.hpp"
#include "StarPlayerFactory.hpp"
#include "StarPlayerStorage.hpp"
#include "StarStatistics.hpp"
#include "StarStatisticsService.hpp"
#include "StarPlayer.hpp"
#include "StarAssets.hpp"
#include "StarWorldClient.hpp"

namespace Star {

TestUniverse::TestUniverse(Vec2U clientWindowSize) {
  auto& root = Root::singleton();

  m_clientWindowSize = clientWindowSize;

  m_storagePath = File::temporaryDirectory();
  auto playerStorage = make_shared<PlayerStorage>(File::relativeTo(m_storagePath, "player"));
  auto statistics = make_shared<Statistics>(File::relativeTo(m_storagePath, "statistics"));
  m_server = make_shared<UniverseServer>(File::relativeTo(m_storagePath, "universe"));
  m_client = make_shared<UniverseClient>(playerStorage, statistics, File::relativeTo(m_storagePath, "universeclient"));

  m_server->start();

  m_mainPlayer = root.playerFactory()->create();
  m_mainPlayer->setSpecies("human");
  m_mainPlayer->finalizeCreation();
  m_mainPlayer->setAdmin(true);
  m_mainPlayer->setModeType(PlayerMode::Survival);
  m_client->setMainPlayer(m_mainPlayer);
  if (auto error = m_client->connect(m_server->addLocalClient(), true, "test", ""))
    throw StarException(strf("Test client could not connect: {}", *error));
}

TestUniverse::~TestUniverse() {
  m_client = {};
  m_server = {};
  m_mainPlayer = {};
  File::removeDirectoryRecursive(m_storagePath);
}

void TestUniverse::warpPlayer(WorldId worldId) {
  m_client->warpPlayer(WarpToWorld(worldId), true);
  auto deadline = Time::monotonicMilliseconds() + 60000;
  while (m_mainPlayer->isTeleporting() || m_client->playerWorld().empty()) {
    if (!m_client->isConnected() || Time::monotonicMilliseconds() > deadline)
      throw StarException(strf("Test warp to {} did not complete", printWorldId(worldId)));
    m_client->update(0.016f);
    Thread::sleep(16);
  }
}

WorldId TestUniverse::currentPlayerWorld() const {
  return m_client->clientContext()->playerWorldId();
}

void TestUniverse::update(unsigned times) {
  for (unsigned i = 0; i < times; ++i) {
    m_client->update(0.016f);
    Thread::sleep(16);
  }
}

List<Drawable> TestUniverse::currentClientDrawables() {
  WorldRenderData renderData;
  auto worldClient = m_client->worldClient();
  worldClient->centerClientWindowOnPlayer(m_clientWindowSize);
  worldClient->render(renderData, 0);

  List<Drawable> drawables;
  for (auto& ed : renderData.entityDrawables) {
    for (auto& p : ed->layers)
      drawables.appendAll(p.second);
  }

  return drawables;
}

}
