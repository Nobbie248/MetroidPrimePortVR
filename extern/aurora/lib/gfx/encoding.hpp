#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
// Binds a GX draw's uniform (group 1 at `offset`) unless the pass's last GX draw
// bound the same: Dawn applies a group with dynamic offsets again on every set,
// and on a tiled GPU each one costs in every bin.
void bind_gx_uniform(const wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bindGroup, uint32_t offset);
// The same for a GX draw's texture bind group (group 2, by its cache reference) and
// its index buffer range: consecutive draws of one model share both.
void bind_gx_textures(const wgpu::RenderPassEncoder& pass, BindGroupRef bindGroup);
void bind_gx_indices(const wgpu::RenderPassEncoder& pass, const wgpu::Buffer& buffer, uint64_t offset, uint64_t size);

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
