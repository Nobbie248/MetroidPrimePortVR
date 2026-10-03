#include "stereo_eyes.hpp"

#include "../internal.hpp"

namespace aurora::gfx {
namespace {
constexpr Module Log{"aurora::gfx::stereo"};
std::array<StereoEyeTarget, AURORA_STEREO_EYE_COUNT> g_eyeTargets;
} // namespace

bool ensure_stereo_eye_targets(const std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT>& eyes) noexcept {
  const uint32_t sampleCount = webgpu::g_graphicsConfig.msaaSamples;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const uint32_t width = eyes[eye].width;
    const uint32_t height = eyes[eye].height;
    if (width == 0 || height == 0) {
      return false;
    }
    auto& target = g_eyeTargets[eye];
    if (target.valid() && target.width == width && target.height == height && target.sampleCount == sampleCount &&
        target.color.format == format && target.depth.format == webgpu::g_graphicsConfig.depthFormat) {
      continue;
    }
    target = {};
    target.color = webgpu::create_render_texture(width, height, sampleCount > 1);
    if (sampleCount > 1) {
      target.resolved = webgpu::create_render_texture(width, height, false);
    }
    target.depth = webgpu::create_depth_texture(width, height);
    target.width = width;
    target.height = height;
    target.sampleCount = sampleCount;
    Log.info("Stereo eye {} replay target {}x{} ({}x MSAA)", eye, width, height, sampleCount);
    if (!target.valid()) {
      target = {};
      return false;
    }
  }
  return true;
}

const StereoEyeTarget& stereo_eye_target(uint32_t eye) noexcept { return g_eyeTargets[eye]; }

void release_stereo_eye_targets() noexcept {
  for (auto& target : g_eyeTargets) {
    target = {};
  }
}

} // namespace aurora::gfx
