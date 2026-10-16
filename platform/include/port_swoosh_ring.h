#pragma once

#include <cmath>

// Remastered's SIDE==2 ORNT swoosh ribbon (CSwooshDescription::Compile 0x29b1a0 ->
// SetupOrientToView 0x29bdbc -> CERotationFromAxis 0x29d298, CVESwooshDirection 0x2a2d88).
// The authored PROT is replaced by a rotation whose local X is normalize(A x Z), with
// A = camera - point (system space) and Z = the trail direction. Ring vertex 0 (angle 0) is
// pos + RRAD * X and vertex 1 (angle 180) is pos - LRAD * X (writer 0x2b52e0: radius is RRAD
// for a <= 0, LRAD for 0 < a <= 180).
namespace PortSwooshRing {
struct Vec {
  float x, y, z;
};

inline Vec Sub(const Vec& a, const Vec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec Cross(const Vec& a, const Vec& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec Scaled(const Vec& v, float m2) {
  const float inv = 1.f / std::sqrt(m2);
  return {v.x * inv, v.y * inv, v.z * inv};
}

// CVector3f::IsNormalizable 0x6b030: every component finite and |v|^2 >= 1e-28 (0x10fd87b5).
inline bool NormalizeDirection(Vec& v) {
  if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
    return false;
  }
  const float m2 = v.x * v.x + v.y * v.y + v.z * v.z;
  if (!(m2 >= 1e-28f)) {
    return false;
  }
  v = Scaled(v, m2);
  return true;
}

// CERotationFromAxis / CParticleMath::CrossIntoMatrixRow: falls back only when
// |v|^2 < CParticleValues::skMinNormalizableMagSqd (2^-46, 0x28800000).
inline bool NormalizeAxis(Vec& v) {
  const float m2 = v.x * v.x + v.y * v.y + v.z * v.z;
  if (m2 < 0x1p-46f) {
    return false;
  }
  v = Scaled(v, m2);
  return true;
}

// Trail direction at a point: normalize(older - newer), one-sided at the ends. Returns false
// when the point has no neighbour (Remastered's evaluator fails; such a point is never part
// of a drawn segment). A direction that cannot be normalised becomes skForwardVector (0, 1, 0).
inline bool Direction(const Vec& pos, const Vec* older, const Vec* newer, Vec& out) {
  if (older == nullptr && newer == nullptr) {
    return false;
  }
  if (older != nullptr && newer != nullptr) {
    out = Sub(*older, *newer);
  } else if (older != nullptr) {
    out = Sub(*older, pos);
  } else {
    out = Sub(pos, *newer);
  }
  if (!NormalizeDirection(out)) {
    out = {0.f, 1.f, 0.f};
  }
  return true;
}

// World axis of local X after the rotation. A degenerate axis or cross product gives the
// fallback rotation sNoRotation (local X = +X); a missing direction is treated the same.
inline Vec WidthAxis(const Vec& pos, const Vec& camera, const Vec* older, const Vec* newer) {
  Vec z;
  if (!Direction(pos, older, newer, z) || !NormalizeAxis(z)) {
    return {1.f, 0.f, 0.f};
  }
  Vec x = Cross(Sub(camera, pos), z);
  if (!NormalizeAxis(x)) {
    return {1.f, 0.f, 0.f};
  }
  return x;
}

inline void Ring(const Vec& pos, const Vec& axis, float leftRad, float rightRad, Vec& v0, Vec& v1) {
  v0 = {pos.x + rightRad * axis.x, pos.y + rightRad * axis.y, pos.z + rightRad * axis.z};
  v1 = {pos.x - leftRad * axis.x, pos.y - leftRad * axis.y, pos.z - leftRad * axis.z};
}
} // namespace PortSwooshRing
