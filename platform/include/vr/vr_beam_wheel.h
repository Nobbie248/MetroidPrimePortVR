// SPDX-License-Identifier: GPL-3.0-or-later
//
// The beam wheel's geometry: PrimedGun's weapon select (Source/Core/Core/HW/
// GCPadEmu.cpp: UpdatePrimedGunWeaponSelect, PrimedGunRollFreeQuat). Holding
// the weapon hand's B freezes a panel 0.26 m ahead of and 0.055 m above the
// aim pose, levelled (its roll removed). The roll-free aim ray's hit on it, or
// the hand's travel when the ray misses, is read along the panel's own right
// and up axes, and the dominant axis picks the beam: up Power, right Wave, down
// Ice, left Plasma. That is the layout of the game's beam menu on the HUD and
// of the C-stick, which the wheel pulses on release.
//
// No game or OpenXR types here, so tests/port_vr_beam_wheel.cpp checks it
// without either. platform/vr/vr_pad.cpp drives it and pulses the C-stick;
// platform/vr/vr_view.cpp hands the hovered beam to the HUD's beam menu.

#pragma once

#include "vr/openxr_screen_math.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace PortVr::BeamWheel {

using screen_math::Quat;
using screen_math::Vec3;

// PrimedGun's slot order. VrPadState::weapon_selected carries these numbers.
enum class Beam : int { None = -1, Power = 0, Wave = 1, Ice = 2, Plasma = 3 };

// The panel: 0.26 m ahead and 0.055 m above the aim pose at the moment the
// modifier was pressed, read in half-extents of 0.21 m with a 0.25 deadzone;
// a hit further out than 1.8 half-extents counts as a miss, and the hand's
// travel over 7.5 cm stands in.
inline constexpr float kPanelForward = 0.26f;
inline constexpr float kPanelUp = 0.055f;
inline constexpr float kPanelHalfExtent = 0.21f;
inline constexpr float kPanelHitLimit = 1.8f;
inline constexpr float kPanelDeadzone = 0.25f;
inline constexpr float kTravelScale = 0.075f;

inline Quat Normalize(Quat q) noexcept {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 1.0e-6f)) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

// The orientation with its roll removed, in the OpenXR reference space (+Y up,
// -Z forward): forward kept, right = forward x world up (levelled), up =
// right x forward. Pointing straight up or down leaves no level right, and the
// orientation is returned as it is.
inline Quat RollFree(const Quat& q) noexcept {
    const Vec3 forward = screen_math::Rotate(q, {0.0f, 0.0f, -1.0f});
    Vec3 right{-forward[2], 0.0f, forward[0]};
    const float length = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (!(length > 1.0e-4f)) {
        return q;
    }
    right = {right[0] / length, 0.0f, right[2] / length};
    const Vec3 up{right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
                  right[0] * forward[1] - right[1] * forward[0]};
    // Rotation matrix columns (right, up, -forward) -> quaternion.
    const float m00 = right[0], m01 = up[0], m02 = -forward[0];
    const float m10 = right[1], m11 = up[1], m12 = -forward[1];
    const float m20 = right[2], m21 = up[2], m22 = -forward[2];
    const float trace = m00 + m11 + m22;
    Quat out{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        out = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        out = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        out = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        out = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return Normalize(out);
}

// The frozen panel, from the aim pose when the modifier was pressed.
struct Panel {
    Quat base_orientation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 base_position{};
    Vec3 center{};
};

inline Panel OpenPanel(const Vec3& position, const Quat& orientation) noexcept {
    Panel panel;
    panel.base_orientation = RollFree(Normalize(orientation));
    panel.base_position = position;
    const Vec3 offset = screen_math::Rotate(panel.base_orientation, {0.0f, kPanelUp, -kPanelForward});
    panel.center = {position[0] + offset[0], position[1] + offset[1], position[2] + offset[2]};
    return panel;
}

// Where the aim is on the panel, in half-extents (x right, y up): the
// roll-free aim ray's hit, or the hand's travel since the panel opened when
// the ray misses or runs along it.
inline std::array<float, 2> Measure(const Panel& panel, const Vec3& position, const Quat& orientation) noexcept {
    const Quat current = RollFree(Normalize(orientation));
    const Vec3 direction = screen_math::Rotate(current, {0.0f, 0.0f, -1.0f});
    const Vec3 panel_right = screen_math::Rotate(panel.base_orientation, {1.0f, 0.0f, 0.0f});
    const Vec3 panel_up = screen_math::Rotate(panel.base_orientation, {0.0f, 1.0f, 0.0f});
    const Vec3 panel_forward = screen_math::Rotate(panel.base_orientation, {0.0f, 0.0f, -1.0f});
    const Vec3 to_panel{panel.center[0] - position[0], panel.center[1] - position[1],
                        panel.center[2] - position[2]};
    const float denominator = screen_math::Dot(direction, panel_forward);
    if (std::fabs(denominator) > 0.025f) {
        const float t = screen_math::Dot(to_panel, panel_forward) / denominator;
        if (t > 0.02f && t < 2.0f) {
            const Vec3 point{position[0] + direction[0] * t - panel.center[0],
                             position[1] + direction[1] * t - panel.center[1],
                             position[2] + direction[2] * t - panel.center[2]};
            const float x = screen_math::Dot(point, panel_right) / kPanelHalfExtent;
            const float y = screen_math::Dot(point, panel_up) / kPanelHalfExtent;
            if (std::fabs(x) <= kPanelHitLimit && std::fabs(y) <= kPanelHitLimit) {
                return {x, y};
            }
        }
    }
    const Vec3 travel = screen_math::Rotate(screen_math::Conjugate(panel.base_orientation),
                                            {position[0] - panel.base_position[0],
                                             position[1] - panel.base_position[1],
                                             position[2] - panel.base_position[2]});
    return {travel[0] / kTravelScale, travel[1] / kTravelScale};
}

// The beam for an aim position taken relative to the first sample after the
// panel opened: the dominant axis, clamped to the panel, outside the deadzone.
inline Beam Pick(float x, float y) noexcept {
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    if (std::fabs(x) < kPanelDeadzone && std::fabs(y) < kPanelDeadzone) {
        return Beam::None;
    }
    if (std::fabs(x) >= std::fabs(y)) {
        return x < 0.0f ? Beam::Plasma : Beam::Wave;
    }
    return y < 0.0f ? Beam::Ice : Beam::Power;
}

// The C-stick direction that selects the beam in the game's control map
// (x right, y up): up Power, right Wave, down Ice, left Plasma.
inline std::array<int, 2> CStickDirection(Beam beam) noexcept {
    switch (beam) {
    case Beam::Power:
        return {0, 1};
    case Beam::Wave:
        return {1, 0};
    case Beam::Ice:
        return {0, -1};
    case Beam::Plasma:
        return {-1, 0};
    case Beam::None:
        break;
    }
    return {0, 0};
}

// The game's CPlayerState::EBeamId, which puts Ice before Wave (0 Power,
// 1 Ice, 2 Wave, 3 Plasma); -1 for none. The HUD's beam menu indexes its boxes
// by it.
inline int GameBeamId(Beam beam) noexcept {
    switch (beam) {
    case Beam::Power:
        return 0;
    case Beam::Ice:
        return 1;
    case Beam::Wave:
        return 2;
    case Beam::Plasma:
        return 3;
    case Beam::None:
        break;
    }
    return -1;
}

} // namespace PortVr::BeamWheel
