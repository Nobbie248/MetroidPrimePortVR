// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_view.h"

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_integration.h"
#include "vr/prime_vr_policy.h"
#include "vr/vr_beam_wheel.h"
#include "vr/vr_pad.h"
#include "vr/vr_settings.h"
#include "port_surface_culling_math.h"

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
float s_ballEntryYaw = 0.f;
CVector3f s_ballEntryDirection(0.f, 1.f, 0.f);
CTransform4f s_unmorphBase = CTransform4f::Identity();
CVector3f s_unmorphOffset = CVector3f::Zero();

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

CTransform4f VrHeadViewTransform(const CTransform4f& cameraXf) noexcept {
    CVector3f origin;
    CVector3f direction;
    if (!VrHeadGaze(cameraXf, origin, direction)) {
        return cameraXf;
    }
    return CTransform4f(VrHeadViewRotation(cameraXf), origin);
}

CMatrix3f VrHeadViewRotation(const CTransform4f& cameraXf) noexcept {
    const CMatrix3f body = cameraXf.BuildMatrix3f();
    if (!ImmersiveNow()) {
        return body;
    }
    OpenXRFrameRequest request{};
    if (!OpenXRLatestFrameRequest(request) || !request.head_valid) {
        return body;
    }
    // As VrHeadGaze: the eyes see the game camera's space through lean^-1 * head.
    const float leanBack = GetVrSettings().lean_back_degrees * kDegreesToRadians;
    const CQuaternion leanInverse = AxisQuaternion(1.f, 0.f, 0.f, -leanBack);
    const CQuaternion head = (leanInverse * PrimeFromXr(request.head_orientation)).BuildNormalized();
    const CTransform4f view = cameraXf.GetRotation() * head.BuildTransform4f();
    // Upright: the head's facing without its roll. The right axis stays level,
    // so up is as close to the world's up as the facing allows; looking
    // straight up or down, where level is undefined, the head's own right.
    const CVector3f forward = view.GetForward();
    CVector3f right = CVector3f::Cross(forward, CVector3f(0.f, 0.f, 1.f));
    if (right.Magnitude() < 1.e-3f) {
        right = view.GetRight();
    }
    right.Normalize();
    return CTransform4f::FromColumns(right, forward, CVector3f::Cross(right, forward), CVector3f::Zero())
        .BuildMatrix3f();
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

void VrBeginBallCamera(const CTransform4f& firstPersonXf) noexcept {
    s_ballEntryYaw = 0.f;
    s_ballEntryDirection = firstPersonXf.GetForward();
    if (!ImmersiveNow()) {
        return;
    }
    const CVector3f headRight = VrHeadViewRotation(firstPersonXf).GetColumn(kDX);
    const CVector3f bodyRight = firstPersonXf.GetRight();
    const float yaw = std::atan2(headRight.GetY(), headRight.GetX()) -
                      std::atan2(bodyRight.GetY(), bodyRight.GetX());
    s_ballEntryYaw = yaw;
    s_ballEntryDirection = CVector3f(-headRight.GetY(), headRight.GetX(), 0.f);
}

CVector3f VrBallCameraEntryDirection() noexcept { return s_ballEntryDirection; }

CTransform4f VrBallCameraBaseTransform(const CTransform4f& camera, float weight) noexcept {
    if (!ImmersiveNow()) {
        return camera;
    }
    CTransform4f base = camera.GetRotation() *
        AxisQuaternion(0.f, 0.f, 1.f, -s_ballEntryYaw * std::clamp(weight, 0.f, 1.f)).BuildTransform4f();
    base.SetTranslation(camera.GetTranslation());
    return base;
}

void VrBeginUnmorphCamera(const CTransform4f& camera, const CVector3f& eyePosition) noexcept {
    s_unmorphBase = camera.GetRotation();
    s_unmorphOffset = camera.GetTranslation() - eyePosition;
}

CTransform4f VrUnmorphCameraBase(const CTransform4f& camera) noexcept {
    CTransform4f view = s_unmorphBase;
    view.SetTranslation(camera.GetTranslation());
    return view;
}

CTransform4f VrUnmorphCameraTransform(const CVector3f& eyePosition, float progress) noexcept {
    CTransform4f view = s_unmorphBase;
    view.SetTranslation(eyePosition + s_unmorphOffset * (1.f - std::clamp(progress, 0.f, 1.f)));
    return view;
}

bool VrSurfaceCullingVolume(const CTransform4f& cameraXf,
                            PortSurfaceCulling::StereoVolume& volume) noexcept {
    volume = {};
    const PortVrSettings settings = GetVrSettings();
    OpenXRFrameRequest request{};
    if (!settings.frustum_culling_enabled || !ImmersiveNow() ||
        !OpenXRLatestFrameRequest(request) || !request.immersive || !request.head_valid ||
        !request.base_valid || !std::isfinite(request.units_per_meter) || request.units_per_meter <= 0.f) {
        return false;
    }
    const CQuaternion leanInverse = AxisQuaternion(1.f, 0.f, 0.f, -settings.lean_back_degrees * kDegreesToRadians);
    const auto vector = [](const CVector3f& v) {
        return PortSurfaceCulling::Vector{v.GetX(), v.GetY(), v.GetZ()};
    };
    for (size_t eye = 0; eye < 2; ++eye) {
        float norm = 0.f;
        for (float component : request.eye_orientation[eye]) { norm += component * component; }
        if (!std::isfinite(norm) || norm < 1.e-8f) { return false; }
        const CQuaternion rotation = (leanInverse * PrimeFromXr(request.eye_orientation[eye])).BuildNormalized();
        const CTransform4f facing = cameraXf.GetRotation() * rotation.BuildTransform4f();
        const std::array<float, 3> fromBase{
            request.eye_position[eye][0] - request.base_position[0],
            request.eye_position[eye][1] - request.base_position[1],
            request.eye_position[eye][2] - request.base_position[2],
        };
        const CVector3f origin = cameraXf.GetTranslation() +
            cameraXf.Rotate(leanInverse.Transform(PrimeFromXr(fromBase)) * request.units_per_meter);
        // Keep 7.5 degrees and 10 cm beyond each located eye. The renderer also
        // retains anything inside its existing draw frustum, including probes.
        if (!volume.eyes[eye].Build(vector(origin), vector(facing.GetRight()), vector(facing.GetForward()),
                                   vector(facing.GetUp()), request.eye_fov[eye],
                                   7.5f * kDegreesToRadians, 0.10f * request.units_per_meter)) {
            return false;
        }
    }
    return true;
}

bool VrNoCameraBob() noexcept { return ImmersiveNow() && GetVrSettings().patch_no_idle_sway; }

bool VrNoArmCannonFidget() noexcept {
    return ImmersiveNow() && GetVrSettings().patch_disable_arm_cannon_idle_fidget;
}

bool VrRemoveCinematicBars() noexcept {
    return OpenXRIsRunning() && GetVrSettings().remove_cinematic_bars;
}

bool VrSkyAtInfinity() noexcept { return OpenXRIsRunning() && GetVrSettings().sky_at_infinity; }

bool VrHideSpaceWarp() noexcept { return OpenXRIsRunning() && !GetVrSettings().space_warp; }

bool VrScanWindowNoZoom() noexcept { return ImmersiveNow() && !GetVrSettings().scan_zoom; }

int VrBeamWheelHoverBeam() noexcept {
    if (!GetVrSettings().beam_wheel_hud_highlight) {
        return -1;
    }
    const VrPadState pad = GetVrPadState();
    if (!pad.active || !pad.gameplay || !pad.weapon_panel) {
        return -1;
    }
    // The wheel counts PrimedGun's way (Power, Wave, Ice, Plasma); the game's
    // EBeamId puts Ice before Wave.
    if (pad.weapon_selected < 0 || pad.weapon_selected > 3) {
        return -1;
    }
    return BeamWheel::GameBeamId(static_cast<BeamWheel::Beam>(pad.weapon_selected));
}

namespace {
bool TrackedCannonPose(const CPlayer& player, const CTransform4f& cameraXf,
                       const PortVrSettings& settings, CVector3f& worldPosition,
                       CQuaternion& worldRotation, float& scale) noexcept {
    if (!settings.patch_cannon_rotation || !ImmersiveNow()) {
        return false;
    }
    const OpenXRInputSnapshot snapshot = OpenXRGetInputSnapshot();
    const OpenXRControllerState& hand = snapshot.controllers[settings.use_right_hand ? 1 : 0];
    if (!snapshot.runtime_active || !hand.connected || !hand.aim_pose.valid) {
        return false;
    }

    OpenXRFrameRequest request{};
    const bool haveRequest = OpenXRLatestFrameRequest(request);
    scale = haveRequest && request.units_per_meter > 0.f ? request.units_per_meter : settings.world_scale;
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
    worldPosition = player.GetEyePosition() + body.Transform(relative);

    // PrimedGun's rotation offsets are local yaw (about up), pitch (about
    // right) and roll (about the aim), applied in that order to the controller.
    const CQuaternion offsets = AxisQuaternion(0.f, 0.f, 1.f, settings.rot_offset_y * kDegreesToRadians) *
                                AxisQuaternion(1.f, 0.f, 0.f, settings.rot_offset_x * kDegreesToRadians) *
                                AxisQuaternion(0.f, 1.f, 0.f, -settings.rot_offset_z * kDegreesToRadians);
    worldRotation = (body * PrimeFromXr(hand.aim_pose.orientation) * offsets).BuildNormalized();
    return true;
}
} // namespace

bool VrCannonAim(const CPlayer& player, const CTransform4f& cameraXf,
                 CVector3f& origin, CVector3f& direction) noexcept {
    CQuaternion rotation;
    float scale;
    if (!TrackedCannonPose(player, cameraXf, GetVrSettings(), origin, rotation, scale)) {
        return false;
    }
    direction = rotation.Transform(CVector3f(0.f, 1.f, 0.f));
    return true;
}

bool VrCannonTransform(const CStateManager& mgr, const CPlayer& player, const CTransform4f& cameraXf,
                       CTransform4f& gunXf) noexcept {
    (void)mgr;
    s_cannon.tracked = false;
    s_cannon.modelOffsetWorld = CVector3f::Zero();
    const PortVrSettings settings = GetVrSettings();
    CVector3f worldPosition;
    CQuaternion worldRotation;
    float scale;
    if (!TrackedCannonPose(player, cameraXf, settings, worldPosition, worldRotation, scale)) {
        s_cannon.smoothValid = false;
        return false;
    }

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

void VrResetCannonSmoothing() noexcept { s_cannon.smoothValid = false; }

CVector3f VrCannonModelOffsetWorld() noexcept {
    return s_cannon.tracked ? s_cannon.modelOffsetWorld : CVector3f::Zero();
}

} // namespace PortVr
