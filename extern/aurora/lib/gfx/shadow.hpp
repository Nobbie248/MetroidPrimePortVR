#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h>

namespace aurora::gx {
struct DrawData;
} // namespace aurora::gx

// Port extension: the sun's shadow map (GXPortSetShadowFrame). One orthographic depth map around
// the camera, drawn after the frame's casters (GXPortRenderShadowMap) and read by the next frame's
// receivers, so a draw's caster and receiver matrices both come from set_frame.
namespace aurora::gfx::shadow {
constexpr wgpu::TextureFormat MapFormat = wgpu::TextureFormat::Depth32Float;

// The receivers' uniform (ShaderInfo::usesShadow), as set_frame fills it: view -> the map's clip
// space for the casters (this frame's map) and the receivers (the previous frame's), as rows; the
// view-space direction to the sun with the normal offset in w; the extra light's colour with the
// PCF step (uv) in w.
struct Uniform {
  float caster[4][4];
  float receiver[4][4];
  float dir[4];
  float color[4];
};
static_assert(sizeof(Uniform) == 10 * 16);

// Registers the map's encoder task (game thread); false if it could not be.
bool ensure_task();
// Sets the frame's sun (FIFO processor): worldToView as GX's view matrix (3 rows of 4), sunDir the
// way its light travels in world space, radius the extent around the camera (<= 0: none). False
// when there is no sun; out is filled otherwise.
bool set_frame(const float worldToView[3][4], const float sunDir[3], float radius, const float color[3],
               Uniform& out);
// Whether a world box [min, max] reaches into the map set_frame would make of the same arguments
// (any thread).
bool box_casts(const float worldToView[3][4], const float sunDir[3], float radius, const float min[3],
               const float max[3]);
// The map's centre in world space for the same arguments (any thread).
void box_center(const float worldToView[3][4], float radius, float center[3]);
// How many draws the last recorded map had (any thread).
uint32_t last_caster_count();
// A draw that casts into this frame's map (FIFO processor).
void add_caster(const gx::DrawData& draw);
// Records the map's pass over the casters added since the last (FIFO processor).
bool record();
// The map and its comparison sampler, for the receivers' bind group.
const wgpu::TextureView& map_view();
const wgpu::Sampler& sampler();
void shutdown();
} // namespace aurora::gfx::shadow
