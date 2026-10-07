// The AUVI rule the converter applies to a material's map texcoords, checked both
// as the rule alone (all seven map coords, the flag and the parameter chosen) and
// through a real conversion: a synthetic model is run through
// PortRemastered::Converter and the texcoord of every map is read back out of the
// CMDL it wrote, which is where a remap that is computed and then ignored would
// show up as no change at all.

#include "port_remastered_convert.h"
#include "port_remastered_anuv_gun.h"
#include "port_remastered_uv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

// The converter's own flag for a PBR material (kPbrFlag there), so the test can
// tell a PBR material from a TEV one without reaching into the converter.
constexpr uint32_t kPbrFlag = 0x4000;

// One material: a base map on `baseCoord`, an MR map on `mrCoord`, and whatever
// parameters `extra` adds.
struct Fixture {
  ModelMaterial material;
  explicit Fixture(uint32_t flags, uint32_t baseCoord, uint32_t mrCoord) {
    material.name = "uvtest";
    material.unk1 = flags;
    material.types.push_back(FourCC('R', 'L', 'T', 'G'));
    material.data.push_back(Texture(FourCC('D', 'I', 'F', 'T'), 0x11111111, baseCoord));
    material.data.push_back(Texture(FourCC('M', 'E', 'T', 'L'), 0x22222222, mrCoord));
  }
  ModelMaterialData Texture(uint32_t usage, uint32_t id, uint32_t coord) {
    ModelMaterialData d;
    d.usage = usage;
    d.kind = ModelMaterialData::Kind::Texture;
    d.texture.usage = usage;
    d.texture.hasUsage = true;
    d.texture.id[0] = uint8_t(id >> 24);
    d.texture.id[3] = uint8_t(id);
    d.texture.texCoord = coord;
    d.texture.wrapX = 1;
    d.texture.wrapY = 1;
    return d;
  }
  ModelMaterialData Auvi(int32_t a, int32_t b, int32_t c, int32_t d,
                         ModelMaterialData::Kind kind = ModelMaterialData::Kind::Int4) {
    ModelMaterialData p;
    p.usage = kAuviUsage;
    p.kind = kind;
    p.int4[0] = a;
    p.int4[1] = b;
    p.int4[2] = c;
    p.int4[3] = d;
    if (kind == ModelMaterialData::Kind::Int1) {
      p.int1 = a;
    }
    return p;
  }
};

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

// Runs a conversion of the fixture's model, standalone (which is how a room's own
// material is converted), and returns the CMDL it wrote. False when it failed,
// with the message in `error`.
bool Convert(const Model& model, std::vector<uint8_t>& cmdl, std::string& error) {
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) {
      cmdl = data;
    }
    return true;
  };
  ConvertOptions opt;
  opt.standalone = true;
  opt.retail = 0xABCD1234;
  opt.skip.clear();  // the default drops a material whose name holds "simple"
  Converter converter(io);
  return converter.Convert(model, opt, error);
}

// A model with one triangle, one material and `uvSets` texcoord sets, so a coord
// past the third is one the vertex shader has no output channel for.
Model BuildModel(const ModelMaterial& material, size_t uvSets) {
  Model model;
  model.materials.push_back(material);
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    for (int c = 0; c < 3; ++c) {
      vb.positions.push_back(float(v + c));
    }
    vb.normals.push_back(0.f);
    vb.normals.push_back(1.f);
    vb.normals.push_back(0.f);
  }
  vb.uvs.resize(uvSets);
  for (size_t s = 0; s < uvSets; ++s) {
    for (int v = 0; v < 3; ++v) {
      vb.uvs[s].push_back(float(v));
      vb.uvs[s].push_back(float(s));
    }
  }
  model.vertexBuffers.push_back(vb);
  ModelMesh mesh;
  mesh.material = 0;
  mesh.vertexBuffer = 0;
  mesh.indices = {0, 1, 2};
  model.meshes.push_back(mesh);
  return model;
}

// Every map's texcoord as the converted CMDL binds it: the TEV fallback's stages
// name one per map, base first. A converter that writes no readable PBR material
// leaves `coords` empty. Every read is bounds-checked, so a converter that emitted
// a malformed CMDL fails the test rather than reading past the end.
bool ReadMapCoords(const std::vector<uint8_t>& d, std::vector<uint32_t>& coords) {
  coords.clear();
  const size_t n = d.size();
  size_t p = 0;
  auto need = [&](size_t at, size_t bytes) { return at <= n && n - at >= bytes; };
  auto word = [&](size_t at, uint32_t& out) {
    if (!need(at, 4)) {
      return false;
    }
    out = Be32(d, at);
    return true;
  };
  // Header: magic, version, flags, the bounding box, then the section count, the
  // material set count and one size per section.
  if (!need(0, 12 + 24 + 4 + 4)) {
    return false;
  }
  uint32_t nsec = 0, ntex = 0, nmats = 0, flags = 0, nmaps = 0, nchan = 0, nstages = 0;
  p = 12 + 24;
  if (!word(p, nsec)) {
    return false;
  }
  p += 8;  // past the section count and the material set count
  if (nsec == 0 || nsec > (n - p) / 4) {
    return false;
  }
  p += 4 * size_t(nsec);
  p = (p + 31) & ~size_t(31);  // the sections follow the header, 32-byte aligned
  // The first section is the material set: the texture ids, then the materials.
  if (!word(p, ntex) || ntex > (n - p - 4) / 4) {
    return false;
  }
  p += 4 + 4 * size_t(ntex);
  if (!word(p, nmats) || nmats == 0 || nmats > (n - p - 4) / 4) {
    return false;
  }
  p += 4 + 4 * size_t(nmats);
  // Material zero's blob, in the order PbrMaterial writes it.
  size_t m = p;
  if (!word(m, flags)) {
    return false;
  }
  m += 4;
  if ((flags & kPbrFlag) == 0) {  // not a PBR material: nothing to read
    return false;
  }
  if (!word(m, nmaps) || nmaps == 0 || nmaps > (n - m - 4) / 4) {
    return false;
  }
  m += 4 + 4 * size_t(nmaps);  // the maps' texture indices
  m += 4;                      // the vertex descriptor
  m += 4;                      // the material's cache id
  if ((flags & 0x8) != 0) {
    m += 8;  // a ColorUnlit material's constant colour
  }
  m += 4;  // the blend factors
  if (!word(m, nchan) || nchan > (n - m - 4) / 4) {
    return false;
  }
  m += 4 + 4 * size_t(nchan);  // the lit channels
  if (!word(m, nstages) || nstages > (n - m - 4) / 20) {
    return false;
  }
  m += 4;
  m += 20 * size_t(nstages);  // the stages, four words and four flags each
  // Then one entry per stage: padding, the map slot it samples and that map's
  // texcoord, base first.
  if (nstages > (n - m) / 4) {
    return false;
  }
  for (uint32_t i = 0; i < nstages; ++i) {
    if (!need(m, 4)) {
      return false;
    }
    m += 2;
    const uint32_t slot = d[m];
    m += 1;
    const uint32_t coord = d[m];
    m += 1;
    if (slot < nmaps) {
      coords.resize(slot + 1);
      coords[slot] = coord;
    }
  }
  return coords.size() == nmaps;
}

// The rule on its own, over the seven coords a material can have: base, MR,
// normal, emissive, then the second layer's base, MR and normal.
void TestRule() {
  {  // Nothing to remap with: every coord is left as it was.
    Fixture f(0x40, 0, 2);
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 0 && coords[1] == 1 && coords[2] == 2 && coords[3] == 3 && coords[4] == 4 && coords[5] == 5 &&
              coords[6] == 6,
          "a flagged material with no AUVI keeps every texcoord");
  }
  {  // The flag is what admits the parameter.
    Fixture f(0x40, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    f.material.unk1 &= ~kAuviFlag;
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 0 && coords[1] == 1 && coords[2] == 2 && coords[3] == 3 && coords[4] == 4 && coords[5] == 5 &&
              coords[6] == 6,
          "an AUVI without the flag is not read");
  }
  {  // Only the first three values are read; a coord past them has no channel.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(2, 1, 0, 3));
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 2 && coords[1] == 1 && coords[2] == 0, "the three output channels are remapped");
    Check(coords[3] == 3 && coords[4] == 4 && coords[5] == 5 && coords[6] == 6,
          "a coord past the third output channel is left alone");
  }
  {  // Every coord is remapped, the second layer's three included: they are the
     // same array the converter gathers the primary four into.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    uint32_t coords[7] = {0, 2, 1, 2, 0, 2, 1};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 1 && coords[1] == 0 && coords[2] == 0 && coords[3] == 0 && coords[4] == 1 && coords[5] == 0 &&
              coords[6] == 0,
          "the three output channels of all seven coords are remapped, the second layer's three included");
  }
  {  // Two AUVIs are a later saying again, not a chain to walk the coords through:
     // (1,0,0) then (0,2,0) leaves texcoord 0 on 0, while chaining walks it 0 -> 1 -> 2.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    f.material.data.push_back(f.Auvi(0, 2, 0, 0));
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 0 && coords[1] == 2 && coords[2] == 0,
          "only the last Int4 AUVI is applied, once (chaining would give coord 0 = 2)");
  }
  {  // An AUVI of another shape is not the parameter.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    f.material.data.push_back(f.Auvi(3, 3, 3, 3, ModelMaterialData::Kind::Scalar));
    uint32_t coords[7] = {0, 0, 0, 0, 0, 0, 0};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 1, "a later AUVI that is not an Int4 is ignored");
    Fixture g(kAuviFlag, 0, 0);
    g.material.data.push_back(g.Auvi(3, 3, 3, 3, ModelMaterialData::Kind::Int1));
    uint32_t again[7] = {0, 0, 0, 0, 0, 0, 0};
    ApplyAuvi(g.material, again, 7);
    Check(again[0] == 0, "an Int1 AUVI is not read either");
  }
  {  // A negative selector is not one the port recognises, so the map keeps the
     // texcoord it had.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, -1, -7, 0));
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 1 && coords[1] == 1 && coords[2] == 2 && coords[3] == 3 && coords[4] == 4 && coords[5] == 5 &&
              coords[6] == 6,
          "a negative mapped value preserves the texcoord");
  }
  Check(Auvi(Fixture(kAuviFlag, 0, 0).material) == nullptr, "Auvi is null without a parameter");
  Check(Auvi(Fixture(0, 0, 0).material) == nullptr, "Auvi is null without the flag");
}

// One conversion of the fixture's material, and the base and MR texcoords the
// written CMDL binds them with.
bool Converted(uint32_t flags, uint32_t baseCoord, uint32_t mrCoord, const std::vector<ModelMaterialData>& params,
               uint32_t& baseOut, uint32_t& mrOut, size_t uvSets = 8) {
  Fixture f(flags, baseCoord, mrCoord);
  for (const ModelMaterialData& p : params) {
    f.material.data.push_back(p);
  }
  std::vector<uint8_t> cmdl;
  std::string error;
  if (!Convert(BuildModel(f.material, uvSets), cmdl, error)) {
    std::fprintf(stderr, "FAIL: the conversion failed: %s\n", error.c_str());
    ++sFailures;
    return false;
  }
  std::vector<uint32_t> coords;
  if (!ReadMapCoords(cmdl, coords) || coords.size() < 2) {
    std::fprintf(stderr, "FAIL: no PBR material in the converted CMDL\n");
    ++sFailures;
    return false;
  }
  baseOut = coords[0];
  mrOut = coords[1];
  return true;
}

// The same rule through the converter, where a remap that never reaches the
// material it belongs to shows up as the base or the MR map still on its own
// texcoord.
void TestConverter() {
  Fixture probe(kAuviFlag, 0, 2);
  uint32_t base = 0xFFFFFFFFu, mr = 0xFFFFFFFFu;

  {  // AUVI (1,0,0): the base's channel 0 is fed from texcoord set 1 and the MR
     // map's channel 2 from set 0.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with an AUVI");
    Check(base == 1 && mr == 0, "AUVI (1,0,0) sends base channel 0 to UV1 and MR channel 2 to UV0");
  }
  {  // The flag is what admits the parameter.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0)};
    Check(Converted(0, 0, 2, params, base, mr), "converted without the flag");
    Check(base == 0 && mr == 2, "without the flag every map keeps its own texcoord");
  }
  {  // And with no parameter to read, the port's own fallback stands.
    Check(Converted(kAuviFlag, 0, 2, {}, base, mr), "converted with no AUVI");
    Check(base == 0 && mr == 2, "with no AUVI every map keeps its own texcoord");
  }
  {  // A coord past the vertex shader's third output channel has no channel to go
     // through, so it is left where it was (the model's texcoord set 4).
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0)};
    Check(Converted(kAuviFlag, 4, 2, params, base, mr), "converted with a coord past the third");
    Check(base == 4 && mr == 0, "a coord past the third output channel is unchanged");
  }
  {  // Two AUVIs: the last one counts, applied once. Chaining them would walk the
     // base map's texcoord 0 through 1 and out to 2.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0), probe.Auvi(0, 2, 0, 0)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with two AUVIs");
    Check(base == 0 && mr == 0, "the last Int4 AUVI is applied once (chaining would give base = 2)");
  }
  {  // A negative selector is not one the port recognises: the map keeps its own.
    const std::vector<ModelMaterialData> params{probe.Auvi(-1, -1, -1, 0)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with a negative AUVI");
    Check(base == 0 && mr == 2, "a negative mapped value preserves the texcoord");
  }
  {  // An AUVI after the Int4 one that is not an Int4 is not read, so the Int4
     // still counts.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0),
                                                probe.Auvi(3, 3, 3, 3, ModelMaterialData::Kind::Scalar)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with a non-Int4 AUVI");
    Check(base == 1 && mr == 0, "a later AUVI of another shape is ignored");
  }
}

// Two maps on one source set through different ANUV transforms: the gun model's
// entry moves transform 1 and leaves 0 still. With AUVI (0,0,0,0) both maps read
// set 0, and one texgen would carry the base along with the MR map's motion, so the
// MR map must be given a texcoord of its own.
void TestSlotSplit() {
  Fixture f(kAuviFlag, 0, 1);
  f.material.data.push_back(f.Auvi(0, 0, 0, 0));
  Model model = BuildModel(f.material, 2);
  model.anuv.assign(kAnuvGun, kAnuvGun + sizeof(kAnuvGun));
  std::vector<uint8_t> cmdl;
  std::string error;
  Check(Convert(model, cmdl, error), "converted with an ANUV");
  std::vector<uint32_t> coords;
  Check(ReadMapCoords(cmdl, coords) && coords.size() >= 2, "a PBR material with an ANUV");
  if (coords.size() >= 2) {
    Check(coords[0] != coords[1], "the moving map gets its own texcoord");
  }
  // With all eight texcoords already declared there is no slot to give, and the
  // maps share (the first transform then drives both).
  model.vertexBuffers[0].uvs.resize(8, model.vertexBuffers[0].uvs[0]);
  cmdl.clear();
  coords.clear();
  Check(Convert(model, cmdl, error) && ReadMapCoords(cmdl, coords) && coords.size() >= 2 && coords[0] == coords[1],
        "with no texcoord left the maps share");
  // Without the ANUV the same material shares set 0.
  model.anuv.clear();
  cmdl.clear();
  coords.clear();
  Check(Convert(model, cmdl, error) && ReadMapCoords(cmdl, coords) && coords.size() >= 2 && coords[0] == coords[1],
        "without an ANUV the maps share their set");
}

} // namespace

int main() {
  TestRule();
  TestConverter();
  TestSlotSplit();
  if (sFailures == 0) {
    std::printf("port_remastered_uv_tests: ok\n");
  }
  return sFailures == 0 ? 0 : 1;
}
