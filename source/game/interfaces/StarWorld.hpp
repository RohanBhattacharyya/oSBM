#pragma once

#include "StarTileEntity.hpp"
#include "StarInteractionTypes.hpp"
#include "StarCollisionBlock.hpp"
#include "StarForceRegions.hpp"
#include "StarWorldGeometry.hpp"
#include "StarTileModification.hpp"
#include "StarLuaRoot.hpp"
#include "StarRpcPromise.hpp"

namespace Star {

STAR_CLASS(World);
STAR_CLASS(TileEntity);
STAR_CLASS(ScriptedEntity);

typedef function<void(World*)> WorldAction;

// Fixed-size history of recently changed collision regions, used to validate
// memoized collision queries (see World::collisionRegionsChangedSince).
// Regions crossing the world wrap seam are recorded as global changes.
class CollisionChangeTracker {
public:
  void record(RectI const& region, unsigned worldWidth) {
    ++m_epoch;
    RectI stored = region;
    if (region.isNull() || region.xMin() < 0 || region.xMax() > (int)worldWidth)
      stored = RectI(); // treated as intersecting everything
    m_ring[m_epoch % RingSize] = {m_epoch, stored};
  }
  uint64_t epoch() const {
    return m_epoch;
  }
  bool changedSince(uint64_t epoch, RectI const& region) const {
    if (epoch > m_epoch)
      return true;
    uint64_t span = m_epoch - epoch;
    if (span == 0)
      return false;
    if (span > RingSize)
      return true;
    for (uint64_t e = epoch + 1; e <= m_epoch; ++e) {
      auto const& entry = m_ring[e % RingSize];
      if (entry.first != e || entry.second.isEmpty() || entry.second.intersects(region))
        return true;
    }
    return false;
  }

private:
  static size_t const RingSize = 64;
  uint64_t m_epoch = 0;
  pair<uint64_t, RectI> m_ring[RingSize];
};

class World {
public:
  virtual ~World() {}

  // Will remain constant throughout the life of the world.
  virtual ConnectionId connection() const = 0;
  virtual WorldGeometry geometry() const = 0;

  // Update frame counter.  Returns the frame that is *currently* being
  // updated, not the *last* frame, so during the first call to update(), this
  // would return 1
  virtual uint64_t currentStep() const = 0;

  // All methods that take int parameters wrap around or clamp so that all int
  // values are valid world indexes.

  virtual MaterialId material(Vec2I const& position, TileLayer layer) const = 0;
  virtual MaterialHue materialHueShift(Vec2I const& position, TileLayer layer) const = 0;
  virtual ModId mod(Vec2I const& position, TileLayer layer) const = 0;
  virtual MaterialHue modHueShift(Vec2I const& position, TileLayer layer) const = 0;
  virtual MaterialColorVariant colorVariant(Vec2I const& position, TileLayer layer) const = 0;
  virtual LiquidLevel liquidLevel(Vec2I const& pos) const = 0;
  virtual LiquidLevel liquidLevel(RectF const& region) const = 0;

  // Tests a tile modification list and returns the ones that are valid.
  virtual TileModificationList validTileModifications(TileModificationList const& modificationList, bool allowEntityOverlap) const = 0;
  // Apply a list of tile modifications in the best order to apply as many
  // possible, and returns the modifications that could not be applied.
  virtual TileModificationList applyTileModifications(TileModificationList const& modificationList, bool allowEntityOverlap) = 0;
  // Swap existing tiles for ones defined in the modification list,
  // and returns the modifications that could not be applied.
  virtual TileModificationList replaceTiles(TileModificationList const& modificationList, TileDamage const& tileDamage, bool applyDamage = false) = 0;
  // If an applied damage would destroy a tile
  virtual bool damageWouldDestroy(Vec2I const& pos, TileLayer layer, TileDamage const& tileDamage) const = 0;

  virtual bool isTileProtected(Vec2I const& pos) const = 0;

  virtual EntityPtr entity(EntityId entityId) const = 0;
  // *If* the entity is initialized immediately and locally, then will use the
  // passed in pointer directly and initialize it, and entity will have a valid
  // id in this world and be ready for use.  This is always the case on the
  // server, but not *always* the case on the client.
  virtual void addEntity(EntityPtr const& entity, EntityId entityId = NullEntityId) = 0;

  virtual EntityPtr closestEntity(Vec2F const& center, float radius, EntityFilter selector = {}) const = 0;

  virtual void forAllEntities(EntityCallback entityCallback) const = 0;

  // Query here is a fuzzy query based on metaBoundBox
  virtual void forEachEntity(RectF const& boundBox, EntityCallback entityCallback) const = 0;
  // Fuzzy metaBoundBox query for intersecting the given line.
  virtual void forEachEntityLine(Vec2F const& begin, Vec2F const& end, EntityCallback entityCallback) const = 0;
  // Performs action for all entities that occupies the given tile position
  // (only entity types laid out in the tile grid).
  virtual void forEachEntityAtTile(Vec2I const& pos, EntityCallbackOf<TileEntity> entityCallback) const = 0;

  // Like forEachEntity, but stops scanning when entityFilter returns true, and
  // returns the EntityPtr found, otherwise returns a null pointer.
  virtual EntityPtr findEntity(RectF const& boundBox, EntityFilter entityFilter) const = 0;
  virtual EntityPtr findEntityLine(Vec2F const& begin, Vec2F const& end, EntityFilter entityFilter) const = 0;
  virtual EntityPtr findEntityAtTile(Vec2I const& pos, EntityFilterOf<TileEntity> entityFilter) const = 0;

  // Is the given tile layer and position occupied by an entity or block?
  virtual bool tileIsOccupied(Vec2I const& pos, TileLayer layer, bool includeEphemeral = false, bool checkCollision = false) const = 0;

  // Returns the collision kind of a tile.
  virtual CollisionKind tileCollisionKind(Vec2I const& pos) const = 0;

  // Iterate over the collision block for each tile in the region.  Collision
  // polys for tiles can extend to a maximum of 1 tile outside of the natural
  // tile bounds.
  virtual void forEachCollisionBlock(RectI const& region, function<void(CollisionBlock const&)> const& iterator) const = 0;

  // True if any entity in the world implements PhysicsEntity (moving
  // collisions / force regions).  Almost always false; lets the movement hot
  // path skip its per-tick physics-entity spatial queries.
  virtual bool hasPhysicsEntities() const = 0;

  // Batch variant of forEachCollisionBlock for the movement hot path: appends
  // pointers to the (freshened) per-tile cached collision blocks in the region
  // in a single call, avoiding a per-tile indirect callback.  Null-collision
  // placeholder blocks are omitted.  The pointers alias the world's internal
  // tile collision cache and are only valid until the next tile modification
  // or collision query, so consume them immediately.  Returns true if any
  // tile in the region had a dirty collision cache (i.e. the region's
  // collision geometry changed since it was last queried by anyone).
  virtual bool getTileCollisionBlocks(RectI const& region, List<CollisionBlock const*>& output) const = 0;

  // Monotonic counter incremented whenever any collision geometry changes
  // anywhere (tile modification, generation, sector load, freshen rebuild).
  virtual uint64_t collisionChangeEpoch() const = 0;

  // Returns true if any collision-geometry change recorded after `epoch`
  // intersects `region` (conservatively true when the change history since
  // `epoch` is no longer fully retained).  Lets movement controllers reuse
  // their previous collision query results unless something changed nearby.
  virtual bool collisionRegionsChangedSince(uint64_t epoch, RectI const& region) const = 0;

  // Is there some connectable tile / tile based entity in this position?  If
  // tilesOnly is true, only checks to see whether that tile is a connectable
  // material.
  virtual bool isTileConnectable(Vec2I const& pos, TileLayer layer, bool tilesOnly = false) const = 0;

  // Returns whether or not a given point is inside any colliding tile.  If
  // collisionSet is Dynamic or Static, then does not intersect with platforms.
  virtual bool pointTileCollision(Vec2F const& point, CollisionSet const& collisionSet = DefaultCollisionSet) const = 0;

  // Returns whether line intersects with any colliding tiles.
  virtual bool lineTileCollision(Vec2F const& begin, Vec2F const& end, CollisionSet const& collisionSet = DefaultCollisionSet) const = 0;
  virtual Maybe<pair<Vec2F, Vec2I>> lineTileCollisionPoint(Vec2F const& begin, Vec2F const& end, CollisionSet const& collisionSet = DefaultCollisionSet) const = 0;

  // Returns a list of all the collidable tiles along the given line.
  virtual List<Vec2I> collidingTilesAlongLine(Vec2F const& begin, Vec2F const& end, CollisionSet const& collisionSet = DefaultCollisionSet, int maxSize = -1, bool includeEdges = true) const = 0;

  // Returns whether the given rect contains any colliding tiles.
  virtual bool rectTileCollision(RectI const& region, CollisionSet const& collisionSet = DefaultCollisionSet) const = 0;

  // Damage multiple tiles, avoiding duplication (objects or plants that occupy
  // more than one tile
  // position are only damaged once)
  virtual TileDamageResult damageTiles(List<Vec2I> const& tilePositions, TileLayer layer, Vec2F const& sourcePosition, TileDamage const& tileDamage, Maybe<EntityId> sourceEntity = {}) = 0;

  virtual InteractiveEntityPtr getInteractiveInRange(Vec2F const& targetPosition, Vec2F const& sourcePosition, float maxRange) const = 0;
  // Can the target entity be reached from the given position within the given radius?
  virtual bool canReachEntity(Vec2F const& position, float radius, EntityId targetEntity, bool preferInteractive = true) const = 0;
  virtual RpcPromise<InteractAction> interact(InteractRequest const& request) = 0;

  virtual float gravity(Vec2F const& pos) const = 0;
  virtual float windLevel(Vec2F const& pos) const = 0;
  virtual float lightLevel(Vec2F const& pos) const = 0;
  virtual bool breathable(Vec2F const& pos) const = 0;
  virtual float threatLevel() const = 0;
  virtual StringList environmentStatusEffects(Vec2F const& pos) const = 0;
  virtual StringList weatherStatusEffects(Vec2F const& pos) const = 0;
  virtual bool exposedToWeather(Vec2F const& pos) const = 0;
  virtual bool isUnderground(Vec2F const& pos) const = 0;
  virtual bool disableDeathDrops() const = 0;
  virtual List<PhysicsForceRegion> forceRegions() const = 0;

  // Gets / sets world-wide properties
  virtual Json getProperty(String const& propertyName, Json const& def = {}) const = 0;
  virtual void setProperty(String const& propertyName, Json const& property) = 0;

  virtual void timer(float delay, WorldAction worldAction) = 0;
  virtual double epochTime() const = 0;
  virtual uint32_t day() const = 0;
  virtual float dayLength() const = 0;
  virtual float timeOfDay() const = 0;

  virtual LuaRootPtr luaRoot() = 0;

  // Locate a unique entity, if the target is local, the promise will be
  // finished before being returned.  If the unique entity is not found, the
  // promise will fail.
  virtual RpcPromise<Vec2F> findUniqueEntity(String const& uniqueEntityId) = 0;

  // Send a message to a local or remote scripted entity.  If the target is
  // local, the promise will be finished before being returned.  Entity id can
  // either be EntityId or a uniqueId.
  virtual RpcPromise<Json> sendEntityMessage(Variant<EntityId, String> const& entity, String const& message, JsonArray const& args = {}) = 0;

  // Helper non-virtual methods.

  bool isServer() const;
  bool isClient() const;

  List<EntityPtr> entityQuery(RectF const& boundBox, EntityFilter selector = {}) const;
  List<EntityPtr> entityLineQuery(Vec2F const& begin, Vec2F const& end, EntityFilter selector = {}) const;

  List<TileEntityPtr> entitiesAtTile(Vec2I const& pos, EntityFilter filter = EntityFilter()) const;

  // Find tiles near the given point that are not occupied (according to
  // tileIsOccupied)
  List<Vec2I> findEmptyTiles(Vec2I pos, unsigned maxDist = 5, size_t maxAmount = 1, bool excludeEphemeral = false) const;

  // Do tile modification that only uses a single tile.
  bool canModifyTile(Vec2I const& pos, TileModification const& modification, bool allowEntityOverlap) const;
  bool modifyTile(Vec2I const& pos, TileModification const& modification, bool allowEntityOverlap);

  TileDamageResult damageTile(Vec2I const& tilePosition, TileLayer layer, Vec2F const& sourcePosition, TileDamage const& tileDamage, Maybe<EntityId> sourceEntity = {});

  // Returns closest entity for which lineCollision between the given center
  // position and the entity position returns false.
  EntityPtr closestEntityInSight(Vec2F const& center, float radius, CollisionSet const& collisionSet = DefaultCollisionSet, EntityFilter selector = {}) const;

  // Returns whether point collides with any collision geometry.
  bool pointCollision(Vec2F const& point, CollisionSet const& collisionSet = DefaultCollisionSet) const;

  // Returns first point along line that collides with any collision geometry, along
  // with the normal of the intersected line, if any.
  Maybe<pair<Vec2F, Maybe<Vec2F>>> lineCollision(Line2F const& line, CollisionSet const& collisionSet = DefaultCollisionSet) const;

  // Returns whether poly collides with any collision geometry.
  bool polyCollision(PolyF const& poly, CollisionSet const& collisionSet = DefaultCollisionSet) const;

  // Helper template methods.  Only queries entities of the given template
  // type, and casts them to the appropriate pointer type.

  template <typename EntityT>
  shared_ptr<EntityT> get(EntityId entityId) const;

  template <typename EntityT>
  List<shared_ptr<EntityT>> query(RectF const& boundBox, EntityFilterOf<EntityT> selector = {}) const;

  template <typename EntityT>
  shared_ptr<EntityT> closest(Vec2F const& center, float radius, EntityFilterOf<EntityT> selector = {}) const;

  template <typename EntityT>
  shared_ptr<EntityT> closestInSight(Vec2F const& center, float radius, CollisionSet const& collisionSet, EntityFilterOf<EntityT> selector = {}) const;

  template <typename EntityT>
  List<shared_ptr<EntityT>> lineQuery(Vec2F const& begin, Vec2F const& end, EntityFilterOf<EntityT> selector = {}) const;

  template <typename EntityT>
  List<shared_ptr<EntityT>> atTile(Vec2I const& pos) const;
};

template <typename EntityT>
shared_ptr<EntityT> World::get(EntityId entityId) const {
  return as<EntityT>(entity(entityId));
}

template <typename EntityT>
List<shared_ptr<EntityT>> World::query(RectF const& boundBox, EntityFilterOf<EntityT> selector) const {
  List<shared_ptr<EntityT>> list;
  forEachEntity(boundBox, [&](EntityPtr const& entity) {
      if (auto e = as<EntityT>(entity)) {
        if (!selector || selector(e))
          list.append(std::move(e));
      }
    });

  return list;
}

template <typename EntityT>
shared_ptr<EntityT> World::closest(Vec2F const& center, float radius, EntityFilterOf<EntityT> selector) const {
  return as<EntityT>(closestEntity(center, radius, entityTypeFilter<EntityT>(selector)));
}

template <typename EntityT>
shared_ptr<EntityT> World::closestInSight(
    Vec2F const& center, float radius, CollisionSet const& collisionSet, EntityFilterOf<EntityT> selector) const {
  return as<EntityT>(closestEntityInSight(center, radius, collisionSet, entityTypeFilter<EntityT>(selector)));
}

template <typename EntityT>
List<shared_ptr<EntityT>> World::lineQuery(
    Vec2F const& begin, Vec2F const& end, EntityFilterOf<EntityT> selector) const {
  List<shared_ptr<EntityT>> list;
  forEachEntityLine(begin, end, [&](EntityPtr entity) {
      if (auto e = as<EntityT>(std::move(entity))) {
        if (!selector || selector(e))
          list.append(std::move(e));
      }
    });

  return list;
}

template <typename EntityT>
List<shared_ptr<EntityT>> World::atTile(Vec2I const& pos) const {
  List<shared_ptr<EntityT>> list;
  forEachEntityAtTile(pos, [&](TileEntityPtr const& entity) {
      if (auto e = as<EntityT>(entity))
        list.append(std::move(e));
    });
  return list;
}
}
