#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
inline constexpr uint64_t UniformBufferSize = 25165824; // 24 MiB
inline constexpr uint64_t VertexBufferSize = 5242880;   // 5 MiB
inline constexpr uint64_t IndexBufferSize = 2097152;    // 2 MiB
inline constexpr uint64_t StorageBufferSize = 8388608;  // 8 MiB
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB
// The least a frame holds of vertices at any scale. With indexed vertices resolved on the
// CPU (GXState::deindexVertices), a virtual-screen transition on the Quest ran 5 MiB out
// with one large draw left to stage. Each MiB costs one per staging buffer plus the device
// copy; only the bytes a frame stages are copied.
inline constexpr uint64_t MinVertexBufferSize = 10485760; // 10 MiB

// What a frame can hold of each kind of data. The constants above are the sizes at
// AuroraConfig::frameBufferScale 1 (vertices: MinVertexBufferSize); initialize() fixes the
// sizes in use for the run.
struct FrameBufferSizes {
  uint64_t vertex = MinVertexBufferSize;
  uint64_t uniform = UniformBufferSize;
  uint64_t index = IndexBufferSize;
  uint64_t storage = StorageBufferSize;
  // The AuroraConfig::frameBufferScale these sizes are for.
  uint32_t scale = 1;

  // Kept across frames (AuroraConfig::residentGeometryMiB): the device buffers hold these
  // after what a frame holds, and no staging buffer does.
  uint64_t residentVertex = 0;
  uint64_t residentIndex = 0;
  uint64_t residentStorage = 0;

  uint64_t staging() const noexcept {
    return vertex + uniform + index + storage + (UseTextureBuffer ? TextureUploadSize : 0);
  }
};
const FrameBufferSizes& frame_buffer_sizes() noexcept;

enum class ResidentBuffer : uint8_t { Vertex, Index, Storage };
// The part of a shared buffer kept across frames, as offsets into that buffer; size 0
// when there is none.
Range resident_region(ResidentBuffer kind) noexcept;
// Copies `size` bytes into a resident region at `offset` (an offset into the buffer, as
// resident_region gives), before the pass being recorded draws. False outside a frame's
// recording, where nothing is copied.
bool queue_resident_upload(ResidentBuffer kind, uint32_t offset, const uint8_t* data, size_t size);

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
