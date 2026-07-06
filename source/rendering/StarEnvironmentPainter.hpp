#pragma once

#include "StarParallax.hpp"
#include "StarWorldRenderData.hpp"
#include "StarAssetTextureGroup.hpp"
#include "StarRenderer.hpp"
#include "StarWorldCamera.hpp"
#include "StarPerlin.hpp"
#include "StarRandomPoint.hpp"

namespace Star {

STAR_CLASS(EnvironmentPainter);

class EnvironmentPainter {
public:
  EnvironmentPainter(RendererPtr renderer);

  void update(float dt);

  void renderStars(float pixelRatio, Vec2F const& screenSize, SkyRenderData const& sky);
  void renderDebrisFields(float pixelRatio, Vec2F const& screenSize, SkyRenderData const& sky);
  void renderBackOrbiters(float pixelRatio, Vec2F const& screenSize, SkyRenderData const& sky);
  void renderPlanetHorizon(float pixelRatio, Vec2F const& screenSize, SkyRenderData const& sky);
  void renderFrontOrbiters(float pixelRatio, Vec2F const& screenSize, SkyRenderData const& sky);
  void renderSky(Vec2F const& screenSize, SkyRenderData const& sky);

  void renderParallaxLayers(Vec2F parallaxWorldPosition, WorldCamera const& camera, ParallaxLayers const& layers, SkyRenderData const& sky);

  void cleanup(int64_t textureTimeout);

#ifdef STAR_SYSTEM_SWITCH
  // Retained per-parallax-layer GPU buffers: a fully-repeating layer's tile
  // grid is periodic, so a buffer built once (with a one-tile safety ring)
  // redraws under any sub-tile camera translation as a single retained draw
  // (renderBuffer + transform) instead of re-submitting every quad through
  // the guest GL driver each refresh. Rebuilt when the layer's textures
  // (animation frame), tint color, grid size, or tile size change.
  struct ParallaxLayerBuffer {
    RenderBufferPtr rb;
    List<TexturePtr> textures;
    Vec4B color;
    float lightMapMultiplier = 0.0f;
    Vec2F origin;
    int cols = 0;
    int rows = 0;
    Vec2F tileSize;
    bool valid = false;
  };
  List<ParallaxLayerBuffer> m_parallaxBuffers;
#endif

private:
  static float const SunriseTime;
  static float const SunsetTime;
  static float const SunFadeRate;
  static float const MaxFade;
  static float const RayPerlinFrequency;
  static float const RayPerlinAmplitude;
  static int const RayCount;
  static float const RayMinWidth;
  static float const RayWidthVariance;
  static float const RayAngleVariance;
  static float const SunRadius;
  static float const RayColorDependenceLevel;
  static float const RayColorDependenceScale;
  static float const RayUnscaledAlphaVariance;
  static float const RayMinUnscaledAlpha;
  static Vec3B const RayColor;

  void drawRays(float pixelRatio, SkyRenderData const& sky, Vec2F start, float length, double time, float alpha);
  void drawRay(float pixelRatio,
      Vec2F start,
      float width,
      float length,
      float angle,
      double time,
      Vec3B rayColor,
      float sum,
      float sunScale,
      float alpha);
  void drawOrbiter(float pixelRatio, Vec2F const& screenSize, SkyRenderData const& sky, SkyOrbiter const& orbiter);

  uint64_t starsHash(SkyRenderData const& sky, Vec2F const& viewSize) const;
  void setupStars(SkyRenderData const& sky);

  RendererPtr m_renderer;
  AssetTextureGroupPtr m_textureGroup;

  double m_timer;
  PerlinF m_rayPerlin;

  uint64_t m_starsHash{};
  List<TexturePtr> m_starTextures;
  shared_ptr<Random2dPointGenerator<pair<size_t, float>>> m_starGenerator;
  // PointData is the index into the debris field's image list (NOT the image
  // String): a String here heap-allocates a copy for every debris item on every
  // frame inside generate()'s appendAll, which dominated the in-world frame.
  // Stars use the same size_t-index approach, which is why they were ~10x cheaper.
  List<shared_ptr<Random2dPointGenerator<pair<size_t, float>, double>>> m_debrisGenerators;
};

}
