#pragma once

#include <webgpu/webgpu_cpp.h>

#include "stereo.hpp"

#include <cstdint>

struct ImDrawData;

// The headset-only settings panel of aurora_imgui_set_stereo_overlay.
//
// Milestone 1 of the Metroid Prime VR port carries only the panel's *layer
// source*: the OpenXR backend may ask for a panel image with the eyes, and it
// gets a transparent one until the panel's ImGui rendering is ported. Everything
// here belongs to the frame worker, like the eye targets.
namespace aurora::stereo_overlay {

// Renders the panel's draw data into the panel texture. Not yet ported: always
// returns a null command buffer (the panel is hidden).
wgpu::CommandBuffer prepare(ImDrawData* drawData, float widthFraction) noexcept;

// Layer mode: the OpenXR backend shows the panel as its own compositor quad
// layer, so it is not drawn into the eyes. Any thread; read by the frame worker.
void set_layer_mode(bool enabled) noexcept;
bool layer_mode() noexcept;

// Frame worker, inside a stereo sink: the image to copy into a panel layer of
// width x height in the eyes' format. A transparent image of that size (cleared
// once by a pass recorded into `encoder`) until the panel is ported. False only
// when no image can be made.
bool layer_source(const wgpu::CommandEncoder& encoder, uint32_t width, uint32_t height, stereo::EyeImage& out) noexcept;

void shutdown() noexcept;

} // namespace aurora::stereo_overlay
