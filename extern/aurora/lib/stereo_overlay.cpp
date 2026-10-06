#include "stereo_overlay.hpp"

#include "internal.hpp"
#include "webgpu/gpu.hpp"

#include <aurora/aurora.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace aurora::stereo_overlay {
namespace {

Module Log("aurora::stereo_overlay");

std::atomic_bool g_layerMode{false};

// The host's latest panel image, as handed over (aurora_set_stereo_panel_image).
struct HostImage {
  std::mutex mutex;
  std::vector<uint8_t> rgba;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t generation = 0;
};

HostImage& Host() {
  static HostImage image;
  return image;
}

// A panel image on the GPU: the host's (uploaded when its generation changes)
// or the transparent stand-in (cleared once per creation).
struct LayerImage {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size{};
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  uint64_t generation = 0;
  bool ready = false;
};
LayerImage g_hostImage;
LayerImage g_transparent;
bool g_loggedFormat = false;

bool ensure_texture(LayerImage& image, uint32_t width, uint32_t height, wgpu::TextureFormat format,
                    const char* label) noexcept {
  if (image.texture && image.size.width == width && image.size.height == height && image.format == format) {
    return true;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst |
               wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {width, height, 1},
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  image = {};
  image.texture = webgpu::g_device.CreateTexture(&descriptor);
  if (!image.texture) {
    Log.error("Could not create the panel layer image ({}x{})", width, height);
    image = {};
    return false;
  }
  image.view = image.texture.CreateView();
  image.size = {width, height, 1};
  image.format = format;
  return true;
}

void use(const LayerImage& image, stereo::EyeImage& out) noexcept {
  out.texture = &image.texture;
  out.view = &image.view;
  out.size = image.size;
  out.format = image.format;
}

// The host's image in the panel's format, or false when there is none of that
// size or the format is not an 8-bit RGBA or BGRA one.
bool host_source(uint32_t width, uint32_t height, wgpu::TextureFormat format, stereo::EyeImage& out) noexcept {
  const bool bgra = format == wgpu::TextureFormat::BGRA8Unorm || format == wgpu::TextureFormat::BGRA8UnormSrgb;
  const bool rgba = format == wgpu::TextureFormat::RGBA8Unorm || format == wgpu::TextureFormat::RGBA8UnormSrgb;
  std::vector<uint8_t> pixels;
  uint64_t generation = 0;
  {
    auto& host = Host();
    std::lock_guard lock(host.mutex);
    if (host.rgba.empty() || host.width != width || host.height != height) {
      return false;
    }
    generation = host.generation;
    if (g_hostImage.ready && g_hostImage.generation == generation && g_hostImage.size.width == width &&
        g_hostImage.size.height == height && g_hostImage.format == format) {
      use(g_hostImage, out);
      return true;
    }
    pixels = host.rgba;
  }
  if (!bgra && !rgba) {
    if (!g_loggedFormat) {
      g_loggedFormat = true;
      Log.warn("The panel layer's format {} takes no 8-bit RGBA image; it stays transparent",
               static_cast<uint32_t>(format));
    }
    return false;
  }
  if (!ensure_texture(g_hostImage, width, height, format, "Stereo panel layer (host image)")) {
    return false;
  }
  if (bgra) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      std::swap(pixels[i], pixels[i + 2]);
    }
  }
  const wgpu::TexelCopyTextureInfo destination{.texture = g_hostImage.texture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = width * 4, .rowsPerImage = height};
  const wgpu::Extent3D size{width, height, 1};
  webgpu::g_queue.WriteTexture(&destination, pixels.data(), pixels.size(), &layout, &size);
  g_hostImage.generation = generation;
  g_hostImage.ready = true;
  use(g_hostImage, out);
  return true;
}

} // namespace

wgpu::CommandBuffer prepare(ImDrawData* /*drawData*/, float /*widthFraction*/) noexcept { return {}; }

void set_layer_mode(bool enabled) noexcept { g_layerMode.store(enabled, std::memory_order_relaxed); }

bool layer_mode() noexcept { return g_layerMode.load(std::memory_order_relaxed); }

void set_image(const void* rgba8, uint32_t width, uint32_t height) noexcept {
  auto& host = Host();
  std::lock_guard lock(host.mutex);
  ++host.generation;
  if (rgba8 == nullptr || width == 0 || height == 0) {
    host.rgba.clear();
    host.width = host.height = 0;
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(rgba8);
  host.rgba.assign(bytes, bytes + static_cast<size_t>(width) * height * 4);
  host.width = width;
  host.height = height;
}

bool layer_source(const wgpu::CommandEncoder& encoder, uint32_t width, uint32_t height,
                  stereo::EyeImage& out) noexcept {
  if (width == 0 || height == 0 || !webgpu::g_device) {
    return false;
  }
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (host_source(width, height, format, out)) {
    return true;
  }
  if (!ensure_texture(g_transparent, width, height, format, "Stereo panel layer (transparent)")) {
    return false;
  }
  if (!g_transparent.ready) {
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
    g_transparent.ready = true;
  }
  use(g_transparent, out);
  return true;
}

void shutdown() noexcept {
  g_hostImage = {};
  g_transparent = {};
}

} // namespace aurora::stereo_overlay

// aurora.h
void aurora_set_stereo_panel_image(const void* rgba8, uint32_t width, uint32_t height) {
  aurora::stereo_overlay::set_image(rgba8, width, height);
}
