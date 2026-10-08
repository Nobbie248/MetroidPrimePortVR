// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "vr/openxr_screen_math.h"
#include "vr/prime_vr_policy.h"

namespace PortVr {

// Like PrimedGun's cinematic screen generation: capture the latest head pose
// when a screen opens, then hold the screen in application space until closed.
class VirtualScreenAnchor {
public:
    void Reset() noexcept { *this = VirtualScreenAnchor{}; }

    void Update(VRPresentationMode presentation, VRGameMode gameMode,
                const screen_math::Pose* trackedHead, float distance, int64_t displayTimeNs) noexcept {
        if (trackedHead != nullptr) {
            lastHead_ = *trackedHead;
            headValid_ = true;
        }
        const bool visible = presentation == VRPresentationMode::VirtualScreen;
        // Scene loading can briefly report gameplay/transition between camera
        // shots. Keep the cinema anchor until a full second outside cinema;
        // an intentional menu/front-end change still captures its own facing.
        const bool cinemaContinuation = gameMode == VRGameMode::Cinematic ||
                                        gameMode == VRGameMode::Transition ||
                                        gameMode == VRGameMode::Unknown || gameMode == VRGameMode::InGame;
        const bool cinemaCooldown = cinemaTimeValid_ && cinemaContinuation &&
                                    displayTimeNs >= lastCinemaTimeNs_ &&
                                    displayTimeNs - lastCinemaTimeNs_ < 1'000'000'000;
        if ((!visible || !visible_ || gameMode != gameMode_) && !cinemaCooldown) {
            valid_ = false;
        }
        if (!cinemaContinuation || (cinemaTimeValid_ && !cinemaCooldown)) {
            cinemaTimeValid_ = false;
        }
        visible_ = visible;
        gameMode_ = gameMode;
        if (visible && gameMode == VRGameMode::Cinematic && (valid_ || headValid_)) {
            lastCinemaTimeNs_ = displayTimeNs;
            cinemaTimeValid_ = true;
        }
        if (!visible || valid_ || !headValid_) {
            return;
        }
        const auto& q = lastHead_.orientation;
        const float yaw = std::atan2(2.f * (q[0] * q[2] + q[3] * q[1]),
                                    1.f - 2.f * (q[0] * q[0] + q[1] * q[1]));
        pose_.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
        const auto offset = screen_math::Rotate(pose_.orientation, {0.f, 0.f, -std::max(0.25f, distance)});
        for (int axis = 0; axis < 3; ++axis) {
            pose_.position[axis] = lastHead_.position[axis] + offset[axis];
        }
        valid_ = true;
    }

    bool Valid() const noexcept { return visible_ && valid_; }
    const screen_math::Pose& Pose() const noexcept { return pose_; }

private:
    screen_math::Pose lastHead_{};
    screen_math::Pose pose_{};
    VRGameMode gameMode_ = VRGameMode::Unknown;
    bool headValid_ = false;
    bool visible_ = false;
    bool valid_ = false;
    int64_t lastCinemaTimeNs_ = 0;
    bool cinemaTimeValid_ = false;
};

} // namespace PortVr
