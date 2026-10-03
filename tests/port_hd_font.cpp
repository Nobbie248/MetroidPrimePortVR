// The .sdfont file, the fit of a distance-field font to a bitmap one, and the
// FONT reader. With arguments it converts the user's own data, which is how a
// test mod gets its font without a full import:
//   port_hd_font_tests <FONT asset> <atlas.rgba> <width> <height> <out.sdfont>

#define PORT_HD_FONT_FILE_ONLY
#include "port_hd_font.h"
#include "port_remastered_font.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

bool Close(float a, float b) { return std::fabs(a - b) < 0.01f; }

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(out, bits);
}

// A capital H as Remastered stores it: 22 pixels of ink, 4 of field around it,
// and the ink one pixel below the baseline.
PortHdFont::Glyph CapitalH() {
  PortHdFont::Glyph glyph;
  glyph.character = 'H';
  glyph.left = -3.f;
  glyph.top = 25.f;
  glyph.width = 25.f;
  glyph.height = 30.f;
  glyph.u0 = 0.25f;
  glyph.v0 = 0.75f;
  glyph.u1 = 0.5f;
  glyph.v1 = 0.25f;
  glyph.advance = 20.f;
  return glyph;
}

PortHdFont::Font SmallFont() {
  PortHdFont::Font font;
  font.width = 8;
  font.height = 4;
  font.padding = 4.f;
  font.perPixel = 0.2f;
  font.glyphs.push_back(CapitalH());
  PortHdFont::Glyph comma = CapitalH();
  comma.character = 0x3001;
  font.glyphs.push_back(comma);
  for (int i = 0; i < 32; ++i) {
    font.distance.push_back(uint8_t(i * 8));
  }
  return font;
}

void TestFile() {
  const PortHdFont::Font font = SmallFont();
  std::vector<uint8_t> bytes;
  Check(PortHdFont::WriteFont(font, bytes), "a font is written");
  PortHdFont::Font read;
  std::string error;
  Check(PortHdFont::ReadFont(bytes.data(), bytes.size(), read, error), "and read back");
  Check(read.width == 8 && read.height == 4 && read.glyphs.size() == 2 && read.distance == font.distance,
        "with its atlas");
  Check(Close(read.padding, 4.f) && Close(read.perPixel, 0.2f), "its field");
  const PortHdFont::Glyph* const h = read.Find('H');
  Check(h != nullptr && Close(h->top, 25.f) && Close(h->u1, 0.5f) && Close(h->advance, 20.f), "and its glyphs");
  Check(read.Find(0x3001) != nullptr && read.Find('I') == nullptr, "glyphs are found by character");

  Check(!PortHdFont::ReadFont(bytes.data(), bytes.size() - 1, read, error), "a cut file is refused");
  std::vector<uint8_t> bad = bytes;
  bad[0] = 'X';
  Check(!PortHdFont::ReadFont(bad.data(), bad.size(), read, error), "as is another file");
  bad = bytes;
  bad[4] = 9;
  Check(!PortHdFont::ReadFont(bad.data(), bad.size(), read, error), "and another version");

  PortHdFont::Font empty = font;
  empty.glyphs.clear();
  Check(!PortHdFont::WriteFont(empty, bytes), "a font needs glyphs");
  PortHdFont::Font shortAtlas = font;
  shortAtlas.distance.pop_back();
  Check(!PortHdFont::WriteFont(shortAtlas, bytes), "and a whole atlas");

  Check(PortHdFont::ParseFileName("deface.sdfont") && PortHdFont::ParseFileName("Deface.SDFONT"), "the file's name");
  Check(!PortHdFont::ParseFileName("deface.sdf") && !PortHdFont::ParseFileName(".sdfont"), "and what is not one");
}

void TestFit() {
  const PortHdFont::Font font = SmallFont();
  const PortHdFont::Glyph& h = font.glyphs[0];
  PortHdFont::Fit fit;
  // A plain bitmap font: the H fills its 22 pixel cell and stands on the line.
  Check(PortHdFont::FitFont(font, 22, 22, false, fit), "a plain font fits");
  Check(Close(fit.scale, 1.f) && Close(fit.baseline, 0.f) && fit.edge == 128, "at its own size");
  PortHdFont::Box box = PortHdFont::GlyphBox(h, fit, 100.f, 50.f, 17, 22);
  // The ink is the box less its padding: it must be the cell, 50 to 72.
  Check(Close(box.top + 4.f, 50.f) && Close(box.bottom - 4.f, 72.f), "the ink fills the cell's height");
  Check(Close((box.left + box.right) / 2.f, 108.5f), "and is centred in its width");

  // Half the size.
  Check(PortHdFont::FitFont(font, 11, 11, false, fit) && Close(fit.scale, 0.5f), "a smaller font scales");
  box = PortHdFont::GlyphBox(h, fit, 0.f, 0.f, 9, 11);
  Check(Close(box.top + 2.f, 0.f) && Close(box.bottom - 2.f, 11.f), "with its padding");

  // An outlined font: a 13 pixel H in a 15 pixel cell whose line is 13 down.
  Check(PortHdFont::FitFont(font, 15, 13, true, fit), "an outlined font fits");
  Check(Close(fit.scale, 13.f / 22.f) && Close(fit.baseline, 1.f), "inside its outline");
  Check(fit.edge < 128 && fit.edge >= 16, "which the edge leaves room for");
  box = PortHdFont::GlyphBox(h, fit, 0.f, 0.f, 13, 13);
  Check(Close(box.top + 4.f * fit.scale, 1.f) && Close(box.bottom - 4.f * fit.scale, 14.f),
        "the ink is a pixel inside the cell");

  PortHdFont::Font noH = font;
  noH.glyphs.erase(noH.glyphs.begin());
  Check(!PortHdFont::FitFont(noH, 22, 22, false, fit), "a font without an H cannot be measured");
  Check(!PortHdFont::FitFont(font, 0, 0, false, fit), "nor an empty cell");
}

void PutGlyph(std::vector<uint8_t>& out, uint16_t character, uint16_t face, uint8_t page) {
  out.push_back(uint8_t(character));
  out.push_back(uint8_t(character >> 8));
  out.push_back(uint8_t(face));
  out.push_back(uint8_t(face >> 8));
  Put32(out, uint32_t(page) << 24);
  const float values[9] = {-3.f, 25.f, 25.f, 30.f, 0.25f, 0.75f, 0.5f, 0.25f, 20.f};
  for (float value : values) {
    PutFloat(out, value);
  }
  Put32(out, 0);
}

void TestAsset() {
  std::vector<uint8_t> asset(0x20, 0);
  std::memcpy(asset.data(), "RFRM", 4);
  std::memcpy(asset.data() + 0x14, "FONT", 4);
  Put32(asset, 2);
  for (int i = 0; i < 32; ++i) {
    asset.push_back(uint8_t(i + 1));
  }
  Put32(asset, 1);
  Put32(asset, 2);
  PutFloat(asset, 32.f);
  PutFloat(asset, 30.f);
  Put32(asset, 2);
  PutFloat(asset, 4.f);
  PutFloat(asset, 6.f);
  Put32(asset, 1);
  asset.insert(asset.end(), 16, 0);
  Put32(asset, 4);
  Put32(asset, 2);
  PutGlyph(asset, 'I', 0, 0);
  // Two kerning pairs after it, which read as a glyph if taken for a record.
  asset[asset.size() - 4] = 2;
  for (const char next : {'H', 'I'}) {
    asset.push_back(uint8_t(next));
    asset.push_back(0);
    PutFloat(asset, -0.78f);
  }
  PutGlyph(asset, 'H', 0, 0);
  PutGlyph(asset, 0x3042, 1, 1);
  PutGlyph(asset, 'H', 2, 3);

  PortRemastered::ModelUuid atlas{};
  PortHdFont::Font font;
  std::string error;
  Check(PortRemastered::ParseFont(asset.data(), asset.size(), atlas, font, error), "a FONT asset is read");
  Check(atlas[0] == 4 && atlas[3] == 1 && atlas[4] == 6 && atlas[15] == 16, "its first texture, in printed order");
  Check(font.glyphs.size() == 2 && font.glyphs[0].character == 'H' && font.glyphs[1].character == 'I',
        "and its first face, sorted");
  Check(Close(font.padding, 4.f) && font.perPixel > 0.f, "with that texture's padding");
  Check(!PortRemastered::ParseFont(asset.data(), asset.size() - 8, atlas, font, error), "a cut asset is refused");
  asset[0x14] = 'X';
  Check(!PortRemastered::ParseFont(asset.data(), asset.size(), atlas, font, error), "as is another asset");

  const uint8_t rgba[16] = {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255};
  Check(PortRemastered::SetFontAtlas(font, 2, 2, rgba, sizeof(rgba)) && font.distance[3] == 40,
        "the atlas is the texture's red");
  Check(!PortRemastered::SetFontAtlas(font, 3, 2, rgba, sizeof(rgba)), "of the size it says");
}

std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int Convert(char** argv) {
  const std::vector<uint8_t> asset = ReadFile(argv[1]);
  const std::vector<uint8_t> rgba = ReadFile(argv[2]);
  PortRemastered::ModelUuid atlas{};
  PortHdFont::Font font;
  std::string error;
  if (!PortRemastered::ParseFont(asset.data(), asset.size(), atlas, font, error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  std::vector<uint8_t> out;
  if (!PortRemastered::SetFontAtlas(font, uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4])), rgba.data(),
                                    rgba.size()) ||
      !PortHdFont::WriteFont(font, out)) {
    std::fprintf(stderr, "%s: not a %sx%s RGBA image\n", argv[2], argv[3], argv[4]);
    return 1;
  }
  std::ofstream file(argv[5], std::ios::binary);
  file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  std::printf("%zu glyphs, padding %g, atlas texture %02x%02x%02x%02x..., %zu bytes\n", font.glyphs.size(),
              font.padding, atlas[0], atlas[1], atlas[2], atlas[3], out.size());
  return file ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 6) {
    return Convert(argv);
  }
  TestFile();
  TestFit();
  TestAsset();
  if (sFailures == 0) {
    std::puts("port_hd_font tests passed");
  }
  return sFailures == 0 ? 0 : 1;
}
