#pragma once

#include <webgpu/webgpu_cpp.h>

#include "stereo.hpp"

#include <cstdint>

struct ImDrawData;

// The headset-only panel layer's image source.
//
// The host draws the panel itself (PrimedGun's VR menu is rasterised on the
// CPU) and hands Aurora the pixels (aurora_set_stereo_panel_image). The OpenXR
// backend asks for the panel's image with the eyes and copies it into the
// panel's own compositor quad layer. Nothing draws the panel into the eyes.
// Everything but set_image belongs to the frame worker, like the eye targets.
namespace aurora::stereo_overlay {

// Not used: the panel is never an ImGui frame here. Always returns a null
// command buffer.
wgpu::CommandBuffer prepare(ImDrawData* drawData, float widthFraction) noexcept;

// Layer mode: the OpenXR backend shows the panel as its own compositor quad
// layer, so it is not drawn into the eyes. Any thread; read by the frame worker.
void set_layer_mode(bool enabled) noexcept;
bool layer_mode() noexcept;

// Any thread: the panel's RGBA8 pixels (straight alpha), copied; null clears them.
void set_image(const void* rgba8, uint32_t width, uint32_t height) noexcept;

// Frame worker, inside a stereo sink: the image to copy into a panel layer of
// width x height in the eyes' format. The host's image when it has exactly that
// size (uploaded when it changed), otherwise a transparent image of that size
// (cleared once by a pass recorded into `encoder`). False only when no image
// can be made.
bool layer_source(const wgpu::CommandEncoder& encoder, uint32_t width, uint32_t height, stereo::EyeImage& out) noexcept;

void shutdown() noexcept;

} // namespace aurora::stereo_overlay
