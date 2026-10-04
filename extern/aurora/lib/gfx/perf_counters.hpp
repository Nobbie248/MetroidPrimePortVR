#pragma once

// Per-frame CPU costs of the frame pipeline, accumulated between two stereo
// statistics lines (recording.cpp log_stereo_frame_stats) while stereo
// diagnostics are on: nanoseconds, and the bytes the FIFO processor consumed.

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace aurora::gfx::perf {

extern std::atomic<uint64_t> g_fifoNs;     // gx::fifo process() on the FIFO processor
extern std::atomic<uint64_t> g_fifoBytes;
extern std::atomic<uint64_t> g_encodeNs;   // encode_op on the render worker
extern std::atomic<uint64_t> g_submitNs;   // the queue submit on the render worker
// Inside process(), in ticks of tick() (the stats line converts): the register
// parsing by kind, then per draw the vertex and index staging, the pipeline
// lookup, the bind groups, build_uniform and push_draw_command...
extern std::atomic<uint64_t> g_fifoXfTicks;
extern std::atomic<uint64_t> g_fifoBpTicks;
extern std::atomic<uint64_t> g_fifoCpTicks;
extern std::atomic<uint64_t> g_fifoAuroraTicks;
extern std::atomic<uint64_t> g_fifoVertsTicks;
extern std::atomic<uint64_t> g_fifoPipelineTicks;
extern std::atomic<uint64_t> g_fifoBindsTicks;
extern std::atomic<uint64_t> g_fifoUniformTicks;
extern std::atomic<uint64_t> g_fifoPushTicks;
// ... and how often each ran.
extern std::atomic<uint32_t> g_fifoXfLoads;
extern std::atomic<uint32_t> g_fifoBpLoads;
extern std::atomic<uint32_t> g_fifoDrawsMerged;
extern std::atomic<uint32_t> g_fifoDrawsPushed;
extern std::atomic<uint32_t> g_fifoPipelineBuilds;
extern std::atomic<uint32_t> g_fifoBindGroupBuilds;
extern std::atomic<uint32_t> g_fifoBindGroupMisses; // ... of which the cache had to build
extern std::atomic<uint64_t> g_fifoResolveTicks;    // resolve_sampled_textures, within the bind groups' time
extern std::atomic<uint32_t> g_fifoUniformBuilds;
// The game thread blocked in gx::fifo::drain() for the FIFO processor, and
// how many drains it asked for.
extern std::atomic<uint64_t> g_drainWaitNs;
extern std::atomic<uint32_t> g_drainCalls;

bool enabled() noexcept;

// The CPU's own counter for the per-draw buckets: steady_clock costs more than
// some of what they measure. The stats line calibrates it against steady_clock.
inline uint64_t tick() noexcept {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  return __rdtsc();
#elif defined(__aarch64__)
  uint64_t value;
  asm volatile("mrs %0, cntvct_el0" : "=r"(value));
  return value;
#else
  return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
inline uint64_t stamp(bool on) noexcept { return on ? tick() : 0; }
inline void add(std::atomic<uint64_t>& sink, uint64_t from, uint64_t to) noexcept {
  sink.fetch_add(to - from, std::memory_order_relaxed);
}
inline void count(std::atomic<uint32_t>& sink, bool on) noexcept {
  if (on) {
    sink.fetch_add(1, std::memory_order_relaxed);
  }
}
// A scope's ticks, and a call counted.
struct Bucket {
  std::atomic<uint64_t>& ticks;
  std::atomic<uint32_t>* const calls;
  const bool on;
  const uint64_t start;
  explicit Bucket(std::atomic<uint64_t>& sink, bool enabled, std::atomic<uint32_t>* counter = nullptr) noexcept
  : ticks(sink), calls(counter), on(enabled), start(enabled ? tick() : 0) {}
  ~Bucket() {
    if (on) {
      ticks.fetch_add(tick() - start, std::memory_order_relaxed);
      if (calls != nullptr) {
        calls->fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
};

struct Timer {
  std::atomic<uint64_t>& sink;
  const bool on;
  const std::chrono::steady_clock::time_point start;
  explicit Timer(std::atomic<uint64_t>& target) noexcept
  : sink(target), on(enabled()), start(on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
  ~Timer() {
    if (on) {
      sink.fetch_add(static_cast<uint64_t>((std::chrono::steady_clock::now() - start).count()),
                     std::memory_order_relaxed);
    }
  }
};

} // namespace aurora::gfx::perf
