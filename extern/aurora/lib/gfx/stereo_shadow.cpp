#include "stereo_shadow.hpp"

#include "resources.hpp"
#include "stereo_replay.hpp"
#include "texture.hpp"
#include "../webgpu/gpu.hpp"

#include <absl/container/flat_hash_map.h>

#include <memory>

namespace aurora::gfx::stereo_shadow {
namespace {

struct Entry {
  std::weak_ptr<TextureRef> mono;
  EyeTextures eyes;
  bool valid = false;
  // The eyes are the two layers of one texture (multiview, stereo_multiview.hpp).
  bool layered = false;
  // The last frame (g_frames) the stand-ins were written or looked up.
  uint32_t lastUse = 0;
};

// Frames a stand-in survives without being written or looked up.
constexpr uint32_t kIdleFrames = 4;

absl::flat_hash_map<const TextureRef*, Entry> g_entries;
bool g_active = false;
bool g_multiview = false;
uint64_t g_epoch = 1;
uint32_t g_frames = 0;

// An eye texture is `size` across and in `mono`'s format.
bool fits(const TextureRef& eye, const TextureRef& mono, EyeSize size) noexcept {
  return eye.size.width == size.width && eye.size.height == size.height && eye.format == mono.format;
}

// A texture made the way the mono one was, `size` across: copies of the EFB in
// the surface format are render textures, converted copies (I4, C8, the RGBA8
// palette results) are conversion textures in their own format.
TextureHandle make_like(const TextureRef& mono, EyeSize size) noexcept {
  const bool render =
      mono.attachmentTextureView && mono.format == webgpu::g_graphicsConfig.surfaceConfiguration.format;
  return render ? new_render_texture(size.width, size.height, mono.gxFormat, "Stereo eye copy")
                : new_conv_texture(size.width, size.height, mono.gxFormat, "Stereo eye converted copy");
}

// Multiview: both eyes' textures as the two layers of one texture made like
// `mono`, `size` across. Each eye is a texture of its own layer (so copies and
// palette conversions write and read it as before), and both carry the view of
// both layers that a multiview draw binds (stereo_multiview.hpp).
EyeTextures make_layered_like(const TextureRef& mono, EyeSize size) noexcept {
  const wgpu::TextureDescriptor descriptor{
      .label = "Stereo eye copies (multiview)",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {size.width, size.height, 2},
      .format = mono.format,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = webgpu::g_device.CreateTexture(&descriptor);
  const wgpu::TextureViewDescriptor arrayDescriptor{
      .label = "Stereo eye copies array view",
      .format = mono.format,
      .dimension = wgpu::TextureViewDimension::e2DArray,
      .arrayLayerCount = 2,
  };
  auto arrayView = texture.CreateView(&arrayDescriptor);
  EyeTextures eyes;
  for (uint32_t eye = 0; eye < 2; ++eye) {
    const wgpu::TextureViewDescriptor layerDescriptor{
        .label = "Stereo eye copy layer view",
        .format = mono.format,
        .dimension = wgpu::TextureViewDimension::e2D,
        .baseArrayLayer = eye,
        .arrayLayerCount = 1,
    };
    auto view = texture.CreateView(&layerDescriptor);
    eyes[eye] = std::make_shared<TextureRef>(texture, view, view, wgpu::Extent3D{size.width, size.height, 1},
                                             mono.format, 1, mono.gxFormat);
    eyes[eye]->arrayTextureView = arrayView;
  }
  return eyes;
}

// The eye's size for a mono texture `mono` across, made from a source
// `monoReference` across whose eye version is `eyeReference` across.
EyeSize eye_size(const TextureRef& mono, EyeSize monoReference, EyeSize eyeReference) noexcept {
  const uint32_t maxDimension = detail::resources().limits.maxTextureDimension2D;
  return {
      .width = stereo_replay::eye_copy_extent(mono.size.width, monoReference.width, eyeReference.width, maxDimension),
      .height =
          stereo_replay::eye_copy_extent(mono.size.height, monoReference.height, eyeReference.height, maxDimension),
  };
}

Entry* find(const TextureRef* mono) noexcept {
  if (mono == nullptr) {
    return nullptr;
  }
  const auto it = g_entries.find(mono);
  if (it == g_entries.end()) {
    return nullptr;
  }
  if (it->second.mono.expired()) {
    // The address now belongs to another texture.
    g_entries.erase(it);
    return nullptr;
  }
  it->second.lastUse = g_frames;
  return &it->second;
}

Entry& ensure(const TextureHandle& mono, const std::array<EyeSize, 2>& sizes) noexcept {
  auto& entry = g_entries[mono.get()];
  entry.lastUse = g_frames;
  const auto current = entry.mono.lock();
  // Layers share a size: multiview eyes always do (stereo_multiview::usable).
  const bool layered = g_multiview && sizes[0].width == sizes[1].width && sizes[0].height == sizes[1].height;
  bool reusable = current == mono && entry.layered == layered;
  for (uint32_t eye = 0; eye < 2 && reusable; ++eye) {
    reusable = entry.eyes[eye] && fits(*entry.eyes[eye], *mono, sizes[eye]);
  }
  if (!reusable) {
    entry.mono = mono;
    entry.eyes = layered ? make_layered_like(*mono, sizes[0])
                         : EyeTextures{make_like(*mono, sizes[0]), make_like(*mono, sizes[1])};
    entry.layered = layered;
    entry.valid = false;
  }
  return entry;
}

void set_valid(Entry& entry, bool valid) noexcept {
  if (entry.valid != valid) {
    entry.valid = valid;
    ++g_epoch;
  }
}

} // namespace

void begin_frame(bool immersive, bool multiview) noexcept {
  g_active = immersive;
  g_multiview = immersive && multiview;
  ++g_epoch;
  ++g_frames;
  // Stand-ins are eye-sized, many times the copy they shadow, so they go as soon as
  // the copy is gone or idle. A copy whose size follows the view (Metroid Prime's fog
  // volumes, gx copy_tex) gets a new mono texture at nearly every size change, freed
  // once 16 newer sizes exist (trim_copy_sizes); a sweep every 600 frames kept
  // hundreds of their stand-ins alive in Magmoor, and Dawn's pools keep the peak, so
  // the Quest game grew past 3.8 GB until the system killed it.
  absl::erase_if(g_entries, [](const auto& item) {
    return item.second.mono.expired() || g_frames - item.second.lastUse > kIdleFrames;
  });
}

bool active() noexcept { return g_active; }

uint64_t epoch() noexcept { return g_epoch; }

EyeTextures copy_targets(const TextureHandle& mono, EyeSize efb, const std::array<EyeSize, 2>& eyeTargets) noexcept {
  if (!mono) {
    return {};
  }
  auto& entry = ensure(mono, {eye_size(*mono, efb, eyeTargets[0]), eye_size(*mono, efb, eyeTargets[1])});
  set_valid(entry, true);
  return entry.eyes;
}

void invalidate(const TextureRef* mono) noexcept {
  if (Entry* entry = find(mono)) {
    set_valid(*entry, false);
  }
}

bool palette_conv(const tex_palette_conv::ConvRequest& mono,
                  std::array<tex_palette_conv::ConvRequest, 2>& eyes) noexcept {
  const Entry* source = g_active ? find(mono.src.get()) : nullptr;
  if (source == nullptr || !source->valid || !mono.dst) {
    invalidate(mono.dst.get());
    return false;
  }
  // ensure() may rehash the map: take the eye sources first.
  const EyeTextures sourceEyes = source->eyes;
  const EyeSize monoSource{mono.src->size.width, mono.src->size.height};
  std::array<EyeSize, 2> sizes{};
  for (uint32_t eye = 0; eye < 2; ++eye) {
    sizes[eye] = eye_size(*mono.dst, monoSource, {sourceEyes[eye]->size.width, sourceEyes[eye]->size.height});
  }
  auto& destination = ensure(mono.dst, sizes);
  for (uint32_t eye = 0; eye < 2; ++eye) {
    eyes[eye] = tex_palette_conv::ConvRequest{
        .variant = mono.variant,
        .src = sourceEyes[eye],
        .dst = destination.eyes[eye],
        .tlut = mono.tlut,
    };
  }
  set_valid(destination, true);
  return true;
}

const TextureRef* eye_texture(const TextureRef* mono, uint32_t eye) noexcept {
  const Entry* entry = find(mono);
  return entry != nullptr && entry->valid ? entry->eyes[eye].get() : nullptr;
}

WGPUTextureView layered_view(const TextureRef* mono) noexcept {
  const Entry* entry = find(mono);
  if (entry == nullptr || !entry->valid || !entry->layered || !entry->eyes[0]) {
    return nullptr;
  }
  return entry->eyes[0]->arrayTextureView.Get();
}

void shutdown() noexcept {
  g_entries.clear();
  g_active = false;
  ++g_epoch;
}

} // namespace aurora::gfx::stereo_shadow
