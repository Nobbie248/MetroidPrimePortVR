#include "vr/vr_room_scale.h"
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "room-scale regression: %s\n", message);
        std::abort();
    }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1.e-5f; }

struct Vector {
    float x = 0.f, y = 0.f, z = 0.f;
    Vector operator*(float scale) const { return {x * scale, y * scale, z * scale}; }
};
struct Physics {
    Vector position, velocity, force, momentum;
    float angular = 0.f;
    Vector GetTranslation() const { return position; }
    void SetTranslation(const Vector& value) { position = value; }
};
struct Actor {
    Physics physics{{0.f, 0.f, 0.f}, {2.f, 1.f, -.2f}, {0.f, 0.f, -9.f}, {1.f, 0.f, 0.f}, 3.f};
    Vector primitiveOffset{0.f, 0.f, .1f};
    Physics lastSafe = physics;
    bool onFloor = true;
    Physics GetPhysicsState() const { return physics; }
    Physics GetLastNonCollidingState() const { return lastSafe; }
    void SetLastNonCollidingState(const Physics& value) { lastSafe = value; }
    Vector GetPrimitiveOffset() const { return primitiveOffset; }
    void ClearForcesAndTorques() { physics.force = {}; }
    void SetMomentumWR(Vector value) { physics.momentum = value; }
    float GetAngularMomentumWR() const { return physics.angular; }
    void SetAngularMomentumWR(float value) { physics.angular = value; }
    void SetVelocityWR(Vector value) { physics.velocity = value; }
    Vector GetTranslation() const { return physics.position; }
    void SetPhysicsState(const Physics& value) { physics = value; }
    void MoveCollisionPrimitive(Vector value) { primitiveOffset = value; }
};
}

int main() {
    // Check the adapter around the actual game solver: tracked velocity is
    // temporary, whereas resolved steps and floor/contact changes persist.
    Actor actor;
    PortVr::RoomScale::MoveGroundedDisplacement(actor, Vector{.1f, 0.f, 0.f}, .02f, [&] {
        Check(Near(actor.physics.velocity.x, 5.f) && actor.physics.velocity.z == 0.f,
              "the ground solver receives precisely the tracked horizontal displacement");
        Check(actor.physics.force.z == 0.f && actor.physics.momentum.x == 0.f && actor.physics.angular == 0.f,
              "the extra ground solve does not integrate gravity, stick momentum or body rotation twice");
        actor.physics.position = {.1f, 0.f, .25f};
        actor.lastSafe = actor.physics;
        actor.physics.velocity = {};
        actor.primitiveOffset = {};
    });
    Check(Near(actor.physics.position.x, .1f) && Near(actor.physics.position.z, .25f),
          "the normal ground solver's resolved step height is retained");
    Check(Near(actor.physics.velocity.x, 2.f) && Near(actor.physics.velocity.z, -.2f) &&
              Near(actor.physics.force.z, -9.f) && Near(actor.physics.momentum.x, 1.f) &&
              Near(actor.physics.angular, 3.f) && Near(actor.primitiveOffset.z, .1f) &&
              Near(actor.lastSafe.velocity.x, 2.f) && Near(actor.lastSafe.position.z, .25f),
          "stick velocity, gravity, momentum, body rotation and primitive offset are restored");
    PortVr::RoomScale::MoveGroundedDisplacement(actor, Vector{.2f, 0.f, 0.f}, .02f, [&] {
        actor.physics.velocity = {}; // The normal solver rejected a wall/too-high step.
    });
    Check(Near(actor.physics.position.x, .1f) && Near(actor.physics.velocity.x, 2.f),
          "a blocked physical step does not create backwards recoil or teleport through a wall");
    PortVr::RoomScale::MoveGroundedDisplacement(actor, Vector{.1f, 0.f, 0.f}, .02f, [&] {
        actor.physics.position.x += .1f;
        actor.onFloor = false; // The solver found no floor at a ledge.
    });
    Check(!actor.onFloor && Near(actor.physics.position.x, .2f),
          "walking off a ledge keeps the solver's falling transition");
    PortVr::RoomScale::MoveGroundedDisplacement(actor, Vector{1.f, 0.f, 0.f}, 0.f, [&] {
        Check(false, "a zero-duration tick must not run a ground solve");
    });
    PortVr::RoomScale::State state;
    const std::array<float, 3> base{1.f, 1.7f, 2.f};
    auto step = state.Update(true, true, 1, base, base);
    Check(step == std::array<float, 3>{}, "first pose must not teleport");
    step = state.Update(true, true, 1, {1.1f, 1.2f, 1.8f}, base);
    Check(Near(step[0], 0.1f) && Near(step[2], -0.2f) && step[1] == 0.f,
          "walking is horizontal; crouching must not move the body vertically");
    Check(Near(state.consumed[0], 0.1f) && Near(state.consumed[2], -0.2f),
          "stereo displacement must be consumed exactly once");
    // Even if collision rejects this step, only the next new delta is applied.
    step = state.Update(true, true, 1, {1.15f, 1.2f, 1.8f}, base);
    Check(Near(step[0], 0.05f), "blocked movement must not build up a backlog");
    step = state.Update(true, true, 2, {4.f, 1.7f, 5.f}, {4.f, 1.7f, 5.f});
    Check(step == std::array<float, 3>{} && state.consumed == std::array<float, 3>{},
          "recenter changes the origin without walking");
    state.Update(false, true, 2, {}, {});
    step = state.Update(true, true, 2, {4.3f, 1.7f, 5.f}, {4.f, 1.7f, 5.f});
    Check(step == std::array<float, 3>{}, "tracking recovery must not teleport");
    state.Update(true, false, 2, {4.4f, 1.7f, 5.f}, {4.f, 1.7f, 5.f});
    step = state.Update(true, true, 2, {4.5f, 1.7f, 5.f}, {4.f, 1.7f, 5.f});
    Check(step == std::array<float, 3>{}, "movement while morphed/frozen must not be replayed");
    step = state.Update(true, true, 2, {7.f, 1.7f, 5.f}, {4.f, 1.7f, 5.f});
    Check(step == std::array<float, 3>{}, "large tracking jumps must not walk through the level");
    step = state.Update(true, true, 2, {7.05f, 1.7f, 5.f}, {4.f, 1.7f, 5.f});
    Check(Near(step[0], 0.05f), "walking resumes after a tracking jump without backlog");
    std::puts("room-scale tracking tests passed");
}
