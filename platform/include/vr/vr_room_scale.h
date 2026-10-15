// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace PortVr::RoomScale {
// Run the game's ground solver for an extra tracked displacement, without
// integrating stick velocity/gravity twice or retaining artificial walking
// velocity. Keep its resolved position and floor/contact bookkeeping.
template <class Actor, class Vector, class GroundSolver>
void MoveGroundedDisplacement(Actor& actor, const Vector& displacement, float dt,
                             GroundSolver&& solve) {
    if (!(dt > 0.f) || !std::isfinite(dt)) {
        return;
    }
    auto physics = actor.GetPhysicsState();
    auto lastSafe = actor.GetLastNonCollidingState();
    const auto primitiveOffset = actor.GetPrimitiveOffset();
    actor.ClearForcesAndTorques();
    actor.SetMomentumWR(displacement * 0.f);
    actor.SetAngularMomentumWR(actor.GetAngularMomentumWR() * 0.f);
    actor.SetVelocityWR(displacement * (1.f / dt));
    solve();
    physics.SetTranslation(actor.GetTranslation());
    lastSafe.SetTranslation(actor.GetLastNonCollidingState().GetTranslation());
    actor.SetPhysicsState(physics);
    actor.SetLastNonCollidingState(lastSafe);
    actor.MoveCollisionPrimitive(primitiveOffset);
}

// Game-thread tracking history. Distances here are metres in OpenXR space.
// Consume blocked movement too: pushing against a wall must not accumulate
// a displacement that launches the player when they turn away from it.
struct State {
    bool valid = false;
    bool moving = false;
    uint64_t generation = 0;
    std::array<float, 3> previous{};
    std::array<float, 3> consumed{};

    std::array<float, 3> Update(bool tracked, bool enabled, uint64_t origin,
                              const std::array<float, 3>& head,
                              const std::array<float, 3>& base) noexcept {
        if (!tracked) {
            valid = moving = false;
            consumed = {};
            return {};
        }
        for (float value : head) {
            if (!std::isfinite(value)) {
                valid = moving = false;
                consumed = {};
                return {};
            }
        }
        std::array<float, 3> delta{};
        if (valid && generation == origin && moving && enabled) {
            delta = {head[0] - previous[0], 0.f, head[2] - previous[2]};
            // Tracking jumps are relocations of the tracking origin, not steps.
            if (delta[0] * delta[0] + delta[2] * delta[2] > 0.25f) {
                delta = {};
            }
        }
        if (!valid || generation != origin) {
            consumed = {};
        }
        if (enabled) {
            consumed = {head[0] - base[0], 0.f, head[2] - base[2]};
        }
        previous = head;
        generation = origin;
        valid = true;
        moving = enabled;
        return delta;
    }
};
} // namespace PortVr::RoomScale
