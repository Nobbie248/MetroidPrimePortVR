#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h>

namespace aurora::gx {
struct DrawData;
} // namespace aurora::gx

// Port extension: Remastered's actor shadow (CSimpleShadow decal mode 1). Each shadowed actor draws
// its models depth only into its own 256x256 orthographic depth map (a layer of one array); the
// receivers' geometry is then redrawn with a decal shader that compares against that layer.
namespace aurora::gfx::actor_shadow {
constexpr wgpu::TextureFormat MapFormat = wgpu::TextureFormat::Depth32Float;
constexpr uint32_t MapSize = 256;
constexpr uint32_t MaxLayers = 16;

// Registers the maps' encoder task (game thread); false if it could not be.
bool ensure_task();
// A draw that casts into a layer's map (FIFO processor).
void add_caster(uint32_t layer, const gx::DrawData& draw);
// Records the maps' pass over the casters added since the last (FIFO processor).
bool record();
// The maps (a 2D array, a layer an actor) and their comparison sampler, for the decals' bind group.
const wgpu::TextureView& map_view();
const wgpu::Sampler& sampler();
void shutdown();
} // namespace aurora::gfx::actor_shadow
