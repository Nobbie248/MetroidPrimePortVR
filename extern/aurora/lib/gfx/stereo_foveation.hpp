// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fixed foveated rendering of the immersive eyes: the fragment density map an
// eye pass runs under (foveation.hpp builds it, webgpu/fdm.hpp uploads it
// through PrimedGun's patched Dawn), and the view of the eye targets the pass
// renders through to get it.
//
// The patched Dawn attaches a map to every render pass whose first colour
// attachment is the texture view the map is bound to. So the eye targets get a
// second, render-attachment-only view (the two-layer array view under
// multiview, one per eye otherwise) that only the eye passes use; the EFB
// copies, the swapchain blit and the desktop mirror keep reading the targets'
// own views and are never affected. Every replayed eye pass of an immersive
// frame renders through the foveated view, splits included: on the Quest's
// Adreno an eye pass is binned anyway, and a load under a density map reads
// the same full-resolution tiles as one without. The virtual screen (menus,
// cinematics) renders elsewhere and is never foveated.
//
// Under multiview the map is a two-layer array, one layer per view, each
// centred on its own eye's forward direction (the asymmetric frustums put it
// off the image centre, towards the nose). Maps are immutable: a new level,
// eye size or field of view uploads a new one, which replaces the bound one
// only once its upload has completed, so the eyes never fall back to an
// unfoveated frame in between.

#include "stereo_frame.hpp"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>

namespace aurora::gfx::stereo_foveation {

// The host's level (aurora_set_stereo_foveation): 0 off, then foveation::Level
// low, medium, high. Any thread; live.
void set_level(uint32_t level) noexcept;
uint32_t level() noexcept;

// The views an immersive frame's eye passes render through while they are
// foveated: the multiview array view, or one per eye; `foveated` false and
// empty views otherwise.
struct FrameViews {
  bool foveated = false;
  wgpu::TextureView multiviewColorView;
  std::array<wgpu::TextureView, AURORA_STEREO_EYE_COUNT> eyeColorViews{};
};

// Game thread, at frame begin after ensure_stereo_eye_targets: rebuilds the
// map when the targets, the level or the fields of view changed, binds it once
// its upload has completed, and says which views this frame's eye passes
// render through.
FrameViews prepare(const std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT>& eyes, bool multiview) noexcept;

// Unbinds and frees the maps: before the eye targets are recreated or released
// (a binding keeps its view, and so the old targets, alive), and at shutdown.
void release() noexcept;

} // namespace aurora::gfx::stereo_foveation
