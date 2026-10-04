#pragma once

// Vulkan multiview for the stereo replay: both eyes of an EFB pass in one render
// pass (DolphinXR's approach on the Quest).
//
// Per-eye replay encodes every recorded draw once per eye. On the Quest's tiled
// Adreno GPU each of those draws costs a few microseconds of state setup in the
// binning pass and in every bin, whatever its size, so two eye passes of ~1,100
// draws cost far more than the pixels they fill. Under multiview an eye pass
// renders into the two layers of the eye targets with view mask 0b11: each draw
// is recorded and bound once and the GPU runs it for both views. Its shader
// reads @builtin(view_index): its uniform is a pair, one GX uniform per eye back
// to back (the eye mask immediate picks the pair or the mono uniform for both),
// and its GX textures are sampled as 2D arrays at layer min(view, layers - 1),
// so an EFB copy taken per eye (stereo_shadow.hpp) is one two-layer texture and
// every other texture its single layer.
//
// The API comes from PrimedGun's Dawn patches (quest/dawn/apply.py: the
// DawnMultiview feature, DawnRenderPassMultiview / DawnRenderPipelineMultiview,
// `enable chromium_experimental_multiview;`). With a stock Dawn it compiles out
// and the replay keeps its per-eye passes.

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

namespace aurora::gfx::stereo_multiview {

#if defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)
inline constexpr bool kApiAvailable = true;
#else
inline constexpr bool kApiAvailable = false;
#endif

// Both eyes: layer 0 is the left eye, layer 1 the right one.
inline constexpr uint32_t kViewMask = 0b11;
inline constexpr uint32_t kViewCount = 2;

// The host's switch (aurora_set_stereo_multiview); on by default. Any thread.
void set_requested(bool requested) noexcept;
bool requested() noexcept;

// Whether a frame whose eyes are `width` x `height` each (both the same) at
// `sampleCount` replays through multiview: the device has the feature, the host
// wants it and the EFB is single-sampled. Game thread, at frame begin.
bool usable(uint32_t leftWidth, uint32_t leftHeight, uint32_t rightWidth, uint32_t rightHeight,
            uint32_t sampleCount) noexcept;

// A draw's uniform pair is its two eye copies as stage_stereo_uniforms pushes
// them, back to back: each is the shader's uniform size, which is already aligned
// to minUniformBufferOffsetAlignment, and the WGSL array declares that stride
// (gx/shader.cpp to_multiview_source).

} // namespace aurora::gfx::stereo_multiview
