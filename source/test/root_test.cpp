#include "StarRoot.hpp"
#include "StarLuaGameConverters.hpp"
#include "StarRootLuaBindings.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(RootTest, MaterialVariantCompatibility) {
  // Golden results from upstream's XXH32 streaming algorithm. Preserve tile
  // appearances when replacing its per-thread heap state with stack state.
  struct Sample {
    Vec2U position;
    String layer;
    unsigned material;
    unsigned mod;
  };
  Sample samples[] = {
    {{0, 0}, "Background", 87, 46},
    {{0, 0}, "Platform", 23, 95},
    {{0, 0}, "Foreground", 72, 95},
    {{1, 2}, "Background", 0, 73},
    {{1, 2}, "Platform", 88, 49},
    {{1, 2}, "Foreground", 82, 56},
    {{4032, 2071}, "Background", 65, 36},
    {{4032, 2071}, "Platform", 52, 81},
    {{4032, 2071}, "Foreground", 34, 12},
    {{0xffffffffu, 1024}, "Background", 49, 12},
    {{0xffffffffu, 1024}, "Platform", 41, 45},
    {{0xffffffffu, 1024}, "Foreground", 88, 34}
  };
  auto engine = LuaEngine::create();
  auto context = engine->createContext();
  context.setCallbacks("root", LuaBindings::makeRootCallbacks());
  for (auto const& sample : samples) {
    EXPECT_EQ(context.invokePath<unsigned>("root.materialVariant", sample.position, sample.layer, 97u), sample.material);
    EXPECT_EQ(context.invokePath<unsigned>("root.modVariant", sample.position, sample.layer, 97u), sample.mod);
    EXPECT_EQ(context.invokePath<unsigned>("root.materialVariant", sample.position, sample.layer, 1u), 0u);
  }
}

TEST(RootTest, All) {
  auto root = Root::singletonPtr();
  EXPECT_TRUE(root);

  EXPECT_TRUE((bool)root->assets());
  EXPECT_TRUE((bool)root->objectDatabase());
  EXPECT_TRUE((bool)root->plantDatabase());
  EXPECT_TRUE((bool)root->projectileDatabase());
  EXPECT_TRUE((bool)root->monsterDatabase());
  EXPECT_TRUE((bool)root->npcDatabase());
  EXPECT_TRUE((bool)root->playerFactory());
  EXPECT_TRUE((bool)root->entityFactory());
  EXPECT_TRUE((bool)root->nameGenerator());
  EXPECT_TRUE((bool)root->itemDatabase());
  EXPECT_TRUE((bool)root->materialDatabase());
  EXPECT_TRUE((bool)root->terrainDatabase());
  EXPECT_TRUE((bool)root->biomeDatabase());
  EXPECT_TRUE((bool)root->liquidsDatabase());
  EXPECT_TRUE((bool)root->statusEffectDatabase());
  EXPECT_TRUE((bool)root->damageDatabase());
  EXPECT_TRUE((bool)root->particleDatabase());
  EXPECT_TRUE((bool)root->effectSourceDatabase());
  EXPECT_TRUE((bool)root->functionDatabase());
  EXPECT_TRUE((bool)root->treasureDatabase());
  EXPECT_TRUE((bool)root->dungeonDefinitions());
  EXPECT_TRUE((bool)root->emoteProcessor());
  EXPECT_TRUE((bool)root->speciesDatabase());
  EXPECT_TRUE((bool)root->imageMetadataDatabase());
  EXPECT_TRUE((bool)root->versioningDatabase());
  EXPECT_TRUE((bool)root->questTemplateDatabase());
  EXPECT_TRUE((bool)root->aiDatabase());
  EXPECT_TRUE((bool)root->techDatabase());
  EXPECT_TRUE((bool)root->codexDatabase());
  EXPECT_TRUE((bool)root->stagehandDatabase());
  EXPECT_TRUE((bool)root->behaviorDatabase());
  EXPECT_TRUE((bool)root->tenantDatabase());
  EXPECT_TRUE((bool)root->danceDatabase());
  EXPECT_TRUE((bool)root->spawnTypeDatabase());

  root->reload();

  EXPECT_TRUE((bool)root->assets());
  EXPECT_TRUE((bool)root->objectDatabase());
  EXPECT_TRUE((bool)root->plantDatabase());
  EXPECT_TRUE((bool)root->projectileDatabase());
  EXPECT_TRUE((bool)root->monsterDatabase());
  EXPECT_TRUE((bool)root->npcDatabase());
  EXPECT_TRUE((bool)root->playerFactory());
  EXPECT_TRUE((bool)root->entityFactory());
  EXPECT_TRUE((bool)root->nameGenerator());
  EXPECT_TRUE((bool)root->itemDatabase());
  EXPECT_TRUE((bool)root->materialDatabase());
  EXPECT_TRUE((bool)root->terrainDatabase());
  EXPECT_TRUE((bool)root->biomeDatabase());
  EXPECT_TRUE((bool)root->liquidsDatabase());
  EXPECT_TRUE((bool)root->statusEffectDatabase());
  EXPECT_TRUE((bool)root->damageDatabase());
  EXPECT_TRUE((bool)root->particleDatabase());
  EXPECT_TRUE((bool)root->effectSourceDatabase());
  EXPECT_TRUE((bool)root->functionDatabase());
  EXPECT_TRUE((bool)root->treasureDatabase());
  EXPECT_TRUE((bool)root->dungeonDefinitions());
  EXPECT_TRUE((bool)root->emoteProcessor());
  EXPECT_TRUE((bool)root->speciesDatabase());
  EXPECT_TRUE((bool)root->imageMetadataDatabase());
  EXPECT_TRUE((bool)root->versioningDatabase());
  EXPECT_TRUE((bool)root->questTemplateDatabase());
  EXPECT_TRUE((bool)root->aiDatabase());
  EXPECT_TRUE((bool)root->techDatabase());
  EXPECT_TRUE((bool)root->codexDatabase());
  EXPECT_TRUE((bool)root->stagehandDatabase());
  EXPECT_TRUE((bool)root->behaviorDatabase());
  EXPECT_TRUE((bool)root->tenantDatabase());
  EXPECT_TRUE((bool)root->danceDatabase());
  EXPECT_TRUE((bool)root->spawnTypeDatabase());
}
