#pragma once

#include "../gfx/types.hpp"
#include "gx.hpp"

#include <array>
#include <cstdint>

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  DrawImmediateData immediateData;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  GXBindGroups bindGroups;
  uint32_t dstAlpha;
  // Stereo replay (gfx/stereo_frame.hpp): the uniform offsets this draw binds
  // in the left and right eye passes, staged along with the mono uniform.
  // UINT32_MAX leaves the draw out of that eye.
  std::array<uint32_t, 2> stereoUniformOffset{UINT32_MAX, UINT32_MAX};
};

constexpr uint32_t GXPipelineConfigVersion = 14;
struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  uint32_t dstAlpha;
  uint32_t polygonOffsetBits;
  uint32_t polygonOffsetScaleBits;
  uint32_t polygonOffsetClampBits;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

wgpu::RenderPipeline create_pipeline([[maybe_unused]] const PipelineConfig& config);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass);
// The same draw in a stereo eye pass, bound to its eye uniform.
void render_eye(const DrawData& data, const wgpu::RenderPassEncoder& pass, uint32_t eye);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx
