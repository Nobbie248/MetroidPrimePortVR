#include "stereo_multiview.hpp"

#include "../webgpu/gpu.hpp"

#include <atomic>

namespace aurora::gfx::stereo_multiview {
namespace {
std::atomic_bool g_requested{true};
} // namespace

void set_requested(bool requested) noexcept { g_requested.store(requested, std::memory_order_relaxed); }

bool requested() noexcept { return g_requested.load(std::memory_order_relaxed); }

bool usable(uint32_t leftWidth, uint32_t leftHeight, uint32_t rightWidth, uint32_t rightHeight,
            uint32_t sampleCount) noexcept {
  if constexpr (!kApiAvailable) {
    return false;
  }
  // One two-layer target holds both eyes, so they share a size; a multisampled
  // EFB would need layered resolves, which the eye passes do not do.
  return webgpu::g_multiviewSupported && requested() && sampleCount <= 1 && leftWidth == rightWidth &&
         leftHeight == rightHeight;
}

} // namespace aurora::gfx::stereo_multiview
