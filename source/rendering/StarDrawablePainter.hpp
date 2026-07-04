#pragma once

#include "StarDrawable.hpp"
#include "StarRenderer.hpp"
#include "StarAssetTextureGroup.hpp"

namespace Star {

STAR_CLASS(DrawablePainter);

class DrawablePainter {
public:
  DrawablePainter(RendererPtr renderer, AssetTextureGroupPtr textureGroup);

  void drawDrawable(Drawable const& drawable);
  // Draws the drawable as if it had been scaled by `scale` about its
  // position and repositioned at `screenPos`, without mutating or copying
  // it. `lineWidthScale` applies to line widths only (historically scaled by
  // pixelRatio, not the full geometry scale). Composing the camera transform
  // here (instead of copy+mutate per drawable per frame) is what lets world
  // drawables be drawn straight out of shared/cached lists.
  void drawDrawable(Drawable const& drawable, float scale, Vec2F const& screenPos, float lineWidthScale);

  void cleanup(int64_t textureTimeout);

private:
  RendererPtr m_renderer;
  AssetTextureGroupPtr m_textureGroup;
};

}
