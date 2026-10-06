#pragma once
// The persistent vertex storage the geometry cache fills (gx/geometry_cache.hpp): one
// buffer, bound in place of the frame's vertex buffer by the draws that read it.

#include <cstdint>
#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx {

// Creates the buffer and its bind group; needs the static bind group layout and the
// frame storage buffer (resources.hpp). AURORA_GEOMETRY_CACHE_MB sizes it (64 when
// unset; 0 turns the cache off).
void initialize_geometry_buffer();
void shutdown_geometry_buffer();
// The buffer's size, 0 without one.
uint64_t geometry_buffer_capacity() noexcept;
// Copies `size` bytes (a multiple of 4) to `offset` of the buffer, through this frame's
// vertex staging: on the queue after everything earlier frames drew, before this
// frame's next op. Recording thread.
void queue_geometry_upload(uint32_t offset, const uint8_t* data, uint32_t size);

namespace detail {
const wgpu::Buffer& geometry_buffer() noexcept;
// Group 0 for a draw reading the buffer: the static layout, with the buffer first and
// the frame storage buffer second.
const wgpu::BindGroup& geometry_bind_group() noexcept;
} // namespace detail

} // namespace aurora::gfx
