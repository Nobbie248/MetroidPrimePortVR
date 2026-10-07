#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// Port extension: the slots that carry a frame's data out of a GPU buffer and back. A slot goes
// Available -> CopySubmitted (the copy is encoded) -> MapPending (the submit has happened, the
// map is asked for) -> Available (the map has come back and been read).
//
// The bookkeeping lives here, apart from the buffer type, so its locking can be tested on the
// CPU. Three things make that discipline necessary rather than stylistic:
//  - a map callback can run on any thread, and the game thread's encode, submit and shutdown
//    all touch the same slots, so every transition is under one lock;
//  - releasing a buffer with a map still in flight runs that map's callback right there on the
//    releasing thread (Dawn cancels it), so a shutdown that drops buffers under the lock
//    deadlocks against the callback's own lock: retire() hands the handles back instead, and the
//    caller releases them once it is out of the lock;
//  - that callback must name a slot without holding the buffer, or the pending map would keep a
//    buffer alive that the slot has let go of, and a callback that arrives late (or twice) could
//    then read or unmap whatever that slot holds now. So a map is identified by a Key, which
//    carries no reference, and every use of a slot takes a new generation.
namespace aurora::gfx::detail {

enum class ReadbackState { Available, CopySubmitted, MapPending };

template <typename Buffer, size_t Count> class ReadbackSlots {
public:
  // Which slot, and which use of it. Holding no buffer, this is all a map's callback carries.
  struct Key {
    size_t index = 0;
    uint64_t generation = 0;
  };

  // A slot's buffer to map, with the key its callback carries. The buffer is the caller's to
  // release when it is done asking; the slot has one of its own all along.
  struct Mapping {
    Key key;
    Buffer buffer{};
  };

  // The slots whose copy has been submitted: `count` of `mappings`, the rest empty. Mapping
  // happens outside the lock, as a map can complete straight away and its callback takes the
  // lock again. Fixed size, so a frame allocates nothing here.
  struct Submitted {
    std::array<Mapping, Count> mappings{};
    size_t count = 0;
  };

  // `make` builds a slot's buffer the first time that slot is used.
  explicit ReadbackSlots(Buffer (*make)()) : m_make(make) {}

  // A free slot's buffer, now marked as copying into it, or an empty buffer when every slot is
  // busy: the caller then skips its copy for a frame. `exposure` is kept for the map's caller.
  // A claimed slot always has a buffer, so every mapping submit() hands back has one too.
  Buffer claim(float exposure) {
    std::lock_guard lock(m_mutex);
    Buffer buffer{};
    for (size_t i = 0; i < Count && !buffer; ++i) {
      const size_t index = (m_next + i) % Count;
      Slot& slot = m_slots[index];
      if (slot.state != ReadbackState::Available) {
        continue;
      }
      if (!slot.buffer) {
        slot.buffer = m_make();
      }
      // Every claim is a new use of the slot, buffer or not, so a callback left over from an
      // earlier one cannot reach this use.
      ++slot.generation;
      slot.state = ReadbackState::CopySubmitted;
      slot.exposure = exposure;
      buffer = slot.buffer;
      m_next = (index + 1) % Count;
    }
    return buffer;
  }

  Submitted submit() {
    std::lock_guard lock(m_mutex);
    Submitted submitted;
    for (size_t index = 0; index < Count; ++index) {
      Slot& slot = m_slots[index];
      if (slot.state != ReadbackState::CopySubmitted) {
        continue;
      }
      slot.state = ReadbackState::MapPending;
      submitted.mappings[submitted.count++] =
          Mapping{.key = Key{.index = index, .generation = slot.generation}, .buffer = slot.buffer};
    }
    return submitted;
  }

  // Finishes the map a key was issued for. False when the slot is not waiting on that map: it has
  // been retired, taken again since (which takes a new generation), or is not mapping at all, as
  // a repeated delivery of one callback is. A callback that fails this must not read, unmap or
  // release what the slot holds now. `underLock(buffer, exposure)` runs with the slot still busy,
  // so no frame can claim it before the caller has read and unmapped it, and the slot is free
  // again afterwards.
  template <typename F> bool complete(const Key& key, F&& underLock) {
    std::lock_guard lock(m_mutex);
    if (key.index >= Count) {
      return false;
    }
    Slot& slot = m_slots[key.index];
    if (slot.state != ReadbackState::MapPending || slot.generation != key.generation) {
      return false;
    }
    std::forward<F>(underLock)(slot.buffer, slot.exposure);
    slot.state = ReadbackState::Available;
    return true;
  }

  // Every buffer, out of the slots and with a new generation, so the caller can release it with
  // no lock held: see the note above. `underLock` runs once the slots are retired, for state the
  // caller keeps beside them.
  template <typename F> std::vector<Buffer> retire(F&& underLock) {
    std::vector<Buffer> retired;
    std::lock_guard lock(m_mutex);
    retired.reserve(Count);
    for (Slot& slot : m_slots) {
      retired.push_back(std::move(slot.buffer));
      ++slot.generation;
      slot.state = ReadbackState::Available;
      slot.exposure = 0.f;
    }
    std::forward<F>(underLock)();
    return retired;
  }

private:
  struct Slot {
    Buffer buffer{};
    ReadbackState state = ReadbackState::Available;
    float exposure = 0.f;
    uint64_t generation = 0;
  };

  std::mutex m_mutex;
  Buffer (*m_make)() = nullptr;
  std::array<Slot, Count> m_slots{};
  size_t m_next = 0;
};
} // namespace aurora::gfx::detail