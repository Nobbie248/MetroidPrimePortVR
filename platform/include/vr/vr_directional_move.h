// SPDX-License-Identifier: GPL-3.0-or-later
//
// Directional movement: PrimedGun's UpdateDirectionalMovement (Source/Core/
// Core/PrimedGun/NativeRuntime.cpp), its "left stick strafe". Outside an orbit
// lock the move stick walks and strafes Samus toward where the move hand's
// controller points (or the head, with the setting): pushing it up walks along
// that controller's heading, sideways strafes across it. Its x no longer turns
// her; the look stick does.
//
// Past the deadzone her horizontal velocity is set along the stick's direction,
// at a speed that ramps toward the movement speed times the stick's push
// (capped at 1). The ramp starts from the speed she already carries, no faster
// than that target, and climbs at the ground or the air acceleration; easing
// the stick back drops it to the new target at once. The direction follows the
// stick at once, in the air too. Inside the deadzone nothing is set and the
// game's friction stops her, as when the stick is let go.
//
// PrimedGun wrote the player's velocity in memory at the end of each frame,
// over the game's own walk. Here CPlayer::ComputeMovement sets it during the
// tick, in place of the move stick's walk (vr_pad.h VrDirectionalMove).
//
// No game or OpenXR types here, so tests/port_vr_directional_move.cpp checks
// it without either. platform/vr/vr_pad.cpp feeds it.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace PortVr::DirectionalMove {

// An OpenXR orientation (x, y, z, w) applied to v.
inline std::array<float, 3> Rotate(const std::array<float, 4>& q, const std::array<float, 3>& v) noexcept {
    // v + 2w (q x v) + 2 q x (q x v), q being the vector part.
    const std::array<float, 3> t{2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                                 2.0f * (q[0] * v[1] - q[1] * v[0])};
    return {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]), v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
            v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
}

// Below this horizontal length of the aim (about 72.5 degrees up or down),
// the controller's top starts to take over the heading from the aim.
constexpr float kTopTakeoverLength = 0.3f;

// A controller's or the head's heading, as (right, forward) in Samus's frame,
// unit length. The tracking space's forward (-Z) is the body's forward: the
// eyes and the cannon are placed from it with no yaw of their own
// (openxr_integration.cpp ViewFromBase, vr_view.cpp VrCannonTransform), so the
// heading in the tracking space is the heading relative to Samus.
//
// The heading is where the aim points, levelled: exact however the controller
// is pitched or rolled. Pointed almost straight at the floor or the sky the
// aim has no heading left, so past about 72 degrees the controller's top takes
// over, all of it when vertical (the top of a controller aimed at the floor
// faces ahead, aimed at the sky behind). PrimedGun took a yaw angle out of the
// quaternion instead, which drifts once the controller is pitched (22 degrees
// for a 30-degree heading aimed 45 degrees down).
inline std::array<float, 2> Heading(const std::array<float, 4>& q) noexcept {
    // OpenXR x right, y up, -z forward.
    const std::array<float, 3> aim = Rotate(q, {0.0f, 0.0f, -1.0f});
    const std::array<float, 3> top = Rotate(q, {0.0f, 1.0f, 0.0f});
    const float level = std::sqrt(aim[0] * aim[0] + aim[2] * aim[2]);
    const float takeover = std::clamp((kTopTakeoverLength - level) / kTopTakeoverLength, 0.0f, 1.0f);
    float right = 0.0f;
    float ahead = 0.0f;
    if (level > 1.0e-4f) {
        right += aim[0] / level * (1.0f - takeover);
        ahead += -aim[2] / level * (1.0f - takeover);
    }
    // Aimed down the top faces the heading; aimed up it faces away from it.
    const float side = aim[1] >= 0.0f ? -1.0f : 1.0f;
    const float top_length = std::sqrt(top[0] * top[0] + top[2] * top[2]);
    if (top_length > 1.0e-4f) {
        right += side * top[0] / top_length * takeover;
        ahead += -side * top[2] / top_length * takeover;
    }
    const float length = std::sqrt(right * right + ahead * ahead);
    if (!std::isfinite(length) || length < 1.0e-4f) {
        return {0.0f, 1.0f};
    }
    return {right / length, ahead / length};
}

// The stick's wish for one frame: a unit direction in Samus's frame and how
// far the stick is pushed (1 at most).
struct Wish {
    bool moving = false; // past the deadzone
    float right = 0.0f;
    float forward = 0.0f;
    float push = 0.0f;
};

// The stick (+x right, +y up) read in the heading's frame, up along it.
// PrimedGun's deadzone is on the stick's whole push, and the push past it is
// not rescaled.
inline Wish FromStick(const std::array<float, 2>& heading, float stick_x, float stick_y, float deadzone) noexcept {
    Wish wish;
    const float push = std::sqrt(stick_x * stick_x + stick_y * stick_y);
    if (!std::isfinite(push) || push < deadzone || push < 1.0e-4f) {
        return wish;
    }
    // The heading's right is the heading turned a quarter clockwise seen from above.
    const float right = heading[0] * stick_y + heading[1] * stick_x;
    const float forward = heading[1] * stick_y - heading[0] * stick_x;
    const float length = std::sqrt(right * right + forward * forward);
    if (!std::isfinite(length) || length < 1.0e-4f) {
        return wish;
    }
    wish.moving = true;
    wish.right = right / length;
    wish.forward = forward / length;
    wish.push = std::min(push, 1.0f);
    return wish;
}

// The speed along the wish, over consecutive ticks of walking.
class Ramp {
public:
    // One tick: Samus carries `flat_speed` (her horizontal speed) and the
    // stick asks for `target`. Returns the speed to set.
    float Step(float flat_speed, float target, float acceleration, float dt) noexcept {
        target = std::max(target, 0.0f);
        if (!std::isfinite(flat_speed)) {
            flat_speed = 0.0f;
        }
        if (m_speed <= 0.0f || m_speed > target) {
            m_speed = std::clamp(flat_speed, 0.0f, target);
        }
        const float step = std::max(acceleration, 0.0f) * std::max(dt, 0.0f);
        m_speed = m_speed < target ? std::min(target, m_speed + step) : std::max(target, m_speed - step);
        return m_speed;
    }

    void Reset() noexcept { m_speed = 0.0f; }
    float Speed() const noexcept { return m_speed; }

private:
    float m_speed = 0.0f;
};

} // namespace PortVr::DirectionalMove
