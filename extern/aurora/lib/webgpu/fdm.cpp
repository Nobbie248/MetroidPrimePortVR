// SPDX-License-Identifier: GPL-3.0-or-later
#include "fdm.hpp"

#include "../internal.hpp"
#include "gpu.hpp"

#include <algorithm>
#include <bit>

// PrimedGun's patched Dawn (quest/dawn/apply.py) ships the fragment density map entry points
// (quest/dawn/aurora_fdm.inc) together with its multiview API, so the multiview macro says whether
// this build links them; the ABI version is compared at runtime (request). Any other Dawn gets the
// stubs below.
#if defined(__ANDROID__) && defined(WEBGPU_DAWN) && defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)
#include <aurora/dawn_fdm_abi.h>
#define AURORA_FDM_SUPPORTED 1
#endif

namespace aurora::webgpu::fdm {
namespace {
Module Log("aurora::webgpu::fdm");

bool g_requested = false;
bool g_available = false;
uint32_t g_texelSize = 0;

// Meta's own maps use 32: rings smooth enough, in a map of a few hundred texels.
constexpr uint32_t kPreferredTexelSize = 32;
} // namespace

void request(bool wanted) noexcept {
  g_requested = wanted;
#ifdef AURORA_FDM_SUPPORTED
  if (wanted && AuroraDawnFdmVersion() != AURORA_DAWN_FDM_ABI) {
    Log.warn("Fragment density maps: the linked Dawn carries ABI {} where Aurora expects {}; not requested "
             "(rebuild the package with quest/Build-QuestDawn.ps1)",
             AuroraDawnFdmVersion(), static_cast<uint32_t>(AURORA_DAWN_FDM_ABI));
    g_requested = false;
  }
  AuroraDawnFdmRequest(g_requested ? 1 : 0);
#endif
}

void device_created() noexcept {
  g_available = false;
  g_texelSize = 0;
#ifdef AURORA_FDM_SUPPORTED
  AuroraDawnFdmCaps caps{};
  if (g_requested && AuroraDawnFdmQuery(g_device.Get(), &caps) != 0) {
    // Density texels cover a power-of-two area within the device's range in each direction.
    const uint32_t smallest = std::max({caps.minTexelWidth, caps.minTexelHeight, 1u});
    const uint32_t largest = std::max(std::min(caps.maxTexelWidth, caps.maxTexelHeight), smallest);
    g_texelSize = std::bit_ceil(std::clamp(kPreferredTexelSize, smallest, largest));
    g_available = true;
    Log.info("Fragment density maps: enabled, {}x{} to {}x{} pixels per texel, using {}", caps.minTexelWidth,
             caps.minTexelHeight, caps.maxTexelWidth, caps.maxTexelHeight, g_texelSize);
  } else if (g_requested) {
    Log.warn("Fragment density maps: unavailable; the device lacks VK_EXT_fragment_density_map for "
             "non-subsampled images, or Dawn does not render through dynamic rendering");
  }
#else
  if (g_requested) {
    Log.warn("Fragment density maps: unavailable; this build links a Dawn without PrimedGun's patches "
             "(quest/Build-QuestDawn.ps1)");
  }
#endif
}

bool available() noexcept { return g_available; }

uint32_t texel_size() noexcept { return g_texelSize; }

uint64_t create_map(uint32_t width, uint32_t height, uint32_t layers, const uint8_t* rg8) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available) {
    return AuroraDawnFdmCreateMap(g_device.Get(), width, height, layers, rg8);
  }
#endif
  (void)width;
  (void)height;
  (void)layers;
  (void)rg8;
  return 0;
}

bool map_ready(uint64_t map) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available && map != 0) {
    return AuroraDawnFdmMapReady(g_device.Get(), map) != 0;
  }
#endif
  (void)map;
  return false;
}

void release_map(uint64_t map) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available && map != 0) {
    AuroraDawnFdmReleaseMap(g_device.Get(), map);
  }
#endif
  (void)map;
}

bool bind(const wgpu::TextureView& view, uint64_t map) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available && view) {
    return AuroraDawnFdmBind(g_device.Get(), view.Get(), map) != 0;
  }
#endif
  (void)view;
  (void)map;
  return false;
}

} // namespace aurora::webgpu::fdm
