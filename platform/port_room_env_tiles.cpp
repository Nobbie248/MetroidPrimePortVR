#include "port_room_env_tiles.h"

#include <algorithm>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace PortRoomEnvTiles {
namespace {

int32_t FloorDiv(int64_t a, int64_t b) { return int32_t(a >= 0 ? a / b : -((-a + b - 1) / b)); }

float Half(const uint8_t* p) {
  const uint32_t h = uint32_t(p[0]) | (uint32_t(p[1]) << 8);
  const int exponent = int((h >> 10) & 0x1F);
  const int mantissa = int(h & 0x3FF);
  if ((h & 0x8000) != 0 || (exponent == 31 && mantissa != 0)) {
    return 0.f;
  }
  if (exponent == 31) {
    return 65504.f;
  }
  return exponent == 0 ? std::ldexp(float(mantissa), -24) : std::ldexp(float(mantissa | 0x400), exponent - 25);
}

uint16_t ToHalf(float v) {
  // Round to nearest even; the input is a non-negative average of halves.
  if (!(v > 0.f)) {
    return 0;
  }
  if (v >= 65504.f) {
    return 0x7BFF;
  }
  int e;
  const float f = std::frexp(v, &e); // v = f * 2^e, f in [0.5, 1)
  int exp = e + 14;
  if (exp <= 0) {
    return uint16_t(std::lrintf(std::ldexp(v, 24)));
  }
  const long m = std::lrintf((f * 2.f - 1.f) * 1024.f);
  return uint16_t(((exp << 10) + m));
}

// Whether a layer-0 point (six bytes) is lit: its mean is non-zero. The importer floors a
// lit point's channels so a lit one never packs as zero.
bool PointLit(const uint8_t* mean) {
  for (int i = 0; i < 6; ++i) {
    if (mean[i] != 0) {
      return true;
    }
  }
  return false;
}

} // namespace

const Tile* TileSet::Find(const TileKey& key) const {
  auto it = std::lower_bound(tiles.begin(), tiles.end(), key, [](const Tile& t, const TileKey& k) { return t.key < k; });
  return it != tiles.end() && it->key == key ? &*it : nullptr;
}

bool TileSet::Lit(int64_t x, int64_t y, int64_t z) const {
  const TileKey key{0, FloorDiv(x, 64), FloorDiv(y, 64), FloorDiv(z, 16)};
  const Tile* t = Find(key);
  if (t == nullptr || t->size[0] == 0) {
    return false;
  }
  const int64_t lx = x - int64_t(key.x) * 64 - t->lo[0], ly = y - int64_t(key.y) * 64 - t->lo[1],
                lz = z - int64_t(key.z) * 16 - t->lo[2];
  if (lx < 0 || ly < 0 || lz < 0 || lx >= t->size[0] || ly >= t->size[1] || lz >= t->size[2]) {
    return false;
  }
  return PointLit(&data[t->offset + ((size_t(lz) * size_t(t->size[1]) + size_t(ly)) * size_t(t->size[0]) + size_t(lx)) * 6]);
}

float OwnAverage(const TileSet& own) {
  double sum = 0.0;
  size_t filled = 0;
  size_t points = 0;
  for (const Tile& t : own.tiles) {
    if (t.key.layer == 0) {
      points += size_t(t.size[0]) * size_t(t.size[1]) * size_t(t.size[2]);
    }
  }
  const size_t step = points / 32768 + 1; // a sample is enough, as in Parse
  size_t index = 0;
  for (const Tile& t : own.tiles) {
    if (t.key.layer != 0) {
      continue;
    }
    const size_t count = size_t(t.size[0]) * size_t(t.size[1]) * size_t(t.size[2]);
    for (size_t i = 0; i < count; ++i, ++index) {
      if (index % step != 0) {
        continue;
      }
      const uint8_t* p = &own.data[t.offset + i * 6];
      if (!PointLit(p)) {
        continue;
      }
      const double luminance = 0.2126 * Half(p) + 0.7152 * Half(p + 2) + 0.0722 * Half(p + 4);
      if (luminance > 0.0) {
        sum += std::log(luminance);
        ++filled;
      }
    }
  }
  return filled != 0 ? float(std::exp(sum / double(filled))) : 0.f;
}

// Before halving, the grid is held at full resolution: 2^25 points is 800 MB at 24 bytes, above any real room.
constexpr size_t kMaxComposedPoints = size_t(1) << 25;
// The blocks a tile set may cover per axis (a room spans tens).
constexpr int64_t kMaxBoxBlocks = 256;
constexpr int64_t kMaxBoxBlockCount = int64_t(1) << 20;

std::vector<TileKey> Region(const TileSet& own) {
  std::set<TileKey> keys;
  std::set<std::array<int32_t, 3>> blocks;
  for (const Tile& t : own.tiles) {
    keys.insert(t.key);
    // The blocks of layer 0 that tile covers (a 128 wide tile holds two).
    if (t.key.layer < 2) {
      blocks.insert({t.key.x, t.key.y, t.key.z});
    } else {
      blocks.insert({t.key.x * 2, t.key.y, t.key.z});
      blocks.insert({t.key.x * 2 + 1, t.key.y, t.key.z});
    }
  }
  const int32_t z0 = FloorDiv(own.box[2], 16), z1 = FloorDiv(own.box[5], 16);
  const int32_t y0 = FloorDiv(own.box[1], 64), y1 = FloorDiv(own.box[4], 64);
  const int32_t x0 = FloorDiv(own.box[0], 64), x1 = FloorDiv(own.box[3], 64);
  if (int64_t(z1) - z0 >= kMaxBoxBlocks || int64_t(y1) - y0 >= kMaxBoxBlocks || int64_t(x1) - x0 >= kMaxBoxBlocks ||
      (int64_t(z1) - z0 + 1) * (int64_t(y1) - y0 + 1) * (int64_t(x1) - x0 + 1) > kMaxBoxBlockCount) {
    return {keys.begin(), keys.end()}; // a box ParseTiles would have refused
  }
  for (int32_t z = z0; z <= z1; ++z) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        blocks.insert({x, y, z});
      }
    }
  }
  for (const auto& b : blocks) {
    for (int layer = 0; layer < kLayers; ++layer) {
      keys.insert({layer, layer < 2 ? b[0] : FloorDiv(b[0], 2), b[1], b[2]});
    }
  }
  return {keys.begin(), keys.end()};
}

namespace {

// Composes the points of `blocks` (layer-0 block coordinates) from the mapped tiles lookup
// finds, cropped to the lit points and one empty point around them. worldToPoint is the one
// matrix of the world's points; no axis may be longer than axisLimit.
bool ComposeBlocks(const float worldToPoint[12], const std::set<std::array<int32_t, 3>>& blocks, const Lookup& lookup,
                   size_t maxPoints, int64_t axisLimit, ResolvedGrid& out, std::string& note) {
  out = {};
  // The mapped copies, per key, looked up once.
  std::map<TileKey, std::pair<std::shared_ptr<const TileSet>, const Tile*>> mapped;
  const auto find = [&](const TileKey& key) -> const std::pair<std::shared_ptr<const TileSet>, const Tile*>& {
    auto it = mapped.find(key);
    if (it == mapped.end()) {
      std::shared_ptr<const TileSet> set = lookup(key);
      const Tile* tile = set != nullptr ? set->Find(key) : nullptr;
      it = mapped.emplace(key, std::make_pair(std::move(set), tile)).first;
    }
    return it->second;
  };

  // Where the lit points are, as the crop of the grid.
  int64_t c0[3] = {INT64_MAX, INT64_MAX, INT64_MAX}, c1[3] = {INT64_MIN, INT64_MIN, INT64_MIN};
  for (const auto& b : blocks) {
    const auto& [set, tile] = find({0, b[0], b[1], b[2]});
    if (tile == nullptr || tile->size[0] == 0) {
      continue;
    }
    const int64_t origin[3] = {int64_t(b[0]) * 64, int64_t(b[1]) * 64, int64_t(b[2]) * 16};
    for (int32_t z = 0; z < tile->size[2]; ++z) {
      for (int32_t y = 0; y < tile->size[1]; ++y) {
        for (int32_t x = 0; x < tile->size[0]; ++x) {
          const uint8_t* p = &set->data[tile->offset + ((size_t(z) * size_t(tile->size[1]) + size_t(y)) * size_t(tile->size[0]) + size_t(x)) * 6];
          if (!PointLit(p)) {
            continue;
          }
          const int64_t q[3] = {origin[0] + tile->lo[0] + x, origin[1] + tile->lo[1] + y, origin[2] + tile->lo[2] + z};
          for (int i = 0; i < 3; ++i) {
            c0[i] = std::min(c0[i], q[i] - 1);
            c1[i] = std::max(c1[i], q[i] + 2);
          }
        }
      }
    }
  }
  if (c1[0] <= c0[0] || c1[1] <= c0[1] || c1[2] <= c0[2]) {
    note = "grid empty";
    return false;
  }
  int64_t size[3] = {c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2]};
  if (size[0] > axisLimit || size[1] > axisLimit || size[2] > axisLimit) {
    note = "grid too large: " + std::to_string(size[0]) + "x" + std::to_string(size[1]) + "x" + std::to_string(size[2]) +
           " points, axis limit " + std::to_string(axisLimit) + "; baked lighting disabled for it";
    return false;
  }
  size_t points = size_t(size[0]) * size_t(size[1]) * size_t(size[2]);
  if (points > kMaxComposedPoints) {
    note = "grid too large: " + std::to_string(size[0]) + "x" + std::to_string(size[1]) + "x" + std::to_string(size[2]) +
           " points, limit " + std::to_string(kMaxComposedPoints) + "; baked lighting disabled for it";
    return false;
  }
  std::vector<uint8_t> pts(points * 24, 0);
  const auto pointAt = [&](int64_t x, int64_t y, int64_t z) {
    return &pts[((size_t(z - c0[2]) * size_t(size[1]) + size_t(y - c0[1])) * size_t(size[0]) + size_t(x - c0[0])) * 24];
  };
  for (const auto& b : blocks) {
    for (int layer = 0; layer < kLayers; ++layer) {
      const int width = TileWidth(layer);
      const TileKey key{layer, layer < 2 ? b[0] : FloorDiv(b[0], 2), b[1], b[2]};
      const auto& [set, tile] = find(key);
      if (tile == nullptr || tile->size[0] == 0) {
        continue;
      }
      const int bytes = TileBytes(layer);
      // This block's x range within the tile (a 128 wide tile holds two blocks).
      const int64_t blockX = int64_t(b[0]) * 64, tileX = int64_t(key.x) * width;
      const int64_t origin[3] = {tileX, int64_t(key.y) * 64, int64_t(key.z) * 16};
      for (int32_t z = 0; z < tile->size[2]; ++z) {
        for (int32_t y = 0; y < tile->size[1]; ++y) {
          for (int32_t x = 0; x < tile->size[0]; ++x) {
            const int64_t q[3] = {origin[0] + tile->lo[0] + x, origin[1] + tile->lo[1] + y, origin[2] + tile->lo[2] + z};
            if (q[0] < blockX || q[0] >= blockX + 64 || q[0] < c0[0] || q[0] >= c1[0] || q[1] < c0[1] || q[1] >= c1[1] ||
                q[2] < c0[2] || q[2] >= c1[2]) {
              continue;
            }
            const uint8_t* src = &set->data[tile->offset + ((size_t(z) * size_t(tile->size[1]) + size_t(y)) * size_t(tile->size[0]) + size_t(x)) * size_t(bytes)];
            uint8_t* dst = pointAt(q[0], q[1], q[2]);
            if (layer < 2) {
              std::memcpy(dst + layer * 6, src, 6);
            } else if (layer == 2) {
              std::memcpy(dst + 12, src, 3);
            } else {
              std::memcpy(dst + 15 + (layer - 3) * 3, src, 3);
            }
          }
        }
      }
    }
  }
  // A point whose mean is zero is no point, whatever the other layers hold there.
  size_t litCount = 0;
  for (size_t i = 0; i < points; ++i) {
    uint8_t* p = &pts[i * 24];
    if (PointLit(p)) {
      ++litCount;
    } else {
      std::memset(p, 0, 24);
    }
  }
  if (litCount == 0) {
    note = "grid empty";
    return false;
  }

  double m[3][4];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) {
      m[i][j] = worldToPoint[i * 4 + j];
    }
    m[i][3] -= double(c0[i]);
  }
  // Android only: the largest rooms have millions of points and ambient light varies slowly,
  // so those are kept at half the resolution, each point the average of its lit ones.
  while (maxPoints != 0 && points > maxPoints) {
    const int64_t half[3] = {(size[0] + 1) / 2, (size[1] + 1) / 2, (size[2] + 1) / 2};
    const size_t fewer = size_t(half[0]) * size_t(half[1]) * size_t(half[2]);
    std::vector<uint8_t> coarse(fewer * 24, 0);
    for (int64_t z = 0; z < half[2]; ++z) {
      for (int64_t y = 0; y < half[1]; ++y) {
        for (int64_t x = 0; x < half[0]; ++x) {
          float sum[12] = {};
          int count = 0;
          for (int corner = 0; corner < 8; ++corner) {
            const int64_t fx = 2 * x + (corner & 1), fy = 2 * y + ((corner >> 1) & 1), fz = 2 * z + (corner >> 2);
            if (fx >= size[0] || fy >= size[1] || fz >= size[2]) {
              continue;
            }
            const uint8_t* p = &pts[((size_t(fz) * size_t(size[1]) + size_t(fy)) * size_t(size[0]) + size_t(fx)) * 24];
            if (!PointLit(p)) {
              continue;
            }
            for (int k = 0; k < 6; ++k) {
              sum[k] += Half(p + 2 * k);
            }
            ++count;
          }
          if (count == 0) {
            continue;
          }
          uint8_t* q = &coarse[((size_t(z) * size_t(half[1]) + size_t(y)) * size_t(half[0]) + size_t(x)) * 24];
          for (int k = 0; k < 6; ++k) {
            const uint16_t h = ToHalf(sum[k] / float(count));
            // A lit point never packs as an empty one.
            const uint16_t keep = (k < 3 && h == 0) ? 1 : h;
            q[2 * k] = uint8_t(keep);
            q[2 * k + 1] = uint8_t(keep >> 8);
          }
          for (int k = 12; k < 24; ++k) {
            int total = 0;
            for (int corner = 0; corner < 8; ++corner) {
              const int64_t fx = 2 * x + (corner & 1), fy = 2 * y + ((corner >> 1) & 1), fz = 2 * z + (corner >> 2);
              if (fx >= size[0] || fy >= size[1] || fz >= size[2]) {
                continue;
              }
              const uint8_t* p = &pts[((size_t(fz) * size_t(size[1]) + size_t(fy)) * size_t(size[0]) + size_t(fx)) * 24];
              total += PointLit(p) ? p[k] : 0;
            }
            q[k] = uint8_t((total + count / 2) / count);
          }
        }
      }
    }
    pts = std::move(coarse);
    points = fewer;
    for (int i = 0; i < 3; ++i) {
      size[i] = half[i];
      for (int j = 0; j < 3; ++j) {
        m[i][j] /= 2;
      }
      m[i][3] = (m[i][3] - 0.5) / 2;
    }
  }

  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) {
      out.worldToGrid[i * 4 + j] = float(m[i][j]);
    }
    out.size[i] = uint32_t(size[i]);
  }
  // The geometric mean of a sample of the lit points' luminance, as Parse takes it.
  double sum = 0.0;
  size_t filled = 0;
  const size_t step = points / 32768 + 1;
  for (size_t i = 0; i < points; i += step) {
    const uint8_t* p = &pts[i * 24];
    const double luminance = 0.2126 * Half(p) + 0.7152 * Half(p + 2) + 0.0722 * Half(p + 4);
    if (luminance > 0.0) {
      sum += std::log(luminance);
      ++filled;
    }
  }
  out.average = filled != 0 ? float(std::exp(sum / double(filled))) : 0.f;
  out.points = std::move(pts);
  char buf[96];
  std::snprintf(buf, sizeof buf, "grid %lldx%lldx%lld %.0f%% lit", (long long)size[0], (long long)size[1],
                (long long)size[2], 100.0 * double(litCount) / double(points));
  note = buf;
  return true;
}

} // namespace

bool ResolveGrid(const TileSet& own, const Lookup& lookup, size_t maxPoints, ResolvedGrid& out, std::string& note) {
  std::set<std::array<int32_t, 3>> blocks;
  for (const TileKey& key : Region(own)) {
    if (key.layer == 0) {
      blocks.insert({key.x, key.y, key.z});
    }
  }
  return ComposeBlocks(own.worldToPoint, blocks, lookup, maxPoints, 1024, out, note);
}

bool ResolveWorld(const float worldToPoint[12], const std::vector<TileKey>& keys, const Lookup& lookup, size_t maxPoints,
                  ResolvedGrid& out, std::string& note) {
  std::set<std::array<int32_t, 3>> blocks;
  for (const TileKey& key : keys) {
    if (key.layer < 2) {
      blocks.insert({key.x, key.y, key.z});
    } else {
      blocks.insert({key.x * 2, key.y, key.z});
      blocks.insert({key.x * 2 + 1, key.y, key.z});
    }
  }
  return ComposeBlocks(worldToPoint, blocks, lookup, maxPoints, 4096, out, note);
}

TileKey TileOf(int layer, int64_t x, int64_t y, int64_t z, int32_t local[3]) {
  const int width = TileWidth(layer);
  const TileKey key{layer, FloorDiv(x, width), FloorDiv(y, 64), FloorDiv(z, 16)};
  if (local != nullptr) {
    local[0] = int32_t(x - int64_t(key.x) * width);
    local[1] = int32_t(y - int64_t(key.y) * 64);
    local[2] = int32_t(z - int64_t(key.z) * 16);
  }
  return key;
}

bool ReadPoint(const Lookup& lookup, int layer, int64_t x, int64_t y, int64_t z, uint8_t out[6]) {
  std::memset(out, 0, 6);
  int32_t local[3];
  const TileKey key = TileOf(layer, x, y, z, local);
  const std::shared_ptr<const TileSet> set = lookup(key);
  const Tile* tile = set != nullptr ? set->Find(key) : nullptr;
  if (tile == nullptr || tile->size[0] == 0) {
    return false;
  }
  const int32_t q[3] = {local[0] - tile->lo[0], local[1] - tile->lo[1], local[2] - tile->lo[2]};
  if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= tile->size[0] || q[1] >= tile->size[1] || q[2] >= tile->size[2]) {
    return false;
  }
  const int bytes = TileBytes(layer);
  std::memcpy(out, &set->data[tile->offset + ((size_t(q[2]) * size_t(tile->size[1]) + size_t(q[1])) * size_t(tile->size[0]) + size_t(q[0])) * size_t(bytes)], size_t(bytes));
  return true;
}

std::vector<TileKey> TileTable::Register(uint32_t node, const std::vector<TileKey>& keys, int32_t priority) {
  std::vector<TileKey> changed;
  for (const TileKey& key : keys) {
    std::vector<Entry>& list = m_entries[key];
    Entry fresh{node, priority, false};
    auto active = std::find_if(list.begin(), list.end(), [](const Entry& e) { return e.active; });
    if (active == list.end()) {
      fresh.active = true;
      changed.push_back(key);
    } else if (active->priority < priority) {
      active->active = false;
      fresh.active = true;
      changed.push_back(key);
    }
    list.insert(list.begin(), fresh);
  }
  return changed;
}

std::vector<TileKey> TileTable::Unregister(uint32_t node) {
  std::vector<TileKey> changed;
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    std::vector<Entry>& list = it->second;
    auto own = std::find_if(list.begin(), list.end(), [node](const Entry& e) { return e.node == node; });
    if (own == list.end()) {
      ++it;
      continue;
    }
    const bool wasActive = own->active;
    list.erase(own);
    if (wasActive) {
      // The first strict maximum in vector order: of equal priorities the most recent.
      auto best = list.end();
      for (auto e = list.begin(); e != list.end(); ++e) {
        if (best == list.end() || e->priority > best->priority) {
          best = e;
        }
      }
      if (best != list.end()) {
        best->active = true;
      }
      changed.push_back(it->first);
    }
    if (list.empty()) {
      it = m_entries.erase(it);
    } else {
      ++it;
    }
  }
  return changed;
}

bool TileTable::Active(const TileKey& key, uint32_t* node) const {
  auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return false;
  }
  for (const Entry& e : it->second) {
    if (e.active) {
      if (node != nullptr) {
        *node = e.node;
      }
      return true;
    }
  }
  return false;
}

} // namespace PortRoomEnvTiles
