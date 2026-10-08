// Directional movement (platform/include/vr/vr_directional_move.h): the
// controller's heading in Samus's frame, the stick read along it, and
// PrimedGun's speed ramp.

#include "vr/vr_directional_move.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr directional move regression failed: %s\n", what);
    std::abort();
  }
}

bool Near(float a, float b, float tolerance = 1.0e-4f) { return std::fabs(a - b) <= tolerance; }

constexpr float kPi = 3.14159265358979f;

// OpenXR orientations (x, y, z, w).
std::array<float, 4> AboutY(float radians) { // yaw, positive turns left
  return {0.0f, std::sin(0.5f * radians), 0.0f, std::cos(0.5f * radians)};
}
std::array<float, 4> AboutX(float radians) { // pitch, positive aims up
  return {std::sin(0.5f * radians), 0.0f, 0.0f, std::cos(0.5f * radians)};
}
std::array<float, 4> AboutZ(float radians) { // roll about the aim
  return {0.0f, 0.0f, std::sin(0.5f * radians), std::cos(0.5f * radians)};
}
std::array<float, 4> Multiply(const std::array<float, 4>& a, const std::array<float, 4>& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

bool HeadingIs(const std::array<float, 2>& heading, float right, float forward) {
  return Near(heading[0], right, 1.0e-3f) && Near(heading[1], forward, 1.0e-3f);
}
} // namespace

int main() {
  using namespace PortVr::DirectionalMove;

  // The heading.
  Check(HeadingIs(Heading({0.0f, 0.0f, 0.0f, 1.0f}), 0.0f, 1.0f), "no rotation: straight ahead");
  Check(HeadingIs(Heading(AboutY(0.5f * kPi)), -1.0f, 0.0f), "turned a quarter left: left");
  Check(HeadingIs(Heading(AboutY(-0.5f * kPi)), 1.0f, 0.0f), "turned a quarter right: right");
  Check(HeadingIs(Heading(AboutY(kPi)), 0.0f, -1.0f), "turned round: behind");
  {
    const float yaw = -30.0f * kPi / 180.0f;
    const std::array<float, 2> expected{std::sin(-yaw), std::cos(yaw)};
    for (const float pitch : {-89.0f, -60.0f, -20.0f, 0.0f, 20.0f, 60.0f, 89.0f}) {
      const auto q = Multiply(AboutY(yaw), AboutX(pitch * kPi / 180.0f));
      Check(HeadingIs(Heading(q), expected[0], expected[1]), "aimed up or down: the yaw's heading");
    }
    // Rolled about its aim, held level: the aim's own heading.
    const auto rolled = Multiply(AboutY(yaw), AboutZ(0.7f));
    Check(HeadingIs(Heading(rolled), expected[0], expected[1]), "rolled and level: the yaw's heading");
    // Pitched and rolled at once, short of where the top takes over: still
    // exact (the top's own heading would be off by the roll).
    for (const float pitch : {-70.0f, -45.0f, 30.0f, 70.0f}) {
      for (const float roll : {-0.7f, 0.5f, 1.2f}) {
        const auto q = Multiply(Multiply(AboutY(yaw), AboutX(pitch * kPi / 180.0f)), AboutZ(roll));
        Check(HeadingIs(Heading(q), expected[0], expected[1]), "pitched and rolled: the yaw's heading");
      }
    }
  }
  Check(HeadingIs(Heading(Multiply(AboutY(-0.5f * kPi), AboutX(-0.5f * kPi))), 1.0f, 0.0f),
        "aimed straight at the floor while facing right: right, from the controller's top");
  Check(HeadingIs(Heading(AboutX(0.5f * kPi)), 0.0f, 1.0f), "aimed straight up: ahead, from the controller's top");
  Check(HeadingIs(Heading({0.0f, 0.0f, 0.0f, 0.0f}), 0.0f, 1.0f), "a degenerate orientation: ahead");

  // The stick along the heading.
  {
    const std::array<float, 2> ahead{0.0f, 1.0f};
    Check(!FromStick(ahead, 0.1f, 0.2f, 0.25f).moving, "inside the deadzone: no wish");
    Check(!FromStick(ahead, 0.0f, 0.0f, 0.0f).moving, "a centred stick with no deadzone: no wish");
    const Wish up = FromStick(ahead, 0.0f, 1.0f, 0.25f);
    Check(up.moving && Near(up.right, 0.0f) && Near(up.forward, 1.0f) && Near(up.push, 1.0f), "up: forward");
    const Wish right = FromStick(ahead, 0.6f, 0.0f, 0.25f);
    Check(right.moving && Near(right.right, 1.0f) && Near(right.forward, 0.0f) && Near(right.push, 0.6f),
          "right: strafe right, at the stick's push");
    const Wish back = FromStick(ahead, 0.0f, -0.5f, 0.25f);
    Check(back.moving && Near(back.forward, -1.0f) && Near(back.push, 0.5f), "down: back");
    const Wish diagonal = FromStick(ahead, 0.9f, 0.9f, 0.25f);
    Check(diagonal.moving && Near(diagonal.right, std::sqrt(0.5f)) && Near(diagonal.forward, std::sqrt(0.5f)) &&
              Near(diagonal.push, 1.0f),
          "a full diagonal: push capped at 1");
    const Wish edge = FromStick(ahead, 0.0f, 0.25f, 0.25f);
    Check(edge.moving && Near(edge.push, 0.25f), "at the deadzone: moving, the push not rescaled");
  }
  {
    // The hand turned a quarter right: up walks right, right walks back.
    const std::array<float, 2> right_hand{1.0f, 0.0f};
    const Wish up = FromStick(right_hand, 0.0f, 1.0f, 0.25f);
    Check(Near(up.right, 1.0f) && Near(up.forward, 0.0f), "heading right, up: right");
    const Wish right = FromStick(right_hand, 1.0f, 0.0f, 0.25f);
    Check(Near(right.right, 0.0f) && Near(right.forward, -1.0f), "heading right, right: back");
    const Wish left = FromStick(right_hand, -1.0f, 0.0f, 0.25f);
    Check(Near(left.right, 0.0f) && Near(left.forward, 1.0f), "heading right, left: forward");
  }

  // The speed ramp.
  {
    const float dt = 1.0f / 60.0f;
    Ramp ramp;
    Check(Near(ramp.Step(0.0f, 14.0f, 45.0f, dt), 0.75f), "from standing: one tick of 45 per second");
    Check(Near(ramp.Step(0.75f, 14.0f, 45.0f, dt), 1.5f), "and the next");
    float speed = 0.0f;
    for (int i = 0; i < 60; ++i) {
      speed = ramp.Step(speed, 14.0f, 45.0f, dt);
    }
    Check(Near(speed, 14.0f), "it reaches the target and holds it");
    Check(Near(ramp.Step(14.0f, 7.0f, 45.0f, dt), 7.0f), "easing the stick back: the new target at once");
    Check(Near(ramp.Step(3.0f, 7.0f, 45.0f, dt), 7.0f), "a wall slowing her does not drop the ramp");

    ramp.Reset();
    Check(Near(ramp.Step(10.0f, 14.0f, 45.0f, dt), 10.75f), "starting while moving: from her speed");
    ramp.Reset();
    Check(Near(ramp.Step(25.0f, 14.0f, 8.0f, dt), 14.0f), "starting faster than the target: the target");
    ramp.Reset();
    Check(Near(ramp.Step(6.0f, 14.0f, 8.0f, dt), 6.0f + 8.0f * dt), "in the air: the air acceleration");
    ramp.Reset();
    Check(Near(ramp.Step(NAN, 14.0f, 45.0f, dt), 0.75f), "a non-finite speed counts as standing");
    ramp.Reset();
    Check(Near(ramp.Step(5.0f, 14.0f, 0.0f, dt), 5.0f), "no acceleration: she keeps her speed");
  }

  std::printf("vr directional move: all checks passed\n");
  return 0;
}
