#pragma once

#include "StarImage.hpp"
#include "StarWorldTiles.hpp"
#include "StarEntityRenderingTypes.hpp"
#include "StarSkyRenderData.hpp"
#include "StarParallax.hpp"
#include "StarParticle.hpp"
#include "StarWeatherTypes.hpp"
#include "StarEntity.hpp"
#include "StarThread.hpp"
#include "StarCellularLighting.hpp"

namespace Star {

struct EntityDrawables {
  EntityHighlightEffect highlightEffect;
  // HashMap, not Map: constructed fresh per entity per frame and typically
  // holds only 1-5 distinct render layers -- a std::map pays a heap-allocated
  // tree node per distinct key on every entity every frame; a flat hash map
  // needs at most one backing allocation total.
  HashMap<EntityRenderLayer, List<Drawable>> layers;
};


struct WorldRenderData {
  void clear();

  WorldGeometry geometry;

  Vec2I tileMinPosition;
  RenderTileArray tiles;
  Vec2I lightMinPosition;
  Lightmap lightMap;

  // Shared, not by-value: entries are usually built fresh per frame, but
  // WorldClient's under-load entity render throttle re-appends the SAME
  // cached entry across frames for static entities -- a refcount bump instead
  // of a deep copy of every Drawable (whose AssetPath strings/directives make
  // copies genuinely expensive at ~100 entities/frame). Consumers must treat
  // entries as immutable.
  List<shared_ptr<EntityDrawables const>> entityDrawables;
  List<Particle> const* particles;

  List<OverheadBar> overheadBars;
  List<Drawable> nametags;

  List<Drawable> backgroundOverlays;
  List<Drawable> foregroundOverlays;

  // Points at WorldClient's cached parallax-layer buffer (rebuilt only when
  // the parallax set or fade state changes, not every frame). Null when no
  // parallax. Valid for the frame it was produced.
  ParallaxLayers const* parallaxLayers = nullptr;

  SkyRenderData skyRenderData;

  bool isFullbright = false;
  float dimLevel = 0.0f;
  Vec3B dimColor;
};

inline void WorldRenderData::clear() {
  tiles.resize({0, 0}); // keep reserved

  entityDrawables.clear();
  particles = nullptr;
  overheadBars.clear();
  nametags.clear();
  backgroundOverlays.clear();
  foregroundOverlays.clear();
  parallaxLayers = nullptr;
}

}
