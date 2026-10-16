#pragma once

#include "types.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::tex_copy_conv {

enum class SampleFilter : uint8_t {
  Nearest,
  Linear,
};

// What a colour copy turns its source into.
enum class CopyDomain : uint8_t {
  // The source as is, through the format's conversion (the 8-bit EFB's copies).
  Display,
  // The RGBA16F HDR scene, post-toned and sRGB-encoded by toneRange's curve first: the display
  // colour a retail copy of the same scene holds, then the format's conversion.
  HdrDisplay,
  // The RGBA16F HDR scene as linear light into an RGBA16F destination (GXPortCopyScene); only
  // the plain colour formats (RGB565 still writes alpha 1).
  Linear,
};

struct ConvRequest {
  GXTexFmt fmt;
  wgpu::TextureView srcView; // View of resolved EFB / offscreen color/depth
  Range uniformRange;        // UV transform uniform (offset + scale)
  TextureHandle dst;         // Destination texture
  SampleFilter sampleFilter = SampleFilter::Nearest;
  CopyDomain domain = CopyDomain::Display;
  Range toneRange; // HdrDisplay: the tone curve's three vec4f rows
};

bool needs_conversion(GXTexFmt fmt);

void initialize();
void shutdown();
void run(const wgpu::CommandEncoder& cmd, const ConvRequest& req);
void blit(const wgpu::CommandEncoder& cmd, const ConvRequest& req);

bool snapshot_depth_supported() noexcept;
void snapshot_depth(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t msaaSamples,
                    const wgpu::TextureView& dst);

} // namespace aurora::gfx::tex_copy_conv
