#include "geometry_buffer.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "resources.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>

namespace aurora::gfx {
namespace {
constexpr Module Log{"aurora::gfx"};
constexpr uint64_t DefaultCapacityMb = 64;

wgpu::Buffer g_buffer;
wgpu::BindGroup g_bindGroup;
std::atomic<uint64_t> g_capacity{0};
} // namespace

void initialize_geometry_buffer() {
  shutdown_geometry_buffer();
  uint64_t mb = DefaultCapacityMb;
  if (const char* value = std::getenv("AURORA_GEOMETRY_CACHE_MB"); value != nullptr && value[0] != '\0') {
    mb = std::strtoull(value, nullptr, 10);
  }
  const auto& res = detail::resources();
  if (mb == 0 || !res.storageBuffer || !res.staticBindGroupLayout) {
    return;
  }
  // Bound whole as storage, so the device's binding limit caps it.
  const uint64_t capacity = std::min({mb << 20, static_cast<uint64_t>(res.limits.maxStorageBufferBindingSize),
                                      static_cast<uint64_t>(res.limits.maxBufferSize)}) &
                            ~static_cast<uint64_t>(3);
  const wgpu::BufferDescriptor descriptor{
      .label = "Geometry cache buffer",
      .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
      .size = capacity,
  };
  g_buffer = webgpu::g_device.CreateBuffer(&descriptor);
  const std::array entries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .buffer = g_buffer,
      },
      wgpu::BindGroupEntry{
          .binding = 1,
          .buffer = res.storageBuffer,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .label = "Geometry cache bind group",
      .layout = res.staticBindGroupLayout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_bindGroup = webgpu::g_device.CreateBindGroup(&bindGroupDescriptor);
  g_capacity.store(capacity, std::memory_order_release);
  Log.info("Geometry cache: {} MiB for resident world geometry", capacity >> 20);
}

void shutdown_geometry_buffer() {
  g_capacity.store(0, std::memory_order_release);
  g_bindGroup = {};
  g_buffer = {};
}

uint64_t geometry_buffer_capacity() noexcept { return g_capacity.load(std::memory_order_acquire); }

namespace detail {
const wgpu::Buffer& geometry_buffer() noexcept { return g_buffer; }
const wgpu::BindGroup& geometry_bind_group() noexcept { return g_bindGroup; }
} // namespace detail

} // namespace aurora::gfx
