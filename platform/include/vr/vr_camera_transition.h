// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace PortVr::CameraTransition {

// Some level hints request an instant cut (zero duration). In immersive
// Morph Ball play, give those camera handoffs a short, bounded transition.
inline float Duration(float authored) noexcept {
    return std::isfinite(authored) && authored > 0.f ? std::max(authored, .1f) : .5f;
}

inline float Blend(float elapsed, float duration) noexcept {
    const float t = std::clamp(elapsed / Duration(duration), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

} // namespace PortVr::CameraTransition
