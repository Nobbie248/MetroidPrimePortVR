#include "resident.hpp"

#include "../internal.hpp"

#include <algorithm>
#include <atomic>
#include <unordered_map>

namespace aurora::gx::resident {
namespace {
constexpr Module Log{"aurora::gx::resident"};

constexpr uint32_t Alignment = 256;

struct State {
  bool initialized = false;
  Allocator vertex;
  Allocator index;
  Allocator storage;
  std::unordered_map<const void*, Entry> entries;
  bool enabled = false; // any region has room
  bool warnedFull = false;
};
State g_state;
// For the game's stats, read off the processor thread.
std::atomic<uint64_t> g_used{0};

void publish_used() noexcept {
  g_used.store(g_state.vertex.used() + g_state.index.used() + g_state.storage.used(), std::memory_order_relaxed);
}

void ensure_initialized() {
  if (g_state.initialized) {
    return;
  }
  g_state.initialized = true;
  g_state.vertex.reset(gfx::resident_region(gfx::ResidentBuffer::Vertex));
  g_state.index.reset(gfx::resident_region(gfx::ResidentBuffer::Index));
  g_state.storage.reset(gfx::resident_region(gfx::ResidentBuffer::Storage));
  g_state.enabled = gfx::resident_region(gfx::ResidentBuffer::Vertex).size != 0 ||
                    gfx::resident_region(gfx::ResidentBuffer::Storage).size != 0;
}

void warn_full(const char* what, size_t size) {
  if (g_state.enabled && !g_state.warnedFull) {
    g_state.warnedFull = true;
    Log.warn("No room left for a resident {} of {} bytes (vertices {}, indices {}, arrays {} in use); it is sent "
             "every frame instead",
             what, size, g_state.vertex.used(), g_state.index.used(), g_state.storage.used());
  }
}
} // namespace

void Allocator::reset(gfx::Range region) noexcept {
  m_free.clear();
  m_used = 0;
  const uint32_t begin = AURORA_ALIGN(region.offset, Alignment);
  const uint64_t end = uint64_t(region.offset) + region.size;
  if (end > begin) {
    m_free.push_back({begin, uint32_t(end - begin)});
  }
}

bool Allocator::alloc(uint32_t size, uint32_t& offset) {
  if (size == 0) {
    return false;
  }
  const uint64_t need = AURORA_ALIGN(uint64_t(size), Alignment);
  for (auto it = m_free.begin(); it != m_free.end(); ++it) {
    if (it->size < need) {
      continue;
    }
    offset = it->offset;
    if (it->size == need) {
      m_free.erase(it);
    } else {
      it->offset += uint32_t(need);
      it->size -= uint32_t(need);
    }
    m_used += need;
    return true;
  }
  return false;
}

void Allocator::free(uint32_t offset, uint32_t size) noexcept {
  if (size == 0) {
    return;
  }
  const uint32_t length = uint32_t(AURORA_ALIGN(uint64_t(size), Alignment));
  m_used -= std::min<uint64_t>(m_used, length);
  auto next = std::lower_bound(m_free.begin(), m_free.end(), offset,
                               [](const Block& block, uint32_t value) { return block.offset < value; });
  auto it = m_free.insert(next, Block{offset, length});
  // Join the block after it, then the one before.
  if (auto after = it + 1; after != m_free.end() && it->offset + it->size == after->offset) {
    it->size += after->size;
    m_free.erase(after);
  }
  if (it != m_free.begin()) {
    auto before = it - 1;
    if (before->offset + before->size == it->offset) {
      before->size += it->size;
      m_free.erase(it);
    }
  }
}

void retain(const void* key, std::unique_ptr<std::vector<uint8_t>> bytes) {
  // The game counts its retains, so a second one only comes after a release.
  release(key);
  Entry entry;
  entry.bytes = std::move(bytes);
  g_state.entries.emplace(key, std::move(entry));
}

void release(const void* key) {
  const auto it = g_state.entries.find(key);
  if (it == g_state.entries.end()) {
    return;
  }
  Entry& entry = it->second;
  if (entry.storage.size != 0) {
    g_state.storage.free(entry.storage.offset, entry.storage.size);
  }
  free_dl(entry);
  g_state.entries.erase(it);
  publish_used();
}

Entry* find(const void* key) noexcept {
  const auto it = g_state.entries.find(key);
  return it != g_state.entries.end() ? &it->second : nullptr;
}

bool array_range(const void* data, uint32_t size, gfx::Range& out) {
  if (g_state.entries.empty()) {
    return false;
  }
  Entry* const entry = find(data);
  if (entry == nullptr || size > entry->bytes->size()) {
    return false;
  }
  if (!entry->arrayTried) {
    entry->arrayTried = true;
    ensure_initialized();
    const auto length = static_cast<uint32_t>(entry->bytes->size());
    uint32_t offset = 0;
    if (!g_state.storage.alloc(length, offset)) {
      warn_full("vertex array", length);
      return false;
    }
    if (!gfx::queue_resident_upload(gfx::ResidentBuffer::Storage, offset, entry->bytes->data(), length)) {
      // Outside a frame: tried again with the next draw that uses it.
      g_state.storage.free(offset, length);
      entry->arrayTried = false;
      return false;
    }
    entry->storage = {offset, length};
    publish_used();
  }
  if (entry->storage.size == 0) {
    return false;
  }
  out = {entry->storage.offset, size};
  return true;
}

bool store_dl(Entry& entry, const std::vector<uint8_t>& verts, const std::vector<uint8_t>& indices) {
  ensure_initialized();
  uint32_t vertOffset = 0;
  uint32_t idxOffset = 0;
  if (!g_state.vertex.alloc(static_cast<uint32_t>(verts.size()), vertOffset)) {
    warn_full("display list", verts.size());
    return false;
  }
  if (!g_state.index.alloc(static_cast<uint32_t>(indices.size()), idxOffset)) {
    g_state.vertex.free(vertOffset, static_cast<uint32_t>(verts.size()));
    warn_full("display list's indices", indices.size());
    return false;
  }
  entry.vert = {vertOffset, static_cast<uint32_t>(verts.size())};
  entry.idx = {idxOffset, static_cast<uint32_t>(indices.size())};
  if (!gfx::queue_resident_upload(gfx::ResidentBuffer::Vertex, vertOffset, verts.data(), verts.size()) ||
      !gfx::queue_resident_upload(gfx::ResidentBuffer::Index, idxOffset, indices.data(), indices.size())) {
    // Outside a frame (both fail alike): made again at its next call.
    free_dl(entry);
    return false;
  }
  publish_used();
  return true;
}

void free_dl(Entry& entry) noexcept {
  if (entry.vert.size != 0) {
    g_state.vertex.free(entry.vert.offset, entry.vert.size);
  }
  if (entry.idx.size != 0) {
    g_state.index.free(entry.idx.offset, entry.idx.size);
  }
  entry.vert = {};
  entry.idx = {};
  entry.chunks.clear();
  entry.dlTried = false;
}

void reset() noexcept {
  g_state = State{};
  publish_used();
}

uint64_t used_bytes() noexcept { return g_state.vertex.used() + g_state.index.used() + g_state.storage.used(); }

} // namespace aurora::gx::resident

uint64_t aurora_get_resident_geometry_used() { return aurora::gx::resident::g_used.load(std::memory_order_relaxed); }
