// The beam wheel (platform/include/vr/vr_beam_wheel.h): pointing the weapon
// hand up, right, down or left from where the wheel opened picks Power, Wave,
// Ice or Plasma, whatever way the player faces and however the controller is
// rolled, and each beam maps to the C-stick direction and the HUD box the game
// uses for it. The levelled frame once had its right axis negated, which
// rolled the panel upside down and swapped up with down and left with right.

#include "vr/vr_beam_wheel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace PortVr::BeamWheel;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr beam wheel regression failed: %s\n", what);
    std::abort();
  }
}

bool Near(float a, float b, float tolerance = 1.0e-3f) { return std::fabs(a - b) <= tolerance; }

bool Near(const Vec3& a, const Vec3& b, float tolerance = 1.0e-3f) {
  return Near(a[0], b[0], tolerance) && Near(a[1], b[1], tolerance) && Near(a[2], b[2], tolerance);
}

// OpenXR reference space: +X right, +Y up, -Z forward.
Quat AxisAngle(const Vec3& axis, float degrees) {
  const float half = degrees * 3.14159265f / 360.0f;
  const float s = std::sin(half);
  return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(half)};
}

// a * b: rotate by b, then by a.
Quat Mul(const Quat& a, const Quat& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quat Yaw(float degrees) { return AxisAngle({0.0f, 1.0f, 0.0f}, degrees); }   // + turns left
Quat Pitch(float degrees) { return AxisAngle({1.0f, 0.0f, 0.0f}, degrees); } // + aims up
Quat Roll(float degrees) { return AxisAngle({0.0f, 0.0f, 1.0f}, degrees); }

// A controller facing `yaw`, aimed `pitch` up and rolled `roll` about its aim.
Quat Aim(float yaw, float pitch, float roll) { return Mul(Yaw(yaw), Mul(Pitch(pitch), Roll(roll))); }

// The wheel as vr_pad.cpp runs it: open the panel, take the first sample as
// the zero, then pick from a later aim.
Beam Gesture(const Vec3& position, const Quat& start, const Vec3& end_position, const Quat& end) {
  const Panel panel = OpenPanel(position, start);
  const std::array<float, 2> zero = Measure(panel, position, start);
  const std::array<float, 2> now = Measure(panel, end_position, end);
  return Pick(now[0] - zero[0], now[1] - zero[1]);
}

// Turning the wrist from the opening aim: up/down is pitch, right/left is yaw.
void CheckFourWays(float yaw, float pitch, float start_roll, float end_roll, const char* what) {
  const Vec3 p{0.2f, 1.1f, -0.3f};
  const Quat start = Aim(yaw, pitch, start_roll);
  const float turn = 20.0f;
  const bool ok = Gesture(p, start, p, Aim(yaw, pitch + turn, end_roll)) == Beam::Power &&
                  Gesture(p, start, p, Aim(yaw - turn, pitch, end_roll)) == Beam::Wave &&
                  Gesture(p, start, p, Aim(yaw, pitch - turn, end_roll)) == Beam::Ice &&
                  Gesture(p, start, p, Aim(yaw + turn, pitch, end_roll)) == Beam::Plasma &&
                  Gesture(p, start, p, Aim(yaw + 3.0f, pitch - 3.0f, end_roll)) == Beam::None;
  if (!ok) {
    std::fprintf(stderr, "facing %.0f, pitch %.0f, roll %.0f -> %.0f: up %d right %d down %d left %d small %d\n",
                 yaw, pitch, start_roll, end_roll,
                 static_cast<int>(Gesture(p, start, p, Aim(yaw, pitch + turn, end_roll))),
                 static_cast<int>(Gesture(p, start, p, Aim(yaw - turn, pitch, end_roll))),
                 static_cast<int>(Gesture(p, start, p, Aim(yaw, pitch - turn, end_roll))),
                 static_cast<int>(Gesture(p, start, p, Aim(yaw + turn, pitch, end_roll))),
                 static_cast<int>(Gesture(p, start, p, Aim(yaw + 3.0f, pitch - 3.0f, end_roll))));
  }
  Check(ok, what);
}

} // namespace

int main() {
  const Vec3 kRight{1.0f, 0.0f, 0.0f};
  const Vec3 kUp{0.0f, 1.0f, 0.0f};
  const Vec3 kForward{0.0f, 0.0f, -1.0f};
  using PortVr::screen_math::Rotate;

  // The levelled frame keeps the aim and turns right/up the same way as the
  // reference space, so the panel is never upside down.
  {
    const Quat level = RollFree({0.0f, 0.0f, 0.0f, 1.0f});
    Check(Near(Rotate(level, kRight), kRight), "a level controller keeps +X as its right");
    Check(Near(Rotate(level, kUp), kUp), "a level controller keeps +Y as its up");
    Check(Near(Rotate(level, kForward), kForward), "a level controller keeps its aim");

    const Quat rolled = RollFree(Roll(40.0f));
    Check(Near(Rotate(rolled, kRight), kRight) && Near(Rotate(rolled, kUp), kUp), "roll is removed");

    const Quat facing_x = RollFree(Yaw(-90.0f));
    Check(Near(Rotate(facing_x, kForward), {1.0f, 0.0f, 0.0f}), "a controller turned right aims at +X");
    Check(Near(Rotate(facing_x, kRight), {0.0f, 0.0f, 1.0f}), "and its right is +Z");

    const Quat raised = RollFree(Aim(30.0f, 35.0f, -60.0f));
    const Vec3 right = Rotate(raised, kRight);
    const Vec3 up = Rotate(raised, kUp);
    Check(Near(right[1], 0.0f), "a raised, rolled controller's right stays level");
    Check(up[1] > 0.5f, "and its up still points up");
    Check(Near(Rotate(raised, kForward), Rotate(Aim(30.0f, 35.0f, -60.0f), kForward)), "and it keeps its aim");
  }

  // The panel sits 5.5 cm above the aim, so the opening ray meets it below its
  // centre; that first sample is the gesture's zero.
  {
    const Panel panel = OpenPanel({0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    Check(Near(panel.center, {0.0f, 1.055f, -0.26f}), "the panel is 26 cm ahead and 5.5 cm up");
    const std::array<float, 2> zero = Measure(panel, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    Check(Near(zero[0], 0.0f) && Near(zero[1], -0.055f / 0.21f), "the opening aim hits below the centre");
  }

  // Up Power, right Wave, down Ice, left Plasma: facing any way, aimed level
  // or raised, with the controller rolled differently at the start and end.
  CheckFourWays(0.0f, 0.0f, 0.0f, 0.0f, "level, facing -Z");
  CheckFourWays(-90.0f, 0.0f, 0.0f, 0.0f, "level, facing +X");
  CheckFourWays(135.0f, 0.0f, 0.0f, 0.0f, "level, facing back-left");
  CheckFourWays(0.0f, 25.0f, 0.0f, 0.0f, "aimed up");
  CheckFourWays(60.0f, -20.0f, 0.0f, 0.0f, "aimed down, facing left");
  CheckFourWays(-30.0f, 10.0f, 50.0f, -35.0f, "rolled");

  // A ray that misses the panel falls back to the hand's travel in the
  // panel's frame: up is up and right is right there too.
  {
    const Vec3 p{0.0f, 1.0f, 0.0f};
    const Quat down = Pitch(-90.0f); // aimed at the floor, parallel to the panel
    const Panel panel = OpenPanel(p, {0.0f, 0.0f, 0.0f, 1.0f});
    const std::array<float, 2> raised = Measure(panel, {0.0f, 1.1f, 0.0f}, down);
    Check(Near(raised[0], 0.0f) && Near(raised[1], 0.1f / 0.075f), "raising the hand reads as up");
    const std::array<float, 2> moved = Measure(panel, {0.1f, 1.0f, 0.0f}, down);
    Check(Near(moved[0], 0.1f / 0.075f) && Near(moved[1], 0.0f), "moving it right reads as right");

    const Panel turned = OpenPanel(p, Yaw(-90.0f)); // facing +X, right is +Z
    const std::array<float, 2> side = Measure(turned, {0.0f, 1.0f, 0.1f}, down);
    Check(Near(side[0], 0.1f / 0.075f) && Near(side[1], 0.0f), "right follows the facing");
  }

  // The pick: dominant axis, clamped, with a 0.25 deadzone.
  Check(Pick(0.1f, -0.2f) == Beam::None, "inside the deadzone nothing is picked");
  Check(Pick(0.3f, 0.0f) == Beam::Wave && Pick(-0.3f, 0.0f) == Beam::Plasma, "x picks Wave / Plasma");
  Check(Pick(0.0f, 0.3f) == Beam::Power && Pick(0.0f, -0.3f) == Beam::Ice, "y picks Power / Ice");
  Check(Pick(0.5f, 0.6f) == Beam::Power && Pick(-0.7f, 0.6f) == Beam::Plasma, "the larger axis wins");
  Check(Pick(1.2f, 1.6f) == Beam::Wave, "both clamp to the panel's edge, and a tie goes to x (PrimedGun)");

  // The game's side: C-stick up Power, right Wave, down Ice, left Plasma, and
  // EBeamId's order (Power, Ice, Wave, Plasma) for the HUD's boxes.
  Check(CStickDirection(Beam::Power) == std::array<int, 2>{0, 1}, "Power is C-stick up");
  Check(CStickDirection(Beam::Wave) == std::array<int, 2>{1, 0}, "Wave is C-stick right");
  Check(CStickDirection(Beam::Ice) == std::array<int, 2>{0, -1}, "Ice is C-stick down");
  Check(CStickDirection(Beam::Plasma) == std::array<int, 2>{-1, 0}, "Plasma is C-stick left");
  Check(CStickDirection(Beam::None) == std::array<int, 2>{0, 0}, "no beam leaves the C-stick centred");
  Check(GameBeamId(Beam::Power) == 0 && GameBeamId(Beam::Ice) == 1 && GameBeamId(Beam::Wave) == 2 &&
            GameBeamId(Beam::Plasma) == 3 && GameBeamId(Beam::None) == -1,
        "the HUD boxes follow EBeamId");

  std::puts("vr beam wheel: all checks passed");
  return 0;
}
