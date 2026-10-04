#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
inline constexpr uint64_t UniformBufferSize = 25165824; // 24 MiB
inline constexpr uint64_t VertexBufferSize = 5242880;   // 5 MiB
inline constexpr uint64_t IndexBufferSize = 2097152;    // 2 MiB
inline constexpr uint64_t StorageBufferSize = 8388608;  // 8 MiB
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB

// What a frame can hold of each kind of data. The constants above are the sizes at
// AuroraConfig::frameBufferScale 1; initialize() fixes the sizes in use for the run.
struct FrameBufferSizes {
  uint64_t vertex = VertexBufferSize;
  uint64_t uniform = UniformBufferSize;
  uint64_t index = IndexBufferSize;
  uint64_t storage = StorageBufferSize;

  uint64_t staging() const noexcept {
    return vertex + uniform + index + storage + (UseTextureBuffer ? TextureUploadSize : 0);
  }
};
const FrameBufferSizes& frame_buffer_sizes() noexcept;

namespace detail {
struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  // The multiview stereo replay's (stereo_multiview.hpp): two GX uniforms wide, an
  // eye pair from the dynamic offset. Only with the feature.
  wgpu::BindGroup multiviewUniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
} // namespace detail
} // namespace aurora::gfx
