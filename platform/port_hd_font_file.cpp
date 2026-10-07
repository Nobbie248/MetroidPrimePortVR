// The .sdfont file and the arithmetic that fits it to a bitmap font (port_hd_font.h).

#define PORT_HD_FONT_FILE_ONLY
#include "port_hd_font.h"
#include "port_bytes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>

namespace PortHdFont {
namespace {

constexpr char kMagic[4] = {'S', 'D', 'F', 'T'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 28;
constexpr size_t kGlyphSize = 40;
constexpr uint32_t kMaxAtlasSide = 8192;

using port::AppendLE32;
using port::AppendLEFloat;
using port::ReadLE32;
using port::ReadLEFloat;

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
  AppendLE32(out, kVersion);
  AppendLE32(out, font.width);
  AppendLE32(out, font.height);
  AppendLEFloat(out, font.padding);
  AppendLEFloat(out, font.perPixel);
  AppendLE32(out, uint32_t(glyphs.size()));
  for (const Glyph& glyph : glyphs) {
    AppendLE32(out, glyph.character);
    for (const float value : {glyph.left, glyph.top, glyph.width, glyph.height, glyph.u0, glyph.v0, glyph.u1,
                              glyph.v1, glyph.advance}) {
      AppendLEFloat(out, value);
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
  if (ReadLE32(data + 4) != kVersion) {
    error = "made for another version of the port";
    return false;
  }
  out = {};
  out.width = ReadLE32(data + 8);
  out.height = ReadLE32(data + 12);
  out.padding = ReadLEFloat(data + 16);
  out.perPixel = ReadLEFloat(data + 20);
  const uint32_t count = ReadLE32(data + 24);
  if (out.width == 0 || out.height == 0 || out.width > kMaxAtlasSide || out.height > kMaxAtlasSide ||
      count == 0 || count > (size - kHeaderSize) / kGlyphSize ||
      size - kHeaderSize - size_t(count) * kGlyphSize != size_t(out.width) * out.height) {
    error = "cut short";
    return false;
  }
  out.glyphs.resize(count);
  const uint8_t* at = data + kHeaderSize;
  for (Glyph& glyph : out.glyphs) {
    glyph.character = ReadLE32(at);
    float* const fields[] = {&glyph.left, &glyph.top, &glyph.width, &glyph.height, &glyph.u0,
                             &glyph.v0,   &glyph.u1,  &glyph.v1,    &glyph.advance};
    for (size_t i = 0; i < 9; ++i) {
      *fields[i] = ReadLEFloat(at + 4 + 4 * i);
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

const std::vector<StandIn>& StandIns() {
  static const std::vector<StandIn> kStandIns = [] {
    std::vector<StandIn> out;
    // Latin-1, from U+00A0; '\0' where no ASCII character comes close.
    static const char kLatin1[] = " !cLoY|S\"Ca<--R-o+23'uP.,1o>\0\0\0?"
                                  "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
                                  "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
    for (uint32_t i = 0; i < sizeof(kLatin1) - 1; ++i) {
      if (kLatin1[i] != '\0') {
        out.push_back({0xA0 + i, kLatin1[i]});
      }
    }
    static const StandIn kOthers[] = {
        {0x0152, 'O'},  {0x0153, 'o'},  {0x0178, 'Y'},  {0x1E9E, 'S'},  {0x2013, '-'},  {0x2014, '-'},
        {0x2018, '\''}, {0x2019, '\''}, {0x201A, ','},  {0x201C, '"'},  {0x201D, '"'},  {0x201E, '"'},
        {0x2026, '.'},  {0x2039, '<'},  {0x203A, '>'},  {0x202F, ' '},  {0x20AC, 'E'},  {0x2122, 'T'},
        {0x2212, '-'},
    };
    out.insert(out.end(), std::begin(kOthers), std::end(kOthers));
    return out;
  }();
  return kStandIns;
}

float AdvanceRatio(const Font& font, uint32_t character, uint32_t base) {
  const Glyph* const glyph = font.Find(character);
  const Glyph* const reference = font.Find(base);
  if (glyph == nullptr || reference == nullptr || !(reference->advance > 0.f) || !(glyph->advance > 0.f)) {
    return 1.f;
  }
  return std::clamp(glyph->advance / reference->advance, 0.25f, 4.f);
}

}  // namespace PortHdFont
