#include "port_surface_culling_math.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "surface culling: %s\n", message); std::abort(); }
}
using namespace PortSurfaceCulling;
constexpr Vector origin{0, 0, 0}, right{1, 0, 0}, forward{0, 1, 0}, up{0, 0, 1};
constexpr float pi = 3.14159265358979f;
constexpr float degree = pi / 180.f;
constexpr std::array<float, 4> square{-pi / 4, pi / 4, pi / 4, -pi / 4};
Bounds Point(float x, float y, float z) { return {{x, y, z}, {x, y, z}}; }
}

int main() {
  EyeVolume eye;
  Check(eye.Build(origin, right, forward, up, square, 0, 0), "valid square view");
  Check(eye.Visible(Point(0, 10, 0)), "forward visible");
  Check(!eye.Visible(Point(11, 10, 0)) && !eye.Visible(Point(-11, 10, 0)), "outside left/right");
  Check(!eye.Visible(Point(0, 10, 11)) && !eye.Visible(Point(0, 10, -11)), "outside top/bottom");
  Check(!eye.Visible(Point(0, -1, 0)), "behind both side planes");
  Check(eye.Visible(Point(10, 10, 0)), "touching a plane stays visible");
  Check(eye.Visible({{9.9f, 10, -1}, {12, 11, 1}}), "intersecting box survives even with centre outside");
  Check(eye.Visible({{-100, 5, -100}, {100, 5.1f, 100}}), "large wall spans view with all corners outside");
  Check(eye.Visible({{-1, -1, -1}, {1, 1, 1}}), "camera inside bounds");
  Check(eye.Visible(Point(0, 100000, 0)), "no arbitrary far cutoff");

  EyeVolume padded;
  Check(padded.Build(origin, right, forward, up, square, 7.5f * degree, 0.1f), "padded view");
  Check(padded.Visible(Point(std::tan(50.f * degree) * 10.f, 10, 0)), "angular head-motion margin");
  Check(padded.Visible(Point(0, -0.09f, 0)), "translation margin behind the eye");
  Check(!padded.Visible(Point(0, -0.3f, 0)), "outside translation margin still rejects");

  StereoVolume stereo;
  const std::array<float, 4> narrow{-20 * degree, 20 * degree, 20 * degree, -20 * degree};
  Check(stereo.eyes[0].Build({-0.035f, 0, 0}, right, forward, up, narrow, 0, 0) &&
        stereo.eyes[1].Build({0.035f, 0, 0}, right, forward, up, narrow, 0, 0), "stereo eye origins");
  const auto leftOnly = Point(-0.065f, 0.1f, 0), rightOnly = Point(0.065f, 0.1f, 0);
  Check(stereo.eyes[0].Visible(leftOnly) && !stereo.eyes[1].Visible(leftOnly) && stereo.Visible(leftOnly),
        "left-eye-only geometry stays");
  Check(stereo.eyes[1].Visible(rightOnly) && !stereo.eyes[0].Visible(rightOnly) && stereo.Visible(rightOnly),
        "right-eye-only geometry stays");
  Check(!stereo.Visible(Point(1, 0.1f, 0)), "outside both eyes is rejected");
  stereo.eyes[1].valid = false;
  Check(stereo.Visible(Point(1, 0.1f, 0)), "unknown eye keeps geometry");

  EyeVolume asymmetric;
  Check(asymmetric.Build(origin, right, forward, up, {-0.2f, 1.f, 0.5f, -0.3f}, 0, 0), "asymmetric view");
  Check(asymmetric.Visible(Point(1, 1, 0)) && !asymmetric.Visible(Point(-1, 1, 0)), "asymmetric horizontal FOV");
  Check(asymmetric.Visible(Point(0, 1, 0.4f)) && !asymmetric.Visible(Point(0, 1, -0.4f)), "asymmetric vertical FOV");
  EyeVolume rotated;
  Check(rotated.Build({10, 20, 30}, {0, 1, 0}, {-1, 0, 0}, up, square, 0, 0), "world-space camera rotation");
  Check(rotated.Visible(Point(9, 20, 30)) && !rotated.Visible(Point(11, 20, 30)), "rotated translated view");

  const float nan = std::numeric_limits<float>::quiet_NaN();
  Check(!ValidBounds({{2, 0, 0}, {1, 1, 1}}) && !ValidBounds({{nan, 0, 0}, {1, 1, 1}}), "invalid bounds detected");
  Check(eye.Visible({{2, 0, 0}, {1, 1, 1}}) && eye.Visible({{nan, 0, 0}, {1, 1, 1}}), "invalid bounds fail open");
  Check(!rotated.Build(origin, right, forward, up, {0, 0, 0, 0}, 0.1f, 0.1f), "missing FOV rejected before padding");
  Check(rotated.Visible(Point(1000, -10, 0)), "failed build invalidates previous volume");
  Check(!rotated.Build(origin, right, forward, up, {-1.5f, 1.5f, 1.f, -1.f}, 0.2f, 0), "overwide FOV fails open");
  Check(!rotated.Build(origin, right, right, up, square, 0, 0), "invalid camera basis fails open");

  // Independent projection check: every sampled box containing a point visible
  // through either asymmetric eye must survive the plane/AABB rejection test.
  const std::array<float, 4> fov{-0.8f, 0.6f, 0.7f, -0.9f};
  for (int i = 0; i < 2; ++i) {
    Check(stereo.eyes[i].Build({i ? 0.035f : -0.035f, 0, 0}, right, forward, up, fov,
                               7.5f * degree, 0.1f), "asymmetric stereo build");
  }
  std::mt19937 random(13);
  std::uniform_real_distribution<float> distance(0.01f, 100.f), fraction(0.f, 1.f), extent(0.f, 2.f);
  for (int i = 0; i < 10000; ++i) {
    const float y = distance(random);
    const float x = (i & 1 ? 0.035f : -0.035f) + y *
        (std::tan(fov[0]) + fraction(random) * (std::tan(fov[1]) - std::tan(fov[0])));
    const float z = y * (std::tan(fov[3]) + fraction(random) * (std::tan(fov[2]) - std::tan(fov[3])));
    const Bounds box{{x - extent(random), y - extent(random), z - extent(random)},
                     {x + extent(random), y + extent(random), z + extent(random)}};
    Check(stereo.Visible(box), "projected visible geometry must never be rejected");
  }
  std::puts("surface culling checks passed (including 10000 projected boxes)");
}
