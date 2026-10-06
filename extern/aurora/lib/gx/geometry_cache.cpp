#include "geometry_cache.hpp"

#include "../gfx/geometry_buffer.hpp"
#include "../gfx/perf_counters.hpp"

#include <absl/container/flat_hash_map.h>
#include <algorithm>

namespace aurora::gx::geometry_cache {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

struct FreeBlock {
  u32 offset;
  u32 size;
};

absl::flat_hash_map<Key, Entry> g_entries;
std::vector<FreeBlock> g_free; // by offset, neighbours merged
u64 g_capacity = 0;
u64 g_residentBytes = 0;

void publish_gauges() noexcept {
  gfx::perf::g_geometryEntries.store(static_cast<uint32_t>(g_entries.size()), std::memory_order_relaxed);
  gfx::perf::g_geometryResidentBytes.store(g_residentBytes, std::memory_order_relaxed);
}

void reset_blocks(u64 capacity) {
  g_entries.clear();
  g_free.clear();
  g_capacity = capacity;
  g_residentBytes = 0;
  if (capacity != 0) {
    g_free.push_back({0, static_cast<u32>(std::min<u64>(capacity, UINT32_MAX))});
  }
  publish_gauges();
}

// The buffer may appear, go or change size between frames (a device reset): the
// entries describe the old one.
void sync_capacity() {
  const u64 capacity = gfx::geometry_buffer_capacity();
  if (capacity != g_capacity) {
    reset_blocks(capacity);
  }
}

// First fit, at a multiple of `stride` so an absolute index is whole.
bool allocate(u32 bytes, u32 stride, u32& out) noexcept {
  for (size_t i = 0; i < g_free.size(); ++i) {
    const FreeBlock block = g_free[i];
    const u64 start = (static_cast<u64>(block.offset) + stride - 1) / stride * stride;
    const u64 end = start + bytes;
    const u64 blockEnd = static_cast<u64>(block.offset) + block.size;
    if (end > blockEnd) {
      continue;
    }
    out = static_cast<u32>(start);
    // What the block keeps: its front up to the aligned start, and its tail
    const u32 front = static_cast<u32>(start - block.offset);
    const u32 tail = static_cast<u32>(blockEnd - end);
    if (front != 0 && tail != 0) {
      g_free[i] = {block.offset, front};
      g_free.insert(g_free.begin() + static_cast<std::ptrdiff_t>(i) + 1, FreeBlock{static_cast<u32>(end), tail});
    } else if (front != 0) {
      g_free[i] = {block.offset, front};
    } else if (tail != 0) {
      g_free[i] = {static_cast<u32>(end), tail};
    } else {
      g_free.erase(g_free.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return true;
  }
  return false;
}

void release(u32 offset, u32 size) noexcept {
  auto it = std::lower_bound(g_free.begin(), g_free.end(), offset,
                             [](const FreeBlock& block, u32 value) { return block.offset < value; });
  it = g_free.insert(it, FreeBlock{offset, size});
  if (it + 1 != g_free.end() && it->offset + it->size == (it + 1)->offset) {
    it->size += (it + 1)->size;
    g_free.erase(it + 1);
  }
  if (it != g_free.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
    (it - 1)->size += it->size;
    g_free.erase(it);
  }
}

void drop(const Entry& entry) noexcept {
  release(entry.offset, entry.bytes);
  g_residentBytes -= entry.bytes;
}
} // namespace

const Entry* find(const Key& key) noexcept {
  sync_capacity();
  const auto it = g_entries.find(key);
  return it != g_entries.end() ? &it->second : nullptr;
}

const Entry* insert(const Key& key, u32 stride, std::span<const u8> records, const std::vector<u32>& indices) {
  sync_capacity();
  const u32 bytes = static_cast<u32>(records.size());
  if (g_capacity == 0 || stride == 0 || bytes == 0 || bytes % 4 != 0 || indices.empty()) {
    return nullptr;
  }
  if (const auto old = g_entries.find(key); old != g_entries.end()) {
    drop(old->second);
    g_entries.erase(old);
  }
  u32 offset = 0;
  if (!allocate(bytes, stride, offset)) {
    static bool sReported = false;
    if (!sReported) {
      sReported = true;
      Log.warn("Geometry cache full ({} MiB): the surfaces beyond it draw the plain way", g_capacity >> 20);
    }
    publish_gauges();
    return nullptr;
  }
  Entry entry{.offset = offset, .bytes = bytes, .stride = stride, .vertexCount = bytes / stride};
  const u32 base = offset / stride;
  entry.indices.resize(indices.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    entry.indices[i] = base + indices[i];
  }
  gfx::queue_geometry_upload(offset, records.data(), bytes);
  g_residentBytes += bytes;
  const auto [it, inserted] = g_entries.emplace(key, std::move(entry));
  publish_gauges();
  return &it->second;
}

void free_set(u32 set) noexcept {
  absl::erase_if(g_entries, [set](const auto& item) {
    if (item.first.set != set) {
      return false;
    }
    drop(item.second);
    return true;
  });
  publish_gauges();
}

void clear() noexcept { reset_blocks(0); }

Stats stats() noexcept {
  return {
      .entries = static_cast<u32>(g_entries.size()),
      .residentBytes = g_residentBytes,
      .capacity = g_capacity,
  };
}

} // namespace aurora::gx::geometry_cache
