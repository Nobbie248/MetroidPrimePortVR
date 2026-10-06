#include "stereo_eyes.hpp"

#include "stereo_foveation.hpp"
#include "stereo_multiview.hpp"
#include "../internal.hpp"

namespace aurora::gfx {
namespace {
constexpr Module Log{"aurora::gfx::stereo"};
std::array<StereoEyeTarget, AURORA_STEREO_EYE_COUNT> g_eyeTargets;
StereoMultiviewTarget g_multiviewTarget;
bool g_multiview = false;

// One texture with a layer per eye (single-sampled: stereo_multiview::usable),
// viewed per eye and whole.
struct LayeredTexture {
  wgpu::Texture texture;
  wgpu::TextureView arrayView;
  std::array<wgpu::TextureView, AURORA_STEREO_EYE_COUNT> layerViews;
};

LayeredTexture make_layered(uint32_t width, uint32_t height, wgpu::TextureFormat format, wgpu::TextureUsage usage,
                            const char* label) noexcept {
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = usage,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {width, height, AURORA_STEREO_EYE_COUNT},
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  LayeredTexture layered;
  layered.texture = webgpu::g_device.CreateTexture(&descriptor);
  const wgpu::TextureViewDescriptor arrayDescriptor{
      .label = label,
      .format = format,
      .dimension = wgpu::TextureViewDimension::e2DArray,
      .arrayLayerCount = AURORA_STEREO_EYE_COUNT,
  };
  layered.arrayView = layered.texture.CreateView(&arrayDescriptor);
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const wgpu::TextureViewDescriptor layerDescriptor{
        .label = label,
        .format = format,
        .dimension = wgpu::TextureViewDimension::e2D,
        .baseArrayLayer = eye,
        .arrayLayerCount = 1,
    };
    layered.layerViews[eye] = layered.texture.CreateView(&layerDescriptor);
  }
  return layered;
}

bool ensure_multiview_targets(const std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT>& eyes) noexcept {
  const uint32_t width = eyes[0].width;
  const uint32_t height = eyes[0].height;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const auto& left = g_eyeTargets[0];
  if (g_multiview && left.valid() && left.width == width && left.height == height && left.color.format == format &&
      left.depth.format == depthFormat) {
    return true;
  }
  // The density maps' bindings hold the old targets (stereo_foveation.hpp).
  stereo_foveation::release();
  const auto color = make_layered(width, height, format,
                                  wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                                      wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,
                                  "Stereo eye targets (multiview)");
  const auto depth = make_layered(width, height, depthFormat,
                                  wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
                                  "Stereo eye depth (multiview)");
  if (!color.texture || !depth.texture) {
    release_stereo_eye_targets();
    return false;
  }
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    auto& target = g_eyeTargets[eye];
    target = {};
    target.color = {
        .texture = color.texture,
        .view = color.layerViews[eye],
        .size = {width, height, 1},
        .format = format,
    };
    target.depth = {
        .texture = depth.texture,
        .view = depth.layerViews[eye],
        .size = {width, height, 1},
        .format = depthFormat,
    };
    target.width = width;
    target.height = height;
    target.sampleCount = 1;
    target.layer = eye;
  }
  g_multiviewTarget = {.colorView = color.arrayView, .depthView = depth.arrayView};
  g_multiview = true;
  Log.info("Stereo eye replay targets {}x{}, both eyes as layers (multiview)", width, height);
  return true;
}
} // namespace

bool ensure_stereo_eye_targets(const std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT>& eyes,
                               bool multiview) noexcept {
  for (const auto& eye : eyes) {
    if (eye.width == 0 || eye.height == 0) {
      return false;
    }
  }
  if (multiview) {
    return ensure_multiview_targets(eyes);
  }
  if (g_multiview) {
    // Leaving multiview: separate targets again.
    release_stereo_eye_targets();
  }
  const uint32_t sampleCount = webgpu::g_graphicsConfig.msaaSamples;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const uint32_t width = eyes[eye].width;
    const uint32_t height = eyes[eye].height;
    auto& target = g_eyeTargets[eye];
    if (target.valid() && target.width == width && target.height == height && target.sampleCount == sampleCount &&
        target.color.format == format && target.depth.format == webgpu::g_graphicsConfig.depthFormat) {
      continue;
    }
    stereo_foveation::release();
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

const StereoMultiviewTarget& stereo_multiview_target() noexcept { return g_multiviewTarget; }

void release_stereo_eye_targets() noexcept {
  // Before the targets go: a map's binding keeps its view, and so the texture, alive.
  stereo_foveation::release();
  for (auto& target : g_eyeTargets) {
    target = {};
  }
  g_multiviewTarget = {};
  g_multiview = false;
}

} // namespace aurora::gfx
