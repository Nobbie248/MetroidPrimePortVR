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
// Recording thread only (the GX FIFO thread, where copies and draws are
// recorded), except begin_frame, which runs between frames.

#include "tex_palette_conv.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>

namespace aurora::gfx::stereo_shadow {

using EyeTextures = std::array<TextureHandle, 2>;

// Frame begin: whether this frame replays per eye.
void begin_frame(bool immersive) noexcept;
bool active() noexcept;
// Changes whenever a stand-in is created, becomes valid or stops being valid,
// so cached per-eye bind groups know to rebuild.
uint64_t epoch() noexcept;

// An EFB copy into `mono` from a pass that replays per eye: the two eye
// textures to copy into (created like `mono`, reused while it lives).
EyeTextures copy_targets(const TextureHandle& mono) noexcept;
// `mono` was written without eye copies: its stand-ins no longer apply.
void invalidate(const TextureRef* mono) noexcept;
// A palette conversion `mono`: when its source has valid stand-ins, fills the
// two eye conversions (eye source, eye destination, same palette) and returns true.
bool palette_conv(const tex_palette_conv::ConvRequest& mono,
                  std::array<tex_palette_conv::ConvRequest, 2>& eyes) noexcept;
// The eye's stand-in for `mono`, or null when it has none valid.
const TextureRef* eye_texture(const TextureRef* mono, uint32_t eye) noexcept;

void shutdown() noexcept;

} // namespace aurora::gfx::stereo_shadow
