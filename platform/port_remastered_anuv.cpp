#include "port_remastered_anuv.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace PortRemastered {

AnuvTransform::AnuvTransform() {
  arg[2].value = 1.0f;
  arg[3].value = 1.0f;
}

bool AnuvTransform::Identity() const {
  const float identity[5] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f};
  for (int i = 0; i < 5; ++i) {
    if (arg[i].kind != AnuvArg::Kind::Const || arg[i].value != identity[i]) {
      return false;
    }
  }
  return true;
}

const char* AnuvSkipName(AnuvSkip skip) {
  switch (skip) {
  case AnuvSkip::None: return "none";
  case AnuvSkip::NotScalarArg: return "argument is not a plain value or curve";
  case AnuvSkip::NotTransform: return "transform id of an unknown kind";
  case AnuvSkip::ContextNotRoot: return "clock not driven by the root time";
  case AnuvSkip::ContextConflict: return "clock reached at two rates";
  case AnuvSkip::ShortCurve: return "curve shorter than its period";
  case AnuvSkip::DoubleWrite: return "two curves feed one word";
  case AnuvSkip::BadIndex: return "index past the end of a table";
  }
  return "?";
}

namespace {

using port::ReadLE32;

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t pos = 0;
  bool ok = true;

  uint32_t U32() {
    if (pos + 4 > size) {
      ok = false;
      return 0;
    }
    const uint32_t v = ReadLE32(data + pos);
    pos += 4;
    return v;
  }
  const uint8_t* Bytes(size_t n) {
    if (n > size - pos) {
      ok = false;
      return nullptr;
    }
    const uint8_t* p = data + pos;
    pos += n;
    return p;
  }
};

// CEvalMemBuilder's blob stores each element byte-swapped; the tail says how big each
// was (a varint count, then two bits per element), so the swap is undone element by
// element. Returns false when the tail doesn't add up.
bool UnswapBlob(const uint8_t* raw, size_t n, std::vector<uint8_t>& out) {
  if (n == 0) {
    return false;
  }
  size_t p = n - 1;
  size_t count = 0;
  while (raw[p] == 0xff) {
    count += 0xff;
    if (p == 0) {
      return false;
    }
    --p;
  }
  count += raw[p];
  if (p == 0) {
    return false;
  }
  const size_t codesEnd = p - 1;
  if (count / 4 > codesEnd) {
    return false;
  }
  out.assign(raw, raw + n);
  static const size_t kSize[4] = {8, 4, 2, 1};
  size_t off = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t at = codesEnd - (i >> 2);
    const size_t sz = kSize[(raw[at] >> ((2 * i) & 6)) & 3];
    if (off + sz > n) {
      return false;
    }
    for (size_t k = 0; k < sz; ++k) {
      out[off + k] = raw[off + sz - 1 - k];
    }
    off += sz;
  }
  return true;
}

struct Program {
  std::vector<uint8_t> blob;
  std::vector<std::vector<uint32_t>> handles;  // raw words
  uint32_t roots[3] = {0, 0, 0};
  std::vector<uint32_t> ids;

  size_t Words() const { return blob.size() / 4; }
  uint32_t W(size_t i) const { return ReadLE32(blob.data() + 4 * i); }
  float F(size_t i) const {
    const uint32_t u = W(i);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  double D(size_t i) const {
    uint64_t u = uint64_t(W(i)) | (uint64_t(W(i + 1)) << 32);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
  }
};

// A clock the root time drives (SetTimeContext): sample position = rate * seconds.
struct Clock {
  double rate = 0.0;
  double period = 0.0;
  bool root = true;
  bool conflict = false;
};

struct Solve {
  uint32_t clock = 0;
  uint32_t handle = 0;
  bool nearest = false;
  bool doubled = false;
};

struct Flattener {
  const Program& p;
  std::map<uint32_t, Clock> clocks;
  std::map<uint32_t, Solve> solves;  // keyed by destination word

  explicit Flattener(const Program& prog) : p(prog) {}

  // Walks the root's time tree the way CEvalDataContext dispatches it, noting which
  // clocks the seconds reach and which words the curves write. `scale` is the factor
  // from seconds to the dispatched time; a clock reached by anything but SetTime
  // (which accumulates or sets a phase) isn't a function of the root time alone.
  void Walk(uint32_t id, double scale, int depth) {
    if (depth > 16) {
      return;
    }
    const uint32_t type = id & 0x1f;
    const uint32_t idx = (id >> 5) & 0xfff;
    if (idx >= p.Words()) {
      return;
    }
    switch (type) {
    case 0:
      Walk(p.W(idx), scale * 60.0, depth + 1);
      break;
    case 1: {
      const uint32_t n = p.W(idx);
      for (uint32_t k = 0; k < n && idx + 1 + k < p.Words(); ++k) {
        Walk(p.W(idx + 1 + k), scale, depth + 1);
      }
      break;
    }
    case 2:
    case 5: {
      Clock& c = clocks[idx];
      c.root = false;
      break;
    }
    case 4: {
      if (idx + 7 >= p.Words()) {
        break;
      }
      const double rate = p.D(idx + 2) * scale;
      auto it = clocks.find(idx);
      if (it == clocks.end()) {
        Clock c;
        c.rate = rate;
        c.period = p.D(idx + 4);
        clocks[idx] = c;
      } else if (std::fabs(it->second.rate - rate) > 1e-9) {
        it->second.conflict = true;
      }
      break;
    }
    case 3:
    case 6: {
      if (idx + 3 >= p.Words()) {
        break;
      }
      Solve s;
      s.clock = p.W(idx + 1);
      s.handle = p.W(idx + 3) & 0xff;
      s.nearest = type == 6;
      const uint32_t dest = p.W(idx);
      s.doubled = solves.count(dest) != 0;
      solves[dest] = s;
      break;
    }
    default:
      break;
    }
  }

  // The argument a word of the blob stands for, read as an E1 value id's first word.
  bool Arg(uint32_t id, AnuvArg& out, AnuvSkip& skip) const {
    if ((id & 0x1f) != 1) {
      skip = AnuvSkip::NotScalarArg;
      return false;
    }
    const uint32_t idx = (id >> 5) & 0xfff;
    if (idx >= p.Words()) {
      skip = AnuvSkip::BadIndex;
      return false;
    }
    auto it = solves.find(idx);
    if (it == solves.end()) {
      out = AnuvArg();
      out.value = p.F(idx);
      return true;
    }
    const Solve& s = it->second;
    if (s.doubled) {
      skip = AnuvSkip::DoubleWrite;
      return false;
    }
    auto ct = clocks.find(s.clock);
    if (ct == clocks.end() || !ct->second.root) {
      skip = AnuvSkip::ContextNotRoot;
      return false;
    }
    if (ct->second.conflict) {
      skip = AnuvSkip::ContextConflict;
      return false;
    }
    if (s.handle >= p.handles.size()) {
      skip = AnuvSkip::BadIndex;
      return false;
    }
    const Clock& c = ct->second;
    const std::vector<uint32_t>& h = p.handles[s.handle];
    // Linear reads one past the last whole step, nearest up to the rounded period.
    if (c.period < 1.0 || c.period + 1.0 > double(h.size())) {
      skip = AnuvSkip::ShortCurve;
      return false;
    }
    out = AnuvArg();
    out.kind = s.nearest ? AnuvArg::Kind::Nearest : AnuvArg::Kind::Linear;
    out.samplesPerSecond = float(c.rate);
    out.period = float(c.period);
    out.samples.resize(size_t(c.period) + 1);
    for (size_t i = 0; i < out.samples.size(); ++i) {
      std::memcpy(&out.samples[i], &h[i], 4);
    }
    return true;
  }

  bool Transform(uint32_t id, AnuvTransform& out, AnuvSkip& skip) const {
    const uint32_t type = id & 0x1f;
    const uint32_t idx = (id >> 5) & 0xfff;
    out = AnuvTransform();
    if (type == 3) {
      return true;
    }
    if (type == 4) {
      if (idx >= p.Words()) {
        skip = AnuvSkip::BadIndex;
        return false;
      }
      return Arg(p.W(idx), out.arg[0], skip);
    }
    if (type == 2) {
      if (idx + 5 > p.Words()) {
        skip = AnuvSkip::BadIndex;
        return false;
      }
      for (int k = 0; k < 5; ++k) {
        if (!Arg(p.W(idx + k), out.arg[k], skip)) {
          return false;
        }
      }
      return true;
    }
    skip = AnuvSkip::NotTransform;
    return false;
  }
};

}  // namespace

bool ParseAnuv(const uint8_t* data, size_t size, Anuv& out, std::string& error) {
  out = Anuv();
  Reader r{data, size};
  const uint32_t numWords = r.U32();
  const uint32_t numHandles = r.U32();
  if (!r.ok || numWords > size / 4 || numHandles > size) {
    error = "implausible ANUV header";
    return false;
  }
  Program prog;
  const uint8_t* raw = r.Bytes(size_t(numWords) * 4);
  if (raw == nullptr || !UnswapBlob(raw, size_t(numWords) * 4, prog.blob)) {
    error = "ANUV blob does not unswap";
    return false;
  }
  for (uint32_t i = 0; i < numHandles; ++i) {
    const uint32_t bytes = r.U32();
    const uint8_t* h = r.Bytes(bytes);
    if (h == nullptr || bytes % 4 != 0) {
      error = "ANUV curve data truncated";
      return false;
    }
    std::vector<uint32_t> words(bytes / 4);
    for (size_t k = 0; k < words.size(); ++k) {
      words[k] = ReadLE32(h + 4 * k);
    }
    prog.handles.push_back(std::move(words));
  }
  for (int i = 0; i < 3; ++i) {
    prog.roots[i] = r.U32();
  }
  const uint32_t numIds = r.U32();
  if (!r.ok || numIds > (size - r.pos) / 4) {
    error = "ANUV id table truncated";
    return false;
  }
  for (uint32_t i = 0; i < numIds; ++i) {
    prog.ids.push_back(r.U32());
  }
  const uint32_t numEntries = r.U32();
  if (!r.ok || numEntries > (size - r.pos) / 16) {
    error = "ANUV entry table truncated";
    return false;
  }
  std::vector<int32_t> entryIds(size_t(numEntries) * 4);
  for (int32_t& v : entryIds) {
    v = int32_t(r.U32());
  }
  const uint32_t numMap = r.U32();
  const uint8_t* map = r.Bytes(numMap);
  if (!r.ok || map == nullptr) {
    error = "ANUV material map truncated";
    return false;
  }
  out.matmap.assign(map, map + numMap);

  Flattener fl(prog);
  fl.Walk(prog.roots[1], 1.0, 0);
  out.entries.resize(numEntries);
  for (uint32_t e = 0; e < numEntries; ++e) {
    AnuvEntry& entry = out.entries[e];
    for (int k = 0; k < 3 && entry.skip == AnuvSkip::None; ++k) {
      const int32_t id = entryIds[size_t(e) * 4 + k];
      if (id < 0 || size_t(id) >= prog.ids.size()) {
        entry.skip = AnuvSkip::BadIndex;
        break;
      }
      fl.Transform(prog.ids[size_t(id)], entry.xf[k], entry.skip);
    }
  }
  return true;
}

namespace {

float EvalArg(const AnuvArg& a, double seconds) {
  if (a.kind == AnuvArg::Kind::Const) {
    return a.value;
  }
  if (a.samples.empty()) {
    return 0.0f;
  }
  // The runtime's guards (CCubeMaterial.cpp PortUvArg), so a curve that is
  // degenerate or read backwards is the same here as in game.
  double x = 0.0;
  if (a.period > 0.0f && std::isfinite(a.period)) {
    x = std::fmod(seconds * double(a.samplesPerSecond), double(a.period));
    if (x < 0.0) {
      x += double(a.period);
    }
  }
  if (!(x >= 0.0)) {
    x = 0.0;
  }
  const size_t last = a.samples.size() - 1;
  if (a.kind == AnuvArg::Kind::Nearest) {
    return a.samples[std::min(size_t(std::floor(x + 0.5)), last)];
  }
  const double fl = std::floor(x);
  const float frac = float(x - fl);
  const size_t lo = std::min(size_t(fl), last);
  const size_t hi = std::min(lo + 1, last);
  return (1.0f - frac) * a.samples[lo] + frac * a.samples[hi];
}

}  // namespace

void EvalAnuvTransform(const AnuvTransform& xf, double seconds, float out[8]) {
  const float a = EvalArg(xf.arg[0], seconds);
  const float b = EvalArg(xf.arg[1], seconds);
  const float c = EvalArg(xf.arg[2], seconds);
  const float d = EvalArg(xf.arg[3], seconds);
  const float e = EvalArg(xf.arg[4], seconds);
  const float s = std::sin(e);
  const float co = std::cos(e);
  out[0] = co * c;
  out[1] = -s * d;
  out[2] = 0.0f;
  out[3] = (a - 0.5f) * co - (b - 0.5f) * s + 0.5f;
  out[4] = s * c;
  out[5] = co * d;
  out[6] = 0.0f;
  out[7] = (a - 0.5f) * s + (b - 0.5f) * co + 0.5f;
}

std::vector<uint32_t> AnuvAnimWords(const AnuvTransform& xf) {
  auto bits = [](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  };
  std::vector<uint32_t> w;
  w.push_back(kAnuvAnimType);
  for (const AnuvArg& a : xf.arg) {
    w.push_back(uint32_t(a.kind));
    if (a.kind == AnuvArg::Kind::Const) {
      w.push_back(bits(a.value));
      continue;
    }
    w.push_back(bits(a.samplesPerSecond));
    w.push_back(bits(a.period));
    w.push_back(uint32_t(a.samples.size()));
    for (float s : a.samples) {
      w.push_back(bits(s));
    }
  }
  return w;
}

}  // namespace PortRemastered
