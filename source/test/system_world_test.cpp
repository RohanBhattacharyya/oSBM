#include "gtest/gtest.h"
#include "StarCelestialDatabase.hpp"
#include "StarSystemWorld.hpp"
#include "StarTime.hpp"

using namespace Star;

namespace {

// Exercise the same delayed-data boundary as the navigation client's slave
// database, using real generated systems from the user's assets.
class PreviewDatabase : public CelestialMasterDatabase {
public:
  Maybe<Vec3I> hiddenSystem;
  unsigned lookups = 0;

  Maybe<CelestialParameters> parameters(CelestialCoordinate const& coord) override {
    ++lookups;
    if (hiddenSystem && coord.location() == *hiddenSystem)
      return {};
    return CelestialMasterDatabase::parameters(coord);
  }

  List<int> childOrbits(CelestialCoordinate const& coord) override {
    ++lookups;
    if (hiddenSystem && coord.location() == *hiddenSystem)
      return {};
    return CelestialMasterDatabase::childOrbits(coord);
  }

  Maybe<bool> hasChildren(CelestialCoordinate const& coord) override {
    ++lookups;
    if (hiddenSystem && coord.location() == *hiddenSystem)
      return {};
    return CelestialMasterDatabase::hasChildren(coord);
  }
};

class PreviewWorld : public SystemWorld {
public:
  PreviewWorld(ClockConstPtr clock, CelestialDatabasePtr database, Vec3I location)
    : SystemWorld(std::move(clock), std::move(database)) { m_location = location; }
  List<SystemObjectPtr> objects() const override { return {}; }
  List<Uuid> objectKeys() const override { return {}; }
  SystemObjectPtr getObject(Uuid const&) const override { return {}; }
};

}

TEST(SystemWorldPreview, VisitedSystemsDoNotShareOrbitGeometry) {
  auto database = make_shared<PreviewDatabase>();
  auto first = database->findRandomWorld(100, 50, {}, 42);
  auto second = database->findRandomWorld(100, 50, {}, 31415);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_NE(first->location(), second->location());
  auto clock = make_shared<Clock>(false);
  PreviewWorld preview(clock, database, first->location());
  PreviewWorld fresh(clock, database, second->location());

  // Warm a current-system cache, then inspect matching orbit numbers in a
  // different visited system. Repeat across planets and moons.
  for (int orbit = 1; orbit <= 5; ++orbit) {
    auto a = first->system().child(orbit);
    preview.planetOrbitDistance(a);
    preview.planetSize(a);
    preview.clusterSize(a);
  }
  auto planets = database->children(second->system());
  ASSERT_FALSE(planets.empty());
  unsigned bodiesChecked = 0;
  for (auto const& planet : planets) {
    auto bodies = database->children(planet);
    bodies.append(planet);
    for (auto const& body : bodies) {
      ++bodiesChecked;
      EXPECT_FLOAT_EQ(fresh.planetOrbitDistance(body), preview.planetOrbitDistance(body));
      EXPECT_FLOAT_EQ(fresh.planetSize(body), preview.planetSize(body));
      EXPECT_EQ(fresh.planetPosition(body), preview.planetPosition(body));
      // A warm navigation frame must reuse geometry, not revisit the DB.
      auto lookups = database->lookups;
      for (unsigned frame = 0; frame < 100; ++frame)
        preview.planetPosition(body);
      EXPECT_EQ(lookups, database->lookups);
    }
  }
  EXPECT_GT(bodiesChecked, 0u);

  // An unavailable preview must not freeze placeholder geometry permanently.
  PreviewWorld delayed(clock, database, first->location());
  database->hiddenSystem = second->location();
  delayed.planetOrbitDistance(*second);
  delayed.planetSize(*second);
  database->hiddenSystem = {};
  EXPECT_FLOAT_EQ(fresh.planetOrbitDistance(*second), delayed.planetOrbitDistance(*second));
  EXPECT_FLOAT_EQ(fresh.planetSize(*second), delayed.planetSize(*second));
}

TEST(SystemWorldPreview, StarListDoesNotFreezeMissingPlanetGeometry) {
  auto master = make_shared<CelestialMasterDatabase>();
  auto first = master->findRandomWorld(100, 50, {}, 42);
  auto second = master->findRandomWorld(100, 50, {}, 31415);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  auto slave = make_shared<CelestialSlaveDatabase>(master->baseInformation());
  auto clock = make_shared<Clock>(false);
  PreviewWorld client(clock, slave, first->location());
  PreviewWorld server(clock, master, first->location());

  for (auto const& system : {first->system(), second->system()}) {
    // A star-map chunk deliberately omits planets/moons. The separate
    // system response may arrive several navigation frames later.
    slave->signalSystem(system);
    auto requests = slave->pullRequests();
    ASSERT_FALSE(requests.empty());
    for (auto const& request : requests) {
      if (request.isLeft())
        slave->pushResponses({master->respondToRequest(request)});
    }
    ASSERT_TRUE(slave->parameters(system));
    EXPECT_FALSE(slave->hasChildren(system));

    auto planets = master->children(system);
    ASSERT_FALSE(planets.empty());
    for (auto const& planet : planets) {
      auto bodies = master->children(planet);
      bodies.append(planet);
      for (auto const& body : bodies) {
        client.planetSize(body);
        client.clusterSize(body);
        client.planetOrbitDistance(body);
        client.planetPosition(body);
      }
    }

    slave->pushResponses({master->respondToRequest(makeRight(system.location()))});
    ASSERT_TRUE(slave->hasChildren(system));
    for (auto const& planet : planets) {
      auto bodies = master->children(planet);
      bodies.append(planet);
      for (auto const& body : bodies) {
        SCOPED_TRACE(body.toJson().repr());
        EXPECT_FLOAT_EQ(server.planetSize(body), client.planetSize(body));
        EXPECT_FLOAT_EQ(server.clusterSize(body), client.clusterSize(body));
        EXPECT_FLOAT_EQ(server.planetOrbitDistance(body), client.planetOrbitDistance(body));
        EXPECT_EQ(server.planetPosition(body), client.planetPosition(body));
        // Ships and objects orbit server positions. Their markers must use
        // the same geometry, including as the universe clock advances.
        CelestialOrbit orbit{body, 1, 0, {3, 4}};
        for (double time : {0.0, 100.0, 10000.0}) {
          clock->setTime(time);
          EXPECT_EQ(server.orbitPosition(orbit), client.orbitPosition(orbit));
        }
      }
    }
  }
}
