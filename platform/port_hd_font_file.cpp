// The .sdfont file and the arithmetic that fits it to a bitmap font (port_hd_font.h).

#define PORT_HD_FONT_FILE_ONLY
#include "port_hd_font.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace PortHdFont {
namespace {

constexpr char kMagic[4] = {'S', 'D', 'F', 'T'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 28;
constexpr size_t kGlyphSize = 40;
constexpr uint32_t kMaxAtlasSide = 8192;

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (8 * i)));
  }
}
void PutF32(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  PutU32(out, bits);
}
uint32_t GetU32(const uint8_t* data) {
  return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
float GetF32(const uint8_t* data) {
  const uint32_t bits = GetU32(data);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

}  // namespace

const Glyph* Font::Find(uint32_t character) const {
  const auto found = std::lower_bound(glyphs.begin(), glyphs.end(), character,
                                      [](const Glyph& glyph, uint32_t c) { return glyph.character < c; });
  return found != glyphs.end() && found->character == character ? &*found : nullptr;
}

bool ParseFileName(const std::string& fileName) {
  static const char kSuffix[] = ".sdfont";
  const size_t length = sizeof(kSuffix) - 1;
  if (fileName.size() <= length) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (std::tolower(static_cast<unsigned char>(fileName[fileName.size() - length + i])) != kSuffix[i]) {
      return false;
    }
  }
  return true;
}

bool WriteFont(const Font& font, std::vector<uint8_t>& out) {
  if (font.glyphs.empty() || font.width == 0 || font.height == 0 || font.width > kMaxAtlasSide ||
      font.height > kMaxAtlasSide || font.distance.size() != size_t(font.width) * font.height) {
    return false;
  }
  std::vector<Glyph> glyphs = font.glyphs;
  std::sort(glyphs.begin(), glyphs.end(),
            [](const Glyph& a, const Glyph& b) { return a.character < b.character; });
  out.clear();
  out.insert(out.end(), kMagic, kMagic + 4);
  PutU32(out, kVersion);
  PutU32(out, font.width);
  PutU32(out, font.height);
  PutF32(out, font.padding);
  PutF32(out, font.perPixel);
  PutU32(out, uint32_t(glyphs.size()));
  for (const Glyph& glyph : glyphs) {
    PutU32(out, glyph.character);
    for (const float value : {glyph.left, glyph.top, glyph.width, glyph.height, glyph.u0, glyph.v0, glyph.u1,
                              glyph.v1, glyph.advance}) {
      PutF32(out, value);
    }
  }
  out.insert(out.end(), font.distance.begin(), font.distance.end());
  return true;
}

bool ReadFont(const uint8_t* data, size_t size, Font& out, std::string& error) {
  if (size < kHeaderSize || std::memcmp(data, kMagic, 4) != 0) {
    error = "not a distance-field font";
    return false;
  }
  if (GetU32(data + 4) != kVersion) {
    error = "made for another version of the port";
    return false;
  }
  out = {};
  out.width = GetU32(data + 8);
  out.height = GetU32(data + 12);
  out.padding = GetF32(data + 16);
  out.perPixel = GetF32(data + 20);
  const uint32_t count = GetU32(data + 24);
  if (out.width == 0 || out.height == 0 || out.width > kMaxAtlasSide || out.height > kMaxAtlasSide ||
      count == 0 || count > (size - kHeaderSize) / kGlyphSize ||
      size - kHeaderSize - size_t(count) * kGlyphSize != size_t(out.width) * out.height) {
    error = "cut short";
    return false;
  }
  out.glyphs.resize(count);
  const uint8_t* at = data + kHeaderSize;
  for (Glyph& glyph : out.glyphs) {
    glyph.character = GetU32(at);
    float* const fields[] = {&glyph.left, &glyph.top, &glyph.width, &glyph.height, &glyph.u0,
                             &glyph.v0,   &glyph.u1,  &glyph.v1,    &glyph.advance};
    for (size_t i = 0; i < 9; ++i) {
      *fields[i] = GetF32(at + 4 + 4 * i);
      if (!std::isfinite(*fields[i])) {
        error = "a glyph is not a number";
        return false;
      }
    }
    at += kGlyphSize;
  }
  for (size_t i = 1; i < out.glyphs.size(); ++i) {
    if (out.glyphs[i - 1].character >= out.glyphs[i].character) {
      error = "glyphs out of order";
      return false;
    }
  }
  out.distance.assign(at, data + size);
  return true;
}

bool FitFont(const Font& font, int cellHeight, int baseline, bool outline, Fit& out) {
  const Glyph* const reference = font.Find(U'H');
  const float ink = reference != nullptr ? reference->height - 2.f * font.padding : 0.f;
  const int border = outline ? 1 : 0;
  if (ink <= 0.f || cellHeight - 2 * border <= 0 || !(font.perPixel > 0.f)) {
    return false;
  }
  out.scale = float(cellHeight - 2 * border) / ink;
  out.baseline = float(cellHeight - border - baseline);
  out.inkBottom = reference->top - reference->height + font.padding;
  out.edge = 128;
  if (outline) {
    // One bitmap pixel out from the ink, as far as the field reaches.
    const float distance = 0.5f - font.perPixel / out.scale;
    out.edge = uint8_t(std::clamp(distance * 255.f + 0.5f, 16.f, 128.f));
  }
  return true;
}

Box GlyphBox(const Glyph& glyph, const Fit& fit, float x, float y, int cellWidth, int glyphBaseline) {
  Box box;
  const float width = glyph.width * fit.scale;
  box.left = x + (float(cellWidth) - width) * 0.5f;
  box.right = box.left + width;
  box.top = y + float(glyphBaseline) + fit.baseline - (glyph.top - fit.inkBottom) * fit.scale;
  box.bottom = box.top + glyph.height * fit.scale;
  return box;
}

}  // namespace PortHdFont
