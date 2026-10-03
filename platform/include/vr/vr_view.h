// SPDX-License-Identifier: GPL-3.0-or-later
//
// The headset's view of the game, for the game thread: what the game camera
// must do differently while the headset shows the world per eye, and the
// tracked arm cannon.
//
// The eyes get the head's rotation and position through the stereo packet
// (aurora composes them into every draw), so the game camera stays the body
// facing: yaw from the stick, no pitch, no bob. The game still needs the head's
// facing for what it computes on the CPU: the culling frustum, the audio
// listener. The cannon follows the controller's aim pose, placed relative to
// the player's eye with PrimedGun's calibration (world scale, position and
// rotation offsets, model offset, smoothing).

#pragma once

#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"

class CFrustumPlanes;
class CPlayer;
class CStateManager;

namespace PortVr {

// The headset is running and this frame is shown immersively (the world per
// eye rather than on the virtual screen).
bool VrImmersive() noexcept;

// `cameraXf` with the head's rotation (relative to the recentred base)
// composed in; `cameraXf` itself when not immersive or the head is unknown.
CTransform4f VrHeadFacingTransform(const CTransform4f& cameraXf) noexcept;

// Replaces `frustum` with the head-facing cone of the settings' culling angle
// when immersive and culling is enabled; false leaves it alone.
bool VrCullingFrustum(const CTransform4f& cameraXf, float nearZ, CFrustumPlanes& frustum) noexcept;

// Immersive: the first-person camera keeps its yaw only (the headset supplies
// the pitch), does not bob, and the arm cannon does not fidget.
bool VrFlattenLookPitch() noexcept;
bool VrNoCameraBob() noexcept;
bool VrNoArmCannonFidget() noexcept;

// The 6DOF cannon. Once per simulation tick from CPlayer::UpdateGunTransform:
// when the tracked controller drives the cannon, replaces `gunXf` (world) and
// returns true. `cameraXf` is the current game camera (its yaw is the body).
bool VrCannonTransform(const CStateManager& mgr, const CPlayer& player, const CTransform4f& cameraXf,
                       CTransform4f& gunXf) noexcept;
// Whether the last VrCannonTransform placed the cannon.
bool VrCannonTracked() noexcept;
// The rendered model's offset from the tracked pose (PrimedGun's model offset
// and its base forward offset), in world units; zero when not tracked.
CVector3f VrCannonModelOffsetWorld() noexcept;

} // namespace PortVr
