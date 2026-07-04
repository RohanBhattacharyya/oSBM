#pragma once

#include "StarWorldRenderData.hpp"
#include "StarTilePainter.hpp"
#include "StarEnvironmentPainter.hpp"
#include "StarTextPainter.hpp"
#include "StarDrawablePainter.hpp"
#include "StarRenderer.hpp"

namespace Star {

STAR_CLASS(WorldPainter);

// Will update client rendering window internally
class WorldPainter {
public:
  WorldPainter();

  void renderInit(RendererPtr renderer);

  void setCameraPosition(WorldGeometry const& worldGeometry, Vec2F const& position);

  WorldCamera& camera();

  void update(float dt);
  void render(WorldRenderData& renderData, function<bool()> lightWaiter);
  void adjustLighting(WorldRenderData& renderData);

private:
  void renderParticles(WorldRenderData& renderData, Particle::Layer layer);
  void renderBars(WorldRenderData& renderData);

  void drawEntityLayer(List<Drawable> const& drawables, EntityHighlightEffect highlightEffect = EntityHighlightEffect());

  // skipOnScreenCheck: entity drawables are already culled per-entity (a
  // spatial query padded a few tiles past the screen), so their per-drawable
  // on-screen test -- whose boundBox costs an image-metadata lookup per
  // drawable per frame -- is nearly pure waste; the GPU clips the rare
  // off-screen quad far cheaper than the CPU can test for it.
  void drawDrawable(Drawable const& drawable, bool skipOnScreenCheck = false);
  void drawDrawableSet(List<Drawable>& drawable);

  WorldCamera m_camera;

  // Under-load background-buffer refresh skipping state (see render()).
  int64_t m_bgLastRenderTimeUs = 0;
  int64_t m_bgFrameCounter = 0;
  Vec2U m_bgValidSize;

  RendererPtr m_renderer;

  TextPainterPtr m_textPainter;
  DrawablePainterPtr m_drawablePainter;
  EnvironmentPainterPtr m_environmentPainter;
  TilePainterPtr m_tilePainter;

  Json m_highlightConfig;
  Map<EntityHighlightEffectType, pair<Directives, Directives>> m_highlightDirectives;

  Vec2F m_entityBarOffset;
  Vec2F m_entityBarSpacing;
  Vec2F m_entityBarSize;
  Vec2F m_entityBarIconOffset;

  // Updated every frame

  AssetsConstPtr m_assets;
  RectF m_worldScreenRect;

  Vec2F m_previousCameraCenter;
  Vec2F m_parallaxWorldPosition;

  float m_preloadTextureChance;
};

}
