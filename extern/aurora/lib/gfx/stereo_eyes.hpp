#pragma once

// The eye render targets of the stereo replay: a colour attachment that
// matches the EFB's format and sample count (so the recorded pipelines bind
// unchanged), its single-sample resolve when the EFB is multisampled, and a
// depth buffer. Created on demand on the game thread, between frames; a pass
// sealed for replay copies the views it needs (gfx/stereo_frame.hpp).

#include "stereo_frame.hpp"
#include "../webgpu/gpu.hpp"

#include <array>
#include <cstdint>

namespace aurora::gfx {

struct StereoEyeTarget {
  webgpu::TextureWithSampler color;    // the attachment (multisampled when the EFB is)
  webgpu::TextureWithSampler resolved; // the resolve target, only with MSAA
  webgpu::TextureWithSampler depth;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sampleCount = 0;

  [[nodiscard]] bool valid() const noexcept { return width != 0 && height != 0 && color.texture && depth.texture; }
  [[nodiscard]] const webgpu::TextureWithSampler& output() const noexcept {
    return sampleCount > 1 ? resolved : color;
  }
};

// Makes both eye targets match the packet's sizes and the current EFB quality.
// Returns false when a size is unusable. Game thread, between frames.
bool ensure_stereo_eye_targets(const std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT>& eyes) noexcept;
const StereoEyeTarget& stereo_eye_target(uint32_t eye) noexcept;
void release_stereo_eye_targets() noexcept;

} // namespace aurora::gfx
