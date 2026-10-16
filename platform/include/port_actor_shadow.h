#pragma once

// Remastered's actor shadow (CSimpleShadow decal mode 1): the pure math. Each shadowed actor draws
// depth only into its own orthographic map looking world-down from its bounds, and the floor
// geometry inside the receiver box is redrawn as a multiply decal (docs/MODS.md "Actor shadows").
// The aurora half is extern/aurora/lib/gfx/actor_shadow.hpp; the game half port_actor_shadow.cpp.

#include <algorithm>
#include <cmath>

class CActor;
class CStateManager;
class CTransform4f;

namespace PortActorShadow {

constexpr float kExpansion = 1.5f;           // receiver radius scale
constexpr float kNormalThreshold = -0.70710678f; // a receiver triangle facing down further than this is dropped
constexpr float kFadeDistance = 4.f;         // fade range when the fade start is unset
constexpr float kBias = 0.015f;              // metres, decal toward the camera
constexpr float kZMargin = 0.25f;            // receiver box beyond the anchor hits
constexpr float kMinHalfHeight = 0.5f;
constexpr float kBoundsGate = 0.05f;         // bounds that moved less keep their shadow
constexpr float kMaxObjectHeight = 20.f;
constexpr float kDefaultFadeStart = 0.5f;
constexpr float kAiFadeStart = 0.625f;
constexpr float kAiAlpha = 0.7f;
constexpr unsigned kAnchors = 5; // 0 = centre of the top face, 1..4 = its corners
constexpr unsigned kMaxMaps = 16;
constexpr unsigned kParamFloats = 64; // GXPortActorShadowDecal's 16 vec4

// The anchor a ray is cast from on an update: 00 01 00 02 00 03 00 04.
inline unsigned AnchorForUpdate(unsigned update) {
  return (update & 1u) == 0u ? 0u : 1u + ((update >> 1) & 3u);
}

// Darkness from the height of the actor's top above the first floor hit below it.
inline float Opacity(float originZ, float firstHitZ, float maxHeight, float userAlpha) {
  return std::clamp(1.f - (originZ - firstHitZ) / maxHeight, 0.f, 1.f) * userAlpha;
}

// The receiver box's half extent in x and y from the caster's bounds extent.
inline float ReceiverRadius(float extentX, float extentY, float scale) {
  return 0.5f * std::sqrt(extentX * extentX + extentY * extentY) * scale * kExpansion;
}

inline bool BoundsMoved(const float a[6], const float b[6]) {
  for (int i = 0; i < 6; ++i) {
    if (std::fabs(a[i] - b[i]) > kBoundsGate) {
      return true;
    }
  }
  return false;
}

// An axis aligned box; its Z range spans the anchor hits plus the margin.
struct Box {
  float centre[3];
  float half[3];
};

inline Box ReceiverBox(float centreX, float centreY, float hitMinZ, float hitMaxZ, float radius) {
  const float lo = hitMinZ - kZMargin;
  const float hi = hitMaxZ + kZMargin;
  return {{centreX, centreY, 0.5f * (lo + hi)}, {radius, radius, std::max(0.5f * (hi - lo), kMinHalfHeight)}};
}

// Where the shadow starts to fade (h0, world z) and 1 / its length. The fade start is a fraction of
// the box's height: full shadow up to it, none at the box's top.
inline void Fade(const Box& box, float fadeStart, float& h0, float& invRange) {
  const float lo = box.centre[2] - box.half[2];
  const float hi = box.centre[2] + box.half[2];
  if (std::fabs(fadeStart) < 1e-5f) {
    h0 = lo;
    invRange = 1.f / kFadeDistance;
    return;
  }
  h0 = lo + fadeStart * (hi - lo);
  invRange = 1.f / std::max(hi - h0, 0.001f);
}

// A plane's normal and offset: inside (or on the positive side) when dot(n, p) + d >= 0.
struct Plane {
  float n[3];
  float d;
};

// The box's six planes, inside pointing in.
inline void BoxPlanes(const Box& box, Plane out[6]) {
  for (int a = 0; a < 3; ++a) {
    Plane lo{{0.f, 0.f, 0.f}, 0.f};
    Plane hi{{0.f, 0.f, 0.f}, 0.f};
    lo.n[a] = 1.f;
    lo.d = -(box.centre[a] - box.half[a]);
    hi.n[a] = -1.f;
    hi.d = box.centre[a] + box.half[a];
    out[a * 2] = lo;
    out[a * 2 + 1] = hi;
  }
}

// A world space plane (or affine function) in GX view space, worldToView being a 3x4 [R | t].
inline Plane ToView(const float worldToView[3][4], const Plane& p) {
  Plane out{};
  for (int r = 0; r < 3; ++r) {
    out.n[r] = worldToView[r][0] * p.n[0] + worldToView[r][1] * p.n[1] + worldToView[r][2] * p.n[2];
  }
  out.d = p.d - (out.n[0] * worldToView[0][3] + out.n[1] * worldToView[1][3] + out.n[2] * worldToView[2][3]);
  return out;
}

// The map's clip space (x, y in -1..1, z in 0..1 from the box's top down) as world space planes.
inline void ClipPlanes(const Box& box, Plane out[3]) {
  out[0] = {{1.f / box.half[0], 0.f, 0.f}, -box.centre[0] / box.half[0]};
  out[1] = {{0.f, 1.f / box.half[1], 0.f}, -box.centre[1] / box.half[1]};
  const float top = box.centre[2] + box.half[2];
  out[2] = {{0.f, 0.f, -0.5f / box.half[2]}, top * 0.5f / box.half[2]};
}

// The caster map's frame: x and y span the caster's bounds, z its height. Written as the
// GXPortActorShadowCasters matrix, four vec4 (clip x, y, z and w).
inline void CasterClip(const float worldToView[3][4], const Box& map, float out[16]) {
  Plane clip[3];
  ClipPlanes(map, clip);
  for (int i = 0; i < 3; ++i) {
    const Plane v = ToView(worldToView, clip[i]);
    out[i * 4 + 0] = v.n[0];
    out[i * 4 + 1] = v.n[1];
    out[i * 4 + 2] = v.n[2];
    out[i * 4 + 3] = v.d;
  }
  out[12] = out[13] = out[14] = 0.f;
  out[15] = 1.f;
}

// GXPortActorShadowDecal's parameters (see the decal shader in aurora's shader.cpp): 0-2 the map's
// clip rows, 3 the world height, 4-9 the receiver box planes, 10 (opacity, fade start z, 1 / fade
// range, map layer), 11 (world up in view space, normal threshold).
inline void DecalParams(const float worldToView[3][4], const Box& map, const Box& receiver, float opacity,
                        float fadeStart, unsigned layer, float out[kParamFloats]) {
  std::fill(out, out + kParamFloats, 0.f);
  CasterClip(worldToView, map, out);
  const Plane height = ToView(worldToView, {{0.f, 0.f, 1.f}, 0.f});
  out[12] = height.n[0];
  out[13] = height.n[1];
  out[14] = height.n[2];
  out[15] = height.d;
  Plane planes[6];
  BoxPlanes(receiver, planes);
  for (int i = 0; i < 6; ++i) {
    const Plane v = ToView(worldToView, planes[i]);
    float* o = out + (4 + i) * 4;
    o[0] = v.n[0];
    o[1] = v.n[1];
    o[2] = v.n[2];
    o[3] = v.d;
  }
  float h0, invRange;
  Fade(receiver, fadeStart, h0, invRange);
  out[40] = opacity;
  out[41] = h0;
  out[42] = invRange;
  out[43] = static_cast<float>(layer);
  // up (world z) as a direction: the planes' normal part without the offset
  out[44] = worldToView[0][2];
  out[45] = worldToView[1][2];
  out[46] = worldToView[2][2];
  out[47] = kNormalThreshold;
}

// Game side (platform/port_actor_shadow.cpp).
// True when actors use the projected shadow in place of the retail blob quad: the setting is on and a
// mod's room geometry is drawn.
bool Active();
void BeginFrame();
void Register(const CActor* actor);
// Renders each actor registered since the last call into its depth map and decals the room geometry under it.
void RenderFrame(const CStateManager& mgr, const CTransform4f& view);

} // namespace PortActorShadow
