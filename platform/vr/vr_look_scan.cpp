// SPDX-License-Identifier: GPL-3.0-or-later
//
// Look to scan: PrimedGun's HMD scan targeting (NativeRuntime.cpp
// UpdateScanTargetingFromHmd, SeedScanIndicatorTargetsFromHmd and the
// "Gun Ray Lock/Scan Target Hook" on CPlayer::FindOrbitTargetId), on the
// decomp's own objects instead of memory reads.
//
// In the scan visor the body keeps its yaw-only facing while the head looks
// around, so the game's screen-box targeting (CPlayer::FindBestOrbitableObject
// against the first-person camera) would pick what is in front of the body.
// Here every tick the scannable objects around the head's gaze are measured
// with PrimedGun's cones (vr/vr_look_scan.h): the ones in the wide cone get
// their scan icons, and the best one in the narrow cone that the eye can see
// becomes the scan target. The game's own rules still apply to both: the
// object must pass CPlayer::ValidateOrbitTargetId (targetable, active, scan
// visor flag, same area, not straight above or below) and the target
// distance test, and the target the game's line-of-sight test.

#include "vr/vr_look_scan.h"
#include "vr/vr_settings.h"
#include "vr/vr_view.h"

#include "Collision/CMaterialFilter.hpp"
#include "Collision/CMaterialList.hpp"
#include "Collision/CRayCastResult.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <algorithm>

namespace PortVr {
namespace {

// PrimedGun keeps the 16 best objects for icons and the 8 best for the target.
constexpr int kMaxNearby = 16;
constexpr int kMaxTargets = 8;
// Line-of-sight tests per tick, best candidates first.
constexpr int kMaxSightTests = 4;

// CPlayerOrbit.cpp's line-of-sight filters, so the head's pick obeys the same
// visibility rule as the game's own scan targeting.
const CMaterialFilter kLineOfSightFilter = CMaterialFilter::MakeIncludeExclude(
    CMaterialList(kMT_Solid), CMaterialList(kMT_ProjectilePassthrough, kMT_ScanPassthrough, kMT_Player));
const CMaterialFilter kOccluderFilter = CMaterialFilter::MakeIncludeExclude(
    CMaterialList(kMT_Solid, kMT_Occluder), CMaterialList(kMT_ProjectilePassthrough, kMT_ScanPassthrough, kMT_Player));

struct Ranked {
    TUniqueId id = kInvalidUniqueId;
    float score = 0.0f;
};

struct LookState {
    bool active = false;
    unsigned int stamp = 0;
    TUniqueId target = kInvalidUniqueId;
    int nearbyCount = 0;
    TUniqueId nearby[kMaxNearby];
};
LookState s_look;

// Results are kept for the tick they were made in and the next, so readers
// that run before the player's input in a tick still see them, and a tick
// that skips the orbit input (morphing, cinematics) lets them lapse.
bool Fresh(const CStateManager& mgr) noexcept { return s_look.active && mgr.GetUpdateFrameIndex() - s_look.stamp <= 2u; }

LookScan::Vec3 ToLook(const CVector3f& v) noexcept { return {v.GetX(), v.GetY(), v.GetZ()}; }

LookScan::Aabb ToLook(const CAABox& box) noexcept { return {ToLook(box.GetMinPoint()), ToLook(box.GetMaxPoint())}; }

// PrimedGun ResolveScanActorRayMetrics, plus the orbit point the lock aims at:
// the nearest to the ray of the object's position, its orbit point, its render
// box and its collision box, the boxes only when small enough to trust.
LookScan::Metrics MeasureActor(const CActor& actor, const CStateManager& mgr, const LookScan::Ray& ray,
                               float maxAlong, float targetingRadius) noexcept {
    LookScan::Metrics best;
    LookScan::Metrics metrics;
    if (LookScan::MetricsForPoint(ray, ToLook(actor.GetTranslation()), maxAlong, metrics) &&
        LookScan::Prefer(metrics, best)) {
        best = metrics;
    }
    if (LookScan::MetricsForPoint(ray, ToLook(actor.GetOrbitPosition(mgr)), maxAlong, metrics) &&
        LookScan::Prefer(metrics, best)) {
        best = metrics;
    }
    const LookScan::Aabb render = ToLook(actor.GetRenderBoundsCached());
    if (LookScan::RenderAabbUsableForAim(render, targetingRadius) &&
        LookScan::MetricsForAabb(ray, render, maxAlong, metrics) && LookScan::Prefer(metrics, best)) {
        best = metrics;
    }
    if (const CPhysicsActor* physics = TCastToConstPtr< CPhysicsActor >(&actor)) {
        const LookScan::Aabb collision = ToLook(physics->GetBoundingBox());
        if (LookScan::PhysicsAabbUsableForAim(collision, targetingRadius) &&
            LookScan::MetricsForAabb(ray, collision, maxAlong, metrics) && LookScan::Prefer(metrics, best)) {
            best = metrics;
        }
    }
    return best;
}

// Keeps the `capacity` lowest scores (PrimedGun's insert_candidate).
void Insert(Ranked* ranked, int& count, int capacity, TUniqueId id, float score) noexcept {
    int slot = count;
    if (count == capacity) {
        slot = static_cast< int >(std::max_element(ranked, ranked + count,
                                                   [](const Ranked& a, const Ranked& b) { return a.score < b.score; }) -
                                  ranked);
        if (score >= ranked[slot].score) {
            return;
        }
    } else {
        ++count;
    }
    ranked[slot] = Ranked{id, score};
}

// CPlayer::FindBestOrbitableObject's test: nothing solid between the eye and
// the object's orbit point, ignoring the object itself and the geometry of
// areas that are occluded.
bool InSight(CStateManager& mgr, const CVector3f& eye, const CActor& actor) noexcept {
    const CVector3f eyeToOrbit = actor.GetOrbitPosition(mgr) - eye;
    if (!eyeToOrbit.CanBeNormalized()) {
        return true;
    }
    const float distance = eyeToOrbit.Magnitude();
    const CVector3f direction = eyeToOrbit.AsNormalized();
    TEntityList nearList;
    mgr.BuildNearList(nearList, eye, direction, distance, kOccluderFilter, &actor);
    for (auto it = nearList.begin(); it != nearList.end();) {
        if (const CEntity* entity = mgr.GetObjectById(*it)) {
            const TAreaId areaId = entity->GetCurrentAreaId();
            if (areaId == kInvalidAreaId ||
                (areaId != mgr.GetNextAreaId() &&
                 mgr.GetWorld()->GetAreaAlways(areaId).GetOcclusionState() == CGameArea::kOS_Occluded)) {
                it = nearList.erase(it);
                continue;
            }
        }
        ++it;
    }
    TUniqueId hitId = kInvalidUniqueId;
    return mgr.RayWorldIntersection(hitId, eye, direction, distance, kLineOfSightFilter, nearList).IsInvalid();
}

} // namespace

void VrLookToScanUpdate(CStateManager& mgr, const CPlayer& player) noexcept {
    const TUniqueId previous = s_look.target;
    s_look.stamp = mgr.GetUpdateFrameIndex();
    s_look.active = false;
    s_look.target = kInvalidUniqueId;
    s_look.nearbyCount = 0;
    if (!VrImmersive() || mgr.GetPlayerState()->GetCurrentVisor() != CPlayerState::kPV_Scan ||
        player.GetMorphballTransitionState() != CPlayer::kMS_Unmorphed) {
        return;
    }
    const PortVrSettings settings = GetVrSettings();
    if (!settings.patch_gun_ray_target) {
        return;
    }
    CVector3f origin;
    CVector3f direction;
    if (!VrHeadGaze(player.GetFirstPersonCameraTransform(mgr), origin, direction)) {
        return;
    }
    s_look.active = true;
    // Orbit disable sources (CPlayer::UpdateOrbitableObjects): nothing to target.
    if (player.CheckOrbitDisableSourceList()) {
        return;
    }

    const LookScan::Ray ray{ToLook(origin), ToLook(direction)};
    const float targetingRadius = LookScan::TargetingRadius(settings.gun_targeting_radius);
    const float maxAlong = LookScan::MaxAlong(settings.gun_targeting_distance);
    const CVector3f reach(maxAlong, maxAlong, maxAlong);
    TEntityList nearList;
    mgr.BuildNearList(nearList, CAABox(origin - reach, origin + reach),
                      CMaterialFilter::MakeInclude(CMaterialList(kMT_Scannable)), &player);

    const CVector3f eye = player.GetEyePosition();
    const float maxTargetDistance = player.GetOrbitMaxTargetDistance(mgr);
    Ranked nearby[kMaxNearby];
    int nearbyCount = 0;
    Ranked targets[kMaxTargets];
    int targetCount = 0;
    for (auto it = nearList.begin(); it != nearList.end(); ++it) {
        const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(*it));
        if (actor == nullptr || actor->GetUniqueId() == player.GetUniqueId() ||
            !actor->GetMaterialList().HasMaterial(kMT_Scannable) || actor->GetScannableObjectInfo() == nullptr) {
            continue;
        }
        // The game's own target rules (CPlayer::FindOrbitableObjects).
        if (player.ValidateOrbitTargetId(actor->GetUniqueId(), mgr) != CPlayer::kOVR_OK) {
            continue;
        }
        if (actor->GetDoTargetDistanceTest() &&
            (actor->GetOrbitPosition(mgr) - eye).Magnitude() > maxTargetDistance) {
            continue;
        }
        const LookScan::Metrics metrics = MeasureActor(*actor, mgr, ray, maxAlong, targetingRadius);
        if (!metrics.valid) {
            continue;
        }
        const float visualCone = LookScan::VisualConePerp(targetingRadius, metrics);
        if (metrics.perp <= visualCone) {
            Insert(nearby, nearbyCount, kMaxNearby, actor->GetUniqueId(), LookScan::VisualScore(metrics, visualCone));
        }
        const float targetCone = LookScan::TargetConePerp(targetingRadius, metrics);
        if (metrics.perp <= targetCone) {
            Insert(targets, targetCount, kMaxTargets, actor->GetUniqueId(), LookScan::TargetScore(metrics, targetCone));
        }
    }

    std::sort(targets, targets + targetCount, [](const Ranked& a, const Ranked& b) { return a.score < b.score; });
    // The current pick stays while it scores close to the best.
    for (int i = 1; i < targetCount; ++i) {
        if (targets[i].id == previous && LookScan::KeepPrevious(targets[i].score, targets[0].score)) {
            std::rotate(targets, targets + i, targets + i + 1);
            break;
        }
    }
    for (int i = 0; i < targetCount && i < kMaxSightTests; ++i) {
        const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(targets[i].id));
        if (actor != nullptr && InSight(mgr, eye, *actor)) {
            s_look.target = targets[i].id;
            break;
        }
    }

    for (int i = 0; i < nearbyCount; ++i) {
        s_look.nearby[i] = nearby[i].id;
    }
    s_look.nearbyCount = nearbyCount;
    // The target always shows its icon (PrimedGun EnsureUidInScanVisorTargets).
    if (s_look.target != kInvalidUniqueId &&
        std::find(s_look.nearby, s_look.nearby + s_look.nearbyCount, s_look.target) ==
            s_look.nearby + s_look.nearbyCount) {
        if (s_look.nearbyCount < kMaxNearby) {
            ++s_look.nearbyCount;
        }
        s_look.nearby[s_look.nearbyCount - 1] = s_look.target;
    }
}

bool VrLookToScanTarget(const CStateManager& mgr, TUniqueId& id) noexcept {
    if (!Fresh(mgr)) {
        return false;
    }
    id = s_look.target;
    return true;
}

const TUniqueId* VrLookToScanNearby(const CStateManager& mgr, int& count) noexcept {
    count = Fresh(mgr) ? s_look.nearbyCount : 0;
    return count > 0 ? s_look.nearby : nullptr;
}

bool VrLookToScanHoldsFacing(const CStateManager& mgr) noexcept { return Fresh(mgr); }

} // namespace PortVr
