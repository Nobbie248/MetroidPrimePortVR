#include "port_savestate.h"
#include "port_bytes.h"

#include <cstring>

// The slot file layout. Kept apart from port_savestate.cpp so the unit test
// can build it without the game.
namespace PortSaveState {
namespace {

constexpr char kMagic[4] = {'M', 'P', 'S', 'S'};
constexpr uint32_t kVersion = 1;
// Game-state blobs are a few hundred bytes; anything past this is not ours.
constexpr uint32_t kMaxBlob = 1u << 20;
constexpr uint32_t kMaxLabel = 256;

using port::AppendLE32;
using port::AppendLE64;
using port::AppendLEFloat;
void PutF64(std::string& out, double d) {
  uint64_t v;
  std::memcpy(&v, &d, 8);
  AppendLE64(out, v);
}
void PutString(std::string& out, const std::string& s) {
  const std::string cut = s.substr(0, kMaxLabel);
  AppendLE32(out, static_cast< uint32_t >(cut.size()));
  out += cut;
}

struct Reader {
  const std::string& data;
  size_t pos = 0;
  bool ok = true;

  bool Need(size_t n) {
    if (!ok || data.size() - pos < n)
      ok = false;
    return ok;
  }
  uint32_t U32() {
    if (!Need(4))
      return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast< uint32_t >(static_cast< unsigned char >(data[pos + i])) << (i * 8);
    pos += 4;
    return v;
  }
  uint64_t U64() {
    const uint64_t lo = U32();
    const uint64_t hi = U32();
    return lo | (hi << 32);
  }
  float F32() {
    const uint32_t v = U32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  double F64() {
    const uint64_t v = U64();
    double d;
    std::memcpy(&d, &v, 8);
    return d;
  }
  std::string String() {
    const uint32_t n = U32();
    if (n > kMaxLabel || !Need(n))
      return {};
    std::string s = data.substr(pos, n);
    pos += n;
    return s;
  }
};

} // namespace

std::string Encode(const Header& header, const std::vector< uint8_t >& blob) {
  std::string out(kMagic, 4);
  AppendLE32(out, kVersion);
  AppendLE32(out, header.worldId);
  AppendLE32(out, static_cast< uint32_t >(header.areaId));
  for (float f : header.position)
    AppendLEFloat(out, f);
  for (float f : header.forward)
    AppendLEFloat(out, f);
  AppendLE32(out, header.morphed ? 1 : 0);
  PutF64(out, header.playTime);
  AppendLE64(out, static_cast< uint64_t >(header.savedAt));
  PutString(out, header.world);
  PutString(out, header.room);
  AppendLE32(out, static_cast< uint32_t >(blob.size()));
  out.append(reinterpret_cast< const char* >(blob.data()), blob.size());
  return out;
}

bool Decode(const std::string& data, Header& header, std::vector< uint8_t >& blob) {
  if (data.size() < 8 || std::memcmp(data.data(), kMagic, 4) != 0)
    return false;
  Reader in{data, 4};
  if (in.U32() != kVersion)
    return false;
  Header h;
  h.worldId = in.U32();
  h.areaId = static_cast< int32_t >(in.U32());
  for (float& f : h.position)
    f = in.F32();
  for (float& f : h.forward)
    f = in.F32();
  h.morphed = in.U32() != 0;
  h.playTime = in.F64();
  h.savedAt = static_cast< int64_t >(in.U64());
  h.world = in.String();
  h.room = in.String();
  const uint32_t size = in.U32();
  if (!in.ok || size == 0 || size > kMaxBlob || !in.Need(size) || in.pos + size != data.size())
    return false;
  for (float f : h.position)
    if (!(f == f) || f > 1e7f || f < -1e7f)
      return false;
  blob.assign(data.begin() + in.pos, data.end());
  header = h;
  return true;
}

} // namespace PortSaveState
