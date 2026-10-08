// SPDX-License-Identifier: GPL-3.0-or-later
//
// Look to scan: the maths of PrimedGun's HMD scan targeting
// (Source/Core/Core/PrimedGun/NativeRuntime.cpp: RayMetricsForPoint,
// RayMetricsForAabb, ResolveScanActorRayMetrics, ScanTargetAimConePerp,
// ScanVisualAimConePerp, ScoreScanTarget, ScoreScanVisualTarget), with the same
// constants, so the feel the user tuned in the Dolphin build carries over.
//
// A scannable object is measured against the head's gaze ray: how far along
// the ray it lies and how far off it (in game units, against the nearest of its
// position, its orbit point and its bounding boxes). Two cones that widen with
// distance then decide what happens to it: inside the wide "visual" cone it
// shows its scan icon, inside the narrow "target" cone it can be picked as the
// scan target, the best score winning.
//
// Look to lock-on and look to grapple pick the orbit target outside the scan
// visor the same way, with the target cone (LockConePerp).
//
// No game types here, so tests/port_vr_look_scan.cpp checks it without the
// game. platform/vr/vr_look_scan.cpp gathers the objects and applies it.

#pragma once

#include <algorithm>
#include <cmath>

namespace PortVr::LookScan {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Aabb {
    Vec3 min;
    Vec3 max;
};

struct Ray {
    Vec3 origin;
    Vec3 direction; // unit length
};

// Where an object lies against the ray: `point` is the measured point (the
// ray's entry into a box it hits), `along` the distance along the ray, `perp`
// the distance off it (zero for a box the ray hits; a box's distance is to its
// surface, roughly), `radius` the box's extent across the ray.
struct Metrics {
    Vec3 point;
    float along = 0.0f;
    float perp = 0.0f;
    float radius = 0.0f;
    bool valid = false;
};

inline float Dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Sub(const Vec3& a, const Vec3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

inline Vec3 Along(const Ray& ray, float distance) noexcept {
    return {ray.origin.x + ray.direction.x * distance, ray.origin.y + ray.direction.y * distance,
            ray.origin.z + ray.direction.z * distance};
}

// PrimedGun ClampFinite.
inline float ClampFinite(float value, float fallback, float low, float high) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

// How far the ray reaches (PrimedGun SeedScanIndicatorTargetsFromHmd: the gun
// targeting distance, kept between 15 and 100 units).
inline float MaxAlong(float targetingDistance) noexcept {
    return ClampFinite(targetingDistance, 60.0f, 15.0f, 100.0f);
}

// The targeting radius the cones are built from (PrimedGun default 4).
inline float TargetingRadius(float radius) noexcept { return ClampFinite(radius, 4.0f, 0.1f, 50.0f); }

// PrimedGun RayMetricsForPoint: a point behind the eye by more than half a
// unit, or beyond the reach, is not measured.
inline bool MetricsForPoint(const Ray& ray, const Vec3& point, float maxAlong, Metrics& out) noexcept {
    const float along = Dot(Sub(point, ray.origin), ray.direction);
    if (!(along >= -0.5f) || along > maxAlong) {
        return false;
    }
    const float clamped = std::max(along, 0.0f);
    const Vec3 off = Sub(point, Along(ray, clamped));
    const float perpSq = Dot(off, off);
    if (!std::isfinite(perpSq)) {
        return false;
    }
    out = {point, clamped, std::sqrt(perpSq), 0.0f, true};
    return true;
}

// PrimedGun AabbLooksUsable.
inline bool AabbUsable(const Aabb& box) noexcept {
    const float values[6] = {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z};
    for (const float value : values) {
        if (!std::isfinite(value) || std::fabs(value) > 100000.0f) {
            return false;
        }
    }
    const float w = box.max.x - box.min.x;
    const float h = box.max.y - box.min.y;
    const float d = box.max.z - box.min.z;
    return w >= 0.0f && h >= 0.0f && d >= 0.0f && w <= 1000.0f && h <= 1000.0f && d <= 1000.0f;
}

inline float AabbMaxExtent(const Aabb& box) noexcept {
    return std::max({box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z});
}

// Some scannable objects carry large trigger volumes, so scan aim only trusts
// boxes no wider than twice the targeting radius, kept between a floor and a
// cap (PrimedGun ScanAabbUsableForAim).
inline bool AabbUsableForAim(const Aabb& box, float targetingRadius, float minExtent, float maxExtentCap) noexcept {
    const float maxExtent = std::clamp(TargetingRadius(targetingRadius) * 2.0f, minExtent, maxExtentCap);
    return AabbUsable(box) && AabbMaxExtent(box) <= maxExtent;
}

inline bool RenderAabbUsableForAim(const Aabb& box, float targetingRadius) noexcept {
    return AabbUsableForAim(box, targetingRadius, 8.0f, 22.0f);
}

inline bool PhysicsAabbUsableForAim(const Aabb& box, float targetingRadius) noexcept {
    return AabbUsableForAim(box, targetingRadius, 6.0f, 10.0f);
}

// PrimedGun RayIntersectsAabb (slabs): the entry distance, zero from inside.
inline bool RayHitsAabb(const Ray& ray, const Aabb& box, float maxAlong, float& hitAlong) noexcept {
    float tMin = 0.0f;
    float tMax = maxAlong;
    const auto axis = [&](float origin, float direction, float low, float high) {
        if (std::fabs(direction) < 0.00001f) {
            return origin >= low && origin <= high;
        }
        float t0 = (low - origin) / direction;
        float t1 = (high - origin) / direction;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        return tMin <= tMax;
    };
    if (!axis(ray.origin.x, ray.direction.x, box.min.x, box.max.x) ||
        !axis(ray.origin.y, ray.direction.y, box.min.y, box.max.y) ||
        !axis(ray.origin.z, ray.direction.z, box.min.z, box.max.z)) {
        return false;
    }
    const bool inside = ray.origin.x >= box.min.x && ray.origin.x <= box.max.x && ray.origin.y >= box.min.y &&
                        ray.origin.y <= box.max.y && ray.origin.z >= box.min.z && ray.origin.z <= box.max.z;
    hitAlong = inside ? 0.0f : std::max(tMin, 0.0f);
    return std::isfinite(hitAlong) && hitAlong <= maxAlong;
}

// PrimedGun RayMetricsForAabb: a box the ray hits is on the ray (perp 0, at
// the entry point); otherwise its centre's distance off the ray, less the
// box's extent across the ray.
inline bool MetricsForAabb(const Ray& ray, const Aabb& box, float maxAlong, Metrics& out) noexcept {
    if (!AabbUsable(box)) {
        return false;
    }
    const Vec3 center{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, (box.min.z + box.max.z) * 0.5f};
    const Vec3 half{(box.max.x - box.min.x) * 0.5f, (box.max.y - box.min.y) * 0.5f, (box.max.z - box.min.z) * 0.5f};
    const Vec3& d = ray.direction;
    const float radiusSq = half.x * half.x * std::max(0.0f, 1.0f - d.x * d.x) +
                           half.y * half.y * std::max(0.0f, 1.0f - d.y * d.y) +
                           half.z * half.z * std::max(0.0f, 1.0f - d.z * d.z);
    const float radius = std::sqrt(std::max(0.0f, radiusSq));

    float hitAlong = 0.0f;
    if (RayHitsAabb(ray, box, maxAlong, hitAlong)) {
        out = {Along(ray, hitAlong), hitAlong, 0.0f, radius, true};
        return true;
    }
    const float along = Dot(Sub(center, ray.origin), d);
    if (!std::isfinite(along) || along < -radius || along > maxAlong + radius) {
        return false;
    }
    const float clamped = std::clamp(along, 0.0f, maxAlong);
    const Vec3 nearest = Along(ray, clamped);
    const Vec3 off = Sub(center, nearest);
    const float perpSq = Dot(off, off);
    if (!std::isfinite(perpSq)) {
        return false;
    }
    out = {nearest, clamped, std::max(0.0f, std::sqrt(perpSq) - radius), radius, true};
    return true;
}

// PrimedGun PreferRayMetrics: the nearer to the ray, distance breaking ties.
inline bool Prefer(const Metrics& candidate, const Metrics& current) noexcept {
    if (!candidate.valid) {
        return false;
    }
    if (!current.valid) {
        return true;
    }
    return candidate.perp + candidate.along * 0.001f < current.perp + current.along * 0.001f;
}

// The narrow cone a scan target must be in (PrimedGun ScanTargetAimConePerp):
// at most this far off the ray, at the object's distance.
inline float TargetConePerp(float targetingRadius, const Metrics& metrics) noexcept {
    const float radius = TargetingRadius(targetingRadius);
    const float base = std::clamp(radius * 0.45f, 0.75f, 2.0f);
    const float distanceSlack = std::clamp(metrics.along - 3.0f, 0.0f, 60.0f) * 0.18f;
    const float boundsSlack = std::min(metrics.radius, 3.0f) * 0.35f;
    const float maxPerp = std::clamp(radius * 2.75f, 3.0f, 14.0f);
    return std::min(base + distanceSlack + boundsSlack, maxPerp);
}

// Look to lock-on and look to grapple: the scan target's cone, a grapple point
// getting PrimedGun's 1.3 times wider one (its gun ray's grapple_max_perp), as
// a point is harder to hold the gaze on than a body.
inline float LockConePerp(float targetingRadius, const Metrics& metrics, bool grapplePoint) noexcept {
    const float cone = TargetConePerp(targetingRadius, metrics);
    return grapplePoint ? cone * 1.3f : cone;
}

// The wide cone an object shows its scan icon in (PrimedGun ScanVisualAimConePerp).
inline float VisualConePerp(float targetingRadius, const Metrics& metrics) noexcept {
    const float radius = TargetingRadius(targetingRadius);
    const float base = std::clamp(radius * 3.0f, 3.0f, 14.0f);
    const float distanceSlack = std::clamp(metrics.along, 0.0f, 80.0f) * 0.55f;
    const float boundsSlack = std::min(metrics.radius, 2.0f) * 0.35f;
    const float maxPerp = std::clamp(radius * 8.0f, 18.0f, 36.0f);
    return std::min(base + distanceSlack + boundsSlack, maxPerp);
}

// Lower is better (PrimedGun ScoreScanTarget).
inline float TargetScore(const Metrics& metrics, float conePerp) noexcept {
    const float coneFraction = metrics.perp / std::max(conePerp, 0.001f);
    return coneFraction * 2.0f + metrics.perp * 0.20f + metrics.along * 0.003f + std::min(metrics.radius, 1.0f) * 0.02f;
}

// Lower is better (PrimedGun ScoreScanVisualTarget).
inline float VisualScore(const Metrics& metrics, float conePerp) noexcept {
    const float coneFraction = metrics.perp / std::max(conePerp, 0.001f);
    return coneFraction + metrics.along * 0.001f + std::min(metrics.radius, 1.0f) * 0.02f;
}

// Not in PrimedGun, which picked afresh every frame: the current pick stays
// while it scores within this much of the best, so head jitter between two
// close objects does not flicker the scan frame's highlight.
inline bool KeepPrevious(float previousScore, float bestScore) noexcept {
    return previousScore <= bestScore * 1.15f + 0.05f;
}

} // namespace PortVr::LookScan
