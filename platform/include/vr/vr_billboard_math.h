// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "vr/openxr_screen_math.h"

namespace PortVr::billboard_math {
using screen_math::Vec3;
using screen_math::Dot;

inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 Unit(const Vec3& value, const Vec3& fallback) noexcept {
    const float length = std::sqrt(Dot(value, value));
    if (!std::isfinite(length) || length < 0.00001f) {
        return fallback;
    }
    return {value[0] / length, value[1] / length, value[2] / length};
}
struct Basis {
    Vec3 right;
    Vec3 forward;
    Vec3 up;
};
// Prime axes: +Y faces away from the viewer, X/Z span the sprite. Its
// facing depends on the viewer's position, not the body's camera direction.
inline Basis Facing(const Vec3& center, const Vec3& viewer, const Vec3& up) noexcept {
    const Vec3 forward = Unit({center[0] - viewer[0], center[1] - viewer[1], center[2] - viewer[2]},
                              {0.f, 1.f, 0.f});
    Vec3 right = Cross(forward, up);
    if (Dot(right, right) < 0.000001f) {
        right = Cross(forward, std::fabs(forward[2]) > 0.9f ? Vec3{0.f, 1.f, 0.f} : Vec3{0.f, 0.f, 1.f});
    }
    right = Unit(right, {1.f, 0.f, 0.f});
    return {right, forward, Unit(Cross(right, forward), {0.f, 0.f, 1.f})};
}
} // namespace PortVr::billboard_math
