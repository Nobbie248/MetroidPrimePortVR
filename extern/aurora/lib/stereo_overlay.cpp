#include "stereo_overlay.hpp"

#include "internal.hpp"
#include "webgpu/gpu.hpp"

#include <atomic>

namespace aurora::stereo_overlay {
namespace {

Module Log("aurora::stereo_overlay");

std::atomic_bool g_layerMode{false};

// The transparent panel image, re-made when the requested size changes and
// cleared once per creation.
struct TransparentLayer {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size{};
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  bool cleared = false;
};
TransparentLayer g_transparent;

} // namespace

wgpu::CommandBuffer prepare(ImDrawData* /*drawData*/, float /*widthFraction*/) noexcept { return {}; }

void set_layer_mode(bool enabled) noexcept { g_layerMode.store(enabled, std::memory_order_relaxed); }

bool layer_mode() noexcept { return g_layerMode.load(std::memory_order_relaxed); }

bool layer_source(const wgpu::CommandEncoder& encoder, uint32_t width, uint32_t height,
                  stereo::EyeImage& out) noexcept {
  if (width == 0 || height == 0 || !webgpu::g_device) {
    return false;
  }
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (!g_transparent.texture || g_transparent.size.width != width || g_transparent.size.height != height ||
      g_transparent.format != format) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Stereo panel layer (transparent)",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                 wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {width, height, 1},
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    g_transparent.texture = webgpu::g_device.CreateTexture(&descriptor);
    if (!g_transparent.texture) {
      Log.error("Could not create the transparent panel layer image ({}x{})", width, height);
      g_transparent = {};
      return false;
    }
    g_transparent.view = g_transparent.texture.CreateView();
    g_transparent.size = {width, height, 1};
    g_transparent.format = format;
    g_transparent.cleared = false;
  }
  if (!g_transparent.cleared) {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = g_transparent.view,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue = {0.0, 0.0, 0.0, 0.0},
        },
    };
    const wgpu::RenderPassDescriptor descriptor{
        .label = "Stereo panel layer clear",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    const auto pass = encoder.BeginRenderPass(&descriptor);
    pass.End();
    g_transparent.cleared = true;
  }
  out.texture = &g_transparent.texture;
  out.view = &g_transparent.view;
  out.size = g_transparent.size;
  out.format = g_transparent.format;
  return true;
}

void shutdown() noexcept { g_transparent = {}; }

} // namespace aurora::stereo_overlay
