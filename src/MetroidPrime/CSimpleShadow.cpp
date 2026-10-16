#include "MetroidPrime/CSimpleShadow.hpp"

#include "MetroidPrime/CGameCollision.hpp"
#include "MetroidPrime/CStateManager.hpp"

#include "Collision/CMaterialFilter.hpp"
#include "Collision/CRayCastResult.hpp"

#include "MetaRender/CCubeRenderer.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CAABox.hpp"
#ifdef TARGET_PC
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "port_actor_shadow.h"
#include "port_debug.h"
#include <algorithm>
#endif

CSimpleShadow::CSimpleShadow(float scale, float userAlpha, float maxObjHeight, float displacement)
: x0_xf(CTransform4f::Identity())
, x30_scale(scale)
, x34_radius(1.f)
, x38_userAlpha(userAlpha)
, x3c_heightAlpha(1.f)
, x40_maxObjHeight(maxObjHeight)
, x44_displacement(displacement)
, x48_24_collision(false)
, x48_25_alwaysCalculateRadius(true)
, x48_26_radiusCalculated(false) {}

void CSimpleShadow::Calculate(const CAABox& aabb, const CTransform4f& xf,
                              const CStateManager& mgr) {
  x48_24_collision = false;
  const CVector3f extent = aabb.GetMaxPoint() - aabb.GetMinPoint();
  const float halfHeight = extent.GetZ() / 2.f;
  const float xExtent = extent.GetX();
  const float yExtent = extent.GetY();

  CVector3f pos = xf.GetTranslation() + CVector3f(0.f, 0.f, halfHeight);
  CVector3f dir(0.0f, 0.0f, -1.0f);
  CRayCastResult res =
      mgr.RayStaticIntersection(pos, dir, x40_maxObjHeight,
                                CMaterialFilter::MakeExclude(CMaterialList(kMT_SeeThrough)));
  float height = x40_maxObjHeight;
  if (res.IsValid()) {
    x48_24_collision = true;
    height = res.GetTime();
  }

  CRayCastResult closestResult = res;

  if (height > 0.1f + halfHeight) {
    TEntityList nearList;
    mgr.BuildNearList(nearList, pos, dir, x40_maxObjHeight,
                      CMaterialFilter::MakeInclude(CMaterialList(kMT_Platform)), nullptr);

    TUniqueId cid = kInvalidUniqueId;
    CRayCastResult resD =
        CGameCollision::RayDynamicIntersection(mgr, cid, pos, dir, x40_maxObjHeight,
                                               CMaterialFilter::GetPassEverything(), nearList);
    if (resD.IsValid() && resD.GetTime() < height) {
      closestResult = resD;
      x48_24_collision = true;
      height = resD.GetTime();
    }
  }

  if (x48_24_collision) {
    x3c_heightAlpha = 1.f - height / x40_maxObjHeight;
    CVector3f normalVector = closestResult.GetPlane().GetNormal();
    x0_xf = CTransform4f::LookAt(normalVector, CVector3f::Zero());
    x0_xf.SetTranslation(closestResult.GetPoint() + x44_displacement * normalVector);
    if (x48_25_alwaysCalculateRadius || !x48_26_radiusCalculated) {
      x34_radius = sqrtf(xExtent * xExtent + yExtent * yExtent) / 2.f;
      x48_26_radiusCalculated = true;
    }
  }
}

#ifdef TARGET_PC
void CSimpleShadow::PortCalculate(const CAABox& bounds, const CStateManager& mgr) {
  const CVector3f lo = bounds.GetMinPoint();
  const CVector3f hi = bounds.GetMaxPoint();
  const float now[6] = {lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ()};
  if (xPortValid && !PortActorShadow::BoundsMoved(now, xPortBounds)) {
    return;
  }
  std::copy(now, now + 6, xPortBounds);
  const float top = hi.GetZ();
  const float cx = (lo.GetX() + hi.GetX()) * 0.5f;
  const float cy = (lo.GetY() + hi.GetY()) * 0.5f;
  float hitMin = 0.f, hitMax = 0.f;
  bool any = false;
  if (xPortExplicit) {
    // UpdateShadowAnchorForExplicitHeight (0xfac1c4): one cast straight down from the bounds'
    // centre; the box is 2 above to 2 below the hit. A miss clears the shadow.
    const float cz = (lo.GetZ() + hi.GetZ()) * 0.5f;
    const CRayCastResult res = mgr.RayStaticIntersection(
        CVector3f(cx, cy, cz), CVector3f(0.f, 0.f, -1.f), x40_maxObjHeight,
        CMaterialFilter::MakeExclude(CMaterialList(kMT_SeeThrough)));
    xPortDynamicBits = 0;
    xPortOpacity = x38_userAlpha;
    xPortValid = res.IsValid();
    if (!xPortValid) {
      return;
    }
    const float hitZ = cz - res.GetTime();
    hitMin = hitZ - 2.f + PortActorShadow::kZMargin;
    hitMax = hitZ + 2.f - PortActorShadow::kZMargin;
    any = true;
  } else {
    // UpdateShadowSampled (0xfab920): one anchor ray per update, from the first calculation on.
    const float anchors[5][2] = {
        {cx, cy},
        {lo.GetX(), lo.GetY()},
        {hi.GetX(), lo.GetY()},
        {hi.GetX(), hi.GetY()},
        {lo.GetX(), hi.GetY()},
    };
    const unsigned a = PortActorShadow::AnchorForUpdate(xPortUpdate++);
    const CVector3f origin(anchors[a][0], anchors[a][1], top);
    const CVector3f down(0.f, 0.f, -1.f);
    const CMaterialFilter filter = CMaterialFilter::MakeExclude(CMaterialList(kMT_SeeThrough));
    const CRayCastResult res = mgr.RayStaticIntersection(origin, down, 20.f, filter);
    xPortHasHit[a] = res.IsValid();
    if (res.IsValid()) {
      xPortHit[a] = top - res.GetTime();
    }
    // A platform under the anchor, nearer than the floor, marks the anchor (node flag +0x10a).
    bool platform = false;
    {
      TEntityList nearList;
      mgr.BuildNearList(nearList, origin, down, 20.f,
                        CMaterialFilter::MakeInclude(CMaterialList(kMT_Platform)), nullptr);
      TUniqueId cid = kInvalidUniqueId;
      const CRayCastResult dyn = CGameCollision::RayDynamicIntersection(
          mgr, cid, origin, down, 20.f, CMaterialFilter::GetPassEverything(), nearList);
      platform = dyn.IsValid() && (!res.IsValid() || dyn.GetTime() < res.GetTime());
      if (platform) {
        xPortHasHit[a] = true;
        xPortHit[a] = top - dyn.GetTime();
      }
    }
    xPortDynamicBits = platform ? (xPortDynamicBits | (1u << a)) : (xPortDynamicBits & ~(1u << a));
    xPortOpacity = 0.f;
    for (unsigned i = 0; i < PortActorShadow::kAnchors; ++i) {
      if (!xPortHasHit[i]) {
        continue;
      }
      if (!any) {
        xPortOpacity = PortActorShadow::Opacity(top, xPortHit[i], 20.f, x38_userAlpha);
        hitMin = hitMax = xPortHit[i];
        any = true;
      } else {
        hitMin = std::min(hitMin, xPortHit[i]);
        hitMax = std::max(hitMax, xPortHit[i]);
      }
    }
    xPortValid = any;
  }
  if (!any) {
    return;
  }
  const float dx = hi.GetX() - lo.GetX();
  const float dy = hi.GetY() - lo.GetY();
  const float dz = hi.GetZ() - lo.GetZ();
  const float map[6] = {cx, cy, (lo.GetZ() + hi.GetZ()) * 0.5f, std::max(dx * 0.5f, 0.01f),
                        std::max(dy * 0.5f, 0.01f), std::max(dz * 0.5f, 0.01f)};
  std::copy(map, map + 6, xPortMap);
  const PortActorShadow::Box box = PortActorShadow::ReceiverBox(
      cx, cy, hitMin, hitMax, PortActorShadow::ReceiverRadius(dx, dy, x30_scale));
  std::copy(box.centre, box.centre + 3, xPortReceiver);
  std::copy(box.half, box.half + 3, xPortReceiver + 3);
}
#endif

void CSimpleShadow::Render(const CTexture* tex) const {
  if (!x48_24_collision)
    return;

  CGraphics::DisableAllLights();
#ifdef TARGET_PC
  // Calculate places the blob once per tick from the sim transform; between
  // ticks draw it at the blend, as its actor is (docs/FRAME_INTERPOLATION.md).
  CTransform4f xf = x0_xf;
  const float t = CCameraManager::GetPresentationInterpolation();
  if (xPortXf.Note(x0_xf, CActor::PortTickGeneration()) && t >= 0.f && t < 1.f &&
      PortDebug::ActorInterpolation()) {
    CTransform4f blend = CTransform4f::Identity();
    CTransform4f cur = CTransform4f::Identity();
    if (CActor::PortBlendRigid(xPortXf.prev, x0_xf, t, blend, cur))
      xf = blend * cur.GetQuickInverse() * x0_xf;
  }
  gpRender->SetModelMatrix(xf);
#else
  gpRender->SetModelMatrix(x0_xf);
#endif
  tex->Load(GX_TEXMAP0, CTexture::kCM_Repeat);
  CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvModulate);
  CGraphics::SetTevOp(kTS_Stage1, CGraphics::kEnvPassthru);
  CGraphics::SetAlphaCompare(kAF_Always, 0, kAO_And, kAF_Always, 0);
  CGraphics::SetDepthWriteMode(true, kE_LEqual, false);
  CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
  float radius = x34_radius * x30_scale;
  CGraphics::StreamBegin(kP_Quads);
  CGraphics::StreamColor(CCast::ToUint8((x3c_heightAlpha * x38_userAlpha) * 255.f) - 0x100);
  CGraphics::StreamTexcoord(0.f, 0.f);
  CGraphics::StreamVertex(CVector3f(-radius, 0.f, -radius));
  CGraphics::StreamTexcoord(0.f, 1.f);
  CGraphics::StreamVertex(CVector3f(radius, 0.f, -radius));
  CGraphics::StreamTexcoord(1.f, 1.f);
  CGraphics::StreamVertex(CVector3f(radius, 0.f, radius));
  CGraphics::StreamTexcoord(1.f, 0.f);
  CGraphics::StreamVertex(CVector3f(-radius, 0.f, radius));
  CGraphics::StreamEnd();
}

const CTransform4f& CSimpleShadow::GetTransform() const { return x0_xf; }

void CSimpleShadow::SetUserAlpha(float alpha) { x38_userAlpha = alpha; }

float CSimpleShadow::GetMaxObjectHeight() const { return x40_maxObjHeight; }

void CSimpleShadow::SetAlwaysCalculateRadius(bool b) { x48_25_alwaysCalculateRadius = b; }

CAABox CSimpleShadow::GetBounds() const {
  const CVector3f& translation = x0_xf.GetTranslation();
  float extent = x34_radius * x30_scale;
  return CAABox(translation - CVector3f(extent, extent, extent),
                translation + CVector3f(extent, extent, extent));
}

CAABox CSimpleShadow::GetMaxShadowBox(const CAABox& aabb) const {
  float extent = x34_radius * x30_scale;
  CVector3f center = aabb.GetCenterPoint();
  CAABox expandedAABB = aabb;
  expandedAABB.AccumulateBounds(center + CVector3f(extent, extent, -GetMaxObjectHeight()));
  expandedAABB.AccumulateBounds(center + CVector3f(-extent, -extent, -GetMaxObjectHeight()));
  return expandedAABB;
}

bool CSimpleShadow::Valid() const { return x48_24_collision; }
