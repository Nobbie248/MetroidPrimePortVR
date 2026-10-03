// Remastered model -> GameCube CMDL/CSKR/TXTR. See port_remastered_convert.h.
//
// The arithmetic follows the reference converter this was ported from step for
// step (double precision geometry, the order sums are taken in, round half to
// even), so that both produce the same bytes for the same model and one can be
// checked against the other. Where a line looks needlessly particular about
// the order of an addition, that is why.

#include "port_remastered_convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "port_remastered_pak.h"

namespace PortRemastered {
namespace {

struct Fail {
  std::string what;
};

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) |
         uint32_t(uint8_t(d));
}

struct Span {
  const uint8_t* p = nullptr;
  size_t n = 0;
};

uint32_t R32(Span s, size_t o) {
  if (o > s.n || s.n - o < 4) {
    throw Fail{"a retail resource is truncated"};
  }
  return (uint32_t(s.p[o]) << 24) | (uint32_t(s.p[o + 1]) << 16) | (uint32_t(s.p[o + 2]) << 8) | s.p[o + 3];
}

uint16_t R16(Span s, size_t o) {
  if (o > s.n || s.n - o < 2) {
    throw Fail{"a retail resource is truncated"};
  }
  return uint16_t((s.p[o] << 8) | s.p[o + 1]);
}

uint8_t R8(Span s, size_t o) {
  if (o >= s.n) {
    throw Fail{"a retail resource is truncated"};
  }
  return s.p[o];
}

float RF(Span s, size_t o) {
  const uint32_t v = R32(s, o);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

Span Sub(Span s, size_t o, size_t n) {
  if (o > s.n || s.n - o < n) {
    throw Fail{"a retail resource is truncated"};
  }
  return Span{s.p + o, n};
}

// A slice the way the reference takes one: clamped to what is there.
Span Slice(Span s, size_t o, size_t n) {
  if (o > s.n) {
    o = s.n;
  }
  return Span{s.p + o, std::min(n, s.n - o)};
}

using Blob = std::vector<uint8_t>;

void P8(Blob& b, uint8_t v) { b.push_back(v); }
void P16(Blob& b, uint32_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void P32(Blob& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void PF(Blob& b, double v) {
  const float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  P32(b, u);
}
void Pad(Blob& b, size_t a = 32) { b.resize((b.size() + a - 1) & ~(a - 1), 0); }
void Append(Blob& b, const Blob& o) { b.insert(b.end(), o.begin(), o.end()); }

size_t Align(size_t n, size_t a = 32) { return (n + a - 1) & ~(a - 1); }

std::string Hex8(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", v);
  return buf;
}

std::string FormatG(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return s;
}

uint32_t Crc32(const std::string& s) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      table[i] = c;
    }
    ready = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : s) {
    c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

// A material parameter as the reference reads it: the float's shortest decimal
// form, read back as a double (it parses them out of a text table).
double ShortestDouble(float f) {
  char buf[40];
  for (int precision = 1; precision <= 9; ++precision) {
    std::snprintf(buf, sizeof(buf), "%.*g", precision, double(f));
    if (std::strtof(buf, nullptr) == f) {
      break;
    }
  }
  return std::strtod(buf, nullptr);
}

// A sum over a row the way numpy takes it (eight running sums past seven
// elements, halves past 128), since a share that sits on a threshold must fall
// on the same side here.
double RowSum(const double* a, size_t n) {
  if (n < 8) {
    double r = 0.0;
    for (size_t i = 0; i < n; ++i) {
      r += a[i];
    }
    return r;
  }
  if (n <= 128) {
    double r[8];
    for (int k = 0; k < 8; ++k) {
      r[k] = a[k];
    }
    size_t i = 8;
    for (; i < n - (n % 8); i += 8) {
      for (int k = 0; k < 8; ++k) {
        r[k] += a[i + k];
      }
    }
    double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) {
      res += a[i];
    }
    return res;
  }
  size_t half = n / 2;
  half -= half % 8;
  return RowSum(a, half) + RowSum(a + half, n - half);
}

// For each of the na points in a, the nearest of the nb points in b (the first
// on a tie).
void Nearest(const double* a, size_t na, const double* b, size_t nb, std::vector<uint32_t>& out) {
  out.resize(na);
  for (size_t i = 0; i < na; ++i) {
    const double x = a[3 * i], y = a[3 * i + 1], z = a[3 * i + 2];
    double best = 0.0;
    uint32_t at = 0;
    for (size_t j = 0; j < nb; ++j) {
      const double dx = x - b[3 * j], dy = y - b[3 * j + 1], dz = z - b[3 * j + 2];
      const double d = (dx * dx + dy * dy) + dz * dz;
      if (j == 0 || d < best) {
        best = d;
        at = uint32_t(j);
      }
    }
    out[i] = at;
  }
}

// ---- retail side ----

struct RetailMaterial {
  uint32_t flags = 0;
  std::vector<uint32_t> tex;
  uint32_t vtx = 0;
  uint32_t group = 0;
  uint16_t blendSrc = 0, blendDst = 0;
  std::vector<uint32_t> chans;
  struct Tev {
    uint32_t color, alpha, colorOp, alphaOp;
  };
  std::vector<Tev> tev;
  std::vector<std::array<uint8_t, 4>> tevTex;
  std::vector<uint32_t> texgen;
  std::vector<uint8_t> uvAnim;  // the section as it is: its size, the count and the animations
};

RetailMaterial ParseMaterial(Span m) {
  RetailMaterial out;
  size_t o = 0;
  out.flags = R32(m, o);
  o += 4;
  const uint32_t nt = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < nt; ++i, o += 4) {
    out.tex.push_back(R32(m, o));
  }
  out.vtx = R32(m, o);
  o += 4;
  out.group = R32(m, o);
  o += 4;
  if (out.flags & 0x8) {
    const uint32_t nk = R32(m, o);
    o += 4 + size_t(nk) * 4;
  }
  out.blendDst = R16(m, o);
  out.blendSrc = R16(m, o + 2);
  o += 4;
  if (out.flags & 0x400) {
    o += 4;
  }
  const uint32_t nch = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < nch; ++i, o += 4) {
    out.chans.push_back(R32(m, o));
  }
  const uint32_t ntev = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < ntev; ++i, o += 20) {
    out.tev.push_back({R32(m, o), R32(m, o + 4), R32(m, o + 8), R32(m, o + 12)});
  }
  for (uint32_t i = 0; i < ntev; ++i, o += 4) {
    out.tevTex.push_back({R8(m, o), R8(m, o + 1), R8(m, o + 2), R8(m, o + 3)});
  }
  const uint32_t ntg = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < ntg; ++i, o += 4) {
    out.texgen.push_back(R32(m, o));
  }
  // The UV animations load the matrices those texgens name: without them a texgen
  // reads whatever matrix the material drawn before left.
  if (o <= m.n && m.n - o >= 8) {
    const Span anim = Slice(m, o, 4 + size_t(R32(m, o)));
    if (anim.n >= 8 && anim.n == 4 + size_t(R32(m, o))) {
      out.uvAnim.assign(anim.p, anim.p + anim.n);
    }
  }
  return out;
}

struct MaterialSet {
  std::vector<uint32_t> tex;
  std::vector<Span> mats;
};

MaterialSet ParseSet(Span sec) {
  MaterialSet out;
  const uint32_t nt = R32(sec, 0);
  size_t o = 4;
  for (uint32_t i = 0; i < nt; ++i, o += 4) {
    out.tex.push_back(R32(sec, o));
  }
  const uint32_t nm = R32(sec, o);
  o += 4;
  const size_t base = o + size_t(nm) * 4;
  uint32_t start = 0;
  for (uint32_t i = 0; i < nm; ++i, o += 4) {
    const uint32_t end = R32(sec, o);
    if (end < start) {
      throw Fail{"a retail material table is out of order"};
    }
    out.mats.push_back(Sub(sec, base + start, end - start));
    start = end;
  }
  return out;
}

// Bytes per vertex in a display list: matrix indices are u8, attributes u16.
size_t VtxStride(uint32_t vtx) {
  size_t n = 0;
  for (int bit = 24; bit < 32; ++bit) {
    if (vtx & (1u << bit)) {
      ++n;
    }
  }
  for (int sh = 0; sh < 24; sh += 2) {
    if ((vtx >> sh) & 3) {
      n += 2;
    }
  }
  return n;
}

int Popcount(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) {
    ++n;
  }
  return n;
}

// Position indices of a display list, as a flat triangle list.
void Triangles(Span dl, size_t stride, size_t mtx, std::vector<uint32_t>& tris) {
  size_t o = 0;
  std::vector<uint32_t> idx;
  while (o + 3 <= dl.n) {
    const uint8_t op = dl.p[o] & 0xF8;
    if (op != 0x80 && op != 0x90 && op != 0x98 && op != 0xA0 && op != 0xA8 && op != 0xB0 && op != 0xB8) {
      break;
    }
    const size_t n = R16(dl, o + 1);
    o += 3;
    if (n * stride > dl.n - o || (n && stride < mtx + 2)) {
      break;
    }
    idx.resize(n);
    for (size_t k = 0; k < n; ++k) {
      idx[k] = R16(dl, o + k * stride + mtx);
    }
    o += n * stride;
    if (op == 0x90) {
      tris.insert(tris.end(), idx.begin(), idx.begin() + n / 3 * 3);
    } else if (op == 0x98) {
      for (size_t k = 0; k + 2 < n; ++k) {
        if (k % 2 == 0) {
          tris.insert(tris.end(), {idx[k], idx[k + 1], idx[k + 2]});
        } else {
          tris.insert(tris.end(), {idx[k + 1], idx[k], idx[k + 2]});
        }
      }
    } else if (op == 0xA0) {
      for (size_t k = 1; k + 1 < n; ++k) {
        tris.insert(tris.end(), {idx[0], idx[k], idx[k + 1]});
      }
    } else if (op == 0x80) {
      for (size_t k = 0; k + 3 < n; k += 4) {
        tris.insert(tris.end(), {idx[k], idx[k + 1], idx[k + 2], idx[k], idx[k + 2], idx[k + 3]});
      }
    }
  }
}

struct Retail {
  Blob data;
  uint32_t flags = 0;
  uint32_t nmat = 0;
  std::vector<Span> secs;
  std::vector<MaterialSet> sets;
  std::vector<RetailMaterial> mats;  // of set 0
  std::vector<double> P;             // every position, xyz
  std::vector<int> vmat;             // the material drawing each position, -1 for none

  size_t Count() const { return vmat.size(); }

  void Parse() {
    const Span d{data.data(), data.size()};
    flags = R32(d, 8);
    const uint32_t nsec = R32(d, 0x24);
    nmat = R32(d, 0x28);
    if (nsec > 0x100000 || nmat == 0 || size_t(nmat) + 6 > nsec) {
      throw Fail{"the retail model has an unexpected layout"};
    }
    size_t o = Align(0x2c + size_t(nsec) * 4);
    for (uint32_t i = 0; i < nsec; ++i) {
      const uint32_t size = R32(d, 0x2c + size_t(i) * 4);
      secs.push_back(Sub(d, o, size));
      o += size;
    }
    for (uint32_t i = 0; i < nmat; ++i) {
      sets.push_back(ParseSet(secs[i]));
    }
    for (const Span& m : sets[0].mats) {
      mats.push_back(ParseMaterial(m));
    }
    // The section is padded to 32 bytes, so the padding reads as a few more
    // vertices at the origin; nothing draws them, and they keep their place.
    const Span ps = secs[nmat];
    const size_t np = ps.n / 4 / 3;
    P.resize(np * 3);
    for (size_t i = 0; i < np * 3; ++i) {
      P[i] = RF(ps, i * 4);
    }
    vmat.assign(np, -1);
    const uint32_t nsurf = R32(secs[nmat + 5], 0);
    std::vector<uint32_t> tris;
    for (size_t si = nmat + 6; si < secs.size() && si < size_t(nmat) + 6 + nsurf; ++si) {
      const Span s = secs[si];
      const uint32_t mat = R32(s, 12);
      const uint32_t dlSize = R32(s, 16) & 0x7FFFFFFF;
      const uint32_t extra = R32(s, 28);
      if (mat >= mats.size()) {
        throw Fail{"a retail surface names a material that does not exist"};
      }
      const uint32_t vtx = mats[mat].vtx;
      tris.clear();
      Triangles(Slice(s, Align(44 + size_t(extra)), dlSize), VtxStride(vtx), size_t(Popcount(vtx >> 24)), tris);
      for (uint32_t t : tris) {
        if (t < np && vmat[t] < 0) {
          vmat[t] = int(mat);
        }
      }
    }
  }
};

bool IsFx(const RetailMaterial& m) { return !(m.blendSrc == 1 && m.blendDst == 0); }

enum class Role { EnvMap, Lightmap, Reflect, Emissive, Diffuse, Keep };

const char* RoleName(Role r) {
  switch (r) {
  case Role::EnvMap:
    return "envmap";
  case Role::Lightmap:
    return "lightmap";
  case Role::Reflect:
    return "reflect";
  case Role::Emissive:
    return "emissive";
  case Role::Diffuse:
    return "diffuse";
  default:
    return "keep";
  }
}

struct SlotRole {
  uint32_t slot;
  uint32_t coord;
  Role role;
  int uvSrc;  // which texcoord attribute the stage's texgen reads, negative for none
};

// What each textured TEV stage of a retail material does with its texture.
std::vector<SlotRole> SlotRoles(const RetailMaterial& m) {
  // TEV colour inputs: CPREV APREV C0 A0 C1 A1 C2 A2 TEXC TEXA RASC RASA ONE HALF KONST ZERO
  enum { CPREV = 0, TEXC = 8, RASC = 10, ONE = 12 };
  std::vector<SlotRole> out;
  const size_t n = std::min(m.tev.size(), m.tevTex.size());
  for (size_t i = 0; i < n; ++i) {
    const uint32_t slot = m.tevTex[i][2], coord = m.tevTex[i][3];
    if (slot == 0xFF || coord >= m.texgen.size()) {
      continue;
    }
    const uint32_t tg = m.texgen[coord];
    const uint32_t src = (tg >> 4) & 31;
    const uint32_t c = m.tev[i].color;
    const uint32_t b = (c >> 5) & 15, cc = (c >> 10) & 15, d = (c >> 15) & 15;
    const uint32_t outReg = (m.tev[i].colorOp >> 9) & 3;
    Role role;
    if (src < 4) {
      role = Role::EnvMap;  // a sphere-mapped texgen over position/normal
    } else if (outReg == 1) {
      role = Role::Lightmap;
    } else if (outReg == 3) {
      role = Role::Reflect;
    } else if ((b == CPREV || d == CPREV) && (b == TEXC || d == TEXC) && cc == ONE) {
      role = Role::Emissive;
    } else if (d == TEXC && b == ONE && cc == RASC) {
      role = Role::Emissive;
    } else {
      role = Role::Diffuse;
    }
    out.push_back({slot, coord, role, int(src) - 4});
  }
  return out;
}

// Retail CSKR: weight groups (bone, weight)... and how many vertices each covers.
struct SkinGroup {
  std::vector<std::pair<uint32_t, float>> weights;
  uint32_t count = 0;
};

std::vector<SkinGroup> ReadCskr(Span d) {
  std::vector<SkinGroup> groups;
  const uint32_t n = R32(d, 0);
  size_t o = 4;
  for (uint32_t i = 0; i < n; ++i) {
    SkinGroup g;
    const uint32_t nw = R32(d, o);
    o += 4;
    for (uint32_t k = 0; k < nw; ++k, o += 8) {
      g.weights.emplace_back(R32(d, o), RF(d, o));
      g.weights.back().second = RF(d, o + 4);
    }
    g.count = R32(d, o);
    o += 4;
    groups.push_back(std::move(g));
  }
  return groups;
}

std::vector<uint32_t> SkinBones(Span d) {
  std::vector<uint32_t> bones;
  for (const SkinGroup& g : ReadCskr(d)) {
    for (const auto& w : g.weights) {
      bones.push_back(w.first);
    }
  }
  std::sort(bones.begin(), bones.end());
  bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
  return bones;
}

// ---- Remastered side ----

enum { kBase = 0, kMr = 1, kNormal = 2, kEmissive = 3, kMaps = 4, kLayeredMaps = 7 };
const char* const kMapName[kMaps] = {"base", "mr", "normal", "emissive"};
// The texel a missing map gets: white, no occlusion / mid roughness / no metal,
// a normal pointing straight out, black.
const char* const kMapNeutral[kMaps] = {"ffffff", "ff9900", "8080ff", "000000"};
const int kPbrMax[kMaps] = {1024, 256, 256, 256};
// Each PBR map is drawn from its .dds, outside the game heap, at up to
// kPbrNative; the TXTR beside it only supplies the sampler state (and is what
// shows where BC textures are unsupported), so it is a stub.
const int kPbrNative[kMaps] = {2048, 1024, 1024, 1024};
const int kPbrStub[kMaps] = {128, 32, 32, 32};
// Multiplier on the PBR emissive map: Remastered authored it for an HDR
// pipeline, and at 1.0 it washes the albedo to grey.
const double kPbrEmissive = 0.35;
// Ceiling on the metalness channel. A full metal has no diffuse, and Prime's
// rooms are dim, so the painted shells go near-black above it.
const double kPbrMetalMax = 0.6;
const double kPbrEmissiveMax = 16.0;
const double kFlatStd = 3.0;
const double kJointSplit = 0.05;
// The game's skin reader keeps three weights a vertex and drops the rest
// without renormalising, so a fourth would leave the vertex pulled to the origin.
const size_t kMaxSkinWeights = 3;
const uint32_t kPbrFlag = 0x4000;  // kStateFlag_PortPBR

struct MapRef {
  bool has = false;
  ModelUuid id{};
  uint32_t coord = 0;
  std::string src;  // how the texture is named in a tag
  bool mean = false;  // drawn as its colour times alpha, averaged: one colour
};

struct RemMaterial {
  std::string name;
  MapRef maps[kMaps];
  double emissive = 1.0;   // Remastered's emissive strength
  double backlight = 0.0;  // and its backlight strength
  bool cutout = false;     // the base map's alpha cuts holes: leaves, grates
  bool blended = false;    // drawn over what is behind it: glass, decals, ice
  bool tinted = false;     // its vertices carry a colour
  bool unlit = false;      // a screen: its own colour and glow, no lighting
  bool mask = false;       // the base map's alpha masks the glow and is no opacity
  double height = 0.0;     // above 0: the threshold of a height-blended alpha
  // A second layer (base, MR, normal) the vertex alpha blends over the first
  // by the two base maps' heights: snow on rock, moss on stone.
  bool layered = false;
  MapRef layer[3];
  double layerSmooth = 0.0;                      // BLSM: the width of the blend's edge
  double layerHeight[4] = {1.0, 0.0, 1.0, 0.0};  // BSAO: scale and offset of each layer's height
  // A shader of its own the port draws (GXSetPBRMaterial's kinds): 1 a second
  // layer on what faces up, 2 a detail map, 3 lava, 4 ice seen into, 5 a
  // liquid's surface (water, poison), 6 a lava pool's, 7 falling water, 8 glass.
  int kind = 0;
  double kindStrength = 0.0;
  double kindParam[4] = {0.0, 0.0, 0.0, 0.0};
  bool vcolor = false;  // it reads the vertex colour, which is no tint
  double tint[3] = {0.0, 0.0, 0.0};  // kinds 5 and 7: the liquid's colour; 8: what is seen through it
  bool hidden = false;  // not drawn: the game has its own
};

struct Buffer {
  size_t n = 0;
  size_t offset = 0;  // of its first vertex in the unified list
  bool loaded = false;
  bool used = false;
  std::vector<double> P, N;
  std::vector<std::vector<double>> uv;
  std::vector<uint8_t> C;  // rgba per vertex, white where the buffer has no colours
  bool colored = false;
  const ModelVertexBuffer* src = nullptr;
  bool skinned = false;
};

struct Prim {
  uint32_t buffer = 0;
  uint32_t mat = 0;
  std::vector<uint32_t> I;
  int rmat = 0;
  int omat = 0;
};

using WeightKey = std::vector<std::pair<uint32_t, double>>;

bool ContainsNoCase(const std::string& lower, const char* needle) { return lower.find(needle) != std::string::npos; }

// Names of effect materials: glow shells, pickups' halos.
bool IsFxName(const std::string& name) {
  const std::string l = Lower(name);
  return ContainsNoCase(l, "pickup") || ContainsNoCase(l, "vfx") || ContainsNoCase(l, "glow") ||
         ContainsNoCase(l, "additive") || ContainsNoCase(l, "_fx") || ContainsNoCase(l, "fx_");
}

int NextPow2(int v) {
  int p = 1;
  while (p < std::max(1, v)) {
    p <<= 1;
  }
  return p;
}

} // namespace

// ---- textures ----

struct Converter::State {
  ConvertIO io;
  std::map<std::string, std::optional<uint32_t>> ids;  // tag -> texture id, or none where retail's stays
  std::map<uint32_t, std::string> owner;               // texture id -> the tag it was written for
  struct Mean {
    float rgb[3];
    int peak;
  };
  std::map<std::string, Mean> means;
  // The last few textures decoded: a map that was measured is usually written
  // straight after, and a bake reads its neighbours.
  std::vector<std::pair<std::string, Image>> opened;
  int pbr = 0, tev = 0;

  void Log(const std::string& line) const {
    if (io.log) {
      io.log(line);
    }
  }

  const Image& Open(const MapRef& map) {
    for (auto& e : opened) {
      if (e.first == map.src) {
        return e.second;
      }
    }
    Image img;
    std::string error;
    if (!io.texture(map.id, img, error)) {
      throw Fail{"texture " + IdToString(map.id) + ": " + error};
    }
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() != size_t(img.width) * size_t(img.height) * 4) {
      throw Fail{"texture " + IdToString(map.id) + " decoded to nothing"};
    }
    // Single precision running sums, in pixel order. On a large bright texture
    // these stop growing and the mean reads low; the thresholds in PbrEmissive
    // were measured against exactly that, so it is kept.
    Mean m{{0.0f, 0.0f, 0.0f}, 0};
    const size_t count = img.rgba.size() / 4;
    for (size_t i = 0; i < count; ++i) {
      for (int c = 0; c < 3; ++c) {
        const uint8_t v = img.rgba[i * 4 + c];
        m.rgb[c] += float(v);
        m.peak = std::max(m.peak, int(v));
      }
    }
    for (float& v : m.rgb) {
      v /= float(count);
    }
    means[map.src] = m;
    if (opened.size() >= 3) {
      opened.erase(opened.begin());
    }
    opened.emplace_back(map.src, std::move(img));
    return opened.back().second;
  }

  Mean MeanOf(const MapRef& map) {
    auto it = means.find(map.src);
    if (it == means.end()) {
      Open(map);
      it = means.find(map.src);
    }
    return it->second;
  }

  // Whether the emissive map adds anything on the PBR path, which sums it over
  // the lit base. A black map does not (every channel's mean under 4 and no
  // texel over 32: the mean alone would also drop small lights on a black
  // field), and neither does a near-copy of the base, which doubles the colour.
  bool PbrEmissive(const MapRef* maps) {
    const Mean e = MeanOf(maps[kEmissive]);
    if (std::max({e.rgb[0], e.rgb[1], e.rgb[2]}) < 4.0f && e.peak <= 32) {
      return false;
    }
    if (maps[kBase].has) {
      const Mean b = MeanOf(maps[kBase]);
      float diff = 0.0f;
      for (int c = 0; c < 3; ++c) {
        diff = std::max(diff, std::fabs(e.rgb[c] - b.rgb[c]));
      }
      const float es = (e.rgb[0] + e.rgb[1]) + e.rgb[2];
      const float bs = (b.rgb[0] + b.rgb[1]) + b.rgb[2];
      const float ratio = bs > 1e-6f ? es / bs : float(double(es) / 1e-6);
      if (diff < 6.0f && std::fabs(ratio - 1.0f) < 0.06f) {
        return false;
      }
    }
    return true;
  }

  // A flat fill has no surface detail to convert: Remastered keeps such a
  // surface's pattern in its shader.
  static bool FlatBase(const Image& img) {
    if (std::max(img.width, img.height) <= 4) {
      return true;
    }
    const size_t count = img.rgba.size() / 4;
    uint64_t sum = 0, squares = 0;
    for (size_t i = 0; i < count; ++i) {
      for (int c = 0; c < 3; ++c) {
        const uint64_t v = img.rgba[i * 4 + c];
        sum += v;
        squares += v * v;
      }
    }
    const long double n = (long double)(count * 3);
    const long double mean = (long double)sum / n;
    const long double var = (long double)squares / n - mean * mean;
    return std::sqrt(double(var > 0 ? var : 0)) < kFlatStd;
  }

  uint32_t TexId(const std::string& tag) const {
    uint32_t h = Crc32("mprem:" + tag);
    while (io.retailId && io.retailId(h)) {
      ++h;
    }
    return h;
  }

  // How much alpha the retail texture in a slot carries, so the replacement
  // keeps as much: "none", "punch" (CMPR with a transparent texel) or "full".
  std::string RetailAlpha(uint32_t id) const {
    Blob d;
    if (!io.retail(FourCC('T', 'X', 'T', 'R'), id, d) || d.size() < 12) {
      return "none";
    }
    const Span s{d.data(), d.size()};
    const uint32_t fmt = R32(s, 0);
    const uint32_t w = R16(s, 4), h = R16(s, 6);
    if (fmt == 0 || fmt == 1 || fmt == 7) {
      return "none";
    }
    if (fmt != 10) {
      return "full";
    }
    const size_t bytes = std::min(size_t(std::max(8u, w)) * std::max(8u, h) / 2, d.size() - 12) / 8 * 8;
    for (size_t o = 12; o < 12 + bytes; o += 8) {
      const uint16_t c0 = R16(s, o), c1 = R16(s, o + 2);
      if (c0 > c1) {
        continue;
      }
      for (int k = 0; k < 4; ++k) {
        const uint8_t b = d[o + 4 + k];
        if ((b & 0xC0) == 0xC0 || (b & 0x30) == 0x30 || (b & 0x0C) == 0x0C || (b & 0x03) == 0x03) {
          return "punch";
        }
      }
    }
    return "none";
  }

  void Write(const std::string& name, const Blob& data) const {
    if (!io.write(name, data)) {
      throw Fail{"could not write " + name};
    }
  }

  static Image Solid(uint8_t r, uint8_t g, uint8_t b) {
    Image img;
    img.width = img.height = 8;
    img.rgba.resize(8 * 8 * 4);
    for (size_t i = 0; i < 64; ++i) {
      img.rgba[i * 4] = r;
      img.rgba[i * 4 + 1] = g;
      img.rgba[i * 4 + 2] = b;
      img.rgba[i * 4 + 3] = 255;
    }
    return img;
  }

  struct Bake {
    bool on = false;
    const MapRef* mr = nullptr;
    const MapRef* normal = nullptr;
    double ao = 0.6, cavity = 0.5, spec = 0.35;
  };

  // Remastered's shading folded into the base colour, texel-local, for the TEV
  // path: occlusion (MR red), cavities (normal-map tilt) and a sheen on smooth
  // metal (MR blue metal, green roughness).
  Image Baked(const Image& base, const Bake& bake) {
    const size_t count = size_t(base.width) * size_t(base.height);
    std::vector<float> shade(count, 1.0f), lift(count, 0.0f);
    auto load = [&](const MapRef& map) {
      const Image& img = Open(map);
      return img.width == base.width && img.height == base.height ? img : Resize(img, base.width, base.height);
    };
    if (bake.mr) {
      const Image t = load(*bake.mr);
      for (size_t i = 0; i < count; ++i) {
        const float r = t.rgba[i * 4] / 255.0f, g = t.rgba[i * 4 + 1] / 255.0f, b = t.rgba[i * 4 + 2] / 255.0f;
        shade[i] *= 1.0f - float(bake.ao) * (1.0f - r);
        lift[i] = float(bake.spec) * b * (1.0f - g) * (1.0f - g);
      }
    }
    if (bake.normal) {
      const Image t = load(*bake.normal);
      for (size_t i = 0; i < count; ++i) {
        const float nx = t.rgba[i * 4] / 255.0f * 2.0f - 1.0f, ny = t.rgba[i * 4 + 1] / 255.0f * 2.0f - 1.0f;
        const float nz = std::sqrt(std::clamp(1.0f - nx * nx - ny * ny, 0.0f, 1.0f));
        shade[i] *= 1.0f - float(bake.cavity) * (1.0f - nz);
      }
    }
    Image out = base;
    for (size_t i = 0; i < count; ++i) {
      for (int c = 0; c < 3; ++c) {
        float v = base.rgba[i * 4 + c] / 255.0f;
        v *= shade[i];
        v += (1.0f - v) * lift[i] * shade[i];  // screen-style, stays under white
        out.rgba[i * 4 + c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    return out;
  }

  // The texture id for a role of a Remastered material, converting and writing
  // the texture the first time its tag is seen. `role` is "pbr:<map>" or a TEV
  // slot role; `alpha` is RetailAlpha() of the texture a TEV slot replaces.
  // No value means the retail texture stays.
  std::optional<uint32_t> Get(const std::string& role, const MapRef* rt, std::string alpha,
                              const ConvertOptions& opt) {
    const MapRef* src = nullptr;
    std::string tag;
    int ncap = 0;  // largest edge of the native .dds, 0 for none
    int k = -1;    // the PBR map, on that path
    Bake bake;
    if (role.rfind("pbr:", 0) == 0) {
      for (int i = 0; i < kMaps; ++i) {
        if (role.compare(4, std::string::npos, kMapName[i]) == 0) {
          k = i;
        }
      }
      if (rt[k].has && (k != kEmissive || rt[k].mean || PbrEmissive(rt))) {
        src = &rt[k];
        tag = std::string("pbr:") + kMapName[k] + ":" + src->src + (src->mean ? ":mean" : "");
        // Named fields, not positional: the size and the alpha follow them.
        if (k == kEmissive && !src->mean) {
          tag += ":escale=" + FormatG(kPbrEmissive);
        }
        if (k == kMr) {
          tag += ":mmax=" + FormatG(kPbrMetalMax);
        }
      } else {
        tag = std::string("const:") + kMapNeutral[k];
      }
    } else if (role == "diffuse" && rt[kBase].has) {
      src = &rt[kBase];
      tag = "base:" + src->src;
      // Only maps sharing the base's texcoord can be baked texel for texel.
      const uint32_t uv = rt[kBase].coord;
      bake.mr = rt[kMr].has && rt[kMr].coord == uv ? &rt[kMr] : nullptr;
      bake.normal = rt[kNormal].has && rt[kNormal].coord == uv ? &rt[kNormal] : nullptr;
      if (!bake.mr) {
        bake.ao = bake.spec = 0.0;
      }
      if (!bake.normal) {
        bake.cavity = 0.0;
      }
      bake.on = bake.ao != 0.0 || bake.cavity != 0.0 || bake.spec != 0.0;
      if (bake.on) {
        tag += ":bake:" + (bake.mr ? bake.mr->src : std::string("-")) + ":" +
               (bake.normal ? bake.normal->src : std::string("-")) + ":" + FormatG(bake.ao) + ":" +
               FormatG(bake.cavity) + ":" + FormatG(bake.spec) + ":0:0";
      }
    } else if (role == "emissive" && rt[kEmissive].has) {
      src = &rt[kEmissive];
      tag = "emis:" + src->src + ":1";
    } else if (role == "reflect" && rt[kMr].has) {
      src = &rt[kMr];
      tag = "refl:" + src->src + ":0.6";
    } else if (role == "envmap") {
      // A sphere-mapped reflection: its texture is a gradient, not surface
      // detail, so there is nothing to convert from Remastered.
      return std::nullopt;
    } else if (role == "lightmap") {
      tag = "const:c0c0c0";
    } else {
      tag = "const:000000";
    }
    if (role == "reflect" && alpha == "punch") {
      alpha = "none";  // the reflectivity map's alpha is 0 by construction
    }
    int cap;
    if (k >= 0) {
      // The albedo is what shows and can be CMPR; AO/roughness/metal would
      // blotch and the normals would tilt, so the normal map stays RGBA8.
      // A cutout's base is the exception: its alpha is the shape.
      // A blended surface's base keeps its whole alpha, which is its opacity.
      alpha = k == kNormal ? "rgba" : k == kBase && (alpha == "punch" || alpha == "blend" || alpha == "mask") ? alpha : "none";
      cap = kPbrMax[k];
      if (src) {
        ncap = opt.nativeMax > 0 ? std::min(kPbrNative[k], opt.nativeMax) : kPbrNative[k];
        cap = std::min(cap, kPbrStub[k]);
      }
    } else if (alpha == "full") {
      cap = 1024;
    } else {
      cap = opt.maxTexture;
    }
    tag += ":" + std::to_string(cap);
    if (ncap) {
      tag += ":dds" + std::to_string(ncap);
    }
    if (src && alpha != "rgba") {
      tag += ":" + alpha;
    }
    const auto known = ids.find(tag);
    if (known != ids.end()) {
      return known->second;
    }
    const uint32_t tid = TexId(tag);
    const auto owned = owner.find(tid);
    if (owned != owner.end() && owned->second != tag) {
      throw Fail{"texture id clash on " + Hex8(tid) + ": " + owned->second + " and " + tag};
    }
    owner[tid] = tag;
    ids[tag] = tid;
    // A PBR map's id is its tag's alone, so whoever wrote it wrote the same file.
    if (k >= 0 && io.claim && !io.claim(tid)) {
      return tid;
    }
    const std::string name = Hex8(tid);
    if (!src) {
      const unsigned long rgb = std::strtoul(tag.substr(6, 6).c_str(), nullptr, 16);
      Write(name + ".TXTR", EncodeTxtrRgba8(Solid(uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb)), 8));
      return tid;
    }
    Image img = Open(*src);
    if (src->mean) {
      double sum[3] = {0.0, 0.0, 0.0};
      const size_t n = img.rgba.size() / 4;
      for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
          sum[c] += double(img.rgba[i * 4 + c]) * double(img.rgba[i * 4 + 3]) / 255.0;
        }
      }
      img = Solid(uint8_t(std::nearbyint(sum[0] / double(n))), uint8_t(std::nearbyint(sum[1] / double(n))),
                  uint8_t(std::nearbyint(sum[2] / double(n))));
    }
    const size_t count = img.rgba.size() / 4;
    const bool isBase = k == kBase || role == "diffuse";
    if (k == kNormal) {
      // Two-channel normal maps leave B at 0; the shader rebuilds z. Keep A opaque.
      for (size_t i = 0; i < count; ++i) {
        img.rgba[i * 4 + 3] = 255;
      }
    }
    if (alpha == "blend") {
      // Remastered's shader squares the base map's alpha into the opacity.
      for (size_t i = 0; i < count; ++i) {
        const unsigned a = img.rgba[i * 4 + 3];
        img.rgba[i * 4 + 3] = uint8_t((a * a + 127) / 255);
      }
    }
    if (isBase && alpha != "punch" && alpha != "blend" && alpha != "mask" && FlatBase(img)) {
      // On the TEV path the retail texture is kept, which beats writing a flat
      // grey over a textured model. On the PBR path a flat albedo is fine: the
      // normal and MR maps carry the surface, so the material gets a solid base.
      if (k != kBase) {
        ids[tag] = std::nullopt;
        return std::nullopt;
      }
      uint64_t sum[3] = {0, 0, 0};
      for (size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) {
          sum[c] += img.rgba[i * 4 + c];
        }
      }
      uint8_t col[3];
      for (int c = 0; c < 3; ++c) {
        col[c] = uint8_t(std::nearbyint(double(sum[c]) / double(count)));
      }
      Write(name + ".TXTR", EncodeTxtrRgba8(Solid(col[0], col[1], col[2]), 8));
      return tid;
    }
    if (bake.on) {
      img = Baked(img, bake);
    }
    // A ramp's mean is already the glow's level, which an emissive map's texels
    // (lit spots on a dark map) are not.
    if (k == kEmissive && !src->mean) {
      const float scale = float(kPbrEmissive);
      for (size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) {
          img.rgba[i * 4 + c] = uint8_t(std::clamp(float(img.rgba[i * 4 + c]) * scale, 0.0f, 255.0f));
        }
      }
    }
    if (k == kMr) {
      const uint8_t ceiling = uint8_t(std::nearbyint(float(kPbrMetalMax * 255.0)));
      for (size_t i = 0; i < count; ++i) {
        img.rgba[i * 4 + 2] = std::min(img.rgba[i * 4 + 2], ceiling);
      }
    }
    if (role == "reflect") {
      // glTF order: G roughness, B metal.
      for (size_t i = 0; i < count; ++i) {
        const float g = img.rgba[i * 4 + 1] / 255.0f, b = img.rgba[i * 4 + 2] / 255.0f;
        const uint8_t v = uint8_t(std::clamp(b * (1.0f - 0.7f * g) * 0.6f, 0.0f, 1.0f) * 255.0f);
        img.rgba[i * 4] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = v;
        img.rgba[i * 4 + 3] = 0;
      }
    }
    if (ncap) {
      const int w = std::max(8, std::min(ncap, NextPow2(img.width)));
      const int h = std::max(8, std::min(ncap, NextPow2(img.height)));
      if (k == kBase && alpha != "punch" && alpha != "blend" && alpha != "mask") {
        for (size_t i = 0; i < count; ++i) {
          img.rgba[i * 4 + 3] = 255;  // alpha "none", as the stub's CMPR
        }
      }
      if (std::max(w, h) > cap) {  // otherwise the stub already holds every texel
        const DdsFormat format = k == kNormal ? DdsFormat::BC5 : DdsFormat::BC7;
        if (w == img.width && h == img.height) {
          Write(name + ".dds", EncodeDds(img, format));
        } else {
          Write(name + ".dds", EncodeDds(Resize(img, w, h), format));
        }
      }
    }
    const int w = std::max(8, std::min(cap, NextPow2(img.width)));
    const int h = std::max(8, std::min(cap, NextPow2(img.height)));
    if (w != img.width || h != img.height) {
      img = Resize(img, w, h);
    }
    if (alpha == "none" || alpha == "punch") {
      Write(name + ".TXTR", EncodeTxtrCmpr(img, alpha == "punch"));
    } else {
      Write(name + ".TXTR", EncodeTxtrRgba8(img, role == "emissive" ? 4 : 8));
    }
    return tid;
  }

  void Convert(const Model& model, const ConvertOptions& opt);
};

namespace {

// Bits of a Remastered material's feature word.
constexpr uint32_t kTransparentFlag = 0x1;   // blended over what is behind it
constexpr uint32_t kVertexColorFlag = 0x10;  // the vertex colour tints it
constexpr uint32_t kCutoutFlag = 0x20;       // one-bit alpha: ground leaves, grates, foliage
constexpr uint32_t kIncanMaskFlag = 0x200;   // the base map's alpha masks the glow
// The first four bytes of the id of a shader whose alpha is read below.
constexpr uint32_t kShaderHeightBlend = 0xCA10C453;  // snow and ice over rock
// Shaders with maps and parameters of their own (TCHn, CCHn), read from their code.
constexpr uint32_t kShaderUpLayer = 0x9EFE0D2E;   // TCH0-2 are a second layer on what faces up, CCH0.x its edge
constexpr uint32_t kShaderDetail = 0x9AB899E7;    // TCH0 is a detail map, on a texcoord of its own
constexpr uint32_t kShaderLava = 0x023388CD;      // the glow is CCH0.x times the vertex alpha
// Falling water, unlit: TCH0's three channels are sheets scrolling at CCH1's and CCH2's speeds
// (times CCH0.x), weighted by the vertex colour, and their sum picks the colour from TCH1, a
// ramp whose row is the vertex alpha. CCH0's y, z and w are the sum's softness and the ramp's
// offsets, CCH3 the colour.
constexpr uint32_t kShaderWaterfall = 0x50412FE7;
constexpr uint32_t kShaderParallax = 0x2F3FB02B;  // TCH0 is seen inside the surface; CCH0 and CCH1.x say how
// Glass: the frame behind it, offset by TCH1's RG noise times CCH0.x and tinted by
// CCH1 x CCH3 x CCH4.x x CCH4.z, where TCH0's R (and the vertex blue) lets it through;
// TCH0's G plus the vertex green glows in CCH2 x CCH4.y, its B is the roughness and its
// A (times the vertex alpha) the opacity of a reflection CCH0.y and CCH1.w scale.
constexpr uint32_t kShaderGlass = 0x231F8383;
// A lava pool's surface (a LavaRenderVolume's model): BCLR is a colour ramp, TCH0 a pattern
// carried along TCH2's flow map in two phases that TCH1's noise offsets, CCH0 the flow's
// strength, its period in seconds and the brightness, CCH1 the maps' scales.
constexpr uint32_t kShaderLavaPool[] = {0x3ADE58B7, 0xB9C24545};
// The arm cannon's beam glow (Wave, Plasma): TCH0's three channels scroll at
// CCH1's and CCH2's speeds and, less the vertex colour, pick a colour from
// TCH1, a ramp whose row is the vertex alpha and whose alpha scales it, times
// CCH0.z. It is drawn as the ramp's mean, a glow that does not move.
constexpr uint32_t kShaderGunGlow[] = {0xA13D6235, 0x62F671E0};
// A shader with parameters of its own (TCHn, CCHn) reads the vertex colour as
// it likes: masks for its extra maps, a colour seen through ice. These are the
// ones read that multiply the albedo by it, as the standard shader does
// (992941B7 is ColorUnlit: a door shield's blue is all vertex colour).
constexpr uint32_t kShaderTints[] = {0x9EFE0D2E, 0xCA10C453, 0x17E458CD, 0xE9DF2188,
                                     0x41A12C9E, 0xD6AA2A3A, 0x992941B7};

// Which of a Remastered material's parameters feed the four maps, and its two
// strengths. Later parameters replace earlier ones, as in the reference.
RemMaterial ReadMaterial(const ModelMaterial& mat, const ConvertOptions& opt) {
  RemMaterial out;
  out.name = mat.name;
  out.cutout = (mat.unk1 & kCutoutFlag) != 0;
  out.blended = (mat.unk1 & kTransparentFlag) != 0 && !out.cutout;
  uint8_t sid[4];
  std::memcpy(sid, &mat.shaderId, 4);
  const uint32_t shader = uint32_t(sid[0]) << 24 | uint32_t(sid[1]) << 16 | uint32_t(sid[2]) << 8 | sid[3];
  bool custom = false;
  for (const ModelMaterialData& d : mat.data) {
    const uint32_t family = d.usage & 0xFFFFFF00u;
    custom = custom || family == (FourCC('T', 'C', 'H', '0') & 0xFFFFFF00u) ||
             family == (FourCC('C', 'C', 'H', '0') & 0xFFFFFF00u);
  }
  out.tinted = (mat.unk1 & kVertexColorFlag) != 0 &&
               (!custom || std::find(std::begin(kShaderTints), std::end(kShaderTints), shader) != std::end(kShaderTints));
  // What the alpha a shader writes is made of is only in its code. Every one
  // read writes the base map's alpha squared, times the vertex alpha, bar these:
  // with the mask flag the base alpha scales the glow instead and the vertex
  // alpha alone is the opacity, a material in no lit pass (screens, holograms)
  // is its own colour and glow, a framebuffer one (ice, glass) writes alpha 1
  // and refracts what is behind it (the port does that for glass, kind 8), and
  // the snow shader cuts its edge by height.
  out.mask = (mat.unk1 & kIncanMaskFlag) != 0;
  out.unlit = mat.types.empty();
  bool lit = false, framebuffer = false;
  for (uint32_t type : mat.types) {
    lit = lit || type == FourCC('R', 'L', 'T', 'G');
    framebuffer = framebuffer || type == FourCC('R', 'F', 'B', 'P');
  }
  if (out.mask) {
    out.blended = out.blended && out.tinted;
  }
  if (framebuffer && lit) {
    out.blended = false;
  }
  if (shader == kShaderHeightBlend) {
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('C', 'C', 'H', '0')) {
        out.height = std::clamp(ShortestDouble(d.color[0]), 0.01, 4.0);
      }
    }
  }
  auto set = [&](int k, const ModelTextureRef& t, MapRef* into = nullptr) {
    if (!t.hasUsage) {
      return;
    }
    MapRef& m = into ? *into : out.maps[k];
    m.has = true;
    // A model stores the id with its first three groups little endian; a pak,
    // and the printed form, have them the other way round.
    m.id = t.id;
    std::swap(m.id[0], m.id[3]);
    std::swap(m.id[1], m.id[2]);
    std::swap(m.id[4], m.id[5]);
    std::swap(m.id[6], m.id[7]);
    m.coord = t.texCoord;
    m.src = opt.texturePrefix + IdToString(m.id) + opt.textureSuffix;
  };
  bool bclr = false;
  const ModelMaterialData* icnc = nullptr;
  const ModelMaterialData* bklt = nullptr;
  for (const ModelMaterialData& d : mat.data) {
    const bool texture = d.kind == ModelMaterialData::Kind::Texture;
    const bool layered = d.kind == ModelMaterialData::Kind::LayeredTexture;
    switch (d.usage) {
    case FourCC('D', 'I', 'F', 'T'):
      // A material with a base colour map keeps a legacy diffuse slot beside
      // it, holding a 1x1 white default: the base colour map is the albedo.
      if (texture && !bclr) {
        set(kBase, d.texture);
      }
      break;
    case FourCC('B', 'C', 'L', 'R'):
      if (texture) {
        set(kBase, d.texture);
        bclr = d.texture.hasUsage;
      }
      break;
    case FourCC('B', 'C', 'R', 'L'):
      if (layered) {
        set(kBase, d.layeredTextures[0]);
        set(kBase, d.layeredTextures[1], &out.layer[kBase]);
      }
      break;
    case FourCC('B', 'L', 'S', 'M'):
      if (d.kind == ModelMaterialData::Kind::Scalar) {
        out.layerSmooth = ShortestDouble(d.scalar);
      }
      break;
    case FourCC('B', 'S', 'A', 'O'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        for (int i = 0; i < 4; ++i) {
          out.layerHeight[i] = ShortestDouble(d.color[i]);
        }
      }
      break;
    case FourCC('M', 'E', 'T', 'L'):
      if (texture) {
        set(kMr, d.texture);
      }
      break;
    case FourCC('M', 'T', 'L', 'L'):
      if (layered) {
        set(kMr, d.layeredTextures[0]);
        set(kMr, d.layeredTextures[1], &out.layer[kMr]);
      }
      break;
    case FourCC('N', 'M', 'A', 'P'):
      if (texture) {
        set(kNormal, d.texture);
      }
      break;
    case FourCC('N', 'R', 'M', 'L'):
      if (layered) {
        set(kNormal, d.layeredTextures[0]);
        set(kNormal, d.layeredTextures[1], &out.layer[kNormal]);
      }
      break;
    case FourCC('I', 'C', 'A', 'N'):
      if (texture) {
        set(kEmissive, d.texture);
      }
      break;
    case FourCC('I', 'C', 'N', 'C'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        icnc = &d;
      }
      break;
    case FourCC('B', 'K', 'L', 'T'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        bklt = &d;
      }
      break;
    default:
      break;
    }
  }
  // The strength is ICNC, the incandescence colour (grey in every material
  // seen, 0.15 to 400), times the shader's INCI parameter where it has one. A
  // render type row names the colour that holds a parameter and the component:
  // INCI / CCH2 / flag 0 is CCH2.x.
  double s = 1.0;
  if (icnc) {
    s = std::max({ShortestDouble(icnc->color[0]), ShortestDouble(icnc->color[1]), ShortestDouble(icnc->color[2])});
  }
  const ModelRenderType* inci = nullptr;
  for (const ModelRenderType& r : mat.renderTypes) {
    if (r.dataId == FourCC('I', 'N', 'C', 'I')) {
      inci = &r;
    }
  }
  if (inci && inci->flag2 < 4) {
    const ModelMaterialData* colour = nullptr;
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == inci->dataType) {
        colour = &d;
      }
    }
    if (colour) {
      s *= ShortestDouble(colour->color[inci->flag2]);
    }
  }
  out.emissive = s;
  // The shaders of their own. ICNC is 1 in every lava material and the strength
  // is CCH0.x instead.
  const ModelMaterialData* tch[3] = {nullptr, nullptr, nullptr};
  const ModelMaterialData* cch[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  for (const ModelMaterialData& d : mat.data) {
    for (uint32_t i = 0; i < 3; ++i) {
      if (d.kind == ModelMaterialData::Kind::Texture && d.usage == FourCC('T', 'C', 'H', char('0' + i))) {
        tch[i] = &d;
      }
    }
    for (uint32_t i = 0; i < 5; ++i) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('C', 'C', 'H', char('0' + i))) {
        cch[i] = &d;
      }
    }
  }
  if (shader == kShaderUpLayer && tch[0] && cch[0]) {
    out.kind = 1;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      if (tch[i]) {
        set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      }
    }
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderDetail && tch[0]) {
    out.kind = 2;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
  } else if (shader == kShaderLava && cch[0]) {
    out.kind = 3;
    out.vcolor = true;
    out.emissive = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderParallax && tch[0] && cch[0]) {
    out.kind = 4;
    out.vcolor = true;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    for (int i = 0; i < 4; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
    out.kindStrength = cch[1] ? ShortestDouble(cch[1]->color[0]) : 0.0;
  } else if (std::find(std::begin(kShaderLavaPool), std::end(kShaderLavaPool), shader) != std::end(kShaderLavaPool) &&
             out.maps[kBase].has && tch[0] && tch[1] && tch[2] && cch[0] && cch[1]) {
    out.kind = 6;
    out.vcolor = true;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      // The shader reads every map on the first texcoord, whatever the material says.
      out.layer[slot[i]].coord = 0;
    }
    out.maps[kBase].coord = 0;
    const double period = ShortestDouble(cch[0]->color[1]);
    out.kindParam[0] = period > 1e-3 ? 1.0 / period : 0.0;  // the game multiplies it by the time
    out.kindParam[1] = ShortestDouble(cch[0]->color[0]);
    out.kindParam[2] = ShortestDouble(cch[1]->color[0]);
    out.kindParam[3] = ShortestDouble(cch[1]->color[2]);
    out.layerHeight[0] = ShortestDouble(cch[1]->color[1]);  // the noise map's scale
    out.layerHeight[1] = 1.0;
    out.kindStrength = ShortestDouble(cch[0]->color[2]);
  } else if (shader == kShaderWaterfall && tch[0] && tch[1] && cch[0] && cch[1] && cch[2]) {
    out.kind = 7;
    out.vcolor = true;
    set(kBase, tch[1]->texture);
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.maps[kBase].coord = out.layer[kBase].coord = 0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
    const double rate = ShortestDouble(cch[0]->color[0]);
    for (int i = 0; i < 4; ++i) {
      out.layerHeight[i] = rate * ShortestDouble(cch[1]->color[i]);
    }
    out.layerSmooth = rate * ShortestDouble(cch[2]->color[0]);
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.kindParam[1] = rate * ShortestDouble(cch[2]->color[1]);
    out.kindParam[2] = ShortestDouble(cch[0]->color[1]);
    out.kindParam[3] = ShortestDouble(cch[0]->color[2]);
    out.kindStrength = ShortestDouble(cch[0]->color[3]);
    for (int i = 0; i < 3; ++i) {
      out.tint[i] = cch[3] ? ShortestDouble(cch[3]->color[i]) : 1.0;
    }
  } else if (shader == kShaderGlass && out.maps[kBase].has && tch[0] && tch[1] && cch[0] && cch[1] && cch[2] &&
             cch[3] && cch[4]) {
    out.kind = 8;
    out.vcolor = true;
    // The mask and the noise are bound as the second layer's base and MR. CCH4.w
    // (8 in every glass material) is no uv scale the maps are read with.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    const double glow = ShortestDouble(cch[4]->color[1]);
    for (int i = 0; i < 3; ++i) {
      out.layerHeight[i] = ShortestDouble(cch[2]->color[i]) * glow;
      out.tint[i] = ShortestDouble(cch[1]->color[i]) * ShortestDouble(cch[3]->color[i]) *
                    ShortestDouble(cch[4]->color[0]) * ShortestDouble(cch[4]->color[2]);
    }
    out.layerHeight[3] = ShortestDouble(cch[1]->color[3]);
    for (int i = 0; i < 3; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
  }
  if (std::find(std::begin(kShaderGunGlow), std::end(kShaderGunGlow), shader) != std::end(kShaderGunGlow) &&
      tch[1] && cch[0] && !out.maps[kEmissive].has) {
    set(kEmissive, tch[1]->texture);
    out.maps[kEmissive].mean = true;
    out.emissive = ShortestDouble(cch[0]->color[2]);
  }
  // BKLT is a parameter vector like CCHn, not a colour: x is the strength (0 to
  // 2), what y means is not known and it is not used.
  out.backlight = bklt ? ShortestDouble(bklt->color[0]) : 0.0;
  // Both layers' base maps and an edge width make a blend; the shader's own
  // floor on the width is not known, so a zero one is the thinnest edge.
  out.layered = out.maps[kBase].has && out.layer[kBase].has && !out.cutout && !out.blended;
  if (out.kind != 7) {
    out.layerSmooth = std::clamp(out.layerSmooth, 1e-3, 16.0);
  }
  if (out.kind == 6) {
    // Its own colour, and the extra maps are no second layer but must be bound like one.
    out.layered = out.unlit = true;
    out.cutout = out.blended = out.tinted = out.mask = false;
    out.height = 0.0;
  }
  if (out.kind == 7) {
    // The ramp's colour and alpha are all of it; the sheets' map is bound as a second layer.
    out.layered = out.unlit = out.blended = true;
    out.cutout = out.tinted = out.mask = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = 0.0;
  }
  if (out.kind == 8) {
    // Lit, but only by its reflection: what shows through is the frame behind it.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = 0.0;
    out.maps[kMr].has = out.maps[kEmissive].has = false;
  }
  // All but lava draw with the second layer's maps.
  if (out.kind != 3 && !out.layered) {
    out.kind = 0;
    out.vcolor = false;
  }
  return out;
}

// The port's material record (CCubeModel::PortSetPBRMaterial), appended to a
// PBR material: emissive multiplier rgb, backlight weight rgb, 'PBRM'; or,
// for a material with a height blend or no lighting, those, the blend's
// threshold, the mode (1 unlit, 2 glow masked by the base alpha, 4 tinted by
// the vertex colour, summed), and 'PBR2'; or, for a layered one, those, the blend's edge width, the scale and offset of
// each layer's height, and 'PBR3'; or, for a shader of its own, those, the kind, its strength
// (compressed like the emissive one) and its four parameters, and 'PBR4'.
// Remastered's emissive strengths are HDR values meant for its bloom, and the
// port's output is 8-bit with none, so the strength is compressed (square
// root, capped) around 1, where the map is drawn as converted.
void PbrRecord(Blob& b, const RemMaterial& m) {
  const double e = std::sqrt(std::min(std::max(m.emissive, 0.0), kPbrEmissiveMax));
  const double k = std::min(std::max(m.backlight, 0.0), 2.0);
  for (int i = 0; i < 3; ++i) {
    // A liquid has no glow of its own, and its colour goes where the glow's would.
    PF(b, m.kind == 5 || m.kind == 7 || m.kind == 8 ? m.tint[i] : e);
  }
  for (int i = 0; i < 3; ++i) {
    PF(b, k);
  }
  if (m.height > 0.0 || m.unlit || m.mask || m.layered || m.tinted || m.kind) {
    PF(b, m.height);
    PF(b, (m.unlit ? 1.0 : 0.0) + (m.mask ? 2.0 : 0.0) + (m.tinted ? 4.0 : 0.0));
    if (m.layered || m.kind) {
      // Only a blend of two layers has an edge.
      PF(b, m.kind == 7 || (m.layered && m.kind <= 1) ? m.layerSmooth : 0.0);
      for (double h : m.layerHeight) {
        PF(b, h);
      }
      if (m.kind) {
        PF(b, double(m.kind));
        PF(b, m.kind >= 5 ? m.kindStrength : std::sqrt(std::min(std::max(m.kindStrength, 0.0), kPbrEmissiveMax)));
        for (double v : m.kindParam) {
          PF(b, v);
        }
        b.insert(b.end(), {'P', 'B', 'R', '4'});
        return;
      }
      b.insert(b.end(), {'P', 'B', 'R', '3'});
      return;
    }
    b.insert(b.end(), {'P', 'B', 'R', '2'});
    return;
  }
  b.insert(b.end(), {'P', 'B', 'R', 'M'});
}

// A retail material rebuilt for the port's PBR path: maps 0-3 are base, MR,
// normal and emissive, each on the texcoord its Remastered map used, lit
// channel 0 kept from retail. The TEV is the fallback the black, shadow,
// thermal and blended paths still draw with (base x lighting + emissive), and
// it samples every map so all are bound. A layered material has three more,
// maps 4-6: the second layer's base, MR and normal.
Blob PbrMaterial(const RetailMaterial& pm, uint32_t vtx, const uint32_t* texIdx, uint32_t group,
                 const uint32_t* coords, const RemMaterial& rem) {
  const int nmaps = rem.layered ? kLayeredMaps : kMaps;
  Blob b;
  const uint32_t flags =
      (pm.flags & 0xFFFF & ~uint32_t(0x8 | 0x40 | 0x100 | 0x400 | 0x800 | 0x2000)) | 0xF0000 | kPbrFlag;
  P32(b, flags);
  P32(b, uint32_t(nmaps));
  for (int i = 0; i < nmaps; ++i) {
    P32(b, texIdx[i]);
  }
  // The vertex descriptor is retail's: it fixes how many texcoord attributes,
  // and so how many indices per vertex, the display list carries.
  P32(b, vtx);
  // The material's cache id: CCubeMaterial::SetCurrent skips the vertex layout
  // and TEV when it matches the previous draw's, so it must differ from retail's.
  P32(b, group);
  P16(b, pm.blendDst);
  P16(b, pm.blendSrc);
  // An unlit surface coloured by its vertices (a door shield) keeps that in the
  // fallback too, which is what draws it whenever the model is not opaque: channel
  // 0 unlit with the vertex colour as its material colour (bit 2), and the alpha
  // taken from it as well.
  const bool vertexGlow = rem.unlit && rem.tinted;
  P32(b, vertexGlow ? 1u : uint32_t(pm.chans.size()));
  if (vertexGlow) {
    P32(b, 0x4);
  } else {
    for (uint32_t c : pm.chans) {
      P32(b, c);
    }
  }
  static const uint32_t tev[kLayeredMaps][3] = {
      {0x7A14F, 0x21CE7, 4},  // ZERO, RASC, TEXC, ZERO: base x channel 0; alpha = base
      {0x3D0F, 0x1CE7, 255},  // ZERO, TEXC, ZERO, CPREV: MR, sampled only
      {0x3D0F, 0x1CE7, 255},  // normal, sampled only
      {0x310F, 0x1CE7, 255},  // ZERO, TEXC, ONE, CPREV: + emissive
      {0x3D0F, 0x1CE7, 255},  // the second layer, sampled only
      {0x3D0F, 0x1CE7, 255},
      {0x3D0F, 0x1CE7, 255},
  };
  // Glass samples the frame behind it, which the game copies into map 7 (the
  // spare buffer's) before it draws one: a stage of its own binds it.
  const int nstages = rem.kind == 8 ? nmaps + 1 : nmaps;
  P32(b, uint32_t(nstages));
  for (int i = 0; i < nstages; ++i) {
    const uint32_t* t = tev[std::min(i, kLayeredMaps - 1)];
    P32(b, t[0]);
    P32(b, i == 0 && vertexGlow ? 0x39487u : t[1]);  // ZERO, TEXA, RASA, ZERO: base x vertex alpha
    P32(b, 0x100);
    P32(b, 0x100);
    P8(b, 0);
    P8(b, 0);
    P8(b, 0);
    P8(b, uint8_t(t[2]));
  }
  for (int i = 0; i < nstages; ++i) {
    P8(b, 0);
    P8(b, 0);
    P8(b, uint8_t(i < nmaps ? i : 7));
    P8(b, uint8_t(i < nmaps ? coords[i] : 0));
  }
  // One texgen per texcoord the maps use. Each reads its own texcoord through the
  // identity (GX_TG_MTX3x4, TEXi, GX_IDENTITY, GX_PTIDENTITY): texgen word 0 would be
  // position through GX_TEXMTX0, a projection by whatever matrix the last animated
  // material loaded, which slid Magmoor's room rock along with the lava. Where
  // retail's texgen for the coord reads that same texcoord it is kept, with retail's
  // UV animations, so an animated coordinate still drives the maps; one over position
  // or normal (an env map) says nothing about a Remastered texcoord.
  const uint32_t n = 1 + *std::max_element(coords, coords + nmaps);
  std::vector<uint32_t> gens(n);
  for (uint32_t i = 0; i < n; ++i) {
    gens[i] = 0x1EBC00 | ((4 + i) << 4);
  }
  bool retailGens = false;
  for (int i = 0; i < nmaps; ++i) {
    const uint32_t c = coords[i];
    if (c < pm.texgen.size() && ((pm.texgen[c] >> 4) & 31) == 4 + c) {
      gens[c] = pm.texgen[c];
      retailGens = true;
    }
  }
  P32(b, n);
  for (uint32_t g : gens) {
    P32(b, g);
  }
  if (retailGens && !pm.uvAnim.empty()) {
    b.insert(b.end(), pm.uvAnim.begin(), pm.uvAnim.end());
  } else {
    P32(b, 4);  // no UV animations
    P32(b, 0);
  }
  PbrRecord(b, rem);
  return b;
}

// One weight per vertex and bone, from the Remastered joints or, for a static
// Remastered model, from the nearest retail vertex.
//
// Each vertex votes its weight on a joint for the bones of the nearest retail
// vertex. A joint takes the bone with the most votes, plus any other with a
// kJointSplit share: several joints sharing a bone is the normal case, and a
// joint covering two retail bones is split between them per vertex, so the
// seam falls where retail has it.
std::vector<WeightKey> SkinWeights(const std::vector<double>& P, size_t n, const std::vector<uint16_t>* J,
                                   const std::vector<float>* W, const Retail& retail, Span cskr,
                                   const std::function<void(const std::string&)>& log) {
  const std::vector<SkinGroup> groups = ReadCskr(cskr);
  std::vector<uint32_t> gid;
  for (size_t i = 0; i < groups.size(); ++i) {
    gid.insert(gid.end(), groups[i].count, uint32_t(i));
  }
  const size_t nr = std::min(gid.size(), retail.Count());
  if (nr == 0) {
    throw Fail{"the retail skin covers no vertices"};
  }
  const double* rp = retail.P.data();
  std::vector<uint32_t> nn;
  Nearest(P.data(), n, rp, nr, nn);
  std::vector<std::vector<std::pair<uint32_t, double>>> weights(n);
  if (J && W) {
    std::vector<uint32_t> bones;
    for (const SkinGroup& g : groups) {
      for (const auto& w : g.weights) {
        bones.push_back(w.first);
      }
    }
    std::sort(bones.begin(), bones.end());
    bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
    const size_t nb = bones.size();
    // The retail vertices' weights, a row per vertex and a column per bone.
    std::vector<double> GW(groups.size() * nb, 0.0);
    for (size_t i = 0; i < groups.size(); ++i) {
      for (const auto& w : groups[i].weights) {
        const size_t col = size_t(std::lower_bound(bones.begin(), bones.end(), w.first) - bones.begin());
        GW[i * nb + col] += double(w.second);
      }
    }
    auto RW = [&](size_t v) { return &GW[size_t(gid[v]) * nb]; };
    uint32_t maxJoint = 0;
    for (size_t i = 0; i < n * 4; ++i) {
      maxJoint = std::max<uint32_t>(maxJoint, (*J)[i]);
    }
    const size_t nj = size_t(maxJoint) + 1;
    std::vector<double> V(nj * nb, 0.0);
    for (int k = 0; k < 4; ++k) {
      for (size_t v = 0; v < n; ++v) {
        const double w = double((*W)[v * 4 + k]);
        const double* row = RW(nn[v]);
        double* dst = &V[size_t((*J)[v * 4 + k]) * nb];
        for (size_t c = 0; c < nb; ++c) {
          dst[c] += w * row[c];
        }
      }
    }
    std::vector<std::vector<uint32_t>> assign(nj);
    size_t mapped = 0, split = 0;
    std::vector<double> share(nb);
    for (size_t j = 0; j < nj; ++j) {
      const double total = RowSum(&V[j * nb], nb);
      if (!(total > 0.0)) {
        continue;
      }
      for (size_t c = 0; c < nb; ++c) {
        share[c] = V[j * nb + c] / total;
      }
      std::vector<uint32_t> order(nb);
      for (size_t c = 0; c < nb; ++c) {
        order[c] = uint32_t(c);
      }
      std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return share[a] > share[b]; });
      for (uint32_t c : order) {
        if (share[c] >= kJointSplit) {
          assign[j].push_back(c);
        }
      }
      if (assign[j].empty()) {
        assign[j].push_back(order[0]);
      }
      ++mapped;
    }
    // A rig cut finer than retail's, both of rigid pieces (the iris doors: each
    // of six leaves is a stack of layers and plates there, one piece here): a
    // piece split between bones tears, and the vote is no guide, since retail's
    // few vertices on a wide leaf are nearer the next leaf's edge than its own.
    // Such a piece rides the bone whose piece is nearest its own, whole.
    size_t rehomed = 0;
    const bool rigidRetail = std::all_of(groups.begin(), groups.end(),
                                         [](const SkinGroup& g) { return g.weights.size() == 1; });
    if (rigidRetail && mapped >= 2 * nb) {
      std::vector<double> bc(nb * 3, 0.0), bn(nb, 0.0), jc(nj * 3, 0.0), jn(nj, 0.0);
      std::vector<bool> loose(nj, false);
      for (size_t v = 0; v < nr; ++v) {
        const double* row = RW(v);
        const size_t c = size_t(std::max_element(row, row + nb) - row);
        for (int a = 0; a < 3; ++a) {
          bc[c * 3 + a] += rp[v * 3 + a];
        }
        bn[c] += 1.0;
      }
      for (size_t v = 0; v < n; ++v) {
        for (int k = 0; k < 4; ++k) {
          const double w = double((*W)[v * 4 + k]);
          const size_t j = (*J)[v * 4 + k];
          if (w >= 0.999) {
            for (int a = 0; a < 3; ++a) {
              jc[j * 3 + a] += P[v * 3 + a];
            }
            jn[j] += 1.0;
          } else if (w >= 1e-3) {
            loose[j] = true;
          }
        }
      }
      for (size_t j = 0; j < nj; ++j) {
        if (assign[j].empty() || loose[j] || jn[j] == 0.0) {
          continue;
        }
        double best = -1.0;
        for (size_t c = 0; c < nb; ++c) {
          if (bn[c] == 0.0) {
            continue;
          }
          double d = 0.0;
          for (int a = 0; a < 3; ++a) {
            const double e = jc[j * 3 + a] / jn[j] - bc[c * 3 + a] / bn[c];
            d += e * e;
          }
          if (best < 0.0 || d < best) {
            assign[j] = {uint32_t(c)};
            best = d;
          }
        }
        ++rehomed;
      }
    }
    for (size_t j = 0; j < nj; ++j) {
      split += assign[j].size() > 1;
    }
    log("  joints mapped " + std::to_string(mapped) + ", " + std::to_string(split) + " split" +
        (rehomed ? ", " + std::to_string(rehomed) + " rigid by nearest piece" : std::string()));
    std::vector<double> VW(n * nb, 0.0);
    std::vector<uint32_t> sel, on, near;
    std::vector<double> selP, onP;
    for (size_t j = 0; j < nj; ++j) {
      const std::vector<uint32_t>& cs = assign[j];
      if (cs.empty()) {
        continue;
      }
      for (int k = 0; k < 4; ++k) {
        sel.clear();
        for (size_t v = 0; v < n; ++v) {
          if ((*J)[v * 4 + k] == j && double((*W)[v * 4 + k]) >= 1e-3) {
            sel.push_back(uint32_t(v));
          }
        }
        if (sel.empty()) {
          continue;
        }
        if (cs.size() == 1) {
          for (uint32_t v : sel) {
            VW[size_t(v) * nb + cs[0]] += double((*W)[size_t(v) * 4 + k]);
          }
          continue;
        }
        // A split joint hands each vertex the weights of the nearest retail
        // vertex that is on one of the joint's bones, restricted to those
        // bones, so a vertex can never leave the joint's own bones.
        on.clear();
        onP.clear();
        std::vector<double> part(cs.size());
        for (size_t r = 0; r < nr; ++r) {
          for (size_t c = 0; c < cs.size(); ++c) {
            part[c] = RW(r)[cs[c]];
          }
          if (RowSum(part.data(), part.size()) > 0.0) {
            on.push_back(uint32_t(r));
            onP.insert(onP.end(), rp + 3 * r, rp + 3 * r + 3);
          }
        }
        selP.clear();
        for (uint32_t v : sel) {
          selP.insert(selP.end(), P.begin() + 3 * size_t(v), P.begin() + 3 * size_t(v) + 3);
        }
        Nearest(selP.data(), sel.size(), onP.data(), on.size(), near);
        for (size_t i = 0; i < sel.size(); ++i) {
          const double* row = RW(on[near[i]]);
          for (size_t c = 0; c < cs.size(); ++c) {
            part[c] = row[cs[c]];
          }
          const double total = RowSum(part.data(), part.size());
          const double w = double((*W)[size_t(sel[i]) * 4 + k]);
          for (size_t c = 0; c < cs.size(); ++c) {
            VW[size_t(sel[i]) * nb + cs[c]] += w * part[c] / total;
          }
        }
      }
    }
    size_t lost = 0;
    for (size_t v = 0; v < n; ++v) {
      double* row = &VW[v * nb];
      if (RowSum(row, nb) <= 0.0) {
        // No weighted joint of this vertex has a bone: take the nearest retail
        // vertex's weights rather than pin it to an arbitrary bone.
        std::copy(RW(nn[v]), RW(nn[v]) + nb, row);
        ++lost;
      }
      for (size_t c = 0; c < nb; ++c) {
        if (row[c] > 0.0) {
          weights[v].emplace_back(bones[c], row[c]);
        }
      }
    }
    if (lost) {
      log("  " + std::to_string(lost) + " vertices with no mapped joint: weights from the nearest retail vertex");
    }
  } else {
    log("  static Remastered model on a skinned retail one: weights from the nearest retail vertex");
    for (size_t v = 0; v < n; ++v) {
      for (const auto& w : groups[gid[nn[v]]].weights) {
        bool found = false;
        for (auto& have : weights[v]) {
          if (have.first == w.first) {
            have.second = double(w.second);
            found = true;
          }
        }
        if (!found) {
          weights[v].emplace_back(w.first, double(w.second));
        }
      }
    }
  }
  // Quantise to 1/64 and renormalise, so vertices that differ by noise share a
  // weight group.
  std::vector<WeightKey> out(n);
  for (size_t v = 0; v < n; ++v) {
    auto& ws = weights[v];
    if (ws.size() > kMaxSkinWeights) {
      std::stable_sort(ws.begin(), ws.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      ws.resize(kMaxSkinWeights);
    }
    double total = 0.0;
    for (const auto& w : ws) {
      total += w.second;
    }
    WeightKey q;
    for (const auto& w : ws) {
      const double r = std::nearbyint(w.second / total * 64.0);
      if (r != 0.0) {
        q.emplace_back(w.first, r / 64.0);
      }
    }
    std::sort(q.begin(), q.end());
    double s = 0.0;
    for (const auto& w : q) {
      s += w.second;
    }
    for (auto& w : q) {
      w.second /= s;
    }
    out[v] = std::move(q);
  }
  return out;
}

Blob CskrBytes(const std::vector<std::pair<const WeightKey*, uint32_t>>& runs, size_t total) {
  Blob b;
  P32(b, uint32_t(runs.size()));
  for (const auto& run : runs) {
    P32(b, uint32_t(run.first->size()));
    for (const auto& w : *run.first) {
      P32(b, w.first);
      PF(b, w.second);
    }
    P32(b, run.second);
  }
  P32(b, 0xFFFFFFFFu);
  P32(b, uint32_t(total));
  P32(b, 0xFFFFFFFFu);
  P32(b, uint32_t(total));
  return b;
}

void SurfaceHeader(Blob& s, const double* centre, uint32_t mat, size_t dlSize, uint32_t extra) {
  for (int c = 0; c < 3; ++c) {
    PF(s, centre[c]);
  }
  P32(s, mat);
  P32(s, uint32_t(dlSize) | 0x80000000u);
  P32(s, 0);
  P32(s, 0);
  P32(s, extra);
  P32(s, 0);
  P32(s, 0);
  PF(s, 1.0);
}

} // namespace

void Converter::State::Convert(const Model& model, const ConvertOptions& opt) {
  Retail retail;
  if (opt.standalone) {
    // What retail's own opaque lit world materials carry (measured over every
    // retail CMDL: flags 0x1083 and one channel word 0x3001 are the common case).
    retail.nmat = 1;
    retail.flags = 0x4;  // the packed texcoord section is there, empty
    RetailMaterial lit;
    lit.flags = 0x1083;
    lit.vtx = 0xF;  // position and normal; the texcoord slots follow the model
    lit.blendSrc = 1;
    lit.blendDst = 0;
    lit.chans = {0x3001};
    retail.mats.push_back(lit);
    // And the same with retail's alpha test, for a Remastered cutout.
    lit.flags |= 0x20;
    retail.mats.push_back(lit);
    // And as retail's blended surfaces are: drawn in the sorted pass over what
    // is behind them, tested against the depth buffer without writing to it.
    lit.flags = 0x1013;
    lit.blendSrc = 4;  // GX_BL_SRCALPHA, GX_BL_INVSRCALPHA
    lit.blendDst = 5;
    retail.mats.push_back(lit);
    // And glass, whose colour is premultiplied: GX_BL_ONE, GX_BL_INVSRCALPHA.
    lit.blendSrc = 1;
    retail.mats.push_back(lit);
  } else {
    if (!io.retail(FourCC('C', 'M', 'D', 'L'), opt.retail, retail.data)) {
      throw Fail{"retail model " + Hex8(opt.retail) + " is not on the disc"};
    }
    retail.Parse();
  }

  const double (*M)[3] = opt.orient;
  const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);

  // Primitives in GameCube space; a vertex buffer is shared by many meshes and
  // is transformed once.
  std::vector<RemMaterial> mats;
  for (const ModelMaterial& m : model.materials) {
    mats.push_back(ReadMaterial(m, opt));
  }
  // A liquid's model says nothing of how it looks (its maps are placeholders): the room does.
  // The material is a blended one of the liquid kind, whose second layer is only there
  // because the kinds are drawn by the layered shader.
  bool liquid = false;
  for (RemMaterial& m : mats) {
    if (opt.water && opt.standalone) {
      m.kind = 5;
      m.layered = m.blended = true;
      m.cutout = m.tinted = m.vcolor = m.unlit = m.mask = m.hidden = false;
      m.height = 0.0;
      m.backlight = 0.0;
      m.maps[kMr].has = m.maps[kEmissive].has = false;
      m.maps[kNormal].has = opt.waterHasNormal;
      m.maps[kNormal].id = opt.waterNormal;
      m.maps[kNormal].src = opt.texturePrefix + IdToString(opt.waterNormal) + opt.textureSuffix;
      for (MapRef& ref : m.maps) {
        ref.coord = 0;
      }
      m.layer[kBase] = m.maps[kBase];
      m.layer[kMr].has = m.layer[kNormal].has = false;
      m.layerSmooth = 0.0;
      std::copy(opt.waterFlow, opt.waterFlow + 4, m.layerHeight);
      std::copy(opt.waterTint, opt.waterTint + 3, m.tint);
      m.kindStrength = opt.waterNormalStrength;
      m.kindParam[0] = 1.0;  // the game multiplies it by the time
      m.kindParam[1] = 0.0;
      m.kindParam[2] = opt.waterTint[3];
      m.kindParam[3] = opt.waterFresnel;
    }
    liquid = liquid || m.kind >= 5;
  }
  std::vector<Buffer> buffers(model.vertexBuffers.size());
  std::vector<Prim> prims;
  std::vector<uint32_t> bufOrder;  // buffers in the order the primitives reach them
  // Each level of detail is a set of meshes of its own, so drawing every mesh
  // stacks the coarse copies on the fine one. Keep the finest level's.
  std::vector<bool> finest(model.meshes.size(), false);
  bool anyFinest = false;
  for (size_t r = 0; r < 5 && r < model.lods.size(); ++r) {
    const ModelLod& range = model.lods[r];
    for (uint64_t i = range.indexOffset; i < uint64_t(range.indexOffset) + range.indexCount; ++i) {
      if (i < model.lodMeshes.size() && model.lodMeshes[i] < finest.size()) {
        finest[model.lodMeshes[i]] = true;
        anyFinest = true;
      }
    }
  }
  for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
    const ModelMesh& mesh = model.meshes[meshIndex];
    if (anyFinest && !finest[meshIndex]) {
      continue;
    }
    if (mesh.material >= mats.size() || mesh.vertexBuffer >= buffers.size()) {
      throw Fail{"a Remastered mesh names a material or vertex buffer that does not exist"};
    }
    const std::string& name = mats[mesh.material].name;
    bool skip = false;
    for (const std::string& s : opt.skip) {
      skip = skip || name.find(s) != std::string::npos;
    }
    if (skip || mats[mesh.material].hidden || (opt.standalone && !mats[mesh.material].maps[kBase].has)) {
      continue;
    }
    Buffer& b = buffers[mesh.vertexBuffer];
    if (!b.loaded) {
      const ModelVertexBuffer& vb = model.vertexBuffers[mesh.vertexBuffer];
      b.loaded = true;
      b.src = &vb;
      b.n = vb.vertexCount;
      // A liquid's surface has no normals: it is flat, and faces up.
      const bool flat = vb.normals.empty() && mats[mesh.material].kind >= 5;
      if (vb.positions.size() != b.n * 3 || (!flat && vb.normals.size() != b.n * 3)) {
        throw Fail{"a Remastered vertex buffer has no positions or normals"};
      }
      if (vb.uvs.empty() || vb.uvs[0].size() != b.n * 2) {
        throw Fail{"a Remastered vertex buffer has no texture coordinates"};
      }
      b.P.resize(b.n * 3);
      b.N.resize(b.n * 3);
      for (size_t v = 0; v < b.n; ++v) {
        double nrm[3];
        for (int r = 0; r < 3; ++r) {
          double p = 0.0, q = 0.0;
          for (int c = 0; c < 3; ++c) {
            p += double(vb.positions[v * 3 + c]) * M[r][c];
            q += (flat ? (c == 1 ? 1.0 : 0.0) : double(vb.normals[v * 3 + c])) * M[r][c];
          }
          b.P[v * 3 + r] = p + opt.offset[r];
          nrm[r] = q;
        }
        const double len = std::max(std::sqrt((nrm[0] * nrm[0] + nrm[1] * nrm[1]) + nrm[2] * nrm[2]), 1e-9);
        for (int r = 0; r < 3; ++r) {
          b.N[v * 3 + r] = nrm[r] / len;
        }
      }
      // Indexed by a material's texcoord, which takes two from each attribute
      // (TEXCOORD_n.xy, then its zw): the vertex shaders hand varying 2 + c to texcoord c.
      for (size_t c = 0; c < vb.uvs.size() * 2; ++c) {
        const std::vector<float>* uv = c % 2 == 0 ? &vb.uvs[c / 2] : c / 2 < vb.uvsZw.size() ? &vb.uvsZw[c / 2] : nullptr;
        b.uv.emplace_back(uv && uv->size() == b.n * 2 ? std::vector<double>(uv->begin(), uv->end()) : std::vector<double>());
      }
      if (opt.water && opt.standalone) {
        // Its maps lie on the surface by where a point is, not by the model's texcoords.
        std::vector<double> planar(b.n * 2);
        for (size_t v = 0; v < b.n; ++v) {
          planar[v * 2] = double(vb.positions[v * 3]) * opt.waterScale[0];
          planar[v * 2 + 1] = double(vb.positions[v * 3 + 2]) * opt.waterScale[1];
        }
        b.uv.assign(1, std::move(planar));
      }
      b.skinned = vb.joints.size() == b.n * 4 && vb.weights.size() == b.n * 4;
      b.C.assign(b.n * 4, 255);
      if (vb.colors.size() == b.n * 4) {
        for (size_t i = 0; i < b.n * 4; ++i) {
          b.C[i] = uint8_t(std::clamp(vb.colors[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        b.colored = true;
      }
    }
    Prim p;
    p.buffer = mesh.vertexBuffer;
    p.mat = mesh.material;
    p.I = mesh.indices;
    p.I.resize(p.I.size() / 3 * 3);
    for (uint32_t i : p.I) {
      if (i >= b.n) {
        throw Fail{"a Remastered mesh indexes past its vertex buffer"};
      }
    }
    if (det < 0) {
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        std::swap(p.I[t], p.I[t + 2]);
      }
    }
    if (!b.used) {
      b.used = true;
      bufOrder.push_back(mesh.vertexBuffer);
    }
    prims.push_back(std::move(p));
  }
  if (prims.empty()) {
    throw Fail{"no primitives left"};
  }
  Log(Hex8(opt.retail) + ": " + std::to_string(prims.size()) + " primitives");

  // Retail material per primitive: forced, or by a vote of the nearest retail
  // vertices among materials of the same kind (blended effect or opaque surface).
  std::vector<double> up;
  std::vector<int> upm;
  for (size_t v = 0; v < retail.Count(); ++v) {
    if (retail.vmat[v] >= 0) {
      up.insert(up.end(), retail.P.begin() + v * 3, retail.P.begin() + v * 3 + 3);
      upm.push_back(retail.vmat[v]);
    }
  }
  for (Prim& p : prims) {
    if (opt.standalone) {
      p.rmat = mats[p.mat].kind == 8 ? 3 : mats[p.mat].cutout ? 1 : mats[p.mat].blended ? 2 : 0;
      continue;
    }
    if (opt.material >= 0) {
      if (size_t(opt.material) >= retail.mats.size()) {
        throw Fail{"the forced retail material does not exist"};
      }
      p.rmat = opt.material;
      continue;
    }
    if (upm.empty()) {
      throw Fail{"the retail model draws nothing"};
    }
    const bool fx = IsFxName(mats[p.mat].name);
    std::vector<double> cand;
    std::vector<int> candMat;
    for (int pass = 0; pass < 2 && cand.empty(); ++pass) {
      for (size_t i = 0; i < upm.size(); ++i) {
        if (pass == 1 || IsFx(retail.mats[upm[i]]) == fx) {
          cand.insert(cand.end(), up.begin() + i * 3, up.begin() + i * 3 + 3);
          candMat.push_back(upm[i]);
        }
      }
    }
    std::vector<uint32_t> vs = p.I;
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
    if (vs.size() > 4096) {
      std::vector<uint32_t> sample(4096);
      const double step = double(vs.size() - 1) / 4095.0;
      for (size_t i = 0; i < 4096; ++i) {
        sample[i] = vs[i == 4095 ? vs.size() - 1 : size_t(double(i) * step)];
      }
      vs = std::move(sample);
    }
    const Buffer& b = buffers[p.buffer];
    std::vector<double> pts(vs.size() * 3);
    for (size_t i = 0; i < vs.size(); ++i) {
      std::copy(b.P.begin() + size_t(vs[i]) * 3, b.P.begin() + size_t(vs[i]) * 3 + 3, pts.begin() + i * 3);
    }
    std::vector<uint32_t> nn;
    Nearest(pts.data(), vs.size(), cand.data(), candMat.size(), nn);
    std::vector<std::pair<int, size_t>> votes;  // in the order first seen, which decides a tie
    for (uint32_t i : nn) {
      auto it = std::find_if(votes.begin(), votes.end(), [&](const auto& e) { return e.first == candMat[i]; });
      if (it == votes.end()) {
        votes.emplace_back(candMat[i], 1);
      } else {
        ++it->second;
      }
    }
    size_t best = 0;
    for (size_t i = 1; i < votes.size(); ++i) {
      if (votes[i].second > votes[best].second) {
        best = i;
      }
    }
    p.rmat = votes[best].first;
  }

  // Unified vertex list: every distinct buffer once.
  size_t n = 0;
  size_t maxuv = 0;
  bool skinned = true;
  for (uint32_t bi : bufOrder) {
    Buffer& b = buffers[bi];
    b.offset = n;
    n += b.n;
    maxuv = std::max(maxuv, b.uv.size() - 1);
    skinned = skinned && b.skinned;
  }
  if (opt.standalone) {
    // A texcoord slot for every set, so each map keeps the one it was made on.
    for (RetailMaterial& m : retail.mats) {
      for (size_t k = 0; k <= std::min<size_t>(maxuv, 7); ++k) {
        m.vtx |= 3u << (8 + 2 * k);
      }
    }
  }
  std::vector<double> P, N;
  P.reserve(n * 3);
  N.reserve(n * 3);
  for (uint32_t bi : bufOrder) {
    P.insert(P.end(), buffers[bi].P.begin(), buffers[bi].P.end());
    N.insert(N.end(), buffers[bi].N.begin(), buffers[bi].N.end());
  }
  auto uvSet = [&](size_t i) {
    std::vector<double> out;
    out.reserve(n * 2);
    for (uint32_t bi : bufOrder) {
      const Buffer& b = buffers[bi];
      const std::vector<double>& uv = i < b.uv.size() && !b.uv[i].empty() ? b.uv[i] : b.uv[0];
      out.insert(out.end(), uv.begin(), uv.end());
    }
    return out;
  };
  for (Prim& p : prims) {
    for (uint32_t& i : p.I) {
      i += uint32_t(buffers[p.buffer].offset);
    }
  }
  // Vertex colours, for a room's own materials that are tinted by them. The
  // alpha is an opacity only on a blended surface (on an opaque one it weighs
  // the material's layers), so a vertex no blended surface uses is opaque.
  // On a retail model, only a glow that is all vertex colour reads them: an
  // unlit Remastered surface over a retail effect (a door shield's cyan rim).
  auto ownGlow = [&](const RemMaterial& m, const RetailMaterial& pm) {
    return !opt.standalone && m.unlit && m.tinted && IsFx(pm);
  };
  std::vector<uint8_t> C;
  bool useColor = false;
  for (const Prim& p : prims) {
    const bool reads = opt.standalone ? mats[p.mat].tinted || mats[p.mat].vcolor : ownGlow(mats[p.mat], retail.mats[p.rmat]);
    useColor = useColor || (reads && buffers[p.buffer].colored);
  }
  if (useColor) {
    C.reserve(n * 4);
    for (uint32_t bi : bufOrder) {
      C.insert(C.end(), buffers[bi].C.begin(), buffers[bi].C.end());
    }
    std::vector<bool> keepAlpha(n, false);
    for (const Prim& p : prims) {
      if (((mats[p.mat].blended || mats[p.mat].layered) && mats[p.mat].tinted) || mats[p.mat].vcolor ||
          ownGlow(mats[p.mat], retail.mats[p.rmat])) {
        for (uint32_t i : p.I) {
          keepAlpha[i] = true;
        }
      }
    }
    for (size_t v = 0; v < n; ++v) {
      if (!keepAlpha[v]) {
        C[v * 4 + 3] = 255;
      }
    }
  }

  // Output materials: one per (retail material, Remastered material) pair, in
  // every material set.
  std::vector<std::pair<int, uint32_t>> keys;
  for (Prim& p : prims) {
    const std::pair<int, uint32_t> key(p.rmat, p.mat);
    auto it = std::find(keys.begin(), keys.end(), key);
    if (it == keys.end()) {
      keys.push_back(key);
      it = keys.end() - 1;
    }
    p.omat = int(it - keys.begin());
  }

  // The texcoord arrays the output carries, each n long. A display list
  // attribute names one by its place in this list.
  std::vector<std::vector<double>> uvArrays;
  std::vector<std::pair<size_t, bool>> uvKeys;
  auto uvIndex = [&](size_t uvi, const char* role) -> uint32_t {
    const bool squeezed = role && opt.squeeze && opt.squeezeRole == role;
    const std::pair<size_t, bool> key(uvi, squeezed);
    const auto it = std::find(uvKeys.begin(), uvKeys.end(), key);
    if (it != uvKeys.end()) {
      return uint32_t(it - uvKeys.begin());
    }
    std::vector<double> u = uvSet(uvi);
    if (squeezed) {
      // A deliberate artistic remap, so it runs on the raw coordinates and the
      // result is left alone.
      const double u0 = opt.squeezeFrom[0], u1 = opt.squeezeFrom[1];
      const double lo = opt.squeezeTo[0], hi = opt.squeezeTo[1];
      for (size_t v = 0; v < n; ++v) {
        u[v * 2] = lo + (hi - lo) * std::clamp((u[v * 2] - u0) / (u1 - u0), 0.0, 1.0);
      }
    } else if (!liquid) {
      // A liquid's maps repeat and move, so its coordinates stay as they are.
      // Remastered UVs are not normalised (U can span -3.9..5.0, or be entirely
      // negative), so a CLAMP sampler collapses the texture onto one edge
      // texel. A set whose span is at most two tiles is a single tile that
      // merely sits outside 0..1 and is remapped onto it (2.0 is measured:
      // offset single tiles land at up to 1.99). A wider span is a tiled
      // layout and must keep its range, or every tile is squeezed into one.
      for (int c = 0; c < 2 && n; ++c) {
        double lo = u[c], hi = u[c];
        for (size_t v = 0; v < n; ++v) {
          lo = std::min(lo, u[v * 2 + c]);
          hi = std::max(hi, u[v * 2 + c]);
        }
        const double span = hi - lo;
        if (span > 1e-6 && span <= 2.0 && (lo < -0.01 || hi > 1.01)) {
          for (size_t v = 0; v < n; ++v) {
            u[v * 2 + c] = (u[v * 2 + c] - lo) / (hi - lo);
          }
        }
      }
    }
    uvKeys.push_back(key);
    uvArrays.push_back(std::move(u));
    return uint32_t(uvArrays.size() - 1);
  };

  std::vector<std::vector<Blob>> setBlobs(retail.nmat);
  std::vector<std::vector<uint32_t>> setTex(retail.nmat);
  auto texIndex = [&](size_t si, uint32_t tid) {
    auto it = std::find(setTex[si].begin(), setTex[si].end(), tid);
    if (it == setTex[si].end()) {
      setTex[si].push_back(tid);
      return uint32_t(setTex[si].size() - 1);
    }
    return uint32_t(it - setTex[si].begin());
  };
  std::vector<std::vector<uint32_t>> dlAttrs;  // per output material: the uv array of each texcoord attribute
  std::vector<bool> dlColor;                   // and whether a colour index comes before them
  for (const auto& key : keys) {
    const int rmat = key.first;
    const RetailMaterial& pm = retail.mats[rmat];
    RemMaterial rem = mats[key.second];
    const bool glow = ownGlow(rem, pm) && useColor;
    // Remastered's missile lock-on highlight is a runtime effect: its map is
    // solid red, so baked in it turns grey shards red.
    if (Lower(rem.name).find("missilelock") != std::string::npos) {
      rem.maps[kEmissive].has = false;
    }
    // A retail material's vertices have no layer weight.
    // A shader of its own needs none: without colours the up-facing one goes by
    // the normal alone.
    if (!opt.standalone) {
      rem.kind = 0;
      rem.vcolor = false;
    }
    rem.layered = rem.layered && opt.standalone && (rem.kind != 0 || (useColor && rem.tinted));
    // Nor do the alpha and shading modes belong on one: what they say is about the
    // Remastered surface, and a retail model keeps the retail material's
    // (bar a glow of its own, whose colour and fade are all in it).
    if (!opt.standalone) {
      rem.unlit = glow;
      rem.mask = false;
      rem.height = 0.0;
    }
    const MapRef* rt = rem.maps;
    // A retail descriptor with no texcoord slot at all cannot feed a material
    // that samples texcoord 0, so one is declared: bits 8..23 are the eight
    // texcoord slots, two bits each, and 3 is the encoding retail uses.
    uint32_t vtx = pm.vtx;
    size_t ntexattr = 0;
    for (int k = 0; k < 8; ++k) {
      ntexattr += ((vtx >> (8 + 2 * k)) & 3) ? 1 : 0;
    }
    if (ntexattr == 0) {
      vtx |= 3u << 8;
      ntexattr = 1;
    }
    // Bits 4 and 5 are colour 0. Only a material that reads it declares it.
    const bool colored = useColor && (opt.standalone ? rem.tinted || rem.vcolor : glow);
    rem.tinted = colored && rem.tinted;
    if (colored) {
      vtx |= 3u << 4;
    }
    dlColor.push_back(colored);
    // PBR needs a base map and an opaque retail material: blended effects keep
    // their TEV.
    // Only a room's own material asks for a cutout: a retail material's alpha
    // test says nothing about what the Remastered map's alpha holds.
    const char* const baseAlpha =
        glow ? "blend" : !opt.standalone ? "" : rem.cutout ? "punch" : rem.mask || rem.layered || rem.height > 0.0 ? "mask" : rem.blended ? "blend" : "";
    const bool usePbr = opt.pbr && rt[kBase].has && (opt.standalone || glow || !IsFx(pm)) &&
                        Get("pbr:base", rt, baseAlpha, opt).has_value();
    if (usePbr) {
      ++pbr;
      // A layered material's base alphas are the two heights the blend compares.
      const int nmaps = rem.layered ? kLayeredMaps : kMaps;
      MapRef both[kLayeredMaps + 1];
      std::copy(rt, rt + kMaps, both);
      std::copy(rem.layer, rem.layer + 3, both + kMaps);
      uint32_t tids[kLayeredMaps];
      for (int k = 0; k < nmaps; ++k) {
        const int m = k % kMaps;
        tids[k] = *Get(std::string("pbr:") + kMapName[m], both + (k - m), m == kBase ? baseAlpha : "", opt);
      }
      // Each map keeps the texcoord set it was authored on. The descriptor
      // fixes how many texcoord attributes exist, and a coord past it cannot be
      // sampled at all, so it is folded onto the base's; and if the base's own
      // is past it too, onto 0, which the descriptor always has.
      size_t bset = std::min<size_t>(rt[kBase].coord, maxuv);
      bset = bset < ntexattr ? bset : 0;
      uint32_t coords[kLayeredMaps];
      for (int k = 0; k < nmaps; ++k) {
        const size_t c = std::min<size_t>(both[k].has ? both[k].coord : bset, maxuv);
        coords[k] = uint32_t(c < ntexattr ? c : bset);
      }
      std::vector<uint32_t> attrs(ntexattr, 0xFFFFFFFFu);
      for (int k = 0; k < nmaps; ++k) {
        attrs[coords[k]] = uvIndex(coords[k], nullptr);
      }
      const uint32_t zero = uvIndex(0, nullptr);
      for (uint32_t& a : attrs) {
        a = a == 0xFFFFFFFFu ? zero : a;
      }
      dlAttrs.push_back(attrs);
      const uint32_t group = 0x40000000u | uint32_t(dlAttrs.size() - 1);
      for (size_t si = 0; si < retail.nmat; ++si) {
        uint32_t idx[kLayeredMaps];
        for (int k = 0; k < nmaps; ++k) {
          idx[k] = texIndex(si, tids[k]);
        }
        setBlobs[si].push_back(PbrMaterial(pm, vtx, idx, group, coords, rem));
      }
      continue;
    }
    ++tev;
    // The TEV path: the retail material with each texture slot refilled from
    // the Remastered map that matches what its stage does.
    const std::vector<SlotRole> roles = SlotRoles(pm);
    std::vector<uint32_t> attrs(ntexattr, 0xFFFFFFFFu);
    std::map<uint32_t, uint32_t> slotIds;
    for (const SlotRole& r : roles) {
      Role role = r.role;
      if (r.slot >= pm.tex.size() || pm.tex[r.slot] >= retail.sets[0].tex.size()) {
        throw Fail{"a retail material samples a texture it does not list"};
      }
      const std::optional<uint32_t> tid =
          Get(RoleName(role), rt, RetailAlpha(retail.sets[0].tex[pm.tex[r.slot]]), opt);
      if (tid) {
        slotIds[r.slot] = *tid;
      } else {
        role = Role::Keep;
      }
      const int rk = role == Role::Emissive ? kEmissive : role == Role::Reflect || role == Role::EnvMap ? kMr : kBase;
      const uint32_t uvi = rt[rk].has ? rt[rk].coord : rt[kBase].has ? rt[kBase].coord : 0;
      if (r.uvSrc >= 0 && size_t(r.uvSrc) < ntexattr && attrs[r.uvSrc] == 0xFFFFFFFFu) {
        attrs[r.uvSrc] = uvIndex(std::min<size_t>(uvi, maxuv), RoleName(role));
      }
    }
    for (uint32_t& a : attrs) {
      if (a == 0xFFFFFFFFu) {
        a = uvIndex(0, nullptr);
      }
    }
    dlAttrs.push_back(attrs);
    for (size_t si = 0; si < retail.nmat; ++si) {
      const MaterialSet& set = retail.sets[si];
      if (size_t(rmat) >= set.mats.size()) {
        throw Fail{"the retail material sets differ in length"};
      }
      Blob blob(set.mats[rmat].p, set.mats[rmat].p + set.mats[rmat].n);
      const RetailMaterial q = ParseMaterial(set.mats[rmat]);
      for (size_t i = 0; i < q.tex.size(); ++i) {
        if (q.tex[i] >= set.tex.size()) {
          throw Fail{"a retail material samples a texture it does not list"};
        }
        const auto it = slotIds.find(uint32_t(i));
        const uint32_t idx = texIndex(si, it != slotIds.end() ? it->second : set.tex[q.tex[i]]);
        for (int k = 0; k < 4; ++k) {
          blob[8 + i * 4 + k] = uint8_t(idx >> (24 - 8 * k));
        }
      }
      if (vtx != q.vtx) {
        // A texcoord slot added above has to reach the material as well: the
        // display list now sends that index.
        const size_t at = 8 + q.tex.size() * 4;
        for (int k = 0; k < 4; ++k) {
          blob[at + k] = uint8_t(vtx >> (24 - 8 * k));
        }
      }
      setBlobs[si].push_back(std::move(blob));
    }
  }
  if (uvArrays.empty()) {  // no material samples a texture, but the section must exist
    uvIndex(0, nullptr);
  }

  // Skinning: the weight group of every vertex. A model's skin is not named by
  // the CMDL: the character and its CINF decide, so a model can appear under
  // several skins, and the weights are written under every one whose skeleton
  // has the bones of the one they were mapped against. A replaced CMDL left
  // with retail's CSKR, laid out for retail's vertex count, explodes.
  std::vector<int> weights;       // per vertex: index into weightKeys
  std::vector<WeightKey> weightKeys;
  std::vector<uint32_t> skins;
  if (!opt.skins.empty()) {
    std::vector<uint32_t> sids = opt.skins;
    std::sort(sids.begin(), sids.end());
    sids.erase(std::unique(sids.begin(), sids.end()), sids.end());
    Blob ref;
    if (!io.retail(FourCC('C', 'S', 'K', 'R'), sids[0], ref)) {
      throw Fail{"retail skin " + Hex8(sids[0]) + " is not on the disc"};
    }
    const Span refSpan{ref.data(), ref.size()};
    const std::vector<uint32_t> refBones = SkinBones(refSpan);
    skins.push_back(sids[0]);
    for (size_t i = 1; i < sids.size(); ++i) {
      Blob other;
      if (io.retail(FourCC('C', 'S', 'K', 'R'), sids[i], other)) {
        const std::vector<uint32_t> bones = SkinBones(Span{other.data(), other.size()});
        if (std::includes(bones.begin(), bones.end(), refBones.begin(), refBones.end())) {
          skins.push_back(sids[i]);
          continue;
        }
      }
      Log("  note: skin " + Hex8(sids[i]) + " has a different skeleton, not writing weights for it");
    }
    std::vector<uint16_t> J;
    std::vector<float> W;
    if (skinned) {
      for (uint32_t bi : bufOrder) {
        J.insert(J.end(), buffers[bi].src->joints.begin(), buffers[bi].src->joints.end());
        W.insert(W.end(), buffers[bi].src->weights.begin(), buffers[bi].src->weights.end());
      }
    }
    const std::vector<WeightKey> perVertex =
        SkinWeights(P, n, skinned ? &J : nullptr, skinned ? &W : nullptr, retail, refSpan,
                    [this](const std::string& line) { Log(line); });
    std::map<WeightKey, int> intern;
    weights.resize(n);
    for (size_t v = 0; v < n; ++v) {
      const auto it = intern.emplace(perVertex[v], int(weightKeys.size()));
      if (it.second) {
        weightKeys.push_back(perVertex[v]);
      }
      weights[v] = it.first->second;
    }
  }
  const bool hasSkin = !skins.empty();

  // Weld: Remastered splits vertices far more than the written attributes
  // need. Vertices equal in every written value (float32 position, normal, all
  // UV arrays, weight group) become one, so this is lossless; the first of
  // each keeps its place.
  {
    const size_t width = (6 + uvArrays.size() * 2) * 4 + 4 + 4;
    std::unordered_map<std::string, uint32_t> seen;
    seen.reserve(n * 2);
    std::vector<uint32_t> vmap(n), rep;
    std::string keyBytes(width, '\0');
    for (size_t v = 0; v < n; ++v) {
      char* out = keyBytes.data();
      auto put = [&](double d) {
        const float f = float(d);
        std::memcpy(out, &f, 4);
        out += 4;
      };
      for (int c = 0; c < 3; ++c) {
        put(P[v * 3 + c]);
      }
      for (int c = 0; c < 3; ++c) {
        put(N[v * 3 + c]);
      }
      for (const auto& a : uvArrays) {
        put(a[v * 2]);
        put(a[v * 2 + 1]);
      }
      const int32_t wid = hasSkin ? weights[v] : 0;
      std::memcpy(out, &wid, 4);
      if (useColor) {
        std::memcpy(out + 4, &C[v * 4], 4);
      }
      const auto it = seen.emplace(keyBytes, uint32_t(rep.size()));
      if (it.second) {
        rep.push_back(uint32_t(v));
      }
      vmap[v] = it.first->second;
    }
    for (Prim& p : prims) {
      for (uint32_t& i : p.I) {
        i = vmap[i];
      }
    }
    auto take = [&](std::vector<double>& a, size_t per) {
      std::vector<double> out(rep.size() * per);
      for (size_t i = 0; i < rep.size(); ++i) {
        std::copy(a.begin() + size_t(rep[i]) * per, a.begin() + size_t(rep[i]) * per + per, out.begin() + i * per);
      }
      a = std::move(out);
    };
    take(P, 3);
    take(N, 3);
    for (auto& a : uvArrays) {
      take(a, 2);
    }
    if (useColor) {
      std::vector<uint8_t> c(rep.size() * 4);
      for (size_t i = 0; i < rep.size(); ++i) {
        std::copy(C.begin() + size_t(rep[i]) * 4, C.begin() + size_t(rep[i]) * 4 + 4, c.begin() + i * 4);
      }
      C = std::move(c);
    }
    if (hasSkin) {
      std::vector<int> w(rep.size());
      for (size_t i = 0; i < rep.size(); ++i) {
        w[i] = weights[rep[i]];
      }
      weights = std::move(w);
    }
    Log("  welded " + std::to_string(n) + " -> " + std::to_string(rep.size()) + " verts");
    n = rep.size();
  }
  const size_t nuv = n * uvArrays.size();
  // Past 16-bit indices, write windowed surfaces (the port's own 'PBIX' base
  // indices).
  const bool big = n >= 0xFFFF || nuv >= 0x10000;

  auto writeCskr = [&](const std::vector<std::pair<const WeightKey*, uint32_t>>& runs, size_t total) {
    const Blob cskr = CskrBytes(runs, total);
    for (uint32_t s : skins) {
      uint32_t id = s;
      for (size_t i = 0; i < opt.outputSkins.size() && i < opt.skins.size(); ++i) {
        if (opt.skins[i] == s) {
          id = opt.outputSkins[i];
        }
      }
      Write(Hex8(id) + ".CSKR", cskr);
    }
  };

  std::vector<Blob> surfs;
  Blob secP, secN, secC, secUV;
  double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
  bool haveBounds = false;
  auto bound = [&](const double* p) {
    for (int c = 0; c < 3; ++c) {
      lo[c] = haveBounds ? std::min(lo[c], p[c]) : p[c];
      hi[c] = haveBounds ? std::max(hi[c], p[c]) : p[c];
    }
    haveBounds = true;
  };
  const size_t kDlChunk = 65535 / 3 * 3;
  if (big) {
    // Each surface covers a run of at most `window` vertices (duplicated where
    // windows meet) and stores base indices after its bounds, which the port
    // adds to every display list index. UVs are interleaved per vertex so one
    // base serves every texture attribute. Skin runs are per window, so weight
    // groups may repeat.
    const size_t k = uvArrays.size();
    const size_t window = 0xFFFF / k;
    std::vector<std::pair<const WeightKey*, uint32_t>> runs;
    size_t start = 0;
    std::vector<int> local(n, -1);
    for (const Prim& p : prims) {
      const std::vector<uint32_t>& attrs = dlAttrs[p.omat];
      std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> chunks;  // vertices, triangles
      std::vector<uint32_t> verts, cur;
      auto flush = [&]() {
        for (uint32_t v : verts) {
          local[v] = -1;
        }
        chunks.emplace_back(std::move(verts), std::move(cur));
        verts.clear();
        cur.clear();
      };
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        const uint32_t a = p.I[t], b = p.I[t + 1], c = p.I[t + 2];
        size_t fresh = local[a] < 0 ? 1 : 0;
        fresh += local[b] < 0 && b != a ? 1 : 0;
        fresh += local[c] < 0 && c != a && c != b ? 1 : 0;
        if (verts.size() + fresh > window) {
          flush();
        }
        for (uint32_t v : {a, b, c}) {
          if (local[v] < 0) {
            local[v] = int(verts.size());
            verts.push_back(v);
          }
          cur.push_back(v);
        }
      }
      if (!cur.empty()) {
        flush();
      }
      for (auto& chunk : chunks) {
        std::vector<uint32_t>& cv = chunk.first;
        if (hasSkin) {
          std::vector<int> keyOrder(weightKeys.size(), -1);
          std::vector<int> order;
          std::vector<uint32_t> counts;
          for (uint32_t v : cv) {
            if (keyOrder[weights[v]] < 0) {
              keyOrder[weights[v]] = int(order.size());
              order.push_back(weights[v]);
              counts.push_back(0);
            }
            ++counts[keyOrder[weights[v]]];
          }
          std::stable_sort(cv.begin(), cv.end(),
                           [&](uint32_t x, uint32_t y) { return keyOrder[weights[x]] < keyOrder[weights[y]]; });
          for (size_t i = 0; i < order.size(); ++i) {
            runs.emplace_back(&weightKeys[order[i]], counts[i]);
          }
        }
        for (size_t i = 0; i < cv.size(); ++i) {
          local[cv[i]] = int(i);
        }
        double centre[3] = {0, 0, 0}, clo[3], chi[3];
        for (size_t i = 0; i < cv.size(); ++i) {
          const double* pos = &P[size_t(cv[i]) * 3];
          for (int c = 0; c < 3; ++c) {
            PF(secP, pos[c]);
            PF(secN, N[size_t(cv[i]) * 3 + c]);
            centre[c] += pos[c];
            clo[c] = i ? std::min(clo[c], pos[c]) : pos[c];
            chi[c] = i ? std::max(chi[c], pos[c]) : pos[c];
          }
          bound(pos);
          if (useColor) {
            secC.insert(secC.end(), C.begin() + size_t(cv[i]) * 4, C.begin() + size_t(cv[i]) * 4 + 4);
          }
          for (const auto& a : uvArrays) {
            PF(secUV, a[size_t(cv[i]) * 2]);
            PF(secUV, a[size_t(cv[i]) * 2 + 1]);
          }
        }
        for (double& c : centre) {
          c /= double(cv.size());
        }
        Blob dl;
        const std::vector<uint32_t>& tris = chunk.second;
        for (size_t s = 0; s < tris.size(); s += kDlChunk) {
          const size_t count = std::min(kDlChunk, tris.size() - s);
          P8(dl, 0x90);
          P16(dl, uint32_t(count));
          for (size_t i = s; i < s + count; ++i) {
            const uint32_t j = uint32_t(local[tris[i]]);
            P16(dl, j);
            P16(dl, j);
            if (dlColor[p.omat]) {
              P16(dl, j);
            }
            for (uint32_t a : attrs) {
              P16(dl, uint32_t(j * k + a));
            }
          }
        }
        Pad(dl);
        for (uint32_t v : cv) {
          local[v] = -1;
        }
        Blob s;
        SurfaceHeader(s, centre, uint32_t(p.omat), dl.size(), 64);
        for (double v : clo) {
          PF(s, v);
        }
        for (double v : chi) {
          PF(s, v);
        }
        P32(s, 0x50424958);  // 'PBIX': position, normal, colour, uv, packed uv bases
        P32(s, uint32_t(start));
        P32(s, uint32_t(start));
        P32(s, useColor ? uint32_t(start) : 0);
        P32(s, uint32_t(start * k));
        P32(s, 0);
        s.resize(s.size() + 16, 0);
        Pad(s);
        Append(s, dl);
        surfs.push_back(std::move(s));
        start += cv.size();
      }
    }
    if (hasSkin) {
      writeCskr(runs, start);
    }
    Log("  windowed: " + std::to_string(surfs.size()) + " surfaces, " + std::to_string(start) + " verts");
    n = start;
  } else {
    // A skin is a list of weight groups, each covering a run of vertices, so
    // the vertices are ordered by group.
    std::vector<uint32_t> order(n);
    for (size_t v = 0; v < n; ++v) {
      order[v] = uint32_t(v);
    }
    if (hasSkin) {
      std::vector<int> keyOrder(weightKeys.size(), -1);
      std::vector<int> first;
      std::vector<uint32_t> counts;
      for (size_t v = 0; v < n; ++v) {
        if (keyOrder[weights[v]] < 0) {
          keyOrder[weights[v]] = int(first.size());
          first.push_back(weights[v]);
          counts.push_back(0);
        }
        ++counts[keyOrder[weights[v]]];
      }
      std::stable_sort(order.begin(), order.end(),
                       [&](uint32_t x, uint32_t y) { return keyOrder[weights[x]] < keyOrder[weights[y]]; });
      std::vector<std::pair<const WeightKey*, uint32_t>> runs;
      for (size_t i = 0; i < first.size(); ++i) {
        runs.emplace_back(&weightKeys[first[i]], counts[i]);
      }
      writeCskr(runs, n);
    }
    std::vector<uint32_t> remap(n);
    for (size_t i = 0; i < n; ++i) {
      remap[order[i]] = uint32_t(i);
    }
    for (size_t i = 0; i < n; ++i) {
      const double* pos = &P[size_t(order[i]) * 3];
      bound(pos);
      for (int c = 0; c < 3; ++c) {
        PF(secP, pos[c]);
        PF(secN, N[size_t(order[i]) * 3 + c]);
      }
      if (useColor) {
        secC.insert(secC.end(), C.begin() + size_t(order[i]) * 4, C.begin() + size_t(order[i]) * 4 + 4);
      }
    }
    for (const auto& a : uvArrays) {
      for (size_t i = 0; i < n; ++i) {
        PF(secUV, a[size_t(order[i]) * 2]);
        PF(secUV, a[size_t(order[i]) * 2 + 1]);
      }
    }
    std::vector<uint8_t> mark(n, 0);
    for (const Prim& p : prims) {
      const std::vector<uint32_t>& attrs = dlAttrs[p.omat];
      Blob dl;
      for (size_t s = 0; s < p.I.size(); s += kDlChunk) {
        const size_t count = std::min(kDlChunk, p.I.size() - s);
        P8(dl, 0x90);
        P16(dl, uint32_t(count));
        for (size_t i = s; i < s + count; ++i) {
          const uint32_t j = remap[p.I[i]];
          mark[j] = 1;
          P16(dl, j);
          P16(dl, j);
          if (dlColor[p.omat]) {
            P16(dl, j);
          }
          for (uint32_t a : attrs) {
            P16(dl, uint32_t(j + a * n));
          }
        }
      }
      Pad(dl);
      // The mean of the surface's vertices, each once, in index order.
      double centre[3] = {0, 0, 0};
      size_t count = 0;
      for (size_t j = 0; j < n; ++j) {
        if (mark[j]) {
          mark[j] = 0;
          const double* pos = &P[size_t(order[j]) * 3];
          for (int c = 0; c < 3; ++c) {
            centre[c] += pos[c];
          }
          ++count;
        }
      }
      for (double& c : centre) {
        c /= double(count);
      }
      Blob s;
      SurfaceHeader(s, centre, uint32_t(p.omat), dl.size(), 0);
      Pad(s);
      Append(s, dl);
      surfs.push_back(std::move(s));
    }
  }
  Pad(secP);
  Pad(secN);
  Pad(secC);
  Pad(secUV);

  std::vector<Blob> secs;
  for (size_t si = 0; si < retail.nmat; ++si) {
    Blob b;
    P32(b, uint32_t(setTex[si].size()));
    for (uint32_t t : setTex[si]) {
      P32(b, t);
    }
    P32(b, uint32_t(setBlobs[si].size()));
    uint32_t end = 0;
    for (const Blob& m : setBlobs[si]) {
      end += uint32_t(m.size());
      P32(b, end);
    }
    for (const Blob& m : setBlobs[si]) {
      Append(b, m);
    }
    Pad(b);
    secs.push_back(std::move(b));
  }
  secs.push_back(std::move(secP));
  secs.push_back(std::move(secN));
  secs.push_back(std::move(secC));  // colours
  secs.push_back(std::move(secUV));
  secs.emplace_back();  // packed texcoords
  {
    Blob table;
    P32(table, uint32_t(surfs.size()));
    for (const Blob& s : surfs) {
      P32(table, uint32_t(s.size()));
    }
    Pad(table);
    secs.push_back(std::move(table));
  }
  size_t tris = 0;
  for (const Prim& p : prims) {
    tris += p.I.size() / 3;
  }
  const size_t nsurf = surfs.size();
  for (Blob& s : surfs) {
    secs.push_back(std::move(s));
  }
  Blob out;
  P32(out, 0xDEADBABE);
  P32(out, 2);
  P32(out, retail.flags & ~uint32_t(0x2));  // float normals
  for (double v : lo) {
    PF(out, v);
  }
  for (double v : hi) {
    PF(out, v);
  }
  P32(out, uint32_t(secs.size()));
  P32(out, retail.nmat);
  for (const Blob& s : secs) {
    P32(out, uint32_t(s.size()));
  }
  Pad(out);
  for (const Blob& s : secs) {
    Append(out, s);
  }
  const uint32_t outputModel = opt.outputModel != 0 ? opt.outputModel : opt.retail;
  Write(Hex8(outputModel) + ".CMDL", out);
  Log("  wrote " + Hex8(outputModel) + ".CMDL: " + std::to_string(n) + " verts, " + std::to_string(tris) + " tris, " +
      std::to_string(keys.size()) + " materials, " + std::to_string(nsurf) + " surfaces");
}

Converter::Converter(ConvertIO io) : m_state(new State) { m_state->io = std::move(io); }

Converter::~Converter() { delete m_state; }

bool Converter::Convert(const Model& model, const ConvertOptions& options, std::string& error) {
  if ((!m_state->io.retail && !options.standalone) || !m_state->io.texture || !m_state->io.write) {
    error = "the converter has no way to read or write";
    return false;
  }
  try {
    m_state->Convert(model, options);
  } catch (const Fail& f) {
    error = Hex8(options.retail) + ": " + f.what;
    return false;
  } catch (const std::exception& e) {
    error = Hex8(options.retail) + ": " + e.what();
    return false;
  }
  return true;
}

int Converter::PbrMaterials() const { return m_state->pbr; }
int Converter::TevMaterials() const { return m_state->tev; }

} // namespace PortRemastered
