#pragma once
// Static world geometry kept on the GPU across frames (GX_AURORA_CALL_CACHED_DL).
//
// A surface's display list is resolved once into flat vertex records (the de-indexed
// layout of gx.hpp deindexed_layout) in one persistent storage buffer
// (gfx/geometry_buffer.hpp), with its triangle-list indices kept absolute into that
// buffer. Each frame, a hit only appends those indices to the frame's index staging:
// the draw reads the buffer with vtx_start 0, and as every block sits at a multiple of
// its stride, index = block offset / stride + vertex, so the surfaces of one material
// merge into one draw wherever their blocks are. Keyed by the geometry set, the
// display list's address and the layout the records were resolved with; a set's
// entries and blocks go with GX_AURORA_FREE_GEOMETRY_SET. FIFO processor thread only.

#include "../internal.hpp"

#include <dolphin/types.h>

#include <cstdint>
#include <span>
#include <vector>

namespace aurora::gx::geometry_cache {

struct Key {
  u32 set = 0;
  u64 address = 0; // the display list
  u64 layout = 0;  // the resolved layout (command_processor.cpp deindex_plan_hash)
  bool operator==(const Key&) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const Key& key) {
    return H::combine(std::move(h), key.set, key.address, key.layout);
  }
};

struct Entry {
  u32 offset = 0; // in the geometry buffer, a multiple of stride
  u32 bytes = 0;  // of records, a multiple of 4
  u32 stride = 0;
  u32 vertexCount = 0;
  std::vector<u32> indices; // triangle list, absolute into the buffer
};

// The entry for a key, or null.
const Entry* find(const Key& key) noexcept;
// Stores a surface's resolved records (uploaded through gfx::queue_geometry_upload)
// and their triangle-list indices, relative to the first record here, absolute in the
// entry. Null when the buffer has no room or the records are not a whole number of
// 4-byte words: the surface then draws the plain way.
const Entry* insert(const Key& key, u32 stride, std::span<const u8> records, const std::vector<u32>& indices);
// Drops a set's entries and frees their blocks. Safe while frames still draw them: the
// uploads that reuse a block are queued after those frames' draws.
void free_set(u32 set) noexcept;
// Drops everything (the FIFO or the buffer is reset).
void clear() noexcept;

struct Stats {
  u32 entries = 0;
  u64 residentBytes = 0;
  u64 capacity = 0;
};
Stats stats() noexcept;

} // namespace aurora::gx::geometry_cache
