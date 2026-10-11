#pragma once

#include "stereo_replay.hpp"
#include <array>

namespace aurora::gfx::stereo_replay {
// A detached HUD follows the complete tracked pose with a 180 ms time
// constant. Quaternion interpolation keeps turns through 180 degrees stable.
class FloatingHud {
  using Quat = std::array<float, 4>;
  bool valid_ = false;
  Quat rotation_{0.f, 0.f, 0.f, 1.f};
  std::array<float, 3> position_{};

  static Quat Rotation(const Mat3x4<float>& m) noexcept {
    Quat q{};
    const float trace = m.m0[0] + m.m1[1] + m.m2[2];
    if (trace > 0.f) {
      const float s = 2.f * std::sqrt(trace + 1.f);
      q = {(m.m2[1] - m.m1[2]) / s, (m.m0[2] - m.m2[0]) / s,
           (m.m1[0] - m.m0[1]) / s, 0.25f * s};
    } else {
      int i = m.m1[1] > m.m0[0] ? 1 : 0;
      if (m.m2[2] > (*(&m.m0 + i))[i]) i = 2;
      const int j = (i + 1) % 3;
      const int k = (i + 2) % 3;
      const float s = 2.f * std::sqrt(std::max(0.f, 1.f + (*(&m.m0 + i))[i] -
                                                  (*(&m.m0 + j))[j] - (*(&m.m0 + k))[k]));
      q[i] = 0.25f * s;
      q[j] = ((*(&m.m0 + i))[j] + (*(&m.m0 + j))[i]) / s;
      q[k] = ((*(&m.m0 + i))[k] + (*(&m.m0 + k))[i]) / s;
      q[3] = ((*(&m.m0 + k))[j] - (*(&m.m0 + j))[k]) / s;
    }
    return q;
  }

public:
  void Reset() noexcept { valid_ = false; }

  Mat3x4<float> Update(const Mat3x4<float>& target, float dt) noexcept {
    Quat next = Rotation(target);
    if (!valid_ || !(dt >= 0.f) || dt > 0.25f) {
      rotation_ = next;
      position_ = {target.m0[3], target.m1[3], target.m2[3]};
      valid_ = true;
    } else {
      const float alpha = 1.f - std::exp(-dt / 0.18f);
      float dot = 0.f;
      for (int i = 0; i < 4; ++i) dot += rotation_[i] * next[i];
      if (dot < 0.f) {
        for (float& value : next) value = -value;
        dot = -dot;
      }
      dot = std::clamp(dot, 0.f, 1.f);
      float keep = 1.f - alpha;
      float take = alpha;
      if (dot < 0.9995f) {
        const float angle = std::acos(dot);
        keep = std::sin((1.f - alpha) * angle) / std::sin(angle);
        take = std::sin(alpha * angle) / std::sin(angle);
      }
      float length = 0.f;
      for (int i = 0; i < 4; ++i) {
        rotation_[i] = keep * rotation_[i] + take * next[i];
        length += rotation_[i] * rotation_[i];
      }
      for (float& value : rotation_) value /= std::sqrt(length);
      for (int i = 0; i < 3; ++i) {
        position_[i] += alpha * ((*(&target.m0 + i))[3] - position_[i]);
      }
    }
    const auto& q = rotation_;
    Mat3x4<float> result{};
    result.m0 = {1.f - 2.f * (q[1]*q[1] + q[2]*q[2]), 2.f * (q[0]*q[1] - q[2]*q[3]),
                 2.f * (q[0]*q[2] + q[1]*q[3]), position_[0]};
    result.m1 = {2.f * (q[0]*q[1] + q[2]*q[3]), 1.f - 2.f * (q[0]*q[0] + q[2]*q[2]),
                 2.f * (q[1]*q[2] - q[0]*q[3]), position_[1]};
    result.m2 = {2.f * (q[0]*q[2] - q[1]*q[3]), 2.f * (q[1]*q[2] + q[0]*q[3]),
                 1.f - 2.f * (q[0]*q[0] + q[1]*q[1]), position_[2]};
    return result;
  }
};

inline Mat3x4<float> inverse_rigid(const Mat3x4<float>& view) noexcept {
  Mat3x4<float> pose{};
  for (int row = 0; row < 3; ++row) {
    auto& out = *(&pose.m0 + row);
    for (int column = 0; column < 3; ++column) out[column] = (*(&view.m0 + column))[row];
    out[3] = -(out[0] * view.m0[3] + out[1] * view.m1[3] + out[2] * view.m2[3]);
  }
  return pose;
}

// Average in pose space, so canted eye rotations do not bias the HUD toward
// one eye or rotate the head-centre position around the tracking origin.
inline Mat3x4<float> center_head_pose(const Mat3x4<float>& leftView,
                                     const Mat3x4<float>& rightView) noexcept {
  const auto left = inverse_rigid(leftView);
  const auto right = inverse_rigid(rightView);
  std::array<float, 3> forward{}, side{};
  for (int row = 0; row < 3; ++row) {
    forward[row] = 0.5f * ((*(&left.m0 + row))[2] + (*(&right.m0 + row))[2]);
    side[row] = 0.5f * ((*(&left.m0 + row))[0] + (*(&right.m0 + row))[0]);
  }
  const auto normalize = [](std::array<float, 3>& v) {
    const float length = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (length < 1.e-6f) return false;
    for (float& value : v) value /= length;
    return true;
  };
  if (!normalize(forward)) return left;
  const float dot = forward[0]*side[0] + forward[1]*side[1] + forward[2]*side[2];
  for (int row = 0; row < 3; ++row) side[row] -= forward[row] * dot;
  if (!normalize(side)) return left;
  const std::array<float, 3> up{forward[1]*side[2] - forward[2]*side[1],
                               forward[2]*side[0] - forward[0]*side[2],
                               forward[0]*side[1] - forward[1]*side[0]};
  Mat3x4<float> pose{};
  for (int row = 0; row < 3; ++row) {
    (*(&pose.m0 + row)) = {side[row], up[row], forward[row],
                          0.5f * ((*(&left.m0 + row))[3] + (*(&right.m0 + row))[3])};
  }
  return pose;
}
} // namespace aurora::gfx::stereo_replay
