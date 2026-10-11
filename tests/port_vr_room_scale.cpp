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
}

int main() {
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
