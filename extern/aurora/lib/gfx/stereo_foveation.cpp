// SPDX-License-Identifier: GPL-3.0-or-later
#include "stereo_foveation.hpp"

#include "foveation.hpp"
#include "stereo_eyes.hpp"
#include "stereo_multiview.hpp"
#include "../internal.hpp"
#include "../webgpu/fdm.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace aurora::gfx::stereo_foveation {
namespace {
constexpr Module Log{"aurora::gfx::foveation"};

std::atomic<uint32_t> g_level{0};

// One bound map: the eye texture it serves, the render-attachment-only view of
// it the eye passes render through, and what the map was built for.
struct Slot {
  wgpu::Texture texture;
  wgpu::TextureView view;
  uint32_t layers = 0;
  uint64_t map = 0;        // bound to `view`
  uint64_t pendingMap = 0; // uploaded, bound once its upload has completed
  // Eye size, level, layers, then each layer's field of view: the tangents in
  // hundredths, finer than a map texel yet coarse enough to ignore pose noise.
  std::array<int32_t, 4 + 4 * AURORA_STEREO_EYE_COUNT> key{};
};
std::array<Slot, AURORA_STEREO_EYE_COUNT> g_slots; // multiview uses the first only

void release_maps(Slot& slot) noexcept {
  if (slot.pendingMap != 0) {
    webgpu::fdm::release_map(slot.pendingMap);
    slot.pendingMap = 0;
  }
  if (slot.map != 0) {
    webgpu::fdm::release_map(slot.map);
    slot.map = 0;
  }
  slot.key = {};
}

void release_slot(Slot& slot) noexcept {
  release_maps(slot);
  slot.view = nullptr;
  slot.texture = nullptr;
  slot.layers = 0;
}

// MP_FOVEATION_LAYERS=1: one map for both views of a multiview pass, each texel the finer of
// the two eyes' densities. A diagnostic, to price the layered map against a shared one.
bool single_layer_map() noexcept {
  static const bool single = [] {
    const char* value = std::getenv("MP_FOVEATION_LAYERS");
    return value != nullptr && value[0] == '1';
  }();
  return single;
}

int32_t hundredths(float value) noexcept { return static_cast<int32_t>(std::lround(value * 100.0f)); }

const char* level_name(foveation::Level level) noexcept {
  switch (level) {
  case foveation::Level::Low:
    return "low";
  case foveation::Level::Medium:
    return "medium";
  case foveation::Level::High:
    return "high";
  default:
    return "off";
  }
}

foveation::EyeFov eye_fov(const StereoEyeParams& eye) noexcept {
  // StereoEyeParams::projection is AuroraStereoEye::projection's sixteen floats, row-major
  // (stereo_host.cpp load_eye copies them as one block).
  static_assert(sizeof(eye.projection) == 16 * sizeof(float));
  return foveation::fov_from_projection(reinterpret_cast<const float*>(&eye.projection));
}

// Gives `slot` a map for `color`, the eye texture of `width` x `height` whose first `layers` layers
// show eyes[firstEye...], and returns the view to render through, or an empty view while no map is
// bound to it.
wgpu::TextureView prepare_slot(Slot& slot, const webgpu::TextureWithSampler& color, uint32_t width,
                               uint32_t height, uint32_t layers, uint32_t firstEye,
                               const std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT>& eyes,
                               foveation::Level level, const char* label) noexcept {
  if (slot.texture.Get() != color.texture.Get() || slot.layers != layers || !slot.view) {
    release_slot(slot);
    // An explicit descriptor: Dawn hands the same object back for every default view of a texture,
    // and this one must differ from the view the copies and the blit read.
    const wgpu::TextureViewDescriptor descriptor{
        .label = label,
        .format = color.format,
        .dimension = layers > 1 ? wgpu::TextureViewDimension::e2DArray : wgpu::TextureViewDimension::e2D,
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = layers,
        .usage = wgpu::TextureUsage::RenderAttachment,
    };
    slot.view = color.texture.CreateView(&descriptor);
    slot.texture = color.texture;
    slot.layers = layers;
    if (!slot.view) {
      Log.warn("Eye foveation: no render view of the {}x{} eye targets", width, height);
      return {};
    }
  }
  std::array<int32_t, 4 + 4 * AURORA_STEREO_EYE_COUNT> key{};
  key[0] = static_cast<int32_t>(width);
  key[1] = static_cast<int32_t>(height);
  key[2] = static_cast<int32_t>(level);
  key[3] = static_cast<int32_t>(layers);
  for (uint32_t layer = 0; layer < layers; ++layer) {
    const foveation::EyeFov fov = eye_fov(eyes[firstEye + layer]);
    key[4 + layer * 4 + 0] = hundredths(fov.tanLeft);
    key[4 + layer * 4 + 1] = hundredths(fov.tanRight);
    key[4 + layer * 4 + 2] = hundredths(fov.tanDown);
    key[4 + layer * 4 + 3] = hundredths(fov.tanUp);
  }
  if (key != slot.key) {
    if (slot.pendingMap != 0) {
      webgpu::fdm::release_map(slot.pendingMap);
      slot.pendingMap = 0;
    }
    slot.key = key;
    const uint32_t texel = webgpu::fdm::texel_size();
    foveation::Map layerMap;
    std::vector<uint8_t> data;
    uint32_t mapWidth = 0;
    uint32_t mapHeight = 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      foveation::build(width, height, texel, eye_fov(eyes[firstEye + layer]), level, layerMap);
      mapWidth = layerMap.width;
      mapHeight = layerMap.height;
      data.insert(data.end(), layerMap.rg8.begin(), layerMap.rg8.end());
    }
    uint32_t mapLayers = layers;
    if (layers > 1 && single_layer_map()) {
      const size_t layerBytes = static_cast<size_t>(mapWidth) * mapHeight * 2;
      for (uint32_t layer = 1; layer < layers; ++layer) {
        for (size_t i = 0; i < layerBytes; ++i) {
          data[i] = std::max(data[i], data[layer * layerBytes + i]);
        }
      }
      data.resize(layerBytes);
      mapLayers = 1;
    }
    slot.pendingMap = webgpu::fdm::create_map(mapWidth, mapHeight, mapLayers, data.data());
    if (slot.pendingMap == 0) {
      Log.warn("Eye foveation {}: no {}x{} density map for the {}x{} eyes", level_name(level), mapWidth, mapHeight,
               width, height);
    } else {
      Log.info("Eye foveation {}: {}x{} density map x{}, {} pixels per texel, for the {}x{} eyes",
               level_name(level), mapWidth, mapHeight, mapLayers, texel, width, height);
    }
  }
  if (slot.pendingMap != 0 && webgpu::fdm::map_ready(slot.pendingMap)) {
    // Bound only once the GPU has it: the previous map keeps the eyes foveated until then, and a
    // pass recorded with a map still uploading would run unfoveated.
    if (webgpu::fdm::bind(slot.view, slot.pendingMap)) {
      if (slot.map != 0) {
        webgpu::fdm::release_map(slot.map);
      }
      slot.map = slot.pendingMap;
    } else {
      Log.warn("Eye foveation: the density map cannot be bound to the {}x{} eye targets", width, height);
      webgpu::fdm::release_map(slot.pendingMap);
    }
    slot.pendingMap = 0;
  }
  return slot.map != 0 ? slot.view : wgpu::TextureView{};
}
} // namespace

void set_level(uint32_t level) noexcept {
  g_level.store(std::min(level, foveation::kLevelCount - 1), std::memory_order_relaxed);
}

uint32_t level() noexcept { return g_level.load(std::memory_order_relaxed); }

FrameViews prepare(const std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT>& eyes, bool multiview) noexcept {
  FrameViews views;
  const auto level = static_cast<foveation::Level>(stereo_foveation::level());
  if (level == foveation::Level::Off || !webgpu::fdm::available()) {
    // Off keeps the maps bound: the eye passes render through the targets' own views, and the
    // maps go with the targets.
    return views;
  }
  const auto& left = stereo_eye_target(0);
  if (!left.valid() || left.sampleCount > 1) {
    // Multisampled eyes resolve at full resolution anyway.
    return views;
  }
  if (multiview) {
    release_slot(g_slots[1]);
    views.multiviewColorView = prepare_slot(g_slots[0], left.color, left.width, left.height,
                                            stereo_multiview::kViewCount, 0, eyes, level, "Stereo eyes (foveated)");
    views.foveated = static_cast<bool>(views.multiviewColorView);
    return views;
  }
  bool bound = true;
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const auto& target = stereo_eye_target(eye);
    views.eyeColorViews[eye] = prepare_slot(g_slots[eye], target.color, target.width, target.height, 1, eye, eyes,
                                            level, eye == 0 ? "Left eye (foveated)" : "Right eye (foveated)");
    bound = bound && views.eyeColorViews[eye];
  }
  // Both eyes or neither, so they always match.
  views.foveated = bound;
  if (!bound) {
    views.eyeColorViews = {};
  }
  return views;
}

void release() noexcept {
  for (auto& slot : g_slots) {
    release_slot(slot);
  }
}

} // namespace aurora::gfx::stereo_foveation
