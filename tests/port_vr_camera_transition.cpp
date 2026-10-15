// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/vr_camera_transition.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "VR camera transition regression: %s\n", message);
        std::abort();
    }
}
}

int main() {
    using namespace PortVr::CameraTransition;
    Check(Duration(0.f) == .5f && Duration(-1.f) == .5f,
          "instant-cut hints receive a finite half-second handoff");
    Check(Duration(std::numeric_limits<float>::quiet_NaN()) == .5f,
          "invalid hint timing cannot poison the camera transform");
    Check(Duration(.001f) >= .1f && Duration(2.f) == 2.f,
          "sub-frame hints are bounded while authored long blends are retained");
    Check(Blend(0.f, .5f) == 0.f && Blend(.5f, .5f) == 1.f,
          "the handoff starts at the source and reaches the exact target");
    Check(Blend(-.1f, .5f) == 0.f && Blend(.7f, .5f) == 1.f,
          "time outside the transition cannot overshoot either camera");
    float previous = 0.f;
    for (int tick = 0; tick <= 30; ++tick) {
        const float blend = Blend(tick / 60.f, .5f);
        Check(std::isfinite(blend) && blend >= previous && blend <= 1.f,
              "the camera progresses smoothly and monotonically at 60 Hz");
        previous = blend;
    }
    Check(Blend(.001f, .5f) < .00002f && 1.f - Blend(.499f, .5f) < .00002f,
          "both endpoints ease in rather than stepping at a camera-ID change");
    Check(Blend(.25f, 0.f) == .5f, "zero-time hint metadata still has a usable midpoint");
    std::puts("port_vr_camera_transition_tests: ok");
}
