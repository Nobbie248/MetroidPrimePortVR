// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snap turn: PrimedGun's UpdateSnapTurn (Source/Core/Core/PrimedGun/
// NativeRuntime.cpp). With the setting on, the look stick no longer turns
// Samus smoothly; flicking it past 0.72 turns her by the snap turn angle at
// once, and the stick must come back under 0.32 before the next flick counts.
// Two snaps are at least eight game frames apart, and a flick made while the
// stick's hand is at the head (the visor gesture's zone) never counts, even
// after the hand leaves the zone with the stick still pushed.
//
// PrimedGun read the right controller's stick whatever the stick layout; here
// it is the look stick, the one smooth turning reads, so swapping the sticks
// moves the snap turn with the turning. The default layout is the same.
//
// PrimedGun rotated the player's transform in memory. Here the tracker only
// says how far to turn; CPlayer::PortVrSnapTurn turns Samus during the tick.
//
// No game or OpenXR types here, so tests/port_vr_snap_turn.cpp checks it
// without either. platform/vr/vr_pad.cpp feeds it.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace PortVr::SnapTurn {

// PrimedGun's flick thresholds, on the stick's x.
constexpr float kEnterThreshold = 0.72f;
constexpr float kExitThreshold = 0.32f;
// PrimedGun's cooldown: eight game frames after a snap.
constexpr double kCooldownSeconds = 8.0 / 60.0;
// PrimedGun's angle choices (SNAP_TURN_DEGREES_CHOICES); a saved angle outside
// them is clamped to the nearest end.
constexpr std::array<int, 4> kDegreeChoices = {30, 45, 60, 90};

inline int ClampDegrees(int degrees) noexcept {
    return std::clamp(degrees, kDegreeChoices.front(), kDegreeChoices.back());
}

struct Input {
    bool enabled = false;    // the snap turn setting
    bool connected = false;  // the look stick's controller
    float stick_x = 0.0f;    // the look stick, +right
    bool near_head = false;  // that controller is in the visor gesture's zone
    bool accepting = false;  // Samus could turn now (first person, unmorphed, no visor change)
    int degrees = 45;        // the snap turn angle setting
    double now = 0.0;        // seconds, monotonic
};

class Tracker {
public:
    // One sample. Returns the turn to make, in degrees (positive turns right),
    // or 0. A flick is used up even when Samus cannot turn, so holding the
    // stick through a menu or a visor change does not snap once it ends.
    float Update(const Input& in) noexcept {
        if (!in.enabled) {
            m_ready = true;
            return 0.0f;
        }
        if (!in.connected) {
            m_ready = true;
            m_cooldown_until = 0.0;
            return 0.0f;
        }
        const float x = std::clamp(in.stick_x, -1.0f, 1.0f);
        if (std::fabs(x) <= kExitThreshold) {
            m_ready = true;
            return 0.0f;
        }
        if (in.near_head) {
            m_ready = false;
            return 0.0f;
        }
        if (!m_ready || std::fabs(x) < kEnterThreshold || in.now < m_cooldown_until) {
            return 0.0f;
        }
        m_ready = false;
        if (!in.accepting) {
            return 0.0f;
        }
        m_cooldown_until = in.now + kCooldownSeconds;
        return (x > 0.0f ? 1.0f : -1.0f) * static_cast<float>(ClampDegrees(in.degrees));
    }

    void Reset() noexcept {
        m_ready = true;
        m_cooldown_until = 0.0;
    }

private:
    bool m_ready = true;
    double m_cooldown_until = 0.0;
};

} // namespace PortVr::SnapTurn
