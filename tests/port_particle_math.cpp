#include "Kyoto/Particles/PortParticleMath.hpp"

#include <cstdio>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

bool Near(float a, float b) { return a > b - 0.0001f && a < b + 0.0001f; }

bool Color(const float* out, float r, float g, float b, float a) {
  return Near(out[0], r) && Near(out[1], g) && Near(out[2], b) && Near(out[3], a);
}
} // namespace

int main() {
  using namespace PortParticleMath;

  // DFCS/DFCP, CalculateBlendValue 0x298f44: (d - B) / (A - B), clamped; 1 at A, 0 at B.
  Check(Near(DistanceBlend(10.f, 10.f, 2.f), 1.f), "blend 1 at A");
  Check(Near(DistanceBlend(2.f, 10.f, 2.f), 0.f), "blend 0 at B");
  Check(Near(DistanceBlend(6.f, 10.f, 2.f), 0.5f), "blend midpoint");
  Check(Near(DistanceBlend(20.f, 10.f, 2.f), 1.f), "blend clamps high");
  Check(Near(DistanceBlend(-5.f, 10.f, 2.f), 0.f), "blend clamps low");
  Check(Near(DistanceBlend(6.f, 2.f, 10.f), 0.5f), "blend reversed midpoint");
  Check(Near(DistanceBlend(2.f, 2.f, 10.f), 1.f), "blend reversed 1 at A");
  Check(Near(DistanceBlend(7.f, 5.f, 5.000001f), 0.f), "blend equal children is 0");

  // The Wave trail gradient (f597d7d3): HDR halves, period 60, no repeat.
  const GradientStop wave[] = {
      {{2.121f, 1.066f, 3.619f, 1.f}, 0.f},          {{1.260f, 0.797f, 2.736f, 1.f}, 0.0648f},
      {{0.0463f, 0.4167f, 1.492f, 0.765f}, 0.1561f}, {{0.0589f, 0.4099f, 1.523f, 0.75f}, 0.1620f},
      {{0.638f, 0.0961f, 2.969f, 0.f}, 0.4329f},     {{0.645f, 0.0927f, 2.984f, 0.f}, 0.4358f},
  };
  float out[4];
  Check(EvalGradient(wave, 6, 60, false, 0, out) && Color(out, 2.121f, 1.066f, 3.619f, 1.f), "frame 0 is the first stop, HDR kept");
  // t = 30/60 = 0.5 is past the last stop: held.
  Check(EvalGradient(wave, 6, 60, false, 30, out) && Color(out, 0.645f, 0.0927f, 2.984f, 0.f), "holds the last stop");
  Check(EvalGradient(wave, 6, 60, false, 500, out) && Color(out, 0.645f, 0.0927f, 2.984f, 0.f), "clamps past the period");
  // Frame 6 -> t = 0.1, between stops 1 and 2: f = (0.1 - 0.0648) / (0.1561 - 0.0648).
  {
    const float f = (0.1f - 0.0648f) / (0.1561f - 0.0648f);
    Check(EvalGradient(wave, 6, 60, false, 6, out) &&
              Color(out, 1.260f + (0.0463f - 1.260f) * f, 0.797f + (0.4167f - 0.797f) * f,
                    2.736f + (1.492f - 2.736f) * f, 1.f + (0.765f - 1.f) * f),
          "lerps all four channels, alpha included");
  }

  // Two stops: ramp over the period, then repeat.
  const GradientStop ramp[] = {{{0.f, 0.f, 0.f, 0.f}, 0.f}, {{4.f, 2.f, 1.f, 1.f}, 1.f}};
  Check(EvalGradient(ramp, 2, 10, false, 5, out) && Color(out, 2.f, 1.f, 0.5f, 0.5f), "linear ramp, HDR above 1");
  Check(EvalGradient(ramp, 2, 10, false, 10, out) && Color(out, 4.f, 2.f, 1.f, 1.f), "end of the period is the last stop");
  Check(EvalGradient(ramp, 2, 10, true, 13, out) && Color(out, 1.2f, 0.6f, 0.3f, 0.3f), "repeat is frame mod period");
  Check(EvalGradient(ramp, 2, 10, true, 10, out) && Color(out, 0.f, 0.f, 0.f, 0.f), "repeat wraps to the first stop");
  Check(EvalGradient(ramp, 2, 10, true, -3, out) && Color(out, 2.8f, 1.4f, 0.7f, 0.7f), "negative frame wraps");

  // A period below 2 is 1: the position is 0 at frame 0, then 1.
  Check(EvalGradient(ramp, 2, 0, false, 0, out) && Color(out, 0.f, 0.f, 0.f, 0.f), "period 0 acts as 1, frame 0");
  Check(EvalGradient(ramp, 2, 1, false, 1, out) && Color(out, 4.f, 2.f, 1.f, 1.f), "period 1, frame 1");
  Check(EvalGradient(ramp, 2, -7, false, 3, out) && Color(out, 4.f, 2.f, 1.f, 1.f), "negative period acts as 1");

  // Stops that do not start at 0 or end at 1 hold their colour outside.
  const GradientStop inner[] = {{{1.f, 0.f, 0.f, 1.f}, 0.25f}, {{0.f, 1.f, 0.f, 1.f}, 0.75f}};
  Check(EvalGradient(inner, 2, 4, false, 0, out) && Color(out, 1.f, 0.f, 0.f, 1.f), "holds before the first stop");
  Check(EvalGradient(inner, 2, 4, false, 4, out) && Color(out, 0.f, 1.f, 0.f, 1.f), "holds after the last stop");
  Check(EvalGradient(inner, 2, 4, false, 2, out) && Color(out, 0.5f, 0.5f, 0.f, 1.f), "interpolates between inner stops");

  // No stops: Remastered returns 0.
  Check(!EvalGradient(inner, 0, 4, false, 1, out) && Color(out, 0.f, 0.f, 0.f, 0.f), "no stops yields zero");

  if (sFailures == 0) {
    std::puts("port_particle_math: OK");
  }
  return sFailures == 0 ? 0 : 1;
}
