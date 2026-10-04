#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
// Binds a GX draw's uniform (group 1 at `offset`) unless the pass's last GX draw
// bound the same: Dawn applies a group with dynamic offsets again on every set,
// and on a tiled GPU each one costs in every bin.
void bind_gx_uniform(const wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bindGroup, uint32_t offset);

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
