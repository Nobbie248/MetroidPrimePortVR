#pragma once

// Per-eye stand-ins for the textures an immersive frame builds from the EFB.
//
// Metroid Prime's thermal and X-ray visors copy the framebuffer into an index
// texture (GXCopyTex as I4 / C8), read it back through a palette and draw it
// over the view. With record-once / replay-per-eye, the copy is taken from the
// mono EFB, so each eye would paste the same flat image, rendered at the game's
// own field of view, over its own view. Here every copy made from an EFB pass
// that replays per eye also gets one copy per eye, taken from that eye's target;
// a palette conversion of such a copy is run per eye as well (DolphinXR's
// "layered palette conversion path"); and a draw that samples either binds the
// eye's version in that eye's pass.
//
// An eye's version has the eye's resolution, not the mono texture's: the mono
// EFB follows the desktop window (or the EFB scale), so a small window would
// otherwise stretch a few hundred pixels of visor or warp over each eye
// (stereo_replay::eye_copy_extent). Draws sample with normalized coordinates
// and measure texel offsets in the mono texture's size (their uniform's), so
// the larger texture only adds detail.
//
// Recording thread only (the GX FIFO thread, where copies and draws are
// recorded), except begin_frame, which runs between frames.

#include "tex_palette_conv.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>

namespace aurora::gfx::stereo_shadow {

using EyeTextures = std::array<TextureHandle, 2>;

struct EyeSize {
  uint32_t width = 0;
  uint32_t height = 0;
};

// Frame begin: whether this frame replays per eye, and whether through multiview
// (stereo_multiview.hpp), whose stand-ins are the two layers of one texture.
void begin_frame(bool immersive, bool multiview = false) noexcept;
bool active() noexcept;
// Changes whenever a stand-in is created, becomes valid or stops being valid,
// so cached per-eye bind groups know to rebuild.
uint64_t epoch() noexcept;

// An EFB copy into `mono` from a pass that replays per eye, whose mono EFB is
// `efb` and whose eye targets are `eyeTargets`: the two eye textures to copy
// into, created like `mono` but each as many times larger as its eye target
// is than the EFB, and reused while `mono` lives, the sizes hold and the
// stand-ins keep being written or looked up (freed after a few idle frames).
EyeTextures copy_targets(const TextureHandle& mono, EyeSize efb, const std::array<EyeSize, 2>& eyeTargets) noexcept;
// `mono` was written without eye copies: its stand-ins no longer apply.
void invalidate(const TextureRef* mono) noexcept;
// A palette conversion `mono`: when its source has valid stand-ins, fills the
// two eye conversions (eye source, eye destination, same palette) and returns
// true. Each eye destination is scaled from `mono`'s as that eye's source is
// from the mono source.
bool palette_conv(const tex_palette_conv::ConvRequest& mono,
                  std::array<tex_palette_conv::ConvRequest, 2>& eyes) noexcept;
// The eye's stand-in for `mono`, or null when it has none valid.
const TextureRef* eye_texture(const TextureRef* mono, uint32_t eye) noexcept;
// Multiview: the 2D array view of both eyes' stand-ins for `mono` (layer = eye), or
// null when it has none valid.
WGPUTextureView layered_view(const TextureRef* mono) noexcept;

void shutdown() noexcept;

} // namespace aurora::gfx::stereo_shadow
