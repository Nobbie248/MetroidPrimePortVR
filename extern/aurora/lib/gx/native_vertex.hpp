#pragma once

// Native vertex fetch for the aligned, resident world records. Keep their stride
// and offsets so absolute indices and material batching do not change. Only the
// byte order of numeric components changes, once when the cache entry is built.
#include "gx.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace aurora::gx::native_vertex {
constexpr size_t MaxAttributes = 16; // WebGPU's baseline vertex attribute limit.
struct Layout {
  std::array<wgpu::VertexAttribute, MaxAttributes> attributes{};
  size_t count = 0;
  bool valid = false;
};

inline Layout layout(const ShaderConfig& config) {
  Layout out;
  if (config.lineMode || config.mapBatch || config.vtxStride == 0 || config.vtxStride % 4 != 0 ||
      config.attrs[GX_VA_POS].attrType == GX_NONE) { return out; }
  for (u32 attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr) {
    const auto& a = config.attrs[attr];
    if (a.attrType == GX_NONE) { continue; }
    if (a.attrType != GX_DIRECT || a.nbt3 || out.count == MaxAttributes) { return out; }
    wgpu::VertexFormat format;
    u32 offset = a.offset, bytes = 0;
    if (attr < GX_VA_POS) {
      // One byte matrix index: fetch its aligned word and select that byte.
      offset &= ~3u;
      bytes = 4;
      format = wgpu::VertexFormat::Uint32;
    } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      if (a.compType != GX_RGBA8) { return out; }
      format = wgpu::VertexFormat::Unorm8x4;
      bytes = 4;
    } else {
      if (a.cnt == 0 || a.cnt > 4 || offset % 4 != 0) { return out; }
      const bool pair = a.cnt <= 2;
      switch (a.compType) {
      case GX_F32: {
        constexpr std::array formats{wgpu::VertexFormat::Float32, wgpu::VertexFormat::Float32x2,
                                     wgpu::VertexFormat::Float32x3, wgpu::VertexFormat::Float32x4};
        format = formats[a.cnt - 1]; bytes = a.cnt * 4; break;
      }
      case GX_U8: format = pair ? wgpu::VertexFormat::Uint8x2 : wgpu::VertexFormat::Uint8x4;
        bytes = pair ? 2 : 4; break;
      case GX_S8: format = pair ? wgpu::VertexFormat::Sint8x2 : wgpu::VertexFormat::Sint8x4;
        bytes = pair ? 2 : 4; break;
      case GX_U16: format = pair ? wgpu::VertexFormat::Uint16x2 : wgpu::VertexFormat::Uint16x4;
        bytes = pair ? 4 : 8; break;
      case GX_S16: format = pair ? wgpu::VertexFormat::Sint16x2 : wgpu::VertexFormat::Sint16x4;
        bytes = pair ? 4 : 8; break;
      default: return out;
      }
    }
    // Integer x3 uses an x4 fetch; the extra component is padding and ignored.
    if (offset + bytes > config.vtxStride || offset % std::min(bytes, 4u) != 0) { return out; }
    out.attributes[out.count] = {.format = format, .offset = offset,
                                 .shaderLocation = static_cast<u32>(out.count)};
    ++out.count;
  }
  out.valid = true;
  return out;
}

inline bool convert(const ShaderConfig& config, std::span<u8> records) {
  if (!layout(config).valid || records.size() % config.vtxStride != 0) { return false; }
  for (u32 attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
    const auto& a = config.attrs[attr];
    if (a.attrType == GX_NONE || a.le || attr == GX_VA_CLR0 || attr == GX_VA_CLR1) { continue; }
    const u32 bytes = a.compType == GX_F32 ? 4 : (a.compType == GX_U16 || a.compType == GX_S16) ? 2 : 1;
    if (bytes == 1) { continue; }
    for (size_t vertex = 0; vertex < records.size(); vertex += config.vtxStride) {
      for (u32 component = 0; component < a.cnt; ++component) {
        auto* value = records.data() + vertex + a.offset + component * bytes;
        std::reverse(value, value + bytes);
      }
    }
  }
  return true;
}
} // namespace aurora::gx::native_vertex
