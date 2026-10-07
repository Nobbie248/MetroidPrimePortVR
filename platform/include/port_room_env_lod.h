#pragma once

// The mip a PBR reflection is read from (PortRoomEnv::Compose, both the probe blend and the
// single probe): plain arithmetic, so it is tested without the game or GX
// (tests/port_room_env.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace PortRoomEnvLod {

// What MP_ROOM_ENV_LOD means when the variable is not set: the cube's own range, whatever it
// is. A NaN cannot be a cap, and CubeLod reads anything nonfinite that way too.
constexpr float kNoCap = std::numeric_limits<float>::quiet_NaN();

// The mip a reflection at roughness 1 reads: the cube's own top mip, `mipCount - 1`, and 0
// for a cube of no more than one mip (so nothing underflows or samples below it).
//
// Remastered scales roughness by exactly this (CMaterialInstanceData_Single stores the probe
// cube's mip count minus one, and its shader samples the cube with textureLod(dir, rough *
// that)), so a cap under it flattens reflections that should still blur.
//
// `cap` is MP_ROOM_ENV_LOD: a finite value of zero or more lowers the mip, never above the
// cube's range. Anything else (unset, negative, NaN, infinite) leaves the cube's own range,
// since a negative mip is not a texture there.
inline float CubeLod(uint32_t mipCount, float cap) {
  const float top = mipCount > 1 ? float(mipCount - 1) : 0.f;
  return std::isfinite(cap) && cap >= 0.f ? std::min(cap, top) : top;
}

} // namespace PortRoomEnvLod
