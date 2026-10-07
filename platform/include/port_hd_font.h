#pragma once

// A mod's distance-field font: sharp text at any resolution.
//
// The disc's fonts are bitmaps drawn for a 640 pixel wide screen. A mod's
// <name>.sdfont holds one typeface as a distance field (each texel is how far
// it is from the glyph's edge, 0.5 on the edge), which stays sharp however far
// it is magnified. The port draws it in place of the disc's glyph images for
// the fonts of the same typeface (Deface), and keeps the disc's own layout:
// advances, kerning and line breaks are still the bitmap font's, so text sits
// where it did.
//
// The file, little endian:
//   char[4] "SDFT", u32 version (1)
//   u32 width, u32 height        the atlas
//   f32 padding                  atlas pixels of field around a glyph's ink
//   f32 perPixel                 how much the distance changes over one atlas pixel
//   u32 glyphCount
//   glyphCount x { u32 character; f32 left, top, width, height, u0, v0, u1, v1, advance }
//   width x height bytes         the distances, rows in order of increasing v
// A glyph's box (padding included) is in atlas pixels from the pen position:
// `left` to its left side, `top` up from the baseline to its top. (u0, v0) is
// the box's top left corner in the atlas and (u1, v1) its bottom right, and
// v0 is the larger: Remastered keeps its atlas bottom row first.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PortHdFont {

struct Glyph {
  uint32_t character = 0;
  float left = 0.f;
  float top = 0.f;
  float width = 0.f;
  float height = 0.f;
  float u0 = 0.f;
  float v0 = 0.f;
  float u1 = 0.f;
  float v1 = 0.f;
  float advance = 0.f;
};

struct Font {
  uint32_t width = 0;
  uint32_t height = 0;
  float padding = 0.f;
  float perPixel = 0.f;
  std::vector<Glyph> glyphs;  // sorted by character
  std::vector<uint8_t> distance;

  const Glyph* Find(uint32_t character) const;
};

// "deface.sdfont" (any case).
bool ParseFileName(const std::string& fileName);
// False for a font without glyphs or whose atlas is not width x height bytes.
bool WriteFont(const Font& font, std::vector<uint8_t>& out);
bool ReadFont(const uint8_t* data, size_t size, Font& out, std::string& error);

// How a bitmap font's glyphs map onto the distance-field ones. Both fonts are
// measured by their 'H': `cellHeight` and `baseline` are the bitmap H's, and
// `outline` says its cell carries a one pixel outline around the ink.
struct Fit {
  float scale = 0.f;     // bitmap pixels per atlas pixel
  float baseline = 0.f;  // where the ink's baseline is, below the line's position
  float inkBottom = 0.f; // the H's lower ink edge above the atlas baseline (atlas pixels)
  uint8_t edge = 128;    // GXSetSDF's outer edge: the ink, or the ink and a one pixel outline
};
bool FitFont(const Font& font, int cellHeight, int baseline, bool outline, Fit& out);

// The corners of a glyph's box for a bitmap glyph drawn at (x, y) in a cell
// `cellWidth` wide whose baseline is `glyphBaseline` below its top.
struct Box {
  float left, top, right, bottom;
};
Box GlyphBox(const Glyph& glyph, const Fit& fit, float x, float y, int cellWidth, int glyphBaseline);

// The disc's Deface fonts hold ASCII only, and the languages the port adds
// need accented letters and typographic punctuation. Each of these characters
// is given the cell of an ASCII stand-in ('é' that of 'e'), which the bitmap
// font draws as is and the distance field draws as the real character.
struct StandIn {
  uint32_t character;
  char base;
};
const std::vector<StandIn>& StandIns();
// How much wider `character` is than `base` in `font` (1 when either is missing).
float AdvanceRatio(const Font& font, uint32_t character, uint32_t base);

}  // namespace PortHdFont

#ifndef PORT_HD_FONT_FILE_ONLY
#include <dolphin/gx/GXStruct.h>

class CGraphicsPalette;
class CRasterFont;

namespace PortHdFont {

// MP_HD_FONT=0 turns the feature off; so does the console's `hdfont`.
bool Enabled();
void SetEnabled(bool enabled);
// Lets go of the loaded font; the next Begin reads the mods' again.
void Reset();
// Sets the render state for drawing `font`'s text from the distance field.
// False (nothing changed) when there is no such font or `font` is another typeface.
bool Begin(const CRasterFont& font);
// Draws `chr` where the bitmap glyph would be, at (x, y), in the palette's fill and
// outline colours times `tint`. False when the distance field lacks the glyph.
bool DrawGlyph(const CRasterFont& font, const CGraphicsPalette* palette, int chr, int x, int y,
               const GXColor& tint);
// Puts back what Begin changed and no bitmap font state sets.
void End();
// True for the disc fonts of the distance field's typeface.
bool SameTypeface(const CRasterFont& font);
// AdvanceRatio in the mods' distance-field font; 1 when the feature is off or
// there is no such font.
float ModAdvanceRatio(uint32_t character, uint32_t base);

}  // namespace PortHdFont
#endif
