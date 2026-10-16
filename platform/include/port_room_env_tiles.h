#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// Which area's copy of a baked-lighting tile is live. Remastered samples one world-wide
// virtual texture whose tiles are keyed by world position (CBakedLightingProbeTexture).
// Each loaded area registers its own copy of every tile; all areas have priority 0, so the
// first copy registered stays mapped and later ones wait on standby. When the mapped
// copy's area unloads, the most recently registered standby copy of another area takes
// over (registration 0x1ca01c, unregistration 0x1ca650); with none, the tile is unmapped.
// Priority is never re-resolved.
namespace PortRoomEnvTiles {

struct TileKey {
  int32_t layer = 0; // 0..5
  int32_t x = 0;     // block coordinates (tile origin / tile size)
  int32_t y = 0;
  int32_t z = 0;
  bool operator<(const TileKey& o) const {
    return std::tie(layer, x, y, z) < std::tie(o.layer, o.x, o.y, o.z);
  }
  bool operator==(const TileKey& o) const {
    return layer == o.layer && x == o.x && y == o.y && z == o.z;
  }
};

// Layers 0 and 1 (mean, lobe) are 64 x 64 x 16 point tiles, layers 2..5 (sharpness and the
// three directions) 128 x 64 x 16. A tile's origin is (bx * width, by * 64, bz * 16).
constexpr int kLayers = 6;
inline int TileWidth(int layer) { return layer < 2 ? 64 : 128; }
// Stored bytes per point: three halves for layers 0 and 1, three bytes for the rest.
inline int TileBytes(int layer) { return layer < 2 ? 6 : 3; }

// One area's own copy of a tile, cropped to the box that holds its non-zero points.
struct Tile {
  TileKey key;
  int32_t lo[3] = {0, 0, 0};   // crop origin, in points within the tile
  int32_t size[3] = {0, 0, 0}; // crop size; 0 = an empty tile that still takes part in residency
  uint32_t offset = 0;         // into TileSet::data, x fastest then y then z
};

// What a .roomenv holds of the world's baked lighting: the area's own tiles, in world
// tile coordinates, with nothing borrowed from a neighbour.
struct TileSet {
  float worldToPoint[12] = {}; // area space to point coordinates (no crop origin)
  int32_t box[6] = {};         // the area's PHDR box in points: lo xyz, hi xyz
  std::vector<Tile> tiles;
  std::vector<uint8_t> data;
  const Tile* Find(const TileKey& key) const;
  // Whether point (x, y, z) of layer 0 is lit by this set's own tile.
  bool Lit(int64_t x, int64_t y, int64_t z) const;
};

// A grid in the runtime form of port_room_env.h: 24 byte points, x fastest.
struct ResolvedGrid {
  float worldToGrid[12] = {};
  uint32_t size[3] = {};
  std::vector<uint8_t> points;
  float average = 0.f;
};

// The geometric mean of the luminance of the lit points of the set's own tiles (a sample);
// 0 with none. What an area's exposure falls back on before its grid is composed.
float OwnAverage(const TileSet& own);
// The keys, over every layer, an area samples: its own tiles and the tiles under its PHDR box.
std::vector<TileKey> Region(const TileSet& own);
// The tile set whose copy of key is mapped, or null (the tile reads zero).
using Lookup = std::function<std::shared_ptr<const TileSet>(const TileKey&)>;
// Composes the area's grid from the mapped tiles of its region, as Remastered's one world
// texture would read it, cropped to the lit points and one empty point around them. False
// with a note when the area has no lit point or the grid is over the limits. maxPoints > 0
// halves the grid until it fits.
bool ResolveGrid(const TileSet& own, const Lookup& lookup, size_t maxPoints, ResolvedGrid& out, std::string& note);

// The one world texture's grid, composed from every key in `keys` as the lookup maps it: the
// points of all loaded areas in the single point space worldToPoint gives, cropped to the lit
// points and one empty point around them (a point outside reads Remastered's transparent
// black border). A tap across a tile seam reads the neighbouring tile's copy, whichever
// area it came from. maxPoints > 0 halves the grid until it fits. False with a note when
// no point is lit or the grid is over the limits.
bool ResolveWorld(const float worldToPoint[12], const std::vector<TileKey>& keys, const Lookup& lookup, size_t maxPoints,
                  ResolvedGrid& out, std::string& note);
// The indirection of the world texture: the tile that holds point (x, y, z) of a layer, and
// the point's place inside it (0..width, 0..64, 0..16).
TileKey TileOf(int layer, int64_t x, int64_t y, int64_t z, int32_t local[3]);
// The stored bytes (6 for layers 0 and 1, 3 otherwise) of that point from the mapped tile;
// zeros and false when the tile is unmapped or the point is outside its crop.
bool ReadPoint(const Lookup& lookup, int layer, int64_t x, int64_t y, int64_t z, uint8_t out[6]);

class TileTable {
public:
  // Registers node's copies of keys (an area finishing its load). Returns the keys whose
  // mapped owner changed.
  std::vector<TileKey> Register(uint32_t node, const std::vector<TileKey>& keys, int32_t priority = 0);
  // Removes every copy of node (an area unloading). Returns the keys whose mapped owner
  // changed, including keys that became unmapped.
  std::vector<TileKey> Unregister(uint32_t node);
  // The node whose copy is mapped for key, or false when the tile is unmapped.
  bool Active(const TileKey& key, uint32_t* node) const;
  // Every mapped key and the node whose copy is mapped: what the world texture holds.
  std::vector<std::pair<TileKey, uint32_t>> Mapped() const {
    std::vector<std::pair<TileKey, uint32_t>> out;
    for (const auto& [key, entries] : m_entries) {
      for (const Entry& entry : entries) {
        if (entry.active) {
          out.emplace_back(key, entry.node);
          break;
        }
      }
    }
    return out;
  }
  void Clear() { m_entries.clear(); }
  size_t KeyCount() const { return m_entries.size(); }

private:
  struct Entry {
    uint32_t node;
    int32_t priority;
    bool active;
  };
  // Front is the most recently registered copy, as a lower_bound insert places it.
  std::map<TileKey, std::vector<Entry>> m_entries;
};

} // namespace PortRoomEnvTiles
