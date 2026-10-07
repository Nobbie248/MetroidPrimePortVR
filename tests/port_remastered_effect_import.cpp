#include "port_remastered_effect_import.h"

#include "port_remastered_effect_convert.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

constexpr uint32_t kGenp = EffectFourCC("GENP");
constexpr uint32_t kMati = EffectFourCC("MATI");
constexpr uint32_t kTxtr = EffectFourCC("TXTR");

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutFourCC(std::vector<uint8_t>& out, const char* text) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(text[i]));
  }
}

void PutProperty(std::vector<uint8_t>& out, const char* name, uint8_t tag) {
  PutFourCC(out, name);
  out.push_back(tag);
}

void PutGenerator(std::vector<uint8_t>& out, bool root) {
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, root ? 1 : 0);
}

void PutGuid(std::vector<uint8_t>& out, const EffectGuid& id) { out.insert(out.end(), id.begin(), id.end()); }

// Between a pak's byte order and an effect's.
EffectGuid Swap(const EffectGuid& id) {
  EffectGuid out = id;
  std::swap(out[0], out[3]);
  std::swap(out[1], out[2]);
  std::swap(out[4], out[5]);
  std::swap(out[6], out[7]);
  return out;
}

// An id carried over from retail, as an effect stores it.
EffectGuid Legacy(uint32_t retail) {
  EffectGuid id = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  for (int i = 0; i < 4; ++i) {
    id[size_t(12 + i)] = uint8_t(retail >> (24 - 8 * i));
  }
  return id;
}

EffectGuid Fresh(uint8_t seed) {
  EffectGuid id;
  for (size_t i = 0; i < id.size(); ++i) {
    id[i] = uint8_t(0xa0 + seed + i);
  }
  return id;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// An effect carried over from retail PART 0x00001234: an embedded child it
// spawns through ICTS, and a material instance whose texture is Remastered's own.
void TestImport() {
  const EffectGuid child = Fresh(0x40);
  const EffectGuid material = Fresh(0x10);  // as the effect stores it
  const EffectGuid texture = Fresh(0x20);   // as the material stores it
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "ICTS", 1);
  PutFourCC(genp, "CNST");
  PutGuid(genp, child);
  PutProperty(genp, "MTIN", 1);
  genp.push_back(0);
  PutGuid(genp, material);
  PutProperty(genp, "LTYP", 0);
  genp.push_back(2);
  PutProperty(genp, "LFOT", 0);
  genp.push_back(2);  // retail 1; the disc's 2 wins
  PutProperty(genp, "_END", 4);
  Put32(genp, 1);
  PutGuid(genp, child);
  PutGenerator(genp, false);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 2);
  PutProperty(genp, "_END", 4);

  std::vector<uint8_t> mati(8, 0);
  PutGuid(mati, texture);

  const EffectGuid effectId = Swap(Legacy(0x1234));  // in pak order
  const EffectGuid unrelated = Fresh(0x60);           // not carried over: skipped
  std::map<std::string, std::vector<uint8_t>> written;
  EffectImportIO io;
  io.effects = {effectId, unrelated};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    if (type == kMati && id == Swap(material)) {
      out = mati;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [&](const EffectGuid& id) -> uint32_t {
    return id == Swap(texture) ? kTxtr : id == Swap(material) ? kMati : 0;
  };
  io.retailId = [](uint32_t id) { return id == 0x1234; };
  // The disc's PART: its light replaces the converted one, the rest is ignored.
  std::vector<uint8_t> disc;
  for (uint32_t word : {EffectFourCC("GPSM"), EffectFourCC("MAXP"), EffectFourCC("CNST"), 9u, EffectFourCC("LTYP"),
                        EffectFourCC("CNST"), 1u, EffectFourCC("LFOT"), EffectFourCC("CNST"), 2u,
                        EffectFourCC("LFOR"), EffectFourCC("CNST"), 0x40400000u, EffectFourCC("_END")}) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      disc.push_back(uint8_t(word >> shift));
    }
  }
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    if (type != EffectFourCC("PART") || id != 0x1234) {
      return false;
    }
    out = disc;
    return true;
  };
  uint32_t next = 0x00ABC000;
  io.freshId = [&](uint32_t) { return next++; };
  io.texture = [&](const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba, std::string& error) {
    if (id != Swap(texture)) {
      error = "no such texture";
      return false;
    }
    width = 512;  // written as a .dds, with a 64-texel TXTR stub
    height = 512;
    rgba.assign(size_t(width) * height * 4, 0xff);
    return true;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  Check(result.candidates == 1 && result.written == 1 && result.failed == 0, "the carried-over effect is written");
  Check(result.parts == 2 && result.textures == 1, "its child and texture are written");
  Check(written.size() == 4 && written.count("00001234.PART") == 1 && written.count("00ABC000.PART") == 1 &&
            written.count("00ABC001.TXTR") == 1 && written.count("00ABC001.dds") == 1,
        "files are named by their retail and new ids");
  if (written.count("00001234.PART") == 0) {
    return;
  }
  std::vector<RetailPartProperty> root;
  std::string error;
  const std::vector<uint8_t>& part = written["00001234.PART"];
  Check(SplitRetailPart(part.data(), part.size(), root, error), "the root reads as a PART");
  bool spawnsChild = false;
  bool drawsTexture = false;
  int lights = 0;
  uint32_t maxp = 0;
  uint32_t lfot = 0;
  uint32_t lfor = 0;
  for (const RetailPartProperty& property : root) {
    const uint32_t word = property.value.size() == 8 ? Be32(property.value.data() + 4) : 0;
    lights += property.fourcc == EffectFourCC("LTYP");
    maxp = property.fourcc == EffectFourCC("MAXP") ? word : maxp;
    lfot = property.fourcc == EffectFourCC("LFOT") ? word : lfot;
    lfor = property.fourcc == EffectFourCC("LFOR") ? word : lfor;
    if (property.fourcc == EffectFourCC("ICTS")) {
      spawnsChild = property.value.size() == 8 && Be32(property.value.data() + 4) == 0x00ABC000;
    }
    if (property.fourcc == EffectFourCC("TEXR")) {
      drawsTexture = property.value.size() == 12 && Be32(property.value.data() + 8) == 0x00ABC001;
    }
  }
  Check(spawnsChild, "the root spawns its child by the child's new id");
  Check(drawsTexture, "the root draws its material's texture by its new id");
  Check(lights == 1 && lfot == 2 && lfor == 0x40400000u, "the root's light is the disc's");
  Check(maxp == 5, "the root keeps its other properties");
  const std::vector<uint8_t>& txtr = written["00ABC001.TXTR"];
  Check(txtr.size() > 12 && Be32(txtr.data()) == 9 && (txtr[4] << 8 | txtr[5]) == 64 && (txtr[6] << 8 | txtr[7]) == 64,
        "the texture's TXTR is an RGBA8 stub of 64");
}
// A generator whose material has no texture that converts would lose its
// TEXR: the import keeps the disc's PART and writes nothing.
void TestLostTexture() {
  const EffectGuid material = Fresh(0x10);
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "MTIN", 1);
  genp.push_back(0);
  PutGuid(genp, material);
  PutProperty(genp, "_END", 4);
  Put32(genp, 0);

  const EffectGuid effectId = Swap(Legacy(0x1234));
  std::map<std::string, std::vector<uint8_t>> written;
  std::vector<std::string> log;
  EffectImportIO io;
  io.effects = {effectId};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [](const EffectGuid&) -> uint32_t { return 0; };
  io.retailId = [](uint32_t id) { return id == 0x1234; };
  uint32_t next = 0x00ABC000;
  io.freshId = [&](uint32_t) { return next++; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  io.log = [&](const std::string& line) { log.push_back(line); };
  const EffectImportResult result = ImportEffects(io);
  Check(result.candidates == 1 && result.written == 0 && result.failed == 1, "an effect that lost its texture fails");
  Check(written.empty(), "nothing is written for it");
  Check(log.size() == 1 && log[0] == "00001234.PART: the root has no texture, the disc's is kept",
        "the log says the disc's PART is kept");
}

// An effect Remastered gave a fresh id replaces the retail PART of the same
// pak name (BombExplo, 1EF973EA).
void TestNamedEffect() {
  std::vector<uint8_t> genp(0x3c, 0);
  std::memcpy(genp.data(), "RFRM", 4);
  std::memcpy(genp.data() + 0x14, "GENP", 4);
  PutGenerator(genp, true);
  PutProperty(genp, "MAXP", 1);
  PutFourCC(genp, "CNST");
  Put32(genp, 5);
  PutProperty(genp, "_END", 4);
  Put32(genp, 0);

  // fb4d5181-cd7e-4c5e-8b11-afe35d30e231, in pak order.
  const EffectGuid effectId = {0xfb, 0x4d, 0x51, 0x81, 0xcd, 0x7e, 0x4c, 0x5e,
                               0x8b, 0x11, 0xaf, 0xe3, 0x5d, 0x30, 0xe2, 0x31};
  std::map<std::string, std::vector<uint8_t>> written;
  EffectImportIO io;
  io.effects = {effectId, Fresh(0x60)};
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    if (type == kGenp && id == effectId) {
      out = genp;
      return true;
    }
    error = "not in the image";
    return false;
  };
  io.typeOf = [](const EffectGuid&) -> uint32_t { return 0; };
  io.retailId = [](uint32_t id) { return id == 0x1EF973EA; };
  io.freshId = [](uint32_t) { return 0x00ABC000u; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    written[name] = data;
    return true;
  };
  const EffectImportResult result = ImportEffects(io);
  Check(result.candidates == 1 && result.written == 1, "the named effect is a candidate and written");
  Check(written.size() == 1 && written.count("1EF973EA.PART") == 1, "it replaces the PART of its name");
}
} // namespace

int main() {
  TestImport();
  TestLostTexture();
  TestNamedEffect();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::printf("port_remastered_effect_import: ok\n");
  return 0;
}
