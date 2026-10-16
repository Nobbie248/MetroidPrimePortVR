#include "MetroidPrime/CGameCollision.hpp"
#include "Collision/CCollidableAABoxSphere.hpp"
#include "Collision/CCollisionInfoList.hpp"
#include "Collision/CInternalRayCastStructure.hpp"
#include "Collision/CMaterialList.hpp"
#include "Collision/CollisionUtil.hpp"
#include "Kyoto/Math/CLine.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Math/CloseEnough.hpp"
#include "MetroidPrime/CAABoxFilter.hpp"
#include "MetroidPrime/CBallFilter.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CGroundMovement.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/UserNames.hpp"
#include "WorldFormat/CAreaOctTree.hpp"
#include "WorldFormat/CMetroidAreaCollider.hpp"

#include "rstl/math.hpp"

#include <Collision/CCollidableSphere.hpp>
#include <Collision/CCollisionPrimitive.hpp>
#include <Collision/CRayCastResult.hpp>
#include <WorldFormat/CCollidableOBBTreeGroup.hpp>

#ifdef TARGET_PC
#include "port_debug.h"
#endif

static int gDebugPrintCount;

void CGameCollision::InitCollision() {
  /* Types */
  CCollisionPrimitive::InitBeginTypes();
  CCollisionPrimitive::InitAddType(CCollidableOBBTreeGroup::GetType());
  CCollisionPrimitive::InitEndTypes();

  /* Colliders */
  CCollisionPrimitive::InitBeginColliders();
  CCollisionPrimitive::InitAddCollider(CCollidableOBBTreeGroup::SphereCollide, "CCollidableSphere",
                                       "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddCollider(CCollidableOBBTreeGroup::AABoxCollide, "CCollidableAABox",
                                       "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddBooleanCollider(CCollidableOBBTreeGroup::SphereCollideBoolean,
                                              "CCollidableSphere", "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddBooleanCollider(CCollidableOBBTreeGroup::AABoxCollideBoolean,
                                              "CCollidableAABox", "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddMovingCollider(CCollidableOBBTreeGroup::CollideMovingAABox,
                                             "CCollidableAABox", "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddMovingCollider(CCollidableOBBTreeGroup::CollideMovingSphere,
                                             "CCollidableSphere", "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddCollider(CGameCollision::NullCollisionCollider,
                                       "CCollidableOBBTreeGroup", "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddBooleanCollider(CGameCollision::NullBooleanCollider,
                                              "CCollidableOBBTreeGroup", "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitAddMovingCollider(CGameCollision::NullMovingCollider,
                                             "CCollidableOBBTreeGroup", "CCollidableOBBTreeGroup");
  CCollisionPrimitive::InitEndColliders();
}

bool CGameCollision::NullCollisionCollider(const CInternalCollisionStructure&,
                                           CCollisionInfoList&) {
  return false;
}

bool CGameCollision::NullBooleanCollider(const CInternalCollisionStructure&) { return false; }

bool CGameCollision::NullMovingCollider(const CInternalCollisionStructure&, const CVector3f&,
                                        double&, CCollisionInfo&) {
  return false;
}

void CGameCollision::BuildAreaCollisionCache(const CStateManager& mgr, CAreaCollisionCache& cache) {
  cache.ClearCache();

  for (CGameArea::CConstChainIterator it = mgr.GetWorld()->GetChainHead(CWorld::kC_Alive);
       it != CWorld::skGlobalEnd; ++it) {
    CMetroidAreaCollider::COctreeLeafCache leafCache(*(*it).GetPostConstructed()->x0_collision);
    const CAreaOctTree& collision = *(*it).GetPostConstructed()->x0_collision;
    CAreaOctTree::Node node(collision.GetTreeMemory(), collision.GetBoundingBox(), collision,
                            collision.GetTreeType());
    CMetroidAreaCollider::BuildOctreeLeafCache(node, cache.GetCacheBounds(), leafCache);
    cache.AddOctreeLeafCache(leafCache);
  }

  IsUser(0);
}

bool CGameCollision::DetectCollisionBoolean(const CStateManager& mgr,
                                            const CCollisionPrimitive& prim, const CTransform4f& xf,
                                            const CMaterialFilter& filter,
                                            const TEntityList& nearList) {
  if (!filter.GetExcludeList().HasMaterial(kMT_NoStaticCollision) &&
      DetectStaticCollisionBoolean(mgr, prim, xf, filter)) {
    return true;
  }

  if (DetectDynamicCollisionBoolean(prim, xf, nearList, mgr)) {
    return true;
  }

  return false;
}

bool CGameCollision::DetectCollisionBoolean_Cached(
    const CStateManager& mgr, CAreaCollisionCache& cache, const CCollisionPrimitive& prim,
    const CTransform4f& xf, const CMaterialFilter& filter, const TEntityList& nearList) {
  if (!filter.GetExcludeList().HasMaterial(kMT_NoStaticCollision) &&
      DetectStaticCollisionBoolean_Cached(mgr, cache, prim, xf, filter)) {
    return true;
  }

  if (DetectDynamicCollisionBoolean(prim, xf, nearList, mgr)) {
    return true;
  }

  return false;
}

bool CGameCollision::DetectCollision(const CStateManager& mgr, const CCollisionPrimitive& prim,
                                     const CTransform4f& xf, const CMaterialFilter& filter,
                                     const TEntityList& nearList, TUniqueId& idOut,
                                     CCollisionInfoList& infoList) {
  bool ret = false;
  idOut = kInvalidUniqueId;
  if (!filter.GetExcludeList().HasMaterial(kMT_NoStaticCollision) &&
      DetectStaticCollision(mgr, prim, xf, filter, infoList)) {
    ret = true;
  }

  TUniqueId id = kInvalidUniqueId;
  if (DetectDynamicCollision(prim, xf, nearList, id, infoList, mgr)) {
    ret = true;
    idOut = id;
  }

  return ret;
}

bool CGameCollision::DetectCollision_Cached(const CStateManager& mgr, CAreaCollisionCache& cache,
                                            const CCollisionPrimitive& prim, const CTransform4f& xf,
                                            const CMaterialFilter& filter,
                                            const TEntityList& nearList, TUniqueId& idOut,
                                            CCollisionInfoList& infoList) {
  bool ret = false;
  idOut = kInvalidUniqueId;
  if (!filter.GetExcludeList().HasMaterial(kMT_NoStaticCollision) &&
      DetectStaticCollision_Cached(mgr, cache, prim, xf, filter, infoList)) {
    ret = true;
  }

  TUniqueId id = kInvalidUniqueId;
  if (DetectDynamicCollision(prim, xf, nearList, id, infoList, mgr)) {
    ret = true;
    idOut = id;
  }

  return ret;
}

bool CGameCollision::DetectStaticCollisionBoolean(const CStateManager& mgr,
                                                  const CCollisionPrimitive& prim,
                                                  const CTransform4f& xf,
                                                  const CMaterialFilter& filter) {
  if (prim.GetPrimType() == 'OBTG') {
    return false;
  }

  const CWorld* world = mgr.GetWorld();
  if (prim.GetPrimType() == 'AABX') {
    CAABox aabb = prim.CalculateAABox(xf);
    CAABox aabb2 = aabb;
    for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
         it != CWorld::skGlobalEnd; ++it) {
      if (CMetroidAreaCollider::AABoxCollisionCheckBoolean((*it).GetOctTree(), aabb2, filter)) {
        return true;
      }
    }
  } else if (prim.GetPrimType() == 'SPHR') {
    CAABox aabb = prim.CalculateAABox(xf);
    CAABox aabb2 = aabb;
    CSphere sphere = static_cast< const CCollidableSphere& >(prim).Transform(xf);
    CSphere sphere2 = sphere;
    for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
         it != CWorld::skGlobalEnd; ++it) {
      if (CMetroidAreaCollider::SphereCollisionCheckBoolean((*it).GetOctTree(), aabb2, sphere2,
                                                            filter)) {
        return true;
      }
    }
  } else if (prim.GetPrimType() == 'ABSH') {
    const CCollidableAABoxSphere& absPrim = static_cast< const CCollidableAABoxSphere& >(prim);
    if (DetectStaticCollisionBoolean(mgr, absPrim.GetCollidableAABox(), xf, filter)) {
      return true;
    }
    if (DetectStaticCollisionBoolean(mgr, absPrim.GetCollidableSphere(), xf, filter)) {
      return true;
    }
  }

  return false;
}

bool CGameCollision::DetectStaticCollisionBoolean_Cached(const CStateManager& mgr,
                                                         CAreaCollisionCache& cache,
                                                         const CCollisionPrimitive& prim,
                                                         const CTransform4f& xf,
                                                         const CMaterialFilter& filter) {
  if (prim.GetPrimType() == 'OBTG') {
    return false;
  }

  bool ret = false;
  CAABox aabb = prim.CalculateAABox(xf);
  CAABox aabb2 = aabb;
  if (!aabb2.Inside(cache.GetCacheBounds())) {
    CAABox newAABB(aabb2.GetMinPoint() - CVector3f(0.2f, 0.2f, 0.2f),
                   aabb2.GetMaxPoint() + CVector3f(0.2f, 0.2f, 0.2f));
    newAABB.AccumulateBounds(cache.GetCacheBounds().GetMinPoint());
    newAABB.AccumulateBounds(cache.GetCacheBounds().GetMaxPoint());
    cache.SetCacheBounds(newAABB);
    BuildAreaCollisionCache(mgr, cache);
    IsUser(0);
  }

  if (cache.HasCacheOverflowed()) {
    return DetectStaticCollisionBoolean(mgr, prim, xf, filter);
  }

  if (prim.GetPrimType() == 'AABX') {
    for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
      if (CMetroidAreaCollider::AABoxCollisionCheckBoolean_Cached(cache.GetOctreeLeafCache(i),
                                                                  aabb2, filter)) {
        ret = true;
        break;
      }
    }
  } else if (prim.GetPrimType() == 'SPHR') {
    CSphere sphere = static_cast< const CCollidableSphere& >(prim).Transform(xf);
    CSphere sphere2 = sphere;
    for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
      if (CMetroidAreaCollider::SphereCollisionCheckBoolean_Cached(cache.GetOctreeLeafCache(i),
                                                                   aabb2, sphere2, filter)) {
        ret = true;
        break;
      }
    }
  } else if (prim.GetPrimType() == 'ABSH') {
    const CCollidableAABoxSphere& absPrim = static_cast< const CCollidableAABoxSphere& >(prim);
    if (DetectStaticCollisionBoolean_Cached(mgr, cache, absPrim.GetCollidableAABox(), xf, filter)) {
      ret = true;
    } else if (DetectStaticCollisionBoolean_Cached(mgr, cache, absPrim.GetCollidableSphere(), xf,
                                                   filter)) {
      ret = true;
    }
  }

  return ret;
}

bool CGameCollision::DetectStaticCollision(const CStateManager& mgr,
                                           const CCollisionPrimitive& prim, const CTransform4f& xf,
                                           const CMaterialFilter& filter,
                                           CCollisionInfoList& infoList) {
  if (prim.GetPrimType() == 'OBTG') {
    return false;
  }

  bool ret = false;
  const CWorld* world = mgr.GetWorld();
  if (prim.GetPrimType() == 'AABX') {
    const CCollidableAABox& boxPrim = static_cast< const CCollidableAABox& >(prim);
    CAABox aabb = boxPrim.CalculateAABox(xf);
    CAABox aabb2 = aabb;
    for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
         it != CWorld::skGlobalEnd; ++it) {
      if (CMetroidAreaCollider::AABoxCollisionCheck((*it).GetOctTree(), aabb2, filter,
                                                    boxPrim.GetMaterial(), infoList)) {
        ret = true;
      }
    }
  } else if (prim.GetPrimType() == 'SPHR') {
    const CCollidableSphere& spherePrim = static_cast< const CCollidableSphere& >(prim);
    CAABox aabb = spherePrim.CalculateAABox(xf);
    CAABox aabb2 = aabb;
    CSphere sphere = spherePrim.Transform(xf);
    CSphere sphere2 = sphere;
    for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
         it != CWorld::skGlobalEnd; ++it) {
      if (CMetroidAreaCollider::SphereCollisionCheck((*it).GetOctTree(), aabb2, sphere2,
                                                     spherePrim.GetMaterial(), filter, infoList)) {
        ret = true;
      }
    }
  } else if (prim.GetPrimType() == 'ABSH') {
    const CCollidableAABoxSphere& absPrim = static_cast< const CCollidableAABoxSphere& >(prim);
    if (DetectStaticCollision(mgr, absPrim.GetCollidableAABox(), xf, filter, infoList)) {
      ret = true;
    }
    if (DetectStaticCollision(mgr, absPrim.GetCollidableSphere(), xf, filter, infoList)) {
      ret = true;
    }
  }

  return ret;
}

bool CGameCollision::DetectStaticCollision_Cached(
    const CStateManager& mgr, CAreaCollisionCache& cache, const CCollisionPrimitive& prim,
    const CTransform4f& xf, const CMaterialFilter& filter, CCollisionInfoList& infoList) {
  if (prim.GetPrimType() == 'OBTG') {
    return false;
  }

  bool ret = false;
  CAABox aabb = prim.CalculateAABox(xf);
  CAABox aabb2 = aabb;
  if (!aabb2.Inside(cache.GetCacheBounds())) {
    CAABox newAABB(aabb2.GetMinPoint() - CVector3f(0.2f, 0.2f, 0.2f),
                   aabb2.GetMaxPoint() + CVector3f(0.2f, 0.2f, 0.2f));
    newAABB.AccumulateBounds(cache.GetCacheBounds().GetMinPoint());
    newAABB.AccumulateBounds(cache.GetCacheBounds().GetMaxPoint());
    cache.SetCacheBounds(newAABB);
    BuildAreaCollisionCache(mgr, cache);
    IsUser(0);
  }

  if (cache.HasCacheOverflowed()) {
    return DetectStaticCollision(mgr, prim, xf, filter, infoList);
  }

  if (prim.GetPrimType() == 'AABX') {
    const CCollidableAABox& boxPrim = static_cast< const CCollidableAABox& >(prim);
    for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
      if (CMetroidAreaCollider::AABoxCollisionCheck_Cached(
              cache.GetOctreeLeafCache(i), aabb2, filter, boxPrim.GetMaterial(), infoList)) {
        ret = true;
      }
    }
  } else if (prim.GetPrimType() == 'SPHR') {
    const CCollidableSphere& spherePrim = static_cast< const CCollidableSphere& >(prim);
    CSphere sphere = spherePrim.Transform(xf);
    CSphere sphere2 = sphere;
    for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
      if (CMetroidAreaCollider::SphereCollisionCheck_Cached(cache.GetOctreeLeafCache(i), aabb2,
                                                            sphere2, spherePrim.GetMaterial(),
                                                            filter, infoList)) {
        ret = true;
      }
    }
  } else if (prim.GetPrimType() == 'ABSH') {
    const CCollidableAABoxSphere& absPrim = static_cast< const CCollidableAABoxSphere& >(prim);
    if (DetectStaticCollision_Cached(mgr, cache, absPrim.GetCollidableAABox(), xf, filter,
                                     infoList)) {
      ret = true;
    }
    if (DetectStaticCollision_Cached(mgr, cache, absPrim.GetCollidableSphere(), xf, filter,
                                     infoList)) {
      ret = true;
    }
  }

  return ret;
}

bool CGameCollision::DetectStaticCollision_Cached_Moving(
    const CStateManager& mgr, CAreaCollisionCache& cache, const CCollisionPrimitive& prim,
    const CTransform4f& xf, const CMaterialFilter& filter, CVector3f dir, CCollisionInfo& infoOut,
    double& dOut) {
  if (prim.GetPrimType() == 'OBTG') {
    return false;
  }

  CVector3f dirScale = dir * dOut;
  CAABox aabb = prim.CalculateAABox(xf);
  CAABox offsetAABB = aabb;
  offsetAABB.AccumulateBounds(aabb.GetMinPoint() + dirScale);
  offsetAABB.AccumulateBounds(aabb.GetMaxPoint() + dirScale);

  if (!offsetAABB.Inside(cache.GetCacheBounds())) {
    CAABox newAABB(offsetAABB.GetMinPoint() - CVector3f(0.2f, 0.2f, 0.2f),
                   offsetAABB.GetMaxPoint() + CVector3f(0.2f, 0.2f, 0.2f));
    newAABB.AccumulateBounds(cache.GetCacheBounds().GetMinPoint());
    newAABB.AccumulateBounds(cache.GetCacheBounds().GetMaxPoint());
    cache.SetCacheBounds(newAABB);
    BuildAreaCollisionCache(mgr, cache);
    IsUser(0);
  }

  if (prim.GetPrimType() == 'AABX') {
    for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
      CCollisionInfo info(CCollisionInfo::kI_Invalid);
      double d = dOut;
      float distance = dOut;
      if (CMetroidAreaCollider::MovingAABoxCollisionCheck_Cached(cache.GetOctreeLeafCache(i), aabb,
                                                                 filter, CMaterialList(kMT_Solid),
                                                                 dir, distance, info, d) &&
          d < dOut) {
        infoOut = info;
        dOut = float(d);
      }
    }
  } else if (prim.GetPrimType() == 'SPHR') {
    const CSphere& sphere = static_cast< const CCollidableSphere& >(prim).GetSphere();
    for (int i = 0; i < int(cache.GetNumCaches()); ++i) {
      CCollisionInfo info(CCollisionInfo::kI_Invalid);
      double d = dOut;
      float distance = dOut;
      if (CMetroidAreaCollider::MovingSphereCollisionCheck_Cached(
              cache.GetOctreeLeafCache(i), aabb,
              CSphere(xf * sphere.GetCenter(), sphere.GetRadius()), filter,
              CMaterialList(kMT_Solid), dir, distance, info, d) &&
          d < dOut) {
        infoOut = info;
        dOut = float(d);
      }
    }
  }

  return infoOut.IsValid();
}

bool CGameCollision::DetectDynamicCollision(const CCollisionPrimitive& prim, const CTransform4f& xf,
                                            const TEntityList& nearList, TUniqueId& idOut,
                                            CCollisionInfoList& infoList,
                                            const CStateManager& mgr) {
  for (const TUniqueId* id = nearList.begin(); id != nearList.end(); ++id) {
    if (const CPhysicsActor* actor = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id))) {
      if (CCollisionPrimitive::Collide(
              CInternalCollisionStructure::CPrimDesc(prim, CMaterialFilter::skPassEverything, xf),
              CInternalCollisionStructure::CPrimDesc(*actor->GetCollisionPrimitive(),
                                                     CMaterialFilter::skPassEverything,
                                                     actor->GetPrimitiveTransform()),
              infoList)) {
        idOut = actor->GetUniqueId();
        return true;
      }
    }
  }

  idOut = kInvalidUniqueId;
  return false;
}

bool CGameCollision::DetectDynamicCollisionBoolean(const CCollisionPrimitive& prim,
                                                   const CTransform4f& xf,
                                                   const TEntityList& nearList,
                                                   const CStateManager& mgr) {
  for (const TUniqueId* id = nearList.begin(); id != nearList.end(); ++id) {
    if (const CPhysicsActor* actor = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id))) {
      if (CCollisionPrimitive::CollideBoolean(
              CInternalCollisionStructure::CPrimDesc(prim, CMaterialFilter::skPassEverything, xf),
              CInternalCollisionStructure::CPrimDesc(*actor->GetCollisionPrimitive(),
                                                     CMaterialFilter::skPassEverything,
                                                     actor->GetPrimitiveTransform()))) {
        return true;
      }
    }
  }

  return false;
}

bool CGameCollision::DetectDynamicCollisionMoving(const CCollisionPrimitive& prim,
                                                  const CTransform4f& xf,
                                                  const TEntityList& nearList, CVector3f dir,
                                                  TUniqueId& idOut, CCollisionInfo& infoOut,
                                                  double& dOut, const CStateManager& mgr) {
  bool ret = false;
  const TUniqueId* id = nearList.begin();

  while (id != nearList.end()) {
    const CPhysicsActor* const actor = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id));
    double d = dOut;
    CCollisionInfo info(CCollisionInfo::kI_Invalid);

    if (actor) {
      if (CCollisionPrimitive::CollideMoving(
              CInternalCollisionStructure::CPrimDesc(prim, CMaterialFilter::skPassEverything, xf),
              CInternalCollisionStructure::CPrimDesc(*actor->GetCollisionPrimitive(),
                                                     CMaterialFilter::skPassEverything,
                                                     actor->GetPrimitiveTransform()),
              dir, d, info) &&
          d < dOut) {
        ret = true;
        infoOut = info;
        dOut = d;
        idOut = actor->GetUniqueId();
      }
    }

    ++id;
  }

  return ret;
}

bool CGameCollision::DetectCollision_Cached_Moving(
    const CStateManager& mgr, CAreaCollisionCache& cache, const CCollisionPrimitive& prim,
    const CTransform4f& xf, const CMaterialFilter& filter, const TEntityList& nearList,
    CVector3f dir, TUniqueId& idOut, CCollisionInfo& infoOut, double& dOut) {
  bool ret = false;
  idOut = kInvalidUniqueId;

  if (!filter.GetExcludeList().HasMaterial(kMT_NoStaticCollision)) {
    if (DetectStaticCollision_Cached_Moving(mgr, cache, prim, xf, filter, dir, infoOut, dOut)) {
      ret = true;
    }
  }

  if (DetectDynamicCollisionMoving(prim, xf, nearList, dir, idOut, infoOut, dOut, mgr)) {
    ret = true;
  }

  return ret;
}

CRayCastResult CGameCollision::RayWorldIntersection(const CStateManager& mgr, TUniqueId& idOut,
                                                    const CVector3f& pos, const CVector3f& dir,
                                                    float mag, const CMaterialFilter& filter,
                                                    const TEntityList& nearList) {
  CRayCastResult staticRes = RayStaticIntersection(mgr, pos, dir, mag, filter);
  CRayCastResult dynamicRes = RayDynamicIntersection(mgr, idOut, pos, dir, mag, filter, nearList);

  if (dynamicRes.IsValid()) {
    if (staticRes.IsInvalid()) {
      return dynamicRes;
    }
    if (staticRes.GetTime() >= dynamicRes.GetTime()) {
      return dynamicRes;
    }
  }

  return staticRes;
}

CRayCastResult CGameCollision::RayDynamicIntersection(const CStateManager& mgr, TUniqueId& idOut,
                                                      const CVector3f& pos, const CVector3f& dir,
                                                      float mag, const CMaterialFilter& filter,
                                                      const TEntityList& nearList) {
  float bestT;
  if (mag > 0.f) {
    bestT = mag;
  } else {
    bestT = 100000.f;
  }
  CRayCastResult ret;

  for (const TUniqueId* id = nearList.begin(); id != nearList.end(); ++id) {
    if (const CPhysicsActor* const actor =
            TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id))) {
      const CRayCastResult result = actor->GetCollisionPrimitive()->CastRay(
          pos, dir, bestT, filter, actor->GetPrimitiveTransform());
      if (result.IsValid() && result.GetTime() < bestT) {
        bestT = result.GetTime();
        ret = result;
        idOut = actor->GetUniqueId();
      }
    }
  }

  return ret;
}

bool CGameCollision::RayDynamicIntersectionBool(const CStateManager& mgr, const CVector3f& pos,
                                                const CVector3f& dir, const CMaterialFilter& filter,
                                                const TEntityList& nearList, const CActor* damagee,
                                                float length) {
  float mag = length > 0.f ? length : 100000.f;

  for (const TUniqueId* id = nearList.begin(); id != nearList.end(); ++id) {
    if (const CPhysicsActor* actor = TCastToConstPtr< CPhysicsActor >(mgr.GetObjectById(*id))) {
      if (damagee == nullptr || actor->GetUniqueId() != damagee->GetUniqueId()) {
        CRayCastResult result = actor->GetCollisionPrimitive()->CastRay(
            pos, dir, mag, filter, actor->GetPrimitiveTransform());
        if (result.IsValid()) {
          return false;
        }
      }
    }
  }

  return true;
}

bool CGameCollision::RayStaticIntersectionBool(const CStateManager& mgr, const CVector3f& pos,
                                               const CVector3f& dir, float mag,
                                               const CMaterialFilter& filter) {
  const CUnitVector3f unitDir = CUnitVector3f(dir.GetX(), dir.GetY(), dir.GetZ());
  CLine line(pos, unitDir);
  const CWorld* world = mgr.GetWorld();
  float length;
  if (mag > 0.f) {
    length = mag;
  } else {
    length = 100000.f;
  }

  for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
       it != CWorld::skGlobalEnd; ++it) {
    const CAreaOctTree& collision = (*it).GetOctTree();
    CAreaOctTree::Node node(collision.GetTreeMemory(), collision.GetBoundingBox(), collision,
                            collision.GetTreeType());
    if (!node.LineTest(line, filter, length)) {
      return false;
    }
  }

  return true;
}

bool CGameCollision::RayStaticIntersectionArea(const CGameArea& area, const CVector3f& pos,
                                               const CVector3f& dir, float mag,
                                               const CMaterialFilter& filter) {
  const CUnitVector3f unitDir = CUnitVector3f(dir.GetX(), dir.GetY(), dir.GetZ());
  CLine line(pos, unitDir);
  float length = mag > 0.f ? mag : 100000.f;

  const CAreaOctTree& collision = area.GetOctTree();
  CAreaOctTree::Node node(collision.GetTreeMemory(), collision.GetBoundingBox(), collision,
                          collision.GetTreeType());
  if (!node.LineTest(line, filter, length)) {
    return false;
  }
  return true;
}

CRayCastResult CGameCollision::RayStaticIntersection(const CStateManager& mgr, const CVector3f& pos,
                                                     const CVector3f& dir, float mag,
                                                     const CMaterialFilter& filter) {
  const CWorld* world = mgr.GetWorld();
  const CUnitVector3f unitDir = CUnitVector3f(dir.GetX(), dir.GetY(), dir.GetZ());
  CLine line(pos, unitDir);
  CRayCastResult ret;
  float bestT = mag > 0.f ? mag : 100000.f;
  for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
       it != CWorld::skGlobalEnd; ++it) {
    CAreaOctTree::SRayResult rayRes;
    const CAreaOctTree& collision = (*it).GetOctTree();
    CAreaOctTree::Node node(collision.GetTreeMemory(), collision.GetBoundingBox(), collision,
                            collision.GetTreeType());
    node.LineTestEx(line, filter, rayRes, mag);

    if (!rayRes.x10_surface || (mag != 0.f && mag < rayRes.x3c_t)) {
      continue;
    }

    if (rayRes.x3c_t < bestT) {
      ret = CRayCastResult(rayRes.x3c_t, pos + dir * rayRes.x3c_t, rayRes.x0_plane,
                           CMaterialList(rayRes.x10_surface->GetSurfaceFlags()));
      bestT = rayRes.x3c_t;
    }
  }

  return ret;
}

bool CGameCollision::IsFloor(const CMaterialList& material, const CVector3f& normal) {
  if (material.HasMaterial(kMT_Floor)) {
    return true;
  }
  return normal.GetZ() > 0.85f;
}

void CGameCollision::MakeCollisionCallbacks(CStateManager& mgr, CPhysicsActor& actor,
                                            const TUniqueId& id, const CCollisionInfoList& list) {
  actor.CollidedWith(id, list, mgr);

  if (id != kInvalidUniqueId) {
    if (CPhysicsActor* otherActor = TCastToPtr< CPhysicsActor >(mgr.ObjectById(id))) {
      CCollisionInfoList swapList(list);
      swapList.Swap(0);
      // Retail passes the original list after swapping the copy.
      otherActor->CollidedWith(actor.GetUniqueId(), list, mgr);
    }
  }
}

void CGameCollision::SendScriptMessages(CStateManager& mgr, CActor& actor0, CActor* actor1,
                                        const CCollisionInfoList& list) {
  bool hasPlatform = false;
  bool hasFloor = false;

  for (int i = 0; i < list.GetCount(); ++i) {
    const CCollisionInfo& info = list[i];
    const CMaterialList& matLeft = info.GetMaterialLeft();
    if (IsFloor(matLeft, info.GetNormalLeft())) {
      hasFloor = true;
      if (matLeft.HasMaterial(kMT_Platform)) {
        hasPlatform = true;
      }
      SendMaterialMessage(mgr, info.GetMaterialLeft(), actor0);
    }
  }

  if (hasFloor) {
    mgr.DeliverScriptMsg(&actor0, kInvalidUniqueId, kSM_OnFloor);
    if (hasPlatform) {
      if (CScriptPlatform* const platform = TCastToPtr< CScriptPlatform >(actor1)) {
        mgr.DeliverScriptMsg(platform, actor0.GetUniqueId(), kSM_AddPlatformRider);
      }
    }
  } else if (actor1 != nullptr) {
    if (CScriptPlatform* const platform = TCastToPtr< CScriptPlatform >(&actor0)) {
      for (int i = 0; i < list.GetCount(); ++i) {
        const CCollisionInfo& info = list[i];
        const CMaterialList& matRight = info.GetMaterialRight();
        if (IsFloor(matRight, info.GetNormalRight()) && matRight.HasMaterial(kMT_Platform)) {
          hasPlatform = true;
          break;
        }
      }
      if (hasPlatform) {
        mgr.DeliverScriptMsg(platform, actor1->GetUniqueId(), kSM_AddPlatformRider);
      }
    }
  }
}

void CGameCollision::SendMaterialMessage(CStateManager& mgr, const CMaterialList& material,
                                         CActor& actor) {
  EScriptObjectMessage msg;
  if (material.HasMaterial(kMT_Ice)) {
    msg = kSM_OnIceSurface;
  } else if (material.HasMaterial(kMT_MudSlow)) {
    msg = kSM_OnMudSlowSurface;
  } else {
    msg = kSM_OnNormalSurface;
  }

  mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, msg);
}

void CGameCollision::ShowCollisionResults(CCollisionInfoList& info, const CColor& color) {}

float CGameCollision::GetCoefficientOfRestitution(const CCollisionInfo& info) { return 0.f; }
static float CollisionImpulseFiniteVsFinite(float mass0, float mass1, float velNormDot,
                                            float restitution) {
  return (-(1.f + restitution) * velNormDot) / ((1.f / mass0) + (1.f / mass1));
}
static float CollisionImpulseFiniteVsInfinite(const float mass, const float velNormDot,
                                              const float restitution) {
  return mass * (-(1.f + restitution) * velNormDot);
}

CVector3f CGameCollision::GetActorRelativeVelocities(const CPhysicsActor* actor0,
                                                     const CPhysicsActor* actor1) {
  float x = actor0->GetVelocityWR().GetX();
  float y = actor0->GetVelocityWR().GetY();
  float z = actor0->GetVelocityWR().GetZ();

  if (actor1 != NULL) {
    const CScriptPlatform* platform = TCastToConstPtr< CScriptPlatform >(actor1);
    bool rider = false;
    if (platform) {
      rider = platform->IsRider(actor0->GetUniqueId());
    }
    if (!rider) {
      x -= actor1->GetVelocityWR().GetX();
      y -= actor1->GetVelocityWR().GetY();
      z -= actor1->GetVelocityWR().GetZ();
    }
  }

  return CVector3f(x, y, z);
}

void CGameCollision::Move(CStateManager& mgr, CPhysicsActor& actor, float dt,
                          const TEntityList* colliderList) {
  if (!actor.GetMovable()) {
    return;
  }

  if (actor.GetMaterialList().HasMaterial(kMT_GroundCollider) || actor.WillMove(mgr)) {
    if (actor.GetApplyRotationWhenInCollision()) {
      actor.AddMotionState(actor.PredictAngularMotion(dt));
    }
    actor.UseCollisionImpulses();

    if (actor.GetMaterialList().HasMaterial(kMT_Solid)) {
      if (actor.GetMaterialList().HasMaterial(kMT_Player)) {
        MovePlayer(mgr, actor, dt, colliderList);
      } else if (actor.GetMaterialList().HasMaterial(kMT_GroundCollider)) {
        CGroundMovement::MoveGroundCollider(mgr, actor, dt, colliderList);
      } else {
        CAABoxFilter filter(actor);
        MoveAndCollide(mgr, actor, dt, filter, colliderList);
      }
    } else {
      CMotionState motion = actor.PredictMotion_Internal(dt);
      actor.AddMotionState(motion);
      actor.ClearForcesAndTorques();
    }

    mgr.UpdateActorInSortedLists(actor);
  }
}

void CGameCollision::MovePlayer(CStateManager& mgr, CPhysicsActor& actor, float dt,
                                const TEntityList* colliderList) {
  actor.SetApplyRotationWhenInCollision(true);
  actor.AddMotionState(actor.PredictAngularMotion(dt));

#ifdef TARGET_PC
  // Remastered movement: Remastered's stepped mover (0xf943b4) caps a standing
  // player's rise before the move, scaling the whole velocity, where retail
  // caps it only after the move. Without the early cap the jump's first tick
  // moves at the uncapped speed (issue #117).
  if (CPlayer* player = TCastToPtr< CPlayer >(&actor)) {
    if (player->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
        player->GetPlayerMovementState() != NPlayer::kMS_OnGround &&
        player->MouseControlsAllowed(mgr) && PortDebug::RemasteredMovement()) {
      const float maxUp = player->GetMaximumPlayerPositiveVerticalVelocity(mgr);
      const CVector3f velocity = player->GetVelocityWR();
      if (velocity.GetZ() > maxUp) {
        player->SetVelocityWR(velocity * (maxUp / velocity.GetZ()));
      }
    }
  }
#endif

  if (actor.IsStandardCollider()) {
    CBallFilter filter(actor);
    MoveAndCollide(mgr, actor, dt, filter, colliderList);
  } else if (actor.GetMaterialList().HasMaterial(kMT_GroundCollider)) {
    if (gkUseNewPlayerMovement) {
      CGroundMovement::MoveGroundCollider_New(mgr, actor, dt, colliderList);
    } else {
      CGroundMovement::MoveGroundCollider(mgr, actor, dt, colliderList);
    }
  } else {
#ifdef TARGET_PC
    if (CPlayer* player = TCastToPtr< CPlayer >(&actor)) {
      if (player->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
          player->MouseControlsAllowed(mgr) && PortDebug::RemasteredMovement()) {
        MoveAirborneStepped(mgr, *player, dt, colliderList);
        actor.SetApplyRotationWhenInCollision(false);
        return;
      }
    }
#endif
    CBallFilter filter(actor);
    MoveAndCollide(mgr, actor, dt, filter, colliderList);
  }

  actor.SetApplyRotationWhenInCollision(false);
}

#ifdef TARGET_PC
// Remastered's airborne player mover: CPhysicsSteppedMoverColliderMP1::MoveObject
// 0xf943b4 / DoMovementStep 0xf948d4, with the impact callback
// CPlayerMP1::ImpactResponseForOffGroundMoverCollider 0xc6d460 and the landing
// check GroundCheckForOffGroundMoverCollider 0xc6d110. Retail MoveAndCollide
// resets a blocked player to its last free state and zeroes its vertical
// speed, so a jump into a lip or wall stalled (issue #117). The mover slides
// along the contact instead.
void CGameCollision::MoveAirborneStepped(CStateManager& mgr, CPlayer& player, float dt,
                                         const TEntityList* colliderList) {
  CPhysicsActor& actor = player;
  const CMaterialFilter& materialFilter = actor.GetMaterialFilter();
  const float mass = actor.GetMass();

  CAABox motionVolume = actor.GetMotionVolume(dt);
  TEntityList nearList;
  if (colliderList != NULL) {
    nearList = *colliderList;
  } else {
    mgr.BuildColliderList(nearList, actor,
                          CAABox(motionVolume.GetMinPoint() - CVector3f(1.f, 1.f, 1.f),
                                 motionVolume.GetMaxPoint() + CVector3f(1.f, 1.f, 1.f)));
  }
  CAreaCollisionCache cache(motionVolume);
  BuildAreaCollisionCache(mgr, cache);

  // NCollisionUtils is_floor / is_wall (0xc7xxxx). Port contacts carry
  // DefaultCollisionMaterial's kind 0xffffffff; kMT_Floor stands in for kind 0
  // (inferred, not proven). Remastered normals are Y-up: normal.up = port Z.
  struct Classify {
    static bool IsFloor(const CMaterialList& mat, const CVector3f& n) {
      // is_floor: up > 0.85 || kind == 0 || (up > 0.7071 && kind >= 3).
      return mat.HasMaterial(kMT_Floor) || n.GetZ() > 0.70710677f;
    }
    static bool IsWall(const CMaterialList& mat, const CVector3f& n) {
      // is_wall: false above 0.85; kind 2 true; else -0.7071 <= up <= 0.7071 && kind > 1.
      return !mat.HasMaterial(kMT_Floor) && n.GetZ() >= -0.70710677f && n.GetZ() <= 0.70710677f;
    }
  };

  bool hadCollision = false;
  bool grounded = false; // Remastered object state == 1
  CCollisionInfoList groundList;
  TUniqueId riderId = kInvalidUniqueId; // object +0x1210

  // Collision test with the primitive shifted by `offset` from the actor.
  struct Test {
    CStateManager& mgr;
    CAreaCollisionCache& cache;
    CPhysicsActor& actor;
    const CMaterialFilter& filter;
    const TEntityList& list;
    bool Blocked(const CVector3f& offset) const {
      actor.MoveCollisionPrimitive(offset);
      const bool b = DetectCollisionBoolean_Cached(mgr, cache, *actor.GetCollisionPrimitive(),
                                                   actor.GetPrimitiveTransform(), filter, list);
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      return b;
    }
  };
  const Test test = {mgr, cache, actor, materialFilter, nearList};

  // 0xc6d110 GroundCheckForOffGroundMoverCollider(from, target): the actor sits
  // at `target` except for its height.
  struct Ground {
    static bool Check(CStateManager& mgr, CAreaCollisionCache& cache, CPlayer& player,
                      const CMaterialFilter& filter, const TEntityList& list,
                      const CVector3f& from, float targetZ) {
      CPhysicsActor& actor = player;
      if (player.GetPlayerMovementState() == NPlayer::kMS_OnGround) {
        return true;
      }
      if (player.GetFallingTime() <= 0.4f) {
        return false;
      }
      const float dz = targetZ - actor.GetTranslation().GetZ();
      const float aabbMin =
          actor.GetCollisionPrimitive()->CalculateAABox(actor.GetPrimitiveTransform())
              .GetMinPoint()
              .GetZ() +
          dz;
      const float feet = aabbMin; // target.up - height (vtable 0x198)
      float probeZ = rstl::max_val(feet, from.GetZ());
      probeZ = rstl::min_val(probeZ, targetZ - 0.002f);
      actor.MoveCollisionPrimitive(CVector3f(0.f, 0.f, probeZ - actor.GetTranslation().GetZ()));
      CCollisionInfoList list2;
      TUniqueId id2 = kInvalidUniqueId;
      DetectCollision_Cached(mgr, cache, *actor.GetCollisionPrimitive(),
                             actor.GetPrimitiveTransform(), filter, list, id2, list2);
      actor.MoveCollisionPrimitive(CVector3f::Zero());
      bool result = false;
      float low = feet - 0.002f;
      for (int i = 0; i < list2.GetCount(); ++i) {
        const CCollisionInfo& info = list2[i];
        const float pz = info.GetPoint().GetZ();
        if (pz > aabbMin || pz <= low) {
          continue;
        }
        result = Classify::IsFloor(info.GetMaterialLeft(), info.GetNormalLeft());
        low = pz;
      }
      return result;
    }
  };

  // 0xf948d4 DoMovementStep: returns the time consumed.
  // One movement step (0xf948d4). `consumedOut` is the time the step reports.
  struct Stepper {
    CStateManager& mgr;
    CAreaCollisionCache& cache;
    CPlayer& player;
    CPhysicsActor& actor;
    const CMaterialFilter& filter;
    const TEntityList& nearList;
    const Test& test;
    float mass;
    float worldDt;
    bool& hadCollision;
    bool& grounded;
    CCollisionInfoList& groundList;
    TUniqueId& riderId;

    // 511-533: post-step 0.1 down probe while rising.
    void PostProbe() {
      if (grounded || actor.GetVelocityWR().GetZ() <= 0.f) {
        return;
      }
      const CVector3f pos = actor.GetTranslation();
      if (Ground::Check(mgr, cache, player, filter, nearList, pos, pos.GetZ() - 0.1f)) {
        grounded = true;
      }
    }

    float Step(float stepDt) {
      const CVector3f startPos = actor.GetTranslation();
      CMotionState motion = actor.PredictMotion_Internal(stepDt);
      if (!test.Blocked(motion.GetTranslation())) {
        // The whole step is free (0xf948d4 line ~511).
        actor.AddMotionState(motion);
        actor.ImpulseWR() = CVector3f::Zero();
        PostProbe();
        return stepDt;
      }

      // Exactly four bisection tests: dt/2, then +-dt/4, dt/8, dt/16; the
      // reported time is the bracket midpoint, the state the last free one.
      float loT = 0.f;
      float hiT = stepDt;
      float t = stepDt * 0.5f;
      float half = stepDt * 0.5f;
      float reported = t;
      while (stepDt * 0.05f < half) {
        const bool blocked = test.Blocked(actor.PredictMotion_Internal(t).GetTranslation());
        half *= 0.5f;
        if (blocked) {
          hiT = t;
          t -= half;
        } else {
          loT = t;
          t += half;
        }
        reported = t;
      }

      // Contacts are gathered at the colliding state (hi).
      CCollisionInfoList list;
      TUniqueId id = kInvalidUniqueId;
      actor.MoveCollisionPrimitive(actor.PredictMotion_Internal(hiT).GetTranslation());
      DetectCollision_Cached(mgr, cache, *actor.GetCollisionPrimitive(),
                             actor.GetPrimitiveTransform(), filter, nearList, id, list);
      actor.MoveCollisionPrimitive(CVector3f::Zero());

      if (list.GetCount() == 0) {
        // No contact list: the state becomes hi (no response).
        actor.AddMotionState(actor.PredictMotion_Internal(hiT));
        actor.ImpulseWR() = CVector3f::Zero();
        PostProbe();
        return reported;
      }

      // Otherwise the state is lo; momentumless impulse seeded from 0x118c.
      actor.AddMotionState(actor.PredictMotion_Internal(loT));
      actor.ImpulseWR() = CVector3f::Zero();
      CVector3f mi = actor.GetMoveImpulseWR();
      const CVector3f stateVel = actor.GetVelocityWR();
      CPhysicsActor* const other = TCastToPtr< CPhysicsActor >(mgr.ObjectById(id));
      CVector3f riderVel = CVector3f::Zero();
      if (other != NULL && riderId != kInvalidUniqueId && id == riderId) {
        riderVel = other->GetVelocityWR();
      }

      // The port's overlap test also reports a player resting on (or grazing)
      // a floor it is not moving into, which the 0xf948d4 per-contact filter
      // skips for every contact (moving away relative to the rider, or a
      // paired ceiling face). Nothing then blocks the step, so it takes the
      // whole dt as a free target does; bisecting to lo would drop the time
      // between lo and dt (a free jump lost 1/32 of its first push).
      {
        bool anyActing = false;
        for (int i = 0; i < list.GetCount(); ++i) {
          const CVector3f cn = list[i].GetNormalLeft();
          if (cn.GetZ() < -0.70710677f || CVector3f::Dot(stateVel - riderVel, cn) <= 0.f) {
            anyActing = true;
            break;
          }
        }
        if (!anyActing) {
          actor.AddMotionState(actor.PredictMotion_Internal(stepDt - loT));
          actor.ImpulseWR() = CVector3f::Zero();
          PostProbe();
          return stepDt;
        }
      }

      bool floorContact = false;
      CCollisionInfoList accepted;
      for (int i = 0; i < list.GetCount(); ++i) {
        const CCollisionInfo& info = list[i];
        CVector3f n = info.GetNormalLeft();
        // Normal components below 2.38e-7 are zeroed (0xf948d4).
        if (fabsf(n.GetX()) < 2.38e-7f) n.SetX(0.f);
        if (fabsf(n.GetY()) < 2.38e-7f) n.SetY(0.f);
        if (fabsf(n.GetZ()) < 2.38e-7f) n.SetZ(0.f);
        // Remastered reports a corner touch as one contact with a secondary
        // normal (+0x44/+0x38; 0xc6d460 picks the working normal when either
        // normal is a ceiling, is_ceiling_normal = up < -0.70710677). The port
        // lists the two faces as separate contacts at one point; the ceiling
        // face of such a pair is dropped so only the wall normal acts (a
        // jump along a ledge corner keeps its rise). Which normal Remastered
        // keeps is inferred from the ledge-jump measurement.
        if (n.GetZ() < -0.70710677f) {
          bool pairedWithOther = false;
          for (int j = 0; j < list.GetCount(); ++j) {
            const CCollisionInfo& o = list[j];
            const CVector3f d = o.GetPoint() - info.GetPoint();
            if (j != i && o.GetNormalLeft().GetZ() >= -0.70710677f &&
                d.MagSquared() < 1e-8f) {
              pairedWithOther = true;
            }
          }
          if (pairedWithOther) continue;
        }
        // A contact the player is already separating from (relative to its
        // rider) is skipped.
        if (CVector3f::Dot(stateVel - riderVel, n) > 0.f) {
          continue;
        }
        accepted.Add(info, false);
        hadCollision = true;

        const bool isFloor = Classify::IsFloor(info.GetMaterialLeft(), n);
        if (isFloor) {
          floorContact = true;
          groundList.Add(info, false);
          if (other != NULL) {
            riderId = id;
          }
        }

        // 0xc6d460 ImpactResponseForOffGroundMoverCollider (flag 0x100 is never
        // set for port contacts: its setter is not in the decompile).
        CVector3f vel = actor.GetVelocityWR();
        const bool flatFloor = fabsf(1.f - n.GetZ()) < 1e-5f;
        if (flatFloor && (vel.GetZ() > 0.f ||
                          actor.GetTranslation().GetZ() <= info.GetPoint().GetZ())) {
          continue;
        }
        if (!isFloor) {
          // Push scale 0.0025 / world step dt * mass.
          mi += n * (0.0025f / worldDt) * mass;
        }
        const float miDot = CVector3f::Dot(n, mi);
        if (miDot < 0.f) {
          mi -= n * miDot; // 0xc186a0
        }
        const float velDot = CVector3f::Dot(n, vel);
        if (velDot <= 1.19e-7f) {
          vel -= n * velDot; // 0xc18700, e = mu = 0
        }
        if (player.GetPlayerMovementState() == NPlayer::kMS_ApplyJump) {
          const bool isWall = Classify::IsWall(info.GetMaterialLeft(), n);
          if (isWall && n.GetZ() < -0.1f && vel.GetZ() > 0.f) {
            vel.SetZ(vel.GetZ() * 0.5f);
          }
          float t1 = 0.25f;
          float t2 = 0.37f;
          float scale = 1.f;
          float mult = 1.f;
          float floorMag = 0.f;
          if (player.CheckSubmerged() &&
              !mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_GravitySuit)) {
            t1 = 0.01f;
            t2 = 0.05f;
            scale = 0.8f;
            mult = 35.f;
            floorMag = 5.f;
          }
          const float az = fabsf(n.GetZ());
          if (az > t1 && (az > t2 || vel.GetZ() < 0.f)) {
            const float mag = rstl::max_val(mult * az, floorMag);
            actor.SetForceWR(CVector3f(0.f, 0.f, 1.f) * (mag + 1.f) * mass * player.GetGravity());
            vel = vel * scale;
          }
        }
        actor.SetVelocityWR(vel);
      }
      // SetMomentumlessImpulse(local_10c0), colliding branch only (line 503).
      actor.SetMoveImpulseWR(mi);

      if (accepted.GetCount() > 0) {
        MakeCollisionCallbacks(mgr, actor, id, accepted);
      }
      if (floorContact && !grounded) {
        if (Ground::Check(mgr, cache, player, filter, nearList, startPos,
                          actor.GetTranslation().GetZ())) {
          grounded = true;
        }
        return reported; // a floor contact skips the post-probe (line 509)
      }
      if (!floorContact) {
        PostProbe();
      }
      return reported;
    }
  };
  Stepper stepper = {mgr,  cache,       player,      actor,    materialFilter,
                     nearList, test,    mass,        dt,       hadCollision,
                     grounded, groundList, riderId};

  // 0xf943b4 MoveObject: one step, then up to ten more while time remains.
  CVector3f prevVel = actor.GetVelocityWR();
  const CVector3f entryVel = prevVel;
  const CVector3f entryPos = actor.GetTranslation();
  float remaining = dt - stepper.Step(dt);
  bool stalledFlag = false;
  if (remaining > 1e-5f) {
    int iter = 0;
    int stallCount = 0;
    for (;;) {
      if (grounded) {
        break;
      }
      const float consumed = stepper.Step(remaining);
      bool keep;
      if (iter < 5 || fabsf(consumed) >= 1e-5f) {
        ++iter;
        keep = true;
        if (close_enough(prevVel, actor.GetVelocityWR(), 1e-4f)) {
          stalledFlag = true;
          const bool second = stallCount > 0;
          ++stallCount;
          if (second) {
            keep = false;
          }
        }
        if (keep) {
          prevVel = actor.GetVelocityWR();
        }
      } else {
        keep = false;
      }
      remaining -= consumed;
      if (!keep || iter > 9 || remaining <= 1e-5f) {
        break;
      }
    }
  }

  if (groundList.GetCount() > 0 && grounded) {
    SendScriptMessages(mgr, actor, NULL, groundList);
  }
  if (!hadCollision) {
    mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_Falling);
  }
  if (!(fabsf(remaining) < 1e-5f && !stalledFlag)) {
    const bool resolved = CollisionFailsafe(mgr, cache, actor, *actor.GetCollisionPrimitive(),
                                            nearList, remaining / dt, 2);
    // 0xf943b4: a failsafe that did not reposition the actor, with the whole
    // dt still unconsumed, stops it dead.
    if (!resolved && fabsf(remaining - dt) < 1e-5f) {
      actor.SetVelocityWR(CVector3f::Zero());
    }
    // 0xf943b4: falling fast (entry up-velocity < -0.75) and still speeding
    // up although the actor did not move down, vertical velocity is zeroed.
    const CVector3f endVel = actor.GetVelocityWR();
    if (entryVel.GetZ() < -0.75f && endVel.GetZ() < entryVel.GetZ() &&
        -0.001f < actor.GetTranslation().GetZ() - entryPos.GetZ()) {
      actor.SetVelocityWR(CVector3f(endVel.GetX(), endVel.GetY(), 0.f));
    }
  }
  actor.ClearForcesAndTorques();
  actor.MoveCollisionPrimitive(CVector3f::Zero());
}
#endif

void CGameCollision::MoveAndCollide(CStateManager& mgr, CPhysicsActor& actor, float dt,
                                    const ICollisionFilter& filter,
                                    const TEntityList* colliderList) {
  bool hadCollision = false;
  bool resolvedCollision = false;
  bool isPlayer = false;
  uint iterCount = 0;
  if (actor.GetMaterialList().HasMaterial(kMT_Player)) {
    isPlayer = true;
  }

  float remainingDt = dt;
  float maxStepDt = dt;
  float stepDt = dt;
  CCollisionInfoList collisionList;
  CMotionState motion = actor.PredictMotion_Internal(dt);
  const float translationMag = motion.GetTranslation().Magnitude();
  float minMoveMag = rstl::max_val(translationMag / (5.f * actor.GetCollisionAccuracyModifier()),
                                   0.0005f / actor.GetCollisionAccuracyModifier());
  const float collisionMinMag = 0.001f / actor.GetCollisionAccuracyModifier();
  const CMaterialFilter& materialFilter = actor.GetMaterialFilter();

  TEntityList nearList;
  CAABox motionVolume = actor.GetMotionVolume(dt);
  if (colliderList != NULL) {
    nearList = *colliderList;
  } else {
    mgr.BuildColliderList(nearList, actor,
                          CAABox(motionVolume.GetMinPoint() - CVector3f(1.f, 1.f, 1.f),
                                 motionVolume.GetMaxPoint() + CVector3f(1.f, 1.f, 1.f)));
  }

  CAreaCollisionCache cache(motionVolume);
  bool skipCacheBuild = actor.GetCollisionPrimitive()->GetPrimType() == 'OBTG' ||
                        materialFilter.GetExcludeList().HasMaterial(kMT_NoStaticCollision);

  if (!skipCacheBuild) {
    BuildAreaCollisionCache(mgr, cache);
    CAABox primAABB = actor.GetCollisionPrimitive()->CalculateAABox(actor.GetPrimitiveTransform());
    CVector3f center = primAABB.GetCenterPoint();
    float minExtent = 0.5f * GetMinExtentForCollisionPrimitive(*actor.GetCollisionPrimitive());
    if (translationMag > minExtent) {
      TUniqueId id = kInvalidUniqueId;
      CVector3f dir = motion.GetTranslation() / translationMag;
      CRayCastResult result =
          mgr.RayWorldIntersection(id, center, dir, translationMag, materialFilter, nearList);
      if (result.IsValid()) {
        stepDt = dt * (result.GetTime() / translationMag);
        motion = actor.PredictMotion_Internal(stepDt);
        maxStepDt = minExtent * (dt / translationMag);
        minMoveMag = rstl::min_val(minExtent, minMoveMag);
      }
    }
  }

  float curDt = stepDt;
  bool loopContinue = true;
  while (loopContinue) {
    actor.MoveCollisionPrimitive(motion.GetTranslation());
    if (DetectCollisionBoolean_Cached(mgr, cache, *actor.GetCollisionPrimitive(),
                                      actor.GetPrimitiveTransform(), materialFilter, nearList)) {
      hadCollision = true;
      if (motion.GetTranslation().Magnitude() < minMoveMag) {
        resolvedCollision = true;
        collisionList.Clear();

        TUniqueId id = kInvalidUniqueId;
        DetectCollision_Cached(mgr, cache, *actor.GetCollisionPrimitive(),
                               actor.GetPrimitiveTransform(), materialFilter, nearList, id,
                               collisionList);

        CPhysicsActor* const otherActor = TCastToPtr< CPhysicsActor >(mgr.ObjectById(id));
        actor.MoveCollisionPrimitive(CVector3f::Zero());

        CCollisionInfoList filteredList;
        CCollisionInfoList backfacedList;

        CVector3f relVel = GetActorRelativeVelocities(&actor, otherActor);
        CollisionUtil::FilterOutBackfaces(relVel, collisionList, backfacedList);
        if (backfacedList.GetCount() > 0) {
          filter.Filter(backfacedList, filteredList);
          if (filteredList.GetCount() == 0 && actor.GetMaterialList().HasMaterial(kMT_Player)) {
            const CMotionState& lastState = actor.GetLastNonCollidingState();
            actor.SetMotionState(CMotionState(
                lastState.GetTranslation(), lastState.GetOrientation(),
                lastState.GetVelocity() * 0.5f, lastState.GetAngularMomentum() * 0.5f));
          }
        }

        MakeCollisionCallbacks(mgr, actor, id, filteredList);
        if (IsUser(0)) {
          ShowCollisionResults(filteredList, CColor::Grey());
        }
        SendScriptMessages(mgr, actor, otherActor, filteredList);
        ResolveCollisions(actor, otherActor, filteredList);

        remainingDt -= stepDt;
        curDt = rstl::min_val(remainingDt, maxStepDt);
        stepDt = curDt;
      } else {
        curDt *= 0.5f;
        stepDt *= 0.5f;
      }
    } else {
      actor.AddMotionState(motion);
      remainingDt -= stepDt;
      stepDt = curDt;
      actor.ClearImpulses();
      actor.MoveCollisionPrimitive(CVector3f::Zero());
    }

    bool shouldContinue = false;
    ++iterCount;
    if (remainingDt > 0.f &&
        (((motion.GetTranslation().Magnitude() > collisionMinMag) && resolvedCollision) ||
         !resolvedCollision)) {
      shouldContinue = true;
    }
    loopContinue = shouldContinue;
    if (iterCount > 1000) {
      loopContinue = false;
    }
    if (loopContinue) {
      motion = actor.PredictMotion_Internal(stepDt);
    }
  }

  const float dtFrac = remainingDt / dt;
  if (!hadCollision && !actor.GetMaterialList().HasMaterial(kMT_GroundCollider)) {
    mgr.DeliverScriptMsg(&actor, kInvalidUniqueId, kSM_Falling);
  }
  if (isPlayer) {
    CollisionFailsafe(mgr, cache, actor, *actor.GetCollisionPrimitive(), nearList, dtFrac, 2);
  }
  actor.ClearForcesAndTorques();
  actor.MoveCollisionPrimitive(CVector3f::Zero());
  IsUser(0);
}

bool CGameCollision::CollisionFailsafe(const CStateManager& mgr, CAreaCollisionCache& cache,
                                       CPhysicsActor& actor, const CCollisionPrimitive& prim,
                                       const TEntityList& nearList, float dtFrac,
                                       uint failsafeTicks) {
  actor.MoveCollisionPrimitive(CVector3f::Zero());
  if (dtFrac > 0.5f) {
    actor.SetNumTicksPartialUpdate(actor.GetNumTicksPartialUpdate() + 1);
  }

  if (actor.GetNumTicksPartialUpdate() > 1 ||
      DetectCollisionBoolean_Cached(mgr, cache, prim, actor.GetPrimitiveTransform(),
                                    actor.GetMaterialFilter(), nearList)) {
    actor.SetNumTicksPartialUpdate(0);
    actor.SetNumTicksStuck(actor.GetNumTicksStuck() + 1);
    if (actor.GetNumTicksStuck() < failsafeTicks) {
      return false;
    }

    const CMotionState& oldState = actor.GetMotionState();
    const CMotionState& lastState = actor.GetLastNonCollidingState();
    actor.SetMotionState(CMotionState(lastState));

    if (!DetectCollisionBoolean_Cached(mgr, cache, prim, actor.GetPrimitiveTransform(),
                                       actor.GetMaterialFilter(), nearList)) {
      actor.SetLastNonCollidingState(
          CMotionState(lastState.GetTranslation(), lastState.GetOrientation(),
                       0.5f * lastState.GetVelocity(), lastState.GetAngularMomentum() * 0.5f));
      gDebugPrintCount++;
      actor.SetNumTicksStuck(0);
      return true; // repositioned to the last non-colliding state (0xc1fa68 returns 1)
    } else {
      actor.SetMotionState(CMotionState(oldState));
      rstl::optional_object< CVector3f > vec =
          FindNonIntersectingVector(mgr, cache, actor, prim, nearList);
      if (vec.valid()) {
        actor.SetMotionState(CMotionState(oldState.GetTranslation() + *vec,
                                          oldState.GetOrientation(), oldState.GetVelocity(),
                                          oldState.GetAngularMomentum()));
        actor.SetLastNonCollidingState(actor.GetMotionState());
        gDebugPrintCount++;
        return true; // pushed out along a non-intersecting vector
      } else {
        gDebugPrintCount++;
        actor.SetLastNonCollidingState(
            CMotionState(lastState.GetTranslation(), lastState.GetOrientation(),
                         0.5f * lastState.GetVelocity(), lastState.GetAngularMomentum() * 0.5f));
      }
    }
  } else {
    actor.SetLastNonCollidingState(actor.GetMotionState());
    actor.SetNumTicksStuck(0);
  }
  return false;
}

rstl::optional_object< CVector3f >
CGameCollision::FindNonIntersectingVector(const CStateManager& mgr, CAreaCollisionCache& cache,
                                          CPhysicsActor& actor, const CCollisionPrimitive& prim,
                                          const TEntityList& nearList) {
  CTransform4f xf = actor.GetPrimitiveTransform();
  CVector3f origin = xf.GetTranslation();
  const CVector3f center = prim.CalculateAABox(xf).GetCenterPoint();
  const CVector3f center2(center);

  float wx, wy, wz;
  for (int i = 2; i < 1000; i += i / 2) {
    const float pos = 0.005f * static_cast< float >(i);
    for (int j = 0; j < 26; ++j) {
      CVector3f vec = CVector3f::Zero();
      switch (j) {
      case 0:
        vec = CVector3f(0.f, pos, 0.f);
        break;
      case 1:
        vec = CVector3f(0.f, -pos, 0.f);
        break;
      case 2:
        vec = CVector3f(pos, 0.f, 0.f);
        break;
      case 3:
        vec = CVector3f(-pos, 0.f, 0.f);
        break;
      case 4:
        vec = CVector3f(0.f, 0.f, pos);
        break;
      case 5:
        vec = CVector3f(0.f, 0.f, -pos);
        break;
      case 6:
        vec = CVector3f(0.f, pos, pos);
        break;
      case 7:
        vec = CVector3f(0.f, -pos, -pos);
        break;
      case 8:
        vec = CVector3f(0.f, -pos, pos);
        break;
      case 9:
        vec = CVector3f(0.f, pos, -pos);
        break;
      case 10:
        vec = CVector3f(pos, 0.f, pos);
        break;
      case 11:
        vec = CVector3f(-pos, 0.f, -pos);
        break;
      case 12:
        vec = CVector3f(-pos, 0.f, pos);
        break;
      case 13:
        vec = CVector3f(pos, 0.f, -pos);
        break;
      case 14:
        vec = CVector3f(pos, pos, 0.f);
        break;
      case 15:
        vec = CVector3f(-pos, -pos, 0.f);
        break;
      case 16:
        vec = CVector3f(-pos, pos, 0.f);
        break;
      case 17:
        vec = CVector3f(pos, -pos, 0.f);
        break;
      case 18:
        vec = CVector3f(pos, pos, pos);
        break;
      case 19:
        vec = CVector3f(-pos, pos, pos);
        break;
      case 20:
        vec = CVector3f(pos, -pos, pos);
        break;
      case 21:
        vec = CVector3f(-pos, -pos, pos);
        break;
      case 22:
        vec = CVector3f(pos, pos, -pos);
        break;
      case 23:
        vec = CVector3f(-pos, pos, -pos);
        break;
      case 24:
        vec = CVector3f(pos, -pos, -pos);
        break;
      case 25:
        vec = CVector3f(-pos, -pos, -pos);
        break;
      }

      wx = origin.GetX() + vec.GetX();
      wy = origin.GetY() + vec.GetY();
      wz = origin.GetZ() + vec.GetZ();
      if (mgr.GetWorld()
              ->GetAreaAlways(mgr.GetNextAreaId())
              .GetAABB()
              .PointInside(CVector3f(wx, wy, wz))) {
        if (mgr.RayCollideWorld(center2, center2 + vec, nearList, CMaterialFilter::skPassEverything,
                                &actor)) {
          xf.SetTranslation(CVector3f(wx, wy, wz));
          if (!DetectCollisionBoolean_Cached(mgr, cache, prim, xf, actor.GetMaterialFilter(),
                                             nearList)) {
            return rstl::optional_object< CVector3f >(vec);
          }
        }
      }
    }
  }

  return rstl::optional_object_null();
}

bool CGameCollision::CanBlock(const CMaterialList& material, const CVector3f& normal) {
  if (material.HasMaterial(kMT_Character) && !material.HasMaterial(kMT_SolidCharacter)) {
    return false;
  }
  if (material.HasMaterial(kMT_NoPlayerCollision)) {
    return false;
  }

  if (material.HasMaterial(kMT_Floor)) {
    return true;
  }

  return normal.GetZ() > 0.85f;
}

void CGameCollision::AvoidStaticCollisionWithinRadius(const CStateManager& mgr,
                                                      CPhysicsActor& actor, uint iterations,
                                                      float dt, float height, float size,
                                                      float mass, float radius) {
  CVector3f actorPos = actor.GetTranslation();
  CVector3f pos = actorPos + CVector3f(0.f, 0.f, height);
  const float largeRadius = 1.2f * radius;
  CVector3f boundsExtent(size + largeRadius, size + largeRadius, largeRadius);
  CAABox aabb(pos - boundsExtent, pos + boundsExtent);
  CAreaCollisionCache cache(aabb);
  BuildAreaCollisionCache(mgr, cache);

  bool cacheOverflowed = cache.HasCacheOverflowed();

  CSphere sphere(pos, radius);

  if (DetectStaticCollisionBoolean_Cached(
          mgr, cache, CCollidableSphere(sphere, CMaterialList(kMT_Solid)), CTransform4f::Identity(),
          CMaterialFilter::MakeExclude(CMaterialList(kMT_Floor)))) {
    return;
  }

  CVector3f velocity = CVector3f::Zero();
  float seg = CMath::Deg2Rad(360.f) / static_cast< float >(static_cast< int >(iterations));
  for (int i = 0; i < static_cast< int >(iterations); ++i) {
    float angle = seg * static_cast< float >(i);
    float cosAngle = CMath::SlowCosineR(angle);
    CVector3f vec(CMath::SlowSineR(angle), cosAngle, 0.f);
    double out = size;
    CCollisionInfo info(CCollisionInfo::kI_Invalid);

    if (cacheOverflowed) {
      cache.ClearCache();
      CAABox aabb2(pos, pos);
      CVector3f step = size * vec;
      aabb2.AccumulateBounds(actorPos + step);
      CVector3f radiusExtent(radius, radius, radius);
      aabb2 = CAABox(aabb2.GetMinPoint() - radiusExtent, aabb2.GetMinPoint() + radiusExtent);
      cache.SetCacheBounds(aabb2);
      BuildAreaCollisionCache(mgr, cache);
    }

    {
      if (DetectStaticCollision_Cached_Moving(
              mgr, cache, CCollidableSphere(sphere, CMaterialList(kMT_Solid)),
              CTransform4f::Identity(), CMaterialFilter::MakeExclude(CMaterialList(kMT_Floor)), vec,
              info, out)) {
        float force = static_cast< float >(size - out) / size /
                      static_cast< float >(static_cast< int >(iterations));
        velocity -= force * vec;
      }
    }
  }

  CVector3f deltaVelocity = dt * (mass * velocity);
  actor.SetVelocityWR(actor.GetVelocityWR() + deltaVelocity);
}

float CGameCollision::GetMinExtentForCollisionPrimitive(const CCollisionPrimitive& prim) {
  if (prim.GetPrimType() == 'SPHR') {
    return 2.f * static_cast< const CCollidableSphere& >(prim).GetSphere().GetRadius();
  }

  if (prim.GetPrimType() == 'AABX') {
    const CAABox& aabb = static_cast< const CCollidableAABox& >(prim).GetBox();
    CVector3f extents = aabb.GetMaxPoint() - aabb.GetMinPoint();
    return rstl::min_val(rstl::min_val(extents[0], extents[1]), extents[2]);
  }

  if (prim.GetPrimType() == 'ABSH') {
    const CCollidableAABoxSphere& absPrim = static_cast< const CCollidableAABoxSphere& >(prim);
    return rstl::min_val(GetMinExtentForCollisionPrimitive(absPrim.GetCollidableAABox()),
                         GetMinExtentForCollisionPrimitive(absPrim.GetCollidableSphere()));
  }
  return 1.f;
}

void CGameCollision::ResolveCollisions(CPhysicsActor& actor0, CPhysicsActor* actor1,
                                       const CCollisionInfoList& list) {
  for (int i = 0; i < list.GetCount(); ++i) {
    CCollisionInfo infoCopy(list[i]);
    const float restitution =
        GetCoefficientOfRestitution(infoCopy) + actor0.GetCoefficientOfRestitutionModifier();
    if (actor1 != nullptr) {
      CollideWithDynamicBodyNoRot(actor0, *actor1, infoCopy, restitution, false);
    } else {
      CollideWithStaticBodyNoRot(actor0, infoCopy.GetMaterialLeft(), infoCopy.GetMaterialRight(),
                                 infoCopy.GetNormalLeft(), restitution, false);
    }
  }
}

void CGameCollision::CollideWithDynamicBodyNoRot(CPhysicsActor& actor0, CPhysicsActor& actor1,
                                                 const CCollisionInfo& info, float restitution,
                                                 bool active) {
  CVector3f normal(info.GetNormalLeft());
  if (active) {
    normal.SetZ(0.f);
  }

  CVector3f relVel = GetActorRelativeVelocities(&actor0, &actor1);
  float mass0 = actor0.GetMass();
  float mass1 = actor1.GetMass();
  float velNormDot = CVector3f::Dot(relVel, normal);

  float maxVel0 =
      rstl::max_val(actor0.GetVelocityWR().Magnitude(), actor0.GetMaximumCollisionVelocity());

  float maxVel1 =
      rstl::max_val(actor1.GetVelocityWR().Magnitude(), actor1.GetMaximumCollisionVelocity());

  bool immovable0 = actor0.GetMaterialList().HasMaterial(kMT_Immovable) || mass0 == 0.f;
  bool immovable1 = actor1.GetMaterialList().HasMaterial(kMT_Immovable) || mass1 == 0.f;

  if (velNormDot < -0.0001f) {
    if (!immovable0) {
      if (!immovable1) {
        float impulse = CollisionImpulseFiniteVsFinite(mass0, mass1, velNormDot, restitution);
        actor0.ApplyImpulseWR(impulse * normal, CAxisAngle::Identity());
        float negImpulse = -impulse;
        actor1.ApplyImpulseWR(negImpulse * normal, CAxisAngle::Identity());
      } else {
        float impulse = CollisionImpulseFiniteVsInfinite(mass0, velNormDot, restitution);
        CVector3f imp = impulse * normal;
        actor0.ApplyImpulseWR(imp, CAxisAngle::Identity());
      }
    } else {
      if (!immovable1) {
        float impulse = -CollisionImpulseFiniteVsInfinite(mass1, velNormDot, restitution);
        actor1.ApplyImpulseWR(impulse * normal, CAxisAngle::Identity());
      } else {
        actor0.SetVelocityWR(CVector3f::Zero());
        actor1.SetVelocityWR(CVector3f::Zero());
      }
    }
    actor0.UseCollisionImpulses();
    actor1.UseCollisionImpulses();
  } else if (velNormDot < 0.1f) {
    if (!immovable0) {
      if (IsUser(0)) {
        gDebugPrintCount = gDebugPrintCount + 1;
      }
      float impulse = 0.05f * mass0;
      CVector3f imp = impulse * normal;
      actor0.ApplyImpulseWR(imp, CAxisAngle::Identity());
      actor0.UseCollisionImpulses();
    }
    if (!immovable1) {
      if (IsUser(0)) {
        gDebugPrintCount = gDebugPrintCount + 1;
      }
      float impulse = -0.05f * mass1;
      CVector3f imp = impulse * normal;
      actor1.ApplyImpulseWR(imp, CAxisAngle::Identity());
      actor1.UseCollisionImpulses();
    }
  }

  float curMag0 = actor0.GetVelocityWR().Magnitude();
  if (curMag0 > maxVel0) {
    actor0.SetVelocityWR(maxVel0 * (actor0.GetVelocityWR() / curMag0));
    IsUser(0);
  }

  float curMag1 = actor1.GetVelocityWR().Magnitude();
  if (curMag1 > maxVel1) {
    actor1.SetVelocityWR(maxVel1 * (actor1.GetVelocityWR() / curMag1));
    IsUser(0);
  }
}

void CGameCollision::CollideWithStaticBodyNoRot(CPhysicsActor& actor, const CMaterialList& mat0,
                                                const CMaterialList& mat1,
                                                const CUnitVector3f& normal, float restitution,
                                                bool active) {
  CVector3f local_30(normal);
  if (((active) && mat0.HasMaterial(kMT_Player)) && !mat1.HasMaterial(kMT_Floor)) {
    local_30.SetZ(0.f);
  }

  if (local_30.CanBeNormalized()) {
    local_30.Normalize();
    const float velNormalDot = CVector3f::Dot(actor.GetVelocityWR(), local_30);
    if (velNormalDot < -0.0001f) {
      const float impulse =
          CollisionImpulseFiniteVsInfinite(actor.GetMass(), velNormalDot, restitution);
      CVector3f local_3c = impulse * local_30;
      actor.ApplyImpulseWR(local_3c, CAxisAngle::Identity());
      actor.UseCollisionImpulses();
    } else if (velNormalDot < 0.001f) {
      CVector3f local_48 = (0.05f * actor.GetMass()) * local_30;
      actor.ApplyImpulseWR(local_48, CAxisAngle::Identity());
      actor.UseCollisionImpulses();
    }
  }
}
