#include "port_remastered_effect_convert.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace PortRemastered;

std::vector<ConvertedPart> ConvertOne(const std::vector<uint8_t>& data, const EffectConvertIO& io);

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

// Remastered side: little-endian, FourCCs byte-reversed.
void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

uint32_t Bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
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

void PutConstant(std::vector<uint8_t>& out, uint32_t word) {
  PutFourCC(out, "CNST");
  Put32(out, word);
}

void PutGenerator(std::vector<uint8_t>& out, bool root) {
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, root ? 1 : 0);
}

void PutGuid(std::vector<uint8_t>& out, const EffectGuid& guid) { out.insert(out.end(), guid.begin(), guid.end()); }

// An id carried over from retail, as an effect stores it.
EffectGuid Legacy(uint32_t retail) {
  EffectGuid guid = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  guid[12] = uint8_t(retail >> 24);
  guid[13] = uint8_t(retail >> 16);
  guid[14] = uint8_t(retail >> 8);
  guid[15] = uint8_t(retail);
  return guid;
}

EffectGuid Fresh(uint8_t seed) {
  EffectGuid guid;
  for (size_t i = 0; i < guid.size(); ++i) {
    guid[i] = uint8_t(0xa0 + seed + i);
  }
  return guid;
}

// A texture carried over from retail, so the generator draws something.
void PutTexr(std::vector<uint8_t>& out, uint32_t retail) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  PutGuid(out, Legacy(retail));
  PutFourCC(out, "NONE");
}

// Retail side: big-endian.
struct Retail {
  std::vector<uint8_t> bytes;
  Retail& f(const char* text) {
    bytes.insert(bytes.end(), text, text + 4);
    return *this;
  }
  // The end of a converted PART: the port-only PIRN marker, then _END.
  Retail& end() { return f("PIRN").f("CNST").w(1).f("_END"); }
  Retail& w(uint32_t value) {
    for (int i = 3; i >= 0; --i) {
      bytes.push_back(uint8_t(value >> (i * 8)));
    }
    return *this;
  }
  Retail& b(uint8_t value) {
    bytes.push_back(value);
    return *this;
  }
};

// A root using most of what the converter undoes, with one embedded child.
std::vector<uint8_t> Effect() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "DVVN", 1);  // Remastered only
  out.push_back(4);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 20);
  PutProperty(out, "LTM2", 1);
  PutConstant(out, 30);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "MULT");
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(3.0f));
  PutProperty(out, "ROTA", 3);
  PutConstant(out, Bits(5.0f));
  PutProperty(out, "ZBUF", 0);
  out.push_back(1);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "KEYE");
  Put32(out, 1);   // percent
  Put32(out, 0);   // unknown
  out.push_back(0);  // loop
  out.push_back(0);  // unknown
  Put32(out, 9);   // loop end
  Put32(out, 0);   // loop start
  Put32(out, 2);   // keys
  for (float c : {1.0f, 0.5f, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
    Put32(out, Bits(c));
  }
  PutProperty(out, "VEL1", 3);
  PutFourCC(out, "MPCB");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.0f));
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(0.0f));
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  PutGuid(out, Legacy(0x1234ABCD));
  PutFourCC(out, "NONE");
  PutProperty(out, "PMDL", 0);
  PutGuid(out, Fresh(0));  // no retail id: dropped
  PutProperty(out, "ICTS", 0);
  PutGuid(out, Legacy(0x0BADF00D));
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0BADF00D));
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 5);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(1));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

constexpr EffectGuid kPlasmaNestedLight = {0x1f, 0x70, 0x11, 0xf9, 0x15, 0xec, 0x8b, 0x45,
                                            0xae, 0x13, 0xca, 0x2b, 0xf6, 0x5d, 0x42, 0xf6};

void PutRealKey(std::vector<uint8_t>& out, float first, float second) {
  PutFourCC(out, "KEYE");
  Put32(out, 1); // percent-keyed
  Put32(out, 0);
  out.push_back(0);
  out.push_back(0);
  Put32(out, 9);
  Put32(out, 0);
  Put32(out, 2);
  Put32(out, Bits(first));
  Put32(out, Bits(second));
}

std::vector<uint8_t> NestedLightEffect(const EffectGuid& childId, uint8_t lightType = 2,
                                       uint8_t falloff = 3, bool includeRadii = true) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, childId);
  PutGenerator(out, false);
  PutProperty(out, "LTYP", 0);
  out.push_back(lightType);
  PutProperty(out, "LFOT", 0);
  out.push_back(falloff);
  PutProperty(out, "LCLR", 0);
  PutFourCC(out, "CNST");
  for (float value : {0.921881f, 0.151383f, 0.0102983f, 1.f}) {
    PutConstant(out, Bits(value));
  }
  PutProperty(out, "LINT", 0);
  PutFourCC(out, "MULT");
  PutConstant(out, Bits(20.f));
  PutRealKey(out, 0.f, 1.f);
  PutProperty(out, "EMTR", 0);
  PutFourCC(out, "SEMR");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.1f));
  PutConstant(out, Bits(0.f));
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.f));
  PutProperty(out, "PMOP", 0);
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.f));
  PutConstant(out, Bits(0.2f));
  PutConstant(out, Bits(0.f));
  if (includeRadii) {
    PutProperty(out, "LIRD", 0);
    PutConstant(out, Bits(0.25f));
    PutProperty(out, "LORD", 0);
    PutConstant(out, Bits(3.f));
  }
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

void TestPlasmaNestedHdrLight() {
  std::string error;
  std::vector<RetailPartProperty> properties;
  const auto parts = ConvertOne(NestedLightEffect(kPlasmaNestedLight), {});
  Check(parts.size() == 2, "the Plasma nested-light fixture converts root and child");
  if (parts.size() == 2) {
    Check(SplitRetailPart(parts[1].part.data(), parts[1].part.size(), properties, error),
          "the marked child with PHDR parses as a PART");
    auto find = [&properties](const char* name) -> const RetailPartProperty* {
      const uint32_t fourcc = uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 |
                              uint32_t(uint8_t(name[2])) << 8 | uint32_t(uint8_t(name[3]));
      for (const RetailPartProperty& property : properties) {
        if (property.fourcc == fourcc) {
          return &property;
        }
      }
      return nullptr;
    };
    const RetailPartProperty* marker = find("PHDR");
    const RetailPartProperty* inner = find("PIR0");
    const RetailPartProperty* outer = find("POR0");
    Check(marker != nullptr && marker->value.size() == 8 && std::memcmp(marker->value.data(), "CNST", 4) == 0 &&
              marker->value[7] == 1,
          "PHDR opts only the exact child into nested PBR lighting");
    Check(inner != nullptr && inner->value.size() == 8 && std::memcmp(inner->value.data(), "CNST", 4) == 0 &&
              ((uint32_t(inner->value[4]) << 24 | uint32_t(inner->value[5]) << 16 |
                uint32_t(inner->value[6]) << 8 | inner->value[7]) == Bits(0.25f)),
          "PIR0 preserves LIRD as the real inner radius");
    Check(outer != nullptr && outer->value.size() == 8 && std::memcmp(outer->value.data(), "CNST", 4) == 0 &&
              ((uint32_t(outer->value[4]) << 24 | uint32_t(outer->value[5]) << 16 |
                uint32_t(outer->value[6]) << 8 | outer->value[7]) == Bits(3.f)),
          "POR0 preserves LORD as the real outer radius");
    const RetailPartProperty* intensity = find("LINT");
    Check(intensity != nullptr && intensity->value.size() >= 16 &&
              std::memcmp(intensity->value.data(), "MULT", 4) == 0,
          "the source 20-times-keyed LINT element remains dynamic");
    Check(find("EMTR") != nullptr && find("PMOP") != nullptr && find("LOFF") == nullptr,
          "the .1 EMTR and .2 PMOP offsets remain non-light particle data");
    Check(find("LTYP") != nullptr && find("LFOT") != nullptr,
          "the legacy GX fallback still receives the mapped retail light properties");
    Check(!parts[0].part.empty() && std::search(parts[0].part.begin(), parts[0].part.end(), "PHDR", "PHDR" + 4) ==
                                          parts[0].part.end(),
          "the unmarked root does not get a duplicate nested-light proxy");
  }

  const auto wrongGuid = ConvertOne(NestedLightEffect(Fresh(7)), {});
  Check(wrongGuid.size() == 2 &&
            std::search(wrongGuid[1].part.begin(), wrongGuid[1].part.end(), "PHDR", "PHDR" + 4) ==
                wrongGuid[1].part.end(),
        "the same light properties on another child do not enable the feature");
  const auto wrongType = ConvertOne(NestedLightEffect(kPlasmaNestedLight, 1, 3), {});
  Check(wrongType.size() == 2 &&
            std::search(wrongType[1].part.begin(), wrongType[1].part.end(), "PHDR", "PHDR" + 4) ==
                wrongType[1].part.end(),
        "a non-point LTYP does not enable the feature");
  const auto wrongFalloff = ConvertOne(NestedLightEffect(kPlasmaNestedLight, 2, 2), {});
  Check(wrongFalloff.size() == 2 &&
            std::search(wrongFalloff[1].part.begin(), wrongFalloff[1].part.end(), "PHDR", "PHDR" + 4) ==
                wrongFalloff[1].part.end(),
        "a non-smooth LFOT does not enable the feature");
  const auto missingRadius = ConvertOne(NestedLightEffect(kPlasmaNestedLight, 2, 3, false), {});
  Check(missingRadius.size() == 2 &&
            std::search(missingRadius[1].part.begin(), missingRadius[1].part.end(), "PHDR", "PHDR" + 4) ==
                missingRadius[1].part.end(),
        "missing validated LIRD/LORD leaves the child unmarked");
}

void TestRetailId() {
  Check(EffectRetailId(Legacy(0x0034CE07)) == 0x0034CE07u, "carried-over id resolves");
  Check(!EffectRetailId(Fresh(0)).has_value(), "a fresh id has no retail id");
}

void TestConvert() {
  const std::vector<uint8_t> data = Effect();
  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }

  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(1) ? 0x5EED0001u : 0u; };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 2, "root and child");
  if (parts.size() != 2) {
    return;
  }

  Retail root;
  root.f("GPSM");
  root.f("MAXP").f("CNST").w(20);
  root.f("LTME").f("CNST").w(29);  // LTM2 is one frame longer
  root.f("SIZE").f("MULT").f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(3.0f));
  root.f("ROTA").f("CNST").w(Bits(-5.0f));
  root.f("ZBUF").f("CNST").b(1);
  root.f("COLR").f("KEYE").w(1).w(0).b(0).b(0).w(9).w(0).w(2);
  for (float c : {1.0f, 0.5f, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
    root.w(Bits(c));
  }
  root.f("VEL1").f("CNST").f("CNST").w(Bits(0.0f)).f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(0.0f));
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  root.f("ICTS").f("CNST").w(0x0BADF00D);
  root.end();
  Check(parts[0].root && parts[0].part == root.bytes, "root converts to retail's bytes");
  Check(parts[0].droppedRetail == 1, "PMDL with no retail id is the one retail property dropped");
  Check(parts[0].dropped.size() == 2, "DVVN and PMDL dropped");

  Retail child;
  child.f("GPSM");
  child.f("MAXP").f("CNST").w(5);
  child.f("TEXR").f("CNST").f("CNST").w(0x5EED0001);
  child.end();
  Check(!parts[1].root && parts[1].id == Legacy(0x0BADF00D), "child keeps its id");
  Check(parts[1].part == child.bytes, "MTIN becomes the child's TEXR");
  Check(parts[1].droppedRetail == 0, "child drops nothing");

  // What the converter writes reads as retail reads it.
  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    Check(SplitRetailPart(part.part.data(), part.part.size(), properties, error), "converted PART reads as retail");
  }
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(root.bytes.data(), root.bytes.size(), properties, error) && properties.size() == 10 &&
            properties[5].fourcc == EffectFourCC("COLR") && properties[5].value.size() == 4 + 22 + 32,
        "retail PART splits into its properties");
}

// The splitter refuses what retail's reader would not take.
void TestSplitRejects() {
  Retail bad;
  bad.f("GPSM").f("SIZE").f("RADD").f("_END");
  std::vector<RetailPartProperty> properties;
  std::string error;
  Check(!SplitRetailPart(bad.bytes.data(), bad.bytes.size(), properties, error) && !error.empty(),
        "unknown element refused");
  Retail spawn;
  spawn.f("GPSM").f("KSSM").f("CNST").w(0).w(0).w(10).w(0).w(1).w(3).w(1).w(0xAABBCCDD).w(0).w(0).w(0).f("_END");
  Check(SplitRetailPart(spawn.bytes.data(), spawn.bytes.size(), properties, error) && properties.size() == 1,
        "KSSM spawn table reads");
}

// An effect with no children has its root flag clear but is still the
// effect's own PART; a colour's keys stored as halves come out as floats; a
// spawn table counts as a retail property left out.
void TestSingleNode() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "KEYE");
  Put32(out, 1);
  Put32(out, 0);
  out.push_back(0);
  out.push_back(0);
  Put32(out, 9);
  Put32(out, 0);
  Put32(out, 1);
  for (uint16_t half : {0x3c00, 0x3800, 0x0000, 0xbc00}) {  // 1, 0.5, 0, -1
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  PutProperty(out, "KSSM", 0);
  PutFourCC(out, "NONE");
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "single-node effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 1 && parts[0].root, "the only GPSM is the root");
  Retail want;
  want.f("GPSM").f("COLR").f("KEYE").w(1).w(0).b(0).b(0).w(9).w(0).w(1);
  want.w(Bits(1.0f)).w(Bits(0.5f)).w(Bits(0.0f)).w(Bits(-1.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "half colour keys widen to floats");
  Check(parts.size() == 1 && parts[0].dropped.empty(), "an empty KSSM is nothing left out");
}

// Remastered's new elements that have a retail equivalent: MPCB's angle form
// and an unrotated ANCR become ANGC, MPRD becomes RAND (and a random LTM2
// comes down by one at both ends), DFCP is kept as the port's element; a vector of nested
// constants is not read as an id.
void TestMappedElements() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "EMTR", 3);
  PutFourCC(out, "SEMR");
  PutFourCC(out, "MPCB");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutFourCC(out, "MPCB");
  PutFourCC(out, "MPAC");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, Bits(720.0f));
  PutConstant(out, Bits(720.0f));
  PutConstant(out, Bits(0.1f));
  PutProperty(out, "LTM2", 3);
  PutFourCC(out, "MPRD");
  PutConstant(out, 17);
  PutConstant(out, 33);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "MULT");
  PutConstant(out, Bits(2.0f));
  PutFourCC(out, "DFCP");
  PutConstant(out, Bits(2.0f));
  PutConstant(out, 0);
  PutProperty(out, "POFS", 0);
  PutFourCC(out, "ANCR");
  PutFourCC(out, "REUL");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
  PutConstant(out, Bits(360.0f));
  PutConstant(out, Bits(360.0f));
  PutConstant(out, Bits(0.5f));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "mapped-element effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("EMTR").f("SEMR").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  want.f("ANGC").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(720.0f)).f("CNST").w(Bits(720.0f)).f("CNST").w(Bits(0.1f));
  want.f("LTME").f("RAND").f("CNST").w(16).f("CNST").w(32);
  want.f("SIZE").f("MULT").f("CNST").w(Bits(2.0f)).f("DFCP").f("CNST").w(Bits(2.0f)).f("CNST").w(0);
  want.f("POFS").f("ANGC").f("CNST").w(0x80000000u).f("CNST").w(0x80000000u);
  want.f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(0.5f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "new elements map onto retail's");
  if (parts.size() == 1 && parts[0].part != want.bytes) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
  }
  Check(parts.size() == 1 && parts[0].approximated.empty(), "DFCP is kept, not approximated");
}

// Properties retail reads as something else than Remastered wrote are left out.
void TestRejects() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "RADD");  // Remastered's, not retail's
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "LFOT", 0);
  out.push_back(3);
  PutProperty(out, "LTYP", 0);
  out.push_back(1);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "reject effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "RADD in SIZE is dropped");
  Retail root;
  root.f("GPSM").f("LFOT").f("CNST").w(3).f("LTYP").f("CNST").w(2);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == root.bytes, "LFOT byte 3 is retail 3, LTYP byte 1 is retail 2");
}
}  // namespace

void PutReul(std::vector<uint8_t>& out, float x) {
  PutFourCC(out, "REUL");
  PutConstant(out, Bits(x));
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
}

// Remastered's shapes with no retail element of their own: ASPR becomes ASPH
// and a cone turned about X an X bias, RNDV a whole-sphere ANGC, and a GRAD
// colour gradient over the life 101 percent keys.
void TestShapes() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "EMTR", 3);
  PutFourCC(out, "ASPR");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutReul(out, -90.0f);
  PutConstant(out, Bits(360.0f));
  PutConstant(out, Bits(180.0f));
  PutConstant(out, Bits(0.25f));
  PutConstant(out, Bits(0.1f));
  PutProperty(out, "PSIV", 3);
  PutFourCC(out, "RNDV");
  PutConstant(out, Bits(0.5f));
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "GRAD");
  out.push_back(2);
  for (uint16_t half : {0x3c00, 0x0000, 0x0000, 0x3c00}) {  // red, opaque, at 0
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  Put32(out, Bits(0.0f));
  for (uint16_t half : {0x0000, 0x0000, 0x3c00, 0x0000}) {  // blue, clear, at 1
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  Put32(out, Bits(1.0f));
  PutFourCC(out, "ILPT");
  PutConstant(out, 100);
  out.push_back(0);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "shapes effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM");
  want.f("EMTR").f("ASPH").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  want.f("CNST").w(Bits(90.0f)).f("CNST").w(0x80000000u);
  want.f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(180.0f)).f("CNST").w(Bits(0.25f)).f("CNST").w(Bits(0.1f));
  want.f("PSIV").f("ANGC").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(360.0f)).f("CNST").w(Bits(360.0f));
  want.f("CNST").w(Bits(0.5f));
  want.f("COLR").f("KEYP").w(1).w(0).b(0).b(0).w(101).w(0).w(101);
  for (int percent = 0; percent <= 100; ++percent) {
    const float t = float(percent) / 100.0f;
    want.w(Bits(1.0f - t)).w(Bits(0.0f)).w(Bits(t)).w(Bits(1.0f - t));
  }
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "ASPR, RNDV and GRAD map onto retail's");
  if (parts.size() == 1) {
    for (const std::string& d : parts[0].dropped) {
      std::fprintf(stderr, "  dropped %s\n", d.c_str());
    }
    std::vector<RetailPartProperty> split;
    Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), split, error), "shapes PART reads as retail");
    Check(parts[0].approximated.size() == 2, "rotated cone and RNDV listed as approximated");
  }
}

// A gradient over as many frames as the particle lives (LTM2) ends with the
// life: its last key is the last stop.
void TestGradientFrames() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "LTM2", 3);
  PutConstant(out, 20);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "GRAD");
  out.push_back(2);
  for (int stop = 0; stop < 2; ++stop) {
    for (int c = 0; c < 4; ++c) {
      const uint16_t half = stop == 0 ? 0x3c00 : 0x0000;
      out.push_back(uint8_t(half));
      out.push_back(uint8_t(half >> 8));
    }
    Put32(out, Bits(float(stop)));
  }
  PutConstant(out, 20);
  out.push_back(0);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "frame gradient parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Retail want;
  want.f("GPSM").f("LTME").f("CNST").w(19).f("COLR").f("KEYP").w(1).w(0).b(0).b(0).w(101).w(0).w(101);
  for (int percent = 0; percent <= 100; ++percent) {
    const float t = float(percent) / 100.0f;
    for (int c = 0; c < 4; ++c) {
      want.w(Bits(1.0f - t));
    }
  }
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a gradient over the life in frames ends with it");
}

// Swoosh and electric children come out as retail SWHC and ELSC under their
// own ids: SBDM becomes the swoosh's AALP, MTIN its TEXR, a zero id NONE. An
// ELC2 has no texture, so its MTIN is left out.
void TestSwooshElectric() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 3);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000AAAA));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 3);
  PutGuid(out, Legacy(0x0000AAAA));
  PutFourCC(out, "SWSH");
  PutProperty(out, "LENG", 1);
  PutConstant(out, 8);
  PutProperty(out, "SBDM", 1);
  PutConstant(out, 1);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(2));
  PutProperty(out, "_END", 4);
  PutGuid(out, Fresh(3));
  PutFourCC(out, "ELSM");
  PutProperty(out, "LWD1", 3);
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000AAAA));
  PutProperty(out, "GPSM", 0);
  PutGuid(out, EffectGuid{});
  PutProperty(out, "_END", 4);
  PutGuid(out, Fresh(4));
  PutFourCC(out, "ELC2");
  PutProperty(out, "SCNT", 1);
  PutConstant(out, 4);
  PutProperty(out, "SIDE", 1);
  PutConstant(out, 3);
  PutProperty(out, "RADS", 3);
  PutConstant(out, Bits(0.5f));
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(2));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});

  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "swoosh and electric effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(2) ? 0x5EED0002u : 0u; };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), io);
  Check(parts.size() == 4, "root and three children");
  if (parts.size() != 4) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    return;
  }

  Retail root;
  root.f("GPSM").f("MAXP").f("CNST").w(3).f("SSWH").f("CNST").w(0x0000AAAA);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts[0].type == EffectFourCC("PART") && parts[0].part == root.bytes, "root references the swoosh by id");

  Retail swoosh;
  swoosh.f("SWSH").f("LENG").f("CNST").w(8).f("AALP").f("CNST").b(1);
  swoosh.f("TEXR").f("CNST").f("CNST").w(0x5EED0002).f("PIRN").f("CNST").w(1).f("_END");
  Check(parts[1].type == EffectFourCC("SWHC") && parts[1].id == Legacy(0x0000AAAA), "swoosh child is an SWHC");
  Check(parts[1].part == swoosh.bytes, "SBDM becomes AALP and MTIN the swoosh's TEXR");
  Check(parts[1].droppedRetail == 0, "swoosh drops nothing");

  Retail electric;
  electric.f("ELSM").f("LWD1").f("CNST").w(Bits(2.0f)).f("SSWH").f("CNST").w(0x0000AAAA).f("GPSM").f("NONE").f("_END");
  Check(parts[2].type == EffectFourCC("ELSC") && parts[2].part == electric.bytes, "ELSM child is an ELSC");

  // An ELC2 child is an ELSC stream led by the port-only PEL2 marker; its fields follow.
  Retail elc2;
  elc2.f("ELSM").f("PEL2").f("CNST").w(1).f("SCNT").f("CNST").w(4);
  elc2.f("SIDE").f("CNST").w(3).f("RADS").f("CNST").w(Bits(0.5f)).f("_END");
  Check(parts[3].type == EffectFourCC("ELSC") && parts[3].part == elc2.bytes, "ELC2 child is an ELSC with PEL2");
  Check(parts[3].droppedRetail == 0, "ELC2 drops no retail property");

  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    Check(SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error),
          "converted child reads as retail");
  }
  std::vector<RetailPartProperty> properties;
  Check(!SplitRetailEffect(EffectFourCC("SWHC"), root.bytes.data(), root.bytes.size(), properties, error),
        "a PART is not an SWHC");
  Retail wrong;
  wrong.f("SWSH").f("MAXP").f("CNST").w(3).f("_END");
  Check(!SplitRetailEffect(EffectFourCC("SWHC"), wrong.bytes.data(), wrong.bytes.size(), properties, error),
        "a PART property in a swoosh refused");
  Check(SplitRetailEffect(EffectFourCC("ELSC"), electric.bytes.data(), electric.bytes.size(), properties, error) &&
            properties.size() == 3,
        "retail ELSC splits into its properties");
}

// Remastered's spawn tables merge into retail's one KSSM: generators by frame
// with the 16-byte entries retail reads, the first swoosh and electric child
// as SSWH/SSSD and SELC/SESD; further swooshes go to the port-only PSWX.
void TestSpawnTable() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "KSSM", 0);
  PutFourCC(out, "CNST");
  for (uint32_t word : {1u, 1u, 40u, 0u}) {  // header, end frame, no events
    Put32(out, word);
  }
  Put32(out, 2);  // tables
  const auto spawn = [&](uint32_t retail, const char* form, bool condition) {
    PutGuid(out, Legacy(retail));
    PutFourCC(out, form);
    Put32(out, 0);
    if (condition) {
      PutConstant(out, Bits(0.5f));
    } else {
      PutFourCC(out, "NONE");
    }
  };
  Put32(out, 0);  // table word
  PutConstant(out, 0);  // selector
  Put32(out, 2);  // frames
  Put32(out, 3);
  Put32(out, 2);
  spawn(0x0000B001, "GENP", false);
  spawn(0x0000B002, "SWSH", false);
  Put32(out, 7);
  Put32(out, 1);
  spawn(0x0000B004, "SWSH", false);
  Put32(out, 0);
  PutConstant(out, 1);  // another selector
  Put32(out, 1);
  Put32(out, 3);
  Put32(out, 2);
  spawn(0x0000B001, "GENP", true);
  spawn(0x0000B003, "ELSM", false);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 4);
  PutGuid(out, Legacy(0x0000B001));
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 2);
  PutProperty(out, "_END", 4);
  for (uint32_t retail : {0x0000B002u, 0x0000B004u}) {
    PutGuid(out, Legacy(retail));
    PutFourCC(out, "SWSH");
    PutProperty(out, "LENG", 1);
    PutConstant(out, 8);
    PutProperty(out, "_END", 4);
  }
  PutGuid(out, Legacy(0x0000B003));
  PutFourCC(out, "ELSM");
  PutProperty(out, "LWD1", 3);
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});

  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "spawn table effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 5, "root and four children");
  if (parts.size() != 5) {
    std::fprintf(stderr, "%s", DumpEffect(effect, out.data()).c_str());
    return;
  }
  Retail root;
  root.f("GPSM").f("KSSM").f("CNST").w(0).w(1).w(40).w(0).w(1);
  root.w(3).w(2).w(0x0000B001).w(0).w(0).w(0).w(0x0000B001).w(0).w(0).w(0);
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  root.f("SSWH").f("CNST").w(0x0000B002).f("SSSD").f("CNST").w(3);
  root.f("SELC").f("CNST").w(0x0000B003).f("SESD").f("CNST").w(3);
  root.f("PSWX").f("CNST").w(1).w(0x0000B004).w(7);  // the second swoosh, port-only
  root.end();
  Check(parts[0].part == root.bytes, "tables merge into retail's KSSM, SSWH, SELC and PSWX");
  Check(parts[0].dropped.empty() && parts[0].droppedRetail == 0, "the second swoosh is kept as PSWX");
  bool merged = false;
  bool selected = false;
  bool conditional = false;
  for (const std::string& note : parts[0].approximated) {
    merged = merged || note == "KSSM: 2 tables merged";
    selected = selected || note == "KSSM: table selector ignored";
    conditional = conditional || note.find("conditions ignored") != std::string::npos;
  }
  Check(merged && selected && conditional, "merge, selector and condition noted");
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) && properties.size() == 8,
        "converted spawn table reads as retail");
}

// A one-generator effect whose body (properties) `body` writes.
template <typename Body>
std::vector<uint8_t> OneGenerator(Body body) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  body(out);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

void PutTxfb(std::vector<uint8_t>& out, bool trst, float scale, bool mirror, float lifeHigh = 1.0f) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "TXFB");
  PutGuid(out, Fresh(5));
  PutFourCC(out, "LFTW");
  PutConstant(out, 0);
  PutConstant(out, Bits(lifeHigh));
  if (!trst) {
    return;
  }
  PutFourCC(out, "TRST");
  PutConstant(out, 0);
  PutConstant(out, 0);
  if (mirror) {
    PutFourCC(out, "KPIN");
    PutFourCC(out, "CREL");
    PutFourCC(out, "LTHN");
    PutFourCC(out, "RAND");
    PutConstant(out, 0);
    PutConstant(out, Bits(1.0f));
    PutConstant(out, Bits(0.5f));
    PutConstant(out, Bits(1.0f));
    PutConstant(out, Bits(scale));
  } else {
    PutConstant(out, Bits(scale));
  }
  PutConstant(out, Bits(1.0f));
  PutConstant(out, 0);
  Put32(out, 0xFFFFFFFFu);
}

void PutTxp2(std::vector<uint8_t>& out, uint32_t cols, uint32_t rows, uint32_t high) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "TXP2");
  PutGuid(out, Fresh(6));
  PutConstant(out, cols);
  PutConstant(out, rows);
  PutFourCC(out, "IRND");
  PutConstant(out, 0);
  PutConstant(out, high);
}

void PutAtx2(std::vector<uint8_t>& out, uint32_t cols, uint32_t percent) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "ATX2");
  PutGuid(out, Fresh(6));
  PutConstant(out, cols);
  PutConstant(out, 4);
  PutFourCC(out, "ILPT");
  PutConstant(out, percent);
  PutConstant(out, 1);
}

void PutModels(std::vector<uint8_t>& out, uint32_t count, uint32_t high) {
  PutProperty(out, "PMDL", 0);
  PutFourCC(out, "SLCT");
  PutFourCC(out, "IRND");
  PutConstant(out, 0);
  PutConstant(out, high);
  PutFourCC(out, "ARRY");
  Put32(out, count);
  for (uint32_t i = 0; i < count; ++i) {
    PutFourCC(out, "CNST");
    PutGuid(out, Fresh(uint8_t(10 + i)));
  }
}

EffectConvertIO AtlasIO() {
  EffectConvertIO io;
  io.assetId = [](const EffectGuid& id, uint32_t type) -> uint32_t {
    for (uint8_t i = 0; i < 16; ++i) {
      if (id == Fresh(i)) {
        return (type == EffectFourCC("CMDL") ? 0xC0DE0000u : 0x7E570000u) + i;
      }
    }
    return 0;
  };
  io.flipbook = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(5)) {
      atlas.id = 0xF11B0001u;
      atlas.cols = 8;
      atlas.rows = 4;
      atlas.frames = 32;
    }
    return atlas;
  };
  return io;
}

std::vector<ConvertedPart> ConvertOne(const std::vector<uint8_t>& data, const EffectConvertIO& io) {
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: effect does not parse: %s\n", error.c_str());
    ++sFailures;
    return {};
  }
  return ConvertEffect(effect, data.data(), io);
}

Retail Patl(uint32_t id, int cols, int rows, int count, int mode, int flip) {
  Retail want;
  want.f("GPSM").f("TEXR").f("PATL").f("CNST").w(id);
  for (const int v : {cols, rows, count, mode, flip}) {
    want.f("CNST").w(uint32_t(v));
  }
  want.end();
  return want;
}

// TXP2's atlas of a random tile and TXFB's flipbook become the port's PATL; any
// other form of either is left out of the PART.
void TestAtlasTexture() {
  const EffectConvertIO io = AtlasIO();
  std::string error;
  std::vector<RetailPartProperty> properties;

  // The tile range is a real over the whole atlas (1f), so 4 x 4 is 16 tiles.
  auto parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 4, Bits(1.0f)); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 4, 4, 16, 0, 0).bytes, "TXP2 becomes a random-tile PATL");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "TXP2 drops nothing");
  Check(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
            properties.size() == 2,
        "a PATL reads as retail");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 2, 5); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 4, 2, 6, 0, 0).bytes, "an int range is the last tile");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 4, 4, 99); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a tile range past the atlas is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxp2(o, 0, 4, Bits(1.0f)); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "an atlas of no columns is refused");

  // ATX2's cycle ILPT(100) is the particle's whole life.
  parts = ConvertOne(OneGenerator([](auto& o) { PutAtx2(o, 2, 100); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0x7E570006u, 2, 4, 8, 1, 0).bytes, "ATX2 becomes a life PATL");
  parts = ConvertOne(OneGenerator([](auto& o) { PutAtx2(o, 2, 50); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "an ATX2 over half the life is refused");

  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, -1.0f, true); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 1).bytes,
        "TXFB with a random mirror is a flipped flipbook");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, 1.0f, false); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 0).bytes,
        "TXFB with an identity TRST is not flipped");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false); }), io);
  Check(parts.size() == 1 && parts[0].part == Patl(0xF11B0001u, 8, 4, 32, 1, 0).bytes, "TXFB with no TRST");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, 0.5f, false); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a TRST scaling x is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, true, -2.0f, true); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a mirror to another scale is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false, 0.5f); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a flipbook over half the life is refused");
  EffectConvertIO bare = io;
  bare.flipbook = nullptr;
  parts = ConvertOne(OneGenerator([](auto& o) { PutTxfb(o, false, 1.0f, false); }), bare);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "no atlas, no flipbook");

  Retail badCount;
  badCount.f("GPSM").f("TEXR").f("PATL").f("CNST").w(1).f("CNST").w(4).f("CNST").w(4).f("_END");
  Check(!SplitRetailPart(badCount.bytes.data(), badCount.bytes.size(), properties, error), "a short PATL is refused");
}

// PMDL's random choice of four models becomes the first as PMDL and all as PMDV.
void TestModelChoice() {
  const EffectConvertIO io = AtlasIO();
  std::string error;
  std::vector<RetailPartProperty> properties;
  auto parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 3); }), io);
  Retail want;
  want.f("GPSM").f("PMDL").f("CNST").w(0xC0DE000A).f("PMDV").f("CNST").w(4);
  for (uint32_t i = 0; i < 4; ++i) {
    want.f("CNST").w(0xC0DE000A + i);
  }
  // A models-only generator draws no quads: SIZE 0 is written for it.
  want.f("SIZE").f("CNST").w(Bits(0.0f));
  want.end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "SLCT of four models is PMDL and PMDV");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "the model choice drops nothing");
  Check(!parts.empty() && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error) &&
            properties.size() == 4,
        "PMDL, PMDV and SIZE read as retail");

  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 2); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "a range short of the array is refused");
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 0, 0); }), io);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "an empty array is refused");
  EffectConvertIO none;
  parts = ConvertOne(OneGenerator([](auto& o) { PutModels(o, 4, 3); }), none);
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "models with no ids are refused");

  Retail one;
  one.f("GPSM").f("PMDL").f("CNST").w(1).f("PMDV").f("CNST").w(2).f("CNST").w(1).f("_END");
  Check(!SplitRetailPart(one.bytes.data(), one.bytes.size(), properties, error), "a PMDV short of its count is refused");
  Retail alone;
  alone.f("GPSM").f("PMDV").f("CNST").w(1).f("CNST").w(1).f("_END");
  Check(!SplitRetailPart(alone.bytes.data(), alone.bytes.size(), properties, error) ||
            properties.size() == 1,
        "a PMDV parses on its own or is refused");
}

// A generator with no texture, material or model draws nothing: its SIZE is
// left out and a SIZE of 0 written before _END instead. One with a TEXR keeps
// its SIZE.
void TestDrawsNothing() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "MAXP", 1);
                 PutConstant(o, 4);
                 PutProperty(o, "SIZE", 3);
                 PutConstant(o, Bits(2.0f));
               }),
               {});
  Retail want;
  want.f("GPSM").f("MAXP").f("CNST").w(4).f("SIZE").f("CNST").w(Bits(0.0f)).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a generator with no texture gets SIZE 0");
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "SIZE: the generator draws nothing",
        "its SIZE is dropped as drawing nothing");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "drawing nothing drops no retail property");

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "MAXP", 1);
                 PutConstant(o, 4);
                 PutProperty(o, "SIZE", 3);
                 PutConstant(o, Bits(2.0f));
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail textured;
  textured.f("GPSM").f("MAXP").f("CNST").w(4).f("SIZE").f("CNST").w(Bits(2.0f));
  textured.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == textured.bytes, "a generator with a TEXR keeps its SIZE");
  Check(!parts.empty() && parts[0].dropped.empty(), "nothing dropped with a TEXR");
}

// PBDM is Remastered's blend mode: 2 is retail's additive AAPH, anything else
// alpha (1 and 3 noted as approximated).
void TestBlendMode() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PBDM", 0);
                 o.push_back(2);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("AAPH").f("CNST").b(1);
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "PBDM 2 becomes AAPH");
  Check(!parts.empty() && parts[0].approximated.empty(), "PBDM 2 is exact");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "PBDM 2 drops nothing");

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PBDM", 0);
                 o.push_back(1);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail alpha;
  alpha.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == alpha.bytes, "PBDM 1 writes nothing");
  Check(!parts.empty() && parts[0].approximated.size() == 1 &&
            parts[0].approximated[0] == "PBDM 1 taken as alpha blending",
        "PBDM 1 is listed as approximated");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "PBDM 1 drops nothing");
}

// GPUA (how much GPU time is free) is taken as 1.
void TestGpuAvailability() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "SIZE", 3);
                 PutFourCC(o, "GPUA");
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("SIZE").f("CNST").w(Bits(1.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "GPUA becomes CNST 1");
  Check(!parts.empty() && parts[0].approximated.size() == 1 && parts[0].approximated[0] == "GPUA taken as 1",
        "GPUA is listed as approximated");
}

// SPAF(index, default) reads an effect parameter the game passes in; retail
// passes none, so it is its default.
void TestParameterDefault() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "SIZE", 3);
                 PutFourCC(o, "SPAF");
                 o.push_back(5);
                 PutConstant(o, Bits(2.5f));
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("SIZE").f("CNST").w(Bits(2.5f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "SPAF becomes its default");
  Check(!parts.empty() && parts[0].approximated.size() == 1 &&
            parts[0].approximated[0] == "SPAF taken as its default",
        "SPAF is listed as approximated");
}

// A direct PMRQ REUL remains the existing PMRT CNST path, including its
// existing refusal to represent IRND.
void TestModelRotation() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PMRQ", 3);
                 PutFourCC(o, "REUL");
                 PutConstant(o, Bits(-90.0f));
                 PutConstant(o, Bits(0.0f));
                 PutFourCC(o, "SCAL");
                 PutConstant(o, Bits(-0.05f));
                 o.push_back(0);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("PMRT").f("CNST").f("CNST").w(Bits(-90.0f)).f("CNST").w(Bits(0.0f));
  want.f("SCAL").f("CNST").w(Bits(-0.05f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "PMRQ REUL becomes PMRT");
  Check(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(), "PMRQ REUL is exact");

  parts = ConvertOne(OneGenerator([](auto& o) {
                 PutProperty(o, "PMRQ", 3);
                 PutFourCC(o, "REUL");
                 PutConstant(o, Bits(90.0f));
                 PutFourCC(o, "IRND");
                 PutConstant(o, Bits(0.0f));
                 PutConstant(o, Bits(360.0f));
                 PutConstant(o, Bits(0.0f));
                 o.push_back(0);
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail plain;
  plain.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == plain.bytes, "PMRQ with IRND writes nothing");
  Check(!parts.empty() && parts[0].dropped.size() == 1 && parts[0].dropped[0] == "PMRQ: an angle with IRND",
        "PMRQ with IRND is listed as dropped");
}

void PutLoopingEmitterKey(std::vector<uint8_t>& out) {
  PutFourCC(out, "KEYE");
  Put32(out, 0);    // emitter time, not life percentage
  Put32(out, 0);    // unknown
  out.push_back(1); // loop
  out.push_back(0); // unknown
  Put32(out, 120);  // end
  Put32(out, 0);    // start
  Put32(out, 120);  // all source samples
  for (int i = 0; i < 120; ++i) {
    const float degrees = i == 119 ? 360.00003f : i == 0 ? 0.f : float(i) * 3.0252087f;
    Put32(out, Bits(degrees));
  }
}

void PutStaticEuler(std::vector<uint8_t>& out, float x, float y, float z, uint8_t order = 0) {
  PutFourCC(out, "REUL");
  PutConstant(out, Bits(x));
  PutConstant(out, Bits(y));
  PutConstant(out, Bits(z));
  out.push_back(order);
}

void TestComposedModelRotation() {
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                  PutProperty(o, "PMRQ", 3);
                  PutFourCC(o, "RADD");
                  PutFourCC(o, "REUL");
                  for (int i = 0; i < 3; ++i) {
                    PutFourCC(o, "IRND");
                    PutConstant(o, Bits(0.f));
                    PutConstant(o, Bits(360.f));
                  }
                  o.push_back(0);
                  PutFourCC(o, "REUL");
                  PutLoopingEmitterKey(o);
                  PutLoopingEmitterKey(o);
                  PutConstant(o, Bits(0.f));
                  o.push_back(0);
                  PutTexr(o, 0x1234ABCD);
                }),
                {});

  Retail want;
  want.f("GPSM").f("PMRT").f("PQAD");
  want.f("CNST");
  for (int i = 0; i < 3; ++i) {
    want.f("IRND").f("CNST").w(Bits(0.f)).f("CNST").w(Bits(360.f));
  }
  want.f("CNST");
  for (int axis = 0; axis < 2; ++axis) {
    want.f("KEYE").w(0).w(0).b(1).b(0).w(120).w(0).w(120);
    for (int i = 0; i < 120; ++i) {
      const float degrees = i == 119 ? 360.00003f : i == 0 ? 0.f : float(i) * 3.0252087f;
      want.w(Bits(degrees));
    }
  }
  want.f("CNST").w(Bits(0.f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes,
        "ordered PMRQ preserves IRND and all 120 looping KEYE samples in PQAD");
  Check(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(),
        "ordered PMRQ has no dropped or approximated rotation data");
  if (!parts.empty()) {
    std::vector<RetailPartProperty> properties;
    std::string error;
    Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
          "PQAD PMRT passes the retail element reader");
  }

  parts = ConvertOne(OneGenerator([](auto& o) {
                PutProperty(o, "PMRQ", 3);
                PutFourCC(o, "RADD");
                PutStaticEuler(o, 90.f, 0.f, 0.f, 1);
                PutStaticEuler(o, 0.f, 90.f, 0.f);
                PutTexr(o, 0x1234ABCD);
              }),
              {});
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "PMRQ: a rotation order other than 0 (XYZ)",
        "ordered PMRQ rejects an unsupported Euler order");

  parts = ConvertOne(OneGenerator([](auto& o) {
                PutProperty(o, "PMRQ", 3);
                PutFourCC(o, "RADD");
                PutStaticEuler(o, 90.f, 0.f, 0.f);
                PutFourCC(o, "CNST");
                PutConstant(o, Bits(0.f));
                PutConstant(o, Bits(90.f));
                PutConstant(o, Bits(0.f));
                PutTexr(o, 0x1234ABCD);
              }),
              {});
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "PMRQ: a rotation that is not REUL",
        "ordered PMRQ refuses a non-REUL operand rather than adding Euler components");
}

// A MATI with one shader and its texture parameters, as ParseMati reads it.
std::vector<uint8_t> Mati(uint32_t shader, const std::vector<std::pair<const char*, EffectGuid>>& textures) {
  std::vector<uint8_t> out(0x6d, 0);
  for (int i = 0; i < 4; ++i) {
    out[0x48 + size_t(i)] = uint8_t(shader >> (24 - 8 * i));
  }
  const uint32_t count = uint32_t(textures.size());
  for (int i = 0; i < 4; ++i) {
    out[0x69 + size_t(i)] = uint8_t(count >> (8 * i));
  }
  for (const auto& texture : textures) {
    out.push_back(6);
    out.insert(out.end(), texture.first, texture.first + 4);
    PutGuid(out, texture.second);
    Put32(out, 0);                // texCoord
    Put32(out, 1);                // filter
    Put32(out, 1);                // wrapX
    Put32(out, 1);                // wrapY
    Put32(out, 0xffffffffu);      // wrapZ
  }
  return out;
}

// A root generator whose material is `Fresh(7)`, with a blend mode and sprite centre.
std::vector<uint8_t> VmatEffect(bool model = false, const EffectGuid& material = Fresh(7), uint32_t pbdm = 2) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 4);
  PutProperty(out, "PBDM", 1);
  PutConstant(out, pbdm);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, material);
  if (model) {
    PutProperty(out, "PMDL", 0);
    PutGuid(out, Legacy(0x1234ABCD));
  }
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

// A model particle with a VMAT carries its converted model as VMSH, when the import has the mesh.
void TestVmsh() {
  const std::vector<uint8_t> data = VmatEffect(true);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "vmsh effect parses");
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [](const EffectGuid&) { return Mati(0x461071b8, {{"TCH0", Fresh(8)}}); };
  io.vfxTexture = [](const EffectGuid&) {
    FlipbookAtlas atlas;
    atlas.id = 0x5EED0008;
    return atlas;
  };
  const std::vector<uint8_t> blob = {0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 1, 1, 2, 3, 4};  // not parsed here
  io.modelMesh = [&blob](uint32_t model) { return model == 0x1234ABCD ? blob : std::vector<uint8_t>(); };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 1, "one part with a model");
  if (parts.size() != 1) {
    return;
  }
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "vmsh part reads as retail");
  const RetailPartProperty* vmsh = nullptr;
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("VMSH")) {
      vmsh = &property;
    }
  }
  Check(vmsh != nullptr, "a model particle with a VMAT writes VMSH");
  if (vmsh != nullptr) {
    const std::vector<uint8_t>& v = vmsh->value;
    Check(v.size() == 8 + blob.size() && std::memcmp(v.data(), "CNST", 4) == 0 && v[7] == blob.size() &&
              std::equal(blob.begin(), blob.end(), v.begin() + 8),
          "VMSH is CNST, the length and the blob");
  }
  // A model the import has no mesh for (a disc model) gets no VMSH.
  io.modelMesh = [](uint32_t) { return std::vector<uint8_t>(); };
  parts = ConvertEffect(effect, data.data(), io);
  properties.clear();
  if (parts.size() == 1 && SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
    for (const RetailPartProperty& property : properties) {
      Check(property.fourcc != EffectFourCC("VMSH"), "no VMSH for a model without a mesh");
    }
  }
}

// A recipe shader's MATI becomes a VMAT, and the port-only properties survive a retail split.
void TestVmat() {
  const std::vector<uint8_t> data = VmatEffect();
  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "vmat effect parses");

  int imports = 0;
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [](const EffectGuid& material) {
    return material == Fresh(7) ? Mati(0x461071b8, {{"TCH0", Fresh(8)}}) : std::vector<uint8_t>();
  };
  io.vfxTexture = [&imports](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) {
      ++imports;
      atlas.id = 0x5EED0008;
    }
    return atlas;
  };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 1, "one part");
  if (parts.size() != 1) {
    return;
  }
  Check(imports == 1, "the ramp texture is imported once");
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "vmat part reads as retail");
  const RetailPartProperty* vmat = nullptr;
  bool vorn = false;
  bool pirn = false;
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("VMAT")) {
      vmat = &property;
    }
    vorn = vorn || property.fourcc == EffectFourCC("VORN");
    pirn = pirn || property.fourcc == EffectFourCC("PIRN");
  }
  Check(vmat != nullptr, "a 461071b8 material writes VMAT");
  Check(vorn, "VORN is always written");
  Check(pirn, "PIRN marks every converted PART");
  if (vmat != nullptr) {
    // CNST + u32 size, then the blob: version 2, features (Ramp = 8), blend 2, one texture.
    const std::vector<uint8_t>& v = vmat->value;
    auto be32 = [&v](size_t at) { return at + 4 <= v.size() ? uint32_t(v[at]) << 24 | v[at + 1] << 16 | v[at + 2] << 8 | v[at + 3] : ~0u; };
    Check(v.size() > 24 && std::memcmp(v.data(), "CNST", 4) == 0, "VMAT is a constant blob");
    Check(be32(8) == 2 && be32(12) == 8 && be32(16) == 2 && be32(20) == 1, "VMAT v2, Ramp, blend 2, one texture");
    Check(be32(24) == 0x5EED0008, "the texture is the imported id");
  }
  Check(parts[0].approximated.empty(), "a recipe material approximates nothing");

  // A shader with no recipe: no VMAT, and a note.
  io.materialData = [](const EffectGuid&) { return Mati(0x12345678, {}); };
  parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 1, "one part without a recipe");
  if (parts.size() == 1) {
    bool note = false;
    for (const std::string& line : parts[0].approximated) {
      note = note || line == "VMAT: shader 12345678 has no recipe";
    }
    Check(note, "an unknown shader is noted as having no recipe");
    properties.clear();
    Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "fallback part reads");
    for (const RetailPartProperty& property : properties) {
      Check(property.fourcc != EffectFourCC("VMAT"), "no VMAT without a recipe");
    }
  }

  // A texture the import cannot write: no VMAT either.
  io.materialData = [](const EffectGuid&) { return Mati(0x461071b8, {{"TCH0", Fresh(9)}}); };
  parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 1, "one part when the texture fails");
  if (parts.size() == 1) {
    properties.clear();
    Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error), "failed-texture part reads");
    for (const RetailPartProperty& property : properties) {
      Check(property.fourcc != EffectFourCC("VMAT"), "no VMAT when a slot texture is not written");
    }
    Check(!parts[0].approximated.empty(), "the missing texture is noted");
  }
}

// A MATI with a CCH0 vector (type 3) after its textures.
std::vector<uint8_t> MatiCch0(uint32_t shader, const std::vector<std::pair<const char*, EffectGuid>>& textures,
                               float x, float y, float z, float w) {
  std::vector<uint8_t> out = Mati(shader, textures);
  out[0x69] += 1;  // one more entry: the CCH0 vector (the counts here are small)
  out.push_back(3);
  out.insert(out.end(), {'C', 'C', 'H', '0'});
  Put32(out, Bits(x));
  Put32(out, Bits(y));
  Put32(out, Bits(z));
  Put32(out, Bits(w));
  return out;
}

// Converts VmatEffect with `mati` as its material, returning the parts and the
// VMAT value (empty when no VMAT was written).
std::pair<std::vector<ConvertedPart>, std::vector<uint8_t>> ConvertVmat(const std::vector<uint8_t>& mati) {
  const std::vector<uint8_t> data = VmatEffect();
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: vmat effect does not parse: %s\n", error.c_str());
    ++sFailures;
    return {};
  }
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(7) ? 0x5EED0007u : 0u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) {
      atlas.id = 0x5EED0008;
    }
    return atlas;
  };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  std::vector<uint8_t> blob;
  if (parts.size() == 1) {
    std::vector<RetailPartProperty> properties;
    if (SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
      for (const RetailPartProperty& property : properties) {
        if (property.fourcc == EffectFourCC("VMAT")) {
          blob = property.value;
        }
      }
    }
  }
  return {parts, blob};
}

uint32_t VmatWord(const std::vector<uint8_t>& v, size_t at) {
  return at + 4 <= v.size() ? uint32_t(v[at]) << 24 | v[at + 1] << 16 | v[at + 2] << 8 | v[at + 3] : ~0u;
}

struct SmtrTestEntry {
  const char* parameter;
  uint8_t component;
  float value;
};

size_t PutSmtrItem(std::vector<uint8_t>& out, const SmtrTestEntry& entry) {
  PutConstant(out, Bits(entry.value));
  const size_t parameterAt = out.size();
  PutFourCC(out, entry.parameter);
  out.push_back(0);  // reserved
  out.push_back(entry.component);
  return parameterAt;
}

std::vector<uint8_t> SmtrVmatEffect(size_t& cch0BytesAt) {
  std::vector<uint8_t> data = VmatEffect();
  const std::vector<SmtrTestEntry> entries = {
      {"CCH0", 0, 0.25f}, {"CCH0", 1, 0.5f}, {"CCH0", 2, 0.75f}, {"NOPE", 0, 0.875f}};
  std::vector<uint8_t> smtr;
  PutProperty(smtr, "SMTR", 0);
  smtr.push_back(0);  // SMTR version
  smtr.push_back(uint8_t(entries.size()));
  smtr.push_back(uint8_t(entries.size()));  // group 0 item count
  cch0BytesAt = size_t(-1);
  for (const SmtrTestEntry& entry : entries) {
    const size_t parameterAt = PutSmtrItem(smtr, entry);
    if (cch0BytesAt == size_t(-1)) {
      cch0BytesAt = data.size() - 13 + parameterAt;
    }
  }
  for (int group = 1; group < 5; ++group) {
    smtr.push_back(0);
  }
  data.insert(data.end() - 13, smtr.begin(), smtr.end());  // before _END, child count and FOOT
  return data;
}

void TestSmtrCch0Overrides() {
  size_t cch0BytesAt = 0;
  const std::vector<uint8_t> data = SmtrVmatEffect(cch0BytesAt);
  Check(cch0BytesAt + 4 <= data.size() && data[cch0BytesAt] == 0x30 && data[cch0BytesAt + 1] == 0x48 &&
            data[cch0BytesAt + 2] == 0x43 && data[cch0BytesAt + 3] == 0x43,
        "SMTR fixture serializes CCH0 as source bytes 30 48 43 43");

  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "raw SMTR effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }
  const auto smtr = std::find_if(effect.properties.begin(), effect.properties.end(),
                                 [](const EffectProperty& property) {
                                   return property.fourcc == EffectFourCC("SMTR");
                                 });
  Check(smtr != effect.properties.end() && smtr->value.size() == 4, "parser retains all raw SMTR entries");
  if (smtr != effect.properties.end() && smtr->value.size() == 4) {
    Check(smtr->value[0].word == EffectFourCC("CCH0"), "raw CCH0 bytes parse to the canonical FourCC");
    Check(smtr->value[3].word != EffectFourCC("CCH0"), "unknown SMTR id remains distinct");
  }

  const EffectGuid texture0 = Fresh(8), texture1 = Fresh(9), texture2 = Fresh(10);
  const std::vector<uint8_t> mati = MatiCch0(0x8e6bfa33,
      {{"TCH0", texture0}, {"TCH1", texture1}, {"TCH2", texture2}}, 0.0f, 0.0f, 0.0f, 0.0f);
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
  io.materialData = [&mati](const EffectGuid& material) {
    return material == Fresh(7) ? mati : std::vector<uint8_t>();
  };
  io.vfxTexture = [](const EffectGuid& texture) {
    FlipbookAtlas atlas;
    if (texture == Fresh(8)) atlas.id = 0x5EED0008;
    if (texture == Fresh(9)) atlas.id = 0x5EED0009;
    if (texture == Fresh(10)) atlas.id = 0x5EED000A;
    return atlas;
  };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 1, "raw SMTR converts through ConvertEffect");
  if (parts.size() != 1) {
    return;
  }

  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error),
        "SMTR output reads as retail PART");
  const auto vsmt = std::find_if(properties.begin(), properties.end(),
                                 [](const RetailPartProperty& property) {
                                   return property.fourcc == EffectFourCC("VSMT");
                                 });
  std::vector<uint8_t> expected;
  const auto appendBe32 = [&expected](uint32_t word) {
    expected.push_back(uint8_t(word >> 24));
    expected.push_back(uint8_t(word >> 16));
    expected.push_back(uint8_t(word >> 8));
    expected.push_back(uint8_t(word));
  };
  expected.insert(expected.end(), {'C', 'N', 'S', 'T'});
  appendBe32(3);
  const std::array<std::pair<uint32_t, float>, 3> overrides = {{{3, 0.25f}, {11, 0.5f}, {12, 0.75f}}};
  for (const auto& [target, value] : overrides) {
    appendBe32(target);
    appendBe32(0);  // real element kind
    expected.insert(expected.end(), {'C', 'N', 'S', 'T'});
    appendBe32(Bits(value));
  }
  Check(vsmt != properties.end() && vsmt->value == expected,
        "CCH0 x maps to the Src override and y/z map to TCH0 warp overrides");
  if (vsmt == properties.end() || vsmt->value != expected) {
    std::fprintf(stderr, "  VSMT expected %zu bytes, got %zu\n", expected.size(),
                 vsmt == properties.end() ? 0 : vsmt->value.size());
    if (vsmt != properties.end()) {
      for (uint8_t byte : vsmt->value) std::fprintf(stderr, "%02x", byte);
      std::fprintf(stderr, "\n");
    }
    for (const std::string& dropped : parts[0].dropped) std::fprintf(stderr, "  dropped: %s\n", dropped.c_str());
  }
  Check(std::find(parts[0].dropped.begin(), parts[0].dropped.end(), "SMTR: a parameter VSMT cannot take") !=
            parts[0].dropped.end(),
        "unknown SMTR parameter id is rejected");

  EffectNode withoutSmtr = effect;
  withoutSmtr.properties.erase(std::remove_if(withoutSmtr.properties.begin(), withoutSmtr.properties.end(),
      [](const EffectProperty& property) { return property.fourcc == EffectFourCC("SMTR"); }),
      withoutSmtr.properties.end());
  const std::vector<ConvertedPart> baseline = ConvertEffect(withoutSmtr, data.data(), io);
  std::vector<RetailPartProperty> baselineProperties;
  Check(baseline.size() == 1 &&
            SplitRetailPart(baseline[0].part.data(), baseline[0].part.size(), baselineProperties, error),
        "SMTR-free baseline converts");
  const auto withoutVsmt = [](std::vector<RetailPartProperty> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](const RetailPartProperty& property) {
                   return property.fourcc == EffectFourCC("VSMT");
                 }), values.end());
    return values;
  };
  const auto convertedBase = withoutVsmt(properties);
  const auto expectedBase = withoutVsmt(baselineProperties);
  bool nonOverrideBytesMatch = convertedBase.size() == expectedBase.size();
  for (size_t i = 0; nonOverrideBytesMatch && i < convertedBase.size(); ++i) {
    nonOverrideBytesMatch = convertedBase[i].fourcc == expectedBase[i].fourcc &&
                            convertedBase[i].value == expectedBase[i].value;
  }
  Check(nonOverrideBytesMatch, "CCH0 conversion changes only the VSMT override property");
}

// The wave orb's exact MATI (MTIN 756533fc-e832-48f2-b1ef-ebb41fcea14f, little-endian groups) and its
// header blend (EBlendMode, the LE u32 at 0x61).
const EffectGuid kWaveMati = {0xfc, 0x33, 0x65, 0x75, 0x32, 0xe8, 0xf2, 0x48,
                              0xb1, 0xef, 0xeb, 0xb4, 0x1f, 0xce, 0xa1, 0x4f};
// aurora::gfx::vfx::AlreadyExposed in a VMAT features word.
constexpr uint32_t kAlreadyExposedBit = 1u << 31;

// PowerBall source fixture IDs, stored as little-endian UUIDs by GENP/MATI.
const EffectGuid kPowerBallChild = {0x82, 0x07, 0x87, 0xae, 0xf4, 0x41, 0x26, 0x4c,
                                    0x88, 0x76, 0xc2, 0xf0, 0x70, 0xcf, 0x1e, 0x06};
const EffectGuid kPowerBallMati = {0x46, 0x2a, 0x75, 0x86, 0x33, 0xe7, 0x23, 0x4d,
                                   0xad, 0xc8, 0xc7, 0x4f, 0x28, 0x15, 0x80, 0xec};
const EffectGuid kPowerBallModel = {0x60, 0xae, 0xf6, 0x4b, 0xe5, 0x3b, 0x51, 0x4d,
                                    0x88, 0xbd, 0x50, 0xbb, 0xd1, 0xbd, 0xf6, 0x71};
const EffectGuid kPowerBallRamp = {0x4f, 0xb8, 0xea, 0xe1, 0xa1, 0x69, 0x03, 0x40,
                                   0x98, 0x6a, 0xed, 0xf9, 0xbd, 0x1e, 0x5b, 0xfe};
const EffectGuid kPowerBallIndirect = {0x79, 0x40, 0x9b, 0xb8, 0x4b, 0x2b, 0x39, 0x49,
                                       0xa2, 0x49, 0xe7, 0x36, 0x09, 0x7f, 0x3a, 0xf0};

// Converts a VmatEffect (PBDM `pbdm`, 0 by default) whose material is `material` and returns the VMAT blob's blend word
// (or the word at `word`: 12 is the features), or ~0u when no VMAT came out.
uint32_t ConvertBlend(std::vector<uint8_t> mati, const EffectGuid& material, bool model, uint32_t pbdm = 0,
                      size_t word = 16) {
  const std::vector<uint8_t> data = VmatEffect(model, material, pbdm);
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: wave blend effect does not parse: %s\n", error.c_str());
    ++sFailures;
    return ~0u;
  }
  EffectConvertIO io;
  io.materialTexture = [&material](const EffectGuid& id) { return id == material ? 0x5EED0007u : 0u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid&) {
    FlipbookAtlas atlas;
    atlas.id = 0x5EED0008;
    return atlas;
  };
  io.modelMesh = [](uint32_t) { return std::vector<uint8_t>(); };
  std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  std::vector<RetailPartProperty> properties;
  if (parts.size() != 1 || !SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
    return ~0u;
  }
  for (const RetailPartProperty& property : properties) {
    if (property.fourcc == EffectFourCC("VMAT")) {
      return VmatWord(property.value, word);
    }
  }
  return ~0u;
}

// A swoosh on the orb's exact MATI keeps SBDM: the override is for model particles only.
void TestWaveOrbSwoosh(const std::vector<uint8_t>& mati) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 3);
  PutProperty(out, "SSWH", 0);
  PutGuid(out, Legacy(0x0000AAAA));
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0000AAAA));
  PutFourCC(out, "SWSH");
  PutProperty(out, "LENG", 1);
  PutConstant(out, 8);
  PutProperty(out, "SBDM", 1);
  PutConstant(out, 0);
  PutProperty(out, "PMDL", 0);
  PutGuid(out, Legacy(0x1234ABCD));
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, kWaveMati);
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "orb swoosh effect parses");
  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& id) { return id == kWaveMati ? 0x5EED0007u : 0u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid&) {
    FlipbookAtlas atlas;
    atlas.id = 0x5EED0008;
    return atlas;
  };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), io);
  uint32_t blend = ~0u, features = ~0u;
  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    if (part.type == EffectFourCC("SWHC") && SplitRetailEffect(part.type, part.part.data(), part.part.size(), properties, error)) {
      for (const RetailPartProperty& property : properties) {
        if (property.fourcc == EffectFourCC("VMAT")) {
          blend = VmatWord(property.value, 16);
          features = VmatWord(property.value, 12);
        }
      }
    }
  }
  Check(blend == 0, "a swoosh on the orb's MATI keeps SBDM (alpha), not AdditiveOne");
  Check(features != ~0u && (features & kAlreadyExposedBit) == 0, "a swoosh on the orb's MATI keeps the room's exposure");
}

// The exposure policy (aurora::gfx::vfx::AlreadyExposed, the features word's bit 31) is the orb's exact
// gate only, tested on its own: it is not read off the blend.
void TestWaveOrbExposurePolicy() {
  auto orb = [](uint32_t shader, uint32_t headerBlend) {
    std::vector<uint8_t> mati = Mati(shader, {{shader == 0x642dd72b ? "BCLR" : "TCH0", Fresh(8)}});
    mati[0x61] = uint8_t(headerBlend);
    return mati;
  };
  const uint32_t flagged = ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true, 0, 12);
  const uint32_t plain = ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true, 0, 12);
  Check(flagged != ~0u && (flagged & kAlreadyExposedBit) != 0, "wave orb MATI + PMDL + 642dd72b + header 1: AlreadyExposed");
  Check(plain != ~0u && (flagged & ~kAlreadyExposedBit) == plain,
        "the policy bit is the only feature difference (the shader features are the recipe's)");
  Check(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true, 2, 12) == flagged, "the policy holds whatever the PBDM");
  const auto unflagged = [](uint32_t f) { return f != ~0u && (f & kAlreadyExposedBit) == 0; };
  Check(unflagged(plain), "near miss: another MATI guid has no policy");
  Check(unflagged(ConvertBlend(orb(0x461071b8, 1), kWaveMati, true, 0, 12)), "near miss: another shader has no policy");
  Check(unflagged(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, false, 0, 12)), "near miss: no PMDL has no policy");
  Check(unflagged(ConvertBlend(orb(0x642dd72b, 0), kWaveMati, true, 0, 12)), "near miss: header blend 0 has no policy");
  Check(unflagged(ConvertBlend(orb(0x642dd72b, 2), kWaveMati, true, 0, 12)), "near miss: header blend 2 has no policy");
  for (uint32_t pbdm = 0; pbdm <= 3; ++pbdm) {
    Check(unflagged(ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true, pbdm, 12)), "no policy from any PBDM alone");
  }
}

std::vector<uint8_t> PowerBallMati(uint32_t shader = 0xe899b1df, uint32_t headerBlend = ~0u) {
  std::vector<uint8_t> mati = MatiCch0(shader, {{"TCH0", kPowerBallRamp}, {"TCH1", kPowerBallIndirect}},
                                       0.5f, 0.5f, 1.0f, 0.0f);
  for (int i = 0; i < 4; ++i) {
    mati[0x61 + size_t(i)] = uint8_t(headerBlend >> (8 * i));
  }
  return mati;
}

struct PowerBallFields {
  EffectGuid child = kPowerBallChild;
  EffectGuid material = kPowerBallMati;
  uint8_t materialTag = 0;
  uint8_t materialSelector = 0;
  EffectGuid model = kPowerBallModel;
  uint32_t shader = 0xe899b1df;
  uint32_t headerBlend = ~0u;
  uint32_t dvvn = 4;
  uint8_t dvvnTag = 1;
  uint32_t pbdm = 0;
  uint8_t pbdmTag = 1;
  uint8_t modelTag = 3;
  uint32_t iexp = 1;
  bool hasModel = true;
  bool hasDvvn = true;
  bool hasPbdm = true;
  bool hasIexp = false;
  bool unsupportedModel = false;
  bool unsupportedDvvn = false;
  bool unsupportedPbdm = false;
  bool bareModel = false;
  bool barePbdm = false;
  bool duplicateMaterial = false;
};

std::vector<uint8_t> PowerBallEffect(const PowerBallFields& fields) {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "DVVN", 1);
  out.push_back(4);  // WPSM/APSM root's DVVN #04
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, fields.child);
  PutGenerator(out, false);
  if (fields.hasDvvn) {
    PutProperty(out, "DVVN", fields.dvvnTag);
    if (fields.unsupportedDvvn) {
      PutFourCC(out, "RAND");
      PutConstant(out, 0);
      PutConstant(out, fields.dvvn);
    } else {
      out.push_back(uint8_t(fields.dvvn));  // source stores DVVN as the raw flag byte (#04)
    }
  }
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 1);
  PutProperty(out, "ITEN", 3);
  PutConstant(out, Bits(3.0f));
  if (fields.hasPbdm) {
    PutProperty(out, "PBDM", fields.pbdmTag);
    if (fields.unsupportedPbdm) {
      PutFourCC(out, "RAND");
      PutConstant(out, 0);
      PutConstant(out, fields.pbdm);
    } else if (fields.barePbdm) {
      out.push_back(uint8_t(fields.pbdm));
    } else {
      PutConstant(out, fields.pbdm);
    }
  }
  PutProperty(out, "MTIN", fields.materialTag);
  out.push_back(fields.materialSelector);
  PutGuid(out, fields.material);
  if (fields.duplicateMaterial) {
    PutProperty(out, "MTIN", 0);
    out.push_back(0);
    PutGuid(out, fields.material);
  }
  if (fields.hasModel) {
    PutProperty(out, "PMDL", fields.modelTag);
    if (fields.unsupportedModel) {
      PutFourCC(out, "RAND");
      PutConstant(out, 0);
      PutConstant(out, 1);
    } else if (fields.bareModel) {
      PutGuid(out, fields.model);
    } else {
      PutFourCC(out, "CNST");
      PutGuid(out, fields.model);
    }
  }
  if (fields.hasIexp) {
    PutProperty(out, "IEXP", 1);
    out.push_back(uint8_t(fields.iexp));
  }
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

uint32_t ConvertPowerBallFeatures(const PowerBallFields& fields) {
  const std::vector<uint8_t> data = PowerBallEffect(fields);
  const std::vector<uint8_t> mati = PowerBallMati(fields.shader, fields.headerBlend);
  EffectNode effect;
  std::string error;
  if (!ParseEffect(data.data(), data.size(), effect, error)) {
    std::fprintf(stderr, "FAIL: PowerBall fixture does not parse: %s\n", error.c_str());
    ++sFailures;
    return ~0u;
  }
  EffectConvertIO io;
  io.assetId = [&fields](const EffectGuid& id, uint32_t type) {
    return type == EffectFourCC("CMDL") && id == fields.model ? 0xC0DE4BF6u : 0u;
  };
  io.materialTexture = [](const EffectGuid&) { return 0x5EED0007u; };
  io.materialData = [&mati](const EffectGuid&) { return mati; };
  io.vfxTexture = [](const EffectGuid& texture) {
    if (texture == kPowerBallRamp) {
      return FlipbookAtlas{0x5EED0008u, 1, 1, 1};
    }
    if (texture == kPowerBallIndirect) {
      return FlipbookAtlas{0x5EED0009u, 1, 1, 1};
    }
    return FlipbookAtlas{};
  };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  for (const ConvertedPart& part : parts) {
    if (part.id != fields.child) {
      continue;
    }
    std::vector<RetailPartProperty> properties;
    if (!SplitRetailPart(part.part.data(), part.part.size(), properties, error)) {
      return ~0u;
    }
    for (const RetailPartProperty& property : properties) {
      if (property.fourcc == EffectFourCC("VMAT")) {
        return VmatWord(property.value, 12);
      }
    }
  }
  return ~0u;
}

void TestPowerBallExposurePolicy() {
  const uint32_t flagged = ConvertPowerBallFeatures({});
  Check(flagged != ~0u && (flagged & kAlreadyExposedBit) != 0,
        "PowerBall MATI e899b1df/header -1 + PMDL/DVVN 4/PBDM 0/no IEXP: AlreadyExposed");
  const auto unflagged = [](uint32_t features) {
    return features != ~0u && (features & kAlreadyExposedBit) == 0;
  };
  const uint32_t plain = ConvertPowerBallFeatures(PowerBallFields{.dvvn = 3});
  Check(plain != ~0u && (flagged & ~kAlreadyExposedBit) == plain,
        "PowerBall exposure policy changes only the AlreadyExposed bit");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.material = Fresh(7)})), "near miss: other MATI GUID");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.materialTag = 1})), "near miss: MTIN tag");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.materialSelector = 1})), "near miss: MTIN selector");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.duplicateMaterial = true})), "duplicate MTIN fails closed");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.child = Fresh(8)})), "near miss: other child GUID");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.shader = 0x461071b8})), "near miss: other MATI shader");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.headerBlend = 1})), "near miss: MATI header blend 1");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.headerBlend = 0})), "near miss: MATI header blend 0");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasModel = false})), "near miss: PMDL absent");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.modelTag = 1})), "near miss: PMDL tag");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.model = Fresh(4)})), "near miss: different PMDL GUID");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.unsupportedModel = true})), "near miss: PMDL expression");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.bareModel = true})), "near miss: PMDL without CNST");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.dvvn = 3})), "near miss: DVVN 3");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.dvvnTag = 3})), "near miss: DVVN tag");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasDvvn = false})), "missing DVVN fails closed");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.unsupportedDvvn = true})), "DVVN expression fails closed");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.pbdm = 1})), "near miss: PBDM 1");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.pbdmTag = 3})), "near miss: PBDM tag");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasPbdm = false})), "missing PBDM fails closed");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.unsupportedPbdm = true})), "PBDM expression fails closed");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.barePbdm = true})), "PBDM without CNST fails closed");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.iexp = 0, .hasIexp = true})), "IEXP false present fails closed");
  Check(unflagged(ConvertPowerBallFeatures(PowerBallFields{.hasIexp = true})), "IEXP true present fails closed");
}

// Only the wave orb's MATI, on a model particle, with header blend 1 takes Additive over PBDM.
void TestWaveOrbHeaderBlend() {
  auto orb = [](uint32_t shader, uint32_t headerBlend) {
    std::vector<uint8_t> mati = Mati(shader, {{shader == 0x642dd72b ? "BCLR" : "TCH0", Fresh(8)}});
    mati[0x61] = uint8_t(headerBlend);
    return mati;
  };
  Check(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true) == 5, "wave orb MATI + PMDL + header blend 1: AdditiveOne (5)");
  Check(ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true) == 0, "other MATI guid keeps PBDM 0");
  Check(ConvertBlend(orb(0x642dd72b, 0), kWaveMati, true) == 0, "header blend 0 keeps PBDM");
  Check(ConvertBlend(orb(0x461071b8, 1), kWaveMati, true) == 0, "another shader keeps PBDM 0");
  Check(ConvertBlend(orb(0x642dd72b, 2), kWaveMati, true) == 0, "header blend 2 keeps PBDM 0");
  Check(ConvertBlend(std::vector<uint8_t>(0x50, 0), kWaveMati, true) == ~0u, "a short MATI writes no VMAT, no crash");
  Check(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, false) == 0, "no PMDL keeps PBDM 0");
  // The older modes are untouched: PBDM 0..3 convert as themselves, on the orb's MATI too without PMDL
  // and on its PMDL with another guid.
  for (uint32_t pbdm = 0; pbdm <= 3; ++pbdm) {
    Check(ConvertBlend(orb(0x642dd72b, 1), Fresh(7), true, pbdm) == pbdm, "PBDM 0..3 convert unchanged");
    Check(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, false, pbdm) == pbdm, "PBDM 0..3 without PMDL unchanged");
  }
  Check(ConvertBlend(orb(0x642dd72b, 1), kWaveMati, true, 2) == 5, "the override is the orb's, whatever its PBDM");
  TestWaveOrbSwoosh(orb(0x642dd72b, 1));
}

// The new recipes: the FrameBuffer shaders draw as a multiply (blend 4), and
// the ice charge beam reads its colour from BCLR alone.
void TestVmatNewRecipes() {
  // FrameBuffer_Indirect_Unlit with no warp: features 0, blend Multiply, no textures.
  auto [parts, blob] = ConvertVmat(Mati(0x3db95827, {}));
  Check(parts.size() == 1 && !blob.empty(), "a 3db95827 material writes VMAT");
  if (!blob.empty()) {
    Check(VmatWord(blob, 8) == 2 && VmatWord(blob, 12) == 0 && VmatWord(blob, 16) == 4 && VmatWord(blob, 20) == 0,
          "3db95827 VMAT v2, no features, blend 4, no textures");
  }
  Check(parts.size() == 1 && parts[0].approximated.empty(), "3db95827 with no warp approximates nothing");

  // The same shader with a warp: the warp is dropped (drawn as a multiply).
  auto warped = ConvertVmat(MatiCch0(0x3db95827, {}, 0.5f, 0.0f, 0.0f, 0.0f));
  Check(warped.first.size() == 1 && !warped.second.empty(), "a warped 3db95827 material still writes VMAT");
  if (!warped.second.empty()) {
    Check(VmatWord(warped.second, 16) == 4, "a warped 3db95827 VMAT still blends as a multiply");
  }
  Check(warped.first.size() == 1 && warped.first[0].approximated.size() == 1 &&
            warped.first[0].approximated[0] ==
                "VMAT: shader 3db95827's scene warp dropped (drawn as a multiply)",
        "a warped 3db95827 notes its scene warp dropped");

  // FrameBuffer_Opacity_Unlit: the opacity texture, still a multiply.
  auto opacity = ConvertVmat(Mati(0xd955396d, {{"TCH0", Fresh(8)}}));
  Check(opacity.first.size() == 1 && !opacity.second.empty(), "a d955396d material writes VMAT");
  if (!opacity.second.empty()) {
    Check(VmatWord(opacity.second, 8) == 2 && VmatWord(opacity.second, 12) == 2 &&
              VmatWord(opacity.second, 16) == 4 && VmatWord(opacity.second, 20) == 1,
          "d955396d VMAT v2, OpacityTex, blend 4, one texture");
    Check(VmatWord(opacity.second, 24) == 0x5EED0008, "d955396d reads the TCH0 texture");
  }
  Check(opacity.first.size() == 1 && opacity.first[0].approximated.empty(), "d955396d approximates nothing");

  // VFX_IceChargeBeam: BCLR as the colour texture, rgb only.
  auto ice = ConvertVmat(Mati(0x9e0605cd, {{"BCLR", Fresh(8)}}));
  Check(ice.first.size() == 1 && !ice.second.empty(), "a 9e0605cd material writes VMAT");
  if (!ice.second.empty()) {
    Check(VmatWord(ice.second, 8) == 2 && VmatWord(ice.second, 12) == (1 | 4096) &&
              VmatWord(ice.second, 16) == 2 && VmatWord(ice.second, 20) == 1,
          "9e0605cd VMAT v2, ColorTex|ColorRgbOnly, blend 2, one texture");
    Check(VmatWord(ice.second, 24) == 0x5EED0008, "9e0605cd reads the BCLR texture");
  }
  Check(ice.first.size() == 1 && ice.first[0].approximated.empty(), "9e0605cd approximates nothing");
}

// A generator with a PMDL but no TEXR or MTIN draws only models: its SIZE is
// left out and a SIZE of 0 written instead, as with one that draws nothing.
void TestModelsOnly() {
  const EffectConvertIO io = AtlasIO();
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutModels(o, 4, 3);
                 PutProperty(o, "SIZE", 3);
                 PutConstant(o, Bits(2.0f));
               }),
               io);
  Retail want;
  want.f("GPSM").f("PMDL").f("CNST").w(0xC0DE000A).f("PMDV").f("CNST").w(4);
  for (uint32_t i = 0; i < 4; ++i) {
    want.f("CNST").w(0xC0DE000A + i);
  }
  want.f("SIZE").f("CNST").w(Bits(0.0f)).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a models-only generator gets SIZE 0");
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "SIZE: the generator draws models only",
        "its SIZE is dropped as models only");
  Check(!parts.empty() && parts[0].droppedRetail == 0, "models only drops no retail property");
}

// PMRQ's camera-facing form: RADD(RAZY, SUB_(CPSS(PLOC, up, REUL(0, 0, 0)),
// REUL(x, y, z))). `angles` writes the second REUL's three angle arguments.
template <typename Angles>
void PutFacingPmrq(std::vector<uint8_t>& out, Angles angles) {
  PutProperty(out, "PMRQ", 3);
  PutFourCC(out, "RADD");
  PutFourCC(out, "RAZY");
  PutFourCC(out, "SUB_");
  PutFourCC(out, "CPSS");
  PutFourCC(out, "PLOC");
  PutFourCC(out, "CNST");
  PutConstant(out, 0);
  PutConstant(out, Bits(1.0f));
  PutConstant(out, 0);
  PutFourCC(out, "REUL");
  PutConstant(out, 0);
  PutConstant(out, 0);
  PutConstant(out, 0);
  out.push_back(0);
  PutFourCC(out, "REUL");
  angles(out);
  out.push_back(0);
}

void TestFacingRotation() {
  // One nonzero angle: PMRT is the negated angles, and PFCM faces the camera.
  auto parts = ConvertOne(OneGenerator([](auto& o) {
                 PutFacingPmrq(o, [](auto& o) {
                   PutConstant(o, 0);
                   PutConstant(o, 0);
                   PutConstant(o, Bits(90.0f));
                 });
                 PutTexr(o, 0x1234ABCD);
               }),
               {});
  Retail want;
  want.f("GPSM").f("PMRT").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(Bits(-90.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  want.f("PFCM").f("CNST").w(1).end();
  Check(parts.size() == 1 && parts[0].part == want.bytes, "a facing PMRQ becomes a negated PMRT with PFCM");
  Check(!parts.empty() && parts[0].dropped.empty() && parts[0].approximated.empty(),
        "a facing PMRQ is exact");

  // An IRND angle is a random spin: taken as 0, and noted.
  parts = ConvertOne(OneGenerator([](auto& o) {
              PutFacingPmrq(o, [](auto& o) {
                PutFourCC(o, "IRND");
                PutConstant(o, Bits(0.0f));
                PutConstant(o, Bits(360.0f));
                PutConstant(o, 0);
                PutConstant(o, 0);
              });
              PutTexr(o, 0x1234ABCD);
            }),
            {});
  Retail still;
  still.f("GPSM").f("PMRT").f("CNST").f("CNST").w(0).f("CNST").w(0).f("CNST").w(0);
  still.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  still.f("PFCM").f("CNST").w(1).end();
  Check(parts.size() == 1 && parts[0].part == still.bytes, "a facing PMRQ with IRND spins nothing");
  Check(!parts.empty() && parts[0].approximated.size() == 1 &&
            parts[0].approximated[0] == "PMRQ: a camera-facing model's random spin left out",
        "a facing PMRQ with IRND is listed as approximated");

  // Two nonzero angles are not a single negation: the PMRQ is dropped as before.
  parts = ConvertOne(OneGenerator([](auto& o) {
              PutFacingPmrq(o, [](auto& o) {
                PutConstant(o, Bits(45.0f));
                PutConstant(o, Bits(30.0f));
                PutConstant(o, 0);
              });
              PutTexr(o, 0x1234ABCD);
            }),
            {});
  Retail plain;
  plain.f("GPSM").f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  Check(parts.size() == 1 && parts[0].part == plain.bytes, "a facing PMRQ with two angles writes nothing");
  Check(!parts.empty() && parts[0].dropped.size() == 1 &&
            parts[0].dropped[0] == "PMRQ: a rotation that is not REUL",
        "a facing PMRQ with two angles is dropped as before");
  if (!parts.empty()) {
    std::string error;
    std::vector<RetailPartProperty> properties;
    bool pfcm = false;
    if (SplitRetailPart(parts[0].part.data(), parts[0].part.size(), properties, error)) {
      for (const RetailPartProperty& property : properties) {
        pfcm = pfcm || property.fourcc == EffectFourCC("PFCM");
      }
    }
    Check(!pfcm, "no PFCM when the facing rotation is refused");
  }
}

int main() {
  TestVmat();
  TestVmsh();
  TestAtlasTexture();
  TestModelChoice();
  TestRetailId();
  TestPlasmaNestedHdrLight();
  TestSwooshElectric();
  TestConvert();
  TestRejects();
  TestSingleNode();
  TestMappedElements();
  TestSplitRejects();
  TestShapes();
  TestGradientFrames();
  TestSpawnTable();
  TestDrawsNothing();
  TestBlendMode();
  TestGpuAvailability();
  TestParameterDefault();
  TestModelRotation();
  TestComposedModelRotation();
  TestVmatNewRecipes();
  TestWaveOrbHeaderBlend();
  TestWaveOrbExposurePolicy();
  TestPowerBallExposurePolicy();
  TestSmtrCch0Overrides();
  TestModelsOnly();
  TestFacingRotation();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::printf("port_remastered_effect_convert: ok\n");
  return 0;
}
