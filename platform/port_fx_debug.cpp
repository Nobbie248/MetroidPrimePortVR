#include "port_fx_debug.h"
#include <cstdarg>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <unordered_set>

#include "Kyoto/Particles/CParticleGen.hpp"

namespace PortFx {
bool gMuteActive = false;
bool gTiming = false;
float gTimeScale = 1.f;
std::atomic< uint32_t > gQuads{0}, gTriangles{0}, gDraws{0};
std::atomic< int64_t > gUpdateNs{0}, gRenderNs{0};

namespace {
std::mutex sMutex;
CParticleGen* sHead = nullptr;
CParticleGen* sTail = nullptr;
uint32_t sNextId = 0;

std::set< uint32_t > sMuted;
bool sSolo = false;

// last-frame values published by FrameBoundary
uint32_t sLastQuads = 0, sLastTris = 0, sLastDraws = 0;
int64_t sLastUpdateNs = 0, sLastRenderNs = 0;

std::string Fourcc(uint32_t k) {
  char b[5] = {char(k >> 24), char(k >> 16), char(k >> 8), char(k), 0};
  return b;
}

std::string Fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

std::vector< CParticleGen* > All() {
  std::vector< CParticleGen* > v;
  for (CParticleGen* g = sHead; g != nullptr; g = g->xPortFxNext) {
    v.push_back(g);
  }
  return v;
}

std::string Line(CParticleGen* g, const PortFxInfo& i) {
  std::string s = Fmt("#%u %s %08X pos(%.1f %.1f %.1f) %d", g->xPortFxId, Fourcc(i.kind).c_str(),
                      i.asset, i.pos[0], i.pos[1], i.pos[2], i.particles);
  if (i.maxParticles >= 0) {
    s += Fmt("/%d", i.maxParticles);
  }
  s += Fmt(" parts frame %d", i.frame);
  if (i.lifetime >= 0) {
    s += Fmt("/%d", i.lifetime);
  }
  s += Fmt(" %s%s", i.emitting ? "emitting" : "idle", i.deletable ? " finished" : "");
  if (!i.children.empty()) {
    s += Fmt(" children=%zu", i.children.size());
  }
  s += i.vfx.empty() ? " retail-draw" : " vfx[" + i.vfx + "]";
  if (gMuteActive && IsMuted(*g)) {
    s += " MUTED";
  }
  return s;
}

void TreeRec(CParticleGen* g, int depth, const Emit& out, std::unordered_set< CParticleGen* >& seen) {
  PortFxInfo i;
  g->PortFxDescribe(i);
  out(std::string(size_t(depth) * 2, ' ') + Line(g, i));
  if (!seen.insert(g).second) {
    return;
  }
  for (CParticleGen* c : i.children) {
    TreeRec(c, depth + 1, out, seen);
  }
}

void AssetsRec(CParticleGen* g, std::vector< uint32_t >& v, std::unordered_set< CParticleGen* >& seen) {
  if (!seen.insert(g).second) {
    return;
  }
  PortFxInfo i;
  g->PortFxDescribe(i);
  if (std::find(v.begin(), v.end(), i.asset) == v.end()) {
    v.push_back(i.asset);
  }
  for (CParticleGen* c : i.children) {
    AssetsRec(c, v, seen);
  }
}

CParticleGen* Find(uint32_t id) {
  for (CParticleGen* g = sHead; g != nullptr; g = g->xPortFxNext) {
    if (g->xPortFxId == id) {
      return g;
    }
  }
  return nullptr;
}

// Roots = generators no other generator lists as a child.
std::vector< CParticleGen* > Roots() {
  std::vector< CParticleGen* > all = All();
  std::unordered_set< CParticleGen* > kids;
  for (CParticleGen* g : all) {
    PortFxInfo i;
    g->PortFxDescribe(i);
    kids.insert(i.children.begin(), i.children.end());
  }
  std::vector< CParticleGen* > roots;
  for (CParticleGen* g : all) {
    if (kids.count(g) == 0) {
      roots.push_back(g);
    }
  }
  return roots;
}
} // namespace

void Register(CParticleGen* gen) {
  std::lock_guard< std::mutex > lock(sMutex);
  gen->xPortFxId = ++sNextId;
  gen->xPortFxPrev = sTail;
  gen->xPortFxNext = nullptr;
  (sTail != nullptr ? sTail->xPortFxNext : sHead) = gen;
  sTail = gen;
}

void Unregister(CParticleGen* gen) {
  std::lock_guard< std::mutex > lock(sMutex);
  if (gen->xPortFxId == 0) {
    return;
  }
  (gen->xPortFxPrev != nullptr ? gen->xPortFxPrev->xPortFxNext : sHead) = gen->xPortFxNext;
  (gen->xPortFxNext != nullptr ? gen->xPortFxNext->xPortFxPrev : sTail) = gen->xPortFxPrev;
  gen->xPortFxPrev = gen->xPortFxNext = nullptr;
  gen->xPortFxId = 0;
}

bool IsMuted(const CParticleGen& gen) {
  const bool in = sMuted.count(gen.PortFxAsset()) != 0;
  return sSolo ? !in : in;
}

void FrameBoundary() {
  sLastQuads = gQuads.exchange(0);
  sLastTris = gTriangles.exchange(0);
  sLastDraws = gDraws.exchange(0);
  sLastUpdateNs = gUpdateNs.exchange(0);
  sLastRenderNs = gRenderNs.exchange(0);
}

void List(const std::string& filter, const Emit& out) {
  std::lock_guard< std::mutex > lock(sMutex);
  std::string f = filter;
  for (char& c : f) {
    c = char(toupper(static_cast< unsigned char >(c)));
  }
  int n = 0;
  for (CParticleGen* g : Roots()) {
    PortFxInfo i;
    g->PortFxDescribe(i);
    std::string line = Line(g, i);
    std::string up = line;
    for (char& c : up) {
      c = char(toupper(static_cast< unsigned char >(c)));
    }
    if (!f.empty() && up.find(f) == std::string::npos) {
      continue;
    }
    out(line);
    ++n;
  }
  out(Fmt("%d root generator(s)", n));
}

bool Tree(uint32_t id, const Emit& out) {
  std::lock_guard< std::mutex > lock(sMutex);
  CParticleGen* g = Find(id);
  if (g == nullptr) {
    return false;
  }
  std::unordered_set< CParticleGen* > seen;
  TreeRec(g, 0, out, seen);
  return true;
}

void Stats(const Emit& out) {
  std::lock_guard< std::mutex > lock(sMutex);
  int counts[3] = {0, 0, 0};
  int parts[3] = {0, 0, 0};
  int roots = 0, total = 0;
  for (CParticleGen* g : All()) {
    PortFxInfo i;
    g->PortFxDescribe(i);
    int k = i.kind == 'PART' ? 0 : i.kind == 'SWHC' ? 1 : 2;
    ++counts[k];
    parts[k] += i.particles;
    ++total;
  }
  roots = int(Roots().size());
  out(Fmt("live generators: %d (%d roots): PART %d (%d particles), SWHC %d (%d), ELSC %d (%d)", total,
          roots, counts[0], parts[0], counts[1], parts[1], counts[2], parts[2]));
  out(Fmt("vfx last frame: %u quads, %u triangles, %u draws", sLastQuads, sLastTris, sLastDraws));
  if (!gTiming) {
    gTiming = true;
    out("timers started now; update/render times show from the next frame");
  }
  out(Fmt("cpu last frame: update %.3f ms, render %.3f ms", double(sLastUpdateNs) / 1e6,
          double(sLastRenderNs) / 1e6));
}

void MuteAdd(uint32_t asset) {
  if (sSolo) {
    sMuted.clear();
    sSolo = false;
  }
  sMuted.insert(asset);
  gMuteActive = true;
}

void SoloSet(const std::vector< uint32_t >& assets) {
  sMuted.clear();
  sMuted.insert(assets.begin(), assets.end());
  sSolo = true;
  gMuteActive = true;
}

void MuteClear() {
  sMuted.clear();
  sSolo = false;
  gMuteActive = false;
}

void MuteList(const Emit& out) {
  if (!gMuteActive) {
    out("nothing muted");
    return;
  }
  std::string s = sSolo ? "solo (only these draw):" : "muted:";
  for (uint32_t a : sMuted) {
    s += Fmt(" %08X", a);
  }
  out(s);
}

std::vector< uint32_t > TreeAssets(uint32_t id) {
  std::lock_guard< std::mutex > lock(sMutex);
  std::vector< uint32_t > v;
  if (CParticleGen* g = Find(id)) {
    std::unordered_set< CParticleGen* > seen;
    AssetsRec(g, v, seen);
  }
  return v;
}

uint32_t IdOf(const CParticleGen* gen) { return gen != nullptr ? gen->xPortFxId : 0; }

CParticleGen* Newest() {
  std::lock_guard< std::mutex > lock(sMutex);
  std::vector< CParticleGen* > roots = Roots();
  return roots.empty() ? nullptr : roots.back();
}

bool Alive(uint32_t id) {
  std::lock_guard< std::mutex > lock(sMutex);
  return Find(id) != nullptr;
}
} // namespace PortFx
