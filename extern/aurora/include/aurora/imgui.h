#ifndef AURORA_IMGUI_H
#define AURORA_IMGUI_H

#include <imgui.h>

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8);
// Frees a texture from aurora_imgui_add_texture. Not while a frame that draws it is still in flight: call it
// the frame after the last one that used it.
void aurora_imgui_remove_texture(ImTextureID texture);

// The headset settings panel: draw data from a second ImGui context, rendered
// into the panel texture once per sealed frame and shown in the headset at
// widthFraction of the virtual screen. Null hides the panel. (Not yet ported:
// accepted and ignored.)
void aurora_imgui_set_stereo_overlay(ImDrawData* drawData, float widthFraction);

#ifdef __cplusplus
}
#endif

#endif
