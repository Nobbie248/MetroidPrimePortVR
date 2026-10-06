#pragma once

#include "../internal.hpp"

#include <cstdint>

namespace aurora::gx::fifo {

struct ProcessResult {
  uint32_t bytesProcessed;
  bool drawDone;
};

// Process GX FIFO commands until the next draw done event or end of buffer
ProcessResult process(const uint8_t* data, uint32_t size) noexcept;
void clear_draw_cache() noexcept;
// Full reset when the FIFO worker is stopped, including persistent bindings.
void reset_draw_cache() noexcept;
// Forgets which vertex layouts take native input (part of reset_draw_cache; tests
// that swap populate_pipeline_config's stubbed layout call it directly).
void clear_native_vertex_choices() noexcept;

} // namespace aurora::gx::fifo
