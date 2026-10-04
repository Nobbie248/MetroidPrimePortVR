#pragma once

#include <aurora/aurora.h>
#include <webgpu/webgpu_cpp.h>

#include "gfx/stereo_frame.hpp"
#include "stereo.hpp"

#include <cstdint>
#include <optional>

// The host side of the stereo path: the application's stereo frame provider,
// the content tag a frame is begun and sealed with, the hand-off of finished
// eyes to the interop sink (the OpenXR backend's bridge), and the mono-copy
// fallback that serves the virtual screen.
//
// An immersive packet is replayed per eye by the recorder and the encoders
// (gfx/stereo_frame.hpp): begin_frame() consumes the packet and prepares the
// eye targets, the frame's passes are re-encoded into them, and encode() hands
// the finished eye images to the sink. A virtual-screen packet, and any
// immersive packet that could not be replayed, gets the finished mono image
// letterboxed into one target instead.
namespace aurora::stereo_host {

struct PendingSink {
  stereo::SinkFrame frame;
};

// Game thread, right after gfx::begin_frame: consumes the provider's packet
// for this frame and decides whether it replays per eye (mode, tag match,
// usable eye targets). The returned state is stored in the frame's packet.
gfx::StereoFrameState begin_frame(uint64_t contentTag) noexcept;

// Frame worker, inside the end-of-frame encoder callback, after the mono
// present passes: encodes the sink's copies into `encoder`. The returned value
// is handed to submitted() right after the command buffer is submitted.
std::optional<PendingSink> encode(wgpu::CommandEncoder& encoder, const gfx::StereoFrameState& state,
                                  uint32_t logicalFrame, uint64_t contentTag) noexcept;
void submitted(const std::optional<PendingSink>& pending) noexcept;

// Game thread, between frames: releases the targets and forgets the provider.
void shutdown() noexcept;

// True on a standalone headset (Android) while the stereo provider and sink are
// registered: the headset is then the only display, and nothing shows the window's
// image, so the window is not presented and an immersive frame's final pass
// renders its eyes only.
bool headset_owns_display() noexcept;

} // namespace aurora::stereo_host
