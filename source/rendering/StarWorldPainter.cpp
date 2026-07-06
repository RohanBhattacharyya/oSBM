#include "StarWorldPainter.hpp"
#include "StarAnimation.hpp"
#include "StarRoot.hpp"
#include "StarConfiguration.hpp"
#include "StarAssets.hpp"
#include "StarJsonExtra.hpp"
#include "StarLogging.hpp"
#include "StarTime.hpp"

namespace Star {

WorldPainter::WorldPainter() {
  m_assets = Root::singleton().assets();

  m_camera.setScreenSize({800, 600});
  m_camera.setCenterWorldPosition(Vec2F());
  m_camera.setPixelRatio(Root::singleton().configuration()->get("zoomLevel").toFloat());

  m_highlightConfig = m_assets->json("/highlights.config");
  for (auto p : m_highlightConfig.get("highlightDirectives").iterateObject())
    m_highlightDirectives.set(EntityHighlightEffectTypeNames.getLeft(p.first), {p.second.getString("underlay", ""), p.second.getString("overlay", "")});

  m_entityBarOffset = jsonToVec2F(m_assets->json("/rendering.config:entityBarOffset"));
  m_entityBarSpacing = jsonToVec2F(m_assets->json("/rendering.config:entityBarSpacing"));
  m_entityBarSize = jsonToVec2F(m_assets->json("/rendering.config:entityBarSize"));
  m_entityBarIconOffset = jsonToVec2F(m_assets->json("/rendering.config:entityBarIconOffset"));
  m_preloadTextureChance = m_assets->json("/rendering.config:preloadTextureChance").toFloat();
}

void WorldPainter::renderInit(RendererPtr renderer) {
  m_assets = Root::singleton().assets();

  m_renderer = std::move(renderer);
  auto textureGroup = m_renderer->createTextureGroup(TextureGroupSize::Large);
  m_textPainter = make_shared<TextPainter>(m_renderer, textureGroup);
  m_tilePainter = make_shared<TilePainter>(m_renderer);
  m_drawablePainter = make_shared<DrawablePainter>(m_renderer, make_shared<AssetTextureGroup>(textureGroup));
  m_environmentPainter = make_shared<EnvironmentPainter>(m_renderer);
}

void WorldPainter::setCameraPosition(WorldGeometry const& geometry, Vec2F const& position) {
  m_camera.setWorldGeometry(geometry);
  m_camera.setCenterWorldPosition(position);
}

WorldCamera& WorldPainter::camera() {
  return m_camera;
}

void WorldPainter::update(float dt) {
  m_environmentPainter->update(dt);
}

void WorldPainter::render(WorldRenderData& renderData, function<bool()> lightWaiter) {
#ifdef STAR_SYSTEM_SWITCH
  // Paint-phase breakdown ([perf-wp]), logged every ~150 frames.
  static int64_t s_wpFrames = 0, s_wpSetup = 0, s_wpEnv = 0, s_wpLight = 0, s_wpWorld = 0, s_wpCleanup = 0;
  int64_t wpLapTime = Time::monotonicMicroseconds();
  auto wpLap = [&wpLapTime](int64_t& acc) {
    int64_t n = Time::monotonicMicroseconds();
    acc += n - wpLapTime;
    wpLapTime = n;
  };
#endif
  m_camera.setScreenSize(m_renderer->screenSize());
  m_camera.setTargetPixelRatio(Root::singleton().configuration()->get("zoomLevel").toFloat());

  m_assets = Root::singleton().assets();

  m_tilePainter->setup(m_camera, renderData);
#ifdef STAR_SYSTEM_SWITCH
  wpLap(s_wpSetup);
#endif

  // Stars, Debris Fields, Sky, and Orbiters

  // Use a fixed pixel ratio for certain things.
  float pixelRatioBasis = m_camera.screenSize()[1] / 1080.0f;
  float starAndDebrisRatio = lerp(0.0625f, pixelRatioBasis * 2.0f, m_camera.pixelRatio());
  float orbiterAndPlanetRatio = lerp(0.125f, pixelRatioBasis * 3.0f, m_camera.pixelRatio());

  bool bgScaled = false;
  bool bgReuse = false;
#ifdef STAR_SYSTEM_FAMILY_MOBILE
  // Draw the environment background (sky, sun + rays, orbiters, horizon,
  // stars, debris, parallax) into the reduced-resolution "background"
  // framebuffer and stretch-blit it under the world layers. This content is
  // all low-frequency and expensive mostly through primitive BUILD + buffer
  // UPLOAD cost (~15ms/frame measured), so under load it's additionally only
  // REFRESHED every 2nd frame: the buffer is marked "preserve" (not cleared
  // at startFrame) and the previous contents are re-blitted on skip frames.
  // At the sub-20fps rates where this engages, the background (sky gradient,
  // distant parallax) moves less than a pixel per frame, so the one-frame
  // staleness is imperceptible.
  {
    int64_t nowUs = Time::monotonicMicroseconds();
    int64_t gapUs = m_bgLastRenderTimeUs != 0 ? nowUs - m_bgLastRenderTimeUs : 0;
    bool wpUnderLoad = gapUs > 50000;
    int64_t bgInterval = gapUs > 75000 ? 4 : 3;
    m_bgLastRenderTimeUs = nowUs;
    ++m_bgFrameCounter;
    Vec2U screenSize = m_renderer->screenSize();
    if (wpUnderLoad && m_bgValidSize == screenSize && (m_bgFrameCounter % bgInterval != 0))
      bgReuse = true;
    else if ((bgScaled = m_renderer->switchFrameBuffer("background")))
      m_bgValidSize = screenSize;
    else
      m_bgValidSize = Vec2U();
  }
#endif

  if (!bgReuse) {
    m_environmentPainter->renderStars(starAndDebrisRatio, Vec2F(m_camera.screenSize()), renderData.skyRenderData);
    m_environmentPainter->renderDebrisFields(starAndDebrisRatio, Vec2F(m_camera.screenSize()), renderData.skyRenderData);
    if (renderData.skyRenderData.type != SkyType::Atmosphereless)
      m_environmentPainter->renderBackOrbiters(orbiterAndPlanetRatio, Vec2F(m_camera.screenSize()), renderData.skyRenderData);
    m_environmentPainter->renderPlanetHorizon(orbiterAndPlanetRatio, Vec2F(m_camera.screenSize()), renderData.skyRenderData);
    m_environmentPainter->renderSky(Vec2F(m_camera.screenSize()), renderData.skyRenderData);
    m_environmentPainter->renderFrontOrbiters(orbiterAndPlanetRatio, Vec2F(m_camera.screenSize()), renderData.skyRenderData);
    if (renderData.skyRenderData.type == SkyType::Atmosphereless)
      m_environmentPainter->renderBackOrbiters(orbiterAndPlanetRatio, Vec2F(m_camera.screenSize()), renderData.skyRenderData);
  }

  // Parallax camera tracking must run exactly once per frame regardless of
  // which render target the parallax layers are drawn into below.
  auto parallaxDelta = m_camera.worldGeometry().diff(m_camera.centerWorldPosition(), m_previousCameraCenter);
  if (parallaxDelta.magnitude() > 10)
    m_parallaxWorldPosition = m_camera.centerWorldPosition();
  else
    m_parallaxWorldPosition += parallaxDelta;
  m_previousCameraCenter = m_camera.centerWorldPosition();
  m_parallaxWorldPosition[1] = m_camera.centerWorldPosition()[1];

  bool parallaxRendered = false;

#ifdef STAR_SYSTEM_FAMILY_MOBILE
  // Parallax layers are several full-screen alpha-blended passes (~5-13ms of
  // fill at full res) of the same low-frequency background content, so when
  // the scaled background buffer is active they're drawn into it too (same
  // z-order: above sky/orbiters, below all world layers). Tradeoff: they are
  // flushed BEFORE this frame's lightMap uniforms are set, so lightMapped
  // parallax layers shade with the previous frame's lightmap -- one frame
  // stale, imperceptible for distant background content.
  if (bgScaled && renderData.parallaxLayers && !renderData.parallaxLayers->empty()) {
    m_environmentPainter->renderParallaxLayers(m_parallaxWorldPosition, m_camera, *renderData.parallaxLayers, renderData.skyRenderData);
    parallaxRendered = true;
  }
#endif

  m_renderer->flush();
#ifdef STAR_SYSTEM_FAMILY_MOBILE
  if (bgReuse) {
    // Skip frame: composite the preserved background from the last refresh.
    parallaxRendered = true;
    m_renderer->switchFrameBuffer("main");
    m_renderer->blitFrameBufferToCurrent("background");
  } else if (bgScaled) {
    m_renderer->switchFrameBuffer("main");
    m_renderer->blitFrameBufferToCurrent("background");
  }
#endif

#ifdef STAR_SYSTEM_SWITCH
  wpLap(s_wpEnv);
#endif
  bool lightMapUpdated = lightWaiter ? lightWaiter() : false;

  m_renderer->setEffectParameter("lightMapEnabled", !renderData.isFullbright);
  if (renderData.isFullbright) {
    m_renderer->setEffectTexture("lightMap", Image::filled(Vec2U(1, 1), { 255, 255, 255, 255 }, PixelFormat::RGB24));
    m_renderer->setEffectParameter("lightMapMultiplier", 1.0f);
  } else {
    if (lightMapUpdated) {
      adjustLighting(renderData);
      m_renderer->setEffectTexture("lightMap", renderData.lightMap);
    }
    // Fixed content-config value, re-read from Assets (mutex + path-parse) every
    // frame previously despite never changing during a session.
    static float const lightMapMultiplier = m_assets->json("/rendering.config:lightMapMultiplier").toFloat();
    m_renderer->setEffectParameter("lightMapMultiplier", lightMapMultiplier);
    m_renderer->setEffectParameter("lightMapScale", Vec2F::filled(TilePixels * m_camera.pixelRatio()));
    m_renderer->setEffectParameter("lightMapOffset", m_camera.worldToScreen(Vec2F(renderData.lightMinPosition)));
  }

#ifdef STAR_SYSTEM_SWITCH
  wpLap(s_wpLight);
#endif
  // Parallax layers (unless already drawn into the scaled background buffer)

  if (!parallaxRendered && renderData.parallaxLayers && !renderData.parallaxLayers->empty())
    m_environmentPainter->renderParallaxLayers(m_parallaxWorldPosition, m_camera, *renderData.parallaxLayers, renderData.skyRenderData);

  // Main world layers

  // Entries reference (not steal) the render data's drawable lists: entries
  // may be shared with WorldClient's entity render cache across frames, so
  // they must be treated as immutable here.
  Map<EntityRenderLayer, List<pair<EntityHighlightEffect, List<Drawable> const*>>> entityDrawables;
  for (auto& ed : renderData.entityDrawables) {
    for (auto& p : ed->layers)
      entityDrawables[p.first].append({ed->highlightEffect, &p.second});
  }

  auto entityDrawableIterator = entityDrawables.begin();
  auto renderEntitiesUntil = [this, &entityDrawables, &entityDrawableIterator](Maybe<EntityRenderLayer> until) {
    while (true) {
      if (entityDrawableIterator == entityDrawables.end())
        break;
      if (until && entityDrawableIterator->first >= *until)
        break;
      for (auto& edl : entityDrawableIterator->second)
        drawEntityLayer(*edl.second, edl.first);
      ++entityDrawableIterator;
    }

    m_renderer->flush();
  };

  renderEntitiesUntil(RenderLayerBackgroundOverlay);
  drawDrawableSet(renderData.backgroundOverlays);
  renderEntitiesUntil(RenderLayerBackgroundTile);
  m_tilePainter->renderBackground(m_camera);
  renderEntitiesUntil(RenderLayerPlatform);
  m_tilePainter->renderMidground(m_camera);
  renderEntitiesUntil(RenderLayerBackParticle);
  renderParticles(renderData, Particle::Layer::Back);
  renderEntitiesUntil(RenderLayerLiquid);
  m_tilePainter->renderLiquid(m_camera);
  renderEntitiesUntil(RenderLayerMiddleParticle);
  renderParticles(renderData, Particle::Layer::Middle);
  renderEntitiesUntil(RenderLayerForegroundTile);
  m_tilePainter->renderForeground(m_camera);
  renderEntitiesUntil(RenderLayerForegroundOverlay);
  drawDrawableSet(renderData.foregroundOverlays);
  renderEntitiesUntil(RenderLayerFrontParticle);
  renderParticles(renderData, Particle::Layer::Front);
  renderEntitiesUntil(RenderLayerOverlay);
  drawDrawableSet(renderData.nametags);
  renderBars(renderData);
  renderEntitiesUntil({});

  auto dimLevel = round(renderData.dimLevel * 255);
  if (dimLevel != 0)
    m_renderer->render(renderFlatRect(RectF::withSize({}, Vec2F(m_camera.screenSize())), Vec4B(renderData.dimColor, dimLevel), 0.0f));

#ifdef STAR_SYSTEM_SWITCH
  wpLap(s_wpWorld);
#endif
  static int64_t const textureTimeout = m_assets->json("/rendering.config:textureTimeout").toInt();
  m_textPainter->cleanup(textureTimeout);
  m_drawablePainter->cleanup(textureTimeout);
  m_environmentPainter->cleanup(textureTimeout);
  m_tilePainter->cleanup();
#ifdef STAR_SYSTEM_SWITCH
  wpLap(s_wpCleanup);
  if (++s_wpFrames >= 150) {
    Logger::info("[perf-wp] setup={:.1f}ms env={:.1f} light={:.1f} world={:.1f} cleanup={:.1f}",
        s_wpSetup / 1e3 / s_wpFrames, s_wpEnv / 1e3 / s_wpFrames, s_wpLight / 1e3 / s_wpFrames,
        s_wpWorld / 1e3 / s_wpFrames, s_wpCleanup / 1e3 / s_wpFrames);
    s_wpFrames = 0;
    s_wpSetup = s_wpEnv = s_wpLight = s_wpWorld = s_wpCleanup = 0;
  }
#endif
}

void WorldPainter::adjustLighting(WorldRenderData& renderData) {
  m_tilePainter->adjustLighting(renderData);
}

void WorldPainter::renderParticles(WorldRenderData& renderData, Particle::Layer layer) {
  static int const textParticleFontSize = m_assets->json("/rendering.config:textParticleFontSize").toInt();
  static int const particleRenderWindowPadding = m_assets->json("/rendering.config:particleRenderWindowPadding").toInt();
  const RectF particleRenderWindow = RectF::withSize(Vec2F(), Vec2F(m_camera.screenSize())).padded(particleRenderWindowPadding);

  if (!renderData.particles)
    return;

  for (Particle const& particle : *renderData.particles) {
    if (layer != particle.layer)
      continue;

    Vec2F position = m_camera.worldToScreen(particle.position);

    if (!particleRenderWindow.contains(position))
      continue;

    Vec2F size = Vec2F::filled(particle.size * m_camera.pixelRatio());

    if (particle.type == Particle::Type::Ember) {
      m_renderer->immediatePrimitives().emplace_back(std::in_place_type_t<RenderQuad>(),
        RectF(position - size / 2, position + size / 2),
        particle.color.toRgba(),
        particle.fullbright ? 0.0f : 1.0f);

    } else if (particle.type == Particle::Type::Streak) {
      // Draw a rotated quad streaking in the direction the particle is coming from.
      // Sadly this looks awful.
      Vec2F dir = particle.velocity.normalized();
      Vec2F sideHalf = dir.rot90() * m_camera.pixelRatio() * particle.size / 2;
      float length = particle.length * m_camera.pixelRatio();
      Vec4B color = particle.color.toRgba();
      float lightMapMultiplier = particle.fullbright ? 0.0f : 1.0f;
      m_renderer->immediatePrimitives().emplace_back(std::in_place_type_t<RenderQuad>(),
        position - sideHalf,
        position + sideHalf,
        position - dir * length + sideHalf,
        position - dir * length - sideHalf,
        color, lightMapMultiplier);

    } else if (particle.type == Particle::Type::Textured || particle.type == Particle::Type::Animated) {
      Drawable drawable;
      if (particle.type == Particle::Type::Textured)
        drawable = Drawable::makeImage(particle.image, 1.0f / TilePixels, true, Vec2F(0, 0));
      else
        drawable = particle.animation->drawable(1.0f / TilePixels);

      if (particle.flip && particle.flippable)
        drawable.scale(Vec2F(-1, 1));
      if (drawable.isImage() && particle.type != Particle::Type::Animated)
        drawable.imagePart().addDirectivesGroup(particle.directives, true);
      drawable.fullbright = particle.fullbright;
      drawable.color = particle.color;
      drawable.rotate(particle.rotation);
      drawable.scale(particle.size);
      drawable.translate(particle.position);
      drawDrawable(std::move(drawable));

    } else if (particle.type == Particle::Type::Text) {
      Vec2F position = m_camera.worldToScreen(particle.position);
      int size = min(128.0f, round((float)textParticleFontSize * m_camera.pixelRatio() * particle.size));
      if (size > 0) {
        m_textPainter->setFontSize(size);
        m_textPainter->setFontColor(particle.color.toRgba());
        m_textPainter->setProcessingDirectives("");
        m_textPainter->setFont("");
        m_textPainter->renderText(particle.string, {position, HorizontalAnchor::HMidAnchor, VerticalAnchor::VMidAnchor});
      }
    }
  }

  m_renderer->flush();
}

void WorldPainter::renderBars(WorldRenderData& renderData) {
  auto offset = m_entityBarOffset;
  for (auto const& bar : renderData.overheadBars) {
    auto position = bar.entityPosition + offset;
    offset += m_entityBarSpacing;
    if (bar.icon) {
      auto iconDrawPosition = position - (m_entityBarSize / 2).round() + m_entityBarIconOffset;
      drawDrawable(Drawable::makeImage(*bar.icon, 1.0f / TilePixels, true, iconDrawPosition));
    }

    if (!bar.detailOnly) {
      auto fullBar = RectF({}, {m_entityBarSize.x() * bar.percentage, m_entityBarSize.y()});
      auto emptyBar = RectF({m_entityBarSize.x() * bar.percentage, 0.0f}, m_entityBarSize);
      auto fullColor = bar.color;
      auto emptyColor = Color::Black;

      drawDrawable(Drawable::makePoly(PolyF(emptyBar), emptyColor, position));
      drawDrawable(Drawable::makePoly(PolyF(fullBar), fullColor, position));
    }
  }

  m_renderer->flush();
}

void WorldPainter::drawEntityLayer(List<Drawable> const& drawables, EntityHighlightEffect highlightEffect) {
  highlightEffect.level *= m_highlightConfig.getFloat("maxHighlightLevel", 1.0);
  if (m_highlightDirectives.contains(highlightEffect.type) && highlightEffect.level > 0) {
    // first pass, draw underlay
    auto underlayDirectives = m_highlightDirectives[highlightEffect.type].first;
    if (!underlayDirectives.empty()) {
      for (auto& d : drawables) {
        if (d.isImage()) {
          auto underlayDrawable = Drawable(d);
          underlayDrawable.fullbright = true;
          underlayDrawable.color = Color::rgbaf(1, 1, 1, highlightEffect.level * d.color.alphaF());
          underlayDrawable.imagePart().addDirectives(underlayDirectives, true);
          drawDrawable(std::move(underlayDrawable));
        }
      }
    }

    // second pass, draw main drawables and overlays
    auto overlayDirectives = m_highlightDirectives[highlightEffect.type].second;
    for (auto const& d : drawables) {
      drawDrawable(d, true);
      if (!overlayDirectives.empty() && d.isImage()) {
        auto overlayDrawable = Drawable(d);
        overlayDrawable.fullbright = true;
        overlayDrawable.color = Color::rgbaf(1, 1, 1, highlightEffect.level * d.color.alphaF());
        overlayDrawable.imagePart().addDirectives(overlayDirectives, true);
        drawDrawable(std::move(overlayDrawable));
      }
    }
  } else {
    for (auto const& d : drawables)
      drawDrawable(d, true);
  }
}

void WorldPainter::drawDrawable(Drawable const& drawable, bool skipOnScreenCheck) {
  // Non-mutating equivalent of the old copy-transform-draw path: the camera
  // transform (worldToScreen + uniform scale about the screen position) is an
  // affine map, so it's composed at primitive-emit time inside
  // DrawablePainter instead of copying and mutating every Drawable -- which
  // matters because Drawable copies carry AssetPath strings/directives and
  // the entity drawable lists are now SHARED with WorldClient's render cache
  // (mutating them would corrupt the cache).
  Vec2F screenPos = m_camera.worldToScreen(drawable.position);
  float scale = m_camera.pixelRatio() * TilePixels;

  if (!skipOnScreenCheck) {
    // Same on-screen test as before: the old code transformed the drawable
    // then took boundBox; since the transform is affine, transforming the
    // world-space boundBox corners is identical.
    RectF worldBound = drawable.boundBox(false);
    RectF screenBound = RectF(
        (worldBound.min() - drawable.position) * scale + screenPos,
        (worldBound.max() - drawable.position) * scale + screenPos);

    // draw the drawable if it's on screen
    // if it's not on screen, there's a random chance to pre-load
    // pre-load is not done on every tick because it's expensive to look up images with long paths
    if (!RectF::withSize(Vec2F(), Vec2F(m_camera.screenSize())).intersects(screenBound)) {
      if (drawable.isImage() && Random::randf() < m_preloadTextureChance)
        m_assets->tryImage(drawable.imagePart().image);
      return;
    }
  }

  m_drawablePainter->drawDrawable(drawable, scale, screenPos, m_camera.pixelRatio());
}

void WorldPainter::drawDrawableSet(List<Drawable>& drawables) {
  for (Drawable const& drawable : drawables)
    drawDrawable(drawable);

  m_renderer->flush();
}

}
