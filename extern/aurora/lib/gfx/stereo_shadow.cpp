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
};

absl::flat_hash_map<const TextureRef*, Entry> g_entries;
bool g_active = false;
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
  return &it->second;
}

Entry& ensure(const TextureHandle& mono, const std::array<EyeSize, 2>& sizes) noexcept {
  auto& entry = g_entries[mono.get()];
  const auto current = entry.mono.lock();
  bool reusable = current == mono;
  for (uint32_t eye = 0; eye < 2 && reusable; ++eye) {
    reusable = entry.eyes[eye] && fits(*entry.eyes[eye], *mono, sizes[eye]);
  }
  if (!reusable) {
    entry.mono = mono;
    entry.eyes = {make_like(*mono, sizes[0]), make_like(*mono, sizes[1])};
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

void begin_frame(bool immersive) noexcept {
  g_active = immersive;
  ++g_epoch;
  if ((++g_frames % 600) == 0) {
    for (auto it = g_entries.begin(); it != g_entries.end();) {
      if (it->second.mono.expired()) {
        g_entries.erase(it++);
      } else {
        ++it;
      }
    }
  }
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

void shutdown() noexcept {
  g_entries.clear();
  g_active = false;
  ++g_epoch;
}

} // namespace aurora::gfx::stereo_shadow
