#pragma once

#include "gx.hpp"

#include <array>
#include <cstdint>

namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
gfx::Range build_uniform(const ShaderInfo& info) noexcept;
// build_uniform() that also stages the stereo eye copies of the uniform
// (gfx/stereo_uniform.hpp) and returns the offsets the draw binds per eye.
gfx::Range build_uniform(const ShaderInfo& info, std::array<uint32_t, 2>& stereoUniformOffsets) noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
