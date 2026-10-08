// Look to scan's maths (platform/include/vr/vr_look_scan.h): PrimedGun's ray
// measures, its two cones and its scores, with the values PrimedGun's
// NativeRuntime.cpp gives for the same inputs.

#include "vr/vr_look_scan.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr look to scan regression failed: %s\n", what);
    std::abort();
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
} // namespace

int main() {
  using namespace PortVr::LookScan;

  // Rotating a target around the head must not shrink its icon as the body's
  // forward depth approaches zero, or goes negative after a head turn.
  const float iconScale = ScanIndicatorScale({0.f, 10.f, 0.f}, 640.f, 16.f, 32.f);
  for (const Vec3 offset : {Vec3{-10.f, 0.f, 0.f}, Vec3{10.f, 0.f, 0.f},
                            Vec3{6.f, 8.f, 0.f}, Vec3{0.f, -10.f, 0.f},
                            Vec3{0.f, 0.f, 10.f}}) {
    Check(Near(ScanIndicatorScale(offset, 640.f, 16.f, 32.f), iconScale),
          "scan icon size is independent of bearing around the head");
  }
  Check(Near(iconScale, 0.5f), "near scan icons retain their maximum size clamp");
  Check(Near(ScanIndicatorScale({0.f, 100.f, 0.f}, 640.f, 16.f, 32.f), 2.5f),
        "distant scan icons retain their minimum size clamp");
  Check(Near(ScanIndicatorScale({}, 640.f, 16.f, 32.f), 1.f),
        "an icon at the head has a finite scale");

  // The gaze: from the origin along Prime's forward (+y).
  const Ray ray{{0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
  Metrics m;

  Check(MetricsForPoint(ray, {0.f, 10.f, 0.f}, 60.f, m) && Near(m.along, 10.f) && Near(m.perp, 0.f),
        "a point on the gaze is on it");
  Check(MetricsForPoint(ray, {3.f, 10.f, 4.f}, 60.f, m) && Near(m.along, 10.f) && Near(m.perp, 5.f),
        "a point off the gaze is measured across it");
  Check(!MetricsForPoint(ray, {0.f, -1.f, 0.f}, 60.f, m), "a point behind the head is ignored");
  Check(MetricsForPoint(ray, {0.f, -0.3f, 2.f}, 60.f, m) && Near(m.along, 0.f) && Near(m.perp, std::sqrt(4.09f)),
        "a point just behind the eye is measured from the eye");
  Check(!MetricsForPoint(ray, {0.f, 70.f, 0.f}, 60.f, m), "a point beyond the reach is ignored");

  // A box on the gaze: the entry point, its extent across the gaze.
  const Aabb ahead{{-1.f, 9.f, -1.f}, {1.f, 11.f, 1.f}};
  Check(MetricsForAabb(ray, ahead, 60.f, m) && Near(m.along, 9.f) && Near(m.perp, 0.f) && Near(m.point.y, 9.f) &&
            Near(m.radius, std::sqrt(2.f)),
        "a box the gaze hits is entered");
  // A box beside it: the centre's distance less the extent.
  const Aabb beside{{4.f, 9.f, -1.f}, {6.f, 11.f, 1.f}};
  Check(MetricsForAabb(ray, beside, 60.f, m) && Near(m.along, 10.f) && Near(m.perp, 5.f - std::sqrt(2.f)),
        "a box beside the gaze is measured to its surface");
  const Aabb around{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
  Check(MetricsForAabb(ray, around, 60.f, m) && Near(m.along, 0.f) && Near(m.perp, 0.f),
        "a box around the head is hit at once");
  const float nan = std::numeric_limits<float>::quiet_NaN();
  Check(!MetricsForAabb(ray, Aabb{{nan, 0.f, 0.f}, {1.f, 1.f, 1.f}}, 60.f, m), "a broken box is ignored");

  // Which boxes scan aim trusts: at most twice the radius, between a floor and a cap.
  Check(RenderAabbUsableForAim(Aabb{{0.f, 0.f, 0.f}, {8.f, 1.f, 1.f}}, 4.f), "an 8 unit render box is trusted");
  Check(!RenderAabbUsableForAim(Aabb{{0.f, 0.f, 0.f}, {8.5f, 1.f, 1.f}}, 4.f), "a wider render box is not");
  Check(RenderAabbUsableForAim(Aabb{{0.f, 0.f, 0.f}, {20.f, 1.f, 1.f}}, 10.f), "a larger radius trusts more");
  Check(!PhysicsAabbUsableForAim(Aabb{{0.f, 0.f, 0.f}, {10.5f, 1.f, 1.f}}, 10.f), "collision boxes cap at 10");

  // The cones at ten units, radius 4, a point.
  Metrics at10;
  at10.along = 10.f;
  at10.valid = true;
  Check(Near(TargetConePerp(4.f, at10), 3.06f), "the target cone widens with distance");
  Check(Near(VisualConePerp(4.f, at10), 17.5f), "the icon cone is much wider");
  Metrics far = at10;
  far.along = 60.f;
  Check(Near(TargetConePerp(4.f, far), 11.f) && Near(VisualConePerp(4.f, far), 32.f), "both cones are capped");
  Check(Near(TargetConePerp(nan, at10), 3.06f), "a broken radius falls back to 4");
  Check(Near(LockConePerp(4.f, at10, false), 3.06f) && Near(LockConePerp(4.f, at10, true), 3.978f),
        "lock-on uses the target cone, a grapple point 1.3 times wider");

  // Scores: nearer the gaze wins, then nearer the head.
  Metrics onGaze = at10;
  Metrics halfCone = at10;
  halfCone.perp = 1.53f;
  Check(Near(TargetScore(onGaze, 3.06f), 0.03f), "on the gaze scores its distance only");
  Check(Near(TargetScore(halfCone, 3.06f), 1.336f), "half way out of the cone");
  Metrics nearer = onGaze;
  nearer.along = 5.f;
  Check(TargetScore(nearer, 3.06f) < TargetScore(onGaze, 3.06f), "the nearer of two on the gaze wins");
  Check(VisualScore(onGaze, 17.5f) < VisualScore(halfCone, 17.5f), "icons rank by the gaze too");
  Check(Prefer(halfCone, Metrics{}) && Prefer(onGaze, halfCone) && !Prefer(halfCone, onGaze),
        "the measure nearest the gaze is kept");

  Check(Near(MaxAlong(200.f), 100.f) && Near(MaxAlong(5.f), 15.f) && Near(MaxAlong(60.f), 60.f),
        "the reach stays between 15 and 100");

  Check(KeepPrevious(1.1f, 1.0f) && !KeepPrevious(1.3f, 1.0f), "the current pick stays while close");
  Check(KeepPrevious(0.04f, 0.0f) && !KeepPrevious(0.06f, 0.0f), "even against a perfect score, by a margin");

  std::puts("port_vr_look_scan_tests: ok");
  return 0;
}
