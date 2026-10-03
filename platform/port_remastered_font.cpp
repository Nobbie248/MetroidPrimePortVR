// Reads Remastered's FONT asset (port_remastered_font.h).

#define PORT_HD_FONT_FILE_ONLY
#include "port_remastered_font.h"

#include <algorithm>
#include <cstring>

namespace PortRemastered {
namespace {

constexpr size_t kHeaderSize = 0x20;
constexpr size_t kGlyphSize = 48;
// A character and how far to move the pen when it follows the glyph.
constexpr size_t kKerningPairSize = 6;
// How much the distance changes over one texel of the atlas. The asset does
// not say; this is the slope measured across the stems of its glyphs.
constexpr float kPerPixel = 53.f / 255.f;

struct Cursor {
  const uint8_t* data;
  size_t size;
  size_t at;

  bool Has(size_t count) const { return count <= size - at; }
  bool U32(uint32_t& out) {
    if (!Has(4)) {
      return false;
    }
    out = uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
          uint32_t(data[at + 3]) << 24;
    at += 4;
    return true;
  }
  bool Float(float& out) {
    uint32_t bits = 0;
    if (!U32(bits)) {
      return false;
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
  }
  // A count followed by that many items of `itemSize` bytes; leaves the cursor on the first.
  bool Array(size_t itemSize, uint32_t& count) { return U32(count) && count <= (size - at) / itemSize; }
};

}  // namespace

bool ParseFont(const uint8_t* data, size_t size, ModelUuid& atlas, PortHdFont::Font& out, std::string& error) {
  out = {};
  if (size < kHeaderSize || std::memcmp(data, "RFRM", 4) != 0 || std::memcmp(data + 0x14, "FONT", 4) != 0) {
    error = "not a FONT asset";
    return false;
  }
  Cursor in{data, size, kHeaderSize};
  uint32_t textures = 0;
  if (!in.Array(16, textures) || textures == 0) {
    error = "no textures";
    return false;
  }
  // Stored little endian, as in the pak: the first three groups are byte
  // swapped against the printed form the paks are indexed by.
  static const uint8_t kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (size_t i = 0; i < 16; ++i) {
    atlas[i] = data[in.at + kOrder[i]];
  }
  in.at += size_t(textures) * 16;

  // One size and one padding per texture, then the faces' line metrics.
  uint32_t unknown = 0;
  uint32_t sizes = 0;
  uint32_t paddings = 0;
  uint32_t metrics = 0;
  if (!in.U32(unknown) || !in.Array(4, sizes)) {
    error = "truncated";
    return false;
  }
  in.at += size_t(sizes) * 4;
  if (!in.Array(4, paddings) || paddings == 0 || !in.Float(out.padding)) {
    error = "truncated";
    return false;
  }
  in.at += size_t(paddings - 1) * 4;
  if (!in.Array(16, metrics)) {
    error = "truncated";
    return false;
  }
  in.at += size_t(metrics) * 16;

  // A glyph record is 44 bytes, a count of kerning pairs and then the pairs
  // themselves, so the records are not all the same length.
  uint32_t glyphs = 0;
  if (!in.Array(kGlyphSize, glyphs) || !in.U32(unknown)) {
    error = "truncated";
    return false;
  }
  for (uint32_t i = 0; i < glyphs; ++i) {
    if (!in.Has(kGlyphSize)) {
      error = "truncated";
      return false;
    }
    const uint8_t* const record = data + in.at;
    in.at += kGlyphSize - 4;
    uint32_t pairs = 0;
    if (!in.Array(kKerningPairSize, pairs)) {
      error = "truncated";
      return false;
    }
    in.at += size_t(pairs) * kKerningPairSize;
    const uint32_t character = uint32_t(record[0]) | uint32_t(record[1]) << 8;
    const uint32_t face = uint32_t(record[2]) | uint32_t(record[3]) << 8;
    // The top byte of the next word is the texture the glyph is on.
    if (face != 0 || record[7] != 0) {
      continue;
    }
    PortHdFont::Glyph glyph;
    glyph.character = character;
    float values[9];
    std::memcpy(values, record + 8, sizeof(values));
    glyph.left = values[0];
    glyph.top = values[1];
    glyph.width = values[2];
    glyph.height = values[3];
    glyph.u0 = values[4];
    glyph.v0 = values[5];
    glyph.u1 = values[6];
    glyph.v1 = values[7];
    glyph.advance = values[8];
    out.glyphs.push_back(glyph);
  }
  std::sort(out.glyphs.begin(), out.glyphs.end(),
            [](const PortHdFont::Glyph& a, const PortHdFont::Glyph& b) { return a.character < b.character; });
  out.glyphs.erase(std::unique(out.glyphs.begin(), out.glyphs.end(),
                               [](const PortHdFont::Glyph& a, const PortHdFont::Glyph& b) {
                                 return a.character == b.character;
                               }),
                   out.glyphs.end());
  if (out.glyphs.empty() || !(out.padding >= 0.f)) {
    error = "no glyphs in the first face";
    return false;
  }
  out.perPixel = kPerPixel;
  return true;
}

bool SetFontAtlas(PortHdFont::Font& font, uint32_t width, uint32_t height, const uint8_t* rgba, size_t size) {
  if (width == 0 || height == 0 || size / 4 / width != height || size % 4 != 0) {
    return false;
  }
  font.width = width;
  font.height = height;
  font.distance.resize(size / 4);
  for (size_t i = 0; i < font.distance.size(); ++i) {
    font.distance[i] = rgba[i * 4];
  }
  return true;
}

}  // namespace PortRemastered
