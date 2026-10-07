#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// "What is this thread doing", for the port's hang watchdog (platform/port_watchdog.cpp).
// Two slots, the main thread and the render worker, each a string literal and the time it
// was set. Lock-free and cheap enough to call around anything that can block in a driver;
// readers see a name and a time that may belong to neighbouring updates, which is fine.
namespace aurora::phase {

enum Slot : int { Main = 0, Render = 1, SlotCount = 2 };

struct State {
  std::atomic<const char*> name{"startup"};
  std::atomic<int64_t> sinceNs{0};
};

inline State& state(Slot slot) noexcept {
  static State s_states[SlotCount];
  return s_states[slot];
}

inline int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// `name` must outlive the process (a string literal).
inline void set(Slot slot, const char* name) noexcept {
  State& s = state(slot);
  s.name.store(name, std::memory_order_relaxed);
  s.sinceNs.store(now_ns(), std::memory_order_relaxed);
}

// Sets a phase and puts the previous one back on exit.
class Scope {
public:
  Scope(Slot slot, const char* name) noexcept : m_slot(slot), m_previous(state(slot).name.load(std::memory_order_relaxed)) {
    set(slot, name);
  }
  ~Scope() { set(m_slot, m_previous); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  Slot m_slot;
  const char* m_previous;
};

} // namespace aurora::phase
