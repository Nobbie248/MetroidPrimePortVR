// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_view.h"

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_integration.h"
#include "vr/prime_vr_policy.h"
#include "vr/vr_settings.h"

#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace PortVr {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegreesToRadians = kPi / 180.0f;
// PrimedGun: the cannon pose keeps 24 % of the previous tick, and snaps when
// the controller jumped more than two units (a tracking glitch or a recenter).
constexpr float kCannonSmoothKeep = 0.24f;
constexpr float kCannonSnapDistance = 2.0f;
// PrimedGun BASE_MODEL_FORWARD_BACK_OFFSET: the model sits this far back along
// the aim, in metres, before the user's model offset.
constexpr float kModelForwardBackOffset = -0.080f;

struct CannonState {
    bool tracked = false;
    bool smoothValid = false;
    CQuaternion smoothRotation;
    CVector3f smoothPosition;
    CVector3f modelOffsetWorld;
};
CannonState s_cannon;

bool ImmersiveNow() noexcept {
    if (!OpenXRIsRunning()) {
        return false;
    }
    return PrimeVRPolicyGetSnapshot().presentation == VRPresentationMode::Immersive;
}

// OpenXR (x right, y up, -z forward) to Prime (x right, y forward, z up): a
// vector (x, y, z) becomes (x, -z, y). The map is a proper rotation, so a
// rotation quaternion's vector part converts the same way.
CVector3f PrimeFromXr(const std::array<float, 3>& v) noexcept { return CVector3f(v[0], -v[2], v[1]); }

CQuaternion PrimeFromXr(const std::array<float, 4>& q) noexcept { return CQuaternion(q[3], q[0], -q[2], q[1]); }

CQuaternion AxisQuaternion(float x, float y, float z, float radians) noexcept {
    const float half = 0.5f * radians;
    const float s = std::sin(half);
    return CQuaternion(std::cos(half), x * s, y * s, z * s);
}

// The camera's yaw about world up as a rotation: the body facing.
CQuaternion BodyYaw(const CTransform4f& cameraXf) noexcept {
    CVector3f forward = cameraXf.GetForward();
    forward[kDZ] = 0.f;
    if (!forward.CanBeNormalized()) {
        return CQuaternion::NoRotation();
    }
    forward.Normalize();
    // Prime's forward is (-sin yaw, cos yaw, 0) for a yaw about +z.
    return AxisQuaternion(0.f, 0.f, 1.f, std::atan2(-forward.GetX(), forward.GetY()));
}

bool HeadRotation(CTransform4f& rotation) noexcept {
    OpenXRFrameRequest request{};
    if (!OpenXRLatestFrameRequest(request) || !request.head_valid) {
        return false;
    }
    rotation = PrimeFromXr(request.head_orientation).BuildNormalized().BuildTransform4f();
    return true;
}

CQuaternion Nlerp(const CQuaternion& from, CQuaternion to, float t) noexcept {
    if (CQuaternion::Dot(from, to) < 0.f) {
        to = CQuaternion(-to.GetScalar(), -to.GetVector());
    }
    const float keep = 1.f - t;
    return CQuaternion(from.GetScalar() * keep + to.GetScalar() * t,
                       from.GetVector() * keep + to.GetVector() * t)
        .BuildNormalized();
}

} // namespace

bool VrImmersive() noexcept { return ImmersiveNow(); }

CTransform4f VrHeadFacingTransform(const CTransform4f& cameraXf) noexcept {
    CTransform4f head = CTransform4f::Identity();
    if (!ImmersiveNow() || !HeadRotation(head)) {
        return cameraXf;
    }
    CTransform4f facing = cameraXf.GetRotation() * head;
    facing.SetTranslation(cameraXf.GetTranslation());
    return facing;
}

bool VrHeadGaze(const CTransform4f& cameraXf, CVector3f& origin, CVector3f& direction) noexcept {
    if (!ImmersiveNow()) {
        return false;
    }
    OpenXRFrameRequest request{};
    if (!OpenXRLatestFrameRequest(request) || !request.head_valid) {
        return false;
    }
    // The eyes see the game camera's space through lean^-1 * head
    // (openxr_integration.cpp ViewFromBase), lean being a pitch about the
    // right axis, which Prime and OpenXR share.
    const float leanBack = GetVrSettings().lean_back_degrees * kDegreesToRadians;
    const CQuaternion leanInverse = AxisQuaternion(1.f, 0.f, 0.f, -leanBack);
    const CQuaternion head = (leanInverse * PrimeFromXr(request.head_orientation)).BuildNormalized();
    direction = cameraXf.Rotate(head.Transform(CVector3f(0.f, 1.f, 0.f)));
    if (!direction.CanBeNormalized()) {
        return false;
    }
    direction.Normalize();
    CVector3f offset = CVector3f::Zero();
    if (request.base_valid) {
        const std::array<float, 3> fromBase{
            request.head_position[0] - request.base_position[0],
            request.head_position[1] - request.base_position[1],
            request.head_position[2] - request.base_position[2],
        };
        offset = leanInverse.Transform(PrimeFromXr(fromBase)) * request.units_per_meter;
    }
    origin = cameraXf.GetTranslation() + cameraXf.Rotate(offset);
    return true;
}

bool VrCullingFrustum(const CTransform4f& cameraXf, float nearZ, CFrustumPlanes& frustum) noexcept {
    if (!ImmersiveNow()) {
        return false;
    }
    const PortVrSettings settings = GetVrSettings();
    if (!settings.frustum_culling_enabled) {
        return false;
    }
    const float degrees = std::clamp(settings.frustum_culling_degrees, 60.0f, 179.0f);
    frustum = CFrustumPlanes(VrHeadFacingTransform(cameraXf), degrees * kDegreesToRadians, 1.0f, nearZ, false, 100.f);
    return true;
}

bool VrFlattenLookPitch() noexcept { return ImmersiveNow(); }

bool VrNoCameraBob() noexcept { return ImmersiveNow() && GetVrSettings().patch_no_idle_sway; }

bool VrNoArmCannonFidget() noexcept {
    return ImmersiveNow() && GetVrSettings().patch_disable_arm_cannon_idle_fidget;
}

bool VrRemoveCinematicBars() noexcept {
    return OpenXRIsRunning() && GetVrSettings().remove_cinematic_bars;
}

bool VrSkyAtInfinity() noexcept { return OpenXRIsRunning() && GetVrSettings().sky_at_infinity; }

bool VrCannonTransform(const CStateManager& mgr, const CPlayer& player, const CTransform4f& cameraXf,
                       CTransform4f& gunXf) noexcept {
    (void)mgr;
    s_cannon.tracked = false;
    s_cannon.modelOffsetWorld = CVector3f::Zero();
    const PortVrSettings settings = GetVrSettings();
    if (!settings.patch_cannon_rotation || !ImmersiveNow()) {
        s_cannon.smoothValid = false;
        return false;
    }
    const OpenXRInputSnapshot snapshot = OpenXRGetInputSnapshot();
    const OpenXRControllerState& hand = snapshot.controllers[settings.use_right_hand ? 1 : 0];
    if (!snapshot.runtime_active || !hand.connected || !hand.aim_pose.valid) {
        s_cannon.smoothValid = false;
        return false;
    }

    OpenXRFrameRequest request{};
    const bool haveRequest = OpenXRLatestFrameRequest(request);
    const float scale = haveRequest && request.units_per_meter > 0.f ? request.units_per_meter : settings.world_scale;
    // The controller relative to the tracking base the eyes are placed from;
    // without a base, relative to the head.
    std::array<float, 3> base{};
    if (haveRequest && request.base_valid) {
        base = request.base_position;
    } else if (snapshot.head_pose.valid) {
        base = snapshot.head_pose.position;
    }
    const std::array<float, 3> relativeXr{
        hand.aim_pose.position[0] - base[0],
        hand.aim_pose.position[1] - base[1],
        hand.aim_pose.position[2] - base[2],
    };
    // PrimedGun's position offsets: x right, y up, z back (OpenXR axes), in metres.
    const CVector3f relative =
        PrimeFromXr(relativeXr) * scale +
        CVector3f(settings.offset_x * scale, -settings.offset_z * scale, settings.offset_y * scale);

    const CQuaternion body = BodyYaw(cameraXf);
    const CVector3f worldPosition = player.GetEyePosition() + body.Transform(relative);

    // PrimedGun's rotation offsets are local yaw (about up), pitch (about
    // right) and roll (about the aim), applied in that order to the controller.
    const CQuaternion offsets = AxisQuaternion(0.f, 0.f, 1.f, settings.rot_offset_y * kDegreesToRadians) *
                                AxisQuaternion(1.f, 0.f, 0.f, settings.rot_offset_x * kDegreesToRadians) *
                                AxisQuaternion(0.f, 1.f, 0.f, -settings.rot_offset_z * kDegreesToRadians);
    const CQuaternion worldRotation = (body * PrimeFromXr(hand.aim_pose.orientation) * offsets).BuildNormalized();

    const CVector3f jump = worldPosition - s_cannon.smoothPosition;
    if (!s_cannon.smoothValid || CVector3f::Dot(jump, jump) > kCannonSnapDistance * kCannonSnapDistance) {
        s_cannon.smoothRotation = worldRotation;
        s_cannon.smoothPosition = worldPosition;
        s_cannon.smoothValid = true;
    } else {
        s_cannon.smoothPosition = s_cannon.smoothPosition * kCannonSmoothKeep + worldPosition * (1.f - kCannonSmoothKeep);
        s_cannon.smoothRotation = Nlerp(s_cannon.smoothRotation, worldRotation, 1.f - kCannonSmoothKeep);
    }

    gunXf = s_cannon.smoothRotation.BuildTransform4f();
    gunXf.SetTranslation(s_cannon.smoothPosition);
    const CVector3f modelLocal(settings.model_offset_x * scale,
                               (kModelForwardBackOffset + settings.model_offset_y) * scale,
                               settings.model_offset_z * scale);
    s_cannon.modelOffsetWorld = gunXf.Rotate(modelLocal);
    s_cannon.tracked = true;
    return true;
}

bool VrCannonTracked() noexcept { return s_cannon.tracked; }

CVector3f VrCannonModelOffsetWorld() noexcept {
    return s_cannon.tracked ? s_cannon.modelOffsetWorld : CVector3f::Zero();
}

} // namespace PortVr
