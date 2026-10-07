// See platform/include/port_remastered_anim.h for what this is and why it exists.
// A port of build/mpr/anim/chpr_anim.py and chpr_skel.py. The arithmetic is done in
// doubles with the same float roundings the Python applies, so the output matches it.

#include "port_remastered_anim.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace PortRemasteredAnim {
namespace {

constexpr float kFramesPerRate = 30.0f;  // Anim::fps

// A problem with the file or a layout the decoder does not know. Thrown inside this
// file only and turned into the error string at the API boundary.
struct Failure {
  std::string message;
};

[[noreturn]] void Fail(const std::string& message) { throw Failure{message}; }

using port::ReadLE16;
using port::ReadLE32;
using port::ReadLEFloat;

// The Python rounds to single precision at a few points; this is that rounding.
double F32(double x) { return double(float(x)); }

// The file as a bounds-checked view.
struct Bytes {
  const uint8_t* data;
  size_t size;

  bool Has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  void Need(size_t offset, size_t count, const char* what) const {
    if (!Has(offset, count)) {
      Fail(std::string("truncated ") + what);
    }
  }
};

// CAnimBitStream: bits are read least significant first. Reading past the end gives
// zeros, as the Python does; callers that loop on the stream check Overrun().
class BitStream {
public:
  BitStream(const Bytes& bytes, uint64_t bitPos) : m_bytes(bytes), m_pos(bitPos) {}

  uint32_t Read(int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      const uint64_t byteIndex = m_pos >> 3;
      const uint32_t byte = byteIndex < m_bytes.size ? m_bytes.data[byteIndex] : 0;
      value |= ((byte >> (m_pos & 7)) & 1u) << i;
      ++m_pos;
    }
    return value;
  }

  bool Overrun() const { return m_pos > uint64_t(m_bytes.size) * 8 + 64; }

private:
  Bytes m_bytes;
  uint64_t m_pos;
};

int64_t SignExtend(int64_t v, int bits) {
  if (bits <= 0) {
    return 0;
  }
  return (v & (int64_t(1) << (bits - 1))) ? v - (int64_t(1) << bits) : v;
}

using Quat = std::array<double, 4>;  // w x y z while decoding

Quat Normalize(const Quat& q) {
  double n = 0.0;
  for (double c : q) {
    n += c * c;
  }
  const double r = n > 0.0 ? 1.0 / std::sqrt(n) : 1.0;
  return {q[0] * r, q[1] * r, q[2] * r, q[3] * r};
}

Quat Lerp(const Quat& a, const Quat& b, double t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t,
          a[3] + (b[3] - a[3]) * t};
}

// anim_rot_lerp2 from the exe: a two-segment lerp through the normalized midpoint,
// taking the short way round.
Quat RotLerp2(const Quat& a, Quat b, double t) {
  double dot = 0.0;
  for (int i = 0; i < 4; ++i) {
    dot += a[size_t(i)] * b[size_t(i)];
  }
  if (dot < -0.001953125) {
    for (double& c : b) {
      c = -c;
    }
  }
  const Quat mid =
      Normalize({(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5, (a[3] + b[3]) * 0.5});
  Quat r;
  if (t < 0.5) {
    r = Lerp(a, mid, 2 * t);
  } else if (t > 0.5) {
    r = Lerp(mid, b, 2 * (t - 0.5));
  } else {
    r = mid;
  }
  return Normalize(r);
}

// One value of a track (a rotation, or for the vec3 track types a translation or scale):
// how to dequantize it and the two keyframes it is currently interpolating between.
struct Value {
  bool vec = false;  // a vec3 value: no flag7 or sign bits, one to three components
  int W = 0;  // bits per component
  int q = 0;  // fixed-point shift
  int isSigned = 0;
  int mask = 0;  // bit i set: component i (x, y, z) is stored
  int flag7 = 0;  // interpolate with RotLerp2 instead of lerp + normalize
  int sgnPrev = 0, sgnCur = 0;  // w is negative
  int64_t prev[3] = {0, 0, 0};
  int64_t cur[3] = {0, 0, 0};
  int64_t curFrame = 0, nextFrame = 0;
  double inv = 0.0;
};

struct Track {
  int type = 0;
  int flag = 0;
  std::vector<Value> values;
};

struct StreamInfo {
  Bytes blob;
  uint32_t offInit = 0;
  uint32_t w1 = 0;  // byte offset of the data bit stream
  uint32_t w2 = 0;  // byte offset of the op stream
  float headerFloat = 0.0f;
  uint32_t frames = 0;
  uint32_t v = 0;  // quantization steps per frame
  uint32_t u6 = 0;  // bits flagging a keyframe change per frame
  std::vector<Track> tracks;
};

// Blob layout: 24 byte header, header bits, init-dequant bit stream, data bit
// stream, op stream (see NOTES-anim.md).
StreamInfo ParseBlob(const Bytes& d, size_t base) {
  d.Need(base, 24, "animation header");
  const uint32_t size = ReadLE32(d.data + base);
  if (size < 24 || !d.Has(base, size)) {
    Fail("animation blob runs past the end of the file");
  }
  StreamInfo info;
  info.blob = Bytes{d.data + base, size};
  const uint16_t offBits = ReadLE16(d.data + base + 4);
  info.offInit = ReadLE16(d.data + base + 6);
  info.headerFloat = ReadLEFloat(d.data + base + 12);
  info.frames = ReadLE16(d.data + base + 16);

  BitStream bs(info.blob, uint64_t(offBits) * 8);
  const uint32_t streamInfos = bs.Read(4);
  bs.Read(10);  // total tracks
  bs.Read(4);
  if (streamInfos != 1) {
    Fail(std::to_string(streamInfos) + " stream infos are not supported");
  }
  info.w1 = bs.Read(20);
  info.w2 = bs.Read(20);
  const uint32_t trackCount = bs.Read(4);
  bs.Read(1);
  bs.Read(1);
  info.v = bs.Read(4);
  info.u6 = bs.Read(4);
  bs.Read(4);
  for (uint32_t i = 0; i < trackCount; ++i) {
    const uint32_t count = bs.Read(10);
    const uint32_t type = bs.Read(3);
    const uint32_t flag = bs.Read(1);
    if (type == 3) {
      bs.Read(6);
    }
    if (type > 2) {
      Fail("track type " + std::to_string(type) + " is not supported");
    }
    Track t;
    t.type = int(type);
    t.flag = int(flag);
    t.values.resize(count);
    info.tracks.push_back(std::move(t));
  }
  if (bs.Overrun()) {
    Fail("animation header bits run past the blob");
  }
  return info;
}

void InitDequant(StreamInfo& info) {
  BitStream bs(info.blob, uint64_t(info.offInit) * 8);
  for (;;) {
    if (bs.Overrun()) {
      Fail("init-dequant stream has no end marker");
    }
    const uint32_t op = bs.Read(4);
    if (op == 0) {
      bs.Read(4);
    } else if (op == 1) {
      break;
    } else if (op == 2 || op == 3) {  // 2 = rotation values, 3 = vec3 values; same layout
      const uint32_t index = bs.Read(4);
      if (index >= info.tracks.size()) {
        Fail("init-dequant names a track that does not exist");
      }
      Track& track = info.tracks[index];
      for (Value& val : track.values) {
        val.W = int(bs.Read(5));
        val.q = int(bs.Read(5));
        val.isSigned = int(bs.Read(1));
        val.mask = int(bs.Read(3));
        val.vec = track.type != 0;
        // The game's do-while loads one component even for mask 0 (into x).
        if (val.vec && val.mask == 0) {
          val.mask = 1;
        }
      }
    } else {
      Fail("init-dequant op " + std::to_string(op) + " is not supported");
    }
  }
}

// CAnimCompStreamInst: walks the data bit stream one frame at a time.
class Stream {
public:
  explicit Stream(StreamInfo& info)
      : m_info(info), m_bs(info.blob, uint64_t(info.w1) * 8) {}

  int64_t Cur() const { return m_cur; }

  void InitFrames() {
    m_cur = 0;
    for (Track& tr : m_info.tracks) {
      for (Value& val : tr.values) {
        for (int i = 0; i < 3; ++i) {
          val.cur[i] = 0;
          val.prev[i] = 0;
        }
        val.curFrame = 0;
        val.nextFrame = 0;
        val.inv = 0.0;
        val.sgnCur = val.sgnPrev = 0;
      }
    }
    Load(SetupNext());
    Load(SetupStepped(0));
  }

  void DequantFrame() {
    ++m_cur;
    if (m_bs.Read(int(m_info.u6))) {
      if (m_cur == (int64_t(m_info.frames) - 1) * int64_t(m_info.v)) {
        Load(SetupStepped(1));
      } else {
        Load(SetupNext());
      }
    }
  }

private:
  std::vector<Value*> SetupNext() {
    const int width = int(m_bs.Read(4));
    std::vector<Value*> list;
    for (Track& tr : m_info.tracks) {
      for (Value& val : tr.values) {
        if (val.nextFrame == m_cur) {
          const int64_t delta = m_bs.Read(width);
          val.curFrame = val.nextFrame;
          val.nextFrame += delta;
          val.inv = delta ? F32(1.0 / double(delta)) : 0.0;
          list.push_back(&val);
        }
      }
    }
    return list;
  }

  std::vector<Value*> SetupStepped(int flag) {
    std::vector<Value*> list;
    for (Track& tr : m_info.tracks) {
      if (tr.flag == flag) {
        for (Value& val : tr.values) {
          list.push_back(&val);
        }
      }
    }
    return list;
  }

  void Load(const std::vector<Value*>& list) {
    for (Value* val : list) {
      for (int i = 0; i < 3; ++i) {
        val->prev[i] = val->cur[i];
        val->cur[i] = 0;
      }
      val->sgnPrev = val->sgnCur;
      if (!val->vec) {
        val->flag7 = int(m_bs.Read(1));
        val->sgnCur = int(m_bs.Read(1));
      }
      for (int c = 0; c < 3; ++c) {
        if (val->mask & (1 << c)) {
          int64_t x = m_bs.Read(val->W);
          if (val->isSigned) {
            x = SignExtend(x, val->W);
          }
          val->cur[c] = x;
        }
      }
    }
  }

  StreamInfo& m_info;
  BitStream m_bs;
  int64_t m_cur = 0;
};

// The rotation of one value at frame time t, as (w, x, y, z).
Quat TweenRot(const Value& val, int64_t t) {
  auto make = [&](const int64_t* ints, int negative) {
    const double scale = std::ldexp(1.0, -val.q);
    double c[3];
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
      c[i] = F32(double(ints[i]) * scale);
      sum += c[i] * c[i];
    }
    const double w = std::sqrt(std::max(0.0, 1.0 - sum));
    return Quat{negative ? -w : w, c[0], c[1], c[2]};
  };
  const Quat prev = make(val.prev, val.sgnPrev);
  if (t == val.curFrame) {
    return prev;
  }
  const Quat cur = make(val.cur, val.sgnCur);
  const double frac = double(t - val.curFrame) * val.inv;
  if (val.flag7) {
    return RotLerp2(prev, cur, frac);
  }
  return Normalize(Lerp(prev, cur, frac));
}

// A vec3 value at frame time t: linear between the two keyframes.
std::array<double, 3> TweenVec3(const Value& val, int64_t t) {
  const double scale = std::ldexp(1.0, -val.q);
  std::array<double, 3> prev, cur;
  for (size_t i = 0; i < 3; ++i) {
    prev[i] = F32(double(val.prev[i]) * scale);
    cur[i] = F32(double(val.cur[i]) * scale);
  }
  if (t == val.curFrame) {
    return prev;
  }
  const double frac = double(t - val.curFrame) * val.inv;
  return {prev[0] + (cur[0] - prev[0]) * frac, prev[1] + (cur[1] - prev[1]) * frac,
          prev[2] + (cur[2] - prev[2]) * frac};
}

// One entry of the op stream: which bone, and where its rotation, scale and
// translation come from (a track value, or an index into the constant pool).
struct Op {
  uint32_t bone;
  uint16_t rot, scale, trans;
};

std::vector<Op> ParseOps(const StreamInfo& info) {
  const Bytes& b = info.blob;
  size_t p = info.w2;
  std::vector<Op> out;
  auto byteAt = [&](size_t at) -> uint8_t {
    b.Need(at, 1, "op stream");
    return b.data[at];
  };
  // The list may end at the end of the blob without a terminator.
  while (p < b.size) {
    const uint8_t op = b.data[p++];
    if (op != 1) {
      break;  // 0 ends the list; the static animation ends in padding instead
    }
    p += 2;
    while (p < b.size) {
      const uint8_t sub = b.data[p++];
      if (sub == 0) {
        break;
      }
      if (sub != 2) {
        Fail("op stream sub-op " + std::to_string(sub) + " is not supported");
      }
      const uint32_t count = byteAt(p);
      b.Need(p + 1, 2, "op stream");
      const uint32_t hdr = ReadLE16(b.data + p + 1);
      p += 3;
      const uint32_t bone = (hdr >> 5) & 0x7ff;
      for (uint32_t k = 0; k < count; ++k) {
        b.Need(p, 6, "op stream");
        out.push_back({bone + k, ReadLE16(b.data + p), ReadLE16(b.data + p + 2), ReadLE16(b.data + p + 4)});
        p += 6;
      }
    }
  }
  return out;
}

// The pooled names: i32 size at 0x25, then NUL separated strings.
std::vector<std::string> ReadNames(const Bytes& d) {
  d.Need(0x25, 4, "name pool");
  const int32_t n = int32_t(ReadLE32(d.data + 0x25));
  if (n < 0 || !d.Has(0x29, size_t(n))) {
    Fail("name pool runs past the end of the file");
  }
  std::vector<std::string> names;
  std::string cur;
  for (int32_t i = 0; i < n; ++i) {
    const char c = char(d.data[0x29 + size_t(i)]);
    if (c == 0) {
      if (!cur.empty()) {
        names.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    names.push_back(cur);
  }
  return names;
}

struct Record {
  size_t offset;
  uint32_t id;
  size_t end;
};

// Compressed animation records: 01 <id u32> <len u32> payload, the next record 12 + len on.
std::vector<Record> ReadRecords(const Bytes& d, size_t start) {
  std::vector<Record> recs;
  size_t o = start;
  while (d.Has(o, 9) && d.data[o] == 1) {
    const uint32_t id = ReadLE32(d.data + o + 1);
    const uint32_t len = ReadLE32(d.data + o + 5);
    if (id > 0xff || len == 0 || !d.Has(o, size_t(12) + len)) {
      break;
    }
    recs.push_back({o, id, o + 12 + len});
    o += 12 + len;
  }
  return recs;
}

// The longest chain of at least two records found by trying every start.
std::vector<Record> FindRecords(const Bytes& d) {
  std::vector<Record> best;
  for (size_t o = 0x100; d.Has(o, 9); ++o) {
    if (d.data[o] == 1 && d.data[o + 2] == 0 && d.data[o + 3] == 0 && d.data[o + 4] == 0) {
      std::vector<Record> r = ReadRecords(d, o);
      if (r.size() >= 2 && r.size() > best.size()) {
        best = std::move(r);
      }
    }
  }
  if (!best.empty()) {
    return best;
  }
  // A file with one animation has no chain: look for `01 id 00 00 00 <size> 18 00 28 00`,
  // the record marker followed by the blob header.
  for (size_t o = 0; d.Has(o, 13); ++o) {
    if (d.data[o] == 1 && d.data[o + 2] == 0 && d.data[o + 3] == 0 && d.data[o + 4] == 0 &&
        ReadLE16(d.data + o + 9) == 0x18 && ReadLE16(d.data + o + 11) == 0x28) {
      const uint32_t len = ReadLE32(d.data + o + 5);
      if (len != 0 && d.Has(o, size_t(12) + len)) {
        best.push_back({o, d.data[o + 1], o + 12 + len});
        o += 12 + len - 1;  // the regex scan does not overlap matches
      }
    }
  }
  return best;
}

// The constant pool: u16 0, u16 n, n floats starting 1 0 0 0, then under 128 bytes of
// other data and the first record. The candidate that ends closest to the record wins.
std::vector<double> FindPool(const Bytes& d, size_t first) {
  bool found = false;
  size_t bestEnd = 0;
  size_t bestOffset = 0;
  uint32_t bestCount = 0;
  const size_t from = first >= 0x20000 ? first - 0x20000 : 0;
  for (size_t o = from; o + 20 < first; ++o) {
    const uint32_t a = ReadLE16(d.data + o);
    const uint32_t n = ReadLE16(d.data + o + 2);
    const size_t end = o + 4 + 4 * size_t(n);
    if (a >= 16 || n < 4 || end > first || first - end >= 128 || (found && end <= bestEnd)) {
      continue;
    }
    if (ReadLEFloat(d.data + o + 4) != 1.0f || ReadLEFloat(d.data + o + 8) != 0.0f ||
        ReadLEFloat(d.data + o + 12) != 0.0f || ReadLEFloat(d.data + o + 16) != 0.0f) {
      continue;
    }
    bool ok = true;
    for (uint32_t i = 0; i < n; ++i) {
      if (!(std::fabs(ReadLEFloat(d.data + o + 4 + 4 * size_t(i))) < 1e6f)) {
        ok = false;
        break;
      }
    }
    if (ok) {
      found = true;
      bestEnd = end;
      bestOffset = o;
      bestCount = n;
    }
  }
  std::vector<double> pool;
  if (!found) {
    return {1.0, 0.0, 0.0, 0.0};
  }
  for (uint32_t i = 0; i < bestCount; ++i) {
    pool.push_back(double(ReadLEFloat(d.data + bestOffset + 4 + 4 * size_t(i))));
  }
  return pool;
}

void Evaluate(const StreamInfo& info, const std::vector<Op>& ops, const std::vector<double>& pool,
              int64_t t, std::vector<Key>& frameKeys, std::vector<bool>& has) {
  auto poolAt = [&](size_t index, size_t count) -> const double* {
    if (index > pool.size() || count > pool.size() - index) {
      Fail("constant pool index out of range");
    }
    return pool.data() + index;
  };
  for (const Op& op : ops) {
    Quat q;  // w x y z
    if (op.rot & 0x8000) {
      const size_t track = (op.rot >> 10) & 0xf;
      const size_t index = op.rot & 0x3ff;
      if (track >= info.tracks.size() || index >= info.tracks[track].values.size()) {
        Fail("rotation names a track value that does not exist");
      }
      q = TweenRot(info.tracks[track].values[index], t);
    } else {
      const double* p = poolAt(op.rot & 0x3fff, 4);
      q = {p[0], p[1], p[2], p[3]};
    }
    auto vecTrack = [&](uint16_t ref) -> std::array<double, 3> {
      const size_t track = (ref >> 10) & 0xf;
      const size_t index = ref & 0x3ff;
      if (track >= info.tracks.size() || index >= info.tracks[track].values.size()) {
        Fail("a vec3 reference names a track value that does not exist");
      }
      return TweenVec3(info.tracks[track].values[index], t);
    };
    double sc[3];
    if (op.scale & 0x8000) {
      if (op.scale & 0x4000) {
        Fail("single-real scale tracks are not supported");
      }
      const std::array<double, 3> v = vecTrack(op.scale);
      sc[0] = v[0];
      sc[1] = v[1];
      sc[2] = v[2];
    } else if (op.scale & 0x4000) {
      sc[0] = sc[1] = sc[2] = *poolAt(op.scale & 0x3fff, 1);
    } else {
      const double* p = poolAt(op.scale & 0x3fff, 3);
      sc[0] = p[0];
      sc[1] = p[1];
      sc[2] = p[2];
    }
    std::array<double, 3> tr;
    if (op.trans & 0x8000) {
      tr = vecTrack(op.trans);
    } else {
      const double* p = poolAt(op.trans & 0x3fff, 3);
      tr = {p[0], p[1], p[2]};
    }
    Key key;
    key.rotation[0] = float(q[1]);
    key.rotation[1] = float(q[2]);
    key.rotation[2] = float(q[3]);
    key.rotation[3] = float(q[0]);
    for (int i = 0; i < 3; ++i) {
      key.scale[i] = float(F32(sc[i] + 1.0));  // stored as a delta from one
      key.translation[i] = float(tr[size_t(i)]);
    }
    frameKeys[op.bone] = key;
    has[op.bone] = true;
  }
}

Anim DecodeAnim(const Bytes& d, const Record& rec, const std::vector<double>& pool) {
  StreamInfo info = ParseBlob(d, rec.offset + 5);
  InitDequant(info);
  Stream stream(info);
  const std::vector<Op> ops = ParseOps(info);
  stream.InitFrames();

  Anim anim;
  anim.id = rec.id;
  anim.fps = kFramesPerRate * info.headerFloat;
  anim.frames = info.frames;
  uint32_t boneCount = 0;
  for (const Op& op : ops) {
    boneCount = std::max(boneCount, op.bone + 1);
  }
  // A damaged header can claim 65535 frames of 2000 bones; real files have well under this.
  if (uint64_t(boneCount) * info.frames > (uint64_t(1) << 23)) {
    Fail("animation is implausibly large");
  }
  anim.bones.assign(boneCount, std::vector<Key>());
  for (std::vector<Key>& keys : anim.bones) {
    keys.reserve(info.frames);
  }
  anim.tracked.assign(boneCount, false);
  for (const Op& op : ops) {
    anim.tracked[op.bone] = true;
  }
  std::vector<Key> frameKeys(boneCount);
  std::vector<bool> has(boneCount);
  for (uint32_t f = 0; f < info.frames; ++f) {
    const int64_t t = int64_t(f) * int64_t(info.v);
    while (stream.Cur() < t) {
      stream.DequantFrame();
    }
    std::fill(has.begin(), has.end(), false);
    Evaluate(info, ops, pool, t, frameKeys, has);
    for (uint32_t b = 0; b < boneCount; ++b) {
      anim.bones[b].push_back(has[b] ? frameKeys[b] : Key());
    }
  }
  return anim;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool ContainsNoCase(const std::string& s, const char* needle) {
  std::string lower = s;
  for (char& c : lower) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find(needle) != std::string::npos;
}

// 3x4 affine matrices, row-major with the translation in column 3 (Remastered's layout).
using Mat = std::array<double, 12>;

Mat MatMul(const Mat& a, const Mat& b) {
  Mat r;
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      double v = 0.0;
      for (size_t k = 0; k < 3; ++k) {
        v += a[i * 4 + k] * b[k * 4 + j];
      }
      if (j == 3) {
        v += a[i * 4 + 3];
      }
      r[i * 4 + j] = v;
    }
  }
  return r;
}

// Inverse of an affine matrix; false when the linear part is singular.
bool MatInverse(const Mat& m, Mat& out) {
  const double a[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                     a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::fabs(det) < 1e-12) {
    return false;
  }
  double inv[3][3];
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      const size_t r0 = i == 0 ? 1 : 0, r1 = i == 2 ? 1 : 2;
      const size_t q0 = j == 0 ? 1 : 0, q1 = j == 2 ? 1 : 2;
      const double cof = (a[r0][q0] * a[r1][q1] - a[r0][q1] * a[r1][q0]) * (((i + j) & 1) ? -1.0 : 1.0);
      inv[j][i] = cof / det;
    }
  }
  const double t[3] = {m[3], m[7], m[11]};
  for (size_t i = 0; i < 3; ++i) {
    out[i * 4 + 0] = inv[i][0];
    out[i * 4 + 1] = inv[i][1];
    out[i * 4 + 2] = inv[i][2];
    out[i * 4 + 3] = -(inv[i][0] * t[0] + inv[i][1] * t[1] + inv[i][2] * t[2]);
  }
  return true;
}

// A Key as a matrix: rotation times scale (columns scaled), then translation.
Mat KeyMatrix(const Key& k) {
  const double x = k.rotation[0], y = k.rotation[1], z = k.rotation[2], w = k.rotation[3];
  const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                          {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                          {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
  Mat m;
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      m[i * 4 + j] = r[i][j] * double(k.scale[j]);
    }
    m[i * 4 + 3] = double(k.translation[i]);
  }
  return m;
}

// The rotation (x y z w), scale (column lengths) and translation of an affine matrix.
Key MatrixKey(const Mat& m) {
  Key key;
  double col[3];
  for (size_t j = 0; j < 3; ++j) {
    col[j] = std::sqrt(m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j]);
    key.scale[j] = float(col[j]);
    if (col[j] == 0.0) {
      col[j] = 1.0;
    }
  }
  double r[3][3];
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      r[i][j] = m[i * 4 + j] / col[j];
    }
  }
  double q[4];  // x y z w
  const double tr = r[0][0] + r[1][1] + r[2][2];
  if (tr > 0) {
    const double s = std::sqrt(tr + 1) * 2;
    q[0] = (r[2][1] - r[1][2]) / s;
    q[1] = (r[0][2] - r[2][0]) / s;
    q[2] = (r[1][0] - r[0][1]) / s;
    q[3] = s / 4;
  } else {
    size_t i = 0;
    for (size_t k = 1; k < 3; ++k) {
      if (r[k][k] > r[i][i]) {
        i = k;
      }
    }
    const size_t j = (i + 1) % 3, k = (i + 2) % 3;
    const double s = std::sqrt(std::max(0.0, 1 + r[i][i] - r[j][j] - r[k][k])) * 2;
    q[i] = s / 4;
    q[j] = (r[j][i] + r[i][j]) / s;
    q[k] = (r[k][i] + r[i][k]) / s;
    q[3] = (r[k][j] - r[j][k]) / s;
  }
  for (size_t i = 0; i < 4; ++i) {
    key.rotation[i] = float(q[i]);
  }
  key.translation[0] = float(m[3]);
  key.translation[1] = float(m[7]);
  key.translation[2] = float(m[11]);
  return key;
}

// The skeleton and skin palette (NOTES-skel.md): the CCharInfo stream read sequentially.
// All offsets are checked, so a short or damaged file fails with a message.
void ReadSkeleton(const Bytes& d, Character& out) {
  size_t p = 0x29;
  auto u8 = [&](size_t at) -> uint32_t {
    d.Need(at, 1, "skeleton");
    return d.data[at];
  };
  auto u16 = [&](size_t at) -> uint32_t {
    d.Need(at, 2, "skeleton");
    return ReadLE16(d.data + at);
  };
  auto u32 = [&](size_t at) -> uint32_t {
    d.Need(at, 4, "skeleton");
    return ReadLE32(d.data + at);
  };

  // The string pool, keeping empty strings: refs index it.
  d.Need(0x25, 4, "name pool");
  const int32_t poolSize = int32_t(ReadLE32(d.data + 0x25));
  if (poolSize < 0 || !d.Has(0x29, size_t(poolSize))) {
    Fail("name pool runs past the end of the file");
  }
  std::vector<std::string> strs(1);
  for (int32_t i = 0; i < poolSize; ++i) {
    const char c = char(d.data[0x29 + size_t(i)]);
    if (c == 0) {
      strs.emplace_back();
    } else {
      strs.back().push_back(c);
    }
  }
  if (strs.back().empty()) {
    strs.pop_back();
  }
  p = 0x29 + size_t(poolSize);

  const size_t tables = u16(p), sets = u16(p + 2);
  p += 4 + 4 * tables + 4 * sets;
  p += 4 * tables;
  std::vector<std::vector<uint32_t>> nameRefs;  // per name set, the refs
  for (size_t i = 0; i < sets; ++i) {
    const size_t count = u32(p);
    if (count > d.size) {
      Fail("name set runs past the end of the file");
    }
    d.Need(p + 4, 8 * count, "name set");
    std::vector<uint32_t> refs(count);
    for (size_t k = 0; k < count; ++k) {
      refs[k] = ReadLE32(d.data + p + 4 + 4 * count + 4 * k);
    }
    nameRefs.push_back(std::move(refs));
    p += 4 + 8 * count;
  }
  p += 2 + u8(p) * 2;

  // SAnimContext
  {
    const size_t a = u8(p), b = u8(p + 1), c = u16(p + 2), dd = u16(p + 4), e = u16(p + 6);
    p += 8 + 6 * a + c + 2 * b + (dd > 8 * b ? dd - 8 * b : 0) + 2 * a + (e > 8 * a ? e - 8 * a : 0);
  }
  const size_t animCount = u16(p), animSet = u16(p + 2);
  p += 4 + 4 * animCount;

  // SAbsContext
  const size_t absA = u8(p), absB = u16(p + 1), absC = u16(p + 3), absD = u16(p + 5), absE = u16(p + 7);
  p += 9 + 6 * absA;
  d.Need(p, absC, "skeleton parents");
  const size_t cblob = p;
  p += absC + absD + absE;
  const size_t boneCount = u16(p), absSet = u16(p + 2);
  p += 4 + 4 * boneCount + 4 * absB + 1;

  // SRenderContext
  const size_t ra = u16(p), rb = u16(p + 2), rc = u16(p + 4), rd = u16(p + 6);
  p += 12;
  std::vector<uint32_t> arr0(ra);
  d.Need(p, 2 * (ra + rb + rc + rd), "render context");
  for (size_t k = 0; k < ra; ++k) {
    arr0[k] = ReadLE16(d.data + p + 2 * k);
  }
  p += 2 * (ra + rb + rc + rd);
  d.Need(p, 48 * (ra + 1 + rd), "inverse binds");
  auto matAt = [&](size_t index) {
    Mat m;
    for (size_t i = 0; i < 12; ++i) {
      m[i] = double(ReadLEFloat(d.data + p + 48 * index + 4 * i));
    }
    return m;
  };

  if (absSet >= nameRefs.size() || animSet >= nameRefs.size() || nameRefs[absSet].size() < boneCount) {
    Fail("skeleton names are missing");
  }
  auto name = [&](uint32_t ref) { return (ref & 0x0fffffff) < strs.size() ? strs[ref & 0x0fffffff] : std::string(); };

  std::vector<int> parent(boneCount, -1);
  for (size_t o = 0; o + 6 < absC;) {
    const uint8_t* r = d.data + cblob + o;
    if ((r[0] == 4 || r[0] == 5) && o + 7 <= absC) {
      const uint32_t self = ReadLE16(r + 1), par = ReadLE16(r + 3);
      if (self % 6 == 0 && par % 6 == 0 && self / 6 < boneCount && par / 6 < boneCount) {
        parent[self / 6] = int(par / 6);
        o += 7;
        continue;
      }
    }
    ++o;
  }

  std::vector<bool> hasWorld(boneCount, false);
  std::vector<Mat> world(boneCount);
  for (size_t i = 0; i < boneCount && i < rd; ++i) {
    hasWorld[i] = MatInverse(matAt(ra + 1 + i), world[i]);
  }
  out.bones.clear();
  std::vector<std::string> boneNames;
  for (size_t i = 0; i < boneCount; ++i) {
    Bone bone;
    bone.name = name(nameRefs[absSet][i]);
    bone.parent = parent[i];
    if (hasWorld[i]) {
      Mat loc = world[i];
      Mat parentInv;
      if (parent[i] >= 0 && hasWorld[size_t(parent[i])] && MatInverse(world[size_t(parent[i])], parentInv)) {
        loc = MatMul(parentInv, world[i]);
      }
      bone.bind = MatrixKey(loc);
    }
    boneNames.push_back(bone.name);
    out.bones.push_back(std::move(bone));
  }
  // Duplicate names: the last bone with the name wins, as in the Python.
  out.animBone.clear();
  for (uint32_t ref : nameRefs[animSet]) {
    int found = -1;
    const std::string n = name(ref);
    for (size_t i = 0; i < boneCount; ++i) {
      if (boneNames[i] == n) {
        found = int(i);
      }
    }
    out.animBone.push_back(found);
  }
  out.jointBone.clear();
  out.inverseBind.clear();
  for (size_t k = 0; k < ra; ++k) {
    const uint32_t bone = arr0[k] / 192;
    out.jointBone.push_back(bone < boneCount ? int(bone) : -1);
    std::array<float, 12> inv;
    const Mat m = matAt(k);
    for (size_t i = 0; i < 12; ++i) {
      inv[i] = float(m[i]);
    }
    out.inverseBind.push_back(inv);
  }
}

}  // namespace

bool ReadCharacter(const std::vector<uint8_t>& chpr, Character& out, std::string& error) {
  out = Character();
  try {
    const Bytes d{chpr.data(), chpr.size()};
    if (!d.Has(0, 0x29) || std::memcmp(d.data, "RFRM", 4) != 0 || std::memcmp(d.data + 0x14, "CHPR", 4) != 0) {
      Fail("not a CHPR file");
    }
    // Animation names, in record order: the pool names after the last "Primary" minus the
    // "meta" ones (a meta animation has no record), else the names with the usual
    // prefixes. Unless there is one per record, which is which is not known: the
    // animations go unnamed, so Find matches none of them.
    const std::vector<std::string> allNames = ReadNames(d);
    std::vector<std::string> animNames;
    for (const std::string& n : allNames) {
      if (StartsWith(n, "spin_") || StartsWith(n, "static") || StartsWith(n, "idle") || StartsWith(n, "anim")) {
        animNames.push_back(n);
      }
    }
    std::vector<std::string> afterPrimary;
    bool primary = false;
    for (const std::string& n : allNames) {
      if (n == "Primary") {
        primary = true;
        afterPrimary.clear();
      } else if (primary && !ContainsNoCase(n, "meta")) {
        afterPrimary.push_back(n);
      }
    }
    const std::vector<Record> recs = FindRecords(d);
    if (recs.empty()) {
      Fail("no animation records found");
    }
    if (primary && afterPrimary.size() == recs.size()) {
      animNames = afterPrimary;
    }
    const std::vector<double> pool = FindPool(d, recs[0].offset);
    for (size_t i = 0; i < recs.size(); ++i) {
      Anim anim = DecodeAnim(d, recs[i], pool);
      anim.name = animNames.size() == recs.size() ? animNames[i] : std::string();
      out.anims.push_back(std::move(anim));
    }
    // After the last record come a few fixed fields, a 16 byte reference (not the
    // skinned model) and then the SMDL reference, 0x22 bytes past the last record.
    const size_t smdl = recs.back().end + 0x22;
    if (d.Has(smdl, 16)) {
      std::copy(d.data + smdl, d.data + smdl + 16, out.skinnedModel.begin());
    }
    // The skeleton is optional: the animations are usable without it.
    try {
      ReadSkeleton(d, out);
    } catch (const Failure& failure) {
      out.bones.clear();
      out.animBone.clear();
      out.jointBone.clear();
      out.inverseBind.clear();
      out.skeletonError = failure.message;
    }
  } catch (const Failure& failure) {
    error = failure.message;
    out = Character();
    return false;
  }
  return true;
}

const Anim* Find(const Character& c, std::string_view name) {
  for (const Anim& a : c.anims) {
    if (!a.name.empty() && a.name == name) {
      return &a;
    }
  }
  return nullptr;
}

bool SkinPose(const Character& c, const Anim& anim, uint32_t frame, std::vector<std::array<float, 12>>& out) {
  out.clear();
  const size_t boneCount = c.bones.size();
  if (boneCount == 0 || frame >= anim.frames || c.jointBone.size() != c.inverseBind.size()) {
    return false;
  }
  // The anim track (op bone id) that drives each skeleton bone.
  std::vector<int> track(boneCount, -1);
  for (size_t j = c.animBone.size(); j-- > 0;) {
    const int b = c.animBone[j];
    if (b >= 0 && size_t(b) < boneCount && j < anim.bones.size() && j < anim.tracked.size() && anim.tracked[j] &&
        frame < anim.bones[j].size()) {
      track[size_t(b)] = int(j);
    }
  }
  std::vector<Mat> world(boneCount);
  std::vector<uint8_t> state(boneCount, 0);  // 0 not done, 1 in progress, 2 done
  // Parents can have a larger index than their children, so resolve by walking up.
  std::vector<size_t> chain;
  for (size_t start = 0; start < boneCount; ++start) {
    chain.clear();
    for (int b = int(start); b >= 0 && state[size_t(b)] == 0; b = c.bones[size_t(b)].parent) {
      state[size_t(b)] = 1;
      chain.push_back(size_t(b));
    }
    for (size_t i = chain.size(); i-- > 0;) {
      const size_t b = chain[i];
      // The keys are relative to the bind pose: a track's frame-0 key is the identity
      // where the animation starts from the bind pose (checked on the debris and the
      // Omega Pirate cinematic, whose joints then land on their bind places exactly). The
      // scale is not: a key's is the bone's own (the debris pieces bound at 0.5 and 0.75
      // keep that in their keys), so the bind's is left out under a key.
      Key unscaled = c.bones[b].bind;
      if (track[b] >= 0) {
        std::fill(std::begin(unscaled.scale), std::end(unscaled.scale), 1.0f);
      }
      const Mat bind = KeyMatrix(unscaled);
      const Mat local = track[b] >= 0 ? MatMul(bind, KeyMatrix(anim.bones[size_t(track[b])][frame])) : bind;
      const int par = c.bones[b].parent;
      if (par >= 0 && state[size_t(par)] == 2) {
        world[b] = MatMul(world[size_t(par)], local);
      } else if (par >= 0) {
        return false;  // a cycle
      } else {
        world[b] = local;
      }
      state[b] = 2;
    }
  }
  for (size_t k = 0; k < c.jointBone.size(); ++k) {
    const int b = c.jointBone[k];
    if (b < 0 || size_t(b) >= boneCount) {
      out.clear();
      return false;
    }
    Mat inv;
    for (size_t i = 0; i < 12; ++i) {
      inv[i] = double(c.inverseBind[k][i]);
    }
    const Mat m = MatMul(world[size_t(b)], inv);
    std::array<float, 12> row;
    for (size_t i = 0; i < 12; ++i) {
      row[i] = float(m[i]);
    }
    out.push_back(row);
  }
  return true;
}

}  // namespace PortRemasteredAnim
