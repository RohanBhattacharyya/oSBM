#pragma once

#include "StarString.hpp"
#include "StarImage.hpp"
#include "StarByteArray.hpp"
#include "StarMap.hpp"

namespace Star {

STAR_EXCEPTION(FontException, StarException);

STAR_STRUCT(FontImpl);
STAR_CLASS(Font);

class Font {
public:
  static FontPtr loadFont(String const& fileName, unsigned pixelSize = 12);
  static FontPtr loadFont(ByteArrayConstPtr const& bytes, unsigned pixelSize = 12);

  Font();
  ~Font();

  Font(Font const&) = delete;
  Font const& operator=(Font const&) = delete;

  // Create a new font from the same data
  FontPtr clone() const;

  void setPixelSize(unsigned pixelSize);
  void setAlphaThreshold(uint8_t alphaThreshold = 0);

  unsigned height() const;
  unsigned width(String::Char c);

  // May return empty image on unrenderable character (Normally, this will
  // render a box, but if there is an internal freetype error this may return
  // an empty image).
  tuple<Image, Vec2I, bool> render(String::Char c);
  bool exists(String::Char c);

private:
  FontImplPtr m_fontImpl;
  ByteArrayConstPtr m_fontBuffer;
  unsigned m_pixelSize;
  uint8_t m_alphaThreshold;
  bool m_loadFailureLogged = false;

  void loadFontImpl();
  // A font that fails to load (e.g. FreeType allocation failure under memory
  // pressure) renders blank glyphs instead of throwing out of the render
  // loop and taking the whole session down. Logged once per font.
  bool tryLoadFontImpl();
  HashMap<pair<String::Char, unsigned>, unsigned> m_widthCache;
};

}
