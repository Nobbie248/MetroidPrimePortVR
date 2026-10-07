#pragma once

#include <cstdint>
#include "frame_packet.hpp"

namespace aurora::gx {
enum class TextureLayout : uint8_t;
}

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
// Binds a GX draw's uniform (group 1 at `offset`) unless the pass's last GX draw
// bound the same: Dawn applies a group with dynamic offsets again on every set,
// and on a tiled GPU each one costs in every bin.
void bind_gx_uniform(const wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bindGroup, uint32_t offset);
// The same for a GX draw's texture bind group (group 2, by its cache reference) and
// its index buffer range: consecutive draws of one model share both. A draw without a
// group of its own keeps the bound one while that has its pipeline's layout (`layout`,
// gx::TextureLayout); else the empty group of that layout (the multiview one with
// `multiview`) goes back.
void bind_gx_textures(const wgpu::RenderPassEncoder& pass, BindGroupRef bindGroup, gx::TextureLayout layout,
                      bool multiview = false);
// Call after setting bind group 2 directly, so the next GX draw sets its own again.
void forget_texture_group();
void bind_gx_indices(const wgpu::RenderPassEncoder& pass, const wgpu::Buffer& buffer, uint64_t offset, uint64_t size,
                     wgpu::IndexFormat format = wgpu::IndexFormat::Uint16);
// Group 0: the frame's vertex buffer, or the geometry cache's (geometry_buffer.hpp)
// for a draw of cached geometry.
void bind_gx_geometry(const wgpu::RenderPassEncoder& pass, bool cached);
// Vertex buffer 0 for native vertex input: the geometry cache's buffer, unless the pass has it bound.
void bind_gx_native_vertices(const wgpu::RenderPassEncoder& pass);

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
