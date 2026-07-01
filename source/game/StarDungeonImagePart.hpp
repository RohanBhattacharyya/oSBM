#pragma once

#include "StarDungeonGenerator.hpp"

namespace Star {

namespace Dungeon {

  STAR_CLASS(ImagePartReader);
  STAR_CLASS(ImageTileset);

  class ImagePartReader : public PartReader {
  public:
    ImagePartReader(ImageTilesetConstPtr tileset) : m_tileset(tileset) {}

    virtual void readAsset(String const& asset) override;
    virtual Vec2U size() const override;

    virtual void forEachTile(TileCallback const& callback) const override;
    virtual void forEachTileAt(Vec2I pos, TileCallback const& callback) const override;

  private:
    List<pair<String, ImageConstPtr>> m_images;
    ImageTilesetConstPtr m_tileset;
    // Resolving a pixel color to a Tile* via m_tileset->getTile() is a hashmap
    // lookup, and image->get() is a real buffer access -- forEachTile used to
    // redo both for every tile on every single call, but a Part is placed via
    // many back-to-back forEachTile passes (once per placement phase, plus
    // canPlace/collidesWithPlaces/scanning), so this is resolved once here and
    // just replayed thereafter.
    List<pair<Vec2I, Tile const*>> m_resolvedTiles;
  };

  class ImageTileset {
  public:
    ImageTileset(Json const& tileset);

    Tile const* getTile(Vec4B color) const;

  private:
    unsigned colorAsInt(Vec4B color) const;

    Map<unsigned, Tile> m_tiles;
  };
}
}
