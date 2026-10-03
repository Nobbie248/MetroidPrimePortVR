// SPDX-License-Identifier: GPL-3.0-or-later
//
// The visor gesture: PrimedGun's XR D-pad (Source/Core/Core/PrimedGun/
// NativeRuntime.cpp: LeftControllerNearHead, StickToDpad, UpdateXrDpad). Hold
// the off-hand controller next to the headset and its stick becomes the
// GameCube D-pad, which the game's own control map turns into visors: up
// combat, left scan, down thermal, right X-ray. While the hand is there its
// stick no longer moves or turns Samus.
//
// PrimedGun fed the D-pad by poking CFinalInput and needed its "XR Visor D-Pad
// Timing Hook" in CPlayer::UpdateVisorState to repeat the press every ten
// frames, because the game drops a visor press made during a visor transition
// or a scan. Here the pad itself is fed, and the tracker lets go of the D-pad
// while the game would drop the press and holds it again once it can take it:
// the press edge then lands right after the transition. A flick stays latched
// for one transition's length so it survives one in progress.
//
// No game or OpenXR types here, so tests/port_vr_visor_dpad.cpp checks it
// without either. platform/vr/vr_pad.cpp feeds it and applies its output.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace PortVr::VisorDpad {

enum class Dir : int { None = -1, Up = 0, Right = 1, Down = 2, Left = 3 };

// PrimedGun's head zone, in the OpenXR reference space (metres, +Y up): a
// sphere 6 cm wider than the radius setting around the head, cut off a little
// below the head (the setting plus 4 cm) and 28 cm above it.
inline bool HandNearHead(const std::array<float, 3>& hand, const std::array<float, 3>& head, float head_radius,
                         float head_y_below) noexcept {
    const float dx = hand[0] - head[0];
    const float dy = hand[1] - head[1];
    const float dz = hand[2] - head[2];
    const float radius = head_radius + 0.06f;
    return dx * dx + dy * dy + dz * dz <= radius * radius && dy >= -(head_y_below + 0.04f) && dy <= 0.28f;
}

// PrimedGun reads the deadzone setting but never above 0.25.
inline float EffectiveDeadzone(float setting) noexcept { return std::min(setting, 0.25f); }

// The stick's direction, with PrimedGun's hysteresis: once a direction is held
// it survives down to 55% of the deadzone, and changing axis there takes a 35%
// lead over the held one.
inline Dir StickToDir(float x, float y, float deadzone, Dir last) noexcept {
    const float magnitude = std::sqrt(x * x + y * y);
    const float exit_deadzone = std::max(0.05f, deadzone * 0.55f);
    if (magnitude < exit_deadzone) {
        return Dir::None;
    }
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    if (ax >= deadzone || ay >= deadzone) {
        return ay >= ax ? (y > 0.0f ? Dir::Up : Dir::Down) : (x > 0.0f ? Dir::Right : Dir::Left);
    }
    if (last == Dir::None) {
        return Dir::None;
    }
    constexpr float kAxisSwitchBias = 1.35f;
    switch (last) {
    case Dir::Up:
    case Dir::Down:
        if (ay * kAxisSwitchBias >= ax) {
            return y >= 0.0f ? Dir::Up : Dir::Down;
        }
        break;
    case Dir::Left:
    case Dir::Right:
        if (ax * kAxisSwitchBias >= ay) {
            return x >= 0.0f ? Dir::Right : Dir::Left;
        }
        break;
    default:
        break;
    }
    return ax > ay ? (x >= 0.0f ? Dir::Right : Dir::Left) : (y >= 0.0f ? Dir::Up : Dir::Down);
}

// The zone stays up this long after the hand leaves it (PrimedGun: 16 frames
// at 60 Hz), so a hand at the zone's edge does not hand the stick back to
// movement mid-flick.
inline constexpr double kZoneGraceSeconds = 16.0 / 60.0;
// A flick stays held this long after the stick is let go: one visor transition
// (CPlayerState's 0.2 s out and 0.2 s back in), so a flick made while one runs
// is still there when the game can take it. PrimedGun latched 8 frames and
// relied on its repeating press instead.
inline constexpr double kLatchSeconds = 0.4;

struct Input {
    bool armed = false;      // gesture on, gameplay, the hand and the head tracked
    bool near_head = false;  // HandNearHead this sample
    float stick_x = 0.0f;    // the hand's stick, +up
    float stick_y = 0.0f;
    float deadzone = 0.25f;  // EffectiveDeadzone
    bool accepting = true;   // the game would take a visor press now
    double now = 0.0;        // seconds, monotonic
};

struct Output {
    bool zone = false;      // the hand's stick belongs to the D-pad: keep it off the pad's sticks
    Dir direction = Dir::None; // the gesture's direction, held or latched
    Dir held = Dir::None;   // the D-pad button to hold this sample (direction, gated)
};

class Tracker {
public:
    Output Update(const Input& in) noexcept {
        if (!in.armed) {
            Reset();
            return {};
        }
        if (in.near_head) {
            has_near_ = true;
            last_near_ = in.now;
        } else if (!has_near_ || in.now - last_near_ >= kZoneGraceSeconds) {
            Reset();
            return {};
        }
        Output out;
        out.zone = true;
        Dir dir = StickToDir(in.stick_x, in.stick_y, in.deadzone, last_dir_);
        if (dir != Dir::None) {
            latched_ = dir;
            latch_until_ = in.now + kLatchSeconds;
        } else if (latched_ != Dir::None && in.now < latch_until_) {
            dir = latched_;
        }
        if (dir == Dir::None) {
            last_dir_ = Dir::None;
            latched_ = Dir::None;
            return out;
        }
        last_dir_ = dir;
        out.direction = dir;
        out.held = in.accepting ? dir : Dir::None;
        return out;
    }

    void Reset() noexcept { *this = Tracker{}; }

private:
    bool has_near_ = false;
    double last_near_ = 0.0;
    Dir last_dir_ = Dir::None; // StickToDir's hysteresis
    Dir latched_ = Dir::None;
    double latch_until_ = 0.0;
};

} // namespace PortVr::VisorDpad
