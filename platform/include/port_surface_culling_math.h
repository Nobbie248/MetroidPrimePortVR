#pragma once

#include <array>
#include <cmath>

namespace PortSurfaceCulling {
using Vector = std::array<float, 3>;
struct Bounds { Vector min, max; };

inline bool ValidBounds(const Bounds& bounds) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(bounds.min[i]) || !std::isfinite(bounds.max[i]) || bounds.min[i] > bounds.max[i]) {
      return false;
    }
  }
  return true;
}

struct Plane {
  Vector normal{}; // Outward unit normal: visible points have dot(normal, point) <= distance.
  float distance = 0.f;
};

struct EyeVolume {
  std::array<Plane, 5> planes{};
  bool valid = false;

  // FOV angles are OpenXR's left, right, up, down. Axes are in world space,
  // with forward pointing into the scene. Expand the FOV before making planes;
  // moving each plane outward also covers eye/head translation between samples.
  bool Build(const Vector& origin, const Vector& right, const Vector& forward, const Vector& up,
             const std::array<float, 4>& fov, float angleMargin, float positionMargin) {
    valid = false;
    if (!std::isfinite(angleMargin) || angleMargin < 0.f ||
        !std::isfinite(positionMargin) || positionMargin < 0.f) { return false; }
    for (const auto& v : {origin, right, forward, up}) {
      for (float component : v) { if (!std::isfinite(component)) { return false; } }
    }
    const std::array<Vector, 3> axes{right, forward, up};
    for (int a = 0; a < 3; ++a) {
      for (int b = a; b < 3; ++b) {
        float dot = 0.f;
        for (int i = 0; i < 3; ++i) { dot += axes[a][i] * axes[b][i]; }
        if (std::abs(dot - (a == b ? 1.f : 0.f)) > 0.005f) { return false; }
      }
    }
    for (float angle : fov) { if (!std::isfinite(angle)) { return false; } }
    if (fov[0] >= fov[1] || fov[3] >= fov[2]) { return false; }
    const float left = fov[0] - angleMargin, rightAngle = fov[1] + angleMargin;
    const float top = fov[2] + angleMargin, bottom = fov[3] - angleMargin;
    constexpr float limit = 1.5707f;
    if (left >= rightAngle || bottom >= top || left <= -limit || rightAngle >= limit ||
        bottom <= -limit || top >= limit) { return false; }
    const auto makePlane = [&](int index, const Vector& axis, float a, float b) {
      auto& plane = planes[index];
      float length2 = 0.f;
      for (int i = 0; i < 3; ++i) {
        plane.normal[i] = axis[i] * a + forward[i] * b;
        length2 += plane.normal[i] * plane.normal[i];
      }
      if (!std::isfinite(length2) || length2 < 1.e-8f) { return false; }
      const float inverseLength = 1.f / std::sqrt(length2);
      plane.distance = positionMargin;
      for (int i = 0; i < 3; ++i) {
        plane.normal[i] *= inverseLength;
        plane.distance += plane.normal[i] * origin[i];
      }
      return std::isfinite(plane.distance);
    };
    valid = makePlane(0, right, -std::cos(left), std::sin(left)) &&
            makePlane(1, right, std::cos(rightAngle), -std::sin(rightAngle)) &&
            makePlane(2, up, std::cos(top), -std::sin(top)) &&
            makePlane(3, up, -std::cos(bottom), std::sin(bottom)) &&
            makePlane(4, forward, -1.f, 0.f);
    return valid;
  }

  bool Visible(const Bounds& bounds) const {
    if (!valid || !ValidBounds(bounds)) { return true; }
    for (const auto& plane : planes) {
      float nearest = 0.f;
      for (int i = 0; i < 3; ++i) {
        nearest += plane.normal[i] * (plane.normal[i] >= 0.f ? bounds.min[i] : bounds.max[i]);
      }
      // Keep touching boxes and a small allowance for floating-point error.
      if (nearest > plane.distance + 0.001f) { return false; }
    }
    return true;
  }
};

struct StereoVolume {
  std::array<EyeVolume, 2> eyes{};
  bool Visible(const Bounds& bounds) const {
    // An unknown eye must keep geometry; one valid eye cannot stand in for both.
    return eyes[0].Visible(bounds) || eyes[1].Visible(bounds);
  }
};
} // namespace PortSurfaceCulling
